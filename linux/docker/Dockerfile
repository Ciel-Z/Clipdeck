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
      dpkg-dev \
      fakeroot \
      file \
      ninja-build \
      pkg-config \
      qtbase5-dev \
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

WORKDIR /workspace
