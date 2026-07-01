#ifndef AI_SWITCH_REPOSITORY_H
#define AI_SWITCH_REPOSITORY_H

#include <memory>                        // std::shared_ptr
#include <optional>                      // std::optional — 查询可能无结果
#include <string>
#include <cstdint>

// 前置声明: 在 .h 里只声明指针, 不包含 full header, 编译更快
class connection_pool;

/**
 * api_key_info — 从数据库查到的一条 API Key 信息
 *
 * 对应 bootstrap.sql 中的 api_keys 表
 */
struct api_key_info {
    int64_t  id;              // 数据库主键
    std::string api_key;      // Key 值
    std::string name;         // 用途名称
    int rate_limit;           // 每分钟限制请求数
    bool enabled;             // 是否启用
};

/**
 * repository — 数据访问层
 * ========================
 *
 * 封装所有 MySQL 查询操作:
 *   - 查 API Key (鉴权用)
 *   - 建会话 (Week 4)
 *   - 存消息 (Week 4)
 *
 * 内部使用 connection_pool 获取连接。
 * 查询完成后自动归还连接。
 */
class repository {
public:
    /**
     * 构造函数
     * @param pool 连接池 (shared_ptr, 和 http_server 共享)
     */
    explicit repository(std::shared_ptr<connection_pool> pool);

    /**
     * 根据 API Key 值查询 Key 信息
     *
     * @param key 客户端传过来的 API Key 字符串
     * @return 如果找到且 enabled = true, 返回 key 信息
     *         如果没找到或被禁用, 返回 std::nullopt
     */
    std::optional<api_key_info> find_api_key(const std::string& key);

private:
    std::shared_ptr<connection_pool> pool_;
};

#endif // AI_SWITCH_REPOSITORY_H
