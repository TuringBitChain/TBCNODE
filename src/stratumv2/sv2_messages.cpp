#include "stratumv2/sv2_messages.h"

#include <arith_uint256.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <hash.h>
#include <streams.h>
#include <consensus/merkle.h>
#include "mining/mining.h"

using namespace node;


Sv2NewTemplateMsg::Sv2NewTemplateMsg(BlockTemplate& block_template, uint64_t template_id, bool future_template)
    : m_template_id{template_id}, m_future_template{future_template}
{
    auto block = block_template.getBlockRef();
    m_version = block->GetBlockHeader().nVersion;

    const CTransactionRef coinbase_tx = block_template.getCoinbaseTx();
    m_coinbase_tx_version = coinbase_tx->CURRENT_VERSION;
    m_coinbase_prefix = coinbase_tx->vin[0].scriptSig;
    m_coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // The coinbase nValue already contains the nFee + the Block Subsidy when built using CreateBlock().
    m_coinbase_tx_value_remaining = static_cast<uint64_t>(coinbase_tx->vout[0].nValue.GetSatoshis());

    // TBCNODE has no SegWit: coinbase never carries a witness commitment output.
    // The pool adds its own payout outputs to cover m_coinbase_tx_value_remaining.
    m_coinbase_tx_outputs_count = 0;

    m_coinbase_tx_locktime = coinbase_tx->nLockTime;

    m_merkle_path = block_template.getCoinbaseMerklePath();
}

Sv2SetNewPrevHashMsg::Sv2SetNewPrevHashMsg(BlockTemplate& block_template, uint64_t template_id)
    : m_template_id{template_id}
{
    auto header = block_template.getBlockHeader();
    m_prev_hash = header.hashPrevBlock;
    m_header_timestamp = header.nTime;
    m_nBits = header.nBits;
    m_target = ArithToUint256(arith_uint256().SetCompact(header.nBits));
}
