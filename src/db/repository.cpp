#include "repository.h"
#include "connection_pool.h"

#include <iostream>      // 日志输出
#include <cstring>       // strlen
#include <random>        // 随机 Key 生成

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
api_key_result repository::find_api_key(const std::string& key) {
    // ---- 1. 借连接 ----
    MYSQL* conn = pool_->borrow();
    if (!conn) {
        std::cerr << "[repository] 无法获取数据库连接\n";
        return {{}, false, true};  // db_error = true
    }

    // ---- 2. 转义参数 (防 SQL 注入) ----
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
        return {{}, false, true};  // db_error = true
    }

    // ---- 4. 获取结果 ----
    MYSQL_RES* result_set = mysql_store_result(conn);
    if (!result_set) {
        std::cerr << "[repository] 获取结果失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return {{}, false, true};  // db_error = true
    }

    // ---- 5. 提取第一行 ----
    api_key_result result;  // found=false, db_error=false 默认

    MYSQL_ROW row = mysql_fetch_row(result_set);
    if (row) {
        // 解析 MySQL 行 → api_key_info 结构体
        api_key_info key_info;
        key_info.id         = row[0] ? std::stoll(row[0]) : 0;
        key_info.api_key    = row[1] ? row[1] : "";
        key_info.name       = row[2] ? row[2] : "";
        key_info.rate_limit = row[3] ? std::stoi(row[3]) : 60;

        // enabled 是 TINYINT(1), MySQL 返回字符串 "0" 或 "1"
        key_info.enabled    = row[4] && std::string(row[4]) == "1";

        if (key_info.enabled) {
            result.found = true;
            result.info = key_info;
        } else {
            std::cout << "[repository] API Key 已被禁用: " << key_info.name << "\n";
        }
    } else {
        // 没找到匹配的行
        std::cout << "[repository] API Key 未找到\n";
    }

    // ---- 6. 清理 ----
    mysql_free_result(result_set);
    pool_->return_connection(conn);

    return result;
}

// ============================================================
// generate_api_key — 生成随机 API Key
//
// 格式: sk_ + 32 位十六进制字符
// 总长度 35, 足够随机, 不可能被猜到
// ============================================================
std::string repository::generate_api_key() {
    static const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::string key = "sk_";
    for (int i = 0; i < 32; ++i) {
        key += hex_chars[dis(gen)];
    }
    return key;
}

// ============================================================
// create_api_key — 创建新的 API Key
//
// 生成随机 Key 并插入数据库, 返回完整的 api_key_info
// ============================================================
api_key_info repository::create_api_key(const std::string& name, int rate_limit) {
    MYSQL* conn = pool_->borrow();

    // 生成随机 Key
    std::string api_key = generate_api_key();

    // 转义参数 (防 SQL 注入)
    char escaped_name[256], escaped_key[256];
    mysql_real_escape_string(conn, escaped_name, name.c_str(), name.length());
    mysql_real_escape_string(conn, escaped_key, api_key.c_str(), api_key.length());

    // 执行 INSERT
    std::string sql = "INSERT INTO api_keys (api_key, name, rate_limit, enabled) VALUES ('"
                      + std::string(escaped_key) + "', '"
                      + std::string(escaped_name) + "', "
                      + std::to_string(rate_limit) + ", 1)";

    if (mysql_query(conn, sql.c_str()) != 0) {
        std::cerr << "[repository] 创建 Key 失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return {0, "", "", 0, false};
    }

    int64_t new_id = mysql_insert_id(conn);
    pool_->return_connection(conn);

    std::cout << "[repository] 创建 API Key: id=" << new_id
              << ", name=" << name << "\n";

    return {new_id, api_key, name, rate_limit, true};
}

// ============================================================
// list_api_keys — 列出所有 API Key
// ============================================================
std::vector<api_key_info> repository::list_api_keys() {
    MYSQL* conn = pool_->borrow();
    std::vector<api_key_info> keys;
    //不需要用户输入，不用防sql
    std::string sql = "SELECT id, api_key, name, rate_limit, enabled FROM api_keys ORDER BY id";

    if (mysql_query(conn, sql.c_str()) != 0) {
        std::cerr << "[repository] 查询 Key 列表失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return keys;
    }

    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) {
        pool_->return_connection(conn);
        return keys;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != nullptr) {
        api_key_info info;
        info.id         = row[0] ? std::stoll(row[0]) : 0;
        info.api_key    = row[1] ? row[1] : "";
        info.name       = row[2] ? row[2] : "";
        info.rate_limit = row[3] ? std::stoi(row[3]) : 60;
        info.enabled    = row[4] && std::string(row[4]) == "1";
        keys.push_back(info);
    }

    mysql_free_result(result);
    pool_->return_connection(conn);
    return keys;
}

// ============================================================
// disable_api_key — 禁用 API Key
//
// 设置 enabled = 0, 不删除数据 (保留审计记录)
// ============================================================
bool repository::disable_api_key(int64_t id) {
    MYSQL* conn = pool_->borrow();
    //整形参数防注入
    std::string sql = "UPDATE api_keys SET enabled = 0 WHERE id = " + std::to_string(id);

    if (mysql_query(conn, sql.c_str()) != 0) {
        std::cerr << "[repository] 禁用 Key 失败: " << mysql_error(conn) << "\n";
        pool_->return_connection(conn);
        return false;
    }

    bool affected = mysql_affected_rows(conn) > 0;
    pool_->return_connection(conn);

    if (affected) {
        std::cout << "[repository] 已禁用 API Key: id=" << id << "\n";
    } else {
        std::cout << "[repository] 未找到要禁用的 Key: id=" << id << "\n";
    }

    return affected;
}
