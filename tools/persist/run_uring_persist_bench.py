#!/usr/bin/env python3
import argparse
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path


def build_resp(*args: str) -> bytes:
    parts = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        b = str(arg).encode()
        parts.append(f"${len(b)}\r\n".encode())
        parts.append(b)
        parts.append(b"\r\n")
    return b"".join(parts)


def req(host: str, port: int, *args: str) -> bytes:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect((host, port))
    s.sendall(build_resp(*args))
    chunks = []
    try:
        while True:
            data = s.recv(4096)
            if not data:
                break
            chunks.append(data)
            if len(data) < 4096:
                break
    except socket.timeout:
        pass
    finally:
        s.close()
    return b"".join(chunks)


def wait_ready(host: str, port: int, retries: int = 60) -> None:
    for _ in range(retries):
        try:
            if req(host, port, "PING").startswith(b"+PONG"):
                return
        except Exception:
            time.sleep(0.2)
    raise RuntimeError("server not ready")


def stop_proc(proc: subprocess.Popen) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=1)
        return
    except Exception:
        pass
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=1)
    except Exception:
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description="io_uring persistence correctness/perf smoke benchmark")
    ap.add_argument("--bin", default="./kvstore")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5140)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--appendfsync", choices=["always", "everysec"], default="always")
    ap.add_argument("--work-dir", default="./artifacts/persist/uring-bench")
    args = ap.parse_args()

    root = Path(args.work_dir).resolve()
    root.mkdir(parents=True, exist_ok=True)
    dump_path = root / "kvstore.dump"
    aof_path = root / "kvstore.aof"
    log_path = root / "server.log"

    for p in (dump_path, aof_path):
        if p.exists():
            p.unlink()

    logf = open(log_path, "ab")
    proc = subprocess.Popen(
        [args.bin, "--port", str(args.port), "--dump", str(dump_path), "--aof", str(aof_path), "--appendfsync", args.appendfsync],
        stdout=logf,
        stderr=logf,
        cwd=str(Path(args.bin).resolve().parent),
        preexec_fn=os.setsid,
    )

    try:
        wait_ready(args.host, args.port)

        t0 = time.perf_counter()
        for i in range(args.count):
            if req(args.host, args.port, "SET", f"bench:key:{i}", f"value:{i}") != b"+OK\r\n":
                raise RuntimeError(f"SET failed at {i}")
        t1 = time.perf_counter()

        save_resp = req(args.host, args.port, "SAVE")
        if save_resp != b"+OK\r\n":
            raise RuntimeError(f"SAVE failed: {save_resp!r}")
        t2 = time.perf_counter()

        aof_size = aof_path.stat().st_size if aof_path.exists() else -1
        dump_size = dump_path.stat().st_size if dump_path.exists() else -1

        stop_proc(proc)
        logf.close()

        logf = open(log_path, "ab")
        proc = subprocess.Popen(
            [args.bin, "--port", str(args.port), "--dump", str(dump_path), "--aof", str(aof_path), "--appendfsync", args.appendfsync],
            stdout=logf,
            stderr=logf,
            cwd=str(Path(args.bin).resolve().parent),
            preexec_fn=os.setsid,
        )
        wait_ready(args.host, args.port)

        sample_idxs = [0, args.count // 2, max(0, args.count - 1)]
        for idx in sample_idxs:
            expected = f"$7\r\nvalue:{idx}\r\n".encode() if idx >= 10 else None
            resp = req(args.host, args.port, "GET", f"bench:key:{idx}")
            if f"value:{idx}".encode() not in resp:
                raise RuntimeError(f"recovery mismatch for key {idx}: {resp!r}")

        t3 = time.perf_counter()

        print("IO_URING_PERSIST_BENCH_PASS")
        print(f"count={args.count}")
        print(f"appendfsync={args.appendfsync}")
        print(f"write_seconds={t1 - t0:.6f}")
        print(f"save_seconds={t2 - t1:.6f}")
        print(f"restart_verify_seconds={t3 - t2:.6f}")
        print(f"aof_size_bytes={aof_size}")
        print(f"dump_size_bytes={dump_size}")
        print(f"artifacts={root}")
        return 0
    finally:
        stop_proc(proc)
        try:
            logf.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
