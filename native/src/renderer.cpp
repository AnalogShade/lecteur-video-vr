#include "renderer.h"
#include "camera-mesh.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr const char* shader = R"(
cbuffer Views : register(b0) {
    float4 frustum;
    float4 head;
    float4 inversePanorama;
    float4 region;
    float4 settings;
    float4 flags;
    float4 cameraField;
};
Texture2D image : register(t0);
SamplerState samplerLinear : register(s0);
struct Vertex {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Vertex vertex(uint i : SV_VertexID) {
    Vertex o;
    o.uv = float2((i << 1) & 2, i & 2);
    o.position = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
    return o;
}
float3 rotate(float4 q, float3 v) {
    return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
float3 linearize(float3 rgb) {
    return lerp(rgb / 12.92, pow((rgb + 0.055) / 1.055, 2.4), step(0.04045, rgb));
}
float4 display(float4 rgb) {
    return float4(flags.y > 0 ? linearize(rgb.rgb) : rgb.rgb, 1);
}
struct MeshPoint {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};
Vertex meshVertex(MeshPoint meshPoint) {
    Vertex result;
    float4 localToWorld = float4(-inversePanorama.xyz, inversePanorama.w);
    float4 worldToEye = float4(-head.xyz, head.w);
    float3 p = rotate(worldToEye, rotate(localToWorld, meshPoint.position));
    float depth = -p.z;
    float2 size = float2(frustum.y - frustum.x, frustum.w - frustum.z);
    float2 center = float2(frustum.y + frustum.x, frustum.w + frustum.z);
    float2 screen = (2 * p.xy * settings.y - center * depth) / size;
    result.position = float4(screen, depth - .007, depth);
    result.uv = meshPoint.uv;
    return result;
}
float4 meshPixel(Vertex input) : SV_TARGET {
    uint w, h;
    image.GetDimensions(w, h);
    float2 margin = .5 / float2(w, h);
    float2 uv = input.uv;
    uv.x += flags.z;
    float2 packed = clamp(uv * region.xy + region.zw, region.zw + margin, region.zw + region.xy - margin);
    return display(image.Sample(samplerLinear, packed));
}
float2 cubeAtlas(float3 ray) {
    float3 a = abs(ray);
    float2 face, local;
    if (a.x >= a.y && a.x >= a.z) {
        face = float2(ray.x > 0 ? 0 : 1, 0);
        local = float2(ray.x > 0 ? ray.z : -ray.z, -ray.y) / a.x;
    } else if (a.y >= a.z) {
        face = ray.y > 0 ? float2(2, 0) : float2(0, 1);
        local = float2(ray.x, ray.y > 0 ? ray.z : -ray.z) / a.y;
    } else {
        face = float2(ray.z < 0 ? 1 : 2, 1);
        local = float2(ray.z < 0 ? ray.x : -ray.x, -ray.y) / a.z;
    }
    uint w, h;
    image.GetDimensions(w, h);
    float2 inset = float2(3, 2) * .5 / (float2(w, h) * region.xy);
    return (face + clamp(.5 + local * .5, inset, 1 - inset)) / float2(3, 2);
}
float4 pixel(Vertex input) : SV_TARGET {
    static const float PI = 3.14159265359;
    static const float4 background = float4(0.035, 0.043, 0.075, 1);
    if (flags.x < 0.0) {
        float4 color = image.Sample(samplerLinear, input.uv);
        if (flags.w > 0.0) {
            const float distance = length((input.uv - cameraField.xy) * cameraField.zw);
            color.rgb = lerp(color.rgb, float3(.13, .07, .24), 1 - smoothstep(8, 9, distance));
            color.rgb = lerp(color.rgb, float3(.85, .75, 1), 1 - smoothstep(5, 6, distance));
        }
        return display(color);
    }
    if (flags.x == 0.0) return display(background);
    float3 ray = normalize(float3(lerp(frustum.x, frustum.y, input.uv.x),
                                  lerp(frustum.w, frustum.z, input.uv.y), -1));
    ray = rotate(inversePanorama, rotate(head, ray));
    float2 uv;
    if (settings.x == 0) {
        if (ray.z >= 0) return display(background);
        float aspect = settings.z;
        float2 extent = float2(min(.95, .55 * aspect), min(.55, .95 / aspect));
        uv = .5 + float2(ray.x, -ray.y) / -ray.z / (2 * extent) / settings.y;
    } else {
        ray = normalize(float3(ray.xy / settings.y, ray.z));
        if (settings.x == 3) {
            float theta = acos(clamp(-ray.z, -1.0, 1.0));
            if (theta > cameraField.x / 2) return display(background);
            float r = length(ray.xy);
            uv = .5 + (r > 0.00001 ? float2(ray.x, -ray.y) / r : float2(0, 0)) * (theta / cameraField.x);
            if (settings.z > 1) uv.x = .5 + (uv.x - .5) / settings.z;
            else uv.y = .5 + (uv.y - .5) * settings.z;
        } else if (settings.x == 4) {
            uv = cubeAtlas(ray);
        } else {
            uv = float2(.5 + (atan2(ray.x, -ray.z) - cameraField.z) / cameraField.x,
                        .5 - (asin(clamp(ray.y, -1.0, 1.0)) - cameraField.w) / cameraField.y);
        }
    }
    if (settings.x != 4) uv.x += flags.z;
    if (settings.x == 2 && cameraField.x >= 2 * PI - .0001) uv.x = frac(uv.x);
    if (any(uv < 0) || any(uv > 1)) return display(background);
    uint w, h;
    image.GetDimensions(w, h);
    float2 halfPixel = .5 / float2(w, h);
    float2 packed = clamp(uv * region.xy + region.zw, region.zw + halfPixel, region.zw + region.xy - halfPixel);
    return display(image.Sample(samplerLinear, packed));
}
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> bytes, error;
    HRESULT hr = D3DCompile(shader, strlen(shader), "LecteurVideoVR", nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytes, &error);
    if (FAILED(hr) && error) throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));
    checked(hr, "Compilation du shader");
    return bytes;
}
}

struct Renderer::Constants {
    XMFLOAT4 frustum{0, 0, 0, 0};
    XMFLOAT4 head{0, 0, 0, 1};
    XMFLOAT4 inversePanorama{0, 0, 0, 1};
    XMFLOAT4 region{1, 1, 0, 0};
    XMFLOAT4 settings{0, 1, 1, 0};
    XMFLOAT4 flags{0, 0, 0, 0};
    XMFLOAT4 cameraField{XM_PI, XM_PI, 0, 0};
};

Renderer::Renderer(const LUID* luid) {
    ComPtr<IDXGIAdapter1> adapter;
    if (luid) {
        ComPtr<IDXGIFactory1> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Création du service DXGI");
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC1 desc{};
            checked(adapter->GetDesc1(&desc), "Identification de la carte graphique");
            if (desc.AdapterLuid.HighPart == luid->HighPart && desc.AdapterLuid.LowPart == luid->LowPart) break;
            adapter.Reset();
        }
        if (!adapter) throw std::runtime_error("La carte graphique demandée par OpenXR est introuvable.");
    }
    D3D_FEATURE_LEVEL feature = D3D_FEATURE_LEVEL_11_0;
    checked(D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        &feature, 1, D3D11_SDK_VERSION, &device_, nullptr, &context_), "Création du périphérique graphique");
    auto vs = compile("vertex", "vs_5_0"), ps = compile("pixel", "ps_5_0");
    checked(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_), "Shader vertex");
    checked(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_), "Shader pixel");
    const auto meshVs = compile("meshVertex", "vs_5_0"), meshPs = compile("meshPixel", "ps_5_0");
    checked(device_->CreateVertexShader(meshVs->GetBufferPointer(), meshVs->GetBufferSize(), nullptr, &meshVertex_),
        "Shader des lentilles");
    checked(device_->CreatePixelShader(meshPs->GetBufferPointer(), meshPs->GetBufferSize(), nullptr, &meshPixel_),
        "Pixels des lentilles");
    const D3D11_INPUT_ELEMENT_DESC meshFormat[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MeshVertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    checked(device_->CreateInputLayout(meshFormat, 2, meshVs->GetBufferPointer(), meshVs->GetBufferSize(), &meshLayout_),
        "Format du maillage");
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    checked(device_->CreateRasterizerState(&rasterizer, &meshRasterizer_), "Rendu du maillage");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Constants);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    checked(device_->CreateBuffer(&desc, nullptr, &constants_), "Paramètres de projection");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    checked(device_->CreateSamplerState(&sampler, &sampler_), "Filtre vidéo");
}

void Renderer::draw(ID3D11RenderTargetView* target, unsigned width, unsigned height, const Camera& camera,
                    const Media& media, const ViewSettings& view, XMFLOAT4 orientation, int eye, bool srgb) {
    Constants c;
    c.frustum = {camera.left, camera.right, camera.bottom, camera.top};
    c.head = camera.rotation;
    XMStoreFloat4(&c.inversePanorama, XMQuaternionConjugate(XMLoadFloat4(&orientation)));
    int index = view.swapEyes ? 1 - eye : eye;
    float aspect = media.aspect();
    if (view.stereo == Stereo::SideBySide) { c.region = {.5f, 1.f, index * .5f, 0}; aspect *= .5f; }
    if (view.stereo == Stereo::TopBottom) { c.region = {1.f, .5f, 0, index * .5f}; aspect *= 2.f; }
    c.settings = {static_cast<float>(view.projection), view.zoom, aspect, 0};
    const float separation = view.stereo == Stereo::Mono ? 0.f : view.convergence * (eye == 0 ? -1.f : 1.f);
    c.flags = {media.ready() ? 1.f : 0.f, srgb ? 1.f : 0.f, separation, 0};
    c.cameraField = {XMConvertToRadians(fieldOfView(view)), XMConvertToRadians(view.verticalFov),
        XMConvertToRadians(view.centerYaw), XMConvertToRadians(view.centerPitch)};
    if (view.projection == Projection::Calibrated && view.mesh && media.ready()) {
        if (view.mesh->fullFrameUv) {
            c.region = {1, 1, 0, 0};
            c.flags.z *= .5f;
        }
        const unsigned meshEye = view.stereo == Stereo::Mono ? 0u : static_cast<unsigned>(index);
        drawMesh(target, width, height, media.image(), c, view.mesh, meshEye, srgb);
        return;
    }
    render(target, width, height, media.image(), c);
}

void Renderer::drawMesh(ID3D11RenderTargetView* target, unsigned width, unsigned height, ID3D11ShaderResourceView* source,
                        const Constants& data, const std::shared_ptr<const CameraMesh>& geometry, unsigned eye, bool srgb) {
    if (cachedGeometry_ != geometry) {
        std::array<MeshBuffers, 2> buffers;
        for (unsigned i = 0; i < geometry->count; ++i) {
            const auto& mesh = geometry->eyes[i];
            const auto upload = [&](const void* pixels, UINT size, UINT kind, ComPtr<ID3D11Buffer>& buffer) {
                D3D11_BUFFER_DESC desc{size, D3D11_USAGE_IMMUTABLE, kind, 0, 0, 0};
                D3D11_SUBRESOURCE_DATA sourceData{pixels, 0, 0};
                checked(device_->CreateBuffer(&desc, &sourceData, &buffer), "Maillage des lentilles");
            };
            upload(mesh.vertices.data(), static_cast<UINT>(mesh.vertices.size() * sizeof(MeshVertex)),
                D3D11_BIND_VERTEX_BUFFER, buffers[i].vertices);
            upload(mesh.triangles.data(), static_cast<UINT>(mesh.triangles.size() * sizeof(uint32_t)),
                D3D11_BIND_INDEX_BUFFER, buffers[i].indices);
            buffers[i].count = static_cast<unsigned>(mesh.triangles.size());
        }
        meshBuffers_ = std::move(buffers);
        cachedGeometry_ = geometry;
    }
    const auto& mesh = meshBuffers_[geometry->count == 2 ? eye : 0];
    const float dark[] = {srgb ? .0027f : .035f, srgb ? .0034f : .043f, srgb ? .0066f : .075f, 1};
    context_->ClearState();
    context_->ClearRenderTargetView(target, dark);
    const D3D11_VIEWPORT area{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context_->RSSetViewports(1, &area);
    context_->RSSetState(meshRasterizer_.Get());
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->IASetInputLayout(meshLayout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(MeshVertex), offset = 0;
    context_->IASetVertexBuffers(0, 1, mesh.vertices.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(mesh.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &data, 0, 0);
    context_->VSSetShader(meshVertex_.Get(), nullptr, 0);
    context_->PSSetShader(meshPixel_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->DrawIndexed(mesh.count, 0, 0);
    ID3D11ShaderResourceView* off = nullptr;
    context_->PSSetShaderResources(0, 1, &off);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

void Renderer::blit(ID3D11RenderTargetView* target, unsigned width, unsigned height, ID3D11ShaderResourceView* source,
                    bool srgb, const XMFLOAT2* cursor) {
    Constants c;
    c.flags = {-1, srgb ? 1.f : 0.f, 0, 0};
    if (cursor) {
        c.flags.w = 1;
        c.cameraField = {cursor->x, cursor->y, static_cast<float>(width), static_cast<float>(height)};
    }
    render(target, width, height, source, c);
}

void Renderer::render(ID3D11RenderTargetView* target, unsigned width, unsigned height,
                      ID3D11ShaderResourceView* source, const Constants& data) {
    context_->ClearState();
    const D3D11_VIEWPORT area{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context_->RSSetViewports(1, &area);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &data, 0, 0);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* off = nullptr;
    context_->PSSetShaderResources(0, 1, &off);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

std::vector<uint8_t> Renderer::luma(const Media& media, unsigned width, unsigned height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> image, staging;
    ComPtr<ID3D11RenderTargetView> target;
    checked(device_->CreateTexture2D(&desc, nullptr, &image), "Échantillon vidéo");
    checked(device_->CreateRenderTargetView(image.Get(), nullptr, &target), "Surface de détection");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checked(device_->CreateTexture2D(&desc, nullptr, &staging), "Lecture de l’échantillon");
    blit(target.Get(), width, height, media.image(), false);
    context_->CopyResource(staging.Get(), image.Get());
    std::vector<uint8_t> values(size_t(width) * height);
    D3D11_MAPPED_SUBRESOURCE sample{};
    checked(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &sample), "Lecture des pixels de détection");
    for (unsigned y = 0; y < height; ++y) {
        const auto* rgb = static_cast<const uint8_t*>(sample.pData) + y * sample.RowPitch;
        for (unsigned x = 0; x < width; ++x, rgb += 4)
            values[y * width + x] = static_cast<uint8_t>((54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8);
    }
    context_->Unmap(staging.Get(), 0);
    return values;
}
