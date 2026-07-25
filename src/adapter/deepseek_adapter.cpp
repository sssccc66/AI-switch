#include "deepseek_adapter.h"

// ============================================================
// 构造函数 — 只需把 DeepSeek 的差异化配置传给基类
//
// api_key       : 调用方传入
// base_url      : 默认 DeepSeek 官方地址 (可覆盖, 便于自建代理/测试)
// default_model : 请求体未指定 model 时使用 "deepseek-chat"
// model_prefix  : adapter_factory 用它匹配 "deepseek-chat"/"deepseek-reasoner" 等
// provider_name : 仅用于日志打印
// ============================================================
deepseek_adapter::deepseek_adapter(const std::string& api_key,
                                   const std::string& base_url)
    : openai_compatible_adapter(api_key, base_url,
                                 /*default_model=*/"deepseek-v4-flash",
                                 /*model_prefix=*/"deepseek",
                                 /*provider_name=*/"deepseek")
{
}