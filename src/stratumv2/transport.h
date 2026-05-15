// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMMON_TRANSPORT_H
#define BITCOIN_COMMON_TRANSPORT_H

#include <chrono>
#include <streams.h>
#include <uint256.h>

#include "net/net.h"
#include "net/net_message.h"
#include "sv2_messages.h"

/** Transport layer version */
enum class TransportProtocolType : uint8_t {
    DETECTING, //!< Peer could be v1 or v2
    V1, //!< Unencrypted, plaintext protocol
    V2, //!< BIP324 protocol
};

/** Convert TransportProtocolType enum to a string value */
std::string TransportTypeAsString(TransportProtocolType transport_type);

/** Maximum length of incoming protocol messages (no message over 4 MB is currently acceptable). */
static const unsigned int MAX_PROTOCOL_MESSAGE_LENGTH = 4 * 1000 * 1000;

typedef int64_t NodeId;

/** The Transport converts one connection's sent messages to wire bytes, and received bytes back. */
class Transport {
public:
    virtual ~Transport() = default;

    struct Info
    {
        TransportProtocolType transport_type;
        std::optional<uint256> session_id;
    };

    /** Retrieve information about this transport. */
    virtual Info GetInfo() const noexcept = 0;

    // 1. Receiver side functions, for decoding bytes received on the wire into transport protocol
    // agnostic CNetMessage (message type & payload) objects.

    /** Returns true if the current message is complete (so GetReceivedMessage can be called). */
    virtual bool ReceivedMessageComplete() const = 0;

    /** Feed wire bytes to the transport.
     *
     * @return false if some bytes were invalid, in which case the transport can't be used anymore.
     *
     * Consumed bytes are chopped off the front of msg_bytes.
     */
    virtual bool ReceivedBytes(bsv::span<const uint8_t>& msg_bytes) = 0;

    /** Retrieve a completed message from transport.
     *
     * This can only be called when ReceivedMessageComplete() is true.
     *
     * If reject_message=true is returned the message itself is invalid, but (other than false
     * returned by ReceivedBytes) the transport is not in an inconsistent state.
     */
    virtual node::Sv2NetMsg& GetReceivedMessage(std::chrono::microseconds time, bool& reject_message) noexcept = 0;

    // 2. Sending side functions, for converting messages into bytes to be sent over the wire.

    /** Report how many bytes have been sent. If bytes_sent=0, this call has no effect. */
    virtual void MarkBytesSent(size_t bytes_sent) noexcept = 0;

    /** Return the memory usage of this transport attributable to buffered data to send. */
    virtual size_t GetSendMemoryUsage() const noexcept = 0;

    // 3. Miscellaneous functions.

    /** Whether upon disconnections, a reconnect with V1 is warranted. */
    virtual bool ShouldReconnectV1() const noexcept = 0;
};

#endif // BITCOIN_COMMON_TRANSPORT_H
