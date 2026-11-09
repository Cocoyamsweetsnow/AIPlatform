// 邮箱验证 / 重置密码落地页。
// 邮件链接形如 /account.html?mode=verify&token=... 或 ?mode=reset&token=...
(function () {
  "use strict";

  var params = new URLSearchParams(window.location.search);
  var mode = params.get("mode") === "reset" ? "reset" : "verify";
  var token = params.get("token") || "";
  var title = document.getElementById("accountTitle");
  var message = document.getElementById("accountMessage");
  var form = document.getElementById("accountForm");
  var input = document.getElementById("newPassword");
  var submit = document.getElementById("accountSubmit");

  function setMessage(text, kind) {
    message.textContent = text;
    message.className = kind ? "account-" + kind : "";
  }

  function post(path, body) {
    return fetch(path, {
      method: "POST",
      headers: { Accept: "application/json", "Content-Type": "application/json" },
      body: JSON.stringify(body)
    }).then(function (response) {
      return response
        .json()
        .catch(function () { return {}; })
        .then(function (payload) {
          if (!response.ok) {
            throw new Error(
              (payload && payload.error && payload.error.message) ||
                "请求失败（HTTP " + response.status + "）"
            );
          }
          return payload;
        });
    });
  }

  if (!token) {
    setMessage("链接缺少 token，请重新从邮件打开。", "error");
    return;
  }

  if (mode === "verify") {
    title.textContent = "验证邮箱";
    setMessage("正在验证…");
    post("/v1/auth/verify-email", { token: token })
      .then(function (pair) {
        if (pair.access_token) {
          try {
            window.localStorage.setItem(
              "aiplatform.auth.v1",
              JSON.stringify({
                accessToken: pair.access_token,
                refreshToken: pair.refresh_token,
                user: pair.user
              })
            );
          } catch (error) {
            /* 无痕模式下写不了 localStorage，仍然允许手动登录 */
          }
        }
        setMessage("邮箱验证成功，正在进入对话…", "ok");
        window.setTimeout(function () {
          window.location.href = "/";
        }, 900);
      })
      .catch(function (error) {
        setMessage(error.message || "验证失败，请重新打开邮件链接。", "error");
      });
    return;
  }

  title.textContent = "重置密码";
  setMessage("请输入新密码。");
  form.hidden = false;
  form.addEventListener("submit", function (event) {
    event.preventDefault();
    var password = input.value;
    if (password.length < 8 || password.length > 128) {
      setMessage("密码长度需要在 8~128 个字符之间。", "error");
      return;
    }
    submit.disabled = true;
    setMessage("正在提交…");
    post("/v1/auth/password/reset", { token: token, new_password: password })
      .then(function () {
        form.hidden = true;
        setMessage("密码已重置，请返回对话页重新登录。", "ok");
      })
      .catch(function (error) {
        setMessage(error.message || "重置失败，请重新打开邮件链接。", "error");
      })
      .then(function () {
        submit.disabled = false;
      });
  });
})();
