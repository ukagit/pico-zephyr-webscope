# Changelog

Alle wichtigen Änderungen dieses Projekts werden hier dokumentiert.

## [0.2.2] - 2026-09-07

### Hinzugefügt

- Firmware- und Buildinformationen über den Shell-Befehl `info`
- Scope-Zeitbasis als Parameter der gemeinsamen Stream-Verbindung
- HTTP-Empfangs-Timeout gegen blockierende Browser-Verbindungen

### Geändert

- FFT und Scope verwenden einen gemeinsamen Binärstream auf Port 8080
- Scope-Schalter arbeitet lokal im Browser und öffnet keine weitere Verbindung
- Zeitbasiswechsel baut den gemeinsamen Stream kontrolliert neu auf
- Listen-Backlog des HTTP-Servers auf 8 erhöht

### Behoben

- sporadische FFT-Abbrüche durch mehrere parallele Dauerverbindungen
- `NetworkError` beim Einschalten des Scopes
- abgelehnte HTTP-Verbindungen bei geöffnetem Firefox
- nicht reagierende Scope-Zeitbasis-Tasten

## [0.2.0] - 2026-09-07

- erster multiplexter `FFT1`-/`SCP1`-Binärstream
- Firmware-Versionsanzeige

## [0.1.0] - 2026-09-06

- erste funktionsfähige WebScope-Version
- ADC/DMA-Erfassung mit 400 kSamples/s
- Scope, FFT und Wasserfall im Browser
- Zephyr-Shell und WLAN-Verbindung

