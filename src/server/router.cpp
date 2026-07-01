#include "router.h"

#include <nlohmann/json.hpp>  // 构建 JSON 错误体

using json = nlohmann::json;

// ============================================================
// make_key — 将 "GET" + "/health" 拼成 "GET /health"
// 用作 unordered_map 的查找键
// ============================================================
std::string router::make_key(http::verb method, const std::string& path) {
    // http::verb::get → "GET", 简单转为大写字符串
    // Beast 提供了 to_string() 方法
    std::string method_str = http::to_string(method);
    return method_str + " " + path;
}

// ============================================================
// add_route — 注册路由
// ============================================================
void router::add_route(http::verb method, const std::string& path, handler_t handler) {
    std::string key = make_key(method, path);
    routes_[key] = std::move(handler);
}

// ============================================================
// dispatch — 分发请求
// 查找匹配的路由, 找不到就返回 404
// ============================================================
http::response<http::string_body> router::dispatch(const http::request<http::string_body>& req) {
    std::string key = make_key(req.method(), std::string(req.target()));

    auto it = routes_.find(key);
    if (it != routes_.end()) {
        // ✅ 找到匹配的路由, 调用处理函数
        return it->second(req);
    }

    // ❌ 没有匹配的路由 → 404
    return make_error_response(
        http::status::not_found,
        "not_found",
        "请求的路径不存在: " + std::string(req.target()),
        req.version(),
        req.keep_alive());
}

// ============================================================
// make_json_response — 构建 JSON 响应
//
// 所有响应的公共设置:
//   - Content-Type: application/json
//   - Server: AI-switch/1.0 (标识网关)
//   - keep-alive 兼容
// ============================================================
http::response<http::string_body> router::make_json_response(
    const std::string& body,
    unsigned version,
    bool keep_alive) {

    http::response<http::string_body> res(http::status::ok, version);
    res.set(http::field::server, "AI-switch/1.0");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = body;
    res.prepare_payload();  // 自动计算 Content-Length
    return res;
}

// ============================================================
// make_error_response — 构建错误响应
//
// 统一错误体格式 (符合文档中的设计):
//   {"error": {"code": "xxx", "message": "xxx"}}
// ============================================================
http::response<http::string_body> router::make_error_response(
    http::status status,
    const std::string& code,
    const std::string& message,
    unsigned version,
    bool keep_alive) {

    // 组装统一的 JSON 错误体
    json error_body = {
        {"error", {
            {"code",    code},
            {"message", message}
        }}
    };

    http::response<http::string_body> res(status, version);
    res.set(http::field::server, "AI-switch/1.0");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(keep_alive);
    res.body() = error_body.dump();  // JSON 对象 → 字符串
    res.prepare_payload();
    return res;
}
