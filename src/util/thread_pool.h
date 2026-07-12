#ifndef AI_SWITCH_THREAD_POOL_H
#define AI_SWITCH_THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

/**
 * thread_pool — 固定大小线程池
 * =============================
 *
 * 解决 std::thread().detach() 在高并发下的问题：
 *   1. 创建线程有开销 → 线程池预创建，复用
 *   2. 线程数无上限 → 限制最大并发数，超出的排队
 *
 * 用法:
 *   thread_pool pool(4);
 *   pool.submit([]() { do_something(); });
 *   auto future = pool.submit([]() { return 42; });
 *   int result = future.get();
 */
class thread_pool {
public:
    /// 创建 fixed_size 个 worker 线程
    explicit thread_pool(size_t fixed_size);

    /// 停止所有线程，等待正在执行的任务完成
    ~thread_pool();

    /// 禁用拷贝
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    /**
     * 提交一个任务到队列
     *
     * @param f  可调用对象（函数/lambda/函数对象）
     * @param args 参数
     * @return std::future<返回值类型>
     *
     * 如果不需要返回值，可以忽略 future:
     *   pool.submit([]() { do_work(); });
     *
     * 如果需要返回值:
     *   auto f = pool.submit([]() { return compute(); });
     *   int result = f.get();
     */
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<decltype(f(args...))> {

        using return_type = decltype(f(args...));

        // 把函数 + 参数打包成 packaged_task
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("线程池已停止");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        cv_.notify_one();  // 唤醒一个 worker 线程
        return result;
    }

    /// 当前正在运行 + 排队的任务数
    size_t pending() const {
        return tasks_.size();
    }

    /// 线程池大小
    size_t size() const { return workers_.size(); }

private:
    /// worker 线程的主循环
    void worker_loop();

    std::vector<std::thread> workers_;          // worker 线程
    std::queue<std::function<void()>> tasks_;   // 任务队列
    mutable std::mutex mutex_;                  // 保护队列
    std::condition_variable cv_;                // 队列空时等待
    std::atomic<bool> stop_{false};             // 停止标志
};

#endif // AI_SWITCH_THREAD_POOL_H
