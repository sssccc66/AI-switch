#ifndef AI_SWITCH_REPOSITORY_H
#define AI_SWITCH_REPOSITORY_H

#include <memory>                        // std::shared_ptr
#include <optional>                      // std::optional
#include <string>
#include <vector>                        // std::vector
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
 * api_key_result — 查询 API Key 的结果
 *
 * 区分"没找到 Key"和"数据库错误"，方便中间件返回正确的状态码
 */
struct api_key_result {
    api_key_info info;         // 查询结果
    bool found = false;        // Key 存在且启用
    bool db_error = false;     // 数据库连接失败
};

/**
 * repository — 数据访问层
 * ========================
 *
 * 封装所有 MySQL 查询操作:
 *   - 查 API Key (鉴权用)
 *   - 建会话 
 *   - 存消息 
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
     * @return 包含查询结果的结构体 (found / db_error / info)
     */
    api_key_result find_api_key(const std::string& key);

    // ---- 管理接口 ----

    /**
     * 创建新的 API Key
     * @param name       Key 用途名
     * @param rate_limit 每分钟限流数
     * @return 创建的 api_key_info
     */
    api_key_info create_api_key(const std::string& name, int rate_limit);

    /// 列出所有 API Key
    std::vector<api_key_info> list_api_keys();

    /**
     * 禁用 API Key (enabled = 0)
     * @param id api_keys 表主键
     * @return true = 禁用成功, false = Key 不存在
     */
    bool disable_api_key(int64_t id);

private:
    std::shared_ptr<connection_pool> pool_;

    /// 生成随机 API Key (sk_ + 32位十六进制)
    static std::string generate_api_key();
};

#endif // AI_SWITCH_REPOSITORY_H
