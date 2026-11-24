"""Replication measurements, for mini redis and real Redis 7.0.15 under the
same conditions. Run inside the Linux container: python3 tests/bench_repl.py

Three questions:
  1. How long does a full sync take for a given number of keys?
  2. How far behind is the replica while the primary is under heavy writes?
  3. Does the primary get slower when a replica is attached?

CPU layout (4 cores): primary on core 0, load generator on core 1, replica
on core 2, this script (the probe client) on core 3. Each number is the
median of RUNS runs unless it says otherwise."""
import csv
import io
import json
import os
import socket
import statistics
import subprocess
import sys
import tempfile
import time

from helpers import ROOT, Client, Server, free_port

MINI = os.path.join(ROOT, "mini-redis")  # the normal optimized build, not the sanitizer one
RUNS = 3
SYNC_SIZES = [10_000, 100_000, 1_000_000]
VALUE = b"v" * 64
PIN = os.path.exists("/usr/bin/taskset")


def pin(cpu):
    return ["taskset", "-c", str(cpu)] if PIN else []


def start(kind, cpu, replicaof=None):
    port = free_port()
    if kind == "mini":
        argv = pin(cpu) + [MINI, "--port", str(port)]
        if replicaof:
            argv += ["--replicaof", "127.0.0.1:%d" % replicaof]
    else:
        argv = pin(cpu) + ["redis-server", "--port", str(port), "--save", "", "--appendonly", "no",
                           "--bind", "127.0.0.1", "--dir", tempfile.mkdtemp(prefix="bench-redis-"),
                           "--repl-diskless-sync-delay", "0"]
        if replicaof:
            argv += ["--replicaof", "127.0.0.1", str(replicaof)]
    srv = Server(argv, port)
    assert srv.alive, srv.stop()
    return srv


def info(cl):
    out = {}
    for line in cl.cmd("INFO", "replication").decode().split("\r\n"):
        if ":" in line and not line.startswith("#"):
            k, v = line.split(":", 1)
            out[k] = v
    return out


def wait_until(fn, timeout=120, poll=0.002):
    deadline = time.time() + timeout
    while not fn():
        if time.time() > deadline:
            raise AssertionError("timed out")
        time.sleep(poll)


def caught_up(pc, rc):
    p, r = info(pc), info(rc)
    return r.get("master_link_status") == "up" and p.get("master_repl_offset") == r.get("slave_repl_offset")


def load_keys(port, n, batch=10_000):
    """SET key:00000000 .. with 64 byte values, pipelined in batches."""
    s = socket.create_connection(("127.0.0.1", port))
    for start_i in range(0, n, batch):
        count = min(batch, n - start_i)
        s.sendall(b"".join(Client.encode("SET", b"key:%08d" % i, VALUE) for i in range(start_i, start_i + count)))
        got = 0
        while got < count:
            got += s.recv(1 << 20).count(b"+OK\r\n")
    s.close()


def med(xs):
    return statistics.median(xs)


# ---------------------------------------------------------------- 1. full sync time

def full_sync(kind, n):
    p = start(kind, 0)
    load_keys(p.port, n)
    r = start(kind, 2)
    rc = r.client()
    t0 = time.perf_counter()
    assert rc.cmd("REPLICAOF", "127.0.0.1", p.port) == "OK"
    wait_until(lambda: info(rc).get("master_link_status") == "up")
    wall_ms = (time.perf_counter() - t0) * 1000
    keys = rc.cmd("DBSIZE")
    assert keys == n, (kind, n, keys)
    row = {"wall_ms": wall_ms}
    if kind == "mini":
        ri, pi = info(rc), info(p.client())
        row["replica_sync_ms"] = int(ri["last_full_sync_usec"]) / 1000
        row["primary_build_ms"] = int(pi["last_snapshot_usec"]) / 1000
    rc.close()
    r.stop()
    p.stop()
    return row


# ---------------------------------------------------------------- 2 and 3. load

def benchmark_cmd(port, pipeline, requests):
    return pin(1) + ["redis-benchmark", "-p", str(port), "-n", str(requests), "-c", "50", "-P", str(pipeline),
                     "-t", "set", "-d", "64", "-r", "100000", "--csv"]


def parse_rps(out):
    for row in csv.reader(io.StringIO(out)):
        if row and row[0] == "SET":
            return float(row[1])
    raise ValueError(out)


def throughput(kind, pipeline, with_replica, requests):
    p = start(kind, 0)
    r = None
    if with_replica:
        r = start(kind, 2, replicaof=p.port)
        rc = r.client()
        wait_until(lambda: info(rc).get("master_link_status") == "up")
    out = subprocess.run(benchmark_cmd(p.port, pipeline, requests), capture_output=True, text=True, check=True).stdout
    rps = parse_rps(out)
    if r:
        pc = p.client()
        wait_until(lambda: caught_up(pc, rc))
        assert rc.cmd("DBSIZE") == pc.cmd("DBSIZE")  # the replica really got every write
        r.stop()
    p.stop()
    return rps


def lag_under_load(kind, pipeline, requests):
    """While redis-benchmark hammers the primary with SETs, write a probe key
    every 20 ms and time how long until the replica shows it. Also sample
    the byte gap between the primary's and the replica's offsets."""
    p = start(kind, 0)
    r = start(kind, 2, replicaof=p.port)
    pc, rc = p.client(), r.client()
    wait_until(lambda: info(rc).get("master_link_status") == "up")
    bench = subprocess.Popen(benchmark_cmd(p.port, pipeline, requests), stdout=subprocess.PIPE, text=True)
    delays, gaps = [], []
    seq = 0
    while bench.poll() is None:
        seq += 1
        want = str(seq).encode()
        pc.cmd("SET", "probe", want)
        t0 = time.perf_counter()  # the primary has applied it and replied
        while rc.cmd("GET", "probe") != want:
            time.sleep(0.0002)
        delays.append((time.perf_counter() - t0) * 1000)
        # Read the replica first, then the primary: the gap can only come out
        # too big (writes landing in between), never too small. An upper limit.
        ri = info(rc)
        pi = info(pc)
        gaps.append(int(pi["master_repl_offset"]) - int(ri["slave_repl_offset"]))
        time.sleep(0.02)
    out = bench.communicate()[0]
    assert delays, "the load finished before any probe was measured; raise the request count"
    t0 = time.perf_counter()
    wait_until(lambda: caught_up(pc, rc), poll=0.0005)
    catch_up_ms = (time.perf_counter() - t0) * 1000
    assert rc.cmd("DBSIZE") == pc.cmd("DBSIZE")
    pc.close()
    rc.close()
    r.stop()
    p.stop()
    delays.sort()
    return {
        "probes": len(delays),
        "delay_ms_median": med(delays),
        "delay_ms_p99": delays[min(len(delays) - 1, int(len(delays) * 0.99))],
        "delay_ms_max": delays[-1],
        "gap_bytes_median": med(gaps),
        "gap_bytes_max": max(gaps),
        "catch_up_ms_after_load": catch_up_ms,
        "load_rps": parse_rps(out),
    }


# ---------------------------------------------------------------- main

def main():
    quick = "--quick" in sys.argv
    sizes = [1000, 10_000] if quick else SYNC_SIZES
    runs = 1 if quick else RUNS
    reqs = {1: 20_000, 16: 100_000} if quick else {1: 400_000, 16: 4_000_000}
    if PIN:
        os.sched_setaffinity(0, {3})
    results = {"machine": {"cpus_in_vm": os.cpu_count(), "pinned": PIN, "runs_per_number": runs}}

    sync = {}
    for kind in ("redis", "mini"):
        for n in sizes:
            rows = [full_sync(kind, n) for _ in range(runs)]
            sync["%s|%d" % (kind, n)] = {k: med([row[k] for row in rows]) for k in rows[0]}
            print("full sync", kind, n, {k: round(v, 1) for k, v in sync["%s|%d" % (kind, n)].items()}, flush=True)
    results["full_sync"] = sync

    tput = {}
    for kind in ("redis", "mini"):
        for pipeline in (1, 16):
            for with_replica in (False, True):
                rps = med([throughput(kind, pipeline, with_replica, reqs[pipeline]) for _ in range(runs)])
                key = "%s|P%d|%s" % (kind, pipeline, "replica" if with_replica else "alone")
                tput[key] = rps
                print("throughput", key, round(rps), flush=True)
    results["primary_set_rps"] = tput

    lag = {}
    for kind in ("redis", "mini"):
        for pipeline in (1, 16):
            rows = [lag_under_load(kind, pipeline, reqs[pipeline]) for _ in range(runs)]
            lag["%s|P%d" % (kind, pipeline)] = {k: med([row[k] for row in rows]) for k in rows[0]}
            print("lag", kind, "P%d" % pipeline, {k: round(v, 2) for k, v in lag["%s|P%d" % (kind, pipeline)].items()},
                  flush=True)
    results["lag_under_load"] = lag

    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(ROOT, "replication_bench.json")
    with open(out, "w") as f:
        json.dump(results, f, indent=2)
    print("saved", out)


if __name__ == "__main__":
    main()
