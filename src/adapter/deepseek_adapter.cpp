#include "deepseek_adapter.h"
#include "stream/sse_handler.h"

#include <curl/curl.h>                   // libcurl: CURL* 等
#include <iostream>                      // 错误日志
#include <stdexcept>                     // std::runtime_error
#include <cstring>                       // memcpy
#include <nlohmann/json.hpp>


using json = nlohmann::json;

// ============================================================
// 非流式写回调
//
// curl 收到数据后调用此函数，将数据累积到 string 中。
// 等所有数据到齐后一次性处理。
// ============================================================
static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userdata);
    str->append(static_cast<char*>(ptr), total);
    return total;
}

// ============================================================
// 流式写回调数据结构
//
// 传给 libcurl 的用户数据指针，包含：
//   - 用户的上层回调 (向客户端转发 chunk)
//   - SSE 解析器 (提取完整消息)
// ============================================================
struct stream_callback_data {
    adapter::stream_callback user_cb;     // 上层回调
    sse_handler parser;                    // SSE 解析器
    bool has_content = false;              // 是否已经有内容了
};

/// 流式写回调 (libcurl 每收到一块数据就调用)
static size_t stream_write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* data = static_cast<stream_callback_data*>(userdata);

    // 用 SSE 解析器提取完整消息
    std::string_view raw(static_cast<char*>(ptr), total);
    auto messages = data->parser.feed(raw);

    for (const auto& msg : messages) {
        // 尝试解析 JSON, 提取 choices[0].delta.content
        try {
            json chunk = json::parse(msg);

            auto& choices = chunk["choices"];
            if (!choices.empty() && choices[0].contains("delta")) {
                auto& delta = choices[0]["delta"];
                if (delta.contains("content")) {
                    std::string content = delta["content"];
                    data->user_cb(content, false);  // 只传文本内容, session 会包装成 SSE 格式
                }
            }
        } catch (...) {
            // 跳过无法解析的 chunk
        }
    }

    // 检查是否结束
    if (data->parser.is_done()) {
        data->user_cb("", true);
    }

    return total;
}

// ============================================================
// 构造函数
// ============================================================
deepseek_adapter::deepseek_adapter(const std::string& api_key,
                                   const std::string& base_url)
    : api_key_(api_key)
    , base_url_(base_url)
{
    std::cout << "[deepseek_adapter] 已初始化, base_url=" << base_url_ << "\n";
}

// ============================================================
// chat_completion — 非流式调用
// ============================================================
nlohmann::json deepseek_adapter::chat_completion(const nlohmann::json& request) {
    // 组装请求体
    nlohmann::json body;
    body["model"]       = request.value("model", "deepseek-chat");
    body["messages"]    = request["messages"];
    body["temperature"] = request.value("temperature", 0.7);
    body["max_tokens"]  = request.value("max_tokens", 2048);
    body["stream"]      = false;  // 非流式

    std::string url = base_url_ + "/chat/completions";
    std::string response_body = http_post(url, body.dump());

    try {
        return nlohmann::json::parse(response_body);
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[deepseek] JSON 解析失败: " << e.what() << "\n";
        return {
            {"error", {
                {"code", "parse_error"},
                {"message", "解析 AI 响应失败: " + std::string(e.what())}
            }}
        };
    }
}

// ============================================================
// chat_completion_stream — 流式调用
//
// 和 chat_completion 不同的是:
//   1. 请求体 stream 设为 true
//   2. 收到每个 SSE chunk 就调 cb(content, false)
//   3. 收到 [DONE] 后调 cb("", true)
//
// 这个函数会阻塞在 curl_easy_perform() 上，
// 但每次收到 chunk 都会通过回调转发出去。
// 调用方应该把回调放到 Asio 事件循环中。
// ============================================================
void deepseek_adapter::chat_completion_stream(
    const nlohmann::json& request, stream_callback cb) {

    // 组装请求体 (stream: true)
    nlohmann::json body;
    body["model"]       = request.value("model", "deepseek-chat");
    body["messages"]    = request["messages"];
    body["temperature"] = request.value("temperature", 0.7);
    body["max_tokens"]  = request.value("max_tokens", 2048);
    body["stream"]      = true;  // 流式!

    std::string body_str = body.dump();  // 必须存局部变量, 不能 body.dump().c_str()
    std::string url = base_url_ + "/chat/completions";

    // ---- 用 libcurl 发请求, 逐 chunk 处理 ----
    CURL* curl = curl_easy_init();
    if (!curl) {
        cb("", true);
        throw std::runtime_error("初始化 libcurl 失败");
    }

    stream_callback_data cb_data;
    cb_data.user_cb = std::move(cb);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());

    // 设置流式回调
    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &cb_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         120L);   // 流式请求可能较长
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);

    // 执行请求 (阻塞, 但每收到 chunk 会调回调)
    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // 检查网络错误
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        throw std::runtime_error(
            "HTTP 请求失败: " + std::string(curl_easy_strerror(res)));
    }

    // 检查 HTTP 状态码
    if (http_code != 200) {
        // 流式请求中如果状态码非 200，可能已经有部分数据通过回调发送了
        // 但还是要通知调用方出错了
        throw std::runtime_error(
            "API 返回错误 (HTTP " + std::to_string(http_code) + ")");
    }
}

// ============================================================
// http_post — 非流式 HTTP POST
// ============================================================
std::string deepseek_adapter::http_post(const std::string& url,
                                        const std::string& body) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("初始化 libcurl 失败");
    }

    std::string response_data;
    std::string error_buffer(CURL_ERROR_SIZE, '\0');

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &response_data);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,     &error_buffer[0]);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(
            "HTTP 请求失败: " + std::string(curl_easy_strerror(res)));
    }

    if (http_code != 200) {
        throw std::runtime_error(
            "API 返回错误 (HTTP " + std::to_string(http_code) + "): "
            + response_data);
    }

    return response_data;
}
