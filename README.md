# daw

Kleine DAW für Windows: 16 Spuren (Audio/MIDI), VST3, Mixer. Hobbyprojekt in
C++20 ohne Fremdbibliotheken.

Aufbau, Regeln und Fahrplan stehen in [ARCHITECTURE.md](ARCHITECTURE.md).

## Bauen

Voraussetzung: Visual Studio 2022 mit "Desktopentwicklung mit C++".

```
cmake -B build
cmake --build build --config Release
build\Release\minidaw.exe
```

Aktueller Stand: Schritt 1, Sinus-Test über WASAPI.
