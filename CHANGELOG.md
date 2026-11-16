# Changelog

All notable changes to this project will be documented in this file.

---

## [v2.0.0]

First published release after v1.6.2. It also contains the work of the unpublished development builds v1.6.3–v1.6.11 (diagnostics, BLE update, BLE log, maintenance switches), listed here together.

### Zigbee stack
- **Changed:** Zigbee stack moved from esp-zigbee-lib 1.6 + esp-zboss-lib (ZBOSS) to **esp-zigbee-lib 2.0.x** (Espressif stack, no ZBOSS), used through its 1.x compatibility layer (`CONFIG_ZB_SDK_1xx=y`). Built with **ESP-IDF v5.5.6**. The deprecation `#warning`s of the compatibility headers are silenced for the `main` component only (`-Wno-cpp`)
- **New:** automatic migration of the network data of devices coming from v1.x: at the first boot `esp_zigbee_nvram_migrate()` copies the data from the old FAT partition `zb_storage` to the NVS partition (namespace `zigbee`), once (marker `zb2_mig/done`). Tested 1.6.11 → 2.0.0 over BLE: the device rejoined with the same address, no pairing needed
- **Changed:** the Zigbee data now live in the default NVS partition (namespace `zigbee`). Factory reset, hard reset and the BLE credentials erase clear that namespace plus `zb_fct` and the old `zb_storage`. Rewriting `nvs_config.bin` therefore also clears the pairing
- **Changed:** the device joins a coordinator on **any channel 11–26** (was channel 25 only, see v1.3.0)
- **Removed:** Zigbee OTA. The OTA cluster is not available in the 1.x compatibility layer of esp-zigbee-lib 2.x; updates are done over BLE or by cable
- **Fix:** events from the physical inputs and from timers (relay/impulse/shutter callbacks) now reach the Zigbee stack through the stack lock, as required by esp-zigbee-lib 2.x. Without it scheduler alarms were lost and the Zigbee watchdog fired after a few minutes of use
- **Fix:** the coordinator keepalive is started only once, on successful steering (a second chain doubled the ping rate)
- **Fix:** roller shutters — the final position is reported explicitly when the shutter stops. esp-zigbee-lib 2.x sends an automatic report only when the change is *greater than* the configured reportable change (espressif/esp-zigbee-sdk#903); with the value 1 requested by ZHA the last 1 % step (e.g. 1 → 0) was never reported and Home Assistant stayed at 99 % after moving the shutter with the wall buttons
- **New:** the state of the virtual switches EP10–13 is reported every time the device becomes operational (Home Assistant kept showing the value they had before a restart)

### Maintenance and diagnostics (from the unpublished v1.6.x builds)
- **New:** private diagnostic cluster **0xFF11** on EP1 with counters stored in NVS: uptime, boots, last reset reason, reset reasons (power-on, brownout, panic, watchdog, untagged software reset), firmware-commanded restarts, network events, and `unexpected_resets` (0x0010)
- **New:** after every boot a single report of `unexpected_resets` is sent to the coordinator, for a Home Assistant alarm on unexpected restarts
- **New:** firmware update over **Bluetooth LE** (`tools/ble_update.html`, Web Bluetooth): 10-minute window, Zigbee not started during the update, optional erase of the Zigbee credentials at the end or alone
- **New:** live log over Bluetooth LE (`tools/ble_log.html`) while Zigbee keeps running; the setting survives restarts, a short press of the BOOT button turns it off
- **New:** virtual On/Off switches **EP10 BLE**, **EP11 PROG** (serial download mode), **EP12 RESCRED** (Zigbee factory reset), **EP13 LOG**
- **New:** serial commands on the console: `prog`, `reset`, `help`
- **New:** restarts requested by the user skip the 3 s recovery deep sleep and are not counted as unexpected
- **New:** ZHA quirk `ha/smartswitch_diag.py`: names of the virtual switches (all enabled), diagnostic cluster and "Riavvii anomali" sensor
- **New:** `tools/serial_log.py` to save the serial output to a file without resetting the board
- **Changed:** CCA threshold set to −60 dBm (`CONFIG_IEEE802154_CCA_THRESHOLD`), the value of the IDF 5.1.2 builds (IDF 5.5 default is −75 dBm)
- **Changed:** core dump disabled in the release build (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`); instructions for the bench build in `sdkconfig.defaults`

---

## [v1.6.2]

- **Fix:** Steering failure loop now triggers a PHY reset instead of retrying indefinitely — after 10 consecutive steering failures (≈ 5 minutes at 30 s intervals), the firmware erases the `phy_init` partition and restarts. `zb_storage` and `zb_fct` are left intact, so the device performs a normal rejoin without requiring permit join on the coordinator. Root cause: RF interference can corrupt the PHY calibration data stored in `phy_init` (`pll_cal exceeds 2ms`), causing the radio to fail every steering attempt in ~144 ms. Erasing `phy_init` forces the radio to recalibrate from scratch on the next boot.
- **Fix:** `zigbee_hard_reset()` now also erases `phy_init` in addition to `zb_storage` and `zb_fct` — ensures a clean PHY state alongside a clean Zigbee credential state when a definitive disconnection is detected.

---

## [v1.6.1]

- **Fix:** RF-induced disconnection now triggers a Zigbee hard reset instead of a deep sleep loop — when 6 consecutive ZDO keepalive pings to the coordinator fail (≈ 4–5 minutes, typically caused by RF interference), the firmware now erases `zb_storage` and `zb_fct` and restarts, forcing a clean rejoin from scratch. The previous deep sleep loop could not self-recover because the corrupted Zigbee stack state persists in flash across deep sleep cycles and is not cleared by deep sleep wakeup. The same hard reset is applied when the post-steering address is `0xFFFF` (ZBOSS bug #727). The NVS configuration (channel types, timings, etc.) is not affected.

---

## [v1.6.0]

- **Fix:** Brownout detector threshold lowered to level 0 (~2.43 V) in `sdkconfig.defaults` — the default threshold was triggering spurious brownout resets caused by fast 230 V switching transients coupled through the AC-input optoisolator (PCB C). The 5 V supply with 3.3 V LDO easily absorbs brief transients without compromising operation
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
