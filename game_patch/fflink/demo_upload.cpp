#include "demo_upload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <zlib.h>

#include <nlohmann/json.hpp>
#include <xlog/xlog.h>

#include <common/HttpRequest.h>
#include <common/version/version.h>

#include "../multi/demo/demo_file.h"
#include "../multi/server.h"
#include "../os/console.h"
#include "../rf/multi.h"
#include "fflink_session.h"
#include "fflink_utils.h"

namespace fflink {

namespace {

constexpr const char* k_ticket_url = "https://link.factionfiles.com/afstats/v1/demoticket.php";
constexpr const char* k_user_agent = AF_USER_AGENT_SUFFIX("AFStatsDemo");

// The hard FactionFiles-side ceiling. The effective limit is the smaller of this and the
// configured fflink_demo_max_mb; a file over either is never enqueued.
constexpr uint64_t k_ff_max_upload_bytes = 100ull * 1024 * 1024;

constexpr unsigned long k_connect_timeout_ms = 5000;
constexpr unsigned long k_send_timeout_ms = 30000;    // per write op; a 64KB chunk is quick
constexpr unsigned long k_receive_timeout_ms = 10000;

// The ticket / upload acks are small JSON; cap the read so a broken or hostile endpoint
// can't stream unbounded data into worker memory.
constexpr size_t k_max_response_bytes = 256 * 1024;

// File read chunk for CRC and for streaming the PUT body, so a demo is never fully buffered.
constexpr size_t k_chunk_bytes = 64 * 1024;

// Backoff before each retry (seconds). After the last entry the file is given up.
constexpr int k_backoff_schedule_s[] = {30, 120, 600, 3600};

// Reschedule for a benign, non-counting deferral (no session key yet).
constexpr auto k_retry_soon = std::chrono::seconds(30);

// How often the per-frame pump considers starting a new upload. One-at-a-time plus this
// spacing keeps uploads low priority against game traffic.
constexpr auto k_pump_interval = std::chrono::seconds(2);

// -------------------------------------------------------------------------
// Outcome the worker reports back to the main thread.
// -------------------------------------------------------------------------

enum class OutcomeKind
{
    success,      // 200 ok or duplicate:true -> apply the post-success action
    definitive,   // give up on this file permanently this process
    transient,    // backoff and retry
    reauth,       // like transient, plus kick a session re-exchange (ticket invalid_gssk)
    retry_soon,   // benign deferral (no GSSK yet); reschedule without spending a retry
    retry_once,   // upload crc_mismatch: one extra retry, then definitive
    long_backoff, // demos_disabled(503)/no_space(507): defer ~1h indefinitely, keep on disk
    reticket,     // upload 401: re-ticket promptly (bounded) without spending a retry
};

struct UploadOutcome
{
    OutcomeKind kind = OutcomeKind::transient;
    std::string detail;  // safe-to-log, already sanitized
    std::string game_id; // informational, from a successful upload
};

struct QueueItem
{
    std::string path;
    int attempts = 0; // transient failures so far
    bool crc_retry_used = false; // the one crc_mismatch retry has been spent
    int reticket_count = 0;      // upload-401 re-ticket attempts so far
    std::chrono::steady_clock::time_point next_attempt{};
};

// -------------------------------------------------------------------------
// Module state. All of it is main-thread-owned; workers never touch it.
// -------------------------------------------------------------------------

std::deque<QueueItem> g_queue; // pending uploads, oldest first
std::unordered_set<std::string> g_queued_paths; // membership mirror of g_queue
std::unordered_set<std::string> g_dropped;      // given up this server session
bool g_busy = false;                            // one upload in flight
std::string g_in_flight_path;
// Bumped whenever the server session changes so a worker started under a prior session
// (whose queue is gone) is ignored when it reports back.
uint32_t g_epoch = 0;
bool g_server_up = false;
std::chrono::steady_clock::time_point g_next_pump{};

uint64_t g_uploads_ok = 0;
std::string g_last_result = "none yet";

// Forward declarations (definitions live below, in call order).
void on_upload_finished(std::string path, uint32_t epoch, UploadOutcome outcome);
void upload_worker(std::string path, uint32_t epoch);
bool is_eligible(const std::string& path, uint64_t* size_out);
void enqueue_path(const std::string& path);
void maybe_start_upload();

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------

std::string base_name(const std::string& path)
{
    const auto p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

bool is_https_url(const std::string& url)
{
    return url.rfind("https://", 0) == 0;
}

bool ends_with_afd(const std::string& path)
{
    return path.size() >= 4 && _stricmp(path.c_str() + path.size() - 4, ".afd") == 0;
}

bool is_valid_session_id(const std::string& s)
{
    if (s.size() != 16) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<uint64_t> file_size_on_disk(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
        return std::nullopt;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return std::nullopt;
    }
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

uint64_t effective_size_limit()
{
    const uint64_t cfg = static_cast<uint64_t>(std::max(1, server_fflink_demo_max_mb())) * 1024ull * 1024ull;
    return std::min(cfg, k_ff_max_upload_bytes);
}

struct DemoIdentity
{
    std::string session_id;
    uint32_t game = 0;
};

std::optional<DemoIdentity> read_demo_identity(const std::string& path)
{
    DemoFileReader reader;
    if (reader.open(path) != DemoFileReader::OpenResult::ok) {
        return std::nullopt;
    }
    const auto& h = reader.header();
    if (!is_valid_session_id(h.afstats_session_id) || h.afstats_game < 1) {
        return std::nullopt;
    }
    return DemoIdentity{h.afstats_session_id, h.afstats_game};
}

// A demo is eligible when it is an .afd whose header carries a valid identity and whose
// on-disk size is within the effective ceiling. Ineligible files are left on disk untouched.
bool is_eligible(const std::string& path, uint64_t* size_out)
{
    if (!ends_with_afd(path)) {
        return false;
    }
    const auto sz = file_size_on_disk(path);
    if (!sz || *sz == 0 || *sz > effective_size_limit()) {
        return false;
    }
    if (!read_demo_identity(path)) {
        return false;
    }
    if (size_out) {
        *size_out = *sz;
    }
    return true;
}

// A read-only Win32 file handle, shared-read so main-thread eligibility reads don't conflict.
struct FileReader
{
    HANDLE h = INVALID_HANDLE_VALUE;

    explicit FileReader(const std::string& path)
    {
        h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~FileReader()
    {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    // Returns bytes read, 0 at EOF, or -1 on error.
    long read(void* buf, DWORD want)
    {
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, nullptr)) {
            return -1;
        }
        return static_cast<long>(got);
    }
};

// CRC-32 of the whole file as it sits on disk (gzip container included), read in chunks so
// the file is never fully buffered. Returns nullopt if the read failed or the file's size
// no longer matches what was measured (bytes and crc must describe the same content).
std::optional<uint32_t> crc32_of_file(const std::string& path, uint64_t expected_size)
{
    FileReader f{path};
    if (!f.ok()) {
        return std::nullopt;
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    std::vector<uint8_t> buf(k_chunk_bytes);
    uint64_t total = 0;
    while (true) {
        const long got = f.read(buf.data(), static_cast<DWORD>(buf.size()));
        if (got < 0) {
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        crc = crc32(crc, buf.data(), static_cast<uInt>(got));
        total += static_cast<uint64_t>(got);
    }
    if (total != expected_size) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(crc);
}

std::string read_capped(HttpRequest& req)
{
    char buf[1024];
    std::string out;
    size_t total = 0;
    while (size_t n = req.read(buf, sizeof(buf))) {
        total += n;
        if (total > k_max_response_bytes) {
            throw std::runtime_error("response exceeded size cap");
        }
        out.append(buf, n);
    }
    return out;
}

std::string extract_error(const std::string& response)
{
    try {
        auto j = nlohmann::json::parse(response);
        if (j.contains("error") && j["error"].is_string()) {
            return sanitize_for_log(j["error"].get<std::string>());
        }
    }
    catch (const std::exception&) {
        // Body wasn't JSON.
    }
    return "unspecified";
}

// -------------------------------------------------------------------------
// The single post-success action. This is the ONLY place a demo file is ever removed from
// disk or relocated, and it runs on exactly one path: a confirmed success (or duplicate).
// delete_after_send true -> delete; false -> move into demos/uploaded. Every other outcome
// leaves the file untouched, so a demo is deleted iff (success AND delete_after_send) and
// moved iff (success AND !delete_after_send).
// -------------------------------------------------------------------------

void move_to_uploaded(const std::string& path)
{
    const std::string dir = demo_file_uploaded_dir(true);
    if (dir.empty()) {
        xlog::warn("[demo-upload] cannot resolve demos/uploaded; leaving uploaded demo in place: {}", path);
        return;
    }
    const std::string leaf = base_name(path);
    std::string dest = dir + "\\" + leaf;
    // Never clobber a distinct demo already there: on collision add a numeric suffix so the
    // "it's uploaded" fact is never lost.
    if (GetFileAttributesA(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string stem = leaf;
        std::string ext;
        if (ends_with_afd(stem)) {
            ext = stem.substr(stem.size() - 4);
            stem = stem.substr(0, stem.size() - 4);
        }
        for (int n = 1; n < 100000; ++n) {
            std::string cand = std::format("{}\\{}_{}{}", dir, stem, n, ext);
            if (GetFileAttributesA(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
                dest = cand;
                break;
            }
        }
    }
    // No MOVEFILE_REPLACE_EXISTING: a file may have appeared at dest since the collision
    // check above (TOCTOU), and overwriting it would clobber a distinct demo. On failure
    // the file is left in place (logged below).
    if (MoveFileA(path.c_str(), dest.c_str())
        || MoveFileExA(path.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED)) {
        xlog::info("[demo-upload] moved uploaded demo to {}", dest);
        return;
    }
    // The file is already stored on FactionFiles, so a failed move loses nothing.
    xlog::warn("[demo-upload] failed to move uploaded demo {} -> {} (error {}); left in place", path, dest,
               GetLastError());
}

void apply_post_success_action(const std::string& path)
{
    if (server_fflink_demo_delete_after_send()) {
        if (demo_file_delete(path)) {
            xlog::info("[demo-upload] deleted uploaded demo (delete_after_send): {}", path);
        }
        // A failed delete still counts as uploaded; the file simply lingers.
        return;
    }
    move_to_uploaded(path);
}

// -------------------------------------------------------------------------
// Worker (detached thread, one demo at a time)
// -------------------------------------------------------------------------

struct Ticket
{
    std::string ticket;
    std::string upload_url;
};

// Requests an upload ticket. Returns true with out_ticket filled, or false with out_fail set.
bool request_ticket(const std::string& gssk, const std::string& session, uint32_t game, uint64_t bytes,
                    uint32_t crc, Ticket& out_ticket, UploadOutcome& out_fail)
{
    const std::string body = nlohmann::json{
        {"gssk", gssk},
        {"session", session},
        {"game", game},
        {"bytes", bytes},
        {"crc32", crc},
        {"format", "afd"},
    }.dump();

    std::string response;
    int status = 0;
    try {
        HttpSession sess(k_user_agent);
        sess.set_connect_timeout(k_connect_timeout_ms);
        sess.set_receive_timeout(k_receive_timeout_ms);

        HttpRequest req(k_ticket_url, "POST", sess);
        req.set_content_type("application/json");
        status = req.send_no_check(body);
        response = read_capped(req);
    }
    catch (const std::exception& e) {
        out_fail = {OutcomeKind::transient, std::string{"ticket network error: "} + e.what(), {}};
        return false;
    }

    if (status == 200) {
        try {
            auto j = nlohmann::json::parse(response);
            out_ticket.ticket = j.at("ticket").get<std::string>();
            out_ticket.upload_url = j.at("upload_url").get<std::string>();
            if (out_ticket.ticket.empty() || !is_https_url(out_ticket.upload_url)) {
                out_fail = {OutcomeKind::definitive, "ticket missing ticket/upload_url or non-https url", {}};
                return false;
            }
            return true;
        }
        catch (const std::exception&) {
            out_fail = {OutcomeKind::transient, "malformed ticket JSON", {}};
            return false;
        }
    }

    const std::string detail = std::format("ticket HTTP {} ({})", status, extract_error(response));
    if (status == 401) {
        // invalid_gssk: same recoverable condition as the events endpoint. Re-exchange + backoff.
        out_fail = {OutcomeKind::reauth, detail, {}};
    }
    else if (status == 429) {
        out_fail = {OutcomeKind::transient, detail, {}};
    }
    else if (status == 503) {
        // demos_disabled: switched off site-wide -> defer on a long cadence, keep the file.
        out_fail = {OutcomeKind::long_backoff, detail, {}};
    }
    else if (status >= 400 && status < 500) {
        // invalid_json / invalid_*_format / invalid_game / invalid_bytes / invalid_crc32 /
        // unsupported_format / method_not_allowed / demo_too_large -> definitive.
        out_fail = {OutcomeKind::definitive, detail, {}};
    }
    else {
        // internal_error (500), anything else -> backoff.
        out_fail = {OutcomeKind::transient, detail, {}};
    }
    return false;
}

// PUTs the file verbatim with the ticket as a bearer token.
UploadOutcome do_put(const std::string& upload_url, const std::string& ticket, const std::string& path,
                     uint64_t bytes)
{
    UploadOutcome out;
    std::string response;
    int status = 0;
    try {
        HttpSession sess(k_user_agent);
        sess.set_connect_timeout(k_connect_timeout_ms);
        sess.set_send_timeout(k_send_timeout_ms);
        sess.set_receive_timeout(k_receive_timeout_ms);

        HttpRequest req(upload_url, "PUT", sess);
        req.add_header("Authorization", "Bearer " + ticket);
        req.set_content_type("application/octet-stream");
        // begin_body sets Content-Length via HttpSendRequestEx; WinINet then sends
        // Expect: 100-continue for a body this size. The bytes are streamed verbatim -
        // never decompressed, re-compressed, or wrapped in multipart.
        req.begin_body(static_cast<size_t>(bytes));

        FileReader f{path};
        if (!f.ok()) {
            return {OutcomeKind::transient, "cannot open demo for upload", {}};
        }
        std::vector<uint8_t> buf(k_chunk_bytes);
        uint64_t sent = 0;
        while (sent < bytes) {
            const DWORD want = static_cast<DWORD>(std::min<uint64_t>(k_chunk_bytes, bytes - sent));
            const long got = f.read(buf.data(), want);
            if (got <= 0) {
                break;
            }
            req.write(buf.data(), static_cast<size_t>(got));
            sent += static_cast<uint64_t>(got);
        }
        if (sent != bytes) {
            // Content-Length was already committed as `bytes`; a short body is a failed transfer.
            return {OutcomeKind::transient, "demo changed size during upload", {}};
        }
        status = req.send_no_check();
        response = read_capped(req);
    }
    catch (const std::exception& e) {
        return {OutcomeKind::transient, std::string{"upload network error: "} + e.what(), {}};
    }

    if (status == 200) {
        try {
            auto j = nlohmann::json::parse(response);
            const bool ok = j.value("ok", false);
            const bool duplicate = j.value("duplicate", false);
            if (ok || duplicate) {
                out.kind = OutcomeKind::success;
                out.detail = duplicate ? "duplicate" : "ok";
                if (j.contains("game_id")) {
                    if (j["game_id"].is_string()) {
                        out.game_id = sanitize_for_log(j["game_id"].get<std::string>());
                    }
                    else if (j["game_id"].is_number_integer()) {
                        out.game_id = std::to_string(j["game_id"].get<int64_t>());
                    }
                }
                return out;
            }
            return {OutcomeKind::transient, "200 without ok/duplicate", {}};
        }
        catch (const std::exception&) {
            return {OutcomeKind::transient, "malformed upload JSON", {}};
        }
    }

    const std::string err = extract_error(response);
    const std::string detail = std::format("upload HTTP {} ({})", status, err);
    if (status == 401) {
        // invalid_ticket / ticket_expired: re-ticket promptly, bounded, without burning a
        // normal backoff retry (a slow uplink can outlast a ticket's validity window).
        out.kind = OutcomeKind::reticket;
    }
    else if (status == 429) {
        out.kind = OutcomeKind::transient;
    }
    else if (status == 507) {
        // no_space: storage below its free-space floor -> defer on a long cadence, keep the file.
        out.kind = OutcomeKind::long_backoff;
    }
    else if (status == 400 && err == "crc_mismatch") {
        // A truncated/corrupted transfer; the contract allows exactly one retry before giving up.
        out.kind = OutcomeKind::retry_once;
    }
    else if (status >= 400 && status < 500) {
        // length_required / too_large / size_mismatch / not_gzip / not_afd /
        // unsupported_format_version / bad_header / uncompressed_too_large / suspicious_ratio /
        // conflict -> definitive.
        out.kind = OutcomeKind::definitive;
    }
    else {
        // busy upstream, internal_error (500), anything else -> backoff.
        out.kind = OutcomeKind::transient;
    }
    out.detail = detail;
    return out;
}

void report(std::string path, uint32_t epoch, UploadOutcome outcome)
{
    enqueue_main_thread_task(
        [path = std::move(path), epoch, outcome = std::move(outcome)]() mutable {
            on_upload_finished(std::move(path), epoch, std::move(outcome));
        });
}

void upload_worker_impl(const std::string& path, uint32_t epoch)
{
    const auto id = read_demo_identity(path);
    const auto sz = file_size_on_disk(path);
    if (!id || !sz || *sz == 0) {
        report(path, epoch, {OutcomeKind::definitive, "demo no longer has a valid identity/size", {}});
        return;
    }
    if (*sz > k_ff_max_upload_bytes) {
        report(path, epoch, {OutcomeKind::definitive, "demo exceeds hard size ceiling", {}});
        return;
    }

    // The session exchange runs independently; without a key defer without spending a retry.
    const std::string gssk = get_gssk();
    if (gssk.empty()) {
        report(path, epoch, {OutcomeKind::retry_soon, "no session key yet", {}});
        return;
    }

    const auto crc = crc32_of_file(path, *sz);
    if (!crc) {
        report(path, epoch, {OutcomeKind::transient, "failed to read demo for crc", {}});
        return;
    }

    Ticket ticket;
    UploadOutcome fail;
    if (!request_ticket(gssk, id->session_id, id->game, *sz, *crc, ticket, fail)) {
        report(path, epoch, std::move(fail));
        return;
    }

    report(path, epoch, do_put(ticket.upload_url, ticket.ticket, path, *sz));
}

void upload_worker(std::string path, uint32_t epoch)
{
    try {
        upload_worker_impl(path, epoch);
    }
    catch (const std::exception& e) {
        xlog::warn("[demo-upload] upload worker terminated: {}", e.what());
        report(std::move(path), epoch, {OutcomeKind::transient, "worker exception", {}});
    }
    catch (...) {
        xlog::warn("[demo-upload] upload worker terminated (unknown)");
        report(std::move(path), epoch, {OutcomeKind::transient, "worker exception", {}});
    }
}

// -------------------------------------------------------------------------
// Main-thread result handling and queue management
// -------------------------------------------------------------------------

std::deque<QueueItem>::iterator find_item(const std::string& path)
{
    return std::find_if(g_queue.begin(), g_queue.end(),
                        [&](const QueueItem& q) { return q.path == path; });
}

void on_upload_finished(std::string path, uint32_t epoch, UploadOutcome outcome)
{
    if (epoch != g_epoch) {
        // A worker from a previous server session; its queue is gone. Ignore entirely.
        return;
    }
    g_busy = false;
    g_in_flight_path.clear();

    auto it = find_item(path);

    switch (outcome.kind) {
    case OutcomeKind::success:
        apply_post_success_action(path);
        if (it != g_queue.end()) {
            g_queued_paths.erase(it->path);
            g_queue.erase(it);
        }
        ++g_uploads_ok;
        g_last_result = std::format("uploaded {}{}", base_name(path),
                                    outcome.game_id.empty() ? "" : std::format(" (game {})", outcome.game_id));
        xlog::info("[demo-upload] {}", g_last_result);
        rf::console::print("Demo {} uploaded{}", base_name(path),
                           outcome.game_id.empty() ? std::string{} : std::format(" (game {})", outcome.game_id));
        break;

    case OutcomeKind::definitive:
        g_dropped.insert(path);
        if (it != g_queue.end()) {
            g_queued_paths.erase(it->path);
            g_queue.erase(it);
        }
        g_last_result = std::format("gave up on {} ({})", base_name(path), outcome.detail);
        xlog::warn("[demo-upload] {}", g_last_result);
        rf::console::print("Demo upload failed: {} ({})", base_name(path), outcome.detail);
        break;

    case OutcomeKind::reauth:
        // invalid_gssk: the existing "re-exchange and resume" path applies unchanged.
        start_session_exchange();
        [[fallthrough]];
    case OutcomeKind::transient:
        if (it != g_queue.end()) {
            if (it->attempts >= static_cast<int>(std::size(k_backoff_schedule_s))) {
                g_dropped.insert(path);
                g_queued_paths.erase(it->path);
                g_queue.erase(it);
                g_last_result = std::format("gave up on {} after retries ({})", base_name(path), outcome.detail);
                xlog::warn("[demo-upload] {}", g_last_result);
                rf::console::print("Demo upload failed: {} after retries ({})", base_name(path), outcome.detail);
            }
            else {
                const int delay = k_backoff_schedule_s[it->attempts];
                ++it->attempts;
                it->next_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
                g_last_result = std::format("retry {} in {}s ({})", base_name(path), delay, outcome.detail);
                xlog::info("[demo-upload] {}", g_last_result);
            }
        }
        break;

    case OutcomeKind::retry_soon:
        if (it != g_queue.end()) {
            it->next_attempt = std::chrono::steady_clock::now() + k_retry_soon;
        }
        g_last_result = std::format("waiting to upload {} ({})", base_name(path), outcome.detail);
        break;

    case OutcomeKind::retry_once:
        // crc_mismatch: one extra try (no attempts increment), then definitive.
        if (it != g_queue.end()) {
            if (!it->crc_retry_used) {
                it->crc_retry_used = true;
                it->next_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                g_last_result = std::format("retry {} in 30s ({})", base_name(path), outcome.detail);
                xlog::info("[demo-upload] {}", g_last_result);
            }
            else {
                g_dropped.insert(path);
                g_queued_paths.erase(it->path);
                g_queue.erase(it);
                g_last_result = std::format("gave up on {} ({})", base_name(path), outcome.detail);
                xlog::warn("[demo-upload] {}", g_last_result);
                rf::console::print("Demo upload failed: {} ({})", base_name(path), outcome.detail);
            }
        }
        break;

    case OutcomeKind::long_backoff:
        // demos_disabled / no_space: not a failure, just deferred. Retry indefinitely at a
        // 1h cadence without spending a retry; the file stays queued and on disk. Log only.
        if (it != g_queue.end()) {
            it->next_attempt = std::chrono::steady_clock::now() + std::chrono::hours(1);
        }
        g_last_result = std::format("deferred {} ~1h ({})", base_name(path), outcome.detail);
        xlog::info("[demo-upload] {}", g_last_result);
        break;

    case OutcomeKind::reticket:
        // upload 401: re-ticket soon (no attempts increment), bounded so a persistent
        // ticket failure cannot loop forever; past the bound treat as definitive.
        if (it != g_queue.end()) {
            if (it->reticket_count < 3) {
                ++it->reticket_count;
                it->next_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                g_last_result = std::format("re-ticketing {} ({})", base_name(path), outcome.detail);
                xlog::info("[demo-upload] {}", g_last_result);
            }
            else {
                g_dropped.insert(path);
                g_queued_paths.erase(it->path);
                g_queue.erase(it);
                g_last_result = std::format("gave up on {} ({})", base_name(path), outcome.detail);
                xlog::warn("[demo-upload] {}", g_last_result);
                rf::console::print("Demo upload failed: {} ({})", base_name(path), outcome.detail);
            }
        }
        break;
    }
}

// Shared queue bookkeeping: appends an already-verified path oldest-first and trims to
// fflink_demo_queue_max. Over cap the oldest QUEUED item is dropped from the queue only -
// never from disk. Callers must have already applied the queued/dropped skip.
void enqueue_checked(const std::string& path)
{
    QueueItem item;
    item.path = path;
    item.next_attempt = std::chrono::steady_clock::now();
    g_queue.push_back(std::move(item));
    g_queued_paths.insert(path);

    const size_t cap = static_cast<size_t>(std::max(1, server_fflink_demo_queue_max()));
    while (g_queue.size() > cap) {
        auto victim = g_queue.begin();
        if (g_busy) {
            // Never evict the item currently uploading; skip it if it is the oldest.
            while (victim != g_queue.end() && victim->path == g_in_flight_path) {
                ++victim;
            }
            if (victim == g_queue.end()) {
                break; // only the in-flight item remains
            }
        }
        xlog::info("[demo-upload] queue over cap; dropping oldest queued demo (left on disk): {}",
                   victim->path);
        g_queued_paths.erase(victim->path);
        g_queue.erase(victim);
    }
}

// Enqueues a single freshly-closed segment: verifies eligibility (header parse) on the main
// thread before the queue bookkeeping.
void enqueue_path(const std::string& path)
{
    if (g_queued_paths.count(path) || g_dropped.count(path)) {
        return;
    }
    if (!is_eligible(path, nullptr)) {
        return;
    }
    enqueue_checked(path);
}

void maybe_start_upload()
{
    if (g_busy) {
        return;
    }
    if (!server_fflink_demo_upload() || !afstats_server_enabled()) {
        return;
    }
    if (get_gssk().empty()) {
        return; // wait for the session exchange
    }
    const auto now = std::chrono::steady_clock::now();
    QueueItem* pick = nullptr;
    for (auto& item : g_queue) {
        if (item.next_attempt <= now) {
            pick = &item; // oldest due item
            break;
        }
    }
    if (!pick) {
        return;
    }
    g_busy = true;
    g_in_flight_path = pick->path;
    try {
        std::thread(upload_worker, pick->path, g_epoch).detach();
        rf::console::print("Uploading demo {} to FactionFiles...", base_name(g_in_flight_path));
    }
    catch (const std::exception& e) {
        xlog::warn("[demo-upload] failed to spawn upload worker: {}", e.what());
        g_busy = false;
        g_in_flight_path.clear();
    }
}

void reset_for_new_server_session()
{
    ++g_epoch; // any in-flight worker's result is now stale and will be ignored
    g_queue.clear();
    g_queued_paths.clear();
    g_dropped.clear();
    g_busy = false;
    g_in_flight_path.clear();
}

ConsoleCommand2 fflink_demo_status_cmd{
    "fflink_demo_status",
    []() {
        rf::console::print("FactionFiles demo upload: {}", server_fflink_demo_upload() ? "enabled" : "disabled");
        rf::console::print("  Queued: {}   In flight: {}", g_queue.size(), g_busy ? "yes" : "no");
        rf::console::print("  Uploaded: {}   Given up: {}", g_uploads_ok, g_dropped.size());
        rf::console::print("  Last result: {}", g_last_result);
    },
    "Show the FactionFiles demo upload queue status.",
};

} // namespace

void demo_upload_do_patch()
{
    fflink_demo_status_cmd.register_cmd();
}

void demo_upload_do_frame()
{
    const bool server = rf::is_multi && rf::is_server;
    if (!server) {
        if (g_server_up) {
            g_server_up = false;
            reset_for_new_server_session();
        }
        return;
    }
    g_server_up = true;

    if (!server_fflink_demo_upload() || !afstats_server_enabled()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_pump) {
        return;
    }
    g_next_pump = now + k_pump_interval;
    maybe_start_upload();
}

void demo_upload_on_segment_closed(const std::string& path)
{
    if (!server_fflink_demo_upload()) {
        return;
    }
    enqueue_path(path);
}

} // namespace fflink
