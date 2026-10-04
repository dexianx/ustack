#!/usr/bin/env bash
# Linux-to-Linux reference: the same 100 MB transfer between two containers on a
# Docker bridge, with netem loss on each container's egress. Runs on the host.
set -uo pipefail
cd "$(dirname "$0")/.."
RUNS=${RUNS:-3}
OUT=${OUT:-bench/results/kernel_baseline.tsv}
BYTES=100000000
: >"$OUT"

docker network create ustack-bench >/dev/null 2>&1
cleanup() { docker rm -f kb-server kb-client >/dev/null 2>&1; }
trap cleanup EXIT
cleanup
for name in kb-server kb-client; do
    docker run -d --name "$name" --network ustack-bench --cap-add=NET_ADMIN \
        -v "$PWD":/work -w /work ustack-dev sleep infinity >/dev/null
    docker exec "$name" ethtool -K eth0 tso off gso off gro off >/dev/null
done

for loss in 1 5 10; do
    for c in kb-server kb-client; do
        docker exec "$c" sh -c "tc qdisc replace dev eth0 root netem loss ${loss}%"
    done
    vals=()
    for _ in $(seq "$RUNS"); do
        docker exec -d kb-server sh -c "./build-linux/genbytes $BYTES | nc -l -p 8080 -q 0"
        sleep 1
        vals+=("$(docker exec kb-client bash -c '
            s=$(date +%s%N); nc -d kb-server 8080 | sha256sum >/tmp/sha; e=$(date +%s%N)
            awk -v ns=$((e - s)) "BEGIN { printf \"%.2f\", ns / 1e9 }"')")
    done
    printf '%s\n' "${vals[@]}" | sort -g | awk -v loss="$loss" '{ v[NR] = $1 }
        END { printf "100 MB at %s%% loss, Linux kernel\t%.4g\t%.4g\t%.4g\ts\n", loss, v[int((NR + 1) / 2)], v[1], v[NR] }' | tee -a "$OUT"
done
