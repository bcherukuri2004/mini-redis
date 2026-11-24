# mini-redis

A small in memory key value database written in C. It speaks the real Redis
protocol, so `redis-cli`, `redis-benchmark` and normal Redis client libraries
work against it. It is a learning project: it covers about 30 commands, not
the whole of Redis, and it is not meant to replace it.

```
$ make
$ ./mini-redis --port 6399 --aof data.aof --fsync everysec
$ redis-cli -p 6399
127.0.0.1:6399> SET greeting hello EX 60
OK
127.0.0.1:6399> GET greeting
"hello"
```

## What is in it

- **Single threaded event loop** on Linux `epoll`. One thread reads from every
  socket that is ready, runs each complete command, and writes the replies, so
  no locks are needed. Handles pipelining, commands split across packets,
  clients that read slowly (output is queued and sent when the socket is
  writable), and clients that disconnect halfway through a command.
- **Redis protocol (RESP)** parser that works on partial input and rejects
  malformed input with an error instead of hanging or crashing. Also accepts
  plain text commands, so `telnet` works.
- **Hash table** written from scratch: chained buckets, doubles in size when
  full, and a random per process hash seed so a client cannot craft keys that
  all collide.
- **Key expiry** two ways, like Redis: a key is deleted when you touch it after
  its time has passed, and a background sweep removes keys nobody asks for.
- **Durability** with an append only file (AOF). Every change is logged, and
  the log is replayed at startup. Three flush policies: `always` (a client is
  told "OK" only after the data is on disk), `everysec`, and `no`.
- **Crash recovery.** A crash can leave half a command at the end of the log.
  The server drops it, trims the file, and carries on. A log that is damaged in
  the middle makes the server refuse to start instead of guessing.
- **Log compaction** with `REWRITEAOF`: writes the current data as a fresh log
  under a temporary name, then atomically renames it over the old one, so a
  crash at any point leaves a complete file.

Commands: `GET SET (EX PX EXAT PXAT NX XX KEEPTTL) DEL EXISTS INCR DECR INCRBY
DECRBY MGET MSET EXPIRE PEXPIRE EXPIREAT PEXPIREAT TTL PTTL PERSIST TYPE KEYS
DBSIZE FLUSHALL PING ECHO SELECT INFO REWRITEAOF QUIT REPLICAOF SLAVEOF SYNC
REPLCONF`, plus stubs that let
`redis-cli` and client libraries connect (`COMMAND CONFIG CLIENT`).

## How it was tested

`make test` runs all of this against a build with AddressSanitizer and
UndefinedBehaviorSanitizer, which stop the program on memory errors and
undefined behavior:

| Test | What it checks |
|---|---|
| `tests/unit.c` (165,401 checks) | number parsing, glob matching, buffer, hash table with 200,000 keys, and the protocol parser fed every possible split point of a command |
| `tests/test_integration.py` (41 tests) | real TCP connections: every command, pipelining 20,000 commands, one byte at a time delivery, a 5 MB value, 300 clients at once, a slow reader, garbage input, and concurrent counters that must not lose an update |
| Crash recovery tests | `kill -9` then restart, replay that must not double count, expired keys that must not come back, truncated and corrupted logs, and `REWRITEAOF` followed by a crash |
| `tests/test_differential.py` | 40 random runs of 4,000 commands each (160,000 total) sent to **real Redis 7.0 and this server side by side**; every reply, including error messages, and the final data must be identical |
| `tests/test_replication.py` (48 tests) | primary and replica over real TCP: full copy, every write command, 20,000 pipelined writes, three replicas, killing either side, a frozen replica that gets cut off and recovers, a fake primary that sends garbage or half a copy, pointing at real Redis or at nothing, and the replica limits |
| `tests/test_replication_vs_redis.py` (15 tests) | the same replication scenarios on a **real Redis 7.0.15 primary and replica** and on a mini redis pair; the replicas must end with the same data and give the same replies and errors |

The comparison test found real differences while this was being written (for
example Redis reports a bad option before a bad number in `SET`), which were
fixed to match.

## Benchmarks

These numbers were measured on a 2 core cloud virtual machine before
replication was added. On an Apple M1 Pro (Linux VM with 4 CPUs under Docker)
the same `make bench` gave higher numbers for both servers and a smaller gap:
with 16 pipelined `SET`, Redis 923,788 and mini redis 1,250,000 requests per
second, so about 1.35 times rather than about 2 times. Those results are in
`bench_results_mac.json`.

Measured with `redis-benchmark` (`make bench`): 400,000 requests, 50 client
connections, 64 byte values, 100,000 random keys. Each number is the median of 3
runs. Real Redis 7.0.15 ran with default settings and the same persistence mode.
Both servers were pinned to one CPU core and the load generator to the other, on a
2 core virtual machine over loopback.

| Persistence | Pipeline | Command | Redis 7.0 (requests/s) | mini redis (requests/s) |
|---|---|---|---|---|
| none | 1 | SET | 96,038 | 104,959 |
| none | 1 | GET | 93,743 | 92,872 |
| none | 1 | INCR | 105,402 | 92,166 |
| none | 16 | SET | 542,005 | 1,061,008 |
| none | 16 | GET | 854,701 | 1,017,812 |
| none | 16 | INCR | 840,336 | 801,603 |
| everysec | 1 | SET | 108,254 | 95,352 |
| everysec | 1 | GET | 99,083 | 87,222 |
| everysec | 1 | INCR | 115,042 | 103,013 |
| everysec | 16 | SET | 410,678 | 804,829 |
| everysec | 16 | GET | 766,284 | 836,820 |
| everysec | 16 | INCR | 686,106 | 729,927 |

How to read this honestly:

- **Without pipelining (one request at a time) the two are about even.** The
  difference between runs of the same test was often 10 to 20 percent, so
  differences that small mean nothing. Most of the time is spent in the
  network system calls, not in the database.
- **With 16 requests pipelined, `SET` was about twice as fast here (1.06 million
  against 542 thousand per second)**, and `GET` and `INCR` were about the same.
  That gap is larger than the noise I saw. I did not profile it. The likely
  reason is that Redis does more work per command (access control, statistics,
  many value types, replication hooks) that this project leaves out. So this does
  not show that this server is better than Redis, only that a deliberately small
  design is cheap per command.
- Latency at the 99th percentile was similar (around 1 to 3 ms under this load).
  The hash table resize pauses described below would show up only in the far
  tail, which this test does not measure.
- I also ran `fsync always` (flush to disk before replying). On this virtual
  machine the disk flush is almost free, so those numbers say nothing about a
  real disk and are left out of the table. They are in `bench-results.json`.


## Replication

A second server can keep a copy of the first one. The copy is called the
replica and the original the primary.

```
$ ./mini-redis --port 6399
$ ./mini-redis --port 6400 --replicaof 127.0.0.1:6399
```

or, on a running server, `REPLICAOF 127.0.0.1 6399` (and `REPLICAOF NO ONE` to
turn it back into a normal server that keeps its data).

How it works:

1. The replica connects to the primary like any client and sends `SYNC`.
2. The primary answers `+FULLSYNC <offset>`, then every key as a `SET` (plus a
   `PEXPIREAT` for keys with a timer), then a marker that says the copy is done.
   This is the same code `REWRITEAOF` uses to compact the log.
3. After that, every change the primary makes is sent to the replica in the
   same form it is written to the log, for example the result of an `INCR`
   rather than the `INCR` itself, and a `DEL` when a key expires.
4. The replica loads the copy into a separate table and swaps it in only when
   it is complete. Its own clients see the old data until then, never half a
   copy, and a copy that breaks off halfway is thrown away.
5. If the link drops, the replica keeps serving its old data, retries once a
   second, and takes a fresh full copy.

Because the server is single threaded, the whole copy is built and queued in
one step, so a write that arrives during a sync is always queued after it.

On a replica, writes from normal clients get the same error as Redis 7:
`READONLY You can't write against a read only replica.` A replica never
expires keys by itself: it hides them from reads and waits for the primary's
`DEL`, as Redis does, so both sides always agree.

`INFO` shows the replication state, including `master_repl_offset` on the
primary and `slave_repl_offset` on the replica (bytes of changes sent and
applied). The field names follow Redis so existing tools can read them.

### Replication measurements

Measured with `python3 tests/bench_repl.py` on an Apple M1 Pro, inside a Linux
VM (Colima, 4 CPUs, 4 GB) under Docker. The primary, the replica, the load
generator and the probe client each had a CPU core of their own. Real Redis
7.0.15 ran under the same conditions with `repl-diskless-sync-delay 0`. Each
number is the median of 3 runs. Another, mostly idle container was running in
the same VM and was using about a third of one CPU when the run started. Raw
results are in `replication_bench.json`.

**Full sync**: time from sending `REPLICAOF` to the replica reporting its link
as up, with all keys loaded (64 byte values, checked with `DBSIZE` every run).

| Keys | Redis 7.0 | mini redis | mini redis: primary building the copy |
|---|---|---|---|
| 10,000 | 10.0 ms | 7.1 ms | 2.9 ms |
| 100,000 | 100.7 ms | 62.4 ms | 30.1 ms |
| 1,000,000 | 1,218.8 ms | 825.5 ms | 352.0 ms |

**Primary speed with and without a replica** (`redis-benchmark` SET, 50
clients, 64 byte values; 400,000 requests without pipelining, 4,000,000 with 16
pipelined). After every run with a replica, the replica was checked to hold the
same number of keys as the primary.

| Pipeline | Server | alone (requests/s) | with a replica (requests/s) | change |
|---|---|---|---|---|
| 1 | Redis 7.0 | 185,357 | 173,989 | 6% slower |
| 1 | mini redis | 198,020 | 198,906 | no drop |
| 16 | Redis 7.0 | 1,026,167 | 761,760 | 26% slower |
| 16 | mini redis | 1,730,104 | 1,107,726 | 36% slower |

**How far behind the replica is under load**: while the benchmark above
hammered the primary, a probe key was written every 20 ms and the replica was
polled until it showed the new value. The delay is counted from the moment the
primary confirmed the write. The byte gap is the primary's offset minus the
replica's, read replica first, so it is an upper limit.

| Pipeline | Server | probes | median delay | 99th percentile | worst | largest byte gap | catch up after load stops |
|---|---|---|---|---|---|---|---|
| 1 | Redis 7.0 | 98 | 0.07 ms | 0.49 ms | 0.51 ms | 6,634 | 0.27 ms |
| 1 | mini redis | 97 | 0.06 ms | 0.48 ms | 0.48 ms | 6,634 | 0.21 ms |
| 16 | Redis 7.0 | 211 | 0.43 ms | 1.48 ms | 2.21 ms | 171,200 | 0.22 ms |
| 16 | mini redis | 168 | 0.19 ms | 1.51 ms | 1.60 ms | 254,339 | 0.17 ms |

How to read this honestly:

- **A replica costs the primary real speed under heavy pipelined writes.**
  mini redis dropped 36 percent at pipeline 16, more than Redis's 26 percent.
  The likely reason is that every write is encoded once more and copied into
  the replica's queue on the same single thread; this was not profiled. mini redis was still faster in absolute terms here, but
  the gap between the two shrank once a replica was attached.
- Without pipelining the difference with or without a replica was within the
  run to run noise (10 to 20 percent was common in the main benchmarks).
- The replica stayed within about 2 ms of the primary in every probe, and
  caught up in under a millisecond once the load stopped. The probe checks the
  replica roughly every 0.2 ms, so delays below that are at the limit of what
  this method can see.
- This is one machine, over loopback, with no real network between the two
  servers. Across machines, network latency would add to every number in the
  lag table.
- Full sync for 1,000,000 keys was faster than Redis here. Redis sends its
  copy in its own binary format while mini redis sends plain commands; the
  sizes of the two copies were not compared, only small string values were
  tried, and nothing was measured over a real network.

### What replication does not do

Said plainly, because real Redis does all of these:

- **No partial resync.** After any dropped link the replica takes a full copy
  again, even if it missed only one write. Redis keeps a backlog and sends only
  what was missed.
- **No compatibility with real Redis replication.** A mini redis replica cannot
  follow a real Redis primary, and the reverse, because Redis sends its full
  copy in its own binary format. Pointing one at the other fails safely and
  retries, and the replica keeps its data.
- **No waiting for replicas.** There is no `WAIT` command. A write the primary
  has confirmed can be lost if the primary dies before sending it on.
- **No chains, no failover.** A replica cannot have replicas of its own, and
  nothing promotes a replica automatically when the primary dies (no Sentinel
  or Cluster).
- **No authentication.** Anyone who can connect can run `REPLICAOF` and point
  the server at an address of their choosing, or run `SYNC` and get a full
  copy of the data. This matches the rest of mini redis, which has no
  passwords; it is why the server listens only on `127.0.0.1` by default.
- **A primary restarted with no saved data wipes its replicas**: they copy the
  now empty primary. Real Redis has the same danger; use `--aof` on a primary
  that has replicas.
- **The full copy is held in memory** on the primary until it is sent, one per
  replica, and building it pauses the primary briefly (352 ms for 1,000,000
  keys above). At most 8 replicas can be attached at once.
- **A replica that falls too far behind is cut off** once its unsent changes
  pass the `--replbuf` limit (256 MB by default), and then takes a full copy.
  If the full copy itself is larger than that limit, the replica can never
  finish syncing.
- **Host names are looked up on the main thread**, so a slow DNS server can
  pause the replica briefly while it reconnects.

## Design notes and known limits

These are choices I made on purpose to keep the code small enough to read. Real
Redis does better on each of them.

- **Resizing the hash table is done all at once.** One insert out of many
  thousands pauses while every key is moved. Redis spreads this over many
  operations. Expect latency spikes at the 99.9th percentile on large data
  sets.
- **`REWRITEAOF` and `KEYS` block the server** while they run. Redis forks a
  child process for the rewrite.
- **`fsync` runs on the main thread**, so a slow disk can briefly pause
  everything. Redis does it on a background thread.
- Only string values, one database, no replication, no transactions, no
  pub/sub, no authentication. It listens on `127.0.0.1` by default for that
  reason.
- A command with a very large number of arguments that arrives slowly is
  re parsed on each packet. Arguments are capped at one million and each
  argument at 64 MB.
- Linux only (uses `epoll`). On a Mac, use the Docker setup below.

## Layout

```
src/util.c      number parsing, glob matching, clocks
src/buf.c       growable byte buffer with a read cursor
src/dict.c      hash table
src/resp.c      protocol parser and encoder
src/commands.c  every command
src/aof.c       logging, replay, fsync, rewrite
src/server.c    event loop and connection handling
src/repl.c      replication, both the primary and the replica side
src/replproto.c building the full copy, reading the primary's first reply
tests/          unit, integration, replication, comparison with Redis, benchmarks
```

## Running it on a Mac with Docker

The server needs Linux, so on a Mac everything runs in a container. The
`Dockerfile` starts from the official Redis 7.0.15 image, so the real Redis
used by the comparison tests and benchmarks is included.

```
docker build -t miniredis .
docker run --rm miniredis make test
docker run --rm miniredis bash -c "make mini-redis && cd tests && python3 bench_repl.py"
```

The first command builds the image, the second runs every test, the third runs
the replication measurements.
