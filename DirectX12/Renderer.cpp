#include "Renderer.h"
#include "Helpers.h"      // ThrowIfFailed
#include "Event.h"        // T_Threads::Event (SignalAll) for the fence-wait bridge
#include <T_Thread.h>     // T_Thread::GetCurrent()->qIndex for worker-local storage
#include <algorithm>      // std::max
#include <cassert>
#include <cstdio>         // swprintf_s
#include <DirectXTex.h>

using namespace Microsoft::WRL;
static const D3D12_INPUT_ELEMENT_DESC inputLayoutDesc[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
// ===========================================================================
// Public lifecycle
// ===========================================================================
ResourceManager* Renderer::GetResourceManager() const { return m_ResourceManager.get(); }
// The whole DX12 bring-up, in dependency order. Read top-to-bottom: each line
// produces an object the next line needs. THIS is the "recipe" you wanted to be
// able to parse and memorize.
void Renderer::Initialize(HWND hWnd, uint32_t width, uint32_t height, bool useWarp)
{
	m_ClientWidth = width;
	m_ClientHeight = height;
	m_UseWarp = useWarp;
	EnableDebugLayer();                 // must precede ANY device work
	m_TearingSupported = CheckTearingSupport();

	m_t0 = std::chrono::high_resolution_clock::now();
	ComPtr<IDXGIAdapter4> adapter = GetAdapter(m_UseWarp);   // 1. pick a GPU
	m_Device = CreateDevice(adapter);                 // 2. the device
	m_RootSignature = CreateRootSignature(); // Defines the "interface" for your shader
	// effectID 0: the default sprite/text shader. Registered here so Submit()'s default
	// effectID=0 and FlushBatchTask's m_Effects[b.effectID] lookup both resolve correctly.
	m_PipelineState = CreatePipelineState(L"VertexShader.cso", L"PixelShader.cso", DefaultAlphaBlend());
	m_Effects.push_back({ m_PipelineState });
	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
	CreateInstanceBuffer(m_Device.Get(), 65536);  // 64K instances max per frame
	// Allocate per-worker submission buffers (heap-allocated to avoid stack overflow)
	m_WorkerLocalStorage = std::make_unique<std::array<WorkerLocalSubmissionData, MAX_WORKERS>>();

	// 2. Create the Resource Desc as an lvalue
	CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(indices));
	// One context SLOT per (layer, task) pair -- see FlushBatchTask's pool-indexing comment
	// for why zLayer ordering across tasks requires this instead of one context per task.
	// Slots start EMPTY (no allocators/lists yet) -- ProvisionLayerContexts creates a layer's
	// worth lazily, the first time that layer is actually used (called from
	// FlushBatchParallel). This lets NUM_LAYERS stay a generous cap with zero memory cost for
	// layers the game never touches, instead of eagerly paying for all of them at startup.
	const int taskCount = m_FlushTaskContextPool.taskCount;
	m_CommandContextPool.resize((size_t)taskCount * NUM_LAYERS);
	m_LayerProvisioned.assign(NUM_LAYERS, false);
	CreateConstantBuffers(); // Creates a buffer for each frame (CPU->GPU)
	m_CommandQueue = CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT); // 3. submission queue
	m_SwapChain = CreateSwapChain(hWnd, m_ClientWidth, m_ClientHeight, m_NumFrames); // 4. back buffers

	// 3. Pass those variables instead of the temporary function calls
	m_Device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_IndexBuffer)
	);
	ibv.BufferLocation = m_IndexBuffer->GetGPUVirtualAddress();
	ibv.Format = DXGI_FORMAT_R32_UINT; // Each index is a 32-bit unsigned integer
	ibv.SizeInBytes = sizeof(indices);
	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
	m_FrameResourceIndex = 0; // app-controlled sync-slot counter, independent of the swap chain

	m_RTVDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m_NumFrames); // 5. RTV heap
	m_RTVDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	UpdateRenderTargetViews();                               // 6. an RTV per back buffer

	// ADD THIS:
	m_SrvHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256); // 256 is an example size
	m_ResourceManager = std::make_unique<ResourceManager>(m_Device.Get(), m_SrvHeap.Get());

	for (int i = 0; i < m_NumFrames; ++i)                    // 7. one allocator per frame
		m_CommandAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);

	m_CommandList = CreateCommandList(                       // 8. the command list (PRE)
		m_CommandAllocators[m_FrameResourceIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	// POST list (RENDER_TARGET->PRESENT barrier), one allocator per frame.
	for (int i = 0; i < m_NumFrames; ++i)
		m_PostAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_PostList = CreateCommandList(
		m_PostAllocators[m_FrameResourceIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	// Debug names for DRED breadcrumbs.
	m_CommandQueue->SetName(L"MainQueue");
	m_CommandList->SetName(L"PRE (clear+barrier)");
	m_PostList->SetName(L"POST (present barrier)");

	m_Fence = CreateFence();                            // 9. CPU/GPU sync
	m_FenceEvent = CreateEventHandle();

	// --- Texture upload ---
	// LoadTexture RECORDS a copy into m_CommandList, so the list must exist (it was just
	// created above) AND be in the recording state. CreateCommandList returns it Closed,
	// so Reset it first. After recording, Close + execute on the queue + Flush so the GPU
	// actually performs the copy and finishes before we build the SRV / start rendering
	// (Flush also keeps m_UploadHeap, which the copy reads from, alive until it completes).
	m_CommandList->Reset(m_CommandAllocators[m_FrameResourceIndex].Get(), nullptr);
	ThrowIfFailed(m_CommandList->Close());
	ID3D12CommandList* uploadLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(1, uploadLists);
	Flush();                              // GPU finished the copy...
	m_ResourceManager->ReleaseUploadBuffers(); // ...so the staging buffers are safe to free

	// Renderer's own built-in Font (used by UpdateFPS()'s SubmitText convenience wrapper).
	// MUST come after everything above: Font::Load() needs m_ResourceManager (texture load)
	// and calls ExecuteUploadCommand(), which does m_CommandList->Reset(...) -- both are only
	// valid from this point on. Loading it any earlier derefs a null m_CommandList/m_ResourceManager.
	font.Load(ExeRelative(L"font.fnt"), ExeRelative(L"font.png"), *this);

	m_IsInitialized = true;
}

// Lazily creates the taskCount allocator/list groups for ONE layer, the first time that
// layer is actually used (called from FlushBatchParallel, on the main thread, before any
// worker touches m_CommandContextPool this frame -- no synchronization needed). Idempotent:
// a layer already provisioned is a no-op. This is what lets NUM_LAYERS be a generous cap
// with zero cost for layers a given game never uses.
void Renderer::ProvisionLayerContexts(int layer) {
	if (m_LayerProvisioned[layer]) return;

	const int taskCount = m_FlushTaskContextPool.taskCount;
	for (int t = 0; t < taskCount; ++t) {
		CommandContext& ctx = m_CommandContextPool[(size_t)layer * taskCount + t];
		for (int i = 0; i < m_NumFrames; ++i) {
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
			HRESULT hr1 = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
			if (FAILED(hr1)) { /* Handle error */ }

			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
			HRESULT hr2 = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), m_PipelineState.Get(), IID_PPV_ARGS(&list));
			if (FAILED(hr2)) { /* Handle error */ }
			list->Close(); // created in the "recording" state; FlushBatchTask Resets it before use

			// Debug name so DRED breadcrumbs identify which worker/layer/frame list died.
			wchar_t nm[64];
			swprintf_s(nm, L"Worker[layer%d][t%d][f%d]", layer, t, i);
			list->SetName(nm);

			ctx.allocators.push_back(alloc);
			ctx.cmdLists.push_back(list);
		}
	}
	m_LayerProvisioned[layer] = true;
}

void Renderer::BeginFrame(const float clearColor[4]) {
	// TWO DIFFERENT indices, deliberately -- see m_FrameResourceIndex's declaration comment.
	// frame: which rotating sync-slot (allocators, instance buffer) to reclaim/reuse.
	// backBuffer: which actual swap-chain back buffer to transition/clear/render into.
	const int frame = m_FrameResourceIndex;
	const int backBuffer = m_CurrentBackBufferIndex;

	// The allocators/command lists are reset per-frame and safe to reuse once the GPU has
	// finished executing the previous use of that frame slot. But the instance buffer is
	// *persistently mapped* and can be read by in-flight draws from multiple frames back.
	// If we only wait on m_FrameFenceValues[frame], we might be reusing the buffer while
	// the GPU is still reading it from a draw submitted 2-3 frames ago.
	// Fix: wait on an earlier frame's fence to ensure >= 2 frames of latency before we
	// reuse the instance buffer slot. This guarantees the GPU has moved past any draw
	// that could be reading m_InstanceBuffer[frame].
	// (No per-frame bucket-metadata reset here anymore: m_Buckets/m_MaterialHandles now
	// PERSIST across frames -- a material's handle is assigned once and reused forever, since
	// meshes/textures/effects live for the program's lifetime. Only the per-frame INSTANCE
	// data, in WorkerLocalSubmissionData, needs clearing -- done in PresentFrame below.)
	int safeFrame = (frame + m_NumFrames - 2) % m_NumFrames;
	WaitForFenceValue(m_FrameFenceValues[frame]);        // allocator is safe to reset
	WaitForFenceValue(m_FrameFenceValues[safeFrame]);    // instance buffer is safe to write

	// Reset all worker allocators to prevent stale commands accumulating
	for (auto& ctx : m_CommandContextPool) {
		if (ctx.allocators.size() > frame) {
			ctx.allocators[frame]->Reset();
		}
	}

	// PRE list (single-threaded): PRESENT -> RENDER_TARGET, then clear. The worker lists
	// draw after this; the POST list (PresentFrame) transitions back to PRESENT. PRE/POST
	// are SEPARATE lists from the workers so a worker's Reset() can't wipe the clear/barrier.
	m_CommandAllocators[frame]->Reset();
	m_CommandList->Reset(m_CommandAllocators[frame].Get(), nullptr);

	auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(
		m_BackBuffers[backBuffer].Get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &toRT);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
		m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), backBuffer, m_RTVDescriptorSize);
	m_CommandList->ClearRenderTargetView(rtv, (FLOAT*)clearColor, 0, nullptr);

	m_CommandList->Close(); // PRE complete; submitted first in PresentFrame
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
	if (!m_IsInitialized) return;                  // ignore WM_SIZE before bring-up
	if (m_ClientWidth == width && m_ClientHeight == height) return;

	m_ClientWidth = std::max(1u, width);          // never a 0-sized back buffer
	m_ClientHeight = std::max(1u, height);

	Flush();                                       // GPU must not reference the buffers
	for (int i = 0; i < m_NumFrames; ++i)
	{
		m_BackBuffers[i].Reset();                  // release before resizing the chain
		// Flush() just guaranteed the GPU is FULLY idle, so every fence value is equally
		// "reached" right now -- source from m_FrameResourceIndex for consistency with the
		// rest of the sync-slot scheme (fence values are indexed by frame-resource slot, not
		// swap-chain back-buffer index).
		m_FrameFenceValues[i] = m_FrameFenceValues[m_FrameResourceIndex];
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	ThrowIfFailed(m_SwapChain->GetDesc(&desc));
	ThrowIfFailed(m_SwapChain->ResizeBuffers(m_NumFrames, m_ClientWidth, m_ClientHeight,
		desc.BufferDesc.Format, desc.Flags));

	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
	UpdateRenderTargetViews();
}

void Renderer::Cleanup()
{
	if (!m_IsInitialized) return;
	Flush();                                       // wait for the GPU to finish
	if (m_FenceEvent) { ::CloseHandle(m_FenceEvent); m_FenceEvent = nullptr; }
	m_IsInitialized = false;
	// ComPtr members release themselves.
}

// ===========================================================================
// Creation steps (each returns the object it builds)
// ===========================================================================
void Renderer::CreateConstantBuffers() {
	// Create a buffer for each frame
	for (int i = 0; i < m_NumFrames; ++i) {
		// Create heap: D3D12_HEAP_TYPE_UPLOAD (CPU can write, GPU can read)
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);

		// 2. Define the Resource Description: It's just a raw buffer.
		CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ConstantBufferData));

		// 3. The Big Function
		ThrowIfFailed(m_Device->CreateCommittedResource(
			&heapProps,                     // 1. Where to put the memory
			D3D12_HEAP_FLAG_NONE,           // 2. Flags (NONE is fine)
			&bufferDesc,                    // 3. Size and type
			D3D12_RESOURCE_STATE_GENERIC_READ, // 4. Initial state (Upload buffers MUST be GENERIC_READ)
			nullptr,                        // 5. Clear value (used for RenderTargets, not buffers)
			IID_PPV_ARGS(&m_ConstantBuffers[i]) // 6. The output
		));
		// Map it ONCE and keep the pointer
		m_ConstantBuffers[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_MappedConstantBufferData[i]));
	}
}
ComPtr<ID3D12RootSignature> Renderer::CreateRootSignature() {
	CD3DX12_ROOT_PARAMETER rootParameters[3];
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // 1 SRV, register t0
	rootParameters[0].InitAsConstantBufferView(0); // b0
	rootParameters[1].InitAsShaderResourceView(1, 0); // 1 is the register (t1), 0 is the space
	// rootParameters[1].InitAsConstants(12, 1); // 8 values, register b1
	rootParameters[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

	// 3. Define a Static Sampler (Required because you used 'register(s0)')
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.ShaderRegister = 0; // Matches 'register(s0)'
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// 4. Create the Root Signature with the sampler included
	CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);

	// If this fails, 'error' will contain the reason why
	if (FAILED(hr)) {
		OutputDebugStringA((char*)error->GetBufferPointer());
		ThrowIfFailed(hr);
	}

	ComPtr<ID3D12RootSignature> rootSig;
	ThrowIfFailed(m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
	return rootSig;
}

// Standard alpha "over" blend -- what every sprite/text draw used before effects existed.
// D3D12_DEFAULT leaves BlendEnable=FALSE (GPU writes sampled RGBA straight to the render
// target, ignoring alpha), which is why text showed solid black boxes before this was added:
// the font atlas's non-ink texels still got their RGB (0,0,0) written verbatim instead of
// being blended away.
D3D12_BLEND_DESC Renderer::DefaultAlphaBlend() {
	D3D12_BLEND_DESC blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	return blend;
}

ComPtr<ID3D12PipelineState> Renderer::CreatePipelineState(
	const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend) {
	auto vertexShaderBlob = ReadFile(ExeRelative(vsPath));
	auto pixelShaderBlob = ReadFile(ExeRelative(psPath));
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.VS = { vertexShaderBlob.data(), vertexShaderBlob.size() };
	psoDesc.PS = { pixelShaderBlob.data(), pixelShaderBlob.size() };
	psoDesc.SampleMask = 0xFFFFFFFF; // Enable all samples

	// Connect the Root Signature and Input Layout. Input layout, root signature, depth/DSV,
	// RTV format, and topology are shared across EVERY effect -- only VS/PS/blend vary. If a
	// future effect needs a different vertex layout or its own root signature, that's a
	// bigger change than this function; for now every effect draws instanced quads through
	// the same instance-buffer/descriptor-table contract as sprites and text.
	psoDesc.pRootSignature = m_RootSignature.Get();
	psoDesc.InputLayout = { inputLayoutDesc, _countof(inputLayoutDesc) };
	psoDesc.BlendState = blend;
	// No depth buffer in this renderer, so depth/stencil must be OFF. The D3D12_DEFAULT
	// desc enables DepthEnable=TRUE; combined with DSVFormat left at UNKNOWN, that makes
	// CreateGraphicsPipelineState fail with E_INVALIDARG. Disable it explicitly.
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.NumRenderTargets = 1;
	psoDesc.SampleDesc.Count = 1;
	D3D12_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.FrontCounterClockwise = FALSE;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.DepthClipEnable = TRUE;
	rasterizerDesc.MultisampleEnable = FALSE;
	rasterizerDesc.AntialiasedLineEnable = FALSE;
	rasterizerDesc.ForcedSampleCount = 0;
	rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	psoDesc.RasterizerState = rasterizerDesc;

	ComPtr<ID3D12PipelineState> pso;
	ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	return pso;
}

uint32_t Renderer::RegisterEffect(const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend) {
	ShaderEffect effect;
	effect.pso = CreatePipelineState(vsPath, psPath, blend);
	m_Effects.push_back(effect);
	return (uint32_t)m_Effects.size() - 1;
}

ComPtr<IDXGIAdapter4> Renderer::GetAdapter(bool useWarp)
{
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;

	if (useWarp)
	{
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 desc;
			dxgiAdapter1->GetDesc1(&desc);
			if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
				SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0,
					__uuidof(ID3D12Device), nullptr)) &&
				desc.DedicatedVideoMemory > maxDedicatedVideoMemory)
			{
				maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
				ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
			}
		}
	}
	return dxgiAdapter4;
}

ComPtr<ID3D12Device2> Renderer::CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
	ComPtr<ID3D12Device2> device;
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(device.As(&pInfoQueue)))
	{
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		D3D12_MESSAGE_SEVERITY Severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
		D3D12_MESSAGE_ID DenyIds[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
		};
		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;
		ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
	}
#endif
	return device;
}

ComPtr<ID3D12CommandQueue> Renderer::CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandQueue> queue;
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = type;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	HRESULT hr = m_Device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
	if (FAILED(hr)) {
		// This will tell you EXACTLY why the queue failed to create
		// It might be "Device removed" or "Invalid Arg"
		char buf[256];
		sprintf_s(buf, "CreateCommandQueue failed with HRESULT 0x%08X", hr);
		MessageBoxA(NULL, buf, "Error", MB_OK);
		throw std::runtime_error(buf);
	}
	return queue;
}

bool Renderer::CheckTearingSupport()
{
	BOOL allowTearing = FALSE;
	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			if (FAILED(factory5->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
				allowTearing = FALSE;
		}
	}
	return allowTearing == TRUE;
}

ComPtr<IDXGISwapChain4> Renderer::CreateSwapChain(HWND hWnd, uint32_t width, uint32_t height, uint32_t bufferCount)
{
	ComPtr<IDXGISwapChain4> dxgiSwapChain4;
	ComPtr<IDXGIFactory4> dxgiFactory4;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Stereo = FALSE;
	desc.SampleDesc = { 1, 0 };
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = bufferCount;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	desc.Flags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
		m_CommandQueue.Get(), hWnd, &desc, nullptr, nullptr, &swapChain1));

	// We handle Alt+Enter fullscreen ourselves.
	ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
	ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));
	return dxgiSwapChain4;
}

ComPtr<ID3D12DescriptorHeap> Renderer::CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
{
	ComPtr<ID3D12DescriptorHeap> heap;
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = numDescriptors;
	desc.Type = type;

	// Only SRV/CBV/UAV heaps used for root tables need to be shader-visible
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
	{
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	}
	else
	{
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	}

	ThrowIfFailed(m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
	return heap;
}
void Renderer::UpdateRenderTargetViews()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	for (int i = 0; i < m_NumFrames; ++i)
	{
		ComPtr<ID3D12Resource> backBuffer;
		ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
		m_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);
		m_BackBuffers[i] = backBuffer;
		rtvHandle.Offset(m_RTVDescriptorSize);
	}
}

ComPtr<ID3D12CommandAllocator> Renderer::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandAllocator> allocator;
	ThrowIfFailed(m_Device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
	return allocator;
}

ComPtr<ID3D12GraphicsCommandList> Renderer::CreateCommandList(ComPtr<ID3D12CommandAllocator> allocator, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12GraphicsCommandList> list;
	ThrowIfFailed(m_Device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
	ThrowIfFailed(list->Close());   // created in the "recording" state; close so Render can Reset it
	return list;
}

ComPtr<ID3D12Fence> Renderer::CreateFence()
{
	ComPtr<ID3D12Fence> fence;
	ThrowIfFailed(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	return fence;
}

HANDLE Renderer::CreateEventHandle()
{
	HANDLE fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent && "Failed to create fence event.");
	return fenceEvent;
}

// ===========================================================================
// Synchronization
// ===========================================================================

uint64_t Renderer::Signal()
{
	uint64_t valueForSignal = ++m_FenceValue;
	ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), valueForSignal));
	return valueForSignal;
}

namespace {
	// Carries the per-wait state for the GPU-fence -> fiber bridge. One Win32 event per wait
	// (not a shared one) so overlapping waits can't stomp each other. The DirectEvent* is the
	// pooled rendezvous handed to us by WaitOnEventDirectArmed -- we signal it by pointer, so
	// there is no name lookup / registry / global lock on the wakeup path.
	struct FenceWaitCtx {
		HANDLE      win32Event = nullptr;
		HANDLE      waitHandle = nullptr;
		T_Threads::DirectEvent* evt = nullptr;
	};
	// Runs on a Win32 thread-pool thread when the GPU fence reaches the target value. It
	// resumes the suspended fiber via a direct pointer -- no T_Threads worker ever blocks.
	VOID CALLBACK FenceWaitCallback(PVOID param, BOOLEAN /*timedOut*/) {
		auto* c = static_cast<FenceWaitCtx*>(param);
		c->evt->Signal();
		::UnregisterWaitEx(c->waitHandle, nullptr); // one-shot, non-blocking cleanup
		::CloseHandle(c->win32Event);
		delete c;
	}
}

void Renderer::WaitForFenceValue(uint64_t value, std::chrono::milliseconds dur)
{
	if (m_Fence->GetCompletedValue() >= value)
		return; // already complete -- no wait

	auto& sched = T_Threads::TaskScheduler::Instance();
	if (sched.IsOnFiber()) {
		// On a fiber: SUSPEND it (freeing the worker to run other jobs) until the GPU
		// reaches 'value'. WaitOnEventArmed registers this fiber as a waiter and marks it
		// parkable, THEN runs the arm lambda -- so the fence callback can't SignalAll before
		// we're a discoverable waiter (no lost wakeup). The callback runs on a Win32
		// thread-pool thread, so nothing in the scheduler blocks.
		// Direct/handle event: no name, no eventRegistry, no registryMtx. WaitOnEventDirectArmed
		// hands us a pooled DirectEvent*; the fence callback signals it by pointer. This removes
		// the unbounded string-registry (and its lock convoy) from the wait path entirely. The
		// recheck-while-loop is belt-and-suspenders against a spurious wake (each wait has its
		// own unique event now, so it isn't strictly needed, but it's cheap and harmless).
		while (m_Fence->GetCompletedValue() < value) {
			sched.WaitOnEventDirectArmed([this, value](T_Threads::DirectEvent* e) {
				auto* c = new FenceWaitCtx();
				c->evt        = e;
				c->win32Event = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
				ThrowIfFailed(m_Fence->SetEventOnCompletion(value, c->win32Event));
				::RegisterWaitForSingleObject(&c->waitHandle, c->win32Event,
					FenceWaitCallback, c, INFINITE, WT_EXECUTEONLYONCE);
			});
		}
		return;
	}

	// Off-fiber (main thread, Resize/Cleanup): plain blocking wait -- no spin, 0% CPU.
	ThrowIfFailed(m_Fence->SetEventOnCompletion(value, m_FenceEvent));
	::WaitForSingleObject(m_FenceEvent, static_cast<DWORD>(dur.count()));
}

void Renderer::Flush()
{
	uint64_t valueForSignal = Signal();
	WaitForFenceValue(valueForSignal);
}

// ===========================================================================
// Debug + diagnostics
// ===========================================================================

void Renderer::EnableDebugLayer()
{
#if defined(_DEBUG)
	// Catch DX12 usage errors at creation time. Must run before any device is made.
	ComPtr<ID3D12Debug> debugInterface;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
	debugInterface->EnableDebugLayer();

	// GPU-Based Validation: validates resource states / descriptors / command buffers on the
	// GPU TIMELINE and reports violations in USER MODE (VS Output window) BEFORE they reach
	// the kernel driver. This is what survives a BSOD (KMODE_EXCEPTION_NOT_HANDLED) -- it
	// turns a bluescreen-from-bad-GPU-work into a readable error. SLOW; debug-only.
	ComPtr<ID3D12Debug1> debug1;
	if (SUCCEEDED(debugInterface.As(&debug1)))
	{
		debug1->SetEnableGPUBasedValidation(TRUE);
		debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
	}
#endif
	// DRED (Device Removed Extended Data): on a TDR / device-removed, this records the GPU's
	// command "breadcrumbs" (which ops actually completed) and the faulting virtual address,
	// so we can name the exact draw/resource that killed the device. MUST be set before the
	// device is created. Enabled in all configs so a Release crash is diagnosable too.
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
	{
		dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	}
}

// Dump DRED data after a device-removed. Call this when a GPU submit/present/fence returns
// DXGI_ERROR_DEVICE_REMOVED/HUNG -- the output goes to the VS Output window.
void Renderer::LogDeviceRemoved()
{
	char line[256];
	HRESULT reason = m_Device ? m_Device->GetDeviceRemovedReason() : E_FAIL;
	sprintf_s(line, "\n*** DEVICE REMOVED. reason = 0x%08lX\n", (unsigned long)reason);
	OutputDebugStringA(line);

	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (!m_Device || FAILED(m_Device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		OutputDebugStringA("  (DRED interface unavailable)\n");
		return;
	}

	// Auto-breadcrumbs: per command list, how far the GPU got before it died.
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
		while (node)
		{
			UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			// Skip lists the GPU fully finished (completed == total) -- only the one(s) it
			// died inside are interesting.
			if (last == node->BreadcrumbCount) { node = node->pNext; continue; }
			sprintf_s(line, "  [breadcrumb] list='%S' queue='%S' completedOps=%u / %u\n",
				node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?",
				node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
				last, node->BreadcrumbCount);
			OutputDebugStringA(line);
			// The op the GPU was executing when it died is at index 'last'.
			if (node->pCommandHistory && last < node->BreadcrumbCount)
			{
				sprintf_s(line, "        died on op #%u = %d\n", last, (int)node->pCommandHistory[last]);
				OutputDebugStringA(line);
			}
			node = node->pNext;
		}
	}

	// Page fault: the virtual address the GPU illegally touched + recently (de)allocated
	// resources near it -- usually pins a use-after-free or out-of-bounds resource.
	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
	{
		sprintf_s(line, "  [pagefault] VA = 0x%llX\n", (unsigned long long)pageFault.PageFaultVA);
		OutputDebugStringA(line);
		for (auto* n = pageFault.pHeadExistingAllocationNode; n; n = n->pNext)
		{
			sprintf_s(line, "        live   alloc '%S' type=%d\n",
				n->ObjectNameW ? n->ObjectNameW : L"?", (int)n->AllocationType);
			OutputDebugStringA(line);
		}
		for (auto* n = pageFault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
		{
			sprintf_s(line, "        freed  alloc '%S' type=%d  <-- recently freed near the fault\n",
				n->ObjectNameW ? n->ObjectNameW : L"?", (int)n->AllocationType);
			OutputDebugStringA(line);
		}
	}
	OutputDebugStringA("*** end DRED dump\n");
}
// Instead of hardcoding 3 vertices, you pass in the buffers

void Renderer::ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task) {
	// 1. Reset (Open) the list
	m_CommandList->Reset(m_CommandAllocators[m_FrameResourceIndex].Get(), nullptr);

	// 2. Perform the recording (The user passes the loading logic here)
	task(m_CommandList.Get());

	// 3. Close the list
	m_CommandList->Close();

	// 4. Submit and Flush
	ID3D12CommandList* ppCommandLists[] = { m_CommandList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	Flush(); // Wait for GPU to finish the upload
}
DirectX::XMFLOAT2 Renderer::GetNDC(float targetX, float targetY)
{
	DirectX::XMFLOAT2 ndc;
	ndc.x = (targetX / (m_ClientWidth / 2.0f)) - 1.0f;
	ndc.y = 1.0f - (targetY / (m_ClientHeight / 2.0f));
	return ndc;
}
void Renderer::Submit(
	const Mesh& mesh,
	TextureResource* tex,
	DirectX::XMFLOAT2 offset,
	float width,
	float height,
	int zLayer,
	DirectX::XMFLOAT4 color,
	float rotation,
	DirectX::XMFLOAT2 uvOffset,
	DirectX::XMFLOAT2 uvScale,
	bool useAlphaFromRGB,
	uint32_t effectID,
	DirectX::XMFLOAT4 effectParams)
{
	DirectX::XMFLOAT2 posNDC = GetNDC(offset.x, offset.y);
	DirectX::XMFLOAT2 sizeNDC =
	{
		(width / (m_ClientWidth / 2.0f)),
		(height / (m_ClientHeight / 2.0f))
	};

	// Look up (or assign, on first use) this material's bucket HANDLE -- a small sequential
	// index into m_Buckets[zLayer], NOT a hash into a fixed-size table. The old scheme hashed
	// (meshID,texID,effectID) into MAX_BUCKETS_PER_LAYER(512) slots and linearly probed past
	// collisions; it had two real problems: TextureResource::texID was never actually assigned
	// (always 0), so the hash didn't use texture identity at all, and the probe loop had NO
	// exit if all 512 slots in a layer ever filled with distinct materials -- an unbounded
	// spin. A real map has neither issue: unordered_map handles its own collisions internally,
	// and m_Buckets[zLayer] grows to fit however many distinct materials that layer actually
	// has. Handles are assigned ONCE and PERSIST across frames (meshes/textures/effects live
	// for the program's lifetime) -- only per-frame INSTANCE data gets cleared each frame.
	uint64_t key = GetMaterialID((Mesh*)&mesh, tex, effectID);
	auto& handleMap = m_MaterialHandles[zLayer];
	auto it = handleMap.find(key);
	uint32_t index;
	if (it != handleMap.end()) {
		index = it->second;
	} else {
		index = (uint32_t)m_Buckets[zLayer].size();
		Batch b;
		b.mesh = (Mesh*)&mesh;
		b.tex = tex;
		b.effectID = effectID;
		b.depth = (float)zLayer;
		m_Buckets[zLayer].push_back(b);
		handleMap[key] = index;

		// Grow EVERY worker's buckets[zLayer] to match, right now -- not just the discovering
		// worker's. Without this, a worker that never happens to submit to this NEW handle
		// keeps a SHORTER vector indefinitely, and the merge loop's "i >= size() -> treat as
		// 0 instances" guard is silently relied on to paper over that gap every single frame.
		// That's correct today (Submit() is main-thread-only in practice) but fragile: it
		// depends on nobody ever observing a stale size, rather than the invariant just
		// holding. Keeping every worker's layer-vector size == m_Buckets[zLayer].size() at
		// all times removes the gap entirely, so there's no "outdated handle index" state to
		// reason about. This only runs on NEW-material discovery (rare), not the hot per-
		// instance path, and matches the existing informal contract that material discovery
		// itself is effectively single-threaded (m_Buckets/m_MaterialHandles have never been
		// synchronized against concurrent discovery either).
		if (m_WorkerLocalStorage) {
			for (auto& worker : *m_WorkerLocalStorage) {
				auto& layerBuckets = worker.buckets[zLayer];
				if (index >= layerBuckets.size()) layerBuckets.resize((size_t)index + 1);
			}
		}
	}

	// Push to THIS WORKER'S local bucket, not the shared one.
	// This eliminates concurrent vector modification races.
	auto* thread = T_Threads::T_Thread::GetCurrent();
	float hasTex = (tex != nullptr) ? 1.0f : 0.0f;
	float alphaFromRGB = useAlphaFromRGB ? 1.0f : 0.0f;
	size_t workerIdx = (thread && thread->qIndex < MAX_WORKERS) ? (size_t)thread->qIndex : 0;
	if (m_WorkerLocalStorage) {
		auto& workerLayerBuckets = (*m_WorkerLocalStorage)[workerIdx].buckets[zLayer];
		// Already grown to fit by the new-material branch above (or by a prior frame); this
		// is just a safety net in case index somehow still exceeds it.
		if (index >= workerLayerBuckets.size()) workerLayerBuckets.resize((size_t)index + 1);
		workerLayerBuckets[index].push_back(
			ObjectData(posNDC, sizeNDC, color, hasTex, rotation, uvOffset, uvScale, alphaFromRGB, effectParams)
		);
	}
}


void Renderer::UpdateFPS()
{
	using clock = std::chrono::high_resolution_clock;
	m_frameCounter++;
	auto t1 = clock::now();
	m_elapsedSeconds += std::chrono::duration<double>(t1 - m_t0).count();
	m_t0 = t1;
	if (m_elapsedSeconds > 1.0)
	{
		// Recompute the DISPLAYED value only once/sec (a proper averaging window) -- but
		// SubmitText() below still runs every call, using this cached value, so the text
		// itself is submitted (and therefore drawn) every frame, not just the one frame/sec
		// this branch happens to fire on.
		m_LastFPS = m_frameCounter / m_elapsedSeconds;
		m_frameCounter = 0;
		m_elapsedSeconds = 0.0;
	}
	SubmitText(font, 10, 10, "FPS: {:.1f}", 1.0f, { 57.0f / 255.0f, 255.0f / 255.0f, 20.0f / 255.0f, 1.0f }, 0.0f, TextAlign::Right, 2, m_LastFPS);
}
void Renderer::UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize) {
	float ar = screenSize.x / (screenSize.y > 0.0f ? screenSize.y : 1.0f);

	// Sync-rotation slot (m_FrameResourceIndex), not the swap-chain back-buffer index -- this
	// constant buffer is a CPU/GPU-sync resource, unrelated to which back buffer is on screen.
	ConstantBufferData* pData = reinterpret_cast<ConstantBufferData*>(m_MappedConstantBufferData[m_FrameResourceIndex]);
	pData->screenSize = screenSize;
	pData->aspectRatio = ar;
}
DirectX::XMFLOAT2 Renderer::GetScreenSize() const {
	return DirectX::XMFLOAT2(static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
}

void Renderer::CreateInstanceBuffer(ID3D12Device* device, UINT maxInstances) {
	UINT bufferSize = maxInstances * sizeof(ObjectData);

	// 1. Define the heap properties (Upload heap)
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	// 2. Define the buffer description
	D3D12_RESOURCE_DESC bufferDesc = {};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = bufferSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	// 3+4. Create + persistently map ONE buffer PER FRAME (avoids the in-flight overwrite).
	for (int i = 0; i < m_NumFrames; ++i) {
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_InstanceBuffer[i])
		));
		ThrowIfFailed(m_InstanceBuffer[i]->Map(0, nullptr, &m_MappedData[i]));
	}
}
uint64_t Renderer::GetMaterialID(Mesh* mesh, TextureResource* tex, uint32_t effectID) {
	// Used as an unordered_map KEY (see Submit/m_MaterialHandles) -- unlike the old scheme,
	// this doesn't need to avoid collisions itself; the map handles that internally. Folding
	// effectID in just needs reasonable distribution, not perfection.
	return ((uint64_t)mesh ^ ((uint64_t)tex << 1)) ^ ((uint64_t)effectID << 48);
}

int Renderer::FlushBatchParallel() {
	// Sync-slot index (m_MappedData is a CPU/GPU-sync resource), NOT the swap-chain
	// back-buffer index -- see m_FrameResourceIndex's declaration comment.
	const int frame = m_FrameResourceIndex;

	// One task per worker context, bounded by the hardware thread count.
	unsigned hw = std::thread::hardware_concurrency();
	int taskCount = (hw > 1) ? (int)(hw - 1) : 1;
	if (taskCount > (int)m_CommandContextPool.size()) taskCount = (int)m_CommandContextPool.size();
	if (taskCount < 1) taskCount = 1;

	// SERIAL section (main thread only): merge all worker-local submissions from all layers
	// into the GPU instance buffer. Submissions are main-thread-only, so this is safe without atomics.
	// Per-layer counts and ABSOLUTE start offsets, sized to that layer's ACTUAL bucket count
	// (m_Buckets[layer].size()) -- no more fixed MAX_BUCKETS_PER_LAYER, no flat all-layers index.
	std::vector<UINT> bucketInstanceCounts[NUM_LAYERS];
	std::vector<UINT> bucketInstanceOffsets[NUM_LAYERS];
	auto* dst = reinterpret_cast<ObjectData*>(m_MappedData[frame]);
	if (!dst) {
		OutputDebugStringA("FATAL: m_MappedData[frame] is null!\n");
		// Clear so PresentFrame's submission loop draws nothing this frame instead of
		// resubmitting a PRIOR frame's m_ActiveLayersThisFrame -- whose command lists'
		// backing allocators BeginFrame already reset this frame. See the overflow-path
		// comment below for the full mechanism; same fix applies to every early return here.
		m_ActiveLayersThisFrame.clear();
		return 0;
	}
	UINT running = 0;
	const UINT MAX_INSTANCE_COUNT = 65536;  // Must match CreateInstanceBuffer size

	// Merge all layers into the instance buffer, tracking count + start offset per bucket.
	// Also track which layers actually got any content, so we only dispatch (and later submit)
	// work for layers that exist this frame -- most layers are typically empty.
	bool layerHasContent[NUM_LAYERS] = {};
	for (int layer = 0; layer < NUM_LAYERS; ++layer) {
		const size_t bucketCount = m_Buckets[layer].size(); // this layer's distinct materials
		bucketInstanceCounts[layer].assign(bucketCount, 0);
		bucketInstanceOffsets[layer].assign(bucketCount, 0);
		if (!m_WorkerLocalStorage) continue;

		for (size_t i = 0; i < bucketCount; ++i) {
			bucketInstanceOffsets[layer][i] = running;   // this bucket starts here

			// Merge instances from all workers into this bucket's slot in the GPU buffer.
			// A worker that never submitted to bucket i simply has a shorter (or empty)
			// buckets[layer] vector -- skip it rather than resizing on the read side.
			for (int w = 0; w < MAX_WORKERS; ++w) {
				auto& workerLayerBuckets = (*m_WorkerLocalStorage)[w].buckets[layer];
				if (i >= workerLayerBuckets.size()) continue;
				const auto& workerBucket = workerLayerBuckets[i];
				UINT count = (UINT)workerBucket.size();
				if (count > 0) {
					// Strict bounds check: prevent buffer overflow
					if (running + count > MAX_INSTANCE_COUNT) {
						char dbgBuf[256];
						sprintf_s(dbgBuf, "ERROR: Would overflow instance buffer! running=%u, count=%u, max=%u\n", running, count, MAX_INSTANCE_COUNT);
						OutputDebugStringA(dbgBuf);
						// m_ActiveLayersThisFrame/m_TasksDispatchedPerLayer still hold the LAST
						// SUCCESSFUL frame's values at this point (this frame never reached the
						// point where they'd be overwritten). If left stale, PresentFrame would
						// submit THOSE layers' command lists -- but BeginFrame already reset
						// their allocators for THIS frame before Submit()/FlushBatchParallel
						// ever ran, so those lists' backing memory is already reclaimed.
						// Submitting them would be corrupted-command-list territory, the exact
						// class of bug that caused today's earlier GPU crashes. Clearing makes
						// PresentFrame submit PRE+POST only -- a blank-but-safe frame.
						m_ActiveLayersThisFrame.clear();
						return 0;  // Fail fast instead of clamping
					}
					const void* src = workerBucket.data();
					if (!src) {
						OutputDebugStringA("ERROR: workerBucket.data() is null!\n");
						m_ActiveLayersThisFrame.clear(); // see overflow-path comment above
						return 0;
					}
					size_t copySize = (size_t)count * sizeof(ObjectData);
					memcpy(dst + running, src, copySize);
					running += count;
					bucketInstanceCounts[layer][i] += count;
					layerHasContent[layer] = true;
				}
			}
		}
	}

	m_ActiveLayersThisFrame.clear();
	for (int layer = 0; layer < NUM_LAYERS; ++layer)
		if (layerHasContent[layer]) m_ActiveLayersThisFrame.push_back(layer);

	// Lazily create THIS layer's command contexts on first use (no-op if already provisioned).
	// Runs here on the main thread, before any task touches m_CommandContextPool this frame.
	for (int layer : m_ActiveLayersThisFrame)
		ProvisionLayerContexts(layer);

	auto& sched = T_Threads::TaskScheduler::Instance();
	T_Threads::WaitGroup wg;
	wg.n.store(0, std::memory_order_relaxed);  // Initialize local wait group
	std::vector<FlushTaskContext*> ctxs;
	ctxs.reserve((size_t)m_ActiveLayersThisFrame.size() * taskCount);

	// Dispatch taskCount tasks PER ACTIVE LAYER (not one flat pass over all layers). Each task
	// still only handles a bucketsPerTask-sized slice, same load-balanced partitioning as
	// before -- it's just scoped to ONE layer now instead of sweeping all NUM_LAYERS, and
	// bucketsPerTask is now computed PER LAYER from its actual (dynamic) bucket count. This is
	// what lets PresentFrame submit strictly layer-major (see FlushBatchTask's pool-indexing
	// comment): the old scheme split by bucket-index range ACROSS all layers, so a high-zLayer
	// object and a low-zLayer object could land in different tasks whose command lists then
	// executed in TASK order, not LAYER order -- zLayer was silently ignored across task
	// boundaries. This fixes that while keeping the same per-task load balancing.
	// How many (task) command lists actually got recorded THIS frame, per active layer --
	// NOT always taskCount. With few distinct materials in a layer, bucketsPerTask can make
	// the loop below stop well short of taskCount (e.g. 2 materials, taskCount=15 -> only
	// t=0,1 run). PresentFrame's submission loop MUST use this exact count, not a blanket
	// taskCount, or it submits/re-submits command lists that were never (re)recorded this
	// frame -- at best stale/duplicate draws, at worst a validation error from resetting an
	// allocator whose list is still referenced by a pending/duplicate submission.
	m_TasksDispatchedPerLayer.assign(NUM_LAYERS, 0);

	int launched = 0;
	for (int layer : m_ActiveLayersThisFrame) {
		// Bound by bucketInstanceCounts[layer].size(), NOT a fresh m_Buckets[layer].size()
		// query -- ctx->bucketInstanceCounts below is a COPY of bucketInstanceCounts[layer],
		// sized back in the merge loop above. Re-querying m_Buckets[layer].size() here would
		// be a SEPARATE read that could disagree with the merge-time size (e.g. if Submit()
		// ever ran between the two loops), letting ctx->endBucket exceed
		// ctx->bucketInstanceCounts's actual size -> out-of-bounds read in FlushBatchTask.
		// Tying the bound to the exact container being copied makes that impossible.
		const int layerBucketCount = (int)bucketInstanceCounts[layer].size();
		const int bucketsPerTask = (layerBucketCount + taskCount - 1) / taskCount; // ceil
		int dispatchedThisLayer = 0;
		for (int t = 0; t < taskCount; ++t) {
			int start = t * bucketsPerTask;
			if (start >= layerBucketCount) break;
			int end = start + bucketsPerTask;
			if (end > layerBucketCount) end = layerBucketCount;

			auto* ctx = m_FlushTaskContextPool.Acquire();
			ctx->renderer = this;
			ctx->bucketInstanceCounts = bucketInstanceCounts[layer];
			ctx->bucketInstanceOffsets = bucketInstanceOffsets[layer];
			ctx->layer = layer;
			ctx->taskIndex = t;
			ctx->startBucket = start;
			ctx->endBucket = end;

			ctxs.push_back(ctx);
			auto* task = sched.CreateTask(FlushBatchTask, ctx, true);
			task->waitGroup = &wg;  // Set BEFORE incrementing counter
			wg.n.fetch_add(1, std::memory_order_release);
			sched.Push(task);
			++launched;
			++dispatchedThisLayer;
		}
		m_TasksDispatchedPerLayer[layer] = dispatchedThisLayer;
	}
	sched.WaitFor(wg);  // all worker lists are recorded + Closed - blocks until all tasks complete
	return launched;
}

void Renderer::PresentFrame() {
	// TWO DIFFERENT indices, deliberately -- see m_FrameResourceIndex's declaration comment.
	const int frame = m_FrameResourceIndex;       // sync-slot: allocators, cmdLists, fence value
	const int backBuffer = m_CurrentBackBufferIndex; // which actual back buffer to present

	// Parallel command list recording: merge worker submissions and launch worker tasks
	// to record draw commands in parallel. FlushBatchParallel handles everything including
	// the GPU buffer upload via memcpy.
	int workerCount = FlushBatchParallel();

	// POST list: transition the back buffer back to PRESENT (single-threaded, executed last).
	m_PostAllocators[frame]->Reset();
	m_PostList->Reset(m_PostAllocators[frame].Get(), nullptr);
	auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
		m_BackBuffers[backBuffer].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT);
	m_PostList->ResourceBarrier(1, &toPresent);
	m_PostList->Close();

	// Execute PRE + all worker lists (LAYER-MAJOR) + POST. Submitting ALL of layer L's task
	// lists before ANY of layer L+1's is what makes zLayer ordering correct across task
	// boundaries -- see FlushBatchTask's pool-indexing comment and FlushBatchParallel's
	// per-layer dispatch. (workerCount from FlushBatchParallel is now activeLayers*taskCount,
	// informational only -- this loop derives the same set directly.)
	std::vector<ID3D12CommandList*> allLists;
	allLists.push_back(m_CommandList.Get());  // PRE: clear + PRESENT->RENDER_TARGET
	const int taskCount = m_FlushTaskContextPool.taskCount;
	for (int layer : m_ActiveLayersThisFrame) {
		// Only the tasks FlushBatchParallel actually dispatched for this layer this frame --
		// see m_TasksDispatchedPerLayer. Using taskCount here would resubmit stale/never-
		// recorded command lists for the (task) slots that layer's material count didn't need.
		const int dispatched = m_TasksDispatchedPerLayer[layer];
		for (int t = 0; t < dispatched; ++t) {
			allLists.push_back(m_CommandContextPool[(size_t)layer * taskCount + t].cmdLists[frame].Get());
		}
	}
	allLists.push_back(m_PostList.Get());     // POST: RENDER_TARGET->PRESENT
	m_CommandQueue->ExecuteCommandLists((UINT)allLists.size(), allLists.data());

	// Present, then ONE fence scheme (m_FrameFenceValues + Signal()). BeginFrame waits on
	// m_FrameFenceValues[frame] before reusing this slot's allocators next time around.
	HRESULT hrPresent = m_SwapChain->Present(m_VSync ? 1 : 0, 0);
	if (FAILED(hrPresent)) {
		// A TDR surfaces here as DEVICE_REMOVED/RESET. Dump DRED (which op/resource killed
		// the GPU) BEFORE throwing, so we get the diagnosis instead of a bare crash.
		if (hrPresent == DXGI_ERROR_DEVICE_REMOVED || hrPresent == DXGI_ERROR_DEVICE_RESET)
			LogDeviceRemoved();
		ThrowIfFailed(hrPresent);
	}
	m_FrameFenceValues[frame] = Signal();
	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex(); // DXGI-controlled, for the NEXT frame's back buffer
	m_FrameResourceIndex = (m_FrameResourceIndex + 1) % m_NumFrames;     // app-controlled, fully independent rotation

	// CPU-side reset for next frame. Safe now: all worker RECORDING finished above (the GPU
	// reads m_InstanceBuffer, not these vectors). Only the per-frame INSTANCE data needs
	// clearing -- m_Buckets/m_MaterialHandles (which material maps to which handle) PERSIST
	// across frames, so there's nothing to reset there. .clear() empties each per-bucket
	// vector's CONTENTS but keeps its allocated capacity, so steady-state frames don't
	// reallocate. Bounded by each worker's OWN buckets[layer].size() (dynamic, grows lazily
	// in Submit -- see WorkerLocalSubmissionData), not a fixed MAX_BUCKETS_PER_LAYER.
	if (m_WorkerLocalStorage) {
		for (int w = 0; w < MAX_WORKERS; ++w) {
			for (int layer = 0; layer < NUM_LAYERS; ++layer) {
				auto& workerLayerBuckets = (*m_WorkerLocalStorage)[w].buckets[layer];
				for (auto& bucket : workerLayerBuckets)
					bucket.clear();
			}
		}
	}
	m_FlushTaskContextPool.Reset();
}