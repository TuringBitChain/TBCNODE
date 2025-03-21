// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "init.h"

#include "addrman.h"
#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "compat/sanity.h"
#include "config.h"
#include "consensus/validation.h"
#include "consensus/consensus.h"
#include "fs.h"
#include "httprpc.h"
#include "httpserver.h"
#include "key.h"
#include "mining/journal_builder.h"
#include "mining/journaling_block_assembler.h"
#include "mining/legacy.h"
#include "net/net.h"
#include "net/net_processing.h"
#include "net/netbase.h"
#include "policy/policy.h"
#include "rpc/register.h"
#include "rpc/server.h"
#include "scheduler.h"
#include "script/scriptcache.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "taskcancellation.h"
#include "timedata.h"
#include "torcontrol.h"
#include "txdb.h"
#include "txmempool.h"
#include "txn_validation_config.h"
#include "txn_validator.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "validationinterface.h"
#include "vmtouch.h"

#ifdef ENABLE_WALLET
#include "wallet/rpcdump.h"
#include "wallet/wallet.h"
#endif
#include "warnings.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <cinttypes>
#include <chrono>

#ifndef WIN32
#include <signal.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bind.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>

#if ENABLE_ZMQ
#include "zmq/zmqnotificationinterface.h"
#endif

static const bool DEFAULT_PROXYRANDOMIZE = true;
static const bool DEFAULT_REST_ENABLE = false;
static const bool DEFAULT_DISABLE_SAFEMODE = false;
static const bool DEFAULT_STOPAFTERBLOCKIMPORT = false;

std::unique_ptr<CConnman> g_connman;
std::unique_ptr<PeerLogicValidation> peerLogic;

#if ENABLE_ZMQ
static CZMQNotificationInterface *pzmqNotificationInterface = nullptr;
#endif

#ifdef WIN32
// Win32 LevelDB doesn't use filedescriptors, and the ones used for accessing
// block files don't count towards the fd_set size limit anyway.
#define MIN_CORE_FILEDESCRIPTORS 0
#else
#define MIN_CORE_FILEDESCRIPTORS 150
#endif

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE = 0,
    BF_EXPLICIT = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    BF_WHITELIST = (1U << 2),
};


//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group created by
// AppInit() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM signal handler sets
// fRequestShutdown, which triggers the DetectShutdownThread(), which interrupts
// the main thread group. DetectShutdownThread() then exits, which causes
// AppInit() to continue (it .joins the shutdown thread). Shutdown() is then
// called to clean up database connections, and stop other threads that should
// only be stopped after the main network-processing threads have exited.
//
// Note that if running -daemon the parent process returns from AppInit2 before
// adding any threads to the threadGroup, so .join_all() returns immediately and
// the parent exits from main().
//

std::shared_ptr<task::CCancellationSource> shutdownSource(task::CCancellationSource::Make());
std::atomic<bool> fDumpMempoolLater(false);

void StartShutdown() {
    shutdownSource->Cancel();
}
task::CCancellationToken GetShutdownToken()
{
    return shutdownSource->GetToken();
}

/**
 * This is a minimally invasive approach to shutdown on LevelDB read errors from
 * the chainstate, while keeping user interface out of the common library, which
 * is shared between bitcoind and non-server tools.
 */
class CCoinsViewErrorCatcher final : public CCoinsViewBacked {
public:
    CCoinsViewErrorCatcher(CCoinsView *view) : CCoinsViewBacked(view) {}
    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override {
        try {
            return CCoinsViewBacked::GetCoin(outpoint, coin);
        } catch (const std::runtime_error &e) {
            uiInterface.ThreadSafeMessageBox(
                _("Error reading from database, shutting down."), "",
                CClientUIInterface::MSG_ERROR);
            LogPrintf("Error reading from database: %s\n", e.what());
            // Starting the shutdown sequence and returning false to the caller
            // would be interpreted as 'entry not found' (as opposed to unable
            // to read data), and could lead to invalid interpretation. Just
            // exit immediately, as we can't continue anyway, and all writes
            // should be atomic.
            abort();
        }
    }
    // Writes do not need similar protection, as failure to write is handled by
    // the caller.
};

static CCoinsViewDB *pcoinsdbview = nullptr;
static CCoinsViewErrorCatcher *pcoinscatcher = nullptr;
static std::unique_ptr<ECCVerifyHandle> globalVerifyHandle;

void Interrupt(boost::thread_group &threadGroup) {
    InterruptHTTPServer();
    InterruptHTTPRPC();
    InterruptRPC();
    InterruptREST();
    InterruptTorControl();
    if (g_connman) g_connman->Interrupt();
    threadGroup.interrupt_all();
}

void Shutdown() {
    LogPrintf("%s: In progress...\n", __func__);
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown) return;

    /// Note: Shutdown() must be able to handle cases in which AppInit2() failed
    /// part of the way, for example if the data directory was found to be
    /// locked. Be sure that anything that writes files or flushes caches only
    /// does this if the respective module was initialized.
    RenameThread("bitcoin-shutoff");
    mempool.AddTransactionsUpdated(1);

    StopHTTPRPC();
    StopREST();
    StopRPC();
    StopHTTPServer();
#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        pwallet->Flush(false);
    }
#endif
    MapPort(false);
    UnregisterValidationInterface(peerLogic.get());
    peerLogic.reset();

    mining::g_miningFactory.reset();

    ShutdownScriptCheckQueues();

    if (g_connman) {
        // call Stop first as CConnman members are using g_connman global
        // variable and they must be shut down before the variable is reset to
        // nullptr (which happens before the destructor is called making Stop
        // call inside CConnman destructor too late)
        g_connman->Stop();
        g_connman.reset();
    }

    StopTorControl();
    UnregisterNodeSignals(GetNodeSignals());
    if (fDumpMempoolLater &&
        gArgs.GetArg("-persistmempool", DEFAULT_PERSIST_MEMPOOL)) {
        DumpMempool();
    }

    {
        LOCK(cs_main);
        if (pcoinsTip != nullptr) {
            FlushStateToDisk();
        }
        delete pcoinsTip;
        pcoinsTip = nullptr;
        delete pcoinscatcher;
        pcoinscatcher = nullptr;
        delete pcoinsdbview;
        pcoinsdbview = nullptr;
        delete pblocktree;
        pblocktree = nullptr;
    }
#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        pwallet->Flush(true);
    }
#endif

#if ENABLE_ZMQ
    if (pzmqNotificationInterface) {
        UnregisterValidationInterface(pzmqNotificationInterface);
        delete pzmqNotificationInterface;
        pzmqNotificationInterface = nullptr;
    }
#endif

#ifndef WIN32
    try {
        fs::remove(GetPidFile());
    } catch (const fs::filesystem_error &e) {
        LogPrintf("%s: Unable to remove pidfile: %s\n", __func__, e.what());
    }
#endif
    UnregisterAllValidationInterfaces();
#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        delete pwallet;
    }
    vpwallets.clear();
#endif
    globalVerifyHandle.reset();
    ECC_Stop();
    LogPrintf("%s: done\n", __func__);
}

/**
 * Signal handlers are very limited in what they are allowed to do, so:
 */
void HandleSIGTERM(int) {
    StartShutdown();
}

void HandleSIGHUP(int) {
    GetLogger().fReopenDebugLog = true;
}

static bool Bind(CConnman &connman, const CService &addr, unsigned int flags) {
    if (!(flags & BF_EXPLICIT) && IsLimited(addr)) return false;
    std::string strError;
    if (!connman.BindListenPort(addr, strError, (flags & BF_WHITELIST) != 0)) {
        if (flags & BF_REPORT_ERROR) return InitError(strError);
        return false;
    }
    return true;
}
void OnRPCStarted() {
    uiInterface.NotifyBlockTip.connect(&RPCNotifyBlockChange);
}

void OnRPCStopped() {
    uiInterface.NotifyBlockTip.disconnect(&RPCNotifyBlockChange);
    RPCNotifyBlockChange(false, nullptr);
    cvBlockChange.notify_all();
    LogPrint(BCLog::RPC, "RPC stopped.\n");
}

void OnRPCPreCommand(const CRPCCommand &cmd) {
    // Observe safe mode.
    std::string strWarning = GetWarnings("rpc");
    if (strWarning != "" &&
        !gArgs.GetBoolArg("-disablesafemode", DEFAULT_DISABLE_SAFEMODE) &&
        !cmd.okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE,
                           std::string("Safe mode: ") + strWarning);
}

std::string HelpMessage(HelpMessageMode mode) {
    const auto defaultBaseParams =
        CreateBaseChainParams(CBaseChainParams::MAIN);
    const auto testnetBaseParams =
        CreateBaseChainParams(CBaseChainParams::TESTNET);
    const auto defaultChainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testnetChainParams =
        CreateChainParams(CBaseChainParams::TESTNET);
    const bool showDebug = gArgs.GetBoolArg("-help-debug", false);

    // When adding new options to the categories, please keep and ensure
    // alphabetical ordering. Do not translate _(...) -help-debug options, Many
    // technical terms, and only a very small audience, so is unnecessary stress
    // to translators.
    std::string strUsage = HelpMessageGroup(_("Options:"));
    strUsage += HelpMessageOpt("-?", _("Print this help message and exit"));
    strUsage += HelpMessageOpt("-version", _("Print version and exit"));
    strUsage += HelpMessageOpt(
        "-alertnotify=<cmd>",
        _("Execute command when a relevant alert is received or we see a "
          "really long fork (%s in cmd is replaced by message)"));
    strUsage += HelpMessageOpt("-blocknotify=<cmd>",
                               _("Execute command when the best block changes "
                                 "(%s in cmd is replaced by block hash)"));
    if (showDebug)
        strUsage += HelpMessageOpt(
            "-blocksonly",
            strprintf(
                _("Whether to operate in a blocks only mode (default: %d)"),
                DEFAULT_BLOCKSONLY));
    strUsage += HelpMessageOpt(
        "-assumevalid=<hex>",
        strprintf(
            _("If this block is in the chain assume that it and its ancestors "
              "are valid and potentially skip their script verification (0 to "
              "verify all, default: %s, testnet: %s)"),
            defaultChainParams->GetConsensus().defaultAssumeValid.GetHex(),
            testnetChainParams->GetConsensus().defaultAssumeValid.GetHex()));
    strUsage += HelpMessageOpt(
        "-conf=<file>", strprintf(_("Specify configuration file (default: %s)"),
                                  BITCOIN_CONF_FILENAME));
    if (mode == HMM_BITCOIND) {
#if HAVE_DECL_DAEMON
        strUsage += HelpMessageOpt(
            "-daemon",
            _("Run in the background as a daemon and accept commands"));
#endif
    }
    strUsage += HelpMessageOpt("-datadir=<dir>", _("Specify data directory"));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-dbbatchsize",
            strprintf(
                "Maximum database write batch size in bytes (default: %u). The value may be given in bytes or with unit (B, kB, MB, GB).",
                nDefaultDbBatchSize));
    }
    strUsage += HelpMessageOpt(
        "-dbcache=<n>",
        strprintf(
            _("Set database cache size in megabytes (%d to %d, default: %d). The value may be given in megabytes or with unit (B, KiB, MiB, GiB)."),
            nMinDbCache, nMaxDbCache, nDefaultDbCache));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-feefilter", strprintf("Tell other nodes to filter invs to us by "
                                    "our mempool min fee (default: %d)",
                                    DEFAULT_FEEFILTER));
    }

    strUsage += HelpMessageOpt(
        "-genesisactivationheight",
        strprintf("Set block height at which genesis should be activated. "
                  "(default: %u).",
                  defaultChainParams->GetConsensus().genesisHeight));

    strUsage += HelpMessageOpt(
        "-loadblock=<file>",
        _("Imports blocks from external blk000??.dat file on startup"));

    strUsage += HelpMessageOpt("-maxmempool=<n>",
                               strprintf(_("Keep the transaction memory pool "
                                           "below <n> megabytes (default: %u%s,  must be at least %d). The value may be given in megabytes or with unit (B, kB, MB, GB)."),
                                         DEFAULT_MAX_MEMPOOL_SIZE, showDebug ? ", 0 to turn off mempool memory sharing with dbcache" : "", std::ceil(DEFAULT_MAX_MEMPOOL_SIZE*0.3)));
    strUsage +=
        HelpMessageOpt("-mempoolexpiry=<n>",
                       strprintf(_("Do not keep transactions in the mempool "
                                   "longer than <n> hours (default: %u)"),
                                 DEFAULT_MEMPOOL_EXPIRY));
    strUsage += HelpMessageOpt("-maxmempoolnonfinal=<n>",
                               strprintf(_("Keep the non-final transaction memory pool "
                                           "below <n> megabytes (default: %u). The value may be given in megabytes or with unit (B, KiB, MiB, GiB)."),
                                         DEFAULT_MAX_NONFINAL_MEMPOOL_SIZE));
    strUsage +=
        HelpMessageOpt("-mempoolexpirynonfinal=<n>",
                       strprintf(_("Do not keep transactions in the non-final mempool "
                                   "longer than <n> hours (default: %u)"),
                                 DEFAULT_NONFINAL_MEMPOOL_EXPIRY));

    if (showDebug) {
        strUsage += HelpMessageOpt("-checknonfinalfreq=<n>",
                       strprintf(_("Run checks on non-final transactions every <n> "
                                   "milli-seconds (default: %u)"),
                                 CTimeLockedMempool::DEFAULT_NONFINAL_CHECKS_FREQ));
    }
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-minimumchainwork=<hex>",
            strprintf(
                "Minimum work assumed to exist on a valid chain in hex "
                "(default: %s, testnet: %s)",
                defaultChainParams->GetConsensus().nMinimumChainWork.GetHex(),
                testnetChainParams->GetConsensus().nMinimumChainWork.GetHex()));
    }
    strUsage +=
        HelpMessageOpt("-persistmempool",
                       strprintf(_("Whether to save the mempool on shutdown "
                                   "and load on restart (default: %u)"),
                                 DEFAULT_PERSIST_MEMPOOL));
    strUsage += HelpMessageOpt(
        "-threadsperblock=<n>",
        strprintf(_("Set the number of script verification threads used when "
                    "validating single block (0 to %d, 0 = auto, default: %d)"),
                  MAX_SCRIPTCHECK_THREADS,
                  DEFAULT_SCRIPTCHECK_THREADS));
    strUsage +=
        HelpMessageOpt(
            "-scriptvalidatormaxbatchsize=<n>",
            strprintf(
                _("Set size of script verification batch per thread (1 to %d, "
                  "default: %d)"),
                std::numeric_limits<uint8_t>::max(),
                DEFAULT_SCRIPT_CHECK_MAX_BATCH_SIZE));

    strUsage +=
        HelpMessageOpt(
        "-maxparallelblocks=<n>",
        strprintf(_("Set the number of block that can be validated in parallel"
                    " across all nodes. If additional block arrive, validation"
                    " of an old block is terminated. (1 to 100, default: %d)"),
                  DEFAULT_SCRIPT_CHECK_POOL_SIZE));
    strUsage +=
        HelpMessageOpt(
            "-maxparallelblocksperpeer=<n>",
            strprintf(
                _("Set the number of blocks that can be validated in parallel "
                  "from a single peer. If peers sends another block, the validation"
                  " of it is delayed. (1 to maxparallelblocks, default: %d)"),
                DEFAULT_NODE_ASYNC_TASKS_LIMIT));

#ifndef WIN32
    strUsage += HelpMessageOpt(
        "-pid=<file>",
        strprintf(_("Specify pid file (default: %s)"), BITCOIN_PID_FILENAME));
#endif

    strUsage += HelpMessageOpt(
        "-preload=<n>",
            _("If n is set to 1, blockchain state will be preloaded into memory. If n is 0, no preload will happen. "
              "Other values for n are not allowed. The default value is 0."
              " This option is not supported on Windows operating systems.")
            );

    strUsage += HelpMessageOpt(
        "-prune=<n>",
        strprintf(
            _("Reduce storage requirements by enabling pruning (deleting) of "
              "old blocks. This allows the pruneblockchain RPC to be called to "
              "delete specific blocks, and enables automatic pruning of old "
              "blocks if a target size in MiB is provided. This mode is "
              "incompatible with -txindex and -rescan. "
              "Warning: Reverting this setting requires re-downloading the "
              "entire blockchain. "
              "(default: 0 = disable pruning blocks, 1 = allow manual pruning "
              "via RPC, >%u = automatically prune block files to stay under "
              "the specified target size in MiB)"),
            MIN_DISK_SPACE_FOR_BLOCK_FILES / ONE_MEBIBYTE));
    strUsage += HelpMessageOpt(
        "-reindex-chainstate",
        _("Rebuild chain state from the currently indexed blocks"));
    strUsage +=
        HelpMessageOpt("-reindex", _("Rebuild chain state and block index from "
                                     "the blk*.dat files on disk"));
    strUsage +=
        HelpMessageOpt("-rejectmempoolrequest", _("Reject every mempool request from "
                                     "non-whitelisted peers."));
#ifndef WIN32
    strUsage += HelpMessageOpt(
        "-sysperms",
        _("Create new files with system default permissions, instead of umask "
          "077 (only effective with disabled wallet functionality)"));
#endif
    strUsage += HelpMessageOpt(
        "-txindex", strprintf(_("Maintain a full transaction index, used by "
                                "the getrawtransaction rpc call (default: %d)"),
                              DEFAULT_TXINDEX));
    strUsage += HelpMessageGroup(_("Connection options:"));
    strUsage += HelpMessageOpt(
        "-addnode=<ip>",
        _("Add a node to connect to and attempt to keep the connection open"));
    if (showDebug) {
        strUsage += HelpMessageOpt("-allowunsolicitedaddr",
            _("Allow inbound peers to send us unsolicted addr messages"));
    }
    strUsage += HelpMessageOpt(
        "-banscore=<n>",
        strprintf(
            _("Threshold for disconnecting misbehaving peers (default: %u)"),
            DEFAULT_BANSCORE_THRESHOLD));
    strUsage += HelpMessageOpt(
        "-bantime=<n>", strprintf(_("Number of seconds to keep misbehaving "
                                    "peers from reconnecting (default: %u)"),
                                  DEFAULT_MISBEHAVING_BANTIME));
    strUsage += HelpMessageOpt("-bind=<addr>",
                               _("Bind to given address and always listen on "
                                 "it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt("-blockstallingmindownloadspeed=<n>",
        strprintf(_("Minimum average download speed (Kbytes/s) we will allow a stalling "
                    "peer to fall to during IBD. A value of 0 means stall detection is "
                    "disabled (default: %uKb/s)"), DEFAULT_MIN_BLOCK_STALLING_RATE));
    if (showDebug) {
        strUsage += HelpMessageOpt("-blockstallingtimeout=<n>",
            strprintf(_("Number of seconds to wait before considering a peer stalling "
                        "during IBD (default: %u)"), DEFAULT_BLOCK_STALLING_TIMEOUT));
        strUsage += HelpMessageOpt("-blockdownloadwindow=<n>",
            strprintf(_("Size of block download window before considering we may be stalling "
                        "during IBD (default: %u)"), DEFAULT_BLOCK_DOWNLOAD_WINDOW));
    }
    strUsage +=
        HelpMessageOpt("-connect=<ip>",
                       _("Connect only to the specified node(s); -noconnect or "
                         "-connect=0 alone to disable automatic connections"));
    strUsage += HelpMessageOpt("-discover",
                               _("Discover own IP addresses (default: 1 when "
                                 "listening and no -externalip or -proxy)"));
    strUsage += HelpMessageOpt(
        "-dns",
        _("Allow DNS lookups for -addnode, -seednode and -connect") + " " +
            strprintf(_("(default: %d)"), DEFAULT_NAME_LOOKUP));
    strUsage += HelpMessageOpt(
        "-dnsseed", _("Query for peer addresses via DNS lookup, if low on "
                      "addresses (default: 1 unless -connect/-noconnect)"));
    strUsage += HelpMessageOpt("-externalip=<ip>",
                               _("Specify your own public address"));
    strUsage += HelpMessageOpt(
        "-forcednsseed",
        strprintf(
            _("Always query for peer addresses via DNS lookup (default: %d)"),
            DEFAULT_FORCEDNSSEED));
    strUsage +=
        HelpMessageOpt("-listen", _("Accept connections from outside (default: "
                                    "1 if no -proxy or -connect/-noconnect)"));
    strUsage += HelpMessageOpt(
        "-listenonion",
        strprintf(_("Automatically create Tor hidden service (default: %d)"),
                  DEFAULT_LISTEN_ONION));
    strUsage += HelpMessageOpt(
        "-maxconnections=<n>",
        strprintf(_("Maintain at most <n> connections to peers (default: %u)"),
                  DEFAULT_MAX_PEER_CONNECTIONS));
    strUsage +=
        HelpMessageOpt("-maxreceivebuffer=<n>",
                       strprintf(_("Maximum per-connection receive buffer "
                                   "in kilobytes (default: %u). The value may be given in kilobytes or with unit (B, kB, MB, GB)."),
                                 DEFAULT_MAXRECEIVEBUFFER));
    strUsage += HelpMessageOpt(
        "-maxsendbuffer=<n>", strprintf(_("Maximum per-connection send buffer "
                                          "in kilobytes (default: %u). The value may be given in kilobytes or with unit (B, kB, MB, GB)."),
                                        DEFAULT_MAXSENDBUFFER));
    strUsage += HelpMessageOpt(
        "-factormaxsendqueuesbytes=<n>",
        strprintf(_("Factor that will be multiplied with excessiveBlockSize"
            " to limit the maximum bytes in all sending queues. If this"
            " size is exceeded, no response to block related P2P messages is sent."
            " (default factor: %u)"),
            DEFAULT_FACTOR_MAX_SEND_QUEUES_BYTES));
    strUsage += HelpMessageOpt(
        "-maxtimeadjustment",
        strprintf(_("Maximum allowed median peer time offset adjustment. Local "
                    "perspective of time may be influenced by peers forward or "
                    "backward by this amount. (default: %u seconds)"),
                  DEFAULT_MAX_TIME_ADJUSTMENT));
    strUsage += HelpMessageOpt(
        "-broadcastdelay=<n>",
        strprintf(
            _("Set inventory broadcast delay duration in millisecond(min: %d, max: %d)"),
            0,MAX_INV_BROADCAST_DELAY));
    strUsage +=
        HelpMessageOpt("-onion=<ip:port>",
                       strprintf(_("Use separate SOCKS5 proxy to reach peers "
                                   "via Tor hidden services (default: %s)"),
                                 "-proxy"));
    strUsage += HelpMessageOpt(
        "-onlynet=<net>",
        _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)"));
    strUsage +=
        HelpMessageOpt("-permitbaremultisig",
                       strprintf(_("Relay non-P2SH multisig (default: %d)"),
                                 DEFAULT_PERMIT_BAREMULTISIG));
    if(showDebug) {
        strUsage += HelpMessageOpt("-p2ptimeout=<n>",
            strprintf(_("Number of seconds before timing out some operations "
                "within the P2P layer. Affected operations include pings and "
                "send/receive inactivity (default: %u seconds)"), DEFAULT_P2P_TIMEOUT_INTERVAL));
    }
    strUsage += HelpMessageOpt(
        "-peerbloomfilters",
        strprintf(_("Support filtering of blocks and transaction with bloom "
                    "filters (default: %d)"),
                  DEFAULT_PEERBLOOMFILTERS));
    strUsage += HelpMessageOpt(
        "-port=<port>",
        strprintf(
            _("Listen for connections on <port> (default: %u or testnet: %u)"),
            defaultChainParams->GetDefaultPort(),
            testnetChainParams->GetDefaultPort()));
    strUsage +=
        HelpMessageOpt("-proxy=<ip:port>", _("Connect through SOCKS5 proxy"));
    strUsage += HelpMessageOpt(
        "-proxyrandomize",
        strprintf(_("Randomize credentials for every proxy connection. This "
                    "enables Tor stream isolation (default: %d)"),
                  DEFAULT_PROXYRANDOMIZE));
    strUsage += HelpMessageOpt(
        "-seednode=<ip>",
        _("Connect to a node to retrieve peer addresses, and disconnect"));
    strUsage += HelpMessageOpt(
        "-timeout=<n>", strprintf(_("Specify connection timeout in "
                                    "milliseconds (minimum: 1, default: %d)"),
                                  DEFAULT_CONNECT_TIMEOUT));
    strUsage += HelpMessageOpt("-torcontrol=<ip>:<port>",
                               strprintf(_("Tor control port to use if onion "
                                           "listening enabled (default: %s)"),
                                         DEFAULT_TOR_CONTROL));
    strUsage += HelpMessageOpt("-torpassword=<pass>",
                               _("Tor control port password (default: empty)"));
#ifdef USE_UPNP
#if USE_UPNP
    strUsage +=
        HelpMessageOpt("-upnp", _("Use UPnP to map the listening port "
                                  "(default: 1 when listening and no -proxy)"));
#else
    strUsage += HelpMessageOpt(
        "-upnp",
        strprintf(_("Use UPnP to map the listening port (default: %u)"), 0));
#endif
#endif
    strUsage +=
        HelpMessageOpt("-whitebind=<addr>",
                       _("Bind to given address and whitelist peers connecting "
                         "to it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt(
        "-whitelist=<IP address or network>",
        _("Whitelist peers connecting from the given IP address (e.g. 1.2.3.4) "
          "or CIDR notated network (e.g. 1.2.3.0/24). Can be specified "
          "multiple times.") +
            " " + _("Whitelisted peers cannot be DoS banned and their "
                    "transactions are always relayed, even if they are already "
                    "in the mempool, useful e.g. for a gateway"));
    strUsage += HelpMessageOpt(
        "-whitelistrelay",
        strprintf(_("Accept relayed transactions received from whitelisted "
                    "peers even when not relaying transactions (default: %d)"),
                  DEFAULT_WHITELISTRELAY));
    strUsage += HelpMessageOpt(
        "-whitelistforcerelay",
        strprintf(_("Force relay of transactions from whitelisted peers even "
                    "if they violate local relay policy (default: %d)"),
                  DEFAULT_WHITELISTFORCERELAY));
    strUsage += HelpMessageOpt(
        "-maxuploadtarget=<n>",
        strprintf(_("Tries to keep outbound traffic under the given target (in "
                    "MiB per 24h), 0 = no limit (default: %d). The value may be given in megabytes or with unit (KiB, MiB, GiB)."),
                  DEFAULT_MAX_UPLOAD_TARGET));

#ifdef ENABLE_WALLET
    strUsage += CWallet::GetWalletHelpString(showDebug);
#endif

#if ENABLE_ZMQ
    strUsage += HelpMessageGroup(_("ZeroMQ notification options:"));
    strUsage += HelpMessageOpt("-zmqpubhashblock=<address>",
                               _("Enable publish hash block in <address>"));
    strUsage +=
        HelpMessageOpt("-zmqpubhashtx=<address>",
                       _("Enable publish hash transaction in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawblock=<address>",
                               _("Enable publish raw block in <address>"));
    strUsage +=
        HelpMessageOpt("-zmqpubrawtx=<address>",
                       _("Enable publish raw transaction in <address>"));
#endif

    strUsage += HelpMessageGroup(_("Debugging/Testing options:"));
    strUsage += HelpMessageOpt("-uacomment=<cmt>",
                               _("Append comment to the user agent string"));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-checkblocks=<n>",
            strprintf(
                _("How many blocks to check at startup (default: %u, 0 = all)"),
                DEFAULT_CHECKBLOCKS));
        strUsage +=
            HelpMessageOpt("-checklevel=<n>",
                           strprintf(_("How thorough the block verification of "
                                       "-checkblocks is (0-4, default: %u)"),
                                     DEFAULT_CHECKLEVEL));
        strUsage += HelpMessageOpt(
            "-checkblockindex",
            strprintf("Do a full consistency check for mapBlockIndex, "
                      "setBlockIndexCandidates, chainActive and "
                      "mapBlocksUnlinked occasionally. Also sets -checkmempool "
                      "(default: %u)",
                      defaultChainParams->DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt(
            "-checkmempool=<n>",
            strprintf("Run checks every <n> transactions (default: %u)",
                      defaultChainParams->DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt(
            "-checkpoints", strprintf("Only accept block chain matching "
                                      "built-in checkpoints (default: %d)",
                                      DEFAULT_CHECKPOINTS_ENABLED));
        strUsage += HelpMessageOpt(
            "-disablesafemode", strprintf("Disable safemode, override a real "
                                          "safe mode event (default: %d)",
                                          DEFAULT_DISABLE_SAFEMODE));
        strUsage += HelpMessageOpt(
            "-testsafemode",
            strprintf("Force safe mode (default: %d)", DEFAULT_TESTSAFEMODE));
        strUsage +=
            HelpMessageOpt("-dropmessagestest=<n>",
                           "Randomly drop 1 of every <n> network messages");
        strUsage +=
            HelpMessageOpt("-fuzzmessagestest=<n>",
                           "Randomly fuzz 1 of every <n> network messages");
        strUsage += HelpMessageOpt(
            "-stopafterblockimport",
            strprintf(
                "Stop running after importing blocks from disk (default: %d)",
                DEFAULT_STOPAFTERBLOCKIMPORT));
        strUsage += HelpMessageOpt(
            "-stopatheight", strprintf("Stop running after reaching the given "
                                       "height in the main chain (default: %u)",
                                       DEFAULT_STOPATHEIGHT));
        strUsage += HelpMessageOpt(
            "-limitancestorcount=<n>",
            strprintf("Do not accept transactions if number of in-mempool "
                      "ancestors is <n> or more (default: %u)",
                      DEFAULT_ANCESTOR_LIMIT));
        strUsage +=
            HelpMessageOpt("-limitancestorsize=<n>",
                           strprintf("Do not accept transactions whose size "
                                     "with all in-mempool ancestors exceeds "
                                     "<n> kilobytes (default: %u). The value may be given in kilobytes or with unit (B, kB, MB, GB).",
                                     DEFAULT_ANCESTOR_SIZE_LIMIT));
        strUsage += HelpMessageOpt(
            "-limitdescendantcount=<n>",
            strprintf("Do not accept transactions if any ancestor would have "
                      "<n> or more in-mempool descendants (default: %u)",
                      DEFAULT_DESCENDANT_LIMIT));
        strUsage += HelpMessageOpt(
            "-limitdescendantsize=<n>",
            strprintf("Do not accept transactions if any ancestor would have "
                      "more than <n> kilobytes of in-mempool descendants "
                      "(default: %u). The value may be given in kilobytes or with unit (B, kB, MB, GB).",
                      DEFAULT_DESCENDANT_SIZE_LIMIT));
    }
    strUsage += HelpMessageOpt(
        "-debug=<category>",
        strprintf(_("Output debugging information (default: %u, supplying "
                    "<category> is optional)"),
                  0) +
            ". " + _("If <category> is not supplied or if <category> = 1, "
                     "output all debugging information.") +
            _("<category> can be:") + " " + ListLogCategories() + ".");
    strUsage += HelpMessageOpt(
        "-debugexclude=<category>",
        strprintf(_("Exclude debugging information for a category. Can be used "
                    "in conjunction with -debug=1 to output debug logs for all "
                    "categories except one or more specified categories.")));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-nodebug", "Turn off debugging messages, same as -debug=0");
    }
    strUsage += HelpMessageOpt(
        "-help-debug",
        _("Show all debugging options (usage: --help -help-debug)"));
    strUsage += HelpMessageOpt(
        "-debugp2pthreadstalls",
        _("Log P2P requests that stall request processing loop for longer than "
          "specified milliseconds (default: disabled)"));
    strUsage += HelpMessageOpt(
        "-logips",
        strprintf(_("Include IP addresses in debug output (default: %d)"),
                  DEFAULT_LOGIPS));
    strUsage += HelpMessageOpt(
        "-logtimestamps",
        strprintf(_("Prepend debug output with timestamp (default: %d)"),
                  DEFAULT_LOGTIMESTAMPS));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-logtimemicros",
            strprintf(
                "Add microsecond precision to debug timestamps (default: %d)",
                DEFAULT_LOGTIMEMICROS));
        strUsage += HelpMessageOpt(
            "-mocktime=<n>",
            "Replace actual time with <n> seconds since epoch (default: 0)");
        strUsage += HelpMessageOpt(
            "-blocksizeactivationtime=<n>",
            "Change time that specifies when new defaults for -blockmaxsize are used");
        strUsage += HelpMessageOpt(
            "-limitfreerelay=<n>",
            strprintf("Continuously rate-limit free transactions to <n> "
                      "kilobytes per minute (default: %u). The value may be given in kilobytes or with unit (B, kB, MB, GB).",
                      DEFAULT_LIMITFREERELAY));
        strUsage +=
            HelpMessageOpt("-relaypriority",
                           strprintf("Require high priority for relaying free "
                                     "or low-fee transactions (default: %d)",
                                     DEFAULT_RELAYPRIORITY));
        strUsage += HelpMessageOpt(
            "-maxsigcachesize=<n>",
            strprintf("Limit size of signature cache to <n> MiB (default: %u). The value may be given in megabytes or with unit (B, KiB, MiB, GiB).",
                      DEFAULT_MAX_SIG_CACHE_SIZE));
        strUsage += HelpMessageOpt(
            "-maxinvalidsigcachesize=<n>",
            strprintf("Limit size of invalid signature cache to <n> MiB (default: %u). The value may be given in megabytes or with unit (B, KiB, MiB, GiB).",
                      DEFAULT_INVALID_MAX_SIG_CACHE_SIZE));
        strUsage += HelpMessageOpt(
            "-maxscriptcachesize=<n>",
            strprintf("Limit size of script cache to <n> MiB (default: %u). The value may be given in megabytes or with unit (B, KiB, MiB, GiB).",
                      DEFAULT_MAX_SCRIPT_CACHE_SIZE));
        strUsage += HelpMessageOpt(
            "-maxtipage=<n>",
            strprintf("Maximum tip age in seconds to consider node in initial "
                      "block download (default: %u)",
                      DEFAULT_MAX_TIP_AGE));
    }
    strUsage += HelpMessageOpt(
        "-minrelaytxfee=<amt>",
        strprintf(
            _("Fees (in %s/kB) smaller than this are considered zero fee for "
              "relaying, mining and transaction creation (default: %s)"),
            CURRENCY_UNIT, FormatMoney(DEFAULT_MIN_RELAY_TX_FEE)));
    strUsage += HelpMessageOpt(
        "-maxtxfee=<amt>",
        strprintf(_("Maximum total fees (in %s) to use in a single wallet "
                    "transaction or raw transaction; setting this too low may "
                    "abort large transactions (default: %s)"),
                  CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MAXFEE)));
    strUsage += HelpMessageOpt(
        "-printtoconsole",
        _("Send trace/debug info to console instead of bitcoind.log file"));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-printpriority", strprintf("Log transaction priority and fee per "
                                        "kB when mining blocks (default: %d)",
                                        DEFAULT_PRINTPRIORITY));
    }
    strUsage += HelpMessageOpt("-shrinkdebugfile",
                               _("Shrink bitcoind.log file on client startup "
                                 "(default: 1 when no -debug)"));

    AppendParamsHelpMessages(strUsage, showDebug);

    strUsage += HelpMessageGroup(_("Node relay options:"));
    strUsage +=
        HelpMessageOpt("-excessiveblocksize=<n>",
                       strprintf(_("Set the maximum block size in bytes we will accept "
                                   "from any source. This is the effective block size "
                                   "hard limit and it is a required parameter (0 = unlimited). "
                                   "The value may be given in bytes or with unit (B, kB, MB, GB).")));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-acceptnonstdtxn",
            strprintf(
                "Relay and mine \"non-standard\" transactions (%sdefault: %u)",
                "testnet/regtest only; ",
                defaultChainParams->RequireStandard()));
        strUsage += HelpMessageOpt(
            "-acceptnonstdoutputs",
            strprintf(
                "Relay and mine transactions that create or consume non standard"
                " outputs after Genesis is activated. (default: %u)",
                GlobalConfig::GetConfig().GetAcceptNonStandardOutput(true)));
        strUsage += HelpMessageOpt(
            "-dustrelayfee=<amt>",
            strprintf("Fee rate (in %s/kB) used to defined dust, the value of "
                      "an output such that it will cost about 1/3 of its value "
                      "in fees at this fee rate to spend it. (default: %s)",
                      CURRENCY_UNIT, FormatMoney(DUST_RELAY_TX_FEE)));
    }
    strUsage += HelpMessageOpt(
        "-datacarrier",
        strprintf(_("Relay and mine data carrier transactions (default: %d)"),
                  DEFAULT_ACCEPT_DATACARRIER));
    strUsage += HelpMessageOpt(
        "-datacarriersize",
        strprintf(_("Maximum size of data in data carrier transactions we "
                    "relay and mine (default: %u). The value may be given in bytes or with unit (B, kB, MB, GB)."),
                  DEFAULT_DATA_CARRIER_SIZE));
    strUsage += HelpMessageOpt(
        "-maxstackmemoryusageconsensus",
        strprintf(_("Set maximum stack memory usage in bytes used for script verification "
                    "we're willing to accept from any source (0 = unlimited) "
                    "after Genesis is activated (consensus level). This is a required parameter. "
                    "The value may be given in bytes or with unit (B, kB, MB, GB).")));
    strUsage += HelpMessageOpt(
        "-maxstackmemoryusagepolicy",
        strprintf(_("Set maximum stack memory usage used for script verification "
                    "we're willing to relay/mine in a single transaction "
                    "(default: %u MB, 0 = unlimited) "
                    "after Genesis is activated (policy level). The value may be given in bytes or with unit (B, kB, MB, GB)."
                    "Must be less or equal to -maxstackmemoryusageconsensus."),
                  DEFAULT_STACK_MEMORY_USAGE_POLICY_AFTER_GENESIS/ONE_MEGABYTE));
    strUsage +=
        HelpMessageOpt("-maxopsperscriptpolicy=<n>",
            strprintf(_("Set maximum number of non-push operations "
                        "we're willing to relay/mine per script (default: unlimited, 0 = unlimited), after Genesis is activated")));
    strUsage += HelpMessageOpt(
        "-maxtxsigopscountspolicy=<n>",
        strprintf("Set maximum allowed number of signature operations we're willing to relay/mine in a single transaction (default: unlimited, 0 = unlimited) after Genesis is activated."));


    strUsage += HelpMessageOpt(
        "-maxstdtxvalidationduration=<n>",
        strprintf(
            _("Set the single standard transaction validation duration threshold in"
              " milliseconds after which the standard transaction validation will"
              " terminate with error and the transaction is not accepted to"
              " mempool (min 5ms, default: %dms)"),
            DEFAULT_MAX_STD_TXN_VALIDATION_DURATION.count()));

    strUsage += HelpMessageOpt(
        "-maxnonstdtxvalidationduration=<n>",
        strprintf(
            _("Set the single non-standard transaction validation duration threshold in"
              " milliseconds after which the non-standard transaction validation will"
              " terminate with error and the transaction is not accepted to"
              " mempool (min 10ms, default: %dms)"),
            DEFAULT_MAX_NON_STD_TXN_VALIDATION_DURATION.count()));

    strUsage +=
        HelpMessageOpt("-maxtxsizepolicy=<n>",
            strprintf(_("Set maximum transaction size in bytes we relay and mine (default: %u MB, min: %u B, 0 = unlimited) after Genesis is activated. "
                        "The value may be given in bytes or with unit (B, kB, MB, GB)."),
                DEFAULT_MAX_TX_SIZE_POLICY_AFTER_GENESIS/ONE_MEGABYTE, MAX_TX_SIZE_POLICY_BEFORE_GENESIS));

    strUsage +=
            HelpMessageOpt("-minconsolidationfactor=<n>",
                           strprintf(_("Set minimum ratio between sum of utxo scriptPubKey sizes spent in a consolidation transaction, to the corresponding sum of output scriptPubKey sizes. "
					   "The ratio between number of consolidation transaction inputs to the number of outputs also needs to be greater or equal to the minimum consolidation factor (default: %u). "
				       "A value of 0 disables free consolidation transactions"),
                                     DEFAULT_MIN_CONSOLIDATION_FACTOR));
    strUsage +=
            HelpMessageOpt("-maxconsolidationinputscriptsize=<n>",
                           strprintf(_("This number is the maximum length for a scriptSig input in a consolidation txn (default: %u). "),
                                     DEFAULT_MAX_CONSOLIDATION_INPUT_SCRIPT_SIZE));

    strUsage +=
            HelpMessageOpt("-minconsolidationinputmaturity=<n>",
                           strprintf(_("Minimum number of confirmations of inputs spent by consolidation transactions (default: %u). "),
                                     DEFAULT_MIN_CONSOLIDATION_INPUT_MATURITY));

    strUsage +=
            HelpMessageOpt("-acceptnonstdconsolidationinput=<n>",
                           strprintf(_("Accept consolidation transactions spending non standard inputs (default: %u). "),
                                     DEFAULT_ACCEPT_NON_STD_CONSOLIDATION_INPUT));

    strUsage += HelpMessageOpt(
        "-maxscriptsizepolicy",
        strprintf("Set maximum script size in bytes we're willing to relay/mine per script after Genesis is activated. "
            "(default: %u, 0 = unlimited). The value may be given in bytes or with unit (B, kB, MB, GB).",
            DEFAULT_MAX_SCRIPT_SIZE_POLICY_AFTER_GENESIS));

    strUsage += HelpMessageOpt(
        "-maxscriptnumlengthpolicy=<n>",
        strprintf("Set maximum allowed number length we're willing to relay/mine in scripts (default: %d, 0 = unlimited) after Genesis is activated. "
            "The value may be given in bytes or with unit (B, kB, MB, GB).",
            DEFAULT_SCRIPT_NUM_LENGTH_POLICY_AFTER_GENESIS));

    strUsage += HelpMessageGroup(_("Block creation options:"));
    strUsage += HelpMessageOpt(
        "-blockmaxsize=<n>",
        strprintf(_("Set maximum block size in bytes we will mine. "
                    "Size of the mined block will never exceed the maximum block size we will accept (-excessiveblocksize). "
                    "The value may be given in bytes or with unit (B, kB, MB, GB). "
                    "If not specified, the following defaults are used: "
                    "Mainnet: %d MB before %s and %d MB after, "
                    "Testnet: %d MB before %s and %d MB after."),
                    defaultChainParams->GetDefaultBlockSizeParams().maxGeneratedBlockSizeBefore / ONE_MEGABYTE,
                    DateTimeStrFormat("%Y-%m-%d %H:%M:%S", defaultChainParams->GetDefaultBlockSizeParams().blockSizeActivationTime),
                    defaultChainParams->GetDefaultBlockSizeParams().maxGeneratedBlockSizeAfter/ONE_MEGABYTE,
                    testnetChainParams->GetDefaultBlockSizeParams().maxGeneratedBlockSizeBefore / ONE_MEGABYTE,
                    DateTimeStrFormat("%Y-%m-%d %H:%M:%S", testnetChainParams->GetDefaultBlockSizeParams().blockSizeActivationTime),
                    testnetChainParams->GetDefaultBlockSizeParams().maxGeneratedBlockSizeAfter / ONE_MEGABYTE
                    ));
    strUsage += HelpMessageOpt(
        "-blockprioritypercentage=<n>",
        strprintf(_("Set maximum percentage of a block reserved to "
                    "high-priority/low-fee transactions (default: %d). NOTE: This is supported only by the legacy block assembler which"
                    " is not default block assembler any more and will be removed in the upcoming release."),
                  DEFAULT_BLOCK_PRIORITY_PERCENTAGE));
    strUsage += HelpMessageOpt(
        "-blockmintxfee=<amt>",
        strprintf(_("Set lowest fee rate (in %s/kB) for transactions to be "
                    "included in block creation. (default: %s)"),
                  CURRENCY_UNIT, FormatMoney(DEFAULT_BLOCK_MIN_TX_FEE)));
    strUsage += HelpMessageOpt(
        "-invalidateblock=<hash>",
        strprintf(_("Permanently marks an existing block as invalid as if it violated "
                    "a consensus rule (same as InvalidateBlock RPC function). "
                    "If specified block header was not received yet, the header will be "
                    "ignored when it is received from a peer. "
                    "This option can be specified multiple times.")));

    strUsage += HelpMessageOpt(
        "-banclientua=<ua>",
        strprintf(_("Ban clients whose User Agent contains specified string (case insensitive). "
                    "This option can be specified multiple times.")));

    if (showDebug) {
        strUsage +=
            HelpMessageOpt("-blockversion=<n>",
                           "Override block version to test forking scenarios");
        strUsage +=
            HelpMessageOpt("-blockcandidatevaliditytest",
                           strprintf(_("Perform validity test on block candidates. Defaults: "
                           "Mainnet: %d, Testnet: %d"), defaultChainParams->TestBlockCandidateValidity(), testnetChainParams->TestBlockCandidateValidity()));
    }

    /** Block assembler */
    strUsage += HelpMessageOpt(
        "-blockassembler=<type>",
        strprintf(_("Set the type of block assembler to use for mining. Supported options are "
                    "LEGACY or JOURNALING. (default: %s)"),
                  enum_cast<std::string>(mining::DEFAULT_BLOCK_ASSEMBLER_TYPE).c_str()));
    strUsage += HelpMessageOpt( 
        "-jbamaxtxnbatch=<max batch size>",
        strprintf(_("Set the maximum number of transactions processed in a batch by the journaling block assembler "
                "(default: %d)"), mining::JournalingBlockAssembler::DEFAULT_MAX_SLOT_TRANSACTIONS)
    );
    if (showDebug) {
        strUsage += HelpMessageOpt( 
            "-jbafillafternewblock",
            strprintf(_("After a new block has been found it can take a short while for the journaling block assembler "
                        "to catch up and return a new candidate containing every transaction in the mempool. "
                        "If this flag is 1, calling getminingcandidate will wait until the JBA has caught up "
                        "and always return a candidate with every available transaction. If it is 0, calls to "
                        "getminingcandidate will always return straight away but may occasionally only contain a "
                        "subset of the available transactions from the mempool (default: %d)"),
                mining::JournalingBlockAssembler::DEFAULT_NEW_BLOCK_FILL)
        );
    }

    strUsage += HelpMessageGroup(_("RPC server options:"));
    strUsage += HelpMessageOpt("-server",
                               _("Accept command line and JSON-RPC commands"));
    strUsage += HelpMessageOpt(
        "-rest", strprintf(_("Accept public REST requests (default: %d)"),
                           DEFAULT_REST_ENABLE));
    strUsage += HelpMessageOpt(
        "-rpcbind=<addr>",
        _("Bind to given address to listen for JSON-RPC connections. Use "
          "[host]:port notation for IPv6. This option can be specified "
          "multiple times (default: bind to all interfaces)"));
    strUsage +=
        HelpMessageOpt("-rpccookiefile=<loc>",
                       _("Location of the auth cookie (default: data dir)"));
    strUsage += HelpMessageOpt("-rpcuser=<user>",
                               _("Username for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcpassword=<pw>",
                               _("Password for JSON-RPC connections"));
    strUsage += HelpMessageOpt(
        "-rpcauth=<userpw>",
        _("Username and hashed password for JSON-RPC connections. The field "
          "<userpw> comes in the format: <USERNAME>:<SALT>$<HASH>. A canonical "
          "python script is included in share/rpcuser. The client then "
          "connects normally using the "
          "rpcuser=<USERNAME>/rpcpassword=<PASSWORD> pair of arguments. This "
          "option can be specified multiple times"));
    strUsage += HelpMessageOpt(
        "-rpcport=<port>",
        strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or "
                    "testnet: %u)"),
                  defaultBaseParams->RPCPort(), testnetBaseParams->RPCPort()));
    strUsage += HelpMessageOpt(
        "-rpcallowip=<ip>",
        _("Allow JSON-RPC connections from specified source. Valid for <ip> "
          "are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. "
          "1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This "
          "option can be specified multiple times"));
    strUsage += HelpMessageOpt(
        "-magicbytes=<hexcode>",
        _("Allow users to split the test net by changing the magicbytes. "
          "This option only work on a network different than mainnet. "
          "default : 0f0f0f0f"));
    strUsage += HelpMessageOpt(
        "-rpcthreads=<n>",
        strprintf(
            _("Set the number of threads to service RPC calls (default: %d)"),
            DEFAULT_HTTP_THREADS));
    strUsage += HelpMessageOpt(
        "-rpccorsdomain=value",
        "Domain from which to accept cross origin requests (browser enforced)");
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-rpcworkqueue=<n>", strprintf("Set the depth of the work queue to "
                                           "service RPC calls (default: %d)",
                                           DEFAULT_HTTP_WORKQUEUE));
        strUsage += HelpMessageOpt(
            "-rpcservertimeout=<n>",
            strprintf("Timeout during HTTP requests (default: %d)",
                      DEFAULT_HTTP_SERVER_TIMEOUT));
    }
     strUsage += HelpMessageOpt(
        "-invalidcsinterval=<n>",
         strprintf("Set the time limit on the reception of invalid message checksums from a single node in milliseconds (default: %dms)",
            DEFAULT_MIN_TIME_INTERVAL_CHECKSUM_MS)) ;

         strUsage += HelpMessageOpt(
        "-invalidcsfreq=<n>",
         strprintf("Set the limit on the number of invalid checksums received over a given time period from a single node  (default: %d)",
            DEFAULT_INVALID_CHECKSUM_FREQUENCY)) ;

    /** COrphanTxns */
    strUsage += HelpMessageGroup(_("Orphan txns config :"));
    strUsage += HelpMessageOpt(
        "-blockreconstructionextratxn=<n>",
        strprintf(_("Extra transactions to keep in memory for compact block "
                    "reconstructions (default: %u)"),
            COrphanTxns::DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN));
    strUsage += HelpMessageOpt(
        "-maxorphantxsize=<n>",
        strprintf(_("Keep at most <n> MB of unconnectable "
                    "transactions in memory (default: %u MB). The value may be given in megabytes or with unit (B, kB, MB, GB)."),
          COrphanTxns::DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE/ONE_MEGABYTE));
    strUsage += HelpMessageOpt(
        "-maxcollectedoutpoints=<n>",
        strprintf(_("Keep at most <n> collected "
                    "outpoints in memory (default: %u)"),
            COrphanTxns::DEFAULT_MAX_COLLECTED_OUTPOINTS));

    /** TxnValidator */
    strUsage += HelpMessageGroup(_("TxnValidator options:"));
    strUsage += HelpMessageOpt(
        "-numstdtxvalidationthreads=<n>",
        strprintf(_("Set the number of 'High' priority threads used to validate standard txns (dynamically calculated default: %d)"),
            GetNumHighPriorityValidationThrs())) ;
    strUsage += HelpMessageOpt(
        "-numnonstdtxvalidationthreads=<n>",
        strprintf(_("Set the number of 'Low' priority threads used to validate non-standard txns (dynamically calculated default: %d)"),
            GetNumLowPriorityValidationThrs())) ;
    strUsage += HelpMessageOpt(
        "-maxstdtxnsperthreadratio=<n>",
        strprintf(_("Set the max ratio for a number of standard txns per 'High' priority thread (default: %d)"),
            DEFAULT_MAX_STD_TXNS_PER_THREAD_RATIO)) ;
    strUsage += HelpMessageOpt(
        "-maxnonstdtxnsperthreadratio=<n>",
        strprintf(_("Set the max ratio for a number of non-standard txns per 'Low' priority thread (default: %d)"),
            DEFAULT_MAX_NON_STD_TXNS_PER_THREAD_RATIO)) ;
    strUsage += HelpMessageOpt(
        "-txnvalidationasynchrunfreq=<n>",
        strprintf("Set run frequency in asynchronous mode (default: %dms)",
            CTxnValidator::DEFAULT_ASYNCH_RUN_FREQUENCY_MILLIS)) ;
    strUsage += HelpMessageOpt(
        "-maxtxnvalidatorasynctasksrunduration=<n>",
        strprintf("Set the maximum validation duration for async tasks in a single run (default: %dms)",
            CTxnValidator::DEFAULT_MAX_ASYNC_TASKS_RUN_DURATION.count())) ;
    strUsage += HelpMessageOpt(
        "-maxcoinsviewcachesize=<n>",
        _("Set the maximum cumulative size of accepted transaction inputs inside coins cache (default: unlimited -> 0). "
            "The value may be given in bytes or with unit (B, kB, MB, GB)."));
    strUsage += HelpMessageOpt(
        "-txnvalidationqueuesmaxmemory=<n>",
        strprintf("Set the maximum memory usage for the transaction queues in MB (default: %d). The value may be given in megabytes or with unit (B, kB, MB, GB).",
            CTxnValidator::DEFAULT_MAX_MEMORY_TRANSACTION_QUEUES)) ;

    strUsage += HelpMessageOpt(
        "-maxpubkeyspermultisigpolicy=<n>",
        strprintf("Set maximum allowed number of public keys we're willing to relay/mine in a single CHECK_MULTISIG(VERIFY) operation (default: unlimited, 0 = unlimited), after Genesis is activated"));

    strUsage += HelpMessageOpt(
        "-maxgenesisgracefulperiod=<n>",
        strprintf(_("Set maximum allowed number of blocks for Genesis graceful period (default: %d) where nodes will not be banned "
                    "for violating Genesis rules in case the calling node is not yet on Genesis height and vice versa. "
                    "Seting 0 will disable Genesis graceful period. Genesis graceful period range :"
                    "(GENESIS_ACTIVATION_HEIGHT - n |...| GENESIS_ACTIVATION_HEIGHT |...| GENESIS_ACTIVATION_HEIGHT + n)"),
            DEFAULT_GENESIS_GRACEFULL_ACTIVATION_PERIOD));

    return strUsage;
}

std::string LicenseInfo() {
    const std::string URL_SOURCE_CODE =
        "<https://github.com/bitcoin-sv/bitcoin-sv>";
    const std::string URL_WEBSITE = "<https://bitcoinsv.io>";

    return CopyrightHolders(
               strprintf(_("Copyright (C) %i-%i"), 2009, COPYRIGHT_YEAR) +
               " ") +
           "\n" + "\n" +
           strprintf(_("Please contribute if you find %s useful. "
                       "Visit %s for further information about the software."),
                     PACKAGE_NAME, URL_WEBSITE) +
           "\n" + strprintf(_("The source code is available from %s."),
                            URL_SOURCE_CODE) +
           "\n" + "\n" + _("This is experimental software.") + "\n" +
           strprintf(_("Distributed under the Open TBC software license, see the "
                       "accompanying file %s"),
                     "LICENSE") +
           "\n" + "\n" +
           strprintf(_("This product includes software developed by the "
                       "OpenSSL Project for use in the OpenSSL Toolkit %s and "
                       "cryptographic software written by Eric Young and UPnP "
                       "software written by Thomas Bernard."),
                     "<https://www.openssl.org>") +
           "\n";
}

static void BlockNotifyCallback(bool initialSync,
                                const CBlockIndex *pBlockIndex) {
    if (initialSync || !pBlockIndex) return;

    std::string strCmd = gArgs.GetArg("-blocknotify", "");

    boost::replace_all(strCmd, "%s", pBlockIndex->GetBlockHash().GetHex());
    boost::thread t(runCommand, strCmd); // thread runs free
}

static bool fHaveGenesis = false;
static boost::mutex cs_GenesisWait;
static CConditionVariable condvar_GenesisWait;

static void BlockNotifyGenesisWait(bool, const CBlockIndex *pBlockIndex) {
    if (pBlockIndex != nullptr) {
        {
            boost::unique_lock<boost::mutex> lock_GenesisWait(cs_GenesisWait);
            fHaveGenesis = true;
        }
        condvar_GenesisWait.notify_all();
    }
}

struct CImportingNow {
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};

// If we're using -prune with -reindex, then delete block files that will be
// ignored by the reindex.  Since reindexing works by starting at block file 0
// and looping until a blockfile is missing, do the same here to delete any
// later block files after a gap. Also delete all rev files since they'll be
// rewritten by the reindex anyway. This ensures that vinfoBlockFile is in sync
// with what's actually on disk by the time we start downloading, so that
// pruning works correctly.
void CleanupBlockRevFiles() {
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for "
              "-reindex with -prune\n");
    fs::path blocksdir = GetDataDir() / "blocks";
    for (fs::directory_iterator it(blocksdir); it != fs::directory_iterator();
         it++) {
        if (is_regular_file(*it) &&
            it->path().filename().string().length() == 12 &&
            it->path().filename().string().substr(8, 4) == ".dat") {
            if (it->path().filename().string().substr(0, 3) == "blk")
                mapBlockFiles[it->path().filename().string().substr(3, 5)] =
                    it->path();
            else if (it->path().filename().string().substr(0, 3) == "rev")
                remove(it->path());
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by keeping
    // a separate counter. Once we hit a gap (or if 0 doesn't exist) start
    // removing block files.
    int nContigCounter = 0;
    for (const std::pair<std::string, fs::path> &item : mapBlockFiles) {
        if (atoi(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

/* shutdownToken must be passed by value to prevent access violation because
 * "import_files" thread can have longer life span than shutdownToken presented with a reference.
 */
void ThreadImport(const Config &config, std::vector<fs::path> vImportFiles, const task::CCancellationToken shutdownToken) {
    RenameThread("bitcoin-loadblk");

    {
        CImportingNow imp;

        // -reindex
        if (fReindex) {
            ReindexAllBlockFiles(config, pblocktree, fReindex);
        }

        // hardcoded $DATADIR/bootstrap.dat
        fs::path pathBootstrap = GetDataDir() / "bootstrap.dat";
        if (fs::exists(pathBootstrap)) {
            FILE *file = fsbridge::fopen(pathBootstrap, "rb");
            if (file) {
                fs::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
                LogPrintf("Importing bootstrap.dat...\n");
                LoadExternalBlockFile(config, file);
                RenameOver(pathBootstrap, pathBootstrapOld);
            } else {
                LogPrintf("Warning: Could not open bootstrap file %s\n",
                          pathBootstrap.string());
            }
        }

        // -loadblock=
        for (const fs::path &path : vImportFiles) {
            FILE *file = fsbridge::fopen(path, "rb");
            if (file) {
                LogPrintf("Importing blocks file %s...\n", path.string());
                LoadExternalBlockFile(config, file);
            } else {
                LogPrintf("Warning: Could not open blocks file %s\n",
                          path.string());
            }
        }

        // scan for better chains in the block chain database, that are not yet
        // connected in the active best chain

        // dummyState is used to report errors, not block related invalidity
        // (see description of ActivateBestChain)
        CValidationState dummyState;
        mining::CJournalChangeSetPtr changeSet { mempool.getJournalBuilder().getNewChangeSet(mining::JournalUpdateReason::INIT) };
        auto source = task::CCancellationSource::Make();
        if (!ActivateBestChain(task::CCancellationToken::JoinToken(source->GetToken(), shutdownToken), config, dummyState, changeSet)) {
            LogPrintf("Failed to connect best block");
            StartShutdown();
        }

        if (gArgs.GetBoolArg("-stopafterblockimport",
                             DEFAULT_STOPAFTERBLOCKIMPORT)) {
            LogPrintf("Stopping after block import\n");
            StartShutdown();
        }
    } // End scope of CImportingNow
    if (gArgs.GetArg("-persistmempool", DEFAULT_PERSIST_MEMPOOL)) {
        LoadMempool(config, shutdownToken);
        fDumpMempoolLater = !shutdownToken.IsCanceled();
    }
}

/** Sanity checks
 *  Ensure that Bitcoin is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void) {
    if (!ECC_InitSanityCheck()) {
        InitError(
            "Elliptic curve cryptography sanity check failure. Aborting.");
        return false;
    }

    if (!glibc_sanity_test() || !glibcxx_sanity_test()) {
        return false;
    }

    if (!Random_SanityCheck()) {
        InitError("OS cryptographic RNG sanity check failure. Aborting.");
        return false;
    }

    return true;
}

static bool AppInitServers(Config &config, boost::thread_group &threadGroup) {
    RPCServer::OnStarted(&OnRPCStarted);
    RPCServer::OnStopped(&OnRPCStopped);
    RPCServer::OnPreCommand(&OnRPCPreCommand);
    if (!InitHTTPServer(config)) return false;
    if (!StartRPC()) return false;
    if (!StartHTTPRPC()) return false;
    if (gArgs.GetBoolArg("-rest", DEFAULT_REST_ENABLE) && !StartREST())
        return false;
    if (!StartHTTPServer()) return false;
    return true;
}

// Parameter interaction based on rules
void InitParameterInteraction() {
    // when specifying an explicit binding address, you want to listen on it
    // even when -connect or -proxy is specified.
    if (gArgs.IsArgSet("-bind")) {
        if (gArgs.SoftSetBoolArg("-listen", true))
            LogPrintf(
                "%s: parameter interaction: -bind set -> setting -listen=1\n",
                __func__);
    }
    if (gArgs.IsArgSet("-whitebind")) {
        if (gArgs.SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -whitebind set -> setting "
                      "-listen=1\n",
                      __func__);
    }

    if (gArgs.IsArgSet("-connect")) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen
        // by default.
        if (gArgs.SoftSetBoolArg("-dnsseed", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting "
                      "-dnsseed=0\n",
                      __func__);
        if (gArgs.SoftSetBoolArg("-listen", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting "
                      "-listen=0\n",
                      __func__);
    }

    if (gArgs.IsArgSet("-proxy")) {
        // to protect privacy, do not listen by default if a default proxy
        // server is specified.
        if (gArgs.SoftSetBoolArg("-listen", false))
            LogPrintf(
                "%s: parameter interaction: -proxy set -> setting -listen=0\n",
                __func__);
        // to protect privacy, do not use UPNP when a proxy is set. The user may
        // still specify -listen=1 to listen locally, so don't rely on this
        // happening through -listen below.
        if (gArgs.SoftSetBoolArg("-upnp", false))
            LogPrintf(
                "%s: parameter interaction: -proxy set -> setting -upnp=0\n",
                __func__);
        // to protect privacy, do not discover addresses by default
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting "
                      "-discover=0\n",
                      __func__);
    }

    if (!gArgs.GetBoolArg("-listen", DEFAULT_LISTEN)) {
        // do not map ports or try to retrieve public IP when not listening
        // (pointless)
        if (gArgs.SoftSetBoolArg("-upnp", false))
            LogPrintf(
                "%s: parameter interaction: -listen=0 -> setting -upnp=0\n",
                __func__);
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf(
                "%s: parameter interaction: -listen=0 -> setting -discover=0\n",
                __func__);
        if (gArgs.SoftSetBoolArg("-listenonion", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting "
                      "-listenonion=0\n",
                      __func__);
    }

    if (gArgs.IsArgSet("-externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -externalip set -> setting "
                      "-discover=0\n",
                      __func__);
    }

    // disable whitelistrelay in blocksonly mode
    if (gArgs.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY)) {
        if (gArgs.SoftSetBoolArg("-whitelistrelay", false))
            LogPrintf("%s: parameter interaction: -blocksonly=1 -> setting "
                      "-whitelistrelay=0\n",
                      __func__);
    }

    // Forcing relay from whitelisted hosts implies we will accept relays from
    // them in the first place.
    if (gArgs.GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
        if (gArgs.SoftSetBoolArg("-whitelistrelay", true))
            LogPrintf("%s: parameter interaction: -whitelistforcerelay=1 -> "
                      "setting -whitelistrelay=1\n",
                      __func__);
    }
}

static std::string ResolveErrMsg(const char *const optname,
                                 const std::string &strBind) {
    return strprintf(_("Cannot resolve -%s address: '%s'"), optname, strBind);
}

void InitLogging() {
    BCLog::Logger &logger = GetLogger();
    logger.fPrintToConsole = gArgs.GetBoolArg("-printtoconsole", false);
    logger.fLogTimestamps =
        gArgs.GetBoolArg("-logtimestamps", DEFAULT_LOGTIMESTAMPS);
    logger.fLogTimeMicros =
        gArgs.GetBoolArg("-logtimemicros", DEFAULT_LOGTIMEMICROS);

    fLogIPs = gArgs.GetBoolArg("-logips", DEFAULT_LOGIPS);

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("%s version %s\n", CLIENT_NAME, FormatFullVersion());
}

namespace { // Variables internal to initialization process only

ServiceFlags nRelevantServices = NODE_NETWORK;
int nMaxConnections;
int nUserMaxConnections;
int nFD;
ServiceFlags nLocalServices = NODE_NETWORK;
} // namespace

[[noreturn]] static void new_handler_terminate() {
    // Rather than throwing std::bad-alloc if allocation fails, terminate
    // immediately to (try to) avoid chain corruption. Since LogPrintf may
    // itself allocate memory, set the handler directly to terminate first.
    std::set_new_handler(std::terminate);
    LogPrintf("Error: Out of memory. Terminating.\n");

    // The log was successful, terminate now.
    std::terminate();
};

bool AppInitBasicSetup() {
// Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, nullptr,
                                             OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
// Enable Data Execution Prevention (DEP)
// Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
// A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
// We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >=
// 0x0601 (Windows 7), which is not correct. Can be removed, when GCCs winbase.h
// is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL(WINAPI * PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(
        GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != nullptr) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif

    if (!SetupNetworking()) return InitError("Initializing networking failed");

#ifndef WIN32
    if (!gArgs.GetBoolArg("-sysperms", false)) {
        umask(077);
    }

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // Reopen bitcoind.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, nullptr);

    // Ignore SIGPIPE, otherwise it will bring the daemon down if the client
    // closes unexpectedly
    signal(SIGPIPE, SIG_IGN);
#endif

    std::set_new_handler(new_handler_terminate);

    return true;
}

bool AppInitParameterInteraction(Config &config) {
    const CChainParams &chainparams = config.GetChainParams();
    // Step 2: parameter interactions

    // also see: InitParameterInteraction()

    // if using block pruning, then disallow txindex
    if (gArgs.GetArg("-prune", 0)) {
        if (gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX))
            return InitError(_("Prune mode is incompatible with -txindex."));
    }

    // if space reserved for high priority transactions is misconfigured
    // stop program execution and warn the user with a proper error message
    const int64_t blkprio = gArgs.GetArg("-blockprioritypercentage",
                                         DEFAULT_BLOCK_PRIORITY_PERCENTAGE);
    if (std::string err; !config.SetBlockPriorityPercentage(blkprio, &err)) {
        return InitError(err);
    }

    // Make sure enough file descriptors are available
    int nBind = std::max(
        (gArgs.IsArgSet("-bind") ? gArgs.GetArgs("-bind").size() : 0) +
            (gArgs.IsArgSet("-whitebind") ? gArgs.GetArgs("-whitebind").size()
                                          : 0),
        size_t(1));
    nUserMaxConnections =
        gArgs.GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS);
    nMaxConnections = std::max(nUserMaxConnections, 0);

    // Trim requested connection counts, to fit into system limitations
    nMaxConnections =
        std::max(std::min(nMaxConnections,
                          (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS -
                                MAX_ADDNODE_CONNECTIONS)),
                 0);
    nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS +
                                   MAX_ADDNODE_CONNECTIONS);
    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));
    nMaxConnections =
        std::min(nFD - MIN_CORE_FILEDESCRIPTORS - MAX_ADDNODE_CONNECTIONS,
                 nMaxConnections);

    if (nMaxConnections < nUserMaxConnections) {
        InitWarning(strprintf(_("Reducing -maxconnections from %d to %d, "
                                "because of system limitations."),
                              nUserMaxConnections, nMaxConnections));
    }

    // Step 3: parameter-to-internal-flags
    if (gArgs.IsArgSet("-debug")) {
        // Special-case: if -debug=0/-nodebug is set, turn off debugging
        // messages
        const std::vector<std::string> &categories = gArgs.GetArgs("-debug");
        if (find(categories.begin(), categories.end(), std::string("0")) ==
            categories.end()) {
            for (const auto &cat : categories) {
                BCLog::LogFlags flag;
                if (!GetLogCategory(flag, cat)) {
                    InitWarning(
                        strprintf(_("Unsupported logging category %s=%s."),
                                  "-debug", cat));
                }
                GetLogger().EnableCategory(flag);
            }
        }
    }

    // Now remove the logging categories which were explicitly excluded
    if (gArgs.IsArgSet("-debugexclude")) {
        for (const std::string &cat : gArgs.GetArgs("-debugexclude")) {
            BCLog::LogFlags flag;
            if (!GetLogCategory(flag, cat)) {
                InitWarning(strprintf(_("Unsupported logging category %s=%s."),
                                      "-debugexclude", cat));
            }
            GetLogger().DisableCategory(flag);
        }
    }

    // Check for -debugnet
    if (gArgs.GetBoolArg("-debugnet", false))
        InitWarning(
            _("Unsupported argument -debugnet ignored, use -debug=net."));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (gArgs.IsArgSet("-socks"))
        return InitError(
            _("Unsupported argument -socks found. Setting SOCKS version isn't "
              "possible anymore, only SOCKS5 proxies are supported."));
    // Check for -tor - as this is a privacy risk to continue, exit here
    if (gArgs.GetBoolArg("-tor", false))
        return InitError(_("Unsupported argument -tor found, use -onion."));

    if (gArgs.GetBoolArg("-benchmark", false))
        InitWarning(
            _("Unsupported argument -benchmark ignored, use -debug=bench."));

    if (gArgs.GetBoolArg("-whitelistalwaysrelay", false))
        InitWarning(_("Unsupported argument -whitelistalwaysrelay ignored, use "
                      "-whitelistrelay and/or -whitelistforcerelay."));

    if (gArgs.IsArgSet("-blockminsize"))
        InitWarning("Unsupported argument -blockminsize ignored.");

    // Checkmempool and checkblockindex default to true in regtest mode
    int ratio = std::min<int>(
        std::max<int>(
            gArgs.GetArg("-checkmempool",
                         chainparams.DefaultConsistencyChecks() ? 1 : 0),
            0),
        1000000);
    if (ratio != 0) {
        mempool.SetSanityCheck(1.0 / ratio);
    }
    fCheckBlockIndex = gArgs.GetBoolArg("-checkblockindex",
                                        chainparams.DefaultConsistencyChecks());
    fCheckpointsEnabled =
        gArgs.GetBoolArg("-checkpoints", DEFAULT_CHECKPOINTS_ENABLED);

    hashAssumeValid = uint256S(
        gArgs.GetArg("-assumevalid",
                     chainparams.GetConsensus().defaultAssumeValid.GetHex()));
    if (!hashAssumeValid.IsNull())
        LogPrintf("Assuming ancestors of block %s have valid signatures.\n",
                  hashAssumeValid.GetHex());
    else
        LogPrintf("Validating signatures for all blocks.\n");

    if (gArgs.IsArgSet("-minimumchainwork")) {
        const std::string minChainWorkStr =
            gArgs.GetArg("-minimumchainwork", "");
        if (!IsHexNumber(minChainWorkStr)) {
            return InitError(strprintf(
                "Invalid non-hex (%s) minimum chain work value specified",
                minChainWorkStr));
        }
        nMinimumChainWork = UintToArith256(uint256S(minChainWorkStr));
    } else {
        nMinimumChainWork =
            UintToArith256(chainparams.GetConsensus().nMinimumChainWork);
    }
    LogPrintf("Setting nMinimumChainWork=%s\n", nMinimumChainWork.GetHex());
    if (nMinimumChainWork <
        UintToArith256(chainparams.GetConsensus().nMinimumChainWork)) {
        LogPrintf("Warning: nMinimumChainWork set below default value of %s\n",
                  chainparams.GetConsensus().nMinimumChainWork.GetHex());
    }
    
    // mempool limits  
    if (std::string err; !config.SetMaxMempool(
        gArgs.GetArgAsBytes("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE, ONE_MEGABYTE), &err))
    {
        return InitError(err);
    }

    // script validation settings
    if(std::string error; !config.SetBlockScriptValidatorsParams(
        gArgs.GetArg("-maxparallelblocks", DEFAULT_SCRIPT_CHECK_POOL_SIZE),
        gArgs.GetArg("-threadsperblock", DEFAULT_SCRIPTCHECK_THREADS),
        gArgs.GetArg("-scriptvalidatormaxbatchsize", DEFAULT_SCRIPT_CHECK_MAX_BATCH_SIZE),
        &error))
    {
        return InitError(error);
    }

    if(std::string error; !config.SetMaxConcurrentAsyncTasksPerNode(
        gArgs.GetArg("-maxparallelblocksperpeer", DEFAULT_NODE_ASYNC_TASKS_LIMIT),
        &error))
    {
        return InitError("-maxparallelblocksperpeer: " + error);
    }

    // Configure memory pool expiry
    if (std::string err; !config.SetMemPoolExpiry(
        gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * SECONDS_IN_ONE_HOUR, &err))
    {
        return InitError(err);
    }
    
    // Configure free transactions limit 
    if (std::string err; !config.SetLimitFreeRelay(
        gArgs.GetArgAsBytes("-limitfreerelay", DEFAULT_LIMITFREERELAY, ONE_KILOBYTE), &err))
    {
        return InitError(err);
    }

    // Configure max orphant Tx size
    if (std::string err; !config.SetMaxOrphanTxSize(
        gArgs.GetArgAsBytes("-maxorphantxsize", 
            COrphanTxns::DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE / ONE_MEGABYTE, ONE_MEGABYTE), &err))
    {
        return InitError(err);
    }

    // Configure height to stop running
    if (std::string err; !config.SetStopAtHeight(
        gArgs.GetArg("-stopatheight", DEFAULT_STOPATHEIGHT), &err))
    {
        return InitError(err);
    }

    // Configure promiscuous memory pool flags
    if (gArgs.IsArgSet("-promiscuousmempoolflags"))
    {
        if (std::string err; !config.SetPromiscuousMempoolFlags(
            gArgs.GetArg("-promiscuousmempoolflags", 0), &err))
        {
            return InitError(err);
        }
    }

    // Configure preferred size of blockfile.
    config.SetPreferredBlockFileSize(
        gArgs.GetArgAsBytes("-preferredblockfilesize",
            DEFAULT_PREFERRED_BLOCKFILE_SIZE));

    // Configure excessive block size.
    if(gArgs.IsArgSet("-excessiveblocksize")) {
        const uint64_t nProposedExcessiveBlockSize =
            gArgs.GetArgAsBytes("-excessiveblocksize", 0);
        if (std::string err; !config.SetMaxBlockSize(nProposedExcessiveBlockSize, &err)) {
            return InitError(err);
        }
    }

    if(gArgs.IsArgSet("-factormaxsendqueuesbytes")) {
        const uint64_t factorMaxSendQueuesBytes = gArgs.GetArg("-factormaxsendqueuesbytes", DEFAULT_FACTOR_MAX_SEND_QUEUES_BYTES);
        config.SetFactorMaxSendQueuesBytes(factorMaxSendQueuesBytes);
    }

    // Configure max generated block size.
    if(gArgs.IsArgSet("-blockmaxsize")) {
        const uint64_t nProposedMaxGeneratedBlockSize =
            gArgs.GetArgAsBytes("-blockmaxsize", 0 /* not used*/);
        if (std::string err; !config.SetMaxGeneratedBlockSize(nProposedMaxGeneratedBlockSize, &err)) {
            return InitError(err);
        }
    }

    // Configure block size related activation time
    if(gArgs.IsArgSet("-blocksizeactivationtime")) {
        const int64_t nProposedActivationTime =
            gArgs.GetArg("-blocksizeactivationtime", 0);
        if (std::string err; !config.SetBlockSizeActivationTime(nProposedActivationTime)){
            return InitError(err);
        }
    }

    // Configure whether to run extra block candidate validity checks
    config.SetTestBlockCandidateValidity(
        gArgs.GetBoolArg("-blockcandidatevaliditytest", chainparams.TestBlockCandidateValidity()));

    // Configure mining block assembler
    if(gArgs.IsArgSet("-blockassembler")) {
        std::string assemblerStr { boost::to_upper_copy<std::string>(gArgs.GetArg("-blockassembler", "")) };
        mining::CMiningFactory::BlockAssemblerType assembler { enum_cast<mining::CMiningFactory::BlockAssemblerType>(assemblerStr) };
        if(assembler == mining::CMiningFactory::BlockAssemblerType::UNKNOWN)
            assembler = mining::DEFAULT_BLOCK_ASSEMBLER_TYPE;
        config.SetMiningCandidateBuilder(assembler);
    }

    // Configure data carrier size.
    if(gArgs.IsArgSet("-datacarriersize")) {
        config.SetDataCarrierSize(gArgs.GetArgAsBytes("-datacarriersize", DEFAULT_DATA_CARRIER_SIZE));
    }

    // Configure descendant limit count.
    if(gArgs.IsArgSet("-limitdescendantcount")) {
        config.SetLimitDescendantCount(gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT));
    }

    // Configure ancestor limit count.
    if(gArgs.IsArgSet("-limitancestorcount")) {
        config.SetLimitAncestorCount(gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT));
    }

    // configure max transaction size policy
    if (gArgs.IsArgSet("-maxtxsizepolicy"))
    {
        int64_t maxTxSizePolicy = gArgs.GetArgAsBytes("-maxtxsizepolicy", DEFAULT_MAX_TX_SIZE_POLICY_AFTER_GENESIS);
        if (std::string err; !config.SetMaxTxSizePolicy(maxTxSizePolicy, &err)) {
            return InitError(err);
        }
    }

    // configure min ratio between tx input to tx output size to be considered free consolidation tx.
    if (gArgs.IsArgSet("-minconsolidationfactor"))
    {
        uint64_t minConsolidationFactor = gArgs.GetArg("-minconsolidationfactor", DEFAULT_MIN_CONSOLIDATION_FACTOR);
        if (std::string err; !config.SetMinConsolidationFactor(minConsolidationFactor, &err)) {
            return InitError(err);
        }
    }

    // configure maxiumum scriptSig input size not considered spam in a consolidation transaction
    if (gArgs.IsArgSet("-maxconsolidationinputscriptsize"))
    {
        uint64_t maxConsolidationInputScriptSize = gArgs.GetArg("-maxconsolidationinputscriptsize", DEFAULT_MAX_CONSOLIDATION_INPUT_SCRIPT_SIZE);
        if (std::string err; !config.SetMaxConsolidationInputScriptSize(maxConsolidationInputScriptSize, &err)) {
            return InitError(err);
        }
    }

    // configure minimum number of confirmations needed by transactions spent in a consolidatin transaction
    if (gArgs.IsArgSet("-minconsolidationinputmaturity"))
    {
        uint64_t param = gArgs.GetArg("-minconsolidationinputmaturity", DEFAULT_MIN_CONSOLIDATION_INPUT_MATURITY);
        if (std::string err; !config.SetMinConsolidationInputMaturity(param, &err)) {
            return InitError(err);
        }
    }

    // configure if non standard inputs for consolidation transactions are allowed
    if (gArgs.IsArgSet("-acceptnonstdconsolidationinput"))
    {
        uint64_t param = gArgs.GetArg("-acceptnonstdconsolidationinput", DEFAULT_ACCEPT_NON_STD_CONSOLIDATION_INPUT);
        if (std::string err; !config.SetAcceptNonStdConsolidationInput(param, &err)) {
            return InitError(err);
        }
    }

    // Configure descendant limit size.
    if(gArgs.IsArgSet("-limitdescendantsize")) {
        config.SetLimitDescendantSize(gArgs.GetArgAsBytes("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT, ONE_KILOBYTE));
    }

    // Configure ancestor limit size.
    if(gArgs.IsArgSet("-limitancestorsize")) {
        config.SetLimitAncestorSize(gArgs.GetArgAsBytes("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT, ONE_KILOBYTE));
    }

    // Configure genesis activation height.
    int64_t genesisActivationHeight = gArgs.GetArg("-genesisactivationheight", chainparams.GetConsensus().genesisHeight);
    if (std::string err; !config.SetGenesisActivationHeight(genesisActivationHeight, &err)) {
        return InitError(err);
    }

    if (std::string err; !config.SetMaxStackMemoryUsage(
        gArgs.GetArgAsBytes("-maxstackmemoryusageconsensus", 0),
        gArgs.GetArgAsBytes("-maxstackmemoryusagepolicy", DEFAULT_STACK_MEMORY_USAGE_POLICY_AFTER_GENESIS),
        &err))
    {
        return InitError(err);
    }

    //Configure max script size after genesis
    if (gArgs.IsArgSet("-maxscriptsizepolicy")) {
        int64_t maxScriptSize = gArgs.GetArgAsBytes("-maxscriptsizepolicy", DEFAULT_MAX_SCRIPT_SIZE_POLICY_AFTER_GENESIS);
        if (std::string err; !config.SetMaxScriptSizePolicy(maxScriptSize, &err)) {
            return InitError(err);
        }
    }


    // block pruning; get the amount of disk space (in MiB) to allot for block &
    // undo files
    int64_t nPruneArg = gArgs.GetArg("-prune", 0);
    if (nPruneArg < 0) {
        return InitError(
            _("Prune cannot be configured with a negative value."));
    }
    nPruneTarget = (uint64_t)nPruneArg * ONE_MEBIBYTE;
    if (nPruneArg == 1) { // manual pruning: -prune=1
        LogPrintf("Block pruning enabled.  Use RPC call "
                  "pruneblockchain(height) to manually prune block and undo "
                  "files.\n");
        nPruneTarget = std::numeric_limits<uint64_t>::max();
        fPruneMode = true;
    } else if (nPruneTarget) {
        if (nPruneTarget < MIN_DISK_SPACE_FOR_BLOCK_FILES) {
            return InitError(
                strprintf(_("Prune configured below the minimum of %d MiB.  "
                            "Please use a higher number."),
                          MIN_DISK_SPACE_FOR_BLOCK_FILES / ONE_MEBIBYTE));
        }
        LogPrintf("Prune configured to target %uMiB on disk for block and undo "
                  "files.\n",
                  nPruneTarget / ONE_MEBIBYTE);
        fPruneMode = true;
    }

    int64_t nSkip = gArgs.GetArg("-pruneblocks", 0);
    if(nSkip == 0) {
        fPruneBlocksMode = false;
    } else if(nSkip == 824188) {
        fPruneBlocksMode = true;
    } else {
        return InitError(
            strprintf("The configuration value of pruneblocks is not currently supported"));
    }

    if(std::string err; !config.SetMaxStdTxnValidationDuration(
        gArgs.GetArg(
            "-maxstdtxvalidationduration",
            DEFAULT_MAX_STD_TXN_VALIDATION_DURATION.count()),
        &err))
    {
        return InitError(err);
    }

    if(std::string err; !config.SetMaxNonStdTxnValidationDuration(
        gArgs.GetArg(
            "-maxnonstdtxvalidationduration",
            DEFAULT_MAX_NON_STD_TXN_VALIDATION_DURATION.count()),
        &err))
    {
        return InitError(err);
    }

    if (!(config.GetMaxStdTxnValidationDuration() < config.GetMaxNonStdTxnValidationDuration())) {
        return InitError(
            strprintf("maxstdtxvalidationduration must be less than maxnonstdtxvalidationduration"));
    }

    if (!(gArgs.GetArg("-maxtxnvalidatorasynctasksrunduration",
            CTxnValidator::DEFAULT_MAX_ASYNC_TASKS_RUN_DURATION.count()) >
        config.GetMaxNonStdTxnValidationDuration().count())) {
        return InitError(
            strprintf("maxtxnvalidatorasynctasksrunduration must be greater than maxnonstdtxvalidationduration"));
    }

    if(std::string err; !config.SetMaxCoinsViewCacheSize(
        gArgs.GetArgAsBytes("-maxcoinsviewcachesize", 0), &err))
    {
        return InitError(err);
    }

    RegisterAllRPCCommands(tableRPC);
#ifdef ENABLE_WALLET
    RegisterWalletRPCCommands(tableRPC);
    RegisterDumpRPCCommands(tableRPC);
#endif

    nConnectTimeout = gArgs.GetArg("-timeout", DEFAULT_CONNECT_TIMEOUT);
    if (nConnectTimeout <= 0) nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

    // Fee-per-kilobyte amount considered the same as "free". If you are mining,
    // be careful setting this: if you set it to zero then a transaction spammer
    // can cheaply fill blocks using 1-satoshi-fee transactions. It should be
    // set above the real cost to you of processing a transaction.
    if (gArgs.IsArgSet("-minrelaytxfee")) {
        Amount n(0);
        auto parsed = ParseMoney(gArgs.GetArg("-minrelaytxfee", ""), n);
        if (!parsed || Amount(0) == n)
            return InitError(AmountErrMsg("minrelaytxfee",
                                          gArgs.GetArg("-minrelaytxfee", "")));
        // High fee check is done afterward in CWallet::ParameterInteraction()
        config.SetMinFeePerKB(CFeeRate(n));
    } else {
        config.SetMinFeePerKB(CFeeRate(DEFAULT_MIN_RELAY_TX_FEE));
    }

    // Sanity check argument for min fee for including tx in block
    // TODO: Harmonize which arguments need sanity checking and where that
    // happens.
    if (gArgs.IsArgSet("-blockmintxfee")) {
        Amount n(0);
        if (!ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
            return InitError(AmountErrMsg("blockmintxfee",
                                          gArgs.GetArg("-blockmintxfee", "")));
        }
        mempool.SetBlockMinTxFee(CFeeRate(n));
    } else {
        mempool.SetBlockMinTxFee(CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE));
    }

    // Feerate used to define dust.  Shouldn't be changed lightly as old
    // implementations may inadvertently create non-standard transactions.
    if (gArgs.IsArgSet("-dustrelayfee")) {
        Amount n(0);
        auto parsed = ParseMoney(gArgs.GetArg("-dustrelayfee", ""), n);
        if (!parsed || Amount(0) == n)
            return InitError(AmountErrMsg("dustrelayfee",
                                          gArgs.GetArg("-dustrelayfee", "")));
        dustRelayFee = CFeeRate(n);
    }

    fRequireStandard =
        !gArgs.GetBoolArg("-acceptnonstdtxn", !chainparams.RequireStandard());
    if (chainparams.RequireStandard() && !fRequireStandard)
        return InitError(
            strprintf("acceptnonstdtxn is not currently supported for %s chain",
                      chainparams.NetworkIDString()));

    config.SetAcceptNonStandardOutput(
        gArgs.GetBoolArg("-acceptnonstdoutputs", config.GetAcceptNonStandardOutput(true)));

#ifdef ENABLE_WALLET
    if (!CWallet::ParameterInteraction()) return false;
#endif

    fIsBareMultisigStd =
        gArgs.GetBoolArg("-permitbaremultisig", DEFAULT_PERMIT_BAREMULTISIG);
    fAcceptDatacarrier =
        gArgs.GetBoolArg("-datacarrier", DEFAULT_ACCEPT_DATACARRIER);

    // Option to startup with mocktime set (used for regression testing):
    SetMockTime(gArgs.GetArg("-mocktime", 0)); // SetMockTime(0) is a no-op

    if (gArgs.GetBoolArg("-peerbloomfilters", DEFAULT_PEERBLOOMFILTERS))
        nLocalServices = ServiceFlags(nLocalServices | NODE_BLOOM);

    // Signal Bitcoin Cash support.
    // TODO: remove some time after the hardfork when no longer needed
    // to differentiate the network nodes.
    nLocalServices = ServiceFlags(nLocalServices | NODE_BITCOIN_CASH);

    nMaxTipAge = gArgs.GetArg("-maxtipage", DEFAULT_MAX_TIP_AGE);

    // Configure the maximum number of sigops we're willing to relay/mine in a single tx
    if (gArgs.IsArgSet("-maxtxsigopscountspolicy"))
    {
        const int64_t value = gArgs.GetArg("-maxtxsigopscountspolicy", DEFAULT_TX_SIGOPS_COUNT_POLICY_AFTER_GENESIS);
        if (std::string err; !config.SetMaxTxSigOpsCountPolicy(value, &err))
        {
            return InitError(err);
        }
    }

    // Configure max number of public keys per MULTISIG operation
    if (gArgs.IsArgSet("-maxpubkeyspermultisigpolicy"))
    {
        const int64_t value = gArgs.GetArg("-maxpubkeyspermultisigpolicy", DEFAULT_PUBKEYS_PER_MULTISIG_POLICY_AFTER_GENESIS);

        std::string err;
        if (!config.SetMaxPubKeysPerMultiSigPolicy(value, &err))
        {
            return InitError(err);
        }
    }

    // Configure maximum length of numbers in scripts
    if (gArgs.IsArgSet("-maxscriptnumlengthpolicy"))
    {
        const int64_t value = gArgs.GetArgAsBytes("-maxscriptnumlengthpolicy", DEFAULT_SCRIPT_NUM_LENGTH_POLICY_AFTER_GENESIS);
        if (std::string err; !config.SetMaxScriptNumLengthPolicy(value, &err))
        {
            return InitError(err);
        }
    }

    // Configure max number of blocks in which Genesis graceful period is active
    if (gArgs.IsArgSet("-maxgenesisgracefulperiod"))
    {
        const int64_t value = gArgs.GetArg("-maxgenesisgracefulperiod", DEFAULT_GENESIS_GRACEFULL_ACTIVATION_PERIOD);

        std::string err;
        if (!config.SetGenesisGracefulPeriod(value, &err))
        {
            return InitError(err);
        }
    }

    if (gArgs.IsArgSet("-invalidateblock"))
    {
        std::set<uint256> invalidBlocks;
        for(auto invalidBlockHashStr : gArgs.GetArgs("-invalidateblock"))
        {
            uint256 hash = uint256S(invalidBlockHashStr);
            invalidBlocks.insert(hash);
        }
        config.SetInvalidBlocks(invalidBlocks);
    }

    if (gArgs.IsArgSet("-banclientua"))
    {
        std::set<std::string> invalidUAClients;
        for (auto invalidClient : gArgs.GetArgs("-banclientua"))
        {
            invalidUAClients.insert(invalidClient);
        }
        config.SetBanClientUA(invalidUAClients);
    }

    return true;
}

static bool LockDataDirectory(bool probeOnly) {
    std::string strDataDir = GetDataDir().string();

    // Make sure only a single Bitcoin process is using the data directory.
    fs::path pathLockFile = GetDataDir() / ".lock";
    // empty lock file; created if it doesn't exist.
    FILE *file = fsbridge::fopen(pathLockFile, "a");
    if (file) fclose(file);

    try {
        static boost::interprocess::file_lock lock(
            pathLockFile.string().c_str());
        if (!lock.try_lock()) {
            return InitError(
                strprintf(_("Cannot obtain a lock on data directory %s. %s is "
                            "probably already running."),
                          strDataDir, _(PACKAGE_NAME)));
        }
        if (probeOnly) {
            lock.unlock();
        }
    } catch (const boost::interprocess::interprocess_exception &e) {
        return InitError(strprintf(_("Cannot obtain a lock on data directory "
                                     "%s. %s is probably already running.") +
                                       " %s.",
                                   strDataDir, _(PACKAGE_NAME), e.what()));
    }
    return true;
}

bool AppInitSanityChecks() {
    // Step 4: sanity checks

    // Initialize elliptic curve code
    std::string sha256_algo = SHA256AutoDetect();
    LogPrintf("Using the '%s' SHA256 implementation\n", sha256_algo);
    RandomInit();
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());

    // Sanity check
    if (!InitSanityCheck()) {
        return InitError(strprintf(
            _("Initialization sanity check failed. %s is shutting down."),
            _(PACKAGE_NAME)));
    }

    // Probe the data directory lock to give an early error message, if possible
    return LockDataDirectory(true);
}

void preloadChainStateThreadFunction()
{
#ifndef WIN32
    auto path = boost::filesystem::canonical(GetDataDir() / "chainstate").string();
    LogPrintf("Preload started\n");
    try {
        auto start = std::chrono::system_clock::now();
        VMTouch vm;

        vm.vmtouch_touch(path);

        auto end = std::chrono::system_clock::now();

        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();
        auto& warnings = vm.get_warnings();
        if (!warnings.empty())
        {
            LogPrintf("Warnings occured during chainstate preload\n:");
            for(auto& warning : warnings)
            {
                LogPrintf("Preload warning:  %s \n", warning.c_str());
            }
        }
        LogPrintf("Preload finished in %" PRId64 " [ms]. Preloaded %" PRId64 " MB of data (%d %% were already present in memory)\n",
            elapsed, vm.total_pages*vm.pagesize/ONE_MEGABYTE, (int) vm.getPagesInCorePercent());

        // verify that pages were not evicted
        VMTouch vm2;
        int stillLoadedPercent = (int) vm2.vmtouch_check(path);

        if (stillLoadedPercent < 90) {
            LogPrintf("WARNING: Only %d %% of data still present in memory after preloading. Increae amount of free RAM to get the benefits of preloading\n", stillLoadedPercent);
        }

    }   catch(const std::runtime_error& ex) {
        LogPrintf("Error while preloading chain state: %s\n", ex.what());
    }

#else
    LogPrintf("Preload is not supported on this platform!");
    return;
#endif
}

void preloadChainState(boost::thread_group &threadGroup)
{
    int64_t preload;
    preload = gArgs.GetArg("-preload", 0);
    if (preload == 0)
    {
        LogPrintf("Chainstate will NOT be preloaded\n");
        return;
    }

    if (preload == 1 )// preload with vmtouch
    {
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "preload", preloadChainStateThreadFunction));
    }
    else
    {
        LogPrintf("Unknown value of -preload. No preloading will be done\n");
    }
}

bool AppInitMain(Config &config, boost::thread_group &threadGroup,
                 CScheduler &scheduler, const task::CCancellationToken& shutdownToken) {
    const CChainParams &chainparams = config.GetChainParams();
    // Step 4a: application initialization

    // After daemonization get the data directory lock again and hold on to it
    // until exit. This creates a slight window for a race condition to happen,
    // however this condition is harmless: it will at most make us exit without
    // printing a message to console.
    if (!LockDataDirectory(false)) {
        // Detailed error printed inside LockDataDirectory
        return false;
    }

#ifndef WIN32
    CreatePidFile(GetPidFile(), getpid());
#endif

    BCLog::Logger &logger = GetLogger();

    bool default_shrinkdebugfile = logger.DefaultShrinkDebugFile();
    if (gArgs.GetBoolArg("-shrinkdebugfile", default_shrinkdebugfile)) {
        // Do this first since it both loads a bunch of bitcoind.log into memory,
        // and because this needs to happen before any other bitcoind.log printing.
        logger.ShrinkDebugFile();
    }

    if (logger.fPrintToDebugLog) {
        if (logger.OpenDebugLog()) {
            return InitError(strprintf(_("Unable to open log file.")));
        }
    }

    if (!logger.fLogTimestamps) {
        LogPrintf("Startup time: %s\n",
                  DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()));
    }
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Using data directory %s\n", GetDataDir().string());
    LogPrintf(
        "Using config file %s\n",
        GetConfigFile(gArgs.GetArg("-conf", BITCOIN_CONF_FILENAME)).string());
    LogPrintf("Using at most %i automatic connections (%i file descriptors "
              "available)\n",
              nMaxConnections, nFD);

    InitSignatureCache();
    InitScriptExecutionCache();

    LogPrintf("Using %u threads for script verification\n",
              config.GetPerBlockScriptValidatorThreadsCount());
    InitScriptCheckQueues(config, threadGroup);

    // Late configuration for globaly constructed objects
    mempool.getNonFinalPool().loadConfig();

    // Start the lightweight task scheduler thread
    scheduler.startServiceThread(threadGroup);

    /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    if (gArgs.GetBoolArg("-server", false)) {
        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        if (!AppInitServers(config, threadGroup)) {
            return InitError(
                _("Unable to start HTTP server. See debug log for details."));
        }
    }

    if (gArgs.IsArgSet("-maxopsperscriptpolicy"))
    {
        const int64_t value = gArgs.GetArg("-maxopsperscriptpolicy", 0);

        std::string err;
        if (!config.SetMaxOpsPerScriptPolicy(value, &err))
        {
            return InitError(err);
        }
    }

    int64_t nStart=0;

// Step 5: verify wallet database integrity
#ifdef ENABLE_WALLET
    if (!CWallet::Verify(chainparams)) {
        return false;
    }
#endif
    // Step 6: network initialization

    // Note that we absolutely cannot open any actual connections
    // until the very end ("start node") as the UTXO/block state
    // is not yet setup and may end up being set up twice if we
    // need to reindex later.

    assert(!g_connman);
    {
        int64_t duration = gArgs.GetArg("-debugp2pthreadstalls", 0);
        g_connman =
            std::make_unique<CConnman>(
                config, GetRand(std::numeric_limits<uint64_t>::max()),
                GetRand(std::numeric_limits<uint64_t>::max()),
                std::chrono::milliseconds{duration > 0 ? duration : 0});
    }
    CConnman &connman = *g_connman;

    peerLogic.reset(new PeerLogicValidation(&connman));
    if (gArgs.IsArgSet("-broadcastdelay")) {
        const int64_t nDelayMillisecs = gArgs.GetArg("-broadcastdelay", DEFAULT_INV_BROADCAST_DELAY);
        if(!SetInvBroadcastDelay(nDelayMillisecs)){
            return InitError(strprintf(_("Error setting broadcastdelay=%d"), nDelayMillisecs));
        }
    }
    RegisterValidationInterface(peerLogic.get());
    RegisterNodeSignals(GetNodeSignals());

    if (gArgs.IsArgSet("-onlynet")) {
        std::set<enum Network> nets;
        for (const std::string &snet : gArgs.GetArgs("-onlynet")) {
            enum Network net = ParseNetwork(snet);
            if (net == NET_UNROUTABLE)
                return InitError(strprintf(
                    _("Unknown network specified in -onlynet: '%s'"), snet));
            nets.insert(net);
        }
        for (int n = 0; n < NET_MAX; n++) {
            enum Network net = (enum Network)n;
            if (!nets.count(net)) SetLimited(net);
        }
    }

    if (gArgs.IsArgSet("-whitelist")) {
        for (const std::string &net : gArgs.GetArgs("-whitelist")) {
            CSubNet subnet;
            LookupSubNet(net.c_str(), subnet);
            if (!subnet.IsValid())
                return InitError(strprintf(
                    _("Invalid netmask specified in -whitelist: '%s'"), net));
            connman.AddWhitelistedRange(subnet);
        }
    }

    bool proxyRandomize =
        gArgs.GetBoolArg("-proxyrandomize", DEFAULT_PROXYRANDOMIZE);
    // -proxy sets a proxy for all outgoing network traffic
    // -noproxy (or -proxy=0) as well as the empty string can be used to not set
    // a proxy, this is the default
    std::string proxyArg = gArgs.GetArg("-proxy", "");
    SetLimited(NET_TOR);
    if (proxyArg != "" && proxyArg != "0") {
        CService resolved(LookupNumeric(proxyArg.c_str(), 9050));
        proxyType addrProxy = proxyType(resolved, proxyRandomize);
        if (!addrProxy.IsValid()) {
            return InitError(
                strprintf(_("Invalid -proxy address: '%s'"), proxyArg));
        }

        SetProxy(NET_IPV4, addrProxy);
        SetProxy(NET_IPV6, addrProxy);
        SetProxy(NET_TOR, addrProxy);
        SetNameProxy(addrProxy);
        SetLimited(NET_TOR, false); // by default, -proxy sets onion as
                                    // reachable, unless -noonion later
    }

    // -onion can be used to set only a proxy for .onion, or override normal
    // proxy for .onion addresses.
    // -noonion (or -onion=0) disables connecting to .onion entirely. An empty
    // string is used to not override the onion proxy (in which case it defaults
    // to -proxy set above, or none)
    std::string onionArg = gArgs.GetArg("-onion", "");
    if (onionArg != "") {
        if (onionArg == "0") {   // Handle -noonion/-onion=0
            SetLimited(NET_TOR); // set onions as unreachable
        } else {
            CService resolved(LookupNumeric(onionArg.c_str(), 9050));
            proxyType addrOnion = proxyType(resolved, proxyRandomize);
            if (!addrOnion.IsValid()) {
                return InitError(
                    strprintf(_("Invalid -onion address: '%s'"), onionArg));
            }
            SetProxy(NET_TOR, addrOnion);
            SetLimited(NET_TOR, false);
        }
    }

    // see Step 2: parameter interactions for more information about these
    fListen = gArgs.GetBoolArg("-listen", DEFAULT_LISTEN);
    fDiscover = gArgs.GetBoolArg("-discover", true);
    fNameLookup = gArgs.GetBoolArg("-dns", DEFAULT_NAME_LOOKUP);
    fRelayTxes = !gArgs.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY);

    if (fListen) {
        bool fBound = false;
        if (gArgs.IsArgSet("-bind")) {
            for (const std::string &strBind : gArgs.GetArgs("-bind")) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(),
                            false)) {
                    return InitError(ResolveErrMsg("bind", strBind));
                }
                fBound |=
                    Bind(connman, addrBind, (BF_EXPLICIT | BF_REPORT_ERROR));
            }
        }
        if (gArgs.IsArgSet("-whitebind")) {
            for (const std::string &strBind : gArgs.GetArgs("-whitebind")) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, 0, false)) {
                    return InitError(ResolveErrMsg("whitebind", strBind));
                }
                if (addrBind.GetPort() == 0) {
                    return InitError(strprintf(
                        _("Need to specify a port with -whitebind: '%s'"),
                        strBind));
                }
                fBound |= Bind(connman, addrBind,
                               (BF_EXPLICIT | BF_REPORT_ERROR | BF_WHITELIST));
            }
        }
        if (!gArgs.IsArgSet("-bind") && !gArgs.IsArgSet("-whitebind")) {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            fBound |=
                Bind(connman, CService(in6addr_any, GetListenPort()), BF_NONE);
            fBound |= Bind(connman, CService(inaddr_any, GetListenPort()),
                           !fBound ? BF_REPORT_ERROR : BF_NONE);
        }
        if (!fBound) {
            return InitError(_("Failed to listen on any port. Use -listen=0 if "
                               "you want this."));
        }
    }

    if (gArgs.IsArgSet("-externalip")) {
        for (const std::string &strAddr : gArgs.GetArgs("-externalip")) {
            CService addrLocal;
            if (Lookup(strAddr.c_str(), addrLocal, GetListenPort(),
                       fNameLookup) &&
                addrLocal.IsValid()) {
                AddLocal(addrLocal, LOCAL_MANUAL);
            } else {
                return InitError(ResolveErrMsg("externalip", strAddr));
            }
        }
    }

    if (gArgs.IsArgSet("-seednode")) {
        for (const std::string &strDest : gArgs.GetArgs("-seednode")) {
            connman.AddOneShot(strDest);
        }
    }

#if ENABLE_ZMQ
    pzmqNotificationInterface = CZMQNotificationInterface::Create();

    if (pzmqNotificationInterface) {
        RegisterValidationInterface(pzmqNotificationInterface);
    }
#endif
    // unlimited unless -maxuploadtarget is set
    uint64_t nMaxOutboundLimit = 0;
    uint64_t nMaxOutboundTimeframe = MAX_UPLOAD_TIMEFRAME;

    if (gArgs.IsArgSet("-maxuploadtarget")) {
        nMaxOutboundLimit =
            gArgs.GetArgAsBytes("-maxuploadtarget", DEFAULT_MAX_UPLOAD_TARGET, ONE_MEBIBYTE);
    }

    // Step 7: load block chain

    fReindex = gArgs.GetBoolArg("-reindex", false);
    bool fReindexChainState = gArgs.GetBoolArg("-reindex-chainstate", false);

    // cache size calculations
    int64_t nTotalCache = gArgs.GetArgAsBytes("-dbcache", nDefaultDbCache, ONE_MEBIBYTE);
    // total cache cannot be less than nMinDbCache
    nTotalCache = std::max(nTotalCache, nMinDbCache << 20);
    // total cache cannot be greater than nMaxDbcache
    nTotalCache = std::min(nTotalCache, nMaxDbCache << 20);
    int64_t nBlockTreeDBCache = nTotalCache / 8;
    nBlockTreeDBCache = std::min(nBlockTreeDBCache,
                                 (gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX)
                                      ? nMaxBlockDBAndTxIndexCache
                                      : nMaxBlockDBCache)
                                     << 20);
    nTotalCache -= nBlockTreeDBCache;
    // use 25%-50% of the remainder for disk cache
    int64_t nCoinDBCache =
        std::min(nTotalCache / 2, (nTotalCache / 4) + (1 << 23));
    // cap total coins db cache
    nCoinDBCache = std::min(nCoinDBCache, nMaxCoinsDBCache << 20);
    nTotalCache -= nCoinDBCache;
    // the rest goes to in-memory cache
    nCoinCacheUsage = nTotalCache;
    int64_t nMempoolSizeMax = config.GetMaxMempool();
    LogPrintf("Cache configuration:\n");
    LogPrintf("* Using %.1fMiB for block index database\n",
              nBlockTreeDBCache * (1.0 / ONE_MEBIBYTE));
    LogPrintf("* Using %.1fMiB for chain state database\n",
              nCoinDBCache * (1.0 / ONE_MEBIBYTE));
    LogPrintf("* Using %.1fMiB for in-memory UTXO set (plus up to %.1fMiB of "
              "unused mempool space)\n",
              nCoinCacheUsage * (1.0 / ONE_MEBIBYTE),
              nMempoolSizeMax * (1.0 / ONE_MEBIBYTE));

    bool fLoaded = false;
    while (!fLoaded && !shutdownToken.IsCanceled()) {
        bool fReset = fReindex;
        std::string strLoadError;

        uiInterface.InitMessage(_("Loading block index..."));

        nStart = GetTimeMillis();
        do {
            try {
                UnloadBlockIndex();
                delete pcoinsTip;
                delete pcoinsdbview;
                delete pcoinscatcher;
                delete pblocktree;

                pblocktree =
                    new CBlockTreeDB(nBlockTreeDBCache, false, fReindex);
                pcoinsdbview = new CCoinsViewDB(nCoinDBCache, false,
                                                fReindex || fReindexChainState);
                pcoinscatcher = new CCoinsViewErrorCatcher(pcoinsdbview);

                if (fReindex) {
                    pblocktree->WriteReindexing(true);
                    // If we're reindexing in prune mode, wipe away unusable
                    // block files and all undo data files
                    if (fPruneMode) {
                        CleanupBlockRevFiles();
                    }
                } else if (pcoinsdbview->IsOldDBFormat()) {
                    strLoadError = _("Refusing to start, older database format detected");
                    break;
                }
                if (shutdownToken.IsCanceled()) break;

                if (!LoadBlockIndex(chainparams)) {
                    strLoadError = _("Error loading block database");
                    break;
                }

                // If the loaded chain has a wrong genesis, bail out immediately
                // (we're likely using a testnet datadir, or the other way
                // around).
                if (!mapBlockIndex.empty() &&
                    mapBlockIndex.count(
                        chainparams.GetConsensus().hashGenesisBlock) == 0) {
                    return InitError(_("Incorrect or no genesis block found. "
                                       "Wrong datadir for network?"));
                }

                // Initialize the block index (no-op if non-empty database was
                // already loaded)
                if (!InitBlockIndex(config)) {
                    strLoadError = _("Error initializing block database");
                    break;
                }

                // Check for changed -txindex state
                if (fTxIndex != gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX)) {
                    strLoadError = _("You need to rebuild the database using "
                                     "-reindex-chainstate to change -txindex");
                    break;
                }

                // Check for changed -prune state.  What we are concerned about
                // is a user who has pruned blocks in the past, but is now
                // trying to run unpruned.
                if (fHavePruned && !fPruneMode) {
                    strLoadError =
                        _("You need to rebuild the database using -reindex to "
                          "go back to unpruned mode.  This will redownload the "
                          "entire blockchain");
                    break;
                }

                if (!ReplayBlocks(config, pcoinsdbview)) {
                    strLoadError =
                        _("Unable to replay blocks. You will need to rebuild "
                          "the database using -reindex-chainstate.");
                    break;
                }

                pcoinsTip = new CCoinsViewCache(pcoinscatcher);
                {
                    LOCK(cs_main);
                    LoadChainTip(chainparams);
                }

                if (!fReindex && chainActive.Tip() != nullptr) {
                    uiInterface.InitMessage(_("Rewinding blocks..."));
                    if (!RewindBlockIndex(config)) {
                        strLoadError = _("Unable to rewind the database to a "
                                         "pre-fork state. You will need to "
                                         "redownload the blockchain");
                        break;
                    }
                }

                uiInterface.InitMessage(_("Verifying blocks..."));
                if (fHavePruned &&
                    gArgs.GetArg("-checkblocks", DEFAULT_CHECKBLOCKS) >
                        MIN_BLOCKS_TO_KEEP) {
                    LogPrintf("Prune: pruned datadir may not have more than %d "
                              "blocks; only checking available blocks",
                              MIN_BLOCKS_TO_KEEP);
                }

                {
                    LOCK(cs_main);
                    CBlockIndex *tip = chainActive.Tip();
                    RPCNotifyBlockChange(true, tip);
                    if (tip &&
                        tip->nTime >
                            GetAdjustedTime() + MAX_FUTURE_BLOCK_TIME) {
                        strLoadError =
                            _("The block database contains a block which "
                              "appears to be from the future. "
                              "This may be due to your computer's date and "
                              "time being set incorrectly. "
                              "Only rebuild the block database if you are sure "
                              "that your computer's date and time are correct");
                        break;
                    }
                }

                if (!CVerifyDB().VerifyDB(
                        config, pcoinsdbview,
                        gArgs.GetArg("-checklevel", DEFAULT_CHECKLEVEL),
                        gArgs.GetArg("-checkblocks", DEFAULT_CHECKBLOCKS),
                        shutdownToken)) {
                    strLoadError = _("Corrupted block database detected");
                    break;
                }

                InvalidateBlocksFromConfig(config);

            } catch (const std::exception &e) {
                LogPrintf("%s\n", e.what());
                strLoadError = _("Error opening block database");
                break;
            }

            fLoaded = true;
        } while (false);

        if (!fLoaded && !shutdownToken.IsCanceled()) {
            // first suggest a reindex
            if (!fReset) {
                bool fRet = uiInterface.ThreadSafeQuestion(
                    strLoadError + ".\n\n" +
                        _("Do you want to rebuild the block database now?"),
                    strLoadError + ".\nPlease restart with -reindex or "
                                   "-reindex-chainstate to recover.",
                    "",
                    CClientUIInterface::MSG_ERROR |
                        CClientUIInterface::BTN_ABORT);
                if (fRet && !shutdownToken.IsCanceled()) {
                    fReindex = true;
                } else {
                    LogPrintf("Aborted block database rebuild. Exiting.\n");
                    return false;
                }
            } else {
                return InitError(strLoadError);
            }
        }
    }

    // As LoadBlockIndex can take several minutes.
    // As the program has not fully started yet, Shutdown() is possibly
    // overkill.
    if (shutdownToken.IsCanceled()) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    // After block chain is loaded check fork tip statuses and
    // restore global safe mode state.
    CheckSafeModeParametersForAllForksOnStartup();

    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);
  


// Step 8: load wallet
#ifdef ENABLE_WALLET
    if (!CWallet::InitLoadWallet(chainparams)) return false;
#else
    LogPrintf("No wallet support compiled in!\n");
#endif

    // Step 9: data directory maintenance

    // if pruning, unset the service bit and perform the initial blockstore
    // prune after any wallet rescanning has taken place.
    if (fPruneMode) {
        LogPrintf("Unsetting NODE_NETWORK on prune mode\n");
        nLocalServices = ServiceFlags(nLocalServices & ~NODE_NETWORK);
        if (!fReindex) {
            uiInterface.InitMessage(_("Pruning blockstore..."));
            PruneAndFlush();
        }
    }

    // Step 10: import blocks

    if (!CheckDiskSpace()) {
        return false;
    }

    // Either install a handler to notify us when genesis activates, or set
    // fHaveGenesis directly.
    // No locking, as this happens before any background thread is started.
    if (chainActive.Tip() == nullptr) {
        uiInterface.NotifyBlockTip.connect(BlockNotifyGenesisWait);
    } else {
        fHaveGenesis = true;
    }

    if (gArgs.IsArgSet("-blocknotify")) {
        uiInterface.NotifyBlockTip.connect(BlockNotifyCallback);
    }

    std::vector<fs::path> vImportFiles;
    if (gArgs.IsArgSet("-loadblock")) {
        for (const std::string &strFile : gArgs.GetArgs("-loadblock")) {
            vImportFiles.push_back(strFile);
        }
    }

    threadGroup.create_thread(
        [&config, vImportFiles, shutdownToken]
        {
            TraceThread(
                "import_files",
                [&config, &vImportFiles, shutdownToken]{ThreadImport(config, vImportFiles, shutdownToken);});
        });

    // Wait for genesis block to be processed
    {
        boost::unique_lock<boost::mutex> lock(cs_GenesisWait);
        while (!fHaveGenesis) {
            condvar_GenesisWait.wait(lock);
        }
        uiInterface.NotifyBlockTip.disconnect(BlockNotifyGenesisWait);
    }

    preloadChainState(threadGroup);

    // Step 11: start node

    //// debug print
    {
        LOCK(cs_main);
        LogPrintf("mapBlockIndex.size() = %u\n", mapBlockIndex.size());
    }
    LogPrintf("nBestHeight = %d\n", chainActive.Height());
    if (gArgs.GetBoolArg("-listenonion", DEFAULT_LISTEN_ONION)) {
        StartTorControl(threadGroup, scheduler);
    }

    Discover(threadGroup);

    // Map ports with UPnP
    MapPort(gArgs.GetBoolArg("-upnp", DEFAULT_UPNP));

    std::string strNodeError;
    CConnman::Options connOptions;
    connOptions.nLocalServices = nLocalServices;
    connOptions.nRelevantServices = nRelevantServices;
    connOptions.nMaxConnections = nMaxConnections;
    connOptions.nMaxOutbound =
        std::min(MAX_OUTBOUND_CONNECTIONS, connOptions.nMaxConnections);
    connOptions.nMaxAddnode = MAX_ADDNODE_CONNECTIONS;
    connOptions.nMaxFeeler = 1;
    connOptions.nBestHeight = chainActive.Height();
    connOptions.uiInterface = &uiInterface;
    connOptions.nSendBufferMaxSize = 
        gArgs.GetArgAsBytes("-maxsendbuffer", DEFAULT_MAXSENDBUFFER, ONE_KILOBYTE);
    connOptions.nReceiveFloodSize =
        gArgs.GetArgAsBytes("-maxreceivebuffer", DEFAULT_MAXRECEIVEBUFFER, ONE_KILOBYTE);

    connOptions.nMaxOutboundTimeframe = nMaxOutboundTimeframe;
    connOptions.nMaxOutboundLimit = nMaxOutboundLimit;

    if (!connman.Start(scheduler, strNodeError, connOptions)) {
        return InitError(strNodeError);
    }

    // Create mining factory
    assert(!mining::g_miningFactory);
    mining::g_miningFactory = std::make_unique<mining::CMiningFactory>(config);

    // Launch non-final mempool periodic checks
    mempool.getNonFinalPool().startPeriodicChecks(scheduler);

    // Step 12: finished

    SetRPCWarmupFinished();

    uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        pwallet->postInitProcess(scheduler);
    }
#endif

    return !shutdownToken.IsCanceled();
}
