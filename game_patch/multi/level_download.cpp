#include <stdexcept>
#include <atomic>
#include <future>
#include <thread>
#include <fstream>
#include <format>
#include <windows.h>
#include <unzip.h>
#include <zlib.h>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <unordered_set>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <common/utils/os-utils.h>
#include <common/utils/string-utils.h>
#include "../rf/multi.h"
#include "../rf/file/file.h"
#include "../rf/file/packfile.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/ui.h"
#include "../rf/input.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/gameseq.h"
#include "../rf/misc.h"
#include "../misc/misc.h"
#include "../os/console.h"
#include "../hud/hud.h"
#include "multi.h"
#include "faction_files.h"
#include "../misc/alpine_settings.h"
#include "../misc/waypoints.h"

// Resolution of the blurred edge-fill bitmap (box-filter downscale of the map image)
constexpr int edge_fill_blur_size = 64;

struct RotationAutodlReport
{
    size_t unique_levels = 0;
    std::vector<std::string> missing_levels;
    std::optional<std::string> error;
};

static bool is_vpp_filename(const char* filename)
{
    return string_iends_with(filename, ".vpp");
}

static std::vector<std::string> unzip(const char* path, const char* output_dir,
    std::function<bool(const char*)> filename_filter)
{
    unzFile archive = unzOpen(path);
    if (!archive) {
#ifdef DEBUG
        xlog::error("unzOpen failed: {}", path);
#endif
        throw std::runtime_error{"cannot open zip file"};
    }

    unz_global_info global_info;
    int code = unzGetGlobalInfo(archive, &global_info);
    if (code != UNZ_OK) {
        xlog::error("unzGetGlobalInfo failed - error {}, path {}", code, path);
        throw std::runtime_error{"cannot open zip file"};
    }

    std::vector<std::string> extracted_files;
    char buf[4096];
    char file_name[MAX_PATH];
    unz_file_info file_info;
    for (unsigned long i = 0; i < global_info.number_entry; i++) {
        code = unzGetCurrentFileInfo(archive, &file_info, file_name, sizeof(file_name), nullptr, 0, nullptr, 0);
        if (code != UNZ_OK) {
            xlog::error("unzGetCurrentFileInfo failed - error {}, path {}", code, path);
            break;
        }

        if (filename_filter(file_name)) {
            xlog::trace("Unpacking {}", file_name);
            auto output_path = std::format("{}\\{}", output_dir, file_name);
            std::ofstream file(output_path, std::ios_base::out | std::ios_base::binary);
            if (!file) {
                xlog::error("Cannot open file: {}", output_path);
                break;
            }

            code = unzOpenCurrentFile(archive);
            if (code != UNZ_OK) {
                xlog::error("unzOpenCurrentFile failed - error {}, path {}", code, path);
                break;
            }

            while ((code = unzReadCurrentFile(archive, buf, sizeof(buf))) > 0) file.write(buf, code);

            if (code < 0) {
                xlog::error("unzReadCurrentFile failed - error {}, path {}", code, path);
                break;
            }

            file.close();
            unzCloseCurrentFile(archive);

            extracted_files.emplace_back(file_name);
        }

        if (i + 1 < global_info.number_entry) {
            code = unzGoToNextFile(archive);
            if (code != UNZ_OK) {
                xlog::error("unzGoToNextFile failed - error {}, path {}", code, path);
                break;
            }
        }
    }

    unzClose(archive);
    xlog::debug("Unzipped");
    return extracted_files;
}

static void load_packfiles(const std::vector<std::string>& packfiles)
{
    rf::vpackfile_set_loading_user_maps(true);
    for (const auto& filename : packfiles) {
        if (!rf::vpackfile_add(filename.c_str(), "user_maps\\multi\\")) {
            xlog::error("vpackfile_add failed - {}", filename);
        }
    }
    rf::vpackfile_set_loading_user_maps(false);
}

static bool gunzip_file(const char* gz_path, const char* output_path)
{
    gzFile gz = gzopen(gz_path, "rb");
    if (!gz) {
        xlog::error("gzopen failed: {}", gz_path);
        return false;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        xlog::error("Cannot open output file: {}", output_path);
        gzclose(gz);
        return false;
    }

    char buf[4096];
    int bytes_read;
    while ((bytes_read = gzread(gz, buf, sizeof(buf))) > 0) {
        out.write(buf, bytes_read);
        if (out.fail()) {
            xlog::error("Write failed during gunzip of: {}", gz_path);
            gzclose(gz);
            return false;
        }
    }

    if (bytes_read < 0) {
        int gz_err;
        const char* gz_msg = gzerror(gz, &gz_err);
        xlog::error("gzread failed for {}: {} (code {})", gz_path, gz_msg ? gz_msg : "unknown", gz_err);
        gzclose(gz);
        return false;
    }

    out.close();
    if (out.fail()) {
        xlog::error("Failed to close/flush output file during gunzip of: {}", gz_path);
        gzclose(gz);
        return false;
    }

    gzclose(gz);
    return true;
}

static bool try_download_and_extract_awp(const std::string& rfl_filename,
    const FactionFilesClient::AwpInfo& awp_info, int max_retries = 1,
    const std::atomic<bool>* abort_flag = nullptr)
{
    auto awp_name = std::string{get_filename_without_ext(rfl_filename.c_str())} + ".awp";
    auto waypoint_dir = get_waypoint_dir();
    if (!waypoint_dir) {
        xlog::error("Failed to get waypoint directory for AWP extraction of {}", awp_name);
        return false;
    }

    auto awp_output_path = std::format("{}\\{}", waypoint_dir.value(), awp_name);

    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        if (abort_flag && abort_flag->load()) {
            xlog::info("AWP download aborted for {}", awp_name);
            return false;
        }
        auto temp_filename = get_temp_path_name("AF_AWP_");
        try {
            xlog::info("AWP download attempt {}/{} for {} (rev {})",
                attempt, max_retries, awp_name, awp_info.revision);

            FactionFilesClient ff_client;
            ff_client.download_map(temp_filename.c_str(), awp_info.download_url,
                [abort_flag](unsigned, std::chrono::milliseconds) {
                    if (abort_flag && abort_flag->load()) {
                        return false;
                    }
                    return true;
                });

            // Gunzip to a temp file in the waypoint directory, then rename on success.
            // This avoids destroying an existing valid AWP on transient failure.
            auto awp_temp_path = awp_output_path + ".tmp";
            bool extracted = gunzip_file(temp_filename.c_str(), awp_temp_path.c_str());
            remove(temp_filename.c_str());

            if (extracted) {
                remove(awp_output_path.c_str());
                if (rename(awp_temp_path.c_str(), awp_output_path.c_str()) == 0) {
                    xlog::info("AWP downloaded and extracted: {}", awp_name);
                    return true;
                }
                xlog::error("Failed to rename AWP temp file to {}", awp_output_path);
            }

            remove(awp_temp_path.c_str());
            xlog::error("AWP gunzip failed for {}", awp_name);
        }
        catch (const std::exception& e) {
            remove(temp_filename.c_str());
            xlog::error("AWP download attempt {}/{} failed for {}: {}",
                attempt, max_retries, awp_name, e.what());
        }
    }

    xlog::error("All AWP download attempts failed for {}", awp_name);
    return false;
}

enum class AwpDownloadResult
{
    map_not_found = -2,
    not_needed = -1,
    failed = 0,
    success = 1,
};

// Background AWP download state for Flow 2 (installed maps)
static std::future<AwpDownloadResult> g_awp_download_future;
static std::atomic<bool> g_awp_download_abort{false};
static bool g_awp_download_active = false;
static bool g_awp_download_force = false;
static std::string g_awp_download_target_map;

static bool level_file_exists(const std::string& filename)
{
    rf::File file;
    return file.find(filename.c_str());
}

bool download_level_if_missing(std::string filename)
{
    if (filename.rfind('.') == std::string::npos) {
        filename += ".rfl";
    }

    if (level_file_exists(filename)) {
        return true;
    }

    rf::console::print("----> Level {} is not installed. Trying to download it from FactionFiles...\n", filename);

    FactionFilesClient ff_client;
    auto level_info = ff_client.find_map(filename.c_str());
    if (!level_info) {
        rf::console::print("Map {} was not found on FactionFiles\n", filename);
        return false;
    }

    auto temp_filename = get_temp_path_name("AF_Level_");
    try {
        rf::console::print("--> Starting level download: {}\n", filename);
        ff_client.download_map(temp_filename.c_str(), level_info->download_url,
            [](unsigned, std::chrono::milliseconds) { return true; });
        rf::console::print("--> Level download completed: {}\n", filename);

        auto output_dir = std::format("{}user_maps\\multi", rf::root_path);
        std::vector<std::string> packfiles = unzip(temp_filename.c_str(), output_dir.c_str(), is_vpp_filename);
        remove(temp_filename.c_str());

        if (packfiles.empty()) {
            xlog::error("--> No packfiles were found for level {}", filename);
            rf::console::print("\n");
            return false;
        }

        // Download AWP file if available (non-fatal)
        // Bot clients always download; normal clients require the setting; dedis never auto-download
        if (level_info->awp_info.has_value() && !rf::is_dedicated_server
            && (client_bot_launch_enabled() || g_alpine_game_config.autodl_download_awps)) {
            try {
                if (try_download_and_extract_awp(filename, level_info->awp_info.value())) {
                    rf::console::print("--> AWP waypoint file downloaded for {}\n", filename);
                }
                else {
                    rf::console::print("--> AWP waypoint file download failed for {}\n", filename);
                }
            }
            catch (const std::exception& e) {
                rf::console::print("--> AWP download failed for {}: {}\n", filename, e.what());
            }
        }

        rf::console::print("--> Installing downloaded level: {}\n", filename);
        load_packfiles(packfiles);
        rf::console::print("--> Level install completed: {}\n", filename);
        rf::console::print("\n");
        return level_file_exists(filename);
    }
    catch (const std::exception& e) {
        remove(temp_filename.c_str());
        xlog::error("--> Level download failed for {}: {}", filename, e.what());
        return false;
    }
}

enum class LevelDownloadState
{
    fetching_info,
    fetching_data,
    not_found,
    failed,
    extracting,
    finished,
};

class LevelDownloadWorker
{
public:
    struct SharedData
    {
        std::atomic<LevelDownloadState> state{LevelDownloadState::fetching_info};
        std::atomic<unsigned> bytes_received{0};
        std::atomic<float> bytes_per_sec{0};
        std::atomic<bool> abort_flag{false};
        std::optional<FactionFilesClient::LevelInfo> level_info;
        std::vector<unsigned char> image_data;
        std::vector<std::string> result_packfiles;
        std::atomic<bool> awp_downloading{false};
        std::atomic<bool> awp_downloaded{false};
        std::atomic<bool> awp_attempted{false};
        std::atomic<bool> work_done{false};
        std::string error;
    };

    LevelDownloadWorker(std::string level_filename, std::shared_ptr<SharedData> shared_data) :
        level_filename_{std::move(level_filename)},
        shared_data_{std::move(shared_data)}
    {}

    void operator()();

private:
    std::string level_filename_;
    std::shared_ptr<SharedData> shared_data_;

    void download_archive(const std::string& download_url, const char* temp_filename);
    static std::vector<std::string> extract_archive(const char* temp_filename);
};

void LevelDownloadWorker::download_archive(const std::string& download_url, const char* temp_filename)
{
    auto callback = [&](unsigned bytes_received, std::chrono::milliseconds duration) {
        if (shared_data_->abort_flag) {
            return false;
        }
        shared_data_->bytes_received = bytes_received;
        auto duration_ms = duration.count();
        if (duration_ms > 0) {
            shared_data_->bytes_per_sec = bytes_received * 1000.0f / duration_ms;
        }
        return true;
    };
    FactionFilesClient ff_client;
    ff_client.download_map(temp_filename, download_url, callback);
}

std::vector<std::string> LevelDownloadWorker::extract_archive(const char* temp_filename)
{
    auto output_dir = std::format("{}user_maps\\multi", rf::root_path);
    std::vector<std::string> packfiles;

    try {
        packfiles = unzip(temp_filename, output_dir.c_str(), is_vpp_filename);
    }
    catch (const std::exception& e) {
        xlog::error("Failed to extract archive '{}': {}", temp_filename, e.what());
    }
    catch (...) {
        xlog::error("Unknown error occurred while extracting archive '{}'", temp_filename);
    }

    if (packfiles.empty()) {
        xlog::error("No packfiles found in downloaded archive '{}'", temp_filename);
    }

    return packfiles;
}

void LevelDownloadWorker::operator()()
{
    try {
        xlog::trace("LevelDownloadWorker started");
        shared_data_->state = LevelDownloadState::fetching_info;
        FactionFilesClient ff_client;
        try {
            shared_data_->level_info = ff_client.find_map(level_filename_.c_str());
        }
        catch (const std::exception& e) {
            std::string msg = e.what();
            if (msg.find("404") != std::string::npos) {
                xlog::trace("Level not found on FactionFiles: {}", level_filename_);
                shared_data_->state = LevelDownloadState::not_found;
                shared_data_->work_done = true;
                return;
            }
            throw;
        }
        if (!shared_data_->level_info) {
            xlog::trace("Level not found: {}", level_filename_);
            shared_data_->state = LevelDownloadState::not_found;
            shared_data_->work_done = true;
            return;
        }
        xlog::trace("LevelDownloadWorker got level info");

        if (!shared_data_->abort_flag && !shared_data_->level_info->image_url.empty()) {
            try {
                FactionFilesClient img_client;
                shared_data_->image_data = img_client.fetch_image(shared_data_->level_info->image_url);
            }
            catch (const std::exception& e) {
                xlog::warn("Failed to fetch map image: {}", e.what());
            }
        }

        auto temp_filename = get_temp_path_name("AF_Level_");
        try {
            shared_data_->state = LevelDownloadState::fetching_data;
            download_archive(shared_data_->level_info.value().download_url, temp_filename.c_str());

            shared_data_->state = LevelDownloadState::extracting;
            shared_data_->result_packfiles = extract_archive(temp_filename.c_str());
            remove(temp_filename.c_str());

            // Download AWP file if available (non-fatal)
            // Bot clients always download; normal clients require the setting; dedis never auto-download
            if (!shared_data_->abort_flag && shared_data_->level_info->awp_info.has_value()
                && !rf::is_dedicated_server
                && (client_bot_launch_enabled() || g_alpine_game_config.autodl_download_awps)) {
                shared_data_->awp_attempted = true;
                shared_data_->awp_downloading = true;
                try {
                    shared_data_->awp_downloaded =
                        try_download_and_extract_awp(level_filename_, shared_data_->level_info->awp_info.value(),
                            1, &shared_data_->abort_flag);
                }
                catch (const std::exception& e) {
                    xlog::error("AWP download failed during level download: {}", e.what());
                }
                shared_data_->awp_downloading = false;
            }

            xlog::trace("LevelDownloadWorker finished");
            shared_data_->state = LevelDownloadState::finished;
        }
        catch (const std::exception& e) {
            remove(temp_filename.c_str());
            shared_data_->error = e.what();
            shared_data_->state = LevelDownloadState::failed;
            xlog::error("Level download failed: {}", e.what());
        }
    }
    catch (const std::exception& e) {
        shared_data_->error = e.what();
        shared_data_->state = LevelDownloadState::failed;
        xlog::error("Level download worker exception: {}", e.what());
    }
    catch (...) {
        shared_data_->error = "unknown error";
        shared_data_->state = LevelDownloadState::failed;
        xlog::error("Level download worker unknown exception");
    }
    shared_data_->work_done = true;
}

class LevelDownloadOperation
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void on_progress([[ maybe_unused ]] LevelDownloadOperation& operation) {}
        virtual void on_finish([[ maybe_unused ]] LevelDownloadOperation& operation, [[ maybe_unused ]] bool success) {}
    };

private:
    std::shared_ptr<LevelDownloadWorker::SharedData> shared_data_;
    std::thread thread_;
    std::unique_ptr<Listener> listener_;
    int image_bm_ = -1;
    int image_w_ = 0;
    int image_h_ = 0;
    int blur_bm_ = -1;
    int avg_fade_v_bm_ = -1; // vertical gradient: avg color (opaque) → transparent
    int avg_fade_h_bm_ = -1; // horizontal gradient: avg color (opaque) → transparent
    bool image_load_attempted_ = false;

public:
    LevelDownloadOperation(std::string level_filename, std::unique_ptr<Listener>&& listener) :
        listener_(std::move(listener))
    {
        shared_data_ = std::make_shared<LevelDownloadWorker::SharedData>();
        thread_ = std::thread(LevelDownloadWorker{std::move(level_filename), shared_data_});
    }

    ~LevelDownloadOperation()
    {
        shared_data_->abort_flag = true;
        if (thread_.joinable()) {
            // If we're at or past extraction, wait for it to finish to avoid corrupt files.
            // Otherwise detach to avoid hanging on network I/O.
            auto state = shared_data_->state.load();
            if (state >= LevelDownloadState::extracting) {
                thread_.join();
            }
            else {
                thread_.detach();
            }
        }
        // Release bitmaps if not shutting down (bitmap system may be torn down during exit)
        if (rf::gameseq_get_state() != rf::GS_QUITING) {
            if (image_bm_ != -1) {
                rf::bm::release(image_bm_);
            }
            if (blur_bm_ != -1) {
                rf::bm::release(blur_bm_);
            }
            if (avg_fade_v_bm_ != -1) {
                rf::bm::release(avg_fade_v_bm_);
            }
            if (avg_fade_h_bm_ != -1) {
                rf::bm::release(avg_fade_h_bm_);
            }
        }
    }

    [[nodiscard]] LevelDownloadState get_state() const
    {
        return shared_data_->state;
    }

    [[nodiscard]] bool has_level_info() const
    {
        // level_info is written before state leaves fetching_info, so check state first
        auto state = shared_data_->state.load(std::memory_order_acquire);
        if (state == LevelDownloadState::fetching_info) {
            return false;
        }
        return shared_data_->level_info.has_value();
    }

    [[nodiscard]] const FactionFilesClient::LevelInfo& get_level_info() const
    {
        // check state before calling this method
        return shared_data_->level_info.value();
    }

    [[nodiscard]] float get_bytes_per_sec() const
    {
        return shared_data_->bytes_per_sec;
    }

    [[nodiscard]] unsigned get_bytes_received() const
    {
        return shared_data_->bytes_received;
    }

    [[nodiscard]] bool is_awp_downloading() const
    {
        return shared_data_->awp_downloading;
    }

private:
    // Creates a game bitmap from RGBA pixel data, returns handle or -1
    static int create_game_bitmap(const unsigned char* rgba_pixels, int w, int h)
    {
        int bm = rf::bm::create(rf::bm::FORMAT_8888_ARGB, w, h);
        if (bm == -1) {
            return -1;
        }

        rf::gr::LockInfo lock_info;
        if (!rf::gr::lock(bm, 0, &lock_info, rf::gr::LOCK_WRITE_ONLY)) {
            rf::bm::release(bm);
            return -1;
        }

        // RGBA → ARGB swizzle
        for (int row = 0; row < h; ++row) {
            auto* src = rgba_pixels + row * w * 4;
            auto* dst = lock_info.data + row * lock_info.stride_in_bytes;
            for (int col = 0; col < w; ++col) {
                dst[0] = src[2]; // B
                dst[1] = src[1]; // G
                dst[2] = src[0]; // R
                dst[3] = src[3]; // A
                src += 4;
                dst += 4;
            }
        }

        rf::gr::unlock(&lock_info);
        return bm;
    }

    void create_image_bitmaps(const unsigned char* pixels, int w, int h)
    {
        // Full resolution bitmap
        image_bm_ = create_game_bitmap(pixels, w, h);
        if (image_bm_ == -1) {
            xlog::error("Failed to create bitmap for map image");
            return;
        }
        image_w_ = w;
        image_h_ = h;
        xlog::info("Created map image bitmap: {}x{}", w, h);

        // Tiny blurred version for edge fill bars (box filter downscale to 64×64)
        std::vector<unsigned char> blurred(edge_fill_blur_size * edge_fill_blur_size * 4);
        for (int by = 0; by < edge_fill_blur_size; ++by) {
            for (int bx = 0; bx < edge_fill_blur_size; ++bx) {
                int sx = bx * w / edge_fill_blur_size;
                int sy = by * h / edge_fill_blur_size;
                int sx_end = std::min((bx + 1) * w / edge_fill_blur_size, w);
                int sy_end = std::min((by + 1) * h / edge_fill_blur_size, h);
                int count = 0;
                int r = 0, g = 0, b = 0;
                for (int iy = sy; iy < sy_end; ++iy) {
                    for (int ix = sx; ix < sx_end; ++ix) {
                        const auto* p = pixels + (iy * w + ix) * 4;
                        r += p[0]; g += p[1]; b += p[2];
                        count++;
                    }
                }
                auto* dst = blurred.data() + (by * edge_fill_blur_size + bx) * 4;
                dst[0] = static_cast<unsigned char>(r / count);
                dst[1] = static_cast<unsigned char>(g / count);
                dst[2] = static_cast<unsigned char>(b / count);
                dst[3] = 255;
            }
        }
        blur_bm_ = create_game_bitmap(blurred.data(), edge_fill_blur_size, edge_fill_blur_size);

        // Compute average edge colors from the blurred image for the fade gradients.
        // Vertical fade (top/bottom bars): average the top and bottom edge rows.
        // Horizontal fade (left/right bars): average the left and right edge columns.
        constexpr int edge_rows = edge_fill_blur_size / 10; // ~10% from each edge
        auto avg_edge = [&](bool vertical) -> std::tuple<unsigned char, unsigned char, unsigned char> {
            int ar = 0, ag = 0, ab = 0, count = 0;
            for (int by = 0; by < edge_fill_blur_size; ++by) {
                for (int bx = 0; bx < edge_fill_blur_size; ++bx) {
                    bool is_edge = vertical
                        ? (by < edge_rows || by >= edge_fill_blur_size - edge_rows)
                        : (bx < edge_rows || bx >= edge_fill_blur_size - edge_rows);
                    if (!is_edge) continue;
                    const auto* p = blurred.data() + (by * edge_fill_blur_size + bx) * 4;
                    ar += p[0]; ag += p[1]; ab += p[2];
                    count++;
                }
            }
            return {
                static_cast<unsigned char>(ar / count),
                static_cast<unsigned char>(ag / count),
                static_cast<unsigned char>(ab / count),
            };
        };

        // Create square gradient bitmaps that fade from the edge average color to transparent.
        constexpr int fade_res = 32;
        auto make_avg_fade = [&](bool vertical) -> int {
            auto [er, eg, eb] = avg_edge(vertical);
            int bm = rf::bm::create(rf::bm::FORMAT_8888_ARGB, fade_res, fade_res);
            if (bm == -1) return -1;
            rf::gr::LockInfo li;
            if (!rf::gr::lock(bm, 0, &li, rf::gr::LOCK_WRITE_ONLY)) {
                rf::bm::release(bm);
                return -1;
            }
            for (int row = 0; row < fade_res; ++row) {
                for (int col = 0; col < fade_res; ++col) {
                    int grad_idx = vertical ? row : col;
                    float t = static_cast<float>(grad_idx) / static_cast<float>(fade_res - 1);
                    auto a = static_cast<unsigned char>(255 * (1.0f - t));
                    auto* dst = li.data + row * li.stride_in_bytes + col * 4;
                    dst[0] = eb; // B
                    dst[1] = eg; // G
                    dst[2] = er; // R
                    dst[3] = a;  // A
                }
            }
            rf::gr::unlock(&li);
            return bm;
        };
        avg_fade_v_bm_ = make_avg_fade(true);
        avg_fade_h_bm_ = make_avg_fade(false);
    }

public:
    // Returns bitmap handle, or -1 if not available yet. Must be called from main thread.
    int get_image_bitmap(int* out_w, int* out_h)
    {
        if (image_bm_ != -1) {
            *out_w = image_w_;
            *out_h = image_h_;
            return image_bm_;
        }

        if (image_load_attempted_) {
            return -1;
        }

        // Image data is written by the worker before state transitions to fetching_data.
        // Load state first (acquire) to establish happens-before with the worker's writes.
        auto state = shared_data_->state.load(std::memory_order_acquire);
        if (state < LevelDownloadState::fetching_data) {
            return -1; // Worker hasn't finished image fetch yet
        }
        image_load_attempted_ = true;
        if (shared_data_->image_data.empty()) {
            return -1; // No image available (fetch failed or no image_url)
        }
        auto image_data = std::move(shared_data_->image_data);

        int channels;
        unsigned char* pixels = stbi_load_from_memory(
            image_data.data(), static_cast<int>(image_data.size()),
            &image_w_, &image_h_, &channels, 4);
        if (!pixels) {
            xlog::warn("Failed to decode map image: {}", stbi_failure_reason());
            return -1;
        }

        create_image_bitmaps(pixels, image_w_, image_h_);
        stbi_image_free(pixels);

        if (image_bm_ == -1) {
            return -1;
        }
        *out_w = image_w_;
        *out_h = image_h_;
        return image_bm_;
    }

    [[nodiscard]] int get_blur_bitmap() const { return blur_bm_; }
    [[nodiscard]] int get_avg_fade_v_bitmap() const { return avg_fade_v_bm_; }
    [[nodiscard]] int get_avg_fade_h_bitmap() const { return avg_fade_h_bm_; }

    [[nodiscard]] bool in_progress() const
    {
        return thread_.joinable() && !shared_data_->work_done;
    }

    bool finished()
    {
        return shared_data_->work_done;
    }

private:
    std::vector<std::string> get_pending_packfiles()
    {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (!shared_data_->error.empty()) {
            xlog::error("Level download failed: {}", shared_data_->error);
            return {};
        }
        return std::move(shared_data_->result_packfiles);
    }

public:
    bool process()
    {
        if (in_progress() && listener_) {
            listener_->on_progress(*this);
        }
        if (!finished()) {
            return false;
        }
        xlog::trace("Background worker finished");
        std::vector<std::string> packfiles = get_pending_packfiles();
        if (packfiles.empty()) {
            if (listener_) {
                listener_->on_finish(*this, false);
            }
        }
        else {
            xlog::trace("Loading packfiles");
            load_packfiles(packfiles);

            // Report AWP download result on the main thread
            if (shared_data_->awp_attempted) {
                if (shared_data_->awp_downloaded) {
                    rf::console::print("AWP waypoint file downloaded for this level\n");
                }
                else {
                    rf::console::print("AWP waypoint file download failed for this level\n");
                }
            }

            if (listener_) {
                listener_->on_finish(*this, true);
            }
        }
        return true;
    }
};

class LevelDownloadManager
{
    std::optional<LevelDownloadOperation> operation_;
    std::future<RotationAutodlReport> rotation_autodl_future_;

public:
    void abort()
    {
        if (operation_) {
            xlog::info("Aborting level download");
            operation_.reset();
        }
    }

    LevelDownloadOperation& start(std::string level_filename, std::unique_ptr<LevelDownloadOperation::Listener>&& listener)
    {
        xlog::info("Starting level download: {}", level_filename);
        return operation_.emplace(std::move(level_filename), std::move(listener));
    }

    [[nodiscard]] const std::optional<LevelDownloadOperation>& get_operation() const
    {
        return operation_;
    }

    [[nodiscard]] std::optional<LevelDownloadOperation>& get_operation_mut()
    {
        return operation_;
    }

    void process()
    {
        if (operation_ && operation_.value().process()) {
            operation_.reset();
        }

        process_rotation_autodl();
    }

    static LevelDownloadManager& instance()
    {
        static LevelDownloadManager inst;
        return inst;
    }

    bool rotation_autodl_in_progress() const
    {
        using namespace std::chrono_literals;
        return rotation_autodl_future_.valid() && rotation_autodl_future_.wait_for(0ms) != std::future_status::ready;
    }

    void rotation_autodl_start(size_t levels_count, std::vector<std::string> unique_levels)
    {
        (void)levels_count;
        if (rotation_autodl_future_.valid()) {
            using namespace std::chrono_literals;
            if (rotation_autodl_future_.wait_for(0ms) == std::future_status::ready) {
                rotation_autodl_future_.get();
            }
        }

        rotation_autodl_future_ = std::async(std::launch::async,
            [levels_count, unique_levels = std::move(unique_levels)]() mutable -> RotationAutodlReport {
                RotationAutodlReport report;
                report.unique_levels = unique_levels.size();
                try {
                    FactionFilesClient ff_client;
                    constexpr size_t MAX_LEVELS_SINGLE_BATCH = 50;
                    std::vector<std::string> missing_levels;
                    std::unordered_set<std::string> missing_level_keys;
                    missing_levels.reserve(unique_levels.size());
                    missing_level_keys.reserve(unique_levels.size());

                    for (size_t start = 0; start < unique_levels.size(); start += MAX_LEVELS_SINGLE_BATCH) {
                        const size_t end = std::min(start + MAX_LEVELS_SINGLE_BATCH, unique_levels.size());
                        std::vector<std::string> batch(unique_levels.begin() + static_cast<std::ptrdiff_t>(start),
                                                       unique_levels.begin() + static_cast<std::ptrdiff_t>(end));
                        std::vector<bool> availability = ff_client.check_maps(batch);

                        for (size_t i = 0; i < batch.size(); ++i) {
                            if (i < availability.size() && availability[i]) {
                                continue;
                            }

                            const auto& filename = batch[i];
                            std::string key = string_to_lower(filename);
                            if (missing_level_keys.insert(key).second) {
                                missing_levels.push_back(filename);
                            }
                        }
                    }

                    report.missing_levels = std::move(missing_levels);
                    return report;
                }
                catch (const std::exception& ex) {
                    report.error = ex.what();
                    return report;
                }
            });
    }

private:
    void process_rotation_autodl()
    {
        if (!rotation_autodl_future_.valid()) {
            return;
        }

        using namespace std::chrono_literals;
        if (rotation_autodl_future_.wait_for(0ms) != std::future_status::ready) {
            return;
        }

        RotationAutodlReport report = rotation_autodl_future_.get();
        if (report.error) {
            rf::console::print("Failed to check levels on FactionFiles: {}\n", report.error.value());
            return;
        }

        if (report.missing_levels.empty()) {
            rf::console::print("{} unique levels on server rotation. All are available for autodownload from FactionFiles.",
                report.unique_levels);
            return;
        }

        rf::console::print("{} unique levels on server rotation. {} are NOT available for autodownload from FactionFiles:",
            report.unique_levels, report.missing_levels.size());
        for (const auto& missing : report.missing_levels) {
            rf::console::print("  {}", missing);
        }
    }
};

class ConsoleReportingDownloadListener : public LevelDownloadOperation::Listener
{
    std::chrono::system_clock::time_point last_progress_print_ = std::chrono::system_clock::now();

public:
    void on_progress(LevelDownloadOperation& operation) override
    {
        if (operation.get_state() == LevelDownloadState::fetching_data) {
            auto now = std::chrono::system_clock::now();
            if (now - last_progress_print_ >= std::chrono::seconds{2}) {
                rf::console::print("Download progress: {:.2f} MB / {:.2f} MB",
                    operation.get_bytes_received() / 1000000.0f,
                    operation.get_level_info().size_in_bytes / 1000000.0f);
                last_progress_print_ = now;
            }
        }
    }

    void on_finish(LevelDownloadOperation& operation, bool success) override
    {
        if (operation.get_state() == LevelDownloadState::not_found) {
            rf::console::print("Map was not found on FactionFiles\n");
        }
        else {
            rf::console::print("Level download {}", success ? "succeeded" : "failed");
        }
    }
};

class SetNewLevelStateDownloadListener : public LevelDownloadOperation::Listener
{
public:
    void on_finish(LevelDownloadOperation&, bool) override
    {
        xlog::trace("Changing game state to GS_NEW_LEVEL");
        rf::gameseq_set_state(rf::GS_NEW_LEVEL, false);
    }
};

void render_progress_bar(int x, int y, int w, int h, float progress)
{
    int border = 2;
    int inner_w = w - 2 * border;
    int inner_h = h - 2 * border;
    int progress_w = static_cast<int>(static_cast<float>(inner_w) * progress);
    if (progress_w > inner_w) {
        progress_w = inner_w;
    }

    int inner_x = x + border;
    int inner_y = y + border;

    rf::gr::set_color(0x40, 0x40, 0x40, 0xFF);
    rf::gr::rect(x, y, w, h);

    if (progress_w > 0) {
        rf::gr::set_color(0, 0x80, 0, 0xFF);
        rf::gr::rect(inner_x, inner_y, progress_w, inner_h);
    }

    if (w > progress_w) {
        rf::gr::set_color(0, 0, 0, 0xFF);
        rf::gr::rect(inner_x + progress_w, inner_y, inner_w - progress_w, inner_h);
    }
}

void multi_level_download_handle_input(int key)
{
    if (!key) {
        return;
    }
    if (rf::multi_chat_is_say_visible()) {
        rf::multi_chat_say_handle_key(key);
    }
    else if (key == rf::KEY_ESC) {
         rf::gameseq_push_state(rf::GS_MAIN_MENU, false, false);
    }
}

void multi_level_download_do_frame()
{
    rf::game_poll(multi_level_download_handle_input);

    int scr_w = rf::gr::screen_width();
    int scr_h = rf::gr::screen_height();

    auto& operation_opt = LevelDownloadManager::instance().get_operation_mut();

    // Background: use map image if available, otherwise default
    bool drew_bg = false;
    if (operation_opt) {
        auto& operation = operation_opt.value();
        int img_w, img_h;
        int img_bm = operation.get_image_bitmap(&img_w, &img_h);
        if (img_bm != -1) {
            rf::gr::set_color(255, 255, 255, 255);

            if (g_alpine_game_config.autodl_blur_background) {
                // Draw sharp image at correct aspect ratio, centered
                float img_aspect = static_cast<float>(img_w) / static_cast<float>(img_h);
                float scr_aspect = static_cast<float>(scr_w) / static_cast<float>(scr_h);
                int draw_w, draw_h;
                if (img_aspect > scr_aspect) {
                    draw_w = scr_w;
                    draw_h = static_cast<int>(scr_w / img_aspect);
                }
                else {
                    draw_h = scr_h;
                    draw_w = static_cast<int>(scr_h * img_aspect);
                }
                int draw_x = (scr_w - draw_w) / 2;
                int draw_y = (scr_h - draw_h) / 2;

                // Fill exposed bars using edge strips from the blurred image,
                // with a fade to average edge color at the screen edges
                int blur_bm = operation.get_blur_bitmap();
                constexpr int bs = edge_fill_blur_size;
                int edge_v = std::max(bs / 10, 1);
                int edge_h = std::max(bs / 10, 1);
                int fade_size = std::max(scr_w, scr_h) / 6;
                constexpr int fade_res = 32;

                if (draw_y > 0 && blur_bm != -1) {
                    // Top bar: stretch top edge strip of blurred image
                    rf::gr::bitmap_scaled(blur_bm, 0, 0, scr_w, draw_y,
                        0, 0, bs, edge_v, false, false, rf::gr::bitmap_clamp_mode);
                    // Bottom bar: stretch bottom edge strip of blurred image
                    rf::gr::bitmap_scaled(blur_bm, 0, draw_y + draw_h, scr_w, scr_h - draw_y - draw_h,
                        0, bs - edge_v, bs, edge_v, false, false, rf::gr::bitmap_clamp_mode);

                    // Fade to average color at screen edges
                    int avg_fade_v = operation.get_avg_fade_v_bitmap();
                    if (avg_fade_v != -1) {
                        rf::gr::set_color(255, 255, 255, 255);
                        int ft = std::min(fade_size, draw_y);
                        rf::gr::bitmap_scaled(avg_fade_v, 0, 0, scr_w, ft,
                            0, 0, fade_res, fade_res, false, false, rf::gr::bitmap_clamp_mode);
                        int fb = std::min(fade_size, scr_h - draw_y - draw_h);
                        rf::gr::bitmap_scaled(avg_fade_v, 0, scr_h - fb, scr_w, fb,
                            0, 0, fade_res, fade_res, false, true, rf::gr::bitmap_clamp_mode);
                    }
                }
                else if (draw_x > 0 && blur_bm != -1) {
                    // Left bar: stretch left edge strip of blurred image
                    rf::gr::bitmap_scaled(blur_bm, 0, 0, draw_x, scr_h,
                        0, 0, edge_h, bs, false, false, rf::gr::bitmap_clamp_mode);
                    // Right bar: stretch right edge strip of blurred image
                    rf::gr::bitmap_scaled(blur_bm, draw_x + draw_w, 0, scr_w - draw_x - draw_w, scr_h,
                        bs - edge_h, 0, edge_h, bs, false, false, rf::gr::bitmap_clamp_mode);

                    // Fade to average color at screen edges
                    int avg_fade_h = operation.get_avg_fade_h_bitmap();
                    if (avg_fade_h != -1) {
                        rf::gr::set_color(255, 255, 255, 255);
                        int fl = std::min(fade_size, draw_x);
                        // Left screen edge: opaque avg at x=0, transparent toward image
                        rf::gr::bitmap_scaled(avg_fade_h, 0, 0, fl, scr_h,
                            0, 0, fade_res, fade_res, false, false, rf::gr::bitmap_clamp_mode);
                        int fr = std::min(fade_size, scr_w - draw_x - draw_w);
                        // Right screen edge: opaque avg at right, transparent toward image
                        rf::gr::bitmap_scaled(avg_fade_h, scr_w - fr, 0, fr, scr_h,
                            0, 0, fade_res, fade_res, true, false, rf::gr::bitmap_clamp_mode);
                    }
                }

                rf::gr::set_color(255, 255, 255, 255);
                rf::gr::bitmap_scaled(img_bm, draw_x, draw_y, draw_w, draw_h, 0, 0, img_w, img_h);
            }
            else {
                // Simple stretch to fill
                rf::gr::bitmap_scaled(img_bm, 0, 0, scr_w, scr_h, 0, 0, img_w, img_h);
            }

            drew_bg = true;
        }
    }
    if (!drew_bg) {
        static int bg_bm = rf::bm::load("demo-gameover.tga", -1, false);
        int bg_bm_w, bg_bm_h;
        rf::bm::get_dimensions(bg_bm, &bg_bm_w, &bg_bm_h);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::bitmap_scaled(bg_bm, 0, 0, scr_w, scr_h, 0, 0, bg_bm_w, bg_bm_h);
    }

    rf::multi_hud_render_chat();

    rf::ControlConfig* ccp = &rf::local_player->settings.controls;
    bool just_pressed;
    if (rf::control_config_check_pressed(ccp, rf::CC_ACTION_CHAT, &just_pressed)) {
        rf::multi_chat_say_show(rf::CHAT_SAY_GLOBAL);
    }
    if (rf::multi_chat_is_say_visible()) {
        rf::multi_chat_say_render();
    }

    int medium_font = hud_get_default_font();
    int medium_font_h = rf::gr::get_font_height(medium_font);

    if (!operation_opt) {
        return;
    }

    auto& operation = operation_opt.value();
    auto state = operation.get_state();

    // Determine status text
    const char* status_text = nullptr;
    if (state == LevelDownloadState::fetching_info) {
        status_text = "Getting level info...";
    }
    else if (state == LevelDownloadState::extracting) {
        status_text = operation.is_awp_downloading()
            ? "Downloading waypoint file..."
            : "Extracting packfiles...";
    }
    else if (state == LevelDownloadState::fetching_data) {
        status_text = "Downloading from FactionFiles...";
    }
    else if (state == LevelDownloadState::not_found) {
        status_text = "Map not found on FactionFiles";
    }
    else if (state == LevelDownloadState::failed) {
        status_text = "Download failed";
    }
    else if (state == LevelDownloadState::finished) {
        status_text = "Loading...";
    }

    // Widget layout
    int padding = static_cast<int>(6 * rf::ui::scale_x);
    int margin = static_cast<int>(4 * rf::ui::scale_x);
    int bar_w = static_cast<int>(360 * rf::ui::scale_x);
    int bar_h = static_cast<int>(14 * rf::ui::scale_x);
    int inner_gap = static_cast<int>(4 * rf::ui::scale_x);
    int line_spacing = medium_font_h + static_cast<int>(2 * rf::ui::scale_x);

    // Widget height: status + bar + name + author + time remaining + padding
    int widget_w = bar_w + padding * 2;
    int widget_h = padding                      // top padding
        + medium_font_h + inner_gap             // status text + gap
        + bar_h + inner_gap                     // progress bar + gap
        + medium_font_h                         // level name
        + line_spacing                          // "by: author"
        + padding;                              // bottom padding
    int widget_x = scr_w - widget_w - margin;
    int widget_y = scr_h - widget_h - margin;

    // Semitransparent black background
    rf::gr::set_color(0, 0, 0, 0xB0);
    rf::gr::rect(widget_x, widget_y, widget_w, widget_h);

    int content_x = widget_x + padding;
    int content_y = widget_y + padding;

    // Status text
    rf::gr::set_color(255, 255, 255, 255);
    if (status_text) {
        rf::gr::string(content_x, content_y, status_text, medium_font);
    }
    content_y += medium_font_h + inner_gap;

    // Progress bar — stays at 100% through extraction and completion
    float progress = 0.0f;
    if (state == LevelDownloadState::fetching_data && operation.has_level_info()) {
        const FactionFilesClient::LevelInfo& info = operation.get_level_info();
        unsigned bytes_received = operation.get_bytes_received();
        progress = static_cast<float>(bytes_received) / static_cast<float>(info.size_in_bytes);
    }
    else if (state == LevelDownloadState::extracting || state == LevelDownloadState::finished) {
        progress = 1.0f;
    }
    render_progress_bar(content_x, content_y, bar_w, bar_h, progress);

    // Progress text on top of the bar
    if (state == LevelDownloadState::fetching_data && operation.has_level_info()) {
        const FactionFilesClient::LevelInfo& info = operation.get_level_info();
        unsigned bytes_received = operation.get_bytes_received();
        float bytes_per_sec = operation.get_bytes_per_sec();

        rf::gr::set_color(255, 255, 255, 255);
        auto progress_str = std::format("{:.2f} MB / {:.2f} MB ({:.2f} MB/s)",
            bytes_received / 1000.0f / 1000.0f,
            info.size_in_bytes / 1000.0f / 1000.0f,
            bytes_per_sec / 1000.0f / 1000.0f);
        int bar_center_x = content_x + bar_w / 2;
        int progress_text_y = content_y + (bar_h - medium_font_h) / 2;
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, bar_center_x, progress_text_y, progress_str.c_str(), medium_font);
    }
    else if ((state == LevelDownloadState::extracting || state == LevelDownloadState::finished)
        && operation.has_level_info()) {
        const FactionFilesClient::LevelInfo& info = operation.get_level_info();
        rf::gr::set_color(255, 255, 255, 255);
        auto progress_str = std::format("{:.2f} MB / {:.2f} MB",
            info.size_in_bytes / 1000.0f / 1000.0f,
            info.size_in_bytes / 1000.0f / 1000.0f);
        int bar_center_x = content_x + bar_w / 2;
        int progress_text_y = content_y + (bar_h - medium_font_h) / 2;
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, bar_center_x, progress_text_y, progress_str.c_str(), medium_font);
    }
    content_y += bar_h + inner_gap;

    // Level name and author
    rf::gr::set_color(255, 255, 255, 255);
    if (operation.has_level_info()) {
        const FactionFilesClient::LevelInfo& info = operation.get_level_info();
        rf::gr::string(content_x, content_y, info.name.c_str(), medium_font);
        content_y += line_spacing;

        auto author_str = std::format(" by {}", info.author);
        rf::gr::string(content_x, content_y, author_str.c_str(), medium_font);

        // Time remaining (right-aligned on same line as author)
        if (state == LevelDownloadState::fetching_data) {
            float bytes_per_sec = operation.get_bytes_per_sec();
            if (bytes_per_sec > 0) {
                unsigned bytes_received = operation.get_bytes_received();
                int remaining_size = (bytes_received < info.size_in_bytes)
                    ? static_cast<int>(info.size_in_bytes - bytes_received) : 0;
                int secs_left = static_cast<int>(remaining_size / bytes_per_sec);
                auto time_left_str = std::format("{} seconds remaining", secs_left);
                auto [tw, th] = rf::gr::get_string_size(time_left_str, medium_font);
                rf::gr::string(content_x + bar_w - tw, content_y, time_left_str.c_str(), medium_font);
            }
        }
    }

    // Scoreboard
    if (rf::control_config_check_pressed(ccp, rf::CC_ACTION_MP_STATS, nullptr)) {
        rf::scoreboard_render_internal(true);
    }
}

static bool next_level_exists()
{
    rf::File file;
    return file.find(rf::level.next_level_filename);
}

CallHook<void(rf::GameState, bool)> process_enter_limbo_packet_gameseq_set_next_state_hook{
    0x0047C091,
    [](rf::GameState state, bool force) {
        xlog::trace("Enter limbo");
        if (rf::gameseq_get_state() == rf::GS_MULTI_LEVEL_DOWNLOAD) {
            // Level changes before we finish downloading the previous one
            // Do not enter the limbo game state because it would crash the game if there is currently no level loaded
            // Instead stay in the level download state until we get the leave limbo packet and download the correct level
            LevelDownloadManager::instance().abort();
        }
        else {
            process_enter_limbo_packet_gameseq_set_next_state_hook.call_target(state, force);
        }
    },
};

CallHook<void(rf::GameState, bool)> process_leave_limbo_packet_gameseq_set_next_state_hook{
    0x0047C24F,
    [](rf::GameState state, bool force) {
        xlog::trace("Leave limbo - next level: {}", rf::level.next_level_filename);
        if (!next_level_exists()) {
            rf::gameseq_set_state(rf::GS_MULTI_LEVEL_DOWNLOAD, false);
            LevelDownloadManager::instance().start(rf::level.next_level_filename,
                std::make_unique<SetNewLevelStateDownloadListener>());
        }
        else {
            process_leave_limbo_packet_gameseq_set_next_state_hook.call_target(state, force);
        }
    },
};

CallHook<void(rf::GameState, bool)> game_new_game_gameseq_set_next_state_hook{
    0x00436959,
    [](rf::GameState state, bool force) {
        if (rf::is_multi && !rf::is_server && !next_level_exists()) {
            rf::gameseq_set_state(rf::GS_MULTI_LEVEL_DOWNLOAD, false);
            LevelDownloadManager::instance().start(rf::level.next_level_filename,
                std::make_unique<SetNewLevelStateDownloadListener>());
        }
        else {
            game_new_game_gameseq_set_next_state_hook.call_target(state, force);
        }
    },
};

CodeInjection join_failed_injection{
    0x0047C4EC,
    []() {
        if (client_bot_launch_enabled() && g_alpine_game_config.bot_quit_when_disconnected) {
            xlog::info("Bot failed to join server - auto-quitting (BotQuitWhenDisconnected=1)");
            rf::gameseq_set_state(rf::GS_QUITING, false);
            return;
        }
        set_jump_to_multi_server_list(true);
    },
};

static void do_download_level(std::string filename, bool force)
{
    if (filename.rfind('.') == std::string::npos) {
        filename += ".rfl";
    }
    if (LevelDownloadManager::instance().get_operation()) {
        xlog::error("Level download is already in progress!");
    }
    else {
        if (!force && rf::get_file_checksum(filename.c_str())) {
            xlog::error("Level already exists on disk! Use download_level_force to download anyway.");
            return;
        }
        LevelDownloadManager::instance().start(filename,
            std::make_unique<ConsoleReportingDownloadListener>());
    }
}

ConsoleCommand2 download_level_cmd{
    "download_level",
    [](std::string filename) {
        do_download_level(filename, false);
    },
    "Downloads level from FactionFiles.com if not already loaded",
    "download_level <rfl_name>",
};

ConsoleCommand2 download_level_force_cmd{
    "download_level_force",
    [](std::string filename) {
        do_download_level(filename, true);
    },
    "Force download of a level from FactionFiles.com, overwriting your local copy if one exists",
    "download_level_force <rfl_name>",
};

ConsoleCommand2 autodl_blur_background_cmd{
    "autodl_blur_background",
    []() {
        g_alpine_game_config.autodl_blur_background = !g_alpine_game_config.autodl_blur_background;
        rf::console::print("Autodownload blur background is {}",
            g_alpine_game_config.autodl_blur_background ? "enabled" : "disabled");
    },
    "Toggle blurred background on level download screen",
    "autodl_blur_background",
};

ConsoleCommand2 autodl_download_awps_cmd{
    "autodl_download_awps",
    []() {
        g_alpine_game_config.autodl_download_awps = !g_alpine_game_config.autodl_download_awps;
        rf::console::print("Autodownload AWP waypoint files is {}",
            g_alpine_game_config.autodl_download_awps ? "enabled" : "disabled");
    },
    "Toggle automatic AWP waypoint file downloading via autodl",
    "autodl_download_awps",
};

ConsoleCommand2 download_awp_force_cmd{
    "download_awp_force",
    [](std::string filename) {
        if (filename.rfind('.') == std::string::npos) {
            filename += ".rfl";
        }

        if (g_awp_download_active) {
            rf::console::print("AWP download already in progress\n");
            return;
        }

        rf::console::print("Querying autodl for AWP: {}\n", filename);
        start_awp_download_for_installed_map(filename, 1, true);
    },
    "Force download AWP waypoint file from autodl, ignoring revision and settings",
    "download_awp_force <rfl_name>",
};

void cancel_awp_download()
{
    if (g_awp_download_active) {
        g_awp_download_abort = true;
        if (g_awp_download_future.valid()) {
            g_awp_download_future.wait();
        }
        g_awp_download_active = false;
        g_awp_download_abort = false;
        g_awp_download_force = false;
        g_awp_download_target_map.clear();
    }
    waypoints_set_awp_download_pending(false);
}

bool start_awp_download_for_installed_map(const std::string& rfl_filename, int max_retries, bool force)
{
    if (g_awp_download_active) {
        xlog::warn("AWP download already in progress, skipping new request for {}", rfl_filename);
        return false;
    }

    // Revision check on main thread to avoid calling rf::File (VPP) from a background thread.
    // For force mode, use nullopt to skip the revision comparison entirely.
    std::optional<int> local_rev;
    if (!force) {
        local_rev = get_local_awp_revision(rfl_filename);
    }

    g_awp_download_active = true;
    g_awp_download_force = force;
    g_awp_download_abort = false;
    g_awp_download_target_map = rfl_filename;
    try {
        g_awp_download_future = std::async(std::launch::async,
            [rfl_filename, max_retries, local_rev]() -> AwpDownloadResult {
                try {
                    if (g_awp_download_abort) return AwpDownloadResult::not_needed;

                    FactionFilesClient ff_client;
                    auto level_info = ff_client.find_map(rfl_filename.c_str());
                    if (!level_info) {
                        xlog::info("Map {} not found on FactionFiles", rfl_filename);
                        return AwpDownloadResult::map_not_found;
                    }
                    if (!level_info->awp_info.has_value()) {
                        xlog::info("No AWP available via autodl for {}", rfl_filename);
                        return AwpDownloadResult::not_needed;
                    }

                    if (local_rev.has_value() && local_rev.value() >= level_info->awp_info->revision) {
                        xlog::info("Local AWP for {} is up to date (rev {} >= {})",
                            rfl_filename, local_rev.value(), level_info->awp_info->revision);
                        return AwpDownloadResult::not_needed;
                    }

                    return try_download_and_extract_awp(rfl_filename, level_info->awp_info.value(),
                        max_retries, &g_awp_download_abort)
                        ? AwpDownloadResult::success : AwpDownloadResult::failed;
                }
                catch (const std::exception& e) {
                    std::string msg = e.what();
                    if (msg.find("404") != std::string::npos) {
                        xlog::info("Map {} not found on FactionFiles", rfl_filename);
                        return AwpDownloadResult::map_not_found;
                    }
                    xlog::error("AWP download for installed map {} failed: {}", rfl_filename, e.what());
                    return AwpDownloadResult::failed;
                }
            });
    }
    catch (const std::exception& e) {
        xlog::error("Failed to start AWP download thread: {}", e.what());
        g_awp_download_active = false;
        g_awp_download_force = false;
        g_awp_download_target_map.clear();
        return false;
    }
    return true;
}

void poll_awp_download()
{
    if (!g_awp_download_active) {
        return;
    }

    if (g_awp_download_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    auto result = AwpDownloadResult::failed;
    try {
        result = g_awp_download_future.get();
    }
    catch (const std::exception& e) {
        xlog::error("AWP download future exception: {}", e.what());
    }

    g_awp_download_active = false;
    bool was_force = g_awp_download_force;
    g_awp_download_force = false;
    g_awp_download_abort = false;
    std::string target_map = std::move(g_awp_download_target_map);

    // Check if this result is for the current level (may have changed during download)
    bool is_current_level = (target_map == std::string{rf::level.filename.c_str()});

    if (waypoints_awp_download_pending()) {
        if (is_current_level) {
            waypoints_on_awp_download_resolved();
            if (result == AwpDownloadResult::success) {
                rf::console::print("AWP waypoint file downloaded and loaded\n");
            }
            else if (result == AwpDownloadResult::failed) {
                rf::console::print("AWP waypoint file download failed\n");
            }
        }
        else {
            // Stale result from a different map — just clear the pending flag
            // and let waypoints_level_init run normally for the current map
            waypoints_on_awp_download_resolved();
            xlog::info("AWP download result discarded (map changed from {} to {})",
                target_map, rf::level.filename.c_str());
        }
    }
    else {
        if (result == AwpDownloadResult::success) {
            rf::console::print("AWP waypoint file downloaded in background\n");
        }
        else if (result == AwpDownloadResult::failed) {
            rf::console::print("AWP waypoint file download failed\n");
        }
        else if (was_force) {
            if (result == AwpDownloadResult::map_not_found) {
                rf::console::print("Map was not found on FactionFiles\n");
            }
            else if (result == AwpDownloadResult::not_needed) {
                rf::console::print("No AWP available via autodl for this map\n");
            }
        }
    }
}

void level_download_do_patch()
{
    join_failed_injection.install();
    game_new_game_gameseq_set_next_state_hook.install();
    process_enter_limbo_packet_gameseq_set_next_state_hook.install();
    process_leave_limbo_packet_gameseq_set_next_state_hook.install();
}

void level_download_init()
{
    download_level_cmd.register_cmd();
    download_level_force_cmd.register_cmd();
    autodl_blur_background_cmd.register_cmd();
    autodl_download_awps_cmd.register_cmd();
    download_awp_force_cmd.register_cmd();
}

void multi_level_download_update()
{
    LevelDownloadManager::instance().process();
}

void multi_level_download_abort()
{
    LevelDownloadManager::instance().abort();
}

bool rotation_autodl_in_progress()
{
    return LevelDownloadManager::instance().rotation_autodl_in_progress();
}

void rotation_autodl_start(size_t levels_count, std::vector<std::string> unique_levels)
{
    LevelDownloadManager::instance().rotation_autodl_start(levels_count, std::move(unique_levels));
}
