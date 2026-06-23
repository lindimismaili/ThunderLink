cmake_minimum_required(VERSION 3.20)
project(ThunderLinkReceiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent Windows header pollution
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE)

# ---------- Dependencies ----------

include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# ---------- Shader Compilation ----------

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Find DXC shader compiler
find_program(DXC_EXECUTABLE dxc
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
    PATH_SUFFIXES x64
)

if(NOT DXC_EXECUTABLE)
    # Fallback to fxc if dxc is not available
    find_program(FXC_EXECUTABLE fxc
        HINTS
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
        PATH_SUFFIXES x64
    )
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

add_executable(ThunderLinkReceiver WIN32 ${SOURCES} ${HEADERS})

target_include_directories(ThunderLinkReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
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

# Pass shader output directory as a define
target_compile_definitions(ThunderLinkReceiver PRIVATE
    SHADER_DIR=L"${SHADER_OUTPUT_DIR}"
)

if(DXC_EXECUTABLE OR FXC_EXECUTABLE)
    add_dependencies(ThunderLinkReceiver Shaders)
endif()

# MSVC-specific settings
if(MSVC)
    target_compile_options(ThunderLinkReceiver PRIVATE /W4 /MP)
else()
    target_compile_options(ThunderLinkReceiver PRIVATE -Wall -Wextra)
endif()
