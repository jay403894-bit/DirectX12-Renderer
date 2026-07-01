#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cwchar>       // wcscmp, wcstol

#include "Window.h"
#include "Renderer.h"
#include "Helpers.h"
#include "Font.h"
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
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	T_Threads::TaskScheduler::Init();
	T_Threads::TaskScheduler& scheduler = T_Threads::TaskScheduler::Instance();

	// 100% client-area scaling, DPI-aware non-client chrome.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// WIC (used by DirectXTex's LoadFromWICFile to load textures) is COM-based and
	// requires a COM apartment on this thread; without this the texture load fails with
	// CO_E_NOTINITIALIZED. Initialize once for the app, uninitialize on exit.
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return -1;

	uint32_t width = 1280, height = 720;
	bool useWarp = false;
	ParseCommandLine(width, height, useWarp);

	Window   window(hInstance, L"Learning DirectX 12", width, height);
	Renderer renderer;
	window.SetRenderer(&renderer);                       // so WM_SIZE reaches the renderer

	renderer.Initialize(window.GetHandle(), width, height, useWarp);
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
	TextureResource* wood;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		wood = resourceManager->LoadTexture(ExeRelative(L"wood.png"), cmd);
		});
	Font font;
	font.Load(ExeRelative(L"font.fnt"), ExeRelative(L"font.png"), renderer);
	Mesh myTriangle = resourceManager->CreateMesh(vertices, 3, triangleIndices, 3);
	Mesh myOtherObject = resourceManager->CreateMesh(moreVertices, 4, quadIndices, 6);
	float rotation = 0.0f;
	float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Define your background

	while (window.ProcessMessages())
	{
		float dt = renderer.GetFrameTime();
		renderer.UpdateGlobalUniforms(renderer.GetScreenSize());
		renderer.BeginFrame(clearColor);
		// Now you are the "Director" - you decide what to draw!
		renderer.Submit(myOtherObject,wood,{ 1280/2,720/2 },100,100, 1, { 1.0f,1.0f,1.0f,1.0f},rotation);
		renderer.Submit(myTriangle, wall, { 200,200 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
		renderer.Submit(myTriangle, wall, { 400,400 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);
		renderer.Submit(myTriangle, wall, { 600,600 }, 200, 200, 1, { 1.0f,1.0f,1.0f,1.0f }, rotation);

		renderer.Submit(myOtherObject, wood, {  500,500 }, 100, 100, 0, { 1.0f,1.0f,1.0f,1.0f }, rotation);

		font.SubmitText(renderer, "Left Aligned", 400.0f, 150.0f, 1.0f, { 1,1,1,1 }, 0.0f, TextAlign::Left,0);
		font.SubmitText(renderer, "Center Aligned", 400.0f, 250.0f, 1.0f, { 1,1,1,1 }, 0.0f, TextAlign::Center,0);
		font.SubmitText(renderer, "Right Aligned", 400.0f, 350.0f, 1.0f, { 1,1,1,1 }, 0.0f, TextAlign::Right,0);
		rotation += 30.0f * dt; // 30 degrees per second, now truly framerate-independent
		renderer.PresentFrame();
	}                               // continuous render loop

	renderer.Cleanup();                                  // flush the GPU, close the fence event
	CoUninitialize();
	return 0;
}
