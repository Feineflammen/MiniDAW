# VST3-Schnittstellen

Unveränderte Kopie von https://github.com/steinbergmedia/vst3_pluginterfaces
(Stand: Commit 4f547e8e102b47de4a8b8aaf343c73b700786372 vom 11.08.2026), ohne den Ordner `test`.

Das sind die offiziellen Schnittstellen-Definitionen von Steinberg, über die
ein Host mit VST3-Plugins spricht. Lizenz: MIT, siehe `pluginterfaces/LICENSE.txt`.

Übersetzt werden daraus nur `base/funknown.cpp` und `base/coreiids.cpp`.
Der gesamte Host-Code liegt in `src/vst3` und ist selbst geschrieben; die
Hilfsklassen des VST3 SDK (`public.sdk`) werden nicht benutzt.

VST ist eine Marke der Steinberg Media Technologies GmbH.
