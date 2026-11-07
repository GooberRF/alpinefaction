#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <plutovg.h>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "map_thumbnails.h"
#include "faction_files.h"
#include "../bmpman/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"

namespace
{
    // Bounding box of the stored preview: large enough for the demo browser's preview pane,
    // small enough that a screenful of levels stays cheap.
    constexpr int max_thumb_w = 480;
    constexpr int max_thumb_h = 270;
    // Bigger images are refused before decoding: a thumbnail doesn't need them, and a 32-bit
    // process can't spare the memory
    constexpr int max_source_dim = 4096;
    constexpr int max_jobs = 3;
    constexpr int max_bitmaps = 64;

    struct Job
    {
        std::string level_filename;
        std::string cache_dir; // empty = no disk cache
        unsigned generation = 0;
        std::vector<uint32_t> argb; // empty = no usable image
        int w = 0;
        int h = 0;
        std::atomic<bool> done = false;
    };

    struct Thumb
    {
        enum class State
        {
            idle, // never requested, or its bitmap was released
            queued,
            loading,
            ready,
            unavailable,
        };
        State state = State::idle;
        int bm = -1;
        int w = 0;
        int h = 0;
        int last_used_frame = 0;
    };

    // Keyed by lowercase level filename
    std::unordered_map<std::string, Thumb> g_thumbs;
    std::deque<std::string> g_queue;
    std::vector<std::shared_ptr<Job>> g_jobs;
    int g_num_bitmaps = 0;
    std::optional<std::string> g_cache_dir; // resolved on first use; "" = unavailable
    // One decode at a time: a full-size 16-bit image needs a few hundred MB while it decodes
    std::mutex g_decode_mutex;
    // Bumped by map_thumbnails_release_bitmaps so a job finishing afterwards doesn't build a bitmap
    unsigned g_generation = 0;

    void decode_downscaled(Job& job, const std::vector<unsigned char>& encoded)
    {
        // The download and cache file caps keep the size well within int
        const int encoded_len = static_cast<int>(encoded.size());
        int src_w = 0, src_h = 0, channels = 0;
        if (!stbi_info_from_memory(encoded.data(), encoded_len, &src_w, &src_h, &channels)) {
            xlog::warn("[MapThumb] failed to decode image for {}: {}", job.level_filename, stbi_failure_reason());
            return;
        }
        if (src_w <= 0 || src_h <= 0 || src_w > max_source_dim || src_h > max_source_dim) {
            xlog::warn("[MapThumb] image for {} rejected ({}x{} exceeds {} px)", job.level_filename, src_w, src_h,
                max_source_dim);
            return;
        }
        const std::lock_guard lock{g_decode_mutex};
        unsigned char* pixels = stbi_load_from_memory(encoded.data(), encoded_len, &src_w, &src_h, &channels, 4);
        if (!pixels) {
            xlog::warn("[MapThumb] failed to decode image for {}: {}", job.level_filename, stbi_failure_reason());
            return;
        }

        const float scale = std::min({1.0f, static_cast<float>(max_thumb_w) / src_w,
            static_cast<float>(max_thumb_h) / src_h});
        job.w = std::max(1, static_cast<int>(src_w * scale));
        job.h = std::max(1, static_cast<int>(src_h * scale));
        job.argb.resize(static_cast<size_t>(job.w) * job.h);

        const auto sw = static_cast<size_t>(src_w);
        const auto sh = static_cast<size_t>(src_h);
        for (size_t y = 0; y < static_cast<size_t>(job.h); ++y) {
            const size_t sy0 = y * sh / job.h;
            const size_t sy1 = std::max(sy0 + 1, (y + 1) * sh / job.h);
            for (size_t x = 0; x < static_cast<size_t>(job.w); ++x) {
                const size_t sx0 = x * sw / job.w;
                const size_t sx1 = std::max(sx0 + 1, (x + 1) * sw / job.w);
                unsigned r = 0, g = 0, b = 0;
                for (size_t sy = sy0; sy < sy1; ++sy) {
                    for (size_t sx = sx0; sx < sx1; ++sx) {
                        const unsigned char* p = pixels + (sy * sw + sx) * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                    }
                }
                const unsigned n = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
                // Opaque, as the JPEG in the disk cache will be
                job.argb[y * job.w + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
        stbi_image_free(pixels);
    }

    // Disk cache of the thumbnails as JPEGs, one per level named after its file (dm01.rfl ->
    // dm01.jpg). A level FactionFiles has no image for gets an empty dm01.missing instead, and is
    // asked for again once that is a day old.
    enum class CacheLookup
    {
        miss,
        image,   // out holds the cached image
        missing, // FactionFiles recently had no image for the level
    };

    constexpr auto missing_recheck_age = std::chrono::days{1};
    constexpr int jpeg_quality = 90;
    // Far above any thumbnail; keeps a file swapped in by hand within decode_downscaled's int
    constexpr std::uintmax_t max_cached_size = 8 * 1024 * 1024;

    // The level filename comes from the demo file, so only a plain file name gets a cache file
    std::optional<std::filesystem::path> cache_file(const std::string& dir, const std::string& key, const char* ext)
    {
        if (dir.empty() || key.empty() || key.size() > 128 || key.front() == '.' || key.front() == ' '
            || key.back() == '.' || key.back() == ' ') {
            return std::nullopt;
        }
        for (const char c : key) {
            // '~' too: a short-name alias like verylo~1 would reach another level's files
            if (c < 0x20 || c > 0x7E || std::strchr("<>:\"/\\|?*~", c)) {
                return std::nullopt;
            }
        }
        // Windows opens a device for these names whatever the extension
        std::string_view base = std::string_view{key}.substr(0, key.find('.'));
        base = base.substr(0, base.find_last_not_of(' ') + 1);
        const bool numbered_device = base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt"))
            && base[3] >= '0' && base[3] <= '9';
        if (numbered_device || base == "con" || base == "prn" || base == "aux" || base == "nul") {
            return std::nullopt;
        }
        const std::string_view name = key.ends_with(".rfl") ? std::string_view{key}.substr(0, key.size() - 4) : key;
        return std::filesystem::path{dir} / (std::string{name} + ext);
    }

    CacheLookup cache_get(const std::string& dir, const std::string& key, std::vector<unsigned char>& out)
    {
        const auto image = cache_file(dir, key, ".jpg");
        if (!image) {
            return CacheLookup::miss;
        }
        std::error_code ec;
        const auto marked = std::filesystem::last_write_time(*cache_file(dir, key, ".missing"), ec);
        if (!ec) {
            // A marker dated in the future (a clock that was off) is due too
            const auto age = std::filesystem::file_time_type::clock::now() - marked;
            return age >= decltype(age)::zero() && age < missing_recheck_age ? CacheLookup::missing : CacheLookup::miss;
        }
        const std::uintmax_t size = std::filesystem::file_size(*image, ec);
        if (ec || size == 0 || size > max_cached_size) {
            return CacheLookup::miss;
        }
        std::ifstream file{*image, std::ios::binary};
        out.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
            out.clear();
            return CacheLookup::miss;
        }
        return CacheLookup::image;
    }

    void cache_put_image(const std::string& dir, const std::string& key, const Job& job)
    {
        const auto image = cache_file(dir, key, ".jpg");
        if (!image) {
            return;
        }
        // Written beside the file and renamed over it, so an interrupted write never leaves a
        // broken image; the thread id keeps another game instance's write apart
        std::filesystem::path tmp = *image;
        tmp += std::format(".{}.tmp", GetCurrentThreadId());
        const std::string tmp_name = tmp.string();
        // PlutoVG's surface is ARGB32 like job.argb (opaque, so premultiplied is the same), and
        // the writer converts the pixels in place while it works
        std::vector<uint32_t> pixels = job.argb;
        plutovg_surface_t* surface = plutovg_surface_create_for_data(
            reinterpret_cast<unsigned char*>(pixels.data()), job.w, job.h, job.w * 4);
        const bool written = plutovg_surface_write_to_jpg(surface, tmp_name.c_str(), jpeg_quality);
        plutovg_surface_destroy(surface);
        std::error_code ec;
        if (written) {
            std::filesystem::rename(tmp, *image, ec);
        }
        if (!written || ec) {
            xlog::warn("[MapThumb] failed to write {}", image->string());
            std::filesystem::remove(tmp, ec);
            return;
        }
        std::filesystem::remove(*cache_file(dir, key, ".missing"), ec);
    }

    void cache_put_missing(const std::string& dir, const std::string& key)
    {
        const auto marker = cache_file(dir, key, ".missing");
        if (!marker) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove(*cache_file(dir, key, ".jpg"), ec);
        {
            std::ofstream file{*marker, std::ios::binary};
        }
        // Reopening an empty marker doesn't always move its time
        std::filesystem::last_write_time(*marker, std::filesystem::file_time_type::clock::now(), ec);
    }

    void cache_forget(const std::string& dir, const std::string& key)
    {
        if (const auto image = cache_file(dir, key, ".jpg")) {
            std::error_code ec;
            std::filesystem::remove(*image, ec);
        }
    }

    const std::string& cache_dir()
    {
        if (!g_cache_dir) {
            const auto dir = std::filesystem::path{rf::root_path} / "alpine_cache" / "map_thumbnails";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                xlog::warn("[MapThumb] failed to create {}: {}", dir.string(), ec.message());
            }
            g_cache_dir = ec ? std::string{} : dir.string();
            // Left by a session that quit mid-write
            for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
                if (it->path().extension() == ".tmp") {
                    std::error_code remove_ec;
                    std::filesystem::remove(it->path(), remove_ec);
                }
            }
        }
        return *g_cache_dir;
    }

    void run_job(const std::shared_ptr<Job>& job)
    {
        const std::string& dir = job->cache_dir;
        const std::string& key = job->level_filename;
        try {
            std::vector<unsigned char> encoded;
            const CacheLookup cached = cache_get(dir, key, encoded);
            bool downloaded = false;
            if (cached == CacheLookup::miss) {
                try {
                    FactionFilesClient client;
                    const auto info = client.find_map(key.c_str());
                    if (info && !info->image_url.empty()) {
                        encoded = client.fetch_image(info->image_url);
                        downloaded = true;
                    }
                    else {
                        cache_put_missing(dir, key);
                    }
                }
                catch (const std::exception& e) {
                    // find_map throws on a 404 for levels FactionFiles doesn't have; any other
                    // failure is left uncached so the next session tries again
                    if (std::string_view{e.what()}.find("404") != std::string_view::npos) {
                        cache_put_missing(dir, key);
                    }
                    xlog::debug("[MapThumb] no image for {}: {}", key, e.what());
                }
            }
            if (!encoded.empty()) {
                decode_downscaled(*job, encoded);
            }
            if (downloaded) {
                // A refused or undecodable image is rechecked daily instead of downloaded every session
                if (job->argb.empty()) {
                    cache_put_missing(dir, key);
                }
                else {
                    cache_put_image(dir, key, *job);
                }
            }
            else if (cached == CacheLookup::image && job->argb.empty()) {
                // Undecodable cached image: drop it so the next session downloads it again
                cache_forget(dir, key);
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[MapThumb] failed to load image for {}: {}", key, e.what());
        }
        job->done.store(true, std::memory_order_release);
    }

    void release_bitmap(Thumb& thumb)
    {
        if (thumb.bm != -1) {
            // bm::release does not evict the D3D11 slot-keyed texture cache
            rf::gr::mark_texture_dirty(thumb.bm);
            rf::bm::release(thumb.bm);
            thumb.bm = -1;
            --g_num_bitmaps;
        }
    }

    void evict_bitmaps()
    {
        while (g_num_bitmaps > max_bitmaps) {
            Thumb* oldest = nullptr;
            for (auto& [key, thumb] : g_thumbs) {
                // Drawn in the last frame: still on screen, so releasing it would only reload it
                if (thumb.bm == -1 || rf::frame_count - thumb.last_used_frame <= 1) {
                    continue;
                }
                if (!oldest
                    || rf::frame_count - thumb.last_used_frame > rf::frame_count - oldest->last_used_frame) {
                    oldest = &thumb;
                }
            }
            if (!oldest) {
                return;
            }
            release_bitmap(*oldest);
            oldest->state = Thumb::State::idle;
        }
    }

    void pump()
    {
        for (auto it = g_jobs.begin(); it != g_jobs.end();) {
            const Job& job = **it;
            if (!job.done.load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            Thumb& thumb = g_thumbs[job.level_filename];
            if (job.argb.empty()) {
                thumb.state = Thumb::State::unavailable;
            }
            else if (job.generation != g_generation) {
                thumb.state = Thumb::State::idle;
            }
            else {
                thumb.bm = bm_create_procedural(job.w, job.h,
                    [&](int x, int y) { return job.argb[static_cast<size_t>(y) * job.w + x]; });
                thumb.w = job.w;
                thumb.h = job.h;
                thumb.state = thumb.bm != -1 ? Thumb::State::ready : Thumb::State::unavailable;
                if (thumb.bm != -1) {
                    ++g_num_bitmaps;
                }
            }
            it = g_jobs.erase(it);
        }
        evict_bitmaps();

        while (static_cast<int>(g_jobs.size()) < max_jobs && !g_queue.empty()) {
            const std::string key = std::move(g_queue.front());
            g_queue.pop_front();
            Thumb& thumb = g_thumbs[key];
            if (thumb.state != Thumb::State::queued) {
                continue;
            }
            // Scrolled out of view before its turn; it is queued again if it comes back
            if (rf::frame_count - thumb.last_used_frame > 1) {
                thumb.state = Thumb::State::idle;
                continue;
            }
            thumb.state = Thumb::State::loading;
            auto job = std::make_shared<Job>();
            job->level_filename = key;
            job->cache_dir = cache_dir();
            job->generation = g_generation;
            // Detached so a slow request never blocks the game; the job is shared with the thread
            std::thread{run_job, job}.detach();
            g_jobs.push_back(std::move(job));
        }
    }
}

int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h)
{
    if (level_filename.empty()) {
        return -1;
    }
    std::string key = string_to_lower(level_filename);
    Thumb& thumb = g_thumbs[key];
    thumb.last_used_frame = rf::frame_count;
    if (thumb.state == Thumb::State::idle) {
        thumb.state = Thumb::State::queued;
        g_queue.push_back(std::move(key));
    }
    pump();
    if (thumb.state != Thumb::State::ready) {
        return -1;
    }
    *out_w = thumb.w;
    *out_h = thumb.h;
    return thumb.bm;
}

void map_thumbnails_release_bitmaps()
{
    ++g_generation;
    g_queue.clear();
    // The bitmap system may already be torn down while quitting
    const bool can_release = rf::gameseq_get_state() != rf::GS_QUIT_GAME;
    for (auto& [key, thumb] : g_thumbs) {
        if (can_release) {
            release_bitmap(thumb);
        }
        if (thumb.state == Thumb::State::ready || thumb.state == Thumb::State::queued) {
            thumb.state = Thumb::State::idle;
        }
    }
}
