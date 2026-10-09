#!/usr/bin/env python3
"""
graph_server 的 Python 客户端示例（多图 + 数组/对象属性）

    python3 scripts/demo.py              # 默认连接 127.0.0.1:8080，操作 social 图
    python3 scripts/demo.py 8099         # 指定端口

依赖：仅标准库。
"""
import json
import sys
import urllib.error
import urllib.request


class GraphClient:
    """极简客户端：把 HTTP 细节收进 request()，对外只暴露图操作语义。

    操作哪张图由 X-Graph 请求头决定；传 graph=None 时用服务端的默认图。
    """

    def __init__(self, base="http://127.0.0.1:8080", graph=None):
        self.base = base.rstrip("/")
        self.graph = graph

    def using(self, graph):
        """返回一份绑定到另一张图的客户端（轻量拷贝）"""
        return GraphClient(self.base, graph)

    # ---------------- 图管理 ----------------
    def graphs(self):
        return self.request("GET", "/api/graphs")

    def create_graph(self, name):
        return self.request("POST", "/api/graphs", {"name": name})

    def delete_graph(self, name):
        return self.request("DELETE", "/api/graphs/%s?confirm=1" % name)

    # ---------------- 底层 ----------------
    def request(self, method, path, body=None):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        headers = {"Content-Type": "application/json"} if data else {}
        if self.graph:
            headers["X-Graph"] = self.graph
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            payload = json.loads(e.read().decode("utf-8"))
        if not payload.get("ok"):
            err = payload.get("error", {})
            raise RuntimeError(f'{method} {path} -> {err.get("code")}: {err.get("message")}')
        return payload["data"]

    # ---------------- 节点 ----------------
    def create_node(self, labels, props=None):
        return self.request("POST", "/api/nodes", {"labels": labels, "props": props or {}})["id"]

    def get_node(self, node_id):
        return self.request("GET", f"/api/nodes/{node_id}")

    def update_node(self, node_id, props=None, add_labels=None, remove_labels=None,
                    array_append=None, array_remove=None, object_merge=None):
        body = {}
        if props:         body["props"] = props
        if add_labels:    body["addLabels"] = add_labels
        if remove_labels: body["removeLabels"] = remove_labels
        if array_append:  body["arrayAppend"] = array_append     # {"tags": ["vip"]}
        if array_remove:  body["arrayRemove"] = array_remove
        if object_merge:  body["objectMerge"] = object_merge     # {"addr": {"city": "上海"}}
        return self.request("PATCH", f"/api/nodes/{node_id}", body)

    def delete_node(self, node_id):
        return self.request("DELETE", f"/api/nodes/{node_id}")

    def list_nodes(self, label=None, full=True, limit=100):
        path = "/api/nodes?full=1&limit=%d" % limit
        if label:
            path += "&label=" + label
        return self.request("GET", path)

    # ---------------- 关系 ----------------
    def create_rel(self, from_id, to_id, rel_type, props=None, weight=None):
        """weight 是这条边的边权，不传则用服务端默认值 1.0"""
        body = {"from": from_id, "to": to_id, "type": rel_type, "props": props or {}}
        if weight is not None:
            body["weight"] = weight
        return self.request("POST", "/api/relationships", body)["id"]

    def delete_rel(self, rel_id):
        return self.request("DELETE", f"/api/relationships/{rel_id}")

    def list_rels(self, rel_type=None, min_weight=None, max_weight=None, full=True):
        path = "/api/relationships?limit=100"
        if full:         path += "&full=1"
        if rel_type:     path += "&type=" + rel_type
        if min_weight is not None: path += "&minWeight=%s" % min_weight
        if max_weight is not None: path += "&maxWeight=%s" % max_weight
        return self.request("GET", path)

    # ---------------- 边权 ----------------
    def set_weight(self, rel_id, weight):
        """给单条边设权"""
        return self.request("PATCH", f"/api/relationships/{rel_id}/weight", {"weight": weight})

    def set_weights(self, weights):
        """批量设权：{rel_id: weight} 或 [[rel_id, weight], ...]"""
        return self.request("POST", "/api/relationships/weights", {"weights": weights})

    def weight_stats(self):
        return self.request("GET", "/api/relationships/weight-stats")

    def weighted_path(self, from_id, to_id, weight_key=None, types=None, direction=None):
        """带权最短路。weight_key 为空时用关系的一等 weight 字段；
        传了 weight_key 则改从 props[weight_key] 取权重。
        types 用来限定只走某些类型的边（例如只看 ROAD，忽略 KNOWS）。"""
        body = {"from": from_id, "to": to_id}
        if weight_key:
            body["weightKey"] = weight_key
        if types:
            body["types"] = types
        if direction:
            body["dir"] = direction
        return self.request("POST", "/api/path/weighted", body)

    # ---------------- 查询 / 算法 ----------------
    def query(self, label=None, where=None, full=True):
        """where 支持操作符：{"age": {"$gte": 30}, "tags": {"$contains": "vip"}}"""
        return self.request("POST", "/api/query",
                            {"label": label, "where": where or {}, "full": full})

    def neighbors(self, node_id, direction="out", full=True):
        return self.request("GET", f"/api/nodes/{node_id}/neighbors?dir={direction}&full=1")

    def shortest_path(self, from_id, to_id):
        return self.request("GET", f"/api/path?from={from_id}&to={to_id}")

    def stats(self):
        return self.request("GET", "/api/stats")

    def save(self):
        return self.request("POST", "/api/admin/save")

    def clear(self):
        return self.request("POST", "/api/admin/clear", {"confirm": True})


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "8080"
    base = f"http://127.0.0.1:{port}"

    print("== 健康检查 ==")
    print(GraphClient(base).request("GET", "/api/health"))

    print("\n== 建两张图：social 与 warehouse ==")
    root = GraphClient(base)
    for name in ("social", "warehouse"):
        try:
            root.create_graph(name)
            print(f"  已创建 {name}")
        except RuntimeError as e:
            print(f"  {name}: {e}")
    for g in root.graphs()["items"]:
        print(f"  {g['name']:<12} 节点 {g['nodes']:<3} 关系 {g['relationships']:<3} "
              f"{'（默认图）' if g['isDefault'] else ''}")

    db = root.using("social")
    print("\n== 清空 social 后重建一个社交小图 ==")
    db.clear()
    alice = db.create_node(["Person"], {"name": "Alice", "age": 30, "city": "北京"})
    bob = db.create_node(["Person"], {"name": "Bob", "age": 26, "city": "上海"})
    carol = db.create_node(["Person", "Engineer"], {"name": "Carol", "age": 33, "city": "北京"})
    acme = db.create_node(["Company"], {"name": "Acme"})
    print(f"节点 id: Alice={alice} Bob={bob} Carol={carol} Acme={acme}")

    db.create_rel(alice, bob, "KNOWS", {"since": 2019})
    db.create_rel(bob, carol, "KNOWS", {"since": 2020})
    db.create_rel(alice, acme, "WORKS_AT", {"since": 2021, "roles": ["dev", "lead"]})
    print("已创建 3 条关系（其中一条带数组属性 roles）")

    print("\n== 数组 / 对象属性 ==")
    db.update_node(alice,
                   props={"tags": ["vip", "early-bird"],
                          "addr": {"city": "北京", "zip": "100000"}})
    print("  Alice:", db.get_node(alice)["props"])
    db.update_node(alice, array_append={"tags": ["gold"]},
                   object_merge={"addr": {"street": "长安街"}})
    print("  追加 tags / 合并 addr 后:", db.get_node(alice)["props"])
    db.update_node(alice, array_remove={"tags": ["early-bird"]})
    print("  删除一个 tag 后:", db.get_node(alice)["props"]["tags"])

    print("\n== 条件查询：tags 含 vip 的 Person ==")
    for n in db.query("Person", {"tags": {"$contains": "vip"}})["items"]:
        print("  ", n["id"], n["props"].get("name"), n["props"].get("tags"))

    print("\n== 条件查询：age >= 30 ==")
    for n in db.query("Person", {"age": {"$gte": 30}})["items"]:
        print("  ", n["id"], n["props"].get("name"), n["props"].get("age"))

    print("\n== 住在北京的 Person ==")
    for n in db.query("Person", {"city": "北京"})["items"]:
        print("  ", n)

    print("\n== Alice 的出边邻居 ==")
    for n in db.neighbors(alice, "out")["neighbors"]:
        print("  ", n["id"], n["labels"], n["props"].get("name"))

    print("\n== Alice -> Carol 的最短路径（无权，只看跳数）==")
    print("  ", db.shortest_path(alice, carol))

    print("\n== 边权：在城市之间修三条带长度的道路 ==")
    r_ab = db.create_rel(alice, bob, "ROAD", {"name": "京沪线"}, weight=5.0)
    r_bc = db.create_rel(bob, carol, "ROAD", {"name": "沪穗线"}, weight=3.0)
    r_ac = db.create_rel(alice, carol, "ROAD", {"name": "直达线"}, weight=20.0)
    print(f"  道路 id: A->B={r_ab}(5.0)  B->C={r_bc}(3.0)  A->C={r_ac}(20.0)")
    print("  注意：Alice->Bob 之间还躺着一条 KNOWS 边（没设权，权重是默认 1.0），")
    print("        所以下面算最短路时用 types=[\"ROAD\"] 限定只看道路。")

    print("\n== 带权最短路：Alice -> Carol（只看 ROAD，应绕道 Bob：5+3=8 < 20）==")
    wp = db.weighted_path(alice, carol, types=["ROAD"])
    print(f"  路径 {wp['nodes']}  总代价 {wp['cost']}  每跳权重 {wp['legWeights']}  "
          f"权重来源 {wp['weightSource']}")
    print("  对照（不限制类型时会走 KNOWS，因为它权重是默认 1.0）:",
          db.weighted_path(alice, carol)["cost"])

    print("\n== 单条改权：把直达线降到 6.0 ==")
    print("  ", db.set_weight(r_ac, 6.0)["weight"])
    wp2 = db.weighted_path(alice, carol, types=["ROAD"])
    print(f"  新的最短路 {wp2['nodes']}  总代价 {wp2['cost']}  每跳权重 {wp2['legWeights']}")

    print("\n== 批量设权 ==")
    print("  ", db.set_weights({r_ab: 1.5, r_bc: 1.5, r_ac: 9.0}))
    wp3 = db.weighted_path(alice, carol, types=["ROAD"])
    print(f"  再算一次 {wp3['nodes']}  总代价 {wp3['cost']}  每跳权重 {wp3['legWeights']}")

    print("\n== 边权统计 ==")
    print("  ", db.weight_stats())

    print("\n== 按权重筛选：只看权重 >= 5 的边 ==")
    for r in db.list_rels(min_weight=5)["items"]:
        print(f"   #{r['id']}: {r['from']}->{r['to']} [{r['type']}] weight={r['weight']}")

    print("\n== 边权非法值会被拒绝（演示错误处理）==")
    try:
        db.set_weight(r_ab, "很重")
    except RuntimeError as e:
        print("  预期报错:", e)

    print("\n== 另一张图 warehouse 与 social 互不影响 ==")
    wh = root.using("warehouse")
    wh.clear()
    wh.create_node(["Item"], {"sku": "A-001", "stock": 100})
    print("  warehouse 统计:", {k: wh.stats()[k] for k in ("graph", "nodes", "relationships")})
    print("  social    统计:", {k: db.stats()[k] for k in ("graph", "nodes", "relationships")})

    print("\n== 改：给 Alice 加年龄与标签 ==")
    db.update_node(alice, props={"age": 31}, add_labels=["Manager"])
    print("  ", db.get_node(alice))

    print("\n== 删：移除 Bob ==")
    print("  ", db.delete_node(bob))
    print("  Carol 是否还在:", db.get_node(carol)["props"]["name"])

    print("\n== 最终统计 ==")
    s = db.stats()
    print(f"  图 {s['graph']}: 节点 {s['nodes']} 个, 关系 {s['relationships']} 条, 标签 {list(s['labels'])}")

    print("\n== 落盘（全部图） ==")
    print("  ", root.save())


if __name__ == "__main__":
    main()
