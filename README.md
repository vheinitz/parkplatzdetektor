# Parkplatzdetektor — Anlerndaten

> Gesamtkonzept des Parkassistenten (Sensor → Gateway → Server → App):
> [`PROJEKT_PROMPT.md`](PROJEKT_PROMPT.md). Der API-Server dazu liegt in
> [`server/`](server/README.md) — Flask + SQLite, für PythonAnywhere.
> Die hier beschriebene Kamera-Erkennung kann später als *ein* Gateway mit
> vielen virtuellen Sensoren an diesen Server melden.

Quelle: <https://www.youtube.com/shorts/_jUIhPSRhks> — Drohnen-Draufsicht auf einen
großen Parkplatz, 1080×1920, 29,97 fps, ~11 s. Die Drohne steht praktisch still,
deshalb gilt **ein** Raster für alle Frames.

## Inhalt

| Pfad | Inhalt |
|---|---|
| `video.webm` | Originalvideo |
| `frames/` | 33 Frames (jedes 10. Bild) |
| `grid.json` | gefittete Rastergeometrie |
| `overlay/` | Frames mit eingezeichnetem Raster (Sichtprüfung) |
| `slots/` | Einzelplatz-Ausschnitte + `index.csv` |
| `detect.py` | **Prototyp:** ein Bild rein → Raster erkennen, freie Plätze zählen |
| `markings.py` | Markierungs-Detektion |
| `fit_grid.py` | Raster fitten → `grid.json` |
| `occupancy.py` | belegt/frei je Stellplatz, per YOLO |
| `models/` | YOLO-Gewichte |
| `draw_grid.py` | Raster zeichnen → `overlay/` |
| `export_slots.py` | Ausschnitte exportieren → `slots/` |

```bash
python3 detect.py bild.jpg            # Prototyp, zeigt das Ergebnis direkt an
python3 fit_grid.py                   # Raster neu bestimmen -> grid.json
python3 draw_grid.py                  # Overlays für alle Frames
python3 export_slots.py --every 8 --margin 6
```

`detect.py` sucht das Raster in genau dem übergebenen Bild — es liest `grid.json`
nicht und setzt nichts voraus. Laufzeit ~1,5 s. **Freie Plätze werden schwarz
ausgemalt**, die Zählung steht im Bild und auf der Konsole:

```
Fahrzeuge erkannt: 557
FREI       6
BELEGT   484
GESAMT   490   (98.8 % belegt)
```

Optionen: `-o out.png`, `--json g.json`, `--no-show`, `--quiet`, `--boxes`,
`--conf`, `--min-overlap`, `--imgsz`, `--model`, `--device`.

## Gemessene Geometrie

| | Block 0 (oben) | Block 1 (unten) |
|---|---|---|
| Spalten | 5 | 5 |
| Unterspalten je Spalte | 2 | 2 |
| Reihen je Spalte | **25** | **24** |
| Stellplätze | 250 | 240 |

**Gesamt: 490 Stellplätze.** Reihenteilung 31,7–32,2 px, Buchttiefe ~73 px,
Spaltenabstand ~224 px. Über 32 px Teilung entspricht 1 px ≈ 8 cm — die
Buchttiefe von ~5,9 m und die Fahrgasse von ~6,3 m passen zu üblichen Maßen.

### Abweichung zur Selbstprüfung

2 Blöcke × 5 Spalten × 2 Unterspalten stimmen. Die geschätzten „ca. 30
Auto-Paare“ je Spalte sind **gemessen 24–25** — Blockhöhe 811 bzw. 772 px
geteilt durch die Teilung von ~31,9 px. Die Rasterlinien liegen im Overlay auf
den weißen Markierungen, und je Zelle steht genau ein Auto; die Zählung ist
damit belastbar. Die Schätzung lag also rund 20 % zu hoch.

## Verfahren

Fahrende Autos auf den Fahrgassen stören nicht: gefittet wird die Geometrie der
**Markierungen**, und die Fahrgassen liegen zwischen den Spalten, also außerhalb
des Rasters.

1. **Markierungen isolieren** — Fahrbahnmarkierungen sind dünne helle,
   entsättigte Linien; Autodächer sind ebenso hell, aber quer dazu viel dicker.
   Ein Top-Hat mit linienförmigem Strukturelement quer zur gesuchten Richtung
   unterdrückt deshalb die Autos.
2. **Blöcke und Spalten** über Struktur-Energie in Zeilen- bzw. Spaltenprojektion.
3. **Kamm-Fit je Spalte** (Teilung + Phase). Nötig, weil einzelne Linien unter
   parkenden Autos unsichtbar sind — reine Peak-Suche findet nur ~80 % und
   verschätzt sich dadurch in der Teilung.
4. **Lokales Einrasten** auf die tatsächlichen Markierungen, Restfehler linear
   über die Reihen modelliert (fängt leichte Perspektivdrift ab).
5. **Randreihen verwerfen**, deren Mitte außerhalb des Blocks liegt.

## Belegung per YOLO

`occupancy.py` erkennt Fahrzeuge mit YOLO und markiert eine Zelle als belegt,
wenn ein Fahrzeug sie ausreichend überdeckt. Auf diesem Bild: **557 Fahrzeuge,
6 freie Plätze**, im Mittel 9,5 über die 33 Frames.

```bash
pip3 install --user ultralytics
python3 detect.py frames/frame_0001.jpg --boxes    # Fahrzeugboxen mitzeichnen
```

### Modellwahl: nicht COCO, sondern DOTA

Die COCO-Gewichte (`yolo11n.pt`) sind hier **unbrauchbar** — COCO enthält
praktisch keine Draufsichten. Gemessen auf `frame_0001.jpg`:

| Gewichte | imgsz | gefundene Fahrzeuge |
|---|---|---|
| `yolo11n.pt` / `yolo11s.pt` (COCO) | 640 / 1280 / 1920 | **0** |
| `yolo11n-obb.pt` (DOTA) | 1024 | **557** |
| `yolo11s-obb.pt` (DOTA) | 1024 | 509 |

Das COCO-Modell hält die Autos für „cell phone“ — auch bei 4-fach
hochskaliertem Ausschnitt. Verwendet wird deshalb `yolo11n-obb.pt`, trainiert
auf DOTA (Luftbilder), mit rotierten Boxen und den Klassen *small vehicle* /
*large vehicle*. Rotierte Boxen passen zur Draufsicht deutlich besser als
achsparallele. Laufzeit ~1 s je Bild auf CPU.

### Zuordnung über Fläche, nicht über den Mittelpunkt

Eine Zelle gilt als belegt, wenn ein Fahrzeug ≥ 20 % ihrer Fläche überdeckt.
Der naheliegende Weg — Fahrzeugmittelpunkt in Zelle — ergab **14 statt 6**
freie Zellen: in der schraffierten Behindertenzone sitzt das regelmäßige Raster
versetzt, dort fällt der Mittelpunkt in die Nachbarzelle. Die Flächenüberlappung
ist gegen diesen Versatz robust.

Fahrende Autos auf den Fahrgassen zählen automatisch nicht mit — sie überlappen
keine Rasterzelle.

### Grenzen

- Die 6 freien Zellen im Testbild sind: 4 wirklich leere Buchten, 1
  Einkaufswagen-Box und 1 Rest-Artefakt aus der Schraffurzone. YOLO zählt die
  Einkaufswagen-Box korrekt nicht als Fahrzeug — für „parkbar?“ ist das aber
  falsch.
- Über die Frames schwankt die Zahl (6–13). Ursachen: bewegte Schatten,
  leichte Drohnendrift, ein- und ausparkende Autos.
- Gelegentlich liegen zwei Detektionen auf einem Auto, deshalb 557 Detektionen
  bei ~490 Stellplätzen plus Fahrzeugen auf Gassen und Straße.
- Die `free`-Spalte in `slots/index.csv` ist ein **Vorschlag**, kein geprüftes
  Label. Zum Anlernen nachkontrollieren.
- Stellschrauben: `--conf`, `--min-overlap`, `--imgsz`, `--model`, `--device 0`
  für GPU.

## Bekannte Einschränkungen

- **Behindertenparkzone**, Block 0 Spalten 3 und 4, oberste ~5 Reihen: dort sind
  die Buchten breiter und durch schraffierte Zugangsstreifen getrennt (im Bild an
  den Rollstuhl-Symbolen erkennbar). Das gleichmäßige Raster bildet diesen
  Bereich nur näherungsweise ab — Zelle und reale Bucht entsprechen sich dort
  nicht 1:1. Für Training am besten ausschließen.
- Einzelne Zellen enthalten **Einkaufswagen-Boxen** statt Autos (Block 0
  Spalte 2). Als eigene Klasse behandeln oder aussortieren.
- Autos **überhängen** die Markierung. `export_slots.py --margin` steuert den
  Rand; Vorgabe 6 px.
- `grid.json` enthält je Rasterlinie `line_conf` (Markierungsstärke, auf den
  Spaltenmedian normiert). Kleine Werte heißen: Linie war verdeckt oder das
  Raster passt dort schlecht. Über `export_slots.py --min-conf` filterbar.
- Das Raster ist **auf `frames/frame_0001.jpg` gefittet**. Bei anderem
  Videomaterial oder bewegter Drohne muss `fit_grid.py` neu laufen bzw. je Frame
  registriert werden.
- Die 33 Frames liegen nur ~0,33 s auseinander, sind also stark redundant. Für
  mehr Varianz eher zeitbasiert abtasten (`ffmpeg -vf fps=1`).
