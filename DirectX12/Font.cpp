#include "Font.h"
#include "Renderer.h"
#include "Helpers.h"
#include <fstream>
#include <sstream>
#include <cstdio>

// Pulls value out of a "key=value" or "key=\"value\"" token. Returns 0 if key not found
// on this line (every field we care about is numeric, so 0 is a safe "absent" default).
static float FindField(const std::string& line, const char* key) {
    size_t pos = line.find(key);
    if (pos == std::string::npos) return 0.0f;
    pos += strlen(key);
    if (pos >= line.size() || line[pos] != '=') return 0.0f;
    ++pos;
    return static_cast<float>(atof(line.c_str() + pos));
}

void Font::Load(const std::wstring& fntPath, const std::wstring& atlasPath, Renderer& renderer) {
    // .fnt is plain text (AngelCode BMFont text format) -- read and parse line by line.
    std::ifstream file(fntPath);
    if (!file) {
        std::string p(fntPath.begin(), fntPath.end());
        throw std::runtime_error("Font::Load: cannot open '" + p + "'");
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("common", 0) == 0) {
            scaleW = FindField(line, "scaleW");
            scaleH = FindField(line, "scaleH");
            lineHeight = FindField(line, "lineHeight");
        } else if (line.rfind("char ", 0) == 0) {
            int id = static_cast<int>(FindField(line, "id"));
            Glyph g;
            g.x = FindField(line, "x");
            g.y = FindField(line, "y");
            g.width = FindField(line, "width");
            g.height = FindField(line, "height");
            g.xoffset = FindField(line, "xoffset");
            g.yoffset = FindField(line, "yoffset");
            g.xadvance = FindField(line, "xadvance");
            glyphs[id] = g;
        } else if (line.rfind("kerning ", 0) == 0) {
            int first = static_cast<int>(FindField(line, "first"));
            int second = static_cast<int>(FindField(line, "second"));
            float amount = FindField(line, "amount");
            kerning[{first, second}] = amount;
        }
    }
    if (scaleW <= 0.0f) scaleW = 1.0f;
    if (scaleH <= 0.0f) scaleH = 1.0f;

    // Atlas texture -- same LoadTexture + ExecuteUploadCommand idiom used for wall.png/wood.png.
    ResourceManager* rm = renderer.GetResourceManager();
    renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
        texture = rm->LoadTexture(atlasPath, cmd);
        });

    // ONE unit quad (-0.5..0.5, UV 0..1), shared by every glyph. Per-instance uvOffset/uvScale
    // (set in DrawText) selects which part of the atlas each instance actually samples --
    // same quad topology as main.cpp's moreVertices/quadIndices.
    Vertex quadVerts[] = {
        { -0.5f,  0.5f, 0.0f,  1,1,1,1,  0.0f, 0.0f }, // Top-Left
        {  0.5f,  0.5f, 0.0f,  1,1,1,1,  1.0f, 0.0f }, // Top-Right
        { -0.5f, -0.5f, 0.0f,  1,1,1,1,  0.0f, 1.0f }, // Bottom-Left
        {  0.5f, -0.5f, 0.0f,  1,1,1,1,  1.0f, 1.0f }, // Bottom-Right
    };
    uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 };
    unitQuad = rm->CreateMesh(quadVerts, 4, quadIndices, 6);
}

float Font::Kerning(int first, int second) const {
    auto it = kerning.find({ first, second });
    return it != kerning.end() ? it->second : 0.0f;
}

void Font::SubmitText(Renderer& renderer, const std::string& text, float x, float y, float scale,
    DirectX::XMFLOAT4 color, float rotation, TextAlign align, int zLayer)
{
    if (!texture || text.empty()) return;

    std::istringstream iss(text);
    std::string line;
    float currentY = y;

    while (std::getline(iss, line, '\n')) {
        float lineWidth = TextWidth(line, scale);
        float startX = x;

        if (align == TextAlign::Center) {
            startX = x - lineWidth * 0.5f;
        }
        else if (align == TextAlign::Left) {           // ← changed
            startX = x - lineWidth;                    // ← was Right, now Left
        }
        else if (align == TextAlign::Right) {          // ← changed
            startX = x;                                // ← was Left, now Right
        }

        SubmitLine(renderer, line, startX, currentY, scale, color, rotation, zLayer);
        currentY += lineHeight * scale * 1.1f;
    }
}

void Font::SubmitLine(Renderer& renderer, const std::string& line, float x, float y, float scale,
    DirectX::XMFLOAT4 color, float rotation, int zLayer)
{
    float penX = 0.0f;
    int prev = -1;

    for (char c : line) {
        int id = static_cast<unsigned char>(c);
        auto it = glyphs.find(id);
        if (it == glyphs.end()) {
            if (id == ' ') {
                penX += 10.0f * scale; // crude space width
            }
            prev = id;
            continue;
        }

        const Glyph& g = it->second;

        if (prev >= 0) penX += Kerning(prev, id) * scale;

        if (g.width > 0 && g.height > 0) {
            float glyphW = g.width * scale;
            float glyphH = g.height * scale;

            float centerX = x + penX + g.xoffset * scale + glyphW * 0.5f;
            float centerY = y + g.yoffset * scale + glyphH * 0.5f;

            DirectX::XMFLOAT2 uvOffset{ g.x / scaleW, g.y / scaleH };
            DirectX::XMFLOAT2 uvScale{ g.width / scaleW, g.height / scaleH };

            renderer.Submit(unitQuad, texture, { centerX, centerY }, glyphW, glyphH,
                zLayer, color, rotation, uvOffset, uvScale, true);
        }

        penX += g.xadvance * scale;
        prev = id;
    }
}

float Font::TextWidth(const std::string& text, float scale) const {
    float width = 0.0f;
    int prev = -1;
    for (char c : text) {
        int id = static_cast<unsigned char>(c);
        auto it = glyphs.find(id);
        if (it == glyphs.end()) { prev = id; continue; }
        if (prev >= 0) width += Kerning(prev, id) * scale;
        width += it->second.xadvance * scale;
        prev = id;
    }
    return width;
}
