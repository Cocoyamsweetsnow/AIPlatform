# 多阶段构建：最终镜像只带二进制 + libcurl，镜像约 100MB。
# 注意：AI 平台的关键是「流式」，不要把 stdout 打成块缓冲——
# 本程序日志用 fflush 即时刷新，可以用 docker logs -f 直接观察。

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY third_party ./third_party
COPY tests ./tests

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home app

COPY --from=build /src/build/aiplatform /usr/local/bin/aiplatform

USER app
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["/usr/local/bin/aiplatform"]
