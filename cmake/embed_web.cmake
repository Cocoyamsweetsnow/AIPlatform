# 把 web/ 下的静态资源生成成 C++ 文件，随二进制一起编译。
#
# 调用方式（CMakeLists.txt 的 add_custom_command）：
#   cmake -DINPUT_DIR=<repo>/web -DOUTPUT_FILE=<build>/generated/web_assets.cpp -P embed_web.cmake
#
# 用 raw string literal 而不是字节数组：源码体积小一半、编译更快，
# 内容里出现的 )AIPWEB" 会通过加长 delimiter 规避。

if(NOT DEFINED INPUT_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "embed_web.cmake 需要 -DINPUT_DIR=... -DOUTPUT_FILE=...")
endif()

file(GLOB_RECURSE asset_files RELATIVE "${INPUT_DIR}" "${INPUT_DIR}/*")
# 不把 .env / .DS_Store 之类的隐藏文件编进公开站点。
list(FILTER asset_files EXCLUDE REGEX "(^|/)\\.[^/]+$")
list(SORT asset_files)
if(NOT asset_files)
  message(FATAL_ERROR "web/ 目录下没有可嵌入的资源: ${INPUT_DIR}")
endif()

set(body "// 由 cmake/embed_web.cmake 自动生成，请勿手改。\n")
string(APPEND body "// 源目录: web/\n\n")
string(APPEND body "#include \"web/assets.h\"\n\n")
string(APPEND body "namespace {\n\n")

set(entries "")
set(asset_count 0)

foreach(rel IN LISTS asset_files)
  file(READ "${INPUT_DIR}/${rel}" content)

  # 路径 -> 合法 C++ 标识符
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" ident "${rel}")
  if(ident MATCHES "^[0-9]")
    set(ident "_${ident}")
  endif()

  # 选一个不会提前结束 raw string 的 delimiter
  set(delim "AIPWEB")
  string(FIND "${content}" ")${delim}\"" close_pos)
  while(NOT close_pos EQUAL -1)
    string(APPEND delim "_")
    string(FIND "${content}" ")${delim}\"" close_pos)
  endwhile()

  string(APPEND body "constexpr char kAsset_${ident}[] = R\"${delim}(${content})${delim}\";\n\n")

  get_filename_component(ext "${rel}" EXT)
  string(TOLOWER "${ext}" ext)
  if(ext STREQUAL ".html")
    set(mime "text/html; charset=utf-8")
  elseif(ext STREQUAL ".css")
    set(mime "text/css; charset=utf-8")
  elseif(ext STREQUAL ".js" OR ext STREQUAL ".mjs")
    set(mime "text/javascript; charset=utf-8")
  elseif(ext STREQUAL ".svg")
    set(mime "image/svg+xml")
  elseif(ext STREQUAL ".json")
    set(mime "application/json; charset=utf-8")
  elseif(ext STREQUAL ".png")
    set(mime "image/png")
  elseif(ext STREQUAL ".ico")
    set(mime "image/x-icon")
  elseif(ext STREQUAL ".woff2")
    set(mime "font/woff2")
  elseif(ext STREQUAL ".txt")
    set(mime "text/plain; charset=utf-8")
  else()
    set(mime "application/octet-stream")
  endif()

  string(APPEND entries "  {\"/${rel}\", std::string_view(kAsset_${ident}, sizeof(kAsset_${ident}) - 1), \"${mime}\"},\n")
  math(EXPR asset_count "${asset_count} + 1")
endforeach()

string(APPEND body "const web::Asset kAssets[] = {\n")
string(APPEND body "${entries}")
string(APPEND body "};\n\n")
string(APPEND body "constexpr std::size_t kAssetCount = ${asset_count};\n\n")
string(APPEND body "}  // namespace\n\n")
string(APPEND body "namespace web {\n\n")
string(APPEND body "const Asset* find_asset(std::string_view path) {\n")
string(APPEND body "  if (path == \"/\") path = \"/index.html\";\n")
string(APPEND body "  for (std::size_t i = 0; i < kAssetCount; ++i) {\n")
string(APPEND body "    if (path == kAssets[i].path) return &kAssets[i];\n")
string(APPEND body "  }\n")
string(APPEND body "  return nullptr;\n")
string(APPEND body "}\n\n")
string(APPEND body "std::size_t asset_count() { return kAssetCount; }\n\n")
string(APPEND body "bool is_public_asset(std::string_view path) { return find_asset(path) != nullptr; }\n\n")
string(APPEND body "}  // namespace web\n")

file(WRITE "${OUTPUT_FILE}" "${body}")
message(STATUS "Embedded ${asset_count} web asset(s) -> ${OUTPUT_FILE}")
