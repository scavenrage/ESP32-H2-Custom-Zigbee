# Changelog

All notable changes to this project will be documented in this file.

---

## [v1.6.0]

- **Fix:** Brownout detector threshold lowered to level 0 (~2.43 V) in `sdkconfig.defaults` — the default threshold was triggering spurious brownout resets caused by fast 230 V switching transients coupled through the AC-input optoisolator (PCB B). The 5 V supply with 3.3 V LDO easily absorbs brief transients without compromising operation
- **Fix:** All recovery-path restarts replaced with `esp_deep_sleep_start()` (3 s wakeup timer) — on ESP32-H2, `esp_restart()` issues a SW_CPU reset that leaves the IEEE 802.15.4 radio in a degraded state (LQI drops to 0, rejoin fails silently). Deep sleep wakeup is equivalent to a power cycle for the radio, ensuring a clean reinitialisation before any Zigbee stack activity
- **New:** Boot-time reset reason check — at the very start of `app_main()`, before any Zigbee initialisation, the firmware checks `esp_reset_reason()`. If the reason is not POWERON, DEEPSLEEP, or EXT (i.e. it is a SW_CPU, brownout, panic, or watchdog reset), the firmware immediately enters deep sleep for 3 s. This prevents the device from attempting to rejoin on a degraded radio and avoids the previous 4–5 minute futile retry cycle
- **New:** Post-steering address sanity check — after a successful `BDB_SIGNAL_STEERING`, the firmware verifies that `esp_zb_get_short_address() != 0xFFFF`. An address of 0xFFFF indicates corrupted ZBOSS state (bug #727: steering reports success but the radio is not actually associated). If detected, the firmware enters deep sleep immediately instead of entering an unrecoverable stuck state

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
