#pragma once

// Small generic wrappers mirroring Rust's Arc<Mutex<T>> / Arc<RwLock<T>> ownership +
// locking pattern used pervasively by SharedDatabase/Executor (executor.rs) for state
// shared across session threads (e.g. `Arc<RwLock<SharedDatabase>>`, `Arc<Mutex<HashMap
// <usize, ProcessInfo>>>`). Always used behind a std::shared_ptr, exactly like Rust's
// Arc<...>. Guards borrow the protected value for their lifetime, like Rust's
// MutexGuard/RwLock{Read,Write}Guard.

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

template <typename T>
class RwLock {
public:
    RwLock() = default;
    explicit RwLock(T value) : value_(std::move(value)) {}

    class ReadGuard {
    public:
        ReadGuard(std::shared_mutex& m, const T& v) : lock_(m), value_(v) {}
        const T& operator*() const { return value_; }
        const T* operator->() const { return &value_; }

    private:
        std::shared_lock<std::shared_mutex> lock_;
        const T& value_;
    };

    class WriteGuard {
    public:
        WriteGuard(std::shared_mutex& m, T& v) : lock_(m), value_(v) {}
        T& operator*() const { return value_; }
        T* operator->() const { return &value_; }

    private:
        std::unique_lock<std::shared_mutex> lock_;
        T& value_;
    };

    ReadGuard read() const { return ReadGuard(mutex_, value_); }
    WriteGuard write() const { return WriteGuard(mutex_, value_); }

private:
    mutable std::shared_mutex mutex_;
    mutable T value_;
};

} // namespace engine
