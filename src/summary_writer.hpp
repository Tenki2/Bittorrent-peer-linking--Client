#pragma once

#include "logging.hpp"
#include "peer_stats.hpp"

#include <libtorrent/torrent_status.hpp>

#include <string>

namespace btclient {

void write_session_summary(
    RuntimeConfig const& config,
    OutputPaths const& output_paths,
    SessionStats const& stats,
    libtorrent::torrent_status const& final_torrent_status,
    std::string const& info_hash,
    std::string const& exit_reason,
    int actual_listen_port,
    bool is_listening);

}  // namespace btclient
