# ---- build stage ----
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    make \
    pkg-config \
    libtorrent-rasterbar-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# copy source
COPY src/ /work/src/

# build btclient
WORKDIR /work/src
RUN cmake -S . -B build \
 && cmake --build build -j"$(nproc)"

# ---- runtime stage ----
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libtorrent-rasterbar2.0 \
    libcurl4 \
    ca-certificates \
    bash \
 && rm -rf /var/lib/apt/lists/*

# app paths
WORKDIR /app

# copy built binary
COPY --from=builder /work/src/build/btclient /usr/local/bin/btclient

# copy torrent + optional baked-in preseed data
COPY docker-assets/torrents/ /opt/torrents/
COPY docker-assets/preseed/ /opt/preseed/

# default writable dirs
RUN mkdir -p /app/data /app/artifacts

# copy entrypoint
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]