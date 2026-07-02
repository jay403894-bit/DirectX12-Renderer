#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cwchar>       // wcscmp, wcstol

#include "Window.h"
#include "Renderer.h"
#include "Helpers.h"
#include "Font.h"
#include "TaskDAG.h"
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
	Renderer renderer;
	window.SetRenderer(&renderer);                       // so WM_SIZE reaches the renderer
	EarlyCheckpoint("Window/Renderer constructed, about to Initialize()");

	renderer.Initialize(window.GetHandle(), width, height, useWarp);
	EarlyCheckpoint("renderer.Initialize() returned");
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
	uint64_t frameNum = 0;

	while (window.ProcessMessages())
	{
		char buf[64]; sprintf_s(buf, "frame %llu: top of loop", (unsigned long long)frameNum);
		checkpoint(buf);

		float dt = renderer.GetFrameTime();
		renderer.UpdateGlobalUniforms(renderer.GetScreenSize());
		rotation += 30.0f * dt; // 30 degrees per second, now truly framerate-independent

		// Per-frame DAG: StartFrame -> {sprites, text} (main-affinity, parallel-in-principle
		// but both actually run on main, serially, via ProcessMainThread) -> PresentFrame.
		// EVERY Submit-producer node MUST be added as a dependency of presentNode below, or
		// PresentFrame's FlushBatchParallel merge can race with a producer still submitting.
		T_Threads::TaskDAG dag(scheduler);
		checkpoint("dag constructed");

		renderer.m_StartFrameCtx = { &renderer, { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } };
		auto* startTask = scheduler.CreateTask(Renderer::StartFrameTask, &renderer.m_StartFrameCtx);
		checkpoint("startTask created");
		auto* startNode = dag.CreateMainNode(startTask);
		checkpoint("startNode created");

		auto* spritesTask = scheduler.CreateTask([&renderer, &myOtherObject, &myTriangle, wood, wall, rotation, &checkpoint] {
			checkpoint("spritesTask: begin");
			renderer.Submit(myOtherObject, wood, { 1280 / 2,720 / 2 }, 100, 100, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 200,200 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 400,400 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myTriangle, wall, { 600,600 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
			renderer.Submit(myOtherObject, wood, { 500,500 }, 100, 100, 0, { 1.0f,1.0f,1.0f,1.0f }, rotation);
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

		// (future systems, e.g. particles: same pattern -- CreateMainNode + AddDependency(startNode),
		//  then AddDependency(presentNode, particlesNode) below, no exceptions)

		renderer.m_PresentFrameCtx = { &renderer };
		auto* presentTask = scheduler.CreateTask(Renderer::PresentFrameTask, &renderer.m_PresentFrameCtx);
		checkpoint("presentTask created");
		auto* presentNode = dag.CreateMainNode(presentTask);
		dag.AddDependency(presentNode, spritesNode);
		dag.AddDependency(presentNode, textNode);
		checkpoint("presentNode wired");

		T_Threads::WaitGroup frameWg;
		frameWg.n.store(1, std::memory_order_relaxed); // 1 = the PresentFrame tail node
		presentTask->waitGroup = &frameWg;             // set BEFORE dag.Submit()

		checkpoint("about to dag.Submit()");
		dag.Submit();
		checkpoint("dag.Submit() returned, entering WaitForMain");
		scheduler.WaitForMain(frameWg); // pumps ProcessMainThread; frame N+1 can't start until this returns
		checkpoint("WaitForMain returned");
		++frameNum;
	}                               // continuous render loop

	renderer.Cleanup();                                  // flush the GPU, close the fence event
	CoUninitialize();
	return 0;
}
