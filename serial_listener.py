#!/usr/bin/env python3
"""
Captures DATA lines from the railway system serial output and saves them to a
timestamped CSV file.

Each output row: <sensor values...>
Output file:     data_<session_start_ms>.csv

Usage:
    python serial_listener.py [COM_PORT]

Requires pyserial:
    pip install pyserial
"""

import serial
import time
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = 115200

session_ts = int(time.time() * 1000)
output_file = f"data_{session_ts}.csv"

print(f"Opening {PORT} at {BAUD} baud.")
print(f"Writing DATA lines to: {output_file}")
print("Press Ctrl+C to stop.\n")

try:
    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        with open(output_file, "w", buffering=1) as f:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if line.startswith("DATA,"):
                    record = line[5:]
                    f.write(record + "\n")
                    print(record)
except KeyboardInterrupt:
    print(f"\nStopped. Data saved to {output_file}.")
except serial.SerialException as e:
    print(f"Serial error: {e}", file=sys.stderr)
    sys.exit(1)
