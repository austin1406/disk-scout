#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <stdexcept>

#include "thread_pool.hpp"

using diskscout::ThreadPool;

TEST(ThreadPool, RunsEverySubmittedTask) {
    std::atomic<int> count{0};
    ThreadPool pool(4);
    for (int i = 0; i < 1000; ++i) pool.submit([&count] { ++count; });
    pool.wait_idle();
    EXPECT_EQ(count.load(), 1000);
}

TEST(ThreadPool, WaitIdleCoversTasksSpawnedByTasks) {
    std::atomic<int> count{0};
    ThreadPool pool(4);

    // Binary tree of depth 6: 1 + 2 + 4 + ... + 64 = 127 tasks, each spawned by its parent.
    std::function<void(int)> spawn = [&](int depth) {
        ++count;
        if (depth < 6) {
            pool.submit([&spawn, depth] { spawn(depth + 1); });
            pool.submit([&spawn, depth] { spawn(depth + 1); });
        }
    };
    pool.submit([&spawn] { spawn(0); });
    pool.wait_idle();

    EXPECT_EQ(count.load(), 127);
}

TEST(ThreadPool, SingleWorkerStillFinishesNestedWork) {
    std::atomic<int> count{0};
    ThreadPool pool(1);
    pool.submit([&] {
        ++count;
        pool.submit([&count] { ++count; });
    });
    pool.wait_idle();
    EXPECT_EQ(count.load(), 2);
}

TEST(ThreadPool, ZeroThreadsIsTreatedAsOne) {
    std::atomic<int> count{0};
    ThreadPool pool(0);
    pool.submit([&count] { ++count; });
    pool.wait_idle();
    EXPECT_EQ(count.load(), 1);
}

TEST(ThreadPool, ThrowingTaskDoesNotStallThePool) {
    std::atomic<int> count{0};
    ThreadPool pool(2);
    pool.submit([] { throw std::runtime_error("boom"); });
    pool.submit([&count] { ++count; });
    pool.wait_idle();
    EXPECT_EQ(count.load(), 1);
}

TEST(ThreadPool, DestructorDrainsQueuedWork) {
    std::atomic<int> count{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 100; ++i) pool.submit([&count] { ++count; });
    }
    EXPECT_EQ(count.load(), 100);
}
