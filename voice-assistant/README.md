# Satellite vocale Home Assistant (Waveshare ESP32-S3-AUDIO-Board)

Configurazione ESPHome basata su
[jnix85/esphome-waveshare-esp32-s3-audio-va](https://github.com/jnix85/esphome-waveshare-esp32-s3-audio-va)
(core pinnato a `v1.1.0`, `min_version` ESPHome 2025.8.0). Il file `waveshare-va.yaml` contiene
solo le impostazioni locali (nome, fuso orario, volume, wake word "Hey Jarvis" in aggiunta ad
"Alexa" e "Okay Nabu").

## Stato
- Validazione schema con ESPHome 2026.9.1: nessun errore di schema; i 4 download dei suoni sono
  falliti per il proxy della sandbox (403), quindi la compilazione completa **non e' stata provata**.
- Mai flashato su hardware: pin, audio e wake word vanno verificati sulla scheda reale.

## Passi
1. In HA installa gli add-on: **ESPHome Device Builder**, **Whisper** (lingua `it`), **Piper** (voce italiana).
2. In HA crea una pipeline Assist in italiano con Whisper + Piper.
3. In ESPHome crea `secrets.yaml` da `secrets.example.yaml` e carica `waveshare-va.yaml`.
4. Primo flash via USB-C (browser Chrome/Edge o `esphome run waveshare-va.yaml`), poi OTA.
5. In HA assegna la pipeline al dispositivo e scegli la wake word dall'entita' dedicata.
