cmake_minimum_required(VERSION 3.20)
project(DirectX12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Third-party library locations (mirrors DirectX12.vcxproj's Additional*Directories)
# Override with -DDX12_LIBS_ROOT=... if these libraries live somewhere else.
# ---------------------------------------------------------------------------
set(DX12_LIBS_ROOT "C:/libs" CACHE PATH "Root folder containing DirectXTex, T_Threads and DirectX headers/libs")

set(DIRECTXTEX_INCLUDE_DIR "${DX12_LIBS_ROOT}/DirectXTex/include")
set(T_THREADS_INCLUDE_DIR  "${DX12_LIBS_ROOT}/T_Threads/include")
set(DIRECTX_INCLUDE_DIR    "${DX12_LIBS_ROOT}/DirectX/include")

set(DIRECTXTEX_LIB_DEBUG   "${DX12_LIBS_ROOT}/DirectXTex/debug")
set(DIRECTXTEX_LIB_RELEASE "${DX12_LIBS_ROOT}/DirectXTex/release")
set(T_THREADS_LIB_DEBUG    "${DX12_LIBS_ROOT}/T_Threads/lib/debug")
set(T_THREADS_LIB_RELEASE  "${DX12_LIBS_ROOT}/T_Threads/lib/release")

# ---------------------------------------------------------------------------
# Sources / headers
# ---------------------------------------------------------------------------
set(DX12_SOURCE_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/DirectX12/src")
set(DX12_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/DirectX12/include")

set(DX12_SOURCES
	${DX12_SOURCE_DIR}/main.cpp
	${DX12_SOURCE_DIR}/Window.cpp
	${DX12_SOURCE_DIR}/RendererCore.cpp
	${DX12_SOURCE_DIR}/Renderer2D.cpp
	${DX12_SOURCE_DIR}/ResourceManager.cpp
	${DX12_SOURCE_DIR}/Font.cpp
)

set(DX12_HEADERS
	${DX12_INCLUDE_DIR}/Colors.h
	${DX12_INCLUDE_DIR}/Font.h
	${DX12_INCLUDE_DIR}/Helpers.h
	${DX12_INCLUDE_DIR}/Mesh.h
	${DX12_INCLUDE_DIR}/Renderer2D.h
	${DX12_INCLUDE_DIR}/RendererCore.h
	${DX12_INCLUDE_DIR}/ResourceManager.h
	${DX12_INCLUDE_DIR}/SpriteAnimation.h
	${DX12_INCLUDE_DIR}/Vertex.h
	${DX12_INCLUDE_DIR}/Window.h
)

# Shader sources are added to the target too (HEADER_FILE_ONLY below) purely so they
# show up next to the .cpp/.h files in the IDE, matching the vcxproj's Solution Explorer layout.
set(DX12_SHADER_SOURCES
	${DX12_SOURCE_DIR}/PixelShader.hlsl
	${DX12_SOURCE_DIR}/VertexShader.hlsl
	${DX12_SOURCE_DIR}/ParticlePS.hlsl
	${DX12_SOURCE_DIR}/ParticleVS.hlsl
	${DX12_SOURCE_DIR}/UpdateParticles.hlsl
	${DX12_SOURCE_DIR}/SpawnParticles.hlsl
	${DX12_SOURCE_DIR}/WaveParticles.hlsl
	${DX12_SOURCE_DIR}/BuildDispatchArgs.hlsl
	${DX12_SOURCE_DIR}/ResetAliveCounter.hlsl
	${DX12_SOURCE_DIR}/CompactParticles.hlsl
	${DX12_SOURCE_DIR}/ParticleCommon.hlsli
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(DirectX12 WIN32
	${DX12_SOURCES}
	${DX12_HEADERS}
	${DX12_SHADER_SOURCES}
)

# Shaders aren't C++ -- keep them visible in the IDE without being fed to the compiler.
set_source_files_properties(${DX12_SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(DirectX12 PRIVATE
	${DX12_INCLUDE_DIR}
	${DIRECTXTEX_INCLUDE_DIR}
	${T_THREADS_INCLUDE_DIR}
	${DIRECTX_INCLUDE_DIR}
)

target_compile_definitions(DirectX12 PRIVATE
	UNICODE
	_UNICODE
	NOMINMAX
	_CONSOLE
)

if (MSVC)
	target_compile_options(DirectX12 PRIVATE /W3 /sdl /permissive-)
endif()

target_link_directories(DirectX12 PRIVATE
	$<$<CONFIG:Debug>:${T_THREADS_LIB_DEBUG}>
	$<$<CONFIG:Debug>:${DIRECTXTEX_LIB_DEBUG}>
	$<$<CONFIG:Release>:${T_THREADS_LIB_RELEASE}>
	$<$<CONFIG:Release>:${DIRECTXTEX_LIB_RELEASE}>
)

target_link_libraries(DirectX12 PRIVATE
	T_Threads.lib
	DirectXTex.lib
	d3d12.lib
	dxgi.lib
)

source_group("src" FILES ${DX12_SOURCES})
source_group("include" FILES ${DX12_HEADERS})
source_group("Shaders" FILES ${DX12_SHADER_SOURCES})

# ---------------------------------------------------------------------------
# HLSL shader compilation (Shader Model 6.1 -> requires dxc, not the legacy fxc).
# CMake has no built-in equivalent of MSBuild's <FxCompile> item, so each shader
# gets its own custom command. .cso files are written next to the .exe because
# Helpers.h's ExeRelative() loads shaders from the running executable's folder.
# ---------------------------------------------------------------------------
find_program(DXC_EXECUTABLE
	NAMES dxc
	HINTS "$ENV{WindowsSdkDir}bin/$ENV{WindowsSdkVersion}x64"
)
if (NOT DXC_EXECUTABLE)
	file(GLOB _dxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
	if (_dxc_candidates)
		list(SORT _dxc_candidates)
		list(GET _dxc_candidates -1 DXC_EXECUTABLE)
	endif()
endif()
if (NOT DXC_EXECUTABLE)
	message(FATAL_ERROR "dxc.exe not found. Install the Windows 10/11 SDK (it ships in Windows Kits/10/bin/<ver>/x64) or pass -DDXC_EXECUTABLE=<path to dxc.exe>.")
endif()
message(STATUS "Using dxc: ${DXC_EXECUTABLE}")

set(DX12_SHADER_OUTPUTS "")

# Ninja (the generator pinned in CMakePresets.json) is single-config, so the exe always
# lands directly in the build dir -- no Debug/Release subfolder to chase via a
# TARGET_FILE_DIR generator expression (which CMake 4.4's function-scope evaluation
# rejects here anyway). This matches ExeRelative() in Helpers.h, which loads .cso files
# from the running executable's own folder.
set_target_properties(DirectX12 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# add_hlsl_shader(<name without extension> <entry point> <shader profile>)
function(add_hlsl_shader name entry profile)
	set(_src "${DX12_SOURCE_DIR}/${name}.hlsl")
	set(_out "${CMAKE_BINARY_DIR}/${name}.cso")
	add_custom_command(
		OUTPUT ${_out}
		COMMAND ${DXC_EXECUTABLE} /nologo /E${entry} /T ${profile} /I ${DX12_SOURCE_DIR} /Fo ${_out} ${_src}
		DEPENDS ${_src} "${DX12_SOURCE_DIR}/ParticleCommon.hlsli"
		COMMENT "Compiling shader ${name}.hlsl (${entry}, ${profile})"
		VERBATIM
	)
	set(DX12_SHADER_OUTPUTS ${DX12_SHADER_OUTPUTS} ${_out} PARENT_SCOPE)
endfunction()

add_hlsl_shader(PixelShader        PSMain ps_6_1)
add_hlsl_shader(VertexShader       VSMain vs_6_1)
add_hlsl_shader(ParticlePS         PSMain ps_6_1)
add_hlsl_shader(ParticleVS         VSMain vs_6_1)
add_hlsl_shader(UpdateParticles    main   cs_6_1)
add_hlsl_shader(SpawnParticles     main   cs_6_1)
add_hlsl_shader(WaveParticles      main   cs_6_1)
add_hlsl_shader(BuildDispatchArgs  main   cs_6_1)
add_hlsl_shader(ResetAliveCounter  main   cs_6_1)
add_hlsl_shader(CompactParticles   main   cs_6_1)

add_custom_target(Shaders DEPENDS ${DX12_SHADER_OUTPUTS})
add_dependencies(DirectX12 Shaders)

# ---------------------------------------------------------------------------
# Optional: Microsoft.GameInput (currently unused -- InputManager.cpp/.h are not
# compiled in and are #include-commented-out in main.cpp). The NuGet package is
# already restored under packages/Microsoft.GameInput.3.4.218. Flip this on and
# add src/InputManager.cpp to DX12_SOURCES if/when GameInput input gets wired back in.
# ---------------------------------------------------------------------------
option(DX12_ENABLE_GAMEINPUT "Link Microsoft.GameInput and define USING_GAMEINPUT" OFF)
if (DX12_ENABLE_GAMEINPUT)
	set(GAMEINPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/packages/Microsoft.GameInput.3.4.218/native")
	target_include_directories(DirectX12 PRIVATE "${GAMEINPUT_ROOT}/include")
	target_link_directories(DirectX12 PRIVATE "${GAMEINPUT_ROOT}/lib/x64")
	target_link_libraries(DirectX12 PRIVATE GameInput.lib)
	target_compile_definitions(DirectX12 PRIVATE USING_GAMEINPUT)
endif()
