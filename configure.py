#!/usr/bin/env python3
"""
configure.py — Configuration wizard for smart_switch (ESP32-H2)

Usage:
    python configure.py

The wizard asks interactive questions and produces:
  1. nvs_config.csv  — NVS key table        (same folder as this script)
  2. nvs_config.bin  — NVS binary partition  (same folder, no ESP-IDF required)

First programming (one-time):
    esptool.py --port COMx write_flash 0x9000 nvs_config.bin   # flash the config

Subsequent OTA updates:
    - Update OTA_FILE_VERSION in ota.h
    - idf.py build
    - Distribute the binary via ZHA (no changes to NVS config)
"""

import os
import sys
import struct

# ── Output paths (same folder as this script) ─────────────────────────────────
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
CSV_PATH    = os.path.join(SCRIPT_DIR, "nvs_config.csv")
BIN_PATH    = os.path.join(SCRIPT_DIR, "nvs_config.bin")
NVS_SIZE    = "0x6000"   # must match partitions.csv

# ── Channel types ──────────────────────────────────────────────────────────────
CH_NONE           = 0
CH_RELAY_STABLE   = 1
CH_RELAY_IMPULSE  = 2
CH_SHUTTER        = 3
CH_DIMMER         = 4

CH_NAMES = {
    CH_NONE:          "Unused",
    CH_RELAY_STABLE:  "Stable relay (ON/OFF)",
    CH_RELAY_IMPULSE: "Impulse relay (digital pushbutton)",
    CH_SHUTTER:       "Roller shutter (uses this channel + the next one)",
    CH_DIMMER:        "PWM dimmer (channel 1 only)",
}

INPUT_BUTTON = 0
INPUT_SWITCH = 1

# ══════════════════════════════════════════════════════════════════════════════
#  UI helpers
# ══════════════════════════════════════════════════════════════════════════════

def title(text):
    print("\n" + "═" * 60)
    print(f"  {text}")
    print("═" * 60)

def section(text):
    print(f"\n── {text} ──────────────────────────────────────")

def ask(prompt, default=None):
    if default is not None:
        full = f"  {prompt} [{default}]: "
    else:
        full = f"  {prompt}: "
    while True:
        val = input(full).strip()
        if val == "" and default is not None:
            return str(default)
        if val:
            return val
        print("  ⚠ Answer required.")

def ask_int(prompt, default=None, min_val=None, max_val=None):
    while True:
        raw = ask(prompt, default)
        try:
            val = int(raw)
        except ValueError:
            print(f"  ⚠ Please enter an integer.")
            continue
        if min_val is not None and val < min_val:
            print(f"  ⚠ Minimum value: {min_val}")
            continue
        if max_val is not None and val > max_val:
            print(f"  ⚠ Maximum value: {max_val}")
            continue
        return val

def ask_choice(prompt, choices, default=None):
    print(f"\n  {prompt}")
    for i, (val, label) in enumerate(choices):
        marker = " ◀ default" if (default is not None and val == default) else ""
        print(f"    {i+1}) {label}{marker}")
    while True:
        raw = ask("Choice", default=None if default is None else
                  str(next(i+1 for i, (v,_) in enumerate(choices) if v == default)))
        try:
            idx = int(raw) - 1
            if 0 <= idx < len(choices):
                return choices[idx][0]
        except ValueError:
            pass
        print(f"  ⚠ Please choose a number between 1 and {len(choices)}.")

def ask_input_type(label="input"):
    return ask_choice(
        f"Input type for {label}:",
        [(INPUT_BUTTON, "Momentary pushbutton"),
         (INPUT_SWITCH, "Bistable switch")],
        default=INPUT_BUTTON
    )

def confirm(prompt, default=True):
    hint = "Y/n" if default else "y/N"
    raw = ask(f"{prompt} ({hint})", "y" if default else "n").lower()
    return raw in ("y", "yes", "")

# ══════════════════════════════════════════════════════════════════════════════
#  Configuration gathering
# ══════════════════════════════════════════════════════════════════════════════

def configure_channels(num_ch):
    channels = [None] * num_ch
    i = 0
    while i < num_ch:
        if channels[i] is not None and channels[i].get("blocked"):
            i += 1
            continue

        section(f"Channel {i+1} of {num_ch}")
        choices = []
        if i == 0:
            choices.append((CH_DIMMER, CH_NAMES[CH_DIMMER]))
        choices.append((CH_RELAY_STABLE,   CH_NAMES[CH_RELAY_STABLE]))
        choices.append((CH_RELAY_IMPULSE,  CH_NAMES[CH_RELAY_IMPULSE]))
        if i + 1 < num_ch and (i % 2 == 0):
            choices.append((CH_SHUTTER, CH_NAMES[CH_SHUTTER]))
        choices.append((CH_NONE, CH_NAMES[CH_NONE]))

        ch_type = ask_choice(f"Channel {i+1} type:", choices, default=CH_RELAY_STABLE)
        ch = {"type": ch_type, "blocked": False}

        if ch_type in (CH_RELAY_STABLE, CH_RELAY_IMPULSE):
            ch["input_type"] = ask_input_type(f"channel {i+1}")
        elif ch_type == CH_SHUTTER:
            channels[i+1] = {"type": CH_NONE, "blocked": True}
            ch["input_up_type"]   = ask_input_type(f"channel {i+1} UP (shutter)")
            ch["input_down_type"] = ask_input_type(f"channel {i+1} DOWN (shutter)")

        channels[i] = ch
        i += 1

    for i in range(num_ch):
        if channels[i] is None:
            channels[i] = {"type": CH_NONE, "blocked": False}
    return channels


def configure_relay_params(channels):
    has_impulse = any(c["type"] == CH_RELAY_IMPULSE for c in channels)
    has_relay   = any(c["type"] in (CH_RELAY_STABLE, CH_RELAY_IMPULSE) for c in channels)
    params = {}
    if has_relay:
        section("Relay / input parameters")
        params["debounce_ms"] = ask_int("Input debounce (ms)", default=200, min_val=10, max_val=2000)
    if has_impulse:
        params["impulse_ms"] = ask_int("Output impulse duration (ms)", default=300, min_val=50, max_val=5000)
    return params


def configure_shutter_params(channels):
    params = {}
    section("Global shutter parameters")
    params["interlock_ms"]   = ask_int("Motor direction change pause (ms)", default=500, min_val=100, max_val=3000)
    params["endstop_ext_ms"] = ask_int("End-stop extra margin (ms)", default=1500, min_val=0, max_val=5000)
    params["debounce_ms"]    = ask_int("Shutter input debounce (ms)", default=200, min_val=10, max_val=2000)

    for i, ch in enumerate(channels):
        if ch["type"] == CH_SHUTTER:
            label = "A" if i == 0 else "B"
            section(f"Travel times — Shutter {label} (channels {i+1}+{i+2})")
            print("  ⚠ Measure with a stopwatch!")
            params[f"sh_{label.lower()}_up_ms"] = ask_int(f"Shutter {label} up travel time (ms)", default=53000, min_val=1000)
            params[f"sh_{label.lower()}_dn_ms"] = ask_int(f"Shutter {label} down travel time (ms)", default=52000, min_val=1000)

    return params


def configure_dimmer_params():
    section("Dimmer parameters")
    params = {}
    params["pwm_freq_hz"]    = ask_int("PWM frequency (Hz)", default=1000, min_val=100, max_val=40000)
    params["pwm_resolution"] = ask_int("PWM resolution (bits, e.g. 13 = 0..8191)", default=13, min_val=8, max_val=14)
    params["fade_ms"]        = ask_int("Fade on/off duration (ms)", default=500, min_val=0, max_val=5000)
    params["long_press_ms"]  = ask_int("Long press threshold for dimming (ms)", default=500, min_val=200, max_val=3000)
    params["debounce_ms"]    = ask_int("Button debounce (ms)", default=50, min_val=10, max_val=500)
    params["dimming_step"]   = ask_int("Dimming step per long press (0-255)", default=5, min_val=1, max_val=50)
    params["step_ms"]        = ask_int("Interval between steps (ms)", default=50, min_val=10, max_val=500)
    params["default_level"]  = ask_int("Default level at power-on (0-254)", default=128, min_val=1, max_val=254)
    return params


# ══════════════════════════════════════════════════════════════════════════════
#  NVS CSV generation
# ══════════════════════════════════════════════════════════════════════════════

def write_nvs_csv(channels, relay_params, shutter_params, dimmer_params, num_ch):
    has_shutter = any(c["type"] == CH_SHUTTER for c in channels)
    has_dimmer  = any(c["type"] == CH_DIMMER  for c in channels)
    has_relay   = any(c["type"] in (CH_RELAY_STABLE, CH_RELAY_IMPULSE) for c in channels)

    rows = []
    R = rows.append   # shortcut

    # Mandatory CSV header for nvs_partition_gen.py
    R("key,type,encoding,value")

    # Namespace
    R("sw_cfg,namespace,,")

    # ── Channels ──
    R(f"num_ch,data,u8,{num_ch}")
    for i in range(4):
        R(f"ch{i}_type,data,u8,{channels[i]['type']}")

    for i in range(4):
        t = channels[i]["type"]
        if t in (CH_RELAY_STABLE, CH_RELAY_IMPULSE):
            R(f"ch{i}_in,data,u8,{channels[i].get('input_type', INPUT_BUTTON)}")
        else:
            R(f"ch{i}_in,data,u8,{INPUT_BUTTON}")

    # Shutter inputs
    for i in [0, 2]:
        if channels[i]["type"] == CH_SHUTTER:
            R(f"ch{i}_in_up,data,u8,{channels[i].get('input_up_type',   INPUT_BUTTON)}")
            R(f"ch{i}_in_dn,data,u8,{channels[i].get('input_down_type', INPUT_BUTTON)}")
        else:
            R(f"ch{i}_in_up,data,u8,{INPUT_BUTTON}")
            R(f"ch{i}_in_dn,data,u8,{INPUT_BUTTON}")

    # ── Relay parameters ──
    R(f"rel_dbnce,data,u16,{relay_params.get('debounce_ms', 200)}")
    R(f"rel_imp_ms,data,u16,{relay_params.get('impulse_ms', 300)}")

    # ── Shutter parameters ──
    R(f"sh_ilock,data,u16,{shutter_params.get('interlock_ms', 500)}")
    R(f"sh_end_ext,data,u16,{shutter_params.get('endstop_ext_ms', 2000)}")
    R(f"sh_dbnce,data,u16,{shutter_params.get('debounce_ms', 50)}")
    R(f"sh_a_up_ms,data,u32,{shutter_params.get('sh_a_up_ms', 53000)}")
    R(f"sh_a_dn_ms,data,u32,{shutter_params.get('sh_a_dn_ms', 52000)}")
    R(f"sh_b_up_ms,data,u32,{shutter_params.get('sh_b_up_ms', 53000)}")
    R(f"sh_b_dn_ms,data,u32,{shutter_params.get('sh_b_dn_ms', 52000)}")

    # ── Dimmer parameters ──
    R(f"dim_freq,data,u32,{dimmer_params.get('pwm_freq_hz', 1000)}")
    R(f"dim_res,data,u8,{dimmer_params.get('pwm_resolution', 13)}")
    R(f"dim_fade,data,u16,{dimmer_params.get('fade_ms', 500)}")
    R(f"dim_lp_ms,data,u16,{dimmer_params.get('long_press_ms', 500)}")
    R(f"dim_dbnce,data,u16,{dimmer_params.get('debounce_ms', 50)}")
    R(f"dim_step,data,u8,{dimmer_params.get('dimming_step', 5)}")
    R(f"dim_step_ms,data,u16,{dimmer_params.get('step_ms', 50)}")
    R(f"dim_def_lvl,data,u8,{dimmer_params.get('default_level', 128)}")
    # Touch button = active-high (1) if dimmer, active-low (0) otherwise
    R(f"dim_btn_lvl,data,u8,{1 if has_dimmer else 0}")

    content = "\n".join(rows) + "\n"
    with open(CSV_PATH, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"\n  ✓ Written: {CSV_PATH}")


# ══════════════════════════════════════════════════════════════════════════════
#  Pure-Python NVS binary writer
#  Implements the ESP-IDF NVS partition format directly, so no IDF is required.
#  Reference: esp-idf/components/nvs_flash/src/nvs_page.hpp
# ══════════════════════════════════════════════════════════════════════════════

_NVS_TYPE_U8  = 0x01
_NVS_TYPE_U16 = 0x02
_NVS_TYPE_U32 = 0x04

_NVS_PAGE_SIZE        = 4096
_NVS_PAGE_HEADER_SIZE = 32    # bytes
_NVS_ENTRY_TABLE_SIZE = 32    # bytes (2 bits × 128 slots)
_NVS_ENTRY_SIZE       = 32    # bytes per entry
_NVS_MAX_ENTRIES      = 126   # usable entries per page
_NVS_STATE_FULL       = 0xFFFFFFFC   # page fully written (matches IDF nvs_partition_gen output)

_NVS_PARTITION_SIZE   = 0x6000   # must match partitions.csv (6 pages)
_NVS_ENTRY_DATA_OFF   = _NVS_PAGE_HEADER_SIZE + _NVS_ENTRY_TABLE_SIZE  # = 64

_NVS_ENC_MAP = {"u8": _NVS_TYPE_U8, "u16": _NVS_TYPE_U16, "u32": _NVS_TYPE_U32}


def _nvs_crc32(data):
    """CRC32 as used by ESP-IDF NVS: reflected poly=0xEDB88320, init=0, final XOR=0xFFFFFFFF."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if (crc & 1) else (crc >> 1)
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def _nvs_entry(ns_idx, etype, key, value_bytes):
    """
    Build a single 32-byte NVS entry.
      ns_idx      – namespace index (0 for namespace entries themselves)
      etype       – _NVS_TYPE_U8 / U16 / U32
      key         – str, max 15 chars (null-terminated in 16-byte field)
      value_bytes – exactly 8 bytes (unused bytes must be 0xFF)
    """
    assert len(value_bytes) == 8, "value_bytes must be 8 bytes"
    key_b = key.encode("ascii")[:15].ljust(16, b"\x00")

    entry = bytearray(b"\xFF" * 32)
    entry[0] = ns_idx
    entry[1] = etype
    entry[2] = 1       # span = 1 (single entry)
    entry[3] = 0xFF    # chunk_idx = 0xFF (unused for primitives)
    # bytes[4:8]  – CRC32 (computed below)
    entry[8:24]  = key_b
    entry[24:32] = value_bytes

    crc = _nvs_crc32(bytes(entry[0:4]) + bytes(entry[8:32]))
    struct.pack_into("<I", entry, 4, crc)
    return bytes(entry)


def _nvs_namespace_entry(ns_name, ns_idx):
    """Namespace entry: ns_idx=0, type=u8, key=ns_name, value=ns_idx (rest=0xFF)."""
    vb = bytearray(b"\xFF" * 8)
    vb[0] = ns_idx
    return _nvs_entry(0, _NVS_TYPE_U8, ns_name, bytes(vb))


def _nvs_data_entry(ns_idx, etype, key, value):
    """Data entry for u8, u16, or u32 (unused value bytes padded with 0xFF)."""
    vb = bytearray(b"\xFF" * 8)
    if   etype == _NVS_TYPE_U8:  vb[0] = value & 0xFF
    elif etype == _NVS_TYPE_U16: struct.pack_into("<H", vb, 0, value & 0xFFFF)
    elif etype == _NVS_TYPE_U32: struct.pack_into("<I", vb, 0, value & 0xFFFFFFFF)
    return _nvs_entry(ns_idx, etype, key, bytes(vb))


def _nvs_build_page(entries):
    """
    Assemble a 4096-byte NVS page (ACTIVE state) from a list of 32-byte entry
    bytestrings.  Returns bytes.
    """
    assert len(entries) <= _NVS_MAX_ENTRIES, "Too many entries for one page"

    page = bytearray(b"\xFF" * _NVS_PAGE_SIZE)

    # ── Page header ──────────────────────────────────────────────────────────
    struct.pack_into("<I", page, 0, _NVS_STATE_FULL)     # state: FULL (all entries written)
    struct.pack_into("<I", page, 4, 0x00000000)           # seq_num
    page[8] = 0xFE                                        # NVS version
    # bytes[9:28] = 0xFF (reserved, already set)
    hdr_crc = _nvs_crc32(bytes(page[4:28]))
    struct.pack_into("<I", page, 28, hdr_crc)

    # ── Entries + entry-state table ──────────────────────────────────────────
    # Entry table starts at offset 32.  Each entry uses 2 bits:
    #   11 = empty (default), 10 = written.
    # For entry i: table byte = 32 + i//4, bit = (i%4)*2  (clear to mark written)
    for i, entry in enumerate(entries):
        off = _NVS_ENTRY_DATA_OFF + i * _NVS_ENTRY_SIZE
        page[off : off + _NVS_ENTRY_SIZE] = entry
        # Mark as WRITTEN: clear the LSB of this entry's 2-bit slot
        tbl_byte = _NVS_PAGE_HEADER_SIZE + i // 4
        bit_pos  = (i % 4) * 2
        page[tbl_byte] &= ~(1 << bit_pos)

    return bytes(page)


def generate_nvs_bin():
    """Generate nvs_config.bin directly from nvs_config.csv — no ESP-IDF required."""

    # ── Parse CSV ─────────────────────────────────────────────────────────────
    try:
        with open(CSV_PATH, "r", encoding="utf-8") as f:
            lines = [l.strip() for l in f if l.strip()]
    except FileNotFoundError:
        print(f"  ⚠ CSV not found: {CSV_PATH}")
        return False

    page_entries    = []
    current_ns_idx  = 0

    for line in lines[1:]:          # skip header row
        parts = line.split(",")
        if len(parts) < 3:
            continue
        key  = parts[0]
        kind = parts[1]
        enc  = parts[2]
        val  = parts[3] if len(parts) > 3 else ""

        if kind == "namespace":
            current_ns_idx += 1
            page_entries.append(_nvs_namespace_entry(key, current_ns_idx))
        elif kind == "data":
            etype = _NVS_ENC_MAP.get(enc)
            if etype is None:
                print(f"  ⚠ Unknown type '{enc}' for key '{key}' — skipped")
                continue
            try:
                value = int(val)
            except ValueError:
                print(f"  ⚠ Cannot parse value '{val}' for key '{key}' — skipped")
                continue
            page_entries.append(_nvs_data_entry(current_ns_idx, etype, key, value))

    if not page_entries:
        print("  ⚠ No entries found in CSV.")
        return False

    # ── Assemble partition: page 0 ACTIVE, pages 1-5 all 0xFF ────────────────
    page0     = _nvs_build_page(page_entries)
    partition = page0 + bytes(b"\xFF" * (_NVS_PARTITION_SIZE - _NVS_PAGE_SIZE))
    assert len(partition) == _NVS_PARTITION_SIZE

    with open(BIN_PATH, "wb") as f:
        f.write(partition)

    print(f"  ✓ Generated: {BIN_PATH}  ({len(page_entries)} entries)")
    return True


# ══════════════════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════════════════

def main():
    title("smart_switch — Configuration wizard")
    print()
    print("  This wizard configures the firmware before first programming.")
    print("  Answer the questions; press ENTER to accept the default value.")
    print()
    print("  Fixed GPIOs:")
    print("    Outputs: CH1=GPIO4, CH2=GPIO5, CH3=GPIO10, CH4=GPIO11")
    print("    Inputs:  CH1=GPIO1, CH2=GPIO0, CH3=GPIO3,  CH4=GPIO2")
    print("    Status LED: GPIO22")

    section("Output channels")
    num_ch = ask_int("How many output channels do you want to use? (1-4)", default=4, min_val=1, max_val=4)

    channels = configure_channels(num_ch)
    while len(channels) < 4:
        channels.append({"type": CH_NONE, "blocked": False})

    has_relay   = any(c["type"] in (CH_RELAY_STABLE, CH_RELAY_IMPULSE) for c in channels)
    has_shutter = any(c["type"] == CH_SHUTTER for c in channels)
    has_dimmer  = any(c["type"] == CH_DIMMER  for c in channels)

    relay_params   = configure_relay_params(channels)   if has_relay   else {}
    shutter_params = configure_shutter_params(channels) if has_shutter else {}
    dimmer_params  = configure_dimmer_params()           if has_dimmer  else {}

    title("Configuration summary")
    for i, ch in enumerate(channels[:num_ch]):
        if ch.get("blocked"):
            print(f"  CH{i+1}: (used by shutter)")
        else:
            print(f"  CH{i+1}: {CH_NAMES.get(ch['type'], '?')}")
    print()

    if not confirm("Write nvs_config.csv and generate NVS binary?", default=True):
        print("\n  Cancelled. No files modified.\n")
        sys.exit(0)

    write_nvs_csv(channels[:4], relay_params, shutter_params, dimmer_params, num_ch)
    bin_ok = generate_nvs_bin()

    title("Done!")
    print("  ┌─ First programming (one-time) ────────────────────────────┐")
    print("  │  idf.py set-target esp32h2   (first time only)            │")
    print("  │  idf.py build                                              │")
    print("  │  idf.py flash                # flash the app              │")
    print("  │  esptool.py --port COMx write_flash 0x9000 nvs_config.bin │")
    print("  └────────────────────────────────────────────────────────────┘")
    print()
    print("  ┌─ Subsequent OTA updates ───────────────────────────────────┐")
    print("  │  (no changes to NVS!)                                      │")
    print("  │  1. Update OTA_FILE_VERSION in ota.h                       │")
    print("  │  2. idf.py build                                           │")
    print("  │  3. Distribute the .bin via ZHA/OTA server                 │")
    print("  └────────────────────────────────────────────────────────────┘")
    print()
    print()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n  Cancelled. No files modified.\n")
        sys.exit(0)
