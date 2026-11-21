#pragma once

#include "media.h"
#include <array>

struct Camera {
    XMFLOAT4 rotation{0, 0, 0, 1};
    float left = -1, right = 1, bottom = -0.56f, top = 0.56f;
    XMFLOAT3 gridEye{0, 1.15f, 0};
};

class Renderer {
public:
    explicit Renderer(const LUID* adapter = nullptr);
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    void draw(ID3D11RenderTargetView* target, unsigned width, unsigned height, const Camera& camera,
              const Media& media, const ViewSettings& settings, XMFLOAT4 orientation, int eye, bool srgb);
    void blit(ID3D11RenderTargetView* target, unsigned width, unsigned height, ID3D11ShaderResourceView* source,
              bool srgb, const XMFLOAT2* cursor = nullptr);
    std::vector<uint8_t> luma(const Media& media, unsigned width, unsigned height);

private:
    struct Constants;
    struct MeshBuffers {
        ComPtr<ID3D11Buffer> vertices, indices;
        unsigned count = 0;
    };
    void drawMesh(ID3D11RenderTargetView* target, unsigned width, unsigned height, ID3D11ShaderResourceView* source,
                  const Constants& constants, const std::shared_ptr<const CameraMesh>& geometry, unsigned eye, bool srgb);
    void render(ID3D11RenderTargetView* target, unsigned width, unsigned height,
                ID3D11ShaderResourceView* source, const Constants& constants);
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11VertexShader> meshVertex_;
    ComPtr<ID3D11PixelShader> meshPixel_;
    ComPtr<ID3D11InputLayout> meshLayout_;
    ComPtr<ID3D11RasterizerState> meshRasterizer_;
    std::shared_ptr<const CameraMesh> cachedGeometry_;
    std::array<MeshBuffers, 2> meshBuffers_;
};
