#ifndef AI_SWITCH_ADAPTER_FACTORY_H
#define AI_SWITCH_ADAPTER_FACTORY_H

#include "adapter.h"
#include <memory>        // std::unique_ptr
#include <vector>
#include <string>

/**
 * adapter_factory — AI 适配器工厂
 * ================================
 *
 * 策略模式中的上下文角色：
 *   维护一组可用的适配器，根据请求中的 model 字段选择合适的适配器。
 *
 * 匹配规则:
 *   model 字段以适配器的 model_prefix() 开头即匹配。
 *   例如: model="gpt-3.5-turbo" → 前缀 "gpt" 匹配 → 返回 OpenAIAdapter
 *
 * 用法:
 *   adapter_factory factory;
 *   factory.register_adapter(std::make_unique<openai_adapter>(key));
 *   factory.register_adapter(std::make_unique<deepseek_adapter>(key));
 *
 *   auto ai = factory.create("gpt-3.5");
 *   if (ai) { ai->chat_completion(request); }
 */
class adapter_factory {
public:
    /// 注册一个适配器
    void register_adapter(std::unique_ptr<adapter> ai);

    /**
     * 根据 model 名字查找匹配的适配器
     *
     * @param model_name 请求中的 model 字段，如 "gpt-3.5" 或 "deepseek-chat"
     * @return 匹配的适配器指针，如果没找到返回 nullptr
     *
     * @note 返回的是原始指针，不转移所有权。
     *       适配器的生命周期由 factory 管理。
     */
    adapter* create(const std::string& model_name) const;

    /// 已注册的适配器数量
    size_t count() const { return adapters_.size(); }

private:
    std::vector<std::unique_ptr<adapter>> adapters_;
};

#endif // AI_SWITCH_ADAPTER_FACTORY_H
