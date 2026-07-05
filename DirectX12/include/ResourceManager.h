#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12.h>
#include "Mesh.h"
#include "Vertex.h"
struct TextureResource {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle; // For creating the view
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle; // For binding to the command list
    uint32_t texID = 0;
};
// Lightweight, cheap-to-copy reference to a texture -- an INDEX into ResourceManager's own
// storage, never a pointer. Client code (BatchItem, ParticleEffectPool, etc.) carries this
// around by value; only code that actually needs to bind/read the texture calls Resolve().
// Decouples every caller from TextureResource's layout/lifetime -- ResourceManager is free to
// change how/where it stores textures internally without any client holding a stale reference.
struct TextureHandle {
    uint32_t id = UINT32_MAX;
    bool IsValid() const { return id != UINT32_MAX; }
    bool operator==(const TextureHandle& other) const { return id == other.id; }
};
struct MeshResource {
    Mesh mesh; // Your existing Mesh struct
    uint32_t meshID = 0;
};
class ResourceManager {
public:
    ResourceManager(ID3D12Device* device, ID3D12DescriptorHeap* sharedSrvHeap);
    void SetSrvHeap(ID3D12DescriptorHeap* heap);
    // Loads a texture (or returns the cached handle). The upload is RECORDED into cmdList --
    // the caller must Execute that list and wait for the GPU before the texture is usable.
    // A manager with only a device cannot upload anything to the GPU; it needs a command
    // list to record the CPU->GPU copy. That missing piece is what broke the renderer.
    TextureHandle LoadTexture(const std::wstring& filename, ID3D12GraphicsCommandList* cmdList);
    // Invalid handle (IsValid() == false) if filename hasn't been loaded.
    TextureHandle GetTexture(const std::wstring& filename);
    // Same upload path as LoadTexture, minus the WIC file load -- a 1x1 RGBA texture of the
    // given color. Cached under a synthetic key so repeated calls with the same color are free
    // after the first. Used as the particle system's untextured-pool fallback (draws flat-
    // colored quads through the SAME textured draw PSO, no separate no-texture shader variant).
    TextureHandle CreateSolidColorTexture(ID3D12GraphicsCommandList* cmdList,
        uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255);
    // Resolves a handle to the real GPU resource data. Only code that actually needs to bind
    // the texture (Renderer2D::FlushBatchTask, RendererCore::RecordParticleDraw) should call
    // this -- everything else should just carry the handle around by value.
    const TextureResource& Resolve(TextureHandle handle) const { return m_TextureStorage[handle.id]; }
    // Call AFTER the upload command list has been executed and waited on. Frees the
    // temporary upload buffers (they must stay alive until the GPU finishes the copy).
    void ReleaseUploadBuffers() { m_UploadBuffers.clear(); }
    Mesh CreateMesh(const Vertex* vertices, uint32_t vCount, const uint32_t* indices, uint32_t iCount);
private:
    int meshCtr = 0;
    ID3D12Device* m_Device;
    ID3D12DescriptorHeap* m_SrvHeap; // Just a pointer, not a ComPtr
    // Textures live here by INDEX -- only ever appended to, never erased, so a TextureHandle
    // stays valid for the program's lifetime once issued. The key map below is purely a
    // filename/synthetic-key -> index cache so repeated loads of the same texture are free.
    std::vector<TextureResource> m_TextureStorage;
    std::unordered_map<std::wstring, uint32_t> m_TextureIndexByKey;
    std::vector<MeshResource> m_Meshes;
    // Upload heaps are read by the GPU during the copy, so they must outlive the recorded
    // command list until it has finished executing. Held here, freed by ReleaseUploadBuffers.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_UploadBuffers;
    UINT m_CurrentDescriptorIndex = 0;
};
