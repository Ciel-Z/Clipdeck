#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image_name="clipdeck-linux-builder:debian10-amd64"

if [[ "${CLIPDECK_REBUILD_IMAGE:-0}" == "1" ]] \
  || ! docker image inspect "${image_name}" >/dev/null 2>&1; then
  docker buildx build \
    --platform linux/amd64 \
    --load \
    --tag "${image_name}" \
    --file "${repo_root}/linux/docker/Dockerfile" \
    "${repo_root}"
fi

docker run --rm \
  --platform linux/amd64 \
  --network none \
  --user "$(id -u):$(id -g)" \
  --env "CLIPDECK_VERSION=${CLIPDECK_VERSION:-0.3.0~preview7}" \
  --volume "${repo_root}:/workspace" \
  --workdir /workspace \
  "${image_name}" \
  bash linux/scripts/package-deb.sh
