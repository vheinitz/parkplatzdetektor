# ESP32-CAM → ESP-NOW → USB → Browser

Bildübertragung ohne Router, Repeater, DHCP oder ARP. Entstanden, weil die
Kamera über das WLAN `ABYRVALG` nicht nutzbar erreichbar war: Sie assoziiert
sich am stärksten Zugangspunkt, und das ist ein Repeater
(`08:B6:57:A5:47:57`), der den ESP32 nicht zum restlichen Netz durchbridged —
ARP vom Rechner aus bleibt erfolglos. Die FRITZ!Box selbst
(`50:E6:36:CD:1E:51`) ist am Standort der Kamera nur mit −87 dBm zu hören,
also zu schwach.

## Aufbau

```
ESP32-CAM (OV2640)          ESP32-C3            Rechner
  CamSender.ino  --ESP-NOW-->  BridgeReceiver.ino --USB--> viewer.py --> Browser
                  Kanal 1,      setzt Pakete        A5 5A A5 5A          :8080
                  Unicast       wieder zusammen     + len + JPEG
```

- **Unicast**, nicht Broadcast: gibt Link-Layer-ACKs und automatische
  Wiederholungen. Gemessen 8063 Pakete ohne einen einzigen Fehlversuch.
- Ein JPEG wird in Pakete von 244 Byte Nutzlast zerlegt (ESP-NOW erlaubt
  250 Byte inklusive Header). QVGA-Bilder brauchen rund 30 Pakete.
- Unvollständige Bilder werden verworfen, nicht halb angezeigt.

## Benutzen

```bash
# Bridge und Sender flashen (arduino-cli, Core esp32:esp32 3.3.3)
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc BridgeReceiver
arduino-cli upload  -b esp32:esp32:esp32c3:CDCOnBoot=cdc -p /dev/ttyACM0 BridgeReceiver
arduino-cli compile -b esp32:esp32:esp32cam CamSender
arduino-cli upload  -b esp32:esp32:esp32cam -p /dev/ttyUSB0 CamSender

# Viewer starten
python3 viewer.py --port /dev/ttyACM0 --http 8080
```

Dann `http://localhost:8080` öffnen. Weitere Endpunkte: `/snapshot` (letztes
Einzelbild), `/stats`, `/stream` (MJPEG).

## Gemessen (2026-07-26)

| Wert | Ergebnis |
|---|---|
| Bildrate | 9,2–9,3 Bilder/s bei 320×240 |
| Bildgröße | 4–7 kB |
| ESP-NOW-Pakete | 8063 gesendet, 0 fehlgeschlagen |
| Verworfene Bilder | 0 von 352 |

## Wenn die MAC der Bridge sich ändert

Die Bridge-MAC steht als `BRIDGE_MAC` in `CamSender.ino` fest
(`0C:4E:A0:30:7A:BC`). Bei einem anderen C3-Board die im Bridge-Log
ausgegebene `Eigene MAC` dort eintragen.

## Board-Eigenheiten der Kamera

- **Kein PSRAM.** Framebuffer liegt im DRAM, deshalb kein UXGA.
- **`esp_camera_init()` gelingt nur zu ca. 60 % pro Versuch.** Deshalb der
  Retry-Loop mit echtem PWDN-Power-Cycle. Ein Reset über RTS/EN startet nur
  den ESP32 neu, nicht den OV2640 — der behält seinen Zustand und antwortet
  danach mit `0x106 ESP_ERR_NOT_SUPPORTED`.
- **pyserial setzt beim `open()` DTR/RTS.** Beide *vor* dem Öffnen auf `False`
  setzen, sonst hält man das Board im Reset (ESP32) bzw. im Bootloader (C3).
  `viewer.py` macht das so.
