
### Table of Contents
1. [Overview](#Overview)<br />
2. [Methods](#Methods)<br />
3. [Method Details](#SomeMethodDetails)<br />


### 1. Overview

<a name="Overview" />

TBC bitcoind provides a [JSON-RPC](http://json-rpc.org/wiki/specification) API.  

The original bitcoind/bitcoin-qt JSON-RPC API documentation is available at 
[https://en.bitcoin.it/wiki/Original_Bitcoin_client/API_Calls_list](https://en.bitcoin.it/wiki/Original_Bitcoin_client/API_Calls_list)


### 2. Methods 

<a name="Methods" />

The following is an overview of the RPC methods and their current status.  Click
the method name for further details such as parameter and return information.

Method|Description|
|------|----------|
|[addnode](#addnode)|Attempts to add or remove a persistent peer.|
|[createrawtransaction](#createrawtransaction)|Returns a new transaction spending the provided inputs and sending to the provided addresses.|
|[decoderawtransaction](#decoderawtransaction)|Returns a JSON object representing the provided serialized, hex-encoded transaction.|
|[decodescript](#decodescript)|Returns a JSON object with information about the provided hex-encoded script.|
|[getaddednodeinfo](#getaddednodeinfo)|Returns information about manually added (persistent) peers.|
|[getbestblockhash](#getbestblockhash)|Returns the hash of the of the best (most recent) block in the longest block chain.|
|[getblock](#getblock)|Returns information about a block given its hash.|
|[getblockcount](#getblockcount)|Returns the number of blocks in the longest block chain.|
|[getblockhash](#getblockhash)|Returns hash of the block in best block chain at the given height.|
|[getblockheader](#getblockheader)|Returns the block header of the block.|
|[getconnectioncount](#getconnectioncount)|Returns the number of active connections to other peers.|
|[getdifficulty](#getdifficulty)|Returns the proof-of-work difficulty as a multiple of the minimum difficulty.|
|[getinfo](#getinfo)|Returns a JSON object containing various state info.|
|[getmempoolinfo](#getmempoolinfo)|Returns a JSON object containing mempool-related information.|
|[getmininginfo](#getmininginfo)|Returns a JSON object containing mining-related information.|
|[getnettotals](#getnettotals)|Returns a JSON object containing network traffic statistics.|
|[getnetworkhashps](#getnetworkhashps)|Returns the estimated network hashes per second for the block heights provided by the parameters.|
|[getpeerinfo](#getpeerinfo)|Returns information about each connected network peer as an array of json objects.|
|[getrawmempool](#getrawmempool)|Returns an array of hashes for all of the transactions currently in the memory pool.|
|[getrawtransaction](#getrawtransaction)|Returns information about a transaction given its hash.|
|[help](#help)|Returns a list of all commands or help for a specified command.|
|[ping](#ping)|Queues a ping to be sent to each connected peer.|
|[sendrawtransaction](#sendrawtransaction)|Submits the serialized, hex-encoded transaction to the local peer and relays it to the network.<br />|
|[stop](#stop)|Shutdown bitcoind.|
|[submitblock](#submitblock)|Attempts to submit a new serialized, hex-encoded block to the network.|
|[validateaddress](#validateaddress)|Verifies the given address is valid.|
|[verifychain](#verifychain)|Verifies the block chain database.|
|verifytxoutproof|Verifies that a proof points to a transaction in a block, returning the transaction it commits to and throwing an RPC error if the block is not in our best chain.|


### 3. Method Details

<a name="SomeMethodDetails" />

<a name="addnode"/>

|   |   |
|---|---|
|Method|addnode|
|Parameters|1. peer (string, required) - ip address and port of the peer to operate on<br />2. command (string, required) - `add` to add a persistent peer, `remove` to remove a persistent peer, or `onetry` to try a single connection to a peer|
|Description|Attempts to add or remove a persistent peer.|
|Returns|Nothing|
[Return to Overview](#MethodOverview)<br />

***
<a name="createrawtransaction"/>

|   |   |
|---|---|
|Method|createrawtransaction|
|Parameters|1. transaction inputs (JSON array, required) - json array of json objects<br />`[`<br />&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"txid": "hash", (string, required) the hash of the input transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"vout": n  (numeric, required) the specific output of the input transaction to redeem`<br />&nbsp;&nbsp;`}, ...`<br />`]`<br />2. addresses and amounts (JSON object, required) - json object with addresses as keys and amounts as values<br />`{`<br />&nbsp;&nbsp;`"address": n.nnn (numeric, required) the address to send to as the key and the amount in BSV as the value`<br />&nbsp;&nbsp;`, ...`<br />`}`<br />3. locktime (int64, optional, default=0) - specifies the transaction locktime.  If non-zero, the inputs will also have their locktimes activated. |
|Description|Returns a new transaction spending the provided inputs and sending to the provided addresses.<br />The transaction inputs are not signed in the created transaction.<br />The `signrawtransaction` RPC command provided by wallet must be used to sign the resulting transaction.|
|Returns|`"transaction" (string) hex-encoded bytes of the serialized transaction`|
|Example Parameters|1. transaction inputs `[{"txid":"e6da89de7a6b8508ce8f371a3d0535b04b5e108cb1a6e9284602d3bfd357c018","vout":1}]`<br />2. addresses and amounts `{"13cgrTP7wgbZYWrY9BZ22BV6p82QXQT3nY": 0.49213337}`<br />3. locktime `0`|
|Example Return|`010000000118c057d3bfd3024628e9a6b18c105e4bb035053d1a378fce08856b7ade89dae6010000`<br />`0000ffffffff0199efee02000000001976a9141cb013db35ecccc156fdfd81d03a11c51998f99388`<br />`ac00000000`<br /><font color="orange">**Newlines added for display purposes.  The actual return does not contain newlines.**</font>|
[Return to Overview](#MethodOverview)<br />

***
<a name="decoderawtransaction"/>

|   |   |
|---|---|
|Method|decoderawtransaction|
|Parameters|1. data (string, required) - serialized, hex-encoded transaction|
|Description|Returns a JSON object representing the provided serialized, hex-encoded transaction.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"txid": "hash",  (string) the hash of the transaction`<br />&nbsp;&nbsp;`"version": n,  (numeric) the transaction version`<br />&nbsp;&nbsp;`"locktime": n,  (numeric) the transaction lock time`<br />&nbsp;&nbsp;`"vin": [  (array of json objects) the transaction inputs as json objects`<br />&nbsp;&nbsp;<font color="orange">For coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"coinbase": "data",  (string) the hex-encoded bytes of the signature script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": n,  (numeric) the script sequence number`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;<font color="orange">For non-coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"txid": "hash", (string) the hash of the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"vout": n, (numeric) the index of the output being redeemed from the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptSig": { (json object) the signature script used to redeem the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "asm", (string) disassembly of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "data",  (string) hex-encoded bytes of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": n,  (numeric) the script sequence number`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}, ...`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"vout": [  (array of json objects) the transaction outputs as json objects`<br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"value": n, (numeric) the value in BSV`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"n": n, (numeric) the index of this transaction output`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptPubKey": { (json object) the public key script used to pay coins`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "asm",  (string) disassembly of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "data", (string) hex-encoded bytes of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"reqSigs": n,  (numeric) the number of required signatures`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"type": "scripttype" (string) the type of the script (e.g. 'pubkeyhash')`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [ (json array of string) the bitcoin addresses associated with this output`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"bitcoinaddress",  (string) the bitcoin address`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`...`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}, ...`<br />&nbsp;&nbsp;`]`<br />`}`|
|Example Return|`{`<br />&nbsp;&nbsp;`"txid": "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b",`<br />&nbsp;&nbsp;`"version": 1,`<br />&nbsp;&nbsp;`"locktime": 0,`<br />&nbsp;&nbsp;`"vin": [`<br />&nbsp;&nbsp;<font color="orange">For coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"coinbase": "04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": 4294967295,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;<font color="orange">For non-coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"txid": "60ac4b057247b3d0b9a8173de56b5e1be8c1d1da970511c626ef53706c66be04",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"vout": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptSig": {`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "3046022100cb42f8df44eca83dd0a727988dcde9384953e830b1f8004d57485e2ede1b9c8f0...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "493046022100cb42f8df44eca83dd0a727988dcde9384953e830b1f8004d57485e2ede1b9c8...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": 4294967295,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"vout": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"value": 50,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"n": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptPubKey": {`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4ce...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "4104678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"reqSigs": 1,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"type": "pubkey"`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;`]`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="decodescript"/>

|   |   |
|---|---|
|Method|decodescript|
|Parameters|1. script (string, required) - hex-encoded script|
|Description|Returns a JSON object with information about the provided hex-encoded script.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"asm": "asm",  (string) disassembly of the script`<br />&nbsp;&nbsp;`"reqSigs": n,  (numeric) the number of required signatures`<br />&nbsp;&nbsp;`"type": "scripttype",  (string) the type of the script (e.g. 'pubkeyhash')`<br />&nbsp;&nbsp;`"addresses": [ (json array of string) the bitcoin addresses associated with this script`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"bitcoinaddress",  (string) the bitcoin address`<br />&nbsp;&nbsp;&nbsp;&nbsp;`...`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"p2sh": "scripthash",  (string) the script hash for use in pay-to-script-hash transactions`<br />`}`|
|Example Return|`{`<br />&nbsp;&nbsp;`"asm": "OP_DUP OP_HASH160 b0a4d8a91981106e4ed85165a66748b19f7b7ad4 OP_EQUALVERIFY OP_CHECKSIG",`<br />&nbsp;&nbsp;`"reqSigs": 1,`<br />&nbsp;&nbsp;`"type": "pubkeyhash",`<br />&nbsp;&nbsp;`"addresses": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"1H71QVBpzuLTNUh5pewaH3UTLTo2vWgcRJ"`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"p2sh": "359b84ff799f48231990ff0298206f54117b08b6"`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getaddednodeinfo"/>

|   |   |
|---|---|
|Method|getaddednodeinfo|
|Parameters|1. dns (boolean, required) - specifies whether the returned data is a JSON object including DNS and connection information, or just a list of added peers<br />2. node (string, optional) - only return information about this specific peer instead of all added peers.|
|Description|Returns information about manually added (persistent) peers.|
|Returns (dns=false)|`["ip:port", ...]`|
|Returns (dns=true)|`[ (json array of objects)`<br />&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addednode": "ip_or_domain",  (string) the ip address or domain of the added peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"connected": true or false,  (boolean) whether or not the peer is currently connected`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [  (json array or objects) DNS lookup and connection information about the peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"address": "ip",  (string) the ip address for this DNS entry`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"connected": "inbound/outbound/false"  (string) the connection 'direction' (if connected)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}, ...`<br />&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`}, ...`<br />`]`|
|Example Return (dns=false)|`["192.168.0.10:8333", "mydomain.org:8333"]`|
|Example Return (dns=true)|`[`<br />&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addednode": "mydomain.org:8333",`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"connected": true,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"address": "1.2.3.4",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"connected": "outbound"`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`},`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"address": "5.6.7.8",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"connected": "false"`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`}`<br />`]`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getbestblockhash"/>

|   |   |
|---|---|
|Method|getbestblockhash|
|Parameters|None|
|Description|Returns the hash of the of the best (most recent) block in the longest block chain.|
|Returns|string|
|Example Return|`0000000000000001f356adc6b29ab42b59f913a396e170f80190dba615bd1e60`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getblock"/>

|   |   |
|---|---|
|Method|getblock|
|Parameters|1. block hash (string, required) - the hash of the block<br />2. verbose (boolean, optional, default=true) - specifies the block is returned as a JSON object instead of hex-encoded string<br />|
|Description|Returns information about a block given its hash.|
|Returns (verbose=false)|`"data" (string) hex-encoded bytes of the serialized block`|
|Returns (verbose=true, verbosetx=false)|`{ (json object)`<br />&nbsp;&nbsp;`"hash": "blockhash",  (string) the hash of the block (same as provided)`<br />&nbsp;&nbsp;`"confirmations": n,  (numeric) the number of confirmations`<br />&nbsp;&nbsp;`"size": n,  (numeric) the size of the block`<br />&nbsp;&nbsp;`"height": n,  (numeric) the height of the block in the block chain`<br />&nbsp;&nbsp;`"version": n,  (numeric) the block version`<br />&nbsp;&nbsp;`"merkleroot": "hash",  (string) root hash of the merkle tree`<br />&nbsp;&nbsp;`"tx": [ (json array of string) the transaction hashes`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"transactionhash",  (string) hash of the parent transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;`...`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"time": n,  (numeric) the block time in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;`"nonce": n,  (numeric) the block nonce`<br />&nbsp;&nbsp;`"bits", n,  (numeric) the bits which represent the block difficulty`<br />&nbsp;&nbsp;`difficulty: n.nn,  (numeric) the proof-of-work difficulty as a multiple of the minimum difficulty`<br />&nbsp;&nbsp;`"previousblockhash": "hash",  (string) the hash of the previous block`<br />&nbsp;&nbsp;`"nextblockhash": "hash",  (string) the hash of the next block (only if there is one)`<br />`}`|
|Returns (verbose=true, verbosetx=true)|`{ (json object)`<br />&nbsp;&nbsp;`"hash": "blockhash",  (string) the hash of the block (same as provided)`<br />&nbsp;&nbsp;`"confirmations": n,  (numeric) the number of confirmations`<br />&nbsp;&nbsp;`"size": n,  (numeric) the size of the block`<br />&nbsp;&nbsp;`"height": n,  (numeric) the height of the block in the block chain`<br />&nbsp;&nbsp;`"version": n,  (numeric) the block version`<br />&nbsp;&nbsp;`"merkleroot": "hash",  (string) root hash of the merkle tree`<br />&nbsp;&nbsp;`"rawtx": [ (array of json objects) the transactions as json objects`<br />&nbsp;&nbsp;&nbsp;&nbsp;`(see getrawtransaction json object details)`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"time": n,  (numeric) the block time in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;`"nonce": n,  (numeric) the block nonce`<br />&nbsp;&nbsp;`"bits", n,  (numeric) the bits which represent the block difficulty`<br />&nbsp;&nbsp;`difficulty: n.nn,  (numeric) the proof-of-work difficulty as a multiple of the minimum difficulty`<br />&nbsp;&nbsp;`"previousblockhash": "hash",  (string) the hash of the previous block`<br />&nbsp;&nbsp;`"nextblockhash": "hash",  (string) the hash of the next block`<br />`}`|
|Example Return (verbose=false)|`"010000000000000000000000000000000000000000000000000000000000000000000000`<br />`3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a29ab5f49`<br />`ffff001d1dac2b7c01010000000100000000000000000000000000000000000000000000`<br />`00000000000000000000ffffffff4d04ffff001d0104455468652054696d65732030332f`<br />`4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f`<br />`6e64206261696c6f757420666f722062616e6b73ffffffff0100f2052a01000000434104`<br />`678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f`<br />`4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5fac00000000"`<br /><font color="orange">**Newlines added for display purposes.  The actual return does not contain newlines.**</font>|
|Example Return (verbose=true, verbosetx=false)|`{`<br />&nbsp;&nbsp;`"hash": "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",`<br />&nbsp;&nbsp;`"confirmations": 277113,`<br />&nbsp;&nbsp;`"size": 285,`<br />&nbsp;&nbsp;`"height": 0,`<br />&nbsp;&nbsp;`"version": 1,`<br />&nbsp;&nbsp;`"merkleroot": "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b",`<br />&nbsp;&nbsp;`"tx": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"`<br />&nbsp;&nbsp;`],`<br />&nbsp;&nbsp;`"time": 1231006505,`<br />&nbsp;&nbsp;`"nonce": 2083236893,`<br />&nbsp;&nbsp;`"bits": "1d00ffff",`<br />&nbsp;&nbsp;`"difficulty": 1,`<br />&nbsp;&nbsp;`"previousblockhash": "0000000000000000000000000000000000000000000000000000000000000000",`<br />&nbsp;&nbsp;`"nextblockhash": "00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048"`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getblockcount"/>

|   |   |
|---|---|
|Method|getblockcount|
|Parameters|None|
|Description|Returns the number of blocks in the longest block chain.|
|Returns|numeric|
|Example Return|`276820`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getblockhash"/>

|   |   |
|---|---|
|Method|getblockhash|
|Parameters|1. block height (numeric, required)|
|Description|Returns hash of the block in best block chain at the given height.|
|Returns|string|
|Example Return|`000000000000000096579458d1c0f1531fcfc58d57b4fce51eb177d8d10e784d`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getblockheader"/>

|   |   |
|---|---|
|Method|getblockheader|
|Parameters|1. block hash (string, required) - the hash of the block<br />2. verbose (boolean, optional, default=true) - specifies the block header is returned as a JSON object instead of a hex-encoded string|
|Description|Returns hex-encoded bytes of the serialized block header.|
|Returns (verbose=false)|`"data" (string) hex-encoded bytes of the serialized block`|
|Returns (verbose=true)|`{ (json object)`<br />&nbsp;&nbsp;`"hash": "blockhash", (string) the hash of the block (same as provided)`<br />&nbsp;&nbsp;`"confirmations": n,  (numeric) the number of confirmations`<br />&nbsp;&nbsp;`"height": n, (numeric) the height of the block in the block chain`<br />&nbsp;&nbsp;`"version": n,  (numeric) the block version`<br />&nbsp;&nbsp;`"merkleroot": "hash",  (string) root hash of the merkle tree`<br />&nbsp;&nbsp;`"time": n,  (numeric) the block time in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;`"nonce": n,  (numeric) the block nonce`<br />&nbsp;&nbsp;`"bits": n,  (numeric) the bits which represent the block difficulty`<br />&nbsp;&nbsp;`"difficulty": n.nn,  (numeric) the proof-of-work difficulty as a multiple of the minimum difficulty`<br />&nbsp;&nbsp;`"previousblockhash": "hash",  (string) the hash of the previous block`<br />&nbsp;&nbsp;`"nextblockhash": "hash",  (string) the hash of the next block (only if there is one)`<br />`}`|
|Example Return (verbose=false)|`"0200000035ab154183570282ce9afc0b494c9fc6a3cfea05aa8c1add2ecc564900000000`<br />`38ba3d78e4500a5a7570dbe61960398add4410d278b21cd9708e6d9743f374d544fc0552`<br />`27f1001c29c1ea3b"`<br /><font color="orange">**Newlines added for display purposes.  The actual return does not contain newlines.**</font>|
|Example Return (verbose=true)|`{`<br />&nbsp;&nbsp;`"hash": "00000000009e2958c15ff9290d571bf9459e93b19765c6801ddeccadbb160a1e",`<br />&nbsp;&nbsp;`"confirmations": 392076,`<br />&nbsp;&nbsp;`"height": 100000,`<br />&nbsp;&nbsp;`"version": 2,`<br />&nbsp;&nbsp;`"merkleroot": "d574f343976d8e70d91cb278d21044dd8a396019e6db70755a0a50e4783dba38",`<br />&nbsp;&nbsp;`"time": 1376123972,`<br />&nbsp;&nbsp;`"nonce": 1005240617,`<br />&nbsp;&nbsp;`"bits": "1c00f127",`<br />&nbsp;&nbsp;`"difficulty": 271.75767393,`<br />&nbsp;&nbsp;`"previousblockhash": "000000004956cc2edd1a8caa05eacfa3c69f4c490bfc9ace820257834115ab35",`<br />&nbsp;&nbsp;`"nextblockhash": "0000000000629d100db387f37d0f37c51118f250fb0946310a8c37316cbc4028"`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getconnectioncount"/>

|   |   |
|---|---|
|Method|getconnectioncount|
|Parameters|None|
|Description|Returns the number of active connections to other peers|
|Returns|numeric|
|Example Return|`8`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getdifficulty"/>

|   |   |
|---|---|
|Method|getdifficulty|
|Parameters|None|
|Description|Returns the proof-of-work difficulty as a multiple of the minimum difficulty.|
|Returns|numeric|
|Example Return|`1180923195.260000`|
[Return to Overview](#MethodOverview)<br />


***
<a name="getinfo"/>

|   |   |
|---|---|
|Method|getinfo|
|Parameters|None|
|Description|Returns a JSON object containing various state info.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"version": n,  (numeric) the version of the server`<br />&nbsp;&nbsp;`"protocolversion": n,  (numeric) the latest supported protocol version`<br />&nbsp;&nbsp;`"blocks": n,  (numeric) the number of blocks processed`<br />&nbsp;&nbsp;`"timeoffset": n,  (numeric) the time offset`<br />&nbsp;&nbsp;`"connections": n,  (numeric) the number of connected peers`<br />&nbsp;&nbsp;`"proxy": "host:port",  (string) the proxy used by the server`<br />&nbsp;&nbsp;`"difficulty": n.nn,  (numeric) the current target difficulty`<br />&nbsp;&nbsp;`"testnet": true or false,  (boolean) whether or not server is using testnet`<br />&nbsp;&nbsp;`"relayfee": n.nn,  (numeric) the minimum relay fee for non-free transactions in BSV/KB`<br />`}`|
|Example Return|`{`<br />&nbsp;&nbsp;`"version": 70000`<br />&nbsp;&nbsp;`"protocolversion": 70001,  `<br />&nbsp;&nbsp;`"blocks": 298963,`<br />&nbsp;&nbsp;`"timeoffset": 0,`<br />&nbsp;&nbsp;`"connections": 17,`<br />&nbsp;&nbsp;`"proxy": "",`<br />&nbsp;&nbsp;`"difficulty": 8000872135.97,`<br />&nbsp;&nbsp;`"testnet": false,`<br />&nbsp;&nbsp;`"relayfee": 0.00001,`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getmempoolinfo"/>

|   |   |
|---|---|
|Method|getmempoolinfo|
|Parameters|None|
|Description|Returns a JSON object containing mempool-related information.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"bytes": n,  (numeric) size in bytes of the mempool`<br />&nbsp;&nbsp;`"size": n,  (numeric) number of transactions in the mempool`<br />`}`|
Example Return|`{`<br />&nbsp;&nbsp;`"bytes": 310768,`<br />&nbsp;&nbsp;`"size": 157,`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getmininginfo"/>

|   |   |
|---|---|
|Method|getmininginfo|
|Parameters|None|
|Description|Returns a JSON object containing mining-related information.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"blocks": n,  (numeric) latest best block`<br />&nbsp;&nbsp;`"currentblocksize": n,  (numeric) size of the latest best block`<br />&nbsp;&nbsp;`"currentblocktx": n,  (numeric) number of transactions in the latest best block`<br />&nbsp;&nbsp;`"difficulty": n.nn,  (numeric) current target difficulty`<br />&nbsp;&nbsp;`"errors": "errors",  (string) any current errors`<br />&nbsp;&nbsp;`"generate": true or false,  (boolean) whether or not server is set to generate coins`<br />&nbsp;&nbsp;`"genproclimit": n,  (numeric) number of processors to use for coin generation (-1 when disabled)`<br />&nbsp;&nbsp;`"hashespersec": n,  (numeric) recent hashes per second performance measurement while generating coins`<br />&nbsp;&nbsp;`"networkhashps": n,  (numeric) estimated network hashes per second for the most recent blocks`<br />&nbsp;&nbsp;`"pooledtx": n,  (numeric) number of transactions in the memory pool`<br />&nbsp;&nbsp;`"testnet": true or false,  (boolean) whether or not server is using testnet`<br />`}`|
|Example Return|`{`<br />&nbsp;&nbsp;`"blocks": 236526,`<br />&nbsp;&nbsp;`"currentblocksize": 185,`<br />&nbsp;&nbsp;`"currentblocktx": 1,`<br />&nbsp;&nbsp;`"difficulty": 256,`<br />&nbsp;&nbsp;`"errors": "",`<br />&nbsp;&nbsp;`"generate": false,`<br />&nbsp;&nbsp;`"genproclimit": -1,`<br />&nbsp;&nbsp;`"hashespersec": 0,`<br />&nbsp;&nbsp;`"networkhashps": 33081554756,`<br />&nbsp;&nbsp;`"pooledtx": 8,`<br />&nbsp;&nbsp;`"testnet": true,`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getnettotals"/>

|   |   |
|---|---|
|Method|getnettotals|
|Parameters|None|
|Description|Returns a JSON object containing network traffic statistics.|
|Returns|`{`<br />&nbsp;&nbsp;`"totalbytesrecv": n,  (numeric) total bytes received`<br />&nbsp;&nbsp;`"totalbytessent": n,  (numeric) total bytes sent`<br />&nbsp;&nbsp;`"timemillis": n  (numeric) number of milliseconds since 1 Jan 1970 GMT`<br />`}`|
|Example Return|`{`<br />&nbsp;&nbsp;`"totalbytesrecv": 1150990,`<br />&nbsp;&nbsp;`"totalbytessent": 206739,`<br />&nbsp;&nbsp;`"timemillis": 1391626433845`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getnetworkhashps"/>

|   |   |
|---|---|
|Method|getnetworkhashps|
|Parameters|1. blocks (numeric, optional, default=120) - The number of blocks, or -1 for blocks since last difficulty change<br />2. height (numeric, optional, default=-1) - Perform estimate ending with this height or -1 for current best chain block height|
|Description|Returns the estimated network hashes per second for the block heights provided by the parameters.|
|Returns|numeric|
|Example Return|`6573971939`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getpeerinfo"/>

|   |   |
|---|---|
|Method|getpeerinfo|
|Parameters|None|
|Description|Returns data about each connected network peer as an array of json objects.|
|Returns|`[`<br />&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addr": "host:port",  (string) the ip address and port of the peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"services": "00000001",  (string) the services supported by the peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"lastrecv": n,  (numeric) time the last message was received in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"lastsend": n,  (numeric) time the last message was sent in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"bytessent": n,  (numeric) total bytes sent`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"bytesrecv": n,  (numeric) total bytes received`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"conntime": n,  (numeric) time the connection was made in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"pingtime": n,  (numeric) number of microseconds the last ping took`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"pingwait": n,  (numeric) number of microseconds a queued ping has been waiting for a response`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"version": n,  (numeric) the protocol version of the peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"subver": "useragent",  (string) the user agent of the peer`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"inbound": true_or_false,  (boolean) whether or not the peer is an inbound connection`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"startingheight": n,  (numeric) the latest block height the peer knew about when the connection was established`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"currentheight": n,  (numeric) the latest block height the peer is known to have relayed since connected`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"syncnode": true_or_false,  (boolean) whether or not the peer is the sync peer`<br />&nbsp;&nbsp;`}, ...`<br />`]`|
|Example Return|`[`<br />&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"addr": "178.172.xxx.xxx:8333",`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"services": "00000001",`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"lastrecv": 1388183523,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"lastsend": 1388185470,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"bytessent": 287592965,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"bytesrecv": 780340,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"conntime": 1388182973,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"pingtime": 405551,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"pingwait": 183023,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"version": 70001,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"subver": "/TBCNODE:2.1.0/",`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"inbound": false,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"startingheight": 276921,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"currentheight": 276955,`<br/>&nbsp;&nbsp;&nbsp;&nbsp;`"syncnode": true,`<br />&nbsp;&nbsp;`}`<br />`]`|
[Return to Overview](#MethodOverview)<br />

***
<a name="getrawtransaction"/>

|   |   |
|---|---|
|Method|getrawtransaction|
|Parameters|1. transaction hash (string, required) - the hash of the transaction<br />2. verbose (int, optional, default=0) - specifies the transaction is returned as a JSON object instead of hex-encoded string|
|Description|Returns information about a transaction given its hash.|
|Returns (verbose=0)|`"data" (string) hex-encoded bytes of the serialized transaction`|
|Returns (verbose=1)|`{ (json object)`<br />&nbsp;&nbsp;`"hex": "data",  (string) hex-encoded transaction`<br />&nbsp;&nbsp;`"txid": "hash",  (string) the hash of the transaction`<br />&nbsp;&nbsp;`"version": n,  (numeric) the transaction version`<br />&nbsp;&nbsp;`"locktime": n,  (numeric) the transaction lock time`<br />&nbsp;&nbsp;`"vin": [  (array of json objects) the transaction inputs as json objects`<br />&nbsp;&nbsp;<font color="orange">For coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"coinbase": "data",  (string) the hex-encoded bytes of the signature script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": n,  (numeric) the script sequence number`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;<font color="orange">For non-coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"txid": "hash", (string) the hash of the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"vout": n, (numeric) the index of the output being redeemed from the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptSig": { (json object) the signature script used to redeem the origin transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "asm", (string) disassembly of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "data",  (string) hex-encoded bytes of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": n,  (numeric) the script sequence number`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}, ...`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"vout": [  (array of json objects) the transaction outputs as json objects`<br />&nbsp;&nbsp;<font color="orange">For coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;  to do :TBC <br />&nbsp;&nbsp;&nbsp;&nbsp;` }`<br />&nbsp;&nbsp; <font color="orange">For non-coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;  `{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"value": n, (numeric) the value in BSV`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"n": n, (numeric) the index of this transaction output`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptPubKey": { (json object) the public key script used to pay coins`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "asm",  (string) disassembly of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "data", (string) hex-encoded bytes of the script`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"reqSigs": n,  (numeric) the number of required signatures`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"type": "scripttype" (string) the type of the script (e.g. 'pubkeyhash')`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [ (json array of string) the bitcoin addresses associated with this output`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"bitcoinaddress",  (string) the bitcoin address`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`...`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}, ...`<br />&nbsp;&nbsp;`]`<br />`}`|
|Example Return (verbose=0)|`"010000000104be666c7053ef26c6110597dad1c1e81b5e6be53d17a8b9d0b34772054bac60000000`<br />`008c493046022100cb42f8df44eca83dd0a727988dcde9384953e830b1f8004d57485e2ede1b9c8f`<br />`022100fbce8d84fcf2839127605818ac6c3e7a1531ebc69277c504599289fb1e9058df0141045a33`<br />`76eeb85e494330b03c1791619d53327441002832f4bd618fd9efa9e644d242d5e1145cb9c2f71965`<br />`656e276633d4ff1a6db5e7153a0a9042745178ebe0f5ffffffff0280841e00000000001976a91406`<br />`f1b6703d3f56427bfcfd372f952d50d04b64bd88ac4dd52700000000001976a9146b63f291c295ee`<br />`abd9aee6be193ab2d019e7ea7088ac00000000`<br /><font color="orange">**Newlines added for display purposes.  The actual return does not contain newlines.**</font>|
|Example Return (verbose=1)|`{`<br />&nbsp;&nbsp;`"hex": "01000000010000000000000000000000000000000000000000000000000000000000000000f...",`<br />&nbsp;&nbsp;`"txid": "90743aad855880e517270550d2a881627d84db5265142fd1e7fb7add38b08be9",`<br />&nbsp;&nbsp;`"version": 1,`<br />&nbsp;&nbsp;`"locktime": 0,`<br />&nbsp;&nbsp;`"vin": [`<br />&nbsp;&nbsp;<font color="orange">For coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{ (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"coinbase": "03708203062f503253482f04066d605108f800080100000ea2122f6f7a636f696e4065757374726174756d2f",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;<font color="orange">For non-coinbase transactions:</font><br />&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"txid": "60ac4b057247b3d0b9a8173de56b5e1be8c1d1da970511c626ef53706c66be04",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"vout": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptSig": {`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "3046022100cb42f8df44eca83dd0a727988dcde9384953e830b1f8004d57485e2ede1b9c8f0...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "493046022100cb42f8df44eca83dd0a727988dcde9384953e830b1f8004d57485e2ede1b9c8...",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"sequence": 4294967295,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`"vout": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;`{`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"value": 25.1394,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"n": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"scriptPubKey": {`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"asm": "OP_DUP OP_HASH160 ea132286328cfc819457b9dec386c4b5c84faa5c OP_EQUALVERIFY OP_CHECKSIG",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"hex": "76a914ea132286328cfc819457b9dec386c4b5c84faa5c88ac",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"reqSigs": 1,`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"type": "pubkeyhash"`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"addresses": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"1NLg3QJMsMQGM5KEUaEu5ADDmKQSLHwmyh",`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;&nbsp;&nbsp;`}`<br />&nbsp;&nbsp;`]`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="help"/>

|   |   |
|---|---|
|Method|help|
|Parameters|1. command (string, optional) - the command to get help for|
|Description|Returns a list of all commands or help for a specified command.<br />When no `command` parameter is specified, a list of avaialable commands is returned<br />When `command` is a valid method, the help text for that method is returned.|
|Returns|string|
|Example Return|getblockcount<br />Returns a numeric for the number of blocks in the longest block chain.|
[Return to Overview](#MethodOverview)<br />

***
<a name="ping"/>

|   |   |
|---|---|
|Method|ping|
|Parameters|None|
|Description|Queues a ping to be sent to each connected peer.<br />Ping times are provided by [getpeerinfo](#getpeerinfo) via the `pingtime` and `pingwait` fields.|
|Returns|Nothing|
[Return to Overview](#MethodOverview)<br />

***
<a name="getrawmempool"/>

|   |   |
|---|---|
|Method|getrawmempool|
|Parameters|1. verbose (boolean, optional, default=false)|
|Description|Returns an array of hashes for all of the transactions currently in the memory pool.<br />The `verbose` flag specifies that each transaction is returned as a JSON object.|
|Returns (verbose=false)|`[ (json array of string)`<br />&nbsp;&nbsp;`"transactionhash", (string) hash of the transaction`<br />&nbsp;&nbsp;`...`<br />`]`|
|Returns (verbose=true)|`{ (json object)`<br />&nbsp;&nbsp;`"transactionhash": { (json object)`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"size": n, (numeric) transaction size in bytes`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"vsize": n, (numeric) transaction virtual size`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"fee" : n, (numeric) transaction fee in bitcoins`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"time": n, (numeric) local time transaction entered pool in seconds since 1 Jan 1970 GMT`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"height": n, (numeric) block height when transaction entered the pool`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"startingpriority": n, (numeric) priority when transaction entered the pool`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"currentpriority": n, (numeric) current priority`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"depends": [ (json array) unconfirmed transactions used as inputs for this transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"transactionhash", (string) hash of the parent transaction`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`...`<br />&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />&nbsp;&nbsp;`}, ...`<br />`}`|
|Example Return (verbose=false)|`[`<br />&nbsp;&nbsp;`"3480058a397b6ffcc60f7e3345a61370fded1ca6bef4b58156ed17987f20d4e7",`<br />&nbsp;&nbsp;`"cbfe7c056a358c3a1dbced5a22b06d74b8650055d5195c1c2469e6b63a41514a"`<br />`]`|
|Example Return (verbose=true)|`{`<br />&nbsp;&nbsp;`"1697a19cede08694278f19584e8dcc87945f40c6b59a942dd8906f133ad3f9cc": {`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"size": 226,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"fee" : 0.0001,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"time": 1387992789,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"height": 276836,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"startingpriority": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"currentpriority": 0,`<br />&nbsp;&nbsp;&nbsp;&nbsp;`"depends": [`<br />&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;`"aa96f672fcc5a1ec6a08a94aa46d6b789799c87bd6542967da25a96b2dee0afb",`<br />&nbsp;&nbsp;&nbsp;&nbsp;`]`<br />`}`|
[Return to Overview](#MethodOverview)<br />

***
<a name="sendrawtransaction"/>

|   |   |
|---|---|
|Method|sendrawtransaction|
|Parameters|1. signedhex (string, required) serialized, hex-encoded signed transaction<br />2. allowhighfees (boolean, optional, default=false) whether or not to allow insanely high fees|
|Description|Submits the serialized, hex-encoded transaction to the local peer and relays it to the network.|
|Returns|`"hash" (string) the hash of the transaction`|
|Example Return|`"1697a19cede08694278f19584e8dcc87945f40c6b59a942dd8906f133ad3f9cc"`|
[Return to Overview](#MethodOverview)<br />

***
<a name="submitblock"/>

|   |   |
|---|---|
|Method|submitblock|
|Parameters|1. data (string, required) serialized, hex-encoded block<br />2. params (json object, optional, default=nil) this parameter is currently ignored|
|Description|Attempts to submit a new serialized, hex-encoded block to the network.|
|Returns (success)|Success: Nothing<br />Failure: `"rejected: reason"` (string)|
[Return to Overview](#MethodOverview)<br />

***
<a name="stop"/>

|   |   |
|---|---|
|Method|stop|
|Parameters|None|
|Description|Shutdown bitcoind.|
|Returns|`"Bitcoin server stopping"` (string)|
[Return to Overview](#MethodOverview)<br />

***
<a name="validateaddress"/>

|   |   |
|---|---|
|Method|validateaddress|
|Parameters|1. address (string, required) - bitcoin address|
|Description|Verify an address is valid.|
|Returns|`{ (json object)`<br />&nbsp;&nbsp;`"isvalid": true or false,  (bool) whether or not the address is valid.`<br />&nbsp;&nbsp;`"address": "bitcoinaddress", (string) the bitcoin address validated.`<br />}|
[Return to Overview](#MethodOverview)<br />

***
<a name="verifychain"/>

|   |   |
|---|---|
|Method|verifychain|
|Parameters|1. checklevel (numeric, optional, default=3) - how in-depth the verification is (0=least amount of checks, higher levels are clamped to the highest supported level)<br />2. numblocks (numeric, optional, default=288) - the number of blocks starting from the end of the chain to verify|
|Description|Verifies the block chain database.<br />The actual checks performed by the `checklevel` parameter is implementation specific.  <br />`checklevel=0` - Look up each block and ensure it can be loaded from the database :TBC <br />`checklevel=1` - Perform basic context-free sanity checks on each block :TBC|
|Returns|`true` or `false` (boolean)|
|Example Return|`true`|
[Return to Overview](#MethodOverview)<br />

