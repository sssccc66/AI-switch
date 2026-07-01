#ifndef AI_SWITCH_ROUTER_H
#define AI_SWITCH_ROUTER_H

#include <boost/beast/http.hpp>        // Beast HTTP 核心类型
#include <functional>                   // std::function
#include <string>
#include <unordered_map>

// Beast 的 http 命名空间, 后续代码用 http:: 前缀访问
namespace http = boost::beast::http;

/**
 * router — URL 路由分发器
 * =========================
 * 责任链的最后一环: 根据 HTTP 方法和路径找到对应的处理函数并调用。
 *
 * 用法:
 *   router r;
 *   r.add_route(http::verb::get, "/health", health_handler);
 *   auto res = r.dispatch(req);  // 自动匹配 → 调用 → 返回响应
 */
class router {
public:
    // ---- 类型别名 ----
    // 处理函数: 接收 HTTP 请求, 返回 HTTP 响应
    using handler_t = std::function<http::response<http::string_body>(
        const http::request<http::string_body>&)>;

    /// 注册一个路由: method + path → handler
    void add_route(http::verb method, const std::string& path, handler_t handler);

    /**
     * 分发请求: 查找匹配的路由并调用处理函数
     * 如果没有匹配的路由, 返回 404 Not Found
     */
    http::response<http::string_body> dispatch(const http::request<http::string_body>& req);

    // ================================================================
    // 静态辅助方法: 构建常见的 HTTP 响应
    // 这些是纯函数, 不依赖 router 的任何状态, 所以是 static
    // ================================================================

    /// 构建 JSON 响应 (200 OK + JSON body)
    static http::response<http::string_body> make_json_response(
        const std::string& body,          // 已经序列化的 JSON 字符串
        unsigned version,                  // HTTP 版本 (req.version())
        bool keep_alive);                  // 是否保持连接 (req.keep_alive())

    /// 构建错误响应 (指定 status + 错误信息, 格式化为 JSON)
    static http::response<http::string_body> make_error_response(
        http::status status,               // HTTP 状态码 (400/401/404/429/500)
        const std::string& code,           // 机器可读的错误码, 如 "not_found"
        const std::string& message,        // 人类可读的错误描述
        unsigned version,
        bool keep_alive);

private:
    // 路由表: key = "GET /health", value = handler
    // 用字符串拼接做 key, 比 (method, path) pair 更简单
    // 哈希表unordered_map 查找复杂度 O(1), 适合高并发场景
    std::unordered_map<std::string, handler_t> routes_;

    /// 将 method + path 拼成路由表查找 key
    static std::string make_key(http::verb method, const std::string& path);
};

#endif // AI_SWITCH_ROUTER_H
