#include "media.h"
#include <mpv/render_d3d11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace {
template<class Function>
Function loadSymbol(HMODULE dll, const char* name) {
    auto address = GetProcAddress(dll, name);
    if (!address) throw std::runtime_error(std::string("Fonction libmpv manquante : ") + name);
    return reinterpret_cast<Function>(address);
}
}

struct Media::State {
    HMODULE dll = nullptr;
    mpv_handle* player = nullptr;
    mpv_render_context* render = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::atomic<bool> changed{true};
    bool frame = false;
    bool resume = false;
    std::chrono::steady_clock::time_point nextPositionSave{};
    int width = 0, height = 0;
    std::wstring message;

#define MPV_SYMBOL(name) decltype(&::name) name = nullptr;
    MPV_SYMBOL(mpv_create)
    MPV_SYMBOL(mpv_initialize)
    MPV_SYMBOL(mpv_set_option_string)
    MPV_SYMBOL(mpv_set_property_string)
    MPV_SYMBOL(mpv_get_property)
    MPV_SYMBOL(mpv_free)
    MPV_SYMBOL(mpv_command)
    MPV_SYMBOL(mpv_error_string)
    MPV_SYMBOL(mpv_wait_event)
    MPV_SYMBOL(mpv_terminate_destroy)
    MPV_SYMBOL(mpv_render_context_create)
    MPV_SYMBOL(mpv_render_context_free)
    MPV_SYMBOL(mpv_render_context_render)
    MPV_SYMBOL(mpv_render_context_set_update_callback)
#undef MPV_SYMBOL

    ~State() {
        if (render) {
            mpv_render_context_set_update_callback(render, nullptr, nullptr);
            mpv_render_context_free(render);
        }
        if (player) mpv_terminate_destroy(player);
        if (dll) FreeLibrary(dll);
    }

    void check(int code) {
        if (code < 0) throw std::runtime_error(mpv_error_string(code));
    }

    void command(const char* a, const char* b = nullptr, const char* c = nullptr) {
        const char* args[] = {a, b, c, nullptr};
        check(mpv_command(player, args));
    }

    template<class T>
    T property(const char* name, mpv_format format) const {
        T value{};
        mpv_get_property(player, name, format, &value);
        return value;
    }

    void setSize(int w, int h) {
        width = w;
        height = h;
        // The CV1 benefits from the panorama's full width even though each eye sees only part of it.
        double scale = std::min(1., 7680. / std::max(w, h));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = std::max(1u, static_cast<UINT>(w * scale));
        desc.Height = std::max(1u, static_cast<UINT>(h * scale));
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        srv.Reset();
        texture.Reset();
        checked(device->CreateTexture2D(&desc, nullptr, &texture), "Texture vidéo");
        checked(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "Vue de la texture vidéo");
    }
};

Media::Media(ID3D11Device* device, bool silent, const fs::path& resumeDirectory) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.dll = LoadLibraryExW(L"libmpv-2.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.dll) throw std::runtime_error("libmpv-2.dll doit se trouver à côté de LecteurVideoVR.exe.");
#define MPV_SYMBOL(name) s.name = loadSymbol<decltype(s.name)>(s.dll, #name);
    MPV_SYMBOL(mpv_create)
    MPV_SYMBOL(mpv_initialize)
    MPV_SYMBOL(mpv_set_option_string)
    MPV_SYMBOL(mpv_set_property_string)
    MPV_SYMBOL(mpv_get_property)
    MPV_SYMBOL(mpv_free)
    MPV_SYMBOL(mpv_command)
    MPV_SYMBOL(mpv_error_string)
    MPV_SYMBOL(mpv_wait_event)
    MPV_SYMBOL(mpv_terminate_destroy)
    MPV_SYMBOL(mpv_render_context_create)
    MPV_SYMBOL(mpv_render_context_free)
    MPV_SYMBOL(mpv_render_context_render)
    MPV_SYMBOL(mpv_render_context_set_update_callback)
#undef MPV_SYMBOL
    s.device = device;
    device->GetImmediateContext(&s.context);
    s.player = s.mpv_create();
    if (!s.player) throw std::runtime_error("Impossible de créer le lecteur vidéo.");
    const auto option = [&](const char* key, const char* value) { s.check(s.mpv_set_option_string(s.player, key, value)); };
    option("config", "no");
    s.resume = !resumeDirectory.empty();
    option("resume-playback", s.resume ? "yes" : "no");
    if (s.resume) {
        option("watch-later-dir", resumeDirectory.u8string().c_str());
        option("watch-later-options", "start");
        option("resume-playback-check-mtime", "yes");
    }
    option("ytdl", "no");
    option("load-scripts", "no");
    option("vo", "libmpv");
    option("hwdec", silent ? "no" : "auto-safe");
    option("keep-open", "yes");
    option("keepaspect", "no");
    option("image-display-duration", "inf");
    option("osc", "no");
    if (silent) option("ao", "null");
    s.check(s.mpv_initialize(s.player));
    mpv_d3d11_init_params init{device};
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_D3D11)},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init}, {}};
    s.check(s.mpv_render_context_create(&s.render, s.player, params));
    s.mpv_render_context_set_update_callback(s.render, [](void* data) {
        static_cast<State*>(data)->changed = true;
    }, &s);
}

Media::~Media() { savePosition(); }

void Media::savePosition() noexcept {
    auto& s = *state_;
    if (!s.resume || !s.frame || !s.message.empty()) return;
    const auto length = duration(), position = time();
    if (!std::isfinite(length) || !std::isfinite(position) || length <= 0) return;
    const bool completed = s.property<int>("eof-reached", MPV_FORMAT_FLAG) != 0 ||
        position >= length - std::min(.8, length * .03);
    const char* command[] = {completed || position < .25 ? "delete-watch-later-config" : "write-watch-later-config", nullptr};
    s.mpv_command(s.player, command);
}

void Media::load(const fs::path& path) {
    auto& s = *state_;
    if (!fs::is_regular_file(path)) throw std::runtime_error("Ce fichier local est introuvable.");
    savePosition();
    s.frame = false;
    s.nextPositionSave = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    s.message.clear();
    s.command("loadfile", path.u8string().c_str(), "replace");
    s.check(s.mpv_set_property_string(s.player, "pause", "no"));
}

void Media::tick() {
    auto& s = *state_;
    for (;;) {
        const auto* event = s.mpv_wait_event(s.player, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            const auto* result = static_cast<const mpv_event_end_file*>(event->data);
            if (result->reason == MPV_END_FILE_REASON_ERROR) s.message = wide(s.mpv_error_string(result->error));
        }
    }
    if (!s.changed.exchange(false)) return;
    int w = static_cast<int>(s.property<int64_t>("width", MPV_FORMAT_INT64));
    int h = static_cast<int>(s.property<int64_t>("height", MPV_FORMAT_INT64));
    if (w <= 0 || h <= 0) return;
    if (w != s.width || h != s.height) s.setSize(w, h);
    mpv_d3d11_fbo target{s.texture.Get()};
    int wait = 0;
    mpv_render_param params[] = {{MPV_RENDER_PARAM_D3D11_FBO, &target}, {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &wait}, {}};
    s.context->ClearState();
    s.check(s.mpv_render_context_render(s.render, params));
    s.frame = true;
    const auto now = std::chrono::steady_clock::now();
    if (s.resume && now >= s.nextPositionSave) {
        savePosition();
        s.nextPositionSave = now + std::chrono::seconds(10);
    }
}

void Media::togglePause() { state_->command("cycle", "pause"); }
void Media::mute() { state_->command("cycle", "mute"); }
bool Media::muted() const { return state_->property<int>("mute", MPV_FORMAT_FLAG) != 0; }
void Media::volume(int percent) { state_->check(state_->mpv_set_property_string(state_->player, "volume", std::to_string(percent).c_str())); }
double Media::volume() const { return state_->property<double>("volume", MPV_FORMAT_DOUBLE); }
void Media::seek(double seconds, bool absolute) {
    if (duration() > 0) state_->command("seek", std::to_string(seconds).c_str(), absolute ? "absolute+exact" : "relative+exact");
}
double Media::time() const { return state_->property<double>("time-pos", MPV_FORMAT_DOUBLE); }
double Media::duration() const { return state_->property<double>("duration", MPV_FORMAT_DOUBLE); }
bool Media::paused() const { return state_->property<int>("pause", MPV_FORMAT_FLAG) != 0; }
bool Media::ready() const { return state_->frame; }
std::wstring Media::error() const { return state_->message; }
std::string Media::sourceStereo() const {
    char* value = state_->property<char*>("video-params/stereo-in", MPV_FORMAT_STRING);
    if (!value) return {};
    std::string mode(value);
    state_->mpv_free(value);
    return mode;
}
float Media::aspect() const { return state_->height ? float(state_->width) / state_->height : 16.f / 9.f; }
ID3D11ShaderResourceView* Media::image() const { return ready() ? state_->srv.Get() : nullptr; }
