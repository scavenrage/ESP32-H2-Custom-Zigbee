#!/usr/bin/env python3
"""
make_ota.py — Creates a Zigbee OTA Upgrade Image (.ota) from an ESP32 .bin file.

Usage:
    python make_ota.py smart_switch

The script:
  - reads the binary name from CMakeLists.txt (project(...))
  - reads OTA_FILE_VERSION, OTA_MANUFACTURER_CODE, OTA_IMAGE_TYPE from main/ota.h
  - saves smart_switch/smart_switch-vX.Y.Z.ota

Advanced usage (explicit paths):
    python make_ota.py smart_switch --bin path/to/fw.bin --version 0x00010003
"""

import struct
import argparse
import os
import re
import sys

OTA_ZIGBEE_STACK_VER        = 0x0002   # ZigBee Pro (r21)
OTA_FILE_TAG                = 0x0000   # Upgrade Image tag
OTA_UPGRADE_FILE_IDENTIFIER = 0x0BEEF11E
OTA_HEADER_VERSION          = 0x0100
OTA_FIELD_CONTROL           = 0x0000   # no optional fields

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------------
#  Read CMakeLists.txt — extract project name (→ binary name)
# ---------------------------------------------------------------------------

def read_project_name(cmake_path: str) -> str:
    with open(cmake_path, "r") as f:
        content = f.read()
    m = re.search(r"^\s*project\((\S+)\)", content, re.MULTILINE)
    if not m:
        raise RuntimeError(f"project() not found in {cmake_path}")
    return m.group(1)


# ---------------------------------------------------------------------------
#  Read ota.h — extract version, manufacturer code and image type
# ---------------------------------------------------------------------------

def read_ota_h(ota_h_path: str) -> dict:
    with open(ota_h_path, "r") as f:
        content = f.read()

    def extract_hex(name):
        m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+)", content)
        if not m:
            raise RuntimeError(f"{name} not found in {ota_h_path}")
        return int(m.group(1), 16)

    return {
        "file_version":      extract_hex("OTA_FILE_VERSION"),
        "manufacturer_code": extract_hex("OTA_MANUFACTURER_CODE"),
        "image_type":        extract_hex("OTA_IMAGE_TYPE"),
    }


# ---------------------------------------------------------------------------
#  Utilities
# ---------------------------------------------------------------------------

def version_str(v: int) -> str:
    """0x00010003 → 'v1.0.3'"""
    major = (v >> 24) & 0xFF
    minor = (v >> 16) & 0xFF
    patch =  v        & 0xFFFF
    return f"v{major}.{minor}.{patch}"


# ---------------------------------------------------------------------------
#  Build .ota file
# ---------------------------------------------------------------------------

def make_ota(bin_path: str, ota_path: str, file_version: int,
             manufacturer_code: int, image_type: int, project_name: str):

    with open(bin_path, "rb") as f:
        firmware = f.read()

    fw_size = len(firmware)

    # Header string: exactly 32 bytes, human-readable description
    raw_str = f"{project_name} OTA image".encode("ascii")
    header_str = raw_str[:32].ljust(32, b'\x00')

    # Sub-element: tag (2) + length (4) + firmware data
    sub_element = struct.pack("<HI", OTA_FILE_TAG, fw_size) + firmware

    # Fixed header: 56 bytes, no optional fields
    header_len = 56
    total_image_size = header_len + len(sub_element)

    header  = struct.pack("<I", OTA_UPGRADE_FILE_IDENTIFIER)
    header += struct.pack("<H", OTA_HEADER_VERSION)
    header += struct.pack("<H", header_len)
    header += struct.pack("<H", OTA_FIELD_CONTROL)
    header += struct.pack("<H", manufacturer_code)
    header += struct.pack("<H", image_type)
    header += struct.pack("<I", file_version)
    header += struct.pack("<H", OTA_ZIGBEE_STACK_VER)
    header += header_str
    header += struct.pack("<I", total_image_size)

    assert len(header) == header_len, f"Header size mismatch: {len(header)} != {header_len}"

    with open(ota_path, "wb") as f:
        f.write(header)
        f.write(sub_element)

    print(f"\nOTA file created: {ota_path}")
    print(f"  Firmware:       {bin_path}  ({fw_size} bytes)")
    print(f"  Version:        0x{file_version:08X}  ({version_str(file_version)})")
    print(f"  Manufacturer:   0x{manufacturer_code:04X}")
    print(f"  Image type:     0x{image_type:04X}")
    print(f"  OTA file size:  {total_image_size} bytes")


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Create a Zigbee OTA image from an ESP32 .bin file",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("project",
        help="Project folder name (e.g. smart_switch)")
    parser.add_argument("--bin", dest="bin_file", default=None,
        help="Explicit .bin path (default: auto from build/)")
    parser.add_argument("--version", default=None,
        help="Explicit version e.g. 0x00010003 (default: read from ota.h)")
    args = parser.parse_args()

    # --- Project folder ---
    project_dir = os.path.join(SCRIPT_DIR, args.project)
    if not os.path.isdir(project_dir):
        print(f"ERROR: project folder not found: {project_dir}", file=sys.stderr)
        sys.exit(1)

    # --- Binary name from CMakeLists.txt ---
    cmake_path = os.path.join(project_dir, "CMakeLists.txt")
    project_name = read_project_name(cmake_path)
    print(f"[auto] project: {args.project}  →  binary: {project_name}.bin")

    # --- .bin path ---
    if args.bin_file:
        bin_path = args.bin_file
    else:
        bin_path = os.path.join(project_dir, "build", f"{project_name}.bin")
        print(f"[auto] .bin: {bin_path}")

    if not os.path.isfile(bin_path):
        print(f"ERROR: file not found: {bin_path}", file=sys.stderr)
        print("       Did you run 'idf.py build' in the project folder?", file=sys.stderr)
        sys.exit(1)

    # --- Read ota.h ---
    ota_h_path = os.path.join(project_dir, "main", "ota.h")
    if not os.path.isfile(ota_h_path):
        print(f"ERROR: ota.h not found: {ota_h_path}", file=sys.stderr)
        sys.exit(1)

    ota_info = read_ota_h(ota_h_path)
    print(f"[auto] version from ota.h: 0x{ota_info['file_version']:08X}"
          f"  ({version_str(ota_info['file_version'])})")
    print(f"[auto] manufacturer: 0x{ota_info['manufacturer_code']:04X}"
          f"  image_type: 0x{ota_info['image_type']:04X}")

    # --- Version override ---
    if args.version:
        v = args.version
        file_version = int(v, 16) if v.startswith("0x") else int(v)
    else:
        file_version = ota_info["file_version"]

    # --- Output in project folder ---
    ota_filename = f"{project_name}-{version_str(file_version)}.ota"
    ota_path     = os.path.join(project_dir, ota_filename)

    make_ota(bin_path, ota_path, file_version,
             ota_info["manufacturer_code"], ota_info["image_type"],
             project_name)
