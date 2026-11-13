# Herkunft

Auszug aus dem ASIO SDK 2.3.4 von Steinberg Media Technologies GmbH:

- Quelle: https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
  (SHA-256 des Archivs: `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`)
- Übernommen, unverändert: `common/asio.h`, `common/asiosys.h`, `common/iasiodrv.h`,
  `LICENSE.txt`, `README.md`, `changes.txt`
- MiniDAW benutzt das SDK unter der **GPLv3** (siehe `LICENSE.txt`, Variante b,
  und `LICENSE` im Hauptordner). Nur die Header werden gebraucht: Sie beschreiben
  die Schnittstelle der ASIO-Treiber. Laden der Treiber, Puffer und
  Formatwandlung stehen in `src/audio/AsioDevice.cpp`.

ASIO is a trademark and software of Steinberg Media Technologies GmbH.
