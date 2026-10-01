# ustack: planning record

This document records how the plan for ustack was made. There are three plans. Each plan has a hard critique.
The third plan is the plan that was built. Section 4 records what changed during the build.

## 0. Checks before the plan

Before the plan, we checked the platform in Docker Desktop.

| Check | Result |
|---|---|
| Kernel | `6.12.76-linuxkit`, arm64, 10 vCPU |
| `ip tuntap add … mode tun` with `--cap-add=NET_ADMIN --device=/dev/net/tun` | Works |
| `tc qdisc add dev tun0 root netem loss 10% delay 5ms` | Works |
| `ip link add veth0 type veth` | Works |

The risky parts of the platform work. Thus, the tests can use the real kernel `netem` to drop packets.

## 1. Plan 1: follow the tutorial

The steps are: open the TUN device, then IPv4 and ICMP, then the TCP handshake, then data and a retransmit timer,
then FIN, then a small HTTP server. The design has one blocking `read()` loop, a linked list of connections,
one `malloc` for each segment, a retransmit queue of segment copies, a fixed 64 KB window, and no TCP options.

### Critique of plan 1

- **It is the student version.** It is the `level-ip` tutorial with new names. A reviewer who knows the tutorial gives it less value.
- **The numbers are bad.** Go-back-N with one retransmit timer and no SACK fails under loss.
  Each lost retransmission costs a full RTO of 200 ms or more.
  At 10% loss, a 100 MB file has approximately 720 RTO stalls. The transfer takes minutes.
- **It is not testable.** The protocol code calls `read()` and `write()` on a real device.
  Each test needs root, a TUN device and a kernel peer. A bug that needs a specific packet order cannot be replayed.
- **"It works with curl" is not a test.** One good run shows nothing about wraparound, reordering, duplicates,
  zero windows or simultaneous close.
- **It has no concurrency.** It has one thread, and a lookup in a linked list is O(n) for each packet.
- **It has no baseline.** "200 MB/s" has no meaning without a comparison.

## 2. Plan 2: a real stack

- **A pure protocol core.** The stack is a value with two inputs: `input(packet, now)` and `advance(now)`.
  All output goes through a `netif->tx` callback. The core does not call the OS and does not read a clock.
- **Deterministic simulation tests**, as in FoundationDB and TigerBeetle. Two stacks exchange data over a simulated
  link with seeded loss, reordering, duplication, corruption and delay, on virtual time.
  Thousands of seeds run, and the invariants are checked after each event.
- **TCP options:** MSS, window scaling, timestamps (RFC 7323) and SACK (RFC 2018).
- **Loss recovery:** a SACK scoreboard and fast retransmit. Congestion control is pluggable: NewReno and CUBIC.
- **Timers and lookup:** a hashed timer wheel with O(1) operations, and a 4-tuple hash table with a keyed hash.
- **An event-driven socket API:** `us_listen`, `us_connect`, `us_read`, `us_write`, `us_close`.

### Critique of plan 2

- **A lost retransmission still needs an RTO.** Duplicate-ACK and SACK fast retransmit cannot find a lost
  *retransmission*. At 10% loss, approximately 1 in 10 repairs is lost again. Thus, plan 2 still stops at the RTO floor.
  The correct fix is time-based detection, RACK-TLP (RFC 8985), as in Linux.
  RACK needs a send time for each segment, so the sender needs a ring of records.
- **One core, one system call for each packet.** TUN does one packet for each `read` or `write`.
  At MTU 1500 on one core, this limit is a few hundred MB/s. A real NIC uses segmentation offload to go faster.
  TUN has the same feature through `IFF_VNET_HDR` and `TUNSETOFFLOAD`.
  We must build it as an optimization and measure it before and after. It must not be hidden as a default.
- **Multiple cores.** `IFF_MULTI_QUEUE` gives one file descriptor for each queue.
  The kernel sends each flow to one queue with a symmetric hash. This permits one shard for each core with no
  shared data, as in Seastar. The data path then has no locks and no atomic operations.
- **No fuzzing.** A packet parser that reads bytes from the network must be fuzzed.
  The pure core makes a libFuzzer target easy.
- **No benchmark method.** Each benchmark must have several runs, a median and a range, the exact command,
  and the same test against **Linux TCP**.
- **netem works in one direction only.** A root qdisc on `tun0` drops only kernel-to-stack packets.
  The stack-to-kernel direction needs an `ifb` redirect or loss injection in the driver. We must cover both directions.

## 3. Plan 3: the final plan

Plan 3 is plan 2 plus: RACK-TLP, multi-queue shards, TSO/GSO offload with measurements, a libFuzzer target,
sanitizer builds, a `tshark` check of the packet format, and a benchmark harness with a Linux kernel baseline.

### Critique of plan 3

- **The scope is large.** To control the risk, each milestone ends with a working demo, and the cut list is
  written before the build.
- **Partial SACK coverage.** Linux splits a buffer when a SACK edge falls inside it. ustack marks a record as SACKed
  only when a SACK block covers all of it. This is safe. The worst case is that the stack sends bytes again that
  the receiver already has. Without offload, each record is one MSS or less, and SACK edges fall on record
  boundaries. Thus, the cost is zero.
- **The comparison with the kernel is not perfect.** The kernel peer and the load generator use the same 10 vCPUs
  as ustack. Each number in the results says this.

The critique accepts plan 3. **This is the plan that was built.** The figures in the README show the design.

### Benchmark method

- All tests run in one container on one VM with 10 vCPUs. The load generator uses the same cores.
- Each benchmark has one warm-up run and 5 measured runs. The result is the median, with the minimum and maximum.
- Each throughput run has a SHA-256 check. A fast run with bad data is a failure.
- **The baseline** is Linux to Linux between two containers, with the same MTU and the same `netem` loss.
- Each result links to the script that produced it.

### Milestones

| # | Milestone | Done when | Cut line |
|---|---|---|---|
| M0 | Tools | Docker image, Makefile, build with `-Werror`, unit test runner | — |
| M1 | Layer 3 | `ping 10.0.0.2` gets replies; UDP echo; ICMP port unreachable | Can ship |
| M2 | TCP core | Handshake, data, FIN and RST, TIME_WAIT, options, RTO; `curl` works | **Minimum project** |
| M3 | Simulation | Two stacks with loss, reordering and duplication; invariants pass over thousands of seeds | |
| M4 | Recovery | SACK, RACK-TLP, NewReno and CUBIC; 100 MB intact at 10% loss | **Strong project** |
| M5 | Scale | Multi-queue shards, 10,000 connections, `wrk` scales with shards | |
| M6 | Offload | `vnet_hdr` TSO/GSO; throughput before and after | |
| M7 | Hardening | Fuzzing with 0 crashes, ASan and UBSan, clean `tshark` captures | |
| M8 | Numbers | Benchmark harness, kernel baseline, results, README | |

**The cut list, written before the build:** IPv6, IP fragment reassembly, ARP and Ethernet (TUN is layer 3), ECN,
TCP Fast Open, pacing, PRR, SYN cookies.

### Questions to prepare for a review

- Why TIME_WAIT exists (late duplicates, and a lost final ACK), and why the active closer holds it.
- What the pseudo-header checksum protects, and why the one's-complement sum does not depend on byte order (RFC 1071 §2).
- How the incremental checksum update works (RFC 1624). The ICMP echo reply uses it.
- Sequence wraparound, `(int32_t)(a - b) < 0`, and PAWS.
- Why RACK finds a lost retransmission and duplicate-ACK counting does not.
- Why window scaling is necessary: the bandwidth-delay product is larger than 64 KB.
- Why Nagle and delayed ACK together cause 40 ms stalls.
- How a multi-queue TUN device sends each flow to one queue, and why shared-nothing removes the locks.
- What TSO and GSO do, and why a partial checksum is correct for local delivery.

## 4. What changed during the build

All milestones M0 to M8 are complete. Nothing on the cut list was necessary.
These assumptions were wrong, and these were the responses:

| Assumption | What happened | Response |
|---|---|---|
| `ifb` gives `netem` in both directions | The Docker Desktop kernel has no `ifb` | Real `netem` from kernel to stack; loss injected by the driver (`--drop-tx`) from stack to kernel |
| Each connection can allocate 1 MiB rings at once | 10,000 connections reserved 20 GB; ASan made this visible | The rings grow when necessary and release memory when empty: 10.4 KB for each HTTP connection |
| The sanitizers can run on macOS | The macOS 27 `dyld` stops inside the ASan initializers | The sanitizers run on Linux in Docker and in CI |
| The default tun queue of 500 packets is sufficient | 16,000 drops at 1,000 connections | `--txqlen`, default 4096 |
| A Docker bridge gives a fair Linux baseline | veth TSO made `netem` drop 64 KB super-packets, which helped Linux | TSO, GSO and GRO are off on both baseline containers |
| A TLP with new data is the best probe (RFC 8985) | The Linux receiver held the ACK for up to 200 ms | The TLP resends the last segment: 5% loss went from 27.0 s to 15.7 s |

The simulator was worth its cost. In its first hour, it found four protocol bugs that a `curl` test does not show.
It now runs 100,000 random connections in approximately two minutes.
