#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# 边权（weight）功能冒烟测试
#
#   用法： bash scripts/weight_test.sh [端口]
#
# 覆盖：创建带权边 / 默认权重 / 单条设权 / 批量设权 / 边权统计 /
#       区间过滤 / 带权最短路 / 非法值校验 / 负权告警 / 重启后持久化 /
#       旧格式(v2, weight 存在 props 里)自动迁移
# -----------------------------------------------------------------------------
set -u

PORT="${1:-8231}"
BASE="http://127.0.0.1:${PORT}"
DIR="/tmp/graph_server_weight_test"
BIN="$(cd "$(dirname "$0")/.." && pwd)/bin/graph_server"
JSON=/tmp/graph_server_weight_test_last.json

PASS=0
FAIL=0

# --dir 是数据目录，目录下每个 *.mgdb 是一张图；--graph 指定默认图
start_server() { "$BIN" --port "${PORT}" --dir "$DIR" --graph default >/dev/null 2>&1 & sleep 1.2; }
stop_server()  { pkill -INT -f "bin/graph_server --port ${PORT}" 2>/dev/null; sleep 0.6; }

title() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }

# req <说明> <方法> <路径> [请求体]
req() {
    local desc="$1" method="$2" path="$3" body="${4:-}"
    local out code ok
    if [ -n "$body" ]; then
        out=$(curl -s -w '\n%{http_code}' -X "$method" -H 'Content-Type: application/json' \
              -d "$body" "${BASE}${path}")
    else
        out=$(curl -s -w '\n%{http_code}' -X "$method" "${BASE}${path}")
    fi
    code=$(printf '%s' "$out" | tail -n1)
    printf '%s' "$out" | sed '$d' > "$JSON"
    ok=$(python3 -c "import json;print(json.load(open('$JSON')).get('ok'))" 2>/dev/null)

    printf '%-44s' "$desc"
    if [ "$ok" = "True" ]; then
        printf '\033[32mOK  %s\033[0m\n' "$code"
        PASS=$((PASS + 1))
    else
        printf '\033[33m%s\033[0m\n' "$code"
        FAIL=$((FAIL + 1))
    fi
    python3 -c "
import json
d = json.load(open('$JSON'))
if d.get('ok'):
    print('   ', json.dumps(d.get('data'), ensure_ascii=False))
else:
    print('   ', d.get('error', {}).get('message', ''))
" 2>/dev/null
}

# expect_fail：预期返回 4xx 且 ok=false
expect_fail() {
    local desc="$1" method="$2" path="$3" body="${4:-}"
    local out code ok
    if [ -n "$body" ]; then
        out=$(curl -s -w '\n%{http_code}' -X "$method" -H 'Content-Type: application/json' \
              -d "$body" "${BASE}${path}")
    else
        out=$(curl -s -w '\n%{http_code}' -X "$method" "${BASE}${path}")
    fi
    code=$(printf '%s' "$out" | tail -n1)
    printf '%s' "$out" | sed '$d' > "$JSON"
    ok=$(python3 -c "import json;print(json.load(open('$JSON')).get('ok'))" 2>/dev/null)
    printf '%-44s' "$desc"
    if [ "$ok" = "False" ] && [ "${code:0:1}" = "4" ]; then
        printf '\033[32mOK  %s（预期报错）\033[0m\n' "$code"
        PASS=$((PASS + 1))
    else
        printf '\033[31m未按预期报错 %s\033[0m\n' "$code"
        FAIL=$((FAIL + 1))
    fi
    python3 -c "
import json
d = json.load(open('$JSON'))
print('   ', d.get('error', {}).get('message', ''))
" 2>/dev/null
}

# dump_rels <图名>：列出全部关系，核对每条边都有 weight
dump_rels() {
    local g="${1:-}"
    if [ -n "$g" ]; then
        curl -s -H "X-Graph: $g" "${BASE}/api/relationships?full=1&limit=20" > "$JSON"
    else
        curl -s "${BASE}/api/relationships?full=1&limit=20" > "$JSON"
    fi
    python3 -c "
import json
d = json.load(open('$JSON'))
if not d.get('ok'):
    print('   读取失败:', d.get('error', {}).get('message', ''))
else:
    dd = d['data']
    print('   共 %d 条' % dd['total'])
    for r in dd['items']:
        print('     #%s: %s->%s [%s] weight=%s props=%s'
              % (r['id'], r['from'], r['to'], r['type'], r['weight'], r['props']))
" 2>/dev/null
}

echo "═══════════════════════════════════════════════"
echo "  边权功能冒烟测试  端口 ${PORT}"
echo "═══════════════════════════════════════════════"

rm -rf "$DIR"; mkdir -p "$DIR"
stop_server
start_server

title "准备：4 个节点"
for n in A B C D; do
    curl -s -X POST -H 'Content-Type: application/json' \
         -d "{\"labels\":[\"City\"],\"props\":{\"name\":\"$n\"}}" "${BASE}/api/nodes" >/dev/null
done
curl -s "${BASE}/api/nodes?limit=10" > "$JSON"
python3 -c "
import json
print('   节点 id:', json.load(open('$JSON'))['data']['items'])" 2>/dev/null

title "创建边：显式指定边权"
req "1->2  weight=4.5"  POST /api/relationships '{"from":1,"to":2,"type":"ROAD","weight":4.5}'
req "1->3  weight=1"    POST /api/relationships '{"from":1,"to":3,"type":"ROAD","weight":1}'
req "2->4  weight=2"    POST /api/relationships '{"from":2,"to":4,"type":"ROAD","weight":2}'
req "3->4  weight=10"   POST /api/relationships '{"from":3,"to":4,"type":"ROAD","weight":10}'
req "2->3  不传 weight（应默认 1.0）" POST /api/relationships '{"from":2,"to":3,"type":"ROAD"}'

title "单条设权"
req "GET 边1 的权重"           GET   /api/relationships/1/weight
req "PUT 边1 -> 0.5"           PUT   /api/relationships/1/weight '{"weight":0.5}'
req "PATCH 边3 -> 1.5"         PATCH /api/relationships/3/weight '{"weight":1.5}'
req "PATCH 边4（weight 顶层字段）" PATCH /api/relationships/4 '{"weight":8}'

title "批量设权"
req "对象写法 {\"1\":0.5,\"2\":3}" POST /api/relationships/weights '{"weights":{"1":0.5,"2":3}}'
req "数组写法 [[3,1.2],[4,9]]"    POST /api/relationships/weights '{"weights":[[3,1.2],[4,9]]}'

title "边权统计"
req "GET /api/relationships/weight-stats" GET /api/relationships/weight-stats

title "按权重区间过滤"
req "minWeight=3"             GET '/api/relationships?minWeight=3'
req "minWeight=1&maxWeight=2" GET '/api/relationships?minWeight=1&maxWeight=2'
expect_fail "minWeight=abc（非法）" GET '/api/relationships?minWeight=abc'

title "带权最短路"
req "1->4（不传 weightKey，用一等 weight）" POST /api/path/weighted '{"from":1,"to":4}'
req "1->4（GET 写法）"                     GET  '/api/path/weighted?from=1&to=4'
req "1->4（weightKey=cost，走 props）"      POST /api/path/weighted '{"from":1,"to":4,"weightKey":"cost"}'
req "无权最短路 1->4（对照，只看跳数）"     GET  '/api/path?from=1&to=4'

title "非法边权校验"
expect_fail "weight 是字符串"   POST  /api/relationships '{"from":1,"to":2,"type":"ROAD","weight":"heavy"}'
expect_fail "weight 是对象"     POST  /api/relationships '{"from":1,"to":2,"type":"ROAD","weight":{"v":1}}'
expect_fail "设权时缺 weight"   PATCH /api/relationships/1/weight '{"w":1}'
expect_fail "设权到不存在的边"  PATCH /api/relationships/9999/weight '{"weight":1}'

title "负权：允许存，最短路给出明确告警"
req "边5 (2->3) -> -2（负权）" PATCH /api/relationships/5/weight '{"weight":-2}'
req "必经负权边的最短路 2->3"  POST /api/path/weighted '{"from":2,"to":3}'

title "完整关系列表（每条边都应带 weight）"
dump_rels

# ---------------------------------------------------------------- 持久化
title "持久化：重启后边权是否保留"
stop_server
start_server
dump_rels
req "重启后再算一次最短路 1->4" POST /api/path/weighted '{"from":1,"to":4}'
stop_server

# ---------------------------------------------------------------- 旧格式迁移
title "旧格式(v2)迁移：weight 原本存在 props 里"
python3 - "$DIR" <<'PY'
import struct, sys, os
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
path = os.path.join(d, "legacy.mgdb")   # 与默认图同一个数据目录，启动时会被自动扫描到
buf = bytearray()
def u32(v): buf.extend(struct.pack('<I', v))
def u64(v): buf.extend(struct.pack('<Q', v))
def s(x):
    b = x.encode(); u32(len(b)); buf.extend(b)
def value(v):
    if isinstance(v, int):
        buf.append(1); buf.extend(struct.pack('<q', v))
    elif isinstance(v, float):
        buf.append(2); buf.extend(struct.pack('<d', v))
    else:
        b = str(v).encode(); buf.append(4); u32(len(b)); buf.extend(b)
def props(p):
    u32(len(p))
    for k, v in p.items():
        s(k); value(v)
buf.extend(b'MINIGDB1')
u32(2)                     # 旧版本 v2：关系记录里没有 weight 字段
u64(2)                     # nodeCount
u64(2)                     # relCount
u64(0)                     # indexCount
u64(3); u64(3)             # nextNodeId / nextRelId
for nid in (1, 2):
    u64(nid); u32(1); s("City"); props({"name": "X%d" % nid})
# 关系1：props 里有数字型 weight=7.5（应被迁移为一等字段）
u64(1); u64(1); u64(2); s("ROAD"); props({"weight": 7.5, "note": "old"})
# 关系2：props 里有字符串型 weight="3"（尽力还原为 3.0）
u64(2); u64(2); u64(1); s("ROAD"); props({"weight": "3"})
buf.extend(b'MINIGDB1')
open(path, 'wb').write(bytes(buf))
print("   已生成 v2 旧文件:", path)
PY

start_server
echo "--- 用 X-Graph: legacy 读取（weight 应从 props 迁移到一等字段）---"
dump_rels legacy
stop_server

echo ""
echo "═══════════════════════════════════════════════"
echo "  通过: ${PASS}    未按预期: ${FAIL}"
echo "═══════════════════════════════════════════════"
rm -rf "$DIR" "$JSON"
