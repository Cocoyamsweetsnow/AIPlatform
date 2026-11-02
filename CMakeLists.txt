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
# 骨架阶段刻意只用「单头文件 + 系统库」，保证 5 分钟内能跑起来：
#   - cpp-httplib : HTTP/1.1 服务端（third_party/httplib.h）
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

# ---- 核心库 -------------------------------------------------------------
add_library(ai_core STATIC
  src/config.cpp
  src/util/id.cpp
  src/llm/mock_provider.cpp
  src/llm/openai_provider.cpp
  src/chat/session_store.cpp
  src/chat/chat_service.cpp
  src/http/server.cpp
)

target_include_directories(ai_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(ai_core PUBLIC ${CURL_TARGET} Threads::Threads)
target_compile_definitions(ai_core PUBLIC
  CPPHTTPLIB_THREAD_POOL_COUNT=64
  CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=8192
)

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
# 默认关闭：主线仍然只依赖 libcurl，没装 Drogon 的机器照常构建。
# 开启前先按 docs/POC-DROGON.md 安装 Drogon；验证完 POC 和这个开关一起删。
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
