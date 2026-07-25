#ifndef AI_SWITCH_ANTHROPIC_ADAPTER_H
#define AI_SWITCH_ANTHROPIC_ADAPTER_H

#include "adapter.h"
#include <string>

/**
 * Anthropic_adapter — Anthropic Claude 适配器
 * =========================================================
 *
 * 背景:
 *   Anthropic Claude 的 API 与 OpenAI 的 Chat Completions API 有显著差异
 *   所以这个类直接继承 adapter 接口, 独立实现请求/解析逻辑。
 *   主要差异:
 *     
 */
class anthropic_adapter : public adapter {
public:
    /**
     * @param api_key       Anthropic 的 API Key
     * @param base_url      API 基础地址,默认anthropic官方地址
     * @param default_model 请求体未指定 model 字段时使用的默认模型名
     * @param anthropic_version  请求头anthropic-version的值（anthropic官方要求必须指定）
     */
    anthropic_adapter(const std::string& api_key,
                       const std::string& base_url="https://api.anthropic.com/v1",
                       const std::string& default_model="claude-3-5-sonnet-20241022",
                       const std::string& anthropic_version="2023-06-01");


    nlohmann::json chat_completion(const nlohmann::json& request) override;
    void chat_completion_stream(const nlohmann::json& request, stream_callback cb) override;

    std::string model_prefix() const override { return "claude"; }

private:
    /**
     * 把统一的OpenAI风格请求体转换为Anthropic风格请求体
     *   - 提取role="system"的消息，放进独立的“system”字段
     *   - 其余消息原样放进“messages”字段
     *   -  补上必填的“max_tokens”
     *    
     * @param request 上层传入的请求json
     * @return strean  是否流式请求
     */
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
    std::string anthropic_version_;
};

#endif // AI_SWITCH_ANTHROPIC_ADAPTER_H