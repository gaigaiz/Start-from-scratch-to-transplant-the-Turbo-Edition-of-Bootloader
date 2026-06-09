"""Headless COM3 write/flow diagnostic. Harmless: writes a few junk bytes
the device will just ignore, then reads for unsolicited data."""
import time
import serial

PORT, BAUD = "COM3", 500000
PAYLOAD = bytes(range(32))


def try_write(tag, **kw):
    print(f"\n=== {tag}  open kw={kw} ===")
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.5, write_timeout=2, **kw)
    except Exception as e:                                  # noqa: BLE001
        print(f"  open FAILED: {e!r}")
        return
    try:
        print(f"  lines on open: cts={s.cts} dsr={s.dsr} "
              f"ri={s.ri} cd={s.cd} dtr={s.dtr} rts={s.rts}")
        t0 = time.time()
        try:
            n = s.write(PAYLOAD)
            print(f"  write({len(PAYLOAD)}) -> returned {n} in "
                  f"{(time.time()-t0)*1000:.1f} ms  [OK, TX works]")
        except serial.SerialTimeoutException:
            print(f"  write TIMED OUT after {(time.time()-t0):.2f}s "
                  f"[driver not transmitting in this mode]")
        # also try with DTR/RTS forced both ways
        for dtr in (True, False):
            for rts in (True, False):
                try:
                    s.dtr = dtr
                    s.rts = rts
                    time.sleep(0.05)
                    t0 = time.time()
                    s.write(PAYLOAD)
                    print(f"  dtr={dtr} rts={rts}: write OK "
                          f"({(time.time()-t0)*1000:.1f} ms)")
                except serial.SerialTimeoutException:
                    print(f"  dtr={dtr} rts={rts}: write TIMEOUT")
                except Exception as e:                       # noqa: BLE001
                    print(f"  dtr={dtr} rts={rts}: {e!r}")
        # listen for any unsolicited bytes from the device
        s.timeout = 0.3
        got = bytearray()
        end = time.time() + 2
        while time.time() < end:
            d = s.read(256)
            if d:
                got += d
        print(f"  read 2s: {len(got)} bytes" +
              (f" -> {bytes(got[:64]).hex(' ')}" if got else " (silent)"))
    finally:
        s.close()


if __name__ == "__main__":
    print(f"pyserial probe on {PORT}@{BAUD}")
    try_write("A: rtscts=F dsrdtr=F (current tool setting)",
              rtscts=False, dsrdtr=False)
    try_write("B: defaults (like a plain assistant)")
    try_write("C: rtscts=True (hardware flow)", rtscts=True)
