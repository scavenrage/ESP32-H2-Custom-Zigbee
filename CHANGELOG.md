# Changelog

All notable changes to this project will be documented in this file.

---

## [v1.5.0]

- **Fix:** ZDO keepalive callback never invoked on route error — when the NWK layer returns a route error (instead of waiting for a ZDP timeout), `coordinator_ping_result` was never called, leaving the device permanently stuck in double-blink with no further alarms scheduled. Added a 40 s safety timeout (`coordinator_ping_timeout_cb`): if the callback does not arrive within 40 seconds, failure is forced manually
- **New:** Automatic restart after consecutive ZDO failures — if the coordinator is unreachable for 6 consecutive cycles (≈ 4–5 minutes), the device calls `esp_restart()`. A reboot clears the neighbor table and routing table, the only reliable recovery from corrupted routing state that `bdb_start_top_level_commissioning(NETWORK_STEERING)` cannot fix (routing table is not cleared on an already-associated device)
- **Improvement:** Periodic attribute reporting — on every successful ZDO ping (every 5 minutes) the firmware sends current relay states and shutter positions with `report=true`. This keeps ZHA updated even when no physical events occur, and stimulates route discovery from the device side to keep routes alive

## [v1.4.0]

- **New:** Coordinator keepalive / island detection — every 5 minutes the device sends a ZDO `ieee_addr_req` to the coordinator (0x0000). If 2 consecutive requests time out (`ESP_ZB_ZDP_STATUS_TIMEOUT`), the path to the coordinator is considered broken and network steering is triggered immediately without restarting. This fixes the silent "island" scenario where two routers maintain a link with each other but have no route to the coordinator — a condition that `NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT` never catches because at least one active neighbor still exists

## [v1.3.0]

- **Fix:** Silent disconnection no longer requires manual power cycle — added handling for `ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT` (0x18): when the device loses all active network links without receiving an explicit LEAVE, it now sets itself to searching state and retries steering after 1 s. Previously the device believed it was still connected (LED remained operational) while ZHA reported it as unavailable
- **Fix:** `ESP_ZB_NLME_STATUS_INDICATION` (0x32) and `ESP_ZB_ZDO_DEVICE_UNAVAILABLE` (0x3c) signals now handled explicitly — they were falling through to the default log handler and causing noisy output with no action taken
- **Improvement:** Zigbee channel scan restricted to channel 25 only (`ESP_ZB_PRIMARY_CHANNEL_MASK = 1UL << 25`) — previously all 16 channels (11–26) were scanned on every steering attempt, significantly slowing down reconnection
- **Improvement:** `IEEE802154_TIMING_OPTIMIZATION` enabled in `sdkconfig.defaults` — improves radio stability and OTA throughput as recommended by Espressif for ESP32-H2
- **Improvement:** `FREERTOS_HZ` raised from 100 to 1000 in `sdkconfig.defaults` — improves task responsiveness, particularly during OTA transfers

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
