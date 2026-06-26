cmake_minimum_required(VERSION 3.20.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(StreamLine)

set(HEADER
    "include/StreamLine.h"
    "include/WaitGroup.h"
    "include/Concept.h"
    "include/Latch.h"
    "include/Barrier.h"
    "include/Pipeline.h"
    "include/CMap.h"
    "include/SpinLock.h"
    "include/SimdPort.h"
)
set(SOURCE
    "src/StreamLine.cpp"
    "src/SpinLock.cpp"

)


add_library(StreamLine STATIC ${HEADER} ${SOURCE})
target_compile_features(StreamLine PUBLIC cxx_std_20)
target_compile_features(StreamLine PRIVATE cxx_constexpr)
target_compile_options(StreamLine PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fexceptions -frtti>
    $<$<CXX_COMPILER_ID:Clang>:-fexceptions -frtti>
    $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
)

message("Using CXX Compiler: ${CMAKE_CXX_COMPILER_ID}")

target_include_directories(StreamLine 
PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Architecture detection for SimdPort.h (x86-64 vs ARM64). Belt-and-suspenders:
# the shim also falls back to compiler builtins (_M_X64 / __aarch64__), so this
# is informational, not load-bearing.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    target_compile_definitions(StreamLine PUBLIC SL_ARM64=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_definitions(StreamLine PUBLIC SL_X64=1)
endif()

include(CTest)
enable_testing()

add_executable(TestInstantiator "src/test.cpp")
target_link_libraries(TestInstantiator StreamLine)
# CMap's concurrency test requires an optimized build: at -O0 the seqlock
# acquire loads are unoptimized, widening the lock-free TOCTOU window enough
# to make the contention race statistically frequent. Force optimization on
# the test target regardless of the selected config.
target_compile_options(TestInstantiator PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O2>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>
)
add_test(NAME TestInstantiator
         COMMAND TestInstantiator)