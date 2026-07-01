/**
 * main.cpp — AI-switch 网关入口
 * ===============================
 *
 * 这是整个程序的启动点, 负责:
 *   1. 解析命令行参数 (--config, --help)
 *   2. 加载配置文件 (默认 config.json, 支持环境变量覆盖)
 *   3. 从环境变量 DB_PASSWORD 覆盖数据库密码
 *   4. 连接 MySQL, 初始化连接池
 *   5. 构建中间件链 (鉴权 → 限流 → 日志)
 *   6. 注册路由
 *   7. 启动 HTTP 服务器 (阻塞, 直到 Ctrl+C)
 *
 * 后续每周会在这里逐步添加更多初始化步骤:
 *   Week 4: 注册 AI 适配器
 *   Week 5: SSE 流式支持
 *   Week 6: 统计、热加载等
 */

#include "util/config.h"
#include "server/http_server.h"
#include "server/router.h"
#include "db/connection_pool.h"     // MySQL 连接池
#include "db/repository.h"          // 数据库查询
#include "middleware/auth_middleware.h"  // 鉴权中间件
#include "middleware/rate_limiter.h"     // 限流中间件

#include <iostream>
#include <string>
#include <cstdlib>       // getenv
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================
// 默认配置文件路径 (可用 --config 覆盖)
// ============================================================
static const std::string CONFIG_PATH = "config.json";

// ============================================================
// 路由处理函数: GET /health
//
// 健康检查接口, 返回服务器运行状态。
// 这是最简单的路由, 用来验证服务是否启动成功。
// ============================================================
http::response<http::string_body> handle_health(const http::request<http::string_body>& req) {
    // 构建健康检查 JSON
    json body = {
        {"status",  "ok"},
        {"version", "1.0.0"},
        {"service", "AI-switch"}
    };

    return router::make_json_response(
        body.dump(),
        req.version(),
        req.keep_alive());
}

// ============================================================
// 路由处理函数: POST /api/chat
//
// Week 1 的桩实现 (stub):
//   暂时只解析请求并返回一个模拟回复。
//   目的是验证 POST 请求 + JSON 收发的完整通路。
//
// Week 4 会替换为真正的 AI 适配器调用。
// ============================================================
http::response<http::string_body> handle_chat(const http::request<http::string_body>& req) {
    // ---- 1. 解析请求体 ----
    json request_body;
    try {
        request_body = json::parse(req.body());
    } catch (const json::parse_error& e) {
        // JSON 格式错误 → 400 Bad Request
        return router::make_error_response(
            http::status::bad_request,
            "invalid_json",
            "请求体不是合法的 JSON: " + std::string(e.what()),
            req.version(),
            req.keep_alive());
    }

    // 提取用户消息 (用于后续处理)
    std::string user_content;
    if (request_body.contains("messages") && !request_body["messages"].empty()) {
        user_content = request_body["messages"].back()["content"];
    }

    // ---- 2. 构建桩响应 (stub response) ----
    // Week 4 会改为: 调用 OpenAI/DeepSeek API 并返回真实结果
    json response_body = {
        {"id", "chat_stub_001"},
        {"model", request_body.value("model", "unknown")},
        {"choices", {{
            {"index", 0},
            {"message", {
                {"role", "assistant"},
                {"content", "这是 Week 1 的桩回复。你说了: \"" + user_content + "\"\n"
                            "真正的 AI 回复将在 Week 4 实现。"}
            }},
            {"finish_reason", "stop"}
        }}},
        {"usage", {
            {"prompt_tokens",     0},
            {"completion_tokens", 0},
            {"total_tokens",      0}
        }}
    };

    // 后续 Weeks: 这里还要把 message 存入数据库、记录日志等

    return router::make_json_response(
        response_body.dump(),
        req.version(),
        req.keep_alive());
}

// ============================================================
// main — 程序入口
// ============================================================
int main(int argc, char* argv[]) {
    std::cout << "========================================\n"
              << "  AI-switch v1.0.0 — C++ AI API 网关\n"
              << "========================================\n";

    // ---- 1. 解析命令行参数 ----
    // 支持:
    //   --config <path>   指定配置文件路径
    //   --help            显示帮助
    std::string config_path = CONFIG_PATH;  // 默认路径
    bool user_set_config = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            user_set_config = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [选项]\n"
                      << "选项:\n"
                      << "  --config <path>  指定配置文件路径 (默认: config.json)\n"
                      << "  --help, -h       显示此帮助信息\n"
                      << "\n"
                      << "环境变量:\n"
                      << "  DB_PASSWORD      覆盖数据库密码\n";
            return 0;
        }
    }

    // ---- 2. 加载配置 ----
    // 如果用户没有指定 --config, 默认 config.json 不存在也没关系
    // 但如果用户指定了 --config 但文件不存在, 就报错退出
    app_config config = app_config::from_file(config_path);

    if (user_set_config && !config.loaded) {
        std::cerr << "[main] 错误: 指定的配置文件不存在: " << config_path << "\n";
        return 1;
    }

    if (user_set_config) {
        std::cout << "[main] 使用配置文件: " << config_path << "\n";
    }

    // ---- 3. 环境变量覆盖配置 ----
    // 数据库密码是敏感信息, 从环境变量读取比写在 config.json 更安全
    // TODO: 如果环境变量 DB_PASSWORD 存在, 覆盖配置文件中的密码，后续连接数据库时使用环境变量的密码，生产部署时用环境变量覆盖    
    const char* env_db_password = std::getenv("DB_PASSWORD");
    if (env_db_password) {
        config.db_password = env_db_password;
        std::cout << "[main] 数据库密码已从环境变量 DB_PASSWORD 覆盖\n";
    }

    // ---- 4. 初始化数据库连接池 ----
    // 如果 MySQL 连不上, connection_pool 构造函数会抛异常
    // 这里单独捕获, 给出清晰的错误信息后退出
    std::shared_ptr<connection_pool> pool;
    std::shared_ptr<repository> repo;
    try {
        std::cout << "[main] 正在连接 MySQL..." << std::endl;
        pool = std::make_shared<connection_pool>(
            config.db_host, config.db_user, config.db_password,
            config.db_name, config.db_port, config.db_pool_size
        );
        repo = std::make_shared<repository>(pool);//仓库模式
    } catch (const std::exception& e) {
        std::cerr << "[main] 数据库初始化失败: " << e.what() << "\n";
        std::cerr << "[main] 提示: 检查 MySQL 是否运行, 以及 config.json 中的数据库配置\n";
        return 1;
    }

    // ---- 5. 构建中间件链 ----
    // 中间件链是一个责任链, 按添加顺序依次执行
    auto mw_chain = std::make_shared<middleware_chain>();

    // 鉴权中间件: 检查每个请求的 Authorization: Bearer <key>
    // 放在第一个位置: 无效的请求在中间件链最早就被拦截, 不浪费后续资源
    mw_chain->add(std::make_unique<auth_middleware>(repo));

    // 限流中间件: 令牌桶算法, 控制请求速率
    // 放在鉴权之后: 只有鉴权通过的请求才消耗令牌
    // 容量 10 = 短时间能并发 10 个请求
    // 速率 5/s = 稳定下来每秒最多 5 个请求
    mw_chain->add(std::make_unique<rate_limiter_middleware>(
        config.rate_limit_capacity,
        config.rate_limit_refill_rate
    ));

    // 后续 Week 4 会在这里添加: 日志中间件
    // mw_chain->add(std::make_unique<log_middleware>(...));

    // ---- 6. 设置路由 ----
    auto router_ptr = std::make_shared<router>();

    // GET /health — 健康检查 (不需要鉴权)
    // 注意: 中间件链会拦截所有请求, 包括 /health
    // 所以在 auth_middleware 里需要放过 /health 路径
    router_ptr->add_route(http::verb::get, "/health", handle_health);

    // POST /api/chat — 聊天接口 (桩实现)
    router_ptr->add_route(http::verb::post, "/api/chat", handle_chat);

    // 后续会加上的路由:
    // POST /api/session
    // GET  /api/session/{id}/history
    // GET  /api/metrics

    // ---- 7. 启动 HTTP 服务器 ----
    try {
        http_server server(config, router_ptr, mw_chain);
        server.run();  // 阻塞, 直到 Ctrl+C
    } catch (const std::exception& e) {
        std::cerr << "[main] 启动失败: " << e.what() << "\n";
        return 1;
    }
    // 受到 Ctrl+C 信号后,server.run()运行结束，try块跳出到这里，server的生命周期完成，调用析构函数，
    std::cout << "[main] AI-switch 已退出, 再见!\n";
    return 0;
    // 服务器退出后, 连接池析构, 所有 MySQL 连接关闭
}
