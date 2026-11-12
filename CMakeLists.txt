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

# Plattformunabhaengiger Kern: Engine, Dateien, Projektmodell.
add_library(dawcore STATIC
    src/engine/Engine.cpp
    src/engine/Synth.cpp
    src/disk/PeakData.cpp
    src/disk/RecordSink.cpp
    src/disk/TrackStream.cpp
    src/disk/WavFile.cpp
    src/model/ProjectController.cpp
)
target_include_directories(dawcore PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(dawcore PUBLIC Threads::Threads)

# Offline-Tests, laufen ohne Audiogeraet.
add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE dawcore Threads::Threads)

enable_testing()
add_test(NAME engine_test COMMAND engine_test)

# Das Programm selbst gibt es nur fuer Windows.
if(WIN32)
    enable_language(RC)
    add_executable(minidaw WIN32
        src/main.cpp
        src/audio/WasapiDevice.cpp
        src/midi/MidiInput.cpp
        src/midi/MidiOutput.cpp
        src/ui/Canvas.cpp
        src/ui/MainView.cpp
        src/ui/MainWindow.cpp
        src/ui/PianoRoll.cpp
        src/minidaw.rc
        src/minidaw.manifest
    )
    target_compile_definitions(minidaw PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(minidaw PRIVATE dawcore ole32 avrt d2d1 dwrite windowscodecs shell32 user32 winmm)
endif()
