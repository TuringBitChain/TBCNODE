# Instructions for Building and Launching TBCNODE on Ubuntu or Mac OSX


## How To Install Dependencies :

For Ubuntu 20.04 LTS:
```
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils
sudo apt-get install libdb-dev
sudo apt-get install libdb++-dev

sudo apt-get install libboost-system-dev libboost-filesystem-dev libboost-chrono-dev libboost-program-options-dev libboost-test-dev libboost-thread-dev
```

For Ubuntu 22.04 LTS:
```
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils
sudo apt-get install libdb-dev
sudo apt-get install libdb++-dev

# Manually install boost (version 1.66.0):
tar -xzf boost_1_66_0.tar.gz
cd boost_1_66_0
./bootstrap.sh --prefix=/home/.../boost-1.66.0
./b2 install   --prefix=/home/.../boost-1.66.0 --with=all
```

For Ubuntu 24.04 LTS:
```
# Install basic build dependencies
sudo apt-get update
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils

# Install Objective-C++ support (required for Ubuntu 24.04)
sudo apt-get install gobjc++ gcc-objc++

# Install Berkeley DB
sudo apt-get install libdb-dev libdb++-dev

# Install additional libraries
sudo apt-get install libzmq3-dev libminiupnpc-dev libnatpmp-dev

# Remove newer Boost versions if installed
sudo apt-get remove libboost*-dev

# Install Boost 1.74 (compatible version for Ubuntu 24.04)
sudo apt-get install libboost1.74-dev libboost-system1.74-dev libboost-filesystem1.74-dev libboost-chrono1.74-dev libboost-program-options1.74-dev libboost-test1.74-dev libboost-thread1.74-dev
```

For Mac OSX 13.1 with Xcode and brew installed:
```
brew install automake berkeley-db libtool boost@1.76 openssl pkg-config libevent
(failed to compile on OSX when using new boost version, such as boost@1.83)
```


## How To Build

C++ compilers are memory-hungry. It is recommended to have at least 1.5 GB of
memory available when compiling. 

### Firstly

```bash
./autogen.sh
```

### Secondly

**For Memory > 1.5GB:**

For UBUNTU 20.04 LTS:
```bash
./configure --enable-cxx --disable-shared --with-pic --enable-prod-build  --prefix=/home/$USER/TBCNODE
```

For UBUNTU 22.04 LTS: 
```
CXXFLAGS="-std=c++17" ./configure --enable-cxx --disable-shared --with-pic --enable-prod-build  --disable-static --disable-tests --disable-bench --with-libs=no --with-seeder=no --prefix=/home/$USER/TBCNODE   --with-boost=/home/.../boost-1.66.0 
```

For UBUNTU 24.04 LTS:
```bash
# Clean previous build if needed
make clean

# Configure with Boost 1.74
CXXFLAGS="-std=c++17" ./configure --enable-cxx --disable-shared --with-pic --enable-prod-build --disable-static --disable-tests --disable-bench --with-libs=no --with-seeder=no --with-boost=/usr --prefix=/home/$USER/TBCNODE
```

For Mac OSX 13.1:
```bash
./configure --enable-cxx --disable-shared --with-pic --enable-prod-build  --with-boost=/opt/homebrew/opt/boost@1.76
```


**On systems with less memory, gcc can be tuned to conserve memory with additional CXXFLAGS:**

```
./configure CXXFLAGS="--param ggc-min-expand=1 --param ggc-min-heapsize=32768 ... "
```

Sometimes, you need to specify the LDFLAGS or CPPFLAGS before ./configure to let the compiler recognize custom paths, 
such as:
```
CPPFLAGS=" -I/opt/homebrew/opt/libevent/include"      ./configure ... 
CPPFLAGS=" -I/opt/homebrew/Cellar/libevent/2.1.12_1"  ./configure ... 
```


### Finally

```bash
make -j$(nproc)  # Use all available CPU cores for faster compilation
make install     # optional: install bin file to the path specified by --prefix=
```


## Output Log of Building:
``` 
...
configure: creating ./config.status
config.status: creating Makefile
config.status: creating libsecp256k1.pc
config.status: creating src/libsecp256k1-config.h
config.status: src/libsecp256k1-config.h is unchanged
config.status: executing depfiles commands
config.status: executing libtool commands
Fixing libtool for -rpath problems.

Options used to compile and link:
  prod build    = no
  with wallet   = yes
  with zmq      = yes
  with test     = yes
  with bench    = yes
  with upnp     = yes
  use asm       = yes
  debug enabled = no
  werror        = no

  sanitizers    
          asan  = no
          tsan  = no
          ubsan = no

  memory allocators
       tcmalloc = no
       jemalloc = no

  target os     = linux
  build os      = 

  CC            = gcc
  CFLAGS        = -g -O2
  CPPFLAGS      =  -DHAVE_BUILD_INFO -D__STDC_FORMAT_MACROS
  CXX           = g++ -std=c++17
  CXXFLAGS      = -g -O2 -Wall -Wextra -Wformat -Wvla -Wformat-security -Wno-unused-parameter
  LDFLAGS       = 
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
blockassembler=journaling  #journaling is default as of 1.0.5RC

#preload mempool
preload=1

# Index all transactions, prune mode don&t support txindex
txindex=1
#reindex=1
#reindex-chainstate=1

#Other Sys
maxmempool=6000
dbcache=1000 

#Other Block
threadsperblock=6
#prune=196000

#Other Tx Conf:
maxscriptsizepolicy=0
blockmintxfee=0.000080
 
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

##############################################################
##            Quick Primer on addnode vs connect            ##
##  Let's say for instance you use addnode=4.2.2.4          ##
##  addnode will connect you to and tell you about the      ##
##    nodes connected to 4.2.2.4.  In addition it will tell ##
##    the other nodes connected to it that you exist so     ##
##    they can connect to you.                              ##
##  connect will not do the above when you 'connect' to it. ##
##    It will *only* connect you to 4.2.2.4 and no one else.##
##                                                          ##
##  So if you're behind a firewall, or have other problems  ##
##  finding nodes, add some using 'addnode'.                ##
##                                                          ##
##  If you want to stay private, use 'connect' to only      ##
##  connect to "trusted" nodes.                             ##
##                                                          ##
##  If you run multiple nodes on a LAN, there's no need for ##
##  all of them to open lots of connections.  Instead       ##
##  'connect' them all to one node that is port forwarded   ##
##  and has lots of connections.                            ##
##       Thanks goes to [Noodle] on Freenode.               ##
##############################################################

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
server=0

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
#pruneblocks=0
EOF
```


### Exe Command

Run bitcoind and bitcoin-cli :
```
/home/$USER/TBCNODE/bin/bitcoind -conf=/home/$USER/TBCNODE/node.noprune.conf -datadir=/home/$USER/TBCNODE/node_data_dir
alias tbc-cli="/home/$USER/TBCNODE/bin/bitcoin-cli -conf=/home/$USER/TBCNODE/node.noprune.conf" 
tbc-cli  getinfo
tbc-cli  getpeerinfo
tbc-cli  listwallets
tbc-cli  listaccounts
tbc-cli  getaddressesbyaccount
tbc-cli  stop
(For developing or test, one may need to start bitcoind with -standalone, such as ".../bitcoind -conf=... -datadir=... -standalone" )
```


#### Docker Deployment

Docker deployment provides a containerized solution that doesn't require installing dependencies on the host system.

**Prerequisites:**
- Docker installed on your system
- No need to install build dependencies or compile from source

**Build Bitcoin Node Image:**
```bash
# Build the Bitcoin node Docker image (includes compilation)
sudo docker build -f Dockerfile-node -t bitcoin-node .
```

**Run Bitcoin Node:**
```bash
sudo docker run -d --name bitcoin-node \
  -p 8332:8332 -p 8333:8333 \
  -v /home/$USER/TBCNODE/node_data_dir:/home/bitcoin/.bitcoin \
  -v /home/$USER/TBCNODE/node.noprune.conf:/home/bitcoin/.bitcoin/bitcoin.conf:ro \
  bitcoin-node
```

**Manage Docker Node:**
```bash
# View logs
sudo docker logs -f bitcoin-node

# Execute RPC commands
sudo docker exec bitcoin-node bitcoin-cli -rpcuser=username -rpcpassword=randompasswd getinfo
sudo docker exec bitcoin-node bitcoin-cli -rpcuser=username -rpcpassword=randompasswd getblockchaininfo

# Stop/Start/Restart node
sudo docker stop bitcoin-node
sudo docker start bitcoin-node
sudo docker restart bitcoin-node

# Remove container (data in volume is preserved)
sudo docker rm bitcoin-node
```

**Build Documentation (Optional):**
```bash
# Build documentation Docker image
sudo docker build -f Dockerfile-doxygen -t bitcoin-docs .

# Generate and serve documentation (accessible at http://localhost:8080)
sudo docker run -d --name bitcoin-docs -p 8080:80 bitcoin-docs

# View documentation logs
sudo docker logs bitcoin-docs

# Stop documentation server
sudo docker stop bitcoin-docs && sudo docker rm bitcoin-docs
```


### Use Process Manager: pm2

We recommend to use pm2 for auto restart and monitoring the node software.

```
sudo npm install pm2 -g
sed -i 's/daemon=1/daemon=0/' node.noprune.conf
pm2 --name tbcd --max-restarts 20 start "/home/$USER/TBCNODE/bin/bitcoind -conf=/home/$USER/TBCNODE/node.noprune.conf -datadir=/home/$USER/TBCNODE/node_data_dir"
pm2 ps
```
