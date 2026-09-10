#!/usr/bin/env python3
# Copyright (c) 2026 The TuringBitChain developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""End-to-end tests for the gettransactionflow RPC.

The test deliberately creates every monetary expectation before calling the
new RPC.  It uses Decimal with TBC's six-decimal unit and accepts transaction
ids only from the node, because version-10 transactions use TuringTXID rather
than the legacy Python framework's whole-transaction double-SHA calculation.
"""

from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import http.client
from io import BytesIO
import json
import re
import subprocess
import threading
import time
import urllib.parse

from test_framework.authproxy import AuthServiceProxy
from test_framework.mininode import CTransaction
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    connect_nodes_bi,
    disconnect_nodes_bi,
    hex_str_to_bytes,
    str_to_b64str,
    sync_blocks,
    wait_until,
)


ATOMIC_PER_TBC = 1_000_000
TBC_QUANTUM = Decimal("0.000001")
FIXED_FEE = Decimal("0.001000")

RESULT_KEYS = {
    "txid",
    "blockhash",
    "blockheight",
    "confirmations",
    "bestblockhash",
    "bestblockheight",
    "input_count",
    "output_count",
    "inputs",
    "outputs",
    "total_input",
    "total_output",
    "fee",
}
INPUT_KEYS = {"n", "prev_txid", "prev_vout", "value"}
OUTPUT_KEYS = {"n", "value"}
MONEY_FIELDS = {
    "input_count",
    "output_count",
    "inputs",
    "outputs",
    "total_input",
    "total_output",
    "fee",
}


def tbc_amount(value):
    """Return an exact six-decimal TBC amount and reject hidden rounding."""
    amount = value if isinstance(value, Decimal) else Decimal(str(value))
    quantized = amount.quantize(TBC_QUANTUM)
    assert_equal(amount, quantized)
    return quantized


def tbc_units(value):
    """Convert an exact TBC amount to its integer 10^-6 wire unit."""
    amount = tbc_amount(value)
    units = amount * ATOMIC_PER_TBC
    assert_equal(units, units.to_integral_exact())
    return int(units)


def units_to_tbc(units):
    return tbc_amount(Decimal(units) / ATOMIC_PER_TBC)


class GetTransactionFlowTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        common = [
            "-whitelist=127.0.0.1",
            "-debug=rpc",
            "-blockmintxfee=0.000060",
            "-mempoolminfeerate=60",
        ]
        # A: indexed wallet node; B: deliberately no txindex; C: indexed and
        # wallet-disabled.  Every node receives an independent clean datadir.
        self.extra_args = [
            common + ["-txindex=1"],
            common + ["-txindex=0"],
            common + ["-txindex=1", "-disablewallet=1"],
        ]

    def create_signed(self, node, inputs, outputs):
        """Create/sign without automatic input selection or automatic change."""
        raw = node.createrawtransaction(inputs, outputs)
        signed = node.signrawtransaction(raw)
        assert_equal(signed["complete"], True)
        decoded = node.decoderawtransaction(signed["hex"])
        assert_equal(decoded["version"], 10)

        # Deserializing the raw bytes here is intentionally limited to reading
        # each serialized int64 nValue.  We do not call mininode.rehash(),
        # whose legacy algorithm is not TuringTXID-compatible for version 10.
        wire_tx = CTransaction()
        wire_tx.deserialize(BytesIO(hex_str_to_bytes(signed["hex"])))
        expected_units = [
            0 if destination == "data" else tbc_units(value)
            for destination, value in outputs.items()
        ]
        assert_equal([output.nValue for output in wire_tx.vout], expected_units)
        return signed["hex"], decoded

    def broadcast_signed(self, node, signed_hex, decoded):
        """Use the node-returned v10 TXID; never calculate it with mininode."""
        txid = node.sendrawtransaction(signed_hex)
        assert_equal(txid, decoded["txid"])
        return txid

    def mine_and_sync(self, miner, count=1):
        blockhashes = miner.generate(count)
        sync_blocks(self.nodes)
        return blockhashes

    @staticmethod
    def find_output(decoded, address):
        matches = [
            output for output in decoded["vout"]
            if address in output["scriptPubKey"].get("addresses", [])
        ]
        assert_equal(len(matches), 1)
        return matches[0]

    def assert_flow(self, node, txid, expected_inputs, expected_outputs):
        """Check the complete public contract against independent expectations."""
        expected_inputs = [
            {
                "n": index,
                "prev_txid": item["prev_txid"].lower(),
                "prev_vout": item["prev_vout"],
                "value": tbc_amount(item["value"]),
            }
            for index, item in enumerate(expected_inputs)
        ]
        expected_outputs = [tbc_amount(value) for value in expected_outputs]
        # Sum integer atomic units first.  This is independent of the new RPC
        # and makes the TBC 10^6 unit explicit instead of inheriting the test
        # framework's legacy BTC 10^8 COIN constant.
        expected_input_units = sum(
            tbc_units(item["value"]) for item in expected_inputs)
        expected_output_units = sum(
            tbc_units(value) for value in expected_outputs)
        expected_fee_units = expected_input_units - expected_output_units
        assert expected_fee_units >= 0
        expected_total_input = units_to_tbc(expected_input_units)
        expected_total_output = units_to_tbc(expected_output_units)
        expected_fee = units_to_tbc(expected_fee_units)

        # Existing RPC data is only a cross-check.  It does not generate the
        # monetary golden values used below.
        raw = node.getrawtransaction(txid, True)
        assert_equal(raw["txid"], txid)
        assert_equal(len(raw["vin"]), len(expected_inputs))
        assert_equal(len(raw["vout"]), len(expected_outputs))
        for index, expected in enumerate(expected_inputs):
            assert_equal(raw["vin"][index]["txid"], expected["prev_txid"])
            assert_equal(raw["vin"][index]["vout"], expected["prev_vout"])
        for index, expected in enumerate(expected_outputs):
            assert_equal(raw["vout"][index]["value"], expected)

        flow = node.gettransactionflow(txid)
        assert_equal(set(flow), RESULT_KEYS)
        assert_equal(flow["txid"], txid.lower())
        assert_equal(flow["blockhash"], raw["blockhash"])
        assert_equal(flow["blockheight"], raw["blockheight"])
        assert_equal(flow["bestblockhash"], node.getbestblockhash())
        assert_equal(flow["bestblockheight"], node.getblockcount())
        assert_equal(
            flow["confirmations"],
            flow["bestblockheight"] - flow["blockheight"] + 1,
        )
        assert flow["confirmations"] >= 1

        assert_equal(flow["input_count"], len(expected_inputs))
        assert_equal(flow["output_count"], len(expected_outputs))
        assert_equal(len(flow["inputs"]), len(expected_inputs))
        assert_equal(len(flow["outputs"]), len(expected_outputs))
        for actual, expected in zip(flow["inputs"], expected_inputs):
            assert_equal(set(actual), INPUT_KEYS)
            assert_equal(actual, expected)
            assert isinstance(actual["value"], Decimal)
        for index, (actual, expected) in enumerate(
                zip(flow["outputs"], expected_outputs)):
            assert_equal(set(actual), OUTPUT_KEYS)
            assert_equal(actual, {"n": index, "value": expected})
            assert isinstance(actual["value"], Decimal)

        assert_equal(flow["total_input"], expected_total_input)
        assert_equal(flow["total_output"], expected_total_output)
        assert_equal(flow["fee"], expected_fee)
        assert isinstance(flow["total_input"], Decimal)
        assert isinstance(flow["total_output"], Decimal)
        assert isinstance(flow["fee"], Decimal)
        return flow

    @staticmethod
    def assert_money_fields_equal(left, right):
        for field in MONEY_FIELDS:
            assert_equal(left[field], right[field])

    @staticmethod
    def raw_http(node, params, authorization="good", request_id="flow-http"):
        """Issue one fresh low-level HTTP request for transport/auth tests."""
        url = urllib.parse.urlparse(node.url)
        headers = {"Content-Type": "application/json"}
        if authorization == "good":
            authpair = url.username + ":" + url.password
            headers["Authorization"] = "Basic " + str_to_b64str(authpair)
        elif authorization == "bad":
            headers["Authorization"] = "Basic " + str_to_b64str("bad:bad")
        elif authorization != "none":
            raise AssertionError("unknown authorization test mode")

        body = json.dumps({
            "jsonrpc": "1.0",
            "id": request_id,
            "method": "gettransactionflow",
            "params": params,
        })
        connection = http.client.HTTPConnection(
            url.hostname, url.port, timeout=30)
        try:
            connection.request("POST", url.path or "/", body, headers)
            response = connection.getresponse()
            payload = response.read().decode("utf8")
            parsed = (json.loads(payload, parse_float=Decimal)
                      if payload else None)
            return response.status, parsed, payload
        finally:
            connection.close()

    def test_strict_parameters(self, node, known_txid):
        self.log.info("Strict parameter, type, format, and named-argument checks")
        assert_raises_rpc_error(
            -1, 'gettransactionflow "txid"', node.gettransactionflow)
        assert_raises_rpc_error(
            -1, 'gettransactionflow "txid"',
            node.gettransactionflow, known_txid, known_txid)
        for value in (None, True, 7, [], {}):
            assert_raises_rpc_error(
                -3, "Expected type string", node.gettransactionflow, value)
        for value in (
            "", "a" * 63, "a" * 65, " " + known_txid,
            known_txid + " ", "0x" + known_txid,
        ):
            assert_raises_rpc_error(
                -8, "txid must be of length 64", node.gettransactionflow, value)
        assert_raises_rpc_error(
            -8, "txid must be hexadecimal string",
            node.gettransactionflow, "g" * 64)
        assert_raises_rpc_error(
            -8, "Unknown named parameter", node.gettransactionflow,
            txid=known_txid, unknown=True)
        assert_equal(
            node.gettransactionflow(known_txid.upper())["txid"],
            known_txid.lower(),
        )

    def test_interfaces_and_auth(self, node, txid, expected):
        self.log.info("F12/F13: RPC, CLI, raw HTTP, named arguments, and auth")
        assert_equal(node.gettransactionflow(txid), expected)
        assert_equal(node.gettransactionflow(txid=txid), expected)
        assert_equal(node.cli.gettransactionflow(txid), expected)
        assert_equal(node.cli.gettransactionflow(txid=txid), expected)

        status, response, raw_body = self.raw_http(
            node, [txid], request_id="flow-pos")
        assert_equal(status, http.client.OK)
        assert_equal(response["error"], None)
        assert_equal(response["result"], expected)
        # UniValue must serialize every public amount as a JSON number with
        # six fractional digits, not as a quoted string or an eight-digit BTC
        # amount.  Parsed Decimal comparisons alone would not prove this.
        amount_literals = re.findall(
            r'"(?:value|total_input|total_output|fee)"\s*:\s*'
            r'(-?\d+\.\d{6})(?=\s*[,}])',
            raw_body,
        )
        assert_equal(
            len(amount_literals),
            expected["input_count"] + expected["output_count"] + 3,
        )

        status, response, _ = self.raw_http(
            node, {"txid": txid}, request_id="flow-named")
        assert_equal(status, http.client.OK)
        assert_equal(response["error"], None)
        assert_equal(response["result"], expected)

        status, response, _ = self.raw_http(
            node, [txid], authorization="none", request_id="flow-no-auth")
        assert_equal(status, http.client.UNAUTHORIZED)
        assert_equal(response, None)
        status, response, _ = self.raw_http(
            node, [txid], authorization="bad", request_id="flow-bad-auth")
        assert_equal(status, http.client.UNAUTHORIZED)
        assert_equal(response, None)

    def record_smoke_metrics(self, node, txid):
        """Record observations only; these are not production latency limits."""
        started = time.monotonic()
        node.gettransactionflow(txid)
        first_ms = (time.monotonic() - started) * 1000

        samples = []
        for _ in range(5):
            started = time.monotonic()
            node.gettransactionflow(txid)
            samples.append((time.monotonic() - started) * 1000)

        barrier = threading.Barrier(2)

        def timed_call(method):
            proxy = AuthServiceProxy(node.url)
            barrier.wait(timeout=10)
            started = time.monotonic()
            result = getattr(proxy, method)(txid) if method == "gettransactionflow" \
                else getattr(proxy, method)()
            return (time.monotonic() - started) * 1000, result

        with ThreadPoolExecutor(max_workers=2) as executor:
            flow_future = executor.submit(timed_call, "gettransactionflow")
            count_future = executor.submit(timed_call, "getblockcount")
            concurrent_flow_ms, concurrent_flow = flow_future.result(timeout=30)
            concurrent_count_ms, concurrent_count = count_future.result(timeout=30)
        assert_equal(concurrent_flow["txid"], txid)
        assert_equal(concurrent_count, node.getblockcount())

        rss_kib = "unavailable"
        try:
            rss_kib = subprocess.check_output(
                ["ps", "-o", "rss=", "-p", str(node.process.pid)],
                universal_newlines=True,
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            pass
        self.log.info(
            "Observational gettransactionflow metrics: first_ms=%.3f "
            "repeat_avg_ms=%.3f concurrent_flow_ms=%.3f "
            "concurrent_getblockcount_ms=%.3f process_rss_kib=%s "
            "(not a cold-cache, peak-RSS, or hard-real-time guarantee)",
            first_ms,
            sum(samples) / len(samples),
            concurrent_flow_ms,
            concurrent_count_ms,
            rss_kib,
        )

    def run_test(self):
        node_a, node_b, node_c = self.nodes

        self.log.info("Create a clean shared chain and reserve explicit inputs")
        initial_blocks = node_a.generate(9)
        sync_blocks(self.nodes)
        spendable = sorted(
            node_a.listunspent(2),
            key=lambda item: (item["confirmations"], item["txid"], item["vout"]),
            reverse=True,
        )
        assert len(spendable) >= 6
        sources = spendable[:6]
        for source in sources:
            source["amount"] = tbc_amount(source["amount"])

        self.log.info("F07: an indexed non-genesis Coinbase is rejected")
        coinbase_txid = node_a.getblock(initial_blocks[0], 1)["tx"][0]
        assert_raises_rpc_error(
            -8, "Coinbase transactions are not supported",
            node_a.gettransactionflow, coinbase_txid)

        self.log.info("F01/F02/F03/F06: explicit multi-input flow")
        source = sources.pop()
        parent_value_0 = Decimal("1234.567890")
        parent_value_1 = source["amount"] - parent_value_0 - FIXED_FEE
        parent_hex, parent_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {
                node_a.getnewaddress(): parent_value_0,
                node_a.getnewaddress(): parent_value_1,
            },
        )
        assert_equal(
            [output["value"] for output in parent_decoded["vout"]],
            [parent_value_0, parent_value_1],
        )
        parent_txid = self.broadcast_signed(node_a, parent_hex, parent_decoded)
        self.mine_and_sync(node_a)

        target_total_input = parent_value_0 + parent_value_1
        target_value_0 = Decimal("1000.000001")
        target_value_1 = target_total_input - target_value_0 - FIXED_FEE
        target_inputs = [
            {"txid": parent_txid, "vout": 0},
            {"txid": parent_txid, "vout": 1},
        ]
        target_hex, target_decoded = self.create_signed(
            node_a,
            target_inputs,
            {
                node_a.getnewaddress(): target_value_0,
                node_a.getnewaddress(): target_value_1,
            },
        )
        target_txid = self.broadcast_signed(node_a, target_hex, target_decoded)
        wait_until(lambda: target_txid in node_a.getrawmempool())
        assert_raises_rpc_error(
            -8, "no active-chain confirmation",
            node_a.gettransactionflow, target_txid)
        # B must reject by runtime index policy, even while it may know the
        # transaction through its Mempool or block store.
        assert_raises_rpc_error(
            -8, "requires -txindex=1",
            node_b.gettransactionflow, target_txid)
        target_block = self.mine_and_sync(node_a)[0]

        expected_target_inputs = [
            {"prev_txid": parent_txid, "prev_vout": 0,
             "value": parent_value_0},
            {"prev_txid": parent_txid, "prev_vout": 1,
             "value": parent_value_1},
        ]
        expected_target_outputs = [target_value_0, target_value_1]
        flow_before_spend = self.assert_flow(
            node_a, target_txid,
            expected_target_inputs, expected_target_outputs)
        assert_equal(node_a.gettxout(parent_txid, 0), None)
        assert_equal(node_a.gettxout(parent_txid, 1), None)

        self.test_strict_parameters(node_a, target_txid)
        self.test_interfaces_and_auth(node_a, target_txid, flow_before_spend)

        self.log.info("F11: a well-formed but unknown TXID is unavailable")
        assert_raises_rpc_error(
            -5, "Transaction unavailable from local node",
            node_a.gettransactionflow, "1" * 64)

        self.log.info("F10: no-index node rejects a confirmed known target")
        assert_raises_rpc_error(
            -8, "requires -txindex=1",
            node_b.gettransactionflow, target_txid)

        self.log.info("F04: spending every target output does not change history")
        spent_target_value = target_value_0 + target_value_1 - FIXED_FEE
        spend_target_hex, spend_target_decoded = self.create_signed(
            node_a,
            [{"txid": target_txid, "vout": 0},
             {"txid": target_txid, "vout": 1}],
            {node_a.getnewaddress(): spent_target_value},
        )
        self.broadcast_signed(node_a, spend_target_hex, spend_target_decoded)
        self.mine_and_sync(node_a)
        assert_equal(node_a.gettxout(target_txid, 0), None)
        assert_equal(node_a.gettxout(target_txid, 1), None)
        flow_after_spend = self.assert_flow(
            node_a, target_txid,
            expected_target_inputs, expected_target_outputs)
        self.assert_money_fields_equal(flow_before_spend, flow_after_spend)

        self.log.info("F05: parent and child confirmed in the same block")
        source = sources.pop()
        same_parent_value = source["amount"] - FIXED_FEE
        same_parent_hex, same_parent_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {node_a.getnewaddress(): same_parent_value},
        )
        same_parent_txid = self.broadcast_signed(
            node_a, same_parent_hex, same_parent_decoded)
        same_child_value = same_parent_value - FIXED_FEE
        same_child_hex, same_child_decoded = self.create_signed(
            node_a,
            [{"txid": same_parent_txid, "vout": 0}],
            {node_a.getnewaddress(): same_child_value},
        )
        same_child_txid = self.broadcast_signed(
            node_a, same_child_hex, same_child_decoded)
        same_block = self.mine_and_sync(node_a)[0]
        block_txids = node_a.getblock(same_block, 1)["tx"]
        assert block_txids.index(same_parent_txid) < block_txids.index(same_child_txid)
        assert_equal(
            node_a.getrawtransaction(same_parent_txid, True)["blockheight"],
            node_a.getrawtransaction(same_child_txid, True)["blockheight"],
        )
        self.assert_flow(
            node_a,
            same_child_txid,
            [{"prev_txid": same_parent_txid, "prev_vout": 0,
              "value": same_parent_value}],
            [same_child_value],
        )

        self.log.info("F17: zero-valued data output is retained in order")
        source = sources.pop()
        data_value = source["amount"] - FIXED_FEE
        data_hex, data_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {node_a.getnewaddress(): data_value, "data": "deadbeef010203"},
        )
        assert_equal([output["value"] for output in data_decoded["vout"]],
                     [data_value, Decimal("0.000000")])
        # A data output has no single spend destination.  This node omits the
        # optional addresses member entirely rather than returning [].
        assert_equal(
            data_decoded["vout"][1]["scriptPubKey"].get("addresses", []), [])
        data_txid = self.broadcast_signed(node_a, data_hex, data_decoded)
        self.mine_and_sync(node_a)
        self.assert_flow(
            node_a,
            data_txid,
            [{"prev_txid": source["txid"], "prev_vout": source["vout"],
              "value": source["amount"]}],
            [data_value, Decimal("0.000000")],
        )

        self.log.info("F09: transaction unrelated to A's wallet; C has no wallet")
        source = sources.pop()
        node_b_address = node_b.getnewaddress()
        funding_b_value = Decimal("50.000000")
        funding_b_change = source["amount"] - funding_b_value - FIXED_FEE
        funding_b_hex, funding_b_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {
                node_b_address: funding_b_value,
                node_a.getnewaddress(): funding_b_change,
            },
        )
        funding_b_output = self.find_output(funding_b_decoded, node_b_address)
        funding_b_txid = self.broadcast_signed(
            node_a, funding_b_hex, funding_b_decoded)
        self.mine_and_sync(node_a)
        wait_until(
            lambda: any(
                item["txid"] == funding_b_txid and
                item["vout"] == funding_b_output["n"]
                for item in node_b.listunspent(1)
            )
        )
        unrelated_value = funding_b_value - FIXED_FEE
        unrelated_hex, unrelated_decoded = self.create_signed(
            node_b,
            [{"txid": funding_b_txid, "vout": funding_b_output["n"]}],
            {node_b.getnewaddress(): unrelated_value},
        )
        unrelated_txid = self.broadcast_signed(
            node_b, unrelated_hex, unrelated_decoded)
        wait_until(lambda: unrelated_txid in node_a.getrawmempool())
        self.mine_and_sync(node_a)
        unrelated_inputs = [
            {"prev_txid": funding_b_txid,
             "prev_vout": funding_b_output["n"],
             "value": funding_b_value},
        ]
        unrelated_on_a = self.assert_flow(
            node_a, unrelated_txid, unrelated_inputs, [unrelated_value])
        unrelated_on_c = self.assert_flow(
            node_c, unrelated_txid, unrelated_inputs, [unrelated_value])
        assert_equal(unrelated_on_a, unrelated_on_c)
        assert_raises_rpc_error(
            -8, "requires -txindex=1",
            node_b.gettransactionflow, unrelated_txid)

        self.log.info("F08: indexed transaction in a genuine stale side block")
        source = sources.pop()
        branch_value = source["amount"] - FIXED_FEE
        side_hex, side_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {node_a.getnewaddress(): branch_value},
        )
        winner_hex, winner_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {node_a.getnewaddress(): branch_value},
        )
        assert side_decoded["txid"] != winner_decoded["txid"]

        disconnect_nodes_bi(self.nodes, 0, 1)
        side_txid = self.broadcast_signed(node_a, side_hex, side_decoded)
        side_block = node_a.generate(1)[0]
        side_height = node_a.getblockcount()
        winner_txid = self.broadcast_signed(node_b, winner_hex, winner_decoded)
        node_b.generate(2)
        sync_blocks([node_b, node_c])
        assert winner_txid in node_b.getblock(
            node_b.getblockhash(node_b.getblockcount() - 1), 1)["tx"]

        connect_nodes_bi(self.nodes, 0, 1)
        sync_blocks(self.nodes)
        assert_equal(node_a.getbestblockhash(), node_b.getbestblockhash())
        assert side_txid not in node_a.getrawmempool()
        side_raw = node_a.getrawtransaction(side_txid, True)
        assert_equal(side_raw["blockhash"], side_block)
        # Verbose getrawtransaction omits blockheight for a stale block in
        # this codebase, so preserve the observed side height before reorg.
        assert node_a.getblockhash(side_height) != side_block
        side_tips = [
            tip for tip in node_a.getchaintips() if tip["hash"] == side_block
        ]
        assert_equal(len(side_tips), 1)
        assert_equal(side_tips[0]["status"], "valid-fork")
        assert_raises_rpc_error(
            -8, "no active-chain confirmation",
            node_a.gettransactionflow, side_txid)

        self.log.info("F14: invalidate/reconsider changes confirmation state")
        source = sources.pop()
        restore_value = source["amount"] - FIXED_FEE
        restore_hex, restore_decoded = self.create_signed(
            node_a,
            [{"txid": source["txid"], "vout": source["vout"]}],
            {node_a.getnewaddress(): restore_value},
        )
        restore_txid = self.broadcast_signed(node_a, restore_hex, restore_decoded)
        restore_block = self.mine_and_sync(node_a)[0]
        restore_inputs = [
            {"prev_txid": source["txid"], "prev_vout": source["vout"],
             "value": source["amount"]},
        ]
        restore_flow = self.assert_flow(
            node_a, restore_txid, restore_inputs, [restore_value])
        node_a.invalidateblock(restore_block)
        assert_raises_rpc_error(
            -8, "no active-chain confirmation",
            node_a.gettransactionflow, restore_txid)
        node_a.reconsiderblock(restore_block)
        wait_until(lambda: node_a.getbestblockhash() == restore_block)
        restored_flow = self.assert_flow(
            node_a, restore_txid, restore_inputs, [restore_value])
        self.assert_money_fields_equal(restore_flow, restored_flow)

        self.log.info("F15: normal restart preserves historical flow")
        before_restart = node_a.gettransactionflow(target_txid)
        self.restart_node(0)
        after_restart = self.assert_flow(
            node_a, target_txid,
            expected_target_inputs, expected_target_outputs)
        assert_equal(before_restart, after_restart)

        self.log.info("F16: repeated reads do not mutate a non-empty Mempool")
        sentinel_txid = node_a.sendtoaddress(
            node_a.getnewaddress(), Decimal("1.000000"))
        wait_until(lambda: sentinel_txid in node_a.getrawmempool())
        tip_before = node_a.getbestblockhash()
        pool_before = node_a.getrawmempool()
        assert sentinel_txid in pool_before
        for _ in range(5):
            self.assert_flow(
                node_a, target_txid,
                expected_target_inputs, expected_target_outputs)
        assert_equal(node_a.getbestblockhash(), tip_before)
        assert_equal(node_a.getrawmempool(), pool_before)

        self.record_smoke_metrics(node_a, target_txid)
        self.log.info("F01-F17 completed; P1 stress/sanitizer cases are separate")


if __name__ == "__main__":
    GetTransactionFlowTest().main()
