#pragma once

#include "logging.hpp"

#include <string>

namespace btclient {

struct ArtifactSendResult {
    int status_code = 0;
    std::string status_text;
    std::string response_body;
};

ArtifactSendResult send_artifacts_to_destination(
    RuntimeConfig const& config,
    OutputPaths const& output_paths);

}  // namespace btclient
