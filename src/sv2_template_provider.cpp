#include "sv2_template_provider.h"

#include <base58.h>
//#include <stratumv2/args.h>
#include <stratumv2/sv2_noise.h>
#include <consensus/merkle.h>
#include <txmempool.h>
//#include <util/readwritefile.h>
//#include <util/thread.h>
#include <validation.h>
#include <iostream>
#include <thread>
#include <sstream>

#include "mining/assembler.h"
#include "net/netbase.h"
#include "util/check.h"
#include "consensus/consensus.h"
#include "fs.h"
#include "utilstrencodings.h"

using mining::BlockAssembler;
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
using mining::CBlockTemplate;


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
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Generated static key, saved to %s\n", bitcoinfs::PathToString(GetStaticKeyFile()));
    }
    //LogPrintLevel(BCLog::SV2, BCLog::Level::Info, "Static key: %s\n", HexStr(m_static_key.GetPubKey()));

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
    //LogInfo("Template Provider authority key: %s\n", EncodeBase58Check(version_pubkey_bytes));
    //LogTrace(BCLog::SV2, "Authority key: %s\n", HexStr(authority_pub_key));

    // Generate and sign certificate
    auto now{GetTime<std::chrono::seconds>()};
    uint16_t version = 0;
    // Start validity a little bit in the past to account for clock difference
    uint32_t valid_from = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count()) - 3600;
    uint32_t valid_to =  std::numeric_limits<unsigned int>::max(); // 2106
    m_certificate = Sv2SignatureNoiseMessage(version, valid_from, valid_to, XOnlyPubKey(m_static_key.GetPubKey()), authority_key);
    m_authority_pubkey = XOnlyPubKey(authority_key.GetPubKey());

    m_connman = std::make_unique<Sv2Connman>(TP_SUBPROTOCOL, m_static_key, m_authority_pubkey, m_certificate.value());
    // TODO: get rid of Init() ???
    Init({});
}

// Sv2TemplateProvider::Sv2TemplateProvider(Config &config, Mining& mining, CTxMemPool& mempool) 
//     : m_config{config}, m_mining{mining}, m_mempool{mempool}
// {
//     // TODO: persist static key
//     CKey static_key;
//     try {
//         AutoFile{fsbridge::fopen(GetStaticKeyFile(), "rb")} >> static_key;
//         //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Reading cached static key from %s\n", fs::PathToString(GetStaticKeyFile()));
//     } catch (const std::ios_base::failure&) {
//         // File is not expected to exist the first time.
//         // In the unlikely event that loading an existing key fails, create a new one.
//     }
//     if (!static_key.IsValid()) {
//         static_key = GenerateRandomKey();
//         try {
//             AutoFile{fsbridge::fopen(GetStaticKeyFile(), "wb")} << static_key;
//         } catch (const std::ios_base::failure&) {
//             //LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error writing static key to %s\n", fs::PathToString(GetStaticKeyFile()));
//             // Continue, because this is not a critical failure.
//         }
//         //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Generated static key, saved to %s\n", fs::PathToString(GetStaticKeyFile()));
//     }
//     //LogPrintLevel(BCLog::SV2, BCLog::Level::Info, "Static key: %s\n", HexStr(static_key.GetPubKey()));

//    // Generate self signed certificate using (cached) authority key
//     // TODO: skip loading authoritity key if -sv2cert is used

//     // Load authority key if cached
//     CKey authority_key;
//     try {
//         AutoFile{fsbridge::fopen(GetAuthorityKeyFile(), "rb")} >> authority_key;
//     } catch (const std::ios_base::failure&) {
//         // File is not expected to exist the first time.
//         // In the unlikely event that loading an existing key fails, create a new one.
//     }
//     if (!authority_key.IsValid()) {
//         authority_key = GenerateRandomKey();
//         try {
//             AutoFile{fsbridge::fopen(GetAuthorityKeyFile(), "wb")} << authority_key;
//         } catch (const std::ios_base::failure&) {
//             //LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error writing authority key to %s\n", fs::PathToString(GetAuthorityKeyFile()));
//             // Continue, because this is not a critical failure.
//         }
//         //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Generated authority key, saved to %s\n", fs::PathToString(GetAuthorityKeyFile()));
//     }
//     // SRI uses base58 encoded x-only pubkeys in its configuration files
//     std::array<unsigned char, 34> version_pubkey_bytes;
//     version_pubkey_bytes[0] = 1;
//     version_pubkey_bytes[1] = 0;
//     m_authority_pubkey = XOnlyPubKey(authority_key.GetPubKey());
//     std::copy(m_authority_pubkey.begin(), m_authority_pubkey.end(), version_pubkey_bytes.begin() + 2);
//     //LogInfo("Template Provider authority key: %s\n", EncodeBase58Check(version_pubkey_bytes));
//     //LogTrace(BCLog::SV2, "Authority key: %s\n", HexStr(m_authority_pubkey));

//     // Generate and sign certificate
//     auto now{GetTime<std::chrono::seconds>()};
//     uint16_t version = 0;
//     // Start validity a little bit in the past to account for clock difference
//     uint32_t valid_from = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count()) - 3600;
//     uint32_t valid_to =  std::numeric_limits<unsigned int>::max(); // 2106
//     Sv2SignatureNoiseMessage certificate = Sv2SignatureNoiseMessage(version, valid_from, valid_to, XOnlyPubKey(static_key.GetPubKey()), authority_key);

//     m_connman = std::make_unique<Sv2Connman>(TP_SUBPROTOCOL, static_key, m_authority_pubkey, certificate);
// }

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
    LogPrintf("Sv2TemplateProvider start\n");
    m_options = options;
    Init(options);

    if (!m_connman->Start(this, m_options.host, m_options.port)) {
        return false;
    }
    // try {
    //     auto sock = BindListenPort(options.port);
    //     m_listening_socket = std::move(sock);
    // } catch (const std::runtime_error& e) {
    //     LogPrintf("Template Provider failed to bind to port %d: %s\n", options.port, e.what());
    //     return false;
    // }

    m_thread_sv2_handler = std::thread(&TraceThread<std::function<void()>>
        , "sv2"
        , std::function<void()>(std::bind(&Sv2TemplateProvider::ThreadSv2Handler, this)));

    m_thread_sv2_mempool_handler = std::thread(&TraceThread<std::function<void()>>
        , "sv2mempool"
        , std::function<void()>(std::bind(&Sv2TemplateProvider::ThreadSv2MempoolHandler, this)));

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
    LogPrint(BCLog::SV2, "interrupt sv2 connect!\n");
    m_flag_interrupt_sv2 = true;
}

void Sv2TemplateProvider::StopThreads()
{
    if (m_thread_sv2_handler.joinable()) {
        m_thread_sv2_handler.join();
    }
    if (m_thread_sv2_mempool_handler.joinable()) {
        m_thread_sv2_mempool_handler.join();
    }
}

std::shared_ptr<Sock> Sv2TemplateProvider::BindListenPort(uint16_t port) const
{
    const CService addr_bind = LookupNumeric("0.0.0.0", port);

    auto sock = CreateSock(addr_bind);
    if (!sock) {
        throw std::runtime_error("Sv2 Template Provider cannot create socket");
    }

    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);

    if (!addr_bind.GetSockAddr(reinterpret_cast<struct sockaddr*>(&sockaddr), &len)) {
        throw std::runtime_error("Sv2 Template Provider failed to get socket address");
    }

    if (sock->Bind(reinterpret_cast<struct sockaddr*>(&sockaddr), len) == SOCKET_ERROR) {
        const int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE) {
            throw std::runtime_error(strprintf("Unable to bind to %d on this computer. %s is probably already running.\n", port, PACKAGE_NAME));
        }

        throw std::runtime_error(strprintf("Unable to bind to %d on this computer (bind returned error %s )\n", port, NetworkErrorString(nErr)));
    }

    constexpr int max_pending_conns{4096};
    if (sock->Listen(max_pending_conns) == SOCKET_ERROR) {
        throw std::runtime_error("Sv2 Template Provider listening socket has an error listening");
    }

    return sock;
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
    auto waitTipChanged = [&](Mining::MillisecondsDouble timeout){
        uint256 previous_hash{WITH_LOCK(::cs_main, return chainActive.Tip()->GetBlockHash();)};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            WAIT_LOCKMt(g_best_block_mutex, lock);
            while (/*!chainman().m_interrupt &&*/ std::chrono::steady_clock::now() < deadline) {
                auto check_time = std::chrono::steady_clock::now() + std::min(timeout, Mining::MillisecondsDouble(1000));
                g_best_block_cv.wait_until(lock, check_time);
                if (g_best_block != previous_hash) break;
                // Obtaining the height here using chainActive.Tip()->nHeight
                // would result in a deadlock, because UpdateTip requires holding cs_main.
            }
        }
        LOCK(::cs_main);
        return std::make_pair(chainActive.Tip()->GetBlockHash(), chainActive.Tip()->nHeight);
    };

    while (!m_flag_interrupt_sv2) {

        auto tip{waitTipChanged(500ms)};
        bool best_block_changed{WITH_LOCK(m_tp_mutex, return m_best_prev_hash != tip.first;)};

        // Keep m_best_prev_hash unset during IBD to avoid pushing outdated
        // templates. Except for signet, because we might be the only miner.
        // if (m_mining.isInitialBlockDownload() && gArgs.GetChainType() != ChainType::SIGNET) {
        //     best_block_changed = false;
        // }

        if (best_block_changed) {
            {
                LOCKMt(m_tp_mutex);
                m_best_prev_hash = tip.first;
                m_last_block_time = GetTime<std::chrono::seconds>();
                m_template_last_update = GetTime<std::chrono::seconds>();
            }

            m_connman->ForEachClient([this, best_block_changed](Sv2Client& client) {
                // For newly connected clients, we call SendWork after receiving
                // CoinbaseOutputDataSize.
                if (client.m_coinbase_tx_outputs_size == 0) return;

                LOCKMt(this->m_tp_mutex);
                Amount dummy_last_fees;
                if (!SendWork(client, /*send_new_prevhash=*/best_block_changed, dummy_last_fees)) {
                    // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Disconnecting client id=%zu\n",
                    //                 client.m_id);
                    LogPrintf("Disconnecting client id=%zu\n",
                                    client.m_id);
                    client.m_disconnect_flag = true;
                }
            });
        }

        LOCKMt(m_tp_mutex);
        PruneBlockTemplateCache();
    }
}

void Sv2TemplateProvider::ThreadSv2HandlerOld()
{
    AssertLockNotHeld(m_clients_mutex);
    AssertLockNotHeld(m_tp_mutex);

    Timer timer;
    unsigned int mempool_last_update = 0;
    unsigned int template_last_update = 0;

    while (!m_flag_interrupt_sv2) {
        if (::IsInitialBlockDownload()) {
            m_interrupt_sv2.sleep_for(std::chrono::milliseconds(100));
            LogPrintf("not IsInitialBlockDownload\n");
            continue;
        }

        // If we've left IBD. Create the listening socket for new sv2 connections.
        if (!m_listening_socket) {
            try {
                auto socket = BindListenPort(m_port);
                m_listening_socket = std::move(socket);
            } catch (const std::runtime_error& e) {
                LogPrint(BCLog::SV2, "thread shutting down due to exception: %s\n", e.what());
                Interrupt();
                continue;
            }
        }
        {
            LOCKMt(m_clients_mutex);
            DisconnectFlagged();
        }

        bool best_block_changed = [this]() {
            WAIT_LOCKMt(g_best_block_mutex, lock);
            auto checktime = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            g_best_block_cv.wait_until(lock, checktime);
            LOCKMt(m_tp_mutex);
            if (m_best_prev_hash != g_best_block && g_best_block != uint256()) {
                LogPrintf("m_best_prev_hash:%s g_best_block:%s\n",
                    HexStr(bsv::span(m_best_prev_hash)), HexStr(bsv::span(g_best_block)));
                m_best_prev_hash = g_best_block;
                return true;
            }
            return false;
        }();

        /** TODO: only look for mempool updates that (likely) impact the next block.
         *        See `doc/stratum-v2.md#mempool-monitoring`
         */
        mempool_last_update = m_mempool.GetTransactionsUpdated();
        bool should_make_template = false;

        if (best_block_changed) {
            LogPrintf("best_block_changed\n");
            {
                LOCKMt(m_tp_mutex);
                m_last_block_time = GetTime<std::chrono::seconds>();
            }

            LOCKMt(m_clients_mutex);
            for (auto& client : m_sv2_clients) {
                client->m_latest_submitted_template_fees = Amount{};
            }

            // Build a new best template, best prev hash and update the block cache.
            LogPrintf("if set should_make_template\n");
            should_make_template = true;
            template_last_update = mempool_last_update;
        }
        else if (timer.trigger())// && mempool_last_update > template_last_update)
        {
            should_make_template = true;
        }
        else
        {
            // LogPrintf("  else mempool_last_update:%d template_last_update:%d  ",mempool_last_update,template_last_update);
            // LogPrintf(" no set should_make_template ");
        }

        {
            LOCKMt(m_tp_mutex);
            PruneBlockTemplateCache();
        }

        if (should_make_template) {
            LOCKMt(m_clients_mutex);
            // Update all clients with the new template and prev hash.
            for (const auto& client : m_sv2_clients) {
                // For newly connected clients, we call SendWork after receiving
                // CoinbaseOutputDataSize.
                if (client->m_coinbase_tx_outputs_size == 0)
                {
                    LogPrintf("m_coinbase_tx_outputs_size == 0");
                    continue;
                }
                LOCKMt(m_tp_mutex);
                Amount dummy_last_fees;
                if (!SendWork(*client.get(), /*send_new_prevhash=*/best_block_changed,dummy_last_fees)) {
                    //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Disconnecting client id=%zu\n",
                                  //client->m_id);
                    LogPrintf("Disconnecting client id=%zu\n",
                                  client->m_id);
                    client->m_disconnect_flag = true;
                    continue;
                }
            }
        }

        // Poll/Select the sockets that need handling.
        Sock::EventsPerSock events_per_sock = WITH_LOCK(m_clients_mutex, return GenerateWaitSockets(m_listening_socket, m_sv2_clients));

        constexpr auto timeout = std::chrono::milliseconds(50);
        if (!events_per_sock.begin()->first->WaitMany(timeout, events_per_sock)) {
            continue;
        }

        // Accept any new connections for sv2 clients.
        const auto listening_sock = events_per_sock.find(m_listening_socket);
        if (listening_sock != events_per_sock.end() && listening_sock->second.occurred & Sock::RECV) {
            struct sockaddr_storage sockaddr;
            socklen_t sockaddr_len = sizeof(sockaddr);

            auto sock = m_listening_socket->Accept(reinterpret_cast<struct sockaddr*>(&sockaddr), &sockaddr_len);
            if (sock) {
                Assume(m_certificate);
                LOCKMt(m_clients_mutex);
                std::unique_ptr transport = std::make_unique<Sv2Transport>(m_static_key, m_certificate.value());
                size_t id{m_sv2_clients.size() + 1};
                std::unique_ptr client = std::make_unique<Sv2Client>(
                    id, std::move(sock), std::move(transport)
                );
                //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "New client id=%zu connected\n", client->m_id);
                m_sv2_clients.emplace_back(std::move(client));
            }
        }

        LOCKMt(m_clients_mutex);
        // Process messages from and for connected sv2_clients.
        for (auto& client : m_sv2_clients) {
            bool has_received_data = false;
            bool has_error_occurred = false;

            const auto socket_it = events_per_sock.find(client->m_sock);
            if (socket_it != events_per_sock.end()) {
                has_received_data = socket_it->second.occurred & Sock::RECV;
                has_error_occurred = socket_it->second.occurred & Sock::ERR;
            }

            if (has_error_occurred) {
                //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Socket receive error, disconnecting client id=%zu\n",
                //client->m_id);
                client->m_disconnect_flag = true;
                continue;
            }

            // Process message queue and any outbound bytes still held by the transport
            auto it = client->m_send_messages.begin();
            std::optional<bool> expected_more;
            while(true) {
                if (it != client->m_send_messages.end()) {
                    // If possible, move one message from the send queue to the transport.
                    // This fails when there is an existing message still being sent,
                    // or when the handshake has not yet completed.
                    if (client->m_transport->SetMessageToSend(*it)) {
                        ++it;
                    }
                }

                const auto& [data, more] = client->m_transport->GetBytesToSendSv2(/*have_next_message=*/it != client->m_send_messages.end());
                size_t total_sent = 0;

                // We rely on the 'more' value returned by GetBytesToSend to correctly predict whether more
                // bytes are still to be sent, to correctly set the MSG_MORE flag. As a sanity check,
                // verify that the previously returned 'more' was correct.
                if (expected_more.has_value()) Assume(!data.empty() == *expected_more);
                expected_more = more;
                ssize_t sent = 0;

                if (!data.empty()) {
                    int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#ifdef MSG_MORE
            if (more) {
                flags |= MSG_MORE;
            }
#endif
                    //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Send %d bytes to client id=%zu\n",
                                  //data.size() - total_sent, client->m_id);
                    LogPrintf("old Send %d bytes to client id=%zu data size:%d message size:%d\n",
                                  data.size() - total_sent, client->m_id, data.size(),client->m_send_messages.size());
                    LogPrintf("data:%s\n",HexStr(bsv::span(data.data() + total_sent, data.size())));
                    for(int i = 0;i < data.size();++i)
                    {
                        LogPrintf("data[%d]:%d ",i,*(data.data() + total_sent + i));
                    }
                    LogPrintf(" over\n");
                    sent = client->m_sock->Send(data.data() + total_sent, data.size() - total_sent, flags);
                }
                if (sent > 0) {
                    // Notify transport that bytes have been processed.
                    client->m_transport->MarkBytesSent(sent);
                    if ((size_t)sent != data.size()) {
                        // could not send full message; stop sending more
                        LogPrintf("break out whill line:%d\n",__LINE__);
                        break;
                    }
                } else {
                    if (sent < 0) {
                        // error
                        int nErr = WSAGetLastError();
                        if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                            //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Socket send error for  client id=%zu: %s\n",
                                          //client->m_id, NetworkErrorString(nErr));
                            LogPrintf("Socket send error for  client id=%zu: %s\n",
                                          client->m_id, NetworkErrorString(nErr));
                            client->m_disconnect_flag = true;
                        }
                    }
                    break;
                }
            }
            // Clear messages that have been handed to transport from the queue
            if(it != client->m_send_messages.end())
            {
                LogPrintf("m_send_messages erase num:%d\n",std::distance(client->m_send_messages.begin(), it));
            }
            client->m_send_messages.erase(client->m_send_messages.begin(), it);

            // Stop processing this client if something went wrong during sending
            if (client->m_disconnect_flag) break;

            if (has_received_data) {
                uint8_t bytes_received_buf[0x10000];

                const auto num_bytes_received = client->m_sock->Recv(bytes_received_buf, sizeof(bytes_received_buf), MSG_DONTWAIT);
                //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Num bytes received from client id=%zu: %d\n",
                              //client->m_id, num_bytes_received);
                LogPrintf("Num bytes received from client id=%zu: %d\n",
                              client->m_id, num_bytes_received);
                for(int i = 0; i < num_bytes_received; ++i)
                {
                    LogPrintf("bytes_received_buf:%d ",bytes_received_buf[i]);
                }
                LogPrintf("LogPrintf over\n");

                if (num_bytes_received <= 0) {
                    //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Disconnecting client id=%zu\n",
                                  //client->m_id);
                    LogPrintf("Disconnecting client id=%zu\n",
                                  client->m_id);
                    client->m_disconnect_flag = true;
                    continue;
                }

                try
                {
                    auto msg_ = bsv::span(bytes_received_buf, num_bytes_received);
                    bsv::span<const uint8_t> msg(reinterpret_cast<const uint8_t*>(msg_.data()), msg_.size());
                    while (msg.size() > 0) {
                        // absorb network data
                        if (!client->m_transport->ReceivedBytes(msg)) {
                            // Serious transport problem
                            //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Transport problem, disconnecting client id=%zu\n",
                                          //client->m_id);
                            LogPrintf("Transport problem, disconnecting client id=%zu\n",
                                          client->m_id);
                            client->m_disconnect_flag = true;
                            continue;
                        }

                        if (client->m_transport->ReceivedMessageComplete()) {
                            Sv2NetMsg msg = client->m_transport->GetReceivedMessage();
                            ProcessSv2Message(msg, *client.get());

                            // bool dummy_reject_message = false;
                            // Sv2NetMsg msg = client->m_transport->GetReceivedMessage(std::chrono::milliseconds(0), dummy_reject_message);
                            // ProcessSv2Message(msg, *client.get());
                        }                        
                        // if (client->m_transport->ReceivedMessageComplete()) {
                        //     Sv2NetMsg msg = client->m_transport->GetReceivedMessage();
                        //     ProcessSv2Message(msg, *client.get());
                        // }
                        else
                        {
                            LogPrintf("no call ProcessSv2Message\n");
                        }
                    }
                } catch (const std::exception& e) {
                    LogPrint(BCLog::SV2,  "Received error when processing client id=%zu message: %s\n", client->m_id, e.what());
                    client->m_disconnect_flag = true;
                }
            }
        }
    }
    LogPrintf("end while\n");
}

void Sv2TemplateProvider::ThreadSv2MempoolHandler()
{
    Timer timer(m_options.fee_check_interval);

    //! Fees for the previous fee_check_interval
    Amount fees_previous_interval{0};
    //! Fees as of the last waitFeesChanged() call
    Amount last_fees{0};

    auto getTipHash = []() -> std::optional<uint256>
    {
        LOCK(::cs_main);
        CBlockIndex* tip{chainActive.Tip()};
        if (!tip) return std::nullopt;
        return tip->GetBlockHash();
    };

    auto waitFeesChanged = [&](Mining::MillisecondsDouble timeout, uint256 tip, Amount fee_delta, Amount& fees_before, bool& tip_changed){
        Assume(getTipHash());
        unsigned int last_mempool_update{m_mempool.GetTransactionsUpdated()};

        auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            while (/*!chainman().m_interrupt &&*/ std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::min(timeout, Mining::MillisecondsDouble(100)));
                if (getTipHash().value() != tip) {
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
        //if (!m_mining.waitFeesChanged(timeout, WITH_LOCK(m_tp_mutex, return m_best_prev_hash;), m_options.fee_delta, last_fees, tip_changed)) {
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
            // For newly connected clients, we call SendWork after receiving
            // CoinbaseOutputDataSize.
            if (client.m_coinbase_tx_outputs_size == 0) return;

            LOCKMt(this->m_tp_mutex);
            // fees_previous_interval is only updated if the fee increase was sufficient,
            // since waitFeesChanged doesn't actually check this yet.

            Amount fees_before = last_fees;
            LogPrintf("call SendWork line:%d\n",__LINE__);
            if (!SendWork(client, /*send_new_prevhash=*/false, fees_before)) {
                // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Disconnecting client id=%zu\n",
                //                 client.m_id);
                LogPrintf("Disconnecting client id=%zu\n",
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

    const auto time_start{std::chrono::steady_clock::now()};

    static CBlockIndex *pindexPrev;
    static int64_t nStart;
    static std::unique_ptr<CBlockTemplate> pblocktemplate{nullptr};

    // Clear pindexPrev so future calls make a new block, despite any
    // failures from here on
    pindexPrev = nullptr;
    nStart = GetTime();

    // Create new block
    if(!mining::g_miningFactory) {
        LogPrintf("No mining factory available");
        return false;
    }
    CScript scriptDummy = CScript() << OP_TRUE;
    pblocktemplate = mining::g_miningFactory->GetAssembler()->CreateNewBlock(scriptDummy, pindexPrev);
    if (!pblocktemplate) {
        LogPrintf("Out of memory");
        return false;
    }

    // pointer for convenience
    CBlockRef blockRef = pblocktemplate->GetBlockRef();
    CBlock *pblock = blockRef.get();

    // Update nTime
    UpdateTime(pblock, m_config, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, m_config);
    pblock->nNonce = 0;
    LogPrintf("new block time:%d\n",pblock->nTime);

    //LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Assemble template: %.2fms\n",
        //Ticks<MillisecondsDouble>(SteadyClock::now() - time_start));
    Sv2NewTemplateMsg new_template{pblocktemplate->GetBlockRef(), m_template_id, future_template};
    Sv2SetNewPrevHashMsg set_new_prev_hash{pblocktemplate->GetBlockRef(), m_template_id};

    newWorkSet = { new_template, std::move(pblocktemplate), set_new_prev_hash};
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

    auto predicate = [prev_hash](const auto& it){
        if (it.second->GetBlockRef()->hashPrevBlock != prev_hash) {
            LogPrintf("PruneBlockTemplateCache prevHash:%s\n",
                HexStr(bsv::span(it.second->GetBlockRef()->hashPrevBlock)));
            return true;
        }
        return false;
    };
    for(auto it = m_block_template_cache.begin(); it != m_block_template_cache.end(); ) {
        if (predicate(*it)) {
            LogPrintf("PruneBlockTemplateCache m_block_template_cache size:%d\n",m_block_template_cache.size());
            it = m_block_template_cache.erase(it);
        } else {
            ++it;
        }
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
    bool flagRet = BuildNewWorkSet(/*future_template=*/send_new_prevhash, client.m_coinbase_tx_outputs_size, new_work_set);

    if (m_best_prev_hash == uint256{}) {
        // g_best_block is set UpdateTip(), so will be 0 when the node starts
        // and no new blocks have arrived.
        m_best_prev_hash = new_work_set.block_template->GetBlockRef()->hashPrevBlock;
    }

    // Do not submit new template if the fee increase is insufficient:
    Amount fees;
    for (Amount fee : new_work_set.block_template->vTxFees) {
        // Skip coinbase
        if (fee < Amount{}) continue;
        fees += fee;
    }
    if (!send_new_prevhash && fees_before != Amount() && fees_before + Amount(m_minimum_fee_delta) > fees) return true;

    //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x71 NewTemplate id=%lu to client id=%zu\n", m_template_id, //client.m_id);
    LogPrintf("Send 0x71 NewTemplate id=%lu to client id=%zu\n", m_template_id, client.m_id);
    client.m_send_messages.emplace_back(new_work_set.new_template);

    if (send_new_prevhash) {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x72 SetNewPrevHash to client id=%zu\n", //client.m_id);
        LogPrintf("Send 0x72 SetNewPrevHash PrevHash:%s to client id=%zu\n", HexStr(bsv::span(new_work_set.prev_hash.m_prev_hash)), client.m_id);
        client.m_send_messages.emplace_back(new_work_set.prev_hash);
    }

    auto block = (new_work_set.block_template)->GetBlockRef().get();
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;
    bnTarget.SetCompact(block->nBits, &fNegative, &fOverflow);

    m_block_template_cache.insert({m_template_id, std::move(new_work_set.block_template)});

    return true;
}

Sock::EventsPerSock Sv2TemplateProvider::GenerateWaitSockets(const std::shared_ptr<Sock>& listen_socket, const Clients& sv2_clients) const
{
    Sock::EventsPerSock events_per_sock;
    events_per_sock.emplace(listen_socket, Sock::Events(Sock::RECV));

    for (const auto& client : sv2_clients) {
        if (!client->m_disconnect_flag && client->m_sock) {
            events_per_sock.emplace(client->m_sock, Sock::Events{Sock::RECV | Sock::ERR});
        }
    }

    return events_per_sock;
}

void Sv2TemplateProvider::ProcessSv2Message(const Sv2NetMsg& sv2_net_msg, Sv2Client& client)
{
    auto &&config = m_config;
    std::stringstream ssTH;
    ssTH << std::this_thread::get_id();
    std::string threadIdStr = ssTH.str();
    LogPrintf("start ProcessSv2Message thread:%s\n",threadIdStr.c_str());
    AssertLockNotHeld(m_tp_mutex);

    DataStream ss (sv2_net_msg.m_msg);

    switch (sv2_net_msg.m_sv2_header.m_msg_type)
    {
    case Sv2MsgType::SETUP_CONNECTION:
    {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x00 SetupConnection from client id=%zu\n",
                      //client.m_id);
        LogPrintf("Received 0x00 SetupConnection from client id=%zu\n",
                      client.m_id);

        if (client.m_setup_connection_confirmed) {
            LogPrint(BCLog::SV2,  "Client client id=%zu connection has already been confirmed\n",
                          client.m_id);
            return;
        }

        Sv2SetupConnectionMsg setup_conn;
        try {
            ss >> setup_conn;
        } catch (const std::exception& e) {
            LogPrint(BCLog::SV2,  "Received invalid SetupConnection message from client id=%zu: %s\n",
                          client.m_id, e.what());
            client.m_disconnect_flag = true;
            return;
        }

        // Disconnect a client that connects on the wrong subprotocol.
        if (setup_conn.m_protocol != TP_SUBPROTOCOL) {
            Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"unsupported-protocol"}};

            //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnectionError to client id=%zu\n",
                          //client.m_id);
            LogPrintf("Send 0x02 SetupConnectionError to client id=%zu\n",
                          client.m_id);
            client.m_send_messages.emplace_back(setup_conn_err);

            client.m_disconnect_flag = true;
            return;
        }

        // Disconnect a client if they are not running a compatible protocol version.
        if ((m_protocol_version < setup_conn.m_min_version) || (m_protocol_version > setup_conn.m_max_version)) {
            Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"protocol-version-mismatch"}};
            //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnection.Error to client id=%zu\n",
                          //client.m_id);
            LogPrintf("Send 0x02 SetupConnection.Error to client id=%zu\n",
                          client.m_id);
            client.m_send_messages.emplace_back(setup_conn_err);

            LogPrint(BCLog::SV2,  "Received a connection from client id=%zu with incompatible protocol_versions: min_version: %d, max_version: %d\n",
                          client.m_id, setup_conn.m_min_version, setup_conn.m_max_version);
            client.m_disconnect_flag = true;
            return;
        }

        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x01 SetupConnection.Success to client id=%zu\n",
                      //client.m_id);
        LogPrintf("Send 0x01 SetupConnection.Success to client id=%zu\n",
                      client.m_id);
        Sv2SetupConnectionSuccessMsg setup_success{m_protocol_version, m_optional_features};
        client.m_send_messages.emplace_back(setup_success);

        client.m_setup_connection_confirmed = true;

        break;
    }
    case Sv2MsgType::COINBASE_OUTPUT_DATA_SIZE:
    {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x70 CoinbaseOutputDataSize from client id=%zu\n",
                      //client.m_id);
        LogPrintf("Received 0x70 CoinbaseOutputDataSize from client id=%zu\n",
                      client.m_id);

        if (!client.m_setup_connection_confirmed) {
            client.m_disconnect_flag = true;
            return;
        }

        Sv2CoinbaseOutputDataSizeMsg coinbase_output_data_size;
        try {
            ss >> coinbase_output_data_size;
            client.m_coinbase_output_data_size_recv = true;
        } catch (const std::exception& e) {
            LogPrint(BCLog::SV2,  "Received invalid CoinbaseOutputDataSize message from client id=%zu: %s\n",
                          client.m_id, e.what());
            client.m_disconnect_flag = true;
            return;
        }

        uint32_t max_additional_size = coinbase_output_data_size.m_coinbase_output_max_additional_size;
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "coinbase_output_max_additional_size=%d bytes\n", max_additional_size);

        if (max_additional_size > MAX_BLOCK_WEIGHT) {
            LogPrint(BCLog::SV2,  "Received impossible CoinbaseOutputDataSize from client id=%zu: %d\n",
                          client.m_id, max_additional_size);
            client.m_disconnect_flag = true;
            return;
        }

        client.m_coinbase_tx_outputs_size = coinbase_output_data_size.m_coinbase_output_max_additional_size;

        // Send new template and prevout
        LOCKMt(m_tp_mutex);
        Amount dummy_last_fees;
        if (!SendWork(client, /*send_new_prevhash=*/true,dummy_last_fees)) {
            return;
        }

        break;
    }
    case Sv2MsgType::SUBMIT_SOLUTION: {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x60 SubmitSolution from client id=%zu\n",
                      //client.m_id);
        LogPrintf("Received 0x60 SubmitSolution from client id=%zu\n",
                      client.m_id);

        if (!client.m_setup_connection_confirmed && !client.m_coinbase_output_data_size_recv) {
            client.m_disconnect_flag = true;
            return;
        }

        Sv2SubmitSolutionMsg submit_solution;
        try {
            ss >> submit_solution;
        } catch (const std::exception& e) {
            LogPrint(BCLog::SV2,  "Received invalid SubmitSolution message from client id=%zu: %e\n",
                          client.m_id, e.what());
            return;
        }

        SubmitSolution(submit_solution);
        break;
    }

    case Sv2MsgType::REQUEST_TRANSACTION_DATA:
    {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x73 RequestTransactionData from client id=%zu\n",
                      //client.m_id);
        LogPrintf("Received 0x73 RequestTransactionData from client id=%zu\n",
                      client.m_id);

        Sv2RequestTransactionDataMsg request_tx_data;
        sv2_net_msg.m_msg;
        try {
            ss >> request_tx_data;
        } catch (const std::exception& e) {
            LogPrint(BCLog::SV2,  "Received invalid RequestTransactionData message from client id=%zu: %e\n",
                          client.m_id, e.what());
            return;
        }
        RequestTransactionData(client,request_tx_data);
        break;
    }

    default: {
        uint8_t msg_type[1]{uint8_t(sv2_net_msg.m_sv2_header.m_msg_type)};
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Warning, "Received unknown message type 0x%s from client id=%zu\n",
                      //HexStr(msg_type), client.m_id);

        LogPrintf("Received unknown message type 0x%s from client id=%zu\n",
                      HexStr(bsv::span(msg_type)), client.m_id);
        break;
    }
    }

    ReceivedMessage(client, sv2_net_msg.m_sv2_header.m_msg_type);
    LogPrintf("end ProcessSv2Message last\n");
}

void Sv2TemplateProvider::ReceivedMessage(Sv2Client& client, node::Sv2MsgType msg_type) {
    switch (msg_type)
    {
    case node::Sv2MsgType::COINBASE_OUTPUT_DATA_SIZE:
    {
        LOCKMt(m_tp_mutex);
        Amount dummy_last_fees;
        //Amount dummy_last_fees;
        if (!SendWork(client, /*send_new_prevhash=*/true, dummy_last_fees)) {
            return;
        }
        break;
    }
    default: {}
    }
}

void Sv2TemplateProvider::RequestTransactionData(Sv2Client& client, node::Sv2RequestTransactionDataMsg msg)
{
    LOCKMt(m_tp_mutex);
    auto cached_block = m_block_template_cache.find(msg.m_template_id);
    if (cached_block != m_block_template_cache.end()) {
        auto block = (*cached_block->second).GetBlockRef();

        if (block->hashPrevBlock != m_best_prev_hash) {
            //LogTrace(BCLog::SV2, "Template id=%lu prevhash=%s, tip=%s\n", msg.m_template_id, HexStr(block.hashPrevBlock), HexStr(m_best_prev_hash));
            LogPrintf("Template id=%lu prevhash=%s, tip=%s\n", msg.m_template_id, HexStr(bsv::span(block->hashPrevBlock)), HexStr(bsv::span(m_best_prev_hash)));
            node::Sv2RequestTransactionDataErrorMsg request_tx_data_error{msg.m_template_id, "stale-template-id"};


            // LogDebug(BCLog::SV2, "Send 0x75 RequestTransactionData.Error (stale-template-id) to client id=%zu\n",
            //         client.m_id);
            LogPrintf("Send 0x75 RequestTransactionData.Error (stale-template-id) to client id=%zu\n",
                    client.m_id);
            client.m_send_messages.emplace_back(request_tx_data_error);
            return;
        }

        //std::vector<uint8_t> witness_reserve_value;
        // auto scriptWitness = block->vtx[0]->vin[0].scriptWitness;
        // if (!scriptWitness.IsNull()) {
        //     std::copy(scriptWitness.stack[0].begin(), scriptWitness.stack[0].end(), std::back_inserter(witness_reserve_value));
        // }
        std::vector<CTransactionRef> txs;
        if (block->vtx.size() > 0) {
            std::copy(block->vtx.begin() + 1, block->vtx.end(), std::back_inserter(txs));
        }

        node::Sv2RequestTransactionDataSuccessMsg request_tx_data_success{msg.m_template_id, /*std::move(witness_reserve_value),*/ std::move(txs)};

        // LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x74 RequestTransactionData.Success to client id=%zu\n",
        //                 client.m_id);
        LogPrintf("Send 0x74 RequestTransactionData.Success to client id=%zu\n",
                        client.m_id);
        client.m_send_messages.emplace_back(request_tx_data_success);
    } else {
        node::Sv2RequestTransactionDataErrorMsg request_tx_data_error{msg.m_template_id, "template-id-not-found"};

        // LogDebug(BCLog::SV2, "Send 0x75 RequestTransactionData.Error (template-id-not-found: %zu) to client id=%zu\n",
        //         msg.m_template_id, client.m_id);
        LogPrintf("Send 0x75 RequestTransactionData.Error (template-id-not-found: %zu) to client id=%zu\n",
                msg.m_template_id, client.m_id);
        client.m_send_messages.emplace_back(request_tx_data_error);
    }
}

void Sv2TemplateProvider::SubmitSolution(node::Sv2SubmitSolutionMsg solution)
{
        // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "version=%d, timestamp=%d, nonce=%d\n",
        //     solution.m_version,
        //     solution.m_header_timestamp,
        //     solution.m_header_nonce
        // );
        LogPrintf("SubmitSolution version=%d, timestamp=%d, nonce=%d\n",
            solution.m_version,
            solution.m_header_timestamp,
            solution.m_header_nonce
        );
        
        CBlockRef block_ptr;
        {
            // We can't hold this lock until submitSolution() because it's
            // possible that the new block arrives via the p2p network at the
            // same time. That leads to a deadlock in g_best_block_mutex.
            LOCKMt(m_tp_mutex);
            auto cached_block_template = m_block_template_cache.find(solution.m_template_id);
            if (cached_block_template == m_block_template_cache.end()) {
                // LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Template with id=%lu is no longer in cache\n",
                // solution.m_template_id);
                LogPrintf("Template with id=%lu is no longer in cache\n",
                    solution.m_template_id);
                return;
            }
            /**
             * It's important to not delete this template from the cache in case
             * another solution is submitted for the same template later.
             *
             * This is very unlikely on mainnet, but not impossible. Many mining
             * devices may be working on the default pool template at the same
             * time and they may not update the new tip right away.
             *
             * The node will never broadcast the second block. It's marked
             * valid-headers in getchaintips. However a node or pool operator
             * may wish to manually inspect the block or keep it as a souvenir.
             * Additionally, because in Stratum v2 the block solution is sent
             * to both the pool node and the template provider node, it's
             * possibly they arrive out of order and two competing blocks propagate
             * on the network. In case of a reorg the node will be able to switch
             * faster because it already has (but not fully validated) the block.
             */
            block_ptr = cached_block_template->second->GetBlockRef();
        }

        auto cb = MakeTransactionRef(std::move(solution.m_coinbase_tx));

        if (block_ptr->vtx.size() == 0) {
            block_ptr->vtx.push_back(cb);
        } else {
            block_ptr->vtx[0] = cb;
        }

        block_ptr->nVersion = solution.m_version;
        block_ptr->nTime    = solution.m_header_timestamp;
        block_ptr->nNonce   = solution.m_header_nonce;

        block_ptr->hashMerkleRoot = BlockMerkleRoot(*block_ptr.get());

        if (!ProcessNewBlock(m_config, block_ptr, true, nullptr)) {
            LogPrintf("ProcessNewBlock failed for block with prevHash:%s\n", HexStr(block_ptr->hashPrevBlock));
        }
}

void Sv2TemplateProvider::CoinbaseOutputDataSize(Sv2Client& client, node::Sv2CoinbaseOutputDataSizeMsg coinbase_tx_outputs_size)
{
    uint32_t max_additional_size = coinbase_tx_outputs_size.m_coinbase_output_max_additional_size;
    //LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "coinbase_output_max_additional_size=%d bytes\n", max_additional_size);

    if (max_additional_size > MAX_BLOCK_WEIGHT) {
        //LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received impossible CoinbaseOutputDataSize from client id=%zu: %d\n",
                        //client.m_id, max_additional_size);
        LogPrintf("Received impossible CoinbaseOutputDataSize from client id=%zu: %d\n",
                        client.m_id, max_additional_size);
        client.m_disconnect_flag = true;
        return;
    }

    client.m_coinbase_tx_outputs_size = coinbase_tx_outputs_size.m_coinbase_output_max_additional_size;
}

void Sv2TemplateProvider::SetupCpmmection(Sv2Client& client
        , node::Sv2SetupConnectionMsg setup_conn
        , const uint16_t protocol_version
        , const uint8_t subprotocol
        , const uint16_t optional_features)
{
    LogPrintf("start SetupCpmmection\n");
    // Disconnect a client that connects on the wrong subprotocol.
    if (setup_conn.m_protocol != subprotocol) {
        node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"unsupported-protocol"}};

        // LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnectionError to client id=%zu\n",
        //               client.m_id);
        LogPrintf("Send 0x02 SetupConnectionError to client id=%zu\n",
                        client.m_id);
        client.m_send_messages.emplace_back(setup_conn_err);

        client.m_disconnect_flag = true;
        LogPrintf("end SetupCpmmection line:%d\n",__LINE__);
        return;
    }

    // Disconnect a client if they are not running a compatible protocol version.
    if ((protocol_version < setup_conn.m_min_version) || (protocol_version > setup_conn.m_max_version)) {
        node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"protocol-version-mismatch"}};
        // LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnection.Error to client id=%zu\n",
        //               client.m_id);
        LogPrintf("Send 0x02 SetupConnection.Error to client id=%zu\n",
                        client.m_id);
        client.m_send_messages.emplace_back(setup_conn_err);

        // LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received a connection from client id=%zu with incompatible protocol_versions: min_version: %d, max_version: %d\n",
        //               client.m_id, setup_conn.m_min_version, setup_conn.m_max_version);
        LogPrintf("Received a connection from client id=%zu with incompatible protocol_versions: min_version: %d, max_version: %d\n",
                        client.m_id, setup_conn.m_min_version, setup_conn.m_max_version);
        client.m_disconnect_flag = true;
        LogPrintf("end SetupCpmmection line:%d\n",__LINE__);
        return;
    }

    // LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x01 SetupConnection.Success to client id=%zu\n",
    //               client.m_id);
    LogPrintf("Send 0x01 SetupConnection.Success to client id=%zu\n",
                    client.m_id);
    node::Sv2SetupConnectionSuccessMsg setup_success{protocol_version, optional_features};
    client.m_send_messages.emplace_back(setup_success);

    client.m_setup_connection_confirmed = true;
    LogPrintf("end SetupCpmmection m_setup_connection_confirmed:%d\n",client.m_setup_connection_confirmed);
}
