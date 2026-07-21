# Instructions for Building and Launching TBCNODE on Ubuntu


## How To Install Dependencies :

For Ubuntu 22.04 LTS:
```
sudo apt-get update
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils
sudo apt-get install libdb-dev
sudo apt-get install libdb++-dev
sudo apt-get install -y libboost-all-dev
sudo apt-get install cmake
sudo apt-get install -y libzmq3-dev
```

For Ubuntu 24.04 LTS:
```
# Install basic build dependencies
sudo apt-get update
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils

# Install Objective-C++ support (required for Ubuntu 24.04)
sudo apt-get install gobjc++

# Install Berkeley DB
sudo apt-get install libdb-dev libdb++-dev

# Install CMake
sudo apt-get install cmake

# Install additional libraries
sudo apt-get install libzmq3-dev libminiupnpc-dev libnatpmp-dev

# Remove newer Boost versions if installed
sudo apt-get remove libboost*-dev

# Install Boost 1.74 (compatible version for Ubuntu 24.04)
sudo apt-get install libboost1.74-dev libboost-system1.74-dev libboost-filesystem1.74-dev libboost-chrono1.74-dev libboost-program-options1.74-dev libboost-test1.74-dev libboost-thread1.74-dev
```


## How To Build

C++ compilers are memory-hungry. It is recommended to have at least 1.5 GB of
memory available when compiling. 

**For Memory > 1.5GB:**

```bash
cmake -B build -S . -DENABLE_PROD_BUILD=ON
cmake --build build -j$(nproc)
```

## How to Launch the TBCNODE software


### Configure

```
cd /home/$USER/TBCNODE
cat > node.noprune.conf << EOF
##
## bitcoin.conf configuration file. Lines beginning with # are comments.
##

#start in background
daemon=1

#Required Consensus Rules for Genesis
excessiveblocksize=10000000000 #10GB
maxstackmemoryusageconsensus=100000000 #100MB

#Mining
#biggest block size you want to mine
blockmaxsize=4000000000 

#preload mempool
preload=1

# Index all transactions, prune mode does't support txindex
txindex=1
#reindex=1
#reindex-chainstate=1

#Other Sys
dbcache=1000 

#Other Block
threadsperblock=6
#prune=196000

#Other Tx Conf:
maxscriptsizepolicy=0

# Minimum feerate (in TBC/kB) for a transaction to be included in blocks
# assembled by this node. 0.000060 TBC/kB = 60 sat/kB, aligned with
# mempoolminfeerate so every accepted transaction is also mineable.
blockmintxfee=0.000060

# Mempool admission feerate floor in satoshis per kB. Applies while mempool
# usage is below mempoolfeerampstart (default: 60).
mempoolminfeerate=60

# Mempool usage (in MB) at which the admission feerate starts to ramp up.
# Below this size the flat mempoolminfeerate floor applies; between this and
# maxmempool the required feerate rises steeply, so the mempool asymptotes
# below the hard cap.
mempoolfeerampstart=3000

# Hard cap (in MB) on mempool memory usage. The mempool is never trimmed;
# once usage reaches this cap new transactions are rejected.
maxmempool=4000
 
# Network-related settings:

# Run on the test network instead of the real bitcoin network.
#testnet=0

# Run a regression test network
#regtest=0

# Connect via a SOCKS5 proxy
#proxy=127.0.0.1:9050

# Bind to given address and always listen on it. Use [host]:port notation for IPv6
#bind=<addr>

# Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6
#whitebind=<addr>

# Use as many addnode= settings as you like to connect to specific peers
#addnode=10.0.0.2:8333

# Alternatively use as many connect= settings as you like to connect ONLY to specific peers
#connect=10.0.0.1:8333

# Listening mode, enabled by default except when 'connect' is being used
#listen=1

# Maximum number of inbound+outbound connections.
maxconnections=12

#
# JSON-RPC options (for controlling a running Bitcoin/bitcoind process)
#
# server=1 tells bitcoind to accept JSON-RPC commands
server=1

# Bind to given address to listen for JSON-RPC connections. Use [host]:port notation for IPv6.
# This option can be specified multiple times (default: bind to all interfaces)
rpcbind=127.0.0.1

# If no rpcpassword is set, rpc cookie auth is sought. The default `-rpccookiefile` name
# is .cookie and found in the `-datadir` being used for bitcoind. This option is typically used
# when the server and client are run as the same user.
#
# If not, you must set rpcuser and rpcpassword to secure the JSON-RPC api. The first
# method(DEPRECATED) is to set this pair for the server and client:
rpcuser=username
rpcpassword=randompasswd
#
# The second method `rpcauth` can be added to server startup argument. It is set at intialization time
# using the output from the script in share/rpcuser/rpcuser.py after providing a username:
# ...

# How many seconds bitcoin will wait for a complete RPC HTTP request.
# after the HTTP connection is established. 
#rpcclienttimeout=30

# By default, only RPC connections from localhost are allowed.
# Specify as many rpcallowip= settings as you like to allow connections from other hosts,
# either as a single IPv4/IPv6 or with a subnet specification.

# NOTE: opening up the RPC port to hosts outside your local trusted network is NOT RECOMMENDED,
# because the rpcpassword is transmitted over the network unencrypted.

# server=1 is read by bitcoind to determine if RPC should be enabled 
#rpcallowip=10.1.1.34/255.255.255.0
#rpcallowip=1.2.3.4/24
#rpcallowip=2001:888:bbbb:0:0:aaaa:555:6666/96

# Listen for RPC connections on this TCP port:
rpcport=8332

# The default value is 0, requesting all data. It can be set to option 824188 (not requesting block 
# data from peer nodes for block 824188 and earlier).
pruneblocks=824188
EOF
```


### Exe Command

Run bitcoind and bitcoin-cli :
```
/home/$USER/TBCNODE/build/src/bitcoind -conf=/home/$USER/TBCNODE/node.noprune.conf -datadir=/home/$USER/TBCNODE/node_data_dir
alias tbc-cli="/home/$USER/TBCNODE/build/src/bitcoin-cli -conf=/home/$USER/TBCNODE/node.noprune.conf" 
tbc-cli  getinfo
tbc-cli  getpeerinfo
tbc-cli  listwallets
tbc-cli  listaccounts
tbc-cli  getaddressesbyaccount
tbc-cli  stop
(For developing or test, one may need to start bitcoind with -standalone, such as ".../bitcoind -conf=... -datadir=... -standalone" )
```


