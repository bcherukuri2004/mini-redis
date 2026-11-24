"""Replication tests: real primary and replica processes over TCP.

Every server here is the sanitizer build when it exists (make test builds
it), and every test checks that no sanitizer report was printed."""
import os
import signal
import socket
import threading
import time

import pytest

from helpers import Client, Err, free_port, start_mini, start_real_redis

READONLY = "READONLY You can't write against a read only replica."


# ---------------------------------------------------------------- helpers

def info(cl):
    out = {}
    for line in cl.cmd("INFO").decode().split("\r\n"):
        if ":" in line and not line.startswith("#"):
            k, v = line.split(":", 1)
            out[k] = v
    return out


def wait_until(fn, timeout=10.0, what="condition"):
    deadline = time.time() + timeout
    while True:
        last = fn()
        if last:
            return last
        if time.time() > deadline:
            raise AssertionError("timed out waiting for " + what)
        time.sleep(0.02)


def link_up(cl):
    return info(cl).get("master_link_status") == "up"


def caught_up(pc, rc):
    """True once the replica has applied every byte the primary sent."""
    p, r = info(pc), info(rc)
    return r.get("master_link_status") == "up" and p["master_repl_offset"] == r["slave_repl_offset"]


def snapshot(cl):
    """Every key with its value and whether it has a TTL."""
    keys = sorted(cl.cmd("KEYS", "*"))
    return {k: (cl.cmd("GET", k), cl.cmd("PTTL", k) >= 0) for k in keys}


def stop_clean(*servers):
    for s in servers:
        err = s.stop()
        assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err


@pytest.fixture
def pair():
    """A primary and a replica that has finished its first full sync."""
    p = start_mini()
    r = start_mini(extra=["--replicaof", "127.0.0.1:%d" % p.port])
    pc, rc = p.client(), r.client()
    wait_until(lambda: link_up(rc), what="replica link")
    yield p, r, pc, rc
    pc.close()
    rc.close()
    stop_clean(r, p)


# ---------------------------------------------------------------- full copy

def test_full_copy_of_existing_data():
    p = start_mini()
    pc = p.client()
    for i in range(2000):
        pc.send("SET", "k%d" % i, "v%d" % i)
    for _ in range(2000):
        pc.read()
    pc.cmd("SET", b"bin\x00\r\nkey", b"\x00\r\n$*\xff")
    pc.cmd("SET", "empty", "")
    pc.cmd("SET", "ttl", "x", "EX", "1000")
    pc.cmd("SET", "short", "x", "PX", "1")
    time.sleep(0.05)  # "short" has expired on the primary: it must not be copied

    r = start_mini()
    rc = r.client()
    rc.cmd("SET", "stale", "old data the full copy must replace")
    assert rc.cmd("REPLICAOF", "127.0.0.1", p.port) == "OK"
    wait_until(lambda: link_up(rc), what="replica link")

    assert rc.cmd("DBSIZE") == 2003
    assert rc.cmd("GET", "stale") is None
    assert rc.cmd("GET", "k1999") == b"v1999"
    assert rc.cmd("GET", b"bin\x00\r\nkey") == b"\x00\r\n$*\xff"
    assert rc.cmd("GET", "empty") == b""
    assert 990_000 < rc.cmd("PTTL", "ttl") <= 1_000_000
    assert rc.cmd("EXISTS", "short") == 0
    i = info(rc)
    assert i["role"] == "slave" and i["full_syncs_completed"] == "1" and i["last_full_sync_keys"] == "2003"
    assert info(pc)["connected_slaves"] == "1"
    pc.close()
    rc.close()
    stop_clean(r, p)


def test_full_copy_of_empty_primary(pair):
    p, r, pc, rc = pair
    assert rc.cmd("DBSIZE") == 0
    assert info(rc)["full_syncs_completed"] == "1"


# ---------------------------------------------------------------- live stream

def test_every_write_command_reaches_the_replica(pair):
    p, r, pc, rc = pair
    pc.cmd("SET", "a", "1")
    pc.cmd("SET", "b", "2", "EX", "500")
    pc.cmd("SET", "c", "3", "PXAT", str(int(time.time() * 1000) + 600_000))
    pc.cmd("SET", "b", "22", "KEEPTTL")
    pc.cmd("SET", "nx", "1", "NX")
    pc.cmd("SET", "nx", "2", "NX")      # does nothing, must not be sent
    pc.cmd("SET", "xx", "1", "XX")      # does nothing, must not be sent
    pc.cmd("MSET", "m1", "x", "m2", "y")
    pc.cmd("INCR", "n")
    pc.cmd("INCRBY", "n", "41")
    pc.cmd("DECR", "n")
    pc.cmd("DECRBY", "n", "10")
    pc.cmd("EXPIRE", "a", "300")
    pc.cmd("PERSIST", "a")
    pc.cmd("PEXPIRE", "m1", "400000")
    pc.cmd("DEL", "m2", "nosuchkey")
    assert isinstance(pc.cmd("INCR", "m1"), Err)  # "x" is not a number: nothing sent
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert snapshot(rc) == snapshot(pc)
    assert rc.cmd("GET", "n") == b"31"
    assert rc.cmd("GET", "b") == b"22" and rc.cmd("TTL", "b") > 0
    assert rc.cmd("TTL", "a") == -1

    pc.cmd("FLUSHALL")
    wait_until(lambda: caught_up(pc, rc), what="FLUSHALL to arrive")
    assert rc.cmd("DBSIZE") == 0


def test_twenty_thousand_pipelined_increments(pair):
    p, r, pc, rc = pair
    for _ in range(20000):
        pc.send("INCR", "counter")
    for _ in range(20000):
        pc.read()
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert rc.cmd("GET", "counter") == b"20000"


def test_large_value_is_copied_whole(pair):
    p, r, pc, rc = pair
    big = os.urandom(5 * 1024 * 1024)
    pc.cmd("SET", "big", big)
    wait_until(lambda: caught_up(pc, rc), what="5 MB value")
    assert rc.cmd("GET", "big") == big


def test_three_replicas_all_match():
    p = start_mini()
    rs = [start_mini(extra=["--replicaof", "127.0.0.1:%d" % p.port]) for _ in range(3)]
    pc = p.client()
    rcs = [r.client() for r in rs]
    for rc in rcs:
        wait_until(lambda: link_up(rc), what="replica link")
    for i in range(3000):
        pc.send("SET", "k%d" % (i % 500), i)
    for _ in range(3000):
        pc.read()
    want = snapshot(pc)
    for rc in rcs:
        wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
        assert snapshot(rc) == want
    assert info(pc)["connected_slaves"] == "3"
    for cl in rcs + [pc]:
        cl.close()
    stop_clean(*rs, p)


def test_offsets_match_after_writes_stop(pair):
    p, r, pc, rc = pair
    for i in range(100):
        pc.cmd("SET", "k", i)
    wait_until(lambda: caught_up(pc, rc), what="offsets to match")
    target = int(info(pc)["master_repl_offset"])
    # The primary also learns the replica's offset from its acks, sent once
    # a second. The heartbeat PING keeps moving the primary's own offset, so
    # check the ack covers everything written above rather than an exact match.
    def acked():
        fields = dict(f.split("=") for f in info(pc)["slave0"].split(","))
        return int(fields["offset"]) >= target
    wait_until(acked, timeout=5, what="replica ack")


# ---------------------------------------------------------------- read only

@pytest.mark.parametrize("cmd", [
    ["SET", "k", "v"], ["DEL", "k"], ["INCR", "k"], ["DECR", "k"], ["INCRBY", "k", "1"], ["DECRBY", "k", "1"],
    ["MSET", "a", "1"], ["EXPIRE", "k", "5"], ["PEXPIRE", "k", "5"], ["EXPIREAT", "k", "5"],
    ["PEXPIREAT", "k", "5"], ["PERSIST", "k"], ["FLUSHALL"],
])
def test_replica_refuses_writes(pair, cmd):
    p, r, pc, rc = pair
    pc.cmd("SET", "k", "from primary")
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert rc.cmd(*cmd) == Err(READONLY)
    assert rc.cmd("GET", "k") == b"from primary"


def test_replica_serves_reads_and_checks_arity_first(pair):
    p, r, pc, rc = pair
    pc.cmd("MSET", "a", "1", "b", "2")
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert rc.cmd("MGET", "a", "b", "c") == [b"1", b"2", None]
    assert rc.cmd("EXISTS", "a", "b") == 2
    assert rc.cmd("TYPE", "a") == "string"
    assert rc.cmd("PING") == "PONG"
    # Wrong number of arguments is reported before READONLY, as in Redis.
    assert rc.cmd("SET", "k") == Err("ERR wrong number of arguments for 'set' command")


# ---------------------------------------------------------------- REPLICAOF

def test_replicaof_argument_errors():
    s = start_mini()
    c = s.client()
    for bad in ["abc", "70000", "-1", "", "1.5", "99999999999999999999"]:
        assert c.cmd("REPLICAOF", "127.0.0.1", bad) == Err("ERR Invalid master port"), bad
    assert c.cmd("REPLICAOF", "x") == Err("ERR wrong number of arguments for 'replicaof' command")
    assert c.cmd("REPLICAOF", "a", "b", "c") == Err("ERR wrong number of arguments for 'replicaof' command")
    assert c.cmd("REPLICAOF", "", "1") == Err("ERR Invalid master host")
    assert c.cmd("REPLICAOF", "h" * 300, "1") == Err("ERR Invalid master host")
    assert c.cmd("REPLICAOF", "no", "one") == "OK"  # already a primary: harmless
    assert c.cmd("SET", "k", "v") == "OK"
    c.close()
    stop_clean(s)


def test_replicaof_same_primary_twice(pair):
    p, r, pc, rc = pair
    assert rc.cmd("REPLICAOF", "127.0.0.1", p.port) == "OK Already connected to specified master"
    assert rc.cmd("SLAVEOF", "127.0.0.1", p.port) == "OK Already connected to specified master"


def test_replicaof_no_one_keeps_data_and_accepts_writes(pair):
    p, r, pc, rc = pair
    pc.cmd("SET", "kept", "yes")
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert rc.cmd("REPLICAOF", "NO", "ONE") == "OK"
    assert info(rc)["role"] == "master"
    assert rc.cmd("GET", "kept") == b"yes"
    assert rc.cmd("SET", "own", "write") == "OK"
    pc.cmd("SET", "kept", "changed later")
    wait_until(lambda: info(pc)["connected_slaves"] == "0", what="primary to see the replica leave")
    time.sleep(0.3)
    assert rc.cmd("GET", "kept") == b"yes"


def test_switching_to_another_primary_replaces_data():
    a, b = start_mini(), start_mini()
    ac, bc = a.client(), b.client()
    ac.cmd("SET", "from", "a")
    bc.cmd("SET", "from", "b")
    bc.cmd("SET", "only_b", "1")
    r = start_mini(extra=["--replicaof", "127.0.0.1:%d" % a.port])
    rc = r.client()
    wait_until(lambda: link_up(rc), what="link to a")
    assert rc.cmd("GET", "from") == b"a"
    assert rc.cmd("REPLICAOF", "127.0.0.1", b.port) == "OK"
    wait_until(lambda: rc.cmd("GET", "from") == b"b", what="switch to b")
    assert rc.cmd("GET", "only_b") == b"1"
    for cl in (ac, bc, rc):
        cl.close()
    stop_clean(r, a, b)


def test_no_chained_replication(pair):
    p, r, pc, rc = pair
    # A replica will not serve as a primary for another replica...
    assert rc.cmd("SYNC") == Err("ERR this server is a replica; chained replication is not supported")
    # ...and a primary with replicas will not become a replica.
    other = start_mini()
    assert pc.cmd("REPLICAOF", "127.0.0.1", other.port) == \
        Err("ERR this server has replicas attached; chained replication is not supported")
    assert info(pc)["role"] == "master"
    stop_clean(other)


# ---------------------------------------------------------------- expiry

def test_replica_hides_expired_keys_but_waits_for_primary_to_delete(pair):
    p, r, pc, rc = pair
    pc.cmd("SET", "soon", "x", "PX", "300")
    wait_until(lambda: caught_up(pc, rc), what="replica to catch up")
    assert rc.cmd("GET", "soon") == b"x"
    time.sleep(0.4)
    # Expired: hidden from reads on the replica, whether or not the primary
    # has sent its DEL yet.
    assert rc.cmd("GET", "soon") is None
    assert rc.cmd("TTL", "soon") == -2
    assert rc.cmd("EXISTS", "soon") == 0
    # The primary's background sweep deletes it and tells the replica.
    wait_until(lambda: rc.cmd("DBSIZE") == 0, what="primary DEL to reach replica")


# ---------------------------------------------------------------- failures

def test_replica_killed_and_restarted_catches_up():
    p = start_mini()
    pc = p.client()
    rport = free_port()
    r = start_mini(port=rport, extra=["--replicaof", "127.0.0.1:%d" % p.port])
    rc = r.client()
    wait_until(lambda: link_up(rc), what="replica link")
    for i in range(1000):
        pc.cmd("SET", "before%d" % i, i)
    wait_until(lambda: caught_up(pc, rc), what="first catch up")
    rc.close()
    r.stop(signal.SIGKILL)
    wait_until(lambda: info(pc)["connected_slaves"] == "0", what="primary to notice")

    for i in range(1000):
        pc.cmd("SET", "during%d" % i, i)
    pc.cmd("DEL", "before0")

    r = start_mini(port=rport, extra=["--replicaof", "127.0.0.1:%d" % p.port])
    rc = r.client()
    wait_until(lambda: caught_up(pc, rc), what="catch up after restart")
    assert snapshot(rc) == snapshot(pc)
    assert rc.cmd("DBSIZE") == 1999
    pc.close()
    rc.close()
    stop_clean(r, p)


def test_primary_killed_and_restarted_with_its_log(tmp_path):
    aof = tmp_path / "primary.aof"
    pport = free_port()
    p = start_mini(aof=aof, fsync="always", port=pport)
    pc = p.client()
    r = start_mini(extra=["--replicaof", "127.0.0.1:%d" % pport])
    rc = r.client()
    wait_until(lambda: link_up(rc), what="replica link")
    pc.cmd("SET", "a", "1")
    wait_until(lambda: caught_up(pc, rc), what="catch up")
    pc.close()
    p.stop(signal.SIGKILL)

    # The replica keeps serving its data while the primary is gone.
    wait_until(lambda: info(rc)["master_link_status"] == "down", what="link down")
    assert rc.cmd("GET", "a") == b"1"
    assert rc.cmd("SET", "a", "2") == Err(READONLY)
    assert r.alive

    p = start_mini(aof=aof, fsync="always", port=pport)
    pc = p.client()
    pc.cmd("SET", "b", "2")
    wait_until(lambda: rc.cmd("GET", "b") == b"2", timeout=10, what="reconnect")
    assert rc.cmd("GET", "a") == b"1"
    assert int(info(rc)["full_syncs_completed"]) == 2
    pc.close()
    rc.close()
    stop_clean(r, p)


def test_replicaof_a_port_with_nothing_on_it():
    port = free_port()
    r = start_mini()
    rc = r.client()
    rc.cmd("SET", "mine", "1")
    assert rc.cmd("REPLICAOF", "127.0.0.1", port) == "OK"
    time.sleep(1.5)  # at least one retry
    assert r.alive
    assert info(rc)["master_link_status"] == "down"
    assert rc.cmd("GET", "mine") == b"1"  # old data kept until a copy arrives
    # A primary that appears later is picked up by the retry loop.
    p = start_mini(port=port)
    pc = p.client()
    pc.cmd("SET", "theirs", "2")
    wait_until(lambda: link_up(rc), timeout=5, what="late primary")
    assert rc.cmd("GET", "theirs") == b"2" and rc.cmd("GET", "mine") is None
    pc.close()
    rc.close()
    stop_clean(r, p)


def test_replicaof_unresolvable_host():
    r = start_mini()
    rc = r.client()
    assert rc.cmd("REPLICAOF", "no.such.host.invalid", "6379") == "OK"
    time.sleep(1.2)
    assert r.alive and info(rc)["master_link_status"] == "down"
    assert rc.cmd("REPLICAOF", "NO", "ONE") == "OK"
    rc.close()
    stop_clean(r)


def test_slow_replica_is_dropped_then_recovers():
    p = start_mini(extra=["--replbuf", "200000"])
    pc = p.client()
    r = start_mini(extra=["--replicaof", "127.0.0.1:%d" % p.port])
    rc = r.client()
    wait_until(lambda: link_up(rc), what="replica link")

    r.proc.send_signal(signal.SIGSTOP)  # the replica stops reading
    value = "x" * 1000
    # 20 MB of changes: more than the kernel's socket buffers can hold, so
    # the unsent part piles up on the primary and passes the 200 KB limit.
    for i in range(20000):
        pc.send("SET", "k%d" % (i % 100), value)
    for _ in range(20000):
        pc.read()
    wait_until(lambda: info(pc)["replicas_dropped_slow"] == "1", what="primary to drop the replica")
    assert info(pc)["connected_slaves"] == "0"
    pc.cmd("SET", "after", "drop")

    r.proc.send_signal(signal.SIGCONT)
    wait_until(lambda: caught_up(pc, rc) and rc.cmd("GET", "after") == b"drop", timeout=15, what="recovery")
    assert snapshot(rc) == snapshot(pc)
    pc.close()
    rc.close()
    stop_clean(r, p)


def test_pointing_at_real_redis_fails_safely():
    real = start_real_redis()
    Client(real.port).cmd("SET", "real", "data")
    r = start_mini()
    rc = r.client()
    rc.cmd("SET", "mine", "1")
    assert rc.cmd("REPLICAOF", "127.0.0.1", real.port) == "OK"
    time.sleep(2.5)  # several attempts
    assert r.alive
    assert info(rc)["master_link_status"] == "down"
    assert rc.cmd("GET", "mine") == b"1"
    assert rc.cmd("GET", "real") is None
    rc.close()
    err = r.stop()
    assert "did not answer SYNC as mini redis does" in err
    assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err
    real.stop()


# ---------------------------------------------------------------- a primary that misbehaves

class FakePrimary:
    """A TCP server that answers SYNC with whatever bytes a test gives it."""

    def __init__(self, reply, then_close=True):
        self.reply, self.then_close = reply, then_close
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.conns = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.conns.append(conn)
            try:
                conn.recv(100)  # the SYNC request
                conn.sendall(self.reply)
                if self.then_close:
                    conn.close()
            except OSError:
                pass

    def close(self):
        self.sock.close()
        for c in self.conns:
            try:
                c.close()
            except OSError:
                pass


def enc(*args):
    return Client.encode(*args)


@pytest.mark.parametrize("reply", [
    b"",                                              # closes at once
    b"+FULLSYNC 0\r\n",                               # header, then closes
    b"+FULLSYNC 0\r\n" + enc("SET", "new", "1"),      # half a copy, then closes
    b"+FULLSYNC 0\r\n" + enc("SET", "new", "1")[:-3], # a command cut in half
    b"+FULLSYNC 0\r\n*1\r\n$abc\r\n",                 # protocol garbage
    b"+FULLSYNC 0\r\n" + os.urandom(3000),            # random bytes
    b"-ERR go away\r\n",
    b"$100\r\nREDIS0010" + b"\x00" * 50,              # what real Redis sends
    b"+FULLSYNC -5\r\n",
    b"+FULLSYNC " + b"9" * 100 + b"\r\n",
])
def test_bad_primary_never_corrupts_replica_data(reply):
    fake = FakePrimary(reply)
    r = start_mini()
    rc = r.client()
    rc.cmd("SET", "mine", "kept")
    assert rc.cmd("REPLICAOF", "127.0.0.1", fake.port) == "OK"
    time.sleep(1.5)
    assert r.alive
    # An incomplete or broken copy is thrown away: the old data stays, and
    # nothing from the half copy shows up.
    assert rc.cmd("GET", "mine") == b"kept"
    assert rc.cmd("GET", "new") is None
    assert info(rc)["master_link_status"] == "down"
    rc.close()
    fake.close()
    stop_clean(r)


def test_reads_during_a_slow_full_copy_see_the_old_data():
    # The fake primary sends half a copy and then holds the connection open.
    fake = FakePrimary(b"+FULLSYNC 0\r\n" + enc("SET", "mine", "replaced"), then_close=False)
    r = start_mini()
    rc = r.client()
    rc.cmd("SET", "mine", "old")
    rc.cmd("REPLICAOF", "127.0.0.1", fake.port)
    wait_until(lambda: info(rc)["master_sync_in_progress"] == "1", what="sync to start")
    time.sleep(0.3)
    assert rc.cmd("GET", "mine") == b"old"
    assert rc.cmd("DBSIZE") == 1
    rc.close()
    fake.close()
    stop_clean(r)


def test_replica_garbage_does_not_crash_primary(pair):
    p, r, pc, rc = pair
    # A client pretends to be a replica, then sends junk acks and garbage.
    s = Client(p.port)
    s.send("SYNC")
    assert s._line().startswith(b"+FULLSYNC ")
    s.send("REPLCONF", "ACK", "notanumber")
    s.send("REPLCONF", "ACK", "-1")
    s.send("REPLCONF", "ACK")
    s.send("SYNC")
    s.send_raw(os.urandom(2000))
    time.sleep(0.3)
    s.close()
    pc.cmd("SET", "still", "fine")
    wait_until(lambda: caught_up(pc, rc), what="real replica unaffected")
    assert rc.cmd("GET", "still") == b"fine"
    assert p.alive


# ---------------------------------------------------------------- persistence on the replica

def test_replica_with_its_own_log_restarts_with_the_copied_data(tmp_path):
    p = start_mini()
    pc = p.client()
    for i in range(500):
        pc.cmd("SET", "k%d" % i, i)
    raof = tmp_path / "replica.aof"
    r = start_mini(aof=raof, fsync="always", extra=["--replicaof", "127.0.0.1:%d" % p.port])
    rc = r.client()
    wait_until(lambda: link_up(rc), what="replica link")
    pc.cmd("SET", "live", "1")
    pc.cmd("DEL", "k0")
    wait_until(lambda: caught_up(pc, rc), what="catch up")
    want = snapshot(rc)
    rc.close()
    stop_clean(r)

    # Restart as a plain server from its own log only.
    r = start_mini(aof=raof)
    rc = r.client()
    assert snapshot(rc) == want
    assert rc.cmd("DBSIZE") == 500
    rc.close()
    pc.close()
    stop_clean(r, p)


# ---------------------------------------------------------------- limits (security review fixes)

def test_too_many_replicas_are_refused():
    p = start_mini()
    pc = p.client()
    pc.cmd("SET", "k", "v")
    fakes = []
    for _ in range(8):
        f = Client(p.port)
        f.send("SYNC")
        assert f._line().startswith(b"+FULLSYNC ")
        fakes.append(f)
    ninth = Client(p.port)
    assert ninth.cmd("SYNC") == Err("ERR too many replicas")
    assert info(pc)["connected_slaves"] == "8"
    # A slot frees up when one leaves.
    fakes.pop().close()
    wait_until(lambda: info(pc)["connected_slaves"] == "7", what="slot to free")
    ninth.send("SYNC")
    assert ninth._line().startswith(b"+FULLSYNC ")
    for f in fakes + [ninth, pc]:
        f.close()
    stop_clean(p)


def test_replica_connection_cannot_pile_up_replies():
    # A connection that becomes a replica and then floods the server with
    # reads it never collects is cut off by the replica limit as soon as its
    # input is processed, not only at the next heartbeat.
    p = start_mini(extra=["--replbuf", "1000000"])
    pc = p.client()
    pc.cmd("SET", "big", "x" * 500_000)
    f = Client(p.port)
    f.send("SYNC")
    assert f._line().startswith(b"+FULLSYNC ")
    f.sock.sendall(Client.encode("GET", "big") * 40)  # 20 MB of replies, never read
    assert f.at_eof(wait=5)
    assert p.alive
    # It was the reply limit that closed it, not the slow replica check.
    assert info(pc)["replicas_dropped_slow"] == "0"
    pc.close()
    stop_clean(p)
