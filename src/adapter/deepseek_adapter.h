#ifndef AI_SWITCH_DEEPSEEK_ADAPTER_H
#define AI_SWITCH_DEEPSEEK_ADAPTER_H

#include "adapter.h"
#include <string>

/**
 * deepseek_adapter — DeepSeek API 适配器
 * =======================================
 *
 * 调用 DeepSeek 的 /v1/chat/completions 接口。
 * DeepSeek API 与 OpenAI API 格式高度兼容，
 * 所以实现逻辑几乎一样，只是端点地址和 API Key 不同。
 *
 * DeepSeek 国内可以直接访问，适合开发调试。
 * 端点: https://api.deepseek.com/v1/chat/completions
 */
class deepseek_adapter : public adapter {
public:
    /**
     * @param api_key  DeepSeek API Key
     * @param base_url API 基础地址，默认 DeepSeek 官方地址
     */
    deepseek_adapter(const std::string& api_key,
                     const std::string& base_url = "https://api.deepseek.com/v1");

    nlohmann::json chat_completion(const nlohmann::json& request) override;
    void chat_completion_stream(const nlohmann::json& request, stream_callback cb) override;

    std::string model_prefix() const override { return "deepseek"; }

private:
    /**
     * 用 libcurl 发送 HTTP POST 请求
     *
     * @param url    完整请求 URL
     * @param body   JSON 请求体字符串
     * @return       HTTP 响应体字符串
     * @throws std::runtime_error 如果请求失败或返回状态码非 200
     */
    std::string http_post(const std::string& url, const std::string& body) const;

    std::string api_key_;
    std::string base_url_;
};

#endif // AI_SWITCH_DEEPSEEK_ADAPTER_H
