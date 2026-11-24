"""Shared helpers: a tiny RESP client and a way to start and stop servers."""
import os
import signal
import socket
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Err(str):
    """An error reply from the server (kept distinct from a normal string)."""


class Client:
    def __init__(self, port, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        self.sock.close()

    # ---- sending ----
    @staticmethod
    def encode(*args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if not isinstance(a, bytes):
                a = str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        return b"".join(out)

    def send(self, *args):
        self.sock.sendall(self.encode(*args))

    def send_raw(self, data):
        self.sock.sendall(data)

    # ---- receiving ----
    def _fill(self):
        chunk = self.sock.recv(1 << 20)
        if not chunk:
            raise ConnectionError("connection closed")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _exact(self, n):
        while len(self.buf) < n + 2:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n + 2:]
        return data

    def read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return Err(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if kind == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise ValueError("bad reply: %r" % line)

    def cmd(self, *args):
        self.send(*args)
        return self.read()

    def at_eof(self, wait=2.0):
        """True if the server closes the connection within `wait` seconds."""
        self.sock.settimeout(wait)
        try:
            while True:
                if not self.sock.recv(65536):
                    return True
        except socket.timeout:
            return False
        except ConnectionError:
            return True


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    """Runs a server binary in the background."""

    def __init__(self, argv, port):
        self.port = port
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        deadline = time.time() + 10
        while True:
            if self.proc.poll() is not None:
                break
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                return
            except OSError:
                if time.time() > deadline:
                    break
                time.sleep(0.02)

    @property
    def alive(self):
        return self.proc.poll() is None

    def client(self):
        return Client(self.port)

    def stop(self, sig=signal.SIGTERM):
        if getattr(self, "early_err", None) is not None:
            return self.early_err
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        err = self.proc.stderr.read().decode(errors="replace")
        self.proc.stderr.close()
        return err


def mini_binary():
    for name in ("mini-redis-asan", "mini-redis"):
        path = os.path.join(ROOT, name)
        if os.path.exists(path):
            return path
    raise FileNotFoundError("build the server first: make")


def start_mini(aof=None, fsync="everysec", port=None, extra=()):
    """Start a server. free_port() only finds a port that was free a moment
    ago; another connection in the test can take it before the server binds
    it. When the server exits saying so, try again (a new port if we picked
    it, the same port a little later if the caller asked for that one)."""
    fixed = port is not None
    for attempt in range(20):
        use = port if fixed else free_port()
        argv = [mini_binary(), "--port", str(use)]
        if aof:
            argv += ["--aof", str(aof), "--fsync", fsync]
        argv += list(extra)
        srv = Server(argv, use)
        if srv.alive:
            return srv
        err = srv.stop()
        srv.early_err = err
        if "cannot listen" not in err:
            return srv  # a real startup failure; some tests expect these
        with open(os.path.join(ROOT, ".port-retries.log"), "a") as f:
            f.write("port %d attempt %d: %s\n" % (use, attempt, err.strip().splitlines()[-1]))
        time.sleep(0.1)
    return srv


def start_real_redis():
    """Real Redis in its own empty folder. Redis writes dump.rdb into its
    folder when a replica asks it for a copy, and loads dump.rdb at startup,
    so sharing a folder would leak keys from one test into the next."""
    port = free_port()
    workdir = tempfile.mkdtemp(prefix="real-redis-")
    argv = ["redis-server", "--port", str(port), "--save", "", "--appendonly", "no", "--bind", "127.0.0.1",
            "--dir", workdir]
    return Server(argv, port)
