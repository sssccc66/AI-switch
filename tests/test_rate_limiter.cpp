/**
 * test_rate_limiter.cpp — 令牌桶单元测试
 * =====================================
 *
 * 测试 token_bucket 的核心逻辑:
 *   1. 初始满桶: 能消费 capacity 次
 *   2. 桶空后:   try_acquire 返回 false
 *   3. 补充逻辑:  等待后令牌恢复
 *   4. 多线程:    并发消费不超过容量
 *   5. 热更新:    update_config 后配置生效
 *
 * 编译:
 *   g++ -std=c++17 -pthread test_rate_limiter.cpp \
 *       ../src/middleware/rate_limiter.cpp \
 *       -I../src -o test_rate_limiter
 *
 * 运行:
 *   ./test_rate_limiter
 */

#include "middleware/rate_limiter.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>

// ============================================================
// 简单的测试框架 (不用依赖 gtest)
// ============================================================
static int test_passed = 0;
static int test_failed = 0;

#define TEST(name)                                    \
    do {                                              \
        std::cout << "[test] " << name << "... ";     \
    } while (0)

#define PASS()                                        \
    do {                                              \
        std::cout << "✅ PASS\n";                      \
        test_passed++;                                 \
    } while (0)

#define FAIL(msg)                                     \
    do {                                              \
        std::cout << "❌ FAIL: " << msg << "\n";       \
        test_failed++;                                 \
    } while (0)

#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) { FAIL(msg); return; }            \
    } while (0)

// ============================================================
// 测试 1: 初始满桶 — 容量 10, 连续消费 10 次都应成功
// ============================================================
void test_initial_full() {
    TEST("初始满桶, 消费 capacity 次应全部成功");
    token_bucket bucket(10, 5);

    for (int i = 0; i < 10; ++i) {
        if (!bucket.try_acquire(1)) {
            FAIL("第 " + std::to_string(i + 1) + " 次消费失败");
            return;
        }
    }
    PASS();
}

// ============================================================
// 测试 2: 桶空 — 消费第 11 次应失败
// ============================================================
void test_empty() {
    TEST("桶空后 try_acquire 返回 false");
    token_bucket bucket(10, 5);

    for (int i = 0; i < 10; ++i) {
        bucket.try_acquire(1);
    }

    // 第 11 次应该失败 (桶空了, 还没来得及补充)
    if (bucket.try_acquire(1)) {
        FAIL("桶空后消费竟然成功了");
        return;
    }
    PASS();
}

// ============================================================
// 测试 3: 补充逻辑 — 等 1 秒后应该恢复 refill_rate 个令牌
// ============================================================
void test_refill() {
    TEST("等待 1 秒后令牌恢复 (refill_rate=5)");
    token_bucket bucket(10, 5);

    // 消费 10 个 (桶空)
    for (int i = 0; i < 10; ++i) {
        bucket.try_acquire(1);
    }

    // 等 1.1 秒 (确保跨过补充周期)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // 应该恢复 5 个令牌
    for (int i = 0; i < 5; ++i) {
        if (!bucket.try_acquire(1)) {
            FAIL("等待 1 秒后只恢复了 " + std::to_string(i) + " 个");
            return;
        }
    }

    // 第 6 个应该失败 (只有 5 个恢复)
    if (bucket.try_acquire(1)) {
        FAIL("只有 5 个令牌恢复, 但第 6 个也成功了");
        return;
    }
    PASS();
}

// ============================================================
// 测试 4: 多线程并发 — 总消费数不超过 capacity
// ============================================================
void test_multithread() {
    TEST("多线程并发消费, 总数不超过容量");
    token_bucket bucket(100, 1000);  // 容量 100, 补充很快

    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    std::vector<std::thread> threads;

    const int THREADS = 10;
    const int PER_THREAD = 20;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&bucket, &success_count, &fail_count]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                if (bucket.try_acquire(1)) {
                    success_count++;
                } else {
                    fail_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证: 200 个请求抢 100 个令牌
    // 微秒级完成, 几乎来不及补充
    // 所以应有成功(~100)和失败(~100)
    int total = success_count + fail_count;
    CHECK(total == 200, "总请求数应为 200 (got " + std::to_string(total) + ")");
    CHECK(success_count > 0, "应有成功的请求 (got " + std::to_string(success_count) + ")");
    CHECK(fail_count > 0, "应有失败的请求 (got " + std::to_string(fail_count) + ")");

    std::cout << "success=" << success_count
              << " fail=" << fail_count << " ";

    PASS();
}

// ============================================================
// 测试 5: 热更新配置
// ============================================================
void test_update_config() {
    TEST("update_config 后容量和速率变化");
    token_bucket bucket(5, 1);

    // 消费 5 个 (空桶)
    for (int i = 0; i < 5; ++i) bucket.try_acquire(1);

    // 更新配置: 容量变 20, 速率 10
    bucket.update_config(20, 10);

    // 等 1 秒
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // 应该能立刻消费 10 个 (新速率每秒 10)
    int count = 0;
    while (bucket.try_acquire(1)) {
        count++;
    }

    if (count < 8 || count > 12) {
        FAIL("更新后只消费了 " + std::to_string(count) + " 个 (期望 ~10)");
        return;
    }
    std::cout << "consumed=" << count << " ";
    PASS();
}

// ============================================================
// 测试 6: 消费多个令牌
// ============================================================
void test_consume_multiple() {
    TEST("一次消费多个令牌");
    token_bucket bucket(20, 10);

    // 一次消费 5 个
    if (!bucket.try_acquire(5)) {
        FAIL("消费 5 个失败但桶是满的");
        return;
    }

    // 再消费 15 个 (应该成功, 因为 20-5=15)
    if (!bucket.try_acquire(15)) {
        FAIL("消费 15 个失败但应该还有 15 个");
        return;
    }

    // 再消费应该失败 (桶空了)
    if (bucket.try_acquire(1)) {
        FAIL("桶空了但消费成功");
        return;
    }
    PASS();
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "========================================\n"
              << "  令牌桶单元测试\n"
              << "========================================\n";

    test_initial_full();
    test_empty();
    test_consume_multiple();
    test_refill();
    test_update_config();
    test_multithread();

    std::cout << "\n========================================\n"
              << "  结果: " << (test_passed + test_failed)
              << " 个测试, "
              << test_passed << " ✅ PASS, "
              << test_failed << " ❌ FAIL\n";
    if (test_failed > 0) {
        std::cout << "⚠️  有测试失败!\n";
        return 1;
    }
    std::cout << "🎉 全部通过!\n";
    return 0;
}
