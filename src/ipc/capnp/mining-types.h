// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_IPC_CAPNP_MINING_TYPES_H
#define BITCOIN_IPC_CAPNP_MINING_TYPES_H

#include <amount.h>
#include <interfaces/mining.h>
#include <ipc/capnp/common.capnp.proxy-types.h>
#include <ipc/capnp/common-types.h>
#include <ipc/capnp/mining.capnp.proxy.h>
#include <node/miner.h>
#include <node/mining_types.h>

#include <mp/proxy-types.h>
#include <mp/type-chrono.h>
#include <mp/type-context.h>
#include <mp/type-data.h>
#include <mp/type-decay.h>
#include <mp/type-interface.h>
#include <mp/type-number.h>
#include <mp/type-optional.h>
#include <mp/type-pointer.h>
#include <mp/type-string.h>
#include <mp/type-struct.h>
#include <mp/type-threadmap.h>
#include <mp/type-vector.h>
#include <mp/type-void.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace mp {

//! Map TBC Amount to/from capnp Int64 via GetSatoshis()/int64_t constructor.
//! Priority<2> ensures this wins over the generic TbcSerializable/TbcUnserializable
//! overloads in common-types.h (which are at Priority<1> and would incorrectly try
//! to serialize Amount as a binary blob into a Data field rather than Int64).
template <typename Value, typename Output>
void CustomBuildField(TypeList<Amount>, Priority<2>, InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    output.set(static_cast<int64_t>(value.GetSatoshis()));
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<Amount>, Priority<2>, InvokeContext& invoke_context,
                               Input&& input, ReadDest&& read_dest)
{
    return read_dest.construct(Amount(input.get()));
}

//! Cross std::chrono::milliseconds as a capnp Float64 (milliseconds, matching upstream
//! MillisecondsDouble), overflow-safe: out-of-int64-ms-range Float64 (e.g. the maxDouble
//! "forever" default) maps to milliseconds::max(); milliseconds::max() builds as maxDouble.
//!
//! Priority<2> beats the generic duration<Rep,Period> overload at Priority<1> in
//! mp/type-chrono.h because Priority<2> derives from Priority<1> (more specific
//! concrete type + higher priority tag => selected by overload resolution).
template <typename Value, typename Output>
void CustomBuildField(TypeList<std::chrono::milliseconds>, Priority<2>, InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    if (value >= std::chrono::milliseconds::max()) {
        output.set(std::numeric_limits<double>::max());
        return;
    }
    output.set(static_cast<double>(value.count()));
}

template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<std::chrono::milliseconds>, Priority<2>, InvokeContext& invoke_context,
                               Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](std::chrono::milliseconds& value) {
        const double ms = input.get();
        constexpr double kMaxMs = static_cast<double>(std::chrono::milliseconds::max().count());
        value = (ms >= kMaxMs) ? std::chrono::milliseconds::max()
                               : std::chrono::milliseconds(static_cast<int64_t>(ms));
    });
}

} // namespace mp

#endif // BITCOIN_IPC_CAPNP_MINING_TYPES_H
