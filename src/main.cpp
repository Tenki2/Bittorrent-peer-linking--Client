#include <libtorrent/session.hpp>
#include <libtorrent/session_status.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/peer_info.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>

namespace lt = libtorrent;
using namespace std::chrono_literals;

static std::string peerIdToHex(lt::peer_id const& pid)
{
    if (pid.is_all_zeros()) return "unknown";

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t b : pid)
    {
        out << std::setw(2) << static_cast<int>(b);
    }
    return out.str();
}

static void drainAlerts(lt::session& session)
{
    std::vector<lt::alert*> alerts;
    session.pop_alerts(&alerts);

    for (lt::alert* a : alerts)
    {
        if (auto* al = lt::alert_cast<lt::listen_succeeded_alert>(a))
            std::cout << "[listen-ok] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::listen_failed_alert>(a))
            std::cout << "[listen-fail] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::tracker_announce_alert>(a))
            std::cout << "[tracker-announce] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::tracker_reply_alert>(a))
            std::cout << "[tracker-reply] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::tracker_error_alert>(a))
            std::cout << "[tracker-error] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::torrent_finished_alert>(a))
            std::cout << "[finished] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::state_changed_alert>(a))
            std::cout << "[state-change] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::peer_connect_alert>(a))
            std::cout << "[peer-connect] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::peer_error_alert>(a))
            std::cout << "[peer-error] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::peer_disconnected_alert>(a))
            std::cout << "[peer-disconnect] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::incoming_connection_alert>(a))
            std::cout << "[incoming-conn] " << al->message() << "\n";

        else if (auto* al = lt::alert_cast<lt::peer_log_alert>(a))
            std::cout << "[peer-log] " << al->message() << "\n";
    }
}

static const char* stateName(libtorrent::torrent_status::state_t s)
{
    using st = libtorrent::torrent_status::state_t;
    switch (s)
    {
        case st::queued_for_checking:  return "queued_for_checking";
        case st::checking_files:       return "checking_files";
        case st::downloading_metadata: return "downloading_metadata";
        case st::downloading:          return "downloading";
        case st::finished:             return "finished";
        case st::seeding:              return "seeding";
        case st::allocating:           return "allocating";
        case st::checking_resume_data: return "checking_resume_data";
        default:                       return "unknown";
    }
}

static bool isMagnetUri(std::string const& input)
{
    return input.rfind("magnet:?", 0) == 0;
}

static void printUsage()
{
    std::cerr << "Usage: btprobe -f <magnet-uri|torrent-file> [options]\n\n"
              << "Options:\n"
              << "  -f, --file <path>   Path to the .torrent file or magnet URI (Required)\n"
              << "  -t, --time <sec>    Runtime in seconds (0 means run indefinitely. Default: 60)\n"
              << "  -p, --port <port>   Listen port (0-65535. Default: 51413)\n"
              << "  -i, --ip <address>  Explicit IP to announce to the tracker (Default: none)\n"
              << "  -h, --help          Show this help message\n";
}

int main(int argc, char** argv)
{
    std::string torrentSource;
    int runtimeSec = 60;
    int listenPort = 51413;
    std::string announceIp = "";

    // Parse command line flags
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        try {
            if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
                torrentSource = argv[++i];
            } else if ((arg == "-t" || arg == "--time") && i + 1 < argc) {
                runtimeSec = std::stoi(argv[++i]);
            } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
                listenPort = std::stoi(argv[++i]);
            } else if ((arg == "-i" || arg == "--ip") && i + 1 < argc) {
                announceIp = argv[++i];
            } else if (arg == "-h" || arg == "--help") {
                printUsage();
                return 0;
            } else {
                std::cerr << "Unknown or incomplete argument: " << arg << "\n";
                printUsage();
                return 2;
            }
        } catch (const std::invalid_argument& e) {
            std::cerr << "Invalid number format for flag: " << arg << "\n";
            return 2;
        }
    }

    if (torrentSource.empty())
    {
        std::cerr << "Error: Torrent source (-f) is required.\n\n";
        printUsage();
        return 2;
    }

    if (listenPort < 0 || listenPort > 65535)
    {
        std::cerr << "Error: Invalid listen port. Must be between 0 and 65535\n";
        return 2;
    }

    bool runForever = (runtimeSec == 0);
    lt::settings_pack settingsPack;

    // Discovery settings
    settingsPack.set_bool(lt::settings_pack::enable_dht, false);
    settingsPack.set_bool(lt::settings_pack::enable_lsd, false);
    settingsPack.set_bool(lt::settings_pack::enable_upnp, false);
    settingsPack.set_bool(lt::settings_pack::enable_natpmp, false);

    // Peer and tracker settings
    settingsPack.set_bool(lt::settings_pack::ssrf_mitigation, false);
    settingsPack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    
    if (!announceIp.empty()) {
        settingsPack.set_str(lt::settings_pack::announce_ip, announceIp);
    }

    std::string listenIfaces = "0.0.0.0:" + std::to_string(listenPort) + ",[::]:" + std::to_string(listenPort);
    settingsPack.set_str(lt::settings_pack::listen_interfaces, listenIfaces);
    
    // Alerts mask
    settingsPack.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::session_log |
        lt::alert_category::torrent_log |
        lt::alert_category::peer_log |
        lt::alert_category::ip_block |
        lt::alert_category::stats |
        lt::alert_category::tracker |
        lt::alert_category::peer);

    lt::session session(settingsPack);
    session.post_session_stats();

    lt::add_torrent_params addTorrentParams;
    lt::error_code errorCode;
    
    if (isMagnetUri(torrentSource))
    {
        addTorrentParams = lt::parse_magnet_uri(torrentSource, errorCode);
        if (errorCode)
        {
            std::cerr << "Invalid magnet URI: " << errorCode.message() << "\n";
            return 2;
        }
    }
    else
    {
        auto torrentInfo = std::make_shared<lt::torrent_info>(torrentSource, errorCode);
        if (errorCode)
        {
            std::cerr << "Failed to load torrent file '" << torrentSource << "': " << errorCode.message() << "\n";
            return 2;
        }
        addTorrentParams.ti = torrentInfo;
    }

    addTorrentParams.save_path = "./data";
    lt::torrent_handle torrentHandle = session.add_torrent(addTorrentParams, errorCode);
    
    lt::torrent_status initialStatus = torrentHandle.status();
    std::cout << "\n=== DIAGNOSTICS ===\n";
    std::cout << "Parsed Info Hash: " << initialStatus.info_hashes.v1 << "\n";
    std::cout << "===================\n\n";
    
    if (errorCode)
    {
        std::cerr << "Failed to add torrent: " << errorCode.message() << "\n";
        return 2;
    }

    if (runForever)
    {
        std::cout << "Added torrent. Running indefinitely (-t 0)...\n";
    }
    else
    {
        std::cout << "Added torrent. Running for " << runtimeSec << " seconds...\n";
    }

    for (int t = 0; runForever || t < runtimeSec; ++t)
    {
        lt::session_status sessionStatus = session.status();
        lt::torrent_status torrentStatus = torrentHandle.status();
        
        std::vector<lt::peer_info> peers;
        torrentHandle.get_peer_info(peers);

        std::ostringstream peerIds;
        peerIds << "[";
        if (peers.empty())
        {
            peerIds << "none";
        }
        else
        {
            std::size_t const maxIds = 5;
            std::size_t const shown = (std::min)(peers.size(), maxIds);
            for (std::size_t i = 0; i < shown; ++i)
            {
                if (i > 0) peerIds << ",";
                peerIds << peerIdToHex(peers[i].pid);
            }

            if (peers.size() > shown)
            {
                peerIds << ",...+" << (peers.size() - shown);
            }
        }
        peerIds << "]";
        
        std::cout
            << "t=" << t
            << " state=" << stateName(torrentStatus.state) << "(" << static_cast<int>(torrentStatus.state) << ")"
            << " progress=" << (torrentStatus.progress * 100.0f) << "%"
            << " seeding=" << (torrentStatus.is_seeding ? "yes" : "no")
            << " peers=" << torrentStatus.num_peers
            << " seeds=" << torrentStatus.num_seeds
            << " down=" << torrentStatus.total_done << "B"
            << " up=" << torrentStatus.total_upload << "B"
            << " dls=" << torrentStatus.download_rate << "B/s"
            << " uls=" << torrentStatus.upload_rate << "B/s"
            << " conns=" << sessionStatus.num_peers
            << " listening=" << (session.is_listening() ? "yes" : "no")
            << " listen_port=" << session.listen_port()
            << " peer_ids=" << peerIds.str()
            << "\n";

        session.post_session_stats();
        drainAlerts(session);
        std::this_thread::sleep_for(1s);
    }

    std::cout << "Done.\n";
    return 0;
}