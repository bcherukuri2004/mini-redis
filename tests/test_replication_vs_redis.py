"""Phase 3: the same replication scenarios on real Redis 7.0.15 and on mini
redis, side by side. Each test runs a scenario on both, then requires the
same outcome: the same data on the replica, the same replies, the same
error messages.

The two cannot replicate from each other (real Redis sends its full copy in
its own binary format), so each runs as its own primary and replica pair."""
import random
import signal
import time

import pytest

from helpers import Err, free_port, start_mini, start_real_redis
from test_differential import gen_command

# Real Redis waits 5 seconds by default before a diskless full sync, hoping
# more replicas will join. Turn that off so both pairs sync at once.
REAL_OPTS = ["--repl-diskless-sync-delay", "0"]


class Real:
    name = "redis"

    @staticmethod
    def primary():
        return start_real_redis(extra=REAL_OPTS)

    @staticmethod
    def replica(primary_port, port=None):
        return start_real_redis(port=port, extra=REAL_OPTS + ["--replicaof", "127.0.0.1", str(primary_port)])


class Mini:
    name = "mini"

    @staticmethod
    def primary():
        return start_mini()

    @staticmethod
    def replica(primary_port, port=None):
        return start_mini(port=port, extra=["--replicaof", "127.0.0.1:%d" % primary_port])


IMPLS = [Real, Mini]


def info(cl):
    out = {}
    for line in cl.cmd("INFO", "replication").decode().split("\r\n"):
        if ":" in line and not line.startswith("#"):
            k, v = line.split(":", 1)
            out[k] = v
    return out


def wait_until(fn, timeout=20.0, what="condition"):
    deadline = time.time() + timeout
    while not fn():
        if time.time() > deadline:
            raise AssertionError("timed out waiting for " + what)
        time.sleep(0.02)


def caught_up(pc, rc):
    p, r = info(pc), info(rc)
    return r.get("master_link_status") == "up" and p.get("master_repl_offset") == r.get("slave_repl_offset")


def state(cl):
    """Every live key, its value, and whether it has a TTL."""
    return {k: (cl.cmd("GET", k), cl.cmd("PTTL", k) >= 0) for k in sorted(cl.cmd("KEYS", "*"))}


class Pair:
    def __init__(self, impl):
        self.impl = impl
        self.p = impl.primary()
        self.rport = free_port()
        self.r = impl.replica(self.p.port, self.rport)
        self.pc, self.rc = self.p.client(), self.r.client()
        wait_until(lambda: info(self.rc).get("master_link_status") == "up", what=impl.name + " link")

    def sync(self):
        wait_until(lambda: caught_up(self.pc, self.rc), what=self.impl.name + " replica to catch up")

    def kill_replica(self):
        self.rc.close()
        self.r.stop(signal.SIGKILL)

    def restart_replica(self):
        self.r = self.impl.replica(self.p.port, self.rport)
        self.rc = self.r.client()

    def close(self):
        self.pc.close()
        self.rc.close()
        errs = [self.r.stop(), self.p.stop()]
        if self.impl is Mini:
            for err in errs:
                assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err


@pytest.fixture
def pairs():
    made = [Pair(impl) for impl in IMPLS]
    yield made
    for pr in made:
        pr.close()


# ---------------------------------------------------------------- the Phase 3 scenario

def test_write_kill_replica_keep_writing_restart_catch_up(pairs):
    """Write keys to the primary and see them on the replica. Kill the
    replica, keep writing, restart it, and check it catches up. Same steps,
    same final result, on real Redis and on mini redis."""
    for pr in pairs:
        for i in range(10_000):
            pr.pc.send("SET", "before:%d" % i, i)
        for _ in range(10_000):
            pr.pc.read()
        pr.sync()
        assert pr.rc.cmd("DBSIZE") == 10_000, pr.impl.name
        assert pr.rc.cmd("GET", "before:9999") == b"9999", pr.impl.name

        pr.kill_replica()
        for i in range(10_000):
            pr.pc.send("SET", "during:%d" % i, i)
        for _ in range(10_000):
            pr.pc.read()
        pr.pc.cmd("DEL", "before:0")
        pr.pc.cmd("INCRBY", "before:1", "100")

        pr.restart_replica()
        pr.sync()
        assert pr.rc.cmd("DBSIZE") == 19_999, pr.impl.name
        assert pr.rc.cmd("GET", "before:0") is None, pr.impl.name
        assert pr.rc.cmd("GET", "before:1") == b"101", pr.impl.name
        assert state(pr.rc) == state(pr.pc), pr.impl.name

    real, mini = pairs
    assert state(real.rc) == state(mini.rc)


# ---------------------------------------------------------------- random workloads

@pytest.mark.parametrize("seed", range(1, 11))
def test_random_workload_leaves_same_data_on_both_replicas(pairs, seed):
    """2,000 random commands (the generator from the comparison test) sent to
    each primary. Afterwards each replica must match its own primary, and
    the two replicas must match each other key by key."""
    rng = random.Random(seed)
    cmds = [gen_command(rng) for _ in range(2000)]
    for pr in pairs:
        for cmd in cmds:
            pr.pc.cmd(*cmd)
        pr.sync()
        assert state(pr.rc) == state(pr.pc), pr.impl.name
    real, mini = pairs
    assert state(real.rc) == state(mini.rc)


# ---------------------------------------------------------------- replies and errors

WRITES = [["SET", "k", "v"], ["DEL", "k"], ["INCR", "k"], ["DECRBY", "k", "2"], ["MSET", "a", "1"],
          ["EXPIRE", "k", "5"], ["PEXPIREAT", "k", "5"], ["PERSIST", "k"], ["FLUSHALL"]]


def test_replica_replies_match_redis(pairs):
    replies = {}
    for pr in pairs:
        pr.pc.cmd("SET", "k", "v")
        pr.sync()
        got = [pr.rc.cmd(*w) for w in WRITES]
        got += [
            pr.rc.cmd("GET", "k"),
            pr.rc.cmd("SET", "k"),  # arity is checked before READONLY
            pr.rc.cmd("REPLICAOF", "127.0.0.1", "abc"),
            pr.rc.cmd("REPLICAOF", "127.0.0.1", "70000"),
            pr.rc.cmd("REPLICAOF", "127.0.0.1", "-1"),
            pr.rc.cmd("REPLICAOF", "x"),
            pr.rc.cmd("REPLICAOF", "127.0.0.1", str(pr.p.port)),
            info(pr.rc)["role"],
            info(pr.pc)["role"],
            info(pr.pc)["connected_slaves"],
        ]
        replies[pr.impl.name] = got
    assert replies["redis"] == replies["mini"]
    assert replies["mini"][0] == Err("READONLY You can't write against a read only replica.")


def test_expired_key_on_replica_matches_redis(pairs):
    seen = {}
    for pr in pairs:
        pr.pc.cmd("SET", "soon", "x", "PX", "300")
        pr.sync()
        before = (pr.rc.cmd("GET", "soon"), pr.rc.cmd("EXISTS", "soon"))
        time.sleep(0.5)
        after = (pr.rc.cmd("GET", "soon"), pr.rc.cmd("EXISTS", "soon"), pr.rc.cmd("TTL", "soon"),
                 pr.rc.cmd("KEYS", "*"))
        seen[pr.impl.name] = (before, after)
    assert seen["redis"] == seen["mini"]
    assert seen["mini"] == ((b"x", 1), (None, 0, -2, []))


def test_promote_replica_matches_redis(pairs):
    seen = {}
    for pr in pairs:
        pr.pc.cmd("MSET", "a", "1", "b", "2")
        pr.sync()
        steps = [pr.rc.cmd("REPLICAOF", "NO", "ONE"), info(pr.rc)["role"], pr.rc.cmd("MGET", "a", "b"),
                 pr.rc.cmd("SET", "c", "3"), pr.rc.cmd("INCR", "a")]
        pr.pc.cmd("SET", "a", "changed on old primary")
        time.sleep(0.3)
        steps.append(pr.rc.cmd("GET", "a"))
        seen[pr.impl.name] = steps
    assert seen["redis"] == seen["mini"]
    assert seen["mini"] == ["OK", "master", [b"1", b"2"], "OK", 2, b"2"]


def test_replica_follows_primary_restart_like_redis():
    """Kill the primary (no persistence), restart it empty on the same port.
    Both kinds of replica keep serving the old data while it is down, then
    reconnect and copy the now empty primary. This is the documented danger
    of a primary without persistence, and real Redis behaves the same way."""
    seen = {}
    for impl in IMPLS:
        pport = free_port()
        p = (start_real_redis(port=pport, extra=REAL_OPTS) if impl is Real else start_mini(port=pport))
        r = impl.replica(pport)
        pc, rc = p.client(), r.client()
        wait_until(lambda: info(rc).get("master_link_status") == "up", what="link")
        pc.cmd("SET", "k", "v")
        wait_until(lambda: caught_up(pc, rc), what="catch up")
        pc.close()
        p.stop(signal.SIGKILL)
        wait_until(lambda: info(rc)["master_link_status"] == "down", what="link down")
        while_down = rc.cmd("GET", "k")
        p = (start_real_redis(port=pport, extra=REAL_OPTS) if impl is Real else start_mini(port=pport))
        pc = p.client()
        wait_until(lambda: caught_up(pc, rc), what=impl.name + " reconnect")
        seen[impl.name] = (while_down, rc.cmd("GET", "k"), rc.cmd("DBSIZE"))
        pc.close()
        rc.close()
        r.stop()
        p.stop()
    assert seen["redis"] == seen["mini"] == (b"v", None, 0)
