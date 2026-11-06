#!/usr/bin/env python3
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Print ordinary mempool events; reconciliation requires separate RPC queries."""

import argparse
import json
import sys


REASONS = {
    "expired", "included-in-block", "collision-in-block-tx", "reorg"
}


def parse_event(frames):
    if len(frames) != 3 or frames[0] != b"txinmempool" or len(frames[1]) != 32:
        raise ValueError("expected topic, 32-byte transaction ID, and JSON frames")
    metadata = json.loads(frames[2].decode("utf-8"))
    if not isinstance(metadata, dict):
        raise ValueError("metadata must be a JSON object")
    status = metadata.get("status")
    keys = {"epoch", "seq", "status"}
    if status == "DISCARDED":
        keys.add("reason")
        if metadata.get("reason") not in REASONS:
            raise ValueError("unknown removal reason")
    elif status != "ACCEPTED":
        raise ValueError("unknown event status")
    if set(metadata) != keys:
        raise ValueError("unexpected metadata fields")
    for field, minimum in (("epoch", 0), ("seq", 1)):
        value = metadata[field]
        if type(value) is not int or not minimum <= value <= 2**63 - 1:
            raise ValueError("invalid signed 64-bit " + field)
    # The wire ID is already in display order; do not reverse it.
    return dict(txid=frames[1].hex(), **metadata)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("endpoint", nargs="?", default="tcp://127.0.0.1:28333")
    parser.add_argument("--count", type=int, default=0,
                        help="exit after this many valid events (default: unlimited)")
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be nonnegative")

    import zmq

    context = zmq.Context()
    subscriber = context.socket(zmq.SUB)
    previous = None
    received = 0
    try:
        subscriber.setsockopt(zmq.SUBSCRIBE, b"txinmempool")
        subscriber.connect(args.endpoint)
        print("Query the current pool with RPC on initial connection; "
              "missed events are not replayed.", file=sys.stderr, flush=True)
        while not args.count or received < args.count:
            try:
                event = parse_event(subscriber.recv_multipart())
            except (ValueError, TypeError) as error:
                print("Invalid event: {}. Reconcile the current pool with RPC."
                      .format(error), file=sys.stderr, flush=True)
                continue
            position = (event["epoch"], event["seq"])
            if previous is not None:
                if position[0] != previous[0]:
                    print("Epoch changed; reconcile the current pool with RPC.",
                          file=sys.stderr, flush=True)
                elif position[1] != previous[1] + 1:
                    print("Sequence gap or out-of-order event; reconcile the current pool with RPC.",
                          file=sys.stderr, flush=True)
            print(json.dumps(event), flush=True)
            previous = position
            received += 1
    except KeyboardInterrupt:
        pass
    finally:
        subscriber.close(linger=0)
        context.term()


if __name__ == "__main__":
    main()
