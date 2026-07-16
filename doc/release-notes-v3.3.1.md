# TBCNODE software version 3.3.1 Release Notes

This release updates the mempool admission and fee policy, simplifies mempool
chain accounting, and improves RPC and P2P robustness and performance.

## Important Changes

### Mempool admission and fee policy

* The mempool is no longer trimmed by evicting transactions when it approaches
  `-maxmempool`. `-maxmempool` is now a hard memory limit and new transactions
  are rejected once the limit is reached.
* The rolling minimum fee and eviction-based fee calculation have been replaced
  by a deterministic admission fee curve. The required fee rate remains at the
  configured floor until the ramp start, then rises hyperbolically as memory
  usage approaches `-maxmempool`.
* The default mempool admission fee floor is 60 satoshis/kB. The default
  `-blockmintxfee` value is also reduced from 500 to 60 satoshis/kB.
* Free consolidation transactions are accepted only while mempool usage is in
  the flat-fee portion of the curve. Above the ramp start, consolidation
  transactions must pay the current dynamic minimum fee.
* The `relayfee` values returned by `getinfo` and `getnetworkinfo` now report the
  current dynamic mempool minimum fee.
* Wallet fee selection and validation now use the current dynamic mempool
  minimum fee.

### Configuration changes

The following startup options have been added:

* `-mempoolminfeerate=<n>` sets the admission fee floor in satoshis per kB. The
  default is 60.
* `-mempoolfeerampstart=<n>` sets the mempool usage at which the admission fee
  starts increasing. The default is 500 MB; byte-size suffixes are supported.

The following legacy options are no longer supported or no longer affect
policy:

* `-minrelaytxfee`, `-limitfreerelay`, and `-relaypriority` have been removed in
  favor of the new mempool admission fee policy.
* `-limitdescendantcount` and `-limitdescendantsize` no longer control mempool
  admission. `-limitancestorsize` is also removed. `-limitancestorcount` now
  limits ancestor chain height and remains the wallet long-chain limit.
* The legacy block assembler has been removed. `-blockassembler` supports only
  `JOURNALING`; `LEGACY` and unknown values fall back to the default journaling
  assembler.
* `-blockprioritypercentage` is retained for configuration compatibility but
  has no effect. The obsolete `-printpriority` help option has been removed.

Node operators should remove obsolete options from configuration files and
update monitoring that relies on the changed RPC fields described below.

### RPC interface changes

* `getmempoolinfo` adds `mempoolfloorfee`, the configured flat admission fee
  floor, and `mempoolfeerampstart`, the ramp-start memory usage in bytes.
  `mempoolminfee` now represents the dynamic fee curve evaluated at current
  mempool usage.
* The verbose transaction objects returned by `getrawmempool true` and
  `getmempoolentry` no longer include `ancestorcount`, `ancestorsize`,
  `ancestorfees`, `descendantcount`, `descendantsize`, or `descendantfees`.
* Non-verbose `getrawtransaction` requests for transactions in the mempool use
  a lock-reduced fast path. The response format is unchanged.
* `getblockstats` no longer inserts unknown block hashes into the internal block
  index while reporting a "Block not found" error.

## Reliability and Internal Changes

* Harden `OP_PARTIAL_HASH` midstate validation by rejecting oversized length
  encodings and partial lengths that are not SHA-256 block aligned.
* Harden compact-block reconstruction against duplicate or invalid `blocktxn`
  handling instead of terminating on internal assertions.
* Remove cached aggregate ancestor and descendant state from mempool entries.
  Topological journal ordering now uses insertion order and correctly repairs
  ordering after chain reorganizations.
* Remove the legacy mining block assembler and its associated mempool indexes
  and bookkeeping.

## List of Pull Requests

* #16: Fix `OP_PARTIAL_HASH` midstate validation.
* #17: Introduce the deterministic mempool fee curve and no-trim policy.
* #18: Remove legacy mempool accounting and block assembly paths, and harden
  compact-block transaction handling.
* #19: Complete mempool state cleanup and align wallet chain-limit behavior.
* #20: Fix `getblockstats` block-index map poisoning.
* #21: Optimize non-verbose mempool `getrawtransaction` requests.

## Scaling Test Network (STN) Reset

N/A

# Previous Releases

* [Version 3.3.0](https://github.com/TuringBitChain/TBCNODE/tree/v3.3.0) - 2026-04-03
