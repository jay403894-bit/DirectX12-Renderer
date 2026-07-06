// Offline atlas packer -- the build-time half of the atlas story (see SpriteAtlas.h's comment
// in the Renderer project for the runtime half). Takes a directory of loose PNGs and produces
// exactly the pair SpriteAtlas::Load() expects: one packed atlas PNG, and a plain-text sidecar
// mapping each source file's name to its pixel rect within that atlas. Never touches D3D12 at
// all -- this is pure CPU image composition via DirectXTex's WIC load/save, run once, offline,
// with its output checked in / shipped alongside the rest of the game's assets.
//
// Usage: AtlasPacker.exe <inputDir> <outAtlasPng> <outSidecarTxt>
// INITGUID + wincodec.h before DirectXTex.h -- DirectXTex.h itself only *declares*
// GUID_ContainerFormatPng (extern), it doesn't define its value; INITGUID makes this
// translation unit's first include of wincodec.h actually emit the definition, avoiding an
// unresolved-external/undeclared-identifier depending on link order.
#define INITGUID
#include <Windows.h>
#include <wincodec.h>
#include <DirectXTex.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
    struct SourceImage {
        std::string key;              // sidecar region name -- filename without extension
        DirectX::ScratchImage scratch; // owns the decoded (and possibly reformatted) pixels
        UINT width = 0, height = 0;
        UINT x = 0, y = 0;             // filled in by the packer below
    };

    // Simple shelf packer: images are placed left-to-right in rows ("shelves"), wrapping to a
    // new shelf once a row would exceed maxWidth. Not as tight as a true bin-packer (skyline/
    // MaxRects), but simple, deterministic, and good enough for the sprite counts this engine
    // actually deals with -- a tighter packer can replace just this function later without
    // touching the sidecar format or SpriteAtlas at all.
    void PackShelves(std::vector<SourceImage>& images, UINT maxWidth, UINT& outAtlasWidth, UINT& outAtlasHeight) {
        // Tallest-first so a shelf's height (set by its first/tallest image) wastes as little
        // space as possible under shorter images placed later on the same shelf.
        std::sort(images.begin(), images.end(), [](const SourceImage& a, const SourceImage& b) {
            return a.height > b.height;
            });

        UINT cursorX = 0, cursorY = 0, shelfHeight = 0;
        for (auto& img : images) {
            if (cursorX != 0 && cursorX + img.width > maxWidth) {
                cursorY += shelfHeight;
                cursorX = 0;
                shelfHeight = 0;
            }
            img.x = cursorX;
            img.y = cursorY;
            cursorX += img.width;
            shelfHeight = std::max(shelfHeight, img.height);
        }
        outAtlasWidth = maxWidth;
        outAtlasHeight = cursorY + shelfHeight;
    }
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::wcerr << L"Usage: AtlasPacker.exe <inputDir> <outAtlasPng> <outSidecarTxt>\n";
        return 1;
    }
    std::wstring inputDir = argv[1];
    std::wstring outAtlasPng = argv[2];
    std::wstring outSidecarTxt = argv[3];

    // WIC (LoadFromWICFile/SaveToWICFile) needs COM initialized on the calling thread -- same
    // requirement ResourceManager.cpp notes for its own WIC calls (there it's satisfied by
    // wWinMain; here there's no such caller, so it's done explicitly).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << hr << L"\n";
        return 1;
    }

    // Gather + sort file paths first so the packed layout is 100% deterministic across runs
    // (matters for reproducible builds / diffable sidecars) -- same reasoning as the raylib
    // AssetRegistry's loadAllAssets comment.
    std::vector<fs::path> filePaths;
    for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == L".png") {
            filePaths.push_back(entry.path());
        }
    }
    std::sort(filePaths.begin(), filePaths.end());

    if (filePaths.empty()) {
        std::wcerr << L"No .png files found under '" << inputDir << L"'\n";
        CoUninitialize();
        return 1;
    }

    std::vector<SourceImage> images;
    images.reserve(filePaths.size());
    uint64_t totalArea = 0;
    UINT maxSingleWidth = 0;

    for (const auto& path : filePaths) {
        DirectX::ScratchImage loaded;
        hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, loaded);
        if (FAILED(hr)) {
            std::wcerr << L"Skipping '" << path.wstring() << L"' -- failed to load (0x"
                << std::hex << hr << L")\n";
            continue;
        }

        const auto& meta = loaded.GetMetadata();
        SourceImage img;
        img.key = path.stem().string(); // filename without extension -- the sidecar's region name
        img.width = (UINT)meta.width;
        img.height = (UINT)meta.height;

        // Normalize every source image to one common format before compositing -- the canvas
        // below is a single ScratchImage, so every source's pixel rows must already be in the
        // same format (and byte layout) the canvas uses.
        if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            DirectX::ScratchImage converted;
            hr = DirectX::Convert(*loaded.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
                DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
            if (FAILED(hr)) {
                std::wcerr << L"Skipping '" << path.wstring() << L"' -- format conversion failed (0x"
                    << std::hex << hr << L")\n";
                continue;
            }
            img.scratch = std::move(converted);
        } else {
            img.scratch = std::move(loaded);
        }

        totalArea += (uint64_t)img.width * img.height;
        maxSingleWidth = std::max(maxSingleWidth, img.width);
        images.push_back(std::move(img));
    }

    if (images.empty()) {
        std::wcerr << L"Every source image failed to load -- nothing to pack.\n";
        CoUninitialize();
        return 1;
    }

    // Rough target width: a roughly-square atlas, never narrower than the widest single source
    // image (otherwise PackShelves could never place it).
    UINT maxWidth = std::max<UINT>(maxSingleWidth, (UINT)std::ceil(std::sqrt((double)totalArea)));
    UINT atlasWidth = 0, atlasHeight = 0;
    PackShelves(images, maxWidth, atlasWidth, atlasHeight);

    DirectX::ScratchImage canvas;
    hr = canvas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, atlasWidth, atlasHeight, 1, 1);
    if (FAILED(hr)) {
        std::wcerr << L"Failed to allocate " << atlasWidth << L"x" << atlasHeight << L" canvas (0x"
            << std::hex << hr << L")\n";
        CoUninitialize();
        return 1;
    }
    const DirectX::Image* canvasImg = canvas.GetImage(0, 0, 0);
    memset(canvasImg->pixels, 0, canvasImg->slicePitch); // BLANK/transparent background

    for (const auto& img : images) {
        const DirectX::Image* src = img.scratch.GetImage(0, 0, 0);
        for (UINT row = 0; row < img.height; ++row) {
            uint8_t* dst = canvasImg->pixels + (size_t)(img.y + row) * canvasImg->rowPitch
                + (size_t)img.x * 4; // 4 bytes/pixel -- R8G8B8A8
            const uint8_t* srcRow = src->pixels + (size_t)row * src->rowPitch;
            memcpy(dst, srcRow, (size_t)img.width * 4);
        }
    }

    hr = DirectX::SaveToWICFile(*canvasImg, DirectX::WIC_FLAGS_NONE,
        GUID_ContainerFormatPng, outAtlasPng.c_str());
    if (FAILED(hr)) {
        std::wcerr << L"Failed to save atlas PNG to '" << outAtlasPng << L"' (0x"
            << std::hex << hr << L")\n";
        CoUninitialize();
        return 1;
    }

    // Sidecar format matches SpriteAtlas::Load()'s parser exactly (SpriteAtlas.cpp):
    //   atlas width=.. height=..
    //   region name="key" x=.. y=.. width=.. height=..
    std::ofstream sidecar(outSidecarTxt);
    if (!sidecar) {
        std::wcerr << L"Failed to open sidecar output '" << outSidecarTxt << L"'\n";
        CoUninitialize();
        return 1;
    }
    sidecar << "atlas width=" << atlasWidth << " height=" << atlasHeight << "\n";
    for (const auto& img : images) {
        sidecar << "region name=\"" << img.key << "\" x=" << img.x << " y=" << img.y
            << " width=" << img.width << " height=" << img.height << "\n";
    }

    std::wcout << L"Packed " << images.size() << L" image(s) into " << atlasWidth << L"x"
        << atlasHeight << L" -> " << outAtlasPng << L" + " << outSidecarTxt << L"\n";

    CoUninitialize();
    return 0;
}
