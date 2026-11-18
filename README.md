# Custom Zigbee Controller – ESP32-H2

A family of Zigbee-based controllers built around the **ESP32-H2**, designed for lighting automation, shutter/blind control, switch retrofitting, and LED dimming. Multiple PCB variants are available to fit different installation scenarios. All devices operate as **Zigbee routers** and are integrated in **Home Assistant (ZHA)**.

Since **v2.0.0** the firmware runs on the Espressif Zigbee stack **esp-zigbee-lib 2.x** (no ZBOSS), and firmware updates are done **over Bluetooth LE** from a web page or by cable (Zigbee OTA has been removed, see [Firmware updates](#firmware-updates)).

**PCB designs:**  https://oshwlab.com/scavenrage/project_oyjbpylj

---

## PCB Variants

All variants run the **same firmware**, configured per-device via `configure.py`.

### A) 4-Channel Relay PCB

A compact, modular relay board for multi-channel automation.

- Up to **4 relay outputs** (10 A rated)
- Up to **4 digital inputs** (3.3 V logic, pull-up)
- **Cuttable PCB**: physically trimmable to 2, 3, or 4 relays
- Integrated **230 VAC → 5 VDC** supply
- Suitable for lighting, impulse relays, and shutter motor control

> For inductive loads (e.g. shutter motors) install a suitable **snubber** near the motor.

---

### B) Opto Input Hat (for PCB A)

A companion board that adds mains-voltage input sensing to PCB A, whose native inputs only accept 3.3 V digital signals (dry-contact switches).

- Up to **4 optoisolated inputs**, one per channel
- Each input: series resistor (150 kΩ) into an **H11AA1M** AC-sensing optocoupler — unlike a standard phototransistor optocoupler, the H11AA1 has two anti-parallel internal LEDs, so it responds correctly to a 230 VAC phase signal without needing rectification
- **Cuttable PCB**: trimmable to 2, 3, or 4 channels, matching however many inputs of PCB A it is paired with
- Lets PCB A be triggered by real mains-phase switches instead of only dry-contact ones

---

### C) Single-Relay PCB (Universal Variant)

Designed for single-load automation and wall-switch retrofits.

- **1 relay output** (10 A rated)
- Integrated **230 VAC → 5 VDC** supply
- Output selectable by hardware: dry-contact or switched 230 V
- Input selectable by hardware: dry-contact or 230 V optoisolated sensing

> For inductive loads a **snubber** is required.

> If the 230 V optoisolated input is used: install a **100 nF 275 VAC X2 safety capacitor** between the line (L) and the switch input terminal (P). This suppresses fast switching transients on the mains line that would otherwise trigger the brownout detector and cause the device to disconnect from the Zigbee network.

---

### D) MOSFET / LED Control PCB

Solid-state output board for LED lamps and drivers with an external low-voltage supply.

- AO3400 MOSFET output stage
- Powered by **5 VDC** external (internal LDO to 3.3 V)
- Input compatible with capacitive touch modules or any 3.3 V digital signal
- Ideal for lamps/drivers that include their own AC supply

---

## Channel Types

On **PCB A and C** (relay outputs), each channel is independently configured via `configure.py` and channel types can be freely mixed on the same device — e.g. channel 1 as a roller shutter, channel 2 as a stable relay, channel 3 as an impulse relay:

| Type | Description |
|------|-------------|
| Stable relay | Standard ON/OFF — state saved in NVS, restored at power-on |
| Impulse relay | Output pulses for a configurable duration, then returns OFF |
| Roller shutter | Paired UP/DOWN outputs with travel-time calibration and position tracking |
| Unused | Channel disabled |

**PCB D** (MOSFET/LED) uses a solid-state output stage instead of relays, so it runs a separate channel type not available on A/C:

| Type | Description |
|------|-------------|
| PWM dimmer | LEDC-based dimmer with fade, long-press dimming, NVS state persistence (channel 1 only) |

---

## GPIO Pinout

| Signal | CH1 | CH2 | CH3 | CH4 |
|--------|-----|-----|-----|-----|
| Output | GPIO 4 | GPIO 5 | GPIO 10 | GPIO 11 |
| Input  | GPIO 1 | GPIO 0 | GPIO 3  | GPIO 2  |

Status LED: **GPIO 22** *(PCB A and C only — not present on PCB D)*  
BOOT button: **GPIO 9** (hold 5 s = Zigbee factory reset; short press = turn the BLE log off)  
Serial console: **UART0, 115200 baud** (TX/RX on the 4-pin programming header of PCB A)

---

## Status LED

> The status LED (GPIO 22) is present on **PCB A (4-channel relay)** and **PCB C (single relay)** only. PCB D (MOSFET/dimmer) does not have an onboard LED.

| Pattern | Meaning |
|---------|---------|
| Fast blink (100 ms) | Booting |
| Double blink every 2 s | Searching for Zigbee network / coordinator not confirmed yet |
| Single blink every 5 s | Operational (heartbeat) |
| Slow blink (500 ms on/off) | BLE update mode |
| 3 fast blinks every 3 s | Error |
| Solid on | Factory reset countdown in progress |

---

## Home Assistant (ZHA)

### Endpoints

| Endpoint | Content |
|----------|---------|
| 1…4 | One endpoint per active channel: **On/Off** (relays, dimmer) or **Window Covering** (shutters). EP1 also hosts the diagnostic cluster |
| 10 | Virtual switch **BLE** — restarts the device in BLE update mode |
| 11 | Virtual switch **PROG** — restarts the device in serial download mode (ROM bootloader) |
| 12 | Virtual switch **RESCRED** — Zigbee factory reset (erases the network credentials) |
| 13 | Virtual switch **LOG** — turns the live log over Bluetooth on/off (kept across restarts) |

The real state of the virtual switches is reported to ZHA every time the device becomes operational, so Home Assistant does not keep showing an old value after a restart.

### Quirk (optional)

`ha/smartswitch_diag.py` is a ZHA quirk (v2) for the model `SmartSwitchO` by `Handmade!`:

- names the virtual switches **BLE**, **PROG**, **RESCRED**, **LOG** (otherwise ZHA shows generic switch names);
- exposes the private diagnostic cluster **0xFF11** and a diagnostic sensor **"Riavvii anomali"** (unexpected restarts).

Install: copy the file to `/config/custom_zha_quirks/` (with `custom_quirks_path` set in the ZHA configuration), restart Home Assistant, then press **Reconfigure** on the device. Everything else (relays, shutters, dimmer) works without the quirk.

### Diagnostic cluster 0xFF11

Counters stored in NVS (namespace `diag`), readable from ZHA (*Manage Zigbee device → Clusters → SmartSwitch Diag → Read attribute*):

| Attr | Name | Meaning |
|------|------|---------|
| 0x0000 | uptime_s | Seconds since boot |
| 0x0001 | boots | Number of boots |
| 0x0002 | last_event | Last reset reason (`esp_reset_reason`) or 100 + firmware cause |
| 0x0003…0x0007 | rr_poweron, rr_brownout, rr_panic, rr_wdt, sw_untagged | Reset reasons |
| 0x0008…0x000B | cs_zb_wdt, cs_hard_reset, cs_phy_reset, cs_other | Restarts commanded by the firmware |
| 0x000C…0x000F | ev_ping_fail, ev_no_links, ev_leave, ev_steer_fail | Network events |
| 0x0010 | unexpected_resets | brownout + panic + watchdog + untagged software resets |

After every boot the device sends a single report of `unexpected_resets` to the coordinator, so an automation can warn when a device restarts unexpectedly. Do **not** configure attribute reporting on cluster 0xFF11 from ZHA.

---

## Firmware Architecture

### Per-device configuration via NVS

Device behaviour is configured at **first programming** using `configure.py`, which generates an NVS partition (`nvs_config.bin`) containing all parameters. The firmware reads this partition at boot — no recompilation needed to change a device's function.

The configuration survives firmware updates. Re-running `configure.py` and reflashing only the NVS partition (address `0x9000`) changes a device's behaviour; since v2.0.0 the Zigbee data also lives in the NVS partition, so rewriting it means **pairing the device again**.

### Zigbee

- Stack: **esp-zigbee-lib 2.x** through its 1.x compatibility layer (`CONFIG_ZB_SDK_1xx`)
- Operates exclusively as a **Zigbee router**; joins a coordinator on **any channel (11–26)**
- Exposes one endpoint per active channel plus the maintenance endpoints 10–13
- **On/Off cluster (0x0006)** for relay and dimmer channels
- **Window Covering cluster (0x0102)** for roller shutter channels; when a shutter stops (end of travel or STOP) its final position is reported explicitly, so Home Assistant shows 0 % / 100 % immediately also when the shutter is moved with the wall buttons
- Network data stored in the default NVS partition (namespace `zigbee`); devices coming from v1.x are migrated automatically (see below)

### Connection resilience

- **Coordinator keepalive** — a ZDO `ieee_addr_req` is sent to the coordinator every 5 minutes to confirm the path is alive, detecting "island" scenarios where the device is connected to other routers but has no route to the coordinator
- **ZDO timeout guard** — if the keepalive callback is never invoked (e.g. NWK route error instead of ZDP timeout), a 40 s safety alarm forces failure detection
- **Periodic attribute sync** — on every successful keepalive, relay states and shutter positions are written again, keeping routes active and ZHA updated
- **Restart when the coordinator is unreachable** — if 6 consecutive keepalive pings fail (≈ 4–5 minutes), the device enters deep sleep for 3 s and restarts **keeping its network credentials**, then rejoins by itself. If the coordinator is still off (e.g. Home Assistant under maintenance) the cycle repeats every ≈ 5 minutes until it is back; no device needs to be paired again. The Zigbee credentials are erased only on request (BOOT 5 s, RESCRED, BLE page) or when the stack reports the invalid address 0xFFFF after steering
- **PHY reset on persistent steering failure** — after 10 consecutive steering failures (≈ 5 minutes) the firmware erases only `phy_init` (RF calibration data) and restarts; the network credentials are kept, so the device rejoins without permit join
- **Clean-reset recovery** — after an unclean reset (panic, watchdog, brownout, software reset without a recorded cause) the device enters deep sleep for 3 s before starting Zigbee: on ESP32-H2 a software reset can leave the 802.15.4 radio degraded, a deep sleep wakeup reinitialises it like a power cycle. Restarts requested by the user (BLE update, factory reset, serial `reset`, BLE log switch) skip the deep sleep
- **Zigbee watchdog** — if the Zigbee stack stops running its scheduled tasks for more than 3 minutes, the device enters deep sleep for 3 s
- **Thread-safe stack access** — events from the physical inputs and from timers reach the Zigbee stack through its lock (required by esp-zigbee-lib 2.x)

### Maintenance over Bluetooth LE

Bluetooth and Zigbee share the single ESP32-H2 radio; BLE is only active when requested:

- **BLE update mode** (EP10 or the page below): Zigbee is not started, the device advertises as `SmartSwitch-XXXX` for 10 minutes and accepts a firmware image from `tools/ble_update.html`. The page can also erase the Zigbee credentials
- **BLE log** (EP13): the device keeps working on Zigbee and streams its log lines live to `tools/ble_log.html` (device name `SmartLog-XXXX`). Turn it off with EP13 or a short press of the BOOT button

Both pages use Web Bluetooth: open them in **Chrome or Edge** (desktop or Android), directly from the file.

### Serial commands

On the serial console (115200 baud): `prog` = restart in download mode, `reset` = normal restart, `help`.

---

## Getting Started

### Prerequisites

| Task | Tool required |
|------|---------------|
| Generate `nvs_config.bin` | Python 3 (no ESP-IDF needed) |
| Flash pre-built binaries | [esptool](https://github.com/espressif/esptool) (standalone release or `pip install esptool`) + a 3.3 V USB-serial adapter |
| Build firmware from source | [ESP-IDF v5.5.6](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32h2/get-started/index.html) |
| Firmware update over the air | Chrome or Edge with Bluetooth (`tools/ble_update.html`) |

---

### Step 1 — Generate device configuration

Run the wizard once for each device type you want to program:

```cmd
python configure.py
```

Only Python 3 is required — no ESP-IDF needed.

The wizard asks:
- How many channels (1–4)
- Type of each channel: stable relay, impulse relay, roller shutter, or PWM dimmer
- Input type for each channel: momentary pushbutton or bistable switch
- Timing parameters (debounce, impulse duration, shutter travel times, etc.)

Output files (generated in the same folder as `configure.py`):
- `nvs_config.csv` — human-readable key/value table, open with any text editor to verify
- `nvs_config.bin` — binary NVS partition ready to flash

---

### Step 2a — Flash pre-built binaries

Use this method if you do **not** need to build from source. Check the files against `binaries/SHA256SUMS.txt`.

**Enter download mode**: disconnect mains power, then power the board from the USB-serial adapter while GPIO 9 is held low (on PCB A: jumper on **H1**, then connect +5V/GND/TX/RX of the 4-pin header). Remove the jumper and power-cycle the board after flashing.

Full programming (erases everything, the device must be paired in ZHA afterwards):

```cmd
esptool --chip esp32h2 --port COM12 --baud 460800 --before no-reset --after no-reset erase-flash
esptool --chip esp32h2 --port COM12 --baud 460800 --before no-reset --after no-reset write-flash --flash-mode dio --flash-freq 48m --flash-size 4MB 0x0 binaries/bootloader.bin 0x8000 binaries/partition-table.bin 0x9000 nvs_config.bin 0x10000 binaries/ota_data_initial.bin 0x20000 binaries/smart_switch.bin
```

Replace `COM12` with your actual port (`/dev/ttyUSBx` on Linux/macOS). With esptool v4 use `esptool.py` and underscores (`erase_flash`, `write_flash`, `--flash_mode`…).

Flash `nvs_config.bin` alone to change the configuration (this also erases the Zigbee pairing):

```cmd
esptool --chip esp32h2 --port COM12 --before no-reset --after no-reset write-flash 0x9000 nvs_config.bin
```

---

### Step 2b — Build from source (optional)

Requires ESP-IDF v5.5.6. Run the following from the **ESP-IDF terminal**:

```cmd
cd firmware
idf.py set-target esp32h2
idf.py build
idf.py flash
esptool --chip esp32h2 --port COM12 write-flash 0x9000 nvs_config.bin
```

> `idf.py flash` writes bootloader, partition table and firmware but does **not** touch the NVS partition, so `nvs_config.bin` must be flashed separately on a new device.

---

## Firmware updates

### Over Bluetooth LE (recommended)

1. In Home Assistant turn on the **BLE** switch (EP10): the device restarts in BLE update mode (LED slow blink).
2. Open `tools/ble_update.html` in Chrome/Edge, choose `smart_switch.bin`, leave *"Erase the Zigbee credentials"* unchecked.
3. Press **Find device and send** and select `SmartSwitch-XXXX`.
4. The device checks the image, restarts with the new firmware and rejoins the network by itself.

If nothing is sent within 10 minutes the device restarts in normal mode.

### By cable

See Step 2a / 2b.

### Zigbee OTA

Removed in v2.0.0: the 1.x compatibility layer of esp-zigbee-lib 2.x does not provide the OTA cluster. `make_ota.py` is kept for building OTA images of **v1.x** firmware only.

### Upgrading from v1.x

- From a v1.x build with BLE update: update over BLE as above. On the first boot v2.0.0 migrates the Zigbee network data from the old `zb_storage` partition (`esp_zigbee_nvram_migrate()`), so the device **stays paired** (tested from v1.6.11). After the update press **Reconfigure** in ZHA if states stop updating.
- From v1.6.2 or older (no BLE update): flash by cable (Step 2a) and pair the device again.

---

See [CHANGELOG.md](CHANGELOG.md) for version history.

## License

- Firmware, Home Assistant quirk and tools: MIT — see [LICENSE](LICENSE).
- Hardware (`hardware/`: schematics, PCB): CERN-OHL-P-2.0 — see [hardware/LICENSE](hardware/LICENSE).
