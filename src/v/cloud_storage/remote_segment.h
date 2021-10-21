/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "cloud_storage/cache_service.h"
#include "cloud_storage/logger.h"
#include "cloud_storage/remote.h"
#include "cloud_storage/types.h"
#include "model/fundamental.h"
#include "model/record.h"
#include "s3/client.h"
#include "storage/parser.h"
#include "storage/types.h"
#include "utils/retry_chain_node.h"

#include <seastar/core/circular_buffer.hh>
#include <seastar/core/io_priority_class.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>

namespace cloud_storage {

static constexpr size_t max_index_error_bytes = 32_KiB;
class download_exception : public std::exception {
public:
    explicit download_exception(download_result r, std::filesystem::path p);

    const char* what() const noexcept override;

    const download_result result;
    std::filesystem::path path;
};

class remote_segment_exception : public std::runtime_error {
public:
    explicit remote_segment_exception(const char* m)
      : std::runtime_error(m) {}
};

class remote_segment final {
public:
    remote_segment(
      remote& r,
      cache& cache,
      s3::bucket_name bucket,
      const manifest& m,
      manifest::key path,
      retry_chain_node& parent);

    const model::ntp& get_ntp() const;

    const model::term_id get_term() const;

    /// Get max offset of the segment (redpada offset)
    const model::offset get_max_rp_offset() const;

    /// Number of non-data batches in all previous
    /// segments
    const model::offset get_base_offset_delta() const;

    /// Get base offset of the segment (redpanda offset)
    const model::offset get_base_rp_offset() const;

    /// Get base offset of the segment (kafka offset)
    const model::offset get_base_kafka_offset() const;

    ss::future<> stop();

    /// create an input stream _sharing_ the underlying file handle
    /// starting at position @pos
    ss::future<ss::input_stream<char>>
    data_stream(size_t pos, const ss::io_priority_class&);

    /// Hydrate the segment
    ///
    /// Method returns key of the segment in cache.
    ss::future<std::filesystem::path> hydrate();

private:
    ss::gate _gate;
    remote& _api;
    cache& _cache;
    s3::bucket_name _bucket;
    const manifest& _manifest;
    manifest::key _path;
    retry_chain_node _rtc;
    mutable retry_chain_logger _ctxlog;
    ss::abort_source _as;
};

struct log_reader_config : public storage::log_reader_config {
    explicit log_reader_config(const storage::log_reader_config& cfg)
      : storage::log_reader_config(cfg)
      , start_offset_redpanda(model::offset::min()) {}

    log_reader_config(
      model::offset start_offset,
      model::offset max_offset,
      ss::io_priority_class prio)
      : storage::log_reader_config(start_offset, max_offset, prio) {}

    /// Same as started_offset but not translated to kafka
    model::offset start_offset_redpanda;
};

class remote_segment_batch_consumer;

/// The segment reader that can be used to fetch data from cloud storage
///
/// The reader invokes 'data_stream' method of the 'remote_segment'
/// which returns hydrated segment from disk.
class remote_segment_batch_reader final {
    friend class remote_segment_batch_consumer;

public:
    remote_segment_batch_reader(
      ss::lw_shared_ptr<remote_segment>,
      log_reader_config& config,
      model::term_id term) noexcept;

    remote_segment_batch_reader(
      remote_segment_batch_reader&&) noexcept = default;
    remote_segment_batch_reader&
    operator=(remote_segment_batch_reader&&) noexcept = delete;
    remote_segment_batch_reader(const remote_segment_batch_reader&) = delete;
    remote_segment_batch_reader& operator=(const remote_segment_batch_reader&)
      = delete;
    ~remote_segment_batch_reader() noexcept = default;

    ss::future<result<ss::circular_buffer<model::record_batch>>>
      read_some(model::timeout_clock::time_point);

    ss::future<> stop();

    /// Get max offset (redpanda offset)
    model::offset max_rp_offset() const { return _seg->get_max_rp_offset(); }

    /// Get base offset (redpanda offset)
    model::offset base_rp_offset() const { return _seg->get_base_rp_offset(); }

private:
    friend class single_record_consumer;
    ss::future<std::unique_ptr<storage::continuous_batch_parser>> init_parser();

    size_t produce(model::record_batch batch);

    ss::lw_shared_ptr<remote_segment> _seg;
    log_reader_config& _config;
    std::unique_ptr<storage::continuous_batch_parser> _parser;
    bool _done{false};
    ss::circular_buffer<model::record_batch> _ringbuf;
    size_t _total_size{0};
    model::term_id _term;
    model::offset _initial_delta;
};

} // namespace cloud_storage
