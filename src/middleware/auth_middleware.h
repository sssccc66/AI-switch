#ifndef AI_SWITCH_AUTH_MIDDLEWARE_H
#define AI_SWITCH_AUTH_MIDDLEWARE_H

#include "middleware/middleware.h"   // 继承自 middleware
#include <memory>                      // std::shared_ptr

// 前置声明 (避免在 .h 中引入 repository.h, 编译更快)
class repository;

/**
 * auth_middleware — API Key 鉴权中间件
 * =====================================
 *
 * 职责:
 *   1. 从 Authorization 头中提取 Bearer API Key
 *   2. 到数据库查询 Key 是否有效
 *   3. 有效 → 在 context 中设置 api_key_id, 放行
 *   4. 无效 → 返回 401 Unauthorized
 *
 * 插入位置:
 *   中间件链的第一环 (鉴权应该在限流之前做, 避免为无效请求消耗令牌)
 */
class auth_middleware : public middleware {
public:
    /**
     * @param repo 数据仓库 (由外部传入, 共享同一个 repository 实例)
     */
    explicit auth_middleware(std::shared_ptr<repository> repo);

    /**
     * 处理请求: 检查 Authorization header
     * 如果鉴权失败, 设置:
     *   ctx.terminated = true
     *   ctx.response = 401 响应
     */
    void process(context& ctx) override;

private:
    std::shared_ptr<repository> repo_;
};

#endif // AI_SWITCH_AUTH_MIDDLEWARE_H
