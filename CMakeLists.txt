cmake_minimum_required(VERSION 3.20)
project(MiniDAW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Plattformunabhaengiger Kern: Engine und Hilfsklassen.
add_library(dawcore STATIC
    src/engine/Engine.cpp
)
target_include_directories(dawcore PUBLIC src)

# Offline-Tests, laufen ohne Audiogeraet.
find_package(Threads REQUIRED)
add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE dawcore Threads::Threads)

enable_testing()
add_test(NAME engine_test COMMAND engine_test)

# Das Programm selbst gibt es nur fuer Windows.
if(WIN32)
    add_executable(minidaw
        src/main.cpp
        src/audio/WasapiDevice.cpp
    )
    target_compile_definitions(minidaw PRIVATE UNICODE _UNICODE)
    target_link_libraries(minidaw PRIVATE dawcore ole32 avrt)
endif()
