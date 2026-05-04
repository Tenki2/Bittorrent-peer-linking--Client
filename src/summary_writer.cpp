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

json build_session_json(
    RuntimeConfig const& config,
    SessionStats const& stats,
    std::string const& exit_reason)
{
    return {
        {"run_id", config.run_id},
        {"session_id", config.session_id},
        {"started_utc", stats.started_utc},
        {"ended_utc", stats.ended_utc},
        {"duration_ms", ns_to_ms(stats.end_mono_ns - stats.start_mono_ns)},
        {"hostname", optional_string(get_hostname())},
        {"client_version", kClientVersion},
        {"client_label", optional_string(config.client_label)},
        {"profile_id", optional_string(config.profile_id)},
        {"destination_url", optional_string(config.destination_url)},
        {"libtorrent_version", lt::version_str},
        {"exit_reason", exit_reason},
    };
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

    json session = build_session_json(config, stats, exit_reason);

    // The summary JSON is organized by how it will be read later: run metadata,
    // requested config, final torrent state, aggregate metrics, then peer rows.
    json runtime_config = {
        {"listen_port", config.listen_port},
        {"listen_port_actual", actual_listen_port},
        {"announce_ip", optional_string(config.announce_ip)},
        {"dht_enabled", config.enable_dht},
        {"lsd_enabled", config.enable_lsd},
        {"upnp_enabled", config.enable_upnp},
        {"natpmp_enabled", config.enable_natpmp},
        {"runtime_seconds_requested", config.runtime_seconds},
        {"run_forever", config.runtime_seconds == 0},
        {"snapshot_interval_ms", config.snapshot_interval_ms},
        {"console_status_enabled", config.console_status},
        {"node_role", config.node_role},
        {"torrent_source_type", config.torrent_source_type},
        {"destination_url", optional_string(config.destination_url)},
        {"save_path", std::filesystem::absolute(config.save_path).string()},
        {"artifacts_dir", output_paths.artifact_dir.string()},
    };

    json torrent = {
        {"source", config.torrent_source},
        {"source_type", config.torrent_source_type},
        {"info_hash", optional_string(info_hash)},
        {"save_path", std::filesystem::absolute(config.save_path).string()},
        {"final_state", state_name(final_torrent_status.state)},
        {"final_progress", static_cast<double>(final_torrent_status.progress) * 100.0},
        {"initial_total_done_bytes", stats.initial_total_done_bytes},
        {"initial_total_upload_bytes", stats.initial_total_upload_bytes},
        {"final_total_done_bytes", final_torrent_status.total_done},
        {"final_total_upload_bytes", final_torrent_status.total_upload},
        {"session_completed_bytes", stats.total_download_bytes},
        {"session_network_downloaded_bytes", network_bytes.downloaded},
        {"session_network_uploaded_bytes", network_bytes.uploaded},
        {"session_downloaded_bytes", network_bytes.downloaded},
        {"session_uploaded_bytes", network_bytes.uploaded},
        {"total_peers_observed", stats.max_peers_observed},
        {"total_seeds_observed", stats.max_seeds_observed},
    };

    json global_metrics = {
        {"total_runtime_ms", ns_to_ms(stats.end_mono_ns - stats.start_mono_ns)},
        {"total_bytes_down", network_bytes.downloaded},
        {"total_bytes_up", network_bytes.uploaded},
        {"file_completed_bytes", stats.total_download_bytes},
        {"mean_download_rate_Bps", optional_double(session_download_rate_summary.mean)},
        {"mean_upload_rate_Bps", optional_double(session_upload_rate_summary.mean)},
        {"download_rate_summary", distribution_summary_to_json(session_download_rate_summary)},
        {"upload_rate_summary", distribution_summary_to_json(session_upload_rate_summary)},
        {"unique_peers_seen", peer_counts.unique_peer_endpoints_seen},
        {"unique_peer_endpoints_seen", peer_counts.unique_peer_endpoints_seen},
        {"peer_connections_observed", peer_counts.peer_connections_observed},
        {"active_peer_connections", peer_counts.active_peer_connections},
        {"fingerprintable_peer_connections", peer_counts.fingerprintable_peer_connections},
        {
            "connection_attempts",
            static_cast<std::int64_t>(stats.successful_peer_connects + stats.connection_failures),
        },
        {"successful_peer_connects", stats.successful_peer_connects},
        {"disconnect_count", stats.disconnect_count},
        {"peer_error_count", stats.peer_error_count},
        {"tracker_announces", stats.tracker_announces},
        {"tracker_replies", stats.tracker_replies},
        {"tracker_errors", stats.tracker_errors},
        {"tracker_warnings", stats.tracker_warnings},
        {"piece_completions", stats.piece_completions},
        {"verification_piece_completions", stats.verification_piece_completions},
        {"incoming_connections", stats.incoming_connection_count},
        {"alerts_dropped_notifications", stats.alerts_dropped_notifications},
        {"alerts_dropped_type_bits_total", stats.alerts_dropped_type_bits_total},
        {"alert_loss_detected", stats.alert_loss_detected},
        {"is_listening", is_listening},
    };

    json files = {
        {"artifact_dir", output_paths.artifact_dir.string()},
        {"state_json", output_paths.state_json_path.string()},
        {"event_log", output_paths.event_log_path.string()},
        {"summary_json", output_paths.summary_json_path.string()},
    };

    json root = {
        {"schema_version", 1},
        {"session", std::move(session)},
        {"config", std::move(runtime_config)},
        {"torrent", std::move(torrent)},
        {"global_metrics", std::move(global_metrics)},
        {"peer_summaries", stats.peer_summaries_json()},
        {"files", std::move(files)},
    };

    std::ofstream stream(output_paths.summary_json_path, std::ios::out | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error(
            "failed to open summary output at " + output_paths.summary_json_path.string());
    }

    stream << root.dump() << "\n";
    if (!stream)
    {
        throw std::runtime_error(
            "failed to write summary output at " + output_paths.summary_json_path.string());
    }
}

}  // namespace btclient
