#include "util.hpp"

#include <libtorrent/time.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <unistd.h>

namespace lt = libtorrent;

namespace btclient {

namespace {

double percentile(std::vector<double> const& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) return 0.0;
    if (sorted_samples.size() == 1) return sorted_samples.front();

    // Interpolate between neighboring samples instead of snapping to one item.
    double const position = fraction * static_cast<double>(sorted_samples.size() - 1);
    std::size_t const lower_index = static_cast<std::size_t>(std::floor(position));
    std::size_t const upper_index = static_cast<std::size_t>(std::ceil(position));
    double const weight = position - static_cast<double>(lower_index);

    return sorted_samples[lower_index] * (1.0 - weight)
        + sorted_samples[upper_index] * weight;
}

json optional_number(std::optional<double> const& value)
{
    if (!value.has_value()) return nullptr;
    return *value;
}

}  // namespace

TimestampPair capture_timestamp()
{
    TimestampPair ts;
    ts.utc_ts = format_utc(std::chrono::system_clock::now());
    ts.mono_ns = monotonic_now_ns();
    return ts;
}

std::string format_utc(std::chrono::system_clock::time_point tp)
{
    std::time_t const raw_time = std::chrono::system_clock::to_time_t(tp);
    std::tm utc_tm{};
    gmtime_r(&raw_time, &utc_tm);

    auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count() % 1000;

    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S")
        << "."
        << std::setw(3)
        << std::setfill('0')
        << millis
        << "Z";
    return out.str();
}

std::string compact_utc_stamp(std::chrono::system_clock::time_point tp)
{
    std::time_t const raw_time = std::chrono::system_clock::to_time_t(tp);
    std::tm utc_tm{};
    gmtime_r(&raw_time, &utc_tm);

    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y%m%dT%H%M%SZ");
    return out.str();
}

std::int64_t monotonic_now_ns()
{
    // steady_clock is for durations. It does not jump if the system clock is
    // adjusted while a run is in progress.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ns_to_ms(std::int64_t ns)
{
    return static_cast<double>(ns) / 1'000'000.0;
}

DistributionSummary summarize_samples(std::vector<double> const& samples)
{
    DistributionSummary summary;
    summary.count = samples.size();
    if (samples.empty()) return summary;

    // Sort a copy so callers can keep appending samples in capture order.
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    double const sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    double const mean = sum / static_cast<double>(sorted.size());

    double variance = 0.0;
    for (double const sample : sorted)
    {
        double const delta = sample - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(sorted.size());

    summary.min = sorted.front();
    summary.max = sorted.back();
    summary.mean = mean;
    summary.stddev = std::sqrt(variance);
    summary.p50 = percentile(sorted, 0.50);
    summary.p95 = percentile(sorted, 0.95);
    return summary;
}

json distribution_summary_to_json(DistributionSummary const& summary)
{
    return {
        {"count", static_cast<std::int64_t>(summary.count)},
        {"min", optional_number(summary.min)},
        {"max", optional_number(summary.max)},
        {"mean", optional_number(summary.mean)},
        {"stddev", optional_number(summary.stddev)},
        {"p50", optional_number(summary.p50)},
        {"p95", optional_number(summary.p95)},
    };
}

json strings_to_json_array(std::vector<std::string> const& values)
{
    return values;
}

std::string generate_id(std::string const& prefix)
{
    std::ostringstream out;
    out << prefix
        << "-"
        << compact_utc_stamp(std::chrono::system_clock::now())
        << "-"
        << ::getpid()
        << "-"
        << (monotonic_now_ns() % 1'000'000);
    return out.str();
}

std::string get_hostname()
{
    char buffer[256];
    if (::gethostname(buffer, sizeof(buffer)) != 0)
    {
        return {};
    }

    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

bool is_magnet_uri(std::string const& input)
{
    return input.rfind("magnet:?", 0) == 0;
}

std::string state_name(lt::torrent_status::state_t state)
{
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    using state_t = lt::torrent_status::state_t;

    switch (state)
    {
        case state_t::queued_for_checking:
            return "queued_for_checking";
        case state_t::checking_files:
            return "checking_files";
        case state_t::downloading_metadata:
            return "downloading_metadata";
        case state_t::downloading:
            return "downloading";
        case state_t::finished:
            return "finished";
        case state_t::seeding:
            return "seeding";
        case state_t::allocating:
            return "allocating";
        case state_t::checking_resume_data:
            return "checking_resume_data";
    }

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return "unknown";
}

std::string info_hash_to_string(lt::info_hash_t const& info_hash)
{
    if (!info_hash.has_v1() && !info_hash.has_v2()) return {};

    std::ostringstream out;
    if (info_hash.has_v1() && !info_hash.has_v2())
    {
        out << info_hash.v1;
    }
    else
    {
        out << info_hash;
    }
    return out.str();
}

std::string peer_id_to_hex(lt::peer_id const& pid)
{
    if (pid.is_all_zeros()) return "unknown";

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t const byte : pid)
    {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::string peer_id_prefix(lt::peer_id const& pid)
{
    if (pid.is_all_zeros()) return {};

    std::string const hex = peer_id_to_hex(pid);
    if (hex == "unknown") return {};
    return hex.substr(0, (std::min)(std::size_t{16}, hex.size()));
}

std::string endpoint_to_string(lt::tcp::endpoint const& endpoint)
{
    std::ostringstream out;
    out << endpoint.address().to_string() << ":" << endpoint.port();
    return out.str();
}

std::string socket_type_label(lt::socket_type_t socket_type)
{
    switch (socket_type)
    {
        case lt::socket_type_t::tcp:
            return "tcp";
        case lt::socket_type_t::socks5:
            return "socks5";
        case lt::socket_type_t::http:
            return "http";
        case lt::socket_type_t::utp:
            return "utp";
        case lt::socket_type_t::i2p:
            return "i2p";
        case lt::socket_type_t::tcp_ssl:
            return "tcp_ssl";
        case lt::socket_type_t::socks5_ssl:
            return "socks5_ssl";
        case lt::socket_type_t::http_ssl:
            return "http_ssl";
        case lt::socket_type_t::utp_ssl:
            return "utp_ssl";
    }

    return "unknown";
}

std::string transport_from_socket_type(lt::socket_type_t socket_type)
{
    switch (socket_type)
    {
        case lt::socket_type_t::utp:
        case lt::socket_type_t::utp_ssl:
            return "utp";

        case lt::socket_type_t::tcp:
        case lt::socket_type_t::tcp_ssl:
        case lt::socket_type_t::socks5:
        case lt::socket_type_t::socks5_ssl:
        case lt::socket_type_t::http:
        case lt::socket_type_t::http_ssl:
            return "tcp";

        default:
            return "unknown";
    }
}

std::string transport_from_peer_info(lt::peer_info const& peer)
{
    if (has_flag(peer.flags, lt::peer_info::utp_socket)) return "utp";
    if (has_flag(peer.flags, lt::peer_info::i2p_socket)) return "unknown";
    return "tcp";
}

std::vector<std::string> peer_flag_labels(lt::peer_info const& peer)
{
    std::vector<std::string> labels;

    if (has_flag(peer.flags, lt::peer_info::interesting)) labels.emplace_back("interesting");
    if (has_flag(peer.flags, lt::peer_info::choked)) labels.emplace_back("we_choked_peer");
    if (has_flag(peer.flags, lt::peer_info::remote_interested)) labels.emplace_back("peer_interested");
    if (has_flag(peer.flags, lt::peer_info::remote_choked)) labels.emplace_back("peer_choked_us");
    if (has_flag(peer.flags, lt::peer_info::supports_extensions)) labels.emplace_back("supports_extensions");
    if (has_flag(peer.flags, lt::peer_info::outgoing_connection)) labels.emplace_back("outgoing_connection");
    if (has_flag(peer.flags, lt::peer_info::handshake)) labels.emplace_back("handshake");
    if (has_flag(peer.flags, lt::peer_info::connecting)) labels.emplace_back("connecting");
    if (has_flag(peer.flags, lt::peer_info::on_parole)) labels.emplace_back("on_parole");
    if (has_flag(peer.flags, lt::peer_info::seed)) labels.emplace_back("seed");
    if (has_flag(peer.flags, lt::peer_info::optimistic_unchoke)) labels.emplace_back("optimistic_unchoke");
    if (has_flag(peer.flags, lt::peer_info::snubbed)) labels.emplace_back("snubbed");
    if (has_flag(peer.flags, lt::peer_info::upload_only)) labels.emplace_back("upload_only");
    if (has_flag(peer.flags, lt::peer_info::endgame_mode)) labels.emplace_back("endgame_mode");
    if (has_flag(peer.flags, lt::peer_info::holepunched)) labels.emplace_back("holepunched");
    if (has_flag(peer.flags, lt::peer_info::i2p_socket)) labels.emplace_back("i2p_socket");
    if (has_flag(peer.flags, lt::peer_info::utp_socket)) labels.emplace_back("utp_socket");
    if (has_flag(peer.flags, lt::peer_info::ssl_socket)) labels.emplace_back("ssl_socket");
    if (has_flag(peer.flags, lt::peer_info::rc4_encrypted)) labels.emplace_back("rc4_encrypted");
    if (has_flag(peer.flags, lt::peer_info::plaintext_encrypted)) labels.emplace_back("plaintext_encrypted");

    return labels;
}

std::vector<std::string> peer_source_labels(lt::peer_info const& peer)
{
    std::vector<std::string> labels;

    if (has_flag(peer.source, lt::peer_info::tracker)) labels.emplace_back("tracker");
    if (has_flag(peer.source, lt::peer_info::dht)) labels.emplace_back("dht");
    if (has_flag(peer.source, lt::peer_info::pex)) labels.emplace_back("pex");
    if (has_flag(peer.source, lt::peer_info::lsd)) labels.emplace_back("lsd");
    if (has_flag(peer.source, lt::peer_info::resume_data)) labels.emplace_back("resume_data");
    if (has_flag(peer.source, lt::peer_info::incoming)) labels.emplace_back("incoming");

    return labels;
}

}  // namespace btclient
