# MiniDAW – Architektur

Dieses Dokument ist die verbindliche Grundlage für jeden Entwicklungsschritt.
Wer Code für das Projekt schreibt (Mensch oder KI), hält sich an diese Regeln.

## Ziel und Umfang

- Kleine DAW für Windows, Hobbyprojekt.
- 16 Spuren (Audio oder MIDI), 4 Busse, 1 Master. Die Anzahl ist fest.
- VST3-Instrumente und -Effekte, Mixer mit Inserts und Sends.
- C++20. Keine Fremdbibliotheken: nur C++-Standardbibliothek, Windows-SDK
  und die VST3-Interface-Header.

Bewusst nicht enthalten: Timestretching, Comping, Notation, Video, Surround,
Tempoänderungen im Projekt, MP3/FLAC, eigene Effekte.

## Threads

| Thread | Aufgabe | Darf blockieren? |
|---|---|---|
| Audio | Engine rechnet einen Block und liefert ihn an WASAPI | nein, niemals |
| UI | Fenster, Eingaben, Projektmodell, Undo | ja |
| Disk | Audiodateien vorlesen, Aufnahmen schreiben, Peak-Dateien | ja |

Kommunikation zwischen den Threads läuft ausschließlich über lock-freie
Ringpuffer mit genau einem Schreiber und einem Leser (SPSC):

- UI → Audio: Kommandos (Fader bewegt, Play, Clip-Liste ersetzt)
- Audio → UI: Pegel, Abspielposition, Aussetzer-Zähler
- Disk → Audio: vorgelesene Audiodaten
- Audio → Disk: aufgenommene Audiodaten

## Regeln für den Audio-Thread

Im Audio-Callback und in allem, was er aufruft, ist verboten:

1. Speicher anfordern oder freigeben (`new`, `delete`, `malloc`, wachsende
   `std::vector`, `std::string`, `std::function` mit Allokation).
2. Locks jeder Art (`std::mutex`, Critical Sections).
3. Datei-, Netzwerk- oder Konsolenzugriffe, auch kein `printf`.
4. Systemaufrufe, die warten können.
5. Exceptions werfen.

Alles, was der Audio-Thread braucht, wird vorher angelegt (`prepare`) oder vom
UI-Thread fertig gebaut und als Zeiger übergeben. Alte Objekte gibt der
Audio-Thread über eine Queue an den UI-Thread zurück, der sie freigibt.

## Signalformat

- Intern überall 32-Bit-Float, nicht verschachtelt: ein Puffer pro Kanal
  (`float* const* buffers`). Das ist auch das Format von VST3.
- Verschachteln (interleaving) und Formatwandlung passieren nur an der Grenze
  zum Audiogerät und beim Dateizugriff.
- Positionen auf der Zeitachse sind 64-Bit-Ganzzahlen in Samples.
  Musikalische Positionen (Takte, Schläge) werden daraus berechnet.

## Signalfluss

Die Struktur ist fest, es gibt keinen dynamischen Graphen:

```
Spur 1..16:  Quelle (Audio-Clips | MIDI → Instrument)
             → Inserts → Fader/Pan → Ausgang (Bus oder Master)
             → Sends (nach Fader) an Busse
Bus 1..4:    Summe der Eingänge → Inserts → Fader/Pan → Master
Master:      Summe → Inserts → Fader → Audiogerät
```

Die Reihenfolge der Berechnung ist immer Spuren, dann Busse, dann Master.
Busse senden nicht an andere Busse. Dadurch sind Rückkopplungen ausgeschlossen
und der Latenzausgleich bleibt einfach: Jeder Pfad wird auf die Latenz des
längsten Pfads verzögert.

## Modell und Engine

- Das **Projektmodell** (Spuren, Clips, Noten, Plugin-Zuordnung, Mixerwerte)
  gehört dem UI-Thread. Nur er ändert es.
- Die **Engine** hält eine eigene, für die Wiedergabe aufbereitete Kopie.
  Nach jeder Modelländerung baut der UI-Thread die betroffenen Teile neu und
  schickt sie per Kommando.
- **Undo/Redo** arbeitet nur auf dem Modell. Jede Änderung ist ein
  Kommando-Objekt mit `apply()` und `revert()`. Es gibt keinen anderen Weg,
  das Modell zu ändern.

## Verzeichnisse

```
src/
  audio/    Audiogerät (WASAPI), Callback-Schnittstelle
  engine/   Transport, Spuren, Mixer, Latenzausgleich
  midi/     MIDI-Geräte, MIDI-Daten
  vst3/     Plugin-Scan, Hosting, Editor-Fenster
  model/    Projektmodell, Undo, Speichern und Laden
  disk/     WAV lesen und schreiben, Streaming, Peak-Dateien
  ui/       Fenster, Zeichnen, Widgets
  util/     Ringpuffer, Hilfsfunktionen
tests/      Offline-Tests der Engine
```

`engine/`, `model/`, `midi/` (Daten) und `util/` enthalten keinen
Windows-Code. Sie bilden die Bibliothek `dawcore`, die auch die Tests benutzen.

## Fahrplan

| Schritt | Inhalt | Stand |
|---|---|---|
| 1 | Audio-Callback über WASAPI, Sinus-Test | fertig, auf Hardware gehört |
| 2 | Engine-Gerüst: Transport, Spuren-Array, Summierung, Kommando-Queue | Code und Offline-Tests fertig, Hörtest offen |
| 3 | WAV-Wiedergabe mit Streaming vom Disk-Thread | offen |
| 4 | Minimale GUI: Spurliste, Timeline, Wellenformen, Transport, Fader | offen |
| 5 | Audio-Aufnahme mit Latenzkorrektur | offen |
| 6 | MIDI-Eingang, MIDI-Clips, Piano Roll | offen |
| 7 | VST3-Hosting: Laden, Verarbeiten, Editor-Fenster, State | offen |
| 8 | Mixer komplett: Inserts, Sends, Busse, Pegel, Latenzausgleich | offen |
| 9 | Projekt speichern und laden, Undo/Redo fertig | offen |
| 10 | Automation, Offline-Export | offen |

Jeder Schritt gilt erst als fertig, wenn er auf echter Hardware läuft und
ohne Knackser klingt.

## Tests

`tests/engine_test.cpp` prüft die Engine offline, ohne Testbibliothek. Die
Tests laufen bei jedem Push auf GitHub (`ctest`). Jede neue Engine-Funktion
bekommt dort mindestens eine Prüfung.

Wertänderungen an Fadern, Mute, Solo und Transport werden über einen Block
überblendet, damit nichts knackt. Ein Test misst den größten Sprung zwischen
zwei Samples.

## Offline-Rendering

Die Engine hängt nicht am Audiogerät. Sie bekommt nur „rechne N Samples“.
Dadurch lässt sie sich ohne Hardware in eine WAV-Datei rendern. Das dient
später dem Export und schon vorher automatischen Tests (Latenzausgleich,
Automation, Clip-Grenzen).
