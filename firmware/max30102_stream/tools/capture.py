"""
Record the MAX30102 stream from the board to a CSV file.

Usage:
    python capture.py COM6 --seconds 120 --out data/recordings/jeffery_rest_01.csv

Keep your finger still on the sensor for the whole recording.
Needs: pip install pyserial
"""
import argparse
import csv
import time
from pathlib import Path

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="serial port of the board, e.g. COM6")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--out", default="recording.csv")
    args = ap.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    rows, status_lines, bad_lines = [], [], 0
    with serial.Serial(args.port, 115200, timeout=1) as ser:
        ser.reset_input_buffer()
        print(f"Recording {args.seconds:.0f} s from {args.port} ... keep your finger still")
        t_end = time.time() + args.seconds
        last_report = time.time()
        while time.time() < t_end:
            line = ser.readline().decode(errors="replace").strip()
            if not line:
                continue
            if line.startswith("#"):
                status_lines.append(line)
                print(line)
                continue
            try:
                seq, ir, red = (int(v) for v in line.split(","))
            except ValueError:
                bad_lines += 1
                continue
            rows.append((time.time(), seq, ir, red))
            if time.time() - last_report >= 5:
                print(f"  {len(rows):6d} samples   last IR = {ir}")
                last_report = time.time()

    with out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["host_time", "seq", "ir", "red"])
        w.writerows(rows)

    if not rows:
        print("No samples received. Is the board streaming? Open the port in a "
              "terminal to check for '# MAX30102 not found' messages.")
        return

    seqs = [r[1] for r in rows]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    duration = rows[-1][0] - rows[0][0]
    irs = [r[2] for r in rows]
    print(f"\nSaved {len(rows)} samples to {out}")
    print(f"Measured rate: {len(rows) / duration:.1f} Hz (should be about 100)")
    print(f"Gaps in sequence numbers: {gaps} (should be 0)")
    print(f"Unparseable lines: {bad_lines}")
    print(f"IR range: {min(irs)} to {max(irs)} (262143 means the ADC is saturated)")


if __name__ == "__main__":
    main()
