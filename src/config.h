// Copyright (c) 2017 Amaury SÉCHET
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#ifndef BITCOIN_CONFIG_H
#define BITCOIN_CONFIG_H

static_assert(sizeof(void*) >= 8, "32 bit systems are not supported");

#include "amount.h"
#include "consensus/consensus.h"
#include "mining/factory.h"
#include "net/net.h"
#include "policy/policy.h"
#include "script/standard.h"
#include "txn_validation_config.h"
#include "validation.h"
#include "script_config.h"

#include <boost/noncopyable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <orphan_txns.h>

class CChainParams;
struct DefaultBlockSizeParams;

class Config : public boost::noncopyable, public CScriptConfig {
public:
    // used to specify default block size related parameters
    virtual void SetDefaultBlockSizeParams(const DefaultBlockSizeParams &params) = 0;
    
    virtual bool SetMaxBlockSize(uint64_t maxBlockSize, std::string* err = nullptr) = 0;
    virtual uint64_t GetMaxBlockSize() const = 0;
    
    virtual bool SetMaxGeneratedBlockSize(uint64_t maxGeneratedBlockSize, std::string* err = nullptr) = 0;
    virtual uint64_t GetMaxGeneratedBlockSize() const = 0;
    virtual uint64_t GetMaxGeneratedBlockSize(int64_t nMedianTimePast) const = 0;
    virtual bool MaxGeneratedBlockSizeOverridden() const = 0;

    virtual bool SetBlockSizeActivationTime(int64_t activationTime, std::string* err = nullptr) = 0;
    virtual int64_t GetBlockSizeActivationTime() const = 0;

    virtual bool SetBlockPriorityPercentage(int64_t blockPriorityPercentage, std::string* err = nullptr) = 0;
    virtual uint8_t GetBlockPriorityPercentage() const = 0;
    virtual const CChainParams &GetChainParams() const = 0;

    virtual bool SetMaxTxSizePolicy(int64_t value, std::string* err = nullptr) = 0;
    virtual uint64_t GetMaxTxSize(bool isGenesisEnabled, bool isConsensus) const = 0;

    virtual bool SetMinConsolidationFactor(uint64_t value, std::string* err = nullptr) = 0;
    virtual uint64_t GetMinConsolidationFactor() const = 0;

    virtual bool SetMaxConsolidationInputScriptSize(uint64_t value, std::string* err = nullptr) = 0;
    virtual uint64_t GetMaxConsolidationInputScriptSize() const = 0;

    virtual bool SetMinConsolidationInputMaturity(uint64_t value, std::string* err = nullptr) = 0;
    virtual uint64_t GetMinConsolidationInputMaturity() const = 0;

    virtual bool SetAcceptNonStdConsolidationInput(uint64_t value, std::string* err = nullptr) = 0;
    virtual bool GetAcceptNonStdConsolidationInput() const = 0;

    virtual void SetMinFeePerKB(CFeeRate amt) = 0;
    virtual CFeeRate GetMinFeePerKB() const = 0;

    virtual void SetBlockMinFeePerKB(CFeeRate amt) = 0;
    virtual CFeeRate GetBlockMinFeePerKB() const = 0;

    virtual void SetPreferredBlockFileSize(uint64_t preferredBlockFileSize) = 0;
    virtual uint64_t GetPreferredBlockFileSize() const = 0;

    virtual void SetDataCarrierSize(uint64_t dataCarrierSize) = 0;
    virtual uint64_t GetDataCarrierSize() const = 0;

    virtual void SetLimitAncestorSize(uint64_t limitAncestorSize) = 0;
    virtual uint64_t GetLimitAncestorSize() const = 0;

    virtual void SetLimitDescendantSize(uint64_t limitDescendantSize) = 0;
    virtual uint64_t GetLimitDescendantSize() const = 0;

    virtual void SetLimitAncestorCount(uint64_t limitAncestorCount) = 0;
    virtual uint64_t GetLimitAncestorCount() const = 0;

    virtual void SetLimitDescendantCount(uint64_t limitDescendantCount) = 0;
    virtual uint64_t GetLimitDescendantCount() const = 0;

    virtual void SetTestBlockCandidateValidity(bool test) = 0;
    virtual bool GetTestBlockCandidateValidity() const = 0;

    virtual void SetFactorMaxSendQueuesBytes(uint64_t factorMaxSendQueuesBytes) = 0;
    virtual uint64_t GetFactorMaxSendQueuesBytes() const = 0;
    virtual uint64_t GetMaxSendQueuesBytes() const = 0; // calculated based on factorMaxSendQueuesBytes

    virtual void SetMiningCandidateBuilder(mining::CMiningFactory::BlockAssemblerType type) = 0;
    virtual mining::CMiningFactory::BlockAssemblerType GetMiningCandidateBuilder() const = 0;

    virtual bool SetGenesisActivationHeight(int64_t genesisActivationHeightIn, std::string* err = nullptr) = 0;
    virtual uint64_t GetGenesisActivationHeight() const = 0;

    virtual bool SetMaxConcurrentAsyncTasksPerNode(
        int maxConcurrentAsyncTasksPerNode,
        std::string* error = nullptr) = 0;
    virtual int GetMaxConcurrentAsyncTasksPerNode() const = 0;

    virtual bool SetBlockScriptValidatorsParams(
        int maxParallelBlocks,
        int perValidatorThreadsCount,
        int perValidatorThreadMaxBatchSize,
        std::string* error = nullptr) = 0;
    virtual int GetMaxParallelBlocks() const = 0;
    virtual int GetPerBlockScriptValidatorThreadsCount() const = 0;
    virtual int GetPerBlockScriptValidationMaxBatchSize() const = 0;

    virtual bool SetMaxOpsPerScriptPolicy(int64_t maxOpsPerScriptPolicyIn, std::string* error) = 0;

    /** Sets the maximum policy number of sigops we're willing to relay/mine in a single tx */
    virtual bool SetMaxTxSigOpsCountPolicy(int64_t maxTxSigOpsCountIn, std::string* err = nullptr) = 0;
    virtual uint64_t GetMaxTxSigOpsCountConsensusBeforeGenesis() const = 0;
    virtual uint64_t GetMaxTxSigOpsCountPolicy(bool isGenesisEnabled) const = 0;

    virtual uint64_t GetMaxBlockSigOpsConsensusBeforeGenesis(uint64_t blockSize) const = 0;

    virtual bool SetMaxPubKeysPerMultiSigPolicy(int64_t maxPubKeysPerMultiSigIn, std::string* err = nullptr) = 0;

    virtual bool SetMaxStdTxnValidationDuration(int ms, std::string* err = nullptr) = 0;
    virtual std::chrono::milliseconds GetMaxStdTxnValidationDuration() const = 0;

    virtual bool SetMaxNonStdTxnValidationDuration(int ms, std::string* err = nullptr) = 0;
    virtual std::chrono::milliseconds GetMaxNonStdTxnValidationDuration() const = 0;

    virtual bool SetMaxStackMemoryUsage(int64_t maxStackMemoryUsageConsensusIn, int64_t maxStackMemoryUsagePolicyIn, std::string* err = nullptr) = 0;

    virtual bool SetMaxScriptSizePolicy(int64_t maxScriptSizePolicyIn, std::string* err = nullptr) = 0;

    virtual bool SetMaxScriptNumLengthPolicy(int64_t maxScriptNumLengthIn, std::string* err = nullptr) = 0;

    virtual bool SetGenesisGracefulPeriod(int64_t genesisGracefulPeriodIn, std::string* err = nullptr) = 0;
    virtual uint64_t GetGenesisGracefulPeriod() const = 0;

    virtual void SetAcceptNonStandardOutput(bool accept) = 0;
    virtual bool GetAcceptNonStandardOutput(bool isGenesisEnabled) const = 0;

    virtual bool SetMaxCoinsViewCacheSize(int64_t max, std::string* err) = 0;
    virtual uint64_t GetMaxCoinsViewCacheSize() const = 0;

    virtual void SetInvalidBlocks(const std::set<uint256>& hashes) = 0;
    virtual const std::set<uint256>& GetInvalidBlocks() const = 0;
    virtual bool IsBlockInvalidated(const uint256& hash) const = 0;

    virtual void SetBanClientUA(const std::set<std::string> uaClients) = 0;
    virtual bool IsClientUABanned(const std::string uaClient) const = 0;

    virtual bool SetMaxMempool(int64_t maxMempool, std::string* err) = 0;
    virtual uint64_t GetMaxMempool() const = 0;

    virtual bool SetMemPoolExpiry(int64_t memPoolExpiry, std::string* err) = 0;
    virtual uint64_t GetMemPoolExpiry() const = 0;

    virtual bool SetLimitFreeRelay(int64_t limitFreeRelay, std::string* err) = 0;
    virtual uint64_t GetLimitFreeRelay() const = 0;

    virtual bool SetMaxOrphanTxSize(int64_t maxOrphanTxSize, std::string* err) = 0;
    virtual uint64_t GetMaxOrphanTxSize() const = 0;

    virtual bool SetStopAtHeight(int64_t StopAtHeight, std::string* err) = 0;
    virtual uint64_t GetStopAtHeight() const = 0;

    virtual bool SetPromiscuousMempoolFlags(int64_t promiscuousMempoolFlags, std::string* err) = 0;
    virtual uint64_t GetPromiscuousMempoolFlags() const = 0;
    virtual bool IsSetPromiscuousMempoolFlags() const = 0;

protected:
    ~Config() = default;
};

class GlobalConfig final : public Config {
public:
    GlobalConfig();

    // Set block size related default. This must be called after constructing GlobalConfig
    void SetDefaultBlockSizeParams(const DefaultBlockSizeParams &params) override;

    bool SetMaxBlockSize(uint64_t maxBlockSize, std::string* err = nullptr) override;
    uint64_t GetMaxBlockSize() const override;

    bool SetMaxGeneratedBlockSize(uint64_t maxGeneratedBlockSize, std::string* err = nullptr) override;
    uint64_t GetMaxGeneratedBlockSize() const override;   
    uint64_t GetMaxGeneratedBlockSize(int64_t nMedianTimePast) const override;
    bool MaxGeneratedBlockSizeOverridden() const override;

    bool SetBlockSizeActivationTime(int64_t activationTime, std::string* err = nullptr) override;
    int64_t GetBlockSizeActivationTime() const override;

    bool SetBlockPriorityPercentage(int64_t blockPriorityPercentage, std::string* err = nullptr) override;
    uint8_t GetBlockPriorityPercentage() const override;
    const CChainParams &GetChainParams() const override;

    bool SetMaxTxSizePolicy(int64_t value, std::string* err = nullptr) override;
    uint64_t GetMaxTxSize(bool isGenesisEnabled, bool isConsensus) const  override;

    bool SetMinConsolidationFactor(uint64_t value, std::string* err = nullptr) override;
    uint64_t GetMinConsolidationFactor() const  override;

    bool SetMaxConsolidationInputScriptSize(uint64_t value, std::string* err = nullptr) override;
    uint64_t GetMaxConsolidationInputScriptSize() const  override;

    bool SetMinConsolidationInputMaturity(uint64_t value, std::string* err = nullptr) override;
    uint64_t GetMinConsolidationInputMaturity() const  override;

    bool SetAcceptNonStdConsolidationInput(uint64_t value, std::string* err = nullptr) override;
    bool GetAcceptNonStdConsolidationInput() const  override;

    void SetMinFeePerKB(CFeeRate amt) override;
    CFeeRate GetMinFeePerKB() const override;

    void SetBlockMinFeePerKB(CFeeRate amt) override;
    CFeeRate GetBlockMinFeePerKB() const override;

    void SetPreferredBlockFileSize(uint64_t preferredBlockFileSize) override;
    uint64_t GetPreferredBlockFileSize() const override;

    void SetDataCarrierSize(uint64_t dataCarrierSize) override;
    uint64_t GetDataCarrierSize() const override;

    void SetLimitAncestorSize(uint64_t limitAncestorSize) override;
    uint64_t GetLimitAncestorSize() const override;

    void SetLimitDescendantSize(uint64_t limitDescendantSize) override;
    uint64_t GetLimitDescendantSize() const override;

    void SetLimitAncestorCount(uint64_t limitAncestorCount) override;
    uint64_t GetLimitAncestorCount() const override;

    void SetLimitDescendantCount(uint64_t limitDescendantCount) override;
    uint64_t GetLimitDescendantCount() const override;

    void SetTestBlockCandidateValidity(bool test) override;
    bool GetTestBlockCandidateValidity() const override;

    void SetFactorMaxSendQueuesBytes(uint64_t factorMaxSendQueuesBytes) override;
    uint64_t GetFactorMaxSendQueuesBytes() const override;
    uint64_t GetMaxSendQueuesBytes() const override;

    void SetMiningCandidateBuilder(mining::CMiningFactory::BlockAssemblerType type) override;
    mining::CMiningFactory::BlockAssemblerType GetMiningCandidateBuilder() const override;

    bool SetGenesisActivationHeight(int64_t genesisActivationHeightIn, std::string* err = nullptr) override;
    uint64_t GetGenesisActivationHeight() const override;

    bool SetMaxConcurrentAsyncTasksPerNode(
        int maxConcurrentAsyncTasksPerNode,
        std::string* error = nullptr) override;
    int GetMaxConcurrentAsyncTasksPerNode() const override;

    bool SetBlockScriptValidatorsParams(
        int maxParallelBlocks,
        int perValidatorThreadsCount,
        int perValidatorThreadMaxBatchSize,
        std::string* error = nullptr) override;
    int GetMaxParallelBlocks() const override;
    int GetPerBlockScriptValidatorThreadsCount() const override;
    int GetPerBlockScriptValidationMaxBatchSize() const override;

    bool SetMaxOpsPerScriptPolicy(int64_t maxOpsPerScriptPolicyIn, std::string* error) override;
    uint64_t GetMaxOpsPerScript(bool isGenesisEnabled, bool consensus) const override;

    bool SetMaxTxSigOpsCountPolicy(int64_t maxTxSigOpsCountIn, std::string* err = nullptr) override;
    uint64_t GetMaxTxSigOpsCountConsensusBeforeGenesis() const override;
    uint64_t GetMaxTxSigOpsCountPolicy(bool isGenesisEnabled) const override;

    uint64_t GetMaxBlockSigOpsConsensusBeforeGenesis(uint64_t blockSize) const override;

    bool SetMaxPubKeysPerMultiSigPolicy(int64_t maxPubKeysPerMultiSigIn, std::string* error = nullptr) override;
    uint64_t GetMaxPubKeysPerMultiSig(bool isGenesisEnabled, bool consensus) const override;

    bool SetMaxStdTxnValidationDuration(int ms, std::string* err = nullptr) override;
    std::chrono::milliseconds GetMaxStdTxnValidationDuration() const override;

    bool SetMaxNonStdTxnValidationDuration(int ms, std::string* err = nullptr) override;
    std::chrono::milliseconds GetMaxNonStdTxnValidationDuration() const override;

    bool SetMaxStackMemoryUsage(int64_t maxStackMemoryUsageConsensusIn, int64_t maxStackMemoryUsagePolicyIn, std::string* err = nullptr) override;
    uint64_t GetMaxStackMemoryUsage(bool isGenesisEnabled, bool consensus) const override;

    
    bool SetMaxScriptSizePolicy(int64_t maxScriptSizePolicyIn, std::string* err = nullptr) override;
    uint64_t GetMaxScriptSize(bool isGenesisEnabled, bool isConsensus) const override;

    bool SetMaxScriptNumLengthPolicy(int64_t maxScriptNumLengthIn, std::string* err = nullptr) override;
    uint64_t GetMaxScriptNumLength(bool isGenesisEnabled, bool isConsensus) const override;

    bool SetGenesisGracefulPeriod(int64_t genesisGracefulPeriodIn, std::string* err = nullptr) override;
    uint64_t GetGenesisGracefulPeriod() const override;

    void SetAcceptNonStandardOutput(bool accept) override;
    bool GetAcceptNonStandardOutput(bool isGenesisEnabled) const override;

    bool SetMaxCoinsViewCacheSize(int64_t max, std::string* err) override;
    uint64_t GetMaxCoinsViewCacheSize() const override {return mMaxCoinsViewCacheSize;}

    void SetInvalidBlocks(const std::set<uint256>& hashes) override; 
    const std::set<uint256>& GetInvalidBlocks() const override;
    bool IsBlockInvalidated(const uint256& hash) const override;

    void SetBanClientUA(const std::set<std::string> uaClients) override;
    bool IsClientUABanned(const std::string uaClient) const override;

    bool SetMaxMempool(int64_t maxMempool, std::string* err) override;
    uint64_t GetMaxMempool() const override;

    bool SetMemPoolExpiry(int64_t memPoolExpiry, std::string* err) override;
    uint64_t GetMemPoolExpiry() const override;

    bool SetLimitFreeRelay(int64_t limitFreeRelay, std::string* err) override;
    uint64_t GetLimitFreeRelay() const override;

    bool SetMaxOrphanTxSize(int64_t maxOrphanTxSize, std::string* err) override;
    uint64_t GetMaxOrphanTxSize() const override;

    bool SetStopAtHeight(int64_t stopAtHeight, std::string* err) override;
    uint64_t GetStopAtHeight() const override;

    bool SetPromiscuousMempoolFlags(int64_t promiscuousMempoolFlags, std::string* err) override;
    uint64_t GetPromiscuousMempoolFlags() const override;
    bool IsSetPromiscuousMempoolFlags() const override;

    // Reset state of this object to match a newly constructed one. 
    // Used in constructor and for unit testing to always start with a clean state
    void Reset(); 
    static GlobalConfig& GetConfig();

private:
    // All fileds are initialized in Reset()    
    CFeeRate feePerKB;
    CFeeRate blockMinFeePerKB;
    uint64_t blockPriorityPercentage;
    uint64_t preferredBlockFileSize;
    uint64_t factorMaxSendQueuesBytes;

    // Block size limits 
    // SetDefaultBlockSizeParams must be called before reading any of those
    bool  setDefaultBlockSizeParamsCalled;
    void  CheckSetDefaultCalled() const;

    // Defines when either maxGeneratedBlockSizeBefore or maxGeneratedBlockSizeAfter is used
    int64_t blockSizeActivationTime;
    uint64_t maxBlockSize;
    // Used when SetMaxBlockSize is called with value 0
    uint64_t defaultBlockSize;
    uint64_t maxGeneratedBlockSizeBefore;
    uint64_t maxGeneratedBlockSizeAfter;
    bool maxGeneratedBlockSizeOverridden;

    uint64_t maxTxSizePolicy;
    uint64_t minConsolidationFactor;
    uint64_t maxConsolidationInputScriptSize;
    uint64_t minConsolidationInputMaturity;
    uint64_t acceptNonStdConsolidationInput;
    uint64_t dataCarrierSize;
    uint64_t limitDescendantCount;
    uint64_t limitAncestorCount;
    uint64_t limitDescendantSize;
    uint64_t limitAncestorSize;

    bool testBlockCandidateValidity;
    mining::CMiningFactory::BlockAssemblerType blockAssemblerType;

    uint64_t genesisActivationHeight;

    int mMaxConcurrentAsyncTasksPerNode;

    int mMaxParallelBlocks;
    int mPerBlockScriptValidatorThreadsCount;
    int mPerBlockScriptValidationMaxBatchSize;

    uint64_t maxOpsPerScriptPolicy;

    uint64_t maxTxSigOpsCountPolicy;
    uint64_t maxPubKeysPerMultiSig;
    uint64_t genesisGracefulPeriod;

    std::chrono::milliseconds mMaxStdTxnValidationDuration;
    std::chrono::milliseconds mMaxNonStdTxnValidationDuration;

    uint64_t maxStackMemoryUsagePolicy;
    uint64_t maxStackMemoryUsageConsensus;

    uint64_t maxScriptSizePolicy;

    uint64_t maxScriptNumLengthPolicy;

    bool mAcceptNonStandardOutput;

    uint64_t mMaxCoinsViewCacheSize;

    uint64_t mMaxMempool;
    uint64_t mMemPoolExpiry;
    uint64_t mLimitFreeRelay;
    uint64_t mMaxOrphanTxSize;
    uint64_t mStopAtHeight;
    uint64_t mPromiscuousMempoolFlags;
    bool mIsSetPromiscuousMempoolFlags;

    std::set<uint256> mInvalidBlocks;
    std::set<std::string> mBannedUAClients;

    bool LessThanZero(int64_t argValue, std::string* err, const std::string& errorMessage);

};

// Dummy for subclassing in unittests
class DummyConfig : public Config {
public:
    DummyConfig();
    DummyConfig(std::string net);

    void SetDefaultBlockSizeParams(const DefaultBlockSizeParams &params) override {  }

    bool SetMaxBlockSize(uint64_t maxBlockSize, std::string* err = nullptr) override {
        SetErrorMsg(err);
        return false; 
    }
    uint64_t GetMaxBlockSize() const override { return 0; }

    bool SetMaxGeneratedBlockSize(uint64_t maxGeneratedBlockSize, std::string* err = nullptr) override {
        SetErrorMsg(err);
        return false; 
    }
    uint64_t GetMaxGeneratedBlockSize() const override { return 0; };
    uint64_t GetMaxGeneratedBlockSize(int64_t nMedianTimePast) const override { return 0; }
    bool MaxGeneratedBlockSizeOverridden() const override { return false; }

    bool SetBlockSizeActivationTime(int64_t activationTime, std::string* err = nullptr) override {
        SetErrorMsg(err);
        return false; 
    }
    int64_t GetBlockSizeActivationTime() const override { return 0; }

    bool SetBlockPriorityPercentage(int64_t blockPriorityPercentage, std::string* err = nullptr) override {
        SetErrorMsg(err);
        return false;
    }
    uint8_t GetBlockPriorityPercentage() const override { return 0; }

    bool SetMaxTxSizePolicy(int64_t value, std::string* err = nullptr) override
    {
        SetErrorMsg(err);
        maxTxSizePolicy = value;
        return false;
    }
    uint64_t GetMaxTxSize(bool isGenesisEnabled, bool isConsensus) const override { return maxTxSizePolicy; }

    bool SetMinConsolidationFactor(uint64_t value, std::string* err = nullptr) override
    {
        SetErrorMsg(err);
        minConsolidationFactor = value;
        return false;
    }
    uint64_t GetMinConsolidationFactor() const override { return minConsolidationFactor; }

    bool SetMaxConsolidationInputScriptSize(uint64_t value, std::string* err = nullptr) override
    {
        SetErrorMsg(err);
        maxConsolidationInputScriptSize = value;
        return false;
    }
    uint64_t GetMaxConsolidationInputScriptSize() const override { return maxConsolidationInputScriptSize; }

    bool SetMinConsolidationInputMaturity(uint64_t value, std::string* err = nullptr) override
    {
        SetErrorMsg(err);
        minConsolidationInputMaturity = value;
        return false;
    }
    uint64_t GetMinConsolidationInputMaturity() const override { return minConsolidationInputMaturity; }

    bool SetAcceptNonStdConsolidationInput(uint64_t value, std::string* err = nullptr) override
    {
        SetErrorMsg(err);
        acceptNonStdConsolidationInput = value;
        return false;
    }
    bool GetAcceptNonStdConsolidationInput() const override { return acceptNonStdConsolidationInput; }

    void SetChainParams(std::string net);
    const CChainParams &GetChainParams() const override { return *chainParams; }

    void SetMinFeePerKB(CFeeRate amt) override{};
    CFeeRate GetMinFeePerKB() const override { return CFeeRate(Amount(0)); }

    void SetBlockMinFeePerKB(CFeeRate amt) override{};
    CFeeRate GetBlockMinFeePerKB() const override { return CFeeRate(Amount(0)); }

    void SetPreferredBlockFileSize(uint64_t preferredBlockFileSize) override {}
    uint64_t GetPreferredBlockFileSize() const override { return 0; }

    uint64_t GetDataCarrierSize() const override { return dataCarrierSize; }
    void SetDataCarrierSize(uint64_t dataCarrierSizeIn) override { dataCarrierSize = dataCarrierSizeIn; }

    void SetLimitAncestorSize(uint64_t limitAncestorSize) override {}
    uint64_t GetLimitAncestorSize() const override { return 0; }

    void SetLimitDescendantSize(uint64_t limitDescendantSize) override {}
    uint64_t GetLimitDescendantSize() const override { return 0; }

    void SetLimitAncestorCount(uint64_t limitAncestorCount) override {}
    uint64_t GetLimitAncestorCount() const override { return 0; }

    void SetLimitDescendantCount(uint64_t limitDescendantCount) override {}
    uint64_t GetLimitDescendantCount() const override { return 0; }

    void SetTestBlockCandidateValidity(bool skip) override {}
    bool GetTestBlockCandidateValidity() const override { return false; }

    void SetFactorMaxSendQueuesBytes(uint64_t factorMaxSendQueuesBytes) override {}
    uint64_t GetFactorMaxSendQueuesBytes() const override { return 0;}
    uint64_t GetMaxSendQueuesBytes() const override { return 0; }

    void SetMiningCandidateBuilder(mining::CMiningFactory::BlockAssemblerType type) override {}
    mining::CMiningFactory::BlockAssemblerType GetMiningCandidateBuilder() const override {
        return mining::CMiningFactory::BlockAssemblerType::JOURNALING;
    }

    bool SetGenesisActivationHeight(int64_t genesisActivationHeightIn, std::string* err = nullptr) override { genesisActivationHeight = static_cast<uint64_t>(genesisActivationHeightIn); return true; }
    uint64_t GetGenesisActivationHeight() const override { return genesisActivationHeight; }

    bool SetMaxConcurrentAsyncTasksPerNode(
        int maxConcurrentAsyncTasksPerNode,
        std::string* error = nullptr) override
    {
        SetErrorMsg(error);

        return false;
    }
    int GetMaxConcurrentAsyncTasksPerNode() const override;

    bool SetBlockScriptValidatorsParams(
        int maxParallelBlocks,
        int perValidatorThreadsCount,
        int perValidatorThreadMaxBatchSize,
        std::string* error = nullptr) override
    {
        SetErrorMsg(error);

        return false;
    }
    int GetMaxParallelBlocks() const override;
    int GetPerBlockScriptValidatorThreadsCount() const override;
    int GetPerBlockScriptValidationMaxBatchSize() const override;
    bool SetMaxStackMemoryUsage(int64_t maxStackMemoryUsageConsensusIn, int64_t maxStackMemoryUsagePolicyIn, std::string* err = nullptr)  override { return true; }
    uint64_t GetMaxStackMemoryUsage(bool isGenesisEnabled, bool consensus) const override { return UINT32_MAX; }

    bool SetMaxOpsPerScriptPolicy(int64_t maxOpsPerScriptPolicyIn, std::string* error) override { return true;  }
    uint64_t GetMaxOpsPerScript(bool isGenesisEnabled, bool consensus) const override
    {
        if (isGenesisEnabled)
        {
            return MAX_OPS_PER_SCRIPT_AFTER_GENESIS;
        }
        else
        {
            return MAX_OPS_PER_SCRIPT_BEFORE_GENESIS;
        }
    }
    bool SetMaxTxSigOpsCountPolicy(int64_t maxTxSigOpsCountIn, std::string* err = nullptr) override { return true; }
    uint64_t GetMaxTxSigOpsCountConsensusBeforeGenesis() const override { return MAX_TX_SIGOPS_COUNT_POLICY_BEFORE_GENESIS; }
    uint64_t GetMaxTxSigOpsCountPolicy(bool isGenesisEnabled) const override { return MAX_TX_SIGOPS_COUNT_POLICY_BEFORE_GENESIS; }

    uint64_t GetMaxBlockSigOpsConsensusBeforeGenesis(uint64_t blockSize) const override { throw std::runtime_error("DummyCofig::GetMaxBlockSigOps not implemented"); }

    bool SetGenesisGracefulPeriod(int64_t genesisGracefulPeriodIn, std::string* err = nullptr) override { return true; }
    uint64_t GetGenesisGracefulPeriod() const override { return DEFAULT_GENESIS_GRACEFULL_ACTIVATION_PERIOD; }

    bool SetMaxPubKeysPerMultiSigPolicy(int64_t maxPubKeysPerMultiSigIn, std::string* err = nullptr) override { return true; }
    uint64_t GetMaxPubKeysPerMultiSig(bool isGenesisEnabled, bool consensus) const override
    {
        if (isGenesisEnabled)
        {
            return MAX_PUBKEYS_PER_MULTISIG_AFTER_GENESIS;
        }
        else
        {
            return MAX_PUBKEYS_PER_MULTISIG_BEFORE_GENESIS;
        }
    }

    bool SetMaxStdTxnValidationDuration(int ms, std::string* err = nullptr) override
    {
        SetErrorMsg(err);

        return false;
    }
    std::chrono::milliseconds GetMaxStdTxnValidationDuration() const override
    {
        return DEFAULT_MAX_STD_TXN_VALIDATION_DURATION;
    }

    bool SetMaxNonStdTxnValidationDuration(int ms, std::string* err = nullptr) override
    {
        SetErrorMsg(err);

        return false;
    }
    std::chrono::milliseconds GetMaxNonStdTxnValidationDuration() const override
    {
        return DEFAULT_MAX_NON_STD_TXN_VALIDATION_DURATION;
    }

    bool SetMaxScriptSizePolicy(int64_t maxScriptSizePolicyIn, std::string* err = nullptr) override 
    {
        SetErrorMsg(err);
        maxScriptSizePolicy = static_cast<uint64_t>(maxScriptSizePolicyIn);
        return true; 
    };
    uint64_t GetMaxScriptSize(bool isGenesisEnabled, bool isConsensus) const override { return maxScriptSizePolicy; };

    bool SetMaxScriptNumLengthPolicy(int64_t maxScriptNumLengthIn, std::string* err = nullptr) override { return true;  }
    uint64_t GetMaxScriptNumLength(bool isGenesisEnabled, bool isConsensus) const override
    {
        if (isGenesisEnabled)
        {
            return MAX_SCRIPT_NUM_LENGTH_AFTER_GENESIS;
        }
        else
        {
            return MAX_SCRIPT_NUM_LENGTH_BEFORE_GENESIS;
        }
    }

    void SetAcceptNonStandardOutput(bool) override {}
    bool GetAcceptNonStandardOutput(bool isGenesisEnabled) const override
    {
        return isGenesisEnabled ? true : !fRequireStandard;
    }

    bool SetMaxCoinsViewCacheSize(int64_t max, std::string* err) override
    {
        SetErrorMsg(err);

        return false;
    }
    uint64_t GetMaxCoinsViewCacheSize() const override {return 0; /* unlimited */}

    bool SetMaxMempool(int64_t maxMempool, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetMaxMempool() const override { return DEFAULT_MAX_MEMPOOL_SIZE * ONE_MEGABYTE; }

    bool SetMemPoolExpiry(int64_t memPoolExpiry, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetMemPoolExpiry() const override { return DEFAULT_MEMPOOL_EXPIRY * SECONDS_IN_ONE_HOUR; }

    bool SetLimitFreeRelay(int64_t limitFreeRelay, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetLimitFreeRelay() const override { return DEFAULT_LIMITFREERELAY * ONE_KILOBYTE; }

    bool SetMaxOrphanTxSize(int64_t maxOrphanTxSize, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetMaxOrphanTxSize() const override { return COrphanTxns::DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE; }

    bool SetStopAtHeight(int64_t stopAtHeight, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetStopAtHeight() const override { return DEFAULT_STOPATHEIGHT; }

    bool SetPromiscuousMempoolFlags(int64_t promiscuousMempoolFlags, std::string* err) override
    {
        SetErrorMsg(err);

        return true;
    }
    uint64_t GetPromiscuousMempoolFlags() const override { return 0; }
    bool IsSetPromiscuousMempoolFlags() const override { return false; }

    void SetInvalidBlocks(const std::set<uint256>& hashes) override 
    { 
        mInvalidBlocks = hashes; 
    };

    const std::set<uint256>& GetInvalidBlocks() const override 
    { 
        return mInvalidBlocks; 
    };

    bool IsBlockInvalidated(const uint256& hash) const override 
    {
        return mInvalidBlocks.find(hash) != mInvalidBlocks.end(); 
    };

    void SetBanClientUA(const std::set<std::string> uaClients) override
    {
        mBannedUAClients = uaClients;
    }
    
    bool IsClientUABanned(const std::string uaClient) const override
    {
        return mBannedUAClients.find(uaClient) != mBannedUAClients.end();
    }


private:
    std::unique_ptr<CChainParams> chainParams;
    uint64_t dataCarrierSize { DEFAULT_DATA_CARRIER_SIZE };
    uint64_t genesisActivationHeight;
    uint64_t maxTxSizePolicy{ DEFAULT_MAX_TX_SIZE_POLICY_AFTER_GENESIS };
    uint64_t minConsolidationFactor{ DEFAULT_MIN_CONSOLIDATION_FACTOR };
    uint64_t maxConsolidationInputScriptSize{DEFAULT_MAX_CONSOLIDATION_INPUT_SCRIPT_SIZE };
    uint64_t minConsolidationInputMaturity { DEFAULT_MIN_CONSOLIDATION_INPUT_MATURITY };
    uint64_t acceptNonStdConsolidationInput { DEFAULT_ACCEPT_NON_STD_CONSOLIDATION_INPUT };
    uint64_t maxScriptSizePolicy { DEFAULT_MAX_SCRIPT_SIZE_POLICY_AFTER_GENESIS };
    std::set<uint256> mInvalidBlocks;
    std::set<std::string> mBannedUAClients;

    void SetErrorMsg(std::string* err)
    {
        if (err)
        {
            *err = "This is dummy config"; 
        } 
    }
};

#endif
