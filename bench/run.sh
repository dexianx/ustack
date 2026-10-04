#!/usr/bin/env bash
# Benchmarks against the Linux kernel peer. Runs inside the dev container.
# Every number is the median of RUNS runs after one warm-up; min/max are reported alongside.
set -uo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-build-linux}
RUNS=${RUNS:-5}
STACK=10.0.0.2
OUT=${OUT:-bench/results/results.tsv}
mkdir -p "$(dirname "$OUT")"
: >"$OUT"
PID=

start() {
    "./$BIN/ustack" --pin "$@" 2>/tmp/ustack.log &
    PID=$!
    for _ in $(seq 50); do ip link show tun0 2>/dev/null | grep -q UP && return; sleep 0.05; done
    cat /tmp/ustack.log; exit 1
}

stop() {
    tc qdisc del dev tun0 root 2>/dev/null
    kill -INT "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    sleep 0.3
}

# summarize NAME UNIT VALUES... -> median/min/max line, appended to $OUT
summarize() {
    local name=$1 unit=$2; shift 2
    printf '%s\n' "$@" | sort -g | awk -v name="$name" -v unit="$unit" '
        { v[NR] = $1 }
        END {
            med = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
            printf "%s\t%.4g\t%.4g\t%.4g\t%s\n", name, med, v[1], v[NR], unit
        }' | tee -a "$OUT"
}

repeat() {
    local name=$1 unit=$2; shift 2
    [ "${WARMUP:-1}" = 1 ] && "$@" >/dev/null
    local vals=()
    for _ in $(seq "$RUNS"); do vals+=("$("$@")"); done
    summarize "$name" "$unit" "${vals[@]}"
}

download_gbps() {
    curl -s -o /dev/null -w '%{speed_download}' "http://$STACK/bytes/$1" | awk '{ printf "%.3f", $1 * 8 / 1e9 }'
}

upload_gbps() {
    local start end
    start=$(date +%s%N)
    head -c "$1" /dev/zero | nc -N "$STACK" 9
    end=$(date +%s%N)
    awk -v b="$1" -v ns=$((end - start)) 'BEGIN { printf "%.3f", b * 8 / ns }'
}

wrk_run() {
    local field=$1; shift
    wrk "$@" --latency "http://$STACK/" 2>/dev/null | awk -v f="$field" '
        function us(s) { if (s ~ /us$/) return s + 0; if (s ~ /ms$/) return s * 1000; return s * 1e6 }
        f == "rps" && /Requests\/sec/ { printf "%.0f", $2 }
        f == "p50" && $1 == "50%"     { printf "%.1f", us($2) }
        f == "p99" && $1 == "99%"     { printf "%.1f", us($2) }'
}

ping_rtt_us() {
    ping -c 2000 -i 0.0005 -q "$STACK" | awk -F'/' '/rtt/ { printf "%.1f", $5 * 1000 }'
}

loss_download_s() {
    local start end
    start=$(date +%s%N)
    curl -sf --max-time 900 "http://$STACK/bytes/$1" | sha256sum >/tmp/loss.sha
    end=$(date +%s%N)
    [ "$(cut -d' ' -f1 </tmp/loss.sha)" = "$("./$BIN/genbytes" "$1" | sha256sum | cut -d' ' -f1)" ] || echo "CORRUPT" >&2
    awk -v ns=$((end - start)) 'BEGIN { printf "%.2f", ns / 1e9 }'
}

echo "== latency"
start
repeat "ping RTT (avg of 2000)" us ping_rtt_us
stop

rss_kb() { awk '/VmRSS/ { print $2 }' "/proc/$PID/status"; }

echo "== memory (fresh process per sample)"
mem=()
for _ in $(seq "$RUNS"); do
    start --msl 500 >/dev/null
    before=$(rss_kb)
    python3 bench/hold_conns.py 10000 4 >/dev/null &
    holder=$!
    sleep 3
    after=$(rss_kb)
    wait "$holder"
    stop
    mem+=("$(( (after - before) * 1024 / 10000 ))")
done
summarize "RSS per established connection (10k held)" bytes "${mem[@]}"

echo "== bulk throughput, MTU 1500, 1 connection"
start
repeat "download 1 GB" Gbit/s download_gbps 1000000000
repeat "upload 1 GB (discard)" Gbit/s upload_gbps 1000000000
stop
start --offload
repeat "download 1 GB, TSO/GSO offload" Gbit/s download_gbps 1000000000
repeat "upload 1 GB, TSO/GSO offload" Gbit/s upload_gbps 1000000000
stop

echo "== HTTP/1.1 keep-alive (wrk, 10 s)"
for shards in 1 2 4; do
    start --shards "$shards"
    repeat "req/s, 100 conns, $shards shard(s)" req/s wrk_run rps -t2 -c100 -d10s
    repeat "req/s, 1000 conns, $shards shard(s)" req/s wrk_run rps -t4 -c1000 -d10s
    stop
done
start
repeat "p50 latency, 100 conns" us wrk_run p50 -t2 -c100 -d10s
repeat "p99 latency, 100 conns" us wrk_run p99 -t2 -c100 -d10s
stop
start --shards 4
repeat "req/s, 10000 concurrent conns, 4 shards" req/s wrk_run rps -t4 -c10000 -d10s
stop

echo "== connection churn (new TCP connection per request)"
for shards in 1 4; do
    start --shards "$shards" --msl 1000
    repeat "conn/s, $shards shard(s)" conn/s wrk_run rps -t4 -c200 -d10s -H "Connection: close"
    stop
done

if [ "${LOSS:-1}" = 1 ]; then
    echo "== 100 MB download under bidirectional random loss (SHA-256 verified)"
    for loss in 1 5 10; do
        start --drop-tx "0.$(printf %02d "$loss")"
        tc qdisc add dev tun0 root netem loss "${loss}%"
        WARMUP=0 RUNS=${LOSS_RUNS:-3} repeat "100 MB at ${loss}% loss, ustack" s loss_download_s 100000000
        stop
    done
fi

echo "results written to $OUT"
