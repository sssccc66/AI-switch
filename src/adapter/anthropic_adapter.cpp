#include "anthropic_adapter.h"

#include <curl/curl.h>                   // libcurl: CURL* 等
#include <iostream>                      // 错误日志
#include <stdexcept>                     // std::runtime_error
#include <utility>                       // memcpy
#include <vector>
#include <string_view>
#include <nlohmann/json.hpp>


using json = nlohmann::json;

// ============================================================
// 非流式写回调(与openai_compatible_adapter相同)
// ============================================================
static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userdata);
    str->append(static_cast<char*>(ptr), total);
    return total;
}

// ============================================================
// anthropic_stream_parser — Anthropic 专用 SSE 事件解析器
// ================================================================
//
// Anthropic 的流式事件格式和 OpenAI 完全不同, 不能复用 sse_handler:
//
//   event: message_start
//   data: {"type":"message_start", ...}
//
//   event: content_block_delta
//   data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"你"}}
//
//   event: message_stop
//   data: {"type":"message_stop"}
//
// 每条事件由 "event: <type>\ndata: <json>\n\n" 组成 (以两个 \n 结尾),
// 结束标志是收到 event: message_stop, 而不是 OpenAI 那种 "data: [DONE]"。
//
// 这个解析器只做两件事:
//   1. 从 libcurl 收到的原始字节流里切出完整的 "event/data" 事件块
//   2. 提取 content_block_delta 里的文本内容, 判断 message_stop
// ============================================================
struct anthropic_stream_parser {
    std::string buffer;
    bool done = false;

    /// 喂入新数据, 返回本次提取出的文本增量列表
    std::vector<std::string> feed(std::string_view raw) {
        buffer.append(raw);
        std::vector<std::string> texts;

        while (true) {
            size_t pos = buffer.find("\n\n");
            if (pos == std::string::npos) break;

            std::string block = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);

            // block 形如:
            //   event: content_block_delta\ndata: {...}
            std::string event_type;
            std::string data_line;

            size_t start = 0;
            while (start <= block.size()) {
                size_t nl = block.find('\n', start);
                std::string line = (nl == std::string::npos)
                    ? block.substr(start)
                    : block.substr(start, nl - start);

                if (line.rfind("event: ", 0) == 0) {
                    event_type = line.substr(7);
                } else if (line.rfind("data: ", 0) == 0) {
                    data_line = line.substr(6);
                }

                if (nl == std::string::npos) break;
                start = nl + 1;
            }

            if (event_type == "message_stop") {
                done = true;
                continue;
            }

            if (event_type == "content_block_delta" && !data_line.empty()) {
                try {
                    json chunk = json::parse(data_line);
                    if (chunk.contains("delta") && chunk["delta"].contains("text")) {
                        texts.push_back(chunk["delta"]["text"].get<std::string>());
                    }
                } catch (...) {
                    // 跳过无法解析的事件
                }
            }
            // 其余事件类型 (message_start / content_block_start /
            // content_block_stop / message_delta / ping) 不携带文本内容, 忽略
        }

        return texts;
    }
};

// ============================================================
// 流式写回调数据结构
//
// 传给 libcurl 的用户数据指针，包含：
//   - 用户的上层回调 (向客户端转发 chunk)
//   - SSE 解析器 (提取完整消息)
// ============================================================
struct stream_callback_data {
    adapter::stream_callback user_cb;     // 上层回调
    anthropic_stream_parser parser;                    // Anthropic SSE 解析器
};

/// 流式写回调 (libcurl 每收到一块数据就调用)
static size_t stream_write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* data = static_cast<stream_callback_data*>(userdata);

    // 用 SSE 解析器提取完整消息
    std::string_view raw(static_cast<char*>(ptr), total);
    auto texts = data->parser.feed(raw);

    for (const auto& text : texts) {
        data->user_cb(text, false);  // 只传文本内容, session 会包装成 SSE 格式
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
anthropic_adapter::anthropic_adapter(const std::string& api_key,
                                     const std::string& base_url,
                                     const std::string& default_model,
                                     const std::string& anthropic_version)
    : api_key_(api_key)
    , base_url_(base_url)
    , default_model_(default_model)
    , anthropic_version_(anthropic_version)
{
    std::cout << "[" << anthropic_adapter << "] 已初始化, base_url=" << base_url_ << "\n";
}

// ============================================================
// build_request_body — 把统一请求格式转换成 Anthropic 要求的格式
//
// 关键转换:
//   1. messages 里 role="system" 的内容提取到独立的 "system" 字段
//      (Anthropic 不允许 messages 数组里出现 role="system")
//   2. max_tokens 是必填字段, 没传则给默认值
// ============================================================
nlohmann::json anthropic_adapter::build_request_body(
    const nlohmann::json& request, bool stream) const {

    nlohmann::json body;
    body["model"]      = request.value("model", default_model_);
    body["max_tokens"] = request.value("max_tokens", 2048);  // Anthropic 必填
    body["temperature"] = request.value("temperature", 0.7);
    body["stream"]     = stream;

    nlohmann::json messages = nlohmann::json::array();
    std::string system_prompt;

    if (request.contains("messages")) {
        for (const auto& msg : request["messages"]) {
            std::string role = msg.value("role", "user");
            if (role == "system") {
                // Anthropic 的 system 是独立字段, 多条 system 消息就拼接起来
                if (!system_prompt.empty()) system_prompt += "\n";
                system_prompt += msg.value("content", "");
            } else {
                messages.push_back(msg);
            }
        }
    }

    body["messages"] = messages;
    if (!system_prompt.empty()) {
        body["system"] = system_prompt;
    }

    return body;
}

// ============================================================
// chat_completion — 非流式调用
//
// Anthropic 响应结构:
//   { "content": [ { "type": "text", "text": "..." } ], ... }
// 为了让上层代码 (router/session) 不用关心底层是哪个厂商,
// 这里把响应转换成 OpenAI 风格返回:
//   { "choices": [ { "message": { "role": "assistant", "content": "..." } } ] }
// ============================================================
nlohmann::json anthropic_adapter::chat_completion(const nlohmann::json& request) {
    nlohmann::json body = build_request_body(request, false);

    std::string url = base_url_ + "/messages";
    std::string response_body = http_post(url, body.dump());

    try {
        json anthropic_resp = json::parse(response_body);

        // Anthropic 返回错误时是 { "type": "error", "error": {...} }, 原样透传
        if (anthropic_resp.contains("error")) {
            return { {"error", anthropic_resp["error"]} };
        }

        std::string text;
        if (anthropic_resp.contains("content") && anthropic_resp["content"].is_array()) {
            for (const auto& block : anthropic_resp["content"]) {
                if (block.value("type", "") == "text") {
                    text += block.value("text", "");
                }
            }
        }

        // 转换成 OpenAI 风格响应, 方便上层统一处理
        return {
            {"id", anthropic_resp.value("id", "")},
            {"model", anthropic_resp.value("model", "")},
            {"choices", {
                {
                    {"index", 0},
                    {"message", {
                        {"role", "assistant"},
                        {"content", text}
                    }},
                    {"finish_reason", anthropic_resp.value("stop_reason", "stop")}
                }
            }}
        };
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[anthropic] JSON 解析失败: " << e.what() << "\n";
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
// ============================================================
void anthropic_adapter::chat_completion_stream(
    const nlohmann::json& request, stream_callback cb) {

    // 组装请求体 (stream: true)
    nlohmann::json body=build_request_body(request, true);
    std::string url = base_url_ + "messages";

    http_post_stream(url, body.dump(), std::move(cb));
}

// ============================================================
// http_post — 非流式 HTTP POST
// ============================================================
std::string anthropic_adapter::http_post(const std::string& url,
                                                 const std::string& body) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("初始化 libcurl 失败");
    }

    std::string response_data;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("x-api-key: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("anthropic-version: " + anthropic_version_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &response_data);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,     &error_buffer[0]);
    // 非流式也用低速超时, 兼容 AI 模型生成时间长的场景
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);    // 每秒至少 1 字节
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);  // 连续 30 秒无数据则超时
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


// ============================================================
// http_post — 非流式 HTTP POST
// ============================================================
void anthropic_adapter::http_post_stream(
    const std::string& url, const std::string& body, stream_callback_t& cb) const {
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
    headers = curl_slist_append(headers, ("x-api-key: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("anthropic-version: " + anthropic_version_).c_str());

    // 设置流式回调
    curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &cb_data);
    // 流式用低速超时: 30 秒内没收到任何数据才超时, 避免长回答被掐断
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);    // 每秒至少 1 字节
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);  // 连续 30 秒无数据则超时
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


