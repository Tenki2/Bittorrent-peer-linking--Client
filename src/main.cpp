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

namespace lt = libtorrent;
using namespace std::chrono_literals;

static std::string peer_id_to_hex(lt::peer_id const& pid)
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

static void drain_alerts(lt::session& ses)
{
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    for (lt::alert* a : alerts)
    {
        if (auto* al = lt::alert_cast<lt::listen_succeeded_alert>(a))
            std::cout << "[listen-ok] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::listen_failed_alert>(a))
            std::cout << "[listen-fail] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::tracker_announce_alert>(a))
            std::cout << "[tracker-announce] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::tracker_reply_alert>(a))
            std::cout << "[tracker-reply] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::tracker_error_alert>(a))
            std::cout << "[tracker-error] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_connect_alert>(a))
            std::cout << "[peer-connect] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_disconnected_alert>(a))
            std::cout << "[peer-disconnect] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::torrent_finished_alert>(a))
            std::cout << "[finished] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::state_changed_alert>(a))
            std::cout << "[state-change] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_connect_alert>(a))
            std::cout << "[peer-connect] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_error_alert>(a))
            std::cout << "[peer-error] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_disconnected_alert>(a))
            std::cout << "[peer-disconnect] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::incoming_connection_alert>(a))
            std::cout << "[incoming-conn] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_log_alert>(a))
            std::cout << "[peer-log] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_connect_alert>(a))
            std::cout << "[peer-connect] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_error_alert>(a))
            std::cout << "[peer-error] " << al->message() << "\n";

        if (auto* al = lt::alert_cast<lt::peer_disconnected_alert>(a))
            std::cout << "[peer-disconnect] " << al->message() << "\n";
    }
}

static const char* state_name(libtorrent::torrent_status::state_t s)
// Get the name of the state. returns number value if this isnt here
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

static bool is_magnet_uri(std::string const& input)
{
    return input.rfind("magnet:?", 0) == 0;
}



int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage:\n  btprobe \"<magnet-uri|torrent-file>\" [runtime_sec] [listen_port]\n"
                  << "  runtime_sec: 0 means run indefinitely\n"
                  << "  listen_port: 0-65535 (default 51413)\n";
        return 2;
    }

    std::string source = argv[1];
    int runtime_sec = (argc >= 3) ? std::stoi(argv[2]) : 60;
    int listen_port = (argc >= 4) ? std::stoi(argv[3]) : 51413;
    bool run_forever = (runtime_sec == 0);
    if (listen_port < 0 || listen_port > 65535)
    {
        std::cerr << "Invalid listen_port: must be between 0 and 65535\n";
        return 2;
    }
    std::string announceIp = (argc >= 5) ? argv[4] : "";
    lt::settings_pack sp;

    // default settings for prototype, will disable dht and potentially others for prod.
    sp.set_bool(lt::settings_pack::enable_dht, false);
    sp.set_bool(lt::settings_pack::enable_lsd, false);
    sp.set_bool(lt::settings_pack::enable_upnp, false);
    sp.set_bool(lt::settings_pack::enable_natpmp, false);

    // Prevent libtorrent from aggressively banning local subnet IPs during testing
    sp.set_bool(lt::settings_pack::ssrf_mitigation, false);
    sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    if (!announceIp.empty()) {
        sp.set_str(lt::settings_pack::announce_ip, announceIp);
    }

    std::string listen_ifaces = "0.0.0.0:" + std::to_string(listen_port) + ",[::]:" + std::to_string(listen_port);
    sp.set_str(lt::settings_pack::listen_interfaces, listen_ifaces);
    // Alerts: include session stats; add more later.
    sp.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::session_log |
        lt::alert_category::torrent_log | // Captures internal torrent engine logic
        lt::alert_category::peer_log |    // Captures the exact reason a peer is dropped
        lt::alert_category::ip_block |    // Alerts if an IP is filtered
        lt::alert_category::stats |
        lt::alert_category::tracker |
        lt::alert_category::peer);

    lt::session ses(sp);

    // Ask libtorrent to emit periodic stats snapshots.
    ses.post_session_stats();

    lt::add_torrent_params atp;
    lt::error_code ec;
    if (is_magnet_uri(source))
    {
        atp = lt::parse_magnet_uri(source, ec);
        if (ec)
        {
            std::cerr << "Invalid magnet URI: " << ec.message() << "\n";
            return 2;
        }
    }
    else
    {
        auto ti = std::make_shared<lt::torrent_info>(source, ec);
        if (ec)
        {
            std::cerr << "Failed to load torrent file '" << source << "': " << ec.message() << "\n";
            return 2;
        }

        atp.ti = ti;
    }

    atp.save_path = "./data";
    lt::torrent_handle th = ses.add_torrent(atp, ec);
    // Diagnostic: Print the exact Info Hash the engine is using
    lt::torrent_status torrentStatus = th.status();
    std::cout << "\n=== DIAGNOSTICS ===\n";
    std::cout << "Parsed Info Hash: " << torrentStatus.info_hashes.v1 << "\n";
    std::cout << "===================\n\n";
    if (ec)
    {
        std::cerr << "Failed to add torrent: " << ec.message() << "\n";
        return 2;
    }


    if (run_forever)
    {
        std::cout << "Added torrent. Running indefinitely (runtime_sec=0)...\n";
    }
    else
    {
        std::cout << "Added torrent. Running for " << runtime_sec << " seconds...\n";
    }

    for (int t = 0; run_forever || t < runtime_sec; ++t)
    {
        // session_status is handy for “health” metrics
        lt::session_status ss = ses.status();

        // torrent_status gives per-torrent counters
        lt::torrent_status ts = th.status();
        std::vector<lt::peer_info> peers;
        th.get_peer_info(peers);

        std::ostringstream peer_ids;
        peer_ids << "[";
        if (peers.empty())
        {
            peer_ids << "none";
        }
        else
        {
            std::size_t const max_ids = 5;
            std::size_t const shown = (std::min)(peers.size(), max_ids);
            for (std::size_t i = 0; i < shown; ++i)
            {
                if (i > 0) peer_ids << ",";
                peer_ids << peer_id_to_hex(peers[i].pid);
            }

            if (peers.size() > shown)
            {
                peer_ids << ",...+" << (peers.size() - shown);
            }
        }
        peer_ids << "]";
        std::cout
            << "t=" << t
            << " state=" << state_name(ts.state) << "(" << static_cast<int>(ts.state) << ")"
            << " progress=" << (ts.progress * 100.0f) << "%"
            << " seeding=" << (ts.is_seeding ? "yes" : "no")
            << " peers=" << ts.num_peers
            << " seeds=" << ts.num_seeds
            << " down=" << ts.total_done << "B"
            << " up=" << ts.total_upload << "B"
            << " dls=" << ts.download_rate << "B/s"
            << " uls=" << ts.upload_rate << "B/s"
            << " conns=" << ss.num_peers
            << " listening=" << (ses.is_listening() ? "yes" : "no") // remove later
            << " listen_port=" << ses.listen_port() // remove later
            << " peer_ids=" << peer_ids.str()
            << "\n";

        // Trigger periodic stats snapshot
        ses.post_session_stats();

        // Drain alerts (where deeper stats arrive)
        drain_alerts(ses);

        std::this_thread::sleep_for(1s);
    }

    std::cout << "Done.\n";
    return 0;
}
