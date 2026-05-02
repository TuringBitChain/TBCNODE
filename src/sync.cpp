// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sync.h"

#include "util.h"
#include "utilstrencodings.h"

#include <cstdio>

#include <boost/thread.hpp>

#ifdef DEBUG_LOCKCONTENTION
void PrintLockContention(const char *pszName, const char *pszFile, int nLine) {
    LogPrintf("LOCKCONTENTION: %s\n", pszName);
    LogPrintf("Locker: %s:%d\n", pszFile, nLine);
}
#endif /* DEBUG_LOCKCONTENTION */

#ifdef DEBUG_LOCKORDER
//
// Early deadlock detection.
// Problem being solved:
//    Thread 1 locks  A, then B, then C
//    Thread 2 locks  D, then C, then A
//     --> may result in deadlock between the two threads, depending on when
//     they run.
// Solution implemented here:
// Keep track of pairs of locks: (A before B), (A before C), etc.
// Complain if any thread tries to lock in a different order.
//

struct CLockLocation {
    // v2.6.1 P0.0a.4 H-G: 加 level 参数（默认 LEVEL_DEFAULT 兼容现有调用）
    CLockLocation(const char *pszName, const char *pszFile, int nLine,
                  bool fTryIn,
                  int levelIn = tbc::lock_hierarchy::LEVEL_DEFAULT) {
        mutexName = pszName;
        sourceFile = pszFile;
        sourceLine = nLine;
        fTry = fTryIn;
        level = levelIn;
    }

    std::string ToString() const {
        return mutexName + "  " + sourceFile + ":" + itostr(sourceLine) +
               (fTry ? " (TRY)" : "") +
               " [level=" + itostr(level) + "]";
    }

    std::string MutexName() const { return mutexName; }
    int Level() const { return level; }

    bool fTry;

private:
    std::string mutexName;
    std::string sourceFile;
    int sourceLine;
    int level;   // v2.6.1 P0.0a.4: lock hierarchy level
};

typedef std::vector<std::pair<void *, CLockLocation>> LockStack;
typedef std::map<std::pair<void *, void *>, LockStack> LockOrders;
typedef std::set<std::pair<void *, void *>> InvLockOrders;

struct LockData {
    // Very ugly hack: as the global constructs and destructors run single
    // threaded, we use this boolean to know whether LockData still exists,
    // as DeleteLock can get called by global CCriticalSection destructors
    // after LockData disappears.
    bool available;
    LockData() : available(true) {}
    ~LockData() { available = false; }

    LockOrders lockorders;
    InvLockOrders invlockorders;
    boost::mutex dd_mutex;
} static lockdata;

boost::thread_specific_ptr<LockStack> lockstack;

static void
potential_deadlock_detected(const std::pair<void *, void *> &mismatch,
                            const LockStack &s1, const LockStack &s2) {
    LogPrintf("POTENTIAL DEADLOCK DETECTED\n");
    LogPrintf("Previous lock order was:\n");
    for (const std::pair<void *, CLockLocation> &i : s2) {
        if (i.first == mismatch.first) {
            LogPrintf(" (1)");
        }
        if (i.first == mismatch.second) {
            LogPrintf(" (2)");
        }
        LogPrintf(" %s\n", i.second.ToString());
    }
    LogPrintf("Current lock order is:\n");
    for (const std::pair<void *, CLockLocation> &i : s1) {
        if (i.first == mismatch.first) {
            LogPrintf(" (1)");
        }
        if (i.first == mismatch.second) {
            LogPrintf(" (2)");
        }
        LogPrintf(" %s\n", i.second.ToString());
    }
    assert(false);
}

// v2.6.1 P0.0a.4 H-G: lock hierarchy level 检查（v2.6.1 P4.6 调整：增量友好规则）
//   规则：仅在新旧两端都标了非 LEVEL_DEFAULT 时检查严格单调（new_level > held_level）
//   - 任一端 LEVEL_DEFAULT → 跳过 level 检查（兼容未标注的 401 callsite，特别是 cs_main）
//   - 两端都标 level → 严格 level 单调，违反则 abort
//   现实背景（P4.6 验证）：cs_main 是 LEVEL_DEFAULT，但实际持有顺序是 cs_main → smtx。
//   若严格规则用 held_level=DEFAULT 也参与比较，会把合法的 "DEFAULT → 1" 当反向。
//   增量友好规则将 DEFAULT 视为"不参与 hierarchy"，只在 named-level 之间生效。
//   注：方向反例（smtx → cs_main）不会因此漏检——pair-tracking 已用 lockorders/invlockorders
//   的 Bitcoin 经典死锁检测捕获。
//   违反 → LogPrintf + abort（仅 DEBUG_LOCKORDER build）
static void check_level_order(void *c, const CLockLocation &locklocation) {
    if (lockstack.get() == nullptr) return;
    int new_level = locklocation.Level();
    if (new_level == tbc::lock_hierarchy::LEVEL_DEFAULT) {
        // 新锁未标 → 不参与 hierarchy 检查
        return;
    }
    for (const std::pair<void *, CLockLocation> &i : *lockstack) {
        if (i.first == c) {
            // 同一个 mutex 重入（boost::recursive_mutex），不检查 level
            return;
        }
        int held_level = i.second.Level();
        if (held_level == tbc::lock_hierarchy::LEVEL_DEFAULT) {
            // 已持锁未标 → 跳过（new 是 marked，但 held 不在 named hierarchy）
            continue;
        }
        // 两端都标 → 严格单调：new_level 必须 > held_level
        if (new_level <= held_level) {
            LogPrintf("LOCK ORDER VIOLATION (level):\n"
                      "  trying to lock %s [level=%d]\n"
                      "  already held %s [level=%d]\n"
                      "  full stack:\n%s",
                      locklocation.ToString().c_str(), new_level,
                      i.second.ToString().c_str(), held_level,
                      LocksHeld().c_str());
            abort();
        }
    }
}

static void push_lock(void *c, const CLockLocation &locklocation, bool fTry) {
    if (lockstack.get() == nullptr) lockstack.reset(new LockStack);

    // P0.0a.4: 先做 level 检查（新增）
    check_level_order(c, locklocation);

    boost::unique_lock<boost::mutex> lock(lockdata.dd_mutex);

    (*lockstack).push_back(std::make_pair(c, locklocation));

    for (const std::pair<void *, CLockLocation> &i : (*lockstack)) {
        if (i.first == c) break;

        std::pair<void *, void *> p1 = std::make_pair(i.first, c);
        if (lockdata.lockorders.count(p1)) continue;
        lockdata.lockorders[p1] = (*lockstack);

        std::pair<void *, void *> p2 = std::make_pair(c, i.first);
        lockdata.invlockorders.insert(p2);
        if (lockdata.lockorders.count(p2))
            potential_deadlock_detected(p1, lockdata.lockorders[p2],
                                        lockdata.lockorders[p1]);
    }
}

static void pop_lock() {
    (*lockstack).pop_back();
}

void EnterCritical(const char *pszName, const char *pszFile, int nLine,
                   void *cs, bool fTry, int level) {
    push_lock(cs, CLockLocation(pszName, pszFile, nLine, fTry, level), fTry);
}

void LeaveCritical() {
    pop_lock();
}

std::string LocksHeld() {
    std::string result;
    for (const std::pair<void *, CLockLocation> &i : *lockstack) {
        result += i.second.ToString() + std::string("\n");
    }
    return result;
}

void AssertLockHeldInternal(const char *pszName, const char *pszFile, int nLine,
                            void *cs) {
    for (const std::pair<void *, CLockLocation> &i : *lockstack) {
        if (i.first == cs) return;
    }
    fprintf(stderr,
            "Assertion failed: lock %s not held in %s:%i; locks held:\n%s",
            pszName, pszFile, nLine, LocksHeld().c_str());
    abort();
}

void DeleteLock(void *cs) {
    if (!lockdata.available) {
        // We're already shutting down.
        return;
    }
    boost::unique_lock<boost::mutex> lock(lockdata.dd_mutex);
    std::pair<void *, void *> item = std::make_pair(cs, (void *)0);
    LockOrders::iterator it = lockdata.lockorders.lower_bound(item);
    while (it != lockdata.lockorders.end() && it->first.first == cs) {
        std::pair<void *, void *> invitem =
            std::make_pair(it->first.second, it->first.first);
        lockdata.invlockorders.erase(invitem);
        lockdata.lockorders.erase(it++);
    }
    InvLockOrders::iterator invit = lockdata.invlockorders.lower_bound(item);
    while (invit != lockdata.invlockorders.end() && invit->first == cs) {
        std::pair<void *, void *> invinvitem =
            std::make_pair(invit->second, invit->first);
        lockdata.lockorders.erase(invinvitem);
        lockdata.invlockorders.erase(invit++);
    }
}

#endif /* DEBUG_LOCKORDER */
