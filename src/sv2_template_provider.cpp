#include "sv2_template_provider.h"

#include <base58.h>
#include <iostream>
#include <thread>
#include <sstream>
#include "util.h"

#include "net/netbase.h"
#include "util/check.h"
#include "fs.h"
#include "utilstrencodings.h"
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

    if (!m_connman->Start(this, m_options.host, m_options.port)) {
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
    m_cap_cv.notify_all();  // wake capacitor thread so it exits cleanly
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
        bool delay_0x72 = false;
        bool signal_cap = false;
        {
            LOCKMt(m_tp_mutex);
            best_block_changed = (m_best_prev_hash != tip->hash);
            if (best_block_changed) {
                m_best_prev_hash = tip->hash;
                m_last_block_time = GetTime<std::chrono::seconds>();
                m_template_last_update = GetTime<std::chrono::seconds>();
                if (m_cap_current_interval > 0.0) {
                    delay_0x72 = true;
                    if (!m_cap_pending) {
                        m_cap_pending = true;
                        signal_cap = true;  // first arm: need to wake capacitor thread
                    }
                }
            }
        }

        m_connman->ForEachClient([this, best_block_changed, delay_0x72](Sv2Client& client) {
            if (!client.m_coinbase_output_data_size_recv) {
                return;
            }

            LOCKMt(this->m_tp_mutex);
            Amount dummy_last_fees;
            if (!SendWork(client, /*send_new_prevhash=*/best_block_changed, dummy_last_fees, /*delay_0x72=*/delay_0x72)) {
                LogPrint(BCLog::SV2, "Disconnecting client id=%zu, reason: failed to send work on tip change\n",
                                client.m_id);
                client.m_disconnect_flag = true;
            }
        });

        // Signal capacitor thread only after pending_prev_hash is populated by SendWork
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
            // fees_previous_interval is only updated if the fee increase was sufficient,
            // since waitFeesChanged doesn't actually check this yet.

            Amount fees_before = last_fees;
            if (!SendWork(client, /*send_new_prevhash=*/false, fees_before)) {
                // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Disconnecting client id=%zu\n",
                //                 client.m_id);
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

bool Sv2TemplateProvider::BuildNewWorkSet(bool future_template, unsigned int coinbase_output_max_additional_size, NewWorkSet &newWorkSet)
{
    AssertLockHeld(m_tp_mutex);

    // Create new block
    CScript scriptDummy = CScript() << OP_TRUE;
    std::shared_ptr<BlockTemplate> pblocktemplate{m_mining.createNewBlock(scriptDummy)};
    if (!pblocktemplate) {
        LogPrint(BCLog::SV2, "BuildNewWorkSet: out of memory or no mining factory\n");
        return false;
    }

    // nTime and nBits are already set correctly by createNewBlock internals (FillBlockHeader).
    // Zero out nNonce so miners start from a clean state.
    pblocktemplate->getBlockRef()->nNonce = 0;
    LogPrint(BCLog::SV2, "BuildNewWorkSet: nTime=%u nBits=%08x\n",
        pblocktemplate->getBlockRef()->nTime, pblocktemplate->getBlockRef()->nBits);

    Sv2NewTemplateMsg new_template{*pblocktemplate, m_template_id, future_template};
    Sv2SetNewPrevHashMsg set_new_prev_hash{*pblocktemplate, m_template_id};

    newWorkSet = { new_template, pblocktemplate, set_new_prev_hash};
    return true;
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

bool Sv2TemplateProvider::SendWork(Sv2Client& client, bool send_new_prevhash, Amount& fees_before, bool delay_0x72)
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
    if (!BuildNewWorkSet(/*future_template=*/send_new_prevhash, client.m_coinbase_tx_outputs_size, new_work_set)) {
        return false;
    }

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
        if (delay_0x72) {
            LogPrint(BCLog::SV2, "Capacitor: hold 0x72 template_id=%lu for client id=%zu (interval=%.3fs)\n",
                new_work_set.prev_hash.m_template_id, client.m_id, m_cap_current_interval);
            client.m_pending_prev_hash = new_work_set.prev_hash;
        } else {
            LogPrint(BCLog::SV2, "Send 0x72 SetNewPrevHash prevhash=%s to client id=%zu\n",
                HexStr(bsv::span(new_work_set.prev_hash.m_prev_hash)), client.m_id);
            client.m_send_messages.emplace_back(new_work_set.prev_hash);
        }
    }

    // Evict oldest entry when cache is full to prevent unbounded memory growth
    if (m_block_template_cache.size() >= MAX_BLOCK_TEMPLATE_CACHE_SIZE) {
        m_block_template_cache.erase(m_block_template_cache.begin());
        LogPrint(BCLog::SV2, "Block template cache full, evicted oldest entry\n");
    }
    m_block_template_cache.insert({m_template_id, std::move(new_work_set.block_template)});

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
            interval = m_cap_current_interval;
        }
        if (interval <= 0.0) continue;

        // Sleep for the interval; wake early only on interrupt
        {
            std::unique_lock<std::mutex> lock(m_cap_mutex);
            m_cap_cv.wait_for(lock,
                std::chrono::duration<double>(interval),
                [this]{ return m_flag_interrupt_sv2.load(); });
        }
        if (m_flag_interrupt_sv2) break;

        // Dispatch all pending 0x72 messages
        LogPrint(BCLog::SV2, "Capacitor: interval=%.3fs elapsed, dispatching pending 0x72\n", interval);
        m_connman->ForEachClient([this](Sv2Client& client) {
            if (!client.m_coinbase_output_data_size_recv) return;
            LOCKMt(this->m_tp_mutex);
            if (client.m_pending_prev_hash.has_value()) {
                LogPrint(BCLog::SV2, "Capacitor: send delayed 0x72 template_id=%lu to client id=%zu\n",
                    client.m_pending_prev_hash->m_template_id, client.m_id);
                client.m_send_messages.emplace_back(*client.m_pending_prev_hash);
                client.m_pending_prev_hash = std::nullopt;
            }
        });

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
