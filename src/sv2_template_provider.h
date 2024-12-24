#ifndef BITCOIN_NODE_SV2_TEMPLATE_PROVIDER_H
#define BITCOIN_NODE_SV2_TEMPLATE_PROVIDER_H

#include <chrono>
#include <stratumv2/sv2_noise.h>
#include <stratumv2/sv2_messages.h>
#include <stratumv2/sv2_transport.h>
#include <stratumv2/sv2_connman.h>
#include <logging.h>
#include "net/net.h"
//#include <node/miner.h>
//#include <util/time.h>
#include <streams.h>
#include "mining/assembler.h"
#include "util/sock.h"
#include "config.h"
#include "mining/mining.h"

class CTxMemPool;

struct Sv2TemplateProviderOptions
{
    /**
     * Host for the server to bind to.
     */
    std::string host{"127.0.0.1"};

    /**
     * The listening port for the server.
     */
    uint16_t port;

    /**
     * The current protocol version of stratum v2 supported by the server. Not to be confused
     * with byte value of identitying the stratum v2 subprotocol.
     */
    uint16_t protocol_version = 2;

    /**
     * Optional protocol features provided by the server.
     */
    uint16_t optional_features = 0;

    /**
     * The default option for the additional space required for coinbase output.
     */
    unsigned int default_coinbase_tx_additional_output_size = 0;

    /**
     * The default flag for all new work.
     */
    bool default_future_templates = true;

    /**
     * Minimum fee delta to send new template upstream
     */
    Amount fee_delta{1000};

    /**
     * Block template update interval (to check for increased fees)
     */
    std::chrono::seconds fee_check_interval{30};
};

/**
 * The main class that runs the template provider server.
 */
class Sv2TemplateProvider : public Sv2EventsInterface
{
    using Sv2NetMsg = node::Sv2NetMsg;
private:
    /**
     * The template provider subprotocol used in setup connection messages. The stratum v2
     * template provider only recognizes its own subprotocol.
     */
    static constexpr uint8_t TP_SUBPROTOCOL{0x02};

    CKey m_static_key;

    std::optional<Sv2SignatureNoiseMessage> m_certificate;

    /** Get name of file to store static key */
    fs::path GetStaticKeyFile();

    /** Get name of file to store authority key */
    fs::path GetAuthorityKeyFile();

    /**
     * The main listening socket for new stratum v2 connections.
     */
    std::shared_ptr<Sock> m_listening_socket;

    /**
    * Minimum fee delta required before submitting an updated template.
    * This may be negative.
    */
    int m_minimum_fee_delta;

    /**
     * The main thread for the template provider.
     */
    std::thread m_thread_sv2_handler;

    /**
     * Signal for handling interrupts and stopping the template provider event loop.
     */
    std::atomic<bool> m_flag_interrupt_sv2{false};
    CThreadInterrupt m_interrupt_sv2;

    /**
     * A list of all connected stratum v2 clients.
     */
    using Clients = std::vector<std::unique_ptr<Sv2Client>>;
    Clients m_sv2_clients GUARDED_BY(m_clients_mutex);

    /**
     * The most recent template id. This is incremented on creating new template,
     * which happens for each connected client.
     */
    uint64_t m_template_id GUARDED_BY(m_tp_mutex){0};

    /**
     * The current best known block hash in the network.
     */
    uint256 m_best_prev_hash GUARDED_BY(m_tp_mutex);

    /** When we last saw a new block connection. Used to cache stale templates
      * for some time after this.
      */
    std::chrono::nanoseconds m_last_block_time GUARDED_BY(m_tp_mutex);

    /**
     * A cache that maps ids used in NewTemplate messages and its associated block template.
     */
    using BlockTemplateCache = std::map<uint64_t, std::unique_ptr<mining::CBlockTemplate>>;
    BlockTemplateCache m_block_template_cache GUARDED_BY(m_tp_mutex);

    /**
     * The currently supported protocol version.
     */
    uint16_t m_protocol_version;

    /**
     * The currently supported optional features.
     */
    uint16_t m_optional_features;

    /**
     * The default additional size output required for NewTemplates.
     */
    unsigned int m_default_coinbase_tx_additional_output_size;

    /**
     * The default setting for sending future templates.
     */
    bool m_default_future_templates;

    /**
     * The configured port to listen for new connections.
     */
    uint16_t m_port;

public:

    Mutex m_clients_mutex;
    Mutex m_tp_mutex;

    /**
    * ChainstateManager and CTxMemPool are both used to build new valid blocks,
    * getting the best known block hash and checking whether the node is still
    * in IBD.
    */
    //ChainstateManager& m_chainman;        //TODO
    //CTxMemPool& m_mempool;
    CTxMemPool& m_mempool;

    XOnlyPubKey m_authority_pubkey;

    //explicit Sv2TemplateProvider(Config &config, CTxMemPool& mempool, Mining& mining);
    explicit Sv2TemplateProvider(Config &config, Mining& mining, CTxMemPool& mempool);

    ~Sv2TemplateProvider();
    /**
     * Starts the template provider server and thread.
     * returns false if port is unable to bind.
     */
    [[nodiscard]] bool Start(const Sv2TemplateProviderOptions& options);

    /**
     * Triggered on interrupt signals to stop the main event loop in ThreadSv2Handler().
     */
    void Interrupt();

    /**
     * Tear down of the template provider thread and any other necessary tear down.
     */
    void StopThreads();

    /**
     *  Helper function to process incoming bytes before a session is established.
     *  Progresses a handshake or fails.
     *
     *  @throws std::runtime_error if any point of the handshake, encryption/decryption
     *  fails.
     */
    void ProcessMaybeSv2Handshake(Sv2Client& client, bsv::span<std::byte> buffer);

    /** Number of clients that are not marked for disconection, used for tests. */
    size_t ConnectedClients() EXCLUSIVE_LOCKS_REQUIRED(m_clients_mutex)
    {
        return std::count_if(m_sv2_clients.begin(), m_sv2_clients.end(), [](const auto& c) {
            return !c->m_disconnect_flag;
        });
    }

    /** Number of clients with m_setup_connection_confirmed, used for tests. */
    size_t FullyConnectedClients() EXCLUSIVE_LOCKS_REQUIRED(m_clients_mutex)
    {
        return std::count_if(m_sv2_clients.begin(), m_sv2_clients.end(), [](const auto& c) {
            return !c->m_disconnect_flag && c->m_setup_connection_confirmed;
        });
    }

    void RequestTransactionData(Sv2Client& client, node::Sv2RequestTransactionDataMsg msg) EXCLUSIVE_LOCKS_REQUIRED(!m_tp_mutex) override;

    void SubmitSolution(node::Sv2SubmitSolutionMsg solution) EXCLUSIVE_LOCKS_REQUIRED(!m_tp_mutex) override;

    void CoinbaseOutputDataSize(Sv2Client& client, node::Sv2CoinbaseOutputDataSizeMsg coinbase_tx_outputs_size) EXCLUSIVE_LOCKS_REQUIRED(!m_tp_mutex) override;

    void SetupCpmmection(Sv2Client& client
        , node::Sv2SetupConnectionMsg setup_conn
        , const uint16_t protocol_version
        , const uint8_t subprotocol
        , const uint16_t optional_features) EXCLUSIVE_LOCKS_REQUIRED(!m_tp_mutex) override;

    /* Block templates that connected clients may be working on */
    BlockTemplateCache& GetBlockTemplates() { return m_block_template_cache; }

private:
    void Init(const Sv2TemplateProviderOptions& options);

    /**
     * Creates a socket and binds the port for new stratum v2 connections.
     * @throws std::runtime_error if port is unable to bind.
     */
    [[nodiscard]] std::shared_ptr<Sock> BindListenPort(uint16_t port) const;

    void DisconnectFlagged() EXCLUSIVE_LOCKS_REQUIRED(m_clients_mutex);

    /**
     * The main thread for the template provider, contains an event loop handling
     * all tasks for the template provider.
     */
    void ThreadSv2Handler() EXCLUSIVE_LOCKS_REQUIRED(!m_clients_mutex, !m_tp_mutex);

    /**
     * Secondary thread for the template provider, contains an event loop handling
     * mempool updates.
     */
    void ThreadSv2MempoolHandler() EXCLUSIVE_LOCKS_REQUIRED(!m_tp_mutex);

    /**
     * NewWorkSet contains the messages matching block for valid stratum v2 work.
     */
    struct NewWorkSet
    {
        node::Sv2NewTemplateMsg new_template;
        std::unique_ptr<mining::CBlockTemplate> block_template;
        node::Sv2SetNewPrevHashMsg prev_hash;
    };

    /**
     * Builds a NewWorkSet that contains the Sv2NewTemplateMsg, a new full block and a Sv2SetNewPrevHashMsg that are all linked to the same work.
     */
    [[nodiscard]] bool BuildNewWorkSet(bool future_template, unsigned int coinbase_output_max_additional_size, NewWorkSet& newWorkSet) EXCLUSIVE_LOCKS_REQUIRED(m_tp_mutex);

    /* Forget templates from before the last block, but with a few seconds margin. */
    void PruneBlockTemplateCache() EXCLUSIVE_LOCKS_REQUIRED(m_tp_mutex);

    /**
     * Sends the best NewTemplate and SetNewPrevHash to a client.
     */
    [[nodiscard]] bool SendWork(Sv2Client& client, bool send_new_prevhash, Amount& fees_before) EXCLUSIVE_LOCKS_REQUIRED(m_tp_mutex);

    /**
     * Generates the socket events for each Sv2Client socket and the main listening socket.
     */
    [[nodiscard]] Sock::EventsPerSock GenerateWaitSockets(const std::shared_ptr<Sock>& listen_socket, const Clients& sv2_clients) const;

    /**
     * Encrypt the header and message payload and send it.
     * @throws std::runtime_error if encrypting the message fails.
     */
    bool EncryptAndSendMessage(Sv2Client& client, Sv2NetMsg& net_msg);

    /**
     * A helper method to read and decrypt multiple Sv2NetMsgs.
     */
    std::vector<Sv2NetMsg> ReadAndDecryptSv2NetMsgs(Sv2Client& client, bsv::span<std::byte> buffer);

private:

    const Config& m_config;

    /**
    * The Mining interface is used to build new valid blocks, get the best known
    * block hash and to check whether the node is still in IBD.
    */
    Mining& m_mining;

    std::unique_ptr<Sv2Connman> m_connman;

    /**
    * Configuration
    */
    Sv2TemplateProviderOptions m_options;


    /**
     * The secondary thread for the template provider.
     */
    std::thread m_thread_sv2_mempool_handler;

    /**
     * Last time we created a new template
     */
    std::chrono::milliseconds m_template_last_update{0};

};

#endif // BITCOIN_NODE_SV2_TEMPLATE_PROVIDER_H
