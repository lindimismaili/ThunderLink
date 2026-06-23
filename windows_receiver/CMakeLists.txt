cmake_minimum_required(VERSION 3.20)
project(ThunderLinkReceiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent Windows header pollution
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE)

# Repo root + shared cross-device log directory (logs/windows.log).
get_filename_component(TLNK_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
file(MAKE_DIRECTORY "${TLNK_REPO_ROOT}/logs")

# ---------- Dependencies ----------

include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Dear ImGui (lightweight native overlay UI). Header/source only -- no CMake of
# its own, so we compile the core + Win32/DX12 backends into our target.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.9
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)

# ---------- Shader Compilation ----------

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Find a shader compiler (dxc preferred, fxc fallback). Search every installed
# Windows 10/11 SDK bin version rather than a hard-coded list.
file(GLOB WINSDK_BIN_DIRS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64"
    "C:/Program Files/Windows Kits/10/bin/10.*/x64"
)

find_program(DXC_EXECUTABLE dxc HINTS ${WINSDK_BIN_DIRS})

if(NOT DXC_EXECUTABLE)
    # Fallback to fxc if dxc is not available
    find_program(FXC_EXECUTABLE fxc HINTS ${WINSDK_BIN_DIRS})
endif()

if(DXC_EXECUTABLE)
    message(STATUS "Shader compiler: ${DXC_EXECUTABLE}")
elseif(FXC_EXECUTABLE)
    message(STATUS "Shader compiler: ${FXC_EXECUTABLE}")
endif()

if(DXC_EXECUTABLE)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT_DIR}/vertex.cso
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E main
                ${SHADER_DIR}/vertex.hlsl -Fo ${SHADER_OUTPUT_DIR}/vertex.cso
        DEPENDS ${SHADER_DIR}/vertex.hlsl
        COMMENT "Compiling vertex shader with DXC"
    )
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT_DIR}/pixel.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E main
                ${SHADER_DIR}/pixel.hlsl -Fo ${SHADER_OUTPUT_DIR}/pixel.cso
        DEPENDS ${SHADER_DIR}/pixel.hlsl
        COMMENT "Compiling pixel shader with DXC"
    )
elseif(FXC_EXECUTABLE)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT_DIR}/vertex.cso
        COMMAND ${FXC_EXECUTABLE} /T vs_5_1 /E main
                /Fo ${SHADER_OUTPUT_DIR}/vertex.cso ${SHADER_DIR}/vertex.hlsl
        DEPENDS ${SHADER_DIR}/vertex.hlsl
        COMMENT "Compiling vertex shader with FXC"
    )
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT_DIR}/pixel.cso
        COMMAND ${FXC_EXECUTABLE} /T ps_5_1 /E main
                /Fo ${SHADER_OUTPUT_DIR}/pixel.cso ${SHADER_DIR}/pixel.hlsl
        DEPENDS ${SHADER_DIR}/pixel.hlsl
        COMMENT "Compiling pixel shader with FXC"
    )
else()
    message(WARNING "No shader compiler found (dxc or fxc). Shaders will be compiled at runtime using D3DCompileFromFile.")
endif()

if(DXC_EXECUTABLE OR FXC_EXECUTABLE)
    add_custom_target(Shaders ALL
        DEPENDS
            ${SHADER_OUTPUT_DIR}/vertex.cso
            ${SHADER_OUTPUT_DIR}/pixel.cso
    )
endif()

# ---------- Main Executable ----------

set(SOURCES
    src/main.cpp
    src/App/Application.cpp
    src/App/Window.cpp
    src/Network/FrameReceiver.cpp
    src/Network/InputSender.cpp
    src/Network/NetworkManager.cpp
    src/Decoder/VideoDecoderMF.cpp
    src/Renderer/D3D12Renderer.cpp
    src/Renderer/TextureManager.cpp
    src/Renderer/SwapChain.cpp
    src/Input/InputCapture.cpp
    src/Input/InputMapper.cpp
    src/Utils/FrameBuffer.cpp
    src/Utils/Logger.cpp
    src/Utils/PerformanceMonitor.cpp
)

set(HEADERS
    src/App/Application.h
    src/App/Config.h
    src/App/Window.h
    src/Network/FrameReceiver.h
    src/Network/InputSender.h
    src/Network/NetworkManager.h
    src/Decoder/VideoDecoder.h
    src/Renderer/D3D12Renderer.h
    src/Renderer/TextureManager.h
    src/Renderer/SwapChain.h
    src/Input/InputCapture.h
    src/Input/InputMapper.h
    src/Utils/FrameBuffer.h
    src/Utils/Logger.h
    src/Utils/PerformanceMonitor.h
)

add_executable(ThunderLinkReceiver WIN32 ${SOURCES} ${HEADERS} ${IMGUI_SOURCES})

target_include_directories(ThunderLinkReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(ThunderLinkReceiver PRIVATE
    d3d12
    dxgi
    d3dcompiler
    dxguid
    ws2_32
    # Media Foundation (hardware H.264/HEVC decode)
    mfplat
    mfuuid
    wmcodecdspuuid
    # Command-line parsing (CommandLineToArgvW)
    shell32
    nlohmann_json::nlohmann_json
)

# Pass shader output directory + cross-device log path as defines
target_compile_definitions(ThunderLinkReceiver PRIVATE
    SHADER_DIR=L"${SHADER_OUTPUT_DIR}"
    TLNK_LOG_FILE="${TLNK_REPO_ROOT}/logs/windows.log"
)

if(DXC_EXECUTABLE OR FXC_EXECUTABLE)
    add_dependencies(ThunderLinkReceiver Shaders)
endif()

# Always copy the shader SOURCES into the output shader dir so the runtime
# D3DCompileFromFile fallback works even when no dxc/fxc is available. The exe
# runs with build/ as its working directory, so it resolves "shaders/*.hlsl".
add_custom_command(TARGET ThunderLinkReceiver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_DIR}/vertex.hlsl ${SHADER_DIR}/pixel.hlsl ${SHADER_OUTPUT_DIR}
    COMMENT "Copying shader sources for runtime fallback"
)

# MSVC-specific settings
if(MSVC)
    target_compile_options(ThunderLinkReceiver PRIVATE /W4 /MP)
else()
    target_compile_options(ThunderLinkReceiver PRIVATE -Wall -Wextra)
endif()
