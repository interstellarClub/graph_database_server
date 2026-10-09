#!/usr/bin/env bash
# 端到端冒烟测试：编译 -> 启动桥接 -> GET/POST 验证 -> 关闭
# 用法: bash scripts/smoke_test.sh
set -euo pipefail

cd "$(dirname "$0")/.."

PORT=${PORT:-18099}
HOST=127.0.0.1
BASE="http://$HOST:$PORT"

echo "== 编译 =="
make all >/dev/null

echo "== 启动桥接服务 (桥接 calc_worker, 端口 $PORT) =="
./bin/cpp_bridge --port "$PORT" --cmd ./bin/calc_worker --cwd . --quiet >/tmp/cpp_bridge_smoke.log 2>&1 &
BRIDGE_PID=$!
trap 'kill $BRIDGE_PID 2>/dev/null || true' EXIT

for _ in $(seq 1 40); do
    if curl -sf "$BASE/health" >/dev/null 2>&1; then break; fi
    sleep 0.1
done

echo "== 1) GET /health =="
curl -s "$BASE/health"; echo

echo "== 2) POST /invoke  {\"op\":\"add\",\"a\":3,\"b\":4} =="
curl -s -X POST "$BASE/invoke" -H 'Content-Type: application/json' \
     -d '{"op":"add","a":3,"b":4}'; echo

echo "== 3) GET /invoke?input={\"op\":\"mul\",\"a\":6,\"b\":7} =="
curl -s -G "$BASE/invoke" --data-urlencode 'input={"op":"mul","a":6,"b":7}'; echo

echo "== 4) GET /invoke?op=sub&a=10&b=4 (键值自动组装) =="
curl -s "$BASE/invoke?op=sub&a=10&b=4"; echo

echo "== 5) 错误分支: 除以 0 =="
curl -s -X POST "$BASE/invoke" -d '{"op":"div","a":1,"b":0}'; echo

echo "== 6) 错误分支: 非法 JSON =="
curl -s -X POST "$BASE/invoke" -d '{oops'; echo

echo "== 7) 中文 / emoji =="
curl -s -X POST "$BASE/invoke" -H 'Content-Type: application/json' \
     -d '{"op":"add","a":1,"b":2,"note":"天津😀"}'; echo

echo "== 完成 =="
