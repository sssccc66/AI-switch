#include "adapter_factory.h"
#include <iostream>   // 日志

// ============================================================
// register_adapter — 注册一个适配器
// ============================================================
void adapter_factory::register_adapter(std::unique_ptr<adapter> ai) {
    std::cout << "[adapter_factory] 注册适配器: "
              << ai->model_prefix() << "\n";
    adapters_.push_back(std::move(ai));
}

// ============================================================
// create — 根据 model 名字查找匹配的适配器
//
// 匹配规则:
//   遍历所有注册的适配器，检查 model 名字是否以适配器的前缀开头。
//
// 例如:
//   model = "gpt-3.5-turbo" → 前缀 "gpt" 匹配 ✅
//   model = "deepseek-chat" → 前缀 "deepseek" 匹配 ✅
//   model = "claude-3"      → 没有注册 "claude" → nullptr
//
// 为什么不精确匹配?
//   模型名字版本很多 (gpt-3.5-turbo, gpt-4, gpt-4-turbo...)
//   用前缀匹配可以覆盖同一系列的所有版本，不需要逐个注册。
// ============================================================
adapter* adapter_factory::create(const std::string& model_name) const {
    for (const auto& ai : adapters_) {
        // 检查 model 名字是否以当前适配器的前缀开头
        // 例如 "deepseek-chat".find("deepseek") == 0 → 匹配
        if (model_name.find(ai->model_prefix()) == 0) {
            return ai.get();
        }
    }

    // 没找到任何匹配的适配器
    std::cerr << "[adapter_factory] 未找到模型 " << model_name
              << " 对应的适配器\n";
    return nullptr;
}
