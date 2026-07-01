#ifndef AI_SWITCH_CONFIG_H
#define AI_SWITCH_CONFIG_H

#include <string>
#include <cstdint>

/**
 * app_config — 运行时配置结构体
 * ===============================
 * 从 config.json 读取, 供 http_server 使用。
 *
 * 所有字段都有默认值, 即使配置文件缺失也能以合理默认值启动。
 */
struct app_config {
    // ---- HTTP 服务器 ----
    std::string host = "0.0.0.0";   // 监听地址 (0.0.0.0 = 所有网卡)
    uint16_t    port = 8080;         // 监听端口
    int         thread_count = 2;    // Asio worker 线程数 (2 通常够用)

    // ---- 数据库 (MySQL) ----
    std::string db_host = "127.0.0.1";
    unsigned int db_port = 3306;
    std::string db_user = "root";
    std::string db_password;
    std::string db_name = "ai_switch";
    int db_pool_size = 4;

    // ---- 限流器 ----
    int64_t rate_limit_capacity = 10;       // 令牌桶容量 (最大突发请求数)
    int64_t rate_limit_refill_rate = 5;     // 每秒补充令牌数 (稳定速率)

    /// 配置文件是否成功加载
    /// from_file() 如果找不到文件, loaded = false, 但会返回默认值
    /// 这样 main.cpp 可以判断: 用户指定了 --config 但没加载到 → 报错退出
    bool loaded = false;

    /// 从 config.json 文件加载配置, 未填的字段保持默认值
    static app_config from_file(const std::string& path);
};

#endif // AI_SWITCH_CONFIG_H
