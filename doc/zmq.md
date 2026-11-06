# Block and Transaction Broadcasting with ZeroMQ

[ZeroMQ](http://zeromq.org/) is a lightweight wrapper around TCP
connections, inter-process communication, and shared-memory,
providing various message-oriented semantics such as publish/subscribe,
request/reply, and push/pull.

The Bitcoin SV daemon can be configured to act as a trusted "border
router", implementing the bitcoin wire protocol and relay, making
consensus decisions, maintaining the local blockchain database,
broadcasting locally generated transactions into the network, and
providing a queryable RPC interface to interact on a polled basis for
requesting blockchain related data. However, there exists only a
limited service to notify external software of events like the arrival
of new blocks or transactions.

The ZeroMQ facility implements a notification interface through a set
of specific notifiers. Currently there are notifiers that publish
blocks and transactions. This read-only facility requires only the
connection of a corresponding ZeroMQ subscriber port in receiving
software; it is not authenticated nor is there any two-way protocol
involvement. Therefore, subscribers should validate the received data
since it may be out of date, incomplete or even invalid.

ZeroMQ sockets are self-connecting and self-healing; that is,
connections made between two endpoints will be automatically restored
after an outage, and either end may be freely started or stopped in
any order.

Because ZeroMQ is message oriented, subscribers receive transactions
and blocks all-at-once and do not need to implement any sort of
buffering or reassembly.

## Prerequisites

The ZeroMQ feature in Bitcoin SV requires ZeroMQ API version 4.3.2 or
newer. Typically, it is packaged by distributions as something like
*libzmq3-dev*. The C++ wrapper for ZeroMQ is *not* needed.

In order to run the example Python client scripts in contrib/ one must
also install *python3-zmq*, though this is not necessary for daemon
operation.

## Enabling

For Autotools builds, ZeroMQ is enabled by default when the necessary
prerequisites are found; otherwise, configure warns and disables it.
To disable it explicitly, use `--disable-zmq` during the *configure* step:

    $ ./configure --disable-zmq (other options)

For CMake builds, `BUILD_BITCOIN_ZMQ` defaults to `ON` and ZeroMQ is a
required dependency. Missing headers or libraries cause configuration to
fail. Use `-DBUILD_BITCOIN_ZMQ=OFF` to disable ZeroMQ explicitly.

To actually enable operation, one must set the appropriate options on
the command line or in the configuration file.

## Usage

Currently, the following notifications are supported:

    -zmqpubhashtx=address
    -zmqpubhashblock=address
    -zmqpubrawblock=address
    -zmqpubrawtx=address
    -zmqpubtxinmempool=address
    -zmqpubdiscardedfrommempool=address
    -zmqpubremovedfrommempoolblock=address
    -zmqpubhashtxincr=address
    -zmqpubhashblockincr=address
    -zmqpubrawblockincr=address
    -zmqpubrawtxincr=address

The socket type is PUB and the address must be a valid ZeroMQ socket
address. The same address can be used in more than one notification, except
that `txinmempool` cannot share an address with `rawblock`.

For instance:

    $ bitcoind -zmqpubhashtx=tcp://127.0.0.1:28332 \
               -zmqpubrawtx=ipc:///tmp/bitcoind.tx.raw

These options can also be provided in bitcoin.conf.

Each PUB notification has a topic and body, where the header
corresponds to the notification type. For instance, for the
notification `-zmqpubhashtx` the topic is `hashtx` (no null
terminator) and the body is the binary transaction hash (32
bytes in display order; `body.hex()` matches the RPC transaction ID).

`-zmqpubdiscardedfrommempool` and `-zmqpubremovedfrommempoolblock` notification body
is in json format:

`{"txid": hexstring, "reason": string, "collidedWith": {txid: hexstring, size: integer, hex: hexstring}}`.

The optional `collidedWith` field identifies the block transaction that
conflicts with the removed transaction or one of its ancestors. It is only
included by `discardedfrommempool` for `collision-in-block-tx`, when the
conflicting transaction is available. `removedfrommempoolblock` contains
only `txid` and `reason`.

`-zmqpubdiscardedfrommempool` notification will contain one of the following reasons:

- expired, mempool-sizelimit-exceeded, collision-in-block-tx

`collision-in-block-tx` occurs when a connected block contains a transaction
that conflicts with a pool transaction. This applies both to ordinary chain
extension and to reorganization. The conflicting pool transaction and its
descendants are removed with this reason. Likewise, expiry removes the
expired transaction and its descendants with reason `expired`, even if the
descendants have not individually expired.

`-zmqpubremovedfrommempoolblock` notification will contain one of the following reasons:

- reorg, included-in-block

Both legacy removal notifiers use `unknown-reason` as a fallback for removal
reasons outside their respective lists.

`hashtxincr` and `rawtxincr` publish transactions from mempool-admission
signals. Their block connection callback does not publish transactions,
including transactions first seen in a block. Block disconnection also does
not directly publish transactions on these topics; a transaction subsequently
readmitted to the pool can trigger a new notification. The original `hashtx`
and `rawtx` topics additionally publish all transactions in connected and
disconnected blocks.

Outside initial block download (IBD), `hashblockincr` and `rawblockincr`
publish each connected block, including blocks connected during a
reorganization. They do not publish block-disconnection events. See the
remarks below for the original block topics and IBD behavior.

These options can also be provided in bitcoin.conf.

ZeroMQ endpoint specifiers for TCP (and others) are documented in the
[ZeroMQ API](http://api.zeromq.org/master:_start).

Client side, then, the ZeroMQ subscriber socket must have the
ZMQ_SUBSCRIBE option set to one or either of these prefixes (for
instance, just `hash`); without doing so will result in no messages
arriving. Please see `contrib/zmq/zmq_sub.py` for a working example.

## Ordinary mempool notifications

Enable the `txinmempool` topic with a TCP or IPC endpoint:

```sh
bitcoind -zmqpubtxinmempool=tcp://127.0.0.1:28333
# Alternative:
bitcoind -zmqpubtxinmempool=ipc:///tmp/bitcoind.mempool
```

The configuration file equivalent is
`zmqpubtxinmempool=tcp://127.0.0.1:28333`. This topic can share an endpoint
with `hashtx`, `rawtx`, or `hashblock`, but not `rawblock`. Invalid endpoints,
bind conflicts, or failure to initialize the epoch counter prevent startup.
The existing four topics retain their behavior.

When configured, the node records events and submits notifications even if
there are no subscribers. When not configured, this event state is disabled.

### Wire format

Every message has exactly three frames:

| Frame | Content |
| --- | --- |
| 1 | ASCII `txinmempool`, without a null terminator |
| 2 | 32-byte binary transaction ID in display order; use `body.hex()` without reversing it |
| 3 | UTF-8 JSON metadata as shown below |

```text
{"epoch":1,"seq":1,"status":"ACCEPTED"}
{"epoch":1,"seq":2,"status":"DISCARDED","reason":"included-in-block"}
```

`ACCEPTED` has exactly `epoch`, `seq`, and `status`; `DISCARDED` also has
`reason`. There is no transaction ID in the JSON and no fourth frame.
`epoch` and `seq` are signed 64-bit integers, not strings. Clients must
preserve integer precision (including values above JavaScript's safe integer
range). Compare sequence numbers only within the same epoch and node.

An enabled stream has a nonnegative epoch and an initial published position
of `0`. The first event is `1`; each event advances the sequence. The epoch
stays constant during a run and changes after restart. The node persists
only the epoch counter in `txinmempool.epoch` in the network data directory;
sequence numbers and historical events are not persisted. Preserve this file
with the data directory to avoid reusing epochs. A corrupt or exhausted
counter causes startup to fail.

### Event semantics

`ACCEPTED` means a transaction actually entered the ordinary mempool.
Ancestors are accepted before their descendants. Duplicate submissions,
rejected transactions, transactions held only as orphans or non-final
transactions, coinbase transactions, and transactions seen only in a block
do not produce this event. Moving a non-final transaction into the ordinary
pool does produce it.

`DISCARDED` means a transaction actually left the ordinary mempool:

| Reason | Removal |
| --- | --- |
| `expired` | Expiry of this transaction or an ancestor |
| `included-in-block` | Inclusion in a connected block |
| `collision-in-block-tx` | This transaction or an ancestor conflicts with a transaction in a connected block |
| `reorg` | Removal following a reorganization |

The current admission size limit rejects transactions before insertion; it
does not perform size-limit eviction. Such rejections produce no
`txinmempool` event. This topic supports only the four reasons listed above.

Each actual entry or exit produces one event, except for file restoration
below. Mining an existing pool transaction produces a discard, without
another acceptance. Re-entry after removal, including reorganization
readmission, gets a new acceptance position. No ordering is promised among
discarded transactions. Events are queued after the pool mutation takes
effect, and sent by a worker without holding the mempool lock.

Transactions restored from `mempool.dat` do not produce `ACCEPTED`, receive
an admission position, or advance the published position. They remain
queryable through pool RPCs. Later removal or re-entry follows the normal
notification rules. Restoration does not replay previous events.

### RPC positions and current pool queries

`getzmqtipsnapshot` takes no parameters and returns both integer fields:

```sh
bitcoin-cli getzmqtipsnapshot
# Enabled, before any successful submission: {"epoch":1,"seq":0}
# Enabled, after the two example events:     {"epoch":1,"seq":2}
# Disabled:                                 {"epoch":-1,"seq":-1}
```

The returned pair describes one observation of the last complete message
successfully submitted to ZeroMQ. "Published" means submission succeeded;
it does not mean any client received the message. Queued or in-flight
messages do not advance this position. The RPC is also available in builds
without ZeroMQ, returning the disabled pair.

`getmempoolentry <txid>` preserves its existing fields and adds top-level
`epoch` and `seq` when the current entry has an acceptance in this epoch.
Both values match that entry's `ACCEPTED` event, even if it is still queued.
They describe admission, not the stream's latest published position, so an
entry's sequence can temporarily exceed the snapshot sequence. Both fields
are omitted when notifications are disabled or the entry was restored
without an acceptance event. A missing entry retains the existing
`-5 / Transaction not in mempool` error. The verbose schemas of
`getrawmempool`, `getmempoolancestors`, and `getmempooldescendants` are unchanged.

If the notification stream fails, `getzmqtipsnapshot` and notification state
reads for existing entries return `RPC_MISC_ERROR (-1)` instead of a stale
position or the disabled pair. A missing entry still returns `-5`.
Send failure, event allocation failure, or exhaustion of the 65,536-event
pending queue stops the stream and logs an error; normal pool operations
continue. Inspect the node log and restart after resolving the cause. The
new run uses a new epoch and clients must reconcile again.

### Subscription and reconciliation

A minimal subscriber is provided in
[`contrib/zmq/txinmempool_sub.py`](../contrib/zmq/txinmempool_sub.py):

```sh
python3 contrib/zmq/txinmempool_sub.py tcp://127.0.0.1:28333
# IPC works with the same client:
python3 contrib/zmq/txinmempool_sub.py ipc:///tmp/bitcoind.mempool
```

The example validates and prints events, and reports epoch changes and
sequence gaps. It does not maintain a local pool or perform RPC reconciliation.
A late subscriber may see a first sequence greater than one. Connection
establishment is asynchronous and there is no history replay.

Clients should query the current pool on initial connection and reconcile
again after reconnecting, changing epochs, or observing a sequence gap.
Use `getrawmempool` to enumerate current entries and `getmempoolentry` to
inspect them. Restored entries without positions must still be included.
Periodic `getzmqtipsnapshot` checks can reveal that the publisher has
advanced beyond the client's last event even when no later message arrives;
a difference may also mean delivery is still pending.

The snapshot contains only the notification position. It is not an atomic
snapshot of that position and subsequent pool RPC calls. The pool can change
during enumeration; an entry may disappear before it is queried, and callers
must retry or reconcile ongoing changes as needed. These APIs provide
positions and current pool queries only, with no historical message query or
replay. Reconciliation recovers current state, not the complete event history.

## Remarks

From the perspective of bitcoind, the ZeroMQ socket is write-only; PUB
sockets do not track application receipt. The node does not track subscribers.
The `txinmempool` notifier maintains its own event positions and current
admission metadata, as described above.

No authentication or authorization is done on connecting clients; it
is assumed that the ZeroMQ port is exposed only to trusted entities,
using other means such as firewalling.

The original `hashblock` and `rawblock` topics notify the new tip rather
than each connected block in a reorganization. They skip notifications
during initial block download (IBD), and when blocks are only disconnected
without connecting a new block. The incremental block topics also skip
notifications during IBD. Subscribers must retrieve any missing chain data
from their last known block to the new tip. These IBD filters apply to the
block topics; `txinmempool` follows the actual pool changes described above.

There are several possibilities that ZMQ notification can get lost
during transmission depending on the communication type your are
using. Legacy notifications append a four-byte little-endian sequence
number as their third frame, starting at zero for each notifier.
`txinmempool` instead uses the JSON third frame described above; it has no
fourth sequence frame. Successful submission to ZeroMQ does not guarantee
delivery, and reconnection does not replay missed messages.
