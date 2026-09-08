FROM alpine:3.22 AS builder

RUN apk add --no-cache \
    build-base \
    cmake \
    boost-dev \
    libevent-dev \
    sqlite-dev \
    zeromq-dev \
    coreutils \
    binutils

WORKDIR /opt/qtc

COPY . .

WORKDIR /opt/qtc/build

RUN cmake .. \
    -DCMAKE_INSTALL_PREFIX="/usr/local/" \
    -DBUILD_DAEMON="ON" \
    -DBUILD_CLI="ON" \
    -DENABLE_WALLET="ON" \
    -DWITH_ZMQ="ON" \
    -DBUILD_TESTS="OFF" \
    -DBUILD_GUI="OFF" \
    -DBUILD_TX="OFF" \
    -DBUILD_UTIL="OFF" \
    -DBUILD_WALLET_TOOL="OFF" \
    -DBUILD_BENCH="OFF" \
    -DBUILD_FUZZ_BINARY="OFF" \
    -DBUILD_UTIL_CHAINSTATE="OFF" \
    -DWITH_BDB="OFF" \
    -DWITH_USDT="OFF" \
    -DINSTALL_MAN="OFF" \
    -DWITH_CCACHE="OFF"

RUN cmake --build . --parallel $(nproc)
RUN cmake --install .

RUN strip --strip-unneeded /usr/local/bin/*

FROM alpine:3.22 AS final

RUN apk add --no-cache \
    libevent \
    sqlite-libs \
    zeromq \
    boost-system \
    boost-filesystem \
    boost-program_options

COPY --from=builder /usr/local/bin/qtcd     /usr/local/bin/qtcd
COPY --from=builder /usr/local/bin/qtc-cli  /usr/local/bin/qtc-cli

RUN addgroup -S qtc && \
    adduser -S -G qtc -H -s /bin/false qtc && \
    mkdir -p /etc/qtc /var/lib/qtcd && \
    chown -R qtc:qtc /etc/qtc /var/lib/qtcd

WORKDIR /var/lib/qtcd
EXPOSE 19335 19334
USER qtc
VOLUME ["/var/lib/qtcd"]

ENTRYPOINT ["qtcd", "-conf=/etc/qtc/qtc.conf", "-datadir=/var/lib/qtcd"]

HEALTHCHECK --interval=5m --timeout=15s --start-period=2m \
    CMD ["qtc-cli", "-conf=/etc/qtc/qtc.conf", "-datadir=/var/lib/qtcd", "getblockchaininfo"]
