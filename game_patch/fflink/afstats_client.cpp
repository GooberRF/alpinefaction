#include "afstats_client.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <json.hpp>
#include <xlog/xlog.h>

#include <common/HttpRequest.h>
#include <common/version/version.h>

#include "../main/main.h"
#include "../multi/alpine_packets.h"
#include "../multi/multi.h"
#include "../os/console.h"
#include "../rf/multi.h"
#include "fflink_utils.h"

namespace fflink {

namespace {

constexpr const char* k_session_url = "https://www.factionfiles.com/pfapi/afstats/v1/playerstatssession.php";
constexpr const char* k_user_agent = AF_USER_AGENT_SUFFIX("AFStatsPlayer");

// Connect and receive timeouts in ms
constexpr unsigned long k_connect_timeout_ms = 3000;
constexpr unsigned long k_receive_timeout_ms = 5000;

// Delays before each retry (seconds). The array size is also the retry count.
constexpr int k_retry_delays_s[] = {2, 5};

// How long a mint_rate_limited response keeps us from asking for a brand new PSK.
constexpr auto k_mint_backoff = std::chrono::minutes(15);

enum class Status
{
    idle,
    in_flight,
    done,
    failed,
};

const char* status_to_str(Status s)
{
    switch (s) {
        case Status::idle:      return "not started";
        case Status::in_flight: return "exchange in progress";
        case Status::done:      return "ready";
        case Status::failed:    return "unavailable";
    }
    return "unknown";
}

struct JoinState
{
    uint32_t join_id = 0;
    std::optional<rf::NetAddr> target;
    bool stats_enabled = false;
    bool exchange_started = false;
    Status status = Status::idle;
    std::string pssk;
    int user_id = -1;
    std::string user_name;
    bool in_game = false;
    bool pssk_sent = false;
};

// Owned by the main thread; workers only ever enqueue tasks that touch it.
JoinState g_state;

// Mirror of g_state.join_id that workers may read, so they can abort between retries.
std::atomic<uint32_t> g_active_join_id{0};

// Survives joins: the FF-side mint limit is per IP per hour, not per connection.
std::chrono::steady_clock::time_point g_mint_backoff_until{};

bool g_stale_link_notice_shown = false;

struct ExchangeOutcome
{
    enum class Kind {
        success,
        client_error,      // 400/405: we built a bad request, retrying can't help
        mint_rate_limited, // 429: give up for this join and back off minting
        transient_error,   // 5xx, network, parse: retry
    };
    Kind kind = Kind::transient_error;
    std::string psk;
    std::string pssk;
    std::string psk_origin;
    std::string fflink_status;
    int user_id = -1;
    std::string user_name;
    std::string error_detail; // safe-to-display message
};

const char* kind_to_str(ExchangeOutcome::Kind kind)
{
    switch (kind) {
        case ExchangeOutcome::Kind::success:           return "success";
        case ExchangeOutcome::Kind::client_error:      return "client_error";
        case ExchangeOutcome::Kind::mint_rate_limited: return "mint_rate_limited";
        case ExchangeOutcome::Kind::transient_error:   return "transient_error";
    }
    return "unknown";
}

ExchangeOutcome do_one_exchange(const std::string& psk, const std::string& fflink_token)
{
    ExchangeOutcome out;

    // Absent credentials must be JSON null; an empty string is a 400 on the FF side.
    const auto as_json = [](const std::string& value) {
        return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    const std::string body =
        nlohmann::json{{"psk", as_json(psk)}, {"fflink_token", as_json(fflink_token)}}.dump();

    // Redacted copy for logging — neither key may ever be written to disk. The
    // placeholder is fixed width so the log does not even carry a key length.
    const auto redacted = [](const std::string& value) {
        return value.empty() ? nlohmann::json(nullptr) : nlohmann::json("<redacted>");
    };
    const std::string body_for_log =
        nlohmann::json{{"psk", redacted(psk)}, {"fflink_token", redacted(fflink_token)}}.dump();
    xlog::warn("[afstats] >>> POST {}", k_session_url);
    xlog::warn("[afstats] >>> User-Agent: {}", k_user_agent);
    xlog::warn("[afstats] >>> Content-Type: application/json");
    xlog::warn("[afstats] >>> Content-Length: {}", body.size());
    xlog::warn("[afstats] >>> body ({} bytes): {}", body.size(), body_for_log);

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
        xlog::warn("[afstats] <<< {} (classified transient)", out.error_detail);
        return out;
    }

    xlog::warn("[afstats] <<< HTTP {} from {} ({} byte body)", status_code, k_session_url, response.size());

    constexpr size_t k_log_response_prefix = 256;
    auto log_body_preview = [&]() {
        const std::string response_preview = sanitize_for_log(
            response.size() <= k_log_response_prefix
                ? std::string_view{response}
                : std::string_view{response}.substr(0, k_log_response_prefix));
        const char* truncated = response.size() > k_log_response_prefix ? "...[truncated]" : "";
        xlog::warn("[afstats] <<< body: {}{}", response_preview, truncated);
    };

    if (status_code == 200) {
        try {
            auto j = nlohmann::json::parse(response);
            auto psk_out = j.at("psk").get<std::string>();
            auto pssk_out = j.at("pssk").get<std::string>();
            if (!is_valid_stats_key_format(psk_out)) {
                throw std::runtime_error("psk has invalid format");
            }
            if (!is_valid_stats_key_format(pssk_out)) {
                throw std::runtime_error("pssk has invalid format");
            }
            out.kind = ExchangeOutcome::Kind::success;
            out.psk = std::move(psk_out);
            out.pssk = std::move(pssk_out);
            if (j.contains("psk_origin") && j["psk_origin"].is_string()) {
                out.psk_origin = sanitize_for_log(j["psk_origin"].get<std::string>());
            }
            if (j.contains("fflink_status") && j["fflink_status"].is_string()) {
                out.fflink_status = sanitize_for_log(j["fflink_status"].get<std::string>());
            }
            if (j.contains("user_id") && j["user_id"].is_number_integer()) {
                out.user_id = j["user_id"].get<int>();
            }
            if (j.contains("user_name") && j["user_name"].is_string()) {
                out.user_name = sanitize_for_log(j["user_name"].get<std::string>());
            }
            xlog::warn("[afstats] <<< parsed OK (classified success)");
            return out;
        }
        catch (const nlohmann::json::parse_error& e) {
            // Never log the body or e.what() here: a parse error quotes the input it
            // choked on, and on a 200 that input is key material.
            xlog::warn("[afstats] HTTP 200 but the body failed to parse (error {} at byte {}); "
                       "body length={} bytes",
                       e.id, e.byte, response.size());
            out.kind = ExchangeOutcome::Kind::transient_error;
            out.error_detail = "malformed JSON response";
            return out;
        }
        catch (const std::exception& e) {
            // Missing/mistyped fields and our own format checks. These describe the
            // shape of the response, never its contents.
            xlog::warn("[afstats] HTTP 200 but validation failed: {}; body length={} bytes", e.what(),
                       response.size());
            out.kind = ExchangeOutcome::Kind::transient_error;
            out.error_detail = std::string{"invalid response: "} + e.what();
            return out;
        }
    }

    log_body_preview();

    std::string error_code = "unspecified";
    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("error") && j["error"].is_string()) {
            error_code = sanitize_for_log(j["error"].get<std::string>());
        }
    }
    catch (const std::exception&) {
        // Body wasn't JSON; leave error_code as default.
    }

    out.error_detail = std::format("HTTP {} ({})", status_code, error_code);
    if (status_code == 429) {
        out.kind = ExchangeOutcome::Kind::mint_rate_limited;
    }
    else if (status_code == 400 || status_code == 405) {
        out.kind = ExchangeOutcome::Kind::client_error;
    }
    else {
        out.kind = ExchangeOutcome::Kind::transient_error;
    }
    xlog::warn("[afstats] <<< {} classified as {}", out.error_detail, kind_to_str(out.kind));
    return out;
}

void maybe_send_pssk()
{
    if (g_state.status != Status::done) {
        xlog::warn("[afstats] holding PSSK delivery: exchange status is {}", status_to_str(g_state.status));
        return;
    }
    if (!g_state.in_game) {
        xlog::warn("[afstats] holding PSSK delivery: not in the game yet");
        return;
    }
    if (g_state.pssk_sent) {
        xlog::warn("[afstats] holding PSSK delivery: already sent for this join");
        return;
    }
    // VERIFICATION PHASE ONLY: logs the session key verbatim, by explicit request.
    xlog::warn("[afstats] delivering PSSK {} to server", g_state.pssk);
    af_send_stats_pssk(g_state.pssk);
    g_state.pssk_sent = true;
    xlog::warn("[afstats] PSSK sent (join_id={})", g_state.join_id);
}

void on_exchange_finished(uint32_t join_id, ExchangeOutcome outcome)
{
    if (join_id != g_state.join_id) {
        xlog::warn("[afstats] dropping stale exchange result (started for join_id={}, current join_id={})",
                   join_id, g_state.join_id);
        return; // the join this was started for is gone
    }

    xlog::warn("[afstats] exchange finished for join_id={} with outcome {}", join_id,
               kind_to_str(outcome.kind));

    if (outcome.kind == ExchangeOutcome::Kind::mint_rate_limited) {
        g_mint_backoff_until = std::chrono::steady_clock::now() + k_mint_backoff;
        xlog::warn("[afstats] mint backoff armed for the next {} minutes", k_mint_backoff.count());
    }

    if (outcome.kind != ExchangeOutcome::Kind::success) {
        xlog::warn("[afstats] no stats session this join: {} ({})", kind_to_str(outcome.kind),
                   outcome.error_detail);
        g_state.status = Status::failed;
        rf::console::print("Stats are unavailable this session ({}).", outcome.error_detail);
        return;
    }

    // The returned PSK is authoritative; it differs from ours whenever we sent
    // none, sent one FactionFiles no longer knows, or the player relinked.
    if (outcome.psk != g_game_config.afstats_psk.value()) {
        g_game_config.afstats_psk = outcome.psk;
        g_game_config.save();
        xlog::warn("[afstats] new PSK persisted to config");
    }
    else {
        xlog::warn("[afstats] PSK unchanged, nothing written to config");
    }

    g_state.status = Status::done;
    g_state.pssk = std::move(outcome.pssk);
    g_state.user_id = outcome.user_id;
    g_state.user_name = std::move(outcome.user_name);

    // VERIFICATION PHASE ONLY: logs the session key verbatim, by explicit request.
    xlog::warn("[afstats] obtained PSSK {}", g_state.pssk);
    xlog::warn("[afstats] session ready (psk_origin={}, fflink_status={}, user_id={}, user_name={})",
               outcome.psk_origin.empty() ? "<absent>" : outcome.psk_origin,
               outcome.fflink_status.empty() ? "<absent>" : outcome.fflink_status, g_state.user_id,
               g_state.user_name.empty() ? "anonymous" : g_state.user_name);

    if (outcome.fflink_status == "unknown" && !g_stale_link_notice_shown) {
        g_stale_link_notice_shown = true;
        xlog::warn("[afstats] fflink token is stale; showing the re-link notice once this session");
        rf::console::print("Your FactionFiles link has expired. Re-link from the launcher.");
    }

    maybe_send_pssk();
}

void exchange_worker_impl(uint32_t join_id, const std::string& psk, const std::string& fflink_token)
{
    constexpr int max_attempts = static_cast<int>(std::size(k_retry_delays_s)) + 1;

    for (int attempt = 0;; ++attempt) {
        const uint32_t active = g_active_join_id.load(std::memory_order_acquire);
        if (active != join_id) {
            xlog::warn("[afstats] worker aborting: join_id={} is stale (active join_id={})", join_id,
                       active);
            return;
        }

        xlog::warn("[afstats] exchange attempt {}/{} for join_id={}", attempt + 1, max_attempts, join_id);

        // Always the same PSK: retrying with null would abandon the player's key.
        ExchangeOutcome outcome = do_one_exchange(psk, fflink_token);

        const bool can_retry = outcome.kind == ExchangeOutcome::Kind::transient_error
            && attempt < static_cast<int>(std::size(k_retry_delays_s));
        if (!can_retry) {
            xlog::warn("[afstats] no further attempts ({}), handing result to the main thread",
                       kind_to_str(outcome.kind));
            enqueue_main_thread_task([join_id, outcome = std::move(outcome)]() mutable {
                on_exchange_finished(join_id, std::move(outcome));
            });
            return;
        }

        const int delay_s = k_retry_delays_s[attempt];
        xlog::warn("[afstats] attempt {} failed ({}); retrying in {}s", attempt + 1, outcome.error_detail,
                   delay_s);
        std::this_thread::sleep_for(std::chrono::seconds(delay_s));
    }
}

void exchange_worker(uint32_t join_id, std::string psk, std::string fflink_token)
{
    // Catch-all wrapper and safe escape for worker thread
    try {
        exchange_worker_impl(join_id, psk, fflink_token);
    }
    catch (const std::exception& e) {
        xlog::warn("[afstats] stats session worker terminated unexpectedly: {}", e.what());
    }
    catch (...) {
        xlog::warn("[afstats] stats session worker terminated with unknown exception");
    }
}

void maybe_start_exchange()
{
    if (g_state.exchange_started) {
        xlog::warn("[afstats] skipping exchange: already started for join_id={}", g_state.join_id);
        return; // join_req resends and the join_accept trigger both land here
    }
    if (client_bot_launch_enabled()) {
        xlog::warn("[afstats] skipping exchange: this client is a launch bot");
        return; // client bots must never mint PSKs
    }

    std::string psk = g_game_config.afstats_psk.value();
    if (!psk.empty() && !is_valid_stats_key_format(psk)) {
        xlog::warn("[afstats] stored PSK is malformed; cleared it and asking FactionFiles for a new one");
        psk.clear();
    }
    // The mint limit only applies to new keys, so a client holding one is never blocked.
    const auto now = std::chrono::steady_clock::now();
    if (psk.empty() && now < g_mint_backoff_until) {
        const auto remaining_s =
            std::chrono::duration_cast<std::chrono::seconds>(g_mint_backoff_until - now).count();
        xlog::warn("[afstats] skipping exchange: mint backoff active for another {}s and no stored PSK",
                   remaining_s);
        return;
    }

    g_state.exchange_started = true;
    g_state.status = Status::in_flight;

    const bool have_token = !g_game_config.fflink_token.value().empty();
    xlog::warn("[afstats] starting exchange (join_id={}, stored PSK: {}, fflink token: {})",
               g_state.join_id, psk.empty() ? "no" : "yes", have_token ? "yes" : "no");

    try {
        std::thread(exchange_worker, g_state.join_id, std::move(psk),
                    g_game_config.fflink_token.value())
            .detach();
    }
    catch (const std::exception& e) {
        xlog::warn("[afstats] failed to spawn stats session thread: {}", e.what());
        g_state.status = Status::failed;
    }
}

ConsoleCommand2 afstats_status_cmd{
    "afstats_status",
    []() {
        rf::console::print("Stats-enabled server: {}", g_state.stats_enabled ? "yes" : "no");
        rf::console::print("  Stats session: {}{}", status_to_str(g_state.status),
            g_state.pssk_sent ? " (delivered to server)" : "");
        // The PSK remembers the account it was linked to, so attribution can hold
        // even when the token we sent was stale.
        rf::console::print("  Signed in as: {}", g_state.user_id < 0
            ? std::string{"anonymous"}
            : (g_state.user_name.empty() ? std::to_string(g_state.user_id) : g_state.user_name));
    },
    "Show the status of this session's FactionFiles player stats link.",
};

} // namespace

void afstats_on_join_req(const rf::NetAddr& addr, bool stats_enabled)
{
    xlog::warn("[afstats] join_req to {} (stats_enabled={})", addr, stats_enabled ? "yes" : "no");

    // The stock cancel path leaves multiplayer without a multi_stop, so a join the
    // player abandoned is only cleared here, when the next one starts elsewhere.
    if (g_state.target && *g_state.target != addr) {
        xlog::warn("[afstats] target changed from {} to {}; dropping the abandoned join's state",
                   *g_state.target, addr);
        afstats_reset();
    }
    g_state.target = addr;

    if (!stats_enabled) {
        xlog::warn("[afstats] browser entry says this server is not stats-enabled; no exchange from join_req");
        return;
    }
    g_state.stats_enabled = true;
    maybe_start_exchange();
}

void afstats_on_join_accept(bool stats_enabled)
{
    xlog::warn("[afstats] join_accept parsed (stats_enabled={}, exchange already started={})",
               stats_enabled ? "yes" : "no", g_state.exchange_started ? "yes" : "no");

    if (!stats_enabled) {
        return;
    }
    g_state.stats_enabled = true;
    maybe_start_exchange();
}

void afstats_on_entered_game()
{
    g_state.in_game = true;
    xlog::warn("[afstats] entered the game (join_id={}, exchange status={})", g_state.join_id,
               status_to_str(g_state.status));
    maybe_send_pssk();
}

void afstats_reset()
{
    const uint32_t next_join_id = g_state.join_id + 1;
    xlog::warn("[afstats] resetting join state (join_id {} -> {})", g_state.join_id, next_join_id);
    g_state = JoinState{};
    g_state.join_id = next_join_id;
    g_active_join_id.store(next_join_id, std::memory_order_release);
}

void afstats_do_patch()
{
    afstats_status_cmd.register_cmd();
}

} // namespace fflink
