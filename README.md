# MiniDAW

Kleine DAW für Windows: 16 Spuren (Audio/MIDI), VST3, Mixer. Hobbyprojekt in
C++20 ohne Fremdbibliotheken. Freie Software unter der GPLv3, kostenlos für alle.

Was es kann: Aufnahme mehrerer Spuren gleichzeitig (WASAPI oder ASIO) mit
gemessener Latenz, MIDI mit Noten-Editor und Metronom, VST3-Instrumente und
-Effekte, eingebaute Effekte (EQ, Kompressor, Hall, Echo, Gate, Chorus und
Mastering mit Limiter und Lautheitsmessung in LUFS),
Mischpult mit Bussen und Latenzausgleich, Automation von Lautstärke
und Panorama, Rückgängig/Wiederholen, Projekte speichern und als WAV exportieren,
MIDI-Dateien importieren und exportieren.

Aufbau, Regeln und Fahrplan stehen in [ARCHITECTURE.md](ARCHITECTURE.md).

## Lizenz und Entstehung

MiniDAW ist freie Software unter der **GNU General Public License, Version 3**
(GPLv3, Text in [LICENSE](LICENSE)). Kurz gesagt: Jeder darf MiniDAW kostenlos
benutzen, weitergeben und verändern. Wer eine veränderte Fassung weitergibt,
muss sie ebenfalls unter der GPLv3 und mit Quellcode weitergeben; so bleibt
MiniDAW und alles, was daraus entsteht, frei. Es gibt keine Gewähr, siehe
Abschnitte 15 und 16 der Lizenz.

Entstehung: Idee, Vorgaben und Tests am echten Gerät kommen von Feineflammen.
Der Programmcode wurde vollständig von einer KI geschrieben (Claude von
Anthropic), Programmsymbol und Titelbild von ChatGPT (OpenAI). Der Ablauf des
Mastering-Effekts geht auf einen Entwurf von ChatGPT zurück. MiniDAW ist ein
kostenloses Hobbyprojekt ohne Gewähr; vor wichtigen Aufnahmen
Sicherungskopien anlegen.

Enthaltene Teile anderer: das VST 3 SDK (nur die Schnittstellen, MIT-Lizenz)
und das ASIO SDK (nur die Header, unter der GPLv3) von Steinberg. Einzelheiten
in [FREMDLIZENZEN.md](FREMDLIZENZEN.md). VST und ASIO sind Marken der Steinberg
Media Technologies GmbH. MiniDAW ist kein Produkt von Steinberg.

Impressum: [feineflammen.de](https://feineflammen.de)

## Fertiges Programm

`bin/minidaw.exe` ist fertig gebaut, Windows 10/11, 64 Bit. Herunterladen und
starten, eine Installation ist nicht nötig; die exe bringt alles mit, was sie
braucht.

- Gebaut von GitHub Actions aus genau diesem Quellcode. Drei unabhängige
  Builds ergaben Byte für Byte dieselbe Datei.
- SHA-256: `E8A7CECAC58D8F7B09BA2756DEAEC911CE6C644F5D86FED651888E8EBB39D1AC`
  (prüfen in PowerShell mit `Get-FileHash minidaw.exe`)
- Browser und Windows warnen beim Herunterladen, weil die Datei nicht signiert
  und weitgehend unbekannt ist. Eine frühere Fassung wurde bei VirusTotal
  geprüft: keiner der Scanner meldete etwas.

Ändert sich der Quellcode, ist diese Datei veraltet, bis sie ersetzt wird.
Der Build auf GitHub warnt dann.

## Bauen

Voraussetzung: Visual Studio 2022 oder die kostenlosen "Build Tools für
Visual Studio 2022", jeweils mit "Desktopentwicklung mit C++".

Doppelklick auf `build.bat` (oder in einer Eingabeaufforderung aufrufen). Das
Skript sucht Visual Studio, baut, führt die Tests aus und nennt am Ende Pfad
und SHA-256-Prüfsumme von `build\Release\minidaw.exe`.

Von Hand geht es in der "Developer PowerShell" mit:

```
cmake -B build
cmake --build build --config Release
build\Release\minidaw.exe
```

Tests ausführen: `ctest --test-dir build -C Release --output-on-failure`

## Bedienung

| Aktion | Wirkung |
|---|---|
| WAV-Datei auf eine Spur ziehen | Clip anlegen; auf dem Spurkopf abgelegt beginnt er bei 0 |
| MIDI-Datei (.mid) auf eine Spur ziehen | MIDI-Clips anlegen, rastet auf ganze Takte ein; jede Spur der Datei bekommt eine eigene Spur |
| Leertaste | Play / Stop |
| Pos1 | zurück zum Anfang |
| Klick in Lineal oder leere Zeitachse | Abspielzeiger setzen |
| Clip ziehen | verschieben, auch auf andere Spuren; rastet an Kanten ein |
| Entf | ausgewählten Clip löschen |
| Mausrad / Strg + Mausrad | scrollen / zoomen |
| Doppelklick auf einen Regler | zurück auf 0 dB bzw. Mitte |
| R am Spurkopf | Spur für die Aufnahme scharfschalten; mehrere Spuren nehmen gleichzeitig auf |
| Rec-Knopf oder Taste R | Aufnahme starten / beenden |
| Mithören | Eingang über die scharfgeschalteten Spuren hören |
| − / + neben "Latenz" | Aufnahme-Latenz in 5-ms-Schritten anpassen; Mausrad 1 ms, Doppelklick setzt zurück |
| Tasten A S D F G H J K L | weiße Tasten ab C auf der scharfgeschalteten Spur |
| Tasten W E T Z U O | schwarze Tasten |
| Y / X | Oktave tiefer / höher |

Menü "Audio": Wiedergabe- und Aufnahmegerät (WASAPI) oder einen ASIO-Treiber direkt wählen, ohne Neustart. Die Wahl und die Aufnahme-Latenz werden je Gerät gespeichert. Mit ASIO laufen Ein- und Ausgang im selben Takt, die Latenz ist kleiner und alle Eingänge der Soundkarte sind da (beim UMC404HD vier). "ASIO-Einstellungen …" öffnet das Fenster des Treibers, dort stellt man die Puffergröße ein; die DAW startet den Treiber danach selbst neu. Ein ASIO-Treiber kann immer nur von einem Programm benutzt werden.

Rückgängig: Strg+Z, Wiederholen: Strg+Y (oder Strg+Umschalt+Z); das Menü "Bearbeiten" nennt den Schritt. Jede Mausbewegung ist ein Schritt, eine ganze Fader-Fahrt also einer. Bis zu 200 Schritte; ein neues oder geladenes Projekt beginnt ohne Vorgeschichte. Plugins (welches Plugin wo steckt, seine Einstellungen) nimmt Rückgängig nicht zurück.

Automation (Lautstärke und Panorama je Spur): Das Feld "Auto" unter R M S schaltet um zwischen aus, "Lautst." und "Pan". Dann zeigt die Spur ihre Kurve statt der Clips:

- Klick setzt einen Punkt, Ziehen verschiebt ihn (rastet am Raster ein, mit Umschalt frei), Rechtsklick löscht ihn. Rechtsklick auf das Feld "Auto" löscht die ganze gezeigte Kurve.
- Beim Abspielen schreibt der Regler: Zeigt die Spur "Lautst.", nimmt der Lautstärke-Regler (im Spurkopf oder im Mischpult) die Bewegung als Kurve auf. Dasselbe gilt für "Pan" und das Panorama.
- Hat eine Spur Punkte, folgt der Regler der Kurve (im Mischpult mit gelbem Griff). Ein gelbes "Auto" im Spurkopf heißt: Hier wirkt eine Kurve, die gerade nicht angezeigt wird.

Menü "Bearbeiten" (Clip vorher anklicken): Am Abspielzeiger teilen (Strg+T), mit ← / → um 1 ms verschieben, mit Umschalt um 10 ms. Clips lassen sich an ihren Kanten kürzen; beim Ziehen mit gedrückter Umschalttaste rasten sie nicht ein.

Menü "Audio" oder "Extras" → "Latenz messen …": Mit einem Kabel von Ausgang 1 oder 2 der Soundkarte in einen Eingang misst die DAW die tatsächliche Verzögerung und stellt die Aufnahme-Latenz darauf ein.

Menü "Datei": Neu (Strg+N), Öffnen (Strg+O), Speichern (Strg+S), Speichern unter (Strg+Umschalt+S), Als WAV exportieren (Strg+E), MIDI-Datei importieren (Strg+I), Als MIDI-Datei exportieren. Projekte haben die Endung `.mdaw` und sind lesbare Textdateien; die Audiodateien bleiben an ihrem Ort.

MIDI-Dateien (Standard-MIDI-Datei Typ 0 und 1, auch .kar und .rmi): Beim Import wird jede Spur der Datei ein eigener MIDI-Clip auf einer eigenen Spur; benutzt eine Spur der Datei mehrere Kanäle, wird sie nach Kanälen aufgeteilt. Tempowechsel der Datei sind eingerechnet, die Noten liegen also zeitlich richtig. Ist das Projekt noch leer, übernimmt es das Tempo der Datei. Der Import aus dem Menü landet auf der ersten leeren Spur ab der gewählten, am Anfang. Der Export schreibt alle MIDI-Clips als Typ-1-Datei mit dem Projekttempo, eine Spur je Projektspur; Kanal ist der MIDI-Ausgang der Spur, sonst Kanal 1 bis 16 nach Spurnummer (Kanal 10, bei General MIDI das Schlagzeug, wird übersprungen). Controller, Pitchbend und Programmwechsel werden weder gelesen noch geschrieben.

Am Spurkopf rechts stehen zwei Felder: oben der Eingang der Spur, unten ihr MIDI-Ausgang (Synth oder MIDI 1 bis 16; Linksklick schaltet weiter, Rechtsklick zurück, Mausrad). Ein Klick auf den Eingang öffnet eine Liste: Stereopaare ("In 1+2", "In 3+4"), einzelne Kanäle ("In 1" bis "In 4") und MIDI (alle oder Kanal 1 bis 16); das Mausrad schaltet direkt weiter. Ein gelber Eingang steht auf einem Kanal, den das Gerät nicht hat. Zum Aufnehmen mehrerer Quellen jede Spur auf ihren Eingang stellen, alle mit R scharfschalten und Rec drücken: Jede Spur bekommt ihre eigene Datei. In der Statusleiste wählt ein Klick auf "MIDI aus" das Ausgabegerät.

Links neben dem Lineal stehen Taktposition, Tempo (Klick +1, Rechtsklick −1, Mausrad) und der Knopf "Klick" für das Metronom.

Noten-Editor: Doppelklick auf einen MIDI-Clip öffnet ihn, Doppelklick auf eine leere Stelle einer MIDI-Spur legt einen neuen Clip von vier Takten an. Klick setzt eine Note, Ziehen verschiebt sie, Ziehen am rechten Rand ändert die Länge, Rechtsklick oder Entf löscht. Unten lässt sich der Anschlag je Note ziehen. Mausrad scrollt durch die Tonhöhen, Umschalt + Mausrad waagerecht, Strg + Mausrad zoomt. Esc oder × schließt.

Plugins: Das Feld "Plugin …" ganz rechts am Spurkopf öffnet eine Liste der VST3-Plugins aus `C:\Program Files\Common Files\VST3` für das Instrument der Spur; es ersetzt den eingebauten Synthesizer. "Fenster" öffnet die Oberfläche des Plugins. Effekte (VST3 oder eingebaut) kommen im Mischpult dazu.

Mischpult: F3, Menü "Ansicht" oder der Knopf "Mixer" links über den Spuren. Je Spur ein Kanalzug, dann Bus 1 bis 4 und der Master. Von oben nach unten:

- Instrument und FX 1 bis 4: Klick auf einen leeren Platz öffnet die Plugin-Liste, Klick auf einen belegten Platz sein Fenster, Rechtsklick tauscht oder entfernt das Plugin ("Kein Effekt").
- 1 bis 4: Sends zu den Bussen, hinter dem Fader. Doppelklick schaltet zwischen aus und 0 dB.
- "→ Master" / "→ Bus n": wohin die Spur geht (Klick weiter, Rechtsklick zurück, Mausrad).
- Panorama, Fader mit Pegelanzeige (Ziehen, mit Umschalt fein; Mausrad 1 dB; Doppelklick 0 dB), M und S.

Busse sind für Hall, Delay oder eine gemeinsame Gruppe gedacht: Effekt auf den Bus, Sends der Spuren aufdrehen. Die Verzögerung der Plugins gleicht die DAW automatisch aus; "Plugin-Latenz" oben zeigt, um wie viel.

Eingebaute Effekte: In der Effekt-Liste jedes FX-Platzes stehen oben EQ, Kompressor, Hall, Echo, Gate, Chorus und Mastering, markiert mit "eingebaut". Sie brauchen keine Plugins, öffnen ein eigenes Fenster und werden mit dem Projekt gespeichert. Überall gilt: Regler ziehen (mit Umschalt fein), Mausrad, Doppelklick setzt zurück.

- EQ: Hochpass, Tiefen (Kuhschwanz), zwei Mitten mit Güte, Höhen (Kuhschwanz), Tiefpass und Ausgangspegel. In der Kurve lassen sich die Griffe ziehen (waagerecht Frequenz, senkrecht Pegel), das Mausrad auf einem Griff ändert die Güte, Doppelklick setzt das Band auf 0 dB. Hoch- und Tiefpass schalten sich beim Anfassen ein.
- Kompressor: Schwelle, Verhältnis, Knie, Attack, Release, Aufholen (auf Wunsch automatisch) und Mischung für parallele Kompression. Das Fenster zeigt die Kennlinie, einen Punkt für den aktuellen Pegel und wie viel gerade abgesenkt wird.
- Hall: Größe, Nachhall (Zeit bis −60 dB), Vorverzögerung, Höhen bis (Dämpfung), Tiefen weg bis, Breite und Mischung. Am besten auf einen Bus legen, Mischung auf 100 % und die Spuren über die Sends dazugeben.
- Echo: im Takt (Notenwert vom 1/1 bis 1/16, auch punktiert und Triole, folgt dem Projekttempo) oder frei in Millisekunden, Rückkopplung, Ping-Pong zwischen links und rechts, Filter für die Wiederholungen (jede wird etwas dumpfer) und Mischung. Wechselt die Zeit, gleitet das Echo wie ein Bandecho.
- Gate: schaltet leise Stellen stumm, etwa das Rauschen des Mikrofons in Gesangspausen. Es öffnet über der Schwelle und schließt erst, wenn der Pegel um die Hysterese darunter fällt und die Haltezeit vorbei ist. "Öffnen" und "Schließen" sind die Zeiten für die ganze Blende, "Absenkung" wie viel leiser es geschlossen wird (80 dB = stumm), "Erkennung ab" lässt Rumpeln unter dieser Frequenz das Gate nicht öffnen. Das Fenster zeigt Pegel, Schwelle und ob das Gate offen ist.
- Chorus: zwei Stimmen je Seite mit langsam schwankender Verzögerung (Tempo, Tiefe, Verzögerung), rechts versetzt, so wird auch ein Mono-Signal breit. "Höhen bis" macht die Stimmen weicher; die Mischung hält die Lautstärke etwa gleich.

Mastering, gedacht für den Master: Das Fenster zeigt die Stufen in Signalreihenfolge, jede mit eigenem Schalter: Eingang, Korrektur (Tiefenfilter, Mulm bei 320 Hz), Kompressor (Pegel wird automatisch ausgeglichen), Sättigung (doppelt überabgetastet, ändert die Lautstärke leiser Stellen nicht), Klang (Bass 110 Hz, Höhen 8 kHz), Stereo (Breite, "Bass mono" unter 120 Hz), Clipper und Limiter. Der Limiter schaut 5 ms voraus, senkt vor einer Spitze gleitend ab und erkennt auch Spitzen zwischen den Samples (True Peak, 8-fach interpoliert). "Lautheit" ist die Verstärkung vor Clipper und Limiter, "Grenze" der höchste erlaubte True Peak.

Rechts im Fenster die Messung nach ITU-R BS.1770 / EBU R128: integrierte Lautheit (mit Gating, wie Streaming-Dienste messen), Kurzzeit (3 s), Momentan (0,4 s), höchster True Peak, Absenkung des Limiters und die integrierte Lautheit am Eingang zum Vergleich. Die Messung beginnt bei jedem Start der Wiedergabe neu, auch beim WAV-Export; nach einem Export steht dort also die Lautheit des ganzen Songs. Richtwert für Streaming: um −14 LUFS, True Peak höchstens −1 dBTP. Regler: ziehen (mit Umschalt fein), Mausrad, Doppelklick setzt zurück. "Umgehen" vergleicht mit dem unbearbeiteten Signal. Die Einstellungen werden im Projekt gespeichert; der Effekt verzögert um etwa 6 ms, was die DAW ausgleicht.

Menü "Hilfe" → "Über MiniDAW …": Version, Lizenz, Herkunft und Impressum.

Unten rechts zeigt eine LED jede eingehende MIDI-Nachricht, daneben steht im Klartext, was ankam (Kanal, Note, Anschlag). Rot bedeutet einen Übertragungsfehler.

Aufnahmen landen unter `Dokumente\MiniDAW\Aufnahmen`, eine Datei je Spur (`Aufnahme_<Datum>_<Uhrzeit>_Spur<n>.wav`, mono oder stereo je nach Eingang).
