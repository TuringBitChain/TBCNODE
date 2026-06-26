// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Placeholder so the bitcoin_ipc target exists when ENABLE_IPC is OFF, keeping
// downstream link lines stable. Replaced by real sources when ENABLE_IPC is ON.
namespace ipc { void EnableMarker() {} }
