# SHA256 hashlock learning example

This example combines existing TBC Script instructions:

```text
OP_SHA256 <32-byte expected digest> OP_EQUAL
```

It adds no consensus opcode, wallet feature, or RPC method. The Python tool
constructs script bytes; the real node's C++ interpreter validates the
transaction when an output is spent.

## Safety and scope

Use isolated regtest coins only. A pure hashlock has no signature requirement:
anyone who learns the preimage can spend the output, including by copying a
preimage revealed in another transaction. The example does not authorize a
particular destination. Do not put real secrets in command-line arguments.

The builder intentionally supports preimages of at most 75 bytes. This is a
learning-tool limit, not TBC's consensus script-size limit. Empty preimages and
single-byte numeric preimages use their exact minimal push encodings.

## Inspect script bytes offline

From the repository root:

```bash
/usr/bin/python3 contrib/script_examples/hashlock.py inspect --preimage hello
```

The text is encoded as UTF-8. JSON output includes the preimage hash, locking
script, unlocking script, byte lengths, and a hash comparison. It is not a
transaction and does not establish mempool acceptance.

Without `--expected-hash`, the tool creates a lock for the supplied preimage.
To test a wrong preimage against the **same** lock, keep the digest fixed:

```bash
/usr/bin/python3 contrib/script_examples/hashlock.py inspect \
  --preimage world \
  --expected-hash 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
```

This prints `hash_matches: false`. A false comparison is a successful
inspection result, not a command error. Invalid arguments exit with code 2.

## Exercise real transactions

First build the project with wallet support in the usual `build/` directory.
The command requires `build/src/bitcoind` and `build/test/config.ini`.

```bash
/usr/bin/python3 contrib/script_examples/hashlock.py demo
```

The existing functional-test framework starts a new isolated node, mines test
funds, confirms a 1 TBC hashlocked output, rejects wrong and missing preimages,
then spends it with `hello` and confirms a 0.999 TBC output. The spending fee
is 0.001 TBC. Node-generated transaction IDs are used for version 10.

The test stops its node. Successful test data is deleted by default. To keep
logs and data, choose a **new** directory:

```bash
/usr/bin/python3 contrib/script_examples/hashlock.py demo \
  --tmpdir /path/to/new/hashlock-test \
  --keep-data
```

An existing directory is rejected, not overwritten. This command does not
connect to an existing node or use its wallet. Do not commit retained test
wallets or RPC configuration.

The offline command uses Python's standard library. The node test depends on
the repository's older `asyncore`-based framework; use Python 3.9-3.11. On the
development Mac, `/usr/bin/python3` provides the verified Python 3.9 runtime.

## Tests

```bash
/usr/bin/python3 contrib/script_examples/test_hashlock.py -v

./build/src/test/test_bitcoin \
  --run_test='script_tests/learning_hashlock_*' \
  --log_level=error --report_level=detailed
```

The Python tests cover actual CLI results and script-builder boundaries. The
C++ cases call the real `VerifyScript()` with correct, wrong, and missing
preimages. The `demo` command covers the complete transaction lifecycle.
Passing these focused tests does not imply that the entire project's older
test suite passes.
