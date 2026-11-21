#pragma once

#include "platform.h"
#include <algorithm>
#include <optional>

namespace VrUi {
constexpr int width = 960;
constexpr int height = 600;
constexpr int rows = 9;
constexpr int firstY = 112;
constexpr int rowStep = 45;
constexpr int rowHeight = 40;
constexpr int margin = 24;
constexpr int sourcesRight = 286;
constexpr int filesLeft = 308;
constexpr float panelWidth = 1.72f;
constexpr float panelHeight = panelWidth * height / width;
constexpr int timelineHeight = 132;
constexpr float timelineWidth = 1.45f;
enum class Surface { None, Full, Timeline };

inline float surfaceWidth(Surface surface) { return surface == Surface::Timeline ? timelineWidth : panelWidth; }
inline int surfacePixels(Surface surface) { return surface == Surface::Timeline ? timelineHeight : height; }
inline float surfaceHeight(Surface surface) { return surfaceWidth(surface) * surfacePixels(surface) / width; }

inline bool hitsTimeline(float u, float v) {
    return u * width >= margin && u * width < width - margin && v >= .35f && v < 1.f;
}

enum class Trigger { Idle, Pressed, Held, Released, Cancelled };

struct Pointer {
    std::optional<XMFLOAT2> uv;
    bool confirm = false;
    Trigger trigger = Trigger::Idle;
};

inline double timelinePosition(float u) {
    constexpr int trackStart = margin + 18;
    return std::clamp((u * width - trackStart) / (width - 2. * trackStart), 0., 1.);
}

class ConfirmPress {
public:
    bool update(bool button, bool trigger) {
        const bool down = button || trigger;
        const bool pressed = down && !held_;
        held_ = down;
        return pressed;
    }

    void requireRelease() { held_ = true; }

private:
    bool held_ = false;
};

class FileGesture {
public:
    int update(int chordDirection, bool horizontalHeld) {
        if (!horizontalHeld) consumed_ = false;
        if (!chordDirection || consumed_) return 0;
        consumed_ = true;
        return chordDirection;
    }

    bool consumesSeek() const { return consumed_; }

private:
    bool consumed_ = false;
};

inline int firstRow(int count, int selected) {
    return std::clamp(selected - rows / 2, 0, std::max(0, count - rows));
}

inline int hitRow(float u, float v, int count, int selected, int left = margin, int right = width - margin) {
    const float y = v * height - firstY;
    if (u * width < left || u * width >= right || y < 0 || y >= rows * rowStep)
        return -1;
    const int row = static_cast<int>(y) / rowStep;
    const int index = firstRow(count, selected) + row;
    return index < count && y - row * rowStep < rowHeight ? index : -1;
}

inline std::optional<XMFLOAT2> aim(FXMVECTOR origin, FXMVECTOR direction, bool insidePanel = true,
                                  Surface surface = Surface::Full) {
    const float z = XMVectorGetZ(direction);
    if (XMVectorGetZ(origin) <= 0.f || z >= -.0001f) return std::nullopt;
    const auto hit = XMVectorMultiplyAdd(direction, XMVectorReplicate(-XMVectorGetZ(origin) / z), origin);
    const XMFLOAT2 uv{.5f + XMVectorGetX(hit) / surfaceWidth(surface), .5f - XMVectorGetY(hit) / surfaceHeight(surface)};
    if (insidePanel && (uv.x < 0 || uv.x >= 1 || uv.y < 0 || uv.y >= 1)) return std::nullopt;
    return uv;
}
}
