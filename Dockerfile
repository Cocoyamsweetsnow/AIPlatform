# 多阶段构建：最终镜像只带二进制 + 运行时依赖，不含编译器和源码。
#
# 两个交付目标（默认是第一个，也是主线）：
#   docker build -t aiplatform:dev .                                # Drogon 主线
#   docker build --target runtime-httplib -t aiplatform:httplib .   # 回退，不编 Drogon
#
# 回退目标的意义：客户机房 / 国产化 OS 上装不了 Drogon 时，用 cpp-httplib 实现
# （API 契约一致，见 docs/adr/0001-http-framework.md），而且这条路径**不会**触发
# 那 10 分钟的 Drogon 编译。
#
# 分层策略：Drogon 编译很慢（-j2 约 7~10 分钟），单独放在一层；
# 只要 DROGON_VERSION 不变，改业务代码不会触发重编。
#
# 注意：AI 平台的关键是「流式」，不要把 stdout 打成块缓冲——
# 本程序日志用 fflush 即时刷新，可以用 docker logs -f 直接观察。

# ---- 1. 基础工具链（两个后端共用）---------------------------------------
FROM debian:bookworm-slim AS toolchain

ARG BUILD_JOBS=2

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config ca-certificates \
      libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- 2. Drogon 工具链（很慢，单独成层，只有主线需要）---------------------
FROM toolchain AS drogon

ARG DROGON_VERSION=v1.9.14
ARG BUILD_JOBS=2

# Drogon 1.9.14 的 newAsyncStreamResponse 是本项目 SSE 的基础，不要降级。
RUN apt-get update && apt-get install -y --no-install-recommends \
      git libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${DROGON_VERSION}" --recursive \
      https://github.com/drogonframework/drogon /opt/drogon \
    && cmake -S /opt/drogon -B /opt/drogon/build \
         -DCMAKE_BUILD_TYPE=Release \
         -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_CTL=OFF \
         -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF \
         -DBUILD_REDIS=OFF -DUSE_SUBMODULE=ON \
    && cmake --build /opt/drogon/build -j"${BUILD_JOBS}" \
    && cmake --install /opt/drogon/build \
    && ldconfig

# ---- 3. 编应用（两个后端各一层）-----------------------------------------
FROM toolchain AS build-httplib
ARG BUILD_JOBS=2
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY third_party ./third_party
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAIPLATFORM_HTTP=httplib \
    && cmake --build build -j"${BUILD_JOBS}"

FROM drogon AS build-drogon
ARG BUILD_JOBS=2
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY third_party ./third_party
COPY tests ./tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAIPLATFORM_HTTP=drogon \
    && cmake --build build -j"${BUILD_JOBS}"

# ---- 4a. 运行：httplib 回退（不需要 Drogon，镜像更小）---------------------
FROM debian:bookworm-slim AS runtime-httplib

RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home app

COPY --from=build-httplib /src/build/aiplatform /usr/local/bin/aiplatform

USER app
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/aiplatform"]

# ---- 4b. 运行：Drogon 主线（默认目标）------------------------------------
FROM debian:bookworm-slim AS runtime-drogon

RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 libjsoncpp25 libuuid1 libssl3 zlib1g \
      libc-ares2 libbrotli1 \
      ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home app

COPY --from=drogon /usr/local/lib/libdrogon.so* /usr/local/lib/
COPY --from=drogon /usr/local/lib/libtrantor.so* /usr/local/lib/
RUN ldconfig

COPY --from=build-drogon /src/build/aiplatform /usr/local/bin/aiplatform

# 早失败门禁：Drogon 到底链接哪些库取决于构建机装了哪些 -dev 包
# （c-ares / brotli / jsoncpp 的 soname 各发行版不同）。缺任何一个 .so
# 就让镜像构建直接红，而不是部署起来才发现动态库缺失。
RUN ! ldd /usr/local/bin/aiplatform | grep "not found"

USER app
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/aiplatform"]
