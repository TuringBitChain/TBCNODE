// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/signal_dispatcher.h"

namespace tbc {
namespace validation {

// v2.6.1 M2 全局实例
// init.cpp 启动时调 Start()，Shutdown 早期调 Stop()
SignalDispatcher g_signal_dispatcher;

} // namespace validation
} // namespace tbc
