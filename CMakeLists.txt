cmake_minimum_required(VERSION 3.10.0)
project(StreamLine VERSION 0.1.0 LANGUAGES C CXX)

#in case i want dependency management
if(DEFINED ENV{VCPKG_ROOT})
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
message("VCPKG found at: $ENV{VCPKG_ROOT}")
else()
message(SEND_ERROR "VCPKG not found")
endif()

set(HEADER
    "include/StreamLine.h"
)
set(SOURCE
    "src/StreamLine.cpp"
)

add_library(StreamLine STATIC ${HEADER} ${SOURCE})
target_compile_features(StreamLine PUBLIC cxx_std_17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

target_include_directories(StreamLine 
PUBLIC
    ${CMAKE_SOURCE_DIR}/include
PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)


include(CTest)
enable_testing()

add_executable(TestInstantiator "src/test.cpp")
target_link_libraries(TestInstantiator StreamLine)
add_test(NAME TestInstantiator
         COMMAND TestInstantiator)