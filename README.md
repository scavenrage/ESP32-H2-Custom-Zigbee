# Custom Zigbee Controller – ESP32-H2

A family of Zigbee-based controllers built around the **ESP32-H2**, designed for lighting automation, shutter/blind control, switch retrofitting, and LED dimming. Multiple PCB variants are available to fit different installation scenarios. All devices operate as **Zigbee routers** and support **Zigbee OTA firmware updates**.

**PCB designs:**  https://oshwlab.com/scavenrage/project_oyjbpylj

---

## PCB Variants

All three variants run the **same firmware**, configured per-device via `configure.py`.

### A) 4-Channel Relay PCB

A compact, modular relay board for multi-channel automation.

- Up to **4 relay outputs** (10 A rated)
- Up to **4 digital inputs** (3.3 V logic, pull-up)
- **Cuttable PCB**: physically trimmable to 2, 3, or 4 relays
- Integrated **230 VAC → 5 VDC** supply
- Suitable for lighting, impulse relays, and shutter motor control

> For inductive loads (e.g. shutter motors) install a suitable **snubber** near the motor.

---

### B) Single-Relay PCB (Universal Variant)

Designed for single-load automation and wall-switch retrofits.

- **1 relay output** (10 A rated)
- Integrated **230 VAC → 5 VDC** supply
- Output selectable by hardware: dry-contact or switched 230 V
- Input selectable by hardware: dry-contact or 230 V optoisolated sensing

> For inductive loads a **snubber** is required.

---

### C) MOSFET / LED Control PCB

Solid-state output board for LED lamps and drivers with an external low-voltage supply.

- AO3400 MOSFET output stage
- Powered by **5 VDC** external (internal LDO to 3.3 V)
- Input compatible with capacitive touch modules or any 3.3 V digital signal
- Ideal for lamps/drivers that include their own AC supply

---

## Channel Types

Each output channel is independently configured via `configure.py`:

| Type | Description |
|------|-------------|
| Stable relay | Standard ON/OFF — state saved in NVS, restored at power-on |
| Impulse relay | Output pulses for a configurable duration, then returns OFF |
| Roller shutter | Paired UP/DOWN outputs with travel-time calibration and position tracking |
| PWM dimmer | LEDC-based dimmer with fade, long-press dimming, NVS state persistence (channel 1 only) |
| Unused | Channel disabled |

---

## GPIO Pinout

| Signal | CH1 | CH2 | CH3 | CH4 |
|--------|-----|-----|-----|-----|
| Output | GPIO 4 | GPIO 5 | GPIO 10 | GPIO 11 |
| Input  | GPIO 1 | GPIO 0 | GPIO 3  | GPIO 2  |

Status LED: **GPIO 22** *(PCB A and B only — not present on PCB C)*  
Factory Reset: **GPIO 9** (BOOT button — hold 5 s)

---

## Status LED

> The status LED (GPIO 22) is present on **PCB A (4-channel relay)** and **PCB B (single relay)** only. PCB C (MOSFET/dimmer) does not have an onboard LED.

| Pattern | Meaning |
|---------|---------|
| Fast blink (100 ms) | Booting |
| Double blink every 2 s | Searching for Zigbee network |
| Single blink every 5 s | Operational (heartbeat) |
| Slow blink (500 ms on/off) | OTA firmware download in progress |
| 3 fast blinks every 3 s | Error |
| Solid on | Factory reset countdown in progress |

---

## Factory Reset

The **BOOT button (GPIO 9)** doubles as a Zigbee factory reset trigger at any time after power-up.

**How to use:**

1. Hold the BOOT button for **5 seconds** — the status LED switches to solid white during the countdown.
2. Release after 5 s — the device erases all Zigbee network data (`zb_storage` and `zb_fct` partitions) and reboots.
3. After reboot, the device starts fresh network steering and can be added to a new Zigbee network (e.g. via ZHA → Add device).

**Notes:**
- Releasing the button before the 5 s window cancels the reset and returns the LED to its previous state.
- The NVS configuration (channel types, timings, etc.) is **not** affected — only the Zigbee network credentials are erased.
- The first 10 s after power-up are ignored to avoid false triggers at boot.

---

## Firmware Architecture

### Per-device configuration via NVS

Device behaviour is configured at **first programming** using `configure.py`, which generates an NVS partition (`nvs_config.bin`) containing all parameters. The firmware reads this partition at boot — no recompilation needed to change a device's function.

NVS configuration survives firmware OTA updates. Re-running `configure.py` and reflashing only the NVS partition (address `0x9000`) is enough to change a device's behaviour.

### Zigbee

- Operates exclusively as a **Zigbee router**
- Exposes one endpoint per active channel
- **On/Off cluster (0x0006)** for relay and dimmer channels
- **Window Covering cluster (0x0102)** for roller shutter channels
- **OTA Upgrade cluster (0x0019)** for wireless firmware updates

---

## Getting Started

### Prerequisites

| Task | Tool required |
|------|---------------|
| Generate `nvs_config.bin` | Python 3 (no ESP-IDF needed) |
| Flash pre-built binaries | [esptool.py](https://github.com/espressif/esptool) |
| Build firmware from source | [ESP-IDF v5.1.2](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32h2/get-started/index.html) |
| Generate OTA image | Python 3 (no ESP-IDF needed) |

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

Use this method if you do **not** need to build from source.

```cmd
esptool.py --chip esp32h2 --port COM12 --baud 460800 write_flash --flash_mode dio --flash_freq 48m --flash_size 4MB 0x0000 binaries/bootloader.bin 0x8000 binaries/partition-table.bin 0x9000 nvs_config.bin 0x10000 binaries/ota_data_initial.bin 0x20000 binaries/smart_switch.bin
```

Replace `COM12` with your actual port (`/dev/ttyUSBx` on Linux/macOS).  
Flash `nvs_config.bin` alone to update the configuration without touching the firmware:

```cmd
esptool.py --port COM12 write_flash 0x9000 nvs_config.bin
```

---

### Step 2b — Build from source (optional)

Requires ESP-IDF v5.1.2. Run the following from the **ESP-IDF terminal**:

```cmd
cd smart_switch
idf.py set-target esp32h2
idf.py build
idf.py flash
esptool.py --port COM12 write_flash 0x9000 nvs_config.bin
```

> `idf.py flash` writes the firmware to the OTA slot (`0x20000`) but does **not** touch the NVS partition, so `nvs_config.bin` must be flashed separately.

---

### Step 3 — OTA firmware update

OTA updates replace the firmware without touching the NVS configuration.

**3a. Bump the firmware version**

Edit `smart_switch/main/ota.h` and increment `OTA_FILE_VERSION`:

```c
/* Format: 0xMMNNPPPP — MM=major, NN=minor, PPPP=patch (16-bit) */
#define OTA_FILE_VERSION  0x01030000   /* e.g. v1.3.0 */
#define OTA_SW_BUILD_ID   "\x06""v1.3.0"
```

**3b. Build and package the OTA image**

```cmd
idf.py build
python make_ota.py smart_switch
```

This generates `smart_switch-vX.Y.Z.ota` in the same folder as `make_ota.py`.

**3c. Distribute via ZHA (Home Assistant)**

1. Copy the `.ota` file to your ZHA OTA folder (usually `config/custom_components/zha/ota/` or as configured in `configuration.yaml`).
2. Restart Home Assistant (or reload the ZHA integration).
3. In ZHA → Device → Update, the new firmware will appear. Trigger the update from there.

---

## Repository Structure

```
Zigbee/
├── configure.py            — configuration wizard (generates nvs_config.csv + nvs_config.bin)
├── make_ota.py             — packages a build into a Zigbee OTA .ota image
├── binaries/               — pre-built binaries for direct flashing
│   ├── bootloader.bin
│   ├── partition-table.bin
│   ├── ota_data_initial.bin
│   └── smart_switch.bin
└── smart_switch/           — ESP-IDF firmware project
    ├── main/
    ├── CMakeLists.txt
    ├── partitions.csv
    └── sdkconfig.defaults
```

---

See [CHANGELOG.md](CHANGELOG.md) for version history.
