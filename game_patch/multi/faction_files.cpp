#include <string>
#include <optional>
#include <fstream>
#include <sstream>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <json.hpp>
#include <xlog/xlog.h>
#include <common/version/version.h>
#include <common/utils/string-utils.h>
#include "../os/console.h"
#include "../rf/multi.h"
#include "faction_files.h"

static const char level_download_agent_name_client[] = AF_USER_AGENT_SUFFIX("Autodl");
static const char level_download_agent_name_dedi[] = AF_USER_AGENT_SUFFIX("Dedi Autodl");
static const char level_download_base_url[] = "https://autodl.factionfiles.com";

FactionFilesClient::FactionFilesClient() :
    session_{rf::is_dedicated_server ? level_download_agent_name_dedi : level_download_agent_name_client}
{
    session_.set_connect_timeout(2000);
    session_.set_receive_timeout(3000);
}

std::optional<FactionFilesClient::LevelInfo> FactionFilesClient::parse_level_info(const std::string& response)
{
    auto j = nlohmann::json::parse(response);

    if (!j.value("found", false)) {
        return {};
    }

    if (!j.contains("file") || j["file"].is_null()) {
        xlog::warn("FactionFiles response has found=true but no file object");
        return {};
    }

    auto& file = j.at("file");
    LevelInfo info;
    info.name = file.at("title").get<std::string>();
    info.author = file.at("author").get<std::string>();
    info.description = file.at("description").get<std::string>();
    info.size_in_bytes = file.at("download_size").get<unsigned>();
    info.download_url = file.at("download_url").get<std::string>();

    if (!info.size_in_bytes) {
        throw std::runtime_error("invalid file size");
    }

    if (info.download_url.empty()) {
        throw std::runtime_error("empty download url");
    }

    info.image_url = file.value("image_url", "");
    xlog::debug("Parsed level info: '{}' by '{}', {} bytes", info.name, info.author, info.size_in_bytes);
    xlog::debug("  description: {}", info.description);
    xlog::debug("  download_url: {}", info.download_url);
    xlog::debug("  image_url: {}", info.image_url);

    return {info};
}

std::optional<FactionFilesClient::LevelInfo> FactionFilesClient::find_map(const char* file_name)
{
    auto url = std::format("{}/v3/find.php?rfl={}", level_download_base_url, encode_uri_component(file_name));

    xlog::trace("Fetching level info: {}", file_name);
    HttpRequest req{url, "GET", session_};
    req.send();

    std::ostringstream response_stream;
    char buf[4096];
    while (size_t num_bytes_read = req.read(buf, sizeof(buf))) {
        response_stream.write(buf, num_bytes_read);
    }

    auto response = response_stream.str();
    if (response.empty()) {
        throw std::runtime_error("empty response");
    }

    xlog::debug("FactionFiles response: {}", response);

    return parse_level_info(response);
}

std::vector<bool> FactionFilesClient::check_maps(const std::vector<std::string>& file_names)
{
    std::string body;
    for (size_t i = 0; i < file_names.size(); ++i) {
        if (i > 0) {
            body += ';';
        }
        body += file_names[i];
    }

    auto url = std::format("{}/checkmaps.php", level_download_base_url);

    xlog::trace("Checking map availability for {} entries", file_names.size());
    HttpRequest req{url, "POST", session_};
    req.set_content_type("text/plain");
    req.send(body);

    std::ostringstream response_stream;
    char buf[1024];
    while (size_t num_bytes_read = req.read(buf, sizeof(buf))) {
        response_stream.write(buf, num_bytes_read);
    }

    auto response = response_stream.str();
    std::vector<bool> results;
    results.reserve(file_names.size());

    std::istringstream response_reader(response);
    std::string line;
    while (std::getline(response_reader, line)) {
        std::string_view trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        results.push_back(trimmed == "found");
    }

    if (results.size() != file_names.size()) {
        xlog::warn("FactionFiles checkmaps count mismatch: sent {}, received {}", file_names.size(), results.size());
        results.resize(file_names.size(), false);
    }

    return results;
}

void FactionFilesClient::download_map(const char* tmp_filename, const std::string& download_url,
    std::function<bool(unsigned bytes_received, std::chrono::milliseconds duration)> callback)
{
    xlog::info("Downloading map from: {}", download_url);
    HttpRequest req{download_url, "GET", session_};
    req.send();

    std::ofstream tmp_file(tmp_filename, std::ios_base::out | std::ios_base::binary);
    if (!tmp_file) {
        xlog::error("Cannot open file: {}", tmp_filename);
        throw std::runtime_error("cannot open file");
    }

    // Note: we are connected here - don't include it in duration so speed calculation can be more precise
    tmp_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    auto download_start = std::chrono::steady_clock::now();
    unsigned total_bytes_read = 0;

    char buf[4096];
    while (true) {
        auto num_bytes_read = req.read(buf, sizeof(buf));
        if (num_bytes_read <= 0)
            break;

        tmp_file.write(buf, num_bytes_read);

        total_bytes_read += num_bytes_read;
        auto time_diff = std::chrono::steady_clock::now() - download_start;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time_diff);

        if (callback && !callback(total_bytes_read, duration)) {
            xlog::debug("Download aborted");
            throw std::runtime_error("download aborted");
        }
    }
}

std::vector<unsigned char> FactionFilesClient::fetch_image(const std::string& image_url)
{
    static constexpr size_t max_image_size = 8 * 1024 * 1024; // 8 MB

    xlog::info("Fetching map image from: {}", image_url);
    HttpRequest req{image_url, "GET", session_};
    req.send();

    std::vector<unsigned char> data;
    char buf[4096];
    while (size_t num_bytes_read = req.read(buf, sizeof(buf))) {
        data.insert(data.end(), buf, buf + num_bytes_read);
        if (data.size() > max_image_size) {
            xlog::warn("Image download exceeded max size ({} bytes), aborting", max_image_size);
            return {};
        }
    }

    xlog::info("Fetched map image: {} bytes", data.size());
    return data;
}

ConsoleCommand2 fflink_status_cmd{
    "fflink_status",
    []() {
        g_game_config.load(); // reload config in case account was linked since game launch
        std::string username = g_game_config.fflink_username.value();
        if (username.empty()) {
            rf::console::print("This client is not linked to a FactionFiles account! "
                "Visit alpinefaction.com/link to learn more and link your account.");
        }
        else {
            rf::console::printf("This client is linked to FactionFiles as %s", username.c_str());
        }
    },
    "Check status of your client's link with FactionFiles.com",
    "fflink_status",
};

void faction_files_do_patch()
{
    fflink_status_cmd.register_cmd();
}
