#include "video-format.h"
#include "camera-mesh.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <string_view>

namespace {
using Position = uint64_t;
struct Tags {
    int projection = -1;
    int stereo = -1;
    bool cubemap3x2 = false;
    std::array<uint32_t, 4> crop{};
    std::vector<uint8_t> mesh;
};
constexpr int customStereo = 100;

uint32_t be32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

bool read(std::ifstream& file, Position offset, void* data, size_t count) {
    file.seekg(static_cast<std::streamoff>(offset));
    return bool(file.read(static_cast<char*>(data), count));
}

void readCrop(std::ifstream& file, Position begin, Position end, Tags& tags) {
    uint8_t bytes[20]{};
    if (end - begin < sizeof(bytes) || !read(file, begin, bytes, sizeof(bytes)) || be32(bytes) != 0) return;
    for (unsigned i = 0; i < 4; ++i) tags.crop[i] = be32(bytes + 4 + i * 4);
}

void readMesh(std::ifstream& file, Position begin, Position end, Tags& tags) {
    if (end - begin < 20 || end - begin > 4 * 1024 * 1024) return;
    std::vector<uint8_t> payload(static_cast<size_t>(end - begin));
    if (read(file, begin, payload.data(), payload.size())) tags.mesh = std::move(payload);
}

void readMp4(std::ifstream& file, Position start, Position end, Tags& tags, unsigned depth, unsigned& budget) {
    for (Position offset = start; end - offset >= 8 && budget; --budget) {
        uint8_t data[16]{};
        if (!read(file, offset, data, 8)) return;
        Position size = be32(data);
        unsigned header = 8;
        if (size == 1) {
            if (end - offset < 16 || !read(file, offset + 8, data + 8, 8)) return;
            size = uint64_t(be32(data + 8)) << 32 | be32(data + 12);
            header = 16;
        } else if (size == 0) size = end - offset;
        if (size < header || size > end - offset) return;
        const Position begin = offset + header, stop = offset + size;
        const std::string_view type(reinterpret_cast<const char*>(data + 4), 4);
        unsigned prefix = 0;
        if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" ||
            type == "stbl" || type == "sv3d" || type == "proj") {}
        else if (type == "stsd") prefix = 8;
        else if (type == "avc1" || type == "avc3" || type == "hvc1" || type == "hev1" || type == "dvhe" ||
                 type == "dvh1" || type == "av01" || type == "vp09" || type == "mp4v" || type == "encv") prefix = 78;
        else if (type == "st3d" && stop - begin >= 5) {
            uint8_t value[5]{};
            if (read(file, begin, value, sizeof(value)) && be32(value) == 0) {
                switch (value[4]) {
                case 0: tags.stereo = 0; break;
                case 1: tags.stereo = 3; break;
                case 2: tags.stereo = 1; break;
                case 3: tags.stereo = customStereo; break;
                case 4: tags.stereo = 11; break;
                }
            }
            prefix = 255;
        } else if (type == "equi") {
            tags.projection = 1;
            readCrop(file, begin, stop, tags);
            prefix = 255;
        } else if (type == "cbmp") {
            uint8_t value[12]{};
            if (stop - begin >= sizeof(value) && read(file, begin, value, sizeof(value)) &&
                be32(value) == 0 && be32(value + 4) == 0 && be32(value + 8) == 0) {
                tags.projection = 2;
                tags.cubemap3x2 = true;
            }
            prefix = 255;
        } else if (type == "mshp" || type == "ytmp") {
            tags.projection = 3;
            readMesh(file, begin, stop, tags);
            prefix = 255;
        } else prefix = 255;
        if (depth < 10 && prefix != 255 && stop - begin >= prefix && budget > 1) {
            --budget;
            readMp4(file, begin + prefix, stop, tags, depth + 1, budget);
            if (!budget) return;
        }
        offset = stop;
    }
}

unsigned readVint(std::ifstream& file, Position start, Position end, bool isId, uint64_t& value) {
    uint8_t bytes[8]{};
    if (start >= end || !read(file, start, bytes, 1) || !bytes[0]) return 0;
    unsigned length = 1, mark = 128;
    while (!(bytes[0] & mark)) { mark >>= 1; ++length; }
    if (length > end - start || (isId && length > 4) || !read(file, start, bytes, length)) return 0;
    value = isId ? bytes[0] : bytes[0] & (mark - 1);
    for (unsigned i = 1; i < length; ++i) value = (value << 8) | bytes[i];
    if (!isId && value == (uint64_t(1) << (7 * length)) - 1) value = UINT64_MAX;
    return length;
}

void readMkv(std::ifstream& file, Position start, Position end, Tags& tags, unsigned depth, unsigned& budget) {
    for (Position offset = start; offset < end && budget; --budget) {
        uint64_t id = 0, size = 0;
        const unsigned idSize = readVint(file, offset, end, true, id);
        if (!idSize) return;
        const unsigned sizeSize = readVint(file, offset + idSize, end, false, size);
        if (!sizeSize) return;
        const Position begin = offset + idSize + sizeSize;
        if (size == UINT64_MAX && id == 0x18538067) size = end - begin;
        if (size > end - begin) return;
        const Position stop = begin + size;
        switch (id) {
        case 0x18538067: case 0x1654AE6B: case 0xAE: case 0xE0: case 0x7670:
            if (depth < 6 && budget > 1) {
                --budget;
                readMkv(file, begin, stop, tags, depth + 1, budget);
                if (!budget) return;
            }
            break;
        case 0x53B8: case 0x7671: {
            uint8_t value = 0;
            if (size != 1 || !read(file, begin, &value, 1)) break;
            if (id == 0x53B8) tags.stereo = value;
            else tags.projection = value;
            break;
        }
        case 0x7672: {
            uint8_t bytes[12]{};
            if (size == sizeof(bytes) && read(file, begin, bytes, sizeof(bytes))) {
                tags.cubemap3x2 = be32(bytes) == 0 && be32(bytes + 4) == 0 && be32(bytes + 8) == 0;
            } else if (size == 20) {
                readCrop(file, begin, stop, tags);
            } else {
                readMesh(file, begin, stop, tags);
            }
            break;
        }
        case 0x1F43B675: return;
        }
        offset = stop;
    }
}

Tags metadata(const fs::path& path) {
    Tags tags;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return tags;
    const auto end = file.tellg();
    uint8_t signature[8]{};
    if (end < 8 || !read(file, 0, signature, sizeof(signature))) return tags;
    unsigned budget = 8192;
    if (be32(signature) == 0x1A45DFA3) {
        readMkv(file, 0, static_cast<Position>(end), tags, 0, budget);
    } else {
        const std::string_view first(reinterpret_cast<const char*>(signature + 4), 4);
        if (first == "ftyp" || first == "moov" || first == "free" || first == "wide" || first == "mdat")
            readMp4(file, 0, static_cast<Position>(end), tags, 0, budget);
    }
    return tags;
}
}

const wchar_t* projectionName(Projection mode) {
    switch (mode) {
    case Projection::Flat: return L"Écran plat";
    case Projection::Equirect180: return L"VR 180°";
    case Projection::Equirect360: return L"VR 360°";
    case Projection::Fisheye180: return L"Fisheye";
    case Projection::Cubemap: return L"Cube 3 × 2";
    case Projection::Calibrated: return L"Lentilles calibrées";
    }
    return L"";
}

const wchar_t* stereoName(Stereo mode) {
    switch (mode) {
    case Stereo::Mono: return L"Mono";
    case Stereo::SideBySide: return L"Côte à côte (SBS)";
    case Stereo::TopBottom: return L"Haut / bas (TB)";
    case Stereo::Calibrated: return L"3D calibrée";
    }
    return L"";
}

float fieldOfView(const ViewSettings& view) {
    return view.horizontalFov ? view.horizontalFov : view.projection == Projection::Equirect360 ? 360.f : 180.f;
}

VideoFormat detectFormat(const fs::path& path) {
    auto name = path.stem().wstring();
    for (auto& c : name) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) c = L' ';
    }
    name = L" " + name + L" ";
    const auto has = [&](std::wstring_view word) { return name.find(L" " + std::wstring(word) + L" ") != name.npos; };
    const Tags tags = metadata(path);
    VideoFormat format;
    auto& view = format.view;
    if (has(L"fisheye") || has(L"fish eye")) {
        view.projection = Projection::Fisheye180;
        for (int angle = 120; angle <= 220; angle += 5) {
            auto value = std::to_wstring(angle);
            if (has(value + L"deg") || has(value)) { view.horizontalFov = static_cast<float>(angle); break; }
        }
    }
    else if (has(L"cubemap") || has(L"cube3x2")) view.projection = Projection::Cubemap;
    else if (has(L"360") || has(L"vr360") || has(L"360vr") || has(L"360deg")) view.projection = Projection::Equirect360;
    else if (has(L"180") || has(L"vr180") || has(L"180vr") || has(L"180deg")) view.projection = Projection::Equirect180;
    format.projectionKnown = view.projection == Projection::Fisheye180 || view.projection == Projection::Cubemap ||
        (view.projection != Projection::Flat && (has(L"equirectangular") || has(L"equirect") || has(L"erp")));
    if (has(L"sbs") || has(L"hsbs") || has(L"fsbs") || has(L"lr") || has(L"rl") || has(L"left right") || has(L"right left"))
        view.stereo = Stereo::SideBySide;
    else if (has(L"tb") || has(L"ou") || has(L"top bottom") || has(L"bottom top")) view.stereo = Stereo::TopBottom;
    format.layoutKnown = view.stereo != Stereo::Mono || has(L"mono") || has(L"2d");
    view.swapEyes = has(L"rl") || has(L"right left") || has(L"bottom top");
    switch (tags.stereo) {
    case 0: view.stereo = Stereo::Mono; view.swapEyes = false; format.layoutKnown = true; break;
    case 1: case 11: view.stereo = Stereo::SideBySide; view.swapEyes = tags.stereo == 11; format.layoutKnown = true; break;
    case 2: case 3: view.stereo = Stereo::TopBottom; view.swapEyes = tags.stereo == 2; format.layoutKnown = true; break;
    }
    if (tags.projection == 1) {
        const auto& bounds = tags.crop;
        constexpr double unit = 4294967296.;
        const double width = 1. - (double(bounds[2]) + bounds[3]) / unit;
        const double height = 1. - (double(bounds[0]) + bounds[1]) / unit;
        if (width > 0. && height > 0.) {
            view.projection = width > .75 ? Projection::Equirect360 : Projection::Equirect180;
            format.projectionKnown = true;
            view.horizontalFov = float(360 * width);
            view.verticalFov = float(180 * height);
            view.centerYaw = float(180 * (double(bounds[2]) - bounds[3]) / unit);
            view.centerPitch = float(90 * (double(bounds[1]) - bounds[0]) / unit);
        }
    } else if (tags.projection == 2 && tags.cubemap3x2) {
        view.projection = Projection::Cubemap;
        view.horizontalFov = 0;
        format.projectionKnown = true;
    }
    else if (tags.projection == 3) {
        format.cameraMesh = true;
        const bool custom = tags.stereo == customStereo || tags.stereo == 15;
        view.mesh = decodeCameraMesh(tags.mesh.data(), tags.mesh.size(), custom);
        if (view.mesh) {
            view.projection = Projection::Calibrated;
            format.projectionKnown = true;
            if (view.mesh->count == 2 && (custom || view.stereo == Stereo::Mono)) {
                view.stereo = Stereo::Calibrated;
                format.layoutKnown = true;
            }
        } else if (view.projection == Projection::Flat) {
            view.projection = Projection::Fisheye180;
        }
    }
    return format;
}
