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
    npm \
    python3 \
    && rm -rf /var/lib/apt/lists/* \
    && npm install pm2 -g

WORKDIR /TBCNODE

COPY --from=builder /TBCNODE/bin /TBCNODE/bin
COPY --from=builder /TBCNODE/lib /TBCNODE/lib
COPY --from=builder /TBCNODE/include /TBCNODE/include
COPY --from=builder /TBCNODE/share /TBCNODE/share

RUN mkdir -p /TBCNODE/node_data_dir && \
    echo '\
##\n\
## bitcoin.conf configuration file. Lines beginning with # are comments.\n\
##\n\
\n\
#limitancestorcount=5\n\
pruneblocks=824188\n\
\n\
rpcthreads=64\n\
rpcworkqueue=256\n\
\n\
standalone=1\n\
disablesafemode=1\n\
minimumchainwork=0000000000000000000000000000000000000000000000000000000000000001\n\
rest=1\n\
rpcservertimeout=120\n\
\n\
#start in background\n\
daemon=1\n\
\n\
#Required Consensus Rules for Genesis\n\
excessiveblocksize=10000000000 #10GB\n\
maxstackmemoryusageconsensus=100000000 #100MB\n\
\n\
#Mining\n\
#biggest block size you want to mine\n\
blockmaxsize=4000000000\n\
blockassembler=journaling  #journaling is default as of 1.0.5\n\
\n\
#preload mempool\n\
preload=1\n\
\n\
# Index all transactions, prune mode don\'t support txindex\n\
txindex=1\n\
\n\
#testnet=1\n\
\n\
#Other Sys\n\
maxmempool=6000\n\
dbcache=1000\n\
\n\
#connect=0\n\
maxconnections=8\n\
\n\
# JSON-RPC options\n\
server=1\n\
rpcbind=0.0.0.0\n\
rpcallowip=0.0.0.0/0\n\
rpcuser=tbcuser\n\
rpcpassword=randompasswd\n\
rpcport=8332\n\
port=8333\n\
\n\
#Other Block\n\
threadsperblock=6\n\
\n\
#Other Tx Conf:\n\
maxscriptsizepolicy=0\n\
blockmintxfee=0.000060\n\
' > /TBCNODE/node.conf

RUN echo '#!/bin/bash\n\
pm2 --name tbcd --max-restarts 20 start "/TBCNODE/bin/bitcoind -conf=/TBCNODE/node.conf -datadir=/TBCNODE/node_data_dir"\n\
pm2 logs\n\
' > /TBCNODE/start.sh && \
    chmod +x /TBCNODE/start.sh

EXPOSE 8332
EXPOSE 8333

CMD ["/TBCNODE/start.sh"] 