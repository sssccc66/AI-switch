# AI-switch

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![Boost](https://img.shields.io/badge/Boost-1.83-green.svg)]()
[![License](https://img.shields.io/badge/license-MIT-blue.svg)]()

AI-switch 是一个高性能异步 AI API 代理网关，统一管理多个 AI 模型的 API 请求，提供鉴权、限流、流式转发等功能。

---

## 特性

- **异步 HTTP 服务** — 基于 Boost.Beast + Asio 的事件循环架构，多线程 worker
- **API Key 鉴权** — 请求级别的密钥验证，支持启用/禁用
- **令牌桶限流** — 原子操作无锁消费，按时间懒补充
- **AI 适配器** — 策略模式设计，已支持 DeepSeek，预留 OpenAI 接口
- **流式响应** — SSE 协议逐 chunk 转发，libcurl 接收 → asio::post → 异步写回
- **管理 API** — 动态创建、列出、禁用 API Key
- **MySQL 持久化** — 连接池复用，超时保护，SQL 注入防护
- **安全配置** — 数据库密码和 API Key 通过环境变量传入，不写死在配置文件
- **优雅退出** — signal_set 捕获终止信号，安全释放所有资源

---

## 架构

```
Client ──→ HTTP Server (Beast + Asio)
                │
          Middleware Chain
          Auth → Rate Limit → Log
                │
          AI Adapter Layer
          DeepSeek / OpenAI
                │
          External AI API
                │
          MySQL (api_keys, sessions, messages)
```

中间件链采用**责任链模式**，可插拔扩展；AI 适配器采用**策略模式**，新增模型只需实现适配器接口。

---

## 快速开始

### 环境

- Ubuntu 24.04 / WSL2
- CMake 3.16+, GCC 13+, Boost 1.83+

### 安装依赖

```bash
sudo apt install -y cmake g++ libboost-all-dev libcurl4-openssl-dev \
                    libmysqlclient-dev nlohmann-json3-dev
```

### 初始化数据库

```bash
sudo mysql -u root < bootstrap.sql
sudo mysql -u root -e "
  CREATE USER IF NOT EXISTS 'ai_switch'@'127.0.0.1' IDENTIFIED BY 'ai_switch_pass';
  GRANT ALL PRIVILEGES ON ai_switch.* TO 'ai_switch'@'127.0.0.1';
  FLUSH PRIVILEGES;
"
```

### 编译运行

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

DEEPSEEK_API_KEY=sk-xxxxx DB_PASSWORD=ai_switch_pass ./ai-switch
```

---

## API 参考

### 系统接口

```bash
# 健康检查（无需鉴权）
curl http://localhost:8080/health
```

### 聊天接口

```bash
# 非流式
curl -X POST http://localhost:8080/api/chat \
  -H "Authorization: Bearer <your-api-key>" \
  -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"你好"}]}'

# 流式（SSE）
curl -N -X POST http://localhost:8080/api/chat \
  -H "Authorization: Bearer <your-api-key>" \
  -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"你好"}],"stream":true}'
```

### 管理接口

```bash
# 创建 API Key
curl -X POST http://localhost:8080/admin/api-keys \
  -H "Authorization: Bearer admin-master-key-001" \
  -H "Content-Type: application/json" \
  -d '{"name":"client-a","rate_limit":100}'

# 列出所有 Key
curl http://localhost:8080/admin/api-keys \
  -H "Authorization: Bearer admin-master-key-001"

# 禁用 Key
curl -X DELETE http://localhost:8080/admin/api-keys \
  -H "Authorization: Bearer admin-master-key-001" \
  -H "Content-Type: application/json" \
  -d '{"id":1}'
```

### 测试数据

初始化后数据库包含以下测试 API Key：

| Key | 说明 |
|-----|------|
| `sk-test-key-001` | 正常使用，60 RPM |
| `sk-test-key-002` | 正常使用，120 RPM |
| `sk-test-key-003` | 已禁用，用于测试鉴权拦截 |

---

## 配置

### 命令行参数

```bash
./ai-switch --config /etc/ai-switch/prod.json
./ai-switch --help
```

### 环境变量

```bash
DB_PASSWORD=xxx DEEPSEEK_API_KEY=xxx OPENAI_API_KEY=xxx ./ai-switch
```

优先级：环境变量 > 配置文件 > 默认值。

### 配置文件

参考 `config.json`：

```json
{
    "server": {
        "host": "0.0.0.0",
        "port": 8080,
        "thread_count": 4,
        "thread_pool_size": 8
    },
    "database": {
        "host": "127.0.0.1",
        "port": 3306,
        "user": "ai_switch",
        "db": "ai_switch",
        "pool_size": 4
    },
    "rate_limiter": {
        "default_capacity": 10,
        "default_refill_rate": 5
    }
}
```

---

## 项目结构

```
AI-switch/
├── CMakeLists.txt
├── config.json
├── bootstrap.sql
├── src/
│   ├── main.cpp
│   ├── server/          # HTTP 服务核心
│   │   ├── http_server  # Asio 服务器封装
│   │   ├── session      # 连接生命周期管理
│   │   └── router       # 路由分发 + 响应构建
│   ├── middleware/       # 中间件链
│   │   ├── middleware    # 基类 + context
│   │   ├── auth         # API Key 鉴权
│   │   ├── rate_limiter # 令牌桶限流
│   │   └── log          # 请求日志
│   ├── adapter/         # AI 适配器
│   │   ├── adapter      # 抽象接口
│   │   ├── deepseek     # DeepSeek 实现
│   │   └── factory      # 适配器工厂
│   ├── stream/          # SSE 流式处理
│   ├── db/              # 数据库层
│   │   ├── connection_pool
│   │   └── repository
│   └── util/            # 工具
│       ├── config
│       └── thread_pool
└── tests/
    └── test_rate_limiter
```

---

## 开发计划

- [x] HTTP 服务 + 路由分发
- [x] MySQL 集成 + API Key 鉴权
- [x] 令牌桶限流 + CAS 原子操作
- [x] DeepSeek AI 适配器（非流式 + 流式 SSE）
- [x] 管理 API（创建/列出/禁用 Key）
- [x] 线程池 + 配置安全优化
- [ ] OpenAI 适配器
- [ ] 配置热加载
- [ ] Prometheus 监控指标

---

## 许可证

MIT
