#pragma once
#include <GameInput.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdint>

using namespace GameInput::v3;

// Thin wrapper over GameInput v3's polling API. IGameInput/IGameInputReading are plain COM
// objects -- every GetCurrentReading() call hands back a NEW reference that must be Release()d,
// so this class owns that lifetime internally and only ever exposes plain bool/float queries.
//
// GameInput has no named-key enum (no "GameInputKeySpace") -- keyboard state is an array of
// GameInputKeyState{scanCode, codePoint, virtualKey, isDeadKey}, one entry per key CURRENTLY
// held down. IsKeyDown/Pressed/Released below take a Win32 virtual-key code (VK_SPACE, 'A', etc.)
// and scan for a matching virtualKey, so callers don't need to touch GameInputKeyState directly.
class InputManager
{
public:
    ~InputManager();

    bool Initialize();
    // Call once per frame, before querying any state. Diffs this frame's readings against last
    // frame's to derive Pressed/Released edges (GameInput only ever gives you a snapshot, not
    // edge events, for polled reads).
    void Update();

    bool IsKeyDown(uint8_t virtualKey) const;
    bool IsKeyPressed(uint8_t virtualKey) const;  // true only on the frame it transitions down
    bool IsKeyReleased(uint8_t virtualKey) const; // true only on the frame it transitions up

    // Actual screen-space cursor position (GameInputMouseState::absolutePositionX/Y) -- what
    // you want for "where is the cursor right now" (UI hit-testing, click position, etc.).
    DirectX::XMFLOAT2 GetMousePos() const {
        return { (float)m_MouseState.absolutePositionX, (float)m_MouseState.absolutePositionY };
    }
    // Relative delta SINCE THE LAST READING (GameInputMouseState::positionX/Y) -- NOT position.
    // Good for camera-look style accumulation; use GetMousePos() if you want where the cursor is.
    float GetMouseDeltaX() const { return (float)m_MouseState.positionX; }
    float GetMouseDeltaY() const { return (float)m_MouseState.positionY; }

    bool IsMouseButtonDown(GameInputMouseButtons button) const;
    bool IsMouseButtonPressed(GameInputMouseButtons button) const;  // true only on the click frame
    bool IsMouseButtonReleased(GameInputMouseButtons button) const; // true only on the release frame

    bool IsButtonDown(GameInputGamepadButtons button) const;
    bool IsButtonPressed(GameInputGamepadButtons button) const;
    bool IsButtonReleased(GameInputGamepadButtons button) const;
    float GetLeftTriggerAxis() const { return m_GamepadState.leftTrigger; }
    float GetRightTriggerAxis() const { return m_GamepadState.rightTrigger; }
    float GetLeftStickX() const { return m_GamepadState.leftThumbstickX; }
    float GetLeftStickY() const { return m_GamepadState.leftThumbstickY; }
    float GetRightStickX() const { return m_GamepadState.rightThumbstickX; }
    float GetRightStickY() const { return m_GamepadState.rightThumbstickY; }

private:
    IGameInput* m_GameInput = nullptr;

    // This frame's / last frame's held-key virtual-key codes -- diffed each Update() to derive
    // Pressed/Released. A std::vector (not a fixed array) since GetKeyState's count is dynamic.
    std::vector<uint8_t> m_KeysDown;
    std::vector<uint8_t> m_PrevKeysDown;

    GameInputMouseState m_MouseState = {};
    GameInputMouseButtons m_PrevMouseButtons = GameInputMouseNone;
    GameInputGamepadState m_GamepadState = {};
    GameInputGamepadButtons m_PrevGamepadButtons = GameInputGamepadNone;

    static bool Contains(const std::vector<uint8_t>& keys, uint8_t virtualKey);
};
