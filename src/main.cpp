#include "artifact_lifecycle.hpp"
#include "artifact_sender.hpp"
#include "logging.hpp"
#include "peer_stats.hpp"
#include "summary_writer.hpp"
#include "util.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/operations.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lt = libtorrent;

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int)
{
    g_stop_requested.store(true);
}

std::string tracker_event_name(lt::event_t event)
{
    switch (event)
    {
        case lt::event_t::completed:
            return "completed";
        case lt::event_t::started:
            return "started";
        case lt::event_t::stopped:
            return "stopped";
        case lt::event_t::paused:
            return "paused";
        case lt::event_t::none:
        default:
            return "none";
    }
}

std::string peer_connect_direction_name(lt::peer_connect_alert::direction_t direction)
{
    return direction == lt::peer_connect_alert::direction_t::in
        ? "inbound"
        : "outbound";
}

bool is_valid_node_role(std::string const& node_role)
{
    return node_role == "victim"
        || node_role == "adversary"
        || node_role == "unknown";
}

bool is_connection_failure_operation(lt::operation_t operation)
{
    switch (operation)
    {
        case lt::operation_t::connect:
        case lt::operation_t::sock_open:
        case lt::operation_t::ssl_handshake:
        case lt::operation_t::sock_bind:
        case lt::operation_t::sock_accept:
            return true;
        default:
            return false;
    }
}

btclient::json base_alert_payload(lt::alert const& alert)
{
    return {
        {"alert_name", alert.what()},
        {"message", alert.message()},
    };
}

btclient::json null_if_empty(std::string const& value)
{
    if (value.empty()) return nullptr;
    return value;
}

constexpr int kArtifactUploadFailureExitCode = 3;

void record_upload_failure_or_throw(
    btclient::RuntimeConfig const& config,
    btclient::OutputPaths const& output_paths,
    std::string const& upload_error)
{
    try
    {
        btclient::record_artifact_upload_failure(config, output_paths, upload_error);
    }
    catch (std::exception const& state_ex)
    {
        throw std::runtime_error(
            upload_error
            + "; additionally failed to update state.json with the upload failure: "
            + state_ex.what());
    }
}

bool retry_pending_artifact_uploads(btclient::RuntimeConfig const& config)
{
    std::vector<btclient::PendingArtifactSession> const pending_sessions =
        btclient::find_pending_artifact_sessions(config.artifacts_dir);

    bool saw_ready_session = false;
    bool uploaded_any = false;

    for (btclient::PendingArtifactSession const& pending_session : pending_sessions)
    {
        if (pending_session.state != btclient::ArtifactState::ReadyToUpload)
        {
            continue;
        }

        saw_ready_session = true;
        if (config.destination_url.empty())
        {
            continue;
        }

        btclient::RuntimeConfig upload_config = config;
        upload_config.run_id = pending_session.run_id.empty()
            ? config.run_id
            : pending_session.run_id;
        upload_config.session_id = pending_session.session_id.empty()
            ? pending_session.output_paths.artifact_dir.filename().string()
            : pending_session.session_id;

        if (config.console_status)
        {
            std::cout
                << "Uploading pending artifacts from "
                << pending_session.output_paths.artifact_dir
                << "\n";
        }

        btclient::ArtifactSendResult send_result;
        try
        {
            send_result = btclient::send_artifacts_to_destination(
                upload_config,
                pending_session.output_paths);
        }
        catch (std::exception const& ex)
        {
            record_upload_failure_or_throw(upload_config, pending_session.output_paths, ex.what());
            throw;
        }

        btclient::OutputPaths const archived_paths =
            btclient::archive_artifact_session(
                upload_config,
                pending_session.output_paths);

        if (config.console_status)
        {
            std::cout
                << "Pending artifacts sent to " << config.destination_url
                << " (" << send_result.status_text << ")\n"
                << "Archived at " << archived_paths.artifact_dir << "\n";
        }

        uploaded_any = true;
    }

    if (saw_ready_session && config.destination_url.empty() && config.console_status)
    {
        std::cout
            << "Ready pending artifacts found under "
            << btclient::pending_artifacts_dir(config.artifacts_dir)
            << "; no destination URL configured, leaving them pending.\n";
    }

    return uploaded_any;
}

void print_usage()
{
    std::cerr
        << "Usage: btclient -f <magnet-uri|torrent-file> [options]\n"
        << "       btclient --destination-url <url> [options]  # retry pending artifacts\n\n"
        << "Options:\n"
        << "  -f, --file <path>              Path to the .torrent file or magnet URI (required)\n"
        << "  -t, --time <sec>               Runtime in seconds (0 means run indefinitely, default: 60)\n"
        << "  -p, --port <port>              Listen port (0-65535, default: 51413)\n"
        << "  -i, --ip <address>             Explicit IP to announce to the tracker\n"
        << "      --run-id <value>           Explicit run identifier\n"
        << "      --session-id <value>       Explicit session identifier\n"
        << "      --node-role <value>        victim | adversary | unknown (default: unknown)\n"
        << "      --artifacts-dir <path>     Base directory for per-run artifacts (default: ./artifacts)\n"
        << "      --event-log <path>         Explicit NDJSON event log path override\n"
        << "      --summary-json <path>      Explicit summary JSON path override\n"
        << "      --destination-url <url>    HTTP URL to POST final artifacts to\n"
        << "      --brain-url <url>          Alias for --destination-url\n"
        << "      --snapshot-interval-ms <n> Snapshot interval in milliseconds (default: 1000)\n"
        << "      --no-console-status        Disable periodic human-readable status lines\n"
        << "      --client-label <value>     Optional client label recorded in artifacts\n"
        << "      --profile-id <value>       Optional profile identifier recorded in artifacts\n"
        << "  -h, --help                     Show this help message\n";
}

void drain_alerts(
    lt::session& session,
    btclient::SessionStats& stats,
    btclient::EventWriter& writer)
{
    std::vector<lt::alert*> alerts;
    session.pop_alerts(&alerts);

    for (lt::alert* alert : alerts)
    {
        btclient::TimestampPair const ts = btclient::capture_timestamp();

        if (auto* al = lt::alert_cast<lt::listen_succeeded_alert>(alert))
        {
            btclient::json payload = base_alert_payload(*al);
            payload["address"] = al->address.to_string();
            payload["port"] = al->port;
            payload["socket_type"] = btclient::socket_type_label(al->socket_type);
            writer.write_event("listen_succeeded", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::listen_failed_alert>(alert))
        {
            btclient::json payload = base_alert_payload(*al);
            payload["address"] = al->address.to_string();
            payload["port"] = al->port;
            payload["operation"] = lt::operation_name(al->op);
            payload["error"] = al->error.message();
            payload["socket_type"] = btclient::socket_type_label(al->socket_type);
            writer.write_event("listen_failed", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::tracker_announce_alert>(alert))
        {
            ++stats.tracker_announces;
            btclient::json payload = base_alert_payload(*al);
            payload["tracker_url"] = al->tracker_url();
            payload["event"] = tracker_event_name(al->event);
            writer.write_event("tracker_announce", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::tracker_reply_alert>(alert))
        {
            ++stats.tracker_replies;
            btclient::json payload = base_alert_payload(*al);
            payload["tracker_url"] = al->tracker_url();
            payload["num_peers"] = al->num_peers;
            writer.write_event("tracker_reply", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::tracker_error_alert>(alert))
        {
            ++stats.tracker_errors;
            btclient::json payload = base_alert_payload(*al);
            payload["tracker_url"] = al->tracker_url();
            payload["times_in_row"] = al->times_in_row;
            payload["error"] = al->error.message();
            payload["operation"] = lt::operation_name(al->op);
            payload["failure_reason"] = al->failure_reason();
            writer.write_event("tracker_error", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::tracker_warning_alert>(alert))
        {
            ++stats.tracker_warnings;
            btclient::json payload = base_alert_payload(*al);
            payload["tracker_url"] = al->tracker_url();
            payload["warning"] = al->warning_message();
            writer.write_event("tracker_warning", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::state_changed_alert>(alert))
        {
            stats.note_torrent_state(al->state);
            btclient::json payload = base_alert_payload(*al);
            payload["previous_state"] = btclient::state_name(al->prev_state);
            payload["state"] = btclient::state_name(al->state);
            writer.write_event("torrent_state_changed", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::torrent_finished_alert>(alert))
        {
            writer.write_event("torrent_finished", base_alert_payload(*al));
        }
        else if (auto* al = lt::alert_cast<lt::peer_connect_alert>(alert))
        {
            std::string const direction = peer_connect_direction_name(al->direction);
            std::string const transport = btclient::transport_from_socket_type(al->socket_type);
            btclient::PeerStats& peer = stats.note_peer_connect(
                al->endpoint,
                al->pid,
                ts,
                direction,
                transport);

            btclient::json payload = base_alert_payload(*al);
            payload["peer_id_hex"] = btclient::peer_id_to_hex(al->pid);
            payload["socket_type"] = btclient::socket_type_label(al->socket_type);
            writer.write_event("peer_connected", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::peer_error_alert>(alert))
        {
            ++stats.peer_error_count;
            if (is_connection_failure_operation(al->op))
            {
                ++stats.connection_failures;
            }

            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            btclient::json payload = base_alert_payload(*al);
            payload["error"] = al->error.message();
            payload["operation"] = lt::operation_name(al->op);
            writer.write_event("peer_error", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::peer_disconnected_alert>(alert))
        {
            btclient::PeerStats* peer = stats.note_peer_disconnect(al->endpoint, al->pid, ts);
            btclient::json payload = base_alert_payload(*al);
            payload["error"] = al->error.message();
            payload["operation"] = lt::operation_name(al->op);
            payload["close_reason"] = static_cast<int>(al->reason);
            payload["socket_type"] = btclient::socket_type_label(al->socket_type);
            writer.write_event(
                "peer_disconnected",
                std::move(payload),
                peer == nullptr ? std::nullopt : std::optional(peer->event_context()));
        }
        else if (auto* al = lt::alert_cast<lt::incoming_connection_alert>(alert))
        {
            ++stats.incoming_connection_count;
            btclient::json payload = base_alert_payload(*al);
            payload["peer_ip"] = al->endpoint.address().to_string();
            payload["peer_port"] = al->endpoint.port();
            payload["socket_type"] = btclient::socket_type_label(al->socket_type);
            payload["transport"] = btclient::transport_from_socket_type(al->socket_type);
            writer.write_event("incoming_connection", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::peer_snubbed_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_snubbed(ts);
            writer.write_event("peer_snubbed", base_alert_payload(*al), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::peer_unsnubbed_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_unsnubbed(ts);
            writer.write_event("peer_unsnubbed", base_alert_payload(*al), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::block_downloading_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_request_sent(ts, static_cast<int>(al->piece_index), al->block_index);
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            payload["block"] = al->block_index;
            writer.write_event("block_request_sent", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::block_finished_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_block_response(ts, static_cast<int>(al->piece_index), al->block_index);
            stats.last_piece_peer_key[static_cast<int>(al->piece_index)] = peer.peer_key;
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            payload["block"] = al->block_index;
            writer.write_event("block_response_received", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::block_timeout_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_timeout(ts, static_cast<int>(al->piece_index), al->block_index);
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            payload["block"] = al->block_index;
            writer.write_event("block_timeout", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::request_dropped_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_drop(ts, static_cast<int>(al->piece_index), al->block_index);
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            payload["block"] = al->block_index;
            writer.write_event("request_dropped", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::incoming_request_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_incoming_request(ts, al->req);
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->req.piece);
            payload["start"] = al->req.start;
            payload["length"] = al->req.length;
            writer.write_event("incoming_request", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::block_uploaded_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            peer.note_block_uploaded(ts, static_cast<int>(al->piece_index), al->block_index);
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            payload["block"] = al->block_index;
            writer.write_event("block_uploaded", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::piece_finished_alert>(alert))
        {
            stats.note_piece_finished(static_cast<int>(al->piece_index));
            btclient::json payload = base_alert_payload(*al);
            payload["piece"] = static_cast<int>(al->piece_index);
            writer.write_event("piece_finished", std::move(payload));
        }
        else if (auto* al = lt::alert_cast<lt::peer_log_alert>(alert))
        {
            btclient::PeerStats& peer = stats.note_peer_event(al->endpoint, al->pid, ts);
            btclient::json payload = base_alert_payload(*al);
            payload["peer_log_event"] = al->event_type;
            payload["direction"] = static_cast<int>(al->direction);
            payload["log_message"] = al->log_message();
            writer.write_event("peer_log", std::move(payload), peer.event_context());
        }
        else if (auto* al = lt::alert_cast<lt::alerts_dropped_alert>(alert))
        {
            stats.note_alert_loss(static_cast<int>(al->dropped_alerts.count()));
            btclient::json payload = base_alert_payload(*al);
            payload["dropped_alert_type_bits"] =
                static_cast<std::int64_t>(al->dropped_alerts.count());
            writer.write_event("alerts_dropped", std::move(payload));
        }
    }
}

void capture_snapshots(
    lt::session& session,
    lt::torrent_handle& torrent_handle,
    btclient::RuntimeConfig const& config,
    btclient::SessionStats& stats,
    btclient::EventWriter& writer,
    std::chrono::steady_clock::time_point process_start)
{
    btclient::TimestampPair const ts = btclient::capture_timestamp();
    lt::torrent_status const torrent_status = torrent_handle.status();

    stats.record_session_snapshot(torrent_status);

    btclient::json session_payload = {
        {"num_peers", torrent_status.num_peers},
        {"is_listening", session.is_listening()},
        {"listen_port", session.listen_port()},
    };
    writer.write_event("session_snapshot", std::move(session_payload));

    btclient::json torrent_payload = {
        {"state", btclient::state_name(torrent_status.state)},
        {"progress", static_cast<double>(torrent_status.progress) * 100.0},
        {"num_peers", torrent_status.num_peers},
        {"num_seeds", torrent_status.num_seeds},
        {"total_done", torrent_status.total_done},
        {"total_upload", torrent_status.total_upload},
        {"download_rate_Bps", torrent_status.download_rate},
        {"upload_rate_Bps", torrent_status.upload_rate},
    };
    writer.write_event("torrent_snapshot", std::move(torrent_payload));

    std::vector<lt::peer_info> peers;
    torrent_handle.get_peer_info(peers);

    for (lt::peer_info const& peer : peers)
    {
        btclient::PeerStats& peer_stats = stats.note_peer_snapshot(
            peer,
            ts,
            config.snapshot_interval_ms);

        btclient::json payload = {
            {"client", peer.client},
            {"payload_down_speed_Bps", peer.payload_down_speed},
            {"payload_up_speed_Bps", peer.payload_up_speed},
            {"down_speed_Bps", peer.down_speed},
            {"up_speed_Bps", peer.up_speed},
            {"total_download_bytes", peer.total_download},
            {"total_upload_bytes", peer.total_upload},
            {"download_queue_length", peer.download_queue_length},
            {"upload_queue_length", peer.upload_queue_length},
            {"timed_out_requests", peer.timed_out_requests},
            {"busy_requests", peer.busy_requests},
            {"requests_in_buffer", peer.requests_in_buffer},
            {"queue_bytes", peer.queue_bytes},
            {"request_timeout", peer.request_timeout},
            {"last_active_ms", lt::total_milliseconds(peer.last_active)},
            {"last_request_ms", lt::total_milliseconds(peer.last_request)},
            {"flags", btclient::strings_to_json_array(btclient::peer_flag_labels(peer))},
            {"sources", btclient::strings_to_json_array(btclient::peer_source_labels(peer))},
            {"peer_id_hex", btclient::peer_id_to_hex(peer.pid)},
            {"downloading_piece", static_cast<int>(peer.downloading_piece_index)},
            {"downloading_block", peer.downloading_block_index},
            {"downloading_progress", peer.downloading_progress},
            {"downloading_total", peer.downloading_total},
        };
        writer.write_event("peer_snapshot", std::move(payload), peer_stats.event_context());
    }

    if (config.console_status)
    {
        auto const elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - process_start).count();
        std::cout
            << "t=" << elapsed_seconds
            << " state=" << btclient::state_name(torrent_status.state)
            << " progress=" << (static_cast<double>(torrent_status.progress) * 100.0) << "%"
            << " peers=" << torrent_status.num_peers
            << " seeds=" << torrent_status.num_seeds
            << " dls=" << torrent_status.download_rate << "B/s"
            << " uls=" << torrent_status.upload_rate << "B/s"
            << "\n";
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try
    {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        btclient::RuntimeConfig config;
        std::optional<std::filesystem::path> event_log_override;
        std::optional<std::filesystem::path> summary_json_override;

        for (int i = 1; i < argc; ++i)
        {
            std::string const arg = argv[i];
            auto require_value = [&](char const* name) -> std::string {
                if (i + 1 >= argc)
                {
                    throw std::invalid_argument(std::string("missing value for ") + name);
                }
                return argv[++i];
            };

            if (arg == "-f" || arg == "--file")
            {
                config.torrent_source = require_value(arg.c_str());
            }
            else if (arg == "-t" || arg == "--time")
            {
                config.runtime_seconds = std::stoi(require_value(arg.c_str()));
            }
            else if (arg == "-p" || arg == "--port")
            {
                config.listen_port = std::stoi(require_value(arg.c_str()));
            }
            else if (arg == "-i" || arg == "--ip")
            {
                config.announce_ip = require_value(arg.c_str());
            }
            else if (arg == "--run-id")
            {
                config.run_id = require_value(arg.c_str());
            }
            else if (arg == "--session-id")
            {
                config.session_id = require_value(arg.c_str());
            }
            else if (arg == "--node-role")
            {
                config.node_role = require_value(arg.c_str());
            }
            else if (arg == "--artifacts-dir")
            {
                config.artifacts_dir = require_value(arg.c_str());
            }
            else if (arg == "--event-log")
            {
                event_log_override = require_value(arg.c_str());
            }
            else if (arg == "--summary-json")
            {
                summary_json_override = require_value(arg.c_str());
            }
            else if (arg == "--destination-url" || arg == "--brain-url")
            {
                config.destination_url = require_value(arg.c_str());
            }
            else if (arg == "--snapshot-interval-ms")
            {
                config.snapshot_interval_ms = std::stoll(require_value(arg.c_str()));
            }
            else if (arg == "--no-console-status")
            {
                config.console_status = false;
            }
            else if (arg == "--client-label")
            {
                config.client_label = require_value(arg.c_str());
            }
            else if (arg == "--profile-id")
            {
                config.profile_id = require_value(arg.c_str());
            }
            else if (arg == "-h" || arg == "--help")
            {
                print_usage();
                return 0;
            }
            else
            {
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }

        if (config.listen_port < 0 || config.listen_port > 65535)
        {
            throw std::invalid_argument("listen port must be between 0 and 65535");
        }
        if (config.runtime_seconds < 0)
        {
            throw std::invalid_argument("runtime seconds must be >= 0");
        }
        if (config.snapshot_interval_ms <= 0)
        {
            throw std::invalid_argument("snapshot interval must be > 0");
        }
        if (!is_valid_node_role(config.node_role))
        {
            throw std::invalid_argument("node role must be victim, adversary, or unknown");
        }

        bool uploaded_pending_artifacts = false;
        try
        {
            uploaded_pending_artifacts = retry_pending_artifact_uploads(config);
        }
        catch (std::exception const& ex)
        {
            std::cerr << "Pending artifact upload/archive failed: " << ex.what() << "\n";
            return kArtifactUploadFailureExitCode;
        }

        if (config.torrent_source.empty())
        {
            if (uploaded_pending_artifacts)
            {
                return 0;
            }
            throw std::invalid_argument("torrent source (-f/--file) is required");
        }

        if (config.run_id.empty()) config.run_id = btclient::generate_id("run");
        if (config.session_id.empty()) config.session_id = btclient::generate_id("session");
        config.torrent_source_type = btclient::is_magnet_uri(config.torrent_source)
            ? "magnet"
            : "torrent_file";

        btclient::OutputPaths const output_paths = btclient::prepare_output_paths(
            config,
            event_log_override,
            summary_json_override);
        btclient::write_artifact_state(
            config,
            output_paths,
            btclient::ArtifactState::Capturing);

        btclient::EventWriter writer(config, output_paths);

        lt::settings_pack settings_pack;
        settings_pack.set_bool(lt::settings_pack::enable_dht, false);
        settings_pack.set_bool(lt::settings_pack::enable_lsd, false);
        settings_pack.set_bool(lt::settings_pack::enable_upnp, false);
        settings_pack.set_bool(lt::settings_pack::enable_natpmp, false);
        settings_pack.set_bool(lt::settings_pack::ssrf_mitigation, false);
        settings_pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        settings_pack.set_str(
            lt::settings_pack::listen_interfaces,
            "0.0.0.0:" + std::to_string(config.listen_port)
                + ",[::]:" + std::to_string(config.listen_port));
    if (!config.announce_ip.empty())
    {
        settings_pack.set_str(lt::settings_pack::announce_ip, config.announce_ip);
    }
    settings_pack.set_int(lt::settings_pack::alert_queue_size, 50000);
    settings_pack.set_int(
        lt::settings_pack::alert_mask,
        lt::alert_category::error
                | lt::alert_category::status
                | lt::alert_category::session_log
                | lt::alert_category::torrent_log
                | lt::alert_category::tracker
                | lt::alert_category::peer
                | lt::alert_category::connect
                | lt::alert_category::piece_progress
                | lt::alert_category::block_progress
                | lt::alert_category::upload
                | lt::alert_category::incoming_request);

        lt::session session(settings_pack);

        btclient::TimestampPair const session_start = btclient::capture_timestamp();
        btclient::SessionStats stats(config.run_id, config.session_id, session_start);

        btclient::json start_payload = {
            {"client_version", btclient::kClientVersion},
            {"hostname", btclient::get_hostname()},
            {"artifact_dir", output_paths.artifact_dir.string()},
        };
        writer.write_event("session_started", std::move(start_payload));

        btclient::json config_payload = {
            {"listen_port", config.listen_port},
            {"announce_ip", null_if_empty(config.announce_ip)},
            {"dht_enabled", config.enable_dht},
            {"lsd_enabled", config.enable_lsd},
            {"upnp_enabled", config.enable_upnp},
            {"natpmp_enabled", config.enable_natpmp},
            {"runtime_seconds", config.runtime_seconds},
            {"snapshot_interval_ms", config.snapshot_interval_ms},
            {"torrent_source_type", config.torrent_source_type},
            {"save_path", std::filesystem::absolute(config.save_path).string()},
            {"client_label", null_if_empty(config.client_label)},
            {"profile_id", null_if_empty(config.profile_id)},
            {"destination_url", null_if_empty(config.destination_url)},
        };
        writer.write_event("session_config", std::move(config_payload));

        lt::add_torrent_params params;
        lt::error_code error_code;
        if (btclient::is_magnet_uri(config.torrent_source))
        {
            params = lt::parse_magnet_uri(config.torrent_source, error_code);
            if (error_code)
            {
                throw std::runtime_error(
                    "invalid magnet URI: " + error_code.message());
            }
        }
        else
        {
            auto torrent_info = std::make_shared<lt::torrent_info>(
                config.torrent_source,
                error_code);
            if (error_code)
            {
                throw std::runtime_error(
                    "failed to load torrent file '" + config.torrent_source
                        + "': " + error_code.message());
            }
            params.ti = torrent_info;
        }

        params.save_path = config.save_path.string();
        lt::torrent_handle torrent_handle = session.add_torrent(params, error_code);
        if (error_code)
        {
            throw std::runtime_error("failed to add torrent: " + error_code.message());
        }

        std::string info_hash;
        lt::torrent_status initial_status = torrent_handle.status();
        stats.set_initial_totals(initial_status);
        info_hash = btclient::info_hash_to_string(initial_status.info_hashes);
        writer.set_info_hash(info_hash);

        btclient::json torrent_added_payload = {
            {"source_type", config.torrent_source_type},
            {"save_path", std::filesystem::absolute(config.save_path).string()},
            {"info_hash", null_if_empty(info_hash)},
        };
        writer.write_event("torrent_added", std::move(torrent_added_payload));

        if (config.console_status)
        {
            std::cout
                << "btclient started: run_id=" << config.run_id
                << " session_id=" << config.session_id
                << " event_log=" << output_paths.event_log_path
                << " summary=" << output_paths.summary_json_path
                << "\n";
        }

        using namespace std::chrono_literals;
        auto const process_start = std::chrono::steady_clock::now();
        auto const deadline = process_start + std::chrono::seconds(config.runtime_seconds);
        auto next_snapshot = process_start;
        auto const poll_interval = std::chrono::milliseconds(
            (std::min)(std::int64_t{100}, config.snapshot_interval_ms));
        bool const run_forever = config.runtime_seconds == 0;

        while (!g_stop_requested.load())
        {
            drain_alerts(session, stats, writer);

            auto const now = std::chrono::steady_clock::now();
            if (now >= next_snapshot)
            {
                capture_snapshots(
                    session,
                    torrent_handle,
                    config,
                    stats,
                    writer,
                    process_start);
                next_snapshot = now + std::chrono::milliseconds(config.snapshot_interval_ms);
            }

            if (!run_forever && now >= deadline)
            {
                break;
            }

            auto sleep_until = next_snapshot;
            if (!run_forever && deadline < sleep_until)
            {
                sleep_until = deadline;
            }

            auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                sleep_until - std::chrono::steady_clock::now());
            auto const sleep_for = remaining > poll_interval ? poll_interval : remaining;
            if (sleep_for > 0ms)
            {
                std::this_thread::sleep_for(sleep_for);
            }
        }

        drain_alerts(session, stats, writer);
        capture_snapshots(
            session,
            torrent_handle,
            config,
            stats,
            writer,
            process_start);

        lt::torrent_status const final_status = torrent_handle.status();
        btclient::TimestampPair const session_end = btclient::capture_timestamp();
        stats.finalize(session_end);

        std::string const exit_reason = g_stop_requested.load()
            ? "signal"
            : (run_forever ? "stopped" : "runtime_elapsed");

        btclient::json finish_payload = {
            {"exit_reason", exit_reason},
            {"duration_ms", btclient::ns_to_ms(stats.end_mono_ns - stats.start_mono_ns)},
            {"final_state", btclient::state_name(final_status.state)},
            {"final_progress", static_cast<double>(final_status.progress) * 100.0},
            {"total_done", final_status.total_done},
            {"total_upload", final_status.total_upload},
        };
        writer.write_event("session_finished", std::move(finish_payload));
        writer.close();

        btclient::write_session_summary(
            config,
            output_paths,
            stats,
            final_status,
            info_hash.empty() ? btclient::info_hash_to_string(final_status.info_hashes) : info_hash,
            exit_reason,
            session.listen_port(),
            session.is_listening());
        btclient::write_artifact_state(
            config,
            output_paths,
            btclient::ArtifactState::ReadyToUpload);

        if (config.console_status)
        {
            std::cout
                << "Artifacts written to " << output_paths.artifact_dir
                << "\nSummary: " << output_paths.summary_json_path
                << "\n";
        }

        if (!config.destination_url.empty())
        {
            try
            {
                btclient::ArtifactSendResult send_result;
                try
                {
                    send_result = btclient::send_artifacts_to_destination(config, output_paths);
                }
                catch (std::exception const& ex)
                {
                    record_upload_failure_or_throw(config, output_paths, ex.what());
                    throw;
                }

                if (config.console_status)
                {
                    std::cout
                        << "Artifacts sent to " << config.destination_url
                        << " (" << send_result.status_text << ")\n";
                }
                btclient::OutputPaths const archived_paths =
                    btclient::archive_artifact_session(config, output_paths);
                if (config.console_status)
                {
                    std::cout
                        << "Artifacts archived at "
                        << archived_paths.artifact_dir
                        << "\n";
                }
            }
            catch (std::exception const& ex)
            {
                std::cerr << "Artifact upload/archive failed: " << ex.what() << "\n";
                return kArtifactUploadFailureExitCode;
            }
        }
        else if (config.console_status)
        {
            std::cout
                << "No destination URL configured; artifacts remain pending and ready "
                << "for a later upload.\n";
        }

        return 0;
    }
    catch (std::exception const& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n\n";
        print_usage();
        return 2;
    }
}
