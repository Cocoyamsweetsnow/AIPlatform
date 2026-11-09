cmake_minimum_required(VERSION 3.20)
project(aiplatform LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ---- 依赖 ---------------------------------------------------------------
#   - Drogon      : HTTP 服务端（主线，需 >= 1.9.14；见 docs/BUILD.md 安装）
#   - cpp-httplib : HTTP 服务端（回退项，third_party/httplib.h，仅 -DAIPLATFORM_HTTP=httplib 时用）
#   - nlohmann/json : JSON（third_party/json.hpp）
#   - libcurl     : 调用上游大模型 API（支持增量读，SSE 必需）
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(CURLPC REQUIRED IMPORTED_TARGET libcurl)
  set(CURL_TARGET PkgConfig::CURLPC)
else()
  find_package(CURL REQUIRED)
  set(CURL_TARGET CURL::libcurl)
endif()

# ---- PostgreSQL 持久化后端（可选）--------------------------------------
# 只依赖客户端运行库 libpq.so.5；头文件随仓库分发到 third_party/libpq/include，
# 因此目标机器不需要 libpq-dev（见 third_party/libpq/README.md）。
# 没有 libpq 的交付环境可以用 -DAIPLATFORM_WITH_POSTGRES=OFF 退回纯内存模式。
option(AIPLATFORM_WITH_POSTGRES "Build the PostgreSQL storage backend" ON)
if(AIPLATFORM_WITH_POSTGRES)
  find_library(PQ_LIBRARY NAMES pq)
  if(NOT PQ_LIBRARY)
    find_file(PQ_LIBPQ_SO NAMES libpq.so.5
      PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)
    if(PQ_LIBPQ_SO)
      set(PQ_LIBRARY "${PQ_LIBPQ_SO}")
    endif()
  endif()
  if(NOT PQ_LIBRARY)
    message(FATAL_ERROR
      "找不到 libpq。请安装 libpq5（运行时）或 libpq-dev（开发包），"
      "或者用 -DAIPLATFORM_WITH_POSTGRES=OFF 构建纯内存版本。")
  endif()
  set(AIP_PQ_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpq/include")
  message(STATUS "PostgreSQL storage: ${PQ_LIBRARY}")
endif()

# ---- HTTP 后端（决策见 docs/adr/0001-http-framework.md）------------------
# drogon  ：主线。连接不占线程，SSE 并发只受 max_streams 与线程数约束。
# httplib ：回退项。Drogon 装不上（客户机房 / 国产化 OS）时仍能构建，
#           代价是每连接一线程、并发流上限 = 线程池大小。
set(AIPLATFORM_HTTP "drogon" CACHE STRING "HTTP backend: drogon (default) or httplib")
set_property(CACHE AIPLATFORM_HTTP PROPERTY STRINGS drogon httplib)

# ---- 用户鉴权（JWT + Argon2id + API Key）--------------------------------
# Argon2id 只依赖 libargon2.so.1；头文件随仓库分发在 third_party/argon2/include。
option(AIPLATFORM_WITH_AUTH "Build user auth (JWT/Argon2id/API keys)" ON)
if(AIPLATFORM_WITH_AUTH)
  find_library(ARGON2_LIBRARY NAMES argon2)
  if(NOT ARGON2_LIBRARY)
    find_file(ARGON2_SO NAMES libargon2.so.1
      PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib)
    if(ARGON2_SO)
      set(ARGON2_LIBRARY "${ARGON2_SO}")
    endif()
  endif()
  if(NOT ARGON2_LIBRARY)
    message(FATAL_ERROR
      "找不到 libargon2。请安装 libargon2-1（运行时）或 libargon2-dev，"
      "或者用 -DAIPLATFORM_WITH_AUTH=OFF 只保留静态 Token。")
  endif()
  set(AIP_ARGON2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/include")
  find_package(OpenSSL REQUIRED)
  message(STATUS "User auth: Argon2 ${ARGON2_LIBRARY}, OpenSSL ${OPENSSL_VERSION}")
endif()

# ---- 核心库 -------------------------------------------------------------
set(AIP_CORE_SOURCES
  src/config.cpp
  src/util/id.cpp
  src/common/metrics.cpp
  src/common/structured_log.cpp
  src/llm/mock_provider.cpp
  src/llm/key_ring.cpp
  src/llm/openai_provider.cpp
  src/chat/session_store.cpp
  src/chat/chat_service.cpp
  src/chat/token_estimate.cpp
  src/chat/context_builder.cpp
  src/chat/idempotency.cpp
  src/quota/quota.cpp
)
if(AIPLATFORM_WITH_POSTGRES)
  list(APPEND AIP_CORE_SOURCES
    src/storage/db.cpp
    src/storage/migrations.cpp
    src/chat/pg_session_store.cpp
    src/chat/pg_idempotency_store.cpp
    src/usage/pg_usage_sink.cpp
  )
endif()
if(AIPLATFORM_WITH_AUTH)
  list(APPEND AIP_CORE_SOURCES
    src/auth/password_hasher.cpp
    src/auth/jwt.cpp
    src/auth/api_key.cpp
    src/auth/email_sender.cpp
    src/auth/auth_service.cpp
  )
  if(AIPLATFORM_WITH_POSTGRES)
    list(APPEND AIP_CORE_SOURCES
      src/auth/pg_user_store.cpp
      src/auth/rate_limiter.cpp
    )
  endif()
endif()
add_library(ai_core STATIC ${AIP_CORE_SOURCES})

# ---- Web UI（编译期嵌入二进制）-----------------------------------------
# 前端保持「零构建」：没有 npm / node_modules，源码直接进二进制，部署仍是一个可执行文件。
# 改完 web/ 重新 cmake --build 即可；新增文件由 CONFIGURE_DEPENDS 触发重新配置。
set(AIP_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
file(GLOB_RECURSE AIP_WEB_FILES CONFIGURE_DEPENDS "${AIP_WEB_DIR}/*")
# 不把 .env / .DS_Store 之类的隐藏文件编进公开站点。
list(FILTER AIP_WEB_FILES EXCLUDE REGEX "/\\.[^/]+$")
if(NOT AIP_WEB_FILES)
  message(FATAL_ERROR "web/ 目录为空：内置 Web UI 至少需要 web/index.html")
endif()

set(AIP_WEB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp")
add_custom_command(
  OUTPUT "${AIP_WEB_GENERATED}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT_DIR=${AIP_WEB_DIR}
          -DOUTPUT_FILE=${AIP_WEB_GENERATED}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake
  DEPENDS ${AIP_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake
  COMMENT "Embedding web UI into aiplatform binary"
  VERBATIM
)
target_sources(ai_core PRIVATE "${AIP_WEB_GENERATED}")

target_include_directories(ai_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(ai_core PUBLIC ${CURL_TARGET} Threads::Threads)
target_compile_definitions(ai_core PUBLIC AIP_VERSION="${PROJECT_VERSION}")

if(AIPLATFORM_WITH_POSTGRES)
  target_include_directories(ai_core PUBLIC ${AIP_PQ_INCLUDE_DIR})
  target_link_libraries(ai_core PUBLIC ${PQ_LIBRARY})
  target_compile_definitions(ai_core PUBLIC AIP_WITH_POSTGRES=1)
endif()

if(AIPLATFORM_WITH_AUTH)
  target_include_directories(ai_core PUBLIC ${AIP_ARGON2_INCLUDE_DIR})
  target_link_libraries(ai_core PUBLIC ${ARGON2_LIBRARY} OpenSSL::Crypto)
  target_compile_definitions(ai_core PUBLIC AIP_WITH_AUTH=1)
endif()

if(AIPLATFORM_HTTP STREQUAL "drogon")
  # Drogon 必须 >= 1.9.14：newAsyncStreamResponse 在 1.9.0 里还不存在。
  # 某些发行版/本地构建用 Boost.UUID 替代 libuuid，DrogonConfig 会要求 Boost::boost。
  find_package(Boost QUIET)
  find_package(Drogon CONFIG REQUIRED)
  target_sources(ai_core PRIVATE src/http/server_drogon.cpp)
  target_link_libraries(ai_core PUBLIC Drogon::Drogon)
  target_compile_definitions(ai_core PUBLIC AIP_HTTP_BACKEND_DROGON=1)
elseif(AIPLATFORM_HTTP STREQUAL "httplib")
  target_sources(ai_core PRIVATE src/http/server_httplib.cpp)
  target_compile_definitions(ai_core PUBLIC
    AIP_HTTP_BACKEND_HTTPLIB=1
    CPPHTTPLIB_THREAD_POOL_COUNT=64
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=8192
  )
else()
  message(FATAL_ERROR "AIPLATFORM_HTTP 只能是 drogon 或 httplib，当前值: '${AIPLATFORM_HTTP}'")
endif()

# ---- 可执行文件 ---------------------------------------------------------
add_executable(aiplatform src/main.cpp)
target_link_libraries(aiplatform PRIVATE ai_core)

# ---- 测试（可选） -------------------------------------------------------
option(AIPLATFORM_BUILD_TESTS "Build unit tests" OFF)
if(AIPLATFORM_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE ai_core)
  add_test(NAME unit_tests COMMAND unit_tests)

  add_executable(usage_tests tests/usage_tests.cpp)
  target_link_libraries(usage_tests PRIVATE ai_core)
  add_test(NAME usage_tests COMMAND usage_tests)

  add_executable(quota_tests tests/quota_tests.cpp)
  target_link_libraries(quota_tests PRIVATE ai_core)
  add_test(NAME quota_tests COMMAND quota_tests)

  add_executable(idempotency_tests tests/idempotency_tests.cpp)
  target_link_libraries(idempotency_tests PRIVATE ai_core)
  add_test(NAME idempotency_tests COMMAND idempotency_tests)

  add_executable(context_tests tests/context_tests.cpp)
  target_link_libraries(context_tests PRIVATE ai_core)
  add_test(NAME context_tests COMMAND context_tests)

  if(AIPLATFORM_WITH_POSTGRES)
    # 需要 AIP_TEST_PG_DSN；没有设置时测试内部直接 skip（CI 里会起 postgres service）。
    add_executable(pg_tests tests/pg_tests.cpp)
    target_link_libraries(pg_tests PRIVATE ai_core)
    add_test(NAME pg_tests COMMAND pg_tests)
  endif()

  if(AIPLATFORM_WITH_AUTH)
    add_executable(auth_tests tests/auth_tests.cpp)
    target_link_libraries(auth_tests PRIVATE ai_core)
    add_test(NAME auth_tests COMMAND auth_tests)
  endif()
endif()

# ---- Drogon SSE POC（可选，决策见 docs/adr/0001） ------------------------
# 迁移已落地到 src/http/server_drogon.cpp，这个 POC 只剩「压测基线」的作用：
# 想复现 docs/POC-DROGON.md 里的 200 路 / 300 路数据时才编它。
# 确认不再需要复现后，可以把这个 option 和 poc/ 目录一起删掉。
option(AIPLATFORM_BUILD_POC "Build the Drogon SSE proof-of-concept" OFF)
if(AIPLATFORM_BUILD_POC)
  find_package(Drogon CONFIG REQUIRED)
  add_executable(poc_drogon_sse
    poc/drogon_sse.cpp
    src/llm/mock_provider.cpp   # 复用真实链路：ThreadedStream + EventQueue
  )
  target_include_directories(poc_drogon_sse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  )
  target_link_libraries(poc_drogon_sse PRIVATE Drogon::Drogon Threads::Threads)
endif()
