#include "auth_middleware.h"
#include "db/repository.h"
#include "server/router.h"      // 复用 router::make_error_response

#include <iostream>

// ============================================================
// Authorization header 格式解析
//
// 标准格式:
//   Authorization: Bearer sk-test-key-001
//
// 我们的解析逻辑:
//   1. 找 "Bearer " 前缀 (大小写不敏感, 但这里简单处理)
//   2. 取后面的字符串作为 API Key
//   3. 如果格式不对, 直接返回 401
//
// 为什么不支持其他认证方式?
//   AI API 网关的场景较为固定, Bearer token 足够了。
//   以后可以扩展支持 Basic Auth 等。
// ============================================================

/// 从 Authorization 头中提取 Bearer Token
/// 返回 token 字符串, 如果格式不对返回空字符串
static std::string extract_bearer_token(const http::request<http::string_body>& req) {
    // 获取 Authorization header
    auto auth_it = req.find(http::field::authorization);
    if (auth_it == req.end()) {
        return "";  // 没有 Authorization 头
    }

    std::string auth_value(auth_it->value());

    // 检查是否以 "Bearer " 开头
    const std::string prefix = "Bearer ";
    if (auth_value.size() <= prefix.size() ||
        auth_value.substr(0, prefix.size()) != prefix) {
        return "";  // 格式不对
    }

    // 提取 Bearer 后面的 token
    return auth_value.substr(prefix.size());
}

// ============================================================
// 构造函数
// ============================================================
auth_middleware::auth_middleware(std::shared_ptr<repository> repo)
    : repo_(std::move(repo))
{
}

// ============================================================
// process — 鉴权主逻辑
//
// 通过条件:
//   1. 请求路径在公开路径白名单中 (如 /health) → 直接放行
//   2. Authorization header 存在且格式为 "Bearer <key>"
//   3. key 在数据库 api_keys 表中存在且 enabled = 1
//
// 拦截条件 (返回 401):
//   - 没有 Authorization 头 (且不在白名单)
//   - 格式不是 "Bearer <key>"
//   - Key 在数据库中不存在
//   - Key 已被禁用 (enabled = 0)
// ============================================================
void auth_middleware::process(context& ctx) {
    // ---- 0. 检查是否在白名单中 ----
    // 某些路径不需要鉴权, 例如健康检查和管理接口
    std::string target(ctx.request.target());
    if (target == "/health" || target.find("/admin/") == 0) {
        // 健康检查和管理接口放行, 不检查 API Key
        return;
    }

    // ---- 1. 提取 Token ----
    std::string token = extract_bearer_token(ctx.request);

    if (token.empty()) {
        std::cout << "[auth] 鉴权失败: 缺少或格式错误的 Authorization 头\n";

        ctx.response = router::make_error_response(
            http::status::unauthorized,      // 401
            "auth_failed",
            "缺少有效的 Authorization 头, 格式: Bearer <api_key>",
            ctx.request.version(),
            ctx.request.keep_alive());
        ctx.terminated = true;
        return;
    }

    // ---- 2. 查询数据库 ----
    auto result = repo_->find_api_key(token);

    // 先检查数据库是否出错（连接池超时等）
    if (result.db_error) {
        std::cerr << "[auth] 数据库错误, 返回 503\n";
        ctx.response = router::make_error_response(
            http::status::service_unavailable,  // 503
            "service_unavailable",
            "服务暂时不可用, 请稍后重试",
            ctx.request.version(),
            ctx.request.keep_alive());
        ctx.terminated = true;
        return;
    }

    // 再检查 Key 是否有效
    if (!result.found) {
        // Key 不存在或被禁用
        std::cout << "[auth] 鉴权失败: API Key 无效或已禁用\n";

        ctx.response = router::make_error_response(
            http::status::unauthorized,
            "auth_failed",
            "API Key 无效或已被禁用",
            ctx.request.version(),
            ctx.request.keep_alive());
        ctx.terminated = true;
        return;
    }

    // ---- 3. ✅ 鉴权通过 ----
    // 在 context 中保存 api_key_id, 后续的中间件 (限流器、日志等) 会用到
    ctx.api_key_id = result.info.id;

    std::cout << "[auth] 鉴权通过: key_id=" << result.info.id
              << " (" << result.info.name << ")\n";

    // terminated 保持 false, chain 继续执行
}
