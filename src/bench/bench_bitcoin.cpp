// Copyright (c) 2015-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "bench.h"

#include "chainparams.h"
#include "config.h"
#include "crypto/sha256.h"
#include "key.h"
#include "random.h"
#include "util.h"
#include "validation.h"

int main(int argc, char **argv) {
    SHA256AutoDetect();
    RandomInit();
    ECC_Start();
    SetupEnvironment();
    // P4.3: 让需要 BlockSizeParams 的 bench (BatchWriteContention 等) 能跑
    SelectParams(CBaseChainParams::MAIN);
    GlobalConfig::GetConfig().SetDefaultBlockSizeParams(
        Params().GetDefaultBlockSizeParams());

    // don't want to write to bitcoind.log file
    GetLogger().fPrintToDebugLog = false;

    benchmark::BenchRunner::RunAll();

    ECC_Stop();
}
