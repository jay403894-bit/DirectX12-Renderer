#pragma once
#include <windows.h>

class Renderer; // <--- Forward declaration instead of #include
class Renderer2D;
class Window {
public:
    Window(HINSTANCE hInstance, const wchar_t* title, int width, int height);
    ~Window();

    void Show();
    bool ProcessMessages(); // Replaces your while(PeekMessage) loop

    // The window forwards size changes to this renderer (set after construction).
    void SetRenderer(Renderer* r) { m_renderer = r; }

    void SetFullscreen(bool fullscreen);   // borderless-fullscreen toggle (Alt+Enter / F11)

    HWND GetHandle() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WndProcSetup(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hWnd;
    HINSTANCE m_hInstance;
    Renderer* m_renderer = nullptr;  // not owned; just notified of resizes
    bool m_fullscreen = false;
    RECT m_windowRect{};             // saved windowed-mode rect, to restore from fullscreen
};