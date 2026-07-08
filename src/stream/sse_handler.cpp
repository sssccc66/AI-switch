#include "sse_handler.h"

// ============================================================
// feed — 处理 libcurl 收到的原始数据, 提取完整 SSE 消息
//
// 处理逻辑:
//   1. 把新数据追加到 buffer_ 末尾
//   2. 在 buffer_ 中查找 \n\n (SSE 消息分隔符)
//   3. 每找到一个完整消息, 去掉 "data: " 前缀后返回
//   4. 未完成的部分留在 buffer_ 中
// ============================================================
std::vector<std::string> sse_handler::feed(std::string_view raw_data) {
    buffer_ += raw_data;
    std::vector<std::string> messages;

    while (true) {
        // 查找消息分隔符 \n\n
        size_t pos = buffer_.find("\n\n");
        if (pos == std::string::npos) {
            break;  // 没有完整消息, 等下一批数据
        }

        // 提取一条完整消息 (不含 \n\n)
        std::string line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 2);  // 从 buffer_ 中移除已处理的部分

        // 去掉 "data: " 前缀
        // SSE 格式: data: {...}\n\n
        const std::string prefix = "data: ";
        if (line.substr(0, prefix.size()) == prefix) {
            line = line.substr(prefix.size());
        }

        // 检查是否是结束标记
        if (line == "[DONE]") {
            done_ = true;
            continue;
        }

        // 跳过空行
        if (line.empty()) {
            continue;
        }

        messages.push_back(line);
    }

    return messages;
}
