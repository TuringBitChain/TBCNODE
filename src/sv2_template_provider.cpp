#include "sv2_template_provider.h"

#include <base58.h>
#include <iostream>
#include <thread>
#include <sstream>
#include <limits>
#include "util.h"

#include "net/netbase.h"
#include "util/check.h"
#include "fs.h"
#include "random.h"
#include "utilstrencodings.h"
#include "timedata.h"
using node::Sv2CoinbaseOutputDataSizeMsg;
using node::Sv2MsgType;
using node::Sv2SetupConnectionMsg;
using node::Sv2SetupConnectionErrorMsg;
using node::Sv2SetupConnectionSuccessMsg;
using node::Sv2NewTemplateMsg;
using node::Sv2SetNewPrevHashMsg;
using node::Sv2RequestTransactionDataMsg;
using node::Sv2RequestTransactionDataSuccessMsg;
using node::Sv2RequestTransactionDataErrorMsg;
using node::Sv2SubmitSolutionMsg;

// ── SV2 Dispatch Rate Limiter ("Capacitor") ──────────────────────────────────
// Initial block-dispatch delay injected after each new block on fast chains.
// Set SV2_CAP_INITIAL_INTERVAL to 0 to disable entirely (normal behaviour).
//   S  = SV2_CAP_INITIAL_INTERVAL  (seconds)
//   t  = SV2_CAP_CONVERGENCE_SECS  (seconds)
//   delta = S² / t  (per-release decrement; releases_to_converge = t / S)
// Example: S=10, t=300 → delta≈0.333s, converges in ~30 releases.
static constexpr double SV2_CAP_INITIAL_INTERVAL = 0.0;   // ← set S here
static constexpr double SV2_CAP_CONVERGENCE_SECS  = 0.0; // ← set t here
static constexpr double SV2_CAP_DELTA =
    (SV2_CAP_INITIAL_INTERVAL > 0.0 && SV2_CAP_CONVERGENCE_SECS > 0.0)
    ? (SV2_CAP_INITIAL_INTERVAL * SV2_CAP_INITIAL_INTERVAL) / SV2_CAP_CONVERGENCE_SECS
    : 0.0;
// ─────────────────────────────────────────────────────────────────────────────

/** Compute the actual release interval from the current capacitor decay value.
 *  @param capInterval  current decay interval (seconds), monotonically decreases
 *  @return sleep duration for the capacitor thread:
 *          - capInterval          when capInterval <= 3 min (direct use)
 *          - biased random in [1 min, 15 min] when capInterval > 3 min,
 *            with 75 % probability below capInterval and 25 % above it.
 */
static double CalcCapReleaseInterval(double capInterval)
{
    constexpr double CAP_MIN_SEC = 60.0;     // 1 minute
    constexpr double CAP_MAX_SEC = 900.0;    // 15 minutes
    constexpr double CAP_THRESHOLD = 180.0;  // 3 minutes

    if (capInterval <= CAP_THRESHOLD) {
        return capInterval;
    }

    double U = static_cast<double>(GetRand(std::numeric_limits<uint64_t>::max())) /
               static_cast<double>(std::numeric_limits<uint64_t>::max());

    if (0.75 > U) {
        // 75 %: uniform in [CAP_MIN_SEC, capInterval]
        double V = static_cast<double>(GetRand(std::numeric_limits<uint64_t>::max())) /
                   static_cast<double>(std::numeric_limits<uint64_t>::max());
        return CAP_MIN_SEC + (capInterval - CAP_MIN_SEC) * V;
    } else {
        // 25 %: uniform in [capInterval, CAP_MAX_SEC]
        double V = static_cast<double>(GetRand(std::numeric_limits<uint64_t>::max())) /
                   static_cast<double>(std::numeric_limits<uint64_t>::max());
        return capInterval + (CAP_MAX_SEC - capInterval) * V;
    }
}

Sv2TemplateProvider::Sv2TemplateProvider(Config &config, Mining& mining, CTxMemPool& mempool) 
    : m_config{config}, m_mining{mining}, m_mempool{mempool}
{
    // Read static key if cached
    try {
        AutoFile{fsbridge::fopen(GetStaticKeyFile(), "rb")} >> m_static_key;
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Reading cached static key from %s\n", bitcoinfs::PathToString(GetStaticKeyFile()));
    } catch (const std::ios_base::failure&) {
        // File is not expected to exist the first time.
        // In the unlikely event that loading an existing key fails, create a new one.
    }
    if (!m_static_key.IsValid()) {
        m_static_key = GenerateRandomKey();
        try {
            AutoFile{fsbridge::fopen(GetStaticKeyFile(), "wb")} << m_static_key;
        } catch (const std::ios_base::failure&) {
            LogPrint(BCLog::SV2,  "Error writing static key to %s\n", bitcoinfs::PathToString(GetStaticKeyFile()));
            // Continue, because this is not a critical failure.
        }
    }

    // Generate self signed certificate using (cached) authority key
    // TODO: skip loading authoritity key if -sv2cert is used

    // Load authority key if cached
    CKey authority_key;
    try {
        AutoFile{fsbridge::fopen(GetAuthorityKeyFile(), "rb")} >> authority_key;
    } catch (const std::ios_base::failure&) {
        // File is not expected to exist the first time.
        // In the unlikely event that loading an existing key fails, create a new one.
    }
    if (!authority_key.IsValid()) {
        authority_key = GenerateRandomKey();
        try {
            AutoFile{fsbridge::fopen(GetAuthorityKeyFile(), "wb")} << authority_key;
        } catch (const std::ios_base::failure&) {
            LogPrint(BCLog::SV2,  "Error writing authority key to %s\n", bitcoinfs::PathToString(GetAuthorityKeyFile()));
            // Continue, because this is not a critical failure.
        }
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Generated authority key, saved to %s\n", bitcoinfs::PathToString(GetAuthorityKeyFile()));
    }
    // SRI uses base58 encoded x-only pubkeys in its configuration files
    std::array<unsigned char, 34> version_pubkey_bytes;
    version_pubkey_bytes[0] = 1;
    version_pubkey_bytes[1] = 0;
    XOnlyPubKey authority_pub_key = XOnlyPubKey(authority_key.GetPubKey());
    std::copy(authority_pub_key.begin(), authority_pub_key.end(), version_pubkey_bytes.begin() + 2);
    LogPrint(BCLog::SV2, "SV2 authority public key (set as tp_authority_public_key in pool config): %s\n",
             EncodeBase58Check(std::vector<uint8_t>(version_pubkey_bytes.begin(), version_pubkey_bytes.end())));

    // Generate and sign certificate
    auto now{GetTime<std::chrono::seconds>()};
    uint16_t version = 0;
    // Start validity a little bit in the past to account for clock difference
    uint32_t valid_from = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count()) - 3600;
    uint32_t valid_to =  std::numeric_limits<unsigned int>::max(); // 2106
    m_certificate = Sv2SignatureNoiseMessage(version, valid_from, valid_to, XOnlyPubKey(m_static_key.GetPubKey()), authority_key);
    m_authority_pubkey = XOnlyPubKey(authority_key.GetPubKey());

    m_connman = std::make_unique<Sv2Connman>(TP_SUBPROTOCOL, m_static_key, m_authority_pubkey, m_certificate.value());
}


fs::path Sv2TemplateProvider::GetStaticKeyFile()
{
    return gArgs.GetDataDirNet() / "sv2_static_key";
}

fs::path Sv2TemplateProvider::GetAuthorityKeyFile()
{
    return gArgs.GetDataDirNet() / "sv2_authority_key";
}

bool Sv2TemplateProvider::Start(const Sv2TemplateProviderOptions& options)
{
    LogPrint(BCLog::SV2, "Sv2TemplateProvider starting on %s:%d\n", m_options.host, options.port);
    m_options = options;
    Init(options);

    if (!m_connman->Start(this, m_options.host, m_options.port, m_options.max_clients)) {
        return false;
    }

    m_thread_sv2_handler = std::thread(&TraceThread<std::function<void()>>
        , "sv2"
        , std::function<void()>(std::bind(&Sv2TemplateProvider::ThreadSv2Handler, this)));

    if (SV2_CAP_INITIAL_INTERVAL > 0.0) {
        m_thread_sv2_capacitor = std::thread(&TraceThread<std::function<void()>>
            , "sv2cap"
            , std::function<void()>(std::bind(&Sv2TemplateProvider::ThreadSv2CapacitorHandler, this)));
    }

    // C2: background builder pre-warms m_cached_workset so handler / capacitor
    // can serve from cache instead of synchronously calling createNewBlock.
    m_builder_thread = std::thread(&TraceThread<std::function<void()>>
        , "sv2builder"
        , std::function<void()>(std::bind(&Sv2TemplateProvider::ThreadBuilder, this)));
    RequestRebuild();   // kick an initial build so first client connect can serve from cache

    return true;
}

void Sv2TemplateProvider::Init(const Sv2TemplateProviderOptions& options)
{
    m_minimum_fee_delta = gArgs.GetArg("-sv2feedelta", DEFAULT_SV2_FEE_DELTA);
    m_port = options.port;
    m_protocol_version = options.protocol_version;
    m_optional_features = options.optional_features;
    m_default_coinbase_tx_additional_output_size = options.default_coinbase_tx_additional_output_size;
    m_default_future_templates = options.default_future_templates;
    m_cap_current_interval = SV2_CAP_INITIAL_INTERVAL;
    if (SV2_CAP_INITIAL_INTERVAL > 0.0) {
        LogPrint(BCLog::SV2, "Capacitor enabled: initial_interval=%.3fs convergence_secs=%.1f delta=%.6fs\n",
            SV2_CAP_INITIAL_INTERVAL, SV2_CAP_CONVERGENCE_SECS, SV2_CAP_DELTA);
    }

    // Size the block template cache against max_clients. Each connected
    // client may keep up to ~CACHE_SLOTS_PER_CLIENT generations of templates
    // alive simultaneously (initial NewTemplate + periodic fee-update sends
    // + capacitor discharges, before the miner moves off the old id).
    // Without scaling, max_clients=1024 would burn the 96-entry default in
    // a single tip-change fan-out and start dropping live work.
    m_max_block_template_cache_size = std::max<size_t>(
        MIN_BLOCK_TEMPLATE_CACHE_SIZE,
        options.max_clients * CACHE_SLOTS_PER_CLIENT);
    LogPrint(BCLog::SV2, "Block template cache cap=%zu (max_clients=%zu × %zu, floor=%zu)\n",
        m_max_block_template_cache_size, options.max_clients,
        CACHE_SLOTS_PER_CLIENT, MIN_BLOCK_TEMPLATE_CACHE_SIZE);
}

Sv2TemplateProvider::~Sv2TemplateProvider()
{
    AssertLockNotHeld(m_clients_mutex);
    AssertLockNotHeld(m_tp_mutex);

    {
        LOCKMt(m_clients_mutex);
        for (const auto& client : m_sv2_clients) {
            //LogTrace(BCLog::SV2, "Disconnecting client id=%zu\n",
                    //client->m_id);
            client->m_disconnect_flag = true;
        }
        DisconnectFlagged();
    }
    m_connman->Interrupt();
    m_connman->StopThreads();

    Interrupt();
    StopThreads();
}

void Sv2TemplateProvider::Interrupt()
{
    LogPrint(BCLog::SV2, "Sv2TemplateProvider interrupted\n");
    m_flag_interrupt_sv2 = true;
    m_cap_cv.notify_all();      // wake capacitor thread so it exits cleanly
    m_builder_cv.notify_all();  // C2: wake builder thread
}

void Sv2TemplateProvider::StopThreads()
{
    if (m_thread_sv2_handler.joinable()) {
        m_thread_sv2_handler.join();
    }
    if (m_thread_sv2_mempool_handler.joinable()) {
        m_thread_sv2_mempool_handler.join();
    }
    if (m_thread_sv2_capacitor.joinable()) {
        m_thread_sv2_capacitor.join();
    }
    if (m_builder_thread.joinable()) {
        m_builder_thread.join();   // C2
    }
}


class Timer {
private:
    std::chrono::seconds m_interval;
    std::chrono::seconds m_last_triggered;

public:
    Timer() {
        m_interval = std::chrono::seconds(gArgs.GetArg("-sv2interval", DEFAULT_SV2_INTERVAL));
        // Initialize the timer to a time point far in the past
        m_last_triggered = GetTime<std::chrono::seconds>() - std::chrono::hours(1);
    }

    Timer(std::chrono::seconds interval) : m_interval(interval) {
        reset();
    }

    bool trigger() {
        auto now{GetTime<std::chrono::seconds>()};
        if (now - m_last_triggered >= m_interval) {
            m_last_triggered = now;
            return true;
        }
        return false;
    }
    void reset() {
        auto now{GetTime<std::chrono::seconds>()};
        m_last_triggered = now;
    }
};

void Sv2TemplateProvider::DisconnectFlagged()
{
    AssertLockHeld(m_clients_mutex);

    // Remove clients that are flagged for disconnection.
    m_sv2_clients.erase(
        std::remove_if(m_sv2_clients.begin(), m_sv2_clients.end(), [](const auto &client) {
            return client->m_disconnect_flag;
    }), m_sv2_clients.end());
}

void Sv2TemplateProvider::ThreadSv2Handler()
{
    while (!m_flag_interrupt_sv2) {
        // Note: initial work is sent in CoinbaseOutputDataSize() callback,
        // so no duplicate initial-work dispatch is needed here.

        auto current_tip = m_mining.getTip();
        if (!current_tip) {
            // Node not yet ready (IBD or startup); wait and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        auto timeout = std::chrono::duration_cast<Mining::MillisecondsDouble>(
            std::chrono::milliseconds(m_options.fee_check_interval));
        auto tip = m_mining.waitTipChanged(current_tip->hash, timeout);
        if (!tip) break; // Node shutting down

        bool best_block_changed = false;
        bool signal_cap = false;
        {
            LOCKMt(m_tp_mutex);
            best_block_changed = (m_best_prev_hash != tip->hash);
            if (best_block_changed) {
                m_best_prev_hash = tip->hash;
                m_last_block_time = GetTime<std::chrono::seconds>();
                m_template_last_update = GetTime<std::chrono::seconds>();
                m_cached_workset.reset();   // C1: tip moved, old template stale
                if (m_cap_current_interval > 0.0 && !m_cap_pending) {
                    m_cap_pending = true;
                    signal_cap = true;  // first arm: need to wake capacitor thread
                }
            }
        }

        // C2: tip moved -> ask builder to refresh immediately (outside m_tp_mutex)
        if (best_block_changed) {
            RequestRebuild();
        }

        m_connman->ForEachClient([this, best_block_changed](Sv2Client& client) {
            if (!client.m_coinbase_output_data_size_recv) {
                return;
            }

            LOCKMt(this->m_tp_mutex);
            if (m_cap_pending) {
                if (best_block_changed) {
                    LogPrint(BCLog::SV2, "Capacitor: mark client id=%zu for discharge\n", client.m_id);
                    client.m_cap_needs_discharge = true;
                }
                return;
            }
            Amount dummy_last_fees;
            if (!SendWork(client, /*send_new_prevhash=*/best_block_changed, dummy_last_fees)) {
                LogPrint(BCLog::SV2, "Disconnecting client id=%zu, reason: failed to send work on tip change\n",
                                client.m_id);
                client.m_disconnect_flag = true;
            }
        });

        // Signal capacitor thread after clients have been marked for discharge
        if (signal_cap) {
            {
                std::lock_guard<std::mutex> lk(m_cap_mutex);
                m_cap_signaled = true;
            }
            m_cap_cv.notify_one();
        }

        LOCKMt(m_tp_mutex);
        PruneBlockTemplateCache();
    }
}

void Sv2TemplateProvider::ThreadSv2MempoolHandler()
{
    Timer timer(m_options.fee_check_interval);

    //! Fees for the previous fee_check_interval
    Amount fees_previous_interval{0};
    //! Fees as of the last waitFeesChanged() call
    Amount last_fees{0};

    auto getTipHash = [this]() -> std::optional<uint256>
    {
        auto tip = m_mining.getTip();
        if (!tip) return std::nullopt;
        return tip->hash;
    };

    auto waitFeesChanged = [&](Mining::MillisecondsDouble timeout, uint256 tip, Amount fee_delta, Amount& fees_before, bool& tip_changed){
        Assume(getTipHash());
        unsigned int last_mempool_update{m_mempool.GetTransactionsUpdated()};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::min(timeout, Mining::MillisecondsDouble(100)));
                auto current = getTipHash();
                if (!current || current.value() != tip) {
                    tip_changed = true;
                    return false;
                }

                // TODO: when cluster mempool is available, actually calculate
                // fees for the next block. This is currently too expensive.
                if (m_mempool.GetTransactionsUpdated() > last_mempool_update) return true;
            }
        }
        return false;
    };

    while (!m_flag_interrupt_sv2) {
        auto timeout{std::min(std::chrono::milliseconds(100), std::chrono::milliseconds(m_options.fee_check_interval))};
        last_fees = fees_previous_interval;
        bool tip_changed{false};
        if (waitFeesChanged(timeout, WITH_LOCK(m_tp_mutex, return m_best_prev_hash;), m_options.fee_delta, last_fees, tip_changed)) {
            if (tip_changed) {
                timer.reset();
                fees_previous_interval = 0;
                last_fees = 0;
            }
            continue;
        }

        // Do not send new templates more frequently than the fee check interval
        if (!timer.trigger()) continue;

        // If we never created a template, continue
        if (m_template_last_update == std::chrono::milliseconds(0)) continue;

        // TODO ensure all connected clients have had work queued up for the latest prevhash.

        // This doesn't have any effect, but it will once waitFeesChanged() updates the last_fees value.
        fees_previous_interval = last_fees;

        m_connman->ForEachClient([this, last_fees, &fees_previous_interval](Sv2Client& client) {
            if (!client.m_coinbase_output_data_size_recv) {
                return;
            }

            LOCKMt(this->m_tp_mutex);
            if (m_cap_pending) {
                LogPrint(BCLog::SV2, "Capacitor: discard fee-update for client id=%zu\n", client.m_id);
                return;
            }
            // fees_previous_interval is only updated if the fee increase was sufficient,
            // since waitFeesChanged doesn't actually check this yet.

            Amount fees_before = last_fees;
            if (!SendWork(client, /*send_new_prevhash=*/false, fees_before)) {
                LogPrint(BCLog::SV2, "Disconnecting client id=%zu, reason: failed to send work on fee update\n",
                                client.m_id);
                client.m_disconnect_flag = true;
            }

            // We don't track fees_before for individual connected clients. Pick the
            // highest value amongst all connected clients (which may vary in additional_coinbase_weight).
            if (fees_before > fees_previous_interval) fees_previous_interval = fees_before;
        });
    }
}

bool Sv2TemplateProvider::BuildNewWorkSetWithId(bool future_template,
                                                 uint64_t template_id,
                                                 NewWorkSet& newWorkSet)
{
    // No m_tp_mutex required — only touches the mining interface and a
    // throwaway BlockTemplate. createNewBlock takes its own cs_main / mempool
    // locks internally.
    CScript scriptDummy = CScript() << OP_TRUE;
    std::shared_ptr<BlockTemplate> pblocktemplate{m_mining.createNewBlock(scriptDummy)};
    if (!pblocktemplate) {
        LogPrint(BCLog::SV2, "BuildNewWorkSetWithId: createNewBlock returned null\n");
        return false;
    }
    pblocktemplate->getBlockRef()->nNonce = 0;
    LogPrint(BCLog::SV2, "BuildNewWorkSet: nTime=%u nBits=%08x\n",
        pblocktemplate->getBlockRef()->nTime, pblocktemplate->getBlockRef()->nBits);

    Sv2NewTemplateMsg new_template{*pblocktemplate, template_id, future_template};
    Sv2SetNewPrevHashMsg set_new_prev_hash{*pblocktemplate, template_id};
    newWorkSet = {new_template, pblocktemplate, set_new_prev_hash};
    return true;
}

bool Sv2TemplateProvider::BuildNewWorkSet(bool future_template, unsigned int coinbase_output_max_additional_size, NewWorkSet &newWorkSet)
{
    AssertLockHeld(m_tp_mutex);
    return BuildNewWorkSetWithId(future_template, m_template_id, newWorkSet);
}

void Sv2TemplateProvider::PruneBlockTemplateCache()
{
    AssertLockHeld(m_tp_mutex);

    // Allow a few seconds for clients to submit a block
    auto recent = GetTime<std::chrono::seconds>() - std::chrono::seconds(10);
    if (m_last_block_time > recent) return;
    // If the blocks prevout is not the tip's prevout, delete it.
    uint256 prev_hash = m_best_prev_hash;

    size_t pruned = 0;
    for (auto it = m_block_template_cache.begin(); it != m_block_template_cache.end(); ) {
        if (it->second->getBlockRef()->hashPrevBlock != prev_hash) {
            it = m_block_template_cache.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    if (pruned > 0) {
        LogPrint(BCLog::SV2, "PruneBlockTemplateCache: removed %zu stale entries, %zu remaining\n",
            pruned, m_block_template_cache.size());
    }
}

bool Sv2TemplateProvider::SendWork(Sv2Client& client, bool send_new_prevhash, Amount& fees_before)
{
    AssertLockHeld(m_tp_mutex);

    // The current implementation doesn't create templates for future empty
    // or speculative blocks. Despite that, we first send NewTemplate with
    // future_template set to true, followed by SetNewPrevHash. We do this
    // both when first connecting and when a new block is found.
    //
    // When the template is update to take newer mempool transactions into
    // account, we set future_template to false and don't send SetNewPrevHash.

    // TODO: reuse template_id for clients with the same m_default_coinbase_tx_additional_output_size
    ++m_template_id;
    NewWorkSet new_work_set;

    // ── C1: template reuse cache ───────────────────────────────────────────
    // For fee-update style sends (send_new_prevhash=false) we can serve the
    // previously built NewWorkSet as long as:
    //   - it was built recently (CACHE_TTL)
    //   - mempool hasn't churned past CACHE_MEMPOOL_DELTA txs
    //   - chain tip hasn't changed
    // The block template (shared_ptr) is reused verbatim; we bump m_template_id
    // so SubmitSolution lookups remain monotonic.
    bool cache_hit = false;
    if (!send_new_prevhash && m_cached_workset.has_value()) {
        const auto now = std::chrono::steady_clock::now();
        const unsigned int cur_seq = m_mempool.GetTransactionsUpdated();
        const auto ttl = m_options.fee_check_interval * CACHE_TTL_MULTIPLIER;
        const bool ttl_ok   = (now - m_cache_built_at) < ttl;
        const bool delta_ok = (cur_seq - m_cache_mempool_seq) < CACHE_MEMPOOL_DELTA;
        const bool prev_ok  = (m_cached_workset->block_template->getBlockRef()->hashPrevBlock
                               == m_best_prev_hash);
        if (ttl_ok && delta_ok && prev_ok) {
            new_work_set = *m_cached_workset;
            new_work_set.new_template.m_template_id = m_template_id;

            // "Selection time" on cache hit: only updates the local
            // prev_hash copy. We deliberately do NOT mutate
            // block_template->getBlockRef()->nTime — that BlockTemplate is
            // shared via shared_ptr with m_block_template_cache, and
            // SubmitSolution writes to block->nTime under m_submit_mutex
            // (after m_tp_mutex is released). Writing nTime here would race
            // with that path and could overwrite the miner's submitted
            // timestamp, causing PoW hash mismatch and a dropped solution.
            //
            // In this branch send_new_prevhash=false, so prev_hash is not
            // emitted on the wire anyway; the local update keeps the field
            // coherent in case this path ever extends to send_new_prevhash.
            const uint32_t now_sec = static_cast<uint32_t>(GetAdjustedTime());
            const uint32_t built_ntime = new_work_set.block_template->getBlockRef()->nTime;
            if (now_sec > built_ntime) {
                new_work_set.prev_hash.m_header_timestamp = now_sec;
            }

            cache_hit = true;
            LogPrint(BCLog::SV2,
                "SendWork: cache hit (age=%llds, dseq=%u, txs=%zu, nTime built=%u sel=%u) -> reuse as id=%lu\n",
                std::chrono::duration_cast<std::chrono::seconds>(now - m_cache_built_at).count(),
                cur_seq - m_cache_mempool_seq,
                new_work_set.block_template->getBlockRef()->vtx.size() - 1,
                built_ntime, now_sec,
                m_template_id);
        }
    }
    if (!cache_hit) {
        if (!BuildNewWorkSet(/*future_template=*/send_new_prevhash, client.m_coinbase_tx_outputs_size, new_work_set)) {
            return false;
        }
        m_cached_workset    = new_work_set;
        m_cache_built_at    = std::chrono::steady_clock::now();
        m_cache_mempool_seq = m_mempool.GetTransactionsUpdated();
    }
    // ───────────────────────────────────────────────────────────────────────

    if (m_best_prev_hash == uint256{}) {
        // g_best_block is set UpdateTip(), so will be 0 when the node starts
        // and no new blocks have arrived.
        m_best_prev_hash = new_work_set.block_template->getBlockRef()->hashPrevBlock;
    }

    // Do not submit new template if the fee increase is insufficient:
    Amount fees;
    for (Amount fee : new_work_set.block_template->getTxFees()) {
        // Skip coinbase
        if (fee < Amount{}) continue;
        fees += fee;
    }
    if (!send_new_prevhash && fees_before != Amount() && fees_before + Amount(m_minimum_fee_delta) > fees) return true;

    // Human-readable template summary (mirrors getblocktemplate key fields)
    {
        auto* block = new_work_set.block_template->getBlockRef().get();
        arith_uint256 bnTarget;
        bnTarget.SetCompact(block->nBits);
        auto tip = m_mining.getTip();
        int height = tip ? tip->height + 1 : -1;
        int64_t coinbase_sat = block->vtx[0]->vout[0].nValue.GetSatoshis();
        int tx_count = static_cast<int>(block->vtx.size()) - 1;
        std::string reason = send_new_prevhash ? "new block" : "fee update";
        LogPrint(BCLog::SV2, "NewTemplate id=%lu [%s]  height=%d  time=%u  bits=%08x"
            "  target=%s  coinbase=%lld sat  txs=%d"
            "  prevhash=%s  client=%zu\n",
            m_template_id, reason, height,
            block->nTime, block->nBits, bnTarget.GetHex(),
            coinbase_sat, tx_count,
            HexStr(bsv::span(block->hashPrevBlock)),
            client.m_id);
    }

    LogPrint(BCLog::SV2, "Send 0x71 NewTemplate id=%lu to client id=%zu\n", m_template_id, client.m_id);
    client.m_send_messages.emplace_back(new_work_set.new_template);

    if (send_new_prevhash) {
        LogPrint(BCLog::SV2, "Send 0x72 SetNewPrevHash prevhash=%s to client id=%zu\n",
            HexStr(bsv::span(new_work_set.prev_hash.m_prev_hash)), client.m_id);
        client.m_send_messages.emplace_back(new_work_set.prev_hash);
    }

    // Drop stale-prevhash entries first (cheap, prevhash-keyed). Then, only
    // if the current-tip set itself exceeds the cap, fall back to evicting
    // the lowest template_id. This minimizes the risk of evicting a
    // template a miner is still working on.
    PruneBlockTemplateCache();
    if (m_block_template_cache.size() >= m_max_block_template_cache_size) {
        m_block_template_cache.erase(m_block_template_cache.begin());
        LogPrint(BCLog::SV2, "Block template cache full (cap=%zu), evicted oldest entry\n",
            m_max_block_template_cache_size);
    }
    // Deep-copy per template_id so SubmitSolution's in-place mutation of
    // vtx[0]/nTime/hashMerkleRoot on one id can't corrupt the state another
    // id is still pointed at. m_cached_workset also keeps its own copy
    // because new_work_set.block_template is now a clone, not the shared
    // pointer from the cache.
    m_block_template_cache.insert({m_template_id, new_work_set.block_template->clone()});

    return true;
}

void Sv2TemplateProvider::ThreadSv2CapacitorHandler()
{
    while (!m_flag_interrupt_sv2) {
        // Wait until the capacitor is armed by a new block (first arm only)
        {
            std::unique_lock<std::mutex> lock(m_cap_mutex);
            m_cap_cv.wait(lock, [this]{ return m_cap_signaled || m_flag_interrupt_sv2.load(); });
            if (m_flag_interrupt_sv2) break;
            m_cap_signaled = false;
        }

        // Read the current interval (m_cap_mutex released above, safe to acquire m_tp_mutex)
        double interval;
        {
            LOCKMt(m_tp_mutex);
            interval = CalcCapReleaseInterval(m_cap_current_interval);
        }
        if (interval <= 0.0) {
            // Defensive: decay already at zero but thread is still running.
            // Clear pending state and exit cleanly to avoid hanging SV2 dispatch.
            LOCKMt(m_tp_mutex);
            m_cap_pending = false;
            LogPrint(BCLog::SV2, "Capacitor: interval=%.3fs, exiting handler\n", interval);
            break;
        }

        // Sleep for the interval; wake early only on interrupt
        {
            std::unique_lock<std::mutex> lock(m_cap_mutex);
            m_cap_cv.wait_for(lock,
                std::chrono::duration<double>(interval),
                [this]{ return m_flag_interrupt_sv2.load(); });
        }
        if (m_flag_interrupt_sv2) break;

        // Discharge: get ONE shared workset (cache-hit if possible, else a
        // single sync build OUTSIDE m_tp_mutex) and fan it out to all clients
        // that still need discharge. The previous per-client BuildNewWorkSet
        // inside ForEachClient (a) duplicated builder's work — logs showed
        // sv2cap producing byte-identical templates seconds after sv2builder —
        // and (b) held m_tp_mutex through createNewBlock, which the builder
        // thread was explicitly designed to avoid.
        LogPrint(BCLog::SV2, "Capacitor: interval=%.3fs elapsed, preparing discharge workset\n", interval);

        NewWorkSet shared_work_set;
        bool have_workset = false;
        bool cache_hit    = false;

        // Step 1: try the C1 cache (same gates as SendWork).
        {
            LOCKMt(m_tp_mutex);
            if (m_cached_workset.has_value()) {
                const auto now = std::chrono::steady_clock::now();
                const unsigned int cur_seq = m_mempool.GetTransactionsUpdated();
                const auto ttl = m_options.fee_check_interval * CACHE_TTL_MULTIPLIER;
                const bool ttl_ok   = (now - m_cache_built_at) < ttl;
                const bool delta_ok = (cur_seq - m_cache_mempool_seq) < CACHE_MEMPOOL_DELTA;
                const bool prev_ok  = (m_cached_workset->block_template->getBlockRef()->hashPrevBlock
                                       == m_best_prev_hash);
                if (ttl_ok && delta_ok && prev_ok) {
                    shared_work_set = *m_cached_workset;
                    // Only update the local prev_hash copy with selection
                    // time. Do NOT write block_template->getBlockRef()->nTime:
                    // that BlockTemplate is shared with m_block_template_cache
                    // and SubmitSolution mutates block->nTime under
                    // m_submit_mutex (with m_tp_mutex released). A write here
                    // would race with that path and could corrupt the
                    // miner-submitted timestamp.
                    //
                    // The prev_hash field IS sent on the wire by capacitor
                    // discharge (send_new_prevhash semantics), so updating
                    // m_header_timestamp here is what actually delivers the
                    // selection-time hint to the miner.
                    const uint32_t now_sec = static_cast<uint32_t>(GetAdjustedTime());
                    const uint32_t built_ntime = shared_work_set.block_template->getBlockRef()->nTime;
                    if (now_sec > built_ntime) {
                        shared_work_set.prev_hash.m_header_timestamp = now_sec;
                    }
                    have_workset = true;
                    cache_hit    = true;
                    LogPrint(BCLog::SV2,
                        "Capacitor: cache hit (age=%llds, dseq=%u, nTime built=%u sel=%u) -> reuse for discharge\n",
                        std::chrono::duration_cast<std::chrono::seconds>(now - m_cache_built_at).count(),
                        cur_seq - m_cache_mempool_seq,
                        built_ntime, now_sec);
                }
            }
        }

        // Step 2: cache miss → one sync build OUTSIDE m_tp_mutex.
        if (!have_workset) {
            LogPrint(BCLog::SV2, "Capacitor: cache miss, building fresh template\n");
            // template_id is a placeholder; per-client id is assigned below.
            if (BuildNewWorkSetWithId(/*future_template=*/true, /*template_id=*/0, shared_work_set)) {
                LOCKMt(m_tp_mutex);
                // Guard against tip moving during the unlocked build.
                if (m_best_prev_hash != uint256{} &&
                    shared_work_set.block_template->getBlockRef()->hashPrevBlock != m_best_prev_hash) {
                    LogPrint(BCLog::SV2, "Capacitor: tip moved during build, discard discharge\n");
                } else {
                    m_cached_workset    = shared_work_set;
                    m_cache_built_at    = std::chrono::steady_clock::now();
                    m_cache_mempool_seq = m_mempool.GetTransactionsUpdated();
                    have_workset = true;
                }
            } else {
                LogPrint(BCLog::SV2, "Capacitor: BuildNewWorkSetWithId failed, skip discharge\n");
            }
        }

        // Step 3: dispatch the shared workset to each client that needs it.
        // Each client gets its own m_template_id (and a per-id entry in
        // m_block_template_cache for SubmitSolution lookups), but the
        // underlying BlockTemplate is shared via shared_ptr — submitSolution
        // overrides nTime/nonce/coinbase per submission, so the share is safe.
        if (have_workset) {
            m_connman->ForEachClient([this, &shared_work_set, cache_hit](Sv2Client& client) {
                if (!client.m_coinbase_output_data_size_recv) return;
                LOCKMt(this->m_tp_mutex);

                if (!client.m_cap_needs_discharge) return;
                client.m_cap_needs_discharge = false;

                ++m_template_id;
                NewWorkSet client_work_set = shared_work_set;  // copies shared_ptr (refcount)
                client_work_set.new_template.m_template_id = m_template_id;
                client_work_set.prev_hash.m_template_id    = m_template_id;

                PruneBlockTemplateCache();
                if (m_block_template_cache.size() >= m_max_block_template_cache_size) {
                    m_block_template_cache.erase(m_block_template_cache.begin());
                }
                // Per-id deep copy — see SendWork insert site for rationale.
                // Each client's template_id owns an isolated BlockTemplate,
                // so SubmitSolution can mutate without affecting sibling ids
                // built off the same shared_work_set.
                m_block_template_cache.insert({m_template_id, client_work_set.block_template->clone()});

                // Human-readable discharge summary — mirrors SendWork's
                // NewTemplate log so the per-id audit trail (time / txs /
                // prevhash) is uniform regardless of dispatch path. Lets you
                // grep `template_id=N` and recover the assembled block state
                // we believed at send time, to cross-check against the
                // miner's later SubmitSolution timestamp/coinbase.
                {
                    auto* block = client_work_set.block_template->getBlockRef().get();
                    int tx_count = static_cast<int>(block->vtx.size()) - 1;
                    int64_t coinbase_sat = block->vtx[0]->vout[0].nValue.GetSatoshis();
                    LogPrint(BCLog::SV2,
                        "Capacitor: discharge 0x71 template_id=%lu [%s] time=%u sel=%u bits=%08x"
                        "  coinbase=%lld sat  txs=%d"
                        "  prevhash=%s  client=%zu\n",
                        m_template_id, cache_hit ? "cache-hit" : "fresh-build",
                        block->nTime, client_work_set.prev_hash.m_header_timestamp,
                        block->nBits, coinbase_sat, tx_count,
                        HexStr(bsv::span(block->hashPrevBlock)),
                        client.m_id);
                }
                client.m_send_messages.emplace_back(client_work_set.new_template);

                LogPrint(BCLog::SV2, "Capacitor: discharge 0x72 template_id=%lu to client id=%zu\n",
                    m_template_id, client.m_id);
                client.m_send_messages.emplace_back(client_work_set.prev_hash);
            });
        }

        // Advance convergence: reduce interval by delta, then clear pending flag
        bool converged = false;
        {
            LOCKMt(m_tp_mutex);
            m_cap_current_interval -= SV2_CAP_DELTA;
            if (m_cap_current_interval < 0.0) m_cap_current_interval = 0.0;
            m_cap_pending = false;
            converged = (m_cap_current_interval == 0.0);
            LogPrint(BCLog::SV2, "Capacitor: discharged, next_interval=%.3fs\n", m_cap_current_interval);
        }
        if (converged) {
            LogPrint(BCLog::SV2, "Capacitor: fully converged, thread exiting\n");
            break;
        }
    }
}

void Sv2TemplateProvider::RequestRebuild()
{
    {
        std::lock_guard<std::mutex> lk(m_builder_mutex);
        m_builder_request_pending = true;
    }
    m_builder_cv.notify_one();
}

void Sv2TemplateProvider::ThreadBuilder()
{
    LogPrint(BCLog::SV2, "Builder thread start (tick=%llds)\n",
        static_cast<long long>(m_options.fee_check_interval.count()));
    while (!m_flag_interrupt_sv2) {
        // Wait for explicit request or one fee_check_interval tick.
        {
            std::unique_lock<std::mutex> lk(m_builder_mutex);
            m_builder_cv.wait_for(lk, m_options.fee_check_interval, [this]{
                return m_builder_request_pending || m_flag_interrupt_sv2.load();
            });
            if (m_flag_interrupt_sv2) break;
            m_builder_request_pending = false;
        }

        // No SV2 client attached -> the cache would never be consumed.
        // Skip the entire build and let the next tick / RequestRebuild
        // retry. This keeps the node idle (zero createNewBlock, zero
        // cs_main contention with RPC) when no pool is connected.
        if (!m_connman->HasActiveClient()) {
            continue;
        }

        // Decide whether a rebuild is warranted; grab a placeholder id.
        // Three independent triggers:
        //   - cold start: nothing in cache yet
        //   - delta:      mempool churn since last build crossed CACHE_MEMPOOL_DELTA
        //                 (same threshold SendWork uses for its sync-rebuild
        //                  fallback, so the two checks agree on "fresh enough")
        //   - age:        cache is about to age out from SendWork's POV
        //                 (TTL = fee_check_interval × CACHE_TTL_MULTIPLIER).
        //                 Refreshing here keeps the next SendWork on the cache
        //                 hit path instead of falling back to sync rebuild on
        //                 the sv2 handler thread.
        bool need_rebuild = false;
        uint64_t placeholder_id = 0;
        unsigned int cur_seq = 0;
        {
            LOCKMt(m_tp_mutex);
            cur_seq = m_mempool.GetTransactionsUpdated();
            const auto now = std::chrono::steady_clock::now();
            if (!m_cached_workset.has_value()) {
                need_rebuild = true;                       // cold start
            } else if ((cur_seq - m_cache_mempool_seq) >= CACHE_MEMPOOL_DELTA) {
                need_rebuild = true;                       // mempool churn crossed threshold
            } else if ((now - m_cache_built_at) > m_options.fee_check_interval) {
                need_rebuild = true;                       // cache about to age out
            }
            placeholder_id = m_template_id + 1;            // non-binding tag; SendWork overrides
        }
        if (!need_rebuild) continue;

        // Heavy build OUTSIDE m_tp_mutex. createNewBlock still grabs cs_main /
        // mempool internally — that contention with RPC remains, but SV2
        // handler / capacitor threads stay responsive.
        NewWorkSet new_work_set;
        if (!BuildNewWorkSetWithId(/*future_template=*/true, placeholder_id, new_work_set)) {
            continue;
        }

        // Commit to cache.
        {
            LOCKMt(m_tp_mutex);
            // Guard against tip having moved while we were building outside the
            // lock: if our build is already stale (prevhash mismatch), drop it.
            if (m_best_prev_hash != uint256{} &&
                new_work_set.block_template->getBlockRef()->hashPrevBlock != m_best_prev_hash) {
                LogPrint(BCLog::SV2, "Builder: tip moved during build, discard\n");
                continue;
            }
            m_cached_workset    = std::move(new_work_set);
            m_cache_built_at    = std::chrono::steady_clock::now();
            m_cache_mempool_seq = m_mempool.GetTransactionsUpdated();
            LogPrint(BCLog::SV2, "Builder: cache refreshed (placeholder_id=%lu)\n", placeholder_id);
        }
    }
    LogPrint(BCLog::SV2, "Builder thread exit\n");
}

void Sv2TemplateProvider::RequestTransactionData(Sv2Client& client, node::Sv2RequestTransactionDataMsg msg)
{
    LOCKMt(m_tp_mutex);
    auto cached_block = m_block_template_cache.find(msg.m_template_id);
    if (cached_block != m_block_template_cache.end()) {
        auto block = (*cached_block->second).getBlockRef();

        if (block->hashPrevBlock != m_best_prev_hash) {
            LogPrint(BCLog::SV2, "RequestTransactionData: stale template id=%lu prevhash=%s tip=%s client=%zu\n",
                msg.m_template_id, HexStr(bsv::span(block->hashPrevBlock)),
                HexStr(bsv::span(m_best_prev_hash)), client.m_id);
            node::Sv2RequestTransactionDataErrorMsg request_tx_data_error{msg.m_template_id, "stale-template-id"};
            client.m_send_messages.emplace_back(request_tx_data_error);
            return;
        }

        std::vector<CTransactionRef> txs;
        if (block->vtx.size() > 0) {
            std::copy(block->vtx.begin() + 1, block->vtx.end(), std::back_inserter(txs));
        }

        size_t tx_count = txs.size();
        node::Sv2RequestTransactionDataSuccessMsg request_tx_data_success{msg.m_template_id, /*std::move(witness_reserve_value),*/ std::move(txs)};
        LogPrint(BCLog::SV2, "Send 0x74 RequestTransactionData.Success id=%lu txs=%zu client=%zu\n",
            msg.m_template_id, tx_count, client.m_id);
        client.m_send_messages.emplace_back(request_tx_data_success);
    } else {
        LogPrint(BCLog::SV2, "RequestTransactionData: template id=%lu not found, client=%zu\n",
            msg.m_template_id, client.m_id);
        node::Sv2RequestTransactionDataErrorMsg request_tx_data_error{msg.m_template_id, "template-id-not-found"};
        client.m_send_messages.emplace_back(request_tx_data_error);
    }
}

void Sv2TemplateProvider::SubmitSolution(node::Sv2SubmitSolutionMsg solution)
{
        LogPrint(BCLog::SV2, "SubmitSolution id=%lu version=%d timestamp=%d nonce=%08x\n",
            solution.m_template_id, solution.m_version,
            solution.m_header_timestamp, solution.m_header_nonce);

        auto cb = MakeTransactionRef(std::move(solution.m_coinbase_tx));

        // Use shared_ptr so the template stays alive after releasing m_tp_mutex.
        // We can't hold the lock through submitSolution() because a concurrent
        // new block via p2p would deadlock on g_best_block_mutex.
        std::shared_ptr<BlockTemplate> block_template;
        {
            LOCKMt(m_tp_mutex);
            auto cached_block_template = m_block_template_cache.find(solution.m_template_id);
            if (cached_block_template == m_block_template_cache.end()) {
                LogPrintf("SV2 SubmitSolution: template id=%lu not in cache, solution dropped\n", solution.m_template_id);
                return;
            }
            block_template = cached_block_template->second; // shared ownership — safe after lock release

            if (block_template->getBlockRef()->hashPrevBlock != m_best_prev_hash) {
                LogPrintf("SV2 SubmitSolution: stale template id=%lu prevhash=%s sv2_tip=%s, solution dropped\n",
                    solution.m_template_id,
                    block_template->getBlockRef()->hashPrevBlock.ToString(),
                    m_best_prev_hash.ToString());
                return;
            }

            // Audit trail for miner-side nTime rolling. tpl_ntime is what the
            // assembler set when this template was built (or, for cache-hit
            // dispatches, the original build's time — the cached value is the
            // same shared BlockTemplate). rolled is the miner's chosen
            // timestamp minus that. Cross-reference vs the corresponding
            // "NewTemplate id=N" / "Capacitor: discharge ... id=N" log entry
            // and the prev-tip's UpdateTip date to see whether the miner
            // rolled forward, backward, or beyond the SetNewPrevHash window.
            int tpl_txs = static_cast<int>(block_template->getBlockRef()->vtx.size()) - 1;
            uint32_t tpl_ntime = block_template->getBlockRef()->nTime;
            int64_t rolled = static_cast<int64_t>(solution.m_header_timestamp) - static_cast<int64_t>(tpl_ntime);
            LogPrint(BCLog::SV2,
                "SubmitSolution audit id=%lu tpl_ntime=%u submit_ntime=%u rolled=%+lld s  txs=%d  prevhash=%s\n",
                solution.m_template_id, tpl_ntime, solution.m_header_timestamp, (long long)rolled,
                tpl_txs, block_template->getBlockRef()->hashPrevBlock.ToString());
        }

        LOCKMt(m_submit_mutex);

        auto current_tip = m_mining.getTip();
        if (!current_tip || block_template->getBlockRef()->hashPrevBlock != current_tip->hash) {
            LogPrintf("SV2 SubmitSolution: stale template id=%lu prevhash=%s chain_tip=%s, solution dropped\n",
                solution.m_template_id,
                block_template->getBlockRef()->hashPrevBlock.ToString(),
                current_tip ? current_tip->hash.ToString() : "none");
            return;
        }

        LogPrint(BCLog::SV2, "SubmitSolution: template_id=%lu prevhash=%s\n",
            solution.m_template_id, block_template->getBlock().hashPrevBlock.ToString());

        if (!block_template->submitSolution(solution.m_version, solution.m_header_timestamp, solution.m_header_nonce, std::move(cb))) {
            LogPrintf("SV2 SubmitSolution: ProcessNewBlock failed for template id=%lu\n", solution.m_template_id);
        }
}

void Sv2TemplateProvider::CoinbaseOutputDataSize(Sv2Client& client, node::Sv2CoinbaseOutputDataSizeMsg coinbase_tx_outputs_size)
{
    uint32_t max_additional_size = coinbase_tx_outputs_size.m_coinbase_output_max_additional_size;
    //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "coinbase_output_max_additional_size=%d bytes\n", max_additional_size);

    if (max_additional_size > MAX_BLOCK_WEIGHT) {
        LogPrint(BCLog::SV2, "CoinbaseOutputDataSize: impossible value %u from client=%zu, disconnecting\n",
            max_additional_size, client.m_id);
        client.m_disconnect_flag = true;
        return;
    }

    const bool size_changed = client.m_initial_work_sent &&
        (client.m_coinbase_tx_outputs_size != coinbase_tx_outputs_size.m_coinbase_output_max_additional_size);
    client.m_coinbase_tx_outputs_size = coinbase_tx_outputs_size.m_coinbase_output_max_additional_size;

    // Send (or re-send) work when the client first declares its coinbase output size,
    // or when it updates the size requirement (sv2-apps restarts monitoring in this case).
    // Without this, a newly connected client may have to wait for a new tip/interval tick.
    if ((!client.m_initial_work_sent || size_changed) && !client.m_disconnect_flag) {
        LOCKMt(m_tp_mutex);
        Amount dummy_last_fees;
        if (!SendWork(client, /*send_new_prevhash=*/true, dummy_last_fees)) {
            LogPrint(BCLog::SV2, "Disconnecting client id=%zu, reason: failed to send work on CoinbaseOutputDataSize\n", client.m_id);
            client.m_disconnect_flag = true;
        }
        client.m_initial_work_sent = true;
    }
}

void Sv2TemplateProvider::SetupCpmmection(Sv2Client& client
        , node::Sv2SetupConnectionMsg setup_conn
        , const uint16_t protocol_version
        , const uint8_t subprotocol
        , const uint16_t optional_features)
{
    // Disconnect a client that connects on the wrong subprotocol.
    if (setup_conn.m_protocol != subprotocol) {
        LogPrint(BCLog::SV2, "SetupConnection: unsupported protocol 0x%02x from client=%zu, disconnecting\n",
            setup_conn.m_protocol, client.m_id);
        node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"unsupported-protocol"}};
        client.m_send_messages.emplace_back(setup_conn_err);
        client.m_disconnect_flag = true;
        return;
    }

    // Disconnect a client if they are not running a compatible protocol version.
    if ((protocol_version < setup_conn.m_min_version) || (protocol_version > setup_conn.m_max_version)) {
        LogPrint(BCLog::SV2, "SetupConnection: version mismatch client=%zu (wants %d-%d, ours %d), disconnecting\n",
            client.m_id, setup_conn.m_min_version, setup_conn.m_max_version, protocol_version);
        node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"protocol-version-mismatch"}};
        client.m_send_messages.emplace_back(setup_conn_err);
        client.m_disconnect_flag = true;
        return;
    }

    LogPrint(BCLog::SV2, "SetupConnection: client=%zu connected (version=%d)\n", client.m_id, protocol_version);
    node::Sv2SetupConnectionSuccessMsg setup_success{protocol_version, optional_features};
    client.m_send_messages.emplace_back(setup_success);
    client.m_setup_connection_confirmed = true;
}
