#ifndef AI_SWITCH_ADAPTER_H
#define AI_SWITCH_ADAPTER_H

#include <string>
#include <functional>      // std::function
#include <nlohmann/json.hpp>

/**
 * adapter — AI 适配器抽象基类
 * =============================
 *
 * 策略模式: 定义统一的 AI API 调用接口。
 * 不同的 AI 服务商各自实现这个接口，调用方只需面对抽象基类。
 *
 * 用法:
 *   std::unique_ptr<adapter> ai = std::make_unique<openai_adapter>(api_key);
 *   auto result = ai->chat_completion(request_body);
 */
class adapter {
public:
    virtual ~adapter() = default;

    /// 流式回调: 每次收到一个内容块时调用
    /// @param content  本次收到的文本块 (可能是空字符串)
    /// @param done     是否结束
    using stream_callback = std::function<void(std::string content, bool done)>;

    /**
     * 非流式聊天补全
     *
     * @param request 符合 OpenAI API 格式的请求体 JSON
     *                { "model": "gpt-3.5", "messages": [...], "temperature": 0.7 }
     * @return AI 返回的完整响应 JSON
     */
    virtual nlohmann::json chat_completion(const nlohmann::json& request) = 0;

    /**
     * 流式聊天补全
     *
     * 通过回调逐块返回 AI 生成的文本, 不等待完整响应。
     *
     * @param request 请求体 JSON (stream 字段会被设为 true)
     * @param cb      每次收到内容块时调用的回调
     * @throws std::runtime_error 如果 HTTP 请求失败
     */
    virtual void chat_completion_stream(const nlohmann::json& request, stream_callback cb) = 0;

    /**
     * 返回该适配器处理的模型名前缀
     * 工厂模式根据此字段匹配请求中的 model 字段
     *
     * 例如:
     *   OpenAIAdapter  → "gpt"
     *   DeepSeekAdapter → "deepseek"
     */
    virtual std::string model_prefix() const = 0;
};

#endif // AI_SWITCH_ADAPTER_H
