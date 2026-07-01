#include "session.h"
#include <iostream>  // 控制台日志, 后续会替换为正式 logger

// ============================================================
// 构造函数
//
// 参数:
//   socket   — 刚 accept 的 TCP socket, 用 std::move 转入 stream_
//   router   — 共享的路由器指针
//   mw_chain — 共享的中间件链指针 (鉴权→限流→日志)
//
// std::move 是必要的: socket 是 move-only 类型, 不能拷贝
// ============================================================
session::session(boost::asio::ip::tcp::socket socket,
                 std::shared_ptr<router> router,
                 std::shared_ptr<middleware_chain> mw_chain)
    : stream_(std::move(socket))      // 将 socket 包装成 tcp_stream
    , router_(std::move(router))      // 保存路由器引用
    , mw_chain_(std::move(mw_chain))  // 保存中间件链引用
{
}

// ============================================================
// run — session 入口
//
// 设置 socket 选项 (禁用 Nagle 算法, 减少延迟),
// 然后启动异步读取循环。
// ============================================================
void session::run() {
    // 禁用 Nagle 算法: 不缓冲小数据包, 对 SSE 流式响应很重要
    // 这个选项不是必须的, 但对 AI 网关这种低延迟场景有益
    boost::beast::error_code ec;
    stream_.socket().set_option(
        boost::asio::ip::tcp::no_delay(true), ec);
    if (ec) {
        std::cerr << "[session] 设置 no_delay 失败: " << ec.message() << "\n";
        // 非致命错误, 继续
    }

    do_read();
}

// ============================================================
// do_read — 异步读取 HTTP 请求
//
// 使用 http::async_read 从 stream_ 中读取一个完整的 HTTP 请求。
// 读到的数据存入 req_ 和 buffer_。
//
// beast::bind_front_handler 类似于 std::bind_front,
// 将 shared_from_this() 绑定到回调, 保证 session 在异步期间存活。
// ============================================================
void session::do_read() {
    // 每次读取前清空请求 (但 buffer 不清空, 可能有部分遗留数据)
    req_ = {};

    http::async_read(stream_, buffer_, req_,
        boost::beast::bind_front_handler(&session::on_read, shared_from_this()));
}

// ============================================================
// on_read — 读取完成回调
//
// 处理读取结果:
//   - end_of_stream: 客户端关闭连接, 我们也关闭
//   - 其他错误: 打印日志, 关闭
//   - 成功: 处理请求
// ============================================================
void session::on_read(boost::beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);  // 暂不使用, 避免编译器警告

    if (ec == http::error::end_of_stream) {
        // 客户端正常关闭连接 (发送了 FIN)
        do_close();
        return;
    }

    if (ec) {
        // 读取失败 (例如连接重置)
        std::cerr << "[session] 读取错误: " << ec.message() << "\n";
        do_close();
        return;
    }

    // ✅ 成功读取一个请求
    handle_request();
}

// ============================================================
// handle_request — 处理请求
//
// 处理流程 (Week 2+):
//   1. 打印请求日志 (方便调试)
//   2. 创建上下文, 运行中间件链 (鉴权 → 限流 → ...)
//   3. 如果中间件拦截了请求, 直接返回它的响应 (如 401)
//   4. 否则交给路由器分发
//
// 这里的处理是同步的 (在当前线程完成), 因为中间件和路由查找
// 都是纯 CPU 或快速 IO 操作, 不会长时间阻塞。
// ============================================================
void session::handle_request() {
    // 打印请求日志 (方便调试)
    std::cout << "[session] "
              << http::to_string(req_.method()) << " "
              << req_.target() << "\n";

    // ---- 1. 运行中间件链 ----
    context ctx(req_);                     // 创建请求上下文
    mw_chain_->process(ctx);               // 依次执行中间件

    if (ctx.terminated) {
        // 中间件拦截了请求 (如鉴权失败返回 401)
        // ctx.response 中有中间件设置的响应
        res_ = std::move(ctx.response.value());
    } else {
        // ---- 2. 中间件通过 → 路由分发 ----
        res_ = router_->dispatch(req_);
    }

    do_write();
}

// ============================================================
// do_write — 异步写回 HTTP 响应
//
// 注意: res_ 是成员变量, 在异步写入期间必须保持有效。
// 这也是为什么 res_ 是成员而不是局部变量。
// ============================================================
void session::do_write() {
    http::async_write(stream_, res_,
        boost::beast::bind_front_handler(&session::on_write, shared_from_this()));
}

// ============================================================
// on_write — 写入完成回调
//
// 如果 keep-alive, 继续读取下一个请求;
// 否则关闭连接。
// ============================================================
void session::on_write(boost::beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        std::cerr << "[session] 写入错误: " << ec.message() << "\n";
        do_close();
        return;
    }

    // need_eof() = true 表示根据 HTTP 协议应该关闭连接
    // 例如 HTTP/1.0 请求或 Connection: close
    if (res_.need_eof()) {
        do_close();
        return;
    }

    // 重置响应对象, 继续读取下一个请求 (HTTP keep-alive 长连接)
    res_ = {};
    do_read();
}

// ============================================================
// do_close — 关闭 TCP 连接
//
// 发送关闭信号, 然后销毁 session。
// ============================================================
void session::do_close() {
    boost::beast::error_code ec;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
    // ec 可忽略 — 如果对端已经关闭, shutdown 会报 not_connected
}
