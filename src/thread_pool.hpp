#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace diskscout {

// Fixed-size thread pool. Tasks may submit further tasks, which is what a recursive
// directory walk needs: wait_idle() returns only once the whole cascade has finished.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        if (thread_count == 0) thread_count = 1;
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Drains any queued work, then joins the workers.
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        task_cv_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        task_cv_.notify_one();
    }

    // Blocks until the queue is empty and no task is running.
    void wait_idle() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping and nothing left to drain
                task = std::move(queue_.front());
                queue_.pop();
                ++active_;
            }

            // A throwing task must not kill its worker thread or leave active_ stuck.
            try {
                task();
            } catch (...) {
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_;
                if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable task_cv_;
    std::condition_variable idle_cv_;
    std::size_t active_ = 0;
    bool stopping_ = false;
};

}  // namespace diskscout
