#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GD32F470 BootLoader OTA upper-computer (flash + monitor, Tkinter GUI).

One window does the full OTA closed loop:
  - pick .bin or .hex (Intel HEX auto-converted to a raw image)
  - send START / DATA / END over USART3 (default 500000-8N1)
  - live progress bar + speed/ETA + ACK/NACK/retransmit/timeout stats
  - parse device ACK/NACK frames and BL text logs ("BL: update OK/FAIL" ...)
  - optional 2nd port to tail the APP USART1 banner ("APP vX.Y running")

Frame header 20B: <IBBHIHHI = magic,type,status,seq,offset,length,reserved,crc32
Protocol mirrors Common/ota_protocol.h and Tools/ota_uart_sender.py.

Run (this machine, conda base has pyserial + tkinter):
  "E:/path/python/miniforge3/python.exe" Tools/ota_gui.py
"""
import binascii
import os
import queue
import struct
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

# ---------------- protocol constants (keep in sync with ota_protocol.h) ----
MAGIC = 0x5AA5C33C
T_START, T_DATA, T_END, T_ACK, T_NACK = 0x01, 0x02, 0x03, 0x81, 0x82
IMG_TYPE_APP1 = 1
APP1_ADDR = 0x08014000
HDR_FMT = "<IBBHIHHI"          # 20 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)
NACK_NAME = {1: "BAD_STATE", 2: "BAD_CRC", 3: "BAD_SEQ", 4: "BAD_OFFSET",
             5: "BAD_LENGTH", 6: "FLASH", 7: "FINAL"}


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def make_frame(ftype, seq, offset, payload, crc_override=None):
    length = len(payload)
    crc = crc_override if crc_override is not None else crc32(payload)
    hdr = struct.pack(HDR_FMT, MAGIC, ftype, 0, seq & 0xFFFF,
                      offset & 0xFFFFFFFF, length & 0xFFFF, 0, crc)
    return hdr + payload


def build_start(app_bytes, version, ver):
    size = len(app_bytes)
    acrc = crc32(app_bytes)
    if ver == "v1":
        return struct.pack("<IIII", MAGIC, version, size, acrc)
    body = struct.pack("<IIIIIIIII", MAGIC, version, size, acrc,
                       2, 40, APP1_ADDR, IMG_TYPE_APP1, 0)   # 36 bytes
    return body + struct.pack("<I", crc32(body))              # +header_crc32 = 40


def load_image(path):
    """Return raw firmware bytes. Accept raw .bin or Intel .hex (auto convert).

    For .hex: flatten all data records by absolute address, take min addr as
    image base, fill gaps with 0xFF up to max addr -- identical to what
    `fromelf --bin` / `objcopy -O binary` produce for a contiguous app.
    """
    ext = os.path.splitext(path)[1].lower()
    with open(path, "rb") as f:
        data = f.read()
    if ext != ".hex":
        return data, None
    mem = {}
    ext_base = 0
    for raw in data.split(b"\n"):
        line = raw.strip()
        if not line or line[:1] != b":":
            continue
        rec = bytes.fromhex(line[1:].decode("ascii"))
        ln, addr_hi, addr_lo, rtype = rec[0], rec[1], rec[2], rec[3]
        payload = rec[4:4 + ln]
        if rtype == 0x00:                                # data
            base = ext_base + (addr_hi << 8) + addr_lo
            for i, byte in enumerate(payload):
                mem[base + i] = byte
        elif rtype == 0x02:                              # ext segment addr
            ext_base = (payload[0] << 8 | payload[1]) << 4
        elif rtype == 0x04:                              # ext linear addr
            ext_base = (payload[0] << 8 | payload[1]) << 16
        elif rtype == 0x01:                              # EOF
            break
    if not mem:
        raise ValueError("HEX 文件没有数据记录")
    lo, hi = min(mem), max(mem)
    buf = bytearray(b"\xFF" * (hi - lo + 1))
    for a, v in mem.items():
        buf[a - lo] = v
    return bytes(buf), lo


# ---------------- OTA worker (runs off the UI thread) ----------------------
class OtaWorker(threading.Thread):
    def __init__(self, cfg, q, stop_evt):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.q = q          # messages -> UI
        self.stop = stop_evt
        self.ser = None     # live serial handle (for force-close button)

    def close_port(self):
        """Force-release the serial port (called by the 关闭串口 button).
        Closing under a blocked read/write makes the worker error out and
        the finally in run() runs -- so the COM is always freed."""
        s = self.ser
        if s is not None:
            try:
                s.close()
            except Exception:                               # noqa: BLE001
                pass

    # -- helpers that push UI messages --
    def log(self, text, tag="info"):
        self.q.put(("log", text, tag))

    def stat(self, **kw):
        self.q.put(("stat", kw))

    def progress(self, off, size, speed, eta):
        self.q.put(("progress", off, size, speed, eta))

    # -- read one reply header (or text flow marker) --
    def read_frame(self, ser, timeout):
        ser.timeout = 0.05
        buf = bytearray()
        end = time.time() + timeout
        text = bytearray()
        magic_bytes = struct.pack("<I", MAGIC)
        while time.time() < end:
            if self.stop.is_set():
                return ("STOP",)
            b = ser.read(1)
            if not b:
                continue
            buf += b
            text += b
            # surface any complete text line (BL logs, etc.)
            if text.endswith(b"\n"):
                line = text.decode("ascii", "replace").strip()
                if line:
                    self._emit_text_line(line)
                text.clear()
            if buf.endswith(b"PAUSE\r\n"):
                return ("PAUSE",)
            if buf.endswith(b"RESUME\r\n"):
                return ("RESUME",)
            i = buf.find(magic_bytes)
            if i >= 0 and len(buf) - i >= HDR_SIZE:
                m, t, st, sq, off, ln, rv, cc = struct.unpack(
                    HDR_FMT, buf[i:i + HDR_SIZE])
                return (t, st, sq, off)
            if len(buf) > 4096:
                del buf[:-64]
        # timeout: hand back whatever raw bytes arrived so the caller can
        # tell "zero bytes (wiring/wrong COM)" from "garbage (baud mismatch)"
        # from "valid reply we failed to frame".
        return ("TIMEOUT", bytes(buf[-64:]))

    def _emit_text_line(self, line):
        tag = "bl"
        low = line.lower()
        if "update ok" in low or "jumping to app" in low:
            tag = "ok"
        elif "fail" in low or "no valid app" in low or "halt" in low:
            tag = "nack"
        self.log("  [dev] " + line, tag)

    def wait_resume(self, ser):
        self.log("  [flow] PAUSE, waiting RESUME...", "info")
        while not self.stop.is_set():
            r = self.read_frame(ser, 30)
            if r and r[0] == "RESUME":
                self.log("  [flow] RESUME", "info")
                return
            if r is None or r[0] == "TIMEOUT":
                self.log("  [flow] resume timeout, continue anyway", "info")
                return

    def send_with_ack(self, ser, frame, retries, timeout, tag):
        for attempt in range(1, retries + 1):
            if self.stop.is_set():
                raise RuntimeError("用户停止")
            ser.write(frame)   # no flush(): FlushFileBuffers stalls on CH340
            r = self.read_frame(ser, timeout)
            if r is None or r[0] == "TIMEOUT":
                self.stat(timeout=1)
                raw = r[1] if r and r[0] == "TIMEOUT" else b""
                if not raw:
                    why = "  OTA口零字节 → 接线/选错COM/设备没发(查 PB10→PC_RX、GND)"
                elif raw.find(struct.pack("<I", MAGIC)) < 0:
                    why = (f"  收到 {len(raw)}B 但无帧头 → 波特率不符/乱码: "
                           f"{raw.hex(' ')}")
                else:
                    why = f"  收到疑似帧但解析失败: {raw.hex(' ')}"
                self.log(f"  {tag}: timeout ({attempt}/{retries}){why}", "err")
                continue
            if r[0] == "STOP":
                raise RuntimeError("用户停止")
            if r[0] == "PAUSE":
                self.wait_resume(ser)
                ser.write(frame)
                r = self.read_frame(ser, timeout)
                if r is None or r[0] in ("PAUSE", "RESUME", "STOP", "TIMEOUT"):
                    continue
            if r[0] == "RESUME":
                continue
            t, st, sq, off = r
            if t == T_ACK:
                self.stat(ack=1)
                return off
            if t == T_NACK:
                name = NACK_NAME.get(st, str(st))
                self.stat(nack=1, nack_name=name)
                self.log(f"  {tag}: NACK {name} dev_off={off}", "nack")
                if st in (3, 4):              # BAD_SEQ / BAD_OFFSET -> resync
                    return off
            if attempt < retries:
                self.stat(retx=1)
        raise RuntimeError(f"{tag}: {retries} 次重试后仍失败")

    def run(self):
        c = self.cfg
        try:
            app, base = load_image(c["bin"])
            size, acrc = len(app), crc32(app)
            if base is not None and base != APP1_ADDR:
                self.log(f"提示: HEX 起始地址 0x{base:08X} != APP1 "
                         f"0x{APP1_ADDR:08X}，请确认固件链接基址", "err")
            self.log(f"镜像 {os.path.basename(c['bin'])}  size={size}  "
                     f"crc32=0x{acrc:08X}  chunk={c['chunk']}  "
                     f"hdr={c['hdr']}", "info")
            self.q.put(("state", "running"))

            # Behave like a plain serial assistant: no write_timeout and no
            # flush() -- on CH340-class drivers a small write_timeout false-
            # fires and FlushFileBuffers stalls, even on a healthy port.
            # rtscts/dsrdtr off so a USB-TTL whose DTR/RTS is wired to the
            # board RESET/BOOT pin can't hold the MCU in reset.
            # A real USB-TTL completes write() in <1 ms; only a ghost /
            # com0com / unbridged-debugger VCP stalls. So a generous
            # write_timeout aborts a dead port fast without ever tripping
            # on real transfers (frames are tiny).
            ser = serial.Serial(c["port"], c["baud"], timeout=1,
                                 write_timeout=4, rtscts=False, dsrdtr=False)
            self.ser = ser
            self.log(f"OTA口已打开 {c['port']}@{c['baud']} "
                     f"(DTR/RTS off, write_timeout=4s)", "info")
            time.sleep(0.2)
            ser.reset_input_buffer()
            # pre-flight: prove the port can actually transmit before we
            # commit to a 2-minute START retry loop on a dead handle.
            try:
                tw = time.time()
                ser.write(b"\x00")
                dtw = (time.time() - tw) * 1000
            except serial.SerialTimeoutException:
                raise RuntimeError(
                    f"{c['port']} 发送自检失败(4s写不出去)：这个口不是真正"
                    f"接到 USART3 的串口(幽灵口/未桥接的调试器VCP/com0com)。"
                    f"换正确的 USB-TTL 口") from None
            self.log(f"发送自检 OK ({dtw:.1f} ms)", "ok")
            ser.reset_input_buffer()
            t0 = time.time()

            self.log("== START (含 Flash 擦除，等待设备 ACK) ==", "info")
            sp = build_start(app, c["appver"], c["hdr"])
            self.send_with_ack(ser, make_frame(T_START, 0, 0, sp),
                               c["retries"], 15, "START")
            self.log("START 已确认，开始发送 DATA...", "ok")

            seq, off = 1, 0
            last_t, last_off = time.time(), 0
            while off < size:
                if self.stop.is_set():
                    raise RuntimeError("用户停止")
                n = min(c["chunk"], size - off)
                chunk = app[off:off + n]
                dev = self.send_with_ack(
                    ser, make_frame(T_DATA, seq, off, chunk),
                    c["retries"], 3, f"DATA seq={seq}")
                self.stat(data=1)
                off = dev if (dev != off + n and dev <= size) else off + n
                seq += 1
                now = time.time()
                if now - last_t >= 0.3 or off >= size:
                    sp_bps = (off - last_off) / max(now - last_t, 1e-3)
                    eta = (size - off) / sp_bps if sp_bps > 1 else 0
                    self.progress(off, size, sp_bps, eta)
                    last_t, last_off = now, off

            self.log("== END (length=0, crc32 字段携带整镜像 CRC) ==", "info")
            self.send_with_ack(
                ser, make_frame(T_END, seq, size, b"", crc_override=acrc),
                c["retries"], 10, "END")
            dt = time.time() - t0
            self.progress(size, size, 0, 0)
            self.log(f"END 已确认，传输耗时 {dt:.1f}s "
                     f"(均速 {size / dt / 1024:.1f} KB/s)。"
                     f"设备将复位进入 BootLoader 执行升级。", "ok")

            mon = c["monitor"]
            if mon > 0:
                self.log(f"--- 监测 BootLoader 升级流程 {mon}s ---", "info")
                ser.timeout = 0.2
                end = time.time() + mon
                text = bytearray()
                while time.time() < end and not self.stop.is_set():
                    d = ser.read(256)
                    if not d:
                        continue
                    text += d
                    while b"\n" in text:
                        line, _, rest = text.partition(b"\n")
                        text = bytearray(rest)
                        s = line.decode("ascii", "replace").strip()
                        if s:
                            self._emit_text_line(s)
            self.log("OTA 流程结束。", "ok")
            self.q.put(("state", "done"))
        except serial.SerialTimeoutException:
            self.log("错误: 写 OTA 口超时 → 这个 COM 枚举到了但没真正连通"
                     "(线松/拔了/选错口/未桥接的调试器VCP)。换正确的 USART3 "
                     "适配器 COM 再试。", "err")
            self.q.put(("state", "error"))
        except Exception as e:                              # noqa: BLE001
            self.log(f"错误: {e}", "err")
            self.q.put(("state", "error"))
        finally:
            if self.ser is not None:
                try:
                    self.ser.close()
                except Exception:                           # noqa: BLE001
                    pass
                self.ser = None
                self.q.put(("log", "OTA口已关闭。", "info"))


# ---------------- optional APP-banner tail (USART1) ------------------------
class BannerTail(threading.Thread):
    def __init__(self, port, baud, q, stop_evt):
        super().__init__(daemon=True)
        self.port, self.baud, self.q, self.stop = port, baud, q, stop_evt

    def run(self):
        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.3)
        except Exception as e:                              # noqa: BLE001
            self.q.put(("log", f"横幅口打开失败: {e}", "err"))
            return
        self.q.put(("log", f"--- 监听 APP 横幅口 {self.port}@{self.baud} ---",
                    "info"))
        text = bytearray()
        while not self.stop.is_set():
            d = ser.read(128)
            if not d:
                continue
            text += d
            while b"\n" in text:
                line, _, rest = text.partition(b"\n")
                text = bytearray(rest)
                s = line.decode("ascii", "replace").strip()
                if s:
                    self.q.put(("log", "  [APP] " + s, "banner"))
        ser.close()


# ---------------- Tkinter UI ----------------------------------------------
class App:
    def __init__(self, root):
        self.root = root
        root.title("STM32 OTA 上位机 — 烧录 + 监测")
        root.geometry("960x680")
        self.q = queue.Queue()
        self.stop_evt = threading.Event()
        self.worker = None
        self.banner = None
        self._build()
        self._refresh_ports()
        self.root.after(60, self._pump)

    # -- layout --
    def _build(self):
        pad = dict(padx=4, pady=3)
        top = ttk.LabelFrame(self.root, text="连接 & 固件")
        top.pack(fill="x", padx=8, pady=6)

        ttk.Label(top, text="OTA串口(USART3)").grid(row=0, column=0, **pad)
        self.cb_port = ttk.Combobox(top, width=10, state="readonly")
        self.cb_port.grid(row=0, column=1, **pad)
        ttk.Button(top, text="刷新", width=5,
                   command=self._refresh_ports).grid(row=0, column=2, **pad)

        ttk.Label(top, text="波特率").grid(row=0, column=3, **pad)
        self.e_baud = ttk.Entry(top, width=8)
        self.e_baud.insert(0, "500000")
        self.e_baud.grid(row=0, column=4, **pad)

        ttk.Label(top, text="固件(.bin/.hex)").grid(row=1, column=0, **pad)
        self.e_bin = ttk.Entry(top, width=58)
        self.e_bin.grid(row=1, column=1, columnspan=4, sticky="we", **pad)
        ttk.Button(top, text="浏览", width=5,
                   command=self._browse).grid(row=1, column=5, **pad)
        self._default_bin()

        ttk.Label(top, text="头版本").grid(row=2, column=0, **pad)
        self.v_hdr = tk.StringVar(value="v2")
        ttk.Radiobutton(top, text="v2(推荐)", variable=self.v_hdr,
                        value="v2").grid(row=2, column=1, sticky="w", **pad)
        ttk.Radiobutton(top, text="v1", variable=self.v_hdr,
                        value="v1").grid(row=2, column=1, sticky="e", **pad)

        ttk.Label(top, text="版本号(hex)").grid(row=2, column=2, **pad)
        self.e_ver = ttk.Entry(top, width=10)
        self.e_ver.insert(0, "0x00010000")
        self.e_ver.grid(row=2, column=3, **pad)

        ttk.Label(top, text="chunk").grid(row=2, column=4, sticky="w", **pad)
        self.e_chunk = ttk.Entry(top, width=6)
        self.e_chunk.insert(0, "256")
        self.e_chunk.grid(row=2, column=4, sticky="e", **pad)

        ttk.Label(top, text="重试").grid(row=3, column=0, **pad)
        self.e_retry = ttk.Entry(top, width=6)
        self.e_retry.insert(0, "8")
        self.e_retry.grid(row=3, column=1, sticky="w", **pad)

        ttk.Label(top, text="升级后监测(s)").grid(row=3, column=1, sticky="e",
                                                  **pad)
        self.e_mon = ttk.Entry(top, width=6)
        self.e_mon.insert(0, "20")
        self.e_mon.grid(row=3, column=2, sticky="w", **pad)

        ttk.Label(top, text="APP横幅口(可选)").grid(row=3, column=3, **pad)
        self.cb_bport = ttk.Combobox(top, width=10, state="readonly")
        self.cb_bport.grid(row=3, column=4, **pad)
        self.e_bbaud = ttk.Entry(top, width=8)
        self.e_bbaud.insert(0, "115200")
        self.e_bbaud.grid(row=3, column=5, **pad)

        # action row
        act = ttk.Frame(self.root)
        act.pack(fill="x", padx=8)
        self.btn_go = ttk.Button(act, text="▶ 开始升级",
                                 command=self._start)
        self.btn_go.pack(side="left", padx=4, pady=4)
        self.btn_stop = ttk.Button(act, text="■ 停止", state="disabled",
                                   command=self._stop)
        self.btn_stop.pack(side="left", padx=4)
        self.btn_close = ttk.Button(act, text="⏏ 关闭串口",
                                    command=self._close_port)
        self.btn_close.pack(side="left", padx=4)
        ttk.Button(act, text="清空日志",
                   command=self._clear).pack(side="left", padx=4)
        ttk.Button(act, text="保存日志",
                   command=self._save).pack(side="left", padx=4)

        # progress
        prog = ttk.LabelFrame(self.root, text="进度")
        prog.pack(fill="x", padx=8, pady=6)
        self.pb = ttk.Progressbar(prog, maximum=100)
        self.pb.pack(fill="x", padx=6, pady=4)
        self.lbl_prog = ttk.Label(prog, text="等待开始…")
        self.lbl_prog.pack(anchor="w", padx=6)
        self.lbl_stat = ttk.Label(
            prog, text="DATA:0  ACK:0  NACK:0  重传:0  超时:0",
            font=("Consolas", 9))
        self.lbl_stat.pack(anchor="w", padx=6, pady=(0, 4))

        # log
        logf = ttk.LabelFrame(self.root, text="日志 / 监测")
        logf.pack(fill="both", expand=True, padx=8, pady=6)
        self.txt = tk.Text(logf, wrap="none", bg="#1e1e1e", fg="#d4d4d4",
                           font=("Consolas", 9), insertbackground="#d4d4d4")
        sb = ttk.Scrollbar(logf, command=self.txt.yview)
        self.txt.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.txt.pack(side="left", fill="both", expand=True)
        self.txt.tag_config("info", foreground="#d4d4d4")
        self.txt.tag_config("ok", foreground="#4ec94e")
        self.txt.tag_config("nack", foreground="#ff6b6b")
        self.txt.tag_config("err", foreground="#ff6b6b",
                            font=("Consolas", 9, "bold"))
        self.txt.tag_config("bl", foreground="#56a8ff")
        self.txt.tag_config("banner", foreground="#c586c0")
        self.txt.configure(state="disabled")

        self.stats = dict(data=0, ack=0, nack=0, retx=0, timeout=0)
        self.nack_kinds = {}

    def _default_bin(self):
        for cand in (r"APP\MDK-ARM\APP\APP.bin",
                     r"APP\MDK-ARM\APP\APP.hex", "v2.hex"):
            p = os.path.join(os.getcwd(), cand)
            if os.path.isfile(p):
                self.e_bin.insert(0, p)
                return

    @staticmethod
    def _dev(s):
        return s.split()[0] if s.strip() else ""

    def _refresh_ports(self):
        items = [f"{p.device}  {p.description}"
                 for p in serial.tools.list_ports.comports()]
        self.cb_port["values"] = items
        self.cb_bport["values"] = [""] + items
        cur = self._dev(self.cb_port.get())
        if items and not cur:
            self.cb_port.set(items[0])

    def _browse(self):
        p = filedialog.askopenfilename(
            title="选择固件",
            filetypes=[("固件", "*.bin *.hex"), ("所有", "*.*")])
        if p:
            self.e_bin.delete(0, "end")
            self.e_bin.insert(0, p)

    # -- start / stop --
    def _start(self):
        if self.worker and self.worker.is_alive():
            return
        try:
            cfg = dict(
                port=self._dev(self.cb_port.get()),
                baud=int(self.e_baud.get()),
                bin=self.e_bin.get().strip(),
                hdr=self.v_hdr.get(),
                appver=int(self.e_ver.get(), 0),
                chunk=int(self.e_chunk.get()),
                retries=int(self.e_retry.get()),
                monitor=int(self.e_mon.get()),
            )
        except ValueError as e:
            messagebox.showerror("参数错误", str(e))
            return
        if not cfg["port"]:
            messagebox.showerror("参数错误", "请选择 OTA 串口")
            return
        if not os.path.isfile(cfg["bin"]):
            messagebox.showerror("参数错误", "固件文件不存在")
            return
        if cfg["chunk"] > 512:
            messagebox.showerror("参数错误", "chunk 必须 <= 512")
            return

        self.stats = dict(data=0, ack=0, nack=0, retx=0, timeout=0)
        self.nack_kinds = {}
        self._update_stat()
        self.pb["value"] = 0
        self.stop_evt.clear()
        self.btn_go["state"] = "disabled"
        self.btn_stop["state"] = "normal"

        bport = self._dev(self.cb_bport.get())
        if bport:
            self.banner = BannerTail(bport, int(self.e_bbaud.get()),
                                     self.q, self.stop_evt)
            self.banner.start()
        self.worker = OtaWorker(cfg, self.q, self.stop_evt)
        self.worker.start()

    def _stop(self):
        self.stop_evt.set()
        self._log("用户请求停止…", "err")

    def _close_port(self):
        """Stop any transfer and force-release both serial ports now."""
        self.stop_evt.set()
        closed = False
        if self.worker and self.worker.is_alive():
            self.worker.close_port()
            closed = True
        if not closed:
            self._log("当前没有打开的串口。", "info")
        else:
            self._log("已请求关闭串口（释放 COM 占用）。", "info")
        self.btn_go["state"] = "normal"
        self.btn_stop["state"] = "disabled"

    # -- queue pump --
    def _pump(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "log":
                    self._log(msg[1], msg[2])
                elif kind == "progress":
                    _, off, size, sp, eta = msg
                    pct = off * 100 // size if size else 0
                    self.pb["value"] = pct
                    self.lbl_prog.config(
                        text=f"{off}/{size} 字节  {pct}%   "
                             f"{sp / 1024:.1f} KB/s   ETA {eta:.0f}s")
                elif kind == "stat":
                    d = msg[1]
                    for k in ("data", "ack", "nack", "retx"):
                        if k in d:
                            self.stats[k] += d[k]
                    if "timeout" in d:
                        self.stats["timeout"] += d["timeout"]
                    if "nack_name" in d:
                        nm = d["nack_name"]
                        self.nack_kinds[nm] = self.nack_kinds.get(nm, 0) + 1
                    self._update_stat()
                elif kind == "state":
                    self._on_state(msg[1])
        except queue.Empty:
            pass
        self.root.after(60, self._pump)

    def _on_state(self, st):
        if st in ("done", "error"):
            self.btn_go["state"] = "normal"
            self.btn_stop["state"] = "disabled"
            self.stop_evt.set()       # also stops banner tail

    def _update_stat(self):
        nk = "  ".join(f"{k}:{v}" for k, v in self.nack_kinds.items())
        self.lbl_stat.config(
            text=f"DATA:{self.stats['data']}  ACK:{self.stats['ack']}  "
                 f"NACK:{self.stats['nack']}  重传:{self.stats['retx']}  "
                 f"超时:{self.stats['timeout']}"
                 + (f"   [{nk}]" if nk else ""))

    def _log(self, text, tag="info"):
        self.txt.configure(state="normal")
        self.txt.insert("end", time.strftime("%H:%M:%S ") + text + "\n", tag)
        self.txt.see("end")
        self.txt.configure(state="disabled")

    def _clear(self):
        self.txt.configure(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.configure(state="disabled")

    def _save(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".txt",
            initialfile=time.strftime("ota_log_%Y%m%d_%H%M%S.txt"))
        if p:
            with open(p, "w", encoding="utf-8") as f:
                f.write(self.txt.get("1.0", "end"))
            messagebox.showinfo("已保存", p)


if __name__ == "__main__":
    r = tk.Tk()
    App(r)
    r.mainloop()
