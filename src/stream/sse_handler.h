#ifndef AI_SWITCH_SSE_HANDLER_H
#define AI_SWITCH_SSE_HANDLER_H

#include <string>
#include <string_view>
#include <vector>

/**
 * SSE (Server-Sent Events) 处理工具
 * =================================
 *
 * SSE 协议格式:
 *   data: {"choices":[{"delta":{"content":"你"}}]}\n\n
 *
 * 两个 \n 之间是一条消息 (SSE event)
 * 每条消息以 "data: " 开头
 * 流结束时发送 "data: [DONE]\n\n"
 *
 * 这个类只做两件事:
 *   1. 从 libcurl 收到的数据中提取完整的 SSE 消息
 *   2. 构造要发给客户端的 SSE 数据块
 */
class sse_handler {
public:
    /**
     * 将 libcurl 收到的原始数据传入, 提取完整的 SSE 消息
     *
     * libcurl 每次 chunk 可能包含:
     *   - 一条完整消息: data: {...}\n\n
     *   - 多条完整消息: data: {...}\n\ndata: {...}\n\n
     *   - 部分消息:     data: {...} (还没收到末尾的 \n\n)
     *
     * 本方法将部分消息暂存在 buffer_ 中, 拼成完整消息后返回。
     *
     * @param raw_data libcurl write_callback 收到的数据
     * @return 提取出的完整消息列表 (去掉 "data: " 前缀和末尾 \n\n)
     */
    std::vector<std::string> feed(std::string_view raw_data);

    /// 判断是否收到了流结束标志 [DONE]
    bool is_done() const { return done_; }

    /// 构造一条发给客户端的 SSE 数据块
    static std::string make_chunk(const std::string& data) {
        return "data: " + data + "\n\n";
    }

    /// 构造流结束标志
    static std::string make_done_chunk() {
        return "data: [DONE]\n\n";
    }

private:
    std::string buffer_;   // 暂存未拼完整的消息
    bool done_ = false;    // 是否收到了 [DONE]
};

#endif // AI_SWITCH_SSE_HANDLER_H
