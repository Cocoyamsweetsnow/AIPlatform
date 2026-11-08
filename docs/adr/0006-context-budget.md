# 0006 token 预算上下文裁剪

- 状态：**已落地（M5）**
- 日期：2026-11-08
- 决策者：项目负责人（AI 按 P0 收尾实施）
- 影响范围：`src/chat/context_builder.*`、`src/chat/chat_service.*`、`src/chat/token_estimate.*`、`src/config.*`、HTTP 错误映射

## 背景

M1 的历史裁剪只看“消息条数”（默认 20 条），一条长消息或超长历史仍可能超过模型上下文；
OpenAI 兼容上游会直接 400，用户只看到 502。需要按 token 预算裁剪。

## 决定

1. **估算**：`chat::estimate_tokens` 按 ASCII 4 字节/token、非 ASCII 1 码点/token 估算；
   这是保守近似，不引入 tokenizer 依赖。
2. **预算**：
   ```
   allowed = AIP_MAX_CONTEXT_TOKENS - AIP_MAX_TOKENS - safety(64)
   ```
   - 默认 `AIP_MAX_CONTEXT_TOKENS=8192`，`AIP_MAX_TOKENS=1024`；
   - 配置校验要求 `AIP_MAX_TOKENS < AIP_MAX_CONTEXT_TOKENS`。
3. **组装顺序**：system（可选）→ 最近历史（从新到旧装入，装不下就停，保持连续）→ 本轮 user。
4. **裁剪**：历史装完后去掉开头的 assistant/tool（部分上游要求第一条是 user/system）。
5. **超预算**：system + user 都放不进 allowed 时抛 `ContextTooLong`，
   HTTP 返回 `413 context_too_long`，不调用上游。
6. **保留**：`AIP_MAX_HISTORY` 仍然限制读取条数，避免一次查太多；token 预算再兜底。
7. **不使用摘要**：M5 只做裁剪；摘要/压缩需要额外模型调用，留到后续。

## 后果

**正面**

- 长历史/长消息不会再把上游打 400；
- 预算在应用层可见、可配置，错误语义明确（413 而不是 502）；
- 估算无依赖、零成本，适合网关快速路径。

**限制**

- 估算不是 tokenizer：中文、代码、JSON 可能偏差；安全余量 64 只是兜底；
- 没有按模型配置不同上下文窗口（一个进程一个 `AIP_MAX_CONTEXT_TOKENS`）；
- 裁剪会丢弃旧历史，不做摘要，用户可能感觉“忘了前面”；
- 没有对 system prompt 做单独上限，system 过长会直接 413；
- 超长单条历史消息会中断装入更早的小消息（保持连续性的取舍）。

## 验收记录（2026-11-08）

| 用例 | 结果 |
|---|---|
| `context_tests` | 预算裁剪、首条非 assistant、超长 user 413、非法预算全部通过 |
| `ctest` | 7 个测试目标全部通过 |
| 手工 curl | 超长 message 返回 413 `context_too_long`（没有调用上游） |
