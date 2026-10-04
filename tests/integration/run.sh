#!/usr/bin/env bash
# Interop tests against the Linux kernel TCP/IP stack. Runs inside the dev container.
set -uo pipefail
cd "$(dirname "$0")/../.."

BIN=${BIN:-build-linux}
STACK=10.0.0.2
PASS=0
FAIL=0
PID=

start() {
    "./$BIN/ustack" "$@" 2>/tmp/ustack.log &
    PID=$!
    for _ in $(seq 50); do ip link show tun0 2>/dev/null | grep -q UP && return; sleep 0.05; done
    echo "ustack failed to start"; cat /tmp/ustack.log; exit 1
}

stop() {
    tc qdisc del dev tun0 root 2>/dev/null
    kill -INT "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
}

check() {
    local name=$1; shift
    if "$@" >/tmp/check.out 2>&1; then
        PASS=$((PASS + 1)); printf '  ok    %s\n' "$name"
    else
        FAIL=$((FAIL + 1)); printf '  FAIL  %s\n' "$name"; sed 's/^/        /' /tmp/check.out | tail -5
    fi
}

expected_sha() { "./$BIN/genbytes" "$1" | sha256sum | cut -d' ' -f1; }

download_ok() {
    local got
    got=$(curl -sf --max-time "${2:-120}" "http://$STACK/bytes/$1" | sha256sum | cut -d' ' -f1)
    [ "$got" = "$(expected_sha "$1")" ]
}

upload_ok() {
    head -c "$1" /dev/urandom >/tmp/upload.bin
    local want got
    want=$(sha256sum </tmp/upload.bin | cut -d' ' -f1)
    got=$(curl -sf --max-time 600 -H 'Expect:' --data-binary @/tmp/upload.bin "http://$STACK/sha256")
    [ "$got" = "$want" ]
}

echo_ok() {
    head -c "$1" /dev/urandom >/tmp/echo.bin
    [ "$(nc -N "$STACK" 7 </tmp/echo.bin | sha256sum)" = "$(sha256sum </tmp/echo.bin)" ]
}

concurrent_ok() {
    local n=$1 pids=() fails=0
    for i in $(seq "$n"); do
        (download_ok $((100000 + i * 1000)) 60) &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p" || fails=$((fails + 1)); done
    [ "$fails" -eq 0 ]
}

pcap_clean() {
    tcpdump -i tun0 -s 0 -w /tmp/cap.pcap 2>/dev/null &
    local dump=$!
    sleep 0.5
    download_ok 2000000 && upload_ok 500000 && ping -c 3 -i 0.2 -q "$STACK" >/dev/null && echo_ok 100000
    sleep 0.5; kill -INT "$dump"; wait "$dump"
    local frames bad
    frames=$(tshark -r /tmp/cap.pcap 2>/dev/null | wc -l)
    bad=$(tshark -r /tmp/cap.pcap -o ip.check_checksum:TRUE -o tcp.check_checksum:TRUE \
        -o udp.check_checksum:TRUE \
        -Y 'ip.checksum.status == "Bad" || tcp.checksum.status == "Bad" || icmp.checksum.status == "Bad" || _ws.malformed' \
        2>/dev/null | wc -l)
    echo "frames=$frames bad=$bad"
    cp /tmp/cap.pcap "${PCAP_OUT:-/tmp/ustack.pcap}" 2>/dev/null
    [ "$frames" -gt 1000 ] && [ "$bad" -eq 0 ]
}

stats_field() { curl -s "http://$STACK/stats" | sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p"; }

echo "== baseline (1 shard)"
start
check "ICMP echo: 200 pings, 0% loss"     sh -c "ping -c 200 -i 0.002 -q $STACK | grep -q ' 0% packet loss'"
check "ICMP echo: 1400-byte payload"      ping -c 3 -i 0.2 -s 1400 -q "$STACK"
check "UDP echo"                          sh -c "[ \"\$(echo udp-roundtrip | nc -u -w1 $STACK 7)\" = udp-roundtrip ]"
check "TCP RST on closed port"            sh -c "! nc -z -w2 $STACK 4321"
check "HTTP GET /"                        sh -c "curl -sf http://$STACK/ | grep -q 'Hello from ustack'"
check "HTTP keep-alive: 100 requests"     sh -c "curl -sf $(printf "http://$STACK/ %.0s" $(seq 100)) | grep -c Hello | grep -qx 100"
check "HTTP 404"                          sh -c "[ \"\$(curl -s -o /dev/null -w '%{http_code}' http://$STACK/nope)\" = 404 ]"
check "download 1 B"                      download_ok 1
check "download 100 MB, SHA-256 match"    download_ok 100000000
check "download 1 GB, SHA-256 match"      download_ok 1000000000
check "upload 64 MB, SHA-256 match"       upload_ok 67108864
check "echo 20 MB full duplex"            echo_ok 20000000
check "32 concurrent downloads"           concurrent_ok 32
check "pcap: tshark finds 0 bad checksums" pcap_clean
check "no checksum or header errors"      sh -c "[ \"$(stats_field tcp_bad)\" = 0 ] && [ \"$(stats_field ip_bad)\" = 0 ]"
stop

echo "== 4 shards (multi-queue tun)"
start --shards 4
check "64 concurrent downloads"           concurrent_ok 64
check "download 100 MB"                   download_ok 100000000
stop

echo "== TSO/GSO offload"
start --offload
check "download 1 GB with offload"        download_ok 1000000000
check "upload 64 MB with offload"         upload_ok 67108864
check "echo 20 MB with offload"           echo_ok 20000000
stop

for loss in 1 5 10; do
    echo "== ${loss}% loss each direction (netem kernel->stack, injected stack->kernel)"
    start --drop-tx "0.$(printf %02d "$loss")"
    tc qdisc add dev tun0 root netem loss "${loss}%"
    check "download 10 MB intact"         download_ok 10000000 300
    check "upload 4 MB intact"            upload_ok 4194304
    check "echo 2 MB intact"              echo_ok 2000000
    stop
done

echo "== reordering + duplication + corruption"
start
tc qdisc add dev tun0 root netem delay 1ms 2ms reorder 25% 50% duplicate 5% corrupt 2%
check "upload 16 MB intact"               upload_ok 16777216
check "corrupted segments rejected"       sh -c "[ \"$(stats_field tcp_bad)\" -gt 0 ] || [ \"$(stats_field ip_bad)\" -gt 0 ]"
stop

echo
echo "integration: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
