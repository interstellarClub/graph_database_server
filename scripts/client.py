#!/usr/bin/env python3
"""Python 客户端示例：演示如何用 GET / POST 调用 cpp_bridge。

用法:
    python3 scripts/client.py
"""
import json
import urllib.error
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:8080"
ROUTE = "/invoke"


def call_post(payload: dict) -> dict:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        BASE + ROUTE,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def call_get(payload: dict) -> dict:
    """GET 方式一：把整个 JSON urlencode 后放进 input 参数"""
    query = urllib.parse.urlencode({"input": json.dumps(payload, ensure_ascii=False)})
    with urllib.request.urlopen(f"{BASE}{ROUTE}?{query}", timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def call_get_params(**kv) -> dict:
    """GET 方式二：直接传键值，服务端自动组装成 JSON 对象"""
    query = urllib.parse.urlencode(kv)
    with urllib.request.urlopen(f"{BASE}{ROUTE}?{query}", timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def show(title: str, result: dict) -> None:
    print(f"--- {title} ---")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    print()


if __name__ == "__main__":
    try:
        show("POST", call_post({"op": "add", "a": 12, "b": 30}))
        show("GET (?input=)", call_get({"op": "mul", "a": 6, "b": 7}))
        show("GET (?k=v)", call_get_params(op="sub", a=100, b=58))
    except urllib.error.URLError as e:
        print(f"请求失败: {e}\n请先启动服务: ./bin/cpp_bridge --cmd ./bin/calc_worker")
