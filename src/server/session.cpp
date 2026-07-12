#include "session.h"
#include "adapter/adapter_factory.h"     // AI 适配器工厂
#include "util/thread_pool.h"             // 线程池
#include "stream/sse_handler.h"           // SSE 格式

#include <iostream>  // 控制台日志
#include <nlohmann/json.hpp>

namespace beast = boost::beast;  // Beast 命名空间缩写

using json = nlohmann::json;

// ============================================================
// 构造函数
// ============================================================
session::session(boost::asio::ip::tcp::socket socket,
                 std::shared_ptr<router> router,
                 std::shared_ptr<middleware_chain> mw_chain,
                 std::shared_ptr<adapter_factory> af,
                 thread_pool* pool)
    : stream_(std::move(socket))
    , router_(std::move(router))
    , mw_chain_(std::move(mw_chain))
    , adapter_factory_(std::move(af))
    , thread_pool_(pool)
{
}

// ============================================================
// run — session 入口
// ============================================================
void session::run() {
    boost::beast::error_code ec;
    stream_.socket().set_option(
        boost::asio::ip::tcp::no_delay(true), ec);
    if (ec) {
        std::cerr << "[session] 设置 no_delay 失败: " << ec.message() << "\n";
    }

    do_read();
}

// ============================================================
// do_read — 异步读取 HTTP 请求
// ============================================================
void session::do_read() {
    req_ = {};

    http::async_read(stream_, buffer_, req_,
        boost::beast::bind_front_handler(&session::on_read, shared_from_this()));
}

// ============================================================
// on_read — 读取完成回调
// ============================================================
void session::on_read(boost::beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec == http::error::end_of_stream) {
        do_close();
        return;
    }

    if (ec) {
        std::cerr << "[session] 读取错误: " << ec.message() << "\n";
        do_close();
        return;
    }

    handle_request();
}

// ============================================================
// handle_request — 处理请求
// ============================================================
void session::handle_request() {
    std::cout << "[session] "
              << http::to_string(req_.method()) << " "
              << req_.target() << "\n";

    context ctx(req_);
    mw_chain_->process(ctx);

    if (ctx.terminated) {
        res_ = std::move(ctx.response.value());
        do_write();
        return;
    }

    try {
        json body = json::parse(req_.body());
        bool want_stream = body.value("stream", false);

        if (want_stream && adapter_factory_ && adapter_factory_->count() > 0) {
            is_streaming_ = true;

            std::string model = body.value("model", "deepseek-chat");
            adapter* ai = adapter_factory_->create(model);

            if (!ai) {
                res_ = router::make_error_response(
                    http::status::bad_request, "unsupported_model",
                    "不支持的模型: " + model,
                    req_.version(), req_.keep_alive());
                do_write();
                return;
            }

            // 发送 SSE 响应头, 完成后自动启动流式线程
            start_stream(req_.version(), req_.keep_alive(),
                [self = shared_from_this(), ai, body]() {
                    // 使用线程池代替 std::thread
                    if (self->thread_pool_) {
                        self->thread_pool_->submit([self, ai, body]() {
                            try {
                                ai->chat_completion_stream(body,
                                    [self](std::string content, bool done) {
                                        boost::asio::post(self->stream_.get_executor(),
                                            [self, content, done]() {
                                                self->send_stream_chunk(
                                                    sse_handler::make_chunk(content), done);
                                            });
                                    });
                            } catch (const std::exception& e) {
                                std::cerr << "[session] 流式错误: " << e.what() << "\n";
                                boost::asio::post(self->stream_.get_executor(),
                                    [self, msg = std::string(e.what())]() {
                                        // 先发一条错误消息，再关闭
                                        std::string err = "data: {\"error\":{\"code\":\"provider_error\","
                                                          "\"message\":\"" + msg + "\"}}\n\n";
                                        self->send_stream_chunk(err, false);
                                        self->send_stream_chunk("", true);
                                    });
                            }
                        });
                    }
                });

            return;
        }
    } catch (...) {
        // JSON 解析失败走正常路由
    }

    // 能走到这里说明：
    //   ① 请求没被中间件拦截
    //   ② 要么客户端没要 stream，要么 JSON 解析失败，要么没有适配器走流式
    //   交给 router 按 URL 分发
    res_ = router_->dispatch(req_);
    do_write();
}

// ============================================================
// do_write — 异步写回 HTTP 响应
// ============================================================
void session::do_write() {
    http::async_write(stream_, res_,
        boost::beast::bind_front_handler(&session::on_write, shared_from_this()));
}

// ============================================================
// on_write — 写入完成回调
// ============================================================
void session::on_write(boost::beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        std::cerr << "[session] 写入错误: " << ec.message() << "\n";
        do_close();
        return;
    }

    if (res_.need_eof()) {
        do_close();
        return;
    }

    if (is_streaming_) {
        return;
    }

    res_ = {};
    do_read();
}

// ============================================================
// start_stream — 开始流式响应
// ============================================================
void session::start_stream(unsigned version, bool keep_alive,
                            std::function<void()> on_ready) {
    auto header = std::make_shared<std::string>(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
        "\r\n");

    auto self = shared_from_this();
    // 用 Asio async_write 绕过 Beast 的 buffer 限制
    boost::asio::async_write(stream_.socket(),
        boost::asio::buffer(header->data(), header->size()),
        [self, header, on_ready = std::move(on_ready)](boost::beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[session] 流式头写入失败: " << ec.message() << "\n";
                self->do_close();
                return;
            }
            self->stream_headers_sent_ = true;
            if (on_ready) {
                on_ready();  // 头发送完成 → 启动数据流
            }
        });
}

// ============================================================
// send_stream_chunk — 发送一个 SSE chunk
// ============================================================
void session::send_stream_chunk(std::string chunk_data, bool done) {
    if (!stream_headers_sent_) {
        return;
    }

    auto self = shared_from_this();

    if (done) {
        auto done_data = std::make_shared<std::string>(sse_handler::make_done_chunk());
        boost::asio::async_write(stream_.socket(),
            boost::asio::buffer(done_data->data(), done_data->size()),
            [self, done_data](boost::beast::error_code ec, std::size_t) {
                boost::ignore_unused(ec);
                self->is_streaming_ = false;
                self->do_close();
            });
        return;
    }

    auto data = std::make_shared<std::string>(std::move(chunk_data));
    boost::asio::async_write(stream_.socket(),
        boost::asio::buffer(data->data(), data->size()),
        [self, data](boost::beast::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[session] 流式写入失败: " << ec.message() << "\n";
                self->is_streaming_ = false;
                self->do_close();
            }
        });
}

// ============================================================
// do_close — 关闭 TCP 连接
// ============================================================
void session::do_close() {
    is_streaming_ = false;
    boost::beast::error_code ec;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
}