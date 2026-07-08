#ifndef AI_SWITCH_HTTP_SERVER_H
#define AI_SWITCH_HTTP_SERVER_H

#include <boost/asio.hpp>                  // asio::io_context, asio::ip::tcp
#include <memory>                           // std::shared_ptr

#include "router.h"                         // 请求路由
#include "middleware/middleware.h"           // 中间件链
#include "util/config.h"                    // 服务器配置

// 前置声明
class adapter_factory;

/**
 * http_server — HTTP 服务器主类
 * ===============================
 *
 * 职责:
 *   1. 绑定端口, 开始监听
 *   2. 接受新连接, 为每个连接创建 session
 *   3. 在 io_context 上运行事件循环 (可多线程)
 *
 * 架构:
 *   acceptor_ 负责 accept 新连接
 *   ioc_      负责驱动所有异步操作
 *   router_   被所有 session 共享 (只读, 线程安全)
 *
 * 典型用法:
 *   auto router = std::make_shared<router>();
 *   router->add_route(http::verb::get, "/health", ...);
 *   http_server server(config, router);
 *   server.run();  // 阻塞, 直到收到停止信号
 */
class http_server {
public:
    /**
     * 构造函数: 创建 acceptor 但不启动
     * @param config  服务器配置 (端口, 线程数等)
     * @param router  共享的路由器 (已注册好路由)
     * @param mw_chain 共享的中间件链
     */
    http_server(const app_config& config,
                std::shared_ptr<router> router,
                std::shared_ptr<middleware_chain> mw_chain,
                std::shared_ptr<adapter_factory> adapter_factory);

    // 禁用拷贝
    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;

    /**
     * 启动服务器: 开始接受连接, 在当前线程阻塞运行事件循环
     *
     * @note 此函数会阻塞到 stop() 被调用或收到信号
     */
    void run();

    /// 优雅停止: 关闭 acceptor, 停止 io_context
    void stop();

private:
    /// 异步接受新连接
    void do_accept();

    // ---- 成员变量 ----

    boost::asio::io_context ioc_;                 // 事件循环引擎 (核心!)
    boost::asio::ip::tcp::acceptor acceptor_;     // TCP 连接接受器
    boost::asio::signal_set signals_;             // 信号集 (SIGINT/SIGTERM → 优雅退出)
    std::shared_ptr<router> router_;              // 路由表 (所有 session 共享)
    std::shared_ptr<middleware_chain> mw_chain_;  // 中间件链 (所有 session 共享)
    std::shared_ptr<adapter_factory> adapter_factory_; // AI 适配器工厂
    int thread_count_;                            // worker 线程数
};

#endif // AI_SWITCH_HTTP_SERVER_H
