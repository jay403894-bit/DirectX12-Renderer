#pragma once
#include <d3d12.h>
#include <wrl/client.h> 
#include <cstdint>      

struct Mesh {
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    uint32_t vertexCount;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    uint32_t indexCount;
    uint32_t meshID;
};