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
DBSIZE FLUSHALL PING ECHO SELECT INFO REWRITEAOF QUIT`, plus stubs that let
`redis-cli` and client libraries connect (`COMMAND CONFIG CLIENT`).

## How it was tested

`make test` runs all of this against a build with AddressSanitizer and
UndefinedBehaviorSanitizer, which stop the program on memory errors and
undefined behavior:

| Test | What it checks |
|---|---|
| `tests/unit.c` (100,000+ checks) | number parsing, glob matching, buffer, hash table with 200,000 keys, and the protocol parser fed every possible split point of a command |
| `tests/test_integration.py` (41 tests) | real TCP connections: every command, pipelining 20,000 commands, one byte at a time delivery, a 5 MB value, 300 clients at once, a slow reader, garbage input, and concurrent counters that must not lose an update |
| Crash recovery tests | `kill -9` then restart, replay that must not double count, expired keys that must not come back, truncated and corrupted logs, and `REWRITEAOF` followed by a crash |
| `tests/test_differential.py` | 40 random runs of 4,000 commands each (160,000 total) sent to **real Redis 7.0 and this server side by side**; every reply, including error messages, and the final data must be identical |

The comparison test found real differences while this was being written (for
example Redis reports a bad option before a bad number in `SET`), which were
fixed to match.

## Benchmarks

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
- Linux only (uses `epoll`).

## Layout

```
src/util.c      number parsing, glob matching, clocks
src/buf.c       growable byte buffer with a read cursor
src/dict.c      hash table
src/resp.c      protocol parser and encoder
src/commands.c  every command
src/aof.c       logging, replay, fsync, rewrite
src/server.c    event loop and connection handling
tests/          unit, integration, comparison with Redis, benchmark
```
