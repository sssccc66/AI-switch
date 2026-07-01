#include "connection_pool.h"

#include <iostream>   // 错误日志
#include <stdexcept>   // std::runtime_error
#include <chrono>      // std::chrono::seconds

// ============================================================
// 构造函数
//
// 预先创建 pool_size 个连接, 放入空闲队列。
// 如果 0 个连接创建成功 → 抛异常, 阻止服务启动。
// 如果部分失败, 打印警告, 但能用的连接继续用。
// ============================================================
connection_pool::connection_pool(const std::string& host,
                                 const std::string& user,
                                 const std::string& password,
                                 const std::string& db,
                                 unsigned int port,
                                 int pool_size)
    : host_(host)
    , user_(user)
    , password_(password)
    , db_(db)
    , port_(port)
    , pool_size_(pool_size)
{
    for (int i = 0; i < pool_size; ++i) {
        MYSQL* conn = create_connection();
        if (conn) {
            idle_connections_.push(conn);
        } else {
            std::cerr << "[connection_pool] 第 " << (i + 1)
                      << " 个连接创建失败\n";
        }
    }

    int available = idle_connections_.size();
    std::cout << "[connection_pool] 连接池就绪: "
              << available << "/" << pool_size
              << " 个连接可用\n";

    // 一个连接都没创建成功 → 直接抛异常, 不让服务启动
    if (available == 0) {
        throw std::runtime_error("无法连接到 MySQL, 请检查数据库配置");
    }
}

// ============================================================
// 析构函数
//
// 关闭所有空闲连接。
// 注意: 如果还有连接被借出未归还, 程序行为是未定义的。
// 在实际项目中可以用引用计数来跟踪借出数量, 但这里为了简洁省略了。
// ============================================================
connection_pool::~connection_pool() {
    while (!idle_connections_.empty()) {
        MYSQL* conn = idle_connections_.front();
        idle_connections_.pop();
        mysql_close(conn);  // 关闭 MySQL 连接, 释放资源
    }
    std::cout << "[connection_pool] 连接池已关闭\n";
}

// ============================================================
// create_connection — 创建并连接一个 MySQL 连接
//
// 步骤:
//   1. mysql_init() — 初始化 MYSQL 对象
//   2. mysql_real_connect() — 连接到 MySQL 服务器
//   3. 设置字符集为 utf8mb4 (支持 emoji + 中文)
//
// 返回值: MYSQL* 或 nullptr (失败时)
// ============================================================
MYSQL* connection_pool::create_connection() {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "[connection_pool] mysql_init 失败\n";
        return nullptr;
    }

    // 连接到 MySQL 服务器
    if (mysql_real_connect(conn, host_.c_str(), user_.c_str(),
                           password_.c_str(), db_.c_str(),
                           port_, nullptr, 0) == nullptr) {
        std::cerr << "[connection_pool] 连接失败: "
                  << mysql_error(conn) << "\n";
        mysql_close(conn);
        return nullptr;
    }

    // 设置字符集 — 支持中文、emoji 等
    // AI 回复中时常有 emoji, utf8mb4 是必须的
    if (mysql_set_character_set(conn, "utf8mb4") != 0) {
        std::cerr << "[connection_pool] 设置字符集失败: "
                  << mysql_error(conn) << "\n";
        // 非致命, 继续
    }

    return conn;
}

// ============================================================
// borrow — 从池中借出一个连接
//
// 线程安全:
//   使用 unique_lock + condition_variable。
//   如果队列为空, 最多等待 5 秒。
//
// 超时保护:
//   之前的版本用 cv_.wait() 无限等待, 如果所有连接
//   都丢失了 (比如 MySQL 挂了), 线程永远卡死。
//   现改为 cv_.wait_for() + 5 秒超时, 超时返回 nullptr。
//   调用方 (repository) 需要检查 nullptr 并处理。
// ============================================================
MYSQL* connection_pool::borrow() {
    std::unique_lock<std::mutex> lock(mutex_);

    // 最多等待 5 秒, 等不到就返回 nullptr
    if (!cv_.wait_for(lock, std::chrono::seconds(5),
                      [this] { return !idle_connections_.empty(); })) {
        std::cerr << "[connection_pool] 等待连接超时 (5s)\n";
        return nullptr;
    }

    // 取出队首连接
    MYSQL* conn = idle_connections_.front();
    idle_connections_.pop();

    return conn;
}

// ============================================================
// return_connection — 归还连接
//
// 将连接放回队列, 并通知一个等待中的线程。
// 使用 notify_one 而不是 notify_all: 只需要唤醒一个等待者,
// 全部唤醒浪费 CPU。
// ============================================================
void connection_pool::return_connection(MYSQL* conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_connections_.push(conn);
    }

    // 通知一个正在 borrow() 中等待的线程
    cv_.notify_one();
}
