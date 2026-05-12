#include "fflink_achievements.h"
#include "fflink_utils.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <xlog/xlog.h>

#include <common/HttpRequest.h>
#include <common/version/version.h>

namespace fflink::achievements {

namespace {

constexpr const char* k_base_url = "https://link.factionfiles.com/afachievement/v1";

UnlockedAchievementsResult parse_response(const std::string& response)
{
    UnlockedAchievementsResult out;

    if (response.empty()) {
        out.error_detail = "empty response";
        return out;
    }

    std::istringstream stream(response);
    std::string token;

    if (!std::getline(stream, token, ',')) {
        out.error_detail = "response missing key field";
        return out;
    }

    try {
        out.returned_key = std::stoi(token);
    }
    catch (const std::exception& e) {
        out.error_detail = std::string{"key field not an integer: "} + e.what();
        return out;
    }

    while (std::getline(stream, token, ',')) {
        if (token.empty()) {
            continue;
        }
        try {
            out.unlocked_uids.push_back(std::stoi(token));
        }
        catch (const std::exception& e) {
            // Non-integer token — log and skip rather than failing the whole batch.
            xlog::warn("[fflink::achievements] skipping non-integer uid token '{}': {}",
                sanitize_for_log(token), e.what());
        }
    }

    out.ok = true;
    return out;
}

std::string fetch_body(HttpRequest& request)
{
    char buf[4096];
    std::ostringstream stream;
    while (size_t n = request.read(buf, sizeof(buf))) {
        stream.write(buf, n);
    }
    return stream.str();
}

void run_sync_blocking(const std::string& token, UnlockedAchievementsResult& out)
{
    const std::string url = std::string{k_base_url} + "/initial/" + encode_uri_component(token);
    xlog::info("[fflink::achievements] >>> GET initial sync");

    try {
        HttpSession session(AF_USER_AGENT_SUFFIX("Achievement Sync"));
        HttpRequest request(url, "GET", session);
        request.send();
        out = parse_response(fetch_body(request));
    }
    catch (const std::exception& e) {
        out.ok = false;
        out.error_detail = std::string{"network error: "} + e.what();
    }
}

void run_push_blocking(const std::string& token, int key, const std::string& payload,
                       UnlockedAchievementsResult& out)
{
    const std::string url = std::string{k_base_url} + "/update/" + encode_uri_component(token);
    const std::string body = "key=" + std::to_string(key) + "," + payload;
    xlog::debug("[fflink::achievements] >>> POST update key={}", key);

    try {
        HttpSession session(AF_USER_AGENT_SUFFIX("Achievement Push"));
        HttpRequest request(url, "POST", session);
        request.set_content_type("application/x-www-form-urlencoded");
        request.send(body);
        out = parse_response(fetch_body(request));
    }
    catch (const std::exception& e) {
        out.ok = false;
        out.error_detail = std::string{"network error: "} + e.what();
    }
}

} // namespace

void sync_async(std::string token, std::function<void(UnlockedAchievementsResult)> on_main)
{
    std::thread([token = std::move(token), on_main = std::move(on_main)]() {
        UnlockedAchievementsResult result;
        run_sync_blocking(token, result);
        if (!result.ok) {
            xlog::error("[fflink::achievements] sync failed: {}", result.error_detail);
        }
        enqueue_main_thread_task([on_main = std::move(on_main), result = std::move(result)]() mutable {
            on_main(std::move(result));
        });
    }).detach();
}

void push_async(std::string token, int key, std::string payload,
                std::function<void(UnlockedAchievementsResult)> on_main)
{
    std::thread([token = std::move(token), key, payload = std::move(payload),
                 on_main = std::move(on_main)]() {
        UnlockedAchievementsResult result;
        run_push_blocking(token, key, payload, result);
        if (!result.ok) {
            xlog::error("[fflink::achievements] push failed (key={}): {}", key, result.error_detail);
        }
        enqueue_main_thread_task([on_main = std::move(on_main), result = std::move(result)]() mutable {
            on_main(std::move(result));
        });
    }).detach();
}

} // namespace fflink::achievements
