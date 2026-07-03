#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cwchar>       // wcscmp, wcstol

#include "../include/Window.h"
#include "../include/Renderer2D.h"
#include "../include/Helpers.h"
#include "../include/Font.h"
#include "../include/TaskDAG.h"
// -w/--width, -h/--height, -warp/--warp. Parsed here (an app concern), then handed
// to the Window (size) and Renderer (warp adapter).
static void ParseCommandLine(uint32_t& width, uint32_t& height, bool& useWarp)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return;

	for (int i = 0; i < argc; ++i)
	{
		if ((wcscmp(argv[i], L"-w") == 0 || wcscmp(argv[i], L"--width") == 0) && i + 1 < argc)
			width = wcstol(argv[++i], nullptr, 10);
		else if ((wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--height") == 0) && i + 1 < argc)
			height = wcstol(argv[++i], nullptr, 10);
		else if (wcscmp(argv[i], L"-warp") == 0 || wcscmp(argv[i], L"--warp") == 0)
			useWarp = true;
	}
	LocalFree(argv);
}

// The whole program now reads as the lifecycle of two objects:
//   Window   -- owns the OS window + message pump (+ fullscreen/keys)
//   Renderer -- owns every GPU object + the render/resize/cleanup logic
// TEMP DIAGNOSTIC: file-scope so it's usable from the very first line of wWinMain, to bisect
// startup (not just the render loop). Remove once the DAG crash is found.
static void EarlyCheckpoint(const char* msg) {
	FILE* f = nullptr; fopen_s(&f, "C:\\temp\\dag_checkpoint.log", "w");
	if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	EarlyCheckpoint("wWinMain: entered");
	T_Threads::TaskScheduler::Init();
	EarlyCheckpoint("TaskScheduler::Init() returned");
	T_Threads::TaskScheduler& scheduler = T_Threads::TaskScheduler::Instance();

	// 100% client-area scaling, DPI-aware non-client chrome.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// WIC (used by DirectXTex's LoadFromWICFile to load textures) is COM-based and
	// requires a COM apartment on this thread; without this the texture load fails with
	// CO_E_NOTINITIALIZED. Initialize once for the app, uninitialize on exit.
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return -1;
	EarlyCheckpoint("CoInitializeEx returned");

	uint32_t width = 1280, height = 720;
	bool useWarp = false;
	ParseCommandLine(width, height, useWarp);

	Window   window(hInstance, L"DirectX 12", width, height);
	// RendererCore: device/swap chain/fences/PRE-POST lists/particle system -- the low-level
	// GPU guts, reusable by a future 3D renderer. Renderer2D: sprite/text batching, driven BY
	// core's BeginFrame/PresentFrame via SetRenderer2D(). See both classes' header comments.
	RendererCore core;
	Renderer2D   renderer;
	core.SetRenderer2D(&renderer);
	window.SetRenderer(&core);                           // so WM_SIZE reaches Core (owns Resize/VSync)
	EarlyCheckpoint("Window/Renderer constructed, about to Initialize()");

	core.Initialize(window.GetHandle(), width, height, useWarp);
	EarlyCheckpoint("core.Initialize() returned");
	renderer.Initialize(core); // builds root sig/PSO/instance buffer/SRV heap/font against core's device
	EarlyCheckpoint("renderer.Initialize(core) returned");
	// Register particle effects AFTER both Initialize() calls (needs core's m_ComputeRootSignature/
	// m_CommandQueue), same pattern as RegisterEffect() for sprite shaders. Each gets its OWN
	// compute shader -- see UpdateParticles.hlsl (straight-line motion) and WaveParticles.hlsl
	// (sin-driven wave) -- and its own particle pool, so they can never step on each other's slots.
	// zLayer=1 here: draws over anything at zLayer 0 (e.g. background tiles) and under zLayer 2+
	// (e.g. the FPS/text overlay) -- see RendererCore::PresentFrame's layer-interleaving comment.
	uint32_t linearEffect = core.RegisterParticleEffect(L"UpdateParticles.cso", 256000, 1);
	uint32_t waveEffect = core.RegisterParticleEffect(L"WaveParticles.cso", 50000, 1);
	EarlyCheckpoint("particle effects registered");
	window.Show();
	Vertex vertices[] = {
		// New X, Y      Color (RGBA)          Tex (U, V)
		{  0.0f,  0.3333f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.5f, 0.0f }, // Top
		{  0.3f, -0.1667f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }, // Right
		{ -0.3f, -0.1667f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }  // Left
	};
	uint32_t triangleIndices[] = { 0, 1, 2 }; // Just one triangle
	// A small red triangle at the bottom-right
	Vertex moreVertices[] = {
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }, // Top-Left
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }, // Top-Right
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }  // Bottom-Right
	};
	uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 }; // The two-triangle quad
	auto resourceManager = renderer.GetResourceManager();
	auto cmdList = renderer.GetCommandList();
	TextureResource* wall;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		wall = resourceManager->LoadTexture(ExeRelative(L"wall.png"), cmd);
		});
	EarlyCheckpoint("wall.png loaded");
	TextureResource* wood;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		wood = resourceManager->LoadTexture(ExeRelative(L"wood.png"), cmd);
		});
	EarlyCheckpoint("wood.png loaded");
	Font font;
	font.Load(ExeRelative(L"font.fnt"), ExeRelative(L"font.png"), renderer);
	EarlyCheckpoint("font.Load() returned");
	Mesh myTriangle = resourceManager->CreateMesh(vertices, 3, triangleIndices, 3);
	Mesh myOtherObject = resourceManager->CreateMesh(moreVertices, 4, quadIndices, 6);
	EarlyCheckpoint("meshes created, about to enter loop");
	float rotation = 0.0f;
	float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Define your background

	// TEMP DIAGNOSTIC: write-through checkpoint log (overwrites one line, so it survives even
	// a hard corruption abort that a normal exit-time flush would lose). Remove once found.
	auto checkpoint = [](const char* msg) {
		FILE* f = nullptr; fopen_s(&f, "C:\\temp\\dag_checkpoint.log", "w");
		if (f) { fprintf(f, "%s\n", msg); fclose(f); }
	};

	while (window.ProcessMessages())
	{

		float dt = renderer.GetFrameTime();
		renderer.UpdateGlobalUniforms(renderer.GetScreenSize());
		rotation += 30.0f * dt; // 30 degrees per second, now truly framerate-independent

		// Per-frame DAG: StartFrame -> {sprites, text} (main-affinity, parallel-in-principle
		// but both actually run on main, serially, via ProcessMainThread) -> PresentFrame.
		// EVERY Submit-producer node MUST be added as a dependency of presentNode below, or
		// PresentFrame's FlushBatchParallel merge can race with a producer still submitting.
		T_Threads::TaskDAG dag(scheduler);
		checkpoint("dag constructed");

		core.m_StartFrameCtx = { &core, { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } };
		auto* startTask = scheduler.CreateTask(RendererCore::StartFrameTask, &core.m_StartFrameCtx);
		checkpoint("startTask created");
		auto* startNode = dag.CreateMainNode(startTask);
		checkpoint("startNode created");

		auto* spritesTask = scheduler.CreateTask([&renderer, &myOtherObject, &myTriangle, wood, wall, rotation, &checkpoint, linearEffect, waveEffect] {
			checkpoint("spritesTask: begin");
			renderer.Submit(myOtherObject, wood, { 1280 / 2,720 / 2 }, 100, 100, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 200,200 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 400,400 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 600,600 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myOtherObject, wood, { 500,500 }, 100, 100, 0, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			// effectID, basePosition, offsetRadius (jitter so multiple spawns scatter instead of
			// stacking), velocity, lifetime, color.
			renderer.RequestSpawn(linearEffect, { 300,300 }, 20.0f, { 0.0f, -50.0f }, 5.0f, { 1.0f, 0.6f, 0.1f, 1.0f });
			// velocity.y is repurposed as wave AMPLITUDE by WaveParticles.hlsl, not a real
			// vertical speed -- see that file's comment.
			renderer.RequestSpawn(waveEffect, { 900,300 }, 20.0f, { 30.0f, 60.0f }, 5.0f, { 0.2f, 0.6f, 1.0f, 1.0f });

			checkpoint("spritesTask: end");
			});
		checkpoint("spritesTask created");
		auto* spritesNode = dag.CreateMainNode(spritesTask);
		dag.AddDependency(spritesNode, startNode);
		checkpoint("spritesNode wired");

		auto* textTask = scheduler.CreateTask([&renderer, &checkpoint] {
			checkpoint("textTask: begin");
			Font* f = renderer.GetSysFont();
			renderer.SubmitText(*f,  400.0f, 150.0f, "Left Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Left, 0);
			renderer.SubmitText(*f,  400.0f, 250.0f, "Center Aligned", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Center, 0);
			renderer.SubmitText(*f,  400.0f, 350.0f, "Right Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Right, 0);
			checkpoint("textTask: UpdateFPS");
			renderer.UpdateFPS(); // its own SubmitText call -- same tier, same rules
			checkpoint("textTask: end");
			});
		checkpoint("textTask created");
		auto* textNode = dag.CreateMainNode(textTask);
		dag.AddDependency(textNode, startNode);
		checkpoint("textNode wired");

		core.m_UpdateParticlesCtx = { &core };
		auto* particlesTask = scheduler.CreateTask(RendererCore::UpdateParticlesTask, &core.m_UpdateParticlesCtx);
		checkpoint("particlesTask created");
		auto* particlesNode = dag.CreateMainNode(particlesTask);
		dag.AddDependency(particlesNode, startNode);
		checkpoint("particlesNode wired");

		core.m_PresentFrameCtx = { &core };
		auto* presentTask = scheduler.CreateTask(RendererCore::PresentFrameTask, &core.m_PresentFrameCtx);
		checkpoint("presentTask created");
		auto* presentNode = dag.CreateMainNode(presentTask);
		dag.AddDependency(presentNode, spritesNode);
		dag.AddDependency(presentNode, textNode);
		dag.AddDependency(presentNode, particlesNode);
		checkpoint("presentNode wired");

		T_Threads::WaitGroup frameWg;
		frameWg.n.store(1, std::memory_order_relaxed); // 1 = the PresentFrame tail node
		presentTask->waitGroup = &frameWg;             // set BEFORE dag.Submit()

		checkpoint("about to dag.Submit()");
		dag.Submit();
		checkpoint("dag.Submit() returned, entering WaitForMain");
		scheduler.WaitForMain(frameWg); // pumps ProcessMainThread; frame N+1 can't start until this returns
		checkpoint("WaitForMain returned");

	}                               // continuous render loop

	core.Cleanup();                                       // flush the GPU, close the fence event
	CoUninitialize();
	return 0;
}
