#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <stb_image.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "map_thumbnails.h"
#include "faction_files.h"
#include "../bmpman/bmpman.h"
#include "../misc/misc.h"
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
    constexpr uint64_t max_cache_size = 512ull * 1024 * 1024;

    struct Job
    {
        std::string level_filename;
        std::string cache_path; // empty = no disk cache
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
    std::optional<std::string> g_cache_path; // resolved on first use; "" = unavailable
    // One decode at a time: a full-size 16-bit image needs a few hundred MB while it decodes
    std::mutex g_decode_mutex;
    // Bumped by map_thumbnails_release_bitmaps so a job finishing afterwards doesn't build a bitmap
    unsigned g_generation = 0;

    void decode_downscaled(Job& job, const std::vector<unsigned char>& encoded)
    {
        // The download cap keeps the size well within int
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
                unsigned r = 0, g = 0, b = 0, a = 0;
                for (size_t sy = sy0; sy < sy1; ++sy) {
                    for (size_t sx = sx0; sx < sx1; ++sx) {
                        const unsigned char* p = pixels + (sy * sw + sx) * 4;
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        a += p[3];
                    }
                }
                const unsigned n = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
                job.argb[y * job.w + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
        stbi_image_free(pixels);
    }

    // Single-file disk cache of the images as downloaded. The file is a header followed by
    // records appended as entries change; the newest record for a level wins, so an update is
    // an append. It is rewritten without the superseded records when they pile up, and without
    // the least recently used images when it outgrows max_cache_size.
    //   record: u8 kind, u8 key_len, key, u64 time (unix seconds), u32 data_len, data
    class ThumbCache
    {
    public:
        enum class Lookup
        {
            miss,    // not cached (or the missing marker is due for a recheck)
            image,   // out holds the cached image
            missing, // FactionFiles recently had no image for the level
        };

        // Sets the file on first use; the job threads all pass the same path
        Lookup get(const std::string& path, const std::string& key, std::vector<unsigned char>& out)
        {
            const std::lock_guard lock{mutex_};
            if (!ensure_loaded(path)) {
                return Lookup::miss;
            }
            const auto it = index_.find(key);
            if (it == index_.end()) {
                return Lookup::miss;
            }
            Entry& entry = it->second;
            const uint64_t now = now_unix();
            if (entry.kind == Kind::missing) {
                return now - entry.time < missing_recheck_seconds ? Lookup::missing : Lookup::miss;
            }
            {
                std::ifstream file{path_, std::ios::binary};
                out.resize(entry.data_len);
                file.seekg(static_cast<std::streamoff>(entry.data_off));
                if (!file.read(reinterpret_cast<char*>(out.data()), entry.data_len)) {
                    out.clear();
                    return Lookup::miss;
                }
            }
            // Refresh the LRU time, at most daily so browsing doesn't grow the file much
            if (now - entry.time >= touch_interval_seconds) {
                entry.time = now;
                append(Kind::touch, key, now, {});
            }
            return Lookup::image;
        }

        void put_image(const std::string& path, const std::string& key, const std::vector<unsigned char>& data)
        {
            put(path, Kind::image, key, now_unix(), data);
        }

        void put_missing(const std::string& path, const std::string& key)
        {
            put(path, Kind::missing, key, now_unix(), {});
        }

        // Drops a level's entry (a marker dated 0 reads as due for a recheck)
        void forget(const std::string& path, const std::string& key)
        {
            put(path, Kind::missing, key, 0, {});
        }

    private:
        enum class Kind : uint8_t
        {
            image = 1,   // data = the image; time = last used
            missing = 2, // no data; time = when FactionFiles was asked
            touch = 3,   // no data; time = last use of the level's image
        };

        struct Entry
        {
            Kind kind = Kind::missing;
            uint64_t time = 0;
            uint64_t data_off = 0;
            uint32_t data_len = 0;
        };

        static constexpr uint32_t magic = 0x43544641; // "AFTC"
        static constexpr uint32_t version = 1;
        static constexpr uint64_t header_size = 8;
        static constexpr size_t record_fixed_size = 1 + 1 + 8 + 4;
        static constexpr uint64_t missing_recheck_seconds = 7 * 24 * 3600;
        static constexpr uint64_t touch_interval_seconds = 24 * 3600;
        // Compaction trims to this fraction of the cap, so it doesn't rerun on every download
        static constexpr uint64_t compact_target_size = max_cache_size / 4 * 3;
        // Superseded records are only worth a rewrite once there is this much of them
        static constexpr uint64_t min_garbage_size = 16 * 1024 * 1024;

        std::mutex mutex_;
        std::string path_;
        bool loaded_ = false;
        bool compact_failed_ = false;
        uint64_t file_size_ = 0; // end of the last valid record; appends go here
        std::unordered_map<std::string, Entry> index_;

        static uint64_t now_unix()
        {
            return static_cast<uint64_t>(std::time(nullptr));
        }

        static uint64_t record_size(const std::string& key, uint32_t data_len)
        {
            return uint64_t{record_fixed_size} + key.size() + data_len;
        }

        template<typename T>
        static void put_bytes(std::vector<unsigned char>& buf, const T& value)
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            buf.insert(buf.end(), bytes, bytes + sizeof(T));
        }

        static std::vector<unsigned char> make_record(Kind kind, const std::string& key, uint64_t time,
            const std::vector<unsigned char>& data)
        {
            std::vector<unsigned char> buf;
            buf.reserve(record_fixed_size + key.size() + data.size());
            put_bytes(buf, kind);
            put_bytes(buf, static_cast<uint8_t>(key.size()));
            buf.insert(buf.end(), key.begin(), key.end());
            put_bytes(buf, time);
            put_bytes(buf, static_cast<uint32_t>(data.size()));
            buf.insert(buf.end(), data.begin(), data.end());
            return buf;
        }

        void apply(Kind kind, const std::string& key, uint64_t time, uint64_t data_off, uint32_t data_len)
        {
            if (kind == Kind::touch) {
                if (auto it = index_.find(key); it != index_.end() && it->second.kind == Kind::image) {
                    it->second.time = time;
                }
                return;
            }
            index_[key] = {kind, time, data_off, data_len};
        }

        // Reads the index; a damaged tail (an interrupted write) is cut off
        bool ensure_loaded(const std::string& path)
        {
            if (loaded_) {
                return !path_.empty();
            }
            loaded_ = true;
            path_ = path;
            if (path_.empty()) {
                return false;
            }
            // Held for the rest of the session: two instances appending to one file would corrupt it
            const std::string lock_path = path_ + ".lock";
            if (CreateFileA(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE,
                nullptr) == INVALID_HANDLE_VALUE) {
                xlog::info("[MapThumb] {} is in use by another instance, running without it", path_);
                path_.clear();
                return false;
            }
            std::error_code ec;
            std::filesystem::remove(path_ + ".tmp", ec);
            std::ifstream file{path_, std::ios::binary};
            if (!file) {
                return true; // no cache yet; the first append creates it
            }
            const uint64_t size = std::filesystem::file_size(path_, ec);
            uint32_t file_magic = 0, file_version = 0;
            file.read(reinterpret_cast<char*>(&file_magic), 4);
            file.read(reinterpret_cast<char*>(&file_version), 4);
            if (ec || !file || file_magic != magic || file_version != version) {
                xlog::info("[MapThumb] starting a new cache in {}", path_);
                return true; // file_size_ 0: the first append rewrites the header
            }
            uint64_t pos = header_size;
            while (pos + record_fixed_size <= size) {
                Kind kind{};
                uint8_t key_len = 0;
                uint64_t time = 0;
                uint32_t data_len = 0;
                std::string key;
                file.read(reinterpret_cast<char*>(&kind), 1);
                file.read(reinterpret_cast<char*>(&key_len), 1);
                key.resize(key_len);
                file.read(key.data(), key_len);
                file.read(reinterpret_cast<char*>(&time), 8);
                file.read(reinterpret_cast<char*>(&data_len), 4);
                const bool valid_kind = kind == Kind::image || kind == Kind::missing || kind == Kind::touch;
                const uint64_t end = pos + record_size(key, data_len);
                if (!file || !valid_kind || key.empty() || end > size || (kind == Kind::image) != (data_len != 0)
                    || data_len > FactionFilesClient::max_image_size) {
                    break;
                }
                apply(kind, key, time, end - data_len, data_len);
                file.seekg(static_cast<std::streamoff>(end));
                pos = end;
            }
            file.close();
            file_size_ = pos;
            if (pos < size) {
                xlog::warn("[MapThumb] cache {} is damaged past byte {}, discarding the rest", path_, pos);
                std::filesystem::resize_file(path_, pos, ec);
            }
            return true;
        }

        bool append(Kind kind, const std::string& key, uint64_t time, const std::vector<unsigned char>& data)
        {
            if (file_size_ == 0) {
                std::ofstream file{path_, std::ios::binary | std::ios::trunc};
                file.write(reinterpret_cast<const char*>(&magic), 4);
                file.write(reinterpret_cast<const char*>(&version), 4);
                if (!file) {
                    return false;
                }
                file_size_ = header_size;
            }
            const std::vector<unsigned char> record = make_record(kind, key, time, data);
            std::fstream file{path_, std::ios::binary | std::ios::in | std::ios::out};
            if (!file) {
                xlog::warn("[MapThumb] failed to open cache {}", path_);
                // Deleted while running: the next append starts a new file. Otherwise (e.g. briefly
                // locked by a backup) the file is kept and only this record is lost.
                if (GetFileAttributesA(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    file_size_ = 0;
                    index_.clear();
                }
                return false;
            }
            file.seekp(static_cast<std::streamoff>(file_size_));
            const auto record_len = static_cast<std::streamsize>(record.size());
            if (!file.write(reinterpret_cast<const char*>(record.data()), record_len)) {
                xlog::warn("[MapThumb] failed to write cache {}", path_);
                return false;
            }
            const uint64_t data_off = file_size_ + record.size() - data.size();
            file_size_ += record.size();
            apply(kind, key, time, data_off, static_cast<uint32_t>(data.size()));
            return true;
        }

        void put(const std::string& path, Kind kind, const std::string& key, uint64_t time,
            const std::vector<unsigned char>& data)
        {
            const std::lock_guard lock{mutex_};
            if (!ensure_loaded(path) || key.empty() || key.size() > 255
                || data.size() > FactionFilesClient::max_image_size) {
                return;
            }
            if (!append(kind, key, time, data) || compact_failed_) {
                return;
            }
            uint64_t live_size = header_size;
            for (const auto& [entry_key, entry] : index_) {
                live_size += record_size(entry_key, entry.data_len);
            }
            const bool garbage = file_size_ - live_size >= std::max(min_garbage_size, live_size);
            if (file_size_ > max_cache_size || garbage) {
                compact();
            }
        }

        void compact()
        {
            std::vector<std::pair<std::string, Entry>> entries{index_.begin(), index_.end()};
            std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.second.time > b.second.time; });

            const std::string tmp_path = path_ + ".tmp";
            std::unordered_map<std::string, Entry> new_index;
            uint64_t new_size = header_size;
            {
                std::ifstream src{path_, std::ios::binary};
                std::ofstream dst{tmp_path, std::ios::binary | std::ios::trunc};
                dst.write(reinterpret_cast<const char*>(&magic), 4);
                dst.write(reinterpret_cast<const char*>(&version), 4);
                const uint64_t now = now_unix();
                std::vector<unsigned char> data;
                for (const auto& [key, entry] : entries) {
                    if (entry.kind == Kind::missing && now - entry.time >= missing_recheck_seconds) {
                        continue;
                    }
                    const uint64_t size = record_size(key, entry.data_len);
                    if (entry.kind == Kind::image && new_size + size > compact_target_size) {
                        continue;
                    }
                    data.resize(entry.data_len);
                    src.seekg(static_cast<std::streamoff>(entry.data_off));
                    if (!src.read(reinterpret_cast<char*>(data.data()), entry.data_len)) {
                        src.clear();
                        continue;
                    }
                    const std::vector<unsigned char> record = make_record(entry.kind, key, entry.time, data);
                    dst.write(reinterpret_cast<const char*>(record.data()),
                        static_cast<std::streamsize>(record.size()));
                    new_index[key] = {entry.kind, entry.time, new_size + size - entry.data_len, entry.data_len};
                    new_size += size;
                }
                if (!dst) {
                    xlog::warn("[MapThumb] failed to compact cache {}", path_);
                    compact_failed_ = true;
                    dst.close();
                    std::error_code ec;
                    std::filesystem::remove(tmp_path, ec);
                    return;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp_path, path_, ec);
            if (ec) {
                xlog::warn("[MapThumb] failed to replace cache {}: {}", path_, ec.message());
                compact_failed_ = true;
                std::filesystem::remove(tmp_path, ec);
                return;
            }
            xlog::debug("[MapThumb] compacted cache from {} to {} bytes", file_size_, new_size);
            index_ = std::move(new_index);
            file_size_ = new_size;
        }
    };

    ThumbCache g_cache;

    const std::string& cache_path()
    {
        if (!g_cache_path) {
            const std::optional<std::string> dir = get_screenshots_dir();
            g_cache_path = dir ? *dir + "\\thumbnails.cache" : std::string{};
        }
        return *g_cache_path;
    }

    void run_job(const std::shared_ptr<Job>& job)
    {
        const std::string& path = job->cache_path;
        const std::string& key = job->level_filename;
        try {
            std::vector<unsigned char> encoded;
            const ThumbCache::Lookup cached = g_cache.get(path, key, encoded);
            bool downloaded = false;
            if (cached == ThumbCache::Lookup::miss) {
                try {
                    FactionFilesClient client;
                    const auto info = client.find_map(key.c_str());
                    if (info && !info->image_url.empty()) {
                        encoded = client.fetch_image(info->image_url);
                        downloaded = true;
                    }
                    else {
                        g_cache.put_missing(path, key);
                    }
                }
                catch (const std::exception& e) {
                    // find_map throws on a 404 for levels FactionFiles doesn't have; any other
                    // failure is left uncached so the next session tries again
                    if (std::string_view{e.what()}.find("404") != std::string_view::npos) {
                        g_cache.put_missing(path, key);
                    }
                    xlog::debug("[MapThumb] no image for {}: {}", key, e.what());
                }
            }
            if (!encoded.empty()) {
                decode_downscaled(*job, encoded);
            }
            if (downloaded) {
                // A refused or undecodable image is rechecked weekly instead of downloaded every session
                if (job->argb.empty()) {
                    g_cache.put_missing(path, key);
                }
                else {
                    g_cache.put_image(path, key, encoded);
                }
            }
            else if (cached == ThumbCache::Lookup::image && job->argb.empty()) {
                // Undecodable cached image: drop it so the next session downloads it again
                g_cache.forget(path, key);
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
            job->cache_path = cache_path();
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
