# Changelog

All notable changes to this project will be documented in this file.

---

## [v1.2.0]

- **Fix:** Dispositivi che vanno offline senza crashare — aggiunta gestione del segnale `ESP_ZB_ZDO_SIGNAL_LEAVE`: il dispositivo ora ritenta automaticamente lo steering dopo 5 s invece di restare offline fino al prossimo riavvio fisico
- **New:** Watchdog applicativo Zigbee — una callback schedulata ogni 60 s verifica che lo stack sia attivo; se non risponde per più di 3 minuti il firmware esegue `esp_restart()` in autonomia
- **Fix:** Correzione numerazione versioni OTA — lo schema corretto è `0xMMNN00PP` (MM=major, NN=minor, PP=patch su 8 bit); il valore precedente `0x00010100` era malformato e produceva versioni senza senso

## [v1.1.0]

- **New:** Zigbee factory reset via BOOT button (GPIO 9) — hold 5 s to erase network credentials and restart steering
- **Fix:** Crash (Guru Meditation) on shutter input events — `nvs_task` stack overflow in `shutter.c` and `relay.c` (1024 → 2048 bytes)
- **Fix:** ZHA entities appearing as unavailable after pairing — incorrect Identify cluster creation in relay endpoint (replaced `esp_zb_zcl_attr_list_create` with `esp_zb_identify_cluster_create`)
- **Fix:** Relay state now synced to Zigbee immediately after network steering, matching existing shutter position sync behaviour

## [v1.0.1]

- Initial public release
