# MiniDAW – Architektur

Dieses Dokument ist die verbindliche Grundlage für jeden Entwicklungsschritt.
Wer Code für das Projekt schreibt (Mensch oder KI), hält sich an diese Regeln.

## Ziel und Umfang

- Kleine DAW für Windows, Hobbyprojekt.
- 16 Spuren (Audio oder MIDI), 4 Busse, 1 Master. Die Anzahl ist fest.
- VST3-Instrumente und -Effekte, Mixer mit Inserts und Sends.
- C++20. Keine Fremdbibliotheken: nur C++-Standardbibliothek, Windows-SDK
  und die VST3-Schnittstellen von Steinberg (`third_party/vst3`, MIT-Lizenz;
  nur Schnittstellen-Header und zwei kleine Quelldateien mit Kennungen, keine
  Hilfsklassen des VST3 SDK).

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

### Kanalzüge und Plätze

- Kanalzüge (`strip`): 0..15 Spuren, 16..19 Busse, 20 Master. Jeder hat fünf
  Plätze (`slot`): 0 Instrument (nur Spuren), 1..4 Effekte.
- Ein Instrument auf Platz 0 ersetzt den eingebauten Synthesizer. Ein Effekt
  auf Platz 0 bearbeitet den Klang wie ein Insert (so landen alte Projekte mit
  einem Effekt je Spur). Ein Instrument auf einem Effektplatz bekommt das MIDI
  der Spur und überschreibt den Klang.
- Sends liegen hinter Fader und Panorama (post-fader). Mute einer Spur schaltet
  auch ihre Sends stumm. Busse sind solo-sicher: Solo einer Spur schaltet die
  Busse nicht ab, nur die anderen Spuren.

### Latenzausgleich

- Jedes Plugin meldet `latencyFrames()`. Die Engine rechnet bei jeder Änderung
  neu: größte Latenz der Spuren, größte der Busse, Summe der Master-Plätze.
- Jede Spur wird hinter ihren Plugins auf die größte Spurlatenz verzögert
  (`engine/DelayLine`), jeder Bus auf die größte Buslatenz; Spuren, die direkt
  auf den Master gehen, zusätzlich um die Buslatenz.
- Statt das Ergebnis zu verzögern, liest die Engine Clips, MIDI und die
  Position für die Plugins um die Gesamtlatenz voraus. Was man hört, liegt
  dadurch genau auf dem Abspielzeiger. Metronom und Aufnahme laufen auf der
  echten Position; live gespielte Noten werden nicht verzögert.
- Nach Stop rechnen die Spuren weiter, bis die Verzögerungen leer sind.

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
- Mit ASIO (`audio/AsioDevice`) gibt es keinen eigenen Eingangs-Thread: Der
  Treiber liefert Ein- und Ausgang im selben Rückruf, die Latenz ist fest.
  Die Treiberschnittstelle ist im Code selbst beschrieben (COM-Klasse aus
  `HKLM\SOFTWARE\ASIO`, Klassenkennung = Schnittstellenkennung); das ASIO-SDK
  wird nicht benutzt. Die Umrechnung der Sampleformate (`audio/AsioFormat.h`)
  ist ohne Windows testbar. Will der Treiber neu gestartet werden (Puffer im
  Treiberfenster geändert), merkt das die UI und öffnet ihn neu.
- `audio/AudioDevice` ist die gemeinsame Sicht auf WASAPI und ASIO.
- Jede Spur hat einen Audio-Eingang: einen Kanal des Geräts oder ein Paar.
  Die Engine bekommt alle Eingangskanäle (bis 64) und sucht sich je Spur die
  passenden heraus. Fehlt ein Kanal, wird Stille aufgenommen; der Controller
  meldet das vorher.
- Die Engine reicht den Eingang jeder aufnehmenden Spur während der Aufnahme
  an ihre eigene `RecordSink` weiter (Ringpuffer). Der Disk-Thread schreibt
  daraus je Spur eine WAV-Datei (32-Bit-Float, mono oder stereo). Der
  Audio-Thread fasst die Dateien nie an. Alle Spuren beginnen im selben Block.
- Ist die Datei fertig, legt der `ProjectController` den Clip an und schiebt
  ihn um die Gesamtlatenz (Eingang + Ausgang) nach vorn.
- Beliebig viele Spuren können scharf sein. Mithören führt den Eingang jeder
  scharfen Audio-Spur durch ihre Effekte, Fader und Panorama, auch bei
  stehendem Transport. Scharfe MIDI-Spuren bekommen das Live-MIDI ihres
  Kanals und nehmen es gemeinsam mit den Audio-Spuren auf.

Ausrichtung des Eingangs: Jedes Aufnahmepaket trägt einen Zeitstempel des
Geräts. Beim Start (und nach einer Störung) legt der Wiedergabe-Thread den
Lesezeiger so, dass ein Eingangs-Sample genau eine Ausgabeperiode plus zwei
Aufnahmeperioden alt ist, wenn es gelesen wird. Damit ist der Abstand zwischen
Ein- und Ausgang bei jedem Programmstart gleich. Ohne Zeitstempel wird wie
früher nach Füllstand ausgerichtet; dann schwankt die Latenz von Start zu
Start um bis zu eine Aufnahmeperiode.

Latenzmessung (`engine/LatencyProbe`, Menü Extras): Mit einem Kabel vom
Ausgang in einen Eingang gibt die Engine sechs kurze Sweeps aus und nimmt
gleichzeitig auf. Ausgabe und Aufnahme laufen über denselben Zähler wie bei
einer echten Aufnahme; die Verschiebung, unter der die Sweeps im Eingang
wieder auftauchen (Kreuzkorrelation, Median der Einzelmessungen), ist genau
die Latenz, um die Aufnahmen verschoben werden müssen. Übernommen wird sie
als Zusatz zur Schätzung des Treibers. Weichen die Einzelmessungen
voneinander ab, wird das gemeldet statt eine Zahl zu liefern.

Bekannte Grenzen: Ohne Messung ist die Latenz eine Schätzung aus den Angaben
des Treibers. Wandler und USB-Übertragung fehlen darin. Haben Ein- und Ausgang
getrennte Taktgeber (zum Beispiel USB-Mikrofon und Onboard-Ausgang), laufen
sie langsam auseinander; der Ringpuffer gleicht das alle paar Minuten mit
einem kurzen Sprung aus. Sauber löst das erst ASIO oder ein Gerät für beides.

## MIDI

- Live-MIDI kommt aus zwei Quellen: den MIDI-Geräten (`midi/MidiInput`, WinMM,
  Rückruf im Thread des Treibers) und der Rechnertastatur (UI-Thread). Beide
  rufen `Engine::postMidi`. Die Schreibseite der Queue teilen sie sich über
  einen Mutex; der Audio-Thread liest ohne Sperre.
- Die Nachrichten gehen an alle scharfgeschalteten MIDI-Spuren, deren Kanal
  passt, und wirken am Anfang des nächsten Blocks.
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

## VST3-Plugins

- Die Engine kennt nur `engine/AudioPlugin`: `process()` mit Stereo-Puffern,
  MIDI-Ereignissen des Blocks und Transport. Ein Instrument bekommt das MIDI
  der Spur anstelle des eingebauten Synthesizers; ein Effekt bearbeitet den
  Klang vor Fader und Panorama. Bis zu ein Instrument und vier Effekte je
  Spur, vier Effekte je Bus und am Master (siehe Signalfluss).
- Besitz: Die Engine bekommt nur einen Zeiger. Der `ProjectController` hält
  das Plugin; ein ersetztes Plugin lebt weiter, bis der Audio-Thread es über
  `popReleasedPlugin()` zurückmeldet. Zerstört wird im UI-Thread.
- `vst3/Vst3Host` ist der plattformunabhängige Host: Plugin anlegen (Component,
  Controller, Verbindung, Zustand abgleichen), Busse in Stereo einrichten,
  rechnen, abbauen. Dazu die Host-Objekte: Ereignisliste und
  Parameteränderungen fester Größe (kein Speicheranfordern im Audio-Thread),
  Speicherstrom, Nachrichten und Attributlisten.
- MIDI-Controller (Haltepedal, Pitchbend ...) kennt VST3 nur als Parameter. Die
  Zuordnung fragt der Host beim Laden über `IMidiMapping` ab und übersetzt im
  Audio-Thread.
- Bedienung im Plugin-Fenster kommt als `performEdit` im UI-Thread an und geht
  über eine lock-freie Queue in den Audio-Thread; Änderungen aus dem
  Audio-Thread gehen über eine zweite Queue zurück an die Oberfläche (`idle()`).
- `vst3/Vst3Module` (Windows) findet `.vst3`-Dateien und -Bündel unter
  `Gemeinsame Dateien\VST3` und lädt sie. Gesucht wird nach Dateinamen, ohne
  die Dateien zu laden. Geladene Dateien bleiben bis zum Programmende geladen.
- `vst3/PluginWindow` (Windows) ist das Fenster für die Plugin-Oberfläche,
  `vst3/PluginManager` verbindet Oberfläche, Projekt und Host.
- `tests/vst3_test.cpp` baut ein kleines VST3-Plugin direkt im Test und prüft
  daran Aufrufreihenfolge, Referenzzählung, MIDI-Übersetzung und Parameterwege.

Offen: Plugins laufen im selben Prozess, ein abstürzendes Plugin reißt die DAW
mit. Ändert ein Plugin seine Latenz während des Betriebs, wird das erst beim
nächsten Laden eines Plugins berücksichtigt.

## Clips bearbeiten

- Teilen, Kürzen und Verschieben ändern nur Position, Ausschnitt (`offset`)
  und Länge. Die Audiodatei bleibt unverändert; wer zu viel abschneidet, zieht
  die Kante wieder zurück.
- An Schnittkanten (mitten im Material, nicht am Anfang oder Ende der Datei)
  blendet `TrackStream` über 128 Frames ein bzw. aus, damit es nicht knackt.
- MIDI-Clips werden beim Teilen nach Notenanfang aufgeteilt; Noten über den
  Schnitt hinaus enden am Schnitt.

## Audiogeräte

`WasapiDevice::endpoints()` listet die angeschlossenen Geräte, `open()` öffnet
sie über ihre Windows-Kennung. Beim Wechsel im Menü "Audio" werden
Audiogerät und Disk-Thread angehalten, das Gerät neu geöffnet und bei
anderer Samplerate oder größerem Puffer alle Plugins mit ihren Einstellungen
neu geladen. Gewählte Geräte und der Latenz-Zusatz je Gerätekombination
stehen in den Einstellungen. Fehlt das gespeicherte Gerät beim Start,
werden die Windows-Standardgeräte benutzt.

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
  audio/    Audiogeräte (WASAPI, ASIO), Callback-Schnittstelle
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
| 7 | VST3-Hosting: Laden, Verarbeiten, Editor-Fenster (ein Plugin je Spur) | fertig, mit echtem Plugin gespielt |
| 8 | Mixer komplett: Inserts, Sends, Busse, Pegel, Latenzausgleich | Code und Offline-Tests fertig (Latenzausgleich samplegenau geprüft), per Screenshot geprüft, Bedientest offen |
| 9 | Projekt speichern und laden, WAV-Export | Code und Offline-Tests fertig, Bedientest offen; Undo/Redo offen |
| 10 | Automation (Lautstärke, Panorama je Spur), Rückgängig/Wiederholen | Code und Offline-Tests fertig, per Screenshot geprüft, Bedientest offen |
| 11 | ASIO, Aufnahme mehrerer Spuren mit eigenem Eingang | Code und Offline-Tests fertig, per Screenshot geprüft, mit ASIO-Treiber NICHT getestet |

Jeder Schritt gilt erst als fertig, wenn er auf echter Hardware läuft und
ohne Knackser klingt.

## Oberfläche

- `ui/Canvas` ist eine dünne Zeichenschicht über Direct2D und DirectWrite.
- `ui/MainView` enthält die gesamte Oberfläche: Aufteilung, Zeichnen und
  Bedienlogik. Sie kennt kein Fenster und bekommt Eingaben als Methodenaufrufe.
  Alle Elemente zeichnet sie selbst, es gibt keine nativen Steuerelemente.
- `ui/MixerView` ist das Mischpult (F3 oder Knopf "Mixer" links über den
  Spuren). Es ersetzt dann Spurköpfe, Zeitachse und Noten-Editor. Plugin-Wünsche
  gibt es per Rückruf an `MainView`, die die Plugin-Auswahl öffnet.
- `ui/Theme.h` enthält Farben, die alle Ansichten teilen; die Fader-Kennlinie
  steht in `engine/FaderCurve.h`, weil die Automation sie auch braucht.
- Jede Mausgeste (Drücken bis Loslassen) ist ein Rückgängig-Schritt:
  `MainView` ruft dafür `beginEdit()` und `endEdit()` am Controller.

## Rückgängig und Automation

- Rückgängig arbeitet mit Schnappschüssen: Vor jeder Änderung merkt sich der
  `ProjectController` das ganze `Project` (Clips teilen sich die Audiodateien
  über `shared_ptr`, das kostet kaum Speicher). Zurück heißt: Schnappschuss
  übernehmen und nur das an die Engine schicken, was sich unterscheidet.
  Plugins und ihre Einstellungen gehören nicht zum Schnappschuss.
- Mausrad-Schritte am selben Regler innerhalb einer Sekunde werden ein Schritt.
- Automation: Punkte je Spur und Parameter im Modell (Lautstärke als
  Fader-Stellung, Panorama -1..1). Die Engine bekommt daraus eine fertige
  `AutomationCurve` (wie die MIDI-Sequenzen: einmal gebaut, über die
  Befehlsschlange übergeben, alte Fassung zurück an den UI-Thread).
- Die Kurve wird je Block zu der Zeit gelesen, zu der das Signal den Fader
  erreicht: Position + Bus- + Master-Latenz. Damit hört man jeden Punkt dort,
  wo er auf der Zeitachse steht, auch mit verzögernden Plugins.
- Schreiben: Zeigt die Spur die Kurve und läuft die Wiedergabe, ersetzt eine
  Regler-Bewegung die Punkte im überstrichenen Bereich. Vor dem ersten
  geschriebenen Punkt bleibt der bisherige feste Wert erhalten.
- `ui/MainWindow` ist das Win32-Fenster: Nachrichten, Zeitgeber (etwa 30
  Bilder pro Sekunde), Drag & Drop, Bildschirmskalierung.
- Die Oberfläche ändert das Projekt nur über den `ProjectController` und liest
  Position und Pegel aus der Engine.
- `minidaw --screenshot bild.png` zeichnet die Oberfläche mit Beispieldaten in
  eine Datei, ohne Fenster und Audiogerät; mit `mixer` als drittem Argument
  das Mischpult, mit `eingang` die Eingangsliste. Der Build auf GitHub erzeugt das
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

## Projektdatei

- Endung `.mdaw`, zeilenweiser Text in UTF-8 (`model/ProjectFile`), bewusst
  lesbar und von Hand reparierbar. Unbekannte Schlüsselwörter werden beim
  Lesen übersprungen, damit ältere Programmversionen neuere Dateien nicht
  zerstören.
- Gespeichert werden Tempo, Master, je Spur Name, Regler, Eingang, Ausgang,
  Sends, Ziel (Master oder Bus), Plugins je Platz (Pfad und Einstellungen als
  Hex-Text), Audio-Clips (Pfad, Position, Ausschnitt) und MIDI-Clips mit Noten;
  dazu je Bus Regler und Plugins und die Plugins des Masters, je Spur die
  Automation (`automation lautstaerke Frame:Wert ...`). Dateiversion 2;
  Dateien der Version 1 (ein Plugin je Spur) werden weiter gelesen. Audiodateien bleiben, wo sie sind:
  Pfade unterhalb des Projektordners stehen relativ in der Datei, andere
  vollständig.
- Geschrieben wird erst in eine Nachbardatei, dann umbenannt. Scheitert das
  Schreiben, bleibt die alte Datei unversehrt.
- Geladen wird erst vollständig in den Speicher; nur wenn das gelingt, wird das
  laufende Projekt ersetzt. Fehlende Audiodateien und nicht ladbare Plugins
  brechen das Laden nicht ab, sondern werden gemeldet.
- Plugins sichert und lädt der VST3-Teil über `PluginPersistence`; das Modell
  kennt keine Plugin-Technik.
- `ProjectController::modified()` merkt sich Änderungen am Modell. Drehen an
  Reglern in Plugin-Fenstern wird dabei nicht erkannt.

Positionen stehen in Frames. Ein Projekt passt deshalb nur zu der Samplerate,
mit der es gespeichert wurde; beim Laden mit anderer Samplerate wird gewarnt.

## Export

`engine/OfflineRender` rechnet das Projekt ohne Audiogerät in eine WAV-Datei
(16 Bit, 24 Bit oder 32-Bit-Float). Dafür hält das Programm Audiogerät und
Disk-Thread an und treibt die Engine aus dem UI-Thread an; danach startet
beides wieder. Im Offline-Modus blendet die Engine Start und Stopp nicht ein
und aus, der Anfang der Datei bleibt also unverändert. MIDI an externe Geräte
wird während des Exports verworfen: Was ein Hardware-Klangerzeuger spielt,
kann nicht im Export landen.

## Offline-Rendering

Die Engine hängt nicht am Audiogerät. Sie bekommt nur „rechne N Samples“.
Dadurch lässt sie sich ohne Hardware in eine WAV-Datei rendern. Das dient
später dem Export und schon vorher automatischen Tests (Latenzausgleich,
Automation, Clip-Grenzen).
