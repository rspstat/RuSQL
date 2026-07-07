#include "engine/thread_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace engine {

struct ThreadPool::Impl {
    std::vector<std::thread> workers;
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<std::function<void()>> tasks;
    bool shutdown = false;

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return shutdown || !tasks.empty(); });
                if (tasks.empty()) {
                    if (shutdown) return;
                    continue;
                }
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }
};

ThreadPool::ThreadPool(std::size_t n_threads) : impl_(new Impl()) {
    if (n_threads == 0) n_threads = 1;
    impl_->workers.reserve(n_threads);
    for (std::size_t i = 0; i < n_threads; i++) {
        impl_->workers.emplace_back([this] { impl_->worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->shutdown = true;
    }
    impl_->cv.notify_all();
    for (auto& t : impl_->workers) t.join();
    delete impl_;
}

ThreadPool& ThreadPool::global() {
    unsigned hw = std::thread::hardware_concurrency();
    static ThreadPool instance(hw == 0 ? 4 : static_cast<std::size_t>(hw));
    return instance;
}

void ThreadPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    if (count == 1) {
        fn(0);
        return;
    }

    auto remaining = std::make_shared<std::atomic<std::size_t>>(count);
    auto done_mtx = std::make_shared<std::mutex>();
    auto done_cv = std::make_shared<std::condition_variable>();
    auto done = std::make_shared<bool>(false);

    for (std::size_t i = 0; i < count; i++) {
        impl_->enqueue([&fn, i, remaining, done_mtx, done_cv, done]() {
            fn(i);
            if (remaining->fetch_sub(1) == 1) {
                {
                    std::lock_guard<std::mutex> lock(*done_mtx);
                    *done = true;
                }
                done_cv->notify_one();
            }
        });
    }

    std::unique_lock<std::mutex> lock(*done_mtx);
    done_cv->wait(lock, [&] { return *done; });
}

} // namespace engine
