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

## Audio-Clips und Streaming

Jede Spur hat einen `TrackStream` mit einem Ringpuffer (65536 Frames Stereo).

- Der UI-Thread übergibt die Clips einer Spur als fertige Liste (`Playlist`).
- Der Disk-Thread liest die Dateien, mischt die Clips der Spur und füllt den
  Ringpuffer fortlaufend entlang der Zeitachse. Zwischen Clips kommt Stille.
- Der Audio-Thread entnimmt nur fertige Frames. Clips und Dateien sieht er nie.

Sprung und Clip-Änderung laufen über denselben Weg: Der Disk-Thread setzt an
einer Position neu an und meldet, bis wohin der alte Pufferinhalt verfällt.
Fehlen Daten, liefert die Spur Stille und holt danach samplegenau wieder auf.
Falsches Material darf nie zu hören sein; das prüfen die Tests.

Noch offen (spätere Schritte): Samplerate-Umrechnung beim Import, Fades an
Clip-Grenzen, Überblendung bei Sprüngen während der Wiedergabe.

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
| 2 | Engine-Gerüst: Transport, Spuren-Array, Summierung, Kommando-Queue | fertig, auf Hardware gehört |
| 3 | WAV-Wiedergabe mit Streaming vom Disk-Thread | fertig, auf Hardware gehört |
| 4 | Minimale GUI: Spurliste, Timeline, Wellenformen, Transport, Fader | Code fertig, per Screenshot geprüft, Bedientest offen |
| 5 | Audio-Aufnahme mit Latenzkorrektur | offen |
| 6 | MIDI-Eingang, MIDI-Clips, Piano Roll | offen |
| 7 | VST3-Hosting: Laden, Verarbeiten, Editor-Fenster, State | offen |
| 8 | Mixer komplett: Inserts, Sends, Busse, Pegel, Latenzausgleich | offen |
| 9 | Projekt speichern und laden, Undo/Redo fertig | offen |
| 10 | Automation, Offline-Export | offen |

Jeder Schritt gilt erst als fertig, wenn er auf echter Hardware läuft und
ohne Knackser klingt.

## Oberfläche

- `ui/Canvas` ist eine dünne Zeichenschicht über Direct2D und DirectWrite.
- `ui/MainView` enthält die gesamte Oberfläche: Aufteilung, Zeichnen und
  Bedienlogik. Sie kennt kein Fenster und bekommt Eingaben als Methodenaufrufe.
  Alle Elemente zeichnet sie selbst, es gibt keine nativen Steuerelemente.
- `ui/MainWindow` ist das Win32-Fenster: Nachrichten, Zeitgeber (etwa 30
  Bilder pro Sekunde), Drag & Drop, Bildschirmskalierung.
- Die Oberfläche ändert das Projekt nur über den `ProjectController` und liest
  Position und Pegel aus der Engine.
- `minidaw --screenshot bild.png` zeichnet die Oberfläche mit Beispieldaten in
  eine Datei, ohne Fenster und Audiogerät. Der Build auf GitHub erzeugt das
  Bild bei jedem Push, damit sich Änderungen an der Oberfläche prüfen lassen.

Wellenformen kommen aus `disk/PeakData`: kleinster und größter Wert je 256
Frames, berechnet von einem Hintergrund-Thread.

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
