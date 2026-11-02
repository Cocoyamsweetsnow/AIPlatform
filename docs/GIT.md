# 用 Git 管理这个项目

> 面向当前仓库状态：已 `git init`、**还没有任何提交**、当前分支 `master`、13 个未跟踪条目、
> 未配置 `user.name` / `user.email`。按下面第 1~3 节走一遍即可进入正轨。

---

## 1. 一次性配置

```bash
# 身份（不配的话 commit 会直接报错）
git config --global user.name "你的名字"
git config --global user.email "you@example.com"

# 新仓库默认用 main 分支（本仓库当前是 master，见下一步改名）
git config --global init.defaultBranch main

# 让 pull 默认 rebase，提交历史更干净（可选，团队统一即可）
git config --global pull.rebase true

# 中文文件名不要被转义成 \344\275\240
git config --global core.quotepath false
```

### 把当前分支改名为 main

```bash
git branch -m main          # git ≥ 2.30 支持对「还没有提交」的仓库改名
git symbolic-ref --short HEAD   # 确认输出 main
```

> 如果团队习惯 `master`，跳过这步，后续命令里的 main 换成 master 即可。

---

## 2. 让 Git 忽略该忽略的东西

`.gitignore` 已经就位（`build/`、`.env`、`*.log`、IDE 配置）。再补一个 `.gitattributes` 统一换行符，避免 Windows 同事把整个仓库改成 CRLF：

```bash
cat > .gitattributes <<'EOF'
* text=auto eol=lf
*.sh text eol=lf
*.bat text eol=crlf
*.png binary
*.jpg binary
*.pdf binary
EOF
```

### 加一个「防手滑」钩子（可选但强烈建议）

密钥一旦进了 Git 历史，删掉文件也没用。用本地钩子挡住：

```bash
cat > .git/hooks/pre-commit <<'EOF'
#!/bin/sh
if git diff --cached --name-only | grep -qE '(^|/)\.env$'; then
  echo "✗ 拒绝提交 .env（含密钥）。请只提交 config/config.example.env" >&2
  exit 1
fi
EOF
chmod +x .git/hooks/pre-commit
```

> `.git/hooks/` 不进版本库，每个开发者自己装一次。团队统一的话，可以在 README 里写一句。

---

## 3. 第一次提交

```bash
git add -A
git status --short          # ← 一定看一眼：不能出现 .env、build/、*.log
git commit -m "chore: 初始化 AIPlatform 项目骨架

- HTTP API + SSE 流式对话 + Provider 抽象
- OpenAI 兼容模型网关与离线 mock 实现
- 路线图、构建手册、选型清单"
git tag -a v0.1.0 -m "第一个可运行的骨架版本"
git log --oneline --decorate   # 应该看到 v0.1.0
```

---

## 4. 日常开发流程（trunk-based，适合 1~5 人）

`main` 永远可运行、可发布；所有改动走**短分支 + 合并**，分支存活不超过 3 天。

```bash
# 1) 从最新 main 拉一个分支
git switch main && git pull
git switch -c feat/pg-session-store

# 2) 小步提交（见第 5 节规范）
git add src/chat/pg_session_store.cpp src/chat/CMakeLists.txt
git commit -m "feat(chat): 增加 PostgreSQL 会话存储实现"

# 3) 推送并开 PR
git push -u origin feat/pg-session-store

# 4) 合并后清理
git switch main && git pull
git branch -d feat/pg-session-store
```

**分支命名**：`feat/xxx`、`fix/xxx`、`docs/xxx`、`chore/xxx`、`refactor/xxx`。

**合并方式**：PR 上用 **Squash and merge**，让 `main` 上一条 PR 对应一个提交，历史干净、回滚简单。

### 提交前自检

```bash
git diff --cached            # 逐行看一遍再提交，能挡掉 90% 的低级错误
```

---

## 5. 提交信息规范（Conventional Commits）

```
<类型>(<范围>): <一句话描述>
```

| 类型 | 用途 |
|---|---|
| `feat` | 新功能（会进 CHANGELOG 的「新特性」） |
| `fix` | 修 bug |
| `docs` | 只改文档 |
| `refactor` | 重构，不改行为 |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `chore` | 构建、依赖、杂项 |
| `ci` | CI 配置 |

**范围**（scope）用本项目的模块名：`llm`、`chat`、`http`、`config`、`build`、`docs`。

```
feat(llm): 支持 Anthropic Messages API
fix(http): 客户端断开时取消上游请求
docs(roadmap): 补充阶段 4 的配额设计
refactor(chat): 抽出 RecordingStream 装饰器
```

好处：以后能用 `git-cliff` / `standard-version` 自动生成 CHANGELOG 和版本号。

---

## 6. 远端与团队协作

```bash
git remote add origin git@github.com:your-org/aiplatform.git
git push -u origin main
git push origin v0.1.0
```

**保护 main 分支**（GitHub）：Settings → Branches → Add rule → 勾选
「Require a pull request before merging」+「Require status checks to pass」。

### CI：每次 push 自动编译 + 跑测试

`.github/workflows/ci.yml`（GitHub；GitLab 用 `.gitlab-ci.yml`，命令一样）：

```yaml
name: ci
on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: 安装依赖
        run: sudo apt-get update && sudo apt-get install -y libcurl4-openssl-dev
      - name: 配置
        run: cmake -S . -B build -DAIPLATFORM_BUILD_TESTS=ON
      - name: 编译
        run: cmake --build build -j"$(nproc)"
      - name: 测试
        run: ctest --test-dir build --output-on-failure
```

> 需要我直接生成这个文件的话说一声（默认没创建，避免你用的是 GitLab）。

---

## 7. 版本与发布

语义化版本 `MAJOR.MINOR.PATCH`；1.0 之前，`0.x` 每次功能发布 bump MINOR。

```bash
# 1) 确认 main 是干净的
git switch main && git pull && git status

# 2) 打标签（附注标签，带说明）
git tag -a v0.2.0 -m "PostgreSQL 持久化 + 多轮上下文落库"

# 3) 推送标签触发发布流水线
git push origin v0.2.0

# 4) 镜像 tag 与 Git tag 对齐，回滚时一目了然
docker build -t registry.example.com/aiplatform:0.2.0 .
docker tag registry.example.com/aiplatform:0.2.0 registry.example.com/aiplatform:latest
```

**回滚**：`git checkout v0.1.0` 看代码，线上回滚则把镜像 tag 切回旧版本即可（DB 迁移要向前兼容，见 ROADMAP 阶段 6）。

---

## 8. 出事了怎么办

### 提交了密钥（.env / API Key）

```bash
# 还没 push：
git rm --cached .env
git commit --amend --no-edit

# 已经 push：
pip install git-filter-repo
git filter-repo --path .env --invert-paths
git push --force
```

**但最重要的一步是：立刻去平台吊销那个 Key。** 历史清干净 ≠ 没泄露，可能已经被爬虫扫走。

### 撤销提交

```bash
git reset --soft HEAD~1     # 撤销提交，改动留在暂存区（未 push 时用）
git revert <sha>            # 已 push 时用：生成一个反向提交，安全
```

> 已推到远端的提交，**永远不要 `git reset --hard` + `push --force`**（除非你确定只有自己在用这个分支）。

### 误提交大文件

```bash
git rm --cached bigfile.bin
echo "bigfile.bin" >> .gitignore
git commit -m "chore: 移除误提交的大文件"
# 若文件已进历史且仓库被 clone 过，用 git-filter-repo 清理
```

### 合并冲突

```bash
git switch feat/xxx
git rebase main             # 把自己的提交挪到最新 main 之上
# 解决冲突 -> git add <文件> -> git rebase --continue
# 想放弃：git rebase --abort
```

### 找「是哪次提交引入的 bug」

```bash
git bisect start
git bisect bad              # 当前版本有问题
git bisect good v0.1.0      # 这个版本是好的
# Git 会自动切到中间版本，你编译测试后回答 good/bad，log2(N) 次就能定位
git bisect reset
```

---

## 9. 本项目的提交约定

| 必须提交 | 绝对不提交 |
|---|---|
| `src/` `tests/` `docs/` `scripts/` | `build/`（已在 .gitignore） |
| `CMakeLists.txt` `Dockerfile` `docker-compose.yml` `deploy/` | `.env`（含密钥） |
| `config/config.example.env`（模板，不含密钥） | `*.log`、`server.out` |
| `third_party/`（单头文件，约 1.2MB，保证离线可编译） | IDE 配置（`.vscode/` `.idea/`） |
| `.gitignore` `.gitattributes` `README.md` | 编译产物、临时文件 |

> 为什么把 `third_party/` 提交进仓库：保证任何人 clone 下来**不联网也能编译**。
> 代价是仓库多 1.2MB；等依赖变多（比如引入 Drogon）再转 vcpkg，见 [DECISIONS.md](DECISIONS.md) 的 D9。

---

## 10. 命令速查

| 目的 | 命令 |
|---|---|
| 看状态 | `git status -sb` |
| 看差异 | `git diff`（工作区）/ `git diff --cached`（暂存区） |
| 看历史 | `git log --oneline --graph --decorate -20` |
| 看某行谁改的 | `git blame -L 10,20 src/http/server.cpp` |
| 临时存一下 | `git stash` / `git stash pop` |
| 撤销工作区改动 | `git restore <file>` |
| 找回误删的分支 | `git reflog` → `git switch -c rescue <sha>` |
| 只提交部分改动 | `git add -p` |
