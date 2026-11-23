"""Differential test: run the same random commands against real Redis and
this server and require identical answers. Real Redis is the reference, so
any disagreement is a bug here (or a place where we must document a gap)."""
import random

import pytest

from helpers import Err, start_mini, start_real_redis

KEYS = ["k%d" % i for i in range(10)] + ["user:1", "user:2", "hello", "hallo"]
VALUES = ["", "0", "1", "-5", "42", "007", "abc", "hello world", "9223372036854775807", "-9223372036854775808",
          "1.5", " 1", "x" * 300, "12345678901234567890"]
GLOBS = ["*", "k*", "k?", "user:*", "h[ae]llo", "h*o", "[^k]*", "nomatch*", "k[0-4]", "*:?"]
TIMING_SENSITIVE = {"TTL", "PTTL"}


def pick(rng, items):
    return rng.choice(items)


def gen_command(rng):
    r = rng.random()
    k = pick(rng, KEYS)
    v = pick(rng, VALUES)
    if r < 0.22:
        cmd = ["SET", k, v]
        if rng.random() < 0.4:
            cmd.append(pick(rng, ["NX", "XX", "KEEPTTL"]))
        if rng.random() < 0.25:
            # Times in the past are left out on purpose: such keys vanish when
            # each server's background sweep happens to run, which makes DBSIZE
            # differ for a moment. Those cases are covered in test_integration.
            opt = pick(rng, ["EX", "PX", "EXAT", "PXAT"])
            if opt in ("EXAT", "PXAT"):
                good = "99999999999" if opt == "EXAT" else "99999999999999"
            else:
                good = pick(rng, ["100000", "200000000"])
            cmd += [opt, pick(rng, [good, good, "0", "-1", "abc", "9223372036854775807"])]
        if rng.random() < 0.05:
            cmd.append(pick(rng, ["BOGUS", "NX", "EX"]))
        return cmd
    if r < 0.38:
        return ["GET", k]
    if r < 0.44:
        return ["DEL"] + [pick(rng, KEYS) for _ in range(rng.randint(1, 3))]
    if r < 0.48:
        return ["EXISTS"] + [pick(rng, KEYS) for _ in range(rng.randint(1, 3))]
    if r < 0.58:
        c = pick(rng, ["INCR", "DECR"])
        return [c, k]
    if r < 0.64:
        return [pick(rng, ["INCRBY", "DECRBY"]), k, pick(rng, ["1", "-7", "100", "9223372036854775807", "-9223372036854775808", "abc", "0"])]
    if r < 0.68:
        n = rng.randint(1, 3)
        args = []
        for _ in range(n):
            args += [pick(rng, KEYS), pick(rng, VALUES)]
        if rng.random() < 0.1:
            args.pop()
        return ["MSET"] + args
    if r < 0.72:
        return ["MGET"] + [pick(rng, KEYS) for _ in range(rng.randint(1, 4))]
    if r < 0.78:
        return [pick(rng, ["EXPIRE", "PEXPIRE"]), k, pick(rng, ["100000", "5000000", "0", "-3", "abc"])]
    if r < 0.80:
        return [pick(rng, ["EXPIREAT", "PEXPIREAT"]), k, pick(rng, ["99999999999", "99999999999999", "1", "abc"])]
    if r < 0.85:
        return [pick(rng, ["TTL", "PTTL"]), k]
    if r < 0.87:
        return ["PERSIST", k]
    if r < 0.89:
        return ["TYPE", k]
    if r < 0.92:
        return ["KEYS", pick(rng, GLOBS)]
    if r < 0.94:
        return ["DBSIZE"]
    if r < 0.95:
        return ["FLUSHALL"]
    if r < 0.97:
        return pick(rng, [["PING"], ["PING", "x"], ["ECHO", "hi"], ["PING", "a", "b"], ["GET"], ["SET", "a"], ["DBSIZE", "x"], ["INCR"]])
    return ["EXISTS"]


def normalize(cmd, reply):
    if isinstance(reply, Err):
        # Messages must match exactly, except the unknown command text, which
        # real Redis extends with the arguments.
        return ("error", str(reply))
    if cmd[0] == "KEYS":
        return sorted(reply)
    return reply


def same(cmd, a, b):
    if cmd[0] in TIMING_SENSITIVE and isinstance(a, int) and isinstance(b, int):
        if a < 0 or b < 0:
            return a == b
        slack = 2 if cmd[0] == "TTL" else 200
        return abs(a - b) <= slack
    return normalize(cmd, a) == normalize(cmd, b)


@pytest.mark.parametrize("seed", range(1, 41))
def test_random_commands_match_real_redis(seed):
    real = start_real_redis()
    mini = start_mini()
    assert real.alive and mini.alive
    rc, mc = real.client(), mini.client()
    rng = random.Random(seed)
    history = []
    try:
        for step in range(4000):
            cmd = gen_command(rng)
            history.append(cmd)
            ra = rc.cmd(*cmd)
            ma = mc.cmd(*cmd)
            if not same(cmd, ra, ma):
                tail = "\n".join(" ".join(map(str, h)) for h in history[-8:])
                pytest.fail("seed %d step %d: %r\n  redis: %r\n  mini:  %r\nlast commands:\n%s" % (seed, step, cmd, ra, ma, tail))
        # Final state must be identical key by key.
        rkeys = sorted(rc.cmd("KEYS", "*"))
        assert rkeys == sorted(mc.cmd("KEYS", "*"))
        for key in rkeys:
            assert rc.cmd("GET", key) == mc.cmd("GET", key)
    finally:
        err = mini.stop()
        real.stop()
    assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err
