#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12.h>
#include <DirectXTex.h>
#include "Mesh.h"
#include "Vertex.h"
struct TextureResource {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle; // For creating the view
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle; // For binding to the command list
    uint32_t texID = 0;
};
struct MeshResource {
    Mesh mesh; // Your existing Mesh struct
    uint32_t meshID = 0;
};
class ResourceManager {
public:
    ResourceManager(ID3D12Device* device, ID3D12DescriptorHeap* sharedSrvHeap);
    void SetSrvHeap(ID3D12DescriptorHeap* heap);
    // Loads a texture (or returns the cached one). The upload is RECORDED into cmdList --
    // the caller must Execute that list and wait for the GPU before the texture is usable.
    // A manager with only a device cannot upload anything to the GPU; it needs a command
    // list to record the CPU->GPU copy. That missing piece is what broke the renderer.
    TextureResource* LoadTexture(const std::wstring& filename, ID3D12GraphicsCommandList* cmdList);
    TextureResource* GetTexture(const std::wstring& filename);
    // Call AFTER the upload command list has been executed and waited on. Frees the
    // temporary upload buffers (they must stay alive until the GPU finishes the copy).
    void ReleaseUploadBuffers() { m_UploadBuffers.clear(); }
    Mesh CreateMesh(const Vertex* vertices, uint32_t vCount, const uint32_t* indices, uint32_t iCount);
private:
    int texCtr = 0;
    int meshCtr = 0;
    ID3D12Device* m_Device;
    ID3D12DescriptorHeap* m_SrvHeap; // Just a pointer, not a ComPtr
    std::unordered_map<std::wstring, TextureResource> m_Textures;
    std::vector<MeshResource> m_Meshes;
    // Upload heaps are read by the GPU during the copy, so they must outlive the recorded
    // command list until it has finished executing. Held here, freed by ReleaseUploadBuffers.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_UploadBuffers;
    UINT m_CurrentDescriptorIndex = 0;
};