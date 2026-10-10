#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
graph_database_server —— 通用 Python 客户端（全接口 · 字典式传参）

只依赖标准库（urllib / json），可通过 HTTP 与图数据库服务交互，
覆盖服务端全部 41 个接口：图管理、节点、关系、边权、条件查询、
遍历与图算法、索引、运维。

------------------------------------------------------------------
一、配置：改脚本头部的 CONFIG 即可，不必改任何函数
------------------------------------------------------------------
    CONFIG = {
        "host":   "127.0.0.1",   # 服务 IP / 域名
        "port":   8080,          # 服务端口
        "graph":  "default",     # 默认操作的图（数据库）名
        "route":  "path",        # 图路由方式：path / header / query
        "timeout": 10,           # 单次请求超时（秒）
        "raise_on_error": True,  # 服务端 ok=false 时抛异常（False 则返回完整信封）
        "no_proxy": True,        # 访问本机时绕过系统 HTTP 代理
    }

    运行时改配置：configure({"port": 8099, "graph": "social"})
    单次调用换图：任意函数的字典里加 "graph": "warehouse"

------------------------------------------------------------------
二、传参约定：每个函数只收一个字典，键为 snake_case，值即业务数据
------------------------------------------------------------------
    create_node({"labels": ["Person"], "props": {"name": "Alice", "age": 30}})
    update_node({"id": 1, "add_labels": ["Manager"], "props": {"age": 31}})
    query({"label": "Person", "where": {"age": {"$gte": 30}}, "full": True})

    返回值统一是服务端响应里的 data 字段（已剥掉 {"ok": ..., "data": ...} 信封）。
    出错时抛 GraphAPIError（含 code / message / status），
    或把 CONFIG["raise_on_error"] 设为 False 后自行检查返回信封里的 "ok"。

------------------------------------------------------------------
三、命令行自检
------------------------------------------------------------------
    python3 scripts/graph_client.py            # 端到端演示全部接口（需服务已启动）
    python3 scripts/graph_client.py --check    # 只做健康检查 + 列出图
"""
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

# =============================================================================
# 配置区：连接参数集中在这里，函数内部不写死任何地址
# =============================================================================

CONFIG = {
    "host": "127.0.0.1",       # 图数据库服务 IP 地址或域名
    "port": 8080,              # 图数据库服务端口号
    "graph": "default",        # 默认操作的图（数据库）名
    "route": "path",           # 图路由方式：path=路径前缀 / header=X-Graph头 / query=?graph=
    "timeout": 10,             # 单次请求超时秒数
    "raise_on_error": True,    # 服务端返回 ok=false 时抛异常；False 则原样返回信封
    "no_proxy": True,          # 主机是本机地址时自动绕过系统 HTTP 代理
}

# 被视为"本机"的主机，用于自动绕过代理（curl 需要 --noproxy，urllib 同理）
_LOOPBACK = {"127.0.0.1", "localhost", "::1", "[::1]", ""}


class GraphAPIError(RuntimeError):
    """服务端返回 ok=false，或网络层出错。"""

    def __init__(self, message, code=None, status=None, method=None, path=None):
        super().__init__(message)
        self.code = code            # 服务端错误码：bad_request / not_found / ...
        self.status = status        # HTTP 状态码：400 / 404 / 409 / 500 ...
        self.method = method
        self.path = path

    def __str__(self):
        parts = [super().__str__()]
        if self.code:
            parts.append("code=%s" % self.code)
        if self.status:
            parts.append("http=%s" % self.status)
        if self.method and self.path:
            parts.append("%s %s" % (self.method, self.path))
        return " | ".join(parts)


# =============================================================================
# 配置与底层请求
# =============================================================================

def configure(overrides=None):
    """合并式修改全局配置，返回生效后的配置副本。

    configure({"host": "192.168.1.9", "port": 8099, "graph": "social"})
    """
    if overrides:
        unknown = set(overrides) - set(CONFIG)
        if unknown:
            raise ValueError("未知配置项: %s（可用: %s）" % (", ".join(sorted(unknown)),
                                                            ", ".join(sorted(CONFIG))))
        for k, v in overrides.items():
            if v is not None:
                CONFIG[k] = v
    return dict(CONFIG)


def current_config():
    """返回当前配置的副本。"""
    return dict(CONFIG)


def _base_url():
    host = str(CONFIG["host"]).strip()
    # 已经是完整 URL（http://... 或 https://...）就直接用
    if host.startswith("http://") or host.startswith("https://"):
        return host.rstrip("/")
    scheme = "https" if str(CONFIG["port"]) == "443" else "http"
    return "%s://%s:%s" % (scheme, host, CONFIG["port"])


def _opener():
    """构造 urlopen 用的 opener；访问本机时绕开代理，避免被 HTTP_PROXY 拦掉。"""
    if CONFIG.get("no_proxy") and str(CONFIG["host"]).strip().lower() in _LOOPBACK:
        return urllib.request.build_opener(urllib.request.ProxyHandler({}))
    return urllib.request.build_opener()


def _qs(query):
    """把 dict 编成 query string；bool 转 1/0，None 跳过，容器用 JSON 文本。"""
    if not query:
        return ""
    flat = []
    for k, v in query.items():
        if v is None:
            continue
        if isinstance(v, bool):
            flat.append((k, "1" if v else "0"))
        elif isinstance(v, (list, tuple, dict)):
            flat.append((k, json.dumps(v, ensure_ascii=False)))
        else:
            flat.append((k, str(v)))
    return urllib.parse.urlencode(flat, encoding="utf-8")


def request(spec):
    """底层通用请求：一次 HTTP 调用，自动处理图路由、JSON、错误。

    spec 字典：
        method  str    必填，GET / POST / PUT / PATCH / DELETE
        path    str    必填，"nodes"、"nodes/1"、"graphs"、"admin/save" ...
                       （不要带 /api 前缀，图前缀会自动拼）
        query   dict   可选，查询参数
        body    dict   可选，JSON 请求体
        graph   str    可选，本次调用作用在哪张图；缺省用 CONFIG["graph"]
        scoped  bool   默认 True：是否受图路由影响。
                       /graphs、/health、/manifest、/admin/save 这类全局接口传 False
    """
    method = str(spec.get("method", "GET")).upper()
    path = str(spec.get("path", "")).lstrip("/")
    query = dict(spec.get("query") or {})
    body = spec.get("body")
    graph = spec.get("graph") or CONFIG["graph"]
    scoped = spec.get("scoped", True)

    url = _base_url()
    headers = {}

    if scoped and graph:
        route = CONFIG["route"]
        if route == "path":
            url += "/api/g/%s/%s" % (urllib.parse.quote(str(graph), safe=""), path)
        elif route == "header":
            url += "/api/%s" % path
            headers["X-Graph"] = str(graph)
        elif route == "query":
            url += "/api/%s" % path
            query.setdefault("graph", graph)
        else:
            raise ValueError('CONFIG["route"] 只能是 path / header / query，当前: %r' % route)
    else:
        url += "/api/%s" % path

    qs = _qs(query)
    if qs:
        url += "?" + qs

    data = None
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with _opener().open(req, timeout=CONFIG["timeout"]) as resp:
            raw = resp.read().decode("utf-8")
            status = resp.getcode()
    except urllib.error.HTTPError as e:            # 4xx / 5xx 也有 JSON 信封
        raw = e.read().decode("utf-8")
        status = e.code
    except urllib.error.URLError as e:
        raise GraphAPIError("无法连接图数据库服务 %s：%s" % (_base_url(), e.reason),
                            code="connection_error", status=None, method=method, path=path)

    try:
        payload = json.loads(raw) if raw else {}
    except ValueError:
        raise GraphAPIError("响应不是合法 JSON: %r" % raw[:200],
                            code="bad_response", status=status, method=method, path=path)

    if not payload.get("ok"):
        err = payload.get("error") or {}
        if CONFIG["raise_on_error"]:
            raise GraphAPIError(err.get("message", "请求失败"),
                                code=err.get("code"), status=status,
                                method=method, path=path)
        return payload
    return payload.get("data")


# ---- 内部小工具 ------------------------------------------------------------

def _require(spec, *keys):
    """校验必填字段，缺了给出清晰报错。"""
    missing = [k for k in keys if k not in spec or spec[k] is None]
    if missing:
        raise ValueError("缺少必填参数: %s（收到: %s）"
                         % (", ".join(missing), json.dumps(spec, ensure_ascii=False)))


def _pick(spec, mapping):
    """按 {spec键: 目标键} 映射挑选非 None 字段。"""
    out = {}
    for src, dst in mapping.items():
        if spec.get(src) is not None:
            out[dst] = spec[src]
    return out


def _join_types(value):
    """关系类型：列表转逗号分隔串（服务端按逗号切分）。"""
    if value is None:
        return None
    if isinstance(value, (list, tuple)):
        return ",".join(str(v) for v in value)
    return str(value)


def _filter_value(value, vtype=None):
    """key/value 过滤里的 value：容器按 JSON 文本传，配合 vtype=json。"""
    if isinstance(value, (list, tuple, dict)):
        return json.dumps(value, ensure_ascii=False)
    return value


# =============================================================================
# 一、健康检查与自描述
# =============================================================================

def health(spec=None):
    """GET /api/health —— 服务状态、数据目录、默认图、全部图概况。

    spec: {}（无参数）
    """
    return request({"method": "GET", "path": "health", "scoped": False})


def manifest(spec=None):
    """GET /api/manifest —— 服务端自描述的接口清单（版本号 + 全部接口）。

    spec: {}（无参数）
    """
    return request({"method": "GET", "path": "manifest", "scoped": False})


def stats(spec=None):
    """GET /api/stats —— 当前图的统计：节点数、关系数、标签分布、类型分布、索引。

    spec: {"graph": "social"}（可选，换图）
    """
    spec = spec or {}
    return request({"method": "GET", "path": "stats", "graph": spec.get("graph")})


def save_all(spec=None):
    """POST /api/admin/save —— 把所有图立即落盘（自动落盘关掉后用它收尾）。

    spec: {}（无参数）
    """
    return request({"method": "POST", "path": "admin/save", "scoped": False})


def clear_current(spec=None):
    """POST /api/admin/clear —— 清空"当前图"（危险）。

    spec: {"confirm": True, "graph": "social"}
    """
    spec = spec or {}
    return request({"method": "POST", "path": "admin/clear",
                    "body": {"confirm": bool(spec.get("confirm", False))},
                    "graph": spec.get("graph")})


# =============================================================================
# 二、图（数据库）管理
# =============================================================================

def list_graphs(spec=None):
    """GET /api/graphs —— 列出所有图及其节点/关系数、文件大小。

    spec: {}（无参数；图管理接口不受图路由影响）
    """
    return request({"method": "GET", "path": "graphs", "scoped": False})


def create_graph(spec):
    """POST /api/graphs —— 新建一张图（立刻生成 <图名>.mgdb）。

    spec: {"name": "social"}        图名，≤64 字符，只允许字母数字 _ - . 空格
    已存在返回 409 already_exists。
    """
    _require(spec, "name")
    return request({"method": "POST", "path": "graphs",
                    "body": {"name": spec["name"]}, "scoped": False})


def graph_info(spec):
    """GET /api/graphs/{name} —— 单张图的统计（节点/关系数、标签分布、索引）。

    spec: {"name": "social"}
    """
    _require(spec, "name")
    return request({"method": "GET", "path": "graphs/%s" % urllib.parse.quote(str(spec["name"])),
                    "scoped": False})


def delete_graph(spec):
    """DELETE /api/graphs/{name} —— 删除整张图（危险，连带删 .mgdb 文件）。

    spec: {"name": "social", "confirm": True}
    """
    _require(spec, "name")
    return request({"method": "DELETE",
                    "path": "graphs/%s" % urllib.parse.quote(str(spec["name"])),
                    "query": {"confirm": "1" if spec.get("confirm", True) else "0"},
                    "body": {"confirm": bool(spec.get("confirm", True))},
                    "scoped": False})


def save_graph(spec):
    """POST /api/graphs/{name}/save —— 只把这一张图落盘。

    spec: {"name": "social"}
    """
    _require(spec, "name")
    return request({"method": "POST",
                    "path": "graphs/%s/save" % urllib.parse.quote(str(spec["name"])),
                    "scoped": False})


def clear_graph(spec):
    """POST /api/graphs/{name}/clear —— 清空这张图（危险，只认请求体 confirm）。

    spec: {"name": "social", "confirm": True}
    """
    _require(spec, "name")
    return request({"method": "POST",
                    "path": "graphs/%s/clear" % urllib.parse.quote(str(spec["name"])),
                    "body": {"confirm": bool(spec.get("confirm", True))},
                    "scoped": False})


# =============================================================================
# 三、节点（增 / 查 / 改 / 删）
# =============================================================================

def create_node(spec=None):
    """POST /api/nodes —— 创建节点。

    spec:
        labels  list[str] 或 str   标签（"Person" 与 ["Person"] 都接受）
        props   dict               属性；值可为标量、数组、对象，支持嵌套
        graph   str                可选，换图
    返回：{"id": 新节点id, "node": {...}}
    """
    spec = spec or {}
    body = {}
    if spec.get("labels") is not None:
        body["labels"] = spec["labels"]
    if spec.get("props") is not None:
        body["props"] = spec["props"]
    return request({"method": "POST", "path": "nodes", "body": body,
                    "graph": spec.get("graph")})


def list_nodes(spec=None):
    """GET /api/nodes —— 列出 / 过滤节点。

    spec:
        label   str    按标签过滤
        key     str    属性键（给了 key 就要给 value）
        value   any    期望值；列表/字典会自动转成 JSON 文本，配 vtype="json"
        vtype   str    auto / int / double / bool / string / json
        op      str    eq ne gt gte lt lte in contains has size（不加 $）
        limit   int    单页条数，>1000 或 =0 会被服务端改成 1000
        offset  int    跳过条数
        full    bool   默认 True：返回完整节点；False 只返回 id 列表
        graph   str    可选，换图
    """
    spec = spec or {}
    q = _pick(spec, {"label": "label", "key": "key", "vtype": "vtype",
                     "op": "op", "limit": "limit", "offset": "offset"})
    if spec.get("value") is not None:
        q["value"] = _filter_value(spec["value"], spec.get("vtype"))
    q["full"] = spec.get("full", True)
    return request({"method": "GET", "path": "nodes", "query": q,
                    "graph": spec.get("graph")})


def get_node(spec):
    """GET /api/nodes/{id} —— 查询单个节点。

    spec: {"id": 1, "graph": "social"}
    """
    _require(spec, "id")
    return request({"method": "GET", "path": "nodes/%s" % spec["id"],
                    "graph": spec.get("graph")})


def replace_node(spec):
    """PUT /api/nodes/{id} —— 整体替换（覆盖式）。

    spec:
        id      int         必填
        labels  list[str]   只接受数组；给出的即为最终标签集（省则标签不变）
        props   dict        整体替换属性：先清空旧属性再写入（省则属性不变）
        graph   str
    """
    _require(spec, "id")
    if spec.get("labels") is not None and not isinstance(spec["labels"], (list, tuple)):
        raise ValueError('PUT 的 labels 必须是数组（如 ["Person"]），整体替换不接受字符串')
    body = _pick(spec, {"labels": "labels", "props": "props"})
    return request({"method": "PUT", "path": "nodes/%s" % spec["id"], "body": body,
                    "graph": spec.get("graph")})


def update_node(spec):
    """PATCH /api/nodes/{id} —— 增量修改（合并式，推荐）。

    spec:
        id             int       必填
        props          dict      合并属性：只覆盖写到的键，其余保留
        add_labels     list[str] 增量加标签
        remove_labels  list[str] 增量删标签
        remove_props   list[str] 按 key 删除属性
        array_append   dict      数组属性追加元素（自动去重）
        array_remove   dict      数组属性删除元素
        object_merge   dict      对象属性浅合并（同名键覆盖）
        graph          str
    服务端执行顺序：addLabels → removeLabels → props → removeProps
                 → arrayAppend → arrayRemove → objectMerge
    """
    _require(spec, "id")
    body = _pick(spec, {"props": "props", "add_labels": "addLabels",
                        "remove_labels": "removeLabels", "remove_props": "removeProps",
                        "array_append": "arrayAppend", "array_remove": "arrayRemove",
                        "object_merge": "objectMerge"})
    if not body:
        raise ValueError("update_node 至少需要给一个要改的字段")
    return request({"method": "PATCH", "path": "nodes/%s" % spec["id"], "body": body,
                    "graph": spec.get("graph")})


def delete_node(spec):
    """DELETE /api/nodes/{id} —— 删除节点（级联删除其全部关系）。

    spec: {"id": 1, "graph": "social"}
    返回：{"id":1, "deleted":true, "cascadeRelationships":2}
    """
    _require(spec, "id")
    return request({"method": "DELETE", "path": "nodes/%s" % spec["id"],
                    "graph": spec.get("graph")})


# ---- 节点的图遍历 ----------------------------------------------------------

def neighbors(spec):
    """GET /api/nodes/{id}/neighbors —— 邻居节点。

    spec:
        id    int  必填
        dir   str  out / in / both（默认 out）
        type  str 或 list  关系类型，多个用列表或逗号分隔串
        full  bool 默认 True：完整节点；False 只给 id
        graph str
    返回 data 里的字段名是 "neighbors"，不是 "items"。
    """
    _require(spec, "id")
    q = {"dir": spec.get("dir", "out"), "full": spec.get("full", True)}
    t = _join_types(spec.get("type"))
    if t:
        q["type"] = t
    return request({"method": "GET", "path": "nodes/%s/neighbors" % spec["id"],
                    "query": q, "graph": spec.get("graph")})


def degree(spec=None):
    """GET /api/nodes/{id}/degree —— 度数（默认双向 both）。

    spec: {"id": 1, "dir": "both", "type": "KNOWS", "graph": "social"}
    """
    spec = spec or {}
    _require(spec, "id")
    q = {"dir": spec.get("dir", "both")}
    t = _join_types(spec.get("type"))
    if t:
        q["type"] = t
    return request({"method": "GET", "path": "nodes/%s/degree" % spec["id"],
                    "query": q, "graph": spec.get("graph")})


def node_relationships(spec):
    """GET /api/nodes/{id}/relationships —— 与该节点相连的关系。

    spec: {"id": 1, "dir": "both", "type": ["KNOWS"], "full": True, "graph": "social"}
    注意：本接口服务端默认 full=true；返回字段是 "relationships"。
    """
    _require(spec, "id")
    q = {"dir": spec.get("dir", "both"), "full": spec.get("full", True)}
    t = _join_types(spec.get("type"))
    if t:
        q["type"] = t
    return request({"method": "GET", "path": "nodes/%s/relationships" % spec["id"],
                    "query": q, "graph": spec.get("graph")})


# =============================================================================
# 四、关系（增 / 查 / 改 / 删）
# =============================================================================

def create_relationship(spec):
    """POST /api/relationships —— 创建关系。

    spec:
        from   int 或数字字符串  起点节点 id（必填，节点不存在 → 404）
        to     int 或数字字符串  终点节点 id（必填）
        type   str              关系类型（必填且非空）
        props  dict             属性，同样支持数组/对象
        weight float            边权，不传则服务端默认 1.0；必须是数字
        graph  str
    返回：{"id": 新关系id, "relationship": {...}}
    """
    _require(spec, "from", "to", "type")
    body = {"from": spec["from"], "to": spec["to"], "type": spec["type"]}
    if spec.get("props") is not None:
        body["props"] = spec["props"]
    if spec.get("weight") is not None:
        body["weight"] = spec["weight"]
    return request({"method": "POST", "path": "relationships", "body": body,
                    "graph": spec.get("graph")})


def list_relationships(spec=None):
    """GET /api/relationships —— 列出 / 过滤关系。

    spec:
        type        str   按类型过滤（本接口不切分逗号，整体当一个类型名）
        from / to   int   两端点；都给出时按端点查，此时 dir 生效
        dir         str   out / in / both，仅在 from+to 都给时生效（默认 out）
        min_weight  num   边权下界
        max_weight  num   边权上界
        limit/offset/full/graph   同 list_nodes
    返回 data 形状：{"total", "offset", "limit", "items"}
    """
    spec = spec or {}
    q = _pick(spec, {"type": "type", "from": "from", "to": "to", "dir": "dir",
                     "limit": "limit", "offset": "offset",
                     "min_weight": "minWeight", "max_weight": "maxWeight"})
    q["full"] = spec.get("full", True)
    return request({"method": "GET", "path": "relationships", "query": q,
                    "graph": spec.get("graph")})


def get_relationship(spec):
    """GET /api/relationships/{id} —— 查询单个关系（含 weight）。

    spec: {"id": 1, "graph": "social"}
    """
    _require(spec, "id")
    return request({"method": "GET", "path": "relationships/%s" % spec["id"],
                    "graph": spec.get("graph")})


def replace_relationship(spec):
    """PUT /api/relationships/{id} —— 整体替换关系属性（覆盖式）。

    spec:
        id           int   必填
        type         str   改类型；内部是"删除+重建"，id 可能变化（看响应 idChanged）
        props        dict  整体替换属性：先清空再写入
        weight       num   同时改边权
        remove_props list  删属性（在 props 之后执行）
        graph        str
    """
    _require(spec, "id")
    body = _pick(spec, {"type": "type", "props": "props", "weight": "weight",
                        "remove_props": "removeProps"})
    if not body:
        raise ValueError("replace_relationship 至少需要给一个要改的字段")
    return request({"method": "PUT", "path": "relationships/%s" % spec["id"],
                    "body": body, "graph": spec.get("graph")})


def update_relationship(spec):
    """PATCH /api/relationships/{id} —— 增量修改关系（合并式，推荐）。

    spec:
        id             int  必填
        type           str  改类型（同样走删除+重建，id 可能变）
        props          dict 合并属性
        weight         num  改边权
        remove_props   list 删属性
        array_append/array_remove/object_merge  dict  容器增量操作，语义同节点
        graph          str
    """
    _require(spec, "id")
    body = _pick(spec, {"type": "type", "props": "props", "weight": "weight",
                        "remove_props": "removeProps", "array_append": "arrayAppend",
                        "array_remove": "arrayRemove", "object_merge": "objectMerge"})
    if not body:
        raise ValueError("update_relationship 至少需要给一个要改的字段")
    return request({"method": "PATCH", "path": "relationships/%s" % spec["id"],
                    "body": body, "graph": spec.get("graph")})


def delete_relationship(spec):
    """DELETE /api/relationships/{id} —— 删除关系。

    spec: {"id": 1, "graph": "social"}
    """
    _require(spec, "id")
    return request({"method": "DELETE", "path": "relationships/%s" % spec["id"],
                    "graph": spec.get("graph")})


# =============================================================================
# 五、边权（weight，关系的一等字段）
# =============================================================================

def get_weight(spec):
    """GET /api/relationships/{id}/weight —— 读单条边的边权。

    spec: {"id": 1, "graph": "social"} → {"id": 1, "weight": 4.5}
    """
    _require(spec, "id")
    return request({"method": "GET", "path": "relationships/%s/weight" % spec["id"],
                    "graph": spec.get("graph")})


def set_weight(spec):
    """PUT/PATCH /api/relationships/{id}/weight —— 给单条边设权。

    spec: {"id": 1, "weight": 2.5, "graph": "social"}
    weight 必须是有限数字，字符串/对象/NaN 会被服务端拒（400）。
    """
    _require(spec, "id", "weight")
    return request({"method": "PATCH", "path": "relationships/%s/weight" % spec["id"],
                    "body": {"weight": spec["weight"]}, "graph": spec.get("graph")})


def set_weights(spec):
    """POST /api/relationships/weights —— 批量设权。

    spec:
        weights  dict 或 list
            dict 写法：{1: 0.5, 2: 3.0}（键会被转成字符串 "1"、"2"）
            list 写法：[[3, 1.2], [4, 9]]
        graph    str
    返回：{"requested":n, "applied":n, "errors":[], "relationships":[{"id","weight"}]}
    """
    _require(spec, "weights")
    return request({"method": "POST", "path": "relationships/weights",
                    "body": {"weights": spec["weights"]}, "graph": spec.get("graph")})


def weight_stats(spec=None):
    """GET /api/relationships/weight-stats —— 边权统计。

    spec: {"graph": "social"}（可省略）
    → {"count", "weightedCount", "defaultWeight", "sum", "min", "max", "avg"}
    """
    spec = spec or {}
    return request({"method": "GET", "path": "relationships/weight-stats",
                    "graph": spec.get("graph")})


# =============================================================================
# 六、条件查询 / 遍历 / 图算法
# =============================================================================

def query(spec=None):
    """POST /api/query —— 条件查询（支持多条件与操作符，首选）。

    spec:
        label   str   标签过滤，空串/省略 = 不限
        where   dict  条件，两种写法：
                      精确   {"city": "北京"}
                      带操作 {"age": {"$gte": 30}, "tags": {"$contains": "vip"}}
                      可选操作符：$ne $gt $gte $lt $lte $in $contains $has $size
                      $contains：数组元素 / 对象值 / 字符串子串
                      $has：     对象含某键 / 数组含某元素
                      $size：    数组/对象长度等于给定值
        limit   int   单页条数（≤0 或 >1000 → 1000）
        offset  int   跳过条数
        full    bool  默认 True：返回完整节点
        graph   str
    返回 data 形状：{"total", "offset", "limit", "items"}
    """
    spec = spec or {}
    body = {"full": spec.get("full", True)}
    if spec.get("label") is not None:
        body["label"] = spec["label"]
    if spec.get("where") is not None:
        body["where"] = spec["where"]
    if spec.get("limit") is not None:
        body["limit"] = spec["limit"]
    if spec.get("offset") is not None:
        body["offset"] = spec["offset"]
    return request({"method": "POST", "path": "query", "body": body,
                    "graph": spec.get("graph")})


def reachable(spec):
    """GET /api/reachable/{id} —— N 跳内可达的节点。

    spec:
        id     int  起点（必填）
        depth  int  最大跳数，默认 1
        dir    str  out / in / both，默认 out
        type   str 或 list  关系类型
        full   bool 默认 True
        graph  str
    返回 data 里的字段名是 "nodes"。
    """
    _require(spec, "id")
    q = {"depth": spec.get("depth", 1), "dir": spec.get("dir", "out"),
         "full": spec.get("full", True)}
    t = _join_types(spec.get("type"))
    if t:
        q["type"] = t
    return request({"method": "GET", "path": "reachable/%s" % spec["id"],
                    "query": q, "graph": spec.get("graph")})


def shortest_path(spec):
    """GET /api/path —— 无权最短路径（只看跳数，BFS）。

    spec: {"from": 1, "to": 3, "dir": "both", "type": ["KNOWS"], "graph": "social"}
    → {"found": true, "hops": 2, "nodes": [1, 2, 3]}（不通时 hops = -1）
    """
    _require(spec, "from", "to")
    q = {"from": spec["from"], "to": spec["to"], "dir": spec.get("dir", "out")}
    t = _join_types(spec.get("type"))
    if t:
        q["type"] = t
    return request({"method": "GET", "path": "path", "query": q,
                    "graph": spec.get("graph")})


def weighted_path(spec):
    """POST /api/path/weighted —— 带权最短路径（Dijkstra）。

    spec:
        from        int  起点（必填）
        to          int  终点（必填）
        weight_key  str  从关系的哪个属性读权重；省略则用一等 weight 字段
        dir         str  out / in / both，默认 out
        types       list 关系类型白名单，如 ["ROAD"]
        graph       str
    → {"found", "cost", "weightKey", "weightSource", "nodes", "relationships", "legWeights"}

    提示：同一对节点之间若还有没设权的边（默认权重 1.0），Dijkstra 会优先走那条，
    看起来像"边权没生效"——记得用 types 限定只看你设过权的那类边。
    路径含负权边时结果不保证最优，响应会带 negativeWeight 告警。
    """
    _require(spec, "from", "to")
    body = {"from": spec["from"], "to": spec["to"], "dir": spec.get("dir", "out")}
    if spec.get("weight_key") is not None:
        body["weightKey"] = spec["weight_key"]
    if spec.get("types") is not None:
        body["types"] = list(spec["types"])
    return request({"method": "POST", "path": "path/weighted", "body": body,
                    "graph": spec.get("graph")})


# =============================================================================
# 七、索引
# =============================================================================

def list_indexes(spec=None):
    """GET /api/indexes —— 列出二级索引。

    spec: {"graph": "social"}（可省略）→ {"total": n, "items": [{"label","key"}]}
    """
    spec = spec or {}
    return request({"method": "GET", "path": "indexes", "graph": spec.get("graph")})


def create_index(spec):
    """POST /api/indexes —— 创建索引（只对标量值生效，容器值不进索引）。

    spec: {"label": "Person", "key": "name", "graph": "social"}
    """
    _require(spec, "label", "key")
    return request({"method": "POST", "path": "indexes",
                    "body": {"label": spec["label"], "key": spec["key"]},
                    "graph": spec.get("graph")})


def drop_index(spec):
    """DELETE /api/indexes —— 删除索引。

    spec: {"label": "Person", "key": "name", "graph": "social"}
    """
    _require(spec, "label", "key")
    return request({"method": "DELETE", "path": "indexes",
                    "query": {"label": spec["label"], "key": spec["key"]},
                    "body": {"label": spec["label"], "key": spec["key"]},
                    "graph": spec.get("graph")})


# =============================================================================
# 八、逃生舱：任意接口
# =============================================================================

def call(spec):
    """直接调用任意接口（新增接口 / 临时调试用）。

    spec 同 request()：{"method","path","query","body","graph","scoped"}
    例：call({"method": "GET", "path": "relationships/weight-stats", "graph": "social"})
    """
    return request(spec)


# 全部对外函数，便于自检与文档生成
ALL_FUNCTIONS = [
    health, manifest, stats, save_all, clear_current,
    list_graphs, create_graph, graph_info, delete_graph, save_graph, clear_graph,
    create_node, list_nodes, get_node, replace_node, update_node, delete_node,
    neighbors, degree, node_relationships,
    create_relationship, list_relationships, get_relationship,
    replace_relationship, update_relationship, delete_relationship,
    get_weight, set_weight, set_weights, weight_stats,
    query, reachable, shortest_path, weighted_path,
    list_indexes, create_index, drop_index,
    call,
]


# =============================================================================
# 命令行自检：端到端跑一遍全部接口
# =============================================================================

def _demo():
    """端到端演示。用法：python3 scripts/graph_client.py"""
    def show(title, value):
        print("\n== %s ==" % title)
        print("  ", json.dumps(value, ensure_ascii=False))

    print("配置:", json.dumps(current_config(), ensure_ascii=False))
    show("健康检查", health())

    # ---- 图管理 ----
    graph = "py_client_demo"
    try:
        show("建图", create_graph({"name": graph}))
    except GraphAPIError as e:
        print("  建图（可能已存在）:", e)
    show("清空该图", clear_graph({"name": graph, "confirm": True}))
    configure({"graph": graph})

    # ---- 增：节点 ----
    alice = create_node({"labels": ["Person"],
                         "props": {"name": "Alice", "age": 30, "city": "北京",
                                   "tags": ["vip"], "addr": {"city": "北京", "zip": "100000"}}})["id"]
    bob = create_node({"labels": ["Person"], "props": {"name": "Bob", "age": 26}})["id"]
    carol = create_node({"labels": ["Person", "Engineer"], "props": {"name": "Carol", "age": 33}})["id"]
    acme = create_node({"labels": ["Company"], "props": {"name": "Acme"}})["id"]
    print("\n== 创建 4 个节点 ==\n   Alice=%s Bob=%s Carol=%s Acme=%s" % (alice, bob, carol, acme))

    # ---- 增：关系（含边权）----
    r_knows = create_relationship({"from": alice, "to": bob, "type": "KNOWS",
                                   "props": {"since": 2019}})["id"]
    create_relationship({"from": bob, "to": carol, "type": "KNOWS", "props": {"since": 2020}})
    create_relationship({"from": alice, "to": acme, "type": "WORKS_AT",
                         "props": {"roles": ["dev", "lead"]}})
    r_ab = create_relationship({"from": alice, "to": bob, "type": "ROAD",
                                "props": {"name": "京沪线"}, "weight": 5.0})["id"]
    r_bc = create_relationship({"from": bob, "to": carol, "type": "ROAD",
                                "props": {"name": "沪穗线"}, "weight": 3.0})["id"]
    r_ac = create_relationship({"from": alice, "to": carol, "type": "ROAD",
                                "props": {"name": "直达线"}, "weight": 20.0})["id"]
    print("\n== 创建 6 条关系（含 3 条带权 ROAD）==")

    # ---- 查 ----
    show("查单个节点", get_node({"id": alice}))
    show("列节点（按标签）", list_nodes({"label": "Person", "limit": 10}))
    show("key/value 过滤", list_nodes({"key": "name", "value": "Alice"}))
    show("条件查询 age>=30", query({"label": "Person", "where": {"age": {"$gte": 30}}})["items"])
    show("数组包含 tags∋vip", query({"where": {"tags": {"$contains": "vip"}}})["items"])
    show("邻居（双向）", neighbors({"id": alice, "dir": "both"}))
    show("度数", degree({"id": alice, "dir": "both"}))
    show("相连关系", node_relationships({"id": alice, "dir": "both"}))
    show("2 跳可达", reachable({"id": alice, "depth": 2}))
    show("无权最短路径", shortest_path({"from": alice, "to": carol}))
    show("带权最短路径（只看 ROAD）",
         weighted_path({"from": alice, "to": carol, "types": ["ROAD"]}))

    # ---- 改 ----
    show("PATCH 改节点（加标签 + 合并属性 + 数组追加 + 对象合并）",
         update_node({"id": alice, "props": {"age": 31}, "add_labels": ["Manager"],
                      "array_append": {"tags": ["gold"]},
                      "object_merge": {"addr": {"street": "长安街"}}}))
    show("PUT 整体替换节点", replace_node({"id": bob, "labels": ["Person"],
                                           "props": {"name": "Bob", "age": 27}}))
    show("PATCH 改关系", update_relationship({"id": r_knows, "props": {"since": 2021},
                                              "array_append": {"roles": ["mentor"]}}))

    # ---- 边权 ----
    show("读边权", get_weight({"id": r_ab}))
    show("改边权", set_weight({"id": r_ac, "weight": 6.0}))
    show("批量设权", set_weights({"weights": {r_ab: 1.5, r_bc: 1.5}}))
    show("边权统计", weight_stats())
    show("按权重筛选 >=5", list_relationships({"min_weight": 5}))
    show("带权最短路（改权后）",
         weighted_path({"from": alice, "to": carol, "types": ["ROAD"]}))

    # ---- 索引 ----
    show("建索引", create_index({"label": "Person", "key": "name"}))
    show("列索引", list_indexes())
    show("删索引", drop_index({"label": "Person", "key": "name"}))

    # ---- 删 ----
    show("删关系", delete_relationship({"id": r_knows}))
    show("删节点（级联）", delete_node({"id": acme}))

    # ---- 统计与落盘 ----
    show("当前图统计", stats())
    show("单图落盘", save_graph({"name": graph}))
    show("全图落盘", save_all())
    show("图信息", graph_info({"name": graph}))
    show("全部图", list_graphs())

    # ---- 错误处理演示 ----
    print("\n== 错误处理演示（读一个不存在的节点）==")
    try:
        get_node({"id": 999999})
    except GraphAPIError as e:
        print("   已捕获:", e, "| code =", e.code, "| http =", e.status)

    # ---- 清理 ----
    show("删图", delete_graph({"name": graph, "confirm": True}))
    print("\n全部接口调用完毕。")


def _check():
    """只做连通性检查。用法：python3 scripts/graph_client.py --check"""
    print("配置:", json.dumps(current_config(), ensure_ascii=False))
    print("健康检查:", json.dumps(health(), ensure_ascii=False))
    print("全部图:", json.dumps(list_graphs(), ensure_ascii=False))


if __name__ == "__main__":
    if "--check" in sys.argv:
        _check()
    else:
        _demo()
