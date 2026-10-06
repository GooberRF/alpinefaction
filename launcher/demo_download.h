#pragma once

#include <string>
#include <optional>
#include <functional>
#include <cstdint>
#include <common/HttpRequest.h>

// Resolves a game id to a short-lived download URL via FactionFiles, then streams
// the .afd verbatim (it is a gzip stream and must NOT be decompressed).
class DemoDownloader
{
public:
    // The optional "game" block from the resolve response - display only, may be absent.
    struct GameInfo
    {
        std::string level_file;
        std::string level_name;
        int gametype = -1;
        std::string gametype_name;
        std::string tc_mod;
        std::string server_name;
        int64_t started_at = 0;
        int64_t duration_ms = 0;
        std::string site_url;
    };

    struct ResolveResult
    {
        std::string download_url;
        uint64_t bytes = 0;
        std::string format;
        int64_t uploaded_at = 0;
        std::optional<GameInfo> game; // null is not an error - see the contract
    };

    enum class ResolveStatus
    {
        ok,
        invalid_id,    // malformed game id / 400
        not_found,     // 404 - never existed, expired, or unknown id (indistinguishable)
        disabled,      // 503 demos_disabled
        server_error,  // 500 / unexpected status
        network_error, // connection failed or unparseable response
    };

    enum class DownloadStatus
    {
        ok,
        link_expired,  // 403 - caller should re-resolve once and retry
        not_found,     // 404 - gone since resolve
        too_large,     // exceeded the size ceiling
        size_mismatch, // bytes received != Content-Length/expected
        canceled,      // progress callback asked to stop
        network_error,
        io_error,
    };

    DemoDownloader();

    // The contract: exactly 16 lowercase hex characters.
    static bool is_valid_game_id(const std::string& game_id);

    ResolveStatus resolve(const std::string& game_id, ResolveResult& out);

    // Streams the response body verbatim (no decompression) to a "<dest>.part" temp,
    // then renames it onto dest_path. Enforces max_bytes and, when expected_bytes > 0,
    // that the received length matches. progress(received_bytes) returns false to cancel.
    DownloadStatus download(const std::string& download_url, const std::string& dest_path,
                            uint64_t max_bytes, uint64_t expected_bytes,
                            const std::function<bool(uint64_t received_bytes)>& progress);

private:
    HttpSession session_;
};
