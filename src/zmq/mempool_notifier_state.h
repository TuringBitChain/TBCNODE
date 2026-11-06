// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#pragma once

#include "fs.h"
#include "uint256.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

/** State for txinmempool, independent of the transport and subscriber count.
 *
 * Call Accepted/Removed after the mempool mutation, under its write lock, and
 * enqueue the returned position before releasing that lock. Publish queued
 * events in sequence order without holding the mempool lock. Entry RPCs must
 * hold the mempool read lock while calling GetAcceptance.
 * Lock order is mempool -> state; this class never acquires the mempool lock.
 * Catch notification errors at the integration boundary: they must not undo
 * or reject an already committed mutation. Call Fail and report the error.
 * Restored entries skip Accepted; their later removal may call Removed.
 * Only current acceptance positions are retained, never a replay history.
 */
class MempoolNotifierState
{
public:
    struct Position
    {
        int64_t epoch;
        int64_t seq;
    };

    enum class Status { DISABLED, RUNNING, FAILED, STOPPED };

    /** Start once, after acquiring the node's data directory lock. Keep the
     * counter file across restarts; only one owner may use it at a time.
     * Unreadable, corrupt or exhausted counters prevent startup.
     * A default-constructed, unused instance performs no file I/O.
     */
    void Start(const fs::path& epochFile);

    /** Wait for an in-flight send, disable the stream and release entry state.
     * Pending events are abandoned. This instance cannot restart. The owner
     * must stop producers and join the publisher before destroying this object.
     */
    void Stop();

    std::optional<Position> Accepted(const uint256& txid);
    // The caller must ensure the transaction really was in the mempool.
    std::optional<Position> Removed(const uint256& txid);
    std::optional<Position> GetAcceptance(const uint256& txid) const;

    /** Last successful submission, or {-1, -1} when disabled/stopped.
     * A failed stream throws rather than reporting a usable stale snapshot.
     */
    Position GetSnapshot() const;
    Status GetStatus() const;

    /** Submit exactly the next allocated event. The sender returns true only
     * after submitting the complete multipart message. False or an exception
     * permanently fails the stream; later events cannot hide that failure.
     * Invalid positions throw before invoking sender.
     * The callback runs without the state lock, but must not call Publish/Stop
     * or acquire the mempool lock. Stop must also run outside the mempool lock.
     */
    bool Publish(Position position, const std::function<bool()>& sender);

    /** Fail closed if recording or enqueueing a committed mutation throws. */
    void Fail();

private:
    void CheckFailureNL() const;
    int64_t NextSequenceNL();

    // Publication/lifecycle operations take this lock before mMutex.
    std::mutex mPublishMutex;
    mutable std::mutex mMutex;
    Status mStatus{Status::DISABLED};
    int64_t mEpoch{-1};
    int64_t mAssigned{0};
    int64_t mPublished{0};
    std::map<uint256, int64_t> mAccepted;
};
