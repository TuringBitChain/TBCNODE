#!/bin/bash
# 使用pm2启动服务
pm2 --name tbcd --max-restarts 20 start "/TBCNODE/bin/bitcoind -conf=/TBCNODE/node.conf -datadir=/TBCNODE/node_data_dir" --no-daemon
# 监控日志
pm2 logs --lines 200
