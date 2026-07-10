#include "log_middleware.h"
#include <iostream>
#include <chrono>

void log_middleware::process(context& ctx) {
    std::cout << "[log] "
              << ctx.request.method_string() << " "
              << ctx.request.target()
              << " | key_id=" << ctx.api_key_id
              << " | " << (ctx.terminated ? "⚠️ 拦截" : "→ 路由")
              << "\n";
}
