#ifndef AI_SWITCH_LOG_MIDDLEWARE_H
#define AI_SWITCH_LOG_MIDDLEWARE_H

#include "middleware/middleware.h"
#include <chrono>

/**
 * log_middleware — 请求日志中间件
 * ================================
 *
 * 记录每次请求的:
 *   - HTTP 方法 + 路径
 *   - 鉴权的 API Key ID
 *   - 处理耗时
 *
 * 放在中间件链的最后一位:
 *   auth → rate_limiter → log
 *
 * 这样日志里能看见鉴权和限流的结果。
 */
class log_middleware : public middleware {
public:
    void process(context& ctx) override;
};

#endif // AI_SWITCH_LOG_MIDDLEWARE_H
