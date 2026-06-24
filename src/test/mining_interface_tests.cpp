#include <interfaces/types.h>
#include <interfaces/mining.h>
#include <node/mining_types.h>
#include <node/context.h>
#include <amount.h>

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

BOOST_AUTO_TEST_SUITE_END()
