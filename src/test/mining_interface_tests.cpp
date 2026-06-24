#include <interfaces/types.h>
#include <interfaces/mining.h>
#include <node/mining_types.h>
#include <node/miner.h>
#include <node/context.h>
#include <amount.h>
#include <chain.h>
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

BOOST_FIXTURE_TEST_CASE(get_tip_matches_chain, TestChain100Setup)
{
    auto tip = node::GetTip();
    BOOST_REQUIRE(tip.has_value());
    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip->height, chainActive.Height());
    BOOST_CHECK(tip->hash == chainActive.Tip()->GetBlockHash());
}

BOOST_FIXTURE_TEST_CASE(wait_tip_changed_returns_on_new_block, TestChain100Setup)
{
    auto before = node::GetTip();
    BOOST_REQUIRE(before.has_value());

    // Mine one more block, then confirm WaitTipChanged observes the new tip immediately.
    CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CreateAndProcessBlock({}, spk);

    std::atomic<bool> interrupt{false};
    auto after = node::WaitTipChanged(before->hash, std::chrono::milliseconds(5000), interrupt);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK(after->hash != before->hash);
    BOOST_CHECK_EQUAL(after->height, before->height + 1);
}

BOOST_FIXTURE_TEST_CASE(wait_tip_changed_times_out, TestChain100Setup)
{
    auto tip = node::GetTip();
    BOOST_REQUIRE(tip.has_value());
    std::atomic<bool> interrupt{false};
    // No new block: should time out and return the same tip.
    auto res = node::WaitTipChanged(tip->hash, std::chrono::milliseconds(200), interrupt);
    BOOST_REQUIRE(res.has_value());
    BOOST_CHECK(res->hash == tip->hash);
}

BOOST_AUTO_TEST_SUITE_END()
