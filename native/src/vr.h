#pragma once

#include "renderer.h"
#include "vr-ui.h"
#include <functional>
#include <memory>
#include <optional>

enum class VrCommand {
    Select, Back, Previous, Next, FocusSources, FocusFiles, Recenter, Rewind, Advance, PreviousFile, NextFile, Timeline
};

class Vr {
public:
    Vr();
    ~Vr();
    const LUID& adapter() const;
    std::string runtime() const;
    void start(Renderer& renderer);
    void close();
    bool active() const;
    void recenter();
    bool frame(Renderer& renderer, const Media& media, ViewSettings& view,
               const std::function<void(VrCommand)>& input,
               const std::function<VrUi::Surface()>& surface,
               const std::function<void(const VrUi::Pointer&)>& point,
               const std::function<ID3D11ShaderResourceView*()>& menu);

private:
    struct State;
    std::unique_ptr<State> state_;
};
