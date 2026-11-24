CC      ?= gcc
CFLAGS  ?= -O2 -g
WARN    := -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter
SRC     := src/util.c src/buf.c src/dict.c src/resp.c src/replproto.c src/commands.c src/aof.c src/repl.c
CORE    := src/util.c src/buf.c src/dict.c src/resp.c src/replproto.c

all: mini-redis

mini-redis: $(SRC) src/server.c src/*.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRC) src/server.c

# Same server built with AddressSanitizer and UndefinedBehaviorSanitizer,
# which catch memory errors that normal builds hide.
mini-redis-asan: $(SRC) src/server.c src/*.h
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) -o $@ $(SRC) src/server.c

tests/unit: tests/unit.c $(CORE) src/*.h
	$(CC) -O1 -g -fsanitize=address,undefined $(WARN) -Isrc -o $@ tests/unit.c $(CORE)

# Unit tests, then integration tests and the comparison against real Redis,
# all against the sanitizer build.
test: tests/unit mini-redis-asan
	./tests/unit
	python3 -m pytest -q tests/test_integration.py tests/test_differential.py tests/test_replication.py tests/test_replication_vs_redis.py

bench: mini-redis
	python3 tests/bench.py

clean:
	rm -f mini-redis mini-redis-asan tests/unit

.PHONY: all test bench clean
