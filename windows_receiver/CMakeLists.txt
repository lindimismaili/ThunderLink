cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ThunderLinkReceiver LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The receiver requires native Windows. For portable regression tests use cmake -S ../tests -B build-tests.")
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prevent Windows header pollution
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

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

# All runtime resources are embedded; launching never depends on the cwd.
option(TLNK_PORTABLE "Use per-user logs and produce the portable app" OFF)
get_filename_component(TLNK_PACKAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../packaging" ABSOLUTE)
file(READ "${TLNK_PACKAGING_DIR}/VERSION" TLNK_VERSION)
string(STRIP "${TLNK_VERSION}" TLNK_VERSION)
string(REPLACE "." "," TLNK_VERSION_COMMAS "${TLNK_VERSION}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vertex.hlsl" TLNK_VERTEX_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pixel.hlsl" TLNK_PIXEL_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS shaders/vertex.hlsl shaders/pixel.hlsl)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${TLNK_PACKAGING_DIR}/windows/EmbeddedShaders.h.in" "generated/EmbeddedShaders.h" @ONLY)
enable_language(RC)
configure_file("${TLNK_PACKAGING_DIR}/windows/ThunderLink.rc.in" "generated/ThunderLink.rc" @ONLY)

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
    src/Cross/CrossController.cpp
    src/Cross/CrossDisplays.cpp
    src/Cross/CrossInputController.cpp
    src/Cross/CrossTransport.cpp
    src/Cross/CrossDiscovery.cpp
    src/Cross/CrossIdentity.cpp
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

add_executable(ThunderLinkReceiver WIN32 ${SOURCES} ${HEADERS} ${IMGUI_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/generated/ThunderLink.rc")
set_target_properties(ThunderLinkReceiver PROPERTIES OUTPUT_NAME ThunderLink)

target_include_directories(ThunderLinkReceiver PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
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
    bcrypt
    ncrypt
    crypt32
    secur32
    advapi32
    dnsapi
    iphlpapi
    wtsapi32
    # Media Foundation (hardware H.264/HEVC decode)
    mf
    mfplat
    mfuuid
    wmcodecdspuuid
    # Command-line parsing (CommandLineToArgvW)
    shell32
    ole32
    dwmapi
    nlohmann_json::nlohmann_json
)

if(TLNK_PORTABLE)
    target_compile_definitions(ThunderLinkReceiver PRIVATE TLNK_PORTABLE=1)
else()
    target_compile_definitions(ThunderLinkReceiver PRIVATE TLNK_LOG_FILE="${TLNK_REPO_ROOT}/logs/windows.log")
endif()
if(MSVC)
    # Manifest is already embedded by the resource compiler.
    # Cross does not need Media Foundation. Screen Share checks availability
    # before invoking its delayed imports so Windows N can still run Cross.
    target_link_libraries(ThunderLinkReceiver PRIVATE delayimp)
    target_link_options(ThunderLinkReceiver PRIVATE /MANIFEST:NO /DELAYLOAD:mfplat.dll /DELAYLOAD:mf.dll)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()

# MSVC-specific settings
if(MSVC)
    target_compile_options(ThunderLinkReceiver PRIVATE /W4 /MP)
else()
    target_compile_options(ThunderLinkReceiver PRIVATE -Wall -Wextra)
endif()
