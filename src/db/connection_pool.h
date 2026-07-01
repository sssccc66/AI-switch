#ifndef AI_SWITCH_CONNECTION_POOL_H
#define AI_SWITCH_CONNECTION_POOL_H

#include <mysql/mysql.h>        // MySQL C API: MYSQL*, mysql_* 函数

#include <string>
#include <queue>                // std::queue — 存放空闲连接
#include <mutex>                // std::mutex — 线程安全
#include <condition_variable>   // 连接不够时, 让调用者等待
#include <memory>               // std::unique_ptr

/**
 * connection_pool — MySQL 连接池
 * ===============================
 *
 * 为什么需要连接池?
 *   每次请求都创建/销毁 MySQL 连接开销很大 (TCP 握手 + MySQL 认证)。
 *   连接池预先创建一批连接, 业务线程"借用/归还", 避免反复创建。
 *
 * 线程安全:
 *   borrow() 和 return_connection() 都可以从多个线程同时调用
 *
 * 用法:
 *   connection_pool pool("127.0.0.1", "root", "password",
 *                        "ai_switch", 3306, 5 // 池大小
 *   );
 *
 *   auto conn = pool.borrow();       // 借一个连接
 *   mysql_query(conn, "SELECT ...");
 *   pool.return_connection(conn);    // 用完归还
 */
class connection_pool {
public:
    /**
     * 构造函数 — 创建连接池
     * @param host     MySQL 主机地址
     * @param user     用户名
     * @param password 密码
     * @param db       数据库名
     * @param port     端口
     * @param pool_size 池中维护的连接数
     */
    connection_pool(const std::string& host,
                    const std::string& user,
                    const std::string& password,
                    const std::string& db,
                    unsigned int port,
                    int pool_size);

    /// 析构 — 关闭所有连接
    ~connection_pool();

    /// 禁用拷贝和赋值 (连接池是 unique 资源)
    connection_pool(const connection_pool&) = delete;
    connection_pool& operator=(const connection_pool&) = delete;

    /**
     * 从池中借出一个连接
     * 如果池为空, 调用线程会阻塞等待直到有连接被归还
     * @return MYSQL* 连接指针, 用完必须归还
     */
    MYSQL* borrow();

    /**
     * 归还连接
     * 唤醒一个正在等待 borrow() 的线程
     */
    void return_connection(MYSQL* conn);

    /// 获取池大小
    int size() const { return pool_size_; }

private:
    /**
     * 创建一个新的 MySQL 连接并连接
     * @return MYSQL* 或 nullptr (失败)
     */
    MYSQL* create_connection();

    // ---- 连接参数 ----
    std::string host_;
    std::string user_;
    std::string password_;
    std::string db_;
    unsigned int port_;
    int pool_size_;

    // ---- 池状态 ----
    std::queue<MYSQL*> idle_connections_;   // 空闲连接队列
    std::mutex mutex_;                       // 保护队列的互斥锁
    std::condition_variable cv_;             // 队列为空时等待
};

#endif // AI_SWITCH_CONNECTION_POOL_H
