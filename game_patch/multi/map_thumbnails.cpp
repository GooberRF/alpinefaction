#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <plutovg.h>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/cache_pack.h>
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
    constexpr int max_downloads = 3;
    constexpr int max_reads = 3;
    constexpr int max_bitmaps = 64;
    constexpr int jpeg_quality = 90;
    // Far above any thumbnail; keeps an entry written by another tool within decode_downscaled's int
    constexpr uint32_t max_cached_size = 8 * 1024 * 1024;

    struct Job
    {
        std::string level_filename;
        bool from_cache = false;
        unsigned generation = 0;
        std::vector<uint32_t> argb; // empty = no usable image
        int w = 0;
        int h = 0;
        bool stored = false;  // added to the disk cache
        bool dropped = false; // its cached image was unusable and left the disk cache
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
    // One decode at a time: a full-size 16-bit image needs a few hundred MB while it decodes
    std::mutex g_decode_mutex;
    // Bumped by map_thumbnails_release_bitmaps so a job finishing afterwards doesn't build a bitmap
    unsigned g_generation = 0;

    void decode_downscaled(Job& job, const std::vector<unsigned char>& encoded)
    {
        // The download and cache caps keep the size well within int
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

    // The disk cache, <rf_root>\alpine_cache\map_thumbs.cache: a cache pack of JPEGs named by lowercase
    // level filename. Levels with no image are only remembered for the session (Thumb::State::unavailable),
    // so FactionFiles is asked again after a restart.
    struct DiskCache
    {
        std::filesystem::path path; // empty = no disk cache; set before the first job starts
        // Held for any use of the index or the file, and through a save
        std::mutex mutex;
        uint64_t write_id = 0;
        std::unordered_map<std::string, cache_pack::Entry> index;
        std::unordered_map<std::string, std::vector<unsigned char>> added; // not saved yet
        std::vector<std::string> removed;                                   // to drop at the next save
    };
    DiskCache g_cache;
    bool g_cache_loaded = false;
    // The names the cache holds, so the main thread can pick a job's slot without the lock
    std::unordered_set<std::string> g_cached_names;
    std::atomic<bool> g_cache_dirty = false;
    std::atomic<bool> g_cache_saving = false;
    std::atomic<int> g_running_jobs = 0;
    // Nothing calls pump while the browser is closed, so the last job to finish starts the save
    std::atomic<bool> g_browser_closed = false;

    // Short and printable, as real level filenames are
    bool is_cacheable_name(const std::string& name)
    {
        return name.size() <= 255 && std::ranges::all_of(name, [](char c) {
            return c >= 0x20 && c <= 0x7E && c != '/' && c != '\\' && c != ':';
        });
    }

    // The caller holds g_cache.mutex, or no job has started yet
    void set_index(cache_pack::Index index)
    {
        g_cache.write_id = index.write_id;
        g_cache.index.clear();
        for (cache_pack::Entry& entry : index.entries) {
            std::string name = entry.name;
            g_cache.index.emplace(std::move(name), std::move(entry));
        }
    }

    void load_cache()
    {
        g_cache_loaded = true;
        try {
            const auto dir = std::filesystem::path{rf::root_path} / "alpine_cache";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                xlog::warn("[MapThumb] failed to create {}: {}", dir.string(), ec.message());
                return;
            }
            g_cache.path = dir / "map_thumbs.cache";
            cache_pack::remove_stale_temps(g_cache.path);
            if (auto index = cache_pack::read_index(g_cache.path)) {
                for (const cache_pack::Entry& entry : index->entries) {
                    g_cached_names.insert(entry.name);
                }
                set_index(std::move(*index));
            }
            else if (std::filesystem::exists(g_cache.path, ec)) {
                xlog::warn("[MapThumb] {} is not a valid cache, starting a new one", g_cache.path.string());
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[MapThumb] failed to load the cache: {}", e.what());
        }
    }

    void start_save();

    void save_cache()
    {
        try {
            const std::lock_guard lock{g_cache.mutex};
            std::vector<cache_pack::NewEntry> added;
            for (const auto& [name, data] : g_cache.added) {
                added.push_back({name, 0, data});
            }
            if (auto index = cache_pack::update(g_cache.path, g_cache.removed, added)) {
                set_index(std::move(*index));
                g_cache.added.clear();
                g_cache.removed.clear();
            }
            else {
                // Kept in memory, for the save that follows the next change
                xlog::warn("[MapThumb] failed to save {}", g_cache.path.string());
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[MapThumb] failed to save the cache: {}", e.what());
        }
        g_cache_saving = false;
        // A job that finished during the save, after the browser closed, couldn't start one
        if (g_browser_closed && g_running_jobs == 0) {
            start_save();
        }
    }

    void start_save()
    {
        bool saving = false;
        if (!g_cache_saving.compare_exchange_strong(saving, true)) {
            return;
        }
        if (!g_cache_dirty.exchange(false)) {
            g_cache_saving = false;
            return;
        }
        try {
            std::thread{save_cache}.detach();
        }
        catch (const std::exception& e) {
            // Like a failed save: kept in memory for the next change
            xlog::warn("[MapThumb] failed to start saving the cache: {}", e.what());
            g_cache_saving = false;
        }
    }

    // PlutoVG's surface is ARGB32 like job.argb (opaque, so premultiplied is the same)
    std::vector<unsigned char> encode_jpeg(const Job& job)
    {
        struct Output
        {
            std::vector<unsigned char> bytes;
            bool failed = false;
        } output;
        // The writer converts the pixels in place while it works
        std::vector<uint32_t> pixels = job.argb;
        plutovg_surface_t* surface = plutovg_surface_create_for_data(
            reinterpret_cast<unsigned char*>(pixels.data()), job.w, job.h, job.w * 4);
        const bool written = plutovg_surface_write_to_jpg_stream(
            surface,
            [](void* closure, void* data, int size) {
                auto& out = *static_cast<Output*>(closure);
                const auto* bytes = static_cast<const unsigned char*>(data);
                // An exception can't cross the C writer
                try {
                    out.bytes.insert(out.bytes.end(), bytes, bytes + size);
                }
                catch (const std::bad_alloc&) {
                    out.failed = true;
                }
            },
            &output, jpeg_quality);
        plutovg_surface_destroy(surface);
        if (!written || output.failed) {
            return {};
        }
        return std::move(output.bytes);
    }

    void read_cached(Job& job)
    {
        const std::string& name = job.level_filename;
        std::vector<unsigned char> encoded;
        // The file couldn't be read just now (another game instance saving it, a scanner holding it),
        // so the entry stays for next time. A corrupt entry reads the same way, and stays until a
        // download replaces it
        bool unreadable = false;
        {
            const std::lock_guard lock{g_cache.mutex};
            // False only when reading the file fails or the entry is corrupt
            const auto read = [&] {
                const auto entry = g_cache.index.find(name);
                return entry == g_cache.index.end() || entry->second.size > max_cached_size
                    || cache_pack::read_data(g_cache.path, g_cache.write_id, entry->second, encoded);
            };
            if (const auto it = g_cache.added.find(name); it != g_cache.added.end()) {
                encoded = it->second;
            }
            else if (!read()) {
                // Another game instance may have saved the cache since the index was read
                auto index = cache_pack::read_index(g_cache.path);
                if (index) {
                    set_index(std::move(*index));
                }
                unreadable = !index || !read();
            }
        }
        if (!encoded.empty()) {
            decode_downscaled(job, encoded);
        }
        if (job.argb.empty() && !unreadable) {
            const std::lock_guard lock{g_cache.mutex};
            if (g_cache.index.erase(name) != 0) {
                g_cache.removed.push_back(name);
                g_cache_dirty = true;
            }
            job.dropped = true;
        }
    }

    void download(Job& job)
    {
        const std::string& name = job.level_filename;
        try {
            FactionFilesClient client;
            const auto info = client.find_map(name.c_str());
            if (info && !info->image_url.empty()) {
                const std::vector<unsigned char> encoded = client.fetch_image(info->image_url);
                if (!encoded.empty()) {
                    decode_downscaled(job, encoded);
                }
            }
        }
        catch (const std::exception& e) {
            // find_map throws on a 404 for levels FactionFiles doesn't have
            xlog::debug("[MapThumb] no image for {}: {}", name, e.what());
        }
        if (job.argb.empty() || g_cache.path.empty() || !is_cacheable_name(name)) {
            return;
        }
        std::vector<unsigned char> jpeg = encode_jpeg(job);
        if (!jpeg.empty()) {
            const std::lock_guard lock{g_cache.mutex};
            g_cache.added[name] = std::move(jpeg);
            g_cache_dirty = true;
            job.stored = true;
        }
    }

    void run_job(const std::shared_ptr<Job>& job)
    {
        try {
            if (job->from_cache) {
                read_cached(*job);
            }
            // A cached image that can't be read or decoded is downloaded again
            if (job->argb.empty()) {
                download(*job);
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[MapThumb] failed to load image for {}: {}", job->level_filename, e.what());
        }
        job->done.store(true, std::memory_order_release);
        if (--g_running_jobs == 0 && g_browser_closed) {
            start_save();
        }
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

    void collect_finished_jobs()
    {
        for (auto it = g_jobs.begin(); it != g_jobs.end();) {
            const Job& job = **it;
            if (!job.done.load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            if (job.dropped) {
                g_cached_names.erase(job.level_filename);
            }
            if (job.stored) {
                g_cached_names.insert(job.level_filename);
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
    }

    // Once nothing is in flight, so a browse rewrites the file a few times at most
    void save_cache_if_idle()
    {
        if (g_jobs.empty() && g_queue.empty()) {
            start_save();
        }
    }

    void start_jobs()
    {
        int reads = 0;
        int downloads = 0;
        for (const auto& job : g_jobs) {
            ++(job->from_cache ? reads : downloads);
        }
        for (auto it = g_queue.begin(); it != g_queue.end() && (reads < max_reads || downloads < max_downloads);) {
            Thumb& thumb = g_thumbs[*it];
            if (thumb.state != Thumb::State::queued) {
                it = g_queue.erase(it);
                continue;
            }
            // Scrolled out of view before its turn; it is queued again if it comes back
            if (rf::frame_count - thumb.last_used_frame > 1) {
                thumb.state = Thumb::State::idle;
                it = g_queue.erase(it);
                continue;
            }
            // Cached levels have their own slots, so they don't wait behind downloads
            const bool from_cache = g_cached_names.contains(*it);
            int& running = from_cache ? reads : downloads;
            if (running >= (from_cache ? max_reads : max_downloads)) {
                ++it;
                continue;
            }
            ++running;
            auto job = std::make_shared<Job>();
            job->level_filename = *it;
            job->from_cache = from_cache;
            job->generation = g_generation;
            it = g_queue.erase(it);
            // Detached so a slow request never blocks the game; the job is shared with the thread
            ++g_running_jobs;
            try {
                std::thread{run_job, job}.detach();
            }
            catch (const std::exception& e) {
                xlog::warn("[MapThumb] failed to start loading {}: {}", job->level_filename, e.what());
                --g_running_jobs;
                thumb.state = Thumb::State::unavailable;
                continue;
            }
            thumb.state = Thumb::State::loading;
            g_jobs.push_back(std::move(job));
        }
    }

    void pump()
    {
        if (!g_cache_loaded) {
            load_cache();
        }
        collect_finished_jobs();
        evict_bitmaps();
        start_jobs();
        save_cache_if_idle();
    }
}

int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h)
{
    if (level_filename.empty()) {
        return -1;
    }
    g_browser_closed = false;
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
    g_browser_closed = true;
    collect_finished_jobs();
    save_cache_if_idle();
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
