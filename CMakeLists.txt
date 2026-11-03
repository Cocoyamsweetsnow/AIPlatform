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

# ---- HTTP 后端（决策见 docs/adr/0001-http-framework.md）------------------
# drogon  ：主线。连接不占线程，SSE 并发只受 max_streams 与线程数约束。
# httplib ：回退项。Drogon 装不上（客户机房 / 国产化 OS）时仍能构建，
#           代价是每连接一线程、并发流上限 = 线程池大小。
set(AIPLATFORM_HTTP "drogon" CACHE STRING "HTTP backend: drogon (default) or httplib")
set_property(CACHE AIPLATFORM_HTTP PROPERTY STRINGS drogon httplib)

# ---- 核心库 -------------------------------------------------------------
add_library(ai_core STATIC
  src/config.cpp
  src/util/id.cpp
  src/llm/mock_provider.cpp
  src/llm/openai_provider.cpp
  src/chat/session_store.cpp
  src/chat/chat_service.cpp
)

target_include_directories(ai_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(ai_core PUBLIC ${CURL_TARGET} Threads::Threads)

if(AIPLATFORM_HTTP STREQUAL "drogon")
  # Drogon 必须 >= 1.9.14：newAsyncStreamResponse 在 1.9.0 里还不存在。
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
