-- ============================================================
-- bootstrap.sql — AI-switch 数据库初始化脚本
-- 用法: sudo mysql -u root < bootstrap.sql
-- ============================================================

-- 1. 创建数据库（如果不存在）
--    utf8mb4 支持 emoji 和特殊字符, AI 模型回复里经常有
CREATE DATABASE IF NOT EXISTS ai_switch
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE ai_switch;

-- ============================================================
-- 2. api_keys — API Key 表
--    存储所有允许访问网关的密钥及其配置
-- ============================================================
CREATE TABLE IF NOT EXISTS api_keys (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    api_key     VARCHAR(64)  NOT NULL UNIQUE COMMENT 'API Key 值, 客户端通过 Authorization 头传入',
    name        VARCHAR(128) NOT NULL COMMENT '标识这个 Key 的用途, 如 "测试用" / "生产环境"',
    rate_limit  INT          NOT NULL DEFAULT 60 COMMENT '每分钟最大请求数, 给限流器用的',
    enabled     TINYINT(1)   NOT NULL DEFAULT 1 COMMENT '1=启用, 0=禁用',
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,

    INDEX idx_api_key (api_key)  -- 查 Key 时走索引, 不用全表扫描
) ENGINE=InnoDB;

-- ============================================================
-- 3. sessions — 会话表 (Week 4 会话管理用, 先建好结构)
--    每个对话一个 session, 可以关联多条消息
-- ============================================================
CREATE TABLE IF NOT EXISTS sessions (
    id          VARCHAR(32)  PRIMARY KEY COMMENT '形如 ses_xxxxx 的会话 ID',
    api_key_id  BIGINT       NOT NULL COMMENT '哪个 Key 创建的会话',
    model       VARCHAR(64)  NOT NULL COMMENT '本次会话使用的模型',
    status      TINYINT      NOT NULL DEFAULT 1 COMMENT '1=活跃, 0=已关闭',
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    FOREIGN KEY (api_key_id) REFERENCES api_keys(id)
) ENGINE=InnoDB;

-- ============================================================
-- 4. messages — 消息表 (Week 4 会话管理用)
--    一条请求或回复
-- ============================================================
CREATE TABLE IF NOT EXISTS messages (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    session_id  VARCHAR(32)  NOT NULL,
    role        VARCHAR(16)  NOT NULL COMMENT 'user/assistant/system',
    content     TEXT         NOT NULL COMMENT '消息内容, AI 回复可能很长',
    tokens      INT          NOT NULL DEFAULT 0 COMMENT '这条消息的 token 数',
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (session_id) REFERENCES sessions(id),
    INDEX idx_session_time (session_id, created_at)  -- 按会话查历史时加速
) ENGINE=InnoDB;

-- ============================================================
-- 5. 插入测试用的 API Key
--    等 Week 2 鉴权写完后, 用这个 Key 测试
-- ============================================================
INSERT INTO api_keys (api_key, name, rate_limit, enabled) VALUES
    ('sk-test-key-001', '测试 Key - 60 RPM',  60,  1),
    ('sk-test-key-002', '测试 Key - 120 RPM', 120, 1),
    ('sk-test-key-003', '已禁用 Key',         60,  0)  -- 用来测试鉴权拦截
ON DUPLICATE KEY UPDATE name = VALUES(name);
-- ON DUPLICATE KEY UPDATE: 重复跑脚本不会报错, 只会更新名字
-- 实际部署后，测试数据应该删除，用新用户的 API Key 替换