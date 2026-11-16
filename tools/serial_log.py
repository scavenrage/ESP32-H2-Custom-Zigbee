"""Shows the device serial output and saves it to a file (instead of "idf.py monitor").

Useful to capture complete logs and core dumps that can be decoded later.
Usage (from the firmware folder, in the ESP-IDF environment, needs pyserial):
    python ..\tools\serial_log.py COM16
Stop with Ctrl+C. Opening the port does not reset the device.
The file is created in tools\logs\ with date and time in its name.
"""

import datetime
import os
import sys

import serial


def main():
    if len(sys.argv) < 2:
        print("usage: python serial_log.py COMx [baud]")
        return 1
    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
    os.makedirs(out_dir, exist_ok=True)
    name = datetime.datetime.now().strftime("serial-%Y%m%d-%H%M%S.log")
    path = os.path.join(out_dir, name)

    # DTR/RTS set to false before opening: otherwise opening the port can reset the board
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.5
    ser.dtr = False
    ser.rts = False
    ser.open()

    print(f"Serial {port} @ {baud} -> {path}  (Ctrl+C to stop)")
    with open(path, "wb") as f:
        try:
            while True:
                data = ser.read(4096)
                if data:
                    f.write(data)
                    f.flush()
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            print(f"\nStopped. Log saved to {path}")
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
