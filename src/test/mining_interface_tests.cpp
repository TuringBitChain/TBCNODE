#include <interfaces/types.h>
#include <interfaces/mining.h>
#include <node/mining_types.h>
#include <node/miner.h>
#include <node/context.h>
#include <amount.h>
#include <chain.h>
#include <config.h>
#include <consensus/merkle.h>
#include <key.h>
#include <pow.h>
#include <script/script.h>
#include <validation.h>

#include <test/test_bitcoin.h>

#include <atomic>
#include <chrono>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(mining_interface_tests)

BOOST_AUTO_TEST_CASE(types_defaults)
{
    interfaces::BlockRef ref;
    BOOST_CHECK_EQUAL(ref.height, -1);

    node::BlockCreateOptions create;
    BOOST_CHECK(create.use_mempool);
    BOOST_CHECK(create.test_block_validity);

    node::BlockWaitOptions wait;
    BOOST_CHECK(wait.fee_threshold == MAX_MONEY);

    node::BlockCheckOptions check;
    BOOST_CHECK(check.check_merkle_root);
    BOOST_CHECK(check.check_pow);

    node::NodeContext ctx;
    BOOST_CHECK(ctx.config == nullptr);
}

namespace {
//! Minimal mock proving the abstract interface is implementable/compiles.
class MockTemplate : public interfaces::BlockTemplate {
public:
    CBlockHeader getBlockHeader() override { return {}; }
    CBlock getBlock() override { return {}; }
    std::vector<Amount> getTxFees() override { return {}; }
    std::vector<int64_t> getTxSigops() override { return {}; }
    node::CoinbaseTx getCoinbaseTx() override { return {}; }
    std::vector<uint256> getCoinbaseMerklePath() override { return {}; }
    bool submitSolution(uint32_t, uint32_t, uint32_t, CTransactionRef) override { return false; }
    std::unique_ptr<interfaces::BlockTemplate> waitNext(node::BlockWaitOptions) override { return nullptr; }
    void interruptWait() override {}
};
} // namespace

BOOST_AUTO_TEST_CASE(interface_is_implementable)
{
    MockTemplate t;
    BOOST_CHECK(t.getTxFees().empty());
    BOOST_CHECK(t.waitNext({}) == nullptr);
}

// All chain-dependent assertions share ONE TestChain100Setup instance. The heavyweight
// regtest fixture does not survive repeated instantiation within a single suite (global
// chain/DB state leaks across instances), so the create/wait/submit behaviours are
// exercised together in one fixture rather than one case each.
BOOST_FIXTURE_TEST_CASE(mining_end_to_end, TestChain100Setup)
{
    const CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // getTip() matches the active chain.
    auto t0 = node::GetTip();
    BOOST_REQUIRE(t0.has_value());
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(t0->height, chainActive.Height());
        BOOST_CHECK(t0->hash == chainActive.Tip()->GetBlockHash());
    }

    // MakeMining -> createNewBlock -> solve PoW -> submitSolution advances the tip by one.
    node::NodeContext ctx;
    ctx.config = &testConfig;
    auto mining = interfaces::MakeMining(ctx);
    BOOST_REQUIRE(mining);

    auto tipA = mining->getTip();
    BOOST_REQUIRE(tipA.has_value());

    node::BlockCreateOptions opts;
    opts.coinbase_output_script = spk;
    auto tmpl = mining->createNewBlock(opts, /*cooldown=*/false);
    BOOST_REQUIRE(tmpl);
    BOOST_CHECK(tmpl->getCoinbaseTx().block_reward_remaining > Amount(0));

    CBlock block = tmpl->getBlock();
    // Solve PoW against the final merkle root (the same one submitSolution recomputes from
    // the coinbase); the assembler does not leave a valid merkle root on the template.
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, testConfig)) ++block.nNonce;
    BOOST_CHECK(tmpl->submitSolution(block.nVersion, block.nTime, block.nNonce, block.vtx[0]));

    auto tipB = mining->getTip();
    BOOST_REQUIRE(tipB.has_value());
    BOOST_CHECK_EQUAL(tipB->height, tipA->height + 1);

    // WaitTipChanged times out (no new block) and returns the same tip.
    std::atomic<bool> interrupt{false};
    auto same = node::WaitTipChanged(tipB->hash, std::chrono::milliseconds(200), interrupt);
    BOOST_REQUIRE(same.has_value());
    BOOST_CHECK(same->hash == tipB->hash);

    // WaitTipChanged returns promptly once a new block connects.
    CreateAndProcessBlock({}, spk);
    auto next = node::WaitTipChanged(tipB->hash, std::chrono::milliseconds(5000), interrupt);
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK(next->hash != tipB->hash);
    BOOST_CHECK_EQUAL(next->height, tipB->height + 1);
}

BOOST_AUTO_TEST_SUITE_END()
