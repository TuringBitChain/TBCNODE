// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SYNC_H
#define BITCOIN_SYNC_H

#include "threadsafety.h"

#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <memory>

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/recursive_mutex.hpp>

#include "util/macros.h"

/////////////////////////////////////////////////
//                                             //
// THE SIMPLE DEFINITION, EXCLUDING DEBUG CODE //
//                                             //
/////////////////////////////////////////////////

/*
CCriticalSection mutex;
    boost::recursive_mutex mutex;

LOCK(mutex);
    boost::unique_lock<boost::recursive_mutex> criticalblock(mutex);

LOCK2(mutex1, mutex2);
    boost::unique_lock<boost::recursive_mutex> criticalblock1(mutex1);
    boost::unique_lock<boost::recursive_mutex> criticalblock2(mutex2);

TRY_LOCK(mutex, name);
    boost::unique_lock<boost::recursive_mutex> name(mutex,
boost::try_to_lock_t);

ENTER_CRITICAL_SECTION(mutex); // no RAII
    mutex.lock();

LEAVE_CRITICAL_SECTION(mutex); // no RAII
    mutex.unlock();
 */

///////////////////////////////
//                           //
// THE ACTUAL IMPLEMENTATION //
//                           //
///////////////////////////////

/**
 * Template mixin that adds -Wthread-safety locking
 * annotations to a subset of the mutex API.
 */
template <typename PARENT> class LOCKABLE AnnotatedMixin : public PARENT {
public:
    void lock() EXCLUSIVE_LOCK_FUNCTION() { PARENT::lock(); }

    void unlock() UNLOCK_FUNCTION() { PARENT::unlock(); }

    bool try_lock() EXCLUSIVE_TRYLOCK_FUNCTION(true) {
        return PARENT::try_lock();
    }

    using unique_lock = std::unique_lock<PARENT>;
#ifdef __clang__
    //! For negative capabilities in the Clang Thread Safety Analysis.
    //! A negative requirement uses the EXCLUSIVE_LOCKS_REQUIRED attribute, in conjunction
    //! with the ! operator, to indicate that a mutex should not be held.
    const AnnotatedMixin& operator!() const { return *this; }
#endif // __clang__
};

#ifdef DEBUG_LOCKORDER
void EnterCritical(const char *pszName, const char *pszFile, int nLine,
                   void *cs, bool fTry = false);
void LeaveCritical();
void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line);
std::string LocksHeld();
void AssertLockHeldInternal(const char *pszName, const char *pszFile, int nLine,
                            void *cs);
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs);
void DeleteLock(void *cs);
#else
static inline void EnterCritical(const char *pszName, const char *pszFile,
                                 int nLine, void *cs, bool fTry = false) {}
static inline void LeaveCritical() {}
inline void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line) {}
static inline void AssertLockHeldInternal(const char *pszName,
                                          const char *pszFile, int nLine,
                                          void *cs) {}
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs) {}
static inline void DeleteLock(void *cs) {}
#endif

#define AssertLockHeld(cs) AssertLockHeldInternal(#cs, __FILE__, __LINE__, &cs)

/**
 * Wrapped boost mutex: supports recursive locking, but no waiting
 * TODO: We should move away from using the recursive lock by default.
 */
class CCriticalSection : public AnnotatedMixin<boost::recursive_mutex> {
public:
    ~CCriticalSection() { DeleteLock((void *)this); }
};

typedef CCriticalSection CDynamicCriticalSection;
/** Wrapped boost mutex: supports waiting but not recursive locking */
typedef AnnotatedMixin<boost::mutex> CWaitableCriticalSection;

/**
 * Wrapped mutex: supports recursive locking, but no waiting
 * TODO: We should move away from using the recursive lock by default.
 */
using RecursiveMutex = AnnotatedMixin<std::recursive_mutex>;

/** Wrapped mutex: supports waiting but not recursive locking */
using Mutex = AnnotatedMixin<std::mutex>;

/** Different type to mark Mutex at global scope
 *
 * Thread safety analysis can't handle negative assertions about mutexes
 * with global scope well, so mark them with a separate type, and
 * eventually move all the mutexes into classes so they are not globally
 * visible.
 *
 * See: https://github.com/bitcoin/bitcoin/pull/20272#issuecomment-720755781
 */
class GlobalMutex : public Mutex { };

#define AssertLockHeld(cs) AssertLockHeldInternal(#cs, __FILE__, __LINE__, &cs)

inline void AssertLockNotHeldInline(const char* name, const char* file, int line, Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, RecursiveMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
#define AssertLockNotHeld(cs) AssertLockNotHeldInline(#cs, __FILE__, __LINE__, &cs)

/** Wrapper around std::unique_lock style lock for MutexType. */
template <typename MutexType>
class SCOPED_LOCKABLE UniqueLock : public MutexType::unique_lock
{
private:
    using Base = typename MutexType::unique_lock;

    void Enter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex());
#ifdef DEBUG_LOCKCONTENTION
        if (Base::try_lock()) return;
        LOG_TIME_MICROS_WITH_CATEGORY(strprintf("lock contention %s, %s:%d", pszName, pszFile, nLine), BCLog::LOCK);
#endif
        Base::lock();
    }

    bool TryEnter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex(), true);
        if (Base::try_lock()) {
            return true;
        }
        LeaveCritical();
        return false;
    }

public:
    UniqueLock(MutexType& mutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(mutexIn) : Base(mutexIn, std::defer_lock)
    {
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    UniqueLock(MutexType* pmutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(pmutexIn)
    {
        if (!pmutexIn) return;

        *static_cast<Base*>(this) = Base(*pmutexIn, std::defer_lock);
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    ~UniqueLock() UNLOCK_FUNCTION()
    {
        if (Base::owns_lock())
            LeaveCritical();
    }

    operator bool()
    {
        return Base::owns_lock();
    }

protected:
    // needed for reverse_lock
    UniqueLock() { }

public:
    /**
     * An RAII-style reverse lock. Unlocks on construction and locks on destruction.
     */
    class reverse_lock {
    public:
        explicit reverse_lock(UniqueLock& _lock, const char* _guardname, const char* _file, int _line) : lock(_lock), file(_file), line(_line) {
            CheckLastCritical((void*)lock.mutex(), lockname, _guardname, _file, _line);
            lock.unlock();
            LeaveCritical();
            lock.swap(templock);
        }

        ~reverse_lock() {
            templock.swap(lock);
            EnterCritical(lockname.c_str(), file.c_str(), line, lock.mutex());
            lock.lock();
        }

     private:
        reverse_lock(reverse_lock const&);
        reverse_lock& operator=(reverse_lock const&);

        UniqueLock& lock;
        UniqueLock templock;
        std::string lockname;
        const std::string file;
        const int line;
     };
     friend class reverse_lock;
};

/** Just a typedef for boost::condition_variable, can be wrapped later if
 * desired */
typedef boost::condition_variable CConditionVariable;

#ifdef DEBUG_LOCKCONTENTION
void PrintLockContention(const char *pszName, const char *pszFile, int nLine);
#endif

/** Wrapper around boost::unique_lock<Mutex> */
template <typename Mutex> class SCOPED_LOCKABLE CMutexLock {
private:
    boost::unique_lock<Mutex> lock;

    void Enter(const char *pszName, const char *pszFile, int nLine) {
        EnterCritical(pszName, pszFile, nLine, (void *)(lock.mutex()));
#ifdef DEBUG_LOCKCONTENTION
        if (!lock.try_lock()) {
            PrintLockContention(pszName, pszFile, nLine);
#endif
            lock.lock();
#ifdef DEBUG_LOCKCONTENTION
        }
#endif
    }

    bool TryEnter(const char *pszName, const char *pszFile, int nLine) {
        EnterCritical(pszName, pszFile, nLine, (void *)(lock.mutex()), true);
        lock.try_lock();
        if (!lock.owns_lock()) LeaveCritical();
        return lock.owns_lock();
    }

public:
    CMutexLock(Mutex &mutexIn, const char *pszName, const char *pszFile,
               int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(mutexIn)
        : lock(mutexIn, boost::defer_lock) {
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    CMutexLock(Mutex *pmutexIn, const char *pszName, const char *pszFile,
               int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(pmutexIn) {
        if (!pmutexIn) return;

        lock = boost::unique_lock<Mutex>(*pmutexIn, boost::defer_lock);
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    ~CMutexLock() UNLOCK_FUNCTION() {
        if (lock.owns_lock()) LeaveCritical();
    }

    operator bool() { return lock.owns_lock(); }
};

typedef CMutexLock<CCriticalSection> CCriticalBlock;

// When locking a GlobalMutex or RecursiveMutex, just check it is not
// locked in the surrounding scope.
template <typename MutexType>
inline MutexType& MaybeCheckNotHeld(MutexType& m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }
template <typename MutexType>
inline MutexType* MaybeCheckNotHeld(MutexType* m) LOCKS_EXCLUDED(m) LOCK_RETURNED(m) { return m; }

#define LOCKMt(cs) UniqueLock UNIQUE_NAME(criticalblock)(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__)
#define LOCK2Mt(cs1, cs2)                                               \
    UniqueLock criticalblock1(MaybeCheckNotHeld(cs1), #cs1, __FILE__, __LINE__); \
    UniqueLock criticalblock2(MaybeCheckNotHeld(cs2), #cs2, __FILE__, __LINE__)
#define TRY_LOCKMt(cs, name) UniqueLock name(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__, true)
#define WAIT_LOCKMt(cs, name) UniqueLock name(MaybeCheckNotHeld(cs), #cs, __FILE__, __LINE__)

//! Run code while locking a mutex.
//!
//! Examples:
//!
//!   WITH_LOCK(cs, shared_val = shared_val + 1);
//!
//!   int val = WITH_LOCK(cs, return shared_val);
//!
//! Note:
//!
//! Since the return type deduction follows that of decltype(auto), while the
//! deduced type of:
//!
//!   WITH_LOCK(cs, return {int i = 1; return i;});
//!
//! is int, the deduced type of:
//!
//!   WITH_LOCK(cs, return {int j = 1; return (j);});
//!
//! is &int, a reference to a local variable
//!
//! The above is detectable at compile-time with the -Wreturn-local-addr flag in
//! gcc and the -Wreturn-stack-address flag in clang, both enabled by default.
#define WITH_LOCK(cs, code) (MaybeCheckNotHeld(cs), [&]() -> decltype(auto) { LOCKMt(cs); code; }())

#define LOCK(cs)                                                               \
    CCriticalBlock PASTE2(criticalblock, __COUNTER__)(cs, #cs, __FILE__,       \
                                                      __LINE__)
#define LOCK2(cs1, cs2)                                                        \
    CCriticalBlock criticalblock1(cs1, #cs1, __FILE__, __LINE__),              \
        criticalblock2(cs2, #cs2, __FILE__, __LINE__)
#define TRY_LOCK(cs, name)                                                     \
    CCriticalBlock name(cs, #cs, __FILE__, __LINE__, true)

#define ENTER_CRITICAL_SECTION(cs)                                             \
    {                                                                          \
        EnterCritical(#cs, __FILE__, __LINE__, (void *)(&cs));                 \
        (cs).lock();                                                           \
    }

#define LEAVE_CRITICAL_SECTION(cs)                                             \
    {                                                                          \
        (cs).unlock();                                                         \
        LeaveCritical();                                                       \
    }

/**
 * RAII exiting and later re-entering a critical section.
 *
 * NOTE: This class unlocks a single layer. In case of reentrant mutexes we
 *       can't know if there is more than one level that we need to unlock so
 *       in that case our thread would remain locked even though this class was
 *       used.
 */
class CTemporaryLeaveCriticalSectionGuard
{
public:
    CTemporaryLeaveCriticalSectionGuard(CCriticalSection& cs)
        : mCs{cs}
    {
        LEAVE_CRITICAL_SECTION(mCs)
    }
    ~CTemporaryLeaveCriticalSectionGuard()
    {
        ENTER_CRITICAL_SECTION(mCs)
    }

    CTemporaryLeaveCriticalSectionGuard(CTemporaryLeaveCriticalSectionGuard&&) = delete;
    CTemporaryLeaveCriticalSectionGuard& operator=(CTemporaryLeaveCriticalSectionGuard&&) = delete;
    CTemporaryLeaveCriticalSectionGuard(const CTemporaryLeaveCriticalSectionGuard&) = delete;
    CTemporaryLeaveCriticalSectionGuard& operator=(const CTemporaryLeaveCriticalSectionGuard&) = delete;

private:
    CCriticalSection& mCs;
};

class CSemaphore {
private:
    boost::condition_variable condition;
    boost::mutex mutex;
    int value;

public:
    CSemaphore(int init) : value(init) {}

    void wait() {
        boost::unique_lock<boost::mutex> lock(mutex);
        while (value < 1) {
            condition.wait(lock);
        }
        value--;
    }

    bool try_wait() {
        boost::unique_lock<boost::mutex> lock(mutex);
        if (value < 1) return false;
        value--;
        return true;
    }

    void post() {
        {
            boost::unique_lock<boost::mutex> lock(mutex);
            value++;
        }
        condition.notify_one();
    }
};

/** RAII-style semaphore lock */
class CSemaphoreGrant {
private:
    std::shared_ptr<CSemaphore> sem {nullptr};
    bool fHaveGrant {false};

public:
    void Acquire() {
        if (fHaveGrant) return;
        sem->wait();
        fHaveGrant = true;
    }

    void Release() {
        if (!fHaveGrant) return;
        sem->post();
        fHaveGrant = false;
    }

    bool TryAcquire() {
        if (!fHaveGrant && sem->try_wait()) fHaveGrant = true;
        return fHaveGrant;
    }

    void MoveTo(CSemaphoreGrant &grant) {
        grant.Release();
        grant.sem = sem;
        grant.fHaveGrant = fHaveGrant;
        fHaveGrant = false;
    }

    CSemaphoreGrant() = default;

    CSemaphoreGrant(const std::shared_ptr<CSemaphore>& sema, bool fTry = false)
        : sem(sema), fHaveGrant(false) {
        if (fTry)
            TryAcquire();
        else
            Acquire();
    }

    ~CSemaphoreGrant() { Release(); }

    operator bool() { return fHaveGrant; }
};

#endif // BITCOIN_SYNC_H
