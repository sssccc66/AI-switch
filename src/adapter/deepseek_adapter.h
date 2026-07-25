#ifndef AI_SWITCH_DEEPSEEK_ADAPTER_H
#define AI_SWITCH_DEEPSEEK_ADAPTER_H

#include "openai_compatible_adapter.h"
#include <string>

/**
 * deepseek_adapter — DeepSeek API 适配器
 * =======================================
 *
 * 调用 DeepSeek 的 /v1/chat/completions 接口。
 * DeepSeek API 与 OpenAI API 格式高度兼容 (请求体、响应结构、
 * 流式 SSE 事件格式都一致), 所以实际的 HTTP 请求组装 / 响应解析
 * 逻辑全部收敛在 openai_compatible_adapter 基类里,
 * 这里只需要提供 DeepSeek 特有的 base_url、默认模型名和路由前缀。
 *
 * DeepSeek 国内可以直接访问，适合开发调试。
 * 端点: https://api.deepseek.com/v1/chat/completions 
 */
class deepseek_adapter : public openai_compatible_adapter {
public:
    /**
     * @param api_key  DeepSeek API Key
     * @param base_url API 基础地址，默认 DeepSeek 官方地址
     */
    explicit deepseek_adapter(const std::string& api_key,
                               const std::string& base_url = "https://api.deepseek.com/v1");
};

#endif // AI_SWITCH_DEEPSEEK_ADAPTER_H