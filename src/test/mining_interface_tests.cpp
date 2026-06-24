#include <interfaces/types.h>
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

BOOST_AUTO_TEST_SUITE_END()
