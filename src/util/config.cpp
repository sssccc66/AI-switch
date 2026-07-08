#include "config.h"

#include <fstream>      // std::ifstream, 读取文件
#include <iostream>     // std::cerr, 错误输出
#include <nlohmann/json.hpp>  // JSON 解析

using json = nlohmann::json;

app_config app_config::from_file(const std::string& path) {
    app_config cfg;

    // 1. 打开配置文件
    std::ifstream file(path);
    if (!file.is_open()) {
        // 文件不存在不是致命错误 — 用默认值继续
        std::cerr << "[config] 警告: 无法打开 " << path
                  << ", 使用默认配置\n";
        return cfg;
    }

    // 2. 解析 JSON
    try {
        json root = json::parse(file);

        // 逐字段读取, 用 value() 方法指定默认值
        // 这样 config.json 中可以只写需要覆盖的字段
        if (root.contains("server")) {
            const auto& srv = root["server"];
            cfg.host         = srv.value("host", cfg.host);
            cfg.port         = srv.value("port", cfg.port);
            cfg.thread_count = srv.value("thread_count", cfg.thread_count);
        }

        // ---- 数据库配置 ----
        if (root.contains("database")) {
            const auto& db = root["database"];
            cfg.db_host     = db.value("host", cfg.db_host);
            cfg.db_port     = db.value("port", cfg.db_port);
            cfg.db_user     = db.value("user", cfg.db_user);
            cfg.db_password = db.value("password", cfg.db_password);
            cfg.db_name     = db.value("db", cfg.db_name);
            cfg.db_pool_size = db.value("pool_size", cfg.db_pool_size);
        }

        // ---- 限流器配置 ----
        if (root.contains("rate_limiter")) {
            const auto& rl = root["rate_limiter"];
            cfg.rate_limit_capacity    = rl.value("default_capacity",     cfg.rate_limit_capacity);
            cfg.rate_limit_refill_rate = rl.value("default_refill_rate", cfg.rate_limit_refill_rate);
        }

        // ---- AI 模型配置 ----
        if (root.contains("ai_models")) {
            const auto& models = root["ai_models"];

            if (models.contains("deepseek")) {
                const auto& ds = models["deepseek"];
                cfg.deepseek_api_key  = ds.value("api_key",  cfg.deepseek_api_key);
                cfg.deepseek_base_url = ds.value("base_url", cfg.deepseek_base_url);
            }
            if (models.contains("openai")) {
                const auto& oa = models["openai"];
                cfg.openai_api_key  = oa.value("api_key",  cfg.openai_api_key);
                cfg.openai_base_url = oa.value("base_url", cfg.openai_base_url);
            }
        }

        std::cout << "[config] 已加载配置: "
                  << cfg.host << ":" << cfg.port
                  << ", threads=" << cfg.thread_count
                  << ", db=" << cfg.db_name
                  << ", rate_limit=" << cfg.rate_limit_refill_rate << "/s"
                  << ", deepseek=" << (cfg.deepseek_api_key.empty() ? "未配置" : "已配置")
                  << ", openai=" << (cfg.openai_api_key.empty() ? "未配置" : "已配置")
                  << "\n";

        cfg.loaded = true;  // ✓ 标记配置已成功加载
    } catch (const json::parse_error& e) {
        // JSON 格式错误 — 仍用默认值继续, 但打印警告
        std::cerr << "[config] 警告: 配置文件格式错误 (" << e.what()
                  << "), 使用默认配置\n";
    }

    return cfg;
}
