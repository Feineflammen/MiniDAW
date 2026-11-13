# daw

Kleine DAW für Windows: 16 Spuren (Audio/MIDI), VST3, Mixer. Hobbyprojekt in
C++20 ohne Fremdbibliotheken.

Aufbau, Regeln und Fahrplan stehen in [ARCHITECTURE.md](ARCHITECTURE.md).

## Fertiges Programm

`bin/minidaw.exe` ist fertig gebaut, Windows 10/11, 64 Bit. Herunterladen und
starten, eine Installation ist nicht nötig. Gebraucht wird die
Visual-C++-Laufzeit von Microsoft, die auf den meisten Rechnern schon vorhanden ist.

- Gebaut von GitHub Actions aus genau diesem Quellcode. Drei unabhängige
  Builds ergaben Byte für Byte dieselbe Datei.
- SHA-256: `BB3247DC19E9FFDDE59224DEE885E7B8DA71C0C01477C3C766EABD111EEE33A3`
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

Aktueller Stand: Mischpult mit Effekten, Bussen und Latenzausgleich; Projekte speichern, laden und als WAV exportieren.

## Bedienung

| Aktion | Wirkung |
|---|---|
| WAV-Datei auf eine Spur ziehen | Clip anlegen; auf dem Spurkopf abgelegt beginnt er bei 0 |
| Leertaste | Play / Stop |
| Pos1 | zurück zum Anfang |
| Klick in Lineal oder leere Zeitachse | Abspielzeiger setzen |
| Clip ziehen | verschieben, auch auf andere Spuren; rastet an Kanten ein |
| Entf | ausgewählten Clip löschen |
| Mausrad / Strg + Mausrad | scrollen / zoomen |
| Doppelklick auf einen Regler | zurück auf 0 dB bzw. Mitte |
| R am Spurkopf | Spur für die Aufnahme scharfschalten |
| Rec-Knopf oder Taste R | Aufnahme starten / beenden |
| Mithören | Eingang über die scharfgeschaltete Spur hören |

| − / + neben "Latenz" | Aufnahme-Latenz in 5-ms-Schritten anpassen; Mausrad 1 ms, Doppelklick setzt zurück |
| Tasten A S D F G H J K L | weiße Tasten ab C auf der scharfgeschalteten Spur |
| Tasten W E T Z U O | schwarze Tasten |
| Y / X | Oktave tiefer / höher |

Menü "Audio": Wiedergabe- und Aufnahmegerät direkt wählen, ohne Neustart. Die Wahl und die Aufnahme-Latenz werden je Gerät gespeichert.

Menü "Bearbeiten" (Clip vorher anklicken): Am Abspielzeiger teilen (Strg+T), mit ← / → um 1 ms verschieben, mit Umschalt um 10 ms. Clips lassen sich an ihren Kanten kürzen; beim Ziehen mit gedrückter Umschalttaste rasten sie nicht ein.

Menü "Extras" → "Latenz messen …": Mit einem Kabel von Ausgang 1 oder 2 der Soundkarte in einen Eingang misst die DAW die tatsächliche Verzögerung und stellt die Aufnahme-Latenz darauf ein.

Menü "Datei": Neu (Strg+N), Öffnen (Strg+O), Speichern (Strg+S), Speichern unter (Strg+Umschalt+S), Als WAV exportieren (Strg+E). Projekte haben die Endung `.mdaw` und sind lesbare Textdateien; die Audiodateien bleiben an ihrem Ort.

Am Spurkopf rechts stehen zwei Felder: oben der Eingang der Spur (Audio, MIDI alle, MIDI 1 bis 16), unten ihr MIDI-Ausgang (Synth oder MIDI 1 bis 16). Linksklick schaltet weiter, Rechtsklick zurück, das Mausrad geht auch. In der Statusleiste wählt ein Klick auf "MIDI aus" das Ausgabegerät.

Links neben dem Lineal stehen Taktposition, Tempo (Klick +1, Rechtsklick −1, Mausrad) und der Knopf "Klick" für das Metronom.

Noten-Editor: Doppelklick auf einen MIDI-Clip öffnet ihn, Doppelklick auf eine leere Stelle einer MIDI-Spur legt einen neuen Clip von vier Takten an. Klick setzt eine Note, Ziehen verschiebt sie, Ziehen am rechten Rand ändert die Länge, Rechtsklick oder Entf löscht. Unten lässt sich der Anschlag je Note ziehen. Mausrad scrollt durch die Tonhöhen, Umschalt + Mausrad waagerecht, Strg + Mausrad zoomt. Esc oder × schließt.

Plugins: Das Feld "Plugin …" ganz rechts am Spurkopf öffnet eine Liste der VST3-Plugins aus `C:\Program Files\Common Files\VST3` für das Instrument der Spur; es ersetzt den eingebauten Synthesizer. "Fenster" öffnet die Oberfläche des Plugins. Effekte kommen im Mischpult dazu.

Mischpult: F3, Menü "Ansicht" oder der Knopf "Mixer" links über den Spuren. Je Spur ein Kanalzug, dann Bus 1 bis 4 und der Master. Von oben nach unten:

- Instrument und FX 1 bis 4: Klick auf einen leeren Platz öffnet die Plugin-Liste, Klick auf einen belegten Platz sein Fenster, Rechtsklick tauscht oder entfernt das Plugin ("Kein Effekt").
- 1 bis 4: Sends zu den Bussen, hinter dem Fader. Doppelklick schaltet zwischen aus und 0 dB.
- "→ Master" / "→ Bus n": wohin die Spur geht (Klick weiter, Rechtsklick zurück, Mausrad).
- Panorama, Fader mit Pegelanzeige (Ziehen, mit Umschalt fein; Mausrad 1 dB; Doppelklick 0 dB), M und S.

Busse sind für Hall, Delay oder eine gemeinsame Gruppe gedacht: Effekt auf den Bus, Sends der Spuren aufdrehen. Die Verzögerung der Plugins gleicht die DAW automatisch aus; "Plugin-Latenz" oben zeigt, um wie viel.

Unten rechts zeigt eine LED jede eingehende MIDI-Nachricht, daneben steht im Klartext, was ankam (Kanal, Note, Anschlag). Rot bedeutet einen Übertragungsfehler.

Aufnahmen landen unter `Dokumente\MiniDAW\Aufnahmen`.
