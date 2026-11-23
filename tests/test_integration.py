"""End to end tests: start the real server binary and talk to it over TCP."""
import os
import signal
import socket
import threading
import time

import pytest

from helpers import Client, Err, start_mini


@pytest.fixture
def server():
    s = start_mini()
    assert s.alive
    yield s
    err = s.stop()
    # The sanitizer build prints a report on any memory error.
    assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err


@pytest.fixture
def c(server):
    cl = server.client()
    yield cl
    cl.close()


# ---------------------------------------------------------------- basics

def test_ping_echo(c):
    assert c.cmd("PING") == "PONG"
    assert c.cmd("PING", "hi") == b"hi"
    assert c.cmd("ECHO", "x y") == b"x y"
    assert isinstance(c.cmd("PING", "a", "b"), Err)


def test_set_get_overwrite_delete(c):
    assert c.cmd("GET", "k") is None
    assert c.cmd("SET", "k", "v1") == "OK"
    assert c.cmd("GET", "k") == b"v1"
    assert c.cmd("SET", "k", "v2") == "OK"
    assert c.cmd("GET", "k") == b"v2"
    assert c.cmd("DEL", "k", "k", "missing") == 1
    assert c.cmd("GET", "k") is None
    assert c.cmd("DBSIZE") == 0


def test_binary_safe_values(c):
    value = bytes(range(256)) + b"\r\n$3\r\n*1\r\n"
    assert c.cmd("SET", b"k\x00ey", value) == "OK"
    assert c.cmd("GET", b"k\x00ey") == value
    assert c.cmd("SET", "empty", "") == "OK"
    assert c.cmd("GET", "empty") == b""


def test_mset_mget_exists(c):
    assert c.cmd("MSET", "a", 1, "b", 2) == "OK"
    assert c.cmd("MGET", "a", "nope", "b") == [b"1", None, b"2"]
    assert c.cmd("EXISTS", "a", "a", "nope") == 2
    assert isinstance(c.cmd("MSET", "a", 1, "b"), Err)


def test_incr_family(c):
    assert c.cmd("INCR", "n") == 1
    assert c.cmd("INCRBY", "n", 41) == 42
    assert c.cmd("DECR", "n") == 41
    assert c.cmd("DECRBY", "n", 50) == -9
    assert c.cmd("GET", "n") == b"-9"
    c.cmd("SET", "s", "abc")
    assert c.cmd("INCR", "s") == Err("ERR value is not an integer or out of range")
    assert isinstance(c.cmd("INCRBY", "n", "x"), Err)
    c.cmd("SET", "big", 2**63 - 1)
    assert "overflow" in c.cmd("INCR", "big")
    assert c.cmd("GET", "big") == str(2**63 - 1).encode()


def test_keys_patterns(c):
    for k in ["user:1", "user:2", "admin:1", "hello", "hallo", "hxllo"]:
        c.cmd("SET", k, 1)
    assert sorted(c.cmd("KEYS", "*")) == sorted([b"user:1", b"user:2", b"admin:1", b"hello", b"hallo", b"hxllo"])
    assert sorted(c.cmd("KEYS", "user:*")) == [b"user:1", b"user:2"]
    assert sorted(c.cmd("KEYS", "h[ae]llo")) == [b"hallo", b"hello"]
    assert sorted(c.cmd("KEYS", "h?llo")) == [b"hallo", b"hello", b"hxllo"]
    assert c.cmd("KEYS", "nothing*") == []


def test_flushall_and_type(c):
    c.cmd("SET", "a", 1)
    assert c.cmd("TYPE", "a") == "string"
    assert c.cmd("TYPE", "b") == "none"
    assert c.cmd("FLUSHALL") == "OK"
    assert c.cmd("DBSIZE") == 0
    assert c.cmd("FLUSHALL", "ASYNC") == "OK"
    assert isinstance(c.cmd("FLUSHALL", "bogus"), Err)


def test_errors(c):
    assert c.cmd("NOPE").startswith("ERR unknown command")
    assert c.cmd("GET") == Err("ERR wrong number of arguments for 'get' command")
    assert c.cmd("SET", "a") == Err("ERR wrong number of arguments for 'set' command")
    assert c.cmd("SET", "a", "b", "BOGUS") == Err("ERR syntax error")
    assert c.cmd("SET", "a", "b", "NX", "XX") == Err("ERR syntax error")
    assert c.cmd("SET", "a", "b", "EX", 0).startswith("ERR invalid expire")
    assert c.cmd("SET", "a", "b", "EX", "x") == Err("ERR value is not an integer or out of range")
    assert c.cmd("SET", "a", "b", "EX") == Err("ERR syntax error")
    # The connection is still usable after errors.
    assert c.cmd("PING") == "PONG"


def test_commands_are_case_insensitive(c):
    assert c.cmd("set", "k", "v") == "OK"
    assert c.cmd("GeT", "k") == b"v"


def test_compat_commands_for_cli_tools(c):
    assert c.cmd("COMMAND") == []
    assert c.cmd("CONFIG", "GET", "save") == []
    assert c.cmd("CLIENT", "SETNAME", "x") == "OK"
    assert c.cmd("SELECT", 0) == "OK"
    assert isinstance(c.cmd("SELECT", 1), Err)
    assert b"mini_redis_version" in c.cmd("INFO")


# ---------------------------------------------------------------- SET options

def test_set_nx_xx_keepttl(c):
    assert c.cmd("SET", "k", "1", "NX") == "OK"
    assert c.cmd("SET", "k", "2", "NX") is None
    assert c.cmd("GET", "k") == b"1"
    assert c.cmd("SET", "other", "x", "XX") is None
    assert c.cmd("SET", "k", "3", "XX") == "OK"
    c.cmd("EXPIRE", "k", 100)
    c.cmd("SET", "k", "4", "KEEPTTL")
    assert 98 <= c.cmd("TTL", "k") <= 100
    c.cmd("SET", "k", "5")
    assert c.cmd("TTL", "k") == -1  # plain SET clears the TTL


# ---------------------------------------------------------------- expiry

def test_expiry_lazy(c):
    c.cmd("SET", "k", "v", "PX", 150)
    assert c.cmd("GET", "k") == b"v"
    time.sleep(0.3)
    assert c.cmd("GET", "k") is None
    assert c.cmd("EXISTS", "k") == 0


def test_expiry_active_removes_untouched_keys(c):
    for i in range(500):
        c.cmd("SET", "tmp:%d" % i, i, "PX", 100)
    c.cmd("SET", "keep", "x")
    deadline = time.time() + 3
    while time.time() < deadline and c.cmd("DBSIZE") > 1:
        time.sleep(0.1)
    # DBSIZE counts keys still in memory, so reaching 1 proves the background
    # sweep deleted them even though nobody ever read them.
    assert c.cmd("DBSIZE") == 1


def test_ttl_expire_persist(c):
    assert c.cmd("TTL", "none") == -2
    c.cmd("SET", "k", "v")
    assert c.cmd("TTL", "k") == -1
    assert c.cmd("EXPIRE", "k", 100) == 1
    assert 99 <= c.cmd("TTL", "k") <= 100
    assert 98000 <= c.cmd("PTTL", "k") <= 100000
    assert c.cmd("PERSIST", "k") == 1
    assert c.cmd("TTL", "k") == -1
    assert c.cmd("PERSIST", "k") == 0
    assert c.cmd("EXPIRE", "missing", 10) == 0
    assert c.cmd("EXPIRE", "k", 0) == 1  # a time in the past deletes the key
    assert c.cmd("GET", "k") is None
    c.cmd("SET", "k", "v")
    assert c.cmd("PEXPIREAT", "k", int(time.time() * 1000) + 100000) == 1
    assert c.cmd("EXPIREAT", "k", int(time.time()) - 5) == 1
    assert c.cmd("EXISTS", "k") == 0


def test_incr_keeps_ttl(c):
    c.cmd("SET", "n", 1, "EX", 100)
    c.cmd("INCR", "n")
    assert 98 <= c.cmd("TTL", "n") <= 100


# ---------------------------------------------------------------- protocol and I/O

def test_inline_commands(c):
    c.send_raw(b"PING\r\n")
    assert c.read() == "PONG"
    c.send_raw(b"SET inline works\r\n")
    assert c.read() == "OK"
    c.send_raw(b"GET inline\r\n")
    assert c.read() == b"works"
    c.send_raw(b"\r\n\r\nPING\r\n")  # blank lines are ignored
    assert c.read() == "PONG"


def test_pipelining(c):
    n = 20000
    c.send_raw(b"".join(c.encode("SET", "k%d" % i, i) + c.encode("GET", "k%d" % i) for i in range(n)))
    for i in range(n):
        assert c.read() == "OK"
        assert c.read() == str(i).encode()


def test_command_split_across_packets(c):
    data = c.encode("SET", "split", "x" * 1000) + c.encode("GET", "split")
    for i in range(0, len(data), 7):
        c.send_raw(data[i:i + 7])
        time.sleep(0.0005)
    assert c.read() == "OK"
    assert c.read() == b"x" * 1000


def test_one_byte_at_a_time(c):
    for b in c.encode("SET", "a", "b"):
        c.send_raw(bytes([b]))
    assert c.read() == "OK"


def test_large_value(c):
    value = os.urandom(5 * 1024 * 1024)
    assert c.cmd("SET", "big", value) == "OK"
    assert c.cmd("GET", "big") == value
    assert c.cmd("DEL", "big") == 1
    assert c.cmd("PING") == "PONG"


def test_slow_reader_gets_all_output(server):
    """The server must hold back output for a client that is not reading yet
    and deliver every byte, in order, once it does."""
    c = server.client()
    value = b"v" * 50000
    c.cmd("SET", "k", value)
    n = 1000
    c.send_raw(c.encode("GET", "k") * n)
    time.sleep(0.5)  # let the server fill its socket buffer and queue the rest
    # A second client must still get prompt answers meanwhile.
    other = server.client()
    assert other.cmd("PING") == "PONG"
    other.close()
    for _ in range(n):
        assert c.read() == value
    c.close()


def test_many_clients(server):
    clients = [server.client() for _ in range(300)]
    for i, cl in enumerate(clients):
        cl.send("SET", "c%d" % i, i)
    for cl in clients:
        assert cl.read() == "OK"
    for i, cl in enumerate(clients):
        cl.send("GET", "c%d" % ((i + 1) % 300))
    for i, cl in enumerate(clients):
        assert cl.read() == str((i + 1) % 300).encode()
    assert b"connected_clients:300" in clients[0].cmd("INFO")
    for cl in clients:
        cl.close()


def test_concurrent_incr_is_exact(server):
    """Several clients incrementing one counter must never lose an update."""
    def worker():
        cl = server.client()
        for _ in range(500):
            cl.cmd("INCR", "counter")
        cl.close()

    threads = [threading.Thread(target=worker) for _ in range(8)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    c = server.client()
    assert c.cmd("GET", "counter") == b"4000"
    c.close()


def test_protocol_error_closes_connection(server):
    for bad in (b"*abc\r\n", b"*1\r\n+OK\r\n", b"*1\r\n$3\r\nabcXY"):
        c = server.client()
        c.send_raw(bad)
        reply = c.read()
        assert isinstance(reply, Err) and "Protocol error" in reply
        assert c.at_eof()
        c.close()
    assert server.client().cmd("PING") == "PONG"  # server itself is fine


def test_client_disconnect_mid_command(server):
    for _ in range(50):
        s = socket.create_connection(("127.0.0.1", server.port))
        s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$10\r\nabc")
        s.close()
    c = server.client()
    assert c.cmd("GET", "k") is None
    assert c.cmd("PING") == "PONG"


def test_quit(c):
    assert c.cmd("QUIT") == "OK"
    assert c.at_eof()


def test_garbage_does_not_crash(server):
    import random

    rng = random.Random(1)
    for _ in range(100):
        s = socket.create_connection(("127.0.0.1", server.port))
        s.sendall(bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 300))))
        s.close()
    assert server.alive
    assert server.client().cmd("PING") == "PONG"


# ---------------------------------------------------------------- persistence

def aof_server(path, fsync="always"):
    return start_mini(aof=path, fsync=fsync)


def snapshot(c):
    """Every key with its value and rough TTL."""
    out = {}
    for k in sorted(c.cmd("KEYS", "*")):
        ttl = c.cmd("TTL", k)
        out[k] = (c.cmd("GET", k), ttl if ttl < 0 else "ttl")
    return out


def test_persistence_survives_kill_9(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path)
    c = s.client()
    c.cmd("SET", "a", "1")
    c.cmd("SET", "b", "x", "EX", 1000)
    c.cmd("MSET", "m1", "v1", "m2", "v2")
    c.cmd("INCR", "n")
    c.cmd("INCRBY", "n", 9)
    c.cmd("DEL", "m1")
    c.cmd("SET", "gone", "x")
    c.cmd("EXPIRE", "gone", 0)
    s.stop(signal.SIGKILL)  # no chance to clean up

    s2 = aof_server(path)
    c = s2.client()
    assert c.cmd("GET", "a") == b"1"
    assert c.cmd("GET", "b") == b"x" and 990 <= c.cmd("TTL", "b") <= 1000
    assert c.cmd("GET", "m1") is None and c.cmd("GET", "m2") == b"v2"
    assert c.cmd("GET", "n") == b"10"
    assert c.cmd("GET", "gone") is None
    assert c.cmd("DBSIZE") == 4
    s2.stop()


def test_replay_does_not_double_apply(tmp_path):
    path = tmp_path / "data.aof"
    for _ in range(3):
        s = aof_server(path)
        c = s.client()
        c.cmd("INCR", "n")
        c.cmd("EXPIRE", "n", 5000)
        s.stop(signal.SIGKILL)
    s = aof_server(path)
    c = s.client()
    assert c.cmd("GET", "n") == b"3"
    assert 4900 <= c.cmd("TTL", "n") <= 5000
    s.stop()


def test_expired_keys_do_not_come_back(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path)
    c = s.client()
    c.cmd("SET", "short", "x", "PX", 200)
    c.cmd("SET", "long", "y", "EX", 1000)
    s.stop(signal.SIGKILL)
    time.sleep(0.4)
    s = aof_server(path)
    c = s.client()
    assert c.cmd("GET", "short") is None
    assert c.cmd("GET", "long") == b"y"
    s.stop()


def test_flushall_persists(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path)
    c = s.client()
    c.cmd("SET", "a", 1)
    c.cmd("FLUSHALL")
    c.cmd("SET", "b", 2)
    s.stop(signal.SIGKILL)
    s = aof_server(path)
    c = s.client()
    assert c.cmd("KEYS", "*") == [b"b"]
    s.stop()


@pytest.mark.parametrize("cut", [1, 5, 13, 20])
def test_truncated_tail_is_recovered(tmp_path, cut):
    """A crash can leave half a command at the end. The server drops it,
    trims the file, and keeps accepting writes that survive another restart."""
    path = tmp_path / "data.aof"
    s = aof_server(path)
    c = s.client()
    c.cmd("SET", "keep", "me")
    c.cmd("SET", "last", "x" * 50)
    s.stop(signal.SIGKILL)
    data = path.read_bytes()
    path.write_bytes(data[:-cut])

    s = aof_server(path)
    assert s.alive
    c = s.client()
    assert c.cmd("GET", "keep") == b"me"
    assert c.cmd("GET", "last") is None
    c.cmd("SET", "after", "recovery")
    s.stop(signal.SIGKILL)

    s = aof_server(path)
    c = s.client()
    assert c.cmd("GET", "keep") == b"me" and c.cmd("GET", "after") == b"recovery"
    s.stop()


def test_corrupt_middle_refuses_to_start(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path)
    c = s.client()
    for i in range(5):
        c.cmd("SET", "k%d" % i, "v%d" % i)
    s.stop(signal.SIGKILL)
    data = bytearray(path.read_bytes())
    data[30] = ord("Z")  # damage the middle of the file
    path.write_bytes(bytes(data))
    s = aof_server(path)
    time.sleep(0.5)
    assert not s.alive and s.proc.returncode != 0
    err = s.stop()
    assert "corrupt" in err or "failed" in err


def test_rewrite_shrinks_file_and_keeps_data(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path, fsync="no")
    c = s.client()
    for i in range(2000):
        c.cmd("SET", "hot", i)  # 2000 log entries for one key
    for i in range(100):
        c.cmd("SET", "k%d" % i, i)
    c.cmd("SET", "ttl", "v", "EX", 1000)
    c.cmd("INCR", "counter")
    c.cmd("INCR", "counter")
    before = snapshot(c)
    size_before = path.stat().st_size
    assert c.cmd("REWRITEAOF") == "OK"
    size_after = path.stat().st_size
    assert size_after < size_before / 5
    assert snapshot(c) == before
    # New writes after the rewrite go to the new file and survive a crash.
    c.cmd("INCR", "counter")
    c.cmd("SET", "post", "rewrite")
    s.stop(signal.SIGTERM)

    s = aof_server(path, fsync="no")
    c = s.client()
    assert c.cmd("GET", "counter") == b"3"
    assert c.cmd("GET", "post") == b"rewrite"
    assert c.cmd("GET", "hot") == b"1999"
    assert 990 <= c.cmd("TTL", "ttl") <= 1000
    assert c.cmd("DBSIZE") == 104
    assert not (tmp_path / "data.aof.tmp").exists()
    s.stop()


def test_rewrite_without_aof_is_an_error(c):
    assert isinstance(c.cmd("REWRITEAOF"), Err)


def test_graceful_shutdown_flushes(tmp_path):
    """With fsync no, a clean SIGTERM must still get everything onto disk."""
    path = tmp_path / "data.aof"
    s = aof_server(path, fsync="no")
    c = s.client()
    for i in range(1000):
        c.cmd("SET", "k%d" % i, i)
    s.stop(signal.SIGTERM)
    assert s.proc.returncode == 0
    s = aof_server(path, fsync="no")
    assert s.client().cmd("DBSIZE") == 1000
    s.stop()


def test_everysec_policy_works(tmp_path):
    path = tmp_path / "data.aof"
    s = aof_server(path, fsync="everysec")
    c = s.client()
    c.cmd("SET", "a", 1)
    time.sleep(1.3)  # at least one fsync tick
    assert path.stat().st_size > 0
    s.stop(signal.SIGKILL)
    s = aof_server(path, fsync="everysec")
    assert s.client().cmd("GET", "a") == b"1"
    s.stop()


def test_set_with_time_already_past_stores_then_expires(c):
    """Matches real Redis 7.0: the key is stored, and gone on first access."""
    assert c.cmd("SET", "old", "v", "PXAT", 1000) == "OK"
    assert c.cmd("GET", "old") is None
    assert c.cmd("EXISTS", "old") == 0
