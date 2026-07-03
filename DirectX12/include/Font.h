#pragma once
#include <string>
#include <unordered_map>
#include <map>
#include <DirectXMath.h>
#include "Mesh.h"
#include "ResourceManager.h"

class Renderer2D;

struct Glyph {
    float x = 0, y = 0, width = 0, height = 0;
    float xoffset = 0, yoffset = 0, xadvance = 0;
};

enum class TextAlign {
    Left,
    Center,
    Right
};

class Font {
public:
    void Load(const std::wstring& fntPath, const std::wstring& atlasPath, Renderer2D& renderer);

    // Main draw function - now with alignment and line support
    void SubmitText(Renderer2D& renderer, const std::string& text, float x, float y, float scale = 1.0f,
        DirectX::XMFLOAT4 color = { 1,1,1,1 }, float rotation = 0.0f,
        TextAlign align = TextAlign::Left, int zLayer = 2);

    float TextWidth(const std::string& text, float scale = 1.0f) const;
    float LineHeight() const { return lineHeight; }

private:
    void SubmitLine(Renderer2D& renderer, const std::string& line, float x, float y, float scale,
        DirectX::XMFLOAT4 color, float rotation, int zLayer);

    float Kerning(int first, int second) const;

    std::unordered_map<int, Glyph> glyphs;
    std::map<std::pair<int, int>, float> kerning;

    TextureResource* texture = nullptr;
    Mesh unitQuad{};
    float scaleW = 1.0f, scaleH = 1.0f;
    float lineHeight = 0.0f;
};