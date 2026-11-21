#pragma once

#include "platform.h"
#include "library.h"
#include <memory>

class Media {
public:
    explicit Media(ID3D11Device* device, bool silent = false, const fs::path& resumeDirectory = {});
    ~Media();
    void load(const fs::path& path);
    void tick();
    void togglePause();
    void seek(double seconds, bool absolute = false);
    void mute();
    bool muted() const;
    void volume(int percent);
    double volume() const;
    double time() const;
    double duration() const;
    bool paused() const;
    bool ready() const;
    std::wstring error() const;
    std::string sourceStereo() const;
    float aspect() const;
    ID3D11ShaderResourceView* image() const;

private:
    void savePosition() noexcept;
    struct State;
    std::unique_ptr<State> state_;
};
