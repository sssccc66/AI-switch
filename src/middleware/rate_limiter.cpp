#include "rate_limiter.h"
#include "server/router.h"     // 复用 router::make_error_response

#include <iostream>             // 日志
#include <algorithm>            // std::min

using clock_type = std::chrono::steady_clock;

// ============================================================
// 构造函数
//
// 初始化时令牌桶是满的 (tokens_ = capacity_)。
// 这样新服务启动后第一个请求不会被限流。
// ============================================================
token_bucket::token_bucket(int64_t capacity, int64_t refill_per_second)
    : tokens_(capacity)              // 初始满桶
    , capacity_(capacity)
    , refill_rate_(refill_per_second)
    , last_refill_(clock_type::now()) // 从此刻开始计时
{
}

// ============================================================
// refill — 补充令牌（懒更新）
//
// 核心思路:
//   不是每秒钟用定时器去补充，而是每次 try_acquire 时
//   计算距离上次补充过去了多久，然后一次性补上。
//
// double-check 模式:
//   第 1 层检查（无锁）: 快速判断是否需要补充，不需要就返回
//   第 2 层检查（有锁）: 拿到锁后重新计算，避免多人同时补
//
// 为什么不用定时器?
//   如果 10 分钟没有请求，这 10 分钟不需要执行任何补充操作。
//   懒更新让"空闲时不消耗 CPU"。
// ============================================================
void token_bucket::refill() {
    // ---- 第 1 层检查: 无锁 ----
    auto now = clock_type::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_refill_).count();

    if (elapsed <= 0) {
        return;  // 还没到 1 秒，不需要补充
    }

    // ---- 第 2 层检查: 加锁后 ----
    // 加锁后重新读时间，因为从第 1 层检查到现在可能已经过了很久
    std::lock_guard<std::mutex> lock(refill_mutex_);

    // double-check: 拿到锁后再次检查 (可能已经被别的线程补充过了)
    auto now2 = clock_type::now();
    elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now2 - last_refill_).count();

    if (elapsed <= 0) {
        return;  // 别的线程已经补充过了
    }

    // 计算要补充的令牌数
    // 用 memory_order_relaxed 因为 mutex 已经保证了 happens-before
    int64_t new_tokens = tokens_.load(std::memory_order_relaxed)
                       + elapsed * refill_rate_;

    // 不能超过桶容量
    if (new_tokens > capacity_) {
        new_tokens = capacity_;
    }

    tokens_.store(new_tokens, std::memory_order_relaxed);
    last_refill_ = now2;
}

// ============================================================
// try_acquire — 尝试消费令牌
//
// 这是整个限流器的核心方法。
//
// 步骤:
//   1. refill() — 先补充可能过期的令牌
//   2. CAS 循环 — 尝试原子消费
//
// CAS (Compare-And-Swap) 说明:
//   compare_exchange_weak(expected, desired) 做的事情:
//     如果 tokens_ == expected, 就把 tokens_ 设为 desired
//     否则 expected 被更新为 tokens_ 的当前值
//
//   循环的意义:
//     多个线程同时消费时, 只有一个会成功。
//     失败的线程重新读取最新值重试。
//
// 为什么不用 lock 而用 CAS?
//   请求量大的时候, CAS 比 mutex 快得多,
//   因为 CAS 是 CPU 指令级的原子操作, 不涉及系统调用。
// ============================================================
bool token_bucket::try_acquire(int64_t tokens) {
    // ---- Step 1: 补充 ----
    refill();

    // ---- Step 2: CAS 消费循环 ----
    int64_t expected = tokens_.load(std::memory_order_relaxed);

    while (expected >= tokens) {
        // 尝试: 如果 tokens_ 还是 expected, 就减去 tokens
        if (tokens_.compare_exchange_weak(
                expected,                     // 期望值 (失败时被更新)
                expected - tokens,            // 目标值
                std::memory_order_acq_rel,    // 成功时的内存序
                std::memory_order_relaxed)) { // 失败时的内存序
            // ✅ CAS 成功 → 消费完成
            return true;
        }
        // ❌ CAS 失败 → expected 已被更新为最新值 → 继续循环
    }

    // 令牌不够 → 限流
    return false;
}

// ============================================================
// update_config — 热更新配置
//
// 运行中调整限流参数, 不需要重启。
// capacity 和 refill_rate 本身不是 atomic 的,
// 但更新频率极低 (人工操作), 所以简单加锁保护。
// 实际生产环境可以用读写锁, 这里省略。
// ============================================================
void token_bucket::update_config(int64_t capacity, int64_t refill_per_second) {
    std::lock_guard<std::mutex> lock(refill_mutex_);

    capacity_ = capacity;
    refill_rate_ = refill_per_second;

    // 如果新的容量比当前令牌数小, 截断
    int64_t current = tokens_.load(std::memory_order_relaxed);
    if (current > capacity_) {
        tokens_.store(capacity_, std::memory_order_relaxed);
    }

    std::cout << "[token_bucket] 配置更新: capacity=" << capacity_
              << ", rate=" << refill_rate_ << "\n";
}

// ============================================================
// rate_limiter_middleware 实现
// ============================================================

rate_limiter_middleware::rate_limiter_middleware(
    int64_t capacity, int64_t refill_per_second)
    : bucket_(capacity, refill_per_second)
{
    std::cout << "[rate_limiter] 限流中间件就绪: capacity="
              << capacity << ", rate=" << refill_per_second << "/s\n";
}

void rate_limiter_middleware::process(context& ctx) {
    // 尝试消费 1 个令牌
    if (bucket_.try_acquire(1)) {
        // ✅ 有令牌, 放行
        return;
    }

    // ❌ 没有令牌, 返回 429 Too Many Requests
    std::cout << "[rate_limiter] 请求被限流 (api_key_id="
              << ctx.api_key_id << ")\n";

    ctx.response = router::make_error_response(
        http::status::too_many_requests,    // 429
        "rate_limited",
        "请求过于频繁, 请稍后重试",
        ctx.request.version(),
        ctx.request.keep_alive());
    ctx.terminated = true;
}
