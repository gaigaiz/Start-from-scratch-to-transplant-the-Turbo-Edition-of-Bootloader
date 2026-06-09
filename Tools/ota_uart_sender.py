#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GD32F470 BootLoader UART OTA 下发脚本。
帧头 20B: <IBBHIHHI  = magic,type,status,seq,offset,length,reserved,crc32
用法:
  python ota_uart_sender.py --port COM5 --baud 500000 --bin App.bin \
         --header-version v2 --chunk 256 --monitor-seconds 10
"""
import argparse, binascii, struct, sys, time
import serial

MAGIC      = 0x5AA5C33C
T_START, T_DATA, T_END, T_ACK, T_NACK = 0x01, 0x02, 0x03, 0x81, 0x82
IMG_TYPE_APP1 = 1
APP1_ADDR     = 0x0800D000
HDR_FMT  = "<IBBHIHHI"          # 20 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)

NACK_NAME = {1:"BAD_STATE",2:"BAD_CRC",3:"BAD_SEQ",4:"BAD_OFFSET",
             5:"BAD_LENGTH",6:"FLASH",7:"FINAL"}

def crc32(b):  return binascii.crc32(b) & 0xFFFFFFFF

def make_frame(ftype, seq, offset, payload, crc_override=None):
    length = len(payload)
    crc = crc_override if crc_override is not None else crc32(payload)
    hdr = struct.pack(HDR_FMT, MAGIC, ftype, 0, seq & 0xFFFF,
                       offset & 0xFFFFFFFF, length & 0xFFFF, 0, crc)
    return hdr + payload

def build_start(app_bytes, version, ver):
    size = len(app_bytes); acrc = crc32(app_bytes)
    if ver == "v1":
        return struct.pack("<IIII", MAGIC, version, size, acrc)
    body = struct.pack("<IIIIIIIII", MAGIC, version, size, acrc,
                        2, 40, APP1_ADDR, IMG_TYPE_APP1, 0)   # 36 bytes
    return body + struct.pack("<I", crc32(body))              # +header_crc32 = 40

def read_frame(ser, timeout):
    """读一个回包帧头（20B）。返回 (type,status,seq,offset) 或 None。
       期间把设备文本 PAUSE/RESUME 透出，交由调用方流控。"""
    ser.timeout = timeout
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        # 文本流控标记
        if buf.endswith(b"PAUSE\r\n"):  return ("PAUSE",)
        if buf.endswith(b"RESUME\r\n"): return ("RESUME",)
        # 帧同步：找 magic
        i = buf.find(struct.pack("<I", MAGIC))
        if i >= 0 and len(buf) - i >= HDR_SIZE:
            m,t,st,sq,off,ln,rv,cc = struct.unpack(HDR_FMT, buf[i:i+HDR_SIZE])
            return (t, st, sq, off)
        if len(buf) > 4096:
            del buf[:-64]
    return None

def wait_resume(ser):
    """收到 PAUSE 后阻塞等 RESUME。"""
    print("  [flow] PAUSE, waiting RESUME...")
    while True:
        r = read_frame(ser, 30)
        if r and r[0] == "RESUME":
            print("  [flow] RESUME")
            return
        if r is None:
            print("  [flow] resume timeout, continue anyway"); return

def send_with_ack(ser, frame, exp_seq, retries, timeout, tag):
    for attempt in range(1, retries + 1):
        ser.write(frame); ser.flush()
        r = read_frame(ser, timeout)
        if r is None:
            print(f"  {tag}: timeout ({attempt}/{retries})"); continue
        if r[0] == "PAUSE":
            wait_resume(ser);  ser.write(frame); ser.flush()
            r = read_frame(ser, timeout)
            if r is None or r[0] in ("PAUSE","RESUME"): continue
        if r[0] == "RESUME":
            continue
        t, st, sq, off = r
        if t == T_ACK:
            return off
        if t == T_NACK:
            print(f"  {tag}: NACK {NACK_NAME.get(st,st)} dev_off={off}")
            if st in (3,4):           # BAD_SEQ/BAD_OFFSET：按设备进度续传
                return off
        # 其它：重试
    raise RuntimeError(f"{tag}: failed after {retries} retries")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--bin",  required=True)
    ap.add_argument("--header-version", choices=["v1","v2"], default="v2")
    ap.add_argument("--app-version", type=lambda x:int(x,0), default=0x00010000)
    ap.add_argument("--chunk", type=int, default=256)
    ap.add_argument("--retries", type=int, default=8)
    ap.add_argument("--monitor-seconds", type=int, default=0)
    a = ap.parse_args()

    if a.chunk > 512: sys.exit("chunk must be <= 512")
    with open(a.bin, "rb") as f: app = f.read()
    size, acrc = len(app), crc32(app)
    print(f"bin={a.bin} size={size} crc32=0x{acrc:08X} chunk={a.chunk} hdr={a.header_version}")

    ser = serial.Serial(a.port, a.baud, timeout=1)
    time.sleep(0.2); ser.reset_input_buffer()

    # START（含 Flash 擦除，超时给足）
    sp = build_start(app, a.app_version, a.header_version)
    send_with_ack(ser, make_frame(T_START, 0, 0, sp), 0, a.retries, 15, "START")
    print("START acked, sending DATA...")

    # DATA
    seq, off = 1, 0
    while off < size:
        n = min(a.chunk, size - off)
        chunk = app[off:off+n]
        dev = send_with_ack(ser, make_frame(T_DATA, seq, off, chunk),
                             seq, a.retries, 3, f"DATA seq={seq}")
        if dev != off + n and dev <= size:
            off = dev
        else:
            off += n
        seq += 1
        if off % 8192 < a.chunk:
            print(f"  progress {off}/{size} ({off*100//size}%)")

    # END：length=0，crc32 字段携带整镜像 CRC
    send_with_ack(ser, make_frame(T_END, seq, size, b"", crc_override=acrc),
                  seq, a.retries, 10, "END")
    print("END acked. Device will reset into BootLoader and upgrade.")

    if a.monitor_seconds > 0:
        print(f"--- monitor {a.monitor_seconds}s ---")
        ser.timeout = 0.2
        end = time.time() + a.monitor_seconds
        while time.time() < end:
            d = ser.read(256)
            if d:
                sys.stdout.write(d.decode("ascii","replace")); sys.stdout.flush()
    ser.close()

if __name__ == "__main__":
    main()
