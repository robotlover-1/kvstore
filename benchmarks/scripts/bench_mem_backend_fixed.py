#!/usr/bin/env python3
import argparse
import csv
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
KV_BIN = ROOT / "kvstore"


def build_resp(*parts: str) -> bytes:
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        b = part.encode()
        out.append(f"${len(b)}\r\n".encode())
        out.append(b)
        out.append(b"\r\n")
    return b"".join(out)


def read_line(sock: socket.socket) -> bytes:
    buf = bytearray()
    while True:
        ch = sock.recv(1)
        if not ch:
            raise ConnectionError("socket closed")
        buf.extend(ch)
        if buf.endswith(b"\r\n"):
            return bytes(buf)


def recv_resp(sock: socket.socket):
    first = sock.recv(1)
    if not first:
        raise ConnectionError("empty response")
    if first in (b"+", b"-", b":"):
        return first + read_line(sock)
    if first == b"$":
        length_line = read_line(sock)
        length = int(length_line[:-2])
        if length == -1:
            return None
        data = bytearray()
        while len(data) < length + 2:
            chunk = sock.recv(length + 2 - len(data))
            if not chunk:
                raise ConnectionError("socket closed during bulk read")
            data.extend(chunk)
        return bytes(data[:-2])
    raise ValueError(f"unsupported RESP type: {first!r}")


def send_cmd(sock: socket.socket, *parts: str):
    sock.sendall(build_resp(*parts))
    return recv_resp(sock)


def wait_ready(port: int, timeout: float = 5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                send_cmd(sock, "INFO")
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"server on port {port} did not become ready")


def parse_proc_status(pid: int):
    info = {}
    with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
        for line in f:
            if ":" not in line:
                continue
            k, v = line.split(":", 1)
            info[k.strip()] = v.strip()
    return info


def parse_kb(field: str) -> int:
    return int(field.split()[0])


def parse_memstat(text: str):
    out = {}
    for line in text.strip().splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def unique_run_tag(backend: str, port: int) -> str:
    return f"{backend}_{port}_{os.getpid()}_{int(time.time() * 1000)}"


def cleanup_paths(*paths: Path):
    for path in paths:
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def run_one_backend(backend: str, port: int, n: int, value_size: int, warmup: int, settle: float):
    run_tag = unique_run_tag(backend, port)
    dump_path = ROOT / f"bench_{run_tag}.dump"
    aof_path = ROOT / f"bench_{run_tag}.aof"
    cleanup_paths(dump_path, aof_path)

    cmd = [
        str(KV_BIN),
        "--port", str(port),
        "--mem", backend,
        "--dump", dump_path.name,
        "--aof", aof_path.name,
    ]
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_ready(port)
        payload = "x" * value_size
        with socket.create_connection(("127.0.0.1", port), timeout=10.0) as sock:
            for i in range(warmup):
                resp = send_cmd(sock, "SET", f"warm:{run_tag}:{i}", payload)
                if not resp or resp[:1] != b"+":
                    raise RuntimeError(f"unexpected warmup SET response: {resp!r}")

            t0 = time.perf_counter()
            for i in range(n):
                resp = send_cmd(sock, "SET", f"key:{run_tag}:{i}", payload)
                if not resp or resp[:1] != b"+":
                    raise RuntimeError(f"unexpected SET response: {resp!r}")
            elapsed = time.perf_counter() - t0
            qps = n / elapsed if elapsed > 0 else 0.0
            time.sleep(settle)
            memstat_raw = send_cmd(sock, "MEMSTAT")
            info_raw = send_cmd(sock, "INFO")

        status = parse_proc_status(proc.pid)
        memstat = parse_memstat(memstat_raw.decode() if isinstance(memstat_raw, bytes) else "")
        info_text = info_raw.decode() if isinstance(info_raw, bytes) else str(info_raw)
        return {
            "backend": backend,
            "port": port,
            "ops": n,
            "value_size": value_size,
            "elapsed_sec": round(elapsed, 6),
            "qps": round(qps, 2),
            "vmrss_kb": parse_kb(status.get("VmRSS", "0 kB")),
            "vmsize_kb": parse_kb(status.get("VmSize", "0 kB")),
            "vmdata_kb": parse_kb(status.get("VmData", "0 kB")),
            "mem_gap_kb": parse_kb(status.get("VmSize", "0 kB")) - parse_kb(status.get("VmRSS", "0 kB")),
            "info": info_text.strip(),
            "memstat_backend": memstat.get("backend", ""),
            "current_small_inuse": memstat.get("current_small_inuse", "0"),
            "peak_small_inuse": memstat.get("peak_small_inuse", "0"),
            "total_small_page_bytes": memstat.get("total_small_page_bytes", "0"),
            "current_large_inuse_bytes": memstat.get("current_large_inuse_bytes", "0"),
            "peak_large_inuse_bytes": memstat.get("peak_large_inuse_bytes", "0"),
            "active_large_map_bytes": memstat.get("active_large_map_bytes", "0"),
            "peak_active_large_map_bytes": memstat.get("peak_active_large_map_bytes", "0"),
            "large_alloc_calls": memstat.get("large_alloc_calls", "0"),
            "small_alloc_calls": memstat.get("small_alloc_calls", "0"),
            "run_tag": run_tag,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2.0)
        cleanup_paths(dump_path, aof_path)


def main():
    parser = argparse.ArgumentParser(description="Benchmark kvstore memory backends")
    parser.add_argument("--ops", type=int, default=20000, help="number of SET operations per backend")
    parser.add_argument("--value-size", type=int, default=128, help="payload bytes per SET")
    parser.add_argument("--warmup", type=int, default=200, help="warmup SET operations before timing")
    parser.add_argument("--base-port", type=int, default=6500, help="base port for backend runs")
    parser.add_argument("--backends", default="libc,jemalloc,custom", help="comma separated backends")
    parser.add_argument("--settle", type=float, default=0.5, help="seconds to wait before sampling process memory")
    parser.add_argument("--csv", default="../data/bench_results.csv", help="output csv filename")
    args = parser.parse_args()

    if not KV_BIN.exists():
        print("kvstore binary not found, run `make` first", file=sys.stderr)
        return 1

    backends = [b.strip() for b in args.backends.split(",") if b.strip()]
    rows = []
    for idx, backend in enumerate(backends):
        row = run_one_backend(backend, args.base_port + idx, args.ops, args.value_size, args.warmup, args.settle)
        rows.append(row)
        print(
            f"{backend}: qps={row['qps']} vmrss_kb={row['vmrss_kb']} "
            f"vmsize_kb={row['vmsize_kb']} mem_gap_kb={row['mem_gap_kb']}"
        )

    csv_path = ROOT / args.csv
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"wrote {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
