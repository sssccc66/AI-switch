# AI switch — C++ 高性能 AI 代理网关

> **定位**：高性能异步 AI API 代理服务，统一管理多模型请求，做转发、限流、鉴权、统计。
> **技术栈**：C++17 + Asio + Beast + MySQL + nlohmann/json
> **代码量**：~2,500 行（全手写，无框架黑盒）
> **开发状态**：✅ Week 1~2 完成 — Week 3 进行中

---

## 目录

1. [架构总览](#一架构总览)
2. [模块设计](#二模块设计)
3. [API 设计](#三-api-设计)
4. [数据库设计](#四数据库设计)
5. [关键设计决策](#五关键设计决策)
6. [开发路径（6 周）](#六开发路径6-周)
7. [部署与配置](#七部署与配置)
8. [学习资源推荐](#八学习资源推荐)
9. [面试预演](#九面试预演)

---

## 一、架构总览

```
 ┌─────────────────────────────────────────────────────────┐
 │                   客户端 (Client)                         │
 │              curl / Postman / 你的前端                     │
 └──────────────────────┬──────────────────────────────────┘
                        │ HTTP/SSE
                        ▼
 ┌─────────────────────────────────────────────────────────┐
 │              HTTP Server (Beast + Asio)                  │
 │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────┐ │
 │  │ 路由分发  │ │ 请求解析  │ │ 响应封装  │ │ SSE 推送   │ │
 │  └──────────┘ └──────────┘ └──────────┘ └────────────┘ │
 └──────────────────────┬──────────────────────────────────┘
                        ▼
 ┌─────────────────────────────────────────────────────────┐
 │                   中间件链 (Middleware Pipeline)          │
 │                                                        │
 │   ┌──────────┐   ┌──────────┐   ┌──────────┐          │
 │   │ 鉴权中间件 │──→│ 限流中间件 │──→│ 日志中间件 │──→ ... │
 │   │API Key   │   │令牌桶    │   │耗时+计数│          │
 │   └──────────┘   └──────────┘   └──────────┘          │
 └──────────────────────┬──────────────────────────────────┘
                        ▼
 ┌─────────────────────────────────────────────────────────┐
 │                  AI 适配器层 (Adapters)                   │
 │  ┌──────────────────────────────────────────────────┐   │
 │  │            Adapter 接口 (抽象基类)                 │   │
 │  └──────┬──────────────────┬───────────────────────┘   │
 │         ▼                  ▼                           │
 │  ┌──────────────┐  ┌──────────────┐                    │
 │  │ OpenAIAdapter│  │ DeepSeekAdapter│                   │
 │  │ /v1/chat     │  │ /v1/chat     │                    │
 │  │ 流式+非流式  │  │ 流式+非流式  │                    │
 │  └──────────────┘  └──────────────┘                    │
 └──────────────────────┬──────────────────────────────────┘
                        │ libcurl HTTP 调用
                        ▼
 ┌─────────────────────────────────────────────────────────┐
 │                 AI 模型 (外部 API)                        │
 │    OpenAI (api.openai.com)  /  DeepSeek (api.deepseek.com)│
 └─────────────────────────────────────────────────────────┘

 ┌─────────────────────────────────────────────────────────┐
 │                   数据持久化层                            │
 │  ┌────────────────┐  ┌────────────────┐                 │
 │  │ MySQL          │  │ 本地文件/内存   │                 │
 │  │ api_keys 表    │  │ 日志文件        │                 │
 │  │ sessions 表    │  │ 限流计数器      │                 │
 │  │ messages 表    │  │ 配置热加载      │                 │
 │  └────────────────┘  └────────────────┘                 │
 └─────────────────────────────────────────────────────────┘
```

### 数据流（一次完整请求）

```
1. Client ──HTTP POST──→ HTTP Server (Beast)
2. HTTP Server ──解析→ 路由分发到 /api/chat
3. 鉴权中间件：检查 Authorization header 中的 API Key
4. 限流中间件：令牌桶 tryAcquire()
5. 日志中间件：记录请求开始时间
6. AI 适配器：组装请求体，调用 libcurl → 外部 AI API
7. 流式处理：逐 chunk 接收 SSE 并转发给客户端
8. 日志中间件：记录耗时、token 用量、写入日志
9. HTTP Server ──响应──→ Client
```

---

## 二、模块设计

### 文件结构

```
AI-switch/
├── CMakeLists.txt
├── config.json                   # 运行时配置（模型端点、限流参数、密钥）
├── bootstrap.sql                 # 数据库初始化脚本
├── AI switch — 高性能 AI 代理网关.md
│
├── src/
│   ├── main.cpp                  # 启动入口：解析参数→加载配置→初始化DB→启动 server
│   │
│   ├── server/
│   │   ├── http_server.h/.cpp    # Beast HTTP 服务器封装
│   │   ├── router.h/.cpp         # URL 路由分发（支持路径匹配）
│   │   └── session.h/.cpp        # 每个连接一个 session，管理异步读写
│   │
│   ├── middleware/
│   │   ├── middleware.h           # 中间件接口（抽象基类）+ context + chain
│   │   ├── auth_middleware.h/.cpp # API Key 鉴权（查 MySQL）
│   │   ├── rate_limiter.h/.cpp    # 令牌桶限流（核心手写）
│   │   └── log_middleware.h/.cpp  # 请求日志 + 耗时统计
│   │
│   ├── adapter/
│   │   ├── adapter.h              # AI 适配器抽象接口
│   │   ├── openai_adapter.h/.cpp  # OpenAI API 实现
│   │   ├── deepseek_adapter.h/.cpp# DeepSeek API 实现
│   │   └── adapter_factory.h/.cpp # 工厂模式创建适配器
│   │
│   ├── stream/
│   │   └── sse_handler.h/.cpp    # SSE 流式转发
│   │
│   ├── model/
│   │   ├── request.h/.cpp        # 统一请求模型
│   │   └── response.h/.cpp       # 统一响应模型
│   │
│   ├── db/
│   │   ├── connection_pool.h/.cpp # MySQL 连接池（超时保护，空池阻止启动）
│   │   └── repository.h/.cpp     # 数据访问（查 Key、存会话等）
│   │
│   └── util/
│       ├── config.h/.cpp          # JSON 配置读取（支持 --config 参数 + 环境变量覆盖）
│       ├── logger.h/.cpp          # 文件日志（同步写 + 缓冲区）
│       └── token_counter.h/.cpp   # Token 估算/计数
│
└── tests/                         # 单元测试
    ├── test_rate_limiter.cpp
    ├── test_router.cpp
    └── test_adapter.cpp
```

### 各模块职责

#### 1. `server/` — HTTP 服务核心

| 类 | 职责 | 关键设计 |
|------|------|---------|
| **http_server** | 启动 Asio io_context，绑定端口，接受连接 | 每个新连接创建一个 `session` 对象；signal_set 优雅退出 |
| **session** | 管理单个 HTTP 连接的生命周期 | 异步读写；先走中间件链 → 再路由分发 |
| **router** | 根据 HTTP 方法和路径分发到处理函数 | 哈希表 O(1) 查找，支持路径参数 |

**C++ 特色设计**：

```cpp
// session 的核心异步循环 + 中间件链
void session::handle_request() {
    context ctx(req_);           // 创建请求上下文
    mw_chain_->process(ctx);     // 走中间件链 (鉴权→限流→...)

    if (ctx.terminated) {
        res_ = std::move(ctx.response.value());  // 中间件拦截
    } else {
        res_ = router_->dispatch(req_);          // 路由分发
    }
    do_write();
}
```

#### 2. `middleware/` — 中间件链

中间件链是责任链模式：

```cpp
class middleware {
public:
    virtual ~middleware() = default;
    virtual void process(context& ctx) = 0;
};

class middleware_chain {
    std::vector<std::unique_ptr<middleware>> middlewares_;
public:
    void process(context& ctx) {
        for (auto& m : middlewares_) {
            m->process(ctx);
            if (ctx.terminated) return;  // 请求被拦截
        }
    }
};
```

#### 3. `middleware/rate_limiter.h` — 令牌桶（面试核心）

这是你最需要手写的类：

```cpp
class token_bucket {
    std::atomic<int64_t> tokens_;     // 当前令牌数
    int64_t capacity_;                 // 桶容量
    int64_t refill_rate_;             // 每秒补充速率
    std::chrono::steady_clock::time_point last_refill_;
    std::mutex refill_mutex_;         // 仅补充时需要

public:
    token_bucket(int64_t capacity, int64_t refill_per_second);

    // 核心方法：尝试消费一个令牌
    bool try_acquire(int64_t tokens = 1);

    void update_config(int64_t capacity, int64_t refill_per_second);
};
```

**try_acquire 实现要点**：

```cpp
bool token_bucket::try_acquire(int64_t tokens) {
    // Step 1: 按时间补充令牌（懒更新）
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_refill_).count();
    
    if (elapsed > 0) {
        std::lock_guard<std::mutex> lock(refill_mutex_);
        elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_refill_).count();
        if (elapsed > 0) {
            tokens_.store(std::min(capacity_,
                tokens_.load(std::memory_order_relaxed) + elapsed * refill_rate_));
            last_refill_ = std::chrono::steady_clock::now();
        }
    }

    // Step 2: 尝试消费（原子操作，无锁）
    int64_t expected = tokens_.load(std::memory_order_relaxed);
    while (expected >= tokens) {
        if (tokens_.compare_exchange_weak(expected, expected - tokens,
                std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false;
}
```

面试官看到这个实现会追问：
- **为什么补充要加锁而消费不加锁？** → 补充涉及除法大概率多线程同时触发浪费 CPU，消费是高频操作需要低延迟
- **CAS 的 ABA 问题？** → 这里不会出现，因为我们只关注数值
- **为什么不用 std::atomic::fetch_sub？** → 需要检查是否够减，CAS 更合适

#### 4. `adapter/` — AI 适配器（策略模式）

```cpp
class adapter {
public:
    virtual ~adapter() = default;
    virtual nlohmann::json chat_completion(const nlohmann::json& request) = 0;
    virtual void chat_completion_stream(
        const nlohmann::json& request,
        std::function<void(std::string_view chunk)> chunk_callback) = 0;
    virtual std::string model_name() const = 0;
    virtual size_t estimate_tokens(const nlohmann::json& request) = 0;
};
```

#### 5. `adapter/openai_adapter.cpp` — 调用外部 API

```cpp
nlohmann::json openai_adapter::chat_completion(const nlohmann::json& request) {
    nlohmann::json body = {
        {"model", request.value("model", "gpt-3.5-turbo")},
        {"messages", request["messages"]},
        {"temperature", request.value("temperature", 0.7)},
        {"stream", false}
    };
    std::string response = http_post(
        "https://api.openai.com/v1/chat/completions", api_key_, body.dump());
    return nlohmann::json::parse(response);
}
```

#### 6. `stream/sse_handler.h` — SSE 流式转发

```cpp
void sse_handler::send_chunk(beast::tcp_stream& stream, std::string_view data) {
    std::string chunk = fmt::format("data: {}\n\n", data);
    http::response<http::string_body> res;
    http::async_write(stream, res, ...);
}
```

#### 7. `util/token_counter.h` — Token 计数

```cpp
size_t estimate_tokens(std::string_view text) {
    size_t tokens = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        if (c < 0x80)      { tokens++; i++; }
        else if (c < 0xE0) { tokens += 2; i += 2; }
        else                { tokens += 3; i += 3; }
    }
    return tokens;
}
```

---

## 三、API 设计

### 3.1 聊天接口

```
POST /api/chat
Authorization: Bearer <api_key>
Content-Type: application/json

Request:
{
    "model": "gpt-3.5-turbo",
    "messages": [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "Hello!"}
    ],
    "stream": true,
    "temperature": 0.7,
    "max_tokens": 2048
}

Response (非流式，stream=false):
{
    "id": "chat_xxxxx",
    "model": "gpt-3.5-turbo",
    "choices": [{
        "index": 0,
        "message": {"role": "assistant", "content": "Hello! How can I help you?"},
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 25, "completion_tokens": 8, "total_tokens": 33}
}

Response (流式，stream=true):
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive

data: {"choices":[{"delta":{"role":"assistant"},"index":0}]}
data: {"choices":[{"delta":{"content":"Hello"},"index":0}]}
data: [DONE]
```

### 3.2 会话管理

```
POST /api/session
Authorization: Bearer <api_key>

Response:
{"session_id": "ses_xxxxx", "created_at": "..."}

GET /api/session/{session_id}/history
Authorization: Bearer <api_key>

Response:
{"session_id": "ses_xxxxx", "messages": [...]}
```

### 3.3 管理接口（Week 6 新增）

```
POST /admin/api-keys
Authorization: Bearer <master_admin_key>

Body:
{
    "name": "用户小张",
    "rate_limit": 60
}

Response:
{"api_key": "sk_xxxxxxxx", "name": "用户小张", "rate_limit": 60}

---

GET /admin/api-keys
Authorization: Bearer <master_admin_key>

Response:
{"api_keys": [{"id": 1, "name": "...", ...}]}
```

### 3.4 系统接口

```
GET /api/health
Response: {"status": "ok", "version": "1.0.0", "service": "AI-switch"}

GET /api/metrics
Response:
{
    "total_requests": 1523,
    "rate_limited": 12,
    "avg_latency_ms": 843,
    "by_model": {"gpt-3.5-turbo": 1200, "deepseek-chat": 323}
}
```

---

## 四、数据库设计

### 4.1 MySQL 表

```sql
-- API Key 表
CREATE TABLE api_keys (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    api_key     VARCHAR(64)  NOT NULL UNIQUE,
    name        VARCHAR(128) NOT NULL COMMENT '标识这个 key 的用途',
    rate_limit  INT          NOT NULL DEFAULT 60 COMMENT '每分钟限制请求数',
    enabled     TINYINT(1)   NOT NULL DEFAULT 1,
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- 会话表
CREATE TABLE sessions (
    id          VARCHAR(32)  PRIMARY KEY,
    api_key_id  BIGINT       NOT NULL,
    model       VARCHAR(64)  NOT NULL,
    status      TINYINT      NOT NULL DEFAULT 1,
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (api_key_id) REFERENCES api_keys(id)
) ENGINE=InnoDB;

-- 消息表
CREATE TABLE messages (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    session_id  VARCHAR(32)  NOT NULL,
    role        VARCHAR(16)  NOT NULL,
    content     TEXT         NOT NULL,
    tokens      INT          NOT NULL DEFAULT 0,
    created_at  TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (session_id) REFERENCES sessions(id),
    INDEX idx_session_time (session_id, created_at)
) ENGINE=InnoDB;

-- 请求日志表
CREATE TABLE request_logs (
    id                BIGINT AUTO_INCREMENT PRIMARY KEY,
    api_key_id        BIGINT       NOT NULL,
    model             VARCHAR(64)  NOT NULL,
    prompt_tokens     INT          NOT NULL DEFAULT 0,
    completion_tokens INT          NOT NULL DEFAULT 0,
    latency_ms        INT          NOT NULL,
    status_code       INT          NOT NULL,
    created_at        TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (api_key_id) REFERENCES api_keys(id),
    INDEX idx_created (created_at)
) ENGINE=InnoDB;
```

### 4.2 为什么不把限流状态放 Redis？

初期版本限流计数器直接放**内存**：
- 对于单机部署，内存就够了
- 避免了 Redis 依赖，部署简单
- 如果以后要水平扩展，再加 Redis + Lua 脚本做分布式限流

面试被问到："你现在的限流是单机的，如果要做分布式的怎么改？"
→ 这就是一个很好的扩展性问题，你答出来就是加分项。

---

## 五、关键设计决策

### 5.1 为什么用 Beast + Asio 而不是 libmicrohttpd/httplib？

| 对比 | Beast + Asio | cpp-httplib | libmicrohttpd |
|------|-------------|------------|---------------|
| 异步 IO | ✅ 原生异步 | ❌ 同步阻塞 | ⚠️ 事件驱动但 API 繁琐 |
| 流式 SSE | ✅ 完全掌控 | ❌ 不支持 | ⚠️ 可以实现但复杂 |
| 学习价值 | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ |
| 面试含金量 | 高（Asio 是 C++ 网络标配）| 低 | 中 |

### 5.2 为什么手写令牌桶而不是用现成库？

因为面试官会问算法细节。手写的每一行都是你面试时的素材：
> "我的限流器用原子变量 CAS 实现消费，mutex 只做补充同步，避免高频消费的锁竞争…"

### 5.3 为什么中间件用责任链模式？

方便扩展：以后加请求修改、加缓存、加 CORS，都只需要新增一个 middleware 子类，插入链中。

### 5.4 为什么分流式和非流式两套接口？

AI 模型的流式响应是 SSE 逐 chunk 返回的，网关需要做到：
1. 从 libcurl 收到 chunk → 2. 解析 SSE data → 3. 转发给客户端
这个过程中连接要保持打开，不能等完整响应再处理。

### 5.5 连接池为什么要有超时保护？

初期版本 borrow() 使用 cv_.wait() 无限等待，如果 MySQL 挂了，所有线程永久卡死。
改进后：5 秒超时 + 返回 nullptr，调用方自行处理；空连接池直接阻止服务启动。

### 5.6 配置文件密码怎么管理？

采用三层覆盖策略（优先级从高到低）：
1. 环境变量 `DB_PASSWORD` — 生产部署用
2. `--config <path>` 指定的配置文件 — 多环境切换
3. 默认 `config.json` — 日常开发

### 5.7 错误处理策略

```cpp
// 统一错误体格式
{
    "error": {
        "code": "rate_limited",      // 机器可读
        "message": "请求过于频繁，请稍后重试",  // 人类可读
        "retry_after": 5             // 可选，建议等待秒数
    }
}

// 常见错误码
// auth_failed       → API Key 无效
// rate_limited      → 被限流
// model_unavailable → 模型暂时不可用
// provider_error    → AI 提供方返回错误
// internal_error    → 网关内部错误
```

---

## 六、开发路径（6 周）

### 总路线图

```
Week 1 ──→ Week 2 ──→ Week 3 ──→ Week 4 ──→ Week 5 ──→ Week 6
 搭 HTTP     鉴权+数据库   限流核心    AI 适配器    流式响应     收尾+优化
 服务         集成                       + 测试       + 管理API
```

---

### Week 1：HTTP 服务搭起来 ✅

**目标**：能在浏览器访问 `http://localhost:8080/health` 返回 JSON

| 天 | 任务 | 产出 | 状态 |
|----|------|------|------|
| Day 1 | 初始化 CMake 项目，安装依赖 | CMake 编译通过 | ✅ |
| Day 2 | 实现 `http_server` | 服务能启动 | ✅ |
| Day 3 | 实现 `session` | 收到请求打印日志 | ✅ |
| Day 4 | 实现 `router` + `GET /health` | curl 返回 JSON | ✅ |
| Day 5 | 请求体和响应体 JSON 序列化 | curl POST 收发 JSON | ✅ |
| Day 6 | 配置文件 `config.json` + `config.h` | 端口等从配置读取 | ✅ |
| Day 7 | **本周验收** | 三项 curl 测试通过 | 🎉 |

**产出**：一个能返回 JSON 的 HTTP 服务，读 `config.json` 启动。

---

### Week 2：鉴权 + 数据库集成 ✅

**目标**：用 API Key 鉴权，存数据到 MySQL

| 天 | 任务 | 产出 | 状态 |
|----|------|------|------|
| Day 1 | MySQL 建表（bootstrap.sql） | 3 张表 + 测试数据 | ✅ |
| Day 2 | 实现 `connection_pool` | 连接池复用连接 | ✅ |
| Day 3 | 实现 `repository` | 数据层查 Key 通过 | ✅ |
| Day 4 | 实现中间件基类 `middleware.h` + `auth_middleware` | 鉴权拦截工作 | ✅ |
| Day 5 | 实现配置优化（--config + DB_PASSWORD） | 可落地部署 | ✅ |
| Day 6 | 连接池保护（超时 + 空池阻止启动） | 不卡死 | ✅ |
| Day 7 | **本周验收** | 正确 Key 通过，错误 Key 返回 401 | 🎉 |

**关键修复**：
- 连接池空时 `borrow()` 无限等待 → 改为 5 秒超时
- 连接池 0 个连接时仍启动服务 → 改为抛异常阻止启动
- 配置文件密码硬编码 → 支持环境变量 `DB_PASSWORD` 覆盖
- 配置文件路径硬编码 → 支持 `--config <path>` 参数

**产出**：带 API Key 鉴权的 HTTP 服务，连接 MySQL 数据库。

---

### Week 3：手写限流核心（最关键的一周）

**目标**：实现令牌桶限流，简历上的"硬通货"

| 天 | 任务 | 产出 |
|----|------|------|
| Day 1 | 实现 `token_bucket` 类骨架 | 类定义完成 |
| Day 2 | 实现 `try_acquire` — CAS 原子操作消费 | 单线程测试通过 |
| Day 3 | 实现 `refill` 逻辑 — 懒更新 + mutex | 多线程测试通过 |
| Day 4 | 实现 `rate_limiter_middleware` | 限流拦截工作 |
| Day 5 | 写单元测试 `test_rate_limiter.cpp` | 100% 覆盖核心路径 |
| Day 6 | 支持按 API Key 区分限流配额 | 不同 Key 不同限额 |
| Day 7 | **本周验收** | 连续请求超过限制被拒绝 |

---

### Week 4：AI 适配器

**目标**：对接真实 AI 模型，能发送请求并拿到响应

| 天 | 任务 | 产出 |
|----|------|------|
| Day 1 | 实现 `adapter` 抽象基类 + `adapter_factory` | 适配器框架 |
| Day 2 | 实现 `openai_adapter` — 非流式调用 | 能调通 OpenAI API |
| Day 3 | 实现 `deepseek_adapter` | 能调通 DeepSeek API |
| Day 4 | 实现路由逻辑 — 根据请求选择适配器 | 不同模型走不同适配器 |
| Day 5 | 实现 `log_middleware` — 记录耗时、token 数 | 日志文件见记录 |
| Day 6 | 会话管理集成 — 自动存消息到数据库 | 对话历史可查 |
| Day 7 | **本周验收**：POST /api/chat 发消息，AI 回复写入数据库 | 🎉 |

---

### Week 5：流式响应 + 完整功能

**目标**：实现 SSE 流式转发，功能完整

| 天 | 任务 | 产出 |
|----|------|------|
| Day 1 | 理解 SSE 协议格式，实现 `sse_handler` 基础 | 手动构造 SSE 输出测试通过 |
| Day 2 | `openai_adapter` 流式调用 | 流式能走通 |
| Day 3 | 流式数据逐 chunk 转发 | 用户端能看到逐字输出 |
| Day 4 | 实现 `token_counter` + 流式 token 统计 | 日志中有 token 计数 |
| Day 5 | 实现 `/api/metrics` 统计接口 | 能查运行统计 |
| Day 6 | 实现配置热加载 | 改配置不重启服务 |
| Day 7 | **本周验收**：curl -N 看到逐字回复 | 🎉 |

---

### Week 6：收尾 + 管理 API + 面试准备

**目标**：完善、测试、管理接口、写文档、模拟面试

| 天 | 任务 | 产出 |
|----|------|------|
| Day 1 | 实现管理 API `/admin/api-keys`：创建/列出 API Key | 动态管理密钥 |
| Day 2 | 错误处理全面 review + 完善错误体格式 | 错误路径都覆盖 |
| Day 3 | 写单元测试 + 压力测试 | 测试覆盖率 > 70% |
| Day 4 | 写 README.md + 项目文档 | 简历可链接 |
| Day 5 | 写项目点评文档（亮点、难点、技术选型理由） | 面试话术 |
| Day 6 | 编译 Release 版本 + 最终验收 | 🚀 |
| Day 7 | **产出最终项目** | 🎉 |

---

## 七、部署与配置

### 7.1 命令行参数

```bash
# 开发模式（使用默认 config.json）
./ai-switch

# 指定配置文件
./ai-switch --config /etc/ai-switch/prod.json

# 帮助
./ai-switch --help
```

### 7.2 环境变量

```bash
# 数据库密码（优先级高于 config.json）
DB_PASSWORD=my_secret ./ai-switch

# 组合使用
DB_PASSWORD=my_secret ./ai-switch --config /etc/ai-switch/prod.json
```

### 7.3 配置优先级

```
环境变量 DB_PASSWORD  >  指定配置文件  >  默认 config.json
   （最高）                （中间）            （最低）
```

### 7.4 开发环境搭建

```bash
# WSL / Ubuntu
sudo apt install cmake g++ libboost-all-dev libcurl4-openssl-dev
sudo apt install libmysqlclient-dev nlohmann-json3-dev

# 初始化数据库
sudo mysql -u root < bootstrap.sql

# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# 运行
DB_PASSWORD=ai_switch_pass ./ai-switch
```

### 7.5 CMakeLists.txt 骨架

```cmake
cmake_minimum_required(VERSION 3.16)
project(ai-switch VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(MySQL REQUIRED)

add_executable(ai-switch
    src/main.cpp
    src/server/http_server.cpp
    src/server/session.cpp
    src/server/router.cpp
    src/middleware/auth_middleware.cpp
    src/middleware/rate_limiter.cpp
    src/middleware/log_middleware.cpp
    src/adapter/openai_adapter.cpp
    src/adapter/deepseek_adapter.cpp
    src/adapter/adapter_factory.cpp
    src/stream/sse_handler.cpp
    src/db/connection_pool.cpp
    src/db/repository.cpp
    src/util/config.cpp
    src/util/logger.cpp
    src/util/token_counter.cpp
)

target_link_libraries(ai-switch
    pthread
    Boost::system
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    ${MySQL_LIBRARIES}
)
```

---

## 八、学习资源推荐

（此部分保留原文档内容）

---

## 九、面试预演

### 项目介绍话术（2 分钟版）

> "我做的项目叫 AI-switch，是一个高性能 C++ AI API 代理网关。
> 核心功能是统一管理多个 AI 模型的请求——做请求转发、鉴权、限流和统计。
>
> 技术选型上，我用了 C++17 和 Asio/Beast 做异步 HTTP 服务，
> 手写了令牌桶限流算法（用原子变量 CAS 实现无锁消费），
> 通过策略模式统一 OpenAI 和 DeepSeek 的 API 差异，
> 并用责任链模式实现了可扩展的中间件管线。
>
> 项目最核心的亮点是限流器的手写实现和基于 Asio 异步模型的高性能网络层。"

### 高频面试题

http重要的设计模式：	异步事件循环 + 多线程共享 io_context + 异步递归 accept。
如何做到高并发？	Asio 的异步 I/O + 多线程事件循环，单机可支撑数千连接。
http_server多路复用的逻辑（epoll）
负载均衡、事件队列、io_context等

**Q：令牌桶和漏桶、固定窗口的区别？**
> 令牌桶关注的是"能否通过"——有令牌就放行，允许一定程度的突发流量。
> 漏桶关注的是"流出速度"——强制匀速输出，不管请求怎么来。
> 我的项目用了令牌桶，因为 AI API 的调用允许短时间突发，更适合我们的场景。

为什么消费不用锁而补充要用？

**Q：你的限流器是单机的，怎么扩展成分布式的？**
> 把令牌状态从内存搬到 Redis，用 Lua 脚本保证原子性。
> 每个请求到 Redis 执行一个 Lua 脚本：补充令牌 → 尝试消费 → 返回结果。
> 不足是网络开销增加一次，但换来了水平扩展能力。

**Q：为什么用 Asio 而不是线程池 + 阻塞 IO？**
> 阻塞 IO 每个连接需要一个线程，C10K 问题。Asio 用 io_context 事件循环驱动，
> 少量线程就能处理大量连接。而且 AI 流式响应是长连接场景，
> 异步模型让一个线程可以同时管理上百个流式连接。

**Q：你现在的代码有哪些可以优化的地方？**
> 三个方向：1) 零拷贝 — 请求体解析时避免字符串复制；
> 2) 内存池 — 预分配 session 对象、HTTP buffer 复用；
> 3) 连接池 — MySQL 连接池目前实现比较简单，可以用更高效的算法。
> 这些都留给后续优化。

**Q：你的连接池如果 MySQL 挂了会怎么样？**
> 构造函数中如果无法创建任何连接，直接抛异常阻止服务启动，避免"运行但不可用"的状况。
> borrow() 有 5 秒超时保护，MySQL 运行中宕机时请求线程不会永久卡死。
> 调用方检测到 nullptr 后返回 503 状态码，配合监控告警。

**Q：数据库密码怎么管理的？**
> 采用三层覆盖策略。config.json 里可以写默认密码用于开发，
> 生产部署时用 `DB_PASSWORD` 环境变量覆盖，不把敏感信息写进文件。
> 配置文件路径也支持 `--config` 参数指定，方便多环境切换。

---

## 附录：开发里程碑

| 周次 | 状态 | 关键产出 | 累计代码量 |
|------|------|---------|-----------|
| Week 1 | ✅ 完成 | HTTP 服务 + 路由 + JSON 收发 | ~400 行 |
| Week 2 | ✅ 完成 | MySQL 连接池 + 鉴权 + 配置优化 | ~800 行 |
| Week 3 | 📝 进行中 | 令牌桶限流器 + 单元测试 | ~1200 行 |
| Week 4 | ⏳ 待开始 | AI 适配器 (OpenAI + DeepSeek) | ~1600 行 |
| Week 5 | ⏳ 待开始 | SSE 流式 + token 计数 + 统计 | ~2000 行 |
| Week 6 | ⏳ 待开始 | 管理 API + 测试 + 文档 + 面试准备 | ~2500 行 |

---

> 文档版本：v1.1 | 最后更新：2026-06-15
