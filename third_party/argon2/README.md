# Argon2 客户端头文件（vendored）

本目录只包含 `argon2.h`，实现由系统 `libargon2.so.1` 提供
（Debian/Ubuntu 包名通常是 `libargon2-1`，开发包 `libargon2-dev`）。

- 上游：<https://github.com/P-H-C/phc-winner-argon2>
- 版本：`20190702`
- 许可证：CC0 1.0 / Apache 2.0（二选一，见头文件）
- 用途：密码哈希 `argon2id_hash_encoded` / `argon2id_verify`

## 为什么 vendored

目标机器经常只装了 `libargon2.so.1` 运行库，没有 `-dev` 包。
只复制公开头文件即可编译，不需要 root 安装开发包。

Windows 构建需要自行提供 `libargon2` 的导入库；本项目当前只支持 Linux。
