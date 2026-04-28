#include "summary_writer.hpp"

#include <libtorrent/version.hpp>

#include <fstream>
#include <stdexcept>

namespace btclient {

namespace {

struct NetworkByteTotals {
    std::int64_t downloaded = 0;
    std::int64_t uploaded = 0;
};

struct PeerObservationCounts {
    std::int64_t unique_peer_endpoints_seen = 0;
    std::int64_t peer_connections_observed = 0;
    std::int64_t active_peer_connections = 0;
    std::int64_t fingerprintable_peer_connections = 0;
};

JsonValue optional_double(std::optional<double> const& value)
{
    if (!value.has_value()) return JsonValue(nullptr);
    return JsonValue(*value);
}

JsonValue optional_string(std::string const& value)
{
    if (value.empty()) return JsonValue(nullptr);
    return JsonValue(value);
}

JsonValue build_session_json(
    RuntimeConfig const& config,
    SessionStats const& stats,
    std::string const& exit_reason)
{
    JsonValue session = JsonValue::object();
    session.add("run_id", config.run_id);
    session.add("session_id", config.session_id);
    session.add("started_utc", stats.started_utc);
    session.add("ended_utc", stats.ended_utc);
    session.add("duration_ms", ns_to_ms(stats.end_mono_ns - stats.start_mono_ns));
    session.add("hostname", optional_string(get_hostname()));
    session.add("client_version", kClientVersion);
    session.add("client_label", optional_string(config.client_label));
    session.add("profile_id", optional_string(config.profile_id));
    session.add("libtorrent_version", lt::version_str);
    session.add("exit_reason", exit_reason);
    return session;
}

NetworkByteTotals peer_network_byte_totals(SessionStats const& stats)
{
    NetworkByteTotals totals;
    for (auto const& [peer_key, peer] : stats.peers_by_key)
    {
        (void)peer_key;
        totals.downloaded += peer.total_download_bytes;
        totals.uploaded += peer.total_upload_bytes;
    }
    return totals;
}

PeerObservationCounts peer_observation_counts(SessionStats const& stats)
{
    PeerObservationCounts counts;
    counts.unique_peer_endpoints_seen = static_cast<std::int64_t>(stats.unique_peers_seen());
    counts.peer_connections_observed = static_cast<std::int64_t>(stats.peers_by_key.size());

    for (auto const& [peer_key, peer] : stats.peers_by_key)
    {
        (void)peer_key;

        bool const had_protocol_activity =
            peer.requests_sent > 0
            || peer.block_responses_received > 0
            || peer.incoming_requests_received > 0
            || peer.blocks_uploaded > 0
            || peer.total_download_bytes > 0
            || peer.total_upload_bytes > 0;
        if (had_protocol_activity)
        {
            ++counts.active_peer_connections;
        }

        int const fingerprint_samples =
            peer.requests_sent + peer.incoming_requests_received;
        if (fingerprint_samples >= 100 && had_protocol_activity)
        {
            ++counts.fingerprintable_peer_connections;
        }
    }

    return counts;
}

}  // namespace

void write_session_summary(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    SessionStats const& stats,
    libtorrent::torrent_status const& final_torrent_status,
    std::string const& info_hash,
    std::string const& exit_reason,
    int actual_listen_port,
    bool is_listening)
{
    DistributionSummary const session_download_rate_summary =
        summarize_samples(stats.session_download_rate_samples);
    DistributionSummary const session_upload_rate_summary =
        summarize_samples(stats.session_upload_rate_samples);
    NetworkByteTotals const network_bytes = peer_network_byte_totals(stats);
    PeerObservationCounts const peer_counts = peer_observation_counts(stats);

    JsonValue session = build_session_json(config, stats, exit_reason);

    // The summary JSON is organized by how it will be read later: run metadata,
    // requested config, final torrent state, aggregate metrics, then peer rows.
    JsonValue runtime_config = JsonValue::object();
    runtime_config.add("listen_port", config.listen_port);
    runtime_config.add("listen_port_actual", actual_listen_port);
    runtime_config.add("announce_ip", optional_string(config.announce_ip));
    runtime_config.add("dht_enabled", config.enable_dht);
    runtime_config.add("lsd_enabled", config.enable_lsd);
    runtime_config.add("upnp_enabled", config.enable_upnp);
    runtime_config.add("natpmp_enabled", config.enable_natpmp);
    runtime_config.add("runtime_seconds_requested", config.runtime_seconds);
    runtime_config.add("run_forever", config.runtime_seconds == 0);
    runtime_config.add("snapshot_interval_ms", config.snapshot_interval_ms);
    runtime_config.add("console_status_enabled", config.console_status);
    runtime_config.add("node_role", config.node_role);
    runtime_config.add("torrent_source_type", config.torrent_source_type);
    runtime_config.add("save_path", std::filesystem::absolute(config.save_path).string());
    runtime_config.add("artifacts_dir", output_paths.artifact_dir.string());

    JsonValue torrent = JsonValue::object();
    torrent.add("source", config.torrent_source);
    torrent.add("source_type", config.torrent_source_type);
    torrent.add("info_hash", optional_string(info_hash));
    torrent.add("save_path", std::filesystem::absolute(config.save_path).string());
    torrent.add("final_state", state_name(final_torrent_status.state));
    torrent.add("final_progress", static_cast<double>(final_torrent_status.progress) * 100.0);
    torrent.add("initial_total_done_bytes", stats.initial_total_done_bytes);
    torrent.add("initial_total_upload_bytes", stats.initial_total_upload_bytes);
    torrent.add("final_total_done_bytes", final_torrent_status.total_done);
    torrent.add("final_total_upload_bytes", final_torrent_status.total_upload);
    torrent.add("session_completed_bytes", stats.total_download_bytes);
    torrent.add("session_network_downloaded_bytes", network_bytes.downloaded);
    torrent.add("session_network_uploaded_bytes", network_bytes.uploaded);
    torrent.add("session_downloaded_bytes", network_bytes.downloaded);
    torrent.add("session_uploaded_bytes", network_bytes.uploaded);
    torrent.add("total_peers_observed", stats.max_peers_observed);
    torrent.add("total_seeds_observed", stats.max_seeds_observed);

    JsonValue global_metrics = JsonValue::object();
    global_metrics.add("total_runtime_ms", ns_to_ms(stats.end_mono_ns - stats.start_mono_ns));
    global_metrics.add("total_bytes_down", network_bytes.downloaded);
    global_metrics.add("total_bytes_up", network_bytes.uploaded);
    global_metrics.add("file_completed_bytes", stats.total_download_bytes);
    global_metrics.add("mean_download_rate_Bps", optional_double(session_download_rate_summary.mean));
    global_metrics.add("mean_upload_rate_Bps", optional_double(session_upload_rate_summary.mean));
    global_metrics.add("download_rate_summary", distribution_summary_to_json(session_download_rate_summary));
    global_metrics.add("upload_rate_summary", distribution_summary_to_json(session_upload_rate_summary));
    global_metrics.add("unique_peers_seen", peer_counts.unique_peer_endpoints_seen);
    global_metrics.add("unique_peer_endpoints_seen", peer_counts.unique_peer_endpoints_seen);
    global_metrics.add("peer_connections_observed", peer_counts.peer_connections_observed);
    global_metrics.add("active_peer_connections", peer_counts.active_peer_connections);
    global_metrics.add(
        "fingerprintable_peer_connections",
        peer_counts.fingerprintable_peer_connections);
    global_metrics.add(
        "connection_attempts",
        static_cast<std::int64_t>(stats.successful_peer_connects + stats.connection_failures));
    global_metrics.add("successful_peer_connects", stats.successful_peer_connects);
    global_metrics.add("disconnect_count", stats.disconnect_count);
    global_metrics.add("peer_error_count", stats.peer_error_count);
    global_metrics.add("tracker_announces", stats.tracker_announces);
    global_metrics.add("tracker_replies", stats.tracker_replies);
    global_metrics.add("tracker_errors", stats.tracker_errors);
    global_metrics.add("tracker_warnings", stats.tracker_warnings);
    global_metrics.add("piece_completions", stats.piece_completions);
    global_metrics.add("verification_piece_completions", stats.verification_piece_completions);
    global_metrics.add("incoming_connections", stats.incoming_connection_count);
    global_metrics.add("alerts_dropped_notifications", stats.alerts_dropped_notifications);
    global_metrics.add("alerts_dropped_type_bits_total", stats.alerts_dropped_type_bits_total);
    global_metrics.add("alert_loss_detected", stats.alert_loss_detected);
    global_metrics.add("is_listening", is_listening);

    JsonValue files = JsonValue::object();
    files.add("artifact_dir", output_paths.artifact_dir.string());
    files.add("event_log", output_paths.event_log_path.string());
    files.add("summary_json", output_paths.summary_json_path.string());

    JsonValue root = JsonValue::object();
    root.add("schema_version", 1);
    root.add("session", std::move(session));
    root.add("config", std::move(runtime_config));
    root.add("torrent", std::move(torrent));
    root.add("global_metrics", std::move(global_metrics));
    root.add("peer_summaries", stats.peer_summaries_json());
    root.add("files", std::move(files));

    std::ofstream stream(output_paths.summary_json_path, std::ios::out | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error(
            "failed to open summary output at " + output_paths.summary_json_path.string());
    }

    stream << root.to_string() << "\n";
    if (!stream)
    {
        throw std::runtime_error(
            "failed to write summary output at " + output_paths.summary_json_path.string());
    }
}

}  // namespace btclient
