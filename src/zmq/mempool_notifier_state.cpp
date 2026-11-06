// Distributed under the Open TBC software license, see the accompanying file LICENSE.

#include "zmq/mempool_notifier_state.h"

#include "util.h"

#include <limits>
#include <memory>
#include <stdexcept>

#ifdef WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
int64_t NextEpoch(const fs::path& path)
{
    int64_t previous{0};
    if (fs::exists(path))
    {
        fs::ifstream input(path);
        if (!(input >> previous) || previous < 0)
        {
            throw std::runtime_error("Cannot read txinmempool epoch counter");
        }
        input >> std::ws;
        if (!input.eof() || input.bad())
        {
            throw std::runtime_error("Invalid txinmempool epoch counter");
        }
    }
    if (previous == std::numeric_limits<int64_t>::max())
    {
        throw std::overflow_error("Txinmempool epoch counter exhausted");
    }

    const int64_t next{previous + 1};
    fs::path temporary{path};
    temporary += ".tmp";
    const auto closeFile = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(closeFile)> file{
        fsbridge::fopen(temporary, "wb"), closeFile};
    if (!file)
    {
        throw std::runtime_error("Cannot create txinmempool epoch counter");
    }
    const std::string value{std::to_string(next) + "\n"};
    if (fwrite(value.data(), 1, value.size(), file.get()) != value.size() ||
        fflush(file.get()) != 0)
    {
        throw std::runtime_error("Cannot write txinmempool epoch counter");
    }
    // Check the flush result before permitting any event to use this epoch.
#ifdef WIN32
    const int committed{_commit(_fileno(file.get()))};
#else
    const int committed{fsync(fileno(file.get()))};
#endif
    if (committed != 0)
    {
        throw std::runtime_error("Cannot commit txinmempool epoch counter");
    }
    if (fclose(file.release()) != 0 || !RenameOver(temporary, path))
    {
        throw std::runtime_error("Cannot replace txinmempool epoch counter");
    }
#ifndef WIN32
    // Persist the rename before publishing under the new epoch.
    const fs::path parent{path.has_parent_path() ? path.parent_path() : fs::path{"."}};
    const int directory{open(parent.string().c_str(), O_RDONLY)};
    if (directory == -1)
    {
        throw std::runtime_error("Cannot open txinmempool epoch directory");
    }
    const int synced{fsync(directory)};
    const int closed{close(directory)};
    if (synced != 0 || closed != 0)
    {
        throw std::runtime_error("Cannot commit txinmempool epoch directory");
    }
#endif
    return next;
}
}

void MempoolNotifierState::Start(const fs::path& epochFile)
{
    std::lock_guard publishLock{mPublishMutex};
    std::lock_guard lock{mMutex};
    if (mStatus != Status::DISABLED)
    {
        throw std::logic_error("Txinmempool state can only start once");
    }
    mEpoch = NextEpoch(epochFile);
    mStatus = Status::RUNNING;
}

void MempoolNotifierState::Stop()
{
    std::lock_guard publishLock{mPublishMutex};
    std::lock_guard lock{mMutex};
    mStatus = Status::STOPPED;
    mAccepted.clear();
}

void MempoolNotifierState::CheckFailureNL() const
{
    if (mStatus == Status::FAILED)
    {
        throw std::runtime_error("Txinmempool notification stream has failed");
    }
}

int64_t MempoolNotifierState::NextSequenceNL()
{
    if (mAssigned == std::numeric_limits<int64_t>::max())
    {
        mStatus = Status::FAILED;
        mAccepted.clear();
        throw std::overflow_error("Txinmempool event sequence exhausted");
    }
    return mAssigned + 1;
}

std::optional<MempoolNotifierState::Position>
MempoolNotifierState::Accepted(const uint256& txid)
{
    std::lock_guard lock{mMutex};
    CheckFailureNL();
    if (mStatus != Status::RUNNING)
    {
        return std::nullopt;
    }
    if (mAccepted.count(txid))
    {
        throw std::logic_error("Duplicate txinmempool acceptance");
    }
    const int64_t next{NextSequenceNL()};
    // Do not consume a sequence number if allocation throws.
    mAccepted.emplace(txid, next);
    mAssigned = next;
    return Position{mEpoch, next};
}

std::optional<MempoolNotifierState::Position>
MempoolNotifierState::Removed(const uint256& txid)
{
    std::lock_guard lock{mMutex};
    CheckFailureNL();
    if (mStatus != Status::RUNNING)
    {
        return std::nullopt;
    }
    mAssigned = NextSequenceNL();
    mAccepted.erase(txid);
    return Position{mEpoch, mAssigned};
}

std::optional<MempoolNotifierState::Position>
MempoolNotifierState::GetAcceptance(const uint256& txid) const
{
    std::lock_guard lock{mMutex};
    CheckFailureNL();
    const auto entry{mAccepted.find(txid)};
    if (entry == mAccepted.end())
    {
        return std::nullopt;
    }
    return Position{mEpoch, entry->second};
}

MempoolNotifierState::Position MempoolNotifierState::GetSnapshot() const
{
    std::lock_guard lock{mMutex};
    CheckFailureNL();
    if (mStatus != Status::RUNNING)
    {
        return {-1, -1};
    }
    return {mEpoch, mPublished};
}

MempoolNotifierState::Status MempoolNotifierState::GetStatus() const
{
    std::lock_guard lock{mMutex};
    return mStatus;
}

bool MempoolNotifierState::Publish(Position position,
                                  const std::function<bool()>& sender)
{
    std::lock_guard publishLock{mPublishMutex};
    {
        std::lock_guard lock{mMutex};
        CheckFailureNL();
        if (mStatus != Status::RUNNING)
        {
            return false;
        }
        if (position.epoch != mEpoch || position.seq <= mPublished ||
            position.seq > mAssigned || position.seq - mPublished != 1)
        {
            throw std::logic_error("Invalid txinmempool publication order");
        }
    }

    bool success;
    try
    {
        success = sender();
    }
    catch (...)
    {
        Fail();
        throw;
    }
    std::lock_guard lock{mMutex};
    if (!success || mStatus == Status::FAILED)
    {
        mStatus = Status::FAILED;
        mAccepted.clear();
        return false;
    }
    mPublished = position.seq;
    return true;
}

void MempoolNotifierState::Fail()
{
    // Producers may hold the mempool lock, so never wait for the publisher.
    // Publish checks for this transition after invoking the sender.
    std::lock_guard lock{mMutex};
    if (mStatus == Status::RUNNING)
    {
        mStatus = Status::FAILED;
        mAccepted.clear();
    }
}
