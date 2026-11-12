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

Tests ausführen: `ctest --test-dir build -C Release --output-on-failure`

Aktueller Stand: Projekte speichern, laden und als WAV exportieren.

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

Menü "Datei": Neu (Strg+N), Öffnen (Strg+O), Speichern (Strg+S), Speichern unter (Strg+Umschalt+S), Als WAV exportieren (Strg+E). Projekte haben die Endung `.mdaw` und sind lesbare Textdateien; die Audiodateien bleiben an ihrem Ort.

Am Spurkopf rechts stehen zwei Felder: oben der Eingang der Spur (Audio, MIDI alle, MIDI 1 bis 16), unten ihr MIDI-Ausgang (Synth oder MIDI 1 bis 16). Linksklick schaltet weiter, Rechtsklick zurück, das Mausrad geht auch. In der Statusleiste wählt ein Klick auf "MIDI aus" das Ausgabegerät.

Links neben dem Lineal stehen Taktposition, Tempo (Klick +1, Rechtsklick −1, Mausrad) und der Knopf "Klick" für das Metronom.

Noten-Editor: Doppelklick auf einen MIDI-Clip öffnet ihn, Doppelklick auf eine leere Stelle einer MIDI-Spur legt einen neuen Clip von vier Takten an. Klick setzt eine Note, Ziehen verschiebt sie, Ziehen am rechten Rand ändert die Länge, Rechtsklick oder Entf löscht. Unten lässt sich der Anschlag je Note ziehen. Mausrad scrollt durch die Tonhöhen, Umschalt + Mausrad waagerecht, Strg + Mausrad zoomt. Esc oder × schließt.

Plugins: Das Feld "Plugin …" ganz rechts am Spurkopf öffnet eine Liste der VST3-Plugins aus `C:\Program Files\Common Files\VST3`. Ein Instrument ersetzt den eingebauten Synthesizer der Spur, ein Effekt bearbeitet ihren Klang. "Fenster" öffnet die Oberfläche des Plugins.

Unten rechts zeigt eine LED jede eingehende MIDI-Nachricht, daneben steht im Klartext, was ankam (Kanal, Note, Anschlag). Rot bedeutet einen Übertragungsfehler.

Aufnahmen landen unter `Dokumente\MiniDAW\Aufnahmen`.
