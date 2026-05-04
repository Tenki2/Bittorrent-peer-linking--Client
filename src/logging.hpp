#pragma once

#include "util.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace btclient {

inline constexpr char kClientVersion[] = "btclient/0.2.0";

struct RuntimeConfig {
    std::string torrent_source;
    std::string torrent_source_type;
    std::string run_id;
    std::string session_id;
    std::string node_role = "unknown";
    std::string announce_ip;
    std::string client_label;
    std::string profile_id;
    std::string destination_url;
    std::filesystem::path artifacts_dir = "./artifacts";
    std::filesystem::path save_path = "./data";
    int runtime_seconds = 60;
    int listen_port = 51413;
    std::int64_t snapshot_interval_ms = 1000;
    bool console_status = true;
    bool enable_dht = false;
    bool enable_lsd = false;
    bool enable_upnp = false;
    bool enable_natpmp = false;
};

struct OutputPaths {
    std::filesystem::path artifact_dir;
    std::filesystem::path state_json_path;
    std::filesystem::path event_log_path;
    std::filesystem::path summary_json_path;
};

struct EventPeerContext {
    std::string peer_key;
    std::string peer_ip;
    int peer_port = 0;
    std::string direction = "unknown";
    std::string transport = "unknown";
};

OutputPaths prepare_output_paths(
    RuntimeConfig const& config,
    std::optional<std::filesystem::path> const& event_log_override,
    std::optional<std::filesystem::path> const& summary_json_override);

class EventWriter {
public:
    EventWriter(RuntimeConfig const& config, OutputPaths const& output_paths);

    void set_info_hash(std::string info_hash);
    void close();
    void write_event(
        std::string event_type,
        json payload,
        std::optional<EventPeerContext> peer_context = std::nullopt);

private:
    RuntimeConfig config_;
    OutputPaths output_paths_;
    std::ofstream stream_;
    std::string info_hash_;
};

}  // namespace btclient
