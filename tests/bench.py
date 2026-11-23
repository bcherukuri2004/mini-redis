"""Compare throughput and latency with real Redis using redis-benchmark.

Both servers run on one CPU core and the load generator on the other, with
the same settings. Run it with: make bench"""
import csv
import io
import json
import os
import shutil
import statistics
import subprocess
import sys

from helpers import ROOT, Server, free_port

REQUESTS = {"memory": 400000, "everysec": 400000, "always": 20000}  # fsync on every write is slow, so fewer
CLIENTS = 50
RUNS = 3
TESTS = "set,get,incr"


def start(kind, mode):
    port = free_port()
    pin = ["taskset", "-c", "0"] if os.path.exists("/usr/bin/taskset") else []
    if kind == "mini":
        argv = pin + [os.path.join(ROOT, "mini-redis"), "--port", str(port)]
        if mode != "memory":
            aof = "/tmp/bench-mini.aof"
            if os.path.exists(aof):
                os.remove(aof)
            argv += ["--aof", aof, "--fsync", mode]
    else:
        argv = pin + ["redis-server", "--port", str(port), "--save", "", "--bind", "127.0.0.1"]
        if mode == "memory":
            argv += ["--appendonly", "no"]
        else:
            aofdir = "/tmp/bench-redis"
            shutil.rmtree(aofdir, ignore_errors=True)
            os.makedirs(aofdir)
            argv += ["--appendonly", "yes", "--appendfsync", mode, "--dir", aofdir]
    return Server(argv, port)


def run_benchmark(port, pipeline, requests):
    cmd = ["taskset", "-c", "1", "redis-benchmark", "-p", str(port), "-n", str(requests), "-c", str(CLIENTS),
           "-P", str(pipeline), "-t", TESTS, "-d", "64", "--csv", "-r", "100000"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = {}
    for row in csv.reader(io.StringIO(out)):
        if row and row[0] != "test":
            rows[row[0]] = {"rps": float(row[1]), "p50": float(row[4]), "p99": float(row[6])}
    return rows


def main():
    results = {}
    modes = sys.argv[1:] or ["memory", "everysec", "always"]
    for mode in modes:
        for pipeline in (1, 16):
            for kind in ("redis", "mini"):
                samples = []
                for _ in range(RUNS):
                    srv = start(kind, mode)
                    try:
                        samples.append(run_benchmark(srv.port, pipeline, REQUESTS[mode]))
                    finally:
                        srv.stop()
                merged = {}
                for name in samples[0]:
                    merged[name] = {
                        "rps": statistics.median(s[name]["rps"] for s in samples),
                        "p50": statistics.median(s[name]["p50"] for s in samples),
                        "p99": statistics.median(s[name]["p99"] for s in samples),
                    }
                results["%s|%s|P%d" % (kind, mode, pipeline)] = merged
                print(kind, mode, "P%d" % pipeline, {k: round(v["rps"]) for k, v in merged.items()}, flush=True)
    path = os.path.join(ROOT, "bench-results.json")
    old = json.load(open(path)) if os.path.exists(path) else {}
    old.update(results)
    json.dump(old, open(path, "w"), indent=2)
    print("saved", path)


if __name__ == "__main__":
    sys.exit(main())
