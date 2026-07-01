#ifndef AI_SWITCH_SESSION_H
#define AI_SWITCH_SESSION_H

#include <boost/beast/core.hpp>      // beast::tcp_stream, flat_buffer
#include <boost/beast/http.hpp>      // http::request, http::response
#include <boost/asio.hpp>            // asio::ip::tcp::socket

#include <memory>    // std::shared_ptr, std::enable_shared_from_this
#include "router.h"              // 用于 dispatch
#include "middleware/middleware.h"  // 中间件链: 鉴权 → 限流 → ...

/**
 * session — 一个 HTTP 连接的生命周期管理
 * =========================================
 *
 * 每个 TCP 连接对应一个 session 对象。
 * 核心是异步读写循环:
 *
 *   do_read() → on_read() → handle_request() → do_write() → on_write()
                                                                      ↓
                                                               (keep-alive) 回到 do_read()
 *
 * 使用 shared_from_this() 保证异步操作期间 session 不会被销毁。
 */
class session : public std::enable_shared_from_this<session> {
public:
    /**
     * 构造函数: 接管 socket (由 acceptor 传递过来)
     * router 和 middleware_chain 都是共享的 (所有 session 共用同一个实例)
     */
    session(boost::asio::ip::tcp::socket socket,
            std::shared_ptr<router> router,
            std::shared_ptr<middleware_chain> mw_chain);

    // 禁用拷贝 (session 是 unique 的资源管理类)
    session(const session&) = delete;
    session& operator=(const session&) = delete;

    /// 启动会话: 开始异步读取第一个请求
    void run();

private:
    /// 步骤 1: 异步读取 HTTP 请求
    void do_read();

    /// 步骤 2: 读取完成回调, 检查错误
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);

    /// 步骤 3: 交给路由器处理请求, 生成响应
    void handle_request();

    /// 步骤 4: 异步写回 HTTP 响应
    void do_write();

    /// 步骤 5: 写入完成回调, 决定是否继续读 (keep-alive)
    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred);

    /// 关闭 TCP 连接
    void do_close();

    // ---- 成员变量 ----

    boost::beast::tcp_stream stream_;          // 异步 TCP 流 (包装了 socket)
    boost::beast::flat_buffer buffer_;         // 读取缓冲区 (自动扩容)
    http::request<http::string_body> req_;     // 当前请求
    http::response<http::string_body> res_;    // 当前响应 (需要存活到 async_write 完成)
    std::shared_ptr<router> router_;           // 路由器 (线程安全, 只读)
    std::shared_ptr<middleware_chain> mw_chain_;  // 中间件链 (鉴权→限流→日志)
};

#endif // AI_SWITCH_SESSION_H
