#include "thumbnails.h"
#include "platform.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace {
class ThumbnailDecoder {
public:
    ~ThumbnailDecoder() {
        if (render_) {
            mpv_render_context_set_update_callback(render_, nullptr, nullptr);
            mpv_render_context_free(render_);
        }
        if (player_) mpv_terminate_destroy(player_);
        if (dll_) FreeLibrary(dll_);
    }

    bool open(const fs::path& path) {
        dll_ = LoadLibraryExW(L"libmpv-2.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) return false;
#define LOAD_MPV(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(dll_, #name)); if (!name) return false;
        LOAD_MPV(mpv_create)
        LOAD_MPV(mpv_initialize)
        LOAD_MPV(mpv_set_option_string)
        LOAD_MPV(mpv_command_async)
        LOAD_MPV(mpv_wait_event)
        LOAD_MPV(mpv_terminate_destroy)
        LOAD_MPV(mpv_render_context_create)
        LOAD_MPV(mpv_render_context_free)
        LOAD_MPV(mpv_render_context_set_update_callback)
        LOAD_MPV(mpv_render_context_update)
        LOAD_MPV(mpv_render_context_render)
#undef LOAD_MPV
        player_ = mpv_create();
        if (!player_) return false;
        const char* options[][2] = {
            {"config", "no"}, {"vo", "libmpv"}, {"idle", "yes"}, {"keep-open", "yes"},
            {"audio", "no"}, {"sub", "no"}, {"osd-level", "0"},
            {"pause", "yes"}, {"start", "5%"}, {"hr-seek", "no"}, {"vd-lavc-threads", "1"},
            {"sws-scaler", "bilinear"},
            {"demuxer-readahead-secs", "0"}, {"demuxer-max-bytes", "2MiB"}, {"cache", "no"},
            {"load-scripts", "no"}, {"autoload-files", "no"}, {"ytdl", "no"}, {"network-timeout", "3"},
            {"audio-file-auto", "no"}, {"sub-auto", "no"}, {"cover-art-auto", "no"}
        };
        for (const auto& option : options) mpv_set_option_string(player_, option[0], option[1]);
        // Avoid the zimg chroma-alignment assertion when scaling cropped or odd-width frames.
        if (mpv_set_option_string(player_, "sws-allow-zimg", "no") < 0) return false;
        if (mpv_initialize(player_) < 0) return false;
        mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)}, {}};
        if (mpv_render_context_create(&render_, player_, params) < 0) return false;
        mpv_render_context_set_update_callback(render_, [](void* data) {
            static_cast<ThumbnailDecoder*>(data)->frameChanged_ = true;
        }, this);
        const auto filename = path.u8string();
        const char* command[] = {"loadfile", filename.c_str(), nullptr};
        return mpv_command_async(player_, 0, command) >= 0;
    }

    template<class StillVisible>
    std::shared_ptr<const Thumbnail> frame(StillVisible stillVisible) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        bool loaded = false, framePending = false;
        while (std::chrono::steady_clock::now() < deadline && stillVisible()) {
            const auto* event = mpv_wait_event(player_, .02);
            if (event->event_id == MPV_EVENT_END_FILE || event->event_id == MPV_EVENT_SHUTDOWN ||
                (event->event_id == MPV_EVENT_COMMAND_REPLY && event->error < 0)) break;
            if (event->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
            if (frameChanged_.exchange(false) && (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME))
                framePending = true;
            if (!loaded || !framePending) continue;
            auto image = std::make_shared<Thumbnail>();
            int size[] = {Thumbnail::width, Thumbnail::height};
            size_t stride = Thumbnail::width * sizeof(std::uint32_t);
            char format[] = "bgr0";
            int block = 0;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_SW_SIZE, size}, {MPV_RENDER_PARAM_SW_FORMAT, format},
                {MPV_RENDER_PARAM_SW_STRIDE, &stride}, {MPV_RENDER_PARAM_SW_POINTER, image->pixels.data()},
                {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {}
            };
            if (mpv_render_context_render(render_, params) >= 0) return image;
            break;
        }
        return {};
    }

private:
    HMODULE dll_ = nullptr;
    mpv_handle* player_ = nullptr;
    mpv_render_context* render_ = nullptr;
    std::atomic<bool> frameChanged_{false};
#define MPV_FUNCTION(name) decltype(&::name) name = nullptr;
    MPV_FUNCTION(mpv_create)
    MPV_FUNCTION(mpv_initialize)
    MPV_FUNCTION(mpv_set_option_string)
    MPV_FUNCTION(mpv_command_async)
    MPV_FUNCTION(mpv_wait_event)
    MPV_FUNCTION(mpv_terminate_destroy)
    MPV_FUNCTION(mpv_render_context_create)
    MPV_FUNCTION(mpv_render_context_free)
    MPV_FUNCTION(mpv_render_context_set_update_callback)
    MPV_FUNCTION(mpv_render_context_update)
    MPV_FUNCTION(mpv_render_context_render)
#undef MPV_FUNCTION
};
}

Thumbnails::~Thumbnails() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void Thumbnails::reset() {
    std::lock_guard lock(mutex_);
    ++generation_;
    visible_.clear();
    indices_.clear();
    cache_.clear();
    changed_ = false;
}

void Thumbnails::show(const std::vector<int>& indices, const std::vector<LibraryEntry>& entries) {
    std::lock_guard lock(mutex_);
    if (indices == indices_) return;
    indices_ = indices;
    visible_.clear();
    ++revision_;
    for (const int index : indices_) {
        const auto& entry = entries[index];
        if (entry.folder || !Library::isVideo(entry.path)) continue;
        visible_.push_back({index, entry.path});
        const auto cached = cache_.find(index);
        if (cached != cache_.end()) cached->second.lastVisible = revision_;
    }
    if (!visible_.empty() && !worker_.joinable())
        worker_ = std::thread(&Thumbnails::run, this);
    wake_.notify_one();
}

std::shared_ptr<const Thumbnail> Thumbnails::get(int index) const {
    std::lock_guard lock(mutex_);
    const auto cached = cache_.find(index);
    return cached == cache_.end() ? nullptr : cached->second.image;
}

bool Thumbnails::changed() {
    std::lock_guard lock(mutex_);
    return std::exchange(changed_, false);
}

bool Thumbnails::wanted(int index) const {
    return std::any_of(visible_.begin(), visible_.end(), [index](const Request& request) { return request.index == index; });
}

void Thumbnails::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        const auto next = std::find_if(visible_.begin(), visible_.end(),
            [&](const Request& request) { return cache_.find(request.index) == cache_.end(); });
        if (next == visible_.end()) {
            wake_.wait(lock);
            continue;
        }
        const auto request = *next;
        const auto generation = generation_;
        lock.unlock();
        bool cancelled = false;
        const auto stillVisible = [&] {
            std::lock_guard guard(mutex_);
            cancelled = stopping_ || generation != generation_ || !wanted(request.index);
            return !cancelled;
        };
        std::shared_ptr<const Thumbnail> image;
        try {
            ThumbnailDecoder decoder;
            if (stillVisible() && decoder.open(request.path)) image = decoder.frame(stillVisible);
        } catch (const std::exception&) {
            // Keep an unreadable file's placeholder and continue with the next video.
        }
        lock.lock();
        if (cancelled || stopping_ || generation != generation_ || !wanted(request.index)) continue;
        constexpr size_t cacheLimit = 128;
        if (cache_.size() >= std::max(cacheLimit, visible_.size())) {
            const auto oldest = std::min_element(cache_.begin(), cache_.end(),
                [](const auto& a, const auto& b) { return a.second.lastVisible < b.second.lastVisible; });
            cache_.erase(oldest);
        }
        changed_ |= static_cast<bool>(image);
        cache_.emplace(request.index, Cached{std::move(image), revision_});
    }
}
