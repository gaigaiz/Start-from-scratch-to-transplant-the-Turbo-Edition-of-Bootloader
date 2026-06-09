"""COM3 loopback test. Jumper the WCH-Link serial TX <-> RX directly
(disconnect from the board first), then run this.

  PASS  -> WCH-Link serial bridge + driver + our code all round-trip.
           The fault is on the board side (wiring to PB10/PB11, GND,
           or the running APP not polling OTA).
  FAIL  -> WCH-Link serial itself isn't bridging RX. Its serial mode
           may need enabling (WCH-LinkUtility), or you're on the wrong
           probe pins. OTA can't work until loopback passes.
"""
import time
import serial

PORT, BAUD = "COM3", 500000
PATTERN = bytes(range(64)) + b"GD32-OTA-LOOPBACK-TEST\r\n"

s = serial.Serial(PORT, BAUD, timeout=1.5, write_timeout=3,
                   rtscts=False, dsrdtr=False)
s.reset_input_buffer()
print(f"{PORT}@{BAUD}: writing {len(PATTERN)} bytes, reading back...")
s.write(PATTERN)
time.sleep(0.2)
got = s.read(len(PATTERN) + 8)
s.close()

print(f"  sent {len(PATTERN)}  got {len(got)}")
if got == PATTERN:
    print("  PASS: exact round-trip. WCH-Link serial OK -> fault is "
          "board-side (TX/RX swap, GND, or APP not polling OTA).")
elif not got:
    print("  FAIL: 0 bytes back. WCH-Link RX not bridged. Enable its "
          "serial/UART mode (WCH-LinkUtility) or check probe TX/RX pins.")
else:
    print(f"  PARTIAL/garbled: {got!r}")
    print("  -> baud not honored by the WCH-Link serial, or noise.")
