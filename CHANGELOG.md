# Changelog

All notable changes to this project will be documented in this file.

---

## [v1.1.0]

- **New:** Zigbee factory reset via BOOT button (GPIO 9) — hold 5 s to erase network credentials and restart steering
- **Fix:** Crash (Guru Meditation) on shutter input events — `nvs_task` stack overflow in `shutter.c` and `relay.c` (1024 → 2048 bytes)
- **Fix:** ZHA entities appearing as unavailable after pairing — incorrect Identify cluster creation in relay endpoint (replaced `esp_zb_zcl_attr_list_create` with `esp_zb_identify_cluster_create`)
- **Fix:** Relay state now synced to Zigbee immediately after network steering, matching existing shutter position sync behaviour

## [v1.0.1]

- Initial public release
