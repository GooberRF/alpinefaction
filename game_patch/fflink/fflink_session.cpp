#include "fflink_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <json.hpp>
#include <xlog/xlog.h>

#include <common/HttpRequest.h>
#include <common/version/version.h>

#include "../multi/server_internal.h"
#include "../os/console.h"
#include "fflink_utils.h"

namespace fflink {

namespace {

constexpr const char* k_session_url = "https://link.factionfiles.com/afstats/v1/session.php";
constexpr const char* k_user_agent = AF_USER_AGENT_SUFFIX("AFStats");

// Connect and receive timeouts in ms
constexpr unsigned long k_connect_timeout_ms = 3000;
constexpr unsigned long k_receive_timeout_ms = 3000;

// Backoff schedule for transient failures (seconds). After the last entry the schedule repeats.
constexpr int k_backoff_schedule_s[] = {5, 30, 120};

std::mutex g_state_mutex;
SessionState g_state;
std::string g_gssk; // protected by g_state_mutex
std::atomic<bool> g_exchange_in_flight{false};

bool is_lower_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool is_valid_gsk_format(std::string_view gsk)
{
    if (gsk.size() != 32) {
        return false;
    }
    for (char c : gsk) {
        if (!is_lower_hex_char(c)) {
            return false;
        }
    }
    return true;
}

bool is_valid_gssk_format(std::string_view gssk)
{
    if (gssk.size() != 32) {
        return false;
    }
    for (char c : gssk) {
        const bool ok =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

void set_state(SessionStatus status, int server_id, std::string_view error_msg, std::string_view gssk)
{
    std::lock_guard lock(g_state_mutex);
    g_state.status = status;
    g_state.server_id = server_id;
    g_state.last_error.assign(error_msg);
    g_gssk.assign(gssk);
}

struct ExchangeOutcome
{
    enum class Kind {
        success,
        client_config_error, // 4xx: don't retry until operator changes config
        transient_error,     // 5xx, network, parse: retry with backoff
    };
    Kind kind = Kind::transient_error;
    int server_id = 0;
    std::string gssk;
    std::string error_detail; // safe-to-display message
};

ExchangeOutcome do_one_exchange(const std::string& gsk)
{
    ExchangeOutcome out;

    const std::string body = nlohmann::json{{"gsk", gsk}}.dump();

    // Redacted copy for logging — the GSK must never be written to disk.
    const std::string body_for_log =
        nlohmann::json{{"gsk", std::string(gsk.size(), '*')}}.dump();
    xlog::info("[fflink] >>> POST {}", k_session_url);
    xlog::info("[fflink] >>> User-Agent: {}", k_user_agent);
    xlog::info("[fflink] >>> Content-Type: application/json");
    xlog::info("[fflink] >>> Content-Length: {}", body.size());
    xlog::info("[fflink] >>> body ({} bytes): {}", body.size(), body_for_log);

    std::string response;
    int status_code = 0;

    try {
        HttpSession session(k_user_agent);
        session.set_connect_timeout(k_connect_timeout_ms);
        session.set_receive_timeout(k_receive_timeout_ms);

        HttpRequest req(k_session_url, "POST", session);
        req.set_content_type("application/json");
        status_code = req.send_no_check(body);

        char buf[1024];
        std::ostringstream stream;
        while (size_t n = req.read(buf, sizeof(buf))) {
            stream.write(buf, n);
        }
        response = stream.str();
    }
    catch (const std::exception& e) {
        out.kind = ExchangeOutcome::Kind::transient_error;
        out.error_detail = std::string{"network error: "} + e.what();
        return out;
    }

    // Always log status code and response prefix so misroutes / proxy interception are diagnosable.
    constexpr size_t k_log_response_prefix = 256;
    const std::string response_preview =
        response.size() <= k_log_response_prefix
            ? response
            : response.substr(0, k_log_response_prefix) + "...[truncated]";
    xlog::info("[fflink] HTTP {} from {}; response: {}", status_code, k_session_url, response_preview);

    if (status_code == 200) {
        // Detect HTML / non-JSON bodies before trying to parse, so the operator-facing
        // message points at the server rather than at our JSON parser.
        std::string_view trimmed = response;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
                                    trimmed.front() == '\r' || trimmed.front() == '\n')) {
            trimmed.remove_prefix(1);
        }
        if (trimmed.empty() || trimmed.front() != '{') {
            out.kind = ExchangeOutcome::Kind::transient_error;
            out.error_detail = "server returned non-JSON response (likely upstream/DB error)";
            return out;
        }

        try {
            auto j = nlohmann::json::parse(response);
            const auto gssk = j.at("gssk").get<std::string>();
            const auto server_id = j.at("server_id").get<int>();
            if (!is_valid_gssk_format(gssk)) {
                throw std::runtime_error("gssk has invalid format");
            }
            out.kind = ExchangeOutcome::Kind::success;
            out.gssk = std::move(gssk);
            out.server_id = server_id;
            return out;
        }
        catch (const std::exception& e) {
            out.kind = ExchangeOutcome::Kind::transient_error;
            out.error_detail = std::string{"malformed JSON response: "} + e.what();
            return out;
        }
    }

    // Non-200: try to extract `error` field for diagnostics.
    std::string error_code = "<no error code>";
    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("error") && j["error"].is_string()) {
            error_code = j["error"].get<std::string>();
        }
    }
    catch (const std::exception&) {
        // Body wasn't JSON; leave error_code as default.
    }

    if (status_code >= 400 && status_code < 500) {
        // 401 (unknown_or_disabled_gsk), 400 (invalid format), 405 (wrong method) — operator must fix config / we have a bug.
        out.kind = ExchangeOutcome::Kind::client_config_error;
        out.error_detail = std::format("HTTP {} ({})", status_code, error_code);
        return out;
    }

    // 5xx and anything else
    out.kind = ExchangeOutcome::Kind::transient_error;
    out.error_detail = std::format("HTTP {} ({})", status_code, error_code);
    return out;
}

void exchange_worker(std::string gsk)
{
    xlog::info("[fflink] starting session-key exchange with FactionFiles");

    int attempt = 0;
    while (true) {
        ExchangeOutcome outcome = do_one_exchange(gsk);

        if (outcome.kind == ExchangeOutcome::Kind::success) {
            xlog::info("[fflink] session established (server_id={})", outcome.server_id);
            set_state(SessionStatus::valid, outcome.server_id, "", outcome.gssk);
            enqueue_console_line(std::format(
                "FactionFiles stats: session established (server id {}).", outcome.server_id));
            g_exchange_in_flight.store(false, std::memory_order_release);
            return;
        }

        if (outcome.kind == ExchangeOutcome::Kind::client_config_error) {
            xlog::error(
                "[fflink] session exchange rejected: {}. Check that the GSK in your server config matches "
                "the one shown on factionfiles.com for this server.",
                outcome.error_detail);
            set_state(SessionStatus::rejected_by_server, 0, outcome.error_detail, "");
            enqueue_console_line(std::format(
                "FactionFiles stats: GSK was rejected by FactionFiles ({}). "
                "Stats reporting disabled until the GSK in your server config is corrected.",
                outcome.error_detail));
            g_exchange_in_flight.store(false, std::memory_order_release);
            return;
        }

        // Transient: backoff and retry.
        const int delay_s = k_backoff_schedule_s[std::min<size_t>(attempt, std::size(k_backoff_schedule_s) - 1)];
        xlog::warn("[fflink] session exchange failed ({}); retrying in {}s", outcome.error_detail, delay_s);
        set_state(SessionStatus::failed, 0, outcome.error_detail, "");
        // Only print the very first transient failure to console, to avoid spamming the operator while we backoff.
        if (attempt == 0) {
            enqueue_console_line(std::format(
                "FactionFiles stats: session exchange failed ({}). Will retry in background.",
                outcome.error_detail));
        }

        std::this_thread::sleep_for(std::chrono::seconds(delay_s));
        ++attempt;
    }
}

} // namespace

void start_session_exchange()
{
    const auto& cfg = g_alpine_server_config;
    if (cfg.fflink_gsk.empty()) {
        // Optional config item; staying silent here to keep startup clean.
        return;
    }

    if (!is_valid_gsk_format(cfg.fflink_gsk)) {
        xlog::error(
            "[fflink] configured fflink_gsk is malformed (must be exactly 32 lowercase hex chars). "
            "FactionFiles stats reporting will be disabled.");
        rf::console::print(
            "FactionFiles stats: GSK in server config is malformed (must be exactly 32 lowercase hex chars). "
            "Stats reporting disabled.");
        set_state(SessionStatus::bad_gsk_format, 0, "malformed GSK in server config", "");
        return;
    }

    bool expected = false;
    if (!g_exchange_in_flight.compare_exchange_strong(expected, true)) {
        xlog::debug("[fflink] session exchange already in flight; skipping new request");
        return;
    }

    rf::console::print("FactionFiles stats: requesting session key from FactionFiles...");
    set_state(SessionStatus::pending, 0, "", "");

    std::thread(exchange_worker, cfg.fflink_gsk).detach();
}

SessionState snapshot_state()
{
    std::lock_guard lock(g_state_mutex);
    return g_state;
}

std::string get_gssk()
{
    std::lock_guard lock(g_state_mutex);
    if (g_state.status != SessionStatus::valid) {
        return {};
    }
    return g_gssk;
}

namespace {

const char* status_to_str(SessionStatus s)
{
    switch (s) {
        case SessionStatus::none:               return "not configured / not yet attempted";
        case SessionStatus::pending:            return "exchange in progress";
        case SessionStatus::valid:              return "session valid";
        case SessionStatus::bad_gsk_format:     return "GSK in config is malformed";
        case SessionStatus::rejected_by_server: return "rejected by FactionFiles";
        case SessionStatus::failed:             return "exchange failed (will retry)";
    }
    return "unknown";
}

ConsoleCommand2 sv_fflink_status_cmd{
    "sv_fflink_status",
    []() {
        const auto state = snapshot_state();
        rf::console::print("FactionFiles stats link: {}", status_to_str(state.status));
        if (state.status == SessionStatus::valid) {
            rf::console::print("  Server id: {}", state.server_id);
        }
        if (!state.last_error.empty()) {
            rf::console::print("  Last error: {}", state.last_error);
        }
    },
    "Show the current FactionFiles stats session-link status.",
};

ConsoleCommand2 sv_fflink_resync_cmd{
    "sv_fflink_resync",
    []() {
        rf::console::print("Re-attempting FactionFiles session exchange...");
        start_session_exchange();
    },
    "Force a fresh FactionFiles session-key exchange (e.g. after fixing the GSK in config).",
};

} // namespace

void session_do_patch()
{
    sv_fflink_status_cmd.register_cmd();
    sv_fflink_resync_cmd.register_cmd();
}

} // namespace fflink
