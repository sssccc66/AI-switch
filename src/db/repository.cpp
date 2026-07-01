#include "repository.h"
#include "connection_pool.h"

#include <iostream>      // 日志输出
#include <cstring>       // strlen

// ============================================================
// 构造函数 — 只需要保存连接池指针
// ============================================================
repository::repository(std::shared_ptr<connection_pool> pool)
    : pool_(std::move(pool))
{
}

// ============================================================
// find_api_key — 根据 Key 值查询 API Key 信息
//
// 流程:
//   1. 从连接池借一个连接
//   2. 转义 SQL 参数 (防止 SQL 注入)
//   3. 执行 SELECT 查询
//   4. 处理结果行
//   5. 归还连接
//
// SQL 注入说明:
//   客户端的 API Key 是用户输入的, 如果直接拼到 SQL 里,
//   恶意请求可以传入 "'; DROP TABLE api_keys; --" 来攻击。
//   mysql_real_escape_string() 会对特殊字符转义, 防止这种攻击。
// ============================================================
std::optional<api_key_info> repository::find_api_key(const std::string& key) {
    // ---- 1. 借连接 ----
    MYSQL* conn = pool_->borrow();
    if (!conn) {
        std::cerr << "[repository] 无法获取数据库连接\n";
        return std::nullopt;
    }

    // ---- 2. 转义参数 (防 SQL 注入) ----
    // 最大长度: API Key 64 字节 × 2 + 1 = 129 (转义后最多少许增长)
    char escaped_key[256];
    mysql_real_escape_string(conn, escaped_key, key.c_str(), key.length());

    // ---- 3. 执行查询 ----
    std::string sql = "SELECT id, api_key, name, rate_limit, enabled "
                      "FROM api_keys "
                      "WHERE api_key = '" + std::string(escaped_key) + "' "
                      "LIMIT 1";

    if (mysql_query(conn, sql.c_str()) != 0) {
        // 查询失败 (例如语法错误或连接断开)
        std::cerr << "[repository] 查询失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return std::nullopt;
    }

    // ---- 4. 获取结果 ----
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        std::cerr << "[repository] 获取结果失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return std::nullopt;
    }

    // ---- 5. 提取第一行 ----
    std::optional<api_key_info> info = std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row) {
        // 解析 MySQL 行 → api_key_info 结构体
        api_key_info key_info;
        key_info.id         = row[0] ? std::stoll(row[0]) : 0;
        key_info.api_key    = row[1] ? row[1] : "";
        key_info.name       = row[2] ? row[2] : "";
        key_info.rate_limit = row[3] ? std::stoi(row[3]) : 60;

        // enabled 是 TINYINT(1), MySQL 返回字符串 "0" 或 "1"
        key_info.enabled    = row[4] && std::string(row[4]) == "1";

        // 只有 enabled = true 的 Key 才返回
        if (key_info.enabled) {
            info = key_info;
        } else {
            std::cout << "[repository] API Key 已被禁用: " << key_info.name << "\n";
        }
    } else {
        // 没找到匹配的行
        std::cout << "[repository] API Key 未找到\n";
    }

    // ---- 6. 清理 ----
    mysql_free_result(result);   // 释放结果集
    pool_->return_connection(conn);  // 归还连接

    return info;
}
