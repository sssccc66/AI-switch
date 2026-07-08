cmake_minimum_required(VERSION 3.16)
project(ai-switch VERSION 1.0.0 LANGUAGES CXX)

# ============================================================
#  C++ 标准: C++17 — 使用 std::optional, string_view, variant 等
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# 查找依赖库
# ============================================================

# Boost: 提供 Asio（网络IO）和 Beast（HTTP协议）
# Beast 是 header-only 库, 无需链接 .so 文件
find_package(Boost REQUIRED COMPONENTS system)
if(Boost_FOUND)
    message(STATUS "Boost version: ${Boost_VERSION}")
endif()

# nlohmann-json: header-only JSON 库
find_package(nlohmann_json REQUIRED)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json found: ${nlohmann_json_VERSION}")
endif()

# MySQL: C API 客户端库 (libmysqlclient)
# 在 Ubuntu 24.04 上包名是 libmysqlclient-dev, 直接链接即可
find_package(MySQL QUIET)
if(NOT MySQL_FOUND)
    # 如果 FindMySQL 模块不工作, 手动查找
    message(STATUS "尝试手动查找 MySQL 库...")
    find_path(MYSQL_INCLUDE_DIR mysql/mysql.h PATHS /usr/include/mysql)
    find_library(MYSQL_LIBRARY mysqlclient PATHS /usr/lib/x86_64-linux-gnu /usr/lib)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        set(MySQL_FOUND TRUE)
        set(MySQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR})
        set(MySQL_LIBRARIES ${MYSQL_LIBRARY})
    endif()
endif()
if(MySQL_FOUND)
    message(STATUS "MySQL include: ${MySQL_INCLUDE_DIRS}")
    message(STATUS "MySQL library: ${MySQL_LIBRARIES}")
else()
    message(FATAL_ERROR "未找到 MySQL 库, 请安装: sudo apt install libmysqlclient-dev")
endif()

# CURL: 用于调用外部 AI API (libcurl)
find_package(CURL REQUIRED)
if(CURL_FOUND)
    message(STATUS "CURL include: ${CURL_INCLUDE_DIRS}")
    message(STATUS "CURL library: ${CURL_LIBRARIES}")
endif()

# ============================================================
# 源文件列表 — Week 5 加入了流式 SSE
# ============================================================
set(SOURCES
    src/main.cpp
    src/util/config.cpp
    src/server/http_server.cpp
    src/server/session.cpp
    src/server/router.cpp
    src/db/connection_pool.cpp
    src/db/repository.cpp
    src/middleware/auth_middleware.cpp
    src/middleware/rate_limiter.cpp
    src/adapter/deepseek_adapter.cpp
    src/adapter/adapter_factory.cpp
    src/stream/sse_handler.cpp
)

# ============================================================
#  编译目标
# ============================================================
add_executable(ai-switch ${SOURCES})

target_include_directories(ai-switch PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MySQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ai-switch PRIVATE
    Boost::system          # Asio 错误码、系统函数等
    nlohmann_json::nlohmann_json  # JSON 解析
    ${MySQL_LIBRARIES}     # MySQL 客户端库 (libmysqlclient)
    ${CURL_LIBRARIES}      # libcurl (调 AI API)
    pthread                # Asio 多线程需要
)

# ============================================================
# 编译选项：打开几乎所有警告（日常开发好习惯）
# ============================================================
target_compile_options(ai-switch PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter  # 回调中未使用的参数有时不可避免
)

# ============================================================
# 允许调试输出 (NDEBUG 控制日志级别)
# ============================================================
target_compile_definitions(ai-switch PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
)

# ============================================================
# 自动将配置文件复制到构建目录
# configure_file 在每次 cmake 配置时拷贝, COPYONLY 不做变量替换
# 这样在 build/ 目录下运行 ./ai-switch 就能找到 config.json
# ============================================================
configure_file(
    ${CMAKE_SOURCE_DIR}/config.json
    ${CMAKE_BINARY_DIR}/config.json
    COPYONLY
)
