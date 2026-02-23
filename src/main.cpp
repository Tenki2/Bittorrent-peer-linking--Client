#include <libtorrent/session.hpp>
#include <libtorrent/session_status.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace lt = libtorrent;
using namespace std::chrono_literals;

static void drain_alerts(lt::session& ses)
{
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    for (lt::alert* a : alerts)
    {
        // Keep this lightweight; you can route to JSONL later.
        // Some useful alerts you’ll likely consume later:
        // - session_stats_alert
        // - peer_log_alert / peer_error_alert (if enabled)
        // - torrent_finished_alert, etc.
        // std::cout << a->message() << "\n";

        if (auto* s = lt::alert_cast<lt::session_stats_alert>(a))
        {
            // s->values() contains a snapshot aligned with lt::session_stats_metrics()
            // You can map indexes->names once at startup.
            (void)s; // placeholder to avoid unused warning
        }
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




int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage:\n  btprobe \"<magnet-uri>\" [runtime_sec]\n";
        return 2;
    }

    std::string magnet = argv[1];
    int runtime_sec = (argc >= 3) ? std::stoi(argv[2]) : 60;

    lt::settings_pack sp;

    // default settings for prototype, will disable dht and potentially others for prod.
    sp.set_bool(lt::settings_pack::enable_dht, true);
    sp.set_bool(lt::settings_pack::enable_lsd, true);
    sp.set_bool(lt::settings_pack::enable_upnp, false);
    sp.set_bool(lt::settings_pack::enable_natpmp, false);

    // Alerts: include session stats; add more later.
    sp.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::session_log |
        lt::alert_category::stats);

    lt::session ses(sp);

    // Ask libtorrent to emit periodic stats snapshots.
    ses.post_session_stats();

    lt::add_torrent_params atp = lt::parse_magnet_uri(magnet);
    atp.save_path = "./data";
    lt::torrent_handle th = ses.add_torrent(atp);

    std::cout << "Added torrent. Running for " << runtime_sec << " seconds...\n";

    for (int t = 0; t < runtime_sec; ++t)
    {
        // session_status is handy for “health” metrics
        lt::session_status ss = ses.status();

        // torrent_status gives per-torrent counters
        lt::torrent_status ts = th.status();
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