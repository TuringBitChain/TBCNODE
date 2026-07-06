#!/usr/bin/env python3
# Copyright (c) 2026 The Open TBC developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""
Sample the live mempool admission-fee curve by repeatedly adding large
transactions and recording getmempoolinfo()['mempoolminfee'] as usage grows.

This is intentionally an explicit/manual functional test: it is slower than the
 usual suite because it pushes the mempool close to its hard size cap and emits
 CSV/SVG artifacts for visual inspection.
"""

from decimal import Decimal
import csv
import importlib.util
import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    bytes_to_hex_str,
    create_confirmed_utxos,
    get_srcdir,
    satoshi_round,
)


TBCCOIN = 1000000


def coin_to_sats(value):
    return int(Decimal(str(value)) * TBCCOIN)


class MempoolFeeRampSampledCurveTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

        self.floor_rate_sats = 60
        self.max_mempool_mb = 300
        self.ramp_start_mb = 200
        self.sample_fraction = Decimal("0.98")
        self.target_points = 40
        self.large_tx_data_bytes = 999000
        self.large_tx_fee_coins = Decimal("10")
        self.utxo_count = 340

        self.extra_args = [[
            "-acceptnonstdtxn=1",
            "-maxmempool={}MB".format(self.max_mempool_mb),
            "-mempoolfeerampstart={}MB".format(self.ramp_start_mb),
            "-mempoolminfeerate={}".format(self.floor_rate_sats),
        ]]

    def build_large_tx(self, node, utxo, fee_coins):
        addr = node.getnewaddress()
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {
            addr: satoshi_round(Decimal(str(utxo["amount"])) - fee_coins),
            "data": bytes_to_hex_str(bytearray(self.large_tx_data_bytes)),
        }
        rawtx = node.createrawtransaction(inputs, outputs)
        signed = node.signrawtransaction(rawtx)
        assert signed["complete"]
        return signed["hex"]

    def expected_min_fee_sats(self, usage_bytes, floor_rate_sats, ramp_start_bytes, max_mempool_bytes):
        if usage_bytes <= ramp_start_bytes or max_mempool_bytes <= ramp_start_bytes:
            return floor_rate_sats
        if usage_bytes >= max_mempool_bytes:
            return 1 << 40
        return (floor_rate_sats * (max_mempool_bytes - ramp_start_bytes)) // (max_mempool_bytes - usage_bytes)

    def load_plotter(self):
        srcdir = get_srcdir(calling_script=__file__)
        plotter_path = os.path.join(srcdir, "contrib", "devtools", "plot_mempool_min_fee_curve.py")
        spec = importlib.util.spec_from_file_location("plot_mempool_min_fee_curve", plotter_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def run_test(self):
        node = self.nodes[0]
        relayfee = Decimal(str(node.getnetworkinfo()["relayfee"]))
        utxos = create_confirmed_utxos(relayfee, node, self.utxo_count, age=110)

        info = node.getmempoolinfo()
        max_mempool = int(info["maxmempool"])
        ramp_start = int(info["mempoolfeerampstart"])
        sample_limit = int(Decimal(max_mempool) * self.sample_fraction)

        self.log.info("Sampling live mempool fee curve up to {} bytes (~{}% of N2={})".format(
            sample_limit, int(self.sample_fraction * 100), max_mempool))

        targets = sorted(set(
            int(sample_limit * i / self.target_points)
            for i in range(1, self.target_points + 1)
        ))

        samples = []
        last_fee_sats = None

        def record_sample():
            nonlocal last_fee_sats
            current = node.getmempoolinfo()
            usage = int(current["usage"])
            fee_sats = coin_to_sats(current["mempoolminfee"])
            expected = self.expected_min_fee_sats(
                usage, self.floor_rate_sats, ramp_start, max_mempool
            )
            assert_equal(fee_sats, expected)
            if last_fee_sats is not None:
                assert fee_sats >= last_fee_sats
            last_fee_sats = fee_sats
            samples.append({
                "usage_bytes": usage,
                "usage_mb": usage / 1_000_000,
                "mempoolminfee_sats": fee_sats,
                "expected_sats": expected,
            })

        record_sample()

        while targets:
            tx_hex = self.build_large_tx(node, utxos.pop(), self.large_tx_fee_coins)
            node.sendrawtransaction(tx_hex, True)
            usage = int(node.getmempoolinfo()["usage"])
            while targets and usage >= targets[0]:
                record_sample()
                targets.pop(0)

        csv_path = os.path.join(self.options.tmpdir, "mempool_feeramp_samples.csv")
        with open(csv_path, "w", newline="") as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=[
                "usage_bytes", "usage_mb", "mempoolminfee_sats", "expected_sats"
            ])
            writer.writeheader()
            writer.writerows(samples)

        plotter = self.load_plotter()
        svg_path = os.path.join(self.options.tmpdir, "mempool_feeramp_samples.svg")
        plotter.render_svg(
            [(row["usage_mb"], row["mempoolminfee_sats"]) for row in samples],
            ramp_start / 1_000_000,
            max_mempool / 1_000_000,
            self.floor_rate_sats,
            1 << 40,
            "linear",
            plotter.Path(svg_path),
        )

        self.log.info("Recorded {} sampled points".format(len(samples)))
        self.log.info("CSV written to {}".format(csv_path))
        self.log.info("SVG written to {}".format(svg_path))


if __name__ == '__main__':
    MempoolFeeRampSampledCurveTest().main()
