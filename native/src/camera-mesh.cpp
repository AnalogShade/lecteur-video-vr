#include "camera-mesh.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <puff.h>
}

namespace {
constexpr uint32_t tag(char a, char b, char c, char d) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d);
}
constexpr size_t inputLimit = 4 * 1024 * 1024;

uint32_t word(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

class Bits {
public:
    Bits(const uint8_t* data, size_t size) : data_(data), end_(size * 8) {}
    uint32_t get(unsigned count) {
        if (count > end_ - position_) throw std::range_error("Maillage tronqué");
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++position_)
            value = (value << 1) | ((data_[position_ / 8] >> (7 - position_ % 8)) & 1);
        return value;
    }
    void align() { get(static_cast<unsigned>((8 - position_ % 8) % 8)); }
private:
    const uint8_t* data_;
    size_t end_;
    size_t position_ = 0;
};

unsigned deltaWidth(uint32_t count) {
    unsigned bits = 0;
    for (uint32_t value = count * 2 - 1; value; value >>= 1) ++bits;
    return bits;
}

uint32_t bounded(Bits& input, uint32_t limit) {
    const uint32_t value = input.get(32);
    if (!value || value > limit) throw std::range_error("Taille du maillage invalide");
    return value;
}

uint32_t nextIndex(Bits& input, unsigned bits, uint32_t previous, uint32_t limit) {
    const auto zigzag = input.get(bits);
    const auto value = static_cast<int>(previous) +
        (zigzag & 1 ? -static_cast<int>(zigzag / 2) - 1 : static_cast<int>(zigzag / 2));
    if (value < 0 || static_cast<uint32_t>(value) >= limit) throw std::range_error("Indice du maillage invalide");
    return static_cast<uint32_t>(value);
}

EyeMesh readEye(const uint8_t* data, size_t size) {
    Bits input(data, size);
    std::vector<float> values(bounded(input, 10000));
    for (auto& value : values) {
        const auto binary = input.get(32);
        std::memcpy(&value, &binary, sizeof(value));
        if (!std::isfinite(value) || std::abs(value) > 10000.f) throw std::range_error("Coordonnée du maillage invalide");
    }
    EyeMesh eye;
    eye.vertices.resize(bounded(input, 32000));
    const auto coordinateCount = static_cast<uint32_t>(values.size());
    const unsigned coordinateBits = deltaWidth(coordinateCount);
    std::array<uint32_t, 5> previous{};
    for (auto& vertex : eye.vertices) {
        for (auto& value : previous) value = nextIndex(input, coordinateBits, value, coordinateCount);
        vertex = {values[previous[0]], values[previous[1]], values[previous[2]],
            values[previous[3]], 1.f - values[previous[4]]};
    }
    input.align();
    const auto lists = bounded(input, 16);
    const auto vertexCount = static_cast<uint32_t>(eye.vertices.size());
    const unsigned vertexBits = deltaWidth(vertexCount);
    for (uint32_t i = 0; i < lists; ++i) {
        const auto texture = input.get(8), topology = input.get(8);
        const auto count = bounded(input, 192000);
        std::vector<uint32_t> indexes;
        if (texture == 0 && topology <= 2) indexes.reserve(count);
        uint32_t prior = 0;
        for (uint32_t j = 0; j < count; ++j) {
            prior = nextIndex(input, vertexBits, prior, vertexCount);
            if (texture == 0 && topology <= 2) indexes.push_back(prior);
        }
        input.align();
        if (topology == 0) {
            eye.triangles.insert(eye.triangles.end(), indexes.begin(), indexes.begin() + indexes.size() / 3 * 3);
        } else {
            for (size_t j = 2; j < indexes.size(); ++j) {
                const uint32_t a = indexes[topology == 2 ? 0 : j - 2], b = indexes[j - 1], c = indexes[j];
                if (a == b || b == c || a == c) continue;
                eye.triangles.push_back(a);
                eye.triangles.push_back(b);
                eye.triangles.push_back(c);
            }
        }
        if (eye.triangles.size() > 600000) throw std::range_error("Trop de triangles");
    }
    return eye;
}
}

std::shared_ptr<const CameraMesh> decodeCameraMesh(const uint8_t* data, size_t size, bool customStereo) {
    // Spherical Video V2 uses a FullBox header, a CRC field and a four-character encoding.
    if (size < 20 || size > inputLimit || word(data) != 0) return {};
    const auto encoding = word(data + 8);
    data += 12;
    size -= 12;
    std::vector<uint8_t> decompressed;
    if (encoding == tag('d', 'f', 'l', '8')) {
        decompressed.resize(inputLimit);
        unsigned long output = static_cast<unsigned long>(decompressed.size());
        unsigned long input = static_cast<unsigned long>(size);
        if (puff(decompressed.data(), &output, data, &input) != 0) return {};
        data = decompressed.data();
        size = output;
    } else if (encoding != tag('r', 'a', 'w', ' ')) return {};
    auto mesh = std::make_shared<CameraMesh>();
    try {
        for (size_t offset = 0; size - offset >= 8;) {
            const auto length = word(data + offset);
            if (length < 8 || length > size - offset) return {};
            if (word(data + offset + 4) == tag('m', 'e', 's', 'h')) {
                if (mesh->count == mesh->eyes.size()) return {};
                auto eye = readEye(data + offset + 8, length - 8);
                if (eye.triangles.empty()) return {};
                mesh->eyes[mesh->count++] = std::move(eye);
            }
            offset += length;
        }
    } catch (const std::range_error&) { return {}; }
    mesh->fullFrameUv = customStereo && mesh->count == 2;
    return mesh->count && (!customStereo || mesh->count == 2) ? mesh : nullptr;
}
