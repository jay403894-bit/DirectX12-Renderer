#include "../include/ResourceManager.h"
#include "../include/Helpers.h"
#include <DirectXTex.h>   // ScratchImage/LoadFromWICFile -- only this .cpp touches DirectXTex types

ResourceManager::ResourceManager(ID3D12Device* device, ID3D12DescriptorHeap* sharedSrvHeap)
    : m_Device(device), m_SrvHeap(sharedSrvHeap) {}
void ResourceManager::SetSrvHeap(ID3D12DescriptorHeap* heap) {
    m_SrvHeap = heap; // If m_SrvHeap is a ComPtr, this will AddRef/Release correctly
}
TextureHandle ResourceManager::LoadTexture(const std::wstring& filename, ID3D12GraphicsCommandList* cmdList) {
    // 1. If it's already loaded, just return its handle
    auto cached = m_TextureIndexByKey.find(filename);
    if (cached != m_TextureIndexByKey.end()) {
        return TextureHandle{ cached->second };
    }

    // Load the image (WIC needs COM initialized on this thread -- done in wWinMain).
    DirectX::ScratchImage image;
    ThrowIfFailed(DirectX::LoadFromWICFile(filename.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image));
    const auto& meta = image.GetMetadata();

    // Describe the GPU texture. DepthOrArraySize and MipLevels MUST be set (>=1) -- leaving
    // them 0 made CreateCommittedResource fail with E_INVALIDARG and (with the ignored
    // HRESULT) produced a null resource that the GPU later faulted on.
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = meta.width;
    texDesc.Height = (UINT)meta.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = meta.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    // Create the DEFAULT-heap texture in COPY_DEST (we're about to copy into it).
    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newResource)));

    // --- Upload: stage the pixels in an UPLOAD buffer, then CopyTextureRegion into the
    //     DEFAULT texture, then transition it to be sampleable. ---
    UINT64 requiredSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSizeInBytes;
    m_Device->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &requiredSize);

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)));

    const BYTE* src = image.GetPixels();
    const UINT64 srcRowPitch = image.GetImages()->rowPitch;
    BYTE* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT row = 0; row < numRows; ++row) {
        memcpy(mapped + layout.Offset + row * layout.Footprint.RowPitch,
               src + row * srcRowPitch, static_cast<size_t>(rowSizeInBytes));
    }
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = newResource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        newResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    // Keep the upload buffer alive until the caller has executed+waited on cmdList.
    m_UploadBuffers.push_back(uploadBuffer);

    // 2. Setup the SRV
    UINT descriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        m_SrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_CurrentDescriptorIndex,
        descriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    // Create the view using newResource
    m_Device->CreateShaderResourceView(newResource.Get(), &srvDesc, cpuHandle);

    // 3. Create the TextureResource
    TextureResource tex;
    tex.resource = newResource;
    tex.gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_SrvHeap->GetGPUDescriptorHandleForHeapStart(),
        m_CurrentDescriptorIndex,
        descriptorSize);

    // 4. Store (by index -- appended, never erased, so this index is a stable handle for the
    // program's lifetime) and return the handle.
    uint32_t index = (uint32_t)m_TextureStorage.size();
    m_TextureStorage.push_back(std::move(tex));
    m_TextureIndexByKey[filename] = index;

    m_CurrentDescriptorIndex++;

    return TextureHandle{ index };
}

TextureHandle ResourceManager::GetTexture(const std::wstring& filename) {
    auto it = m_TextureIndexByKey.find(filename);
    if (it != m_TextureIndexByKey.end()) {
        return TextureHandle{ it->second };
    }
    return TextureHandle{}; // invalid handle -- caller checks IsValid()
}

TextureHandle ResourceManager::CreateSolidColorTexture(ID3D12GraphicsCommandList* cmdList,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Synthetic cache key encodes the color so different solid colors don't collide/alias.
    // Real textures are always loaded from an actual file path (see LoadTexture), so this
    // distinctive, non-path-shaped prefix can't collide with one.
    wchar_t key[32];
    swprintf_s(key, L"__solid_%02x%02x%02x%02x__", r, g, b, a);
    std::wstring keyStr(key);
    auto cached = m_TextureIndexByKey.find(keyStr);
    if (cached != m_TextureIndexByKey.end()) {
        return TextureHandle{ cached->second };
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newResource)));

    UINT64 requiredSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSizeInBytes;
    m_Device->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &requiredSize);

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)));

    uint8_t pixel[4] = { r, g, b, a };
    BYTE* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped + layout.Offset, pixel, sizeof(pixel));
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = newResource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        newResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    m_UploadBuffers.push_back(uploadBuffer);

    UINT descriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        m_SrvHeap->GetCPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Device->CreateShaderResourceView(newResource.Get(), &srvDesc, cpuHandle);

    TextureResource tex;
    tex.resource = newResource;
    tex.gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);

    uint32_t index = (uint32_t)m_TextureStorage.size();
    m_TextureStorage.push_back(std::move(tex));
    m_TextureIndexByKey[keyStr] = index;
    m_CurrentDescriptorIndex++;
    return TextureHandle{ index };
}
Mesh ResourceManager::CreateMesh(const Vertex* vertices, uint32_t vCount, const uint32_t* indices, uint32_t iCount) {
    Mesh mesh;
    // ... [Copy your existing CreateMesh implementation code here] ...
    mesh.vertexCount = vCount;
    mesh.indexCount = iCount;
    mesh.meshID = meshCtr;
    meshCtr++;
    // --- Vertex Buffer ---
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC vBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Vertex) * vCount);

    ThrowIfFailed(m_Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &vBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.vertexBuffer)));

    void* pVData;
    mesh.vertexBuffer->Map(0, nullptr, &pVData);
    memcpy(pVData, vertices, sizeof(Vertex) * vCount);
    mesh.vertexBuffer->Unmap(0, nullptr);

    mesh.vertexBufferView.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vertexBufferView.StrideInBytes = sizeof(Vertex);
    mesh.vertexBufferView.SizeInBytes = sizeof(Vertex) * vCount;

    // --- Index Buffer ---
    CD3DX12_RESOURCE_DESC iBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t) * iCount);

    ThrowIfFailed(m_Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &iBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.indexBuffer)));

    void* pIData;
    mesh.indexBuffer->Map(0, nullptr, &pIData);
    memcpy(pIData, indices, sizeof(uint32_t) * iCount);
    mesh.indexBuffer->Unmap(0, nullptr);

    mesh.indexBufferView.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    mesh.indexBufferView.SizeInBytes = sizeof(uint32_t) * iCount;
    uint32_t newID = (uint32_t)m_Meshes.size();
    m_Meshes.push_back({ mesh, newID });
    return mesh;
}