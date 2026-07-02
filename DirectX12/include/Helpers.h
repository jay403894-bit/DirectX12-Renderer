#pragma once



#define WIN32_LEAN_AND_MEAN

#include <Windows.h> // For HRESULT
#include <exception>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <cstdio>    // sprintf_s

// From DXSampleHelper.h 

// Source: https://github.com/Microsoft/DirectX-Graphics-Samples


inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        char buf[64];
        sprintf_s(buf, "HRESULT failed: 0x%08lX", static_cast<unsigned long>(hr));
        OutputDebugStringA(buf);   // shows in the VS Output window
        OutputDebugStringA("\n");
        throw std::runtime_error(buf);
    }
}
// Resolve a filename to sit next to the running .exe (= the build output dir, where
// FxCompile writes the .cso). This makes shader loading independent of the working
// directory -- which is what bit us: VS debugs with cwd = $(ProjectDir), so a stale
// .cso left in the project root was loaded instead of the freshly built one.
inline std::wstring ExeRelative(const std::wstring& filename) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir + filename;
}

inline std::vector<char> ReadFile(const std::wstring& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::string narrow(filename.begin(), filename.end());
        throw std::runtime_error("ReadFile: cannot open '" + narrow + "'");
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(static_cast<size_t>(size));
    file.read(buffer.data(), size);
    return buffer;
}
