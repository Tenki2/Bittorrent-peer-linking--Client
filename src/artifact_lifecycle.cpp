#include "artifact_lifecycle.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace btclient {

namespace {

std::string json_string_or_empty(json const& value, char const* key)
{
    auto const it = value.find(key);
    if (it == value.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

json read_json_file(std::filesystem::path const& path)
{
    std::ifstream stream(path);
    if (!stream)
    {
        throw std::runtime_error("failed to open JSON file: " + path.string());
    }

    json parsed;
    stream >> parsed;
    return parsed;
}

std::filesystem::path path_from_state_file(
    json const& state,
    std::filesystem::path const& session_dir,
    char const* key,
    std::filesystem::path fallback)
{
    auto const files = state.find("files");
    if (files == state.end() || !files->is_object()) return fallback;

    std::string const value = json_string_or_empty(*files, key);
    if (value.empty()) return fallback;

    std::filesystem::path path(value);
    if (path.is_relative())
    {
        path = session_dir / path;
    }
    return std::filesystem::absolute(path);
}

void ensure_parent_directory(std::filesystem::path const& file_path)
{
    std::filesystem::path const parent = file_path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
}

void write_json_file(std::filesystem::path const& path, json const& value)
{
    ensure_parent_directory(path);

    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("failed to open JSON output at " + path.string());
    }

    stream << value.dump(2) << "\n";
    if (!stream)
    {
        throw std::runtime_error("failed to write JSON output at " + path.string());
    }
}

std::string created_utc_for_state(OutputPaths const& output_paths, TimestampPair const& now)
{
    if (!std::filesystem::exists(output_paths.state_json_path))
    {
        return now.utc_ts;
    }

    try
    {
        json const previous = read_json_file(output_paths.state_json_path);
        std::string const created_utc = json_string_or_empty(previous, "created_utc");
        if (!created_utc.empty()) return created_utc;
    }
    catch (...)
    {
        // If the previous state file is damaged, replace it with a valid state.
    }

    return now.utc_ts;
}

json load_existing_state_or_empty(OutputPaths const& output_paths)
{
    if (!std::filesystem::exists(output_paths.state_json_path))
    {
        return json::object();
    }

    try
    {
        json previous = read_json_file(output_paths.state_json_path);
        if (previous.is_object()) return previous;
    }
    catch (...)
    {
        // Damaged state files are replaced with a valid manifest below.
    }

    return json::object();
}

json base_state_json(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    ArtifactState state,
    TimestampPair const& now)
{
    json previous = load_existing_state_or_empty(output_paths);

    json state_json = {
        {"schema_version", 1},
        {"state", artifact_state_name(state)},
        {"run_id", config.run_id},
        {"session_id", config.session_id},
        {"created_utc", created_utc_for_state(output_paths, now)},
        {"updated_utc", now.utc_ts},
        {"artifact_dir", output_paths.artifact_dir.string()},
        {"destination_url_configured", !config.destination_url.empty()},
        {
            "files",
            {
                {"state_json", output_paths.state_json_path.string()},
                {"event_log", output_paths.event_log_path.string()},
                {"summary_json", output_paths.summary_json_path.string()},
            },
        },
    };

    auto const failures = previous.find("upload_failures");
    if (failures != previous.end() && failures->is_array())
    {
        state_json["upload_failures"] = *failures;
    }

    auto const last_failure = previous.find("last_upload_failure");
    if (last_failure != previous.end() && last_failure->is_object())
    {
        state_json["last_upload_failure"] = *last_failure;
    }

    auto const attempt_count = previous.find("upload_attempt_count");
    if (attempt_count != previous.end() && attempt_count->is_number_integer())
    {
        state_json["upload_attempt_count"] = *attempt_count;
    }

    auto const last_attempt_utc = previous.find("last_upload_attempt_utc");
    if (last_attempt_utc != previous.end() && last_attempt_utc->is_string())
    {
        state_json["last_upload_attempt_utc"] = *last_attempt_utc;
    }

    return state_json;
}

}  // namespace

std::string artifact_state_name(ArtifactState state)
{
    switch (state)
    {
        case ArtifactState::Capturing:
            return "capturing";
        case ArtifactState::ReadyToUpload:
            return "ready_to_upload";
        case ArtifactState::Archived:
            return "archived";
        case ArtifactState::Unknown:
        default:
            return "unknown";
    }
}

ArtifactState artifact_state_from_string(std::string const& value)
{
    if (value == "capturing") return ArtifactState::Capturing;
    if (value == "ready_to_upload") return ArtifactState::ReadyToUpload;
    if (value == "archived") return ArtifactState::Archived;
    return ArtifactState::Unknown;
}

std::filesystem::path pending_artifacts_dir(std::filesystem::path const& artifacts_dir)
{
    return std::filesystem::absolute(artifacts_dir / "pending");
}

std::filesystem::path archive_artifacts_dir(std::filesystem::path const& artifacts_dir)
{
    return std::filesystem::absolute(artifacts_dir / "archive");
}

OutputPaths artifact_paths_for_session_dir(std::filesystem::path const& session_dir)
{
    OutputPaths paths;
    paths.artifact_dir = std::filesystem::absolute(session_dir);
    paths.state_json_path = paths.artifact_dir / "state.json";
    paths.event_log_path = paths.artifact_dir / "session_events.ndjson";
    paths.summary_json_path = paths.artifact_dir / "session_summary.json";
    return paths;
}

void write_artifact_state(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    ArtifactState state)
{
    std::filesystem::create_directories(output_paths.artifact_dir);

    TimestampPair const now = capture_timestamp();
    json state_json = base_state_json(config, output_paths, state, now);

    write_json_file(output_paths.state_json_path, state_json);
}

void record_artifact_upload_failure(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    std::string const& error_message)
{
    std::filesystem::create_directories(output_paths.artifact_dir);

    TimestampPair const now = capture_timestamp();
    json state_json =
        base_state_json(config, output_paths, ArtifactState::ReadyToUpload, now);

    std::int64_t attempt_count = 0;
    auto const attempt_count_it = state_json.find("upload_attempt_count");
    if (attempt_count_it != state_json.end() && attempt_count_it->is_number_integer())
    {
        attempt_count = attempt_count_it->get<std::int64_t>();
    }
    ++attempt_count;

    json failure = {
        {"attempt_utc", now.utc_ts},
        {"destination_url", config.destination_url},
        {"error", error_message},
    };

    if (!state_json.contains("upload_failures") || !state_json["upload_failures"].is_array())
    {
        state_json["upload_failures"] = json::array();
    }
    state_json["upload_failures"].push_back(failure);
    state_json["last_upload_failure"] = std::move(failure);
    state_json["upload_attempt_count"] = attempt_count;
    state_json["last_upload_attempt_utc"] = now.utc_ts;

    write_json_file(output_paths.state_json_path, state_json);
}

std::vector<PendingArtifactSession> find_pending_artifact_sessions(
    std::filesystem::path const& artifacts_dir)
{
    std::vector<PendingArtifactSession> sessions;
    std::filesystem::path const pending_dir = pending_artifacts_dir(artifacts_dir);
    if (!std::filesystem::exists(pending_dir)) return sessions;

    for (auto const& entry : std::filesystem::directory_iterator(pending_dir))
    {
        if (!entry.is_directory()) continue;

        PendingArtifactSession session;
        session.output_paths = artifact_paths_for_session_dir(entry.path());
        session.session_id = session.output_paths.artifact_dir.filename().string();
        session.has_state_file = std::filesystem::exists(session.output_paths.state_json_path);

        if (session.has_state_file)
        {
            try
            {
                json const state_json = read_json_file(session.output_paths.state_json_path);
                session.state_text = json_string_or_empty(state_json, "state");
                session.state = artifact_state_from_string(session.state_text);
                session.run_id = json_string_or_empty(state_json, "run_id");

                std::string const manifest_session_id =
                    json_string_or_empty(state_json, "session_id");
                if (!manifest_session_id.empty())
                {
                    session.session_id = manifest_session_id;
                }

                session.output_paths.state_json_path = path_from_state_file(
                    state_json,
                    session.output_paths.artifact_dir,
                    "state_json",
                    session.output_paths.state_json_path);
                session.output_paths.event_log_path = path_from_state_file(
                    state_json,
                    session.output_paths.artifact_dir,
                    "event_log",
                    session.output_paths.event_log_path);
                session.output_paths.summary_json_path = path_from_state_file(
                    state_json,
                    session.output_paths.artifact_dir,
                    "summary_json",
                    session.output_paths.summary_json_path);

                session.state_file_valid = true;
            }
            catch (...)
            {
                session.state = ArtifactState::Unknown;
                session.state_text = "invalid";
            }
        }

        sessions.push_back(std::move(session));
    }

    std::sort(
        sessions.begin(),
        sessions.end(),
        [](PendingArtifactSession const& lhs, PendingArtifactSession const& rhs) {
            return lhs.output_paths.artifact_dir.filename()
                < rhs.output_paths.artifact_dir.filename();
        });
    return sessions;
}

OutputPaths archive_artifact_session(
    RuntimeConfig const& config,
    OutputPaths const& pending_output_paths)
{
    std::filesystem::path const archive_dir =
        archive_artifacts_dir(config.artifacts_dir)
        / pending_output_paths.artifact_dir.filename();

    if (std::filesystem::exists(archive_dir))
    {
        throw std::runtime_error(
            "archive artifact directory already exists: " + archive_dir.string());
    }

    std::filesystem::create_directories(archive_dir.parent_path());
    std::filesystem::rename(pending_output_paths.artifact_dir, archive_dir);

    OutputPaths archived_output_paths = artifact_paths_for_session_dir(archive_dir);
    write_artifact_state(config, archived_output_paths, ArtifactState::Archived);
    return archived_output_paths;
}

}  // namespace btclient
