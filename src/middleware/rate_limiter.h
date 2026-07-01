#ifndef AI_SWITCH_RATE_LIMITER_H
#define AI_SWITCH_RATE_LIMITER_H

#include <atomic>                          // std::atomic, CAS
#include <chrono>                          // 时间计算
#include <cstdint>                         // int64_t
#include <mutex>                           // refill 时加锁
#include "middleware/middleware.h"          // context, middleware 基类

// ============================================================
// token_bucket — 令牌桶
// =========================
//
// 核心算法，面试重点。
//
// 原理:
//   桶里有一定数量的"令牌"，请求到来时拿走一个。
//   令牌按固定速率补充（每秒 refill_rate 个），
//   桶有容量上限（capacity），满了不再补充。
//
// 两个核心操作:
//   1. refill — 懒更新: 只在 try_acquire 被调用时计算"过去多久了，该补多少"
//   2. consume — 原子 CAS 消费，无锁
//
// 为什么消费无锁、补充有锁?
//   消费是高频操作（每个请求都触发），CAS 比锁快得多。
//   补充是低频操作（每秒最多触发几次），加锁避免多个线程重复计算。
// ============================================================
class token_bucket {
public:
    /**
     * @param capacity          桶容量 (最大突发量)
     * @param refill_per_second 每秒补充速率
     */
    token_bucket(int64_t capacity, int64_t refill_per_second);

    /// 禁用拷贝 (内部有 mutex, 不可拷贝)
    token_bucket(const token_bucket&) = delete;
    token_bucket& operator=(const token_bucket&) = delete;

    /**
     * 尝试消费 tokens 个令牌
     *
     * @param  需要消耗的令牌数 (通常为 1)
     * @return true  = 令牌够, 放行
     *         false = 令牌不够, 限流
     */
    bool try_acquire(int64_t tokens = 1);

    /**
     * 热更新配置 (线程安全)
     * 运行中改变限流参数, 不用重启服务
     */
    void update_config(int64_t capacity, int64_t refill_per_second);

    /// 获取当前令牌数 (近似值，用于监控)
    int64_t current_tokens() const {
        return tokens_.load(std::memory_order_relaxed);
    }

private:
    /**
     * 补充令牌 — 懒更新
     *
     * 计算上次补充到现在过去了多久，按速率补令牌。
     * 外层无锁检查，内层 double-check + mutex。
     *
     * 为什么 double-check?
     *   多个线程可能同时进入外层 if (elapsed > 0)，
     *   但只需要一个线程执行补充操作。
     *   内层再次检查，确保只有第一个拿到锁的线程执行补充。
     */
    void refill();

    // ---- 成员变量 ----

    std::atomic<int64_t> tokens_;     // 当前令牌数 (atomic, 多线程无锁读写)
    int64_t capacity_;                 // 桶容量 (构造函数设好就不变了)
    int64_t refill_rate_;             // 每秒补充数 (可热更新)

    // 注意: last_refill_ 不是 atomic 的,
    // 它只在 refill() 的 mutex 保护下读写, 所以安全
    std::chrono::steady_clock::time_point last_refill_;

    std::mutex refill_mutex_;         // 补充时互斥 (double-check 内层)
};

// ============================================================
// rate_limiter_middleware — 限流中间件
// ====================================
//
// 插入中间件链，对每个请求检查令牌桶。
// 如果令牌不够，返回 429 Too Many Requests。
//
// 位置: 在 auth_middleware 之后
//   auth_middleware → rate_limiter_middleware → ...
//
// 为什么放在鉴权后面?
//   无效的 API Key 直接在鉴权被拦，不消耗令牌。
// ============================================================
class rate_limiter_middleware : public middleware {
public:
    /**
     * @param capacity          桶容量
     * @param refill_per_second 每秒补充速率
     */
    rate_limiter_middleware(int64_t capacity, int64_t refill_per_second);

    /**
     * 处理请求: 检查令牌桶
     * 如果被限流, 设置:
     *   ctx.terminated = true
     *   ctx.response = 429 Too Many Requests
     */
    void process(context& ctx) override;

private:
    token_bucket bucket_;  // 令牌桶实例
};

#endif // AI_SWITCH_RATE_LIMITER_H
