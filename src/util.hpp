#pragma once

#include <libtorrent/info_hash.hpp>
#include <libtorrent/peer_id.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/socket_type.hpp>
#include <libtorrent/torrent_status.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace btclient {

struct TimestampPair {
    std::string utc_ts;
    std::int64_t mono_ns = 0;
};

// Small JSON writer used for artifacts. It keeps this tool self-contained and
// covers only the value types we need in the event log and summary files.
class JsonValue {
public:
    enum class Kind {
        null_value,
        bool_value,
        integer_value,
        double_value,
        string_value,
        object_value,
        array_value
    };

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool value);
    JsonValue(int value);
    JsonValue(std::int64_t value);
    JsonValue(double value);
    JsonValue(char const* value);
    JsonValue(std::string value);

    static JsonValue object();
    static JsonValue array();

    JsonValue& add(std::string key, JsonValue value);
    JsonValue& push(JsonValue value);

    std::string to_string() const;
    Kind kind() const;

private:
    static std::string escape(std::string const& input);
    std::string serialize() const;

    Kind kind_ = Kind::null_value;
    bool bool_value_ = false;
    std::int64_t integer_value_ = 0;
    double double_value_ = 0.0;
    std::string string_value_;
    std::vector<std::pair<std::string, JsonValue>> object_values_;
    std::vector<JsonValue> array_values_;
};

struct DistributionSummary {
    std::size_t count = 0;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> mean;
    std::optional<double> stddev;
    std::optional<double> p50;
    std::optional<double> p95;
};

// Libtorrent exposes many state fields as bit flags. This keeps those checks
// readable at the call site.
template <typename Flags>
bool has_flag(Flags value, Flags flag)
{
    return static_cast<bool>(value & flag);
}

TimestampPair capture_timestamp();
std::string format_utc(std::chrono::system_clock::time_point tp);
std::string compact_utc_stamp(std::chrono::system_clock::time_point tp);
std::int64_t monotonic_now_ns();
double ns_to_ms(std::int64_t ns);

DistributionSummary summarize_samples(std::vector<double> const& samples);
JsonValue distribution_summary_to_json(DistributionSummary const& summary);
JsonValue strings_to_json_array(std::vector<std::string> const& values);

std::string generate_id(std::string const& prefix);
std::string get_hostname();
bool is_magnet_uri(std::string const& input);
std::string state_name(libtorrent::torrent_status::state_t state);
std::string info_hash_to_string(libtorrent::info_hash_t const& info_hash);
std::string peer_id_to_hex(libtorrent::peer_id const& pid);
std::string peer_id_prefix(libtorrent::peer_id const& pid);
std::string endpoint_to_string(libtorrent::tcp::endpoint const& endpoint);
std::string socket_type_label(libtorrent::socket_type_t socket_type);
std::string transport_from_socket_type(libtorrent::socket_type_t socket_type);
std::string transport_from_peer_info(libtorrent::peer_info const& peer);
std::vector<std::string> peer_flag_labels(libtorrent::peer_info const& peer);
std::vector<std::string> peer_source_labels(libtorrent::peer_info const& peer);

}  // namespace btclient
