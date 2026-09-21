#!/usr/bin/env python3
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test txinmempool startup, transport configuration and legacy topic coexistence."""

import os
from pathlib import Path
import struct
import subprocess

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import assert_equal, check_zmq_test_requirements, zmq_port


class TxInMempoolConfigTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_nodes(self):
        check_zmq_test_requirements(self.options.configfile,
                                   SkipTest("bitcoind has not been built with zmq enabled"))
        super().setup_nodes()

    def check_shared_address(self, address, rawblock_address):
        import zmq
        context = zmq.Context()
        subscriber = context.socket(zmq.SUB)
        subscriber.setsockopt(zmq.RCVTIMEO, 5000)
        topics = {b"hashtx", b"rawtx", b"hashblock", b"rawblock"}
        for topic in topics:
            subscriber.setsockopt(zmq.SUBSCRIBE, topic)
        subscriber.connect(address)
        subscriber.connect(rawblock_address)
        args = ["-zmqpubtxinmempool=" + address,
                "-zmqpubhashtx=" + address, "-zmqpubrawtx=" + address,
                "-zmqpubhashblock=" + address, "-zmqpubrawblock=" + rawblock_address]
        try:
            self.start_node(0, args)
            # Wait for the PUB/SUB subscription to propagate using real blocks.
            observed = set()
            for _ in range(30):
                self.nodes[0].generate(1)
                while subscriber.poll(100):
                    observed.add(subscriber.recv_multipart()[0])
                if observed == topics:
                    break
            assert_equal(observed, topics)

            previous = {}
            for _ in range(2):
                blockhash = self.nodes[0].generate(1)[0]
                messages = {}
                for _ in range(4):
                    frames = subscriber.recv_multipart()
                    assert_equal(len(frames), 3)
                    topic, body, sequence = frames
                    assert topic not in messages
                    assert_equal(len(sequence), 4)
                    sequence = struct.unpack("<I", sequence)[0]
                    if topic in previous:
                        assert_equal(sequence, previous[topic] + 1)
                    previous[topic] = sequence
                    messages[topic] = body
                assert_equal(set(messages), topics)
                assert_equal(messages[b"hashblock"].hex(), blockhash)
                assert_equal(messages[b"rawblock"].hex(), self.nodes[0].getblock(blockhash, False))
                txid = self.nodes[0].getblock(blockhash)["tx"][0]
                assert_equal(messages[b"hashtx"].hex(), txid)
                assert_equal(self.nodes[0].decoderawtransaction(messages[b"rawtx"].hex())["txid"], txid)
            self.stop_node(0)
        finally:
            subscriber.close(linger=0)
            context.term()

    def run_test(self):
        node = self.nodes[0]
        epoch_file = Path(node.datadir) / "regtest" / "txinmempool.epoch"
        assert not epoch_file.exists()
        help_text = subprocess.check_output(node.binary + ["-help"], text=True)
        assert "-zmqpubtxinmempool=<address>" in help_text
        self.stop_node(0)

        tcp = "tcp://127.0.0.1:{}".format(zmq_port(0))
        rawblock = "tcp://127.0.0.1:{}".format(zmq_port(1))
        addresses = [tcp]
        if os.name != "nt":
            addresses.append("ipc://" + str(Path(self.options.tmpdir) / "txinmempool.sock"))

        self.log.info("Check forbidden sharing and failed-bind cleanup")
        for address in addresses:
            self.assert_start_raises_init_error(
                0, ["-zmqpubtxinmempool=" + address, "-zmqpubrawblock=" + address],
                "-zmqpubtxinmempool cannot share an address with -zmqpubrawblock")
        self.assert_start_raises_init_error(
            0, ["-zmqpubhashblock=" + tcp, "-zmqpubtxinmempool=invalid://address"],
            "Unable to initialize -zmqpubtxinmempool")
        assert not epoch_file.exists()

        self.log.info("Check startup and epoch renewal without any subscriber")
        self.start_node(0, ["-zmqpubtxinmempool=" + tcp])
        first_epoch = int(epoch_file.read_text())
        assert first_epoch >= 0
        self.stop_node(0)
        self.start_node(0, ["-zmqpubtxinmempool=" + tcp])
        assert int(epoch_file.read_text()) > first_epoch
        self.stop_node(0)

        self.log.info("Check legacy topics sharing TCP and IPC addresses")
        for address in addresses:
            self.check_shared_address(address, rawblock)

        self.log.info("Check epoch failure cleanup and disabled startup")
        saved_epoch = epoch_file.read_text()
        epoch_file.write_text("invalid\n")
        self.assert_start_raises_init_error(
            0, ["-zmqpubtxinmempool=" + tcp], "Cannot read txinmempool epoch counter")
        epoch_file.write_text(saved_epoch)
        self.start_node(0, ["-zmqpubtxinmempool=" + tcp])
        enabled_epoch = epoch_file.read_text()
        self.stop_node(0)
        self.start_node(0, [])
        assert_equal(epoch_file.read_text(), enabled_epoch)


if __name__ == '__main__':
    TxInMempoolConfigTest().main()
