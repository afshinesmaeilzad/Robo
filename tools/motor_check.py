#!/usr/bin/env python3
"""Guided L293D wiring check.

Flash motor_check/motor_check.ino first, then run:

    python3 tools/motor_check.py

Put the robot on a box so the wheels spin free. The script drives each L293D
input on its own, asks what you saw, and prints what to fix at the end.
"""

import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing.  Install it with:  python3 -m pip install pyserial")

BAUD = 115200

STEPS = [
    ("1", "LEFT  IN1  (GPIO14 -> L293D pin 2)",  "left",  "forward"),
    ("2", "LEFT  IN2  (GPIO15 -> L293D pin 7)",  "left",  "backward"),
    ("3", "RIGHT IN3  (GPIO13 -> L293D pin 10)", "right", "forward"),
    ("4", "RIGHT IN4  (GPIO12 -> L293D pin 15)", "right", "backward"),
]


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    candidates = [p.device for p in list_ports.comports()
                  if "usbserial" in p.device or "usbmodem" in p.device or "wchusb" in p.device]
    if not candidates:
        sys.exit("No USB serial port found.  Plug in the ESP32-CAM, or pass the port:\n"
                 "    python3 tools/motor_check.py /dev/cu.usbserial-10")
    return candidates[0]


def ask(question, options):
    """Ask until the answer is one of options (single letters)."""
    prompt = "%s [%s] " % (question, "/".join(options))
    while True:
        answer = input(prompt).strip().lower()
        if answer in options:
            return answer
        print("   Please answer with one of: %s" % ", ".join(options))


def drain(ser, seconds):
    """Print whatever the board says for a while."""
    end = time.time() + seconds
    while time.time() < end:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            print("   | " + line)


def main():
    port = find_port()
    print("Opening %s at %d baud" % (port, BAUD))

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.4
    ser.dtr = False          # do not reset the board on open
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()

    print()
    print("Put the robot on a box so the wheels spin free.")
    input("Press Enter when ready... ")

    results = {}
    for key, label, wheel, direction in STEPS:
        print()
        print("=" * 60)
        print("Testing %s" % label)
        print("Expect: the %s wheel turns %s, the other wheel stays still." % (wheel, direction))
        print("=" * 60)
        ser.reset_input_buffer()
        ser.write(key.encode())
        drain(ser, 3.0)

        answer = ask("What happened?  correct / wrong way / other wheel / nothing",
                     ["c", "w", "o", "n"])
        results[key] = answer

    ser.write(b"s")
    ser.close()

    print()
    print("=" * 60)
    print("RESULT")
    print("=" * 60)

    if all(v == "n" for v in results.values()):
        print("Nothing moved at all.  Check, in this order:")
        print("  1. The grounds are joined: battery -, L293D pins 4/5/12/13, ESP32-CAM GND.")
        print("  2. L293D pin 16 (VCC1) and pin 8 (VCC2) are at +6V.")
        print("  3. L293D pins 1 and 9 (EN1/EN2) are tied to +6V, not left floating.")
        print("  4. The motors themselves: touch their wires straight to the battery.")
        return

    labels = {"c": "correct", "w": "turned the wrong way", "o": "moved the other wheel", "n": "did not move"}
    for key, label, wheel, direction in STEPS:
        print("  %s  ->  %s" % (label, labels[results[key]]))

    print()
    fixes = []
    if results["1"] == "w" or results["2"] == "w":
        fixes.append("Left wheel runs backwards: swap the LEFT motor's two wires "
                     "on the L293D (pins 3 and 6).")
    if results["3"] == "w" or results["4"] == "w":
        fixes.append("Right wheel runs backwards: swap the RIGHT motor's two wires "
                     "on the L293D (pins 11 and 14).")
    if "o" in results.values():
        fixes.append("Left and right are swapped: swap the motor pairs, "
                     "pins 3/6 with pins 11/14.")
    dead = [label for key, label, _, _ in STEPS if results[key] == "n"]
    if dead and len(dead) < 4:
        fixes.append("These inputs did nothing: %s.  Check those jumper wires, and the "
                     "enable pin for that side (pin 1 for the left, pin 9 for the right)."
                     % "; ".join(dead))

    if fixes:
        print("What to fix:")
        for f in fixes:
            print("  - " + f)
    else:
        print("All four inputs behaved correctly.  The L293D wiring is good.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
