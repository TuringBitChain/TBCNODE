FROM ubuntu:20.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    python3 

RUN apt-get install -y \
    libtool \
    autotools-dev \
    automake \
    pkg-config \
    libssl-dev \
    libevent-dev \
    bsdmainutils \
    libdb-dev \
    libdb++-dev

RUN apt-get install -y \
    libboost-system-dev \
    libboost-filesystem-dev \
    libboost-chrono-dev \
    libboost-program-options-dev \
    libboost-test-dev \
    libboost-thread-dev

WORKDIR /TBCNODE

COPY . .

RUN ./autogen.sh
RUN ./configure --enable-cxx --disable-shared --with-pic --enable-prod-build --prefix=/TBCNODE
RUN make -j 32
RUN make install

FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl-dev \
    libevent-dev \
    libboost-system-dev \
    libboost-filesystem-dev \
    libboost-chrono-dev \
    libboost-program-options-dev \
    libboost-thread-dev \
    libdb-dev \
    libdb++-dev \
    curl \
    ca-certificates \
    python3 \
    && curl -fsSL https://deb.nodesource.com/setup_16.x | bash - \
    && apt-get install -y nodejs \
    && npm install pm2 -g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /TBCNODE

COPY --from=builder /TBCNODE/bin /TBCNODE/bin
COPY --from=builder /TBCNODE/lib /TBCNODE/lib
COPY --from=builder /TBCNODE/include /TBCNODE/include
COPY --from=builder /TBCNODE/share /TBCNODE/share
COPY node.conf /TBCNODE/node.conf

RUN mkdir -p /TBCNODE/node_data_dir

COPY start.sh /TBCNODE/start.sh
RUN chmod +x /TBCNODE/start.sh

# 暴露RPC和P2P端口
EXPOSE 8332
EXPOSE 8333

# 为了防止配置文件换行符问题，确保配置文件使用Unix格式
RUN apt-get update && apt-get install -y dos2unix && \
    dos2unix /TBCNODE/node.conf && \
    dos2unix /TBCNODE/start.sh && \
    apt-get remove -y dos2unix && \
    apt-get autoremove -y && \
    rm -rf /var/lib/apt/lists/* && \
    # 确保数据目录有正确的权限
    mkdir -p /TBCNODE/node_data_dir/blocks /TBCNODE/node_data_dir/chainstate && \
    chmod -R 755 /TBCNODE/node_data_dir

# 健康检查
HEALTHCHECK --interval=30s --timeout=30s --start-period=5s --retries=3 \
    CMD curl -s --data-binary '{"jsonrpc": "1.0", "id":"healthcheck", "method": "getblockcount", "params": [] }' \
    -H 'content-type: text/plain;' http://tbcuser:randompasswd@127.0.0.1:8332/ || exit 1

# 设置默认启动命令
ENTRYPOINT ["/bin/bash", "-c"]
CMD ["/TBCNODE/start.sh"] 