#pragma once

#include "logging.hpp"

#include <libtorrent/peer_id.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/peer_request.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_status.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace btclient {

struct RequestKey {
    int piece_index = 0;
    int block_index = 0;

    bool operator<(RequestKey const& other) const;
};

struct PeerStats {
    // Stable identity for this observed connection.
    std::string peer_key;
    std::string endpoint_key;
    std::string peer_ip;
    int peer_port = 0;
    std::string direction = "unknown";
    std::string transport = "unknown";
    std::string peer_id_hex = "unknown";
    std::string peer_id_prefix_hex;
    std::string client_name;
    std::set<std::string> observed_flags;
    std::set<std::string> observed_sources;

    // Wall-clock strings are useful in artifacts; monotonic timestamps are used
    // for measuring durations.
    std::string first_seen_utc;
    std::string last_seen_utc;
    std::string connected_utc;
    std::string disconnected_utc;
    std::int64_t first_seen_mono_ns = 0;
    std::int64_t last_seen_mono_ns = 0;
    std::int64_t connected_mono_ns = 0;
    std::int64_t disconnected_mono_ns = 0;

    // Current connection state, as best as alerts and snapshots can tell us.
    bool active_connection = true;
    bool saw_connect_alert = false;
    bool state_flags_initialized = false;
    bool remote_choked = false;
    bool local_choked = false;
    bool snubbed = false;
    bool interesting = false;
    bool remote_interested = false;
    bool seed = false;

    // Event counters collected for this peer.
    int requests_sent = 0;
    int block_responses_received = 0;
    int request_timeouts = 0;
    int requests_dropped = 0;
    int pieces_finished = 0;
    int incoming_requests_received = 0;
    int blocks_uploaded = 0;
    int snubbed_count = 0;
    int unsnubbed_count = 0;
    int choked_count = 0;
    int unchoked_count = 0;
    int we_choked_count = 0;
    int we_unchoked_count = 0;

    std::int64_t total_download_bytes = 0;
    std::int64_t total_upload_bytes = 0;
    double peak_download_rate_Bps = 0.0;
    double peak_upload_rate_Bps = 0.0;

    // Raw samples kept until the summary JSON is written.
    std::optional<double> first_request_delay_ms;
    std::optional<double> first_incoming_request_delay_ms;
    std::optional<double> first_block_upload_delay_ms;
    std::optional<std::int64_t> last_request_sent_mono_ns;
    std::optional<std::int64_t> last_incoming_request_mono_ns;
    std::optional<std::int64_t> last_block_uploaded_mono_ns;
    std::optional<std::int64_t> last_interaction_mono_ns;
    std::optional<std::int64_t> last_activity_window_mono_ns;
    bool last_activity_window_active = true;
    std::int64_t active_time_ns = 0;
    std::int64_t idle_time_ns = 0;

    std::vector<double> request_gap_ms_samples;
    std::vector<double> block_latency_ms_samples;
    std::vector<double> incoming_request_gap_ms_samples;
    std::vector<double> block_upload_gap_ms_samples;
    std::vector<double> request_to_upload_latency_ms_samples;
    std::vector<double> incoming_request_burst_size_samples;
    std::vector<double> incoming_request_burst_duration_ms_samples;
    std::vector<double> incoming_request_burst_idle_gap_ms_samples;
    std::vector<double> interaction_gap_ms_samples;
    std::vector<double> download_rate_samples;
    std::vector<double> upload_rate_samples;
    std::vector<double> outstanding_depth_samples;
    std::map<RequestKey, std::int64_t> outstanding_requests;
    std::map<RequestKey, std::vector<std::int64_t>> pending_upload_requests;

    bool incoming_request_burst_active = false;
    int current_incoming_request_burst_size = 0;
    std::int64_t current_incoming_request_burst_start_mono_ns = 0;
    std::int64_t current_incoming_request_burst_last_mono_ns = 0;

    PeerStats() = default;
    PeerStats(
        std::string peer_key_value,
        std::string endpoint_key_value,
        std::string peer_ip_value,
        int peer_port_value,
        TimestampPair const& ts,
        std::string direction_value,
        std::string transport_value);

    EventPeerContext event_context() const;

    void mark_seen(TimestampPair const& ts);
    void note_connect(
        TimestampPair const& ts,
        std::string const& direction_value,
        std::string const& transport_value,
        bool from_connect_alert);
    void note_disconnect(TimestampPair const& ts);
    void note_peer_id(libtorrent::peer_id const& pid);
    void note_request_sent(TimestampPair const& ts, int piece_index, int block_index);
    void note_block_response(TimestampPair const& ts, int piece_index, int block_index);
    void note_timeout(TimestampPair const& ts, int piece_index, int block_index);
    void note_drop(TimestampPair const& ts, int piece_index, int block_index);
    void note_incoming_request(TimestampPair const& ts, libtorrent::peer_request const& request);
    void note_block_uploaded(TimestampPair const& ts, int piece_index, int block_index);
    void note_snubbed(TimestampPair const& ts);
    void note_unsnubbed(TimestampPair const& ts);
    void update_from_snapshot(
        libtorrent::peer_info const& peer,
        TimestampPair const& ts,
        std::int64_t snapshot_interval_ms);
    void finalize(TimestampPair const& session_end);
    void reset_outstanding_requests();

    JsonValue to_json() const;

private:
    void note_interaction(std::int64_t mono_ns);
    void note_incoming_request_burst(std::int64_t mono_ns);
    void flush_incoming_request_burst();
    void record_outstanding_depth(double depth);
    void advance_activity_window(std::int64_t mono_ns, bool active_now);
};

struct SessionStats {
    // Session-wide counters and the collection of peers seen during the run.
    std::string run_id;
    std::string session_id;
    std::string started_utc;
    std::string ended_utc;
    std::int64_t start_mono_ns = 0;
    std::int64_t end_mono_ns = 0;

    int tracker_announces = 0;
    int tracker_replies = 0;
    int tracker_errors = 0;
    int tracker_warnings = 0;
    int incoming_connection_count = 0;
    int successful_peer_connects = 0;
    int disconnect_count = 0;
    int peer_error_count = 0;
    int connection_failures = 0;
    int alerts_dropped_notifications = 0;
    int alerts_dropped_type_bits_total = 0;
    bool alert_loss_detected = false;
    int piece_completions = 0;
    int verification_piece_completions = 0;

    int max_peers_observed = 0;
    int max_seeds_observed = 0;
    std::int64_t initial_total_done_bytes = 0;
    std::int64_t initial_total_upload_bytes = 0;
    std::int64_t final_total_done_bytes = 0;
    std::int64_t final_total_upload_bytes = 0;
    std::int64_t total_download_bytes = 0;
    std::int64_t total_upload_bytes = 0;
    libtorrent::torrent_status::state_t latest_torrent_state =
        libtorrent::torrent_status::checking_resume_data;

    std::set<std::string> unique_peer_endpoints;
    std::unordered_map<std::string, int> connect_sequence_by_endpoint;
    std::unordered_map<std::string, std::string> active_peer_key_by_endpoint;
    std::map<std::string, PeerStats> peers_by_key;
    std::unordered_map<int, std::string> last_piece_peer_key;
    std::vector<double> session_download_rate_samples;
    std::vector<double> session_upload_rate_samples;

    SessionStats(std::string run_id_value, std::string session_id_value, TimestampPair const& start);

    PeerStats& ensure_peer_for_endpoint(
        libtorrent::tcp::endpoint const& endpoint,
        TimestampPair const& ts,
        std::string direction = "unknown",
        std::string transport = "unknown");

    PeerStats& note_peer_connect(
        libtorrent::tcp::endpoint const& endpoint,
        libtorrent::peer_id const& pid,
        TimestampPair const& ts,
        std::string const& direction,
        std::string const& transport);

    PeerStats& note_peer_event(
        libtorrent::tcp::endpoint const& endpoint,
        libtorrent::peer_id const& pid,
        TimestampPair const& ts,
        std::string direction = "unknown",
        std::string transport = "unknown");

    PeerStats* note_peer_disconnect(
        libtorrent::tcp::endpoint const& endpoint,
        libtorrent::peer_id const& pid,
        TimestampPair const& ts);

    PeerStats& note_peer_snapshot(
        libtorrent::peer_info const& peer,
        TimestampPair const& ts,
        std::int64_t snapshot_interval_ms);

    void set_initial_totals(libtorrent::torrent_status const& torrent_status);
    void note_torrent_state(libtorrent::torrent_status::state_t state);
    void note_alert_loss(int dropped_alert_type_bits);
    void note_piece_finished(int piece_index);
    void record_session_snapshot(libtorrent::torrent_status const& torrent_status);
    void finalize(TimestampPair const& end);
    std::size_t unique_peers_seen() const;
    JsonValue peer_summaries_json() const;
};

}  // namespace btclient
