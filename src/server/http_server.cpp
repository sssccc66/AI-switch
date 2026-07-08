#include "http_server.h"
#include "session.h"        // 每个新连接创建一个 session

#include <iostream>          // 控制台输出
#include <thread>            // 多线程 worker
#include <vector>

// ============================================================
// 构造函数
//
// 1. 用 config 中的地址创建 acceptor
// 2. 设置重用地址选项 (SO_REUSEADDR)
//      好处: 服务重启时端口不会 "Address already in use"
// ============================================================
http_server::http_server(const app_config& config,
                         std::shared_ptr<router> router,
                         std::shared_ptr<middleware_chain> mw_chain,
                         std::shared_ptr<adapter_factory> adapter_factory)
    : ioc_(config.thread_count + 1)    // io_context 的并发提示 (非精确限制)
    , acceptor_(ioc_)                   // acceptor 绑定到 io_context
    , signals_(ioc_, SIGINT, SIGTERM)   // 注册要捕获的信号
    , router_(std::move(router))
    , mw_chain_(std::move(mw_chain))
    , adapter_factory_(std::move(adapter_factory))
    , thread_count_(config.thread_count)
{
    boost::beast::error_code ec;

    // 1. 解析地址
    auto address = boost::asio::ip::make_address(config.host, ec);
    if (ec) {
        std::cerr << "[server] 无效的地址: " << config.host << " — " << ec.message() << "\n";
        throw std::runtime_error("invalid host address");
    }

    // 2. 打开 acceptor
    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    if (ec) {
        std::cerr << "[server] 打开 acceptor 失败: " << ec.message() << "\n";
        throw std::runtime_error("acceptor open failed");
    }

    // 3. 设置 SO_REUSEADDR — 允许快速重启
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        std::cerr << "[server] 设置 reuse_address 失败: " << ec.message() << "\n";
        // 非致命, 继续
    }

    // 4. 绑定端口
    boost::asio::ip::tcp::endpoint endpoint(address, config.port);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "[server] 绑定 " << config.host << ":" << config.port
                  << " 失败: " << ec.message() << "\n";
        throw std::runtime_error("bind failed");
    }

    // 5. 开始监听 (backlog = 10, 表示等待 accept 的连接队列长度)
    acceptor_.listen(10, ec);
    if (ec) {
        std::cerr << "[server] 监听失败: " << ec.message() << "\n";
        throw std::runtime_error("listen failed");
    }

    std::cout << "[server] HTTP 服务已启动: http://" << config.host
              << ":" << config.port << "\n";
}

// ============================================================
// run — 启动事件循环
//
// 使用 Asio 原生的 signal_set 处理 Ctrl+C, 信号被集成到
// 事件循环中, 不会出现 POSIX 信号处理函数与事件循环争抢的问题。
//
// 架构:
//   1. 注册 signal_set 异步等待
//   2. 启动第一个异步 accept
//   3. 所有线程 (包括主线程) 都调用 ioc_.run() 处理事件
//   4. 收到信号 → signal_set 回调调用 ioc_.stop() → 所有线程退出
// ============================================================
void http_server::run() {
    // ---- 注册信号处理 (Asio 原生方式) ----
    // signals_ 已经在构造函数中绑定了 SIGINT + SIGTERM
    // 这里注册异步等待: 收到信号时自动调用 ioc_.stop()
    signals_.async_wait([this](boost::beast::error_code ec, int sig) {
        if (ec) return;  // 比如 signals_ 被销毁
        std::cout << "\n[server] 收到信号 " << sig << " (SIG"
                  << (sig == SIGINT ? "INT" : "TERM")
                  << "), 正在优雅停止...\n";
        stop();  // 关闭 acceptor + 停止 io_context
    });

    // ---- 启动第一个 accept ----
    do_accept();

    // ---- 启动 worker 线程 ----
    // 所有 worker 线程都调用 ioc_.run() — 这是 Asio 事件循环的主循环
    // 它们会一直运行, 直到 ioc_.stop() 被调用 (stop() 中触发)
    std::vector<std::thread> workers;
    for (int i = 0; i < thread_count_; ++i) {
        workers.emplace_back([this, i] {
            std::cout << "[server] Worker 线程 #" << (i + 1) << " 已启动\n";
            ioc_.run();
        });
    }

    std::cout << "[server] 主线程已加入事件循环 (共 "
              << (thread_count_ + 1) << " 个线程处理请求)\n";

    // ---- 主线程也参与事件循环 ----
    // 之前版本这里用了 while + run_one() + 信号变量检查,
    // 但那种方式有 bug: run_one() 阻塞时不响应信号。
    // 改用 asio::signal_set 后, 信号是事件循环的一部分,
    // 主线程直接跑 run() 即可, 信号到来时自动终止。
    ioc_.run();

    // ---- 等待 worker 线程结束 ----
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    std::cout << "[server] 服务已停止\n";
}

// ============================================================
// stop — 优雅关闭
//
// 1. 关闭 acceptor → 不再接受新连接, pending 的 async_accept 收到 operation_aborted
// 2. 停止 io_context → 所有正在 run() 的线程退出, session 会收到 cancelled 错误
// ============================================================
void http_server::stop() {
    boost::beast::error_code ec;

    // 1. 关闭 acceptor → 不再接受新连接
    acceptor_.close(ec);
    if (ec) {
        std::cerr << "[server] 关闭 acceptor 出错: " << ec.message() << "\n";
    }

    // 2. 停止 io_context → 所有 ioc_.run() 返回
    // 正在处理的 session 异步操作会收到 cancelled 错误码
    ioc_.stop();
}

// ============================================================
// do_accept — 异步接受新连接
//
// 每个新连接创建一个 session 对象, session 接管 socket 后
// 立刻开始下一次 accept, 然后 session 在后台处理请求。
//
// 这样就实现了: 一个线程可以同时管理多个连接
// (这正是 Asio 异步模型的优势)
// ============================================================
void http_server::do_accept() {
    // acceptor_.async_accept 异步等待新连接
    // socket 在回调中创建并传入
    acceptor_.async_accept(
        [this](boost::beast::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (ec) {
                // acceptor 已关闭 (比如 stop() 被调用)
                // 这是正常的停止流程, 不是错误
                if (ec != boost::asio::error::operation_aborted) {
                    std::cerr << "[server] 接受连接失败: " << ec.message() << "\n";
                }
                return;
            }

            // ✅ 收到新连接 → 创建 session, 启动它
            auto ses = std::make_shared<session>(
                std::move(socket), router_, mw_chain_, adapter_factory_);
            ses->run();

            // 继续等待下一个连接 (异步循环)
            do_accept();
        });
}
