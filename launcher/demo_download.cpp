#include "demo_download.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <windows.h>
#include <xlog/xlog.h>
#include <nlohmann/json.hpp>
#include <common/version/version.h>

// The download URL comes from the response.
static const char demo_resolve_base_url[] = "https://autodl.factionfiles.com/aflauncher/v1/demo.php?id=";

static constexpr size_t max_resolve_response_bytes = 256 * 1024;

DemoDownloader::DemoDownloader() : session_(AF_USER_AGENT_SUFFIX("DemoDownload"))
{
    session_.set_connect_timeout(3000);
    session_.set_receive_timeout(5000);
}

bool DemoDownloader::is_valid_game_id(const std::string& game_id)
{
    if (game_id.size() != 16)
        return false;
    for (char c : game_id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok)
            return false;
    }
    return true;
}

DemoDownloader::ResolveStatus DemoDownloader::resolve(const std::string& game_id, ResolveResult& out)
{
    if (!is_valid_game_id(game_id))
        return ResolveStatus::invalid_id;

    const std::string url = std::string(demo_resolve_base_url) + game_id;
    xlog::info("Resolving demo: {}", url);

    std::string body;
    int status = 0;
    try {
        HttpRequest req{url, "GET", session_};
        status = req.send_no_check();
        std::stringstream ss;
        char buf[4096];
        size_t received = 0;
        while (true) {
            size_t n = req.read(buf, sizeof(buf));
            if (n == 0)
                break;
            received += n;
            if (received > max_resolve_response_bytes)
                return ResolveStatus::server_error;
            ss.write(buf, static_cast<std::streamsize>(n));
        }
        body = ss.str();
    }
    catch (const std::exception& e) {
        xlog::error("Demo resolve network error: {}", e.what());
        return ResolveStatus::network_error;
    }

    if (status == 400)
        return ResolveStatus::invalid_id;
    if (status == 404)
        return ResolveStatus::not_found;
    if (status == 503)
        return ResolveStatus::disabled;
    if (status != 200)
        return ResolveStatus::server_error;

    try {
        auto j = nlohmann::json::parse(body);
        if (!j.value("found", false))
            return ResolveStatus::not_found;
        if (!j.contains("demo") || !j["demo"].is_object())
            return ResolveStatus::server_error;

        const auto& d = j["demo"];
        out.download_url = d.value("download_url", std::string{});
        out.bytes = d.value("bytes", uint64_t{0});
        out.format = d.value("format", std::string{});
        out.uploaded_at = d.value("uploaded_at", int64_t{0});
        if (out.download_url.empty())
            return ResolveStatus::server_error;

        // The game block is display-only and may be null; a null is not an error.
        if (j.contains("game") && j["game"].is_object()) {
            const auto& g = j["game"];
            GameInfo gi;
            gi.level_file = g.value("level_file", std::string{});
            gi.level_name = g.value("level_name", std::string{});
            gi.gametype = g.value("gametype", -1);
            gi.gametype_name = g.value("gametype_name", std::string{});
            gi.tc_mod = g.value("tc_mod", std::string{});
            gi.server_name = g.value("server_name", std::string{});
            gi.started_at = g.value("started_at", int64_t{0});
            gi.duration_ms = g.value("duration_ms", int64_t{0});
            gi.site_url = g.value("site_url", std::string{});
            out.game = std::move(gi);
        }
        return ResolveStatus::ok;
    }
    catch (const std::exception& e) {
        xlog::error("Demo resolve parse error: {}", e.what());
        return ResolveStatus::network_error;
    }
}

DemoDownloader::DownloadStatus DemoDownloader::download(
    const std::string& download_url, const std::string& dest_path, uint64_t max_bytes, uint64_t expected_bytes,
    const std::function<bool(uint64_t)>& progress)
{
    // HTTPS only; a non-https URL would trip the http-parser's assert (a no-op in release).
    if (download_url.rfind("https://", 0) != 0) {
        xlog::error("Demo download URL is not https");
        return DownloadStatus::network_error;
    }
    if (expected_bytes > 0 && expected_bytes > max_bytes)
        return DownloadStatus::too_large;

    const std::string part_path = dest_path + ".part";
    uint64_t total = 0;
    try {
        HttpRequest req{download_url, "GET", session_};
        const int status = req.send_no_check();
        if (status == 403)
            return DownloadStatus::link_expired; // signed URL expired/rotated -> caller re-resolves once
        if (status == 404)
            return DownloadStatus::not_found;
        if (status != 200)
            return DownloadStatus::network_error;

        // We never send Accept-Encoding and the response carries no Content-Encoding, so
        // WinINet hands us the gzip .afd byte-for-byte. Write it verbatim - do NOT decompress.
        std::ofstream out(part_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return DownloadStatus::io_error;

        char buf[16384];
        while (true) {
            const size_t n = req.read(buf, sizeof(buf));
            if (n == 0)
                break;
            total += n;
            if (total > max_bytes) {
                out.close();
                std::remove(part_path.c_str());
                return DownloadStatus::too_large;
            }
            out.write(buf, static_cast<std::streamsize>(n));
            if (!out) {
                out.close();
                std::remove(part_path.c_str());
                return DownloadStatus::io_error;
            }
            if (progress && !progress(total)) {
                out.close();
                std::remove(part_path.c_str());
                return DownloadStatus::canceled;
            }
        }
        out.flush();
        out.close();
        if (!out) {
            std::remove(part_path.c_str());
            return DownloadStatus::io_error;
        }
    }
    catch (const std::exception& e) {
        xlog::error("Demo download network error: {}", e.what());
        std::remove(part_path.c_str());
        return DownloadStatus::network_error;
    }

    if (expected_bytes > 0 && total != expected_bytes) {
        xlog::warn("Demo download size mismatch: got {}, expected {}", total, expected_bytes);
        std::remove(part_path.c_str());
        return DownloadStatus::size_mismatch;
    }

    // Move the completed file into place (replacing any stale copy).
    if (!MoveFileExA(part_path.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        xlog::error("Failed to move downloaded demo into place, error {}", GetLastError());
        std::remove(part_path.c_str());
        return DownloadStatus::io_error;
    }
    return DownloadStatus::ok;
}
