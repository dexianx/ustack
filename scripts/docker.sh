#!/usr/bin/env bash
# Run a command inside the dev container with the capabilities ustack needs.
set -euo pipefail
cd "$(dirname "$0")/.."
docker image inspect ustack-dev >/dev/null 2>&1 || docker build -t ustack-dev docker/
exec docker run --rm -i --cap-add=NET_ADMIN --device=/dev/net/tun \
    --ulimit nofile=1048576:1048576 -v "$PWD":/work -w /work ustack-dev bash -c "$*"
