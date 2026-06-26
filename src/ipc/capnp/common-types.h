// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_CAPNP_COMMON_TYPES_H
#define BITCOIN_IPC_CAPNP_COMMON_TYPES_H

// NOTE: This header is included by Cap'n Proto–generated proxy-types files
// which compile as C++20.  TBC's CDataStream (from streams.h) cannot be used
// here because it depends on zero_after_free_allocator which uses deprecated
// C++17 allocator typedefs removed in C++20.  Instead we provide a minimal
// self-contained stream (IpcDataStream) that satisfies TBC's Serialize /
// Unserialize template requirements without pulling in streams.h.

#include <interfaces/types.h>        // interfaces::BlockRef
#include <primitives/transaction.h>  // CTransaction, CMutableTransaction, deserialize_type, deserialize
#include <serialize.h>               // SER_NETWORK, Serialize<>, Unserialize<>
#include <univalue.h>
#include <version.h>                 // PROTOCOL_VERSION

#include <mp/proxy-types.h>
#include <mp/type-data.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ipc::capnp {

//! Minimal C++20-safe byte stream for serializing TBC types via the Serialize()
//! / Unserialize() template mechanism.  Provides the write()/read() interface
//! needed by serialize.h without including streams.h (which uses a C++17-only
//! custom allocator incompatible with C++20).
struct IpcDataStream {
    std::vector<char> buf;
    std::size_t read_pos{0};
    int nType{0};
    int nVersion{0};

    IpcDataStream(int type, int version) : nType(type), nVersion(version) {}

    int GetType() const { return nType; }
    int GetVersion() const { return nVersion; }

    void write(const char* p, std::size_t n) {
        buf.insert(buf.end(), p, p + n);
    }

    void read(char* p, std::size_t n) {
        if (read_pos + n > buf.size()) {
            throw std::ios_base::failure("IpcDataStream::read(): end of data");
        }
        std::memcpy(p, buf.data() + read_pos, n);
        read_pos += n;
    }

    std::size_t size() const { return buf.size() - read_pos; }
    const char* data() const { return buf.data() + read_pos; }

    template <typename T>
    IpcDataStream& operator<<(const T& obj) {
        ::Serialize(*this, obj);
        return *this;
    }
    template <typename T>
    IpcDataStream& operator>>(T& obj) {
        ::Unserialize(*this, obj);
        return *this;
    }
};

} // namespace ipc::capnp

namespace mp {

//! Concepts for TBC serializable types.
//! Defined locally (C++20, legal in the bitcoin_ipc C++20 libraries).

template <typename T>
concept TbcSerializable = requires(const T& a, ipc::capnp::IpcDataStream& s) {
    a.Serialize(s);
};

template <typename T>
concept TbcUnserializable = requires(T& a, ipc::capnp::IpcDataStream& s) {
    a.Unserialize(s);
};

//! A type is TbcDeserializable if it can be constructed from
//! (deserialize_type, IpcDataStream&).  CTransaction satisfies this because
//! it delegates to CMutableTransaction(deserialize, s) which calls
//! UnserializeTransaction() using the stream's read() method.
template <typename T>
concept TbcDeserializable =
    std::is_constructible_v<T, deserialize_type, ipc::capnp::IpcDataStream&>;

//! Build: serialize any TBC-Serializable type into a capnp Data field.
//!
//! Overloads multiprocess library's CustomBuildField hook (Priority<1>).
//! The std::is_same_v guard prevents this overload from taking priority over
//! more-specific Priority<2> / type-data.h overloads for byte-span types.
template <typename LocalType, typename Value, typename Output>
void CustomBuildField(
    TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context,
    Value&& value, Output&& output)
requires TbcSerializable<std::remove_cv_t<std::remove_reference_t<LocalType>>>
{
    ipc::capnp::IpcDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    value.Serialize(stream);
    auto result = output.init(stream.buf.size());
    std::memcpy(result.begin(), stream.buf.data(), stream.buf.size());
}

//! Read: deserialize via Unserialize() for mutable types that are NOT
//! constructible via (deserialize_type, stream) — e.g. CMutableTransaction.
template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(
    TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context,
    Input&& input, ReadDest&& read_dest)
requires TbcUnserializable<LocalType> && (!TbcDeserializable<LocalType>)
{
    return read_dest.update([&](LocalType& value) {
        if (!input.has()) return;
        auto data = input.get();
        ipc::capnp::IpcDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream.write(reinterpret_cast<const char*>(data.begin()), data.size());
        value.Unserialize(stream);
    });
}

//! Read: deserialize via the (deserialize, stream) constructor for immutable
//! types — specifically CTransaction which has const fields and no Unserialize.
template <typename LocalType, typename Input, typename ReadDest>
decltype(auto) CustomReadField(
    TypeList<LocalType>, Priority<1>, InvokeContext& invoke_context,
    Input&& input, ReadDest&& read_dest)
requires TbcDeserializable<LocalType>
{
    assert(input.has());
    auto data = input.get();
    ipc::capnp::IpcDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream.write(reinterpret_cast<const char*>(data.begin()), data.size());
    return read_dest.construct(::deserialize, stream);
}

//! UniValue crosses as its JSON string.
template <typename Value, typename Output>
void CustomBuildField(
    TypeList<UniValue>, Priority<1>, InvokeContext& invoke_context,
    Value&& value, Output&& output)
{
    std::string str = value.write();
    auto result = output.init(str.size());
    std::memcpy(result.begin(), str.data(), str.size());
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(
    TypeList<UniValue>, Priority<1>, InvokeContext& invoke_context,
    Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](UniValue& value) {
        auto data = input.get();
        value.read(std::string{reinterpret_cast<const char*>(data.begin()), data.size()});
    });
}

} // namespace mp

#endif // BITCOIN_IPC_CAPNP_COMMON_TYPES_H
