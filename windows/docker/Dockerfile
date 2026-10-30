FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build-time only; set by windows/scripts/build-spike.sh from the host proxy.
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY=${http_proxy}
ARG HTTPS_PROXY=${https_proxy}

# MinGW-w64 13 (posix threads, SEH) is ABI compatible with the MinGW 11.2
# toolchain that builds the official Qt 6.5 win64_mingw binaries. Wine is only
# used for a headless smoke test; it cannot validate DWM effects.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && echo "Acquire::Retries \"10\";" > /etc/apt/apt.conf.d/80-retries \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      ninja-build \
      g++ \
      libgl1-mesa-dri \
      g++-mingw-w64-x86-64-posix \
      mingw-w64-tools \
      python3-pip \
      python3-venv \
      wine64 \
      wine \
      xvfb \
      xauth \
      zip \
    && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix \
    && update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix

ARG QT_VERSION=6.5.3
RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.1.21 \
    && /opt/aqt/bin/aqt install-qt linux desktop ${QT_VERSION} gcc_64 -O /opt/qt \
    && /opt/aqt/bin/aqt install-qt windows desktop ${QT_VERSION} win64_mingw -O /opt/qt \
    && rm -rf /opt/qt/${QT_VERSION}/gcc_64/doc /opt/qt/${QT_VERSION}/mingw_64/doc

ENV QT_VERSION=${QT_VERSION} \
    QT_HOST_PATH=/opt/qt/${QT_VERSION}/gcc_64 \
    QT_WINDOWS_PATH=/opt/qt/${QT_VERSION}/mingw_64 \
    WINEDEBUG=-all
