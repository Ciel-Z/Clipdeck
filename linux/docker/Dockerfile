# syntax=docker/dockerfile:1.7
FROM debian:buster-slim

ENV DEBIAN_FRONTEND=noninteractive

# Debian 10 is the closest public build baseline to UOS 20 (glibc 2.28).
# Buster is archived, so the image deliberately uses archive.debian.org.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && sed -i \
      -e 's|deb.debian.org/debian|archive.debian.org/debian|g' \
      -e 's|security.debian.org/debian-security|archive.debian.org/debian-security|g' \
      -e '/buster-updates/d' \
      /etc/apt/sources.list \
    && apt-get \
      -o Acquire::Check-Valid-Until=false \
      -o Acquire::Retries=10 \
      -o Acquire::http::Pipeline-Depth=0 \
      update \
    && apt-get \
      -o Acquire::Check-Valid-Until=false \
      -o Acquire::Retries=10 \
      -o Acquire::http::Pipeline-Depth=0 \
      install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      dpkg-dev \
      fakeroot \
      file \
      ninja-build \
      pkg-config \
      qtbase5-dev \
      qtdeclarative5-dev \
      qtwebengine5-dev \
      libqt5svg5 \
      libqt5sql5-sqlite \
      libqt5x11extras5-dev \
      libsqlite3-dev \
      libx11-dev \
      libxcomposite-dev \
      libxext-dev \
      libxrender-dev \
      libxtst-dev \
      libxcb1-dev

# LAN sync needs libsodium >= 1.0.18 (ristretto255); Debian 10 ships 1.0.17.
# Built static from the official release, pinned by its SHA-256.
ARG SODIUM_VERSION=1.0.20
ARG SODIUM_SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
RUN curl -fsSL -o /tmp/libsodium.tar.gz \
      "https://download.libsodium.org/libsodium/releases/libsodium-${SODIUM_VERSION}.tar.gz" \
    && echo "${SODIUM_SHA256}  /tmp/libsodium.tar.gz" | sha256sum -c - \
    && mkdir -p /tmp/libsodium && tar -xzf /tmp/libsodium.tar.gz -C /tmp/libsodium --strip-components=1 \
    && cd /tmp/libsodium \
    && ./configure --prefix=/opt/libsodium --disable-shared --enable-static --with-pic \
    && make -j"$(nproc)" && make install \
    && rm -rf /tmp/libsodium /tmp/libsodium.tar.gz

WORKDIR /workspace
