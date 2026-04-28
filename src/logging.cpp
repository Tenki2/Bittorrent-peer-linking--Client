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

    // Normal runs write into artifacts/<run>/<session>. The explicit path
    // options are mainly for tests and one-off captures.
    paths.artifact_dir = std::filesystem::absolute(
        config.artifacts_dir / config.run_id / config.session_id);
    std::filesystem::create_directories(paths.artifact_dir);

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

void EventWriter::write_event(
    std::string event_type,
    JsonValue payload,
    std::optional<EventPeerContext> peer_context)
{
    TimestampPair const ts = capture_timestamp();

    // Every line in the event log is a complete JSON object. Keeping the
    // envelope fields consistent makes later analysis much easier.
    JsonValue event = JsonValue::object();
    event.add("utc_ts", ts.utc_ts);
    event.add("mono_ns", ts.mono_ns);
    event.add("run_id", config_.run_id);
    event.add("session_id", config_.session_id);
    event.add("event_type", std::move(event_type));
    event.add("node_role", config_.node_role);
    event.add("torrent_source", config_.torrent_source);
    if (info_hash_.empty())
    {
        event.add("info_hash", JsonValue(nullptr));
    }
    else
    {
        event.add("info_hash", info_hash_);
    }

    if (peer_context.has_value())
    {
        event.add("peer_key", peer_context->peer_key);
        event.add("peer_ip", peer_context->peer_ip);
        event.add("peer_port", peer_context->peer_port);
        event.add("direction", peer_context->direction);
        event.add("transport", peer_context->transport);
    }

    event.add("payload", std::move(payload));

    stream_ << event.to_string() << "\n";
    stream_.flush();
}

}  // namespace btclient
