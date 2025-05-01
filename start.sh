#!/bin/bash
pm2 --name tbcd --max-restarts 20 start "/TBCNODE/bin/bitcoind -conf=/TBCNODE/node.conf -datadir=/TBCNODE/node_data_dir" --no-daemon
pm2 logs --lines 200
