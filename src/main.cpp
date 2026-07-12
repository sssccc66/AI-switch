/**
 * main.cpp — AI-switch 网关入口
 * ===============================
 *
 * 这是整个程序的启动点, 负责:
 *   1. 解析命令行参数 (--config, --help)
 *   2. 加载配置文件 (默认 config.json, 支持环境变量覆盖)
 *   3. 从环境变量 DB_PASSWORD / DEEPSEEK_API_KEY 覆盖敏感配置
 *   4. 连接 MySQL, 初始化连接池
 *   5. 构建中间件链 (鉴权 → 限流 → 日志)
 *   6. 初始化 AI 适配器工厂 (DeepSeek / OpenAI)
 *   7. 创建线程池 (流式请求)
 *   8. 注册路由
 *   9. 启动 HTTP 服务器 (阻塞, 直到 Ctrl+C)
 *
 */

#include "util/config.h"
#include "server/http_server.h"
#include "server/router.h"
#include "db/connection_pool.h"     // MySQL 连接池
#include "db/repository.h"          // 数据库查询
#include "middleware/auth_middleware.h"  // 鉴权中间件
#include "middleware/rate_limiter.h"     // 限流中间件
#include "middleware/log_middleware.h"   // 日志中间件
#include "adapter/adapter_factory.h"    // AI 适配器工厂
#include "adapter/deepseek_adapter.h"   // DeepSeek 适配器
#include "util/thread_pool.h"           // 线程池

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
                      << "  DB_PASSWORD      覆盖数据库密码\n"
                      << "  DEEPSEEK_API_KEY 覆盖 DeepSeek API Key\n"
                      << "  OPENAI_API_KEY   覆盖 OpenAI API Key\n";
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
    const char* env_db_password = std::getenv("DB_PASSWORD");
    if (env_db_password) {
        config.db_password = env_db_password;
        std::cout << "[main] 数据库密码已从环境变量 DB_PASSWORD 覆盖\n";
    }

    // AI API Key 也从环境变量读取，不写死在配置文件里
    const char* env_deepseek_key = std::getenv("DEEPSEEK_API_KEY");
    if (env_deepseek_key) {
        config.deepseek_api_key = env_deepseek_key;
        std::cout << "[main] DeepSeek API Key 已从环境变量 DEEPSEEK_API_KEY 覆盖\n";
    }

    const char* env_openai_key = std::getenv("OPENAI_API_KEY");
    if (env_openai_key) {
        config.openai_api_key = env_openai_key;
        std::cout << "[main] OpenAI API Key 已从环境变量 OPENAI_API_KEY 覆盖\n";
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

    // 日志中间件: 记录请求信息和耗时
    mw_chain->add(std::make_unique<log_middleware>());

    // ---- 6. 初始化 AI 适配器工厂 ----
    auto ai_factory = std::make_shared<adapter_factory>();
    if (!config.deepseek_api_key.empty()) {
        ai_factory->register_adapter(
            std::make_unique<deepseek_adapter>(
                config.deepseek_api_key,
                config.deepseek_base_url
            )
        );
    }
    if (ai_factory->count() == 0) {
        std::cerr << "[main] 警告: 没有配置任何 AI 适配器\n";
    }

    // ---- 7. 创建线程池 ----
    thread_pool ai_thread_pool(config.thread_pool_size);

    // ---- 8. 设置路由 ----
    auto router_ptr = std::make_shared<router>();

    // GET /health — 健康检查 (不需要鉴权)
    // 注意: 中间件链会拦截所有请求, 包括 /health
    // 所以在 auth_middleware 里需要放过 /health 路径
    router_ptr->add_route(http::verb::get, "/health", handle_health);

    // POST /api/chat — 聊天接口 (调用 AI 适配器)
    // 使用 lambda 捕获 ai_factory, 在请求到来时调用
    router_ptr->add_route(http::verb::post, "/api/chat",
        [ai_factory](const http::request<http::string_body>& req)
            -> http::response<http::string_body> {

            // ---- 1. 解析请求体 ----
            json request_body;
            try {
                request_body = json::parse(req.body());
            } catch (const json::parse_error& e) {
                return router::make_error_response(
                    http::status::bad_request,
                    "invalid_json",
                    "请求体不是合法的 JSON: " + std::string(e.what()),
                    req.version(),
                    req.keep_alive());
            }

            // ---- 2. 检查是否有适配器可用 ----
            if (ai_factory->count() == 0) {
                return router::make_error_response(
                    http::status::service_unavailable,
                    "no_ai_configured",
                    "服务未配置任何 AI 模型, 请先设置 API Key",
                    req.version(),
                    req.keep_alive());
            }

            // ---- 3. 根据 model 选择适配器 ----
            std::string model = request_body.value("model", "deepseek-chat");//  从请求体取 model 字段，没写则默认 "deepseek-chat"
            adapter* ai = ai_factory->create(model);

            if (!ai) {
                return router::make_error_response(
                    http::status::bad_request,
                    "unsupported_model",
                    "不支持的模型: " + model,
                    req.version(),
                    req.keep_alive());
            }

            // ---- 4. 调用 AI API ----
            try {
                json response_body = ai->chat_completion(request_body);
                return router::make_json_response(
                    response_body.dump(),
                    req.version(),
                    req.keep_alive());
            } catch (const std::exception& e) {
                return router::make_error_response(
                    http::status::bad_gateway,
                    "provider_error",
                    "AI 服务响应错误: " + std::string(e.what()),
                    req.version(),
                    req.keep_alive());
            }
        }
    );

    // ---- 管理 API  ----
    // 管理接口的 Master Key 验证
    auto verify_admin = [&config](const http::request<http::string_body>& req) {
        auto auth = req.find(http::field::authorization);
        if (auth == req.end()) return false;
        std::string val(auth->value());
        return val == "Bearer " + config.admin_master_key;
    };

    // POST /admin/api-keys — 创建新 Key
    router_ptr->add_route(http::verb::post, "/admin/api-keys",
        [&config, repo, verify_admin](const http::request<http::string_body>& req)
            -> http::response<http::string_body> {

            if (!verify_admin(req)) {
                return router::make_error_response(
                    http::status::unauthorized, "auth_failed",
                    "无效的管理 Key", req.version(), req.keep_alive());
            }

            json body;
            try { body = json::parse(req.body()); }
            catch (...) {
                return router::make_error_response(
                    http::status::bad_request, "invalid_json",
                    "请求体不是合法 JSON", req.version(), req.keep_alive());
            }

            std::string name = body.value("name", "");
            int rate_limit = body.value("rate_limit", 60);
            if (name.empty()) {
                return router::make_error_response(
                    http::status::bad_request, "missing_name",
                    "缺少 name 字段", req.version(), req.keep_alive());
            }

            auto key = repo->create_api_key(name, rate_limit);
            json result = {{"api_key", key.api_key}, {"name", key.name}, {"rate_limit", key.rate_limit}};
            return router::make_json_response(result.dump(), req.version(), req.keep_alive());
        }
    );

    // GET /admin/api-keys — 列出所有 Key
    router_ptr->add_route(http::verb::get, "/admin/api-keys",
        [&config, repo, verify_admin](const http::request<http::string_body>& req)
            -> http::response<http::string_body> {

            if (!verify_admin(req)) {
                return router::make_error_response(
                    http::status::unauthorized, "auth_failed",
                    "无效的管理 Key", req.version(), req.keep_alive());
            }

            auto keys = repo->list_api_keys();
            json arr = json::array();
            for (const auto& k : keys) {
                arr.push_back({{"id", k.id}, {"api_key", k.api_key},
                               {"name", k.name}, {"rate_limit", k.rate_limit},
                               {"enabled", k.enabled}});
            }
            return router::make_json_response(arr.dump(), req.version(), req.keep_alive());
        }
    );

    // DELETE /admin/api-keys/{id} — 禁用 Key
    // 注意: URL 中的 ID 通过请求体传入 (简化实现)
    router_ptr->add_route(http::verb::delete_, "/admin/api-keys",
        [&config, repo, verify_admin](const http::request<http::string_body>& req)
            -> http::response<http::string_body> {

            if (!verify_admin(req)) {
                return router::make_error_response(
                    http::status::unauthorized, "auth_failed",
                    "无效的管理 Key", req.version(), req.keep_alive());
            }

            json body;
            try { body = json::parse(req.body()); }
            catch (...) {
                return router::make_error_response(
                    http::status::bad_request, "invalid_json",
                    "请传 {\"id\": N}", req.version(), req.keep_alive());
            }

            int64_t id = body.value("id", 0LL);
            if (id <= 0) {
                return router::make_error_response(
                    http::status::bad_request, "invalid_id",
                    "请传有效的 id", req.version(), req.keep_alive());
            }

            if (repo->disable_api_key(id)) {
                json result = {{"status", "disabled"}, {"id", id}};
                return router::make_json_response(result.dump(), req.version(), req.keep_alive());
            }

            return router::make_error_response(
                http::status::not_found, "not_found",
                "未找到 id=" + std::to_string(id) + " 的 Key",
                req.version(), req.keep_alive());
        }
    );

    // ---- 9. 启动 HTTP 服务器 ----
    try {
        http_server server(config, router_ptr, mw_chain, ai_factory, &ai_thread_pool);
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
