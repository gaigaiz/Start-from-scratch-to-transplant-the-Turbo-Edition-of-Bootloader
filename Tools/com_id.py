"""Identify every COM port (description / VID:PID) and test writeability.
A real USB-TTL completes write() in <10 ms; a ghost/Bluetooth port times
out (~2 s). Harmless: writes 8 junk bytes the device ignores."""
import time
import serial
import serial.tools.list_ports

print("=== enumerated ports ===")
for p in serial.tools.list_ports.comports():
    print(f"  {p.device:7} | {p.description} | hwid={p.hwid}")

for port in ("COM3", "COM11", "COM12", "COM6"):
    print(f"\n=== write test {port}@500000 ===")
    try:
        s = serial.Serial(port, 500000, timeout=0.3, write_timeout=2)
    except Exception as e:                                  # noqa: BLE001
        print(f"  open FAILED: {e!r}")
        continue
    try:
        t0 = time.time()
        try:
            s.write(b"\x00\x01\x02\x03\x04\x05\x06\x07")
            dt = (time.time() - t0) * 1000
            verdict = "REAL adapter (TX works)" if dt < 50 else "slow/suspect"
            print(f"  write OK in {dt:.1f} ms -> {verdict}")
        except serial.SerialTimeoutException:
            print("  write TIMEOUT 2s -> GHOST/Bluetooth port, NOT a USB-TTL")
    finally:
        s.close()
