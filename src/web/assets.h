// 编译期嵌入的 Web UI 静态资源。
//
// web/ 下的文件由 cmake/embed_web.cmake 生成成 raw string 常量，
// 随 ai_core 一起编进二进制——部署时不需要额外带一个静态目录，
// 也避免了把工作目录暴露成静态站点（见 server_drogon.cpp 的 kDisabledDocumentRoot）。
//
// 只在 HTTP 层使用：/、/assets/*、/favicon.svg 走这里。
#pragma once

#include <cstddef>
#include <string_view>

namespace web {

struct Asset {
  std::string_view path;          // URL 路径，例如 "/assets/app.js"
  std::string_view content;       // 原始字节（UTF-8）
  std::string_view content_type;  // 响应 Content-Type
};

/// 按 URL 路径精确查找；"/" 等价于 "/index.html"。未收录返回 nullptr。
/// 只做精确匹配，不参与文件系统路径解析，因此不存在目录穿越问题。
const Asset* find_asset(std::string_view path);

/// 当前二进制里收录的静态资源数量（测试 / 诊断用）。
std::size_t asset_count();

/// 是否是无需 Bearer Token 的公开静态资源（HTML / CSS / JS / favicon）。
bool is_public_asset(std::string_view path);

}  // namespace web
