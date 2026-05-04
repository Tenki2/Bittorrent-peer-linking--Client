#include "artifact_sender.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace btclient {

namespace {

bool starts_with(std::string const& value, std::string const& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

void validate_destination_url(std::string const& url)
{
    if (url.empty())
    {
        throw std::invalid_argument("destination URL is empty");
    }
    if (!starts_with(url, "http://") && !starts_with(url, "https://"))
    {
        throw std::invalid_argument("destination URL must start with http:// or https://");
    }
}

void ensure_upload_file_exists(std::filesystem::path const& path, std::string const& label)
{
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error(
            "cannot upload " + label + "; file does not exist: " + path.string());
    }
}

void check_curl(CURLcode code, std::string const& action)
{
    if (code != CURLE_OK)
    {
        throw std::runtime_error(action + ": " + curl_easy_strerror(code));
    }
}

struct CurlGlobal {
    CurlGlobal()
    {
        check_curl(curl_global_init(CURL_GLOBAL_DEFAULT), "failed to initialize libcurl");
    }

    ~CurlGlobal()
    {
        curl_global_cleanup();
    }
};

struct CurlHandle {
    CurlHandle()
        : handle(curl_easy_init())
    {
        if (handle == nullptr)
        {
            throw std::runtime_error("failed to create libcurl easy handle");
        }
    }

    ~CurlHandle()
    {
        curl_easy_cleanup(handle);
    }

    CURL* handle = nullptr;
};

struct CurlMime {
    explicit CurlMime(CURL* curl)
        : form(curl_mime_init(curl))
    {
        if (form == nullptr)
        {
            throw std::runtime_error("failed to create libcurl MIME form");
        }
    }

    ~CurlMime()
    {
        curl_mime_free(form);
    }

    curl_mime* form = nullptr;
};

curl_mimepart* add_part(curl_mime* form)
{
    curl_mimepart* part = curl_mime_addpart(form);
    if (part == nullptr)
    {
        throw std::runtime_error("failed to add MIME upload part");
    }
    return part;
}

void add_text_part(curl_mime* form, char const* name, std::string const& value)
{
    curl_mimepart* part = add_part(form);
    check_curl(curl_mime_name(part, name), "failed to name MIME text part");
    check_curl(
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED),
        "failed to set MIME text part data");
}

void add_file_part(
    curl_mime* form,
    char const* field_name,
    std::filesystem::path const& path,
    char const* content_type)
{
    curl_mimepart* part = add_part(form);
    std::string const path_string = path.string();
    std::string const filename = path.filename().string();

    check_curl(curl_mime_name(part, field_name), "failed to name MIME file part");
    check_curl(curl_mime_filedata(part, path_string.c_str()), "failed to set MIME file data");
    check_curl(curl_mime_filename(part, filename.c_str()), "failed to set MIME filename");
    check_curl(curl_mime_type(part, content_type), "failed to set MIME content type");
}

void add_artifact_form_fields(
    curl_mime* form,
    RuntimeConfig const& config,
    OutputPaths const& output_paths)
{
    add_text_part(form, "run_id", config.run_id);
    add_text_part(form, "session_id", config.session_id);
    add_text_part(form, "node_role", config.node_role);
    add_text_part(form, "client_version", kClientVersion);
    add_text_part(form, "state_json_path", output_paths.state_json_path.string());
    add_text_part(form, "event_log_path", output_paths.event_log_path.string());
    add_text_part(form, "summary_json_path", output_paths.summary_json_path.string());
}

void add_artifact_files(curl_mime* form, OutputPaths const& output_paths)
{
    ensure_upload_file_exists(output_paths.state_json_path, "state JSON");
    ensure_upload_file_exists(output_paths.event_log_path, "event log");
    ensure_upload_file_exists(output_paths.summary_json_path, "summary JSON");

    add_file_part(
        form,
        "state_json",
        output_paths.state_json_path,
        "application/json");
    add_file_part(
        form,
        "event_log",
        output_paths.event_log_path,
        "application/x-ndjson");
    add_file_part(
        form,
        "summary_json",
        output_paths.summary_json_path,
        "application/json");
}

std::string trim_http_line(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    {
        line.pop_back();
    }
    return line;
}

std::size_t write_response_body(char* data, std::size_t size, std::size_t count, void* user_data)
{
    auto* body = static_cast<std::string*>(user_data);
    body->append(data, size * count);
    return size * count;
}

std::size_t capture_status_line(char* data, std::size_t size, std::size_t count, void* user_data)
{
    std::size_t const byte_count = size * count;
    std::string line(data, byte_count);
    if (starts_with(line, "HTTP/"))
    {
        *static_cast<std::string*>(user_data) = trim_http_line(std::move(line));
    }
    return byte_count;
}

ArtifactSendResult perform_upload(
    RuntimeConfig const& config,
    OutputPaths const& output_paths)
{
    CurlGlobal curl_global;
    CurlHandle curl;
    CurlMime form(curl.handle);

    add_artifact_form_fields(form.form, config, output_paths);
    add_artifact_files(form.form, output_paths);

    std::string response_body;
    std::string status_line;

    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_URL, config.destination_url.c_str()),
        "failed to set destination URL");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_USERAGENT, kClientVersion),
        "failed to set upload user agent");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_MIMEPOST, form.form),
        "failed to attach MIME upload form");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_response_body),
        "failed to set response body callback");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &response_body),
        "failed to set response body storage");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, capture_status_line),
        "failed to set response header callback");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &status_line),
        "failed to set response header storage");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L),
        "failed to set upload connect timeout");
    check_curl(
        curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS, 60000L),
        "failed to set upload timeout");

    CURLcode const perform_result = curl_easy_perform(curl.handle);
    if (perform_result != CURLE_OK)
    {
        throw std::runtime_error(
            "artifact upload failed: " + std::string(curl_easy_strerror(perform_result)));
    }

    long response_code = 0;
    check_curl(
        curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &response_code),
        "failed to read upload response code");

    ArtifactSendResult result;
    result.status_code = static_cast<int>(response_code);
    result.status_text = status_line.empty()
        ? ("HTTP status " + std::to_string(response_code))
        : status_line;
    result.response_body = std::move(response_body);

    if (response_code < 200 || response_code >= 300)
    {
        throw std::runtime_error(
            "destination rejected artifacts with status: " + result.status_text);
    }

    return result;
}

}  // namespace

ArtifactSendResult send_artifacts_to_destination(
    RuntimeConfig const& config,
    OutputPaths const& output_paths)
{
    validate_destination_url(config.destination_url);
    return perform_upload(config, output_paths);
}

}  // namespace btclient
