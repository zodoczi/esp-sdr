#!/usr/bin/env python3
"""ESP32-S3 SPEC load-balancing benchmark (needs pyserial).

Runs SPEC over the matrix FFT size x sample rate x DUAL mode and prints a
table and a CSV: FFTs/s, coverage, core loads, late_max, longest core-0 step,
longest USB service gap, hand-off deferrals, drops and CRC errors.

  python tools/spec_lb_bench.py COM7 [--seconds 10] [--csv out.csv]
"""
import argparse, csv, struct, sys, time, zlib

RATES = {0: 80_000_000, 1: 40_000_000, 6: 16_000_000}
CPU_HZ = 240_000_000


def cmd(ser, line, timeout=2.0):
    ser.reset_input_buffer()
    ser.write((line + "\n").encode())
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += ser.read(ser.in_waiting or 1)
        if b"\n" in buf:
            return buf.split(b"\n", 1)[0].decode(errors="replace").strip()
    return ""


def run_spec(ser, nfft, rate, seconds):
    ser.reset_input_buffer()
    ser.write(f"SPEC {int(seconds*1000)} 1 1 0 {rate} {nfft} 1\n".encode())
    buf = bytearray()
    res = dict(frames=0, ffts=0, crc_err=0, sps=[], end=None, start=None)
    deadline = time.time() + seconds + 5
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        while True:
            if res["start"] is None:
                i = buf.find(b"\n")
                if i < 0:
                    break
                res["start"] = buf[:i].decode(errors="replace"); del buf[:i+1]
                if not res["start"].startswith("SPEC "):
                    return res
                continue
            m = min([x for x in (buf.find(b"SPC1"), buf.find(b"SPS1"), buf.find(b"SPECEND")) if x >= 0], default=-1)
            if m < 0:
                del buf[:max(0, len(buf) - 8)]; break
            del buf[:m]
            if buf.startswith(b"SPECEND"):
                i = buf.find(b"\n")
                if i < 0:
                    break
                res["end"] = buf[:i].decode().split(); return res
            if buf.startswith(b"SPS1"):
                if len(buf) < 40:
                    break
                body, crc = bytes(buf[:36]), struct.unpack_from("<I", buf, 36)[0]
                if zlib.crc32(body) == crc:
                    res["sps"].append(struct.unpack("<4sHHHHIIIIHHI", body))
                    del buf[:40]
                else:
                    res["crc_err"] += 1; del buf[:4]
                continue
            if len(buf) < 28:
                break
            n = 1 << buf[26]
            if n > 65536:
                del buf[:4]; continue
            if len(buf) < 28 + n + 4:
                break
            crc = struct.unpack_from("<I", buf, 28 + n)[0]
            if zlib.crc32(bytes(buf[:28 + n])) == crc:
                res["frames"] += 1; res["ffts"] += struct.unpack_from("<H", buf, 20)[0]
                del buf[:28 + n + 4]
            else:
                res["crc_err"] += 1; del buf[:4]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port"); ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--csv", default="spec_lb_bench.csv")
    ap.add_argument("--nfft", default="4096,2048"); ap.add_argument("--rates", default="0,1,6")
    ap.add_argument("--dual", default="0,1,2")
    a = ap.parse_args()
    import serial
    ser = serial.Serial(a.port, 115200, timeout=0.05)
    print(cmd(ser, "CAPS")); print(cmd(ser, "LBSTAT 1"), cmd(ser, "LBSTAT?"))
    rows = []
    for nfft in map(int, a.nfft.split(",")):
        for rate in map(int, a.rates.split(",")):
            for dual in map(int, a.dual.split(",")):
                cmd(ser, f"DUAL {dual}")
                r = run_spec(ser, nfft, rate, a.seconds)
                e = r["end"] or []
                ast = cmd(ser, "ASSIST?").split()[1:]
                ast += ["0"] * (22 - len(ast))
                sps = r["sps"][1:] or r["sps"]
                avg = lambda k: sum(s[k] for s in sps) / len(sps) if sps else 0
                el_s = int(e[5]) / 1e6 if len(e) > 5 else a.seconds
                ffts = int(e[11]) if len(e) > 11 else r["ffts"]
                row = dict(nfft=nfft, msps=RATES[rate] // 1_000_000, dual=dual,
                           status=e[1] if len(e) > 1 else "noend",
                           ffts_s=round(ffts / el_s), cov_pct=round(100 * ffts * nfft / (RATES[rate] * el_s), 2),
                           c0_pct=round(avg(1) / 10, 1), c1_pct=round(avg(2) / 10, 1),
                           c0_share_pct=round(sum(x[4] >> 8 for x in sps) / len(sps), 1) if sps else 0,
                           sps_cov_pct=round(avg(3) / 10, 1),
                           late_max=e[6] if len(e) > 6 else "",
                           step_max_us=round(int(ast[8]) / 240, 1), usb_gap_max_us=round(int(ast[9]) / 240, 1),
                           handoff_defer=ast[10], acc_mode=ast[7], margin=ast[6],
                           frames=r["frames"], drops=e[9] if len(e) > 9 else "",
                           abandoned=e[10] if len(e) > 10 else "", crc_err=r["crc_err"],
                           c0_claims=ast[18], c0_aborts=ast[19], c1_timeouts=ast[20], fast=ast[21],
                           cost_cp_st_acc="/".join(ast[12:15]), warm_cp_st_acc="/".join(ast[15:18]))
                rows.append(row)
                print(" ".join(f"{k}={v}" for k, v in row.items()), flush=True)
    cmd(ser, "DUAL 1")
    keys = list(rows[0])
    w = [max(len(k), *(len(str(r[k])) for r in rows)) for k in keys]
    print("\n" + " ".join(k.rjust(x) for k, x in zip(keys, w)))
    for r in rows:
        print(" ".join(str(r[k]).rjust(x) for k, x in zip(keys, w)))
    with open(a.csv, "w", newline="") as f:
        cw = csv.DictWriter(f, keys); cw.writeheader(); cw.writerows(rows)
    print(f"\nCSV: {a.csv}")


if __name__ == "__main__":
    sys.exit(main())
