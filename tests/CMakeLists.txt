cmake_minimum_required(VERSION 3.20)
project(ThunderLinkPortableTests LANGUAGES CXX)

enable_testing()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(CrossLayoutTests CrossLayoutTests.cpp)
target_include_directories(CrossLayoutTests PRIVATE ../common)
if(MSVC)
    target_compile_options(CrossLayoutTests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(CrossLayoutTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME CrossLayoutTests COMMAND CrossLayoutTests)

add_executable(CrossInputStateTests CrossInputStateTests.cpp)
target_include_directories(CrossInputStateTests PRIVATE ../common)
if(MSVC)
    target_compile_options(CrossInputStateTests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(CrossInputStateTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME CrossInputStateTests COMMAND CrossInputStateTests)

add_executable(CrossFramingTests CrossFramingTests.cpp)
target_include_directories(CrossFramingTests PRIVATE ../common)
if(MSVC)
    target_compile_options(CrossFramingTests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(CrossFramingTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME CrossFramingTests COMMAND CrossFramingTests)

add_executable(CrossHeartbeatTests CrossHeartbeatTests.cpp)
target_include_directories(CrossHeartbeatTests PRIVATE ../common)
if(MSVC)
    target_compile_options(CrossHeartbeatTests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(CrossHeartbeatTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME CrossHeartbeatTests COMMAND CrossHeartbeatTests)

add_executable(FrameBufferTests
    FrameBufferTests.cpp
    ../windows_receiver/src/Utils/FrameBuffer.cpp
    ../windows_receiver/src/Utils/Logger.cpp
    ../windows_receiver/src/Utils/PerformanceMonitor.cpp
)
target_include_directories(FrameBufferTests PRIVATE
    ../common
    ../windows_receiver/src
    ../windows_receiver/src/Utils
)
target_link_libraries(FrameBufferTests PRIVATE Threads::Threads)
target_compile_definitions(FrameBufferTests PRIVATE
    TLNK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MSVC)
    target_compile_options(FrameBufferTests PRIVATE /W4 /UNDEBUG)
    target_compile_definitions(FrameBufferTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_compile_options(FrameBufferTests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME FrameBufferTests COMMAND FrameBufferTests)

# Network manager transport tests are maintained with the receiver because they
# exercise receiver-owned interfaces.  Keep the basic reassembly suite usable
# independently when that optional directory is not present.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../windows_receiver/tests/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../windows_receiver/tests"
                     "${CMAKE_CURRENT_BINARY_DIR}/receiver-tests")
endif()
