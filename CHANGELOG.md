# Changelog

All notable changes to this project will be documented in this file.

---

## [v1.2.0]

- **Fix:** Device going offline without crashing — added handling for `ESP_ZB_ZDO_SIGNAL_LEAVE` signal: device now automatically retries network steering after 5 s instead of staying offline until power cycled
- **New:** Application-level Zigbee watchdog — a scheduler alarm fires every 60 s to confirm the stack is alive; if no feed is received for more than 3 minutes, the firmware calls `esp_restart()` automatically
- **Fix:** OTA version numbering corrected — scheme is now `0xMMNN00PP` (MM=major, NN=minor, PP=patch); previous value `0x00010100` was malformed and produced nonsensical version strings

## [v1.1.0]

- **New:** Zigbee factory reset via BOOT button (GPIO 9) — hold 5 s to erase network credentials and restart steering
- **Fix:** Crash (Guru Meditation) on shutter input events — `nvs_task` stack overflow in `shutter.c` and `relay.c` (1024 → 2048 bytes)
- **Fix:** ZHA entities appearing as unavailable after pairing — incorrect Identify cluster creation in relay endpoint (replaced `esp_zb_zcl_attr_list_create` with `esp_zb_identify_cluster_create`)
- **Fix:** Relay state now synced to Zigbee immediately after network steering, matching existing shutter position sync behaviour

## [v1.0.1]

- Initial public release
