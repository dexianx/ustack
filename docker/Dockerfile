FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang libclang-rt-18-dev llvm lld gdb make \
        iproute2 iputils-ping netcat-openbsd curl socat iperf3 wrk \
        tcpdump tshark python3 kmod ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
RUN apt-get update && apt-get install -y --no-install-recommends ethtool \
    && rm -rf /var/lib/apt/lists/*
