#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

inline void checked(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " (Direct3D " + std::to_string(hr) + ")");
}
