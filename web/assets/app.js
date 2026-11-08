/* AIPlatform Web UI — 无依赖单页应用。
 *
 * 设计约束：
 *   - 不引入任何构建工具 / 第三方库，源码直接进二进制；
 *   - 所有模型输出按不可信文本处理：先转义再拼 HTML，链接只放行 http(s)/mailto；
 *   - 会话保存在 localStorage，服务端会话是内存态；
 *   - 流式解析用 fetch + ReadableStream（EventSource 不能带 Authorization 头）。
 */
(function () {
  "use strict";

  var STORAGE = {
    sessions: "aiplatform.sessions.v1",
    prefs: "aiplatform.prefs.v1",
    current: "aiplatform.current.v1",
    auth: "aiplatform.auth.v1"
  };
  var MAX_SESSIONS = 40;
  var MAX_MESSAGES = 120;
  var DEFAULT_MAX_BYTES = 32 * 1024;

  // Safari 无痕模式 / 禁用站点数据时 localStorage 会抛异常；
  // 此时界面仍可用，只是不持久化——不要因此裁剪内存里的会话。
  var storageOk = (function () {
    try {
      var probe = "__aiplatform_probe__";
      window.localStorage.setItem(probe, "1");
      window.localStorage.removeItem(probe);
      return true;
    } catch (err) {
      return false;
    }
  })();

  var state = {
    sessions: [],
    currentId: null,
    config: null,
    health: null,
    healthChecked: false,
    healthLoading: false,
    prefs: { token: "", theme: "dark" },
    auth: null,
    authBusy: false,
    authGate: false,
    scopeVersion: 0,
    streaming: false,
    streamingMessageId: null,
    abortController: null,
    scrollPinned: true,
    composing: false
  };

  var dom = {};
  var pendingStreamMessage = null;
  var streamRenderTimer = null;

  /* ------------------------------------------------------------------ */
  /* 基础工具                                                            */
  /* ------------------------------------------------------------------ */

  function $(selector, root) {
    return (root || document).querySelector(selector);
  }

  function uid(prefix) {
    return (
      prefix +
      "_" +
      Date.now().toString(36) +
      "_" +
      Math.random().toString(36).slice(2, 8)
    );
  }

  function escapeHtml(value) {
    return String(value == null ? "" : value).replace(/[&<>"']/g, function (ch) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch];
    });
  }

  function icon(name) {
    return '<svg class="icon" aria-hidden="true"><use href="#i-' + name + '"></use></svg>';
  }

  function byteLength(text) {
    try {
      return new TextEncoder().encode(text).length;
    } catch (err) {
      return text.length * 3;
    }
  }

  function formatClock(timestamp) {
    try {
      return new Date(timestamp || Date.now()).toLocaleTimeString("zh-CN", {
        hour: "2-digit",
        minute: "2-digit"
      });
    } catch (err) {
      return "";
    }
  }

  function timeAgo(timestamp) {
    var diff = Date.now() - (Number(timestamp) || Date.now());
    if (diff < 0) diff = 0;
    if (diff < 60 * 1000) return "刚刚";
    if (diff < 60 * 60 * 1000) return Math.floor(diff / 60000) + " 分钟前";
    if (diff < 24 * 60 * 60 * 1000) return Math.floor(diff / 3600000) + " 小时前";
    if (diff < 7 * 24 * 60 * 60 * 1000) return Math.floor(diff / 86400000) + " 天前";
    try {
      return new Date(timestamp).toLocaleDateString("zh-CN", { month: "numeric", day: "numeric" });
    } catch (err) {
      return "";
    }
  }

  function startOfDay(timestamp) {
    var date = new Date(timestamp);
    date.setHours(0, 0, 0, 0);
    return date.getTime();
  }

  function dayGroup(timestamp) {
    var today = startOfDay(Date.now());
    var day = startOfDay(timestamp);
    if (day >= today) return "今天";
    if (day === today - 86400000) return "昨天";
    if (day >= today - 6 * 86400000) return "最近 7 天";
    return "更早";
  }

  function formatDuration(ms) {
    if (!ms || ms < 0) return "";
    if (ms < 1000) return Math.max(1, Math.round(ms)) + "ms";
    if (ms < 60000) return (ms / 1000).toFixed(ms < 10000 ? 1 : 0) + "s";
    return Math.floor(ms / 60000) + "m " + Math.round((ms % 60000) / 1000) + "s";
  }

  function formatBytes(bytes) {
    if (bytes < 1024) return bytes + " B";
    return (bytes / 1024).toFixed(bytes < 10240 ? 1 : 0) + " KB";
  }

  function makeTitle(text) {
    var line = String(text || "").replace(/\s+/g, " ").trim();
    if (!line) return "新对话";
    return line.length > 42 ? line.slice(0, 42) + "…" : line;
  }

  function readJSON(key, fallback) {
    try {
      var raw = window.localStorage.getItem(key);
      if (!raw) return fallback;
      return JSON.parse(raw);
    } catch (err) {
      return fallback;
    }
  }

  function writeJSON(key, value) {
    if (!storageOk) return false;
    try {
      window.localStorage.setItem(key, JSON.stringify(value));
      return true;
    } catch (err) {
      return false;
    }
  }

  /* ------------------------------------------------------------------ */
  /* 账号状态 / 本地存储作用域                                           */
  /* ------------------------------------------------------------------ */

  function normalizeUser(raw) {
    if (!raw || typeof raw !== "object") return null;
    var id = Number(raw.id);
    if (!id || id <= 0) return null;
    return {
      id: id,
      email: typeof raw.email === "string" ? raw.email : "",
      display_name: typeof raw.display_name === "string" ? raw.display_name : "",
      role: typeof raw.role === "string" ? raw.role : "user"
    };
  }

  function normalizeAuth(raw) {
    if (!raw || typeof raw !== "object") return null;
    var access = typeof raw.accessToken === "string" ? raw.accessToken : "";
    var refresh = typeof raw.refreshToken === "string" ? raw.refreshToken : "";
    var user = normalizeUser(raw.user);
    if (!access || !user) return null;
    return { accessToken: access, refreshToken: refresh, user: user };
  }

  function loadAuth() {
    return normalizeAuth(readJSON(STORAGE.auth, null));
  }

  function persistAuth() {
    if (!storageOk) return;
    try {
      if (!state.auth) {
        window.localStorage.removeItem(STORAGE.auth);
        return;
      }
      window.localStorage.setItem(STORAGE.auth, JSON.stringify(state.auth));
    } catch (err) {
      /* 隐私模式 / 存储满：本次登录仍可用，只是刷新后需要重新登录 */
    }
  }

  function authUser() {
    return state.auth && state.auth.user ? state.auth.user : null;
  }

  // 本地会话按账号隔离：登录前是 .anon，登录后是 .u<user_id>。
  // 这样同一浏览器切换账号时不会看到上一个账号的本地缓存。
  function sessionScope() {
    var user = authUser();
    return user ? "u" + String(user.id) : "anon";
  }

  function scopedStorageKey(base) {
    return base + "." + sessionScope();
  }

  function migrateLegacyStorage() {
    if (!storageOk) return;
    // 旧版本的 key 没有账号前缀，统一迁到匿名作用域；无论当前是否登录都执行，
    // 这样退出登录后仍能找回升级前的本地会话。
    try {
      var sessionsKey = STORAGE.sessions + ".anon";
      if (!window.localStorage.getItem(sessionsKey)) {
        var legacySessions = window.localStorage.getItem(STORAGE.sessions);
        if (legacySessions) window.localStorage.setItem(sessionsKey, legacySessions);
      }
      var currentKey = STORAGE.current + ".anon";
      if (!window.localStorage.getItem(currentKey)) {
        var legacyCurrent = window.localStorage.getItem(STORAGE.current);
        if (legacyCurrent) window.localStorage.setItem(currentKey, legacyCurrent);
      }
    } catch (err) {
      /* 迁移失败不影响新会话；老 key 保留不动 */
    }
  }

  function applyTokenPair(pair) {
    if (!pair || typeof pair.access_token !== "string" || !pair.access_token) return false;
    var user = normalizeUser(pair.user) || authUser();
    if (!user) return false;
    state.auth = {
      accessToken: pair.access_token,
      refreshToken: typeof pair.refresh_token === "string" ? pair.refresh_token : "",
      user: user
    };
    persistAuth();
    updateAuthUI();
    return true;
  }

  function clearAuth() {
    state.auth = null;
    persistAuth();
    updateAuthUI();
  }

  function userAuthAvailable() {
    var auth = state.config && state.config.auth;
    return Boolean(auth && auth.user_accounts);
  }

  function authToken() {
    if (state.auth && state.auth.accessToken) return state.auth.accessToken;
    return state.prefs.token;
  }

  function hasCredentials() {
    return Boolean(authToken());
  }

  function toast(message, type, duration) {
    if (!dom.toastStack) return;
    var node = document.createElement("div");
    node.className = "toast is-" + (type || "info");
    node.textContent = message;
    dom.toastStack.appendChild(node);
    window.requestAnimationFrame(function () {
      node.classList.add("is-visible");
    });
    window.setTimeout(function () {
      node.classList.remove("is-visible");
      window.setTimeout(function () {
        node.remove();
      }, 220);
    }, duration || (type === "error" ? 4600 : 3000));
  }

  function copyText(text) {
    function fallback() {
      var area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      var ok = false;
      try {
        ok = document.execCommand("copy");
      } catch (err) {
        ok = false;
      }
      area.remove();
      if (ok) toast("已复制", "success", 1600);
      else toast("复制失败，请手动选择文本", "error");
    }

    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(
        function () {
          toast("已复制", "success", 1600);
        },
        fallback
      );
    } else {
      fallback();
    }
  }

  /* ------------------------------------------------------------------ */
  /* Markdown 渲染（先转义，再只生成白名单标签）                          */
  /* ------------------------------------------------------------------ */

  var KEYWORDS = new Set(
    (
      "const let var function return if else for while do switch case break continue new delete " +
      "class struct enum union namespace using public private protected virtual override final " +
      "static constexpr consteval constinit inline template typename auto this nullptr true false " +
      "import export from as async await try catch throw throws finally yield typeof instanceof " +
      "interface extends implements package void int long short char bool float double string " +
      "size_t std fn impl trait mut pub crate match loop where def lambda pass with in is not and " +
      "or None True False null nil func go defer chan range select map package type"
    ).split(/\s+/)
  );

  var HASH_COMMENT_LANGS = /^(py|python|sh|bash|shell|zsh|yaml|yml|toml|rb|ruby|r|makefile)$/i;
  var C_COMMENT_LANGS = /^(c|cpp|c\+\+|cc|h|hpp|js|javascript|jsx|ts|typescript|tsx|java|go|rust|rs|json|jsonc|sql|swift|kt|kotlin|cs|csharp|php|scala)$/i;

  function highlightCode(code, lang) {
    var isHash = HASH_COMMENT_LANGS.test(lang);
    var isC = C_COMMENT_LANGS.test(lang);
    if (!isHash && !isC) return escapeHtml(code);

    var pattern;
    if (/^(json|jsonc)$/i.test(lang)) {
      pattern = /("(?:\\.|[^"\\])*"|\b\d+(?:\.\d+)?\b|\b(?:true|false|null)\b)/g;
    } else if (isHash) {
      pattern = /(#[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`|\b\d+(?:\.\d+)?\b|\b[A-Za-z_][A-Za-z0-9_]*\b)/g;
    } else {
      pattern = /(\/\*[\s\S]*?\*\/|\/\/[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`|\b\d+(?:\.\d+)?\b|\b[A-Za-z_][A-Za-z0-9_]*\b)/g;
    }

    var out = "";
    var last = 0;
    var match;
    while ((match = pattern.exec(code))) {
      out += escapeHtml(code.slice(last, match.index));
      var token = match[0];
      var cls = "";
      if (/^(\/\/|\/\*|#)/.test(token)) cls = "tok-comment";
      else if (/^["'`]/.test(token)) cls = "tok-string";
      else if (/^\d/.test(token)) cls = "tok-number";
      else if (KEYWORDS.has(token)) cls = "tok-keyword";
      else if (/^[A-Z][A-Za-z0-9_]*$/.test(token)) cls = "tok-type";
      out += cls
        ? '<span class="' + cls + '">' + escapeHtml(token) + "</span>"
        : escapeHtml(token);
      last = match.index + token.length;
    }
    out += escapeHtml(code.slice(last));
    return out;
  }

  function renderCodeBlock(code, lang) {
    var safeLang = String(lang || "text").replace(/[^\w+#.-]/g, "").slice(0, 24) || "text";
    return (
      '<div class="code-block">' +
      '<div class="code-head"><span>' +
      escapeHtml(safeLang) +
      '</span><button class="code-copy" type="button" data-action="copy-code">' +
      icon("copy") +
      "复制</button></div>" +
      '<pre><code class="language-' +
      escapeHtml(safeLang) +
      '">' +
      highlightCode(code, safeLang) +
      "</code></pre></div>"
    );
  }

  function renderInline(text) {
    var tokens = [];
    var value = escapeHtml(text);

    value = value.replace(/`([^`\n]+)`/g, function (all, code) {
      tokens.push("<code>" + code + "</code>");
      return "\u0000" + (tokens.length - 1) + "\u0000";
    });

    value = value.replace(/\[([^\]\n]+)\]\(([^)\s]+)\)/g, function (all, label, href) {
      var decoded = href.replace(/&amp;/g, "&");
      if (!/^(https?:\/\/|mailto:)/i.test(decoded)) return all;
      tokens.push(
        '<a href="' +
          escapeHtml(decoded) +
          '" target="_blank" rel="noopener noreferrer">' +
          label +
          "</a>"
      );
      return "\u0000" + (tokens.length - 1) + "\u0000";
    });

    value = value.replace(/(^|[\s(（])(https?:\/\/[^\s<()（）]+)/g, function (all, prefix, url) {
      var trailing = url.match(/[.,;:!?，。；：！？]+$/);
      var clean = trailing ? url.slice(0, -trailing[0].length) : url;
      var tail = trailing ? trailing[0] : "";
      var decoded = clean.replace(/&amp;/g, "&");
      tokens.push(
        '<a href="' +
          escapeHtml(decoded) +
          '" target="_blank" rel="noopener noreferrer">' +
          escapeHtml(decoded) +
          "</a>"
      );
      return prefix + "\u0000" + (tokens.length - 1) + "\u0000" + tail;
    });

    value = value
      .replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>")
      .replace(/__([^_\n]+)__/g, "<strong>$1</strong>")
      .replace(/~~([^~\n]+)~~/g, "<del>$1</del>")
      .replace(/(^|[\s(（])\*([^*\n]+)\*(?=$|[\s)）.,!?，。！？])/g, "$1<em>$2</em>")
      .replace(/(^|[\s(（])_([^_\n]+)_(?=$|[\s)）.,!?，。！？])/g, "$1<em>$2</em>");

    value = value.replace(/\u0000(\d+)\u0000/g, function (all, index) {
      return tokens[Number(index)] || "";
    });
    return value;
  }

  function splitTableRow(row) {
    return String(row)
      .trim()
      .replace(/^\|/, "")
      .replace(/\|$/, "")
      .split("|")
      .map(function (cell) {
        return cell.trim();
      });
  }

  function renderMarkdown(source) {
    var text = String(source == null ? "" : source).replace(/\r\n?/g, "\n");
    var lines = text.split("\n");
    var out = [];
    var paragraph = [];
    var list = null;
    var quote = [];

    function flushParagraph() {
      if (!paragraph.length) return;
      out.push("<p>" + paragraph.map(renderInline).join("<br>") + "</p>");
      paragraph = [];
    }

    function flushList() {
      if (!list) return;
      var tag = list.ordered ? "ol" : "ul";
      out.push(
        "<" +
          tag +
          ">" +
          list.items
            .map(function (item) {
              return "<li>" + renderInline(item) + "</li>";
            })
            .join("") +
          "</" +
          tag +
          ">"
      );
      list = null;
    }

    function flushQuote() {
      if (!quote.length) return;
      out.push("<blockquote>" + quote.map(renderInline).join("<br>") + "</blockquote>");
      quote = [];
    }

    function flushAll() {
      flushParagraph();
      flushList();
      flushQuote();
    }

    var i = 0;
    while (i < lines.length) {
      var line = lines[i];

      var fence = line.match(/^\s*```+\s*([^\s`]*).*$/);
      if (fence) {
        flushAll();
        var lang = fence[1] || "";
        var code = [];
        i += 1;
        while (i < lines.length && !/^\s*```+\s*$/.test(lines[i])) {
          code.push(lines[i]);
          i += 1;
        }
        if (i < lines.length) i += 1;
        out.push(renderCodeBlock(code.join("\n"), lang));
        continue;
      }

      if (!line.trim()) {
        flushAll();
        i += 1;
        continue;
      }

      if (/^\s*(?:-{3,}|\*{3,}|_{3,})\s*$/.test(line)) {
        flushAll();
        out.push("<hr>");
        i += 1;
        continue;
      }

      var heading = line.match(/^(#{1,6})\s+(.+?)\s*#*\s*$/);
      if (heading) {
        flushAll();
        var level = heading[1].length;
        out.push("<h" + level + ">" + renderInline(heading[2]) + "</h" + level + ">");
        i += 1;
        continue;
      }

      if (/^\s*>/.test(line)) {
        flushParagraph();
        flushList();
        quote.push(line.replace(/^\s*>\s?/, ""));
        i += 1;
        continue;
      }

      var bullet = line.match(/^\s*[-*+]\s+(.+)$/);
      var ordered = line.match(/^\s*\d+[.)]\s+(.+)$/);
      if (bullet || ordered) {
        flushParagraph();
        flushQuote();
        var isOrdered = Boolean(ordered);
        if (list && list.ordered !== isOrdered) flushList();
        if (!list) list = { ordered: isOrdered, items: [] };
        list.items.push(bullet ? bullet[1] : ordered[1]);
        i += 1;
        continue;
      }

      if (
        line.indexOf("|") !== -1 &&
        i + 1 < lines.length &&
        /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)+\|?\s*$/.test(lines[i + 1])
      ) {
        flushAll();
        var headers = splitTableRow(line);
        i += 2;
        var rows = [];
        while (i < lines.length && lines[i].trim() && lines[i].indexOf("|") !== -1) {
          rows.push(splitTableRow(lines[i]));
          i += 1;
        }
        out.push(
          '<div class="table-wrap"><table><thead><tr>' +
            headers
              .map(function (cell) {
                return "<th>" + renderInline(cell) + "</th>";
              })
              .join("") +
            "</tr></thead><tbody>" +
            rows
              .map(function (row) {
                return (
                  "<tr>" +
                  headers
                    .map(function (unused, column) {
                      return "<td>" + renderInline(row[column] || "") + "</td>";
                    })
                    .join("") +
                  "</tr>"
                );
              })
              .join("") +
            "</tbody></table></div>"
        );
        continue;
      }

      flushList();
      flushQuote();
      paragraph.push(line);
      i += 1;
    }

    flushAll();
    return out.join("\n");
  }

  /* ------------------------------------------------------------------ */
  /* 会话持久化                                                          */
  /* ------------------------------------------------------------------ */

  function normalizeMessage(raw) {
    if (!raw || typeof raw !== "object") return null;
    if (raw.role !== "user" && raw.role !== "assistant") return null;
    var message = {
      id: typeof raw.id === "string" ? raw.id : uid("m"),
      role: raw.role,
      content: typeof raw.content === "string" ? raw.content : "",
      createdAt: Number(raw.createdAt) || Date.now(),
      model: typeof raw.model === "string" ? raw.model : "",
      error: typeof raw.error === "string" ? raw.error : "",
      elapsedMs: Number(raw.elapsedMs) || 0,
      usage: raw.usage && typeof raw.usage === "object" ? raw.usage : null
    };
    if (raw.role === "assistant") {
      var allowed = { streaming: 1, done: 1, error: 1, stopped: 1 };
      message.state = allowed[raw.state] ? raw.state : "done";
      if (message.state === "streaming") {
        message.state = message.content ? "stopped" : "error";
        if (!message.content) message.error = "页面刷新，生成已中断";
      }
    }
    return message;
  }

  function normalizeSession(raw) {
    if (!raw || typeof raw !== "object") return null;
    if (typeof raw.id !== "string" || !raw.id) return null;
    var messages = [];
    if (Array.isArray(raw.messages)) {
      messages = raw.messages
        .map(normalizeMessage)
        .filter(Boolean)
        .slice(-MAX_MESSAGES);
    }
    return {
      id: raw.id,
      serverId: typeof raw.serverId === "string" && raw.serverId ? raw.serverId : null,
      title: typeof raw.title === "string" && raw.title ? raw.title : "对话",
      createdAt: Number(raw.createdAt) || Date.now(),
      updatedAt: Number(raw.updatedAt) || Date.now(),
      messages: messages
    };
  }

  function loadSessions() {
    var raw = readJSON(scopedStorageKey(STORAGE.sessions), []);
    if (!Array.isArray(raw)) return [];
    return raw
      .map(normalizeSession)
      .filter(Boolean)
      .sort(function (a, b) {
        return b.updatedAt - a.updatedAt;
      })
      .slice(0, MAX_SESSIONS);
  }

  function normalizePrefs(raw) {
    var prefs = { token: "", theme: "dark" };
    if (!raw || typeof raw !== "object") return prefs;
    if (typeof raw.token === "string") prefs.token = raw.token;
    if (raw.theme === "dark" || raw.theme === "light" || raw.theme === "system") {
      prefs.theme = raw.theme;
    }
    return prefs;
  }

  function persistSessions() {
    if (!storageOk) return;
    if (writeJSON(scopedStorageKey(STORAGE.sessions), state.sessions)) return;
    // 配额满：丢掉一半旧会话再试一次，至少保住当前对话。
    var keep = currentSession();
    state.sessions = state.sessions.slice(0, Math.max(1, Math.floor(state.sessions.length / 2)));
    if (keep) {
      var present = state.sessions.some(function (session) {
        return session.id === keep.id;
      });
      if (!present) state.sessions.unshift(keep);
    }
    if (!writeJSON(scopedStorageKey(STORAGE.sessions), state.sessions)) {
      toast("本地存储已满，会话未能保存", "error");
    }
  }

  function persistPrefs() {
    if (storageOk) writeJSON(STORAGE.prefs, state.prefs);
  }

  function persistCurrent() {
    if (!storageOk) return;
    try {
      window.localStorage.setItem(scopedStorageKey(STORAGE.current), state.currentId || "");
    } catch (err) {
      /* 忽略：只影响刷新后恢复哪条会话 */
    }
  }

  function loadCurrentId() {
    try {
      var raw = window.localStorage.getItem(scopedStorageKey(STORAGE.current));
      if (!raw) return null;
      if (!state.sessions.some(function (session) { return session.id === raw; })) return null;
      return raw;
    } catch (err) {
      return null;
    }
  }

  function currentSession() {
    if (!state.currentId) return null;
    for (var i = 0; i < state.sessions.length; i += 1) {
      if (state.sessions[i].id === state.currentId) return state.sessions[i];
    }
    return null;
  }

  // 登录 / 退出后切换到对应账号的本地会话作用域（云端会话同步是下一步）。
  function reloadSessionScope() {
    state.scopeVersion += 1;
    state.currentId = loadCurrentId();
    state.sessions = loadSessions();
    renderSessionList();
    renderChat();
    updateComposerState();
    if (state.currentId && dom.chatScroll) {
      window.requestAnimationFrame(scrollToBottom);
    }
  }

  function createSession(title) {
    var now = Date.now();
    return {
      id: uid("c"),
      serverId: null,
      title: makeTitle(title),
      createdAt: now,
      updatedAt: now,
      messages: []
    };
  }

  function trimSession(session) {
    if (session.messages.length <= MAX_MESSAGES) return;
    session.messages = session.messages.slice(-MAX_MESSAGES);
    while (session.messages.length && session.messages[0].role !== "user") {
      session.messages.shift();
    }
  }

  /* ------------------------------------------------------------------ */
  /* 主题                                                                */
  /* ------------------------------------------------------------------ */

  function resolveTheme(preference) {
    if (preference !== "system") return preference;
    try {
      return window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
    } catch (err) {
      return "dark";
    }
  }

  function applyTheme() {
    var resolved = resolveTheme(state.prefs.theme);
    document.documentElement.setAttribute("data-theme", resolved);
    var meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute("content", resolved === "light" ? "#f3f5fb" : "#05070d");
    updateThemeButtons();
  }

  function updateThemeButtons() {
    if (!dom.themeSeg) return;
    var buttons = dom.themeSeg.querySelectorAll("[data-theme-choice]");
    for (var i = 0; i < buttons.length; i += 1) {
      buttons[i].setAttribute(
        "aria-checked",
        buttons[i].getAttribute("data-theme-choice") === state.prefs.theme ? "true" : "false"
      );
    }
  }

  function setTheme(theme) {
    if (theme !== "dark" && theme !== "light" && theme !== "system") return;
    state.prefs.theme = theme;
    persistPrefs();
    applyTheme();
  }

  function toggleTheme() {
    var resolved = resolveTheme(state.prefs.theme);
    setTheme(resolved === "dark" ? "light" : "dark");
  }

  /* ------------------------------------------------------------------ */
  /* 渲染：会话列表 / 消息                                                */
  /* ------------------------------------------------------------------ */

  function renderSessionList() {
    if (!dom.sessionList) return;
    var sorted = state.sessions.slice().sort(function (a, b) {
      return b.updatedAt - a.updatedAt;
    });

    if (!sorted.length) {
      dom.sessionList.innerHTML =
        '<p class="sessions-empty">还没有对话。<br>发一条消息开始，会话会自动保存在本机。</p>';
      return;
    }

    var order = ["今天", "昨天", "最近 7 天", "更早"];
    var groups = {};
    order.forEach(function (label) {
      groups[label] = [];
    });
    sorted.forEach(function (session) {
      groups[dayGroup(session.updatedAt)].push(session);
    });

    var html = "";
    order.forEach(function (label) {
      if (!groups[label].length) return;
      html += '<div class="session-group-label">' + label + "</div>";
      groups[label].forEach(function (session) {
        var active = session.id === state.currentId;
        html +=
          '<div class="session-item' +
          (active ? " is-active" : "") +
          '">' +
          '<button class="session-open" type="button" data-open="' +
          escapeHtml(session.id) +
          '" title="' +
          escapeHtml(session.title) +
          '">' +
          '<span class="session-icon" aria-hidden="true">' +
          icon("chat") +
          "</span>" +
          '<span class="session-body"><span class="session-title">' +
          escapeHtml(session.title) +
          '</span><span class="session-time">' +
          escapeHtml(timeAgo(session.updatedAt)) +
          "</span></span></button>" +
          '<button class="session-del" type="button" data-delete="' +
          escapeHtml(session.id) +
          '" aria-label="删除会话">' +
          icon("trash") +
          "</button></div>";
      });
    });
    dom.sessionList.innerHTML = html;
  }

  function messageStats(message) {
    if (message.state === "streaming") return "生成中…";
    var parts = [];
    if (message.elapsedMs) parts.push(formatDuration(message.elapsedMs));
    var total = message.usage && Number(message.usage.total_tokens);
    if (total) parts.push(total + " tok");
    return parts.join(" · ");
  }

  function updateAssistantContent(article, message, isLast) {
    var body = article.querySelector("[data-body]");
    if (!body) return;
    if (message.state === "streaming" && !message.content) {
      body.classList.add("is-typing");
      body.innerHTML = '<span class="typing" aria-label="正在生成"><i></i><i></i><i></i></span>';
    } else {
      body.classList.remove("is-typing");
      body.innerHTML = renderMarkdown(message.content);
      if (message.state === "streaming") {
        body.insertAdjacentHTML("beforeend", '<span class="caret" aria-hidden="true"></span>');
      }
    }

    var note = article.querySelector("[data-note]");
    if (note) {
      if (message.state === "error") {
        note.innerHTML =
          '<div class="msg-note">' +
          icon("alert") +
          '<span class="msg-note-text">' +
          escapeHtml(message.error || "生成失败，请重试") +
          "</span></div>";
      } else if (message.state === "stopped") {
        note.innerHTML =
          '<div class="msg-note is-warn">' +
          icon("stop") +
          '<span class="msg-note-text">已停止生成' +
          (message.content ? "，上方为已返回的内容" : "") +
          "。</span></div>";
      } else if (message.state === "done" && !message.content) {
        note.innerHTML =
          '<div class="msg-note is-warn">' + icon("alert") + '<span class="msg-note-text">模型没有返回内容。</span></div>';
      } else {
        note.innerHTML = "";
      }
    }

    var actions = article.querySelector("[data-actions]");
    if (actions) {
      var parts = [];
      if (message.content) {
        parts.push(
          '<button class="msg-action" type="button" data-action="copy-message">' +
            icon("copy") +
            "复制</button>"
        );
      }
      if (isLast && message.state !== "streaming") {
        parts.push(
          '<button class="msg-action" type="button" data-action="retry">' +
            icon("refresh") +
            "重新生成</button>"
        );
      }
      var stats = messageStats(message);
      if (stats) parts.push('<span class="msg-stats">' + escapeHtml(stats) + "</span>");
      actions.innerHTML = parts.join("");
    }
  }

  function buildMessageNode(message, index, total) {
    var isLast = index === total - 1;
    if (message.role === "user") {
      var userArticle = document.createElement("article");
      userArticle.className = "message user";
      userArticle.dataset.id = message.id;
      var bubble = document.createElement("div");
      bubble.className = "user-bubble";
      bubble.textContent = message.content;
      userArticle.appendChild(bubble);
      return userArticle;
    }

    var article = document.createElement("article");
    article.className = "message assistant is-" + (message.state || "done");
    article.dataset.id = message.id;

    var avatar = document.createElement("div");
    avatar.className = "msg-avatar";
    avatar.setAttribute("aria-hidden", "true");
    avatar.innerHTML = icon("spark");

    var main = document.createElement("div");
    main.className = "msg-main";
    var model = message.model || (state.config && state.config.model) || "";
    main.innerHTML =
      '<div class="msg-meta"><span class="msg-author">AIPlatform</span>' +
      (model ? '<span class="msg-model">' + escapeHtml(model) + "</span>" : "") +
      '<span class="msg-time">' +
      escapeHtml(formatClock(message.createdAt)) +
      "</span></div>" +
      '<div class="msg-body markdown" data-body></div>' +
      "<div data-note></div>" +
      '<div class="msg-actions" data-actions></div>';

    article.appendChild(avatar);
    article.appendChild(main);
    updateAssistantContent(article, message, isLast);
    return article;
  }

  function renderChat() {
    if (!dom.messages) return;
    var session = currentSession();
    var messages = session ? session.messages : [];
    dom.welcome.hidden = messages.length > 0;
    dom.messages.innerHTML = "";
    messages.forEach(function (message, index) {
      dom.messages.appendChild(buildMessageNode(message, index, messages.length));
    });
    dom.chatTitle.textContent = session ? session.title : "新对话";
    document.title = session ? session.title + " · AIPlatform" : "AIPlatform";
  }

  function findMessageNode(id) {
    if (!dom.messages) return null;
    var nodes = dom.messages.children;
    for (var i = 0; i < nodes.length; i += 1) {
      if (nodes[i].dataset.id === id) return nodes[i];
    }
    return null;
  }

  function updateStreamingNode(message) {
    var article = findMessageNode(message.id);
    if (!article || !message.content) return;
    var body = article.querySelector("[data-body]");
    if (!body) return;
    body.classList.remove("is-typing");
    body.innerHTML =
      renderMarkdown(message.content) + '<span class="caret" aria-hidden="true"></span>';
  }

  function scheduleStreamingRender(message) {
    pendingStreamMessage = message;
    if (streamRenderTimer) return;
    streamRenderTimer = window.setTimeout(function () {
      streamRenderTimer = null;
      var target = pendingStreamMessage;
      if (!target || target.id !== state.streamingMessageId) return;
      updateStreamingNode(target);
      if (state.scrollPinned) scrollToBottom();
    }, 36);
  }

  /* ------------------------------------------------------------------ */
  /* 滚动                                                                */
  /* ------------------------------------------------------------------ */

  function scrollToBottom() {
    if (!dom.chatScroll) return;
    dom.chatScroll.scrollTop = dom.chatScroll.scrollHeight;
    state.scrollPinned = true;
    if (dom.scrollBottomBtn) dom.scrollBottomBtn.classList.remove("is-visible");
  }

  function onChatScroll() {
    var distance =
      dom.chatScroll.scrollHeight - dom.chatScroll.scrollTop - dom.chatScroll.clientHeight;
    state.scrollPinned = distance < 110;
    if (dom.scrollBottomBtn) {
      dom.scrollBottomBtn.classList.toggle("is-visible", distance > 220);
    }
    if (dom.topbar) {
      dom.topbar.classList.toggle("is-scrolled", dom.chatScroll.scrollTop > 6);
    }
  }

  /* ------------------------------------------------------------------ */
  /* 账号 UI / 登录注册                                                  */
  /* ------------------------------------------------------------------ */

  var authMode = "login";

  function accountInitial(user) {
    var source = (user && (user.display_name || user.email)) || "?";
    return String(source).trim().charAt(0).toUpperCase() || "?";
  }

  function updateAuthUI() {
    var user = authUser();
    var available = userAuthAvailable();

    if (dom.accountLoginBtn) {
      dom.accountLoginBtn.hidden = !available || Boolean(user);
    }
    if (dom.accountCard) {
      dom.accountCard.hidden = !user;
    }
    if (user) {
      if (dom.accountAvatar) dom.accountAvatar.textContent = accountInitial(user);
      if (dom.accountName) {
        dom.accountName.textContent = user.display_name || user.email || "已登录";
      }
      if (dom.accountEmail) dom.accountEmail.textContent = user.email || "";
      if (dom.accountCard) dom.accountCard.title = user.email || "";
    }
    if (dom.bannerTokenBtn) {
      dom.bannerTokenBtn.textContent = available ? "登录 / 注册" : "填写 Token";
    }
    if (dom.authBannerText) {
      dom.authBannerText.textContent = available
        ? "登录账号，或填写 JWT、API Key 或静态 AIP_TOKEN 后才能开始对话。"
        : "填写 JWT、API Key 或静态 AIP_TOKEN 后才能开始对话。";
    }
    if (dom.settingsDialog) updateSettingsView();
  }

  function setAuthError(message) {
    if (!dom.authError) return;
    dom.authError.hidden = !message;
    dom.authError.textContent = message || "";
  }

  function updateAuthSubmit() {
    if (!dom.authSubmit) return;
    dom.authSubmit.disabled = state.authBusy;
    if (state.authBusy) {
      dom.authSubmit.textContent = authMode === "register" ? "注册中…" : "登录中…";
    } else {
      dom.authSubmit.textContent = authMode === "register" ? "注册并登录" : "登录";
    }
  }

  function setAuthMode(mode) {
    authMode = mode === "register" ? "register" : "login";
    var register = authMode === "register";

    if (dom.authTabs) {
      for (var i = 0; i < dom.authTabs.length; i += 1) {
        var tab = dom.authTabs[i];
        var active = tab.getAttribute("data-auth-tab") === authMode;
        tab.classList.toggle("is-active", active);
        tab.setAttribute("aria-selected", active ? "true" : "false");
      }
    }
    if (dom.authTitle) dom.authTitle.textContent = register ? "创建账号" : "欢迎回来";
    if (dom.authSubtitle) {
      dom.authSubtitle.textContent = register
        ? "注册后，会话和用量会绑定到你的账号。"
        : "登录后，会话会绑定到你的账号。";
    }
    if (dom.authNameField) dom.authNameField.hidden = !register;
    if (dom.authPassword) {
      dom.authPassword.setAttribute("autocomplete", register ? "new-password" : "current-password");
    }
    if (dom.authSwitchText) {
      dom.authSwitchText.textContent = register ? "已有账号？" : "还没有账号？";
    }
    if (dom.authSwitchBtn) {
      dom.authSwitchBtn.textContent = register ? "去登录" : "立即注册";
    }
    setAuthError("");
    updateAuthSubmit();
  }

  function openAuthDialog(mode) {
    if (!userAuthAvailable()) {
      if (state.config && state.config.auth_required) {
        openSettings(true);
        toast("当前后端未启用账号体系，请使用访问 Token", "warn", 5000);
      } else {
        toast("当前后端未启用用户账号（需要 PostgreSQL + AIP_JWT_SECRET）", "warn", 5000);
      }
      return;
    }
    setAuthMode(mode || "login");
    if (dom.authEmail) {
      dom.authEmail.value = "";
      if (authMode === "login" && authUser()) dom.authEmail.value = authUser().email || "";
    }
    if (dom.authPassword) dom.authPassword.value = "";
    if (dom.authName) dom.authName.value = "";
    if (dom.authDialog && !dom.authDialog.open) dom.authDialog.showModal();
    window.setTimeout(function () {
      if (dom.authEmail) dom.authEmail.focus();
    }, 30);
  }

  function closeAuthDialog() {
    if (dom.authDialog && dom.authDialog.open) dom.authDialog.close();
  }

  // 账号模式下，未登录不允许进入聊天壳：复用登录弹窗，但铺满页面并禁止关闭。
  // 静态 Token 模式不走这里，仍然使用原来的 banner + 设置面板。
  function showAuthGate(mode) {
    if (!userAuthAvailable()) return;
    state.authGate = true;
    document.body.classList.add("is-auth-gate");
    if (dom.authDialog) dom.authDialog.classList.add("is-gate");
    if (dom.settingsDialog && dom.settingsDialog.open) closeSettings();
    openAuthDialog(mode || "login");
  }

  function hideAuthGate() {
    state.authGate = false;
    document.body.classList.remove("is-auth-gate");
    if (dom.authDialog) dom.authDialog.classList.remove("is-gate");
    closeAuthDialog();
  }

  function submitAuth(event) {
    if (event) event.preventDefault();
    if (state.authBusy) return;

    var email = dom.authEmail ? dom.authEmail.value.trim() : "";
    var password = dom.authPassword ? dom.authPassword.value : "";
    var displayName = dom.authName ? dom.authName.value.trim() : "";
    if (!email) {
      setAuthError("请输入邮箱");
      return;
    }
    if (password.length < 8 || password.length > 128) {
      setAuthError("密码长度需要在 8~128 个字符之间");
      return;
    }

    var mode = authMode;
    var payload = { email: email, password: password };
    if (mode === "register" && displayName) payload.display_name = displayName;

    state.authBusy = true;
    setAuthError("");
    updateAuthSubmit();

    fetch(mode === "register" ? "/v1/auth/register" : "/v1/auth/login", {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    })
      .then(function (response) {
        return response
          .json()
          .catch(function () {
            return {};
          })
          .then(function (body) {
            if (!response.ok) {
              var message =
                (body && body.error && body.error.message) ||
                "请求失败（HTTP " + response.status + "）";
              var error = new Error(message);
              error.status = response.status;
              throw error;
            }
            return body;
          });
      })
      .then(function (pair) {
        if (!applyTokenPair(pair)) throw new Error("登录返回的数据不完整");
        stopStreaming();
        if (state.authGate) hideAuthGate();
        else closeAuthDialog();
        reloadSessionScope();
        updateServiceUI();
        toast(
          mode === "register" ? "注册成功，已登录" : "登录成功，已切换到账号会话",
          "success",
          3500
        );
      })
      .catch(function (error) {
        setAuthError(error.message || "登录失败，请稍后重试");
      })
      .then(function () {
        state.authBusy = false;
        updateAuthSubmit();
      });
  }

  function logout() {
    if (!authUser()) return;
    if (!window.confirm("退出登录？本地会话仍会保留在该账号下。")) return;
    stopStreaming();
    clearAuth();
    reloadSessionScope();
    updateServiceUI();
    if (userAuthAvailable()) showAuthGate("login");
    else toast("已退出登录，已切换回本地会话", "success");
  }

  function needsCredentials() {
    if (userAuthAvailable() && !authUser()) return true;
    return Boolean(state.config && state.config.auth_required && !hasCredentials());
  }

  function promptForCredentials() {
    if (userAuthAvailable()) {
      showAuthGate("login");
      toast("请先登录或注册", "warn");
    } else {
      openSettings(true);
      toast("请先填写访问 Token", "warn");
    }
  }

  /* ------------------------------------------------------------------ */
  /* 服务状态 / 配置                                                      */
  /* ------------------------------------------------------------------ */

  function setStatus(kind) {
    if (!dom.statusDot) return;
    dom.statusDot.className = "status-dot";
    if (kind === "online") dom.statusDot.classList.add("is-online");
    else if (kind === "draining") dom.statusDot.classList.add("is-draining");
    else if (kind === "degraded") dom.statusDot.classList.add("is-draining");
    else if (kind === "offline") dom.statusDot.classList.add("is-offline");
    else dom.statusDot.classList.add("is-pending");

    if (dom.livePill) {
      dom.livePill.classList.remove("is-offline", "is-draining");
      if (kind === "offline") dom.livePill.classList.add("is-offline");
      if (kind === "draining") dom.livePill.classList.add("is-draining");
    }

    var labels = {
      online: "服务正常",
      draining: "正在排空",
      degraded: "存储异常",
      offline: "服务离线",
      pending: "正在连接…"
    };
    if (dom.statusText) dom.statusText.textContent = labels[kind] || labels.pending;
    if (dom.liveText) {
      dom.liveText.textContent =
        kind === "online"
          ? "在线"
          : kind === "draining"
            ? "排空"
            : kind === "degraded"
              ? "异常"
              : kind === "offline"
                ? "离线"
                : "连接中";
    }
  }

  function updateServiceUI() {
    var model = (state.config && state.config.model) || (state.health && state.health.model) || "mock";
    if (dom.modelPill) dom.modelPill.textContent = model;
    if (dom.footerModel) dom.footerModel.textContent = model;
    var storage =
      (state.config && state.config.storage) || (state.health && state.health.storage) || "memory";
    if (dom.footerStorage) {
      dom.footerStorage.textContent =
        storage === "postgres" ? "PostgreSQL 持久化" : "服务端内存会话重启即失效";
    }

    if (state.health) {
      var status =
        state.health.status === "draining"
          ? "draining"
          : state.health.storage_ok === false
            ? "degraded"
            : "online";
      setStatus(status);
      if (dom.statusMeta) {
        var streams =
          Number(state.health.active_streams || 0) + "/" + Number(state.health.max_streams || 0);
        dom.statusMeta.textContent =
          (state.health.provider || "model") + " · 活跃流 " + streams;
      }
    } else if (state.healthChecked) {
      setStatus("offline");
      if (dom.statusMeta) dom.statusMeta.textContent = "检查后端是否已启动";
    } else {
      setStatus("pending");
      if (dom.statusMeta) dom.statusMeta.textContent = "/healthz";
    }

    var authRequired = Boolean(state.config && state.config.auth_required);
    if (dom.authBanner) {
      dom.authBanner.hidden = !(authRequired && !hasCredentials());
    }
    if (dom.offlineBanner) {
      dom.offlineBanner.hidden = !(state.healthChecked && !state.health);
    }
    updateAuthUI();
  }

  function updateSettingsView() {
    var authConfig = state.config && state.config.auth;
    var accountMode = Boolean(authConfig && authConfig.user_accounts);
    var user = authUser();
    if (dom.authState) {
      if (!state.config) {
        dom.authState.textContent = "尚未读取到服务配置";
        dom.authState.classList.remove("is-on");
      } else if (accountMode) {
        if (user) {
          dom.authState.textContent =
            "已登录：" + (user.email || "当前账号") + "；账号 Token 优先于下方手动 Token";
          dom.authState.classList.remove("is-on");
        } else if (state.prefs.token) {
          dom.authState.textContent = "未登录；当前使用手动 Token。也可以登录账号。";
          dom.authState.classList.add("is-on");
        } else {
          dom.authState.textContent = "服务已启用账号体系，请登录或注册后对话";
          dom.authState.classList.add("is-on");
        }
      } else if (state.config.auth_required) {
        dom.authState.textContent = state.prefs.token
          ? "服务要求 Token；若收到 401，请检查 Token 是否正确"
          : "服务已启用鉴权，必须填写 Token 才能对话";
        dom.authState.classList.add("is-on");
      } else {
        dom.authState.textContent = "当前服务未启用鉴权（AIP_TOKEN 为空）";
        dom.authState.classList.remove("is-on");
      }
    }
    if (dom.tokenInput) {
      if (accountMode && user) {
        dom.tokenInput.placeholder = "已登录；此处可填 API Key 或静态 AIP_TOKEN";
      } else if (accountMode) {
        dom.tokenInput.placeholder = "可选：API Key / 静态 AIP_TOKEN";
      } else {
        dom.tokenInput.placeholder = state.config && state.config.auth_required
          ? "粘贴 AIP_TOKEN 对应的 Token"
          : "未启用鉴权，可留空";
      }
    }
    if (dom.infoEndpoint) dom.infoEndpoint.textContent = window.location.origin;
    if (dom.infoModel) {
      dom.infoModel.textContent = state.config
        ? (state.config.model || "—") + " · " + (state.config.provider || "—")
        : "—";
    }
    if (dom.infoBackend) {
      dom.infoBackend.textContent = state.config ? state.config.http || "—" : "—";
    }
    if (dom.infoStorage) {
      dom.infoStorage.textContent = state.config
        ? (state.config.storage === "postgres" ? "PostgreSQL" : "内存（重启即丢）")
        : "—";
    }
    if (dom.infoVersion) {
      dom.infoVersion.textContent = state.config ? state.config.version || "—" : "—";
    }
    updateThemeButtons();
  }

  // 页面加载时用 /v1/auth/me 验证本地保存的 access token；过期则自动 refresh 一次。
  function validateSession() {
    if (!state.auth) return Promise.resolve();
    return apiFetch("/v1/auth/me", { headers: { Accept: "application/json" } }, true)
      .then(function (response) {
        if (!response.ok) {
          if (response.status === 401) handleAuthExpired();
          return null;
        }
        return response.json().then(function (body) {
          var user = normalizeUser(body && body.user);
          if (!user || !state.auth) return;
          state.auth.user = user;
          persistAuth();
          updateAuthUI();
          updateSettingsView();
        });
      })
      .catch(function () {
        /* 网络失败不清理本地登录态；下一次请求还会再试 */
      });
  }

  function loadConfig() {
    return fetch("/v1/config", { cache: "no-store", headers: { Accept: "application/json" } })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (config) {
        state.config = config;
        updateServiceUI();
        return config;
      })
      .catch(function () {
        state.config = null;
        updateServiceUI();
        return null;
      });
  }

  function loadHealth() {
    if (state.healthLoading) return Promise.resolve();
    state.healthLoading = true;
    if (dom.statusRetry) dom.statusRetry.classList.add("is-spinning");
    return fetch("/healthz", { cache: "no-store", headers: { Accept: "application/json" } })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (health) {
        state.health = health;
      })
      .catch(function () {
        state.health = null;
      })
      .then(function () {
        state.healthChecked = true;
        state.healthLoading = false;
        if (dom.statusRetry) dom.statusRetry.classList.remove("is-spinning");
        updateServiceUI();
      });
  }

  function retryConnection() {
    state.healthChecked = false;
    loadConfig();
    loadHealth();
  }

  /* ------------------------------------------------------------------ */
  /* SSE / 发送                                                          */
  /* ------------------------------------------------------------------ */

  function ApiError(message, status, type) {
    this.name = "ApiError";
    this.message = message;
    this.status = status;
    this.type = type;
    this.stack = new Error(message).stack;
  }
  ApiError.prototype = Object.create(Error.prototype);
  ApiError.prototype.constructor = ApiError;

  function authHeaders(withContentType) {
    var headers = { Accept: "application/json" };
    if (withContentType) headers["Content-Type"] = "application/json";
    var token = authToken();
    if (token) headers.Authorization = "Bearer " + token;
    return headers;
  }

  var refreshPromise = null;

  // 只有登录/注册/刷新本身不应该再触发“401 后刷新”，其余接口（含 /v1/auth/me）
  // 都可以在 access token 过期时自动换一次。
  function isPublicAuthPath(url) {
    return /\/v1\/auth\/(login|register|refresh)(?:\?|$)/.test(url);
  }

  function refreshSession() {
    if (!state.auth || !state.auth.refreshToken) {
      return Promise.reject(new Error("没有可用的 refresh token"));
    }
    if (refreshPromise) return refreshPromise;
    var refreshToken = state.auth.refreshToken;
    refreshPromise = fetch("/v1/auth/refresh", {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      body: JSON.stringify({ refresh_token: refreshToken })
    })
      .then(function (response) {
        if (!response.ok) throw new Error("刷新登录失败（HTTP " + response.status + "）");
        return response.json();
      })
      .then(function (pair) {
        if (!applyTokenPair(pair)) throw new Error("刷新登录返回无效");
        return true;
      })
      .catch(function (error) {
        // 不在这里清账号：调用方拿到 401 后再统一清理并切换本地会话作用域，
        // 避免“state.auth 已空、但页面还停留在上一个账号会话”的中间态。
        throw error;
      })
      .then(
        function (value) {
          refreshPromise = null;
          return value;
        },
        function (error) {
          refreshPromise = null;
          throw error;
        }
      );
    return refreshPromise;
  }

  // 带 Bearer 的 fetch：401 时用 refresh token 换一次新 access token，再重试一次。
  // 流式请求也走这里；401 发生在响应头阶段，重试不会重复已经读到的 SSE 内容。
  function apiFetch(url, init, retry) {
    var options = init ? Object.assign({}, init) : {};
    options.headers = Object.assign({}, options.headers || {}, authHeaders(false));
    return fetch(url, options).then(function (response) {
      var canRetry =
        retry !== false &&
        response.status === 401 &&
        !isPublicAuthPath(url) &&
        state.auth &&
        state.auth.refreshToken;
      if (!canRetry) return response;
      return refreshSession()
        .then(function () {
          return apiFetch(url, init, false);
        })
        .catch(function () {
          return response;
        });
    });
  }

  function handleAuthExpired() {
    if (!state.auth) return;
    stopStreaming();
    clearAuth();
    reloadSessionScope();
    updateServiceUI();
    if (userAuthAvailable()) showAuthGate("login");
    else toast("登录已过期，请重新登录", "warn", 5000);
  }

  function readApiError(response) {
    return response
      .json()
      .then(function (body) {
        return {
          type: (body && body.error && body.error.type) || "http_error",
          message:
            (body && body.error && body.error.message) || "请求失败（HTTP " + response.status + "）"
        };
      })
      .catch(function () {
        return { type: "http_error", message: "请求失败（HTTP " + response.status + "）" };
      });
  }

  function handleSseFrame(frame, assistant) {
    if (!frame || !frame.trim()) return;
    var eventName = "message";
    var dataLines = [];
    var lines = frame.split(/\r?\n/);
    for (var i = 0; i < lines.length; i += 1) {
      var line = lines[i];
      if (!line || line.charAt(0) === ":") continue;
      var colon = line.indexOf(":");
      var field = colon === -1 ? line : line.slice(0, colon);
      var value = colon === -1 ? "" : line.slice(colon + 1);
      if (value.charAt(0) === " ") value = value.slice(1);
      if (field === "event") eventName = value;
      else if (field === "data") dataLines.push(value);
    }
    if (!dataLines.length) return;
    var payloadText = dataLines.join("\n");
    if (payloadText === "[DONE]") {
      if (assistant.state === "streaming") assistant.state = "done";
      return;
    }
    var payload;
    try {
      payload = JSON.parse(payloadText);
    } catch (err) {
      return;
    }
    if (eventName === "error" || (payload && payload.error)) {
      assistant.state = "error";
      assistant.error =
        (payload && payload.error && payload.error.message) || "模型服务返回错误";
      return;
    }
    var choice = payload && payload.choices && payload.choices[0];
    var delta = choice && choice.delta;
    if (delta && typeof delta.content === "string" && delta.content) {
      assistant.content += delta.content;
      scheduleStreamingRender(assistant);
    }
    if (payload && payload.usage) assistant.usage = payload.usage;
    if (choice && choice.finish_reason) assistant.finishReason = choice.finish_reason;
  }

  function consumeSseBuffer(buffer, assistant) {
    var match;
    while ((match = /\r?\n\r?\n/.exec(buffer))) {
      var frame = buffer.slice(0, match.index);
      buffer = buffer.slice(match.index + match[0].length);
      handleSseFrame(frame, assistant);
    }
    return buffer;
  }

  function sendMessage(rawText, options) {
    var opts = options || {};
    var text = String(rawText == null ? "" : rawText).trim();
    if (!text || state.streaming) return;

    var maxBytes = (state.config && state.config.max_message_bytes) || DEFAULT_MAX_BYTES;
    if (byteLength(text) > maxBytes) {
      toast("消息超过 " + Math.round(maxBytes / 1024) + "KB 限制", "error");
      return;
    }
    if (needsCredentials()) {
      if (dom.input) dom.input.value = text;
      autosizeInput();
      updateComposerState();
      promptForCredentials();
      return;
    }

    var scopeVersion = state.scopeVersion;
    var session = currentSession();
    if (!session) {
      session = createSession(text);
      state.sessions.unshift(session);
      state.currentId = session.id;
      persistCurrent();
    }

    var appendUser = opts.appendUser !== false;
    if (appendUser) {
      var userCount = session.messages.filter(function (message) {
        return message.role === "user";
      }).length;
      session.messages.push({
        id: uid("m"),
        role: "user",
        content: text,
        createdAt: Date.now(),
        model: ""
      });
      if (userCount === 0) session.title = makeTitle(text);
    }

    var assistant = {
      id: uid("m"),
      role: "assistant",
      content: "",
      createdAt: Date.now(),
      state: "streaming",
      model: (state.config && state.config.model) || (state.health && state.health.model) || "",
      error: "",
      usage: null,
      elapsedMs: 0
    };
    session.messages.push(assistant);
    trimSession(session);
    session.updatedAt = Date.now();

    state.streaming = true;
    state.streamingMessageId = assistant.id;
    state.abortController = new AbortController();
    state.scrollPinned = true;
    persistSessions();
    renderSessionList();
    renderChat();
    scrollToBottom();
    updateComposerState();

    var startedAt = window.performance && performance.now ? performance.now() : Date.now();
    var body = { message: text, stream: true };
    if (session.serverId) body.session_id = session.serverId;

    apiFetch("/v1/chat/completions", {
      method: "POST",
      headers: authHeaders(true),
      body: JSON.stringify(body),
      signal: state.abortController.signal
    })
      .then(function (response) {
        if (!response.ok) {
          return readApiError(response).then(function (info) {
            if (response.status === 401) {
              if (state.auth) {
                handleAuthExpired();
                openAuthDialog("login");
              } else if (userAuthAvailable()) {
                openAuthDialog("login");
              } else {
                state.config = Object.assign({}, state.config, { auth_required: true });
                updateServiceUI();
                openSettings(true);
              }
            }
            throw new ApiError(info.message, response.status, info.type);
          });
        }
        var sessionId = response.headers.get("X-Session-Id");
        if (sessionId) {
          session.serverId = sessionId;
          persistSessions();
        }
        if (!response.body || !response.body.getReader) {
          throw new Error("当前浏览器不支持流式响应");
        }
        var reader = response.body.getReader();
        var decoder = new TextDecoder("utf-8");
        var buffer = "";
        function pump() {
          return reader.read().then(function (result) {
            if (result.done) {
              buffer += decoder.decode();
              if (buffer.trim()) handleSseFrame(buffer, assistant);
              return undefined;
            }
            buffer += decoder.decode(result.value, { stream: true });
            buffer = consumeSseBuffer(buffer, assistant);
            return pump();
          });
        }
        return pump();
      })
      .catch(function (error) {
        if (error && (error.name === "AbortError" || error.code === 20)) {
          if (assistant.content) {
            assistant.state = "stopped";
          } else {
            assistant.state = "error";
            assistant.error = "已停止生成";
          }
        } else {
          assistant.state = "error";
          assistant.error =
            error instanceof ApiError
              ? error.message
              : "连接失败：" + ((error && error.message) || "网络异常");
          toast(assistant.error, "error");
        }
      })
      .then(function () {
        if (scopeVersion !== state.scopeVersion) {
          // 登录 / 退出 / 登录过期导致会话作用域切换：不要把这轮消息写进新账号的缓存。
          state.streaming = false;
          state.streamingMessageId = null;
          state.abortController = null;
          updateComposerState();
          return;
        }
        var endAt = window.performance && performance.now ? performance.now() : Date.now();
        assistant.elapsedMs = Math.max(1, Math.round(endAt - startedAt));
        if (assistant.state === "streaming") assistant.state = "done";
        state.streaming = false;
        state.streamingMessageId = null;
        state.abortController = null;
        session.updatedAt = Date.now();
        trimSession(session);
        persistSessions();

        var sameSession = state.currentId === session.id;
        var previousTop = dom.chatScroll ? dom.chatScroll.scrollTop : 0;
        renderSessionList();
        if (sameSession) {
          renderChat();
          if (state.scrollPinned) scrollToBottom();
          else if (dom.chatScroll) dom.chatScroll.scrollTop = previousTop;
        }
        updateComposerState();
        if (sameSession && dom.input) dom.input.focus();
      });
  }

  function stopStreaming() {
    if (state.abortController) {
      try {
        state.abortController.abort();
      } catch (err) {
        /* 忽略 */
      }
    }
  }

  function retryMessage(session, message) {
    if (state.streaming) return;
    var index = session.messages.indexOf(message);
    if (index < 0) return;
    var userMessage = null;
    for (var i = index; i >= 0; i -= 1) {
      if (session.messages[i].role === "user") {
        userMessage = session.messages[i];
        break;
      }
    }
    if (!userMessage) {
      toast("找不到对应的用户消息，无法重新生成", "error");
      return;
    }
    session.messages = session.messages.slice(0, i + 1);
    session.updatedAt = Date.now();
    persistSessions();
    renderChat();
    sendMessage(userMessage.content, { appendUser: false });
  }

  /* ------------------------------------------------------------------ */
  /* 输入框                                                              */
  /* ------------------------------------------------------------------ */

  function autosizeInput() {
    if (!dom.input) return;
    dom.input.style.height = "auto";
    dom.input.style.height = Math.min(dom.input.scrollHeight, 200) + "px";
  }

  function updateComposerState() {
    if (!dom.sendBtn || !dom.input) return;
    var text = dom.input.value;
    var maxBytes = (state.config && state.config.max_message_bytes) || DEFAULT_MAX_BYTES;
    var bytes = byteLength(text);
    var over = bytes > maxBytes;

    if (dom.charCount) {
      var show = bytes > 0 && bytes >= Math.min(2048, maxBytes * 0.5);
      dom.charCount.hidden = !show;
      dom.charCount.textContent = formatBytes(bytes) + " / " + Math.round(maxBytes / 1024) + " KB";
      dom.charCount.classList.toggle("is-limit", over);
    }

    if (state.streaming) {
      dom.sendBtn.disabled = false;
      dom.sendBtn.classList.add("is-stop");
      dom.sendBtn.setAttribute("aria-label", "停止生成");
    } else {
      dom.sendBtn.disabled = !text.trim() || over;
      dom.sendBtn.classList.remove("is-stop");
      dom.sendBtn.setAttribute("aria-label", "发送消息");
    }
  }

  function submitComposer() {
    if (state.streaming) {
      stopStreaming();
      return;
    }
    var text = dom.input.value;
    if (!text.trim()) return;
    var maxBytes = (state.config && state.config.max_message_bytes) || DEFAULT_MAX_BYTES;
    if (byteLength(text) > maxBytes) {
      toast("消息超过 " + Math.round(maxBytes / 1024) + "KB 限制", "error");
      return;
    }
    if (needsCredentials()) {
      promptForCredentials();
      return;
    }
    dom.input.value = "";
    autosizeInput();
    updateComposerState();
    sendMessage(text);
  }

  function beginPrompt(prompt) {
    if (state.streaming) {
      toast("正在生成，请先停止或等待完成", "warn");
      return;
    }
    if (needsCredentials()) {
      dom.input.value = prompt;
      autosizeInput();
      updateComposerState();
      promptForCredentials();
      return;
    }
    sendMessage(prompt);
  }

  /* ------------------------------------------------------------------ */
  /* 设置 / 对话框                                                       */
  /* ------------------------------------------------------------------ */

  function openSettings(focusToken) {
    if (!dom.settingsDialog) return;
    if (dom.tokenInput) {
      dom.tokenInput.value = state.prefs.token;
      dom.tokenInput.type = "password";
    }
    if (dom.toggleTokenBtn) dom.toggleTokenBtn.textContent = "显示";
    updateSettingsView();
    if (!dom.settingsDialog.open) {
      dom.settingsDialog.showModal();
    }
    if (focusToken && dom.tokenInput) {
      window.setTimeout(function () {
        dom.tokenInput.focus();
        dom.tokenInput.select();
      }, 30);
    }
  }

  function closeSettings() {
    if (dom.settingsDialog && dom.settingsDialog.open) dom.settingsDialog.close();
  }

  function saveSettings() {
    var token = dom.tokenInput ? dom.tokenInput.value.trim() : "";
    state.prefs.token = token;
    persistPrefs();
    updateServiceUI();
    closeSettings();
    if (token) toast("Token 已保存", "success");
    else toast("已清除 Token", "success");
    if (state.config && state.config.auth_required && !hasCredentials()) {
      toast(
        userAuthAvailable()
          ? "服务端启用了鉴权；请登录账号或填写 Token，否则无法对话"
          : "服务端启用了鉴权，未填写 Token 将无法对话",
        "warn"
      );
    }
  }

  function deleteSession(id) {
    var session = null;
    for (var i = 0; i < state.sessions.length; i += 1) {
      if (state.sessions[i].id === id) {
        session = state.sessions[i];
        break;
      }
    }
    if (!session) return;
    if (!window.confirm('删除会话「' + session.title + '」？此操作只影响本机记录。')) return;
    if (state.currentId === id) {
      stopStreaming();
      state.currentId = null;
      persistCurrent();
    }
    state.sessions = state.sessions.filter(function (item) {
      return item.id !== id;
    });
    persistSessions();
    renderSessionList();
    renderChat();
    toast("会话已删除", "success");
  }

  function clearAllSessions() {
    if (!state.sessions.length) {
      toast("没有可清空的本地会话");
      return;
    }
    if (!window.confirm("清空全部本地会话？只会删除本机记录，服务端内存会话不受影响。")) return;
    stopStreaming();
    state.sessions = [];
    state.currentId = null;
    persistCurrent();
    persistSessions();
    renderSessionList();
    renderChat();
    updateComposerState();
    toast("已清空本地会话", "success");
  }

  function newChat() {
    stopStreaming();
    state.currentId = null;
    persistCurrent();
    renderSessionList();
    renderChat();
    updateComposerState();
    if (dom.chatScroll) dom.chatScroll.scrollTop = 0;
    if (dom.input) dom.input.focus();
    closeSidebar();
  }

  function selectSession(id) {
    var target = null;
    for (var i = 0; i < state.sessions.length; i += 1) {
      if (state.sessions[i].id === id) {
        target = state.sessions[i];
        break;
      }
    }
    if (!target) return;
    if (state.streaming) stopStreaming();
    state.currentId = id;
    persistCurrent();
    renderSessionList();
    renderChat();
    scrollToBottom();
    updateComposerState();
    closeSidebar();
    if (!target.messages.length && target.serverId) {
      syncHistory(target);
    }
  }

  function syncHistory(session) {
    apiFetch("/v1/sessions/" + encodeURIComponent(session.serverId) + "/messages", {
      headers: authHeaders(false)
    })
      .then(function (response) {
        if (!response.ok) {
          if (response.status === 401 && state.auth) handleAuthExpired();
          return null;
        }
        return response.json();
      })
      .then(function (data) {
        if (!data || !Array.isArray(data.messages) || !data.messages.length) return;
        if (state.currentId !== session.id || session.messages.length) return;
        session.messages = data.messages
          .filter(function (message) {
            return message && (message.role === "user" || message.role === "assistant");
          })
          .map(function (message) {
            return {
              id: uid("m"),
              role: message.role,
              content: typeof message.content === "string" ? message.content : "",
              createdAt: Date.now(),
              state: message.role === "assistant" ? "done" : undefined,
              model: (state.config && state.config.model) || ""
            };
          });
        renderChat();
        scrollToBottom();
      })
      .catch(function () {
        /* 服务端历史不可用时不打断本地会话 */
      });
  }

  /* ------------------------------------------------------------------ */
  /* 侧边栏                                                              */
  /* ------------------------------------------------------------------ */

  function openSidebar() {
    if (!dom.sidebar) return;
    dom.sidebar.classList.add("is-open");
    if (dom.sidebarScrim) dom.sidebarScrim.hidden = false;
  }

  function closeSidebar() {
    if (!dom.sidebar) return;
    dom.sidebar.classList.remove("is-open");
    if (dom.sidebarScrim) dom.sidebarScrim.hidden = true;
  }

  /* ------------------------------------------------------------------ */
  /* 事件绑定                                                            */
  /* ------------------------------------------------------------------ */

  function bindEvents() {
    if (dom.newChatBtn) dom.newChatBtn.addEventListener("click", newChat);
    if (dom.clearSessionsBtn) dom.clearSessionsBtn.addEventListener("click", clearAllSessions);
    if (dom.settingsBtn) dom.settingsBtn.addEventListener("click", function () { openSettings(false); });
    if (dom.settingsBtnTop) dom.settingsBtnTop.addEventListener("click", function () { openSettings(false); });
    if (dom.themeBtn) dom.themeBtn.addEventListener("click", toggleTheme);
    if (dom.bannerTokenBtn) {
      dom.bannerTokenBtn.addEventListener("click", function () {
        if (userAuthAvailable()) showAuthGate("login");
        else openSettings(true);
      });
    }
    if (dom.accountLoginBtn) {
      dom.accountLoginBtn.addEventListener("click", function () { showAuthGate("login"); });
    }
    if (dom.accountLogoutBtn) dom.accountLogoutBtn.addEventListener("click", logout);
    if (dom.authClose) {
      dom.authClose.addEventListener("click", function () {
        if (!state.authGate) closeAuthDialog();
      });
    }
    if (dom.authForm) dom.authForm.addEventListener("submit", submitAuth);
    if (dom.authSwitchBtn) {
      dom.authSwitchBtn.addEventListener("click", function () {
        setAuthMode(authMode === "register" ? "login" : "register");
        if (dom.authEmail) dom.authEmail.focus();
      });
    }
    if (dom.authTokenBtn) {
      dom.authTokenBtn.addEventListener("click", function () {
        if (state.authGate) return;
        closeAuthDialog();
        openSettings(true);
      });
    }
    if (dom.authTabs) {
      for (var tabIndex = 0; tabIndex < dom.authTabs.length; tabIndex += 1) {
        dom.authTabs[tabIndex].addEventListener("click", function (event) {
          var mode = event.currentTarget.getAttribute("data-auth-tab");
          if (mode) setAuthMode(mode);
        });
      }
    }
    if (dom.authDialog) {
      dom.authDialog.addEventListener("click", function (event) {
        var rect = dom.authDialog.getBoundingClientRect();
        var inside =
          event.clientX >= rect.left &&
          event.clientX <= rect.right &&
          event.clientY >= rect.top &&
          event.clientY <= rect.bottom;
        if (!inside && !state.authGate) closeAuthDialog();
      });
      dom.authDialog.addEventListener("cancel", function (event) {
        if (state.authGate) event.preventDefault();
      });
      dom.authDialog.addEventListener("close", function () {
        state.authBusy = false;
        updateAuthSubmit();
      });
    }
    if (dom.statusRetry) dom.statusRetry.addEventListener("click", retryConnection);
    if (dom.offlineRetryBtn) dom.offlineRetryBtn.addEventListener("click", retryConnection);
    if (dom.scrollBottomBtn) dom.scrollBottomBtn.addEventListener("click", scrollToBottom);
    if (dom.chatScroll) dom.chatScroll.addEventListener("scroll", onChatScroll, { passive: true });

    if (dom.menuBtn) dom.menuBtn.addEventListener("click", openSidebar);
    if (dom.sidebarClose) dom.sidebarClose.addEventListener("click", closeSidebar);
    if (dom.sidebarScrim) dom.sidebarScrim.addEventListener("click", closeSidebar);

    if (dom.sessionList) {
      dom.sessionList.addEventListener("click", function (event) {
        var deleteButton = event.target.closest("[data-delete]");
        if (deleteButton) {
          event.preventDefault();
          deleteSession(deleteButton.getAttribute("data-delete"));
          return;
        }
        var openButton = event.target.closest("[data-open]");
        if (openButton) {
          event.preventDefault();
          selectSession(openButton.getAttribute("data-open"));
        }
      });
    }

    if (dom.suggestions) {
      dom.suggestions.addEventListener("click", function (event) {
        var button = event.target.closest("[data-prompt]");
        if (button) beginPrompt(button.getAttribute("data-prompt"));
      });
    }

    if (dom.messages) {
      dom.messages.addEventListener("click", function (event) {
        var button = event.target.closest("[data-action]");
        if (!button) return;
        var action = button.getAttribute("data-action");
        if (action === "copy-code") {
          var block = button.closest(".code-block");
          var code = block && block.querySelector("pre code");
          if (code) copyText(code.textContent);
          return;
        }
        var article = button.closest(".message");
        var session = currentSession();
        if (!article || !session) return;
        var message = null;
        for (var i = 0; i < session.messages.length; i += 1) {
          if (session.messages[i].id === article.dataset.id) {
            message = session.messages[i];
            break;
          }
        }
        if (!message) return;
        if (action === "copy-message") copyText(message.content);
        else if (action === "retry") retryMessage(session, message);
      });
    }

    if (dom.input) {
      dom.input.addEventListener("input", function () {
        autosizeInput();
        updateComposerState();
      });
      dom.input.addEventListener("compositionstart", function () {
        state.composing = true;
      });
      dom.input.addEventListener("compositionend", function () {
        state.composing = false;
      });
      dom.input.addEventListener("keydown", function (event) {
        if (event.key !== "Enter" || event.shiftKey) return;
        if (event.isComposing || state.composing || event.keyCode === 229) return;
        event.preventDefault();
        if (!state.streaming) submitComposer();
      });
    }
    if (dom.sendBtn) dom.sendBtn.addEventListener("click", submitComposer);

    if (dom.settingsClose) dom.settingsClose.addEventListener("click", closeSettings);
    if (dom.settingsCancel) dom.settingsCancel.addEventListener("click", closeSettings);
    if (dom.settingsSave) dom.settingsSave.addEventListener("click", saveSettings);
    if (dom.settingsClearBtn) {
      dom.settingsClearBtn.addEventListener("click", function () {
        clearAllSessions();
        closeSettings();
      });
    }
    if (dom.toggleTokenBtn) {
      dom.toggleTokenBtn.addEventListener("click", function () {
        if (!dom.tokenInput) return;
        var show = dom.tokenInput.type === "password";
        dom.tokenInput.type = show ? "text" : "password";
        dom.toggleTokenBtn.textContent = show ? "隐藏" : "显示";
      });
    }
    if (dom.tokenInput) {
      dom.tokenInput.addEventListener("keydown", function (event) {
        if (event.key === "Enter") {
          event.preventDefault();
          saveSettings();
        }
      });
    }
    if (dom.themeSeg) {
      dom.themeSeg.addEventListener("click", function (event) {
        var button = event.target.closest("[data-theme-choice]");
        if (button) setTheme(button.getAttribute("data-theme-choice"));
      });
    }
    if (dom.settingsDialog) {
      dom.settingsDialog.addEventListener("click", function (event) {
        var rect = dom.settingsDialog.getBoundingClientRect();
        var inside =
          event.clientX >= rect.left &&
          event.clientX <= rect.right &&
          event.clientY >= rect.top &&
          event.clientY <= rect.bottom;
        if (!inside) closeSettings();
      });
    }

    document.addEventListener("keydown", function (event) {
      if ((event.metaKey || event.ctrlKey) && !event.shiftKey && event.key.toLowerCase() === "k") {
        event.preventDefault();
        newChat();
        return;
      }
      if (event.key === "Escape" && dom.sidebar && dom.sidebar.classList.contains("is-open")) {
        closeSidebar();
      }
    });

    try {
      var media = window.matchMedia("(prefers-color-scheme: light)");
      if (media.addEventListener) {
        media.addEventListener("change", function () {
          if (state.prefs.theme === "system") applyTheme();
        });
      }
    } catch (err) {
      /* 老浏览器没有 matchMedia 事件，忽略 */
    }
  }

  /* ------------------------------------------------------------------ */
  /* 初始化                                                              */
  /* ------------------------------------------------------------------ */

  function cacheDom() {
    dom.sidebar = $("#sidebar");
    dom.sidebarScrim = $("#sidebarScrim");
    dom.sidebarClose = $("#sidebarClose");
    dom.menuBtn = $("#menuBtn");
    dom.newChatBtn = $("#newChatBtn");
    dom.clearSessionsBtn = $("#clearSessionsBtn");
    dom.settingsBtn = $("#settingsBtn");
    dom.settingsBtnTop = $("#settingsBtnTop");
    dom.themeBtn = $("#themeBtn");
    dom.statusCard = $("#statusCard");
    dom.statusDot = $("#statusDot");
    dom.statusText = $("#statusText");
    dom.statusMeta = $("#statusMeta");
    dom.statusRetry = $("#statusRetry");
    dom.livePill = $("#livePill");
    dom.liveText = $("#liveText");
    dom.modelPill = $("#modelPill");
    dom.footerModel = $("#footerModel");
    dom.footerStorage = $("#footerStorage");
    dom.authBanner = $("#authBanner");
    dom.authBannerText = $("#authBannerText");
    dom.bannerTokenBtn = $("#bannerTokenBtn");
    dom.accountLoginBtn = $("#accountLoginBtn");
    dom.accountCard = $("#accountCard");
    dom.accountAvatar = $("#accountAvatar");
    dom.accountName = $("#accountName");
    dom.accountEmail = $("#accountEmail");
    dom.accountLogoutBtn = $("#accountLogoutBtn");
    dom.authDialog = $("#authDialog");
    dom.authClose = $("#authClose");
    dom.authForm = $("#authForm");
    dom.authTabs = document.querySelectorAll("[data-auth-tab]");
    dom.authTitle = $("#authTitle");
    dom.authSubtitle = $("#authSubtitle");
    dom.authNameField = $("#authNameField");
    dom.authEmail = $("#authEmail");
    dom.authPassword = $("#authPassword");
    dom.authName = $("#authName");
    dom.authError = $("#authError");
    dom.authSubmit = $("#authSubmit");
    dom.authSwitchText = $("#authSwitchText");
    dom.authSwitchBtn = $("#authSwitchBtn");
    dom.authTokenBtn = $("#authTokenBtn");
    dom.offlineBanner = $("#offlineBanner");
    dom.offlineRetryBtn = $("#offlineRetryBtn");
    dom.chatScroll = $("#chatScroll");
    dom.welcome = $("#welcome");
    dom.messages = $("#messages");
    dom.suggestions = $("#suggestions");
    dom.scrollBottomBtn = $("#scrollBottomBtn");
    dom.topbar = $("#topbar");
    dom.chatTitle = $("#chatTitle");
    dom.input = $("#input");
    dom.sendBtn = $("#sendBtn");
    dom.charCount = $("#charCount");
    dom.sessionList = $("#sessionList");
    dom.toastStack = $("#toastStack");
    dom.settingsDialog = $("#settingsDialog");
    dom.settingsClose = $("#settingsClose");
    dom.settingsCancel = $("#settingsCancel");
    dom.settingsSave = $("#settingsSave");
    dom.settingsClearBtn = $("#settingsClearBtn");
    dom.tokenInput = $("#tokenInput");
    dom.toggleTokenBtn = $("#toggleTokenBtn");
    dom.authState = $("#authState");
    dom.themeSeg = $("#themeSeg");
    dom.infoEndpoint = $("#infoEndpoint");
    dom.infoModel = $("#infoModel");
    dom.infoBackend = $("#infoBackend");
    dom.infoStorage = $("#infoStorage");
    dom.infoVersion = $("#infoVersion");
  }

  function init() {
    cacheDom();
    state.auth = loadAuth();
    state.prefs = normalizePrefs(readJSON(STORAGE.prefs, null));
    migrateLegacyStorage();
    state.sessions = loadSessions();
    state.currentId = loadCurrentId();

    applyTheme();
    bindEvents();
    renderSessionList();
    renderChat();
    updateComposerState();
    updateServiceUI();

    loadConfig().then(function () {
      updateServiceUI();
      if (userAuthAvailable() && !authUser()) {
        showAuthGate("login");
      } else if (!userAuthAvailable()) {
        hideAuthGate();
      }
      validateSession();
      loadHealth();
    });

    // 本地只有会话 ID、没有消息（例如清过站点数据）时，从服务端补一次历史。
    var restored = currentSession();
    if (restored && restored.serverId && !restored.messages.length) {
      syncHistory(restored);
    }

    if (state.currentId && dom.chatScroll) {
      window.requestAnimationFrame(scrollToBottom);
    }

    window.setInterval(function () {
      if (!document.hidden) loadHealth();
    }, 20000);

    if (dom.input) {
      autosizeInput();
      // 触屏设备加载时不要主动弹软键盘；桌面端聚焦方便直接输入。
      try {
        if (!window.matchMedia("(pointer: coarse)").matches) dom.input.focus();
      } catch (err) {
        dom.input.focus();
      }
    }

    if (!storageOk) {
      window.setTimeout(function () {
        toast("浏览器禁用了本地存储，本次对话不会被保存", "warn", 5000);
      }, 700);
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
