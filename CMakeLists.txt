cmake_minimum_required(VERSION 3.20)
project(MiniDAW LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Die beiden Quelldateien der VST3-Schnittstellen (Kennungen der Basis-
# Schnittstellen). Fremder Code: ohne unsere Warnstufe uebersetzen.
add_library(vst3interfaces STATIC
    third_party/vst3/pluginterfaces/base/funknown.cpp
    third_party/vst3/pluginterfaces/base/coreiids.cpp
)
target_include_directories(vst3interfaces PUBLIC third_party/vst3)
if(MSVC)
    target_compile_options(vst3interfaces PRIVATE /W0 /utf-8)
else()
    target_compile_options(vst3interfaces PRIVATE -w)
endif()

# Reproduzierbar bauen: keine Zeitstempel in den Dateien. Gleicher Quellcode
# und gleicher Compiler ergeben dann Byte fuer Byte dieselbe exe.
if(MSVC)
    add_compile_options(/Brepro)
    add_link_options(/Brepro)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Plattformunabhaengiger Kern: Engine, Dateien, Projektmodell.
add_library(dawcore STATIC
    src/engine/Engine.cpp
    src/engine/LatencyProbe.cpp
    src/engine/OfflineRender.cpp
    src/engine/Synth.cpp
    src/disk/PeakData.cpp
    src/disk/RecordSink.cpp
    src/disk/TrackStream.cpp
    src/disk/WavFile.cpp
    src/model/ProjectController.cpp
    src/model/ProjectFile.cpp
)
target_include_directories(dawcore PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(dawcore PUBLIC Threads::Threads)

# Plattformunabhaengiger Teil des VST3-Hosts.
add_library(vst3host STATIC
    src/vst3/Vst3Host.cpp
    src/vst3/Vst3Iids.cpp
)
target_link_libraries(vst3host PUBLIC dawcore vst3interfaces)

# Offline-Tests, laufen ohne Audiogeraet und ohne echte Plugins.
add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE dawcore Threads::Threads)
add_executable(vst3_test tests/vst3_test.cpp)
target_link_libraries(vst3_test PRIVATE vst3host)

enable_testing()
add_test(NAME engine_test COMMAND engine_test)
add_test(NAME vst3_test COMMAND vst3_test)

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
        src/ui/MixerView.cpp
        src/ui/PianoRoll.cpp
        src/vst3/PluginManager.cpp
        src/vst3/PluginWindow.cpp
        src/vst3/Vst3Module.cpp
        src/minidaw.rc
        src/minidaw.manifest
    )
    target_compile_definitions(minidaw PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(minidaw PRIVATE dawcore vst3host ole32 avrt d2d1 dwrite windowscodecs shell32 user32 winmm comdlg32)
endif()
