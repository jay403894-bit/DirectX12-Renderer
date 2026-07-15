#include "../include/Window.h"
#include "../include/RendererCore.h"   // full definition needed here to call m_renderer->Resize()
#include "../include/imgui/imgui.h"    // IMGUI_IMPL_API + ImGui::GetCurrentContext
using namespace JLib;

// imgui_impl_win32.h deliberately hides this declaration inside an `#if 0` block (to avoid
// pulling <windows.h> into that helper header) and instructs you to forward-declare it in
// your own .cpp -- so `#include`ing the backend header does NOT bring it in. Window.h
// already includes <windows.h>, so the Win32 types below are available here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

Window::Window(HINSTANCE hInstance, const wchar_t* title, int width, int height, bool resizable)
    : m_hInstance(hInstance) {

    const wchar_t* className = L"DX12WindowClass";
    WNDCLASSEXW wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WndProcSetup, 0, 0, hInstance,
                       NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, className, NULL };
    RegisterClassExW(&wc);

    // Dropping WS_THICKFRAME/WS_MAXIMIZEBOX isn't just "no drag-resize" -- it also removes
    // Windows' system-enforced minimum resizable-window width/height (SM_CXMIN/SM_CYMIN). A
    // RESIZABLE window gets silently clamped to that minimum by CreateWindowExW/SetWindowPos
    // alike, no matter what size is requested -- observed firsthand sizing a small fixed-layout
    // window (a 300px-wide Tetris board): asking for an outer width well under the minimum still
    // produced a much wider client area, and shrinking it back down via SetWindowPos afterward
    // had NO effect at all, both consistent with a hard floor rather than a DPI rounding error.
    m_baseStyle = resizable ? WS_OVERLAPPEDWINDOW
                            : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));

    // CreateWindowExW's width/height are the OUTER window rect (title bar + borders included),
    // not the client area -- without this adjustment, the actual drawable area ends up smaller
    // than requested by the non-client chrome (title bar ~31-39px, thin borders). Usually
    // invisible on a large window, but anything sizing content to exactly fill the window (e.g.
    // a game board sized cellSize*rows == requested height) will visibly come up short.
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, m_baseStyle, FALSE);
    int outerWidth = rect.right - rect.left;
    int outerHeight = rect.bottom - rect.top;

    m_hWnd = CreateWindowExW(NULL, className, title, m_baseStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        outerWidth, outerHeight, NULL, NULL, hInstance, this); // PASS 'this' HERE

    // AdjustWindowRect's border-size math comes from SYSTEM DPI, not this specific window's
    // actual per-monitor DPI (the app is per-monitor-DPI-aware -- see main.cpp's
    // SetThreadDpiAwarenessContext call) -- on a non-100%-scaled display those can disagree by a
    // few pixels, so the client area we actually got can still miss the target. Measure it and,
    // if it's off, grow/shrink the OUTER rect by the exact delta and resize once more. This
    // converges in one correction pass since the chrome size itself doesn't change between calls
    // (a NON-resizable window has no minimum-size floor to fight, so this always converges here).
    RECT actualClient{};
    GetClientRect(m_hWnd, &actualClient);
    int actualWidth = actualClient.right - actualClient.left;
    int actualHeight = actualClient.bottom - actualClient.top;
    if (actualWidth != width || actualHeight != height) {
        int correctedOuterWidth = outerWidth + (width - actualWidth);
        int correctedOuterHeight = outerHeight + (height - actualHeight);
        SetWindowPos(m_hWnd, nullptr, 0, 0, correctedOuterWidth, correctedOuterHeight,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

Window::~Window() {
    if (m_hWnd) DestroyWindow(m_hWnd);
}

// This is the trick: It catches the creation message and sets the class pointer
LRESULT CALLBACK Window::WndProcSetup(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
        SetWindowLongPtr(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Window::WndProc));
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK Window::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Feed every message to ImGui's Win32 backend FIRST -- this is the only path by which
    // ImGui learns mouse position, clicks, and keys. Without it, ImGui windows can't be
    // dragged and no widget (button, radio, drag) responds. The context check guards the
    // window messages that arrive before InitImGui creates the context (the window is
    // created before RendererCore::InitImGui runs). Returning true when ImGui consumes a
    // message keeps it from also driving game input, but note the game's own input path is
    // GameInput (InputManager), not WndProc, so this only gates the WndProc-based handling
    // (resize/vsync/fullscreen/quit) below -- which is the desired behavior when a UI is up.
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    auto* pWindow = reinterpret_cast<Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (pWindow && pWindow->m_renderer) {
            RECT rc{};
            GetClientRect(hWnd, &rc);
            pWindow->m_renderer->Resize(rc.right - rc.left, rc.bottom - rc.top);
        }
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    {
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        switch (wParam) {
        case 'V':
            if (pWindow && pWindow->m_renderer)
                pWindow->m_renderer->SetVSync(!pWindow->m_renderer->VSync());
            break;
        // NOTE: VK_ESCAPE is deliberately NOT handled here. It used to PostQuitMessage(0),
        // which killed the whole app before any scene could see the key -- so a scene's
        // Escape->PopScene (return to menu) was pre-empted by a hard exit, and Escape quit
        // even from the start menu. Escape is now owned by the scene stack (see each Scene's
        // HandleInput); only the start menu's own QUIT command actually ends the program.
        case VK_F11:
            if (pWindow) pWindow->SetFullscreen(!pWindow->m_fullscreen);
            break;
        case VK_RETURN:
            if (alt && pWindow) pWindow->SetFullscreen(!pWindow->m_fullscreen);
            break;
        }
        return 0;
    }
    case WM_SYSCHAR:
        return 0;   // swallow Alt+Enter so DefWindowProc doesn't play the system ding
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void Window::Show() {
    ShowWindow(m_hWnd, SW_SHOW);
}

void Window::SetFullscreen(bool fullscreen) {
    if (m_fullscreen == fullscreen) return;
    m_fullscreen = fullscreen;

    if (m_fullscreen) {
        GetWindowRect(m_hWnd, &m_windowRect);   // remember the windowed rect to restore later

        // Borderless: strip the caption/frame so the client area covers the screen.
        UINT style = WS_OVERLAPPEDWINDOW &
            ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        SetWindowLongW(m_hWnd, GWL_STYLE, style);

        HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEX mi = {};
        mi.cbSize = sizeof(MONITORINFOEX);
        GetMonitorInfo(hMon, &mi);
        SetWindowPos(m_hWnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        ShowWindow(m_hWnd, SW_MAXIMIZE);
    }
    else {
        // Restore window decorations + the saved windowed rect -- m_baseStyle, NOT a hardcoded
        // WS_OVERLAPPEDWINDOW, so a non-resizable window (see the constructor) comes back
        // non-resizable instead of gaining a thick frame/maximize box it never had.
        SetWindowLong(m_hWnd, GWL_STYLE, m_baseStyle);
        SetWindowPos(m_hWnd, HWND_NOTOPMOST,
            m_windowRect.left, m_windowRect.top,
            m_windowRect.right - m_windowRect.left,
            m_windowRect.bottom - m_windowRect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        ShowWindow(m_hWnd, SW_NORMAL);
    }
}

bool Window::ProcessMessages() {
    MSG msg = {};
    if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.message != WM_QUIT;
}

/*int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Window myWindow(hInstance, L"Learning DirectX 12", 1280, 720);
    myWindow.Show();

    // Now initialize your DX12 objects using myWindow.GetHandle()

    while (myWindow.ProcessMessages()) {
        // Your Update and Render calls here
    }
    return 0;
}*/