#pragma once

// Small generic wrappers mirroring Rust's Arc<Mutex<T>> / Arc<RwLock<T>> ownership +
// locking pattern used pervasively by SharedDatabase/Executor (executor.rs) for state
// shared across session threads (e.g. `Arc<RwLock<SharedDatabase>>`, `Arc<Mutex<HashMap
// <usize, ProcessInfo>>>`). Always used behind a std::shared_ptr, exactly like Rust's
// Arc<...>. Guards borrow the protected value for their lifetime, like Rust's
// MutexGuard/RwLock{Read,Write}Guard.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>

namespace engine {

template <typename T>
class Mutex {
public:
    Mutex() = default;
    explicit Mutex(T value) : value_(std::move(value)) {}

    class Guard {
    public:
        Guard(std::mutex& m, T& v) : lock_(m), value_(v) {}
        T& operator*() const { return value_; }
        T* operator->() const { return &value_; }

    private:
        std::unique_lock<std::mutex> lock_;
        T& value_;
    };

    Guard lock() const { return Guard(mutex_, value_); }

private:
    mutable std::mutex mutex_;
    mutable T value_;
};

// Row-level-concurrency Stage 4/5 correctness fix (found via a real, reproducible hang
// under concurrent-reader stress testing): a bare std::shared_mutex does not guarantee a
// pending exclusive request gets honored before newly-arriving shared requests --
// std::shared_mutex on Windows is SRWLOCK-backed, and SRWLOCK does not promise writer
// priority/fairness. table_data_locks[table] (executor.hpp) is acquired shared VERY
// frequently (every plain SELECT) and exclusive comparatively rarely but for
// correctness-required durations (INSERT/UPDATE/DELETE's mutation phase) -- under
// sustained reader load (e.g. several reader threads each looping many SELECTs), a
// writer's exclusive request can be starved indefinitely by a continuous stream of new
// shared requests that keep "cutting in line" ahead of it, even though no individual
// reader holds its lock for long. Confirmed via a real test: 8 reader + 2 writer threads
// hung the whole process for hours (spinning at high CPU across the reader threads, the
// writer threads' join() never returning). RwLock<T>'s own structural mutex_ below has
// the identical exposure pattern (every single statement acquires it shared or exclusive
// at the very start of execute()), so it uses this same fair primitive too.
//
// A first attempt used an entry-mutex ("writer_gate_") that writers hold while waiting
// and readers briefly route through when a writer is pending. That reduced but did NOT
// eliminate the hang -- std::mutex itself does not guarantee FIFO fairness on Windows
// either, so a waiting writer could still repeatedly lose the race for writer_gate_ to a
// stream of newly-arriving readers each checking "is a writer waiting?" at a moment it
// happened to read as false-ish/racy under high contention (confirmed empirically: it
// fixed a smaller-scale repro but the original full-scale one still hung for hours of
// CPU time). This version makes the reader/writer admission decision explicit program
// logic gated by ONE mutex + condition_variable, never relying on any underlying
// primitive's unspecified fairness: lock_shared() refuses to proceed (waits on cv_)
// whenever a writer is active OR waiting, full stop -- so once a writer increments
// writers_waiting_, EVERY subsequent lock_shared() call blocks until that writer (and
// any writer ahead of it) has run, with no way for a later-arriving reader to slip past.
// Writer-vs-writer fairness is bounded by however many writers are ahead in
// writers_waiting_ (a plain counter, not a queue, so ties are broken by whichever
// woken thread's OS scheduler runs first -- fine for the small writer counts this
// codebase actually has). Drop-in compatible with std::shared_lock<FairSharedMutex>/
// std::unique_lock<FairSharedMutex> (same lock()/unlock()/lock_shared()/unlock_shared()
// surface as std::shared_mutex).
class FairSharedMutex {
public:
    void lock() {
        std::unique_lock<std::mutex> lk(mutex_);
        writers_waiting_++;
        cv_.wait(lk, [this] { return !writer_active_ && readers_active_ == 0; });
        writers_waiting_--;
        writer_active_ = true;
    }
    void unlock() {
        std::unique_lock<std::mutex> lk(mutex_);
        writer_active_ = false;
        cv_.notify_all();
    }
    void lock_shared() {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this] { return !writer_active_ && writers_waiting_ == 0; });
        readers_active_++;
    }
    void unlock_shared() {
        std::unique_lock<std::mutex> lk(mutex_);
        readers_active_--;
        if (readers_active_ == 0) cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int readers_active_ = 0;
    int writers_waiting_ = 0;
    bool writer_active_ = false;
};

template <typename T>
class RwLock {
public:
    RwLock() = default;
    explicit RwLock(T value) : value_(std::move(value)) {}

    class ReadGuard {
    public:
        ReadGuard(FairSharedMutex& m, const T& v) : lock_(m), value_(v) {}
        const T& operator*() const { return value_; }
        const T* operator->() const { return &value_; }

    private:
        std::shared_lock<FairSharedMutex> lock_;
        const T& value_;
    };

    class WriteGuard {
    public:
        WriteGuard(FairSharedMutex& m, T& v) : lock_(m), value_(v) {}
        T& operator*() const { return value_; }
        T* operator->() const { return &value_; }

    private:
        std::unique_lock<FairSharedMutex> lock_;
        T& value_;
    };

    ReadGuard read() const { return ReadGuard(mutex_, value_); }
    WriteGuard write() const { return WriteGuard(mutex_, value_); }

private:
    mutable FairSharedMutex mutex_;
    mutable T value_;
};

} // namespace engine
