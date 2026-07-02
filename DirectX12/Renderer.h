#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dx12.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <cstdint>
#include <chrono>
#include <unordered_map>
#include "Colors.h"
#include "ResourceManager.h"
#include "DirectXTex.h"
#include "TaskScheduler.h"
#include "Mesh.h"
#include "Vertex.h"
#include "Font.h"
#define MAX_BUCKETS_PER_LAYER 512
#define NUM_LAYERS 4

struct ObjectData {
    DirectX::XMFLOAT2 pos;
    DirectX::XMFLOAT2 size;
    DirectX::XMFLOAT4 color;
	float hasTexture; // 1.0f if the object has a texture, 0.0f otherwise
    float rotation;
    // useAlphaFromRGB: 1.0f = this instance's texture has no usable alpha channel (e.g. a
    // BMFont atlas with outline=0, where the glyph shape is baked into RGB as white-ink-on-
    // black instead of real transparency). The pixel shader derives coverage/alpha from RGB
    // luminance instead of texture.a, and uses this instance's requested `color` for RGB
    // instead of the texture's. 0.0f = normal texture.a-based sampling (repurposes one of
    // the two former alignment-padding floats -- struct size unchanged).
    float useAlphaFromRGB;
    float padding; // still 1 spare float for 16-byte alignment
    // Per-instance UV sub-rect (0..1 of the bound texture): final UV = mesh.uv * uvScale + uvOffset.
    // Appended as its own 16-byte row so the existing layout above is untouched. Identity
    // (offset=0,0 scale=1,1) reproduces the old whole-texture behavior for every existing
    // sprite. Lets text glyphs share ONE unit-quad mesh + the font atlas texture, varying
    // only this per-instance rect -- an entire string batches into one instanced draw call.
    DirectX::XMFLOAT2 uvOffset;
    DirectX::XMFLOAT2 uvScale;
    // Free-form per-effect payload: each effectID's own shader (see ShaderEffect/RegisterEffect)
    // defines what these 4 floats mean -- e.g. a glow shader might read .x as intensity and
    // .yzw as a tint. Prevents ObjectData growing a new named field per effect the way
    // useAlphaFromRGB did for text; the DEFAULT shader (effectID 0) ignores this entirely.
    DirectX::XMFLOAT4 effectParams;
    ObjectData(DirectX::XMFLOAT2 p, DirectX::XMFLOAT2 s, DirectX::XMFLOAT4 c, float hasTex, float rot,
               DirectX::XMFLOAT2 uvOff = { 0.0f, 0.0f }, DirectX::XMFLOAT2 uvScl = { 1.0f, 1.0f },
               float alphaFromRGB = 0.0f, DirectX::XMFLOAT4 effParams = { 0.0f, 0.0f, 0.0f, 0.0f })
        : pos(p), size(s), color(c), hasTexture(hasTex), rotation(rot),
          useAlphaFromRGB(alphaFromRGB), padding(0.0f), uvOffset(uvOff), uvScale(uvScl),
          effectParams(effParams)
    {
    }
};

struct Batch {
    Mesh* mesh = nullptr;
    TextureResource* tex = nullptr;
    uint32_t effectID = 0;   // which PSO (see ShaderEffect) draws this bucket
    float depth = 0.0f;
    // (instance data lives in WorkerLocalSubmissionData, not here -- this is metadata only)
};

// One registered shader/PSO. effectID 0 is always the default (sprites + text) shader,
// registered automatically in Initialize(). Root signature is shared across effects unless
// one is explicitly given -- only give an effect its own if it needs different bound
// resources; a different blend/rasterizer state or VS/PS pair does NOT require a new root sig.
struct ShaderEffect {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

// Per-worker (fiber) submission buffers. Each worker accumulates ObjectData locally.
// At flush time, all worker buffers are merged into the GPU instance buffer (single-threaded).
// This eliminates concurrent vector modification races.
// Indexed by [layer][bucketHandle]. bucketHandle is a small sequential index assigned the
// first time a (mesh,tex,effect) combo is submitted (see Renderer::Submit) -- NOT a hash, so
// there's no collision/probing and no fixed cap (the old hash-into-512-slots-then-linear-probe
// scheme degraded badly since TextureResource::texID was never actually assigned, and had an
// unbounded-probe risk past 512 distinct materials in one layer). Each layer's inner vector
// grows lazily to fit however many distinct materials THAT layer actually uses.
struct alignas(64) WorkerLocalSubmissionData {
    std::array<std::vector<std::vector<ObjectData>>, NUM_LAYERS> buckets;
};

struct CommandContext {
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>> cmdLists;    bool isBusy = false;
};


// Owns every persistent GPU object and the per-frame render/resize logic.
// Public API is the lifecycle: Initialize -> (Render each frame / Resize on WM_SIZE) -> Cleanup.
class Renderer {
public:
    struct FlushTaskContext {
        Renderer* renderer;                         // Access to m_Buckets + constant buffers
        // Indexed simply by bucket handle WITHIN 'layer' (see below) -- each task only ever
        // processes ONE layer now, so these hold just THAT layer's counts/offsets, not a
        // flat all-layers array. (A bucket handle used in two layers is still two entirely
        // separate m_Buckets[layer] vectors, so no cross-layer collision is possible.)
        std::vector<UINT> bucketInstanceCounts;    // Per-bucket instance count, for THIS layer only
        std::vector<UINT> bucketInstanceOffsets;   // Per-bucket ABSOLUTE start index in the buffer, THIS layer only
        int layer;                                   // THIS task processes ONLY this one layer
        int taskIndex;                              // The ID used to index into the pool (0..taskCount-1)
        int startBucket;                            // The range of buckets (within 'layer') this task processes
        int endBucket;
    };
    struct FlushTaskContextPool {
        unsigned hw = std::thread::hardware_concurrency();
        int taskCount = (hw > 1) ? (int)(hw - 1) : 1;
        std::vector<FlushTaskContext> pool;
        std::atomic<size_t> nextIndex{ 0 };

        // One slot per (layer, task) pair -- worst case every layer is active. Empty layers
        // are skipped at dispatch time (see FlushBatchParallel), so unused slots just sit
        // idle; this is a one-time size, not a per-frame cost.
        FlushTaskContextPool() {
            pool.resize((size_t)taskCount * NUM_LAYERS);
        }
        FlushTaskContext* Acquire() {
            size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
            // Add a safety check here if you exceed your pool size
            return &pool[index];
        }

        void Reset() {
            nextIndex.store(0, std::memory_order_relaxed);
        }
    };
    // ---- DAG task wrappers for BeginFrame/PresentFrame -----------------------------------
    // Both context instances are SINGLE, reused every frame -- safe ONLY because frames stay
    // serialized (frame N+1's StartFrame node is never built/fired until frame N's tail node
    // has completed, so the previous frame's context has definitely already been read by the
    // time this frame overwrites it). If frames are ever allowed to overlap, this MUST become
    // a real per-in-flight-frame pool (same shape as FlushTaskContextPool) -- reusing a single
    // instance across genuinely concurrent frames would be a data race on the context fields.
    struct StartFrameContext {
        Renderer* renderer;
        float clearColor[4];
    };
    static void DagCheckpoint(const char* msg) {
        FILE* f = nullptr; fopen_s(&f, "C:\\temp\\dag_checkpoint.log", "w");
        if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    }
    static void StartFrameTask(void* data) {
        auto* ctx = static_cast<StartFrameContext*>(data);
        DagCheckpoint("StartFrameTask: begin (about to call BeginFrame)");
        // TEMP DIAGNOSTIC: Task::Execute() is noexcept, so any exception thrown in here
        // (e.g. ThrowIfFailed) would otherwise hit std::terminate() before its message ever
        // surfaces -- catch + log to a file so the real error is visible, then remove this.
        try { ctx->renderer->BeginFrame(ctx->clearColor); }
        catch (const std::exception& e) {
            FILE* f = nullptr; fopen_s(&f, "C:\\temp\\dag_crash.log", "a");
            if (f) { fprintf(f, "StartFrameTask threw: %s\n", e.what()); fclose(f); }
            throw;
        }
        DagCheckpoint("StartFrameTask: end");
    }
    struct PresentFrameContext {
        Renderer* renderer;
    };
    static void PresentFrameTask(void* data) {
        auto* ctx = static_cast<PresentFrameContext*>(data);
        DagCheckpoint("PresentFrameTask: begin (about to call PresentFrame)");
        try { ctx->renderer->PresentFrame(); }
        catch (const std::exception& e) {
            FILE* f = nullptr; fopen_s(&f, "C:\\temp\\dag_crash.log", "a");
            if (f) { fprintf(f, "PresentFrameTask threw: %s\n", e.what()); fclose(f); }
            throw;
        }
        DagCheckpoint("PresentFrameTask: end");
        // PresentFrame's own logic is UNCHANGED
    }
    StartFrameContext m_StartFrameCtx{};
    PresentFrameContext m_PresentFrameCtx{};
    // ----------------------------------------------------------------------------------------

    void Submit(const Mesh& mesh, TextureResource* tex, DirectX::XMFLOAT2 offset, float width, float height, int zLayer=0, DirectX::XMFLOAT4 color = {1.0f,1.0f,1.0f,1.0f}, float rotation = 0.0f, DirectX::XMFLOAT2 uvOffset = {0.0f,0.0f}, DirectX::XMFLOAT2 uvScale = {1.0f,1.0f}, bool useAlphaFromRGB = false, uint32_t effectID = 0, DirectX::XMFLOAT4 effectParams = {0.0f,0.0f,0.0f,0.0f});
    template<typename... Args>
    void SubmitText(Font& font, float x, float y, const std::string& fmt, 
        float scale = 1.0f,
        DirectX::XMFLOAT4 color = { 1.0f,1.0f,1.0f,1.0f },
        float rotation = 0.0f,
        TextAlign align = TextAlign::Left,
        int zLayer = 2,
        Args&&... args)
    {
        std::string text = std::vformat(fmt, std::make_format_args(args...));
        font.SubmitText(*this, text, x, y, scale, color, rotation, align, zLayer);
    }
    inline Font* GetSysFont() { return &font; }
    // Compiles a VS/PS pair + blend state into a new PSO and returns its effectID (index into
    // m_Effects). effectID 0 is the default sprite/text shader, registered by Initialize().
    // Call AFTER Initialize() (needs m_Device + m_RootSignature). Shares the existing root
    // signature -- only add an override param for a new one if an effect needs different
    // bound resources (most effects just need a different VS/PS/blend, not a new root sig).
    uint32_t RegisterEffect(const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend);
    //void Submit(DirectX::XMFLOAT2 offset, float width, float height, DirectX::XMFLOAT4 color, float rotation);
    static void FlushBatchTask(void* data) {
        FlushTaskContext* ctx = static_cast<FlushTaskContext*>(data);
        Renderer* r = ctx->renderer;
        // TWO DIFFERENT indices, deliberately: frameResourceIdx picks which rotating CPU/GPU
        // sync-slot set to use (allocator, cmdList, constant buffer, instance buffer);
        // backBufferIdx picks which actual swap-chain back buffer / RTV to render into. They
        // are NOT the same value in general -- see m_FrameResourceIndex's declaration comment.
        const int frameResourceIdx = r->m_FrameResourceIndex;
        const int backBufferIdx = r->m_CurrentBackBufferIndex;

        // Each task owns its OWN context (cmd list + allocator for this frame). NEVER touch
        // the shared/main context here -- two threads recording one list = corruption, and
        // resetting the main list would wipe the clear/barrier recorded in BeginFrame.
        // Indexed by (layer, taskIndex): PresentFrame submits these lists LAYER-MAJOR, so a
        // (mesh,tex) pair at zLayer 9 is GUARANTEED to execute on the GPU after everything at
        // zLayer 0, regardless of which task's bucket-range either one happened to hash into.
        const int taskCount = r->m_FlushTaskContextPool.taskCount;
        CommandContext& myCtx = r->m_CommandContextPool[(size_t)ctx->layer * taskCount + ctx->taskIndex];
        auto& alloc = myCtx.allocators[frameResourceIdx];
        auto& cl    = myCtx.cmdLists[frameResourceIdx];

        // Safe to reset: BeginFrame waited on this frame-resource slot's fence before dispatch.
        alloc->Reset();
        cl->Reset(alloc.Get(), r->m_PipelineState.Get());

        // State only -- NO clear, NO back-buffer barrier (those live in the PRE/POST lists).
        ID3D12DescriptorHeap* ppHeaps[] = { r->m_SrvHeap.Get() };
        cl->SetDescriptorHeaps(1, ppHeaps);
        cl->SetGraphicsRootSignature(r->m_RootSignature.Get());

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            r->m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            backBufferIdx, r->m_RTVDescriptorSize); // must match the ACTUAL back buffer, not the sync slot
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // bind only; PRE already cleared it
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)r->m_ClientWidth, (float)r->m_ClientHeight, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)r->m_ClientWidth, (LONG)r->m_ClientHeight };
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor);
        cl->SetGraphicsRootConstantBufferView(0, r->m_ConstantBuffers[frameResourceIdx]->GetGPUVirtualAddress());

        // Tracks the PSO currently bound on THIS list, so we only call SetPipelineState when
        // a bucket's effect actually differs from the previous one -- buckets are already
        // grouped by (mesh,tex,effect) via the bucket key, so same-effect buckets in a row
        // cost nothing extra; a real switch only happens at an effect boundary.
        uint32_t lastEffect = UINT32_MAX;
        const int layer = ctx->layer; // this task's ONE assigned layer -- see the pool-indexing comment above
        for (int i = ctx->startBucket; i < ctx->endBucket; ++i) {
            const Batch& b = r->m_Buckets[layer][i];
            if (b.mesh == nullptr) continue;

            // ctx->bucketInstanceCounts/Offsets hold ONLY this task's layer, indexed directly
            // by bucket handle (no flat all-layers math needed -- each layer's m_Buckets is
            // its own vector, so there's no cross-layer index collision to disambiguate).
            UINT count = ctx->bucketInstanceCounts[i];
            if (count == 0) continue;

            if (b.effectID != lastEffect) {
                cl->SetPipelineState(r->m_Effects[b.effectID].pso.Get());
                lastEffect = b.effectID;
            }

            // ABSOLUTE offset the merge placed this bucket at -- no sequential accumulation,
            // so disjoint buffer regions across layers are addressed correctly.
            UINT64 byteOffset = (UINT64)ctx->bucketInstanceOffsets[i] * sizeof(ObjectData);

            cl->IASetVertexBuffers(0, 1, &b.mesh->vertexBufferView);
            cl->IASetIndexBuffer(&b.mesh->indexBufferView);
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootDescriptorTable(2, b.tex->gpuHandle);
            cl->SetGraphicsRootShaderResourceView(1, r->m_InstanceBuffer[frameResourceIdx]->GetGPUVirtualAddress() + byteOffset);
            cl->DrawIndexedInstanced(b.mesh->indexCount, count, 0, 0, 0);
        }
        cl->Close();
    }
    int  FlushBatchParallel(); // records worker draw lists; returns how many were recorded
    void PresentFrame();
    ResourceManager* GetResourceManager() const;
    void ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task);
    void UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize);
    DirectX::XMFLOAT2 GetScreenSize() const;
    DirectX::XMFLOAT2 GetNDC(float targetX, float targetY);
    void Initialize(HWND hWnd, uint32_t width, uint32_t height, bool useWarp = false);
    void BeginFrame(const float clearColor[4]);
    void Resize(uint32_t width, uint32_t height);
    void Cleanup();
    void SetVSync(bool v) { m_VSync = v; }
    bool VSync() const { return m_VSync; }
    bool IsInitialized() const { return m_IsInitialized; }
    void   UpdateFPS();                 // accumulates frames, logs FPS once per second

    ID3D12GraphicsCommandList* GetCommandList() {
        if (m_CommandList == nullptr) {
            OutputDebugStringA("ERROR: m_CommandList is NULL! Check initialization order.\n");
            // You can put a breakpoint here in Visual Studio
        }
        return m_CommandList.Get();
    }
private:

    struct alignas(256) ConstantBufferData {
        float time;
        float padding1, padding2, padding3;
        DirectX::XMFLOAT2 screenSize;
        float aspectRatio;
        float padding4; // Final padding to maintain 16-byte alignment
    };
    CommandContext* m_CurrentContext;
    bool m_IsRenderTargetState = false; // Start as false
    std::atomic<bool> m_BarrierIssued = false;
    std::vector<CommandContext> m_CommandContextPool;
    // Which zLayers actually had content THIS frame, in ascending order -- filled by
    // FlushBatchParallel, consumed by PresentFrame to submit command lists LAYER-MAJOR
    // (all of layer L's task lists before any of layer L+1's), which is what makes zLayer
    // ordering correct across task boundaries. See FlushBatchTask's pool-indexing comment.
    std::vector<int> m_ActiveLayersThisFrame;
    // How many (task) command lists were ACTUALLY recorded per layer this frame -- may be
    // LESS than taskCount when a layer has few distinct materials. PresentFrame's submission
    // loop must use this, not a blanket taskCount, or it resubmits stale/never-recorded lists.
    std::vector<int> m_TasksDispatchedPerLayer;
    // m_CommandContextPool is sized for NUM_LAYERS up front, but a layer's taskCount worth of
    // allocators/lists are only actually CREATED the first time that layer is used (see
    // ProvisionLayerContexts). Lets NUM_LAYERS stay a generous cap with zero memory cost for
    // layers you never touch, instead of paying for all of them from frame 0.
    std::vector<bool> m_LayerProvisioned;
    void ProvisionLayerContexts(int layer);
    // ---- creation steps: each builds and returns one object (the "recipe" reads as a
    //      sequence of these in Initialize). Members they depend on must be set first. ----
    // Add to private:
    D3D12_INDEX_BUFFER_VIEW ibv;
    // m_Buckets[layer] grows dynamically as new (mesh,tex,effect) combos are discovered --
    // no fixed cap, no hash collisions. m_MaterialHandles[layer] maps a material's key (see
    // GetMaterialID) to its index in m_Buckets[layer], assigned ONCE the first time that
    // material is submitted and PERSISTED across frames (meshes/textures/effects live for the
    // program's lifetime, so there's no need to rediscover them every frame -- only the
    // per-frame INSTANCE data, in WorkerLocalSubmissionData, gets cleared each frame).
    std::vector<Batch> m_Buckets[NUM_LAYERS];
    std::unordered_map<uint64_t, uint32_t> m_MaterialHandles[NUM_LAYERS];

    // Per-worker (fiber) submission buffers. Index by T_Thread::GetCurrent()->qIndex.
    // Heap-allocated to avoid stack overflow (2MB+ of vectors).
    static constexpr int MAX_WORKERS = 32;
    std::unique_ptr<std::array<WorkerLocalSubmissionData, MAX_WORKERS>> m_WorkerLocalStorage;

    static inline const uint32_t indices[] = { 0, 1, 2, 1, 3, 2 };
    Microsoft::WRL::ComPtr<ID3D12Resource> m_IndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState; // effectID 0's PSO (also m_Effects[0])
    std::vector<ShaderEffect> m_Effects;
    // Add to private creation methods:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_TextureResource;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature();
    // vsPath/psPath/blend are explicit (no silent "no blending" default) -- Initialize() and
    // RegisterEffect() both pass them. Everything else about the PSO (input layout, root
    // signature, rasterizer/depth state, RTV format) stays fixed across effects.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreatePipelineState(
        const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend);
    static D3D12_BLEND_DESC DefaultAlphaBlend(); // standard alpha "over" blend, used by effectID 0
    Microsoft::WRL::ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp);
    Microsoft::WRL::ComPtr<ID3D12Device2> CreateDevice(Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter);
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type);
    Microsoft::WRL::ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd, uint32_t width, uint32_t height, uint32_t bufferCount);
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors);
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type);
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CreateCommandList(Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator, D3D12_COMMAND_LIST_TYPE type);
    Microsoft::WRL::ComPtr<ID3D12Fence> CreateFence();
    void CreateInstanceBuffer(ID3D12Device* device, UINT maxInstances);
    uint64_t GetMaterialID(Mesh* mesh, TextureResource* tex, uint32_t effectID);
    HANDLE CreateEventHandle();
    bool   CheckTearingSupport();
    void   UpdateRenderTargetViews();   // (re)creates an RTV for each swap-chain back buffer
    void   EnableDebugLayer();
    void   LogDeviceRemoved();          // dumps DRED breadcrumbs + page fault on device-removed
    // _DEBUG only; MUST run before the device is created
    void CreateConstantBuffers();
    // ---- synchronization helpers (operate on m_CommandQueue / m_Fence / m_FenceEvent) ----
    uint64_t Signal();
    void     WaitForFenceValue(uint64_t value, std::chrono::milliseconds dur = std::chrono::milliseconds::max());
    void     Flush();   // Signal + wait: GPU idle, safe to release/resize resources

    // ============================ persistent state ============================
    static const uint8_t m_NumFrames = 3;
	FlushTaskContextPool m_FlushTaskContextPool;
	std::unique_ptr<ResourceManager> m_ResourceManager;
    Font font;

    // Core devices
    Microsoft::WRL::ComPtr<ID3D12Device2>      m_Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CommandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain4>    m_SwapChain;


    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;


    // Per-frame data (the triple-buffer set)
    Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[m_NumFrames];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocators[m_NumFrames];
    uint64_t m_FrameFenceValues[m_NumFrames] = {};
	Microsoft::WRL::ComPtr<ID3D12Resource> m_UploadHeap; // Temporary buffer for texture upload
    // Pipeline helpers
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList; // PRE list: clear + PRESENT->RT
    // POST list: RENDER_TARGET->PRESENT barrier, executed last each frame.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_PostList;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_PostAllocators[m_NumFrames];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_RTVDescriptorHeap;
    UINT m_RTVDescriptorSize = 0;
    // m_CurrentBackBufferIndex: WHICH SWAP-CHAIN BACK BUFFER to render into this frame --
    // DXGI-controlled (via GetCurrentBackBufferIndex()). ONLY use this for things tied to the
    // actual back buffer resource: m_BackBuffers[x], and the RTV descriptor offset.
    UINT m_CurrentBackBufferIndex = 0;
    // m_FrameResourceIndex: WHICH ROTATING SET of per-frame CPU/GPU-sync resources to use --
    // app-controlled, advanced by exactly 1 (mod m_NumFrames) once per PresentFrame call,
    // INDEPENDENT of the swap chain. Use this for: command allocators (m_CommandAllocators,
    // m_PostAllocators, CommandContext::allocators/cmdLists), m_ConstantBuffers,
    // m_InstanceBuffer/m_MappedData, m_FrameFenceValues. With DXGI_SWAP_EFFECT_FLIP_DISCARD
    // (+ tearing support, both used by this swap chain), GetCurrentBackBufferIndex() is NOT
    // documented to follow a strict round-robin sequence -- tying CPU/GPU sync-slot selection
    // to it risks reusing a sync resource the GPU is still using, or skipping one, if DXGI ever
    // returns a repeated or out-of-order index. Keeping this counter fully app-controlled
    // removes that dependency entirely.
    UINT m_FrameResourceIndex = 0;

    // Synchronization
    Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
    uint64_t m_FenceValue = 0;
    HANDLE   m_FenceEvent = nullptr;
	// Constant buffers (one per frame)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ConstantBuffers[m_NumFrames];

    // You also need to keep the pointer to the "mapped" memory
    // so you can write to it every frame without calling Map/Unmap
    uint8_t* m_MappedConstantBufferData[m_NumFrames];

    // Instance buffers: ONE PER FRAME. A single shared buffer let the CPU overwrite
    // instance data while the GPU was still reading it for an in-flight frame -> GPU
    // hang / "device ran into a problem" (TDR). Per-frame + the BeginFrame fence wait
    // means we only write a buffer the GPU has finished with.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_InstanceBuffer[m_NumFrames];
    void* m_MappedData[m_NumFrames] = {};
    // Config / dimensions
    uint32_t m_ClientWidth = 1280;
    uint32_t m_ClientHeight = 720;
    bool m_VSync = true;
    bool m_TearingSupported = false;
    bool m_UseWarp = false;
    bool m_IsInitialized = false;

    // FPS measurement
    uint64_t m_frameCounter = 0;
    double   m_elapsedSeconds = 0.0;
    std::chrono::high_resolution_clock::time_point m_t0;
    // Last COMPUTED fps value -- updated once/sec inside UpdateFPS()'s averaging window, but
    // the text must be SUBMITTED every frame (this renderer has no persistent draw state --
    // skip a frame's Submit() and it just doesn't draw that frame) or it flickers, visible for
    // one frame per second instead of staying on screen continuously.
    double m_LastFPS = 0.0;

    // Delta time, independent of the FPS-averaging clock above -- see GetFrameTime().
    std::chrono::high_resolution_clock::time_point m_LastFrameTime;
    bool m_FirstFrameTimeCall = true;

public:
    // Seconds elapsed since the LAST call to GetFrameTime() (not since last frame Present --
    // call this once per game-loop iteration, same spot every time, e.g. right after
    // ProcessMessages()/before Submit() calls, so "once per call" lines up with "once per
    // frame"). First call ever returns 0 (no prior timepoint to diff against) instead of some
    // huge bogus value measured from process startup.
    float GetFrameTime() {
        auto now = std::chrono::high_resolution_clock::now();
        if (m_FirstFrameTimeCall) {
            m_LastFrameTime = now;
            m_FirstFrameTimeCall = false;
            return 0.0f;
        }
        float dt = std::chrono::duration<float>(now - m_LastFrameTime).count();
        m_LastFrameTime = now;
        return dt;
    }
};
