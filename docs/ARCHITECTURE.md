# Architektur

## Ziel

Der Raspberry Pi Pico 2 W soll Messdaten erfassen, aber nicht wie ein einfacher
TFT-Controller jeden Bildpunkt selbst zeichnen. Die rechen- und
speicherintensiven Grafikoperationen übernimmt der Webbrowser.

## Datenweg

```text
GP26 / ADC0
    ↓
RP2350 ADC-FIFO
    ↓ DREQ_ADC
DMA-Ringpuffer
    ↓
Scope- und Spektrum-Aufbereitung
    ↓
gemeinsamer TCP-Binärstream auf Port 8080
    ↓
JavaScript im Browser
    ├── Scope-Canvas
    ├── FFT-Berechnung und Spektrum-Canvas
    └── Wasserfall-Canvas
```

## Firmware-Komponenten

### `src/adc_dma.c`

- initialisiert ADC0 an GP26
- konfiguriert eine Abtastrate von 400.000 Samples/s
- überträgt ADC-Werte über DMA in Speicherblöcke
- verwaltet Blockzustände und Statistikzähler
- liefert fertige Datenblöcke an die Anwendung

### `src/main.c`

- stellt die Zephyr-Shell bereit
- verbindet das WLAN mit gespeicherten Zugangsdaten
- betreibt den HTTP-Server auf Port 8080
- liefert die komplette HTML-/JavaScript-Seite aus dem Flash aus
- erzeugt Scope-Daten für verschiedene Zeitbasen
- erzeugt Zeitdaten für schnelle und hochauflösende FFT-Modi
- multiplexiert `FFT1`- und `SCP1`-Pakete in einem Binärstream

## Warum nur eine dauerhafte Datenverbindung?

Frühere Versionen verwendeten getrennte Verbindungen für FFT und Scope. Unter
anhaltender Datenlast erschöpfte dies zeitweise Netzwerkressourcen des
CYW43439-/Zephyr-Stacks. Version 0.2.x verwendet deshalb einen gemeinsamen
Binärstream.

Zusätzlich besitzt der HTTP-Server:

- einen Listen-Backlog von 8 Verbindungen
- ein Empfangs-Timeout für angenommene, aber noch leere HTTP-Verbindungen

Dadurch blockieren von Firefox vorsorglich geöffnete Verbindungen nicht mehr
dauerhaft den einzigen HTTP-Thread.

## Paketformat

Alle kontinuierlich übertragenen Frames beginnen mit einem gepackten Header
von 32 Byte. Mehrbytewerte werden Little Endian übertragen.

| Offset | Größe | Feld |
|---:|---:|---|
| 0 | 4 | Magic `FFT1` oder `SCP1` |
| 4 | 4 | Sequenznummer |
| 8 | 4 | Abtastrate |
| 12 | 2 | Anzahl `uint16_t`-Samples |
| 14 | 2 | reserviert |
| 16 | 4 | Scope-Step, bei FFT null |
| 20 | 4 | Frequenz in Millihertz |
| 24 | 2 | Minimum |
| 26 | 2 | Maximum |
| 28 | 2 | Mittelwert |
| 30 | 2 | reserviert |

Direkt anschließend folgen `sample_count` vorzeichenlose 16-Bit-ADC-Werte.

## Scope

Der Scope stellt 100 Punkte dar. Die Zeitbasis bestimmt, wie viele
ADC-Abtastwerte zu einem dargestellten Punkt zusammengefasst beziehungsweise
übersprungen werden. Sehr langsame Zeitbasen benötigen deshalb länger, bevor
ein kompletter Datensatz vorliegt.

Der Browser zeichnet:

- Raster und Spannungsmarken
- Signalverlauf
- Minimum, Maximum und Mittelwert
- erkannte Frequenz

## FFT und Wasserfall

Der Pico überträgt ADC-Zeitdaten. JavaScript führt im Browser die FFT aus und
zeichnet Spektrum und Wasserfall. Dadurch verbleiben trigonometrische
Berechnung, Farbabbildung und Canvas-Rendering auf dem PC.

Verfügbare Darstellungsbereiche:

- 20 kHz
- 50 kHz
- 100 kHz
- 200 kHz

Die angezeigte Bin-Auflösung ergibt sich aus effektiver Abtastrate geteilt
durch FFT-Länge und wird direkt in der Weboberfläche angegeben.

## Nebenläufigkeit

ADC/DMA, Datenaufbereitung, HTTP, USB-Shell und WLAN laufen in getrennten
Zephyr-Kontexten. Ein Mutex schützt gemeinsam genutzte Scope- und
Spektrumdaten. Der Binärstream kopiert einen konsistenten Datensatz, bevor er
ihn über TCP sendet.

