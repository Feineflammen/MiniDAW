cmake_minimum_required(VERSION 3.20)
project(MiniDAW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(minidaw
    src/main.cpp
    src/audio/WasapiDevice.cpp
)

target_include_directories(minidaw PRIVATE src)
target_compile_definitions(minidaw PRIVATE UNICODE _UNICODE)
target_link_libraries(minidaw PRIVATE ole32 avrt)

if(MSVC)
    target_compile_options(minidaw PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(minidaw PRIVATE -Wall -Wextra)
endif()
