#ifndef AI_SWITCH_MIDDLEWARE_H
#define AI_SWITCH_MIDDLEWARE_H

#include <boost/beast/http.hpp>     // http::request, http::response
#include <memory>                    // std::unique_ptr
#include <optional>                  // std::optional — C++17
#include <vector>
#include <cstdint>                   // int64_t

namespace http = boost::beast::http;

/**
 * context — 请求上下文
 * =========================
 *
 * 这是贯穿中间件链的数据载体。
 * 每个中间件读取/修改 context, 然后传给下一个中间件。
 *
 * 字段说明:
 *   request     — 原始的 HTTP 请求 (只读引用)
 *   response    — 如果某个中间件拦截了请求, 在这里设置响应
 *   terminated  — 是否被拦截 (为 true 时, 链停止, 直接返回 response)
 *   api_key_id  — 鉴权中间件查到的 API Key ID, 后续给限流器和日志用
 */
struct context {
    // 注意: 这里存的是引用, context 必须在 request 的生命周期内使用
    const http::request<http::string_body>& request;

    // 拦截响应: 如果被中间件拦截, 这里会有值
    std::optional<http::response<http::string_body>> response;

    // 链是否终止 (中间件决定拦截后会设为 true)
    bool terminated = false;

    // ---- 鉴权数据 (由 auth_middleware 填充) ----
    int64_t api_key_id = -1;   // -1 = 未鉴权或鉴权失败

    explicit context(const http::request<http::string_body>& req)
        : request(req) {}
};

/**
 * middleware — 中间件抽象基类
 * ============================
 *
 * 责任链模式中的一环。
 * 每个中间件可以:
 *   1. 检查 context, 通过后让链继续
 *   2. 拦截请求 (设置 response + terminated = true)
 *
 * 典型用法:
 *   class auth_middleware : public middleware {
 *       context_ptr process(context_ptr ctx) override;
 *   };
 */
class middleware {
public:
    virtual ~middleware() = default;

    /**
     * 处理请求上下文
     * @param ctx 当前的上下文 (可修改)
     * @return 处理后的上下文
     *
     * 如果要拦截请求, 设置:
     *   ctx->response = 响应对象;
     *   ctx->terminated = true;
     * 然后返回 ctx。
     */
    virtual void process(context& ctx) = 0;
};

/**
 * middleware_chain — 中间件链
 * ===========================
 *
 * 按注册顺序依次调用每个中间件。
 * 只要中间件设置了 terminated = true, 链立刻停止。
 *
 * 用法:
 *   middleware_chain chain;
 *   chain.add(std::make_unique<auth_middleware>(pool));
 *   chain.add(std::make_unique<rate_limiter>(...));
 *
 *   context ctx(req);
 *   chain.process(ctx);
 *   if (ctx.terminated) {
 *       // 中间件拦截了请求, ctx.response 中有响应
 *   }
 */
class middleware_chain {
public:
    /// 添加一个中间件到链尾
    void add(std::unique_ptr<middleware> mw) {
        middlewares_.push_back(std::move(mw));
    }

    /**
     * 依次执行所有中间件
     * 如果某个中间件设置了 terminated = true, 提前结束
     */
    void process(context& ctx) {
        for (auto& mw : middlewares_) {
            mw->process(ctx);
            if (ctx.terminated) {
                return;  // 中间件拦截了请求
            }
        }
    }

    /// 清空所有中间件
    void clear() { middlewares_.clear(); }

    /// 中间件数量
    size_t size() const { return middlewares_.size(); }

private:
    std::vector<std::unique_ptr<middleware>> middlewares_;
};

#endif // AI_SWITCH_MIDDLEWARE_H
