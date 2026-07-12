#include "thread_pool.h"
#include <iostream>

// ============================================================
// 构造函数：创建 fixed_size 个线程
// ============================================================
thread_pool::thread_pool(size_t fixed_size) {
    for (size_t i = 0; i < fixed_size; ++i) {
        workers_.emplace_back(&thread_pool::worker_loop, this);
    }
    std::cout << "[thread_pool] " << fixed_size << " 个 worker 线程已启动\n";
}

// ============================================================
// 析构函数：停止所有线程
//
// 先设置 stop_ 标志，唤醒所有等待的线程，
// 然后一个一个 join（等待它们完成当前任务）
// ============================================================
thread_pool::~thread_pool() {
    stop_ = true;
    cv_.notify_all();  // 唤醒所有 worker，让它们检测 stop_ 并退出

    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }

    std::cout << "[thread_pool] 已关闭\n";
}

// ============================================================
// worker_loop — 每个 worker 线程的主循环
//
// 不断从队列取任务执行。队列为空时等待。
// stop_ = true 时退出循环。
// ============================================================
void thread_pool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // 等待直到有任务 or 线程池要关闭
            cv_.wait(lock, [this]() {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) {
                return;  // 没有待处理的任务了，退出
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 执行任务（在锁外执行，不阻塞其他线程取任务）
        try {
            task();//调用 session 提交的那个 lambda回调
        } catch (const std::exception& e) {
            std::cerr << "[thread_pool] 任务异常: " << e.what() << "\n";
        }
    }
}
