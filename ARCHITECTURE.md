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

## Aufnahme

- Das Audiogerät öffnet neben der Ausgabe das Standard-Aufnahmegerät. Ein
  eigener Thread liest dessen Pakete in einen Ringpuffer; der Audio-Thread der
  Ausgabe gibt den Takt vor und holt sich pro Block die passende Menge.
  Windows rechnet den Eingang auf die Samplerate der Ausgabe um.
- Die Engine reicht den Eingang während der Aufnahme an eine `RecordSink`
  weiter (Ringpuffer). Der Disk-Thread schreibt daraus eine WAV-Datei
  (32-Bit-Float). Der Audio-Thread fasst die Datei nie an.
- Ist die Datei fertig, legt der `ProjectController` den Clip an und schiebt
  ihn um die Gesamtlatenz (Eingang + Ausgang) nach vorn.
- Immer nur eine Spur ist scharfgeschaltet. Mithören führt den Eingang durch
  Fader und Panorama dieser Spur, auch bei stehendem Transport.

Bekannte Grenzen: Die Latenz ist eine Schätzung aus den Angaben des Treibers
und dem Füllstand des Ringpuffers, kein Messwert. Haben Ein- und Ausgang
getrennte Taktgeber (zum Beispiel USB-Mikrofon und Onboard-Ausgang), laufen
sie langsam auseinander; der Ringpuffer gleicht das alle paar Minuten mit
einem kurzen Sprung aus. Sauber löst das erst ASIO oder ein Gerät für beides.

## MIDI

- Live-MIDI kommt aus zwei Quellen: den MIDI-Geräten (`midi/MidiInput`, WinMM,
  Rückruf im Thread des Treibers) und der Rechnertastatur (UI-Thread). Beide
  rufen `Engine::postMidi`. Die Schreibseite der Queue teilen sie sich über
  einen Mutex; der Audio-Thread liest ohne Sperre.
- Die Nachrichten gehen an die scharfgeschaltete Spur und wirken am Anfang des
  nächsten Blocks.
- Jede Spur hat einen eingebauten Synthesizer (`engine/Synth`: Sägezahn,
  Tiefpass, Hüllkurve, 16 Stimmen). Er ist die Klangquelle für MIDI, bis in
  Schritt 7 VST-Instrumente dazukommen, und bleibt danach der Ersatz für
  Spuren ohne Instrument.
- Der Synthesizer läuft durch Fader, Panorama und Pegelanzeige der Spur und
  klingt auch bei stehendem Transport.

### Eingang und Ausgang je Spur

- **Eingang:** Audio, MIDI alle Kanäle oder ein fester Kanal 1 bis 16. Er
  bestimmt, was die Spur aufnimmt, und filtert das Live-MIDI. Die
  Rechnertastatur umgeht den Kanalfilter.
- **MIDI-Ausgang:** eingebauter Synthesizer oder Kanal 1 bis 16 am
  MIDI-Ausgabegerät. Der Kanal wird beim Senden umgeschrieben; gespeichert
  sind Noten ohne Kanal.

### MIDI-Clips

- Noten liegen im Modell in Frames relativ zum Clip-Anfang (wie alle
  Positionen), nicht in Takten. Das Tempo dient nur dem Raster.
- Der `ProjectController` baut aus allen MIDI-Clips einer Spur eine sortierte
  Ereignisliste (`MidiSequence`) und übergibt sie der Engine als Zeiger. Die
  abgelöste Liste gibt der Audio-Thread über eine Queue an den UI-Thread
  zurück, der sie freigibt (`collectGarbage`).
- Die Engine spielt die Ereignisse samplegenau: Der Synthesizer wird zwischen
  den Ereignissen stückweise gerechnet. Bei Stopp, Sprung oder neuen Daten
  bekommen klingende Noten ein "Note aus".

### Tempo, Raster, Noten-Editor

- Das Projekt hat ein festes Tempo (BPM) im 4/4-Takt. Es bestimmt Lineal,
  Raster und Metronom, verschiebt aber keine vorhandenen Noten oder Clips.
- Das Metronom rechnet die Engine: ein kurzer Klick genau auf jedem Schlag.
- `ui/PianoRoll` ist der Noten-Editor. Während des Ziehens arbeitet er auf
  einer Kopie der Noten und übergibt sie beim Loslassen mit
  `ProjectController::updateMidiClip`. Das Raster richtet sich an der
  Zeitachse aus, nicht am Clip-Anfang.

### MIDI-Aufnahme

Der Audio-Thread meldet Live-Noten mit ihrer Zeitachsen-Position über eine
Queue an den UI-Thread. Die Position berücksichtigt, wann die Note eintraf
(Zeitstempel beim Eingang) und die Ausgangslatenz. Der `ProjectController`
setzt daraus Noten zusammen und legt am Ende den Clip an.

### MIDI-Ausgabe

`midi/MidiOutput` hat einen eigenen Thread. Die Engine versieht jede
Nachricht mit dem Zeitpunkt, zu dem das zugehörige Audio hörbar wird
(Blockzeit + Position im Block + Ausgangslatenz); der Thread sendet sie dann,
auf etwa 1 ms genau. Der Audio-Thread ruft nie in den MIDI-Treiber.

Einstellungen, die zum Rechner gehören (Latenz-Zusatz, MIDI-Ausgabegerät),
liegen in `%APPDATA%\MiniDAW\einstellungen.txt`.

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
| 4 | Minimale GUI: Spurliste, Timeline, Wellenformen, Transport, Fader | fertig, auf Hardware bedient |
| 5 | Audio-Aufnahme mit Latenzkorrektur | fertig, mit echtem Eingang getestet; Latenz-Zusatz einstellbar |
| 6a | MIDI-Eingang, eingebauter Synthesizer, live spielen | fertig, mit Keyboard gespielt |
| 6b | MIDI-Clips, MIDI-Aufnahme, Kanalwahl je Spur, MIDI-Ausgabegerät | Code und Offline-Tests fertig, vom Benutzer NICHT getestet (übersprungen) |
| 6c | Piano Roll, Tempo, Taktraster, Metronom | Code fertig, per Screenshot geprüft, Bedientest offen |
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
