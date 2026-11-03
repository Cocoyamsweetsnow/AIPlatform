#!/usr/bin/env python3
"""SSE 并发冒烟压测：只用标准库，不需要装 k6 / wrk。

用法：
    # 1) 真实链路（主线接口）：200 路 POST /v1/chat/completions
    python3 scripts/sse_load.py -n 200 \
        --path /v1/chat/completions --body '{"message":"压测"}' \
        --pid "$(docker inspect -f '{{.State.Pid}}' aip-drogon-test)"

    # 2) 纯连接：500 路保持 5 秒（线程数应基本不涨，测的是「连接不占线程」）
    python3 scripts/sse_load.py -n 500 --path /healthz --hold 5 --pid <pid>

    # 3) POC（旧）：200 路 /sse
    ./build-poc/poc_drogon_sse 8081
    python3 scripts/sse_load.py -n 200 --pid "$(pgrep -f poc_drogon_sse)"

判据见 docs/POC-DROGON.md。退出码：0 = 全部成功，1 = 有失败。

只支持 http://（POC 不需要 TLS）；要测 https 请用 k6。
"""

import argparse
import os
import socket
import statistics
import sys
import threading
import time
import urllib.parse


def percentile(values, p):
    if not values:
        return float("nan")
    ordered = sorted(values)
    k = int(round((p / 100.0) * (len(ordered) - 1)))
    return ordered[max(0, min(len(ordered) - 1, k))]


def run_one(host, port, path, timeout, hold, barrier, results, idx, method="GET", body=None):
    sock = None
    start = None
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        sock.settimeout(timeout)
        barrier.wait(timeout=timeout)

        payload = b""
        lines = [
            "{} {} HTTP/1.1".format(method, path),
            "Host: {}:{}".format(host, port),
            "Accept: text/event-stream",
        ]
        if body is not None:
            payload = body.encode("utf-8")
            lines.append("Content-Type: application/json")
            lines.append("Content-Length: {}".format(len(payload)))
        request = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + payload

        start = time.perf_counter()
        sock.sendall(request)

        buf = b""
        head_end = -1
        status = 0
        ttft = None
        done_seen = False

        while True:
            # /idle 这类流不会自己结束：收到首帧就停，剩下的时间由 --hold 保持连接。
            if hold > 0 and ttft is not None:
                break
            if time.perf_counter() - start > timeout:
                break

            chunk = sock.recv(8192)
            if not chunk:
                break
            buf += chunk

            if head_end < 0:
                pos = buf.find(b"\r\n\r\n")
                if pos >= 0:
                    head_end = pos + 4
                    parts = buf[: buf.find(b"\r\n")].split(b" ")
                    if len(parts) > 1:
                        status = int(parts[1])
                    if status != 200:
                        break

            if ttft is None and head_end >= 0 and len(buf) > head_end:
                ttft = time.perf_counter() - start
            if b"[DONE]" in buf:
                done_seen = True
                break

        if hold > 0 and status == 200 and ttft is not None:
            time.sleep(hold)

        total = time.perf_counter() - start
        if ttft is None:
            ttft = total

        results[idx] = {
            "ok": status == 200 and (done_seen or (hold > 0 and ttft is not None)),
            "status": status,
            "ttft": ttft,
            "total": total,
            "frames": buf.count(b"data: "),
            "error": "" if status == 200 else "HTTP {}".format(status),
        }
    except threading.BrokenBarrierError:
        results[idx] = {"ok": False, "error": "barrier broken"}
        try:
            barrier.abort()
        except Exception:
            pass
    except Exception as exc:  # noqa: BLE001 - 压测脚本，记录即可
        results[idx] = {"ok": False, "error": repr(exc)}
        try:
            barrier.abort()
        except Exception:
            pass
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


class Sampler(threading.Thread):
    """采样服务端 RSS 与线程数，用来对比「每连接一线程」的差距。"""

    def __init__(self, pid, interval=0.02):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self.peak_rss_kb = 0
        self.peak_threads = 0
        self._stop = threading.Event()

    def run(self):
        status_path = "/proc/{}/status".format(self.pid)
        task_path = "/proc/{}/task".format(self.pid)
        while not self._stop.is_set():
            try:
                with open(status_path, "r") as fh:
                    for line in fh:
                        if line.startswith("VmRSS:"):
                            self.peak_rss_kb = max(self.peak_rss_kb, int(line.split()[1]))
                            break
                self.peak_threads = max(self.peak_threads, len(os.listdir(task_path)))
            except OSError:
                break
            self._stop.wait(self.interval)

    def stop(self):
        self._stop.set()


def main():
    parser = argparse.ArgumentParser(description="SSE 并发冒烟压测（仅标准库）")
    parser.add_argument("--base", default="http://127.0.0.1:8081", help="服务地址")
    parser.add_argument("-n", "--connections", type=int, default=100, help="并发连接数")
    parser.add_argument("--path", default="/sse?text=loadtest", help="请求路径")
    parser.add_argument("--timeout", type=float, default=60.0, help="单连接超时（秒）")
    parser.add_argument(
        "--hold",
        type=float,
        default=0.0,
        help="收到首帧后保持连接 N 秒再断开（测 /idle 这类不结束的流）",
    )
    parser.add_argument("--pid", type=int, default=0, help="服务端 pid，用于采样 RSS / 线程数")
    parser.add_argument(
        "--method",
        default=None,
        help="请求方法；默认 GET，给了 --body 时默认 POST",
    )
    parser.add_argument(
        "--body",
        default=None,
        help="请求体（JSON 字符串），用来打 POST /v1/chat/completions",
    )
    args = parser.parse_args()

    method = args.method or ("POST" if args.body is not None else "GET")

    parsed = urllib.parse.urlsplit(args.base)
    if parsed.scheme != "http":
        parser.error("只支持 http://（https 请用 k6）")
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80

    sampler = None
    if args.pid:
        sampler = Sampler(args.pid)
        sampler.start()

    results = [None] * args.connections
    barrier = threading.Barrier(args.connections)
    threads = [
        threading.Thread(
            target=run_one,
            args=(
                host,
                port,
                args.path,
                args.timeout,
                args.hold,
                barrier,
                results,
                i,
                method,
                args.body,
            ),
            daemon=True,
        )
        for i in range(args.connections)
    ]

    wall_start = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=args.timeout + args.hold + 10)
    wall = time.perf_counter() - wall_start

    if sampler:
        sampler.stop()

    ok = [r for r in results if r and r["ok"]]
    bad = [r for r in results if not r or not r["ok"]]
    ttfts = [r["ttft"] * 1000 for r in ok]
    totals = [r["total"] * 1000 for r in ok]
    frames = [r["frames"] for r in ok]

    print("== 结果 ==")
    print("并发连接 : {}".format(args.connections))
    print("成功     : {}   失败: {}".format(len(ok), len(bad)))
    if ttfts:
        print(
            "TTFT     : p50 {:.0f}ms  p95 {:.0f}ms  max {:.0f}ms".format(
                percentile(ttfts, 50), percentile(ttfts, 95), max(ttfts)
            )
        )
        print(
            "总耗时   : p50 {:.0f}ms  p95 {:.0f}ms  max {:.0f}ms".format(
                percentile(totals, 50), percentile(totals, 95), max(totals)
            )
        )
        if frames and max(frames) > 0:
            print("帧数     : 平均 {:.1f} frame/流".format(statistics.mean(frames)))
    print("墙钟     : {:.2f}s".format(wall))
    if sampler:
        print("线程数   : 峰值 {}".format(sampler.peak_threads))
        print("RSS      : 峰值 {:.1f} MB".format(sampler.peak_rss_kb / 1024.0))

    if bad:
        print("\n== 失败样例（最多 5 条） ==")
        for r in bad[:5]:
            print("  {}".format(r))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
