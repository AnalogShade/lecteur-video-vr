#include "stereo-probe.h"
#include <algorithm>
#include <cmath>

namespace {
struct Tile {
    const std::vector<uint8_t>& pixels;
    int left, top, width, height;
    double at(int x, int y) const { return pixels[(top + y) * stereoProbeWidth + left + x]; }
};

double patch(const Tile& left, const Tile& right, int cx, int cy, int dx, int dy) {
    double sumL = 0, sumR = 0, ll = 0, rr = 0, lr = 0;
    constexpr int n = 64;
    for (int y = -4; y < 4; ++y) for (int x = -4; x < 4; ++x) {
        const double a = left.at(cx + x, cy + y);
        const double b = right.at(cx + x + dx, cy + y + dy);
        sumL += a;
        sumR += b;
        ll += a * a;
        rr += b * b;
        lr += a * b;
    }
    ll -= sumL * sumL / n;
    rr -= sumR * sumR / n;
    if (std::min(ll, rr) < 900.) return -1.;
    return (lr - sumL * sumR / n) / std::sqrt(ll * rr);
}

double match(const Tile& left, const Tile& right, int horizontal = 5, int vertical = 1) {
    double votes = 0;
    int usable = 0;
    const int xMargin = horizontal + 5, yMargin = vertical + 5;
    for (int y = 1; y <= 5; ++y) for (int x = 1; x <= 6; ++x) {
        const int cx = xMargin + (left.width - 2 * xMargin) * x / 7;
        const int cy = yMargin + (left.height - 2 * yMargin) * y / 6;
        double best = -1.;
        for (int dy = -vertical; dy <= vertical; ++dy) for (int dx = -horizontal; dx <= horizontal; ++dx)
            best = std::max(best, patch(left, right, cx, cy, dx, dy));
        if (best > -.99) {
            ++usable;
            votes += std::clamp((best - .4) / .6, 0., 1.);
        }
    }
    return usable >= 12 ? votes / usable : 0.;
}

bool viewsDiffer(const Tile& first, const Tile& second) {
    // A logo duplicated in both halves can look like a lens circle, but supplies no stereo evidence.
    int changed = 0, total = 0;
    double difference = 0;
    for (int y = 8; y < first.height - 8; y += 2) {
        for (int x = 8; x < first.width - 8; x += 2) {
            const auto delta = std::abs(first.at(x, y) - second.at(x, y));
            changed += delta >= 5.;
            difference += delta;
            ++total;
        }
    }
    return changed * 10 >= total && difference >= 2. * total;
}

bool stereoScore(double leftRight, double topBottom, double threshold) {
    return std::max(leftRight, topBottom) >= threshold && std::abs(leftRight - topBottom) >= .16;
}

bool lensCircle(const Tile& tile) {
    int darkCorners = 0, brightInside = 0;
    for (int x : {7, 13, 87, 93}) for (int y : {7, 13, 87, 93}) {
        darkCorners += tile.at(x * tile.width / 100, y * tile.height / 100) < 14;
    }
    for (int x : {24, 40, 60, 76}) for (int y : {24, 40, 60, 76}) {
        brightInside += tile.at(x * tile.width / 100, y * tile.height / 100) > 34;
    }
    return darkCorners >= 14 && brightInside >= 9;
}

bool near(float value, float expected) { return std::abs(value / expected - 1.f) < .055f; }
}

bool applyDecoderStereo(VideoFormat& format, const std::string& mode) {
    if (format.layoutKnown) return false;
    auto& view = format.view;
    if (mode == "sbs2l" || mode == "sbs2r") {
        view.stereo = Stereo::SideBySide;
        view.swapEyes = mode == "sbs2r";
    } else if (mode == "ab2l" || mode == "ab2r") {
        view.stereo = Stereo::TopBottom;
        view.swapEyes = mode == "ab2r";
    } else return false;
    format.layoutKnown = true;
    return true;
}

bool inspectStereoFrame(VideoFormat& format, const std::vector<uint8_t>& luma, float packedAspect) {
    if (luma.size() != stereoProbeWidth * stereoProbeHeight || (format.layoutKnown && format.projectionKnown)) return false;
    constexpr int w = stereoProbeWidth, h = stereoProbeHeight;
    const Tile left{luma, 0, 0, w / 2, h}, right{luma, w / 2, 0, w / 2, h};
    const Tile top{luma, 0, 0, w, h / 2}, bottom{luma, 0, h / 2, w, h / 2};
    auto& view = format.view;
    bool changed = false;
    if (!format.layoutKnown) {
        const bool sideDistinct = viewsDiffer(left, right), stackDistinct = viewsDiffer(top, bottom);
        const bool sideGeometry = near(packedAspect, 2.f) || near(packedAspect, 4.f);
        const bool stackGeometry = near(packedAspect, .5f) || near(packedAspect, 1.f);
        double side = sideDistinct ? match(left, right) : 0.;
        double stack = stackDistinct ? match(top, bottom) : 0.;
        const bool narrowGeometry = side > stack ? sideGeometry : stackGeometry;
        if (!stereoScore(side, stack, narrowGeometry ? .60 : .70)) {
            if (!sideGeometry && !stackGeometry) return false;
            side = sideDistinct ? match(left, right, 14, 3) : 0.;
            stack = stackDistinct ? match(top, bottom, 14, 3) : 0.;
            const bool wideGeometry = side > stack ? sideGeometry : stackGeometry;
            if (!wideGeometry || !stereoScore(side, stack, .58)) return false;
        }
        view.stereo = side > stack ? Stereo::SideBySide : Stereo::TopBottom;
        format.layoutKnown = changed = true;
    }
    if (!format.projectionKnown && (view.stereo == Stereo::SideBySide || view.stereo == Stereo::TopBottom)) {
        const bool horizontal = view.stereo == Stereo::SideBySide;
        if (!(horizontal ? viewsDiffer(left, right) : viewsDiffer(top, bottom))) return changed;
        const float perEye = horizontal ? packedAspect * .5f : packedAspect * 2.f;
        if (perEye >= .75f && perEye <= 1.35f &&
            (horizontal ? lensCircle(left) && lensCircle(right) : lensCircle(top) && lensCircle(bottom))) {
            view.projection = Projection::Fisheye180;
        } else if (view.projection == Projection::Flat) {
            if (near(perEye, 1.f)) view.projection = Projection::Equirect180;
            else if (near(perEye, 2.f)) view.projection = Projection::Equirect360;
        }
        format.projectionKnown = true;
        changed = true;
    }
    return changed;
}
