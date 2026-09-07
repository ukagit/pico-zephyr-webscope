# Fehlerdiagnose

## `west: command not found`

```bash
cd /home/ulrich/Dokumente/zephyrproject
source .venv/bin/activate
source zephyr/zephyr-env.sh
```

## `Can't find target/rp2350.cfg`

Die OpenOCD-Version aus dem Zephyr SDK enthält möglicherweise noch keine
RP2350-Zieldatei. Eine passende OpenOCD-Installation verwenden und deren
`share/openocd/scripts` als Suchpfad angeben.

## Falsche Firmware nach erfolgreichem Build

Immer kontrollieren, welches Image der Flash-Befehl verwendet. Für den
normalen Build ist dies:

```text
build/zephyr/zephyr.hex
```

Nicht versehentlich ein älteres `build-ap/`-Image flashen.

Die laufende Version zeigt:

```text
info
```

## Webseite nicht erreichbar

```text
wifi status
net iface
```

`wifi status` muss `State: COMPLETED` anzeigen. Die IP-Adresse kann sich nach
einem Neustart durch DHCP ändern.

Vom PC prüfen:

```bash
ping -c 3 PICO-IP
curl -v --max-time 5 http://PICO-IP:8080/
```

`Keine Route zum Zielrechner` bedeutet, dass der Pico unter dieser Adresse
nicht erreichbar ist; es ist kein HTTP-Fehler.

## Browser zeigt alte Funktionen

Firefox-Tab schließen und die Seite mit `Strg+Umschalt+R` neu laden. Die im
Firmware-Image enthaltenen Zeichenketten lassen sich prüfen mit:

```bash
strings build/zephyr/zephyr.elf | rg 'scope-step|scope-enable|streamScope'
```

## Scope bleibt leer

ADC-Status prüfen und Erfassung starten:

```text
adc status
adc start 1000000
```

Danach im Browser `Scope einschalten` wählen.

## Langsame Zeitbasis reagiert verzögert

Das ist erwartbar. Bei 2 s/div umfasst ein vollständiges Bild viele Sekunden.
Nach der Umschaltung muss zunächst ein neuer Datensatz aufgenommen werden.

## `NetworkError when attempting to fetch resource`

Ab Firmware 0.2.2 benötigen Scope-Schalter und Zeitbasis keine zusätzliche
dauerhafte Verbindung. FFT und Scope laufen gemeinsam über `/fft-stream`.

UART auf folgende Meldungen prüfen:

```text
FFT stream: browser connected
FFT stream: browser disconnected
WebDisplay: send failed, errno=...
```

## ADC-Diagnose

```text
adc status
```

Ein gesunder Lauf zeigt unter anderem:

```text
running: yes
dropped: 0
DMA errors: 0
```

## CPU- und Stackauslastung

```text
system load
```

Besonders auf sehr geringe freie Stackreserven achten. Eine hohe Idle-Zeit ist
normal und zeigt, dass das Canvas-Rendering erfolgreich in den Browser
ausgelagert wurde.

