// Copyright (c) 2012-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coins.h"

#include "consensus/consensus.h"
#include "memusage.h"
#include "random.h"

#include <cassert>
#include <config.h>

bool CCoinsView::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    return false;
}
bool CCoinsView::HaveCoin(const COutPoint &outpoint) const {
    return false;
}
uint256 CCoinsView::GetBestBlock() const {
    return uint256();
}
std::vector<uint256> CCoinsView::GetHeadBlocks() const {
    return std::vector<uint256>();
}
bool CCoinsView::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) {
    return false;
}
CCoinsViewCursor *CCoinsView::Cursor() const {
    return nullptr;
}
CCoinsViewCursor* CCoinsView::Cursor(const TxId &txId) const {
    return nullptr;
}

CCoinsViewBacked::CCoinsViewBacked(CCoinsView *viewIn) : base(viewIn) {}
bool CCoinsViewBacked::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    return base->GetCoin(outpoint, coin);
}
bool CCoinsViewBacked::HaveCoin(const COutPoint &outpoint) const {
    return base->HaveCoin(outpoint);
}
uint256 CCoinsViewBacked::GetBestBlock() const {
    return base->GetBestBlock();
}
std::vector<uint256> CCoinsViewBacked::GetHeadBlocks() const {
    return base->GetHeadBlocks();
}
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) {
    base = &viewIn;
}
bool CCoinsViewBacked::BatchWrite(CCoinsMap &mapCoins,
                                  const uint256 &hashBlock) {
    return base->BatchWrite(mapCoins, hashBlock);
}
CCoinsViewCursor *CCoinsViewBacked::Cursor() const {
    return base->Cursor();
}
CCoinsViewCursor* CCoinsViewBacked::Cursor(const TxId &txId) const {
    return base->Cursor(txId);
}
size_t CCoinsViewBacked::EstimateSize() const {
    return base->EstimateSize();
}

SaltedOutpointHasher::SaltedOutpointHasher()
    : k0(GetRand(std::numeric_limits<uint64_t>::max())),
      k1(GetRand(std::numeric_limits<uint64_t>::max())) {}

CCoinsViewCache::CCoinsViewCache(CCoinsView *baseIn)
    : CCoinsViewBacked(baseIn), cachedCoinsUsage(0) {}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return memusage::DynamicUsage(cacheCoins) + cachedCoinsUsage;
}

CCoinsMap::iterator
CCoinsViewCache::FetchCoinNL(const COutPoint &outpoint) const {
    // here to get coin info zws!!!
    CCoinsMap::iterator it = cacheCoins.find(outpoint);
    if (it != cacheCoins.end()) {
        return it;
    }
    Coin tmp;
    if (!base->GetCoin(outpoint, tmp)) {
        return cacheCoins.end();
    }
    CCoinsMap::iterator ret =
        cacheCoins
            .emplace(std::piecewise_construct, std::forward_as_tuple(outpoint),
                     std::forward_as_tuple(std::move(tmp)))
            .first;
    if (ret->second.coin.IsSpent()) {
        // The parent only has an empty entry for this outpoint; we can consider
        // our version as fresh.
        ret->second.flags = CCoinsCacheEntry::FRESH;
    }
    cachedCoinsUsage += ret->second.coin.DynamicMemoryUsage();
    return ret;
}

bool CCoinsViewCache::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    CCoinsMap::const_iterator it = FetchCoinNL(outpoint);
    if (it == cacheCoins.end()) {
        return false;
    }
    coin = it->second.coin;
    return true;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin coin,
                              bool possible_overwrite,
                              uint64_t genesisActivationHeight) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    assert(!coin.IsSpent());
    if (coin.GetTxOut().scriptPubKey.IsUnspendable( coin.GetHeight() >= genesisActivationHeight)) {
        return;
    }
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) =
        cacheCoins.emplace(std::piecewise_construct,
                           std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!inserted) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    }
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error(
                "Adding new coin that replaces non-pruned entry");
        }
        fresh = !(it->second.flags & CCoinsCacheEntry::DIRTY);
    }
    it->second.coin = std::move(coin);
    it->second.flags |=
        CCoinsCacheEntry::DIRTY | (fresh ? CCoinsCacheEntry::FRESH : 0);
    cachedCoinsUsage += it->second.coin.DynamicMemoryUsage();
}

void AddCoins(CCoinsViewCache &cache, const CTransaction &tx, int nHeight, uint64_t genesisActivationHeight,
              bool check) {
    bool fCoinbase = tx.IsCoinBase();
    const TxId txid = tx.GetId();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const COutPoint outpoint(txid, i);
        bool overwrite = check ? cache.HaveCoin(outpoint) : fCoinbase;
        // Always set the possible_overwrite flag to AddCoin for coinbase txn,
        // in order to correctly deal with the pre-BIP30 occurrences of
        // duplicate coinbase transactions.
        cache.AddCoin(outpoint, Coin(tx.vout[i], nHeight, fCoinbase),
                      overwrite, genesisActivationHeight);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin *moveout) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    CCoinsMap::iterator it = FetchCoinNL(outpoint);
    if (it == cacheCoins.end()) {
        return false;
    }
    cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.flags & CCoinsCacheEntry::FRESH) {
        cacheCoins.erase(it);
    } else {
        it->second.flags |= CCoinsCacheEntry::DIRTY;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return AccessCoinNL(outpoint);
}

const Coin& CCoinsViewCache::AccessCoinNL(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoinNL(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;
    }
    return it->second.coin;
}

bool CCoinsViewCache::HaveCoin(const COutPoint &outpoint) const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return HaveCoinNL(outpoint);
}

bool CCoinsViewCache::HaveCoinNL(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoinNL(outpoint);
    return it != cacheCoins.end() && !it->second.coin.IsSpent();
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint &outpoint) const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    CCoinsMap::const_iterator it = cacheCoins.find(outpoint);
    return it != cacheCoins.end();
}

uint256 CCoinsViewCache::GetBestBlock() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    if (hashBlock.IsNull()) {
        hashBlock = base->GetBestBlock();
    }
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    hashBlock = hashBlockIn;
}

bool CCoinsViewCache::BatchWrite(CCoinsMap &mapCoins,
                                 const uint256 &hashBlockIn) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        // Ignore non-dirty entries (optimization).
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            CCoinsMap::iterator itUs = cacheCoins.find(it->first);
            if (itUs == cacheCoins.end()) {
                // The parent cache does not have an entry, while the child does
                // We can ignore it if it's both FRESH and pruned in the child
                if (!(it->second.flags & CCoinsCacheEntry::FRESH &&
                      it->second.coin.IsSpent())) {
                    // Otherwise we will need to create it in the parent and
                    // move the data up and mark it as dirty
                    CCoinsCacheEntry &entry = cacheCoins[it->first];
                    entry.coin = std::move(it->second.coin);
                    cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
                    entry.flags = CCoinsCacheEntry::DIRTY;
                    // We can mark it FRESH in the parent if it was FRESH in the
                    // child. Otherwise it might have just been flushed from the
                    // parent's cache and already exist in the grandparent
                    if (it->second.flags & CCoinsCacheEntry::FRESH)
                        entry.flags |= CCoinsCacheEntry::FRESH;
                }
            } else {
                // Assert that the child cache entry was not marked FRESH if the
                // parent cache entry has unspent outputs. If this ever happens,
                // it means the FRESH flag was misapplied and there is a logic
                // error in the calling code.
                if ((it->second.flags & CCoinsCacheEntry::FRESH) &&
                    !itUs->second.coin.IsSpent())
                    throw std::logic_error("FRESH flag misapplied to cache "
                                           "entry for base transaction with "
                                           "spendable outputs");

                // Found the entry in the parent cache
                if ((itUs->second.flags & CCoinsCacheEntry::FRESH) &&
                    it->second.coin.IsSpent()) {
                    // The grandparent does not have an entry, and the child is
                    // modified and being pruned. This means we can just delete
                    // it from the parent.
                    cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                    cacheCoins.erase(itUs);
                } else {
                    // A normal modification.
                    cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                    itUs->second.coin = std::move(it->second.coin);
                    cachedCoinsUsage += itUs->second.coin.DynamicMemoryUsage();
                    itUs->second.flags |= CCoinsCacheEntry::DIRTY;
                    // NOTE: It is possible the child has a FRESH flag here in
                    // the event the entry we found in the parent is pruned. But
                    // we must not copy that FRESH flag to the parent as that
                    // pruned state likely still needs to be communicated to the
                    // grandparent.
                }
            }
        }
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }
    hashBlock = hashBlockIn;
    return true;
}

CCoinsViewCursor* CCoinsViewCache::Cursor() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return base->Cursor();
}

CCoinsViewCursor* CCoinsViewCache::Cursor(const TxId &txId) const {
    std::unique_lock<std::mutex> lock{ mCoinsViewCacheMtx };
    return base->Cursor(txId);
}

std::vector<uint256> CCoinsViewCache::GetHeadBlocks() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return base->GetHeadBlocks();
}

void CCoinsViewCache::SetBackend(CCoinsView &viewIn) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    base = &viewIn;
}

size_t CCoinsViewCache::EstimateSize() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return base->EstimateSize();
}

bool CCoinsViewCache::Flush() {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    bool fOk = base->BatchWrite(cacheCoins, hashBlock);
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    return fOk;
}

void CCoinsViewCache::Uncache(const COutPoint &outpoint) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    UncacheNL(outpoint);
}

void CCoinsViewCache::UncacheNL(const COutPoint &outpoint) {
    CCoinsMap::iterator it = cacheCoins.find(outpoint);
    if (it != cacheCoins.end() && it->second.flags == 0) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
        cacheCoins.erase(it);
    }
}

void CCoinsViewCache::Uncache(const std::vector<COutPoint>& vOutpoints) {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    for (const COutPoint &outpoint : vOutpoints) {
         UncacheNL(outpoint);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return cacheCoins.size();
}

const CTxOut &CCoinsViewCache::GetOutputFor(const CTxIn &input) const {
    std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
    return GetOutputForNL(input);
}

const CTxOut &CCoinsViewCache::GetOutputForNL(const CTxIn &input) const {
    const Coin &coin = AccessCoinNL(input.prevout);
    assert(!coin.IsSpent());
    return coin.GetTxOut();
}

Amount CCoinsViewCache::GetValueIn(const CTransaction &tx) const {
    if (tx.IsCoinBase()) {
        return Amount(0);
    }

    Amount nResult(0);
    {
        std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
        for (const auto& input: tx.vin) {
            nResult += GetOutputForNL(input).nValue;
        }
    }
    return nResult;
}

bool CCoinsViewCache::HaveInputs(const CTransaction &tx) const {
    if (tx.IsCoinBase()) {
        return true;
    }
    {
        std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
        for (const auto& input: tx.vin) {
            if (!HaveCoinNL(input.prevout)) {
                return false;
            }
        }
    }
    return true;
}

std::optional<bool> CCoinsViewCache::HaveInputsLimited(
    const CTransaction &tx,
    size_t maxCachedCoinsUsage) const
{
    if (tx.IsCoinBase()) {
        return true;
    }
    {
        std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
        for (const auto& input: tx.vin) {
            if (!HaveCoinNL(input.prevout)) {
                return false;
            }

            if(maxCachedCoinsUsage > 0 && cachedCoinsUsage >= maxCachedCoinsUsage)
            {
                return {};
            }
        }
    }
    return true;
}

double CCoinsViewCache::GetPriority(const CTransaction &tx, int nHeight,
                                    Amount &inChainInputValue) const {
    inChainInputValue = Amount(0);
    if (tx.IsCoinBase()) {
        return 0.0;
    }
    double dResult = 0.0;
    {
        std::unique_lock<std::mutex> lock { mCoinsViewCacheMtx };
        for (const CTxIn &txin : tx.vin) {
            const Coin &coin = AccessCoinNL(txin.prevout);
            if (coin.IsSpent()) {
                continue;
            }
            if (int64_t(coin.GetHeight()) <= nHeight) {
                dResult += double(coin.GetTxOut().nValue.GetSatoshis()) *
                           (nHeight - coin.GetHeight());
                inChainInputValue += coin.GetTxOut().nValue;
            }
        }
    }
    return tx.ComputePriority(dResult);
}

static const int MAX_VIEW_ITERATIONS = 100;

const Coin AccessByTxid(const CCoinsViewCache& view, const TxId& txid)
{
    // wtih MAX_VIEW_ITERATIONS we are avoiding for loop to MAX_OUTPUTS_PER_TX (in millions after genesis)
    // performance testing indicates that after 100 look up by cursor becomes faster

    for (int n = 0; n < MAX_VIEW_ITERATIONS; n++) {
        const Coin& alternate = view.AccessCoin(COutPoint(txid, n));
        if (!alternate.IsSpent()) {
            return alternate;
        }
    }

    // for large output indexes delegate search to db cursor/iterator by key prefix (txId)

    COutPoint key;
    Coin coin;

    std::unique_ptr<CCoinsViewCursor> cursor{ view.Cursor(txid) };

    if (cursor->Valid())
    {
        cursor->GetKey(key);
    }
    while (cursor->Valid() && key.GetTxId() == txid)
    {
        if (!cursor->GetValue(coin))
        {
            return coinEmpty;
        }
        if (!coin.IsSpent())
        {
            return coin;
        }
        cursor->Next();
        if (cursor->Valid())
        {
            cursor->GetKey(key);
        }
    }
    return coinEmpty;
}
