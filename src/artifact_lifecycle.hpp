#pragma once

#include "logging.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace btclient {

enum class ArtifactState {
    Capturing,
    ReadyToUpload,
    Archived,
    Unknown,
};

struct PendingArtifactSession {
    OutputPaths output_paths;
    std::string run_id;
    std::string session_id;
    ArtifactState state = ArtifactState::Unknown;
    std::string state_text;
    bool has_state_file = false;
    bool state_file_valid = false;
};

std::string artifact_state_name(ArtifactState state);
ArtifactState artifact_state_from_string(std::string const& value);

std::filesystem::path pending_artifacts_dir(std::filesystem::path const& artifacts_dir);
std::filesystem::path archive_artifacts_dir(std::filesystem::path const& artifacts_dir);
OutputPaths artifact_paths_for_session_dir(std::filesystem::path const& session_dir);

void write_artifact_state(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    ArtifactState state);

void record_artifact_upload_failure(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    std::string const& error_message);

std::vector<PendingArtifactSession> find_pending_artifact_sessions(
    std::filesystem::path const& artifacts_dir);

OutputPaths archive_artifact_session(
    RuntimeConfig const& config,
    OutputPaths const& pending_output_paths);

}  // namespace btclient
