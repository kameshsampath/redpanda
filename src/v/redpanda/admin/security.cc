/*
 * Copyright 2023 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */
#include "cluster/controller.h"
#include "cluster/security_frontend.h"
#include "json/json.h"
#include "json/stringbuffer.h"
#include "kafka/server/server.h"
#include "redpanda/admin/api-doc/security.json.hh"
#include "redpanda/admin/server.h"
#include "redpanda/admin/util.h"
#include "security/credential_store.h"
#include "security/oidc_authenticator.h"
#include "security/oidc_service.h"
#include "security/request_auth.h"
#include "security/role_store.h"
#include "security/scram_algorithm.h"
#include "security/scram_authenticator.h"
#include "security/scram_credential.h"

#include <seastar/coroutine/as_future.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/url.hh>

#include <optional>
#include <sstream>

namespace {

// TODO: factor out generic serialization from seastar http exceptions
security::scram_credential parse_scram_credential(const json::Document& doc) {
    if (!doc.IsObject()) {
        throw ss::httpd::bad_request_exception(fmt::format("Not an object"));
    }

    if (!doc.HasMember("algorithm") || !doc["algorithm"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String algo missing"));
    }
    const auto algorithm = std::string_view(
      doc["algorithm"].GetString(), doc["algorithm"].GetStringLength());
    validate_no_control(
      algorithm, admin_server::string_conversion_exception{algorithm});

    if (!doc.HasMember("password") || !doc["password"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String password smissing"));
    }
    const auto password = doc["password"].GetString();
    validate_no_control(
      password, admin_server::string_conversion_exception{"PASSWORD"});

    security::scram_credential credential;

    if (algorithm == security::scram_sha256_authenticator::name) {
        credential = security::scram_sha256::make_credentials(
          password, security::scram_sha256::min_iterations);

    } else if (algorithm == security::scram_sha512_authenticator::name) {
        credential = security::scram_sha512::make_credentials(
          password, security::scram_sha512::min_iterations);

    } else {
        throw ss::httpd::bad_request_exception(
          fmt::format("Unknown scram algorithm: {}", algorithm));
    }

    return credential;
}

bool match_scram_credential(
  const json::Document& doc, const security::scram_credential& creds) {
    // Document is pre-validated via earlier parse_scram_credential call
    const auto password = ss::sstring(doc["password"].GetString());
    const auto algorithm = std::string_view(
      doc["algorithm"].GetString(), doc["algorithm"].GetStringLength());
    validate_no_control(
      algorithm, admin_server::string_conversion_exception{algorithm});

    if (algorithm == security::scram_sha256_authenticator::name) {
        return security::scram_sha256::validate_password(
          password, creds.stored_key(), creds.salt(), creds.iterations());
    } else if (algorithm == security::scram_sha512_authenticator::name) {
        return security::scram_sha512::validate_password(
          password, creds.stored_key(), creds.salt(), creds.iterations());
    } else {
        throw ss::httpd::bad_request_exception(
          fmt::format("Unknown scram algorithm: {}", algorithm));
    }
}

bool is_no_op_user_write(
  security::credential_store& store,
  security::credential_user username,
  security::scram_credential credential) {
    auto user_opt = store.get<security::scram_credential>(username);
    if (user_opt.has_value()) {
        return user_opt.value() == credential;
    } else {
        return false;
    }
}

enum class role_errc {
    malformed_def = 40001,
    invalid_name = 40002,
    unrecognized_field = 40003,
    member_list_conflict = 40004,
    role_not_found = 40401,
    role_already_exists = 40901,
    role_name_conflict = 40902,
};

// NOTE(oren): bogus -Wunneeded-internal-declaration here from clang-tidy (?)
std::ostream& operator<<(std::ostream& os, role_errc code) {
    switch (code) {
    case role_errc::malformed_def:
        return os << "Malformed request";
    case role_errc::invalid_name:
        return os << "Invalid role name";
    case role_errc::unrecognized_field:
        return os << "Unrecognized field";
    case role_errc::member_list_conflict:
        return os << "Conflict between 'add' and 'remove' lists";
    case role_errc::role_not_found:
        return os << "Role not found";
    case role_errc::role_already_exists:
        return os << "Role already exists";
    case role_errc::role_name_conflict:
        return os << "Role name conflict";
    }
    __builtin_unreachable();
}

ss::http::reply::status_type role_errc_to_status(role_errc c) {
    return ss::http::reply::status_type{static_cast<int>(c) / 100};
}

ss::httpd::security_json::role_member
role_member_to_json(const security::role_member& m) {
    ss::httpd::security_json::role_member j_member;
    j_member.name = ss::sstring{m.name()};
    j_member.principal_type = static_cast<
      ss::httpd::security_json::role_member::role_member_principal_type>(
      m.type());
    return j_member;
}

} // namespace

namespace json {
void rjson_serialize(
  json::Writer<json::StringBuffer>& w,
  const ss::httpd::security_json::rbac_error_body& v) {
    w.StartObject();
    w.Key("message");
    w.String(v.message());
    w.Key("code");
    w.Uint(v.code());
    w.EndObject();
}
} // namespace json

namespace {
std::string
role_errc_to_json(role_errc e, std::optional<std::string_view> msg) {
    ss::httpd::security_json::rbac_error_body body;
    body.code = static_cast<int>(e);
    if (msg.has_value()) {
        body.message = fmt::format("{}: {}", e, msg.value());
    } else {
        body.message = fmt::format("{}", e);
    }

    json::StringBuffer sb;
    json::Writer<json::StringBuffer> writer(sb);
    using ::json::rjson_serialize;
    rjson_serialize(writer, body);
    return {sb.GetString(), sb.GetSize()};
}

void throw_role_exception(
  role_errc ec, std::optional<std::string_view> msg = std::nullopt) {
    throw ss::httpd::base_exception(
      role_errc_to_json(ec, msg), role_errc_to_status(ec));
}

void throw_on_role_command_err(std::error_code ec) {
    if (ec.category() == cluster::error_category()) {
        switch (cluster::errc(ec.value())) {
        case cluster::errc::role_does_not_exist:
            throw_role_exception(role_errc::role_not_found);
            break;
        case cluster::errc::role_exists:
            throw_role_exception(role_errc::role_already_exists);
            break;
        default:
            break;
        }
    }
}

absl::flat_hash_set<security::role_member>
parse_json_members_list(const json::Document& doc, std::string_view key) {
    bool has_key = doc.HasMember(key.data());

    if (!has_key) {
        return {};
    } else if (!doc[key.data()].IsArray()) {
        throw_role_exception(
          role_errc::malformed_def, fmt::format("Array '{}' missing.", key));
    }

    std::vector<security::role_member> result;
    const auto& mem_arr = doc[key.data()].GetArray();
    result.reserve(mem_arr.Size());
    absl::c_transform(
      mem_arr,
      std::back_inserter(result),
      [](const auto& p) -> security::role_member {
          if (!p.IsObject()) {
              throw_role_exception(
                role_errc::malformed_def,
                fmt::format("Role member is not a JSON object"));
          }
          if (!p.HasMember("name") || !p["name"].IsString()) {
              throw_role_exception(
                role_errc::malformed_def,
                fmt::format("String 'name' missing from role_member"));
          }
          if (
            !p.HasMember("principal_type") || !p["principal_type"].IsString()) {
              throw_role_exception(
                role_errc::malformed_def,
                fmt::format(
                  "String 'principal_type' missing from role_member"));
          }

          ss::sstring p_type{p["principal_type"].GetString()};
          ss::sstring name{p["name"].GetString()};
          if (p_type != "User") {
              throw_role_exception(
                role_errc::malformed_def,
                fmt::format(
                  "Role membership reserved for user principals, got {{{}:{}}}",
                  p_type,
                  name));
          }
          return {security::role_member_type::user, p["name"].GetString()};
      });
    return {result.begin(), result.end()};
}

} // namespace

void admin_server::register_security_routes() {
    register_route<superuser>(
      ss::httpd::security_json::create_user,
      [this](std::unique_ptr<ss::http::request> req) {
          return create_user_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::delete_user,
      [this](std::unique_ptr<ss::http::request> req) {
          return delete_user_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::update_user,
      [this](std::unique_ptr<ss::http::request> req) {
          return update_user_handler(std::move(req));
      });

    register_route<user>(
      ss::httpd::security_json::oidc_whoami,
      [this](std::unique_ptr<ss::http::request> req) {
          return oidc_whoami_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::oidc_keys_cache_invalidate,
      [this](std::unique_ptr<ss::http::request> req) {
          return oidc_keys_cache_invalidate_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::oidc_revoke,
      [this](std::unique_ptr<ss::http::request> req) {
          return oidc_revoke_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::list_users,
      [this](std::unique_ptr<ss::http::request> req) {
          bool include_ephemeral = req->get_query_param("include_ephemeral")
                                   == "true";

          auto pred = [include_ephemeral](auto const& c) {
              return include_ephemeral
                     || security::credential_store::is_not_ephemeral(c);
          };
          auto creds = _controller->get_credential_store().local().range(pred);

          std::vector<ss::sstring> users{};
          users.reserve(std::distance(creds.begin(), creds.end()));
          for (const auto& [user, type] : creds) {
              users.push_back(user());
          }
          return ss::make_ready_future<ss::json::json_return_type>(
            std::move(users));
      });

    // RBAC stubs

    register_route<user, true>(
      ss::httpd::security_json::list_user_roles,
      [this](
        std::unique_ptr<ss::http::request> req, request_auth_result auth_result)
        -> ss::future<ss::json::json_return_type> {
          return list_user_roles_handler(
            std::move(req), std::move(auth_result));
      });

    register_route<superuser>(
      ss::httpd::security_json::list_roles,
      []([[maybe_unused]] std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          ss::httpd::security_json::roles_list body;
          co_return ss::json::json_return_type(body);
      });

    register_route<superuser>(
      ss::httpd::security_json::create_role,
      [this](std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          return create_role_handler(std::move(req));
      });

    register_route<superuser>(
      ss::httpd::security_json::get_role,
      []([[maybe_unused]] std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          throw_role_exception(role_errc::role_not_found);
          co_return ss::json::json_return_type(ss::json::json_void());
      });

    register_route<superuser>(
      ss::httpd::security_json::update_role,
      []([[maybe_unused]] std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          throw_role_exception(role_errc::role_not_found);
          co_return ss::json::json_return_type(ss::json::json_void());
      });

    register_route<superuser>(
      ss::httpd::security_json::delete_role,
      []([[maybe_unused]] std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          throw_role_exception(role_errc::role_not_found);
          co_return ss::json::json_return_type(ss::json::json_void());
      });

    register_route<superuser>(
      ss::httpd::security_json::list_role_members,
      [this](std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          ss::sstring role_v;
          if (!admin::path_decode(req->param["role"], role_v)) {
              vlog(
                adminlog.debug,
                "Invalid parameter 'role' got {{{}}}",
                req->param["role"]);
              throw_role_exception(role_errc::invalid_name);
          }

          auto role_name = security::role_name{std::move(role_v)};
          auto role = _controller->get_role_store().local().get(role_name);
          if (!role.has_value()) {
              vlog(adminlog.debug, "Role '{}' does not exist", role_name);
              throw_role_exception(role_errc::role_not_found);
          }
          ss::httpd::security_json::role_members_list j_res;
          for (const auto& mem : role.value().members()) {
              j_res.members.push(role_member_to_json(mem));
          }

          return ssx::now(ss::json::json_return_type(j_res));
      });

    register_route<superuser>(
      ss::httpd::security_json::update_role_members,
      [this]([[maybe_unused]] std::unique_ptr<ss::http::request> req)
        -> ss::future<ss::json::json_return_type> {
          return update_role_members_handler(std::move(req));
      });
}

ss::future<ss::json::json_return_type>
admin_server::create_user_handler(std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    auto doc = co_await parse_json_body(req.get());

    auto credential = parse_scram_credential(doc);

    if (!doc.HasMember("username") || !doc["username"].IsString()) {
        throw ss::httpd::bad_request_exception(
          fmt::format("String username missing"));
    }

    auto username = security::credential_user(doc["username"].GetString());
    validate_no_control(username(), string_conversion_exception{username()});

    if (!security::validate_scram_username(username())) {
        throw ss::httpd::bad_request_exception(
          fmt::format("Invalid SCRAM username {{{}}}", username()));
    }

    if (is_no_op_user_write(
          _controller->get_credential_store().local(), username, credential)) {
        vlog(
          adminlog.debug,
          "User {} already exists with matching credential",
          username);
        co_return ss::json::json_return_type(ss::json::json_void());
    }

    auto err
      = co_await _controller->get_security_frontend().local().create_user(
        username, credential, model::timeout_clock::now() + 5s);
    vlog(
      adminlog.debug, "Creating user '{}' {}:{}", username, err, err.message());

    if (err == cluster::errc::user_exists) {
        // Idempotency: if user is same as one that already exists,
        // suppress the user_exists error and return success.
        const auto& credentials_store
          = _controller->get_credential_store().local();
        std::optional<security::scram_credential> creds
          = credentials_store.get<security::scram_credential>(username);
        if (creds.has_value() && match_scram_credential(doc, creds.value())) {
            co_return ss::json::json_return_type(ss::json::json_void());
        }
    }

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::delete_user_handler(std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    ss::sstring user_v;
    if (!admin::path_decode(req->param["user"], user_v)) {
        throw ss::httpd::bad_param_exception{fmt::format(
          "Invalid parameter 'user' got {{{}}}", req->param["user"])};
    }
    auto user = security::credential_user(user_v);

    if (!_controller->get_credential_store().local().contains(user)) {
        vlog(adminlog.debug, "User '{}' already gone during deletion", user);
        co_return ss::json::json_return_type(ss::json::json_void());
    }

    auto err
      = co_await _controller->get_security_frontend().local().delete_user(
        user, model::timeout_clock::now() + 5s);
    vlog(adminlog.debug, "Deleting user '{}' {}:{}", user, err, err.message());
    if (err == cluster::errc::user_does_not_exist) {
        // Idempotency: removing a non-existent user is successful.
        co_return ss::json::json_return_type(ss::json::json_void());
    }
    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::update_user_handler(std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    ss::sstring user_v;
    if (!admin::path_decode(req->param["user"], user_v)) {
        throw ss::httpd::bad_param_exception{fmt::format(
          "Invalid parameter 'user' got {{{}}}", req->param["user"])};
    }
    auto user = security::credential_user(user_v);

    auto doc = co_await parse_json_body(req.get());

    auto credential = parse_scram_credential(doc);

    if (is_no_op_user_write(
          _controller->get_credential_store().local(), user, credential)) {
        vlog(
          adminlog.debug,
          "User {} already exists with matching credential",
          user);
        co_return ss::json::json_return_type(ss::json::json_void());
    }

    auto err
      = co_await _controller->get_security_frontend().local().update_user(
        user, credential, model::timeout_clock::now() + 5s);
    vlog(adminlog.debug, "Updating user {}:{}", err, err.message());
    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::oidc_whoami_handler(std::unique_ptr<ss::http::request> req) {
    auto auth_hdr = req->get_header("authorization");
    if (!auth_hdr.starts_with(authz_bearer_prefix)) {
        throw ss::httpd::base_exception{
          "Invalid Authorization header",
          ss::http::reply::status_type::unauthorized};
    }

    security::oidc::authenticator auth{_controller->get_oidc_service().local()};
    auto res = auth.authenticate(auth_hdr.substr(authz_bearer_prefix.length()));

    if (res.has_error()) {
        throw ss::httpd::base_exception{
          "Invalid Authorization header",
          ss::http::reply::status_type::unauthorized};
    }

    ss::httpd::security_json::oidc_whoami_response j_res{};
    j_res.id = res.assume_value().principal.name();
    j_res.expire = res.assume_value().expiry.time_since_epoch() / 1s;

    co_return ss::json::json_return_type(j_res);
}

ss::future<ss::json::json_return_type>
admin_server::oidc_keys_cache_invalidate_handler(
  std::unique_ptr<ss::http::request>) {
    auto f = co_await ss::coroutine::as_future(
      _controller->get_oidc_service().invoke_on_all(
        [](auto& s) { return s.refresh_keys(); }));
    if (f.failed()) {
        ss::httpd::security_json::oidc_keys_cache_invalidate_error_response res;
        res.error_message = ssx::sformat("", f.get_exception());
        co_return ss::json::json_return_type(res);
    }
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type>
admin_server::oidc_revoke_handler(std::unique_ptr<ss::http::request>) {
    auto f = co_await ss::coroutine::as_future(
      _controller->get_oidc_service().invoke_on_all(
        [](auto& s) { return s.refresh_keys(); }));
    if (f.failed()) {
        ss::httpd::security_json::oidc_keys_cache_invalidate_error_response res;
        res.error_message = ssx::sformat("", f.get_exception());
        co_return ss::json::json_return_type(res);
    }
    co_await _kafka_server.invoke_on_all([](kafka::server& ks) {
        return ks.revoke_credentials(security::oidc::sasl_authenticator::name);
    });
    co_return ss::json::json_return_type(ss::json::json_void());
}

ss::future<ss::json::json_return_type> admin_server::list_user_roles_handler(
  std::unique_ptr<ss::http::request> req, request_auth_result auth_result) {
    ss::sstring filter = req->get_query_param("filter");

    security::role_member member{
      security::role_member_type::user, auth_result.get_username()};

    auto rng = _controller->get_role_store().local().range(
      [&filter, &member](const auto& e) {
          return security::role_store::has_member(e, member)
                 && security::role_store::name_prefix_filter(e, filter);
      });

    ss::httpd::security_json::roles_list body;
    std::for_each(rng.begin(), rng.end(), [&body](const auto& rn) {
        ss::httpd::security_json::role_description j_desc;
        j_desc.name = ss::sstring{rn()};
        body.roles.push(j_desc);
    });
    co_return ss::json::json_return_type(body);
}

ss::future<ss::json::json_return_type>
admin_server::create_role_handler(std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }
    auto doc = co_await parse_json_body(req.get());

    if (!doc.IsObject()) {
        vlog(adminlog.debug, "Request body is not a JSON object");
        throw_role_exception(
          role_errc::malformed_def, "Request body is not a JSON object");
    }

    if (!doc.HasMember("role") || !doc["role"].IsString()) {
        vlog(adminlog.debug, "String 'role' missing from request body");
        throw_role_exception(
          role_errc::malformed_def, "Missing string field 'role'");
    }

    auto role_name = security::role_name{doc["role"].GetString()};
    validate_no_control(role_name(), string_conversion_exception{role_name()});

    if (!security::validate_scram_username(role_name())) {
        throw_role_exception(role_errc::invalid_name);
    }

    ss::httpd::security_json::role_definition j_res;
    j_res.role = role_name();

    security::role role{};
    auto err
      = co_await _controller->get_security_frontend().local().create_role(
        role_name, role, model::timeout_clock::now() + 5s);

    if (err == cluster::errc::role_exists) {
        // Idempotency: if the empty role already exists,
        // suppress the role_exists error and return success.
        if (_controller->get_role_store().local().get(role_name) == role) {
            co_return ss::json::json_return_type(j_res);
        } else {
            throw_role_exception(role_errc::role_already_exists);
        }
    }
    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_return_type(j_res);
}

ss::future<ss::json::json_return_type>
admin_server::update_role_members_handler(
  std::unique_ptr<ss::http::request> req) {
    if (need_redirect_to_leader(model::controller_ntp, _metadata_cache)) {
        // In order that we can do a reliably ordered validation of
        // the request (and drop no-op requests), run on controller leader;
        throw co_await redirect_to_leader(*req, model::controller_ntp);
    }

    ss::sstring role_v;
    if (!admin::path_decode(req->param["role"], role_v)) {
        vlog(
          adminlog.debug,
          "Invalid parameter 'role' got {{{}}}",
          req->param["role"]);
        throw_role_exception(role_errc::invalid_name);
    }

    bool create_if_not_found = false;
    if (const auto it = req->query_parameters.find("create");
        it != req->query_parameters.end()) {
        auto param = it->second;
        absl::c_transform(param, param.begin(), ::tolower);
        std::istringstream(param) >> std::boolalpha >> create_if_not_found;
    }

    auto doc = co_await parse_json_body(req.get());
    if (!doc.IsObject()) {
        vlog(adminlog.debug, "Request body is not a JSON object");
        throw_role_exception(
          role_errc::malformed_def, "Request body is not a JSON object");
    }

    auto role_name = security::role_name(std::move(role_v));
    auto add = parse_json_members_list(doc, "add");
    auto remove = parse_json_members_list(doc, "remove");
    if (absl::c_any_of(remove, [&add](auto m) { return add.contains(m); })) {
        throw_role_exception(role_errc::member_list_conflict);
    }

    auto curr_members = _controller->get_role_store()
                          .local()
                          .get(role_name)
                          .value_or(security::role{})
                          .members();

    // Members diff accounting for the response body
    absl::erase_if(
      add, [&curr_members](const auto& m) { return curr_members.contains(m); });

    absl::erase_if(remove, [&curr_members](const auto& m) {
        return !curr_members.contains(m);
    });

    ss::httpd::security_json::role_member_update_response j_res;
    j_res.role = role_name();
    j_res.created = false;

    absl::c_for_each(add, [&curr_members, &j_res](const auto& a) {
        curr_members.insert(a);
        j_res.added.push(role_member_to_json(a));
    });

    absl::c_for_each(remove, [&curr_members, &j_res](const auto& r) {
        curr_members.erase(r);
        j_res.removed.push(role_member_to_json(r));
    });

    auto err
      = co_await _controller->get_security_frontend().local().update_role(
        role_name,
        security::role{curr_members},
        model::timeout_clock::now() + 5s);
    if (err == cluster::errc::role_does_not_exist && create_if_not_found) {
        j_res.created = true;
        err = co_await _controller->get_security_frontend().local().create_role(
          role_name,
          security::role{std::move(curr_members)},
          model::timeout_clock::now() + 5s);
    }

    throw_on_role_command_err(err);

    vlog(
      adminlog.debug,
      "{} role '{}' {}:{}",
      j_res.created() ? "Creating" : "Updating",
      role_name(),
      err,
      err.message());

    co_await throw_on_error(*req, err, model::controller_ntp);
    co_return ss::json::json_return_type(j_res);
}
