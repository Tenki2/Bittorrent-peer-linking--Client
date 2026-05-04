#include "logging.hpp"

#include <stdexcept>

namespace btclient {

namespace {

void ensure_parent_directory(std::filesystem::path const& file_path)
{
    std::filesystem::path const parent = file_path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
}

}  // namespace

OutputPaths prepare_output_paths(
    RuntimeConfig const& config,
    std::optional<std::filesystem::path> const& event_log_override,
    std::optional<std::filesystem::path> const& summary_json_override)
{
    OutputPaths paths;

    // Normal runs write into artifacts/pending/<session>. The explicit path
    // options are mainly for tests and one-off captures.
    paths.artifact_dir = std::filesystem::absolute(
        config.artifacts_dir / "pending" / config.session_id);
    std::filesystem::path const archive_dir = std::filesystem::absolute(
        config.artifacts_dir / "archive" / config.session_id);
    if (std::filesystem::exists(archive_dir))
    {
        throw std::runtime_error(
            "archive artifact directory already exists: " + archive_dir.string()
            + "; choose a different --session-id");
    }
    if (std::filesystem::exists(paths.artifact_dir))
    {
        if (!std::filesystem::is_directory(paths.artifact_dir))
        {
            throw std::runtime_error(
                "artifact path exists but is not a directory: " + paths.artifact_dir.string());
        }
        if (!std::filesystem::is_empty(paths.artifact_dir))
        {
            throw std::runtime_error(
                "artifact directory already exists and is not empty: "
                + paths.artifact_dir.string()
                + "; choose a different --session-id or resolve the pending artifacts");
        }
    }
    std::filesystem::create_directories(paths.artifact_dir);

    paths.state_json_path = paths.artifact_dir / "state.json";
    paths.event_log_path = event_log_override.has_value()
        ? std::filesystem::absolute(*event_log_override)
        : (paths.artifact_dir / "session_events.ndjson");
    paths.summary_json_path = summary_json_override.has_value()
        ? std::filesystem::absolute(*summary_json_override)
        : (paths.artifact_dir / "session_summary.json");

    ensure_parent_directory(paths.event_log_path);
    ensure_parent_directory(paths.summary_json_path);

    return paths;
}

EventWriter::EventWriter(RuntimeConfig const& config, OutputPaths const& output_paths)
    : config_(config)
    , output_paths_(output_paths)
    , stream_(output_paths_.event_log_path, std::ios::out | std::ios::trunc)
{
    if (!stream_)
    {
        throw std::runtime_error(
            "failed to open event log at " + output_paths_.event_log_path.string());
    }
}

void EventWriter::set_info_hash(std::string info_hash)
{
    info_hash_ = std::move(info_hash);
}

void EventWriter::close()
{
    if (!stream_.is_open()) return;

    stream_.close();
    if (!stream_)
    {
        throw std::runtime_error(
            "failed to close event log at " + output_paths_.event_log_path.string());
    }
}

void EventWriter::write_event(
    std::string event_type,
    json payload,
    std::optional<EventPeerContext> peer_context)
{
    TimestampPair const ts = capture_timestamp();

    // Every line in the event log is a complete JSON object. Keeping the
    // envelope fields consistent makes later analysis much easier.
    json event = {
        {"utc_ts", ts.utc_ts},
        {"mono_ns", ts.mono_ns},
        {"run_id", config_.run_id},
        {"session_id", config_.session_id},
        {"event_type", std::move(event_type)},
        {"node_role", config_.node_role},
        {"torrent_source", config_.torrent_source},
        {"info_hash", info_hash_.empty() ? json(nullptr) : json(info_hash_)},
    };

    if (peer_context.has_value())
    {
        event["peer_key"] = peer_context->peer_key;
        event["peer_ip"] = peer_context->peer_ip;
        event["peer_port"] = peer_context->peer_port;
        event["direction"] = peer_context->direction;
        event["transport"] = peer_context->transport;
    }

    event["payload"] = std::move(payload);

    stream_ << event.dump() << "\n";
    stream_.flush();
}

}  // namespace btclient
