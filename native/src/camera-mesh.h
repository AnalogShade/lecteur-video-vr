#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct MeshVertex { float x, y, z, u, v; };

struct EyeMesh {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> triangles;
};

struct CameraMesh {
    std::array<EyeMesh, 2> eyes;
    unsigned count = 0;
    bool fullFrameUv = false;
};

std::shared_ptr<const CameraMesh> decodeCameraMesh(const uint8_t* data, size_t size, bool customStereo);
