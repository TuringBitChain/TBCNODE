#!/usr/bin/env python3
# Copyright (c) 2026 The Open TBC developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""
Manual getrawtransaction benchmark.

Measures non-verbose mempool-hit QPS and latency both without contention and
while concurrent verbose requests contend for cs_main. This test is excluded
from the default functional suite because performance results depend on the
host. Use --min-contended-ratio to turn the reported ratio into an assertion.
"""

from concurrent.futures import ThreadPoolExecutor
import json
import math
import threading
import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy


class GetRawTransactionPerformanceTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-rpcthreads=32", "-rpcworkqueue=128"]]

    def add_options(self, parser):
        parser.add_option(
            "--requests", dest="requests", default=1000, type="int",
            help="RPC requests per measured run (default: %default)")
        parser.add_option(
            "--workers", dest="workers", default=8, type="int",
            help="Concurrent measured clients (default: %default)")
        parser.add_option(
            "--contenders", dest="contenders", default=8, type="int",
            help="Concurrent verbose cs_main contenders (default: %default)")
        parser.add_option(
            "--min-contended-ratio", dest="min_contended_ratio",
            default=0.0, type="float",
            help="Fail if contended/baseline non-verbose QPS is below this "
                 "ratio; 0 only reports results (default: %default)")

    @staticmethod
    def percentile(values, percentile):
        ordered = sorted(values)
        index = max(0, math.ceil(len(ordered) * percentile / 100) - 1)
        return ordered[index]

    def benchmark(self, txid, expected_hex, verbose, requests, workers):
        counts = [requests // workers] * workers
        for index in range(requests % workers):
            counts[index] += 1

        ready = threading.Barrier(workers + 1)
        start = threading.Event()

        def rpc_worker(worker_index, count):
            proxy = get_rpc_proxy(
                self.nodes[0].url, worker_index + 100, timeout=60)
            proxy.getblockcount()  # Establish the HTTP connection before timing.
            ready.wait()
            start.wait()

            latencies = []
            for _ in range(count):
                request_start = time.perf_counter()
                result = proxy.getrawtransaction(txid, verbose)
                latencies.append(time.perf_counter() - request_start)
                if verbose:
                    assert_equal(result["hex"], expected_hex)
                else:
                    assert_equal(result, expected_hex)
            return latencies

        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [
                executor.submit(rpc_worker, index, count)
                for index, count in enumerate(counts)
            ]
            ready.wait()
            run_start = time.perf_counter()
            start.set()
            latencies = []
            for future in futures:
                latencies.extend(future.result())
            elapsed = time.perf_counter() - run_start

        assert_equal(len(latencies), requests)
        return {
            "requests": requests,
            "workers": workers,
            "elapsed_s": elapsed,
            "qps": requests / elapsed,
            "p50_ms": self.percentile(latencies, 50) * 1000,
            "p95_ms": self.percentile(latencies, 95) * 1000,
            "p99_ms": self.percentile(latencies, 99) * 1000,
        }

    def start_cs_main_contenders(self, txid, contenders):
        stop = threading.Event()
        ready = threading.Barrier(contenders + 1)
        counts = [0] * contenders
        errors = []

        def contender(worker_index):
            try:
                proxy = get_rpc_proxy(
                    self.nodes[0].url, worker_index + 1000, timeout=60)
                proxy.getblockcount()
                ready.wait()
                while not stop.is_set():
                    result = proxy.getrawtransaction(txid, True)
                    assert_equal(result["txid"], txid)
                    counts[worker_index] += 1
            except Exception as error:
                errors.append(error)
                stop.set()

        threads = [
            threading.Thread(target=contender, args=(index,), daemon=True)
            for index in range(contenders)
        ]
        for thread in threads:
            thread.start()
        ready.wait()

        def finish():
            stop.set()
            for thread in threads:
                thread.join(timeout=60)
            if errors:
                raise errors[0]
            return sum(counts)

        return finish

    def run_test(self):
        if self.options.requests < 1:
            raise AssertionError("--requests must be positive")
        if self.options.workers < 1:
            raise AssertionError("--workers must be positive")
        if self.options.contenders < 1:
            raise AssertionError("--contenders must be positive")

        node = self.nodes[0]
        node.generate(101)
        address = node.getnewaddress()
        txid = node.sendtoaddress(address, 1)
        expected_hex = node.gettransaction(txid)["hex"]
        assert txid in node.getrawmempool()

        # Warm the code path and caches before collecting measurements.
        for _ in range(20):
            assert_equal(node.getrawtransaction(txid, False), expected_hex)

        baseline = self.benchmark(
            txid, expected_hex, False,
            self.options.requests, self.options.workers)
        verbose = self.benchmark(
            txid, expected_hex, True,
            self.options.requests, self.options.workers)

        finish_contenders = self.start_cs_main_contenders(
            txid, self.options.contenders)
        # Let all contenders enter their steady request loop.
        time.sleep(0.2)
        try:
            contended = self.benchmark(
                txid, expected_hex, False,
                self.options.requests, self.options.workers)
        finally:
            contender_requests = finish_contenders()
        if contender_requests == 0:
            raise AssertionError("cs_main contender threads made no requests")

        ratio = contended["qps"] / baseline["qps"]
        report = {
            "non_verbose_baseline": baseline,
            "verbose_baseline": verbose,
            "non_verbose_with_cs_main_contention": contended,
            "contenders": self.options.contenders,
            "contender_requests": contender_requests,
            "contended_to_baseline_qps_ratio": ratio,
        }
        self.log.info("getrawtransaction benchmark:\n%s", json.dumps(
            report, indent=2, sort_keys=True))

        if (self.options.min_contended_ratio > 0 and
                ratio < self.options.min_contended_ratio):
            raise AssertionError(
                "contended/baseline QPS ratio {:.3f} is below {:.3f}".format(
                    ratio, self.options.min_contended_ratio))


if __name__ == "__main__":
    GetRawTransactionPerformanceTest().main()
