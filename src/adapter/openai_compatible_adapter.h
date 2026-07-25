#ifndef AI_SWITCH_OPENAI_COMPATIBLE_ADAPTER_H
#define AI_SWITCH_OPENAI_COMPATIBLE_ADAPTER_H

#include "adapter.h"
#include <string>

/**
 * D_adapter — OpenAI 协议兼容适配器基类
 * =========================================================
 *
 * 背景:
 *   OpenAI、DeepSeek、以及其他很多厂商 (月之暗面 Kimi、通义千问兼容模式等)
 *   都直接照搬了 OpenAI 的 Chat Completions API 格式:
 *     - 请求体: { "model", "messages", "temperature", "max_tokens", "stream" }
 *     - 非流式响应: choices[0].message.content
 *     - 流式响应:   SSE, choices[0].delta.content, 以 "data: [DONE]" 结束
 *     - 认证头:     Authorization: Bearer <api_key>
 *
 *   这些厂商之间唯一的区别是 base_url、api_key、默认模型名、
 *   以及 adapter_factory 用来路由请求的 model 前缀。
 *   所以把 HTTP 请求组装 / 响应解析 / SSE 处理这部分公共逻辑
 *   收敛到这个基类里, 具体厂商的适配器 (deepseek_adapter / openai_adapter)
 *   只需要在构造函数里传入各自的差异化配置即可, 不需要重复实现一遍。
 *
 * 注意:
 *   这个基类只适用于"协议真正兼容 OpenAI"的厂商。
 *   如果要接入协议不同的厂商 (比如 Anthropic Claude 的 /v1/messages,
 *   使用 x-api-key 认证、system 字段独立、流式事件类型不同),
 *   不应该勉强复用这个基类, 而应该直接继承顶层的 adapter 接口,
 *   独立实现一套请求/解析逻辑。
 */
class openai_compatible_adapter : public adapter {
public:
    /**
     * @param api_key       该厂商的 API Key
     * @param base_url      API 基础地址 (不含 /chat/completions), 例如:
     *                      "https://api.deepseek.com/v1 "
     *                      "https://api.openai.com/v1 "
     * @param default_model 请求体未指定 model 字段时使用的默认模型名
     * @param model_prefix  adapter_factory 路由匹配用的模型名前缀, 例如 "deepseek"、"gpt"
     * @param provider_name 仅用于日志打印, 例如 "deepseek"、"openai"
     */
    openai_compatible_adapter(std::string api_key,
                               std::string base_url,
                               std::string default_model,
                               std::string model_prefix,
                               std::string provider_name);

    nlohmann::json chat_completion(const nlohmann::json& request) override;
    void chat_completion_stream(const nlohmann::json& request, stream_callback cb) override;

    std::string model_prefix() const override { return model_prefix_; }

private:
    /// 根据请求体和 stream 标志组装符合 OpenAI 协议的请求 JSON
    nlohmann::json build_request_body(const nlohmann::json& request, bool stream) const;

    /**
     * 用 libcurl 发送 HTTP POST 请求 (非流式)
     *
     * @param url    完整请求 URL
     * @param body   JSON 请求体字符串
     * @return       HTTP 响应体字符串
     * @throws std::runtime_error 如果请求失败或返回状态码非 200
     */
    std::string http_post(const std::string& url, const std::string& body) const;

    /// 流式 HTTP POST, 每收到一个 SSE chunk 就通过 cb 转发内容
    void http_post_stream(const std::string& url, const std::string& body,
                           stream_callback cb) const;

    std::string api_key_;
    std::string base_url_;
    std::string default_model_;
    std::string model_prefix_;
    std::string provider_name_;
};

#endif // AI_SWITCH_OPENAI_COMPATIBLE_ADAPTER_H