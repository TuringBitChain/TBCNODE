# Copyright (c) 2013-2016 The Bitcoin Core developers
# Copyright (c) 2018-2020 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

DIST_SUBDIRS = secp256k1 univalue

AM_LDFLAGS = $(PTHREAD_CFLAGS) $(LIBTOOL_LDFLAGS) $(HARDENED_LDFLAGS)
AM_CXXFLAGS = $(HARDENED_CXXFLAGS) $(ERROR_CXXFLAGS)
AM_CPPFLAGS = $(HARDENED_CPPFLAGS)
EXTRA_LIBRARIES =

if EMBEDDED_UNIVALUE
LIBUNIVALUE = univalue/libunivalue.la

$(LIBUNIVALUE): $(wildcard univalue/lib/*) $(wildcard univalue/include/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C $(@D) $(@F)
else
LIBUNIVALUE = $(UNIVALUE_LIBS)
endif

BITCOIN_INCLUDES=-I$(builddir) -I$(builddir)/obj $(BDB_CPPFLAGS) $(BOOST_CPPFLAGS) $(LEVELDB_CPPFLAGS) $(CRYPTO_CFLAGS) $(SSL_CFLAGS)

BITCOIN_INCLUDES += -I$(srcdir)/secp256k1/include
BITCOIN_INCLUDES += $(UNIVALUE_CFLAGS)

BITCOIN_SEEDER_INCLUDES = -I$(srcdir)/seeder
BITCOIN_SEEDER_INCLUDES += $(BITCOIN_INCLUDES)

LIBBITCOIN_SERVER=libbitcoin_server.a
LIBBITCOIN_COMMON=libbitcoin_common.a
LIBBITCOIN_CONSENSUS=libbitcoin_consensus.a
LIBBITCOIN_CLI=libbitcoin_cli.a
LIBBITCOIN_UTIL=libbitcoin_util.a
LIBBITCOIN_CRYPTO=crypto/libbitcoin_crypto.a
LIBSECP256K1=secp256k1/libsecp256k1.la

if ENABLE_ZMQ
LIBBITCOIN_ZMQ=libbitcoin_zmq.a
endif
if BUILD_BITCOIN_LIBS
LIBBITCOINCONSENSUS=libbitcoinconsensus.la
endif
if BUILD_BITCOIN_SEEDER
LIBBITCOIN_SEEDER=libbitcoin_seeder.a
endif
if ENABLE_WALLET
LIBBITCOIN_WALLET=libbitcoin_wallet.a
endif

$(LIBSECP256K1): $(wildcard secp256k1/src/*) $(wildcard secp256k1/include/*)
	$(AM_V_at)$(MAKE) $(AM_MAKEFLAGS) -C $(@D) $(@F)

# Make is not made aware of per-object dependencies to avoid limiting building parallelization
# But to build the less dependent modules first, we manually select their order here:
EXTRA_LIBRARIES += \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_COMMON) \
  $(LIBBITCOIN_CONSENSUS) \
  $(LIBBITCOIN_SERVER) \
  $(LIBBITCOIN_CLI) \
  $(LIBBITCOIN_SEEDER) \
  $(LIBBITCOIN_WALLET) \
  $(LIBBITCOIN_ZMQ)

lib_LTLIBRARIES = $(LIBBITCOINCONSENSUS)

bin_PROGRAMS =
noinst_PROGRAMS =
TESTS =
BENCHMARKS =

if BUILD_BITCOIND
  bin_PROGRAMS += bitcoind
endif

if BUILD_BITCOIN_SEEDER
  bin_PROGRAMS += bitcoin-seeder
endif

if BUILD_BITCOIN_UTILS
  bin_PROGRAMS += bitcoin-cli bitcoin-tx bitcoin-miner
endif

.PHONY: FORCE check-symbols check-security
# bitcoin core #
BITCOIN_CORE_H = \
  addrdb.h \
  addrman.h \
  async_file_reader.h \
  base58.h \
  bloom.h \
  blockencodings.h \
  blockfileinfostore.h \
  blockstreams.h \
  blockvalidation.h \
  chain.h \
  chainparams.h \
  chainparamsbase.h \
  chainparamsseeds.h \
  checkpoints.h \
  checkqueue.h \
  checkqueuepool.h \
  clientversion.h \
  coins.h \
  compat.h \
  compat/byteswap.h \
  compat/endian.h \
  compat/sanity.h \
  compressor.h \
  config.h \
  consensus/consensus.h \
  core_io.h \
  core_memusage.h \
  cuckoocache.h \
  dstencode.h \
  enum_cast.h \
  fs.h \
  httprpc.h \
  httpserver.h \
  indirectmap.h \
  init.h \
  key.h \
  keystore.h \
  dbwrapper.h \
  limitedmap.h \
  locked_ref.h \
  logging.h \
  memusage.h \
  merkleblock.h \
  mining/assembler.h \
  mining/candidates.h \
  mining/factory.h \
  mining/journal.h \
  mining/journal_builder.h \
  mining/journal_change_set.h \
  mining/journal_entry.h \
  mining/journaling_block_assembler.h \
  mining/legacy.h \
  net/association.h \
  net/net.h \
  net/netaddress.h \
  net/netbase.h \
  net/net_message.h \
  net/net_processing.h \
  net/net_types.h \
  net/node_stats.h \
  net/send_queue_bytes.h \
  net/stream.h \
  netmessagemaker.h \
  noui.h \
  orphan_txns.h \
  policy/fees.h \
  policy/policy.h \
  pow.h \
  protocol.h \
  random.h \
  reverselock.h \
  rpc/blockchain.h \
  rpc/client.h \
  rpc/jsonwriter.h \
  rpc/mining.h \
  rpc/misc.h \
  rpc/protocol.h \
  rpc/server.h \
  rpc/tojson.h \
  rpc/register.h \
  rpc/text_writer.h \
  scheduler.h \
  script_config.h \
  script/scriptcache.h \
  script/sigcache.h \
  script/sign.h \
  script/standard.h \
  script/ismine.h \
  streams.h \
  support/allocators/secure.h \
  support/allocators/zeroafterfree.h \
  support/cleanse.h \
  support/events.h \
  support/lockedpool.h \
  sync.h \
  task.h \
  taskcancellation.h \
  task_helpers.h \
  threadpool.h \
  threadpoolT.h \
  threadpriority.h \
  threadsafety.h \
  threadinterrupt.h \
  timedata.h \
  time_locked_mempool.h \
  torcontrol.h \
  txdb.h \
  txmempool.h \
  tx_mempool_info.h \
  txn_double_spend_detector.h \
  txn_handlers.h \
  txn_propagator.h \
  txn_recent_rejects.h \
  txn_sending_details.h \
  txn_util.h \
  txn_validation_config.h \
  txn_validation_data.h \
  txn_validation_result.h \
  txn_validator.h \
  ui_interface.h \
  undo.h \
  util.h \
  utilmoneystr.h \
  utiltime.h \
  validation.h \
  validationinterface.h \
  versionbits.h \
  vmtouch.h \
  wallet/coincontrol.h \
  wallet/crypter.h \
  wallet/db.h \
  wallet/finaltx.h \
  wallet/rpcdump.h \
  wallet/rpcwallet.h \
  wallet/wallet.h \
  wallet/walletdb.h \
  warnings.h \
  zmq/zmqabstractnotifier.h \
  zmq/zmqconfig.h\
  zmq/zmqnotificationinterface.h \
  zmq/zmqpublishnotifier.h


obj/build.h: FORCE
	@$(MKDIR_P) "$(builddir)/obj"
	@$(top_srcdir)/share/genbuild.sh "$(abs_top_builddir)/src/obj/build.h" \
	  "$(abs_top_srcdir)"
libbitcoin_util_a-clientversion.$(OBJEXT): obj/build.h

# server:
libbitcoin_server_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES) $(MINIUPNPC_CPPFLAGS) $(EVENT_CFLAGS) $(EVENT_PTHREADS_CFLAGS)
libbitcoin_server_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_server_a_SOURCES = \
  addrman.cpp \
  addrdb.cpp \
  bloom.cpp \
  blockencodings.cpp \
  blockfileinfostore.cpp \
  chain.cpp \
  checkpoints.cpp \
  config.cpp \
  httprpc.cpp \
  httpserver.cpp \
  init.cpp \
  dbwrapper.cpp \
  merkleblock.cpp \
  mining/assembler.cpp \
  mining/candidates.cpp \
  mining/factory.cpp \
  mining/journal.cpp \
  mining/journal_builder.cpp \
  mining/journal_change_set.cpp \
  mining/journaling_block_assembler.cpp \
  mining/legacy.cpp \
  net/association.cpp \
  net/net.cpp \
  net/net_message.cpp \
  net/net_processing.cpp \
  net/stream.cpp \
  noui.cpp \
  orphan_txns.cpp \
  policy/fees.cpp \
  policy/policy.cpp \
  pow.cpp \
  rest.cpp \
  rpc/abc.cpp \
  rpc/blockchain.cpp \
  rpc/mining.cpp \
  rpc/mining-fbb.cpp \
  rpc/misc.cpp \
  rpc/net.cpp \
  rpc/rawtransaction.cpp \
  rpc/server.cpp \
  script/scriptcache.cpp \
  script/sigcache.cpp \
  script/ismine.cpp \
  timedata.cpp \
  time_locked_mempool.cpp \
  torcontrol.cpp \
  txdb.cpp \
  txmempool.cpp \
  tx_mempool_info.cpp \
  txn_double_spend_detector.cpp \
  txn_propagator.cpp \
  txn_validation_data.cpp \
  txn_recent_rejects.cpp \
  txn_validator.cpp \
  ui_interface.cpp \
  validation.cpp \
  validationinterface.cpp \
  vmtouch.cpp \
  $(BITCOIN_CORE_H)

if ENABLE_ZMQ
libbitcoin_zmq_a_CPPFLAGS = $(BITCOIN_INCLUDES) $(ZMQ_CFLAGS)
libbitcoin_zmq_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_zmq_a_SOURCES = \
  zmq/zmqabstractnotifier.cpp \
  zmq/zmqnotificationinterface.cpp \
  zmq/zmqpublishnotifier.cpp
endif


# wallet: only linked when wallet enabled
libbitcoin_wallet_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
libbitcoin_wallet_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_wallet_a_SOURCES = \
  wallet/crypter.cpp \
  wallet/db.cpp \
  wallet/finaltx.cpp \
  wallet/rpcdump.cpp \
  wallet/rpcwallet.cpp \
  wallet/wallet.cpp \
  wallet/walletdb.cpp \
  $(BITCOIN_CORE_H)

# crypto primitives library
crypto_libbitcoin_crypto_a_CPPFLAGS = $(AM_CPPFLAGS)
crypto_libbitcoin_crypto_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
crypto_libbitcoin_crypto_a_SOURCES = \
  crypto/aes.cpp \
  crypto/aes.h \
  crypto/chacha20.h \
  crypto/chacha20.cpp \
  crypto/common.h \
  crypto/hmac_sha256.cpp \
  crypto/hmac_sha256.h \
  crypto/hmac_sha512.cpp \
  crypto/hmac_sha512.h \
  crypto/ripemd160.cpp \
  crypto/ripemd160.h \
  crypto/sha1.cpp \
  crypto/sha1.h \
  crypto/sha256.cpp \
  crypto/sha256.h \
  crypto/sha512.cpp \
  crypto/sha512.h

if USE_ASM
crypto_libbitcoin_crypto_a_SOURCES += crypto/sha256_sse4.cpp
endif

# consensus: shared between all executables that validate any consensus rules.
libbitcoin_consensus_a_CPPFLAGS = $(AM_CPPFLAGS) $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) $(OPENSSL_LIBS) $(BITCOIN_INCLUDES)
libbitcoin_consensus_a_CXXFLAGS = $(AM_CXXFLAGS) $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) $(OPENSSL_LIBS) $(PIE_FLAGS)
libbitcoin_consensus_a_SOURCES = \
  amount.h \
  arith_uint256.cpp \
  arith_uint256.h \
  big_int.cpp \
  big_int.h \
  consensus/merkle.cpp \
  consensus/merkle.h \
  consensus/params.h \
  consensus/validation.h \
  hash.cpp \
  hash.h \
  prevector.h \
  primitives/block.cpp \
  primitives/block.h \
  primitives/transaction.cpp \
  primitives/transaction.h \
  pubkey.cpp \
  pubkey.h \
  script/bitcoinconsensus.cpp \
  script/sighashtype.h \
  script/instruction.h \
  script/instruction_iterator.h \
  script/interpreter.cpp \
  script/interpreter.h \
  script/int_serialization.h \
  script/limitedstack.cpp \
  script/limitedstack.h \
  script/opcodes.cpp \
  script/opcodes.h \
  script/script_flags.h \
  script/script.cpp \
  script/script.h \
  script/script_num.cpp \
  script/script_num.h \
  script/script_error.cpp \
  script/script_error.h \
  serialize.h \
  span.h \
  tinyformat.h \
  uint256.cpp \
  uint256.h \
  utilstrencodings.cpp \
  utilstrencodings.h \
  version.h

# common: shared between bitcoind and non-server tools
libbitcoin_common_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
libbitcoin_common_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_common_a_SOURCES = \
  amount.cpp \
  base58.cpp \
  chainparams.cpp \
  config.cpp \
  coins.cpp \
  compressor.cpp \
  dstencode.cpp \
  core_read.cpp \
  core_write.cpp \
  key.cpp \
  keystore.cpp \
  net/netaddress.cpp \
  net/netbase.cpp \
  protocol.cpp \
  rpc/jsonwriter.cpp \
  scheduler.cpp \
  script/sign.cpp \
  script/standard.cpp \
  warnings.cpp \
  $(BITCOIN_CORE_H)

# util: shared between all executables.
# This library *must* be included to make sure that the glibc
# backward-compatibility objects and their sanity checks are linked.
libbitcoin_util_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
libbitcoin_util_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_util_a_SOURCES = \
  support/lockedpool.cpp \
  chainparamsbase.cpp \
  clientversion.cpp \
  compat/glibc_sanity.cpp \
  compat/glibcxx_sanity.cpp \
  compat/strnlen.cpp \
  fs.cpp \
  logging.cpp \
  random.cpp \
  rpc/protocol.cpp \
  support/cleanse.cpp \
  sync.cpp \
  threadinterrupt.cpp \
  threadpriority.cpp \
  uint256.cpp \
  uint256.h \
  util.cpp \
  utilmoneystr.cpp \
  utilstrencodings.cpp \
  utiltime.cpp \
  $(BITCOIN_CORE_H)

if GLIBC_BACK_COMPAT
libbitcoin_util_a_SOURCES += compat/glibc_compat.cpp
AM_LDFLAGS += $(COMPAT_LDFLAGS)
endif

# cli:
libbitcoin_cli_a_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
libbitcoin_cli_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_cli_a_SOURCES = \
  rpc/client.cpp \
  $(BITCOIN_CORE_H)

# seeder library
libbitcoin_seeder_a_CPPFLAGS = $(AM_CPPFLAGS) $(PIE_FLAGS) $(BITCOIN_SEEDER_INCLUDES)
libbitcoin_seeder_a_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
libbitcoin_seeder_a_SOURCES = \
  seeder/bitcoin.cpp \
  seeder/bitcoin.h \
  seeder/compat.h \
  seeder/db.cpp \
  seeder/db.h \
  seeder/dns.cpp \
  seeder/dns.h \
  seeder/strlcpy.h \
  seeder/util.h

nodist_libbitcoin_util_a_SOURCES = $(srcdir)/obj/build.h
#

# bitcoind binary #
bitcoind_SOURCES = bitcoind.cpp
bitcoind_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
bitcoind_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
bitcoind_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
bitcoind_SOURCES += bitcoind-res.rc
endif

bitcoind_LDADD = \
  $(LIBBITCOIN_SERVER) \
  $(LIBBITCOIN_COMMON) \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_WALLET) \
  $(LIBBITCOIN_ZMQ) \
  $(LIBBITCOIN_CONSENSUS) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBLEVELDB) \
  $(LIBLEVELDB_SSE42) \
  $(LIBMEMENV) \
  $(LIBSECP256K1)

bitcoind_LDADD += \
  $(BOOST_LIBS) \
  $(BDB_LIBS) \
  $(OPENSSL_LIBS) \
  $(MINIUPNPC_LIBS) \
  $(EVENT_PTHREADS_LIBS) \
  $(EVENT_LIBS) \
  $(ZMQ_LIBS) \
  $(AIO_LIBS)

# bitcoin-cli binary #
bitcoin_cli_SOURCES = bitcoin-cli.cpp
bitcoin_cli_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES) $(EVENT_CFLAGS)
bitcoin_cli_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
bitcoin_cli_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
bitcoin_cli_SOURCES += bitcoin-cli-res.rc
endif

bitcoin_cli_LDADD = \
  $(LIBBITCOIN_CLI) \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_CRYPTO)

bitcoin_cli_LDADD += $(BOOST_LIBS) $(OPENSSL_LIBS) $(EVENT_LIBS)
#

# bitcoin-seeder binary #
bitcoin_seeder_SOURCES = seeder/main.cpp
bitcoin_seeder_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_SEEDER_INCLUDES)
bitcoin_seeder_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
bitcoin_seeder_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

bitcoin_seeder_LDADD = \
  $(LIBBITCOIN_SEEDER) \
  $(LIBBITCOIN_COMMON) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_CRYPTO)

bitcoin_seeder_LDADD += $(BOOST_LIBS) $(OPENSSL_LIBS)
#

# bitcoin-tx binary #
bitcoin_tx_SOURCES = bitcoin-tx.cpp
bitcoin_tx_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES)
bitcoin_tx_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
bitcoin_tx_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)

if TARGET_WINDOWS
bitcoin_tx_SOURCES += bitcoin-tx-res.rc
endif

bitcoin_tx_LDADD = \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_COMMON) \
  $(LIBBITCOIN_UTIL) \
  $(LIBBITCOIN_CONSENSUS) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBSECP256K1)

bitcoin_tx_LDADD += $(BOOST_LIBS) $(OPENSSL_LIBS)
#

# bitcoin-miner binary #
bitcoin_miner_SOURCES = bitcoin-miner.cpp \
arith_uint256.cpp \
arith_uint256.h \
uint256.cpp \
uint256.h \
crypto/sha512.cpp \
crypto/sha512.h \
crypto/chacha20.cpp \
crypto/chacha20.h
 
bitcoin_miner_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES) $(EVENT_CFLAGS)
bitcoin_miner_CXXFLAGS = $(AM_CXXFLAGS) $(PIE_FLAGS)
bitcoin_miner_LDFLAGS = $(RELDFLAGS) $(AM_LDFLAGS) $(LIBTOOL_APP_LDFLAGS)
 
bitcoin_miner_LDADD = \
  $(LIBBITCOIN_CLI) \
  $(LIBBITCOIN_CRYPTO) \
  $(LIBUNIVALUE) \
  $(LIBBITCOIN_UTIL)
 
bitcoin_miner_LDADD += $(BOOST_LIBS) $(OPENSSL_LIBS) $(EVENT_LIBS)
#

# bitcoinconsensus library #
if BUILD_BITCOIN_LIBS
include_HEADERS = script/bitcoinconsensus.h
libbitcoinconsensus_la_SOURCES = $(crypto_libbitcoin_crypto_a_SOURCES) $(libbitcoin_consensus_a_SOURCES)

if GLIBC_BACK_COMPAT
  libbitcoinconsensus_la_SOURCES += compat/glibc_compat.cpp
endif

libbitcoinconsensus_la_LDFLAGS = $(AM_LDFLAGS) -no-undefined $(RELDFLAGS)
libbitcoinconsensus_la_LIBADD = $(LIBSECP256K1)
libbitcoinconsensus_la_CPPFLAGS = $(AM_CPPFLAGS) $(BITCOIN_INCLUDES) $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) $(OPENSSL_LIBS) -I$(builddir)/obj -I$(srcdir)/secp256k1/include -DBUILD_BITCOIN_INTERNAL
libbitcoinconsensus_la_CXXFLAGS = $(AM_CXXFLAGS) $(BITCOIN_INCLUDES) $(OPENSSL_INCLUDES) $(OPENSSL_LDFLAGS) $(OPENSSL_LIBS) $(PIE_FLAGS)

endif
#

CTAES_DIST =  crypto/ctaes/bench.c
CTAES_DIST += crypto/ctaes/ctaes.c
CTAES_DIST += crypto/ctaes/ctaes.h
CTAES_DIST += crypto/ctaes/README.md
CTAES_DIST += crypto/ctaes/test.c

CLEANFILES = $(EXTRA_LIBRARIES)

CLEANFILES += *.gcda *.gcno
CLEANFILES += compat/*.gcda compat/*.gcno
CLEANFILES += consensus/*.gcda consensus/*.gcno
CLEANFILES += crypto/*.gcda crypto/*.gcno
CLEANFILES += policy/*.gcda policy/*.gcno
CLEANFILES += primitives/*.gcda primitives/*.gcno
CLEANFILES += rpc/*.gcda rpc/*.gcno
CLEANFILES += script/*.gcda script/*.gcno
CLEANFILES += seeder/*.gcda seeder/*.gcno
CLEANFILES += support/*.gcda support/*.gcno
CLEANFILES += univalue/*.gcda univalue/*.gcno
CLEANFILES += wallet/*.gcda wallet/*.gcno
CLEANFILES += wallet/test/*.gcda wallet/test/*.gcno
CLEANFILES += zmq/*.gcda zmq/*.gcno

DISTCLEANFILES = obj/build.h

EXTRA_DIST = $(CTAES_DIST)

clean-local:
	-$(MAKE) -C secp256k1 clean
	-$(MAKE) -C univalue clean
	-rm -f leveldb/*/*.gcda leveldb/*/*.gcno leveldb/helpers/memenv/*.gcda leveldb/helpers/memenv/*.gcno
	-rm -rf test/__pycache__

.rc.o:
	@test -f $(WINDRES)
	## FIXME: How to get the appropriate modulename_CPPFLAGS in here?
	$(AM_V_GEN) $(WINDRES) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(CPPFLAGS) -DWINDRES_PREPROC -i $< -o $@

.mm.o:
	$(AM_V_CXX) $(OBJCXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) \
	  $(CPPFLAGS) $(AM_CXXFLAGS) $(AM_CXXFLAGS) $(PIE_FLAGS) $(CXXFLAGS) -c -o $@ $<

check-symbols: $(bin_PROGRAMS)
if GLIBC_BACK_COMPAT
	@echo "Checking glibc back compat..."
	$(AM_V_at) READELF=$(READELF) CPPFILT=$(CPPFILT) $(top_srcdir)/contrib/devtools/symbol-check.py $(bin_PROGRAMS)
endif

check-security: $(bin_PROGRAMS)
if HARDEN
	@echo "Checking binary security..."
	$(AM_V_at) READELF=$(READELF) OBJDUMP=$(OBJDUMP) $(top_srcdir)/contrib/devtools/security-check.py $(bin_PROGRAMS)
endif

%.pb.cc %.pb.h: %.proto
	@test -f $(PROTOC)
	$(AM_V_GEN) $(PROTOC) --cpp_out=$(@D) --proto_path=$(<D) $<

if EMBEDDED_LEVELDB
include Makefile.leveldb.include
endif

if ENABLE_TESTS
include Makefile.test.include
endif

if ENABLE_BENCH
include Makefile.bench.include
endif
