/*
 * Copyright 2023 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "json/json.h"
#include "json/stringbuffer.h"
#include "seastarx.h"
#include "utils/named_type.h"
#include "version.h"

#include <seastar/core/sstring.hh>

#include <string_view>

namespace security::audit {
static constexpr std::string_view ocsf_api_version = "1.0.0";
static constexpr std::string_view vendor_name = "Redpanda Data, Inc.";

using port_t = named_type<uint16_t, struct port_t_type>;
// OCSF defines timestamp as a signed long (64-bit) value that holds
// milliseconds since Unix epoch
using timestamp_t = named_type<int64_t, struct timestamp_t_type>;
// OCSF defines type as a signed integer
using type_uid = named_type<int32_t, struct type_uid_type>;

template<typename T>
concept has_equality_fields = requires(T t) { t.equality_fields(); };

// Defines the category of the event
// https://schema.ocsf.io/
enum class category_uid : uint8_t {
    system_activity = 1,
    findings = 2,
    iam = 3,
    network_activity = 4,
    discovery = 5,
    application_activity = 6
};

std::ostream& operator<<(std::ostream&, const category_uid&);

// Defines the class of the event
// https://schema.ocsf.io/
enum class class_uid : uint16_t {
    file_system_activity = 1001,
    kernel_extension_activity = 1002,
    kernel_activity = 1003,
    memory_activity = 1004,
    module_activity = 1005,
    scheduled_job_activity = 1006,
    process_activity = 1007,
    security_finding = 2001,
    account_change = 3001,
    authentication = 3002,
    authorize_session = 3003,
    entity_management = 3004,
    user_access_management = 3005,
    group_management = 3006,
    network_activity = 4001,
    http_activity = 4002,
    dns_activity = 4003,
    dhcp_activity = 4004,
    rdp_activity = 4005,
    smb_activity = 4006,
    ssh_activity = 4007,
    ftp_activity = 4008,
    email_activity = 4009,
    network_file_activity = 4010,
    email_file_activity = 4011,
    email_url_activity = 4012,
    device_inventory_info = 5001,
    device_config_state = 5002,
    web_resource_activity = 6001,
    application_lifecycle = 6002,
    api_activity = 6003,
    web_resource_access_activity = 6004
};

std::ostream& operator<<(std::ostream&, const class_uid&);

// Severity of the event
// Each class defines the same severity fields
enum class severity_id : uint8_t {
    unknown = 0,
    informational = 1,
    low = 2,
    medium = 3,
    high = 4,
    critical = 5,
    fatal = 6,
    other = 99,
};

// Characteristics of a service
// https://schema.ocsf.io/1.0.0/objects/service?extensions=
struct service {
    ss::sstring name;

    auto equality_fields() const { return std::tie(name); }
};

// Information pertaining to an API request and response
// https://schema.ocsf.io/1.0.0/objects/api?extensions=
struct api {
    ss::sstring operation;
    service service;

    auto equality_fields() const { return std::tie(operation, service); }
};

// Information about the software product feature that generated the event
// https://schema.ocsf.io/1.0.0/objects/feature?extensions=
struct feature {
    ss::sstring name;

    auto equality_fields() const { return std::tie(name); }
};

// Characteristics of a software product
// https://schema.ocsf.io/1.0.0/objects/product?extensions=
struct product {
    ss::sstring name;
    ss::sstring uid;
    ss::sstring vendor_name;
    ss::sstring version;
    std::optional<feature> feature;

    auto equality_fields() const {
        return std::tie(name, uid, vendor_name, version, feature);
    }
};

// Defines the characteristics for the Redpanda product
static inline const product& redpanda_product() {
    static const product redpanda_product_struct{
      .name = "Redpanda",
      .vendor_name = ss::sstring{vendor_name},
      .version = ss::sstring{redpanda_git_version()}};
    return redpanda_product_struct;
}

// Metadata associated with the event
// https://schema.ocsf.io/1.0.0/objects/metadata?extensions=
struct metadata {
    product product;
    std::vector<ss::sstring> profiles;
    ss::sstring version;

    auto equality_fields() const {
        return std::tie(product, profiles, version);
    }
};

// Defines the static OCSF metadata for events generated by Redpanda
static inline const metadata& ocsf_redpanda_metadata() {
    static const metadata ocsf_metadata{
      .product = redpanda_product(), .version = ss::sstring{ocsf_api_version}};

    return ocsf_metadata;
}

// Defines the static OCSF metadata for events generated by Redpanda using the
// cloud profile
static inline const metadata& ocsf_redpanda_metadata_cloud_profile() {
    static const metadata ocsf_metadata{
      .product = redpanda_product(),
      .profiles = {"cloud"},
      .version = ss::sstring{ocsf_api_version}};

    return ocsf_metadata;
}

// Characteristics of a network endpoint
// https://schema.ocsf.io/1.0.0/objects/network_endpoint?extensions=
struct network_endpoint {
    std::vector<ss::sstring> intermediate_ips;
    net::unresolved_address addr;
    ss::sstring name;
    ss::sstring svc_name;
    ss::sstring uid;
};

// The applicable policies
// https://schema.ocsf.io/1.0.0/objects/policy?extensions=
struct policy {
    ss::sstring desc;
    ss::sstring name;

    auto equality_fields() const { return std::tie(desc, name); }
};

// Details about an authorization outcome and associated policies
// https://schema.ocsf.io/1.0.0/objects/authorization?extensions=
struct authorization_result {
    ss::sstring decision;
    std::optional<policy> policy;

    auto equality_fields() const { return std::tie(decision, policy); }
};

// Characteristics of a user/person or security principal
// https://schema.ocsf.io/1.0.0/objects/user?extensions=
struct user {
    enum class type : int {
        unknown = 0,
        user = 1,
        admin = 2,
        system = 3,
        other = 99
    };

    ss::sstring domain;
    ss::sstring name;
    type type_id{type::unknown};
    ss::sstring uid;

    auto equality_fields() const {
        return std::tie(domain, name, type_id, uid);
    }
};

// Details about a user, role or process that initiated or performed an activity
// https://schema.ocsf.io/1.0.0/objects/actor?extensions=
struct actor {
    std::vector<authorization_result> authorizations;
    user user;

    auto equality_fields() const { return std::tie(authorizations, user); }
};

// Details about an ACL binding
struct acl_binding_detail {
    std::optional<ss::sstring> resource_type;
    std::optional<ss::sstring> resource_name;
    std::optional<ss::sstring> pattern_type;
    std::optional<ss::sstring> acl_principal;
    std::optional<ss::sstring> acl_host;
    std::optional<ss::sstring> acl_operation;
    std::optional<ss::sstring> acl_permission;

    auto equality_fields() const {
        return std::tie(
          resource_type,
          resource_name,
          pattern_type,
          acl_principal,
          acl_host,
          acl_operation,
          acl_permission);
    }
};

// Details about a resource
struct resource_detail {
    ss::sstring name;
    ss::sstring type;
    std::optional<acl_binding_detail> data;

    auto equality_fields() const { return std::tie(name, type, data); }
};

// Characteristics about an authorization event that used ACLs
struct authorization_metadata {
    struct {
        ss::sstring host;
        ss::sstring op;
        ss::sstring permission_type;
        ss::sstring principal;
    } acl_authorization;

    struct {
        ss::sstring name;
        ss::sstring pattern;
        ss::sstring type;
    } resource;

    auto equality_fields() const {
        return std::tie(
          acl_authorization.host,
          acl_authorization.op,
          acl_authorization.permission_type,
          acl_authorization.principal,
          resource.name,
          resource.pattern,
          resource.type);
    }
};

// Defines the contents of the unmapped field for API activity events
struct api_activity_unmapped {
    std::optional<authorization_metadata> authorization_metadata;

    auto equality_fields() const { return std::tie(authorization_metadata); }
};

// Headers sent in an HTTP request or response
// https://schema.ocsf.io/1.0.0/objects/http_header?extensions=
struct http_header {
    ss::sstring name;
    ss::sstring value;

    auto equality_fields() const { return std::tie(name, value); }
};

// Characteristics of a URL
// https://schema.ocsf.io/1.0.0/objects/url?extensions=
struct uniform_resource_locator {
    ss::sstring hostname;
    ss::sstring path;
    port_t port;
    ss::sstring scheme;
    ss::sstring url_string;

    auto equality_fields() const {
        return std::tie(hostname, path, port, scheme, url_string);
    }
};

// Attributes of a request made to a webserver
// https://schema.ocsf.io/1.0.0/objects/http_request?extensions=
struct http_request {
    std::vector<http_header> http_headers;
    ss::sstring http_method;
    uniform_resource_locator url;
    ss::sstring user_agent;
    ss::sstring version;

    auto equality_fields() const {
        return std::tie(http_headers, http_method, url, user_agent, version);
    }
};

// Information about a cloud account
// https://schema.ocsf.io/1.0.0/objects/cloud?extensions=
struct cloud {
    ss::sstring provider;

    auto equality_fields() const { return std::tie(provider); }
};
} // namespace security::audit

namespace json {
namespace sa = security::audit;

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::service& service) {
    w.StartObject();
    w.Key("name");
    rjson_serialize(w, service.name);
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::api& api) {
    w.StartObject();
    w.Key("operation");
    rjson_serialize(w, api.operation);
    w.Key("service");
    rjson_serialize(w, api.service);
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::feature& f) {
    w.StartObject();
    w.Key("name");
    rjson_serialize(w, f.name);
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::product& p) {
    w.StartObject();
    if (p.feature.has_value()) {
        w.Key("feature");
        rjson_serialize(w, p.feature.value());
    }
    w.Key("name");
    rjson_serialize(w, p.name);
    w.Key("uid");
    rjson_serialize(w, p.uid);
    w.Key("vendor_name");
    rjson_serialize(w, p.vendor_name);
    w.Key("version");
    rjson_serialize(w, p.version);
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::metadata& m) {
    w.StartObject();
    w.Key("product");
    rjson_serialize(w, m.product);
    if (!m.profiles.empty()) {
        w.Key("profiles");
        rjson_serialize(w, m.profiles);
    }
    w.Key("version");
    rjson_serialize(w, m.version);
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::network_endpoint& n) {
    w.StartObject();
    if (!n.intermediate_ips.empty()) {
        w.Key("intermediate_ips");
        rjson_serialize(w, n.intermediate_ips);
    }
    w.Key("ip");
    rjson_serialize(w, n.addr.host());
    if (!n.name.empty()) {
        w.Key("name");
        rjson_serialize(w, n.name);
    }
    w.Key("port");
    rjson_serialize(w, n.addr.port());
    if (!n.svc_name.empty()) {
        w.Key("svc_name");
        rjson_serialize(w, n.svc_name);
    }
    if (!n.uid.empty()) {
        w.Key("uid");
        rjson_serialize(w, n.uid);
    }
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::policy& p) {
    w.StartObject();
    w.Key("desc");
    rjson_serialize(w, p.desc);
    w.Key("name");
    rjson_serialize(w, p.name);
    w.EndObject();
}

inline void rjson_serialize(
  Writer<StringBuffer>& w, const sa::authorization_result& authz) {
    w.StartObject();
    w.Key("decision");
    rjson_serialize(w, authz.decision);
    if (authz.policy) {
        w.Key("policy");
        rjson_serialize(w, authz.policy);
    }

    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::user& user) {
    w.StartObject();
    if (!user.domain.empty()) {
        w.Key("domain");
        rjson_serialize(w, user.domain);
    }
    w.Key("name");
    rjson_serialize(w, user.name);
    w.Key("type_id");
    rjson_serialize(w, user.type_id);
    if (!user.uid.empty()) {
        w.Key("uid");
        rjson_serialize(w, user.uid);
    }
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::actor& actor) {
    w.StartObject();
    w.Key("authorizations");
    rjson_serialize(w, actor.authorizations);
    w.Key("user");
    rjson_serialize(w, actor.user);
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::acl_binding_detail& acl) {
    w.StartObject();
    if (acl.resource_type.has_value()) {
        w.Key("resource_type");
        rjson_serialize(w, acl.resource_type);
    }
    if (acl.resource_name.has_value()) {
        w.Key("resource_name");
        rjson_serialize(w, acl.resource_name);
    }
    if (acl.pattern_type.has_value()) {
        w.Key("pattern_type");
        rjson_serialize(w, acl.pattern_type);
    }
    if (acl.acl_principal.has_value()) {
        w.Key("acl_principal");
        rjson_serialize(w, acl.acl_principal);
    }
    if (acl.acl_host.has_value()) {
        w.Key("acl_host");
        rjson_serialize(w, acl.acl_host);
    }
    if (acl.acl_operation.has_value()) {
        w.Key("acl_operation");
        rjson_serialize(w, acl.acl_operation);
    }
    if (acl.acl_permission.has_value()) {
        w.Key("acl_permission");
        rjson_serialize(w, acl.acl_permission);
    }
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::resource_detail& resource) {
    w.StartObject();
    w.Key("name");
    rjson_serialize(w, resource.name);
    w.Key("type");
    rjson_serialize(w, resource.type);
    if (resource.data.has_value()) {
        w.Key("data");
        rjson_serialize(w, *resource.data);
    }
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::authorization_metadata& m) {
    w.StartObject();
    w.Key("acl_authorization");
    w.StartObject();
    w.Key("host");
    ::json::rjson_serialize(w, m.acl_authorization.host);
    w.Key("op");
    ::json::rjson_serialize(w, m.acl_authorization.op);
    w.Key("permission_type");
    ::json::rjson_serialize(w, m.acl_authorization.permission_type);
    w.Key("principal");
    ::json::rjson_serialize(w, m.acl_authorization.principal);
    w.EndObject();
    w.Key("resource");
    w.StartObject();
    w.Key("name");
    ::json::rjson_serialize(w, m.resource.name);
    w.Key("pattern");
    ::json::rjson_serialize(w, m.resource.pattern);
    w.Key("type");
    ::json::rjson_serialize(w, m.resource.type);
    w.EndObject();
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::api_activity_unmapped& u) {
    w.StartObject();
    if (u.authorization_metadata) {
        w.Key("authorization_metadata");
        ::json::rjson_serialize(w, u.authorization_metadata.value());
    }
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::http_header& h) {
    w.StartObject();
    w.Key("name");
    json::rjson_serialize(w, h.name);
    w.Key("value");
    json::rjson_serialize(w, h.value);
    w.EndObject();
}

inline void rjson_serialize(
  Writer<StringBuffer>& w, const sa::uniform_resource_locator& url) {
    w.StartObject();
    w.Key("hostname");
    rjson_serialize(w, url.hostname);
    w.Key("path");
    rjson_serialize(w, url.path);
    w.Key("port");
    rjson_serialize(w, url.port);
    w.Key("scheme");
    rjson_serialize(w, url.scheme);
    w.Key("url_string");
    rjson_serialize(w, url.url_string);
    w.EndObject();
}

inline void
rjson_serialize(Writer<StringBuffer>& w, const sa::http_request& r) {
    w.StartObject();
    w.Key("http_headers");
    rjson_serialize(w, r.http_headers);
    w.Key("http_method");
    rjson_serialize(w, r.http_method);
    w.Key("url");
    rjson_serialize(w, r.url);
    w.Key("user_agent");
    rjson_serialize(w, r.user_agent);
    w.Key("version");
    rjson_serialize(w, r.version);
    w.EndObject();
}

inline void rjson_serialize(Writer<StringBuffer>& w, const sa::cloud& c) {
    w.StartObject();
    w.Key("provider");
    rjson_serialize(w, c.provider);
    w.EndObject();
}
} // namespace json
