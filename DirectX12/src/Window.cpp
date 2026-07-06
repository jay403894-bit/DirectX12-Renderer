#include "../include/Window.h"
#include "../include/RendererCore.h"   // full definition needed here to call m_renderer->Resize()
using namespace JGL;

Window::Window(HINSTANCE hInstance, const wchar_t* title, int width, int height)
    : m_hInstance(hInstance) {

    const wchar_t* className = L"DX12WindowClass";
    WNDCLASSEXW wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WndProcSetup, 0, 0, hInstance,
                       NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, className, NULL };
    RegisterClassExW(&wc);

    m_hWnd = CreateWindowExW(NULL, className, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        width, height, NULL, NULL, hInstance, this); // PASS 'this' HERE
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
        case VK_ESCAPE:
            PostQuitMessage(0);
            break;
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
        // Restore window decorations + the saved windowed rect.
        SetWindowLong(m_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
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