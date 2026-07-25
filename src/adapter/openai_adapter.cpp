#include "openai_adapter.h"

// ============================================================
// 构造函数 — 只需把 OpenAI 的差异化配置传给基类
//
// api_key       : 调用方传入
// base_url      : 默认 OpenAI 官方地址 (可覆盖, 便于自建代理/测试)
// default_model : 请求体未指定 model 时使用 "gpt-3.5-turbo"
// model_prefix  : adapter_factory 用它匹配 "gpt-3.5-turbo"/"gpt-4" 等
// provider_name : 仅用于日志打印
// ============================================================
openai_adapter::openai_adapter(const std::string& api_key,
                                 const std::string& base_url)
    : openai_compatible_adapter(api_key, base_url,
                                 /*default_model=*/"gpt-3.5-turbo",
                                 /*model_prefix=*/"gpt",
                                 /*provider_name=*/"openai")
{
}