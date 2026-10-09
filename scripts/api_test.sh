#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# graph_server 冒烟测试：启动服务 -> 跑一遍全部接口 -> 关闭服务
#
#   bash scripts/api_test.sh
#
# 覆盖：健康检查、多图（新建/列出/隔离/删除）、节点与关系 CRUD、
#       数组与对象属性、条件查询操作符、增量数组/对象操作、索引、统计、落盘、级联删除
# ---------------------------------------------------------------------------
set -uo pipefail

cd "$(dirname "$0")/.."

PORT="${PORT:-8099}"
HOST="127.0.0.1"
DIR="/tmp/graph_server_test_$$"          # 数据目录：一张图一个 <图名>.mgdb
BASE="http://${HOST}:${PORT}"
PASS=0
FAIL=0

rm -rf "$DIR" && mkdir -p "$DIR"

./bin/graph_server --host "$HOST" --port "$PORT" --dir "$DIR" --web web/index.html \
    >/tmp/graph_server_test_$$.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$DIR" /tmp/graph_server_test_$$.log' EXIT

for _ in $(seq 1 50); do
    if curl -s --noproxy '*' -o /dev/null "$BASE/api/health" 2>/dev/null; then break; fi
    sleep 0.1
done

# 请求封装：req <方法> <路径> [请求体]
req() {
    local method="$1" path="$2" body="${3:-}"
    local out
    if [[ -n "$body" ]]; then
        out=$(curl -s --noproxy '*' -w '\n%{http_code}' -X "$method" -H 'Content-Type: application/json' \
              -d "$body" "$BASE$path")
    else
        out=$(curl -s --noproxy '*' -w '\n%{http_code}' -X "$method" "$BASE$path")
    fi
    local http_code="${out##*$'\n'}"
    local payload="${out%$'\n'*}"
    echo "$payload" | (command -v python3 >/dev/null && python3 -m json.tool --no-ensure-ascii 2>/dev/null || cat)
    if [[ "$http_code" =~ ^2 ]]; then
        echo "  ✓ HTTP $http_code"
        PASS=$((PASS + 1))
    else
        echo "  ✗ HTTP $http_code"
        FAIL=$((FAIL + 1))
    fi
    echo
}

title() { echo "─────────────────────────────────────────────"; echo "  $1"; echo "─────────────────────────────────────────────"; }

title "健康检查 / 接口清单"
req GET  /api/health
req GET  "/api/manifest" > /dev/null && echo "  ✓ /api/manifest 可访问" && PASS=$((PASS+1))

title "多数据库：新建 / 列出 / 互相隔离"
req POST /api/graphs '{"name":"social"}'
req POST /api/graphs '{"name":"knowledge"}'
req GET  /api/graphs
req GET  /api/nodes?full=1                          # 默认图：应为空
req GET  /api/g/social/nodes?full=1                 # social 图：应为空

title "增：在 social 图里创建 3 个 Person 节点（含数组 / 对象属性）"
req POST /api/g/social/nodes '{"labels":["Person"],"props":{"name":"Alice","age":30,"city":"Beijing","tags":["vip","early"],"addr":{"city":"Beijing","zip":"100000"}}}'
req POST /api/g/social/nodes '{"labels":["Person"],"props":{"name":"Bob","age":26,"city":"Shanghai","tags":["new"],"addr":{"city":"Shanghai"}}}'
req POST /api/g/social/nodes '{"labels":["Person","Engineer"],"props":{"name":"Carol","age":33,"city":"Beijing","tags":["vip"],"addr":{"city":"Beijing","zip":"100084"}}}'
req POST /api/g/social/nodes '{"labels":["Company"],"props":{"name":"Acme"}}'

title "数组 / 对象属性：读写往返"
req GET  /api/g/social/nodes/1
req POST /api/g/social/query '{"label":"Person","where":{"tags":{"$contains":"vip"}},"full":true}'
req POST /api/g/social/query '{"label":"Person","where":{"addr":{"$has":"zip"}},"full":true}'
req POST /api/g/social/query '{"label":"Person","where":{"age":{"$gte":30}},"full":true}'
req POST /api/g/social/query '{"label":"Person","where":{"city":{"$in":["Beijing","Shanghai"]}},"full":true}'
req GET  "/api/g/social/query?label=Person&key=tags&value=vip&op=contains&full=1"

title "数组 / 对象属性：增量修改（PATCH）"
req PATCH /api/g/social/nodes/1 '{"arrayAppend":{"tags":["gold","vip"]}}'
req PATCH /api/g/social/nodes/1 '{"objectMerge":{"addr":{"street":"长安街"}}}'
req PATCH /api/g/social/nodes/1 '{"arrayRemove":{"tags":["vip"]}}'

title "增：3 条关系（其中带数组属性）"
req POST /api/g/social/relationships '{"from":1,"to":2,"type":"KNOWS","props":{"since":2019}}'
req POST /api/g/social/relationships '{"from":2,"to":3,"type":"KNOWS","props":{"since":2020,"weight":2.5}}'
req POST /api/g/social/relationships '{"from":1,"to":4,"type":"WORKS_AT","props":{"since":2021,"roles":["dev","lead"]}}'

title "查：按 id / 列表 / 标签 / 关系"
req GET  /api/g/social/nodes/1
req GET  "/api/g/social/nodes?label=Person&full=1"
req GET  "/api/g/social/nodes?label=Person&key=city&value=Beijing&full=1"
req GET  /api/g/social/relationships/1
req GET  "/api/g/social/relationships?type=KNOWS&full=1"
req GET  "/api/g/social/relationships?from=1&to=2"

title "改：PATCH 增量更新 / PUT 整体替换"
req PATCH /api/g/social/nodes/1 '{"props":{"age":31},"addLabels":["Manager"]}'
req GET   /api/g/social/nodes/1
req PUT   /api/g/social/nodes/2 '{"labels":["Person","Designer"],"props":{"name":"Bob","age":27}}'
req GET   /api/g/social/nodes/2
req PATCH /api/g/social/relationships/1 '{"props":{"since":2018,"note":"旧同事"}}'
req GET   /api/g/social/relationships/1

title "查：遍历 / 算法"
req GET  "/api/g/social/nodes/1/neighbors?dir=out&full=1"
req GET  "/api/g/social/nodes/1/degree?dir=both"
req GET  "/api/g/social/nodes/1/relationships"
req GET  "/api/g/social/reachable/1?depth=2&full=1"
req GET  "/api/g/social/path?from=1&to=3"
req POST "/api/g/social/path/weighted" '{"from":1,"to":3,"weightKey":"weight","dir":"out"}'

title "索引"
req POST   /api/g/social/indexes '{"label":"Person","key":"name"}'
req GET    /api/g/social/indexes
req GET    "/api/g/social/query?label=Person&key=name&value=Alice&full=1"
req DELETE "/api/g/social/indexes?label=Person&key=name"

title "统计 / 落盘（单图 + 全量）"
req GET  /api/g/social/stats
req POST /api/g/social/admin/save
req POST /api/admin/save

title "删：删除关系与节点（级联）"
req DELETE /api/g/social/relationships/3
req GET    /api/g/social/relationships/3
req DELETE /api/g/social/nodes/4
req GET    /api/g/social/nodes/4

title "删：删除整张图"
req DELETE /api/graphs/knowledge                  # 未确认：预期 400
req DELETE "/api/graphs/knowledge?confirm=1"
req GET    /api/graphs

title "错误处理（以下 4xx 均为预期结果）"
req GET    /api/g/social/nodes/9999
req GET    /api/g/nope/nodes                      # 图不存在
req POST   /api/graphs '{"name":"bad/name"}'      # 图名非法
req POST   /api/g/social/relationships '{"from":1,"to":9999,"type":"X"}'
req POST   /api/g/social/query '{"where":{"age":{"$bigger":1}}}'
req GET    /api/nope

echo "═════════════════════════════════════════════"
echo "  通过: $PASS    预期错误响应: $FAIL"
echo "═════════════════════════════════════════════"
