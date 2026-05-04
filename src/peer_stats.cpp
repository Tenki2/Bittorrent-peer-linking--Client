#include "peer_stats.hpp"

#include <libtorrent/time.hpp>

#include <algorithm>

namespace lt = libtorrent;

namespace btclient {

namespace {

constexpr double kIncomingRequestBurstGapMs = 100.0;

json optional_double(std::optional<double> const& value)
{
    if (!value.has_value()) return nullptr;
    return *value;
}

json optional_string(std::string const& value)
{
    if (value.empty()) return nullptr;
    return value;
}

std::vector<std::string> sorted_strings(std::set<std::string> const& values)
{
    return std::vector<std::string>(values.begin(), values.end());
}

std::int64_t connected_duration_ns(PeerStats const& peer)
{
    std::int64_t const start = peer.connected_mono_ns != 0 ? peer.connected_mono_ns : peer.first_seen_mono_ns;
    std::int64_t const end = peer.disconnected_mono_ns != 0 ? peer.disconnected_mono_ns : peer.last_seen_mono_ns;
    if (start == 0 || end <= start) return 0;
    return end - start;
}

bool peer_has_recent_activity(
    lt::peer_info const& peer,
    double download_rate,
    double upload_rate,
    std::int64_t snapshot_interval_ms)
{
    return download_rate > 0.0
        || upload_rate > 0.0
        || peer.download_queue_length > 0
        || peer.upload_queue_length > 0
        || lt::total_milliseconds(peer.last_active) <= snapshot_interval_ms * 2;
}

bool state_implies_network_piece_completion(lt::torrent_status::state_t state)
{
    using state_t = lt::torrent_status::state_t;
    switch (state)
    {
        case state_t::downloading:
        case state_t::finished:
        case state_t::seeding:
            return true;
        default:
            return false;
    }
}

int block_index_from_request(lt::peer_request const& request)
{
    if (request.length <= 0) return 0;
    return request.start / request.length;
}

}  // namespace

bool RequestKey::operator<(RequestKey const& other) const
{
    if (piece_index != other.piece_index) return piece_index < other.piece_index;
    return block_index < other.block_index;
}

PeerStats::PeerStats(
    std::string peer_key_value,
    std::string endpoint_key_value,
    std::string peer_ip_value,
    int peer_port_value,
    TimestampPair const& ts,
    std::string direction_value,
    std::string transport_value)
    : peer_key(std::move(peer_key_value))
    , endpoint_key(std::move(endpoint_key_value))
    , peer_ip(std::move(peer_ip_value))
    , peer_port(peer_port_value)
    , direction(std::move(direction_value))
    , transport(std::move(transport_value))
    , first_seen_utc(ts.utc_ts)
    , last_seen_utc(ts.utc_ts)
    , connected_utc(ts.utc_ts)
    , first_seen_mono_ns(ts.mono_ns)
    , last_seen_mono_ns(ts.mono_ns)
    , connected_mono_ns(ts.mono_ns)
    , last_activity_window_mono_ns(ts.mono_ns)
{
}

EventPeerContext PeerStats::event_context() const
{
    EventPeerContext context;
    context.peer_key = peer_key;
    context.peer_ip = peer_ip;
    context.peer_port = peer_port;
    context.direction = direction;
    context.transport = transport;
    return context;
}

void PeerStats::mark_seen(TimestampPair const& ts)
{
    if (first_seen_mono_ns == 0)
    {
        first_seen_mono_ns = ts.mono_ns;
        first_seen_utc = ts.utc_ts;
    }

    last_seen_mono_ns = ts.mono_ns;
    last_seen_utc = ts.utc_ts;
}

void PeerStats::note_connect(
    TimestampPair const& ts,
    std::string const& direction_value,
    std::string const& transport_value,
    bool from_connect_alert)
{
    mark_seen(ts);

    // Snapshot data can arrive before the explicit connect alert. Keep early
    // guesses only until the alert gives us the authoritative direction/socket.
    if (!from_connect_alert || !saw_connect_alert)
    {
        if (!direction_value.empty() && direction == "unknown") direction = direction_value;
        if (!transport_value.empty() && transport == "unknown") transport = transport_value;
    }

    active_connection = true;
    if (connected_mono_ns == 0)
    {
        connected_mono_ns = ts.mono_ns;
        connected_utc = ts.utc_ts;
    }

    if (!last_activity_window_mono_ns.has_value())
    {
        last_activity_window_mono_ns = ts.mono_ns;
    }

    if (from_connect_alert)
    {
        saw_connect_alert = true;
        direction = direction_value;
        transport = transport_value;
    }
}

void PeerStats::note_disconnect(TimestampPair const& ts)
{
    mark_seen(ts);
    flush_incoming_request_burst();
    advance_activity_window(ts.mono_ns, false);
    disconnected_mono_ns = ts.mono_ns;
    disconnected_utc = ts.utc_ts;
    active_connection = false;
    outstanding_requests.clear();
    pending_upload_requests.clear();
    record_outstanding_depth(0.0);
}

void PeerStats::note_peer_id(lt::peer_id const& pid)
{
    std::string const hex = peer_id_to_hex(pid);
    if (hex == "unknown") return;

    peer_id_hex = hex;
    std::string const prefix = peer_id_prefix(pid);
    if (!prefix.empty()) peer_id_prefix_hex = prefix;
}

void PeerStats::note_request_sent(TimestampPair const& ts, int piece_index, int block_index)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++requests_sent;

    // Track both pacing between requests and the latency of each outstanding
    // piece/block request once its response arrives.
    if (last_request_sent_mono_ns.has_value())
    {
        request_gap_ms_samples.push_back(
            ns_to_ms(ts.mono_ns - *last_request_sent_mono_ns));
    }
    else if (connected_mono_ns != 0)
    {
        first_request_delay_ms = ns_to_ms(ts.mono_ns - connected_mono_ns);
    }

    last_request_sent_mono_ns = ts.mono_ns;
    outstanding_requests[{piece_index, block_index}] = ts.mono_ns;
    record_outstanding_depth(static_cast<double>(outstanding_requests.size()));
}

void PeerStats::note_block_response(TimestampPair const& ts, int piece_index, int block_index)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++block_responses_received;

    RequestKey const key{piece_index, block_index};
    auto const it = outstanding_requests.find(key);
    if (it != outstanding_requests.end())
    {
        block_latency_ms_samples.push_back(ns_to_ms(ts.mono_ns - it->second));
        outstanding_requests.erase(it);
    }

    record_outstanding_depth(static_cast<double>(outstanding_requests.size()));
}

void PeerStats::note_timeout(TimestampPair const& ts, int piece_index, int block_index)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++request_timeouts;
    outstanding_requests.erase({piece_index, block_index});
    record_outstanding_depth(static_cast<double>(outstanding_requests.size()));
}

void PeerStats::note_drop(TimestampPair const& ts, int piece_index, int block_index)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++requests_dropped;
    outstanding_requests.erase({piece_index, block_index});
    record_outstanding_depth(static_cast<double>(outstanding_requests.size()));
}

void PeerStats::note_incoming_request(TimestampPair const& ts, lt::peer_request const& request)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++incoming_requests_received;

    if (last_incoming_request_mono_ns.has_value())
    {
        incoming_request_gap_ms_samples.push_back(
            ns_to_ms(ts.mono_ns - *last_incoming_request_mono_ns));
    }
    else if (connected_mono_ns != 0)
    {
        first_incoming_request_delay_ms = ns_to_ms(ts.mono_ns - connected_mono_ns);
    }

    last_incoming_request_mono_ns = ts.mono_ns;
    note_incoming_request_burst(ts.mono_ns);
    pending_upload_requests[{static_cast<int>(request.piece), block_index_from_request(request)}]
        .push_back(ts.mono_ns);
}

void PeerStats::note_block_uploaded(TimestampPair const& ts, int piece_index, int block_index)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++blocks_uploaded;

    if (last_block_uploaded_mono_ns.has_value())
    {
        block_upload_gap_ms_samples.push_back(
            ns_to_ms(ts.mono_ns - *last_block_uploaded_mono_ns));
    }
    else if (connected_mono_ns != 0)
    {
        first_block_upload_delay_ms = ns_to_ms(ts.mono_ns - connected_mono_ns);
    }

    last_block_uploaded_mono_ns = ts.mono_ns;

    RequestKey const key{piece_index, block_index};
    auto request_it = pending_upload_requests.find(key);
    if (request_it != pending_upload_requests.end() && !request_it->second.empty())
    {
        std::int64_t const request_mono_ns = request_it->second.front();
        request_it->second.erase(request_it->second.begin());
        if (ts.mono_ns >= request_mono_ns)
        {
            request_to_upload_latency_ms_samples.push_back(
                ns_to_ms(ts.mono_ns - request_mono_ns));
        }
        if (request_it->second.empty())
        {
            pending_upload_requests.erase(request_it);
        }
    }
}

void PeerStats::note_snubbed(TimestampPair const& ts)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++snubbed_count;
    snubbed = true;
}

void PeerStats::note_unsnubbed(TimestampPair const& ts)
{
    mark_seen(ts);
    note_interaction(ts.mono_ns);
    ++unsnubbed_count;
    snubbed = false;
}

void PeerStats::update_from_snapshot(
    lt::peer_info const& peer,
    TimestampPair const& ts,
    std::int64_t snapshot_interval_ms)
{
    mark_seen(ts);
    note_peer_id(peer.pid);

    // Snapshots fill in details that individual alerts often do not include.
    if (!peer.client.empty()) client_name = peer.client;
    if (direction == "unknown")
    {
        if (has_flag(peer.flags, lt::peer_info::outgoing_connection))
        {
            direction = "outbound";
        }
        else if (has_flag(peer.source, lt::peer_info::incoming))
        {
            direction = "inbound";
        }
    }
    if (transport == "unknown")
    {
        transport = transport_from_peer_info(peer);
    }

    for (std::string const& label : peer_flag_labels(peer))
    {
        observed_flags.insert(label);
    }
    for (std::string const& label : peer_source_labels(peer))
    {
        observed_sources.insert(label);
    }

    total_download_bytes = peer.total_download;
    total_upload_bytes = peer.total_upload;

    double const download_rate = peer.payload_down_speed > 0
        ? static_cast<double>(peer.payload_down_speed)
        : static_cast<double>(peer.down_speed);
    double const upload_rate = peer.payload_up_speed > 0
        ? static_cast<double>(peer.payload_up_speed)
        : static_cast<double>(peer.up_speed);

    download_rate_samples.push_back(download_rate);
    upload_rate_samples.push_back(upload_rate);
    peak_download_rate_Bps = (std::max)(peak_download_rate_Bps,
        (std::max)(download_rate, static_cast<double>(peer.download_rate_peak)));
    peak_upload_rate_Bps = (std::max)(peak_upload_rate_Bps,
        (std::max)(upload_rate, static_cast<double>(peer.upload_rate_peak)));

    double const outstanding_depth = (std::max)(
        static_cast<double>(outstanding_requests.size()),
        static_cast<double>(peer.download_queue_length));
    record_outstanding_depth(outstanding_depth);

    bool const remote_choked_now = has_flag(peer.flags, lt::peer_info::remote_choked);
    bool const local_choked_now = has_flag(peer.flags, lt::peer_info::choked);
    bool const snubbed_now = has_flag(peer.flags, lt::peer_info::snubbed);

    if (state_flags_initialized)
    {
        if (!remote_choked && remote_choked_now) ++choked_count;
        if (remote_choked && !remote_choked_now) ++unchoked_count;
        if (!local_choked && local_choked_now) ++we_choked_count;
        if (local_choked && !local_choked_now) ++we_unchoked_count;
    }
    else
    {
        state_flags_initialized = true;
    }

    remote_choked = remote_choked_now;
    local_choked = local_choked_now;
    snubbed = snubbed_now;
    interesting = has_flag(peer.flags, lt::peer_info::interesting);
    remote_interested = has_flag(peer.flags, lt::peer_info::remote_interested);
    seed = has_flag(peer.flags, lt::peer_info::seed);

    bool const active_now = peer_has_recent_activity(
        peer,
        download_rate,
        upload_rate,
        snapshot_interval_ms);

    advance_activity_window(ts.mono_ns, active_now);

    if (active_now)
    {
        if (!last_interaction_mono_ns.has_value()
            || ts.mono_ns - *last_interaction_mono_ns
                >= snapshot_interval_ms * 1'000'000)
        {
            note_interaction(ts.mono_ns);
        }
    }
}

void PeerStats::finalize(TimestampPair const& session_end)
{
    flush_incoming_request_burst();
    if (active_connection)
    {
        note_disconnect(session_end);
    }
    else if (last_activity_window_mono_ns.has_value())
    {
        advance_activity_window(session_end.mono_ns, false);
    }
}

void PeerStats::reset_outstanding_requests()
{
    outstanding_requests.clear();
    pending_upload_requests.clear();
    record_outstanding_depth(0.0);
}

json PeerStats::to_json() const
{
    DistributionSummary const request_gap_summary = summarize_samples(request_gap_ms_samples);
    DistributionSummary const latency_summary = summarize_samples(block_latency_ms_samples);
    DistributionSummary const incoming_request_gap_summary =
        summarize_samples(incoming_request_gap_ms_samples);
    DistributionSummary const block_upload_gap_summary =
        summarize_samples(block_upload_gap_ms_samples);
    DistributionSummary const request_to_upload_latency_summary =
        summarize_samples(request_to_upload_latency_ms_samples);
    DistributionSummary const incoming_request_burst_size_summary =
        summarize_samples(incoming_request_burst_size_samples);
    DistributionSummary const incoming_request_burst_duration_summary =
        summarize_samples(incoming_request_burst_duration_ms_samples);
    DistributionSummary const incoming_request_burst_idle_gap_summary =
        summarize_samples(incoming_request_burst_idle_gap_ms_samples);
    DistributionSummary const interaction_gap_summary = summarize_samples(interaction_gap_ms_samples);
    DistributionSummary const download_rate_summary = summarize_samples(download_rate_samples);
    DistributionSummary const upload_rate_summary = summarize_samples(upload_rate_samples);
    DistributionSummary const outstanding_depth_summary = summarize_samples(outstanding_depth_samples);
    double const observation_duration_ms = ns_to_ms(connected_duration_ns(*this));
    double const sample_score = (std::min)(
        1.0,
        static_cast<double>(requests_sent + incoming_requests_received) / 1000.0);
    double const duration_score = (std::min)(1.0, observation_duration_ms / 30000.0);
    double const observation_quality_score = sample_score * duration_score;

    json state_transitions = {
        {"peer_choked_us", choked_count},
        {"peer_unchoked_us", unchoked_count},
        {"we_choked_peer", we_choked_count},
        {"we_unchoked_peer", we_unchoked_count},
        {"peer_snubbed", snubbed_count},
        {"peer_unsnubbed", unsnubbed_count},
    };

    return {
        {"peer_key", peer_key},
        {"peer_ip", peer_ip},
        {"peer_port", peer_port},
        {"direction", direction},
        {"transport", transport},
        {"peer_id_hex", optional_string(peer_id_hex == "unknown" ? "" : peer_id_hex)},
        {"peer_id_prefix", optional_string(peer_id_prefix_hex)},
        {"client", optional_string(client_name)},
        {"capability_flags", strings_to_json_array(sorted_strings(observed_flags))},
        {"peer_sources", strings_to_json_array(sorted_strings(observed_sources))},
        {"first_seen_utc", optional_string(first_seen_utc)},
        {"last_seen_utc", optional_string(last_seen_utc)},
        {"connected_utc", optional_string(connected_utc)},
        {"disconnected_utc", optional_string(disconnected_utc)},
        {"first_seen_mono_ns", first_seen_mono_ns},
        {"last_seen_mono_ns", last_seen_mono_ns},
        {"total_connected_duration_ms", ns_to_ms(connected_duration_ns(*this))},
        {"connection_duration_ms", ns_to_ms(connected_duration_ns(*this))},
        {"first_request_delay_ms", optional_double(first_request_delay_ms)},
        {"first_incoming_request_delay_ms", optional_double(first_incoming_request_delay_ms)},
        {"first_block_upload_delay_ms", optional_double(first_block_upload_delay_ms)},
        {"mean_request_gap_ms", optional_double(request_gap_summary.mean)},
        {"std_request_gap_ms", optional_double(request_gap_summary.stddev)},
        {"p50_request_gap_ms", optional_double(request_gap_summary.p50)},
        {"p95_request_gap_ms", optional_double(request_gap_summary.p95)},
        {"mean_incoming_request_gap_ms", optional_double(incoming_request_gap_summary.mean)},
        {"std_incoming_request_gap_ms", optional_double(incoming_request_gap_summary.stddev)},
        {"p50_incoming_request_gap_ms", optional_double(incoming_request_gap_summary.p50)},
        {"p95_incoming_request_gap_ms", optional_double(incoming_request_gap_summary.p95)},
        {"mean_block_upload_gap_ms", optional_double(block_upload_gap_summary.mean)},
        {"std_block_upload_gap_ms", optional_double(block_upload_gap_summary.stddev)},
        {"p50_block_upload_gap_ms", optional_double(block_upload_gap_summary.p50)},
        {"p95_block_upload_gap_ms", optional_double(block_upload_gap_summary.p95)},
        {"mean_request_to_upload_latency_ms", optional_double(request_to_upload_latency_summary.mean)},
        {"std_request_to_upload_latency_ms", optional_double(request_to_upload_latency_summary.stddev)},
        {"p50_request_to_upload_latency_ms", optional_double(request_to_upload_latency_summary.p50)},
        {"p95_request_to_upload_latency_ms", optional_double(request_to_upload_latency_summary.p95)},
        {"mean_block_latency_ms", optional_double(latency_summary.mean)},
        {"std_block_latency_ms", optional_double(latency_summary.stddev)},
        {"p50_block_latency_ms", optional_double(latency_summary.p50)},
        {"p95_block_latency_ms", optional_double(latency_summary.p95)},
        {"mean_interaction_gap_ms", optional_double(interaction_gap_summary.mean)},
        {"mean_incoming_request_burst_size", optional_double(incoming_request_burst_size_summary.mean)},
        {"p95_incoming_request_burst_size", optional_double(incoming_request_burst_size_summary.p95)},
        {"mean_incoming_request_burst_duration_ms", optional_double(incoming_request_burst_duration_summary.mean)},
        {"p95_incoming_request_burst_duration_ms", optional_double(incoming_request_burst_duration_summary.p95)},
        {"mean_incoming_request_burst_idle_gap_ms", optional_double(incoming_request_burst_idle_gap_summary.mean)},
        {"p95_incoming_request_burst_idle_gap_ms", optional_double(incoming_request_burst_idle_gap_summary.p95)},
        {"active_time_ms", ns_to_ms(active_time_ns)},
        {"idle_time_ms", ns_to_ms(idle_time_ns)},
        {"observation_quality_score", observation_quality_score},
        {"observation_sample_count", requests_sent + incoming_requests_received},
        {"requests_sent", requests_sent},
        {"block_responses_received", block_responses_received},
        {"request_timeouts", request_timeouts},
        {"requests_dropped", requests_dropped},
        {"pieces_finished", pieces_finished},
        {"snubbed_count", snubbed_count},
        {"unsnubbed_count", unsnubbed_count},
        {"choked_count", choked_count},
        {"unchoked_count", unchoked_count},
        {"mean_download_rate_Bps", optional_double(download_rate_summary.mean)},
        {"mean_upload_rate_Bps", optional_double(upload_rate_summary.mean)},
        {"peak_download_rate_Bps", peak_download_rate_Bps},
        {"peak_upload_rate_Bps", peak_upload_rate_Bps},
        {"request_gap_summary", distribution_summary_to_json(request_gap_summary)},
        {"block_latency_summary", distribution_summary_to_json(latency_summary)},
        {"incoming_request_gap_summary", distribution_summary_to_json(incoming_request_gap_summary)},
        {"block_upload_gap_summary", distribution_summary_to_json(block_upload_gap_summary)},
        {"request_to_upload_latency_summary", distribution_summary_to_json(request_to_upload_latency_summary)},
        {"incoming_request_burst_size_summary", distribution_summary_to_json(incoming_request_burst_size_summary)},
        {"incoming_request_burst_duration_summary", distribution_summary_to_json(incoming_request_burst_duration_summary)},
        {"incoming_request_burst_idle_gap_summary", distribution_summary_to_json(incoming_request_burst_idle_gap_summary)},
        {"interaction_gap_summary", distribution_summary_to_json(interaction_gap_summary)},
        {"download_rate_summary", distribution_summary_to_json(download_rate_summary)},
        {"upload_rate_summary", distribution_summary_to_json(upload_rate_summary)},
        {"outstanding_request_depth_summary", distribution_summary_to_json(outstanding_depth_summary)},
        {"state_transition_counts", std::move(state_transitions)},
        {"total_download_bytes", total_download_bytes},
        {"total_upload_bytes", total_upload_bytes},
        {"incoming_requests_received", incoming_requests_received},
        {"blocks_uploaded", blocks_uploaded},
        {"supports_extensions", observed_flags.count("supports_extensions") > 0},
        {"is_seed", seed},
        {"remote_choked", remote_choked},
        {"local_choked", local_choked},
        {"snubbed", snubbed},
    };
}

void PeerStats::note_interaction(std::int64_t mono_ns)
{
    if (last_interaction_mono_ns.has_value() && mono_ns > *last_interaction_mono_ns)
    {
        interaction_gap_ms_samples.push_back(ns_to_ms(mono_ns - *last_interaction_mono_ns));
    }
    last_interaction_mono_ns = mono_ns;
}

void PeerStats::note_incoming_request_burst(std::int64_t mono_ns)
{
    if (!incoming_request_burst_active)
    {
        incoming_request_burst_active = true;
        current_incoming_request_burst_size = 1;
        current_incoming_request_burst_start_mono_ns = mono_ns;
        current_incoming_request_burst_last_mono_ns = mono_ns;
        return;
    }

    double const gap_ms = ns_to_ms(mono_ns - current_incoming_request_burst_last_mono_ns);
    if (gap_ms <= kIncomingRequestBurstGapMs)
    {
        ++current_incoming_request_burst_size;
        current_incoming_request_burst_last_mono_ns = mono_ns;
        return;
    }

    flush_incoming_request_burst();
    incoming_request_burst_idle_gap_ms_samples.push_back(gap_ms);
    incoming_request_burst_active = true;
    current_incoming_request_burst_size = 1;
    current_incoming_request_burst_start_mono_ns = mono_ns;
    current_incoming_request_burst_last_mono_ns = mono_ns;
}

void PeerStats::flush_incoming_request_burst()
{
    if (!incoming_request_burst_active) return;

    incoming_request_burst_size_samples.push_back(
        static_cast<double>(current_incoming_request_burst_size));
    incoming_request_burst_duration_ms_samples.push_back(ns_to_ms(
        current_incoming_request_burst_last_mono_ns
            - current_incoming_request_burst_start_mono_ns));

    incoming_request_burst_active = false;
    current_incoming_request_burst_size = 0;
    current_incoming_request_burst_start_mono_ns = 0;
    current_incoming_request_burst_last_mono_ns = 0;
}

void PeerStats::record_outstanding_depth(double depth)
{
    outstanding_depth_samples.push_back(depth);
}

void PeerStats::advance_activity_window(std::int64_t mono_ns, bool active_now)
{
    if (last_activity_window_mono_ns.has_value() && mono_ns > *last_activity_window_mono_ns)
    {
        std::int64_t const delta = mono_ns - *last_activity_window_mono_ns;
        if (last_activity_window_active)
        {
            active_time_ns += delta;
        }
        else
        {
            idle_time_ns += delta;
        }
    }

    last_activity_window_mono_ns = mono_ns;
    last_activity_window_active = active_now;
}

SessionStats::SessionStats(
    std::string run_id_value,
    std::string session_id_value,
    TimestampPair const& start)
    : run_id(std::move(run_id_value))
    , session_id(std::move(session_id_value))
    , started_utc(start.utc_ts)
    , start_mono_ns(start.mono_ns)
{
}

PeerStats& SessionStats::ensure_peer_for_endpoint(
    lt::tcp::endpoint const& endpoint,
    TimestampPair const& ts,
    std::string direction,
    std::string transport)
{
    std::string const endpoint_key = endpoint_to_string(endpoint);
    unique_peer_endpoints.insert(endpoint_key);

    auto const active_it = active_peer_key_by_endpoint.find(endpoint_key);
    if (active_it != active_peer_key_by_endpoint.end())
    {
        return peers_by_key.at(active_it->second);
    }

    int const sequence = ++connect_sequence_by_endpoint[endpoint_key];
    std::string const peer_key = endpoint_key + "#" + std::to_string(sequence);

    // A peer may reconnect from the same endpoint. The sequence suffix keeps
    // each connection record distinct while preserving the endpoint identity.
    PeerStats peer(
        peer_key,
        endpoint_key,
        endpoint.address().to_string(),
        endpoint.port(),
        ts,
        std::move(direction),
        std::move(transport));
    auto const [it, inserted] = peers_by_key.emplace(peer_key, std::move(peer));
    (void)inserted;
    active_peer_key_by_endpoint[endpoint_key] = peer_key;
    return it->second;
}

PeerStats& SessionStats::note_peer_connect(
    lt::tcp::endpoint const& endpoint,
    lt::peer_id const& pid,
    TimestampPair const& ts,
    std::string const& direction,
    std::string const& transport)
{
    PeerStats& peer = ensure_peer_for_endpoint(endpoint, ts, direction, transport);
    if (!peer.saw_connect_alert)
    {
        ++successful_peer_connects;
    }
    peer.note_connect(ts, direction, transport, true);
    peer.note_peer_id(pid);
    return peer;
}

PeerStats& SessionStats::note_peer_event(
    lt::tcp::endpoint const& endpoint,
    lt::peer_id const& pid,
    TimestampPair const& ts,
    std::string direction,
    std::string transport)
{
    PeerStats& peer = ensure_peer_for_endpoint(endpoint, ts, std::move(direction), std::move(transport));
    peer.mark_seen(ts);
    peer.note_peer_id(pid);
    return peer;
}

PeerStats* SessionStats::note_peer_disconnect(
    lt::tcp::endpoint const& endpoint,
    lt::peer_id const& pid,
    TimestampPair const& ts)
{
    PeerStats& peer = note_peer_event(endpoint, pid, ts);
    peer.note_disconnect(ts);
    active_peer_key_by_endpoint.erase(peer.endpoint_key);
    ++disconnect_count;
    return &peer;
}

PeerStats& SessionStats::note_peer_snapshot(
    lt::peer_info const& peer,
    TimestampPair const& ts,
    std::int64_t snapshot_interval_ms)
{
    std::string direction = "unknown";
    if (has_flag(peer.flags, lt::peer_info::outgoing_connection))
    {
        direction = "outbound";
    }
    else if (has_flag(peer.source, lt::peer_info::incoming))
    {
        direction = "inbound";
    }

    PeerStats& stats = ensure_peer_for_endpoint(
        peer.ip,
        ts,
        direction,
        transport_from_peer_info(peer));
    stats.update_from_snapshot(peer, ts, snapshot_interval_ms);
    return stats;
}

void SessionStats::note_piece_finished(int piece_index)
{
    bool const network_piece = state_implies_network_piece_completion(latest_torrent_state);
    if (!network_piece)
    {
        ++verification_piece_completions;
        return;
    }

    ++piece_completions;
    auto const it = last_piece_peer_key.find(piece_index);
    if (it == last_piece_peer_key.end()) return;

    auto const peer_it = peers_by_key.find(it->second);
    if (peer_it != peers_by_key.end())
    {
        ++peer_it->second.pieces_finished;
    }
}

void SessionStats::set_initial_totals(lt::torrent_status const& torrent_status)
{
    initial_total_done_bytes = torrent_status.total_done;
    initial_total_upload_bytes = torrent_status.total_upload;
    final_total_done_bytes = torrent_status.total_done;
    final_total_upload_bytes = torrent_status.total_upload;
    total_download_bytes = 0;
    total_upload_bytes = 0;
    latest_torrent_state = torrent_status.state;
}

void SessionStats::note_torrent_state(lt::torrent_status::state_t state)
{
    latest_torrent_state = state;
}

void SessionStats::note_alert_loss(int dropped_alert_type_bits)
{
    alert_loss_detected = true;
    ++alerts_dropped_notifications;
    alerts_dropped_type_bits_total += dropped_alert_type_bits;
    last_piece_peer_key.clear();

    for (auto& [peer_key, peer] : peers_by_key)
    {
        (void)peer_key;
        peer.reset_outstanding_requests();
    }
}

void SessionStats::record_session_snapshot(lt::torrent_status const& torrent_status)
{
    latest_torrent_state = torrent_status.state;
    final_total_done_bytes = torrent_status.total_done;
    final_total_upload_bytes = torrent_status.total_upload;
    total_download_bytes = (std::max)(
        std::int64_t{0},
        final_total_done_bytes - initial_total_done_bytes);
    total_upload_bytes = (std::max)(
        std::int64_t{0},
        final_total_upload_bytes - initial_total_upload_bytes);
    session_download_rate_samples.push_back(static_cast<double>(torrent_status.download_rate));
    session_upload_rate_samples.push_back(static_cast<double>(torrent_status.upload_rate));
    max_peers_observed = (std::max)(max_peers_observed, torrent_status.num_peers);
    max_seeds_observed = (std::max)(max_seeds_observed, torrent_status.num_seeds);
}

void SessionStats::finalize(TimestampPair const& end)
{
    ended_utc = end.utc_ts;
    end_mono_ns = end.mono_ns;

    for (auto& [peer_key, peer] : peers_by_key)
    {
        (void)peer_key;
        peer.finalize(end);
    }
}

std::size_t SessionStats::unique_peers_seen() const
{
    return unique_peer_endpoints.size();
}

json SessionStats::peer_summaries_json() const
{
    json peers = json::array();
    for (auto const& [peer_key, peer] : peers_by_key)
    {
        (void)peer_key;
        peers.push_back(peer.to_json());
    }
    return peers;
}

}  // namespace btclient
