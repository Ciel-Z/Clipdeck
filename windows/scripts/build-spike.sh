#!/usr/bin/env bash
# Builds the Phase 0 Windows spike from macOS/Linux via Docker:
#   bash windows/scripts/build-spike.sh
# Output: artifacts/windows/ClipDeckSpike.zip
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image_name="clipdeck-windows-builder:qt6.5-mingw"

# A host proxy on 127.0.0.1 is unreachable from containers; route RUN steps
# through host.docker.internal instead. Pulling the base image still uses the
# Docker daemon's own proxy settings.
container_proxy="${https_proxy:-${HTTPS_PROXY:-}}"
container_proxy="${container_proxy/127.0.0.1/host.docker.internal}"
container_proxy="${container_proxy/localhost/host.docker.internal}"

if [[ "${CLIPDECK_REBUILD_IMAGE:-0}" == "1" ]] \
  || ! docker image inspect "${image_name}" >/dev/null 2>&1; then
  docker buildx build \
    --platform linux/amd64 \
    --load \
    --build-arg "http_proxy=${container_proxy}" \
    --build-arg "https_proxy=${container_proxy}" \
    --build-arg "HTTP_PROXY=${container_proxy}" \
    --build-arg "HTTPS_PROXY=${container_proxy}" \
    --tag "${image_name}" \
    --file "${repo_root}/windows/docker/Dockerfile" \
    "${repo_root}/windows/docker"
fi

docker run --rm \
  --platform linux/amd64 \
  --network none \
  --env "CLIPDECK_SKIP_WINE=${CLIPDECK_SKIP_WINE:-0}" \
  --volume "${repo_root}:/workspace" \
  --workdir /workspace \
  "${image_name}" \
  bash windows/scripts/cross-build-spike.sh
