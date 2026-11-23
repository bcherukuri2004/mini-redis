# Linux build and test environment for mini redis.
# Starts from the official Redis 7.0.15 image so the real redis server and
# redis benchmark are present for the comparison tests and the benchmark.
FROM redis:7.0.15-bookworm

# Compiler, make, Python with pytest, and taskset (part of util linux).
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential python3 python3-pytest util-linux \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# The official image starts redis by default; we want a shell instead.
ENTRYPOINT []
CMD ["bash"]
