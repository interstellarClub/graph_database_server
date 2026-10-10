# graph_database_server —— C++ 图数据库服务器（内核 + HTTP + 服务，三合一单仓库）

> 一个**零第三方依赖**的 C++17 工程：自带图数据库内核、自带 HTTP 服务器，开箱即得一个
> **可以通过网络请求对图数据库执行增删查改（CRUD）** 的服务程序，也可以只取其中某一层单独使用。

**本仓库由三个原本独立的项目合并而成**，现在是**一个整体编译的单一工程**：

| 原项目 | 在本仓库中的位置 | 职责 |
| --- | --- | --- |
| `minigraph` | `src/minigraph/` + `include/minigraph/` | 图数据库**内核**：节点/关系/标签/属性/索引/遍历/持久化 |
| `cpp_http_bridge` | `src/http/` + `include/*.hpp` | **HTTP 层**：POSIX socket 服务器、子进程桥接、零依赖 JSON 库 |
| `graph_server` | `src/graph_server/` + `include/graph_server/` + `src/graph_server_main.cpp` | **服务层**：多图管理、HTTP 路由、JSON↔Value 转换、图算法封装 |

```
                    ┌──────────────────────────────────────────────────────────┐
                    │                graph_database_server（本仓库）              │
                    └──────────────────────────────────────────────────────────┘

    HTTP 请求                                                        二进制落盘
 curl / 浏览器 ────────> [ HttpServer ] ──> [ GraphService ] ──> [ MiniGraph ] ──> data/*.mgdb
       <──────────────    （src/http）       （src/graph_server）   （src/minigraph）
        JSON 响应          每连接一线程        路由 + 互斥锁          索引/遍历/持久化
                                                    │
                                                    └─ 多张图：social.mgdb / warehouse.mgdb / ...
```

**一句话概括三层关系**：`MiniGraph` 负责"把图存对"，`HttpServer` 负责"把字节收发对"，
`GraphService` 负责"把 HTTP 语义翻译成图操作"。三层之间只通过头文件契约通信，任何一层都可以被单独替换。

---

## 目录

- [一、快速开始](#一快速开始)
- [二、目录结构](#二目录结构)
- [三、第一层：minigraph 图数据库内核](#三第一层minigraph-图数据库内核)
- [四、第二层：cpp_http_bridge HTTP 层](#四第二层cpp_http_bridge-http-层)
- [五、第三层：graph_server 图数据库网络服务](#五第三层graph_server-图数据库网络服务)
- [六、完整接口清单](#六完整接口清单)
- [七、Python 客户端](#七python-客户端)
- [八、设计要点](#八设计要点)
- [九、测试](#九测试)
- [十、平台与注意事项](#十平台与注意事项)
- [附录 A：与原三项目的对照](#附录-a与原三项目的对照)

---

## 一、快速开始

### 1.1 编译

**方式 A：Makefile（推荐，只需 g++ / clang++ + make，无需 cmake）**

```bash
cd graph_database_server
make                 # 一次编译全部 7 个目标，产物在 bin/
```

**方式 B：CMake**

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j4
```

两种方式的产物都输出到 `bin/`。

### 1.2 构建目标一览

| 可执行文件 | 源码入口 | 说明 |
| --- | --- | --- |
| `bin/graph_server` | `src/graph_server_main.cpp` | **主程序**：图数据库网络服务（HTTP CRUD + 可视化控制台） |
| `bin/cpp_bridge` | `src/cpp_bridge_main.cpp` | 把本地控制台程序包装成 HTTP 接口的桥接服务 |
| `bin/minigraph_demo` | `examples/minigraph_demo.cpp` | 图数据库内核演示程序（含交互式 shell） |
| `bin/embedded_server` | `examples/embedded_server.cpp` | 不用子进程、直接把 HTTP 嵌进自己程序的范例 |
| `bin/echo_worker` | `examples/echo_worker.cpp` | 被桥接的示例程序（回显，含 stderr 演示） |
| `bin/calc_worker` | `examples/calc_worker.cpp` | 被桥接的示例程序（四则运算） |
| `bin/json_tests` | `tests/test_json.cpp` | 零依赖 JSON 库自测（28 项） |

Makefile 还提供更便捷的组合命令：

| 命令 | 作用 |
| --- | --- |
| `make run` | 编译并启动图数据库服务（`127.0.0.1:8080`，数据目录 `data/`，控制台 `web/index.html`） |
| `make bridge` | 编译并启动 HTTP 桥接（桥接 `bin/calc_worker`，端口 `8081`） |
| `make test` | 编译后跑冒烟测试：JSON 自测 + API 回归 + 边权专项 |
| `make smoke` | 编译后跑桥接端到端冒烟测试 |
| `make clean` | 清理 `build/` 与 `bin/` |

### 1.3 启动图数据库服务

```bash
./bin/graph_server --dir data --graph default
```

启动后：

- 浏览器打开 <http://127.0.0.1:8080/> —— **可视化控制台**（可切换 / 新建 / 删除图）
- `curl http://127.0.0.1:8080/api/health` —— 健康检查
- `curl http://127.0.0.1:8080/api/graphs` —— 图（数据库）列表
- `curl http://127.0.0.1:8080/api/manifest` —— 接口清单（自描述，服务自己告诉你有哪些接口）

`Ctrl-C` 退出，退出前自动落盘所有图。

### 1.4 启动 HTTP 桥接服务（可选）

```bash
./bin/cpp_bridge --port 8081 --cmd ./bin/calc_worker
curl -X POST http://127.0.0.1:8081/invoke -H 'Content-Type: application/json' \
     -d '{"op":"add","a":3,"b":4}'
# => {"exit_code":0,"timed_out":false,"duration_ms":4,...,"data":{"ok":true,"result":7.0},"ok":true,"error":""}
```

### 1.5 运行内核演示（可选）

```bash
mkdir -p /tmp/mg && cd /tmp/mg
/path/to/graph_database_server/bin/minigraph_demo            # 跑一遍完整演示
/path/to/graph_database_server/bin/minigraph_demo --shell    # 交互式命令行，输入 help 看命令
```

---

## 二、目录结构

```
graph_database_server/
├── CMakeLists.txt                  整体构建（可选）
├── Makefile                        整体构建（推荐，直接 g++/clang++）
├── README.md                       本文档
├── include/                        ── 对外头文件（三层共用同一根目录）
│   ├── minigraph/
│   │   ├── Value.h                 属性值类型（null/int/double/bool/string/array/object）+ Props
│   │   ├── Types.h                 Node / Relationship / Direction / WeightedPath / WeightStats
│   │   ├── Storage.h               二进制读写器 + 文件格式定义
│   │   └── GraphDB.h               ★ 内核唯一的对外接口
│   ├── graph_server/
│   │   ├── JsonValue.h             JSON <-> Value 类型转换（含数组/对象递归转换）
│   │   └── GraphService.h          服务层：多图管理 + 路由 + 图操作
│   ├── json.hpp                    零依赖 JSON 解析/序列化库（单头文件）
│   ├── http_server.hpp             HTTP 服务器接口
│   └── child_process.hpp           子进程管道调用接口
├── src/
│   ├── minigraph/                  ── 第一层：图数据库内核
│   │   ├── Value.cpp               值的类型转换与序列化
│   │   ├── Storage.cpp             快照读写（tmp + rename 原子落盘）
│   │   └── GraphDB.cpp             CRUD、索引、查询、图算法、持久化
│   ├── http/                       ── 第二层：HTTP 层
│   │   ├── http_server.cpp         POSIX socket：解析请求、每连接一线程
│   │   └── child_process.cpp       fork/exec + 三路非阻塞管道 + poll + 超时 SIGKILL
│   ├── graph_server/               ── 第三层：服务层
│   │   ├── JsonValue.cpp
│   │   └── GraphService.cpp        多图管理 + CRUD + 遍历 + 算法 + 索引 + 查询操作符
│   ├── graph_server_main.cpp       主程序入口：启动参数、CORS、静态页、信号退出
│   └── cpp_bridge_main.cpp         桥接服务入口：参数、路由、输入输出组装
├── examples/
│   ├── minigraph_demo.cpp          内核演示 + 交互式 shell
│   ├── embedded_server.cpp         嵌入式 HTTP 范例
│   ├── echo_worker.cpp             被桥接示例（回显）
│   └── calc_worker.cpp             被桥接示例（四则运算）
├── tests/test_json.cpp             JSON 库自测（28 项）
├── scripts/
│   ├── api_test.sh                 图数据库服务接口回归（53 项 + 9 类预期错误）
│   ├── weight_test.sh              边权专项测试（26 项）
│   ├── smoke_test.sh               桥接端到端冒烟测试
│   ├── demo.py                     Python 客户端 GraphClient + 示例
│   └── client.py                   桥接服务 Python 客户端示例
├── web/index.html                  可视化控制台（可切换图）
├── data/                           数据目录（一张图一个 <图名>.mgdb）
└── docs/
    ├── 请求参数详解.md             全部 HTTP 接口的参数逐项说明
    └── graph_server技术说明书.docx  技术实现说明书
```

> **为什么 `include/` 只有一份？** 三个子项目原本各自带一份 `include/`，合并后统一到仓库根的
> `include/` 下，用子目录区分归属（`minigraph/`、`graph_server/`）与平铺的 `.hpp`。
> 所有源文件都通过 `-Iinclude` 引用，因此**源码本身一行未改**，只是换了落盘位置。

---

## 三、第一层：minigraph 图数据库内核

参考 Neo4j 的核心概念（节点 / 关系 / 标签 / 属性 / 索引 / 遍历）实现的图数据库库，
**零第三方依赖**，数据以**二进制文件**持久化。目标是：功能够用、代码好读、方便改。

### 3.1 三分钟上手

```cpp
#include "minigraph/GraphDB.h"
using namespace minigraph;

GraphDB db("social.mgdb");     // 数据文件（二进制）
db.open();                     // 有则载入，无则新建

// 增
uint64_t alice = db.createNode({"Person"}, {{"name", "Alice"}, {"age", 30}});
uint64_t bob   = db.createNode({"Person"}, {{"name", "Bob"},   {"age", 26}});
db.createRelationship(alice, bob, "KNOWS", {{"since", 2019}, {"weight", 2.0}});

// 查
db.findNodesByLabel("Person");                     // 按标签
db.findNodes("Person", "city", Value("Beijing"));  // 按标签 + 属性
db.findNodes([](const Node& n){                    // 自定义谓词全表扫描
    return n.props.count("age") && n.props.at("age").asInt() > 25;
});

// 改
db.setNodeProperty(alice, "age", Value(31));
db.updateNode(bob, {{"city", "Hangzhou"}});

// 遍历 / 算法
db.neighbors(alice, Direction::Out, {"KNOWS"});    // 邻居
db.shortestPath(alice, bob);                       // 最少跳数路径（BFS）
db.shortestWeightedPath(alice, bob);               // Dijkstra 带权最短路（用一等 weight 字段）

// 数组 / 对象属性（可任意嵌套）
db.createNode({"Person"}, {
    {"name", "Erin"},
    {"tags", Value(ValueList{Value("vip"), Value("early-bird")})},
    {"addr", Value(ValueMap{{"city", Value("Beijing")}, {"zip", Value("100000")}})},
});
const Node* n = db.getNode(6);
n->props.at("tags").at(0)->asString();          // "vip"
n->props.at("addr").at("city")->asString();     // "Beijing"
n->props.at("tags").contains(Value("vip"));     // true

// 删（删节点会级联删除其所有关系）
db.removeNode(bob);
db.removeRelationship(1);

db.save();   // 落盘（析构时也会兜底保存一次）
```

### 3.2 属性值类型（`Value`）

`Value` 支持 7 种类型（与 JSON 一一对应）：
`null` / `int` / `double` / `bool` / `string` / `array`（`ValueList = std::vector<Value>`）/
`object`（`ValueMap = std::map<std::string, Value>`）。

- 数组与对象**可任意嵌套**
- 二级索引只收录标量值；按数组/对象值查找时自动退化为全表扫描
- 容器不参与 `propIndex_` 建索引，但标签索引、关系类型索引照常生效

### 3.3 API 一览（全部在 `GraphDB` 上）

**生命周期 / 持久化**

| 函数 | 说明 |
| --- | --- |
| `GraphDB(path)` | 构造，指定二进制数据文件路径 |
| `open()` | 打开；文件不存在则创建空库 |
| `save()` | 立即落盘 |
| `close()` | 保存并清空内存 |
| `setAutoSave(bool)` | 每次修改是否自动落盘（默认 `true`） |
| `clear()` | 清空全部数据 |
| `dirty()` / `path()` / `isOpen()` | 是否有未落盘改动 / 文件路径 / 是否已打开 |

**节点 CRUD**：`createNode(labels, props)` · `getNode(id)` · `setNodeProperty` ·
`removeNodeProperty` · `updateNode`（合并）· `replaceNodeProperties`（整体替换）·
`addNodeLabel` · `removeNodeLabel` · `removeNode`（级联删关系）

**关系 CRUD**：`createRelationship(from, to, type, props, weight)`（失败返回 0）·
`getRelationship(id)` · `setRelationshipProperty` · `removeRelationshipProperty` ·
`updateRelationship` · `removeRelationship`

**边权（一等字段）**

| 函数 | 说明 |
| --- | --- |
| `createRelationship(..., double weight = 1.0)` | 建边时直接给权 |
| `setRelationshipWeight(id, w)` | 单条设权；`w` 必须是有限数（非 NaN/Inf），负数允许存 |
| `getRelationshipWeight(id, out)` | 取单条边权重 |
| `setRelationshipWeights(map<id, w>)` | 批量设权，返回成功条数（非法值/不存在的 id 跳过） |
| `weightStats()` | 全库统计：`sum` / `min` / `max` / `avg` / `weightedCount` |
| `findRelationshipsByWeight(minW, maxW)` | 按权重闭区间筛选 |
| `isValidWeight(w)` | 静态：是否为有限数 |

**查询**：`findNodesByLabel(label)` · `findNodes(label, key, value)`（有索引走索引，无索引全表扫描）·
`findNodes(predicate)` · `findRelationships(type)` · `findRelationships(from, to, dir, types)`

**索引**：`createIndex(label, key)` · `dropIndex` · `hasIndex` · `listIndexes`
> 索引定义会随数据文件一起持久化，重启后不用重建。标签索引与关系类型索引内置自动维护，无需手动创建。

**遍历与算法**：`neighbors(id, dir, types)` · `degree(id, dir, types)` ·
`incidentRelationships(id, dir, types)` · `reachable(start, maxDepth, dir, types)` ·
`shortestPath(from, to, dir, types)`（BFS，跳数最少）·
`shortestWeightedPath(from, to, weightKey, dir, types)`（Dijkstra）

> `types` 传空 `{}` 表示不限关系类型；`dir` 取 `Direction::Out / In / Both`。
> `shortestWeightedPath` 的 `weightKey` 为空时用一等 `weight` 字段（推荐）；非空则从 `props[weightKey]`
> 取值（兼容旧用法，缺失按 `1.0`）。路径上出现负权边时，结果的 `negativeWeight` 会置 `true`，
> 表示 `cost` 不可信（Dijkstra 不支持负权）。

**统计**：`nodeCount()` · `relationshipCount()` · `allNodeIds()` · `allRelationshipIds()` · `printStats(cout)`

### 3.4 二进制文件格式

```
[Header ] magic "MINIGDB1"(8B) | version(u32) | nodeCount(u64) | relCount(u64)
          | indexCount(u64) | nextNodeId(u64) | nextRelId(u64)
[Nodes  ] id(u64) | labelCount(u32) | labels... | propCount(u32) | (key, value)...
[Rels   ] id(u64) | from(u64) | to(u64) | type(str) | props... | weight(double)
[Indexes] indexCount 条 (label, key)
[Footer ] magic(8B)      ← 读到它说明文件写入完整
```

- `Value` 编码：`type(u8)` + 载荷，`0 null / 1 int(i64) / 2 double / 3 bool / 4 string /
  5 array(u32 元素数 + 元素…) / 6 object(u32 键数 + (key, value)…)`
- 版本：`1` 只有标量；`2` 支持数组/对象；`3` 在关系记录末尾追加 `weight(double)`
  （读 1/2/3 都行；写的时候若数据里没有容器且所有边权都是默认 `1.0`，仍写 v1，旧版程序照样能打开）
- 读 v1/v2 旧文件时自动把 `props["weight"]` 迁移成一等 `weight` 字段，并从 `props` 中移除
  （避免同一份权重存两处）
- 整数、长度全部**显式小端**写入，无结构体直接 `memcpy`，跨机器拷贝无对齐/字节序问题
- 落盘先写 `xxx.mgdb.tmp`，写完再 `rename` 覆盖，避免崩溃留下半截文件
- 读取时校验 magic、version、尾部 magic，损坏会报明确错误而不是静默出错

### 3.5 设计取舍

| 主题 | 说明 |
| --- | --- |
| 时间复杂度 | id 直查 O(1)；标签/属性索引 O(log n + 命中数)；未建索引的属性查询是全表扫描 O(n) |
| 内存模型 | 全量常驻内存，适合百万级以内的图；更大规模可把 `nodes_`/`rels_` 换成磁盘分页结构 |
| 想加事务 | 在 `GraphDB.cpp` 的写操作里记 undo 日志，提交时清空、回滚时反向执行即可 |
| 想加 Cypher | 在 `GraphDB` 之上再加一层解析器，把语句翻译成现有的 `findNodes` / `neighbors` 调用 |
| 想批量导入更快 | `db.setAutoSave(false)`，导完再 `db.save()` |
| id 复用 | 删除后的 id 会进 free list 复用；`nextNodeId`/`nextRelId` 会持久化，重启不会撞 id |

---

## 四、第二层：cpp_http_bridge HTTP 层

用 C++17 从零实现的 HTTP 服务器与「HTTP ⇄ 本地控制台程序」桥接方案，
零第三方依赖（不需要 Boost / Poco / nlohmann-json / libcurl）。

```
        GET  /invoke?input=<JSON>                stdin  (一行 JSON)
Client ─────────────────────────────► [cpp_bridge] ───────────────► 你的控制台程序
        POST /invoke  (body: JSON)     fork+管道    ◄─────────────── stdout (一行 JSON)
       ◄─────────────────────────────  超时/退出码
                     响应 JSON
```

### 4.1 快速开始

```bash
make                                   # 编译
./bin/cpp_bridge --port 8080 --cmd ./bin/calc_worker

curl -X POST http://127.0.0.1:8080/invoke -H 'Content-Type: application/json' \
     -d '{"op":"add","a":3,"b":4}'
curl -G http://127.0.0.1:8080/invoke --data-urlencode 'input={"op":"mul","a":6,"b":7}'
curl 'http://127.0.0.1:8080/invoke?op=sub&a=10&b=4'      # 键值自动组装成 JSON 对象
curl http://127.0.0.1:8080/health                        # 健康检查

bash scripts/smoke_test.sh                               # 一键端到端冒烟测试
python3 scripts/client.py                                # Python 客户端示例
```

### 4.2 被桥接程序的约定（很简单）

你的控制台程序只需遵守两条：

1. 从 **stdin** 读一行 JSON（桥接层写入 `紧凑JSON + "\n"`）；
2. 向 **stdout** 输出一行 JSON。

```cpp
#include "json.hpp"                 // 直接复用本仓库自带的 JSON 库
std::ostringstream ss; ss << std::cin.rdbuf();   // 读全部 stdin
Json in; Json::tryParse(ss.str(), in, &err);
Json out = Json::object(); out.set("result", 42);
std::cout << out.dump() << std::endl;            // 写一行 JSON 到 stdout
return 0;                                        // 退出码会被桥接层采集
```

> stdout 里可以有其他输出吗？不行——桥接层会把 stdout 整体当 JSON 解析；
> 调试信息请写到 stderr（会被单独采集进响应的 `stderr` 字段）。

### 4.3 命令行参数

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| `--cmd <command>` | 要桥接的程序命令，经 `/bin/sh -c` 执行（支持管道、重定向）。省略时进入内置 echo 演示模式 | 无 |
| `--host <addr>` | 监听地址，`0.0.0.0` 允许外部访问 | `127.0.0.1` |
| `--port <port>` | 监听端口 | `8080` |
| `--route <path>` | 业务路由 | `/invoke` |
| `--cwd <dir>` | 子进程工作目录 | 继承当前目录 |
| `--timeout <ms>` | 子进程超时（超时 SIGKILL），`0` 不限时 | `3000` |
| `--max-body <bytes>` | 请求体上限 | `4194304` |
| `--pretty` | 响应 JSON 缩进美化 | 关闭 |
| `--no-newline` | 写 stdin 时不补换行 | 关闭 |
| `--no-cors` | 关闭 CORS 头 | 开启 |
| `-q` / `--quiet` | 不打印访问日志 | 关闭 |

### 4.4 HTTP 接口与错误码

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | 健康检查（存活时长、累计请求数） |
| GET | `/info` | 查看当前桥接配置 |
| GET | `/` | 接口说明 |
| GET | `/invoke?input=<urlencoded JSON>` | 方式一：整个 JSON 塞进 `input` |
| GET | `/invoke?k1=v1&k2=v2` | 方式二：键值自动组装成 JSON 对象（`true/false/null/数字` 自动转成对应类型） |
| POST | `/invoke` | body 为 JSON（`Content-Type: application/json`） |
| OPTIONS | 任意 | CORS 预检 |

**成功响应**

```json
{
  "ok": true,
  "data": { "ok": true, "op": "add", "result": 7.0 },
  "stdout": "{\"ok\":true,...}\n",
  "stderr": "",
  "exit_code": 0,
  "timed_out": false,
  "duration_ms": 4,
  "error": ""
}
```

**异常响应**（HTTP 状态码 + `ok:false` + `error` 说明）

| 场景 | 状态码 |
| --- | --- |
| 请求体不是合法 JSON | 400 |
| 路径不存在 | 404 |
| 请求体超过 `--max-body` | 413 |
| 程序 stdout 不是合法 JSON | 502 |
| 程序执行超时 | 504 |
| 程序无法启动 | 500 |
| 程序退出码非 0 | 200（响应里 `ok:false`，`error` 给出退出码） |

### 4.5 嵌入式集成（不用子进程）

逻辑就在当前进程里时，直接复用 `HttpServer`（`graph_server` 就是这么做的）：

```cpp
#include "http_server.hpp"
#include "json.hpp"

httpbridge::HttpServer server("127.0.0.1", 8081);
server.setHandler([](const httpbridge::HttpRequest& req) {
    Json in = Json::parse(req.body);
    return httpbridge::HttpResponse::json(200, handle(in).dump());
});
server.start();
server.serveForever();
```

完整范例见 `examples/embedded_server.cpp`。

### 4.6 JSON 库用法速查

```cpp
#include "json.hpp"
using minijson::Json;

Json v = Json::parse(R"({"name":"Tom","scores":[98,87.5]})");
v["name"].asString();        // "Tom"
v["scores"][1].asDouble();   // 87.5
v["age"].asIntOr(0);         // 键不存在也不抛异常
v.contains("name");          // true

Json o = Json::object();     // 保持插入顺序
o.set("ok", true);
o.set("n", 42);
o["list"].push_back("a");
std::string s = o.dump();    // 紧凑; o.dump(2) 为缩进美化

Json bad; std::string err;
if (!Json::tryParse(text, bad, &err)) { /* err 含出错偏移量 */ }
```

支持：严格 RFC 8259 语法（拒绝尾逗号、前导 0、单引号）、`\uXXXX` 与代理对（emoji 正常）、
紧凑/美化输出、UTF-8 透传、错误带偏移量。

### 4.7 已验证的行为

| 项目 | 结果 |
| --- | --- |
| JSON 库自测 | 28/28 通过 |
| POST / GET（两种 GET 形式） | 正常 |
| 200KB 大 payload | 正常（非阻塞写+读，无管道死锁，69ms） |
| 并发 20 请求 | 全部 200 |
| 超时（限时 1s / 程序睡 5s） | 1001ms 时 SIGKILL，返回 504、`exit_code:137` |
| 程序非 0 退出 | 响应 `ok:false` 并给出退出码与 stdout |
| 中文 / emoji | 正常透传 |
| CORS 预检 | 正常 |

---

## 五、第三层：graph_server 图数据库网络服务

把图数据库内核与 HTTP 层组合起来，得到一个**可以通过网络请求对图数据库执行增删查改**的服务程序。
**三个核心能力**：

1. **多数据库**：一个进程同时托管多张图，每张图一个 `data/<图名>.mgdb` 文件，彼此数据完全隔离。
2. **数组与对象**：属性值不再只有标量，`null / int / double / bool / string` 之外新增 `array` 与 `object`，可任意嵌套。
3. **边权**：每条关系都带一个 `weight` 字段（默认 `1.0`），可单独/批量设置、按区间筛选，是 Dijkstra 带权最短路唯一认的权重来源。

### 5.1 启动参数

```bash
./bin/graph_server --dir data --graph default
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--host` | `127.0.0.1` | 监听地址 |
| `--port` | `8080` | 监听端口 |
| `--dir` | `data` | **数据目录**，一张图一个 `<图名>.mgdb` |
| `--graph` | `default` | 默认图名（不带任何图标识的请求作用于它） |
| `--data` | （兼容旧参数） | 等价于 `--dir 文件所在目录 --graph 文件名（去 .mgdb）` |
| `--web` | `web/index.html` | 控制台页面 |
| `--no-autosave` | 关 | 关闭自动落盘，需手动 `POST /api/admin/save` |
| `-h` / `--help` | | 帮助 |

### 5.2 通用约定

**响应格式**（所有接口一致）：

```json
{ "ok": true,  "data": { ... } }
{ "ok": false, "error": { "code": "not_found", "message": "节点不存在: 42" } }
```

**HTTP 状态码**：`200` 查询成功 · `201` 创建成功 · `400` 请求有误 ·
`404` 资源不存在 · `405` 方法不支持 · `409` 图已存在 · `500` 服务端错误

**属性值类型**：`null` / 整数 / 浮点 / `true|false` / 字符串 / **数组** / **对象**（可任意嵌套）。

```json
{
  "name": "Alice",
  "age": 30,
  "tags": ["vip", "early-bird"],
  "addr": { "city": "Beijing", "zip": "100000", "geo": { "lat": 39.9, "lng": 116.4 } }
}
```

> 数组/对象会原样落盘并原样返回；嵌套层数上限 32 层（防栈溢出）。
> 二级索引只收录标量值，按数组/对象值查询时自动退化为全表扫描（功能不受影响）。

**通用 query 参数**

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `limit` | 100（上限 1000） | 分页大小 |
| `offset` | 0 | 分页偏移 |
| `full` | 0 | `full=1` 返回完整对象，否则只返回 id 数组 |
| `dir` | `out` | 方向：`out` / `in` / `both` |
| `type` | 不限 | 关系类型过滤，逗号分隔：`type=KNOWS,WORKS_AT` |
| `graph` | 默认图 | 指定操作哪张图 |

### 5.3 多数据库（多张图）

一张图 = 一个独立的 `GraphDB` 实例 = 数据目录里的一个 `<图名>.mgdb` 文件。
启动时自动扫描目录、载入全部图；请求操作哪张图按以下优先级决定：

| 优先级 | 方式 | 示例 |
| --- | --- | --- |
| 1 | 路径前缀 | `/api/g/social/nodes` |
| 2 | 请求头 | `X-Graph: social` |
| 3 | query 参数 | `/api/nodes?graph=social` |
| 4 | 默认图 | 启动参数 `--graph`（缺省 `default`） |

```bash
# 新建图
curl -X POST http://127.0.0.1:8080/api/graphs -H 'Content-Type: application/json' -d '{"name":"social"}'
# {"ok":true,"data":{"name":"social","file":"data/social.mgdb","created":true,"isDefault":false}}

# 列出所有图（含各自的节点/关系数）
curl http://127.0.0.1:8080/api/graphs

# 往指定图里写数据（三种写法等价）
curl -X POST http://127.0.0.1:8080/api/g/social/nodes -H 'Content-Type: application/json' -d '{"labels":["Person"]}'
curl -X POST http://127.0.0.1:8080/api/nodes -H 'X-Graph: social' -H 'Content-Type: application/json' -d '{"labels":["Person"]}'
curl -X POST 'http://127.0.0.1:8080/api/nodes?graph=social' -H 'Content-Type: application/json' -d '{"labels":["Person"]}'

# 某张图的统计 / 落盘 / 清空
curl http://127.0.0.1:8080/api/g/social/stats
curl -X POST http://127.0.0.1:8080/api/graphs/social/save
curl -X POST http://127.0.0.1:8080/api/graphs/social/clear -H 'Content-Type: application/json' -d '{"confirm":true}'

# 删除整张图（危险，需确认；会删掉对应的 .mgdb 文件）
curl -X DELETE 'http://127.0.0.1:8080/api/graphs/social?confirm=1'
```

图名规则：字母、数字、`_`、`-`、`.`、空格，最长 64 字符（不能含斜杠）。
删除默认图后会自动改指另一张图；一张图都不剩时重建空的默认图。

### 5.4 节点接口

**创建**

```bash
curl -X POST http://127.0.0.1:8080/api/nodes \
  -H 'Content-Type: application/json' \
  -d '{"labels":["Person"],"props":{"name":"Alice","age":30,"city":"Beijing"}}'
```

```json
{ "ok": true, "data": { "id": 1,
  "node": { "id": 1, "labels": ["Person"],
            "props": { "age": 30, "city": "Beijing", "name": "Alice" } } } }
```

**查询**

```bash
curl http://127.0.0.1:8080/api/nodes/1                      # 按 id
curl 'http://127.0.0.1:8080/api/nodes?label=Person&full=1'  # 按标签
curl 'http://127.0.0.1:8080/api/nodes?label=Person&key=city&value=Beijing&full=1'
curl 'http://127.0.0.1:8080/api/nodes?label=Person&key=tags&value=vip&op=contains&full=1'
```

**修改**

```bash
# PATCH：增量修改（只动传进来的字段）
curl -X PATCH http://127.0.0.1:8080/api/nodes/1 -H 'Content-Type: application/json' \
  -d '{"props":{"age":31},"addLabels":["Manager"],"removeProps":["city"]}'

# PUT：整体替换 labels 与 props
curl -X PUT http://127.0.0.1:8080/api/nodes/1 -H 'Content-Type: application/json' \
  -d '{"labels":["Person","Manager"],"props":{"name":"Alice","age":31}}'
```

**数组 / 对象属性的增量修改（PATCH 专属）**

| 字段 | 语义 | 示例 |
| --- | --- | --- |
| `arrayAppend` | 追加元素（已存在则跳过，自动去重） | `{"arrayAppend":{"tags":["gold","vip"]}}` |
| `arrayRemove` | 删除匹配的元素 | `{"arrayRemove":{"tags":["vip"]}}` |
| `objectMerge` | 与已有对象浅合并 | `{"objectMerge":{"addr":{"street":"长安街"}}}` |

```bash
curl -X PATCH http://127.0.0.1:8080/api/nodes/1 -H 'Content-Type: application/json' \
  -d '{"arrayAppend":{"tags":["gold"]},"objectMerge":{"addr":{"street":"长安街"}}}'
```

> 属性不存在时会自动创建（`arrayAppend` 建空数组、`objectMerge` 建空对象）；
> 原值类型不是数组/对象时会被覆盖成新的数组/对象。关系的 PATCH 同样支持这三个字段。

**删除（级联删除其所有关系）**

```bash
curl -X DELETE http://127.0.0.1:8080/api/nodes/1
# {"ok":true,"data":{"id":1,"deleted":true,"cascadeRelationships":2}}
```

### 5.5 关系接口

```bash
# 创建（属性同样支持数组/对象）
curl -X POST http://127.0.0.1:8080/api/relationships -H 'Content-Type: application/json' \
  -d '{"from":1,"to":2,"type":"KNOWS","props":{"since":2019,"roles":["dev","lead"]}}'

# 查询
curl http://127.0.0.1:8080/api/relationships/1
curl 'http://127.0.0.1:8080/api/relationships?type=KNOWS&full=1'
curl 'http://127.0.0.1:8080/api/relationships?from=1&to=2'

# 修改（PATCH 合并属性；PUT 替换属性，也可改 type）
curl -X PATCH http://127.0.0.1:8080/api/relationships/1 -H 'Content-Type: application/json' \
  -d '{"props":{"since":2018,"note":"旧同事"}}'

# 删除
curl -X DELETE http://127.0.0.1:8080/api/relationships/1
```

### 5.6 边权（weight）

**每条关系都有一个 `weight` 字段**（一等字段，不在 `props` 里），默认 `1.0`。
它是 Dijkstra 带权最短路唯一认的权重来源。

```bash
# 创建时直接给边权（不传就是 1.0）
curl -X POST http://127.0.0.1:8080/api/relationships -H 'Content-Type: application/json' \
  -d '{"from":1,"to":2,"type":"ROAD","weight":4.5,"props":{"name":"京沪线"}}'
# 响应里的关系对象会带 weight：
# {"id":1,"from":1,"to":2,"type":"ROAD","weight":4.5,"props":{"name":"京沪线"}}

# 查 / 改单条边的权重
curl http://127.0.0.1:8080/api/relationships/1/weight
curl -X PATCH http://127.0.0.1:8080/api/relationships/1/weight \
  -H 'Content-Type: application/json' -d '{"weight":2.5}'

# 也可以在改关系时顺手改（PUT / PATCH 都支持顶层 weight 字段）
curl -X PATCH http://127.0.0.1:8080/api/relationships/1 \
  -H 'Content-Type: application/json' -d '{"weight":2.5,"props":{"note":"限行"}}'

# 批量设权（两种写法都行）
curl -X POST http://127.0.0.1:8080/api/relationships/weights -H 'Content-Type: application/json' \
  -d '{"weights":{"1":0.5,"2":3,"3":1.2}}'
curl -X POST http://127.0.0.1:8080/api/relationships/weights -H 'Content-Type: application/json' \
  -d '{"weights":[[1,0.5],[2,3],[3,1.2]]}'
# -> {"requested":3,"applied":3,"errors":[],"relationships":[{"id":1,"weight":0.5},...]}

# 边权统计
curl http://127.0.0.1:8080/api/relationships/weight-stats
# -> {"count":5,"weightedCount":4,"defaultWeight":1.0,"sum":14.7,"min":0.5,"max":9,"avg":2.94}

# 按权重区间筛选（可与 type/from/to 组合，取交集）
curl 'http://127.0.0.1:8080/api/relationships?minWeight=3&full=1'
curl 'http://127.0.0.1:8080/api/relationships?minWeight=1&maxWeight=2&full=1'
```

**边权的四条规则**

| 情况 | 行为 |
| --- | --- |
| 不传 `weight` | 取默认值 `1.0`（与无权最短路等价，所以老数据不用改就能继续用 Dijkstra） |
| `weight` 不是数字（`"5"`、`{}`） | **400** `weight 必须是数字（整数或小数）` —— 不会静默退化成 1.0 |
| `weight` 是 `NaN` / `Infinity` | **400** `weight 必须是有限数` |
| `weight` 是负数 | 允许存（最长路等场景有用）；但最短路响应会带 `negativeWeight: true` 与 `warning`，明确告诉你 cost 不可信 |

默认权重 `1.0` 是刻意选的：这样"没设过权的边"在带权最短路里表现得和无权最短路一致，
你可以先建图跑通逻辑，再逐步给关键边上权，不会突然算不出路径。

### 5.7 查询、遍历与图算法

**条件查询操作符**

`where` 里既可以直接给值，也可以给操作符对象：

```bash
curl -X POST http://127.0.0.1:8080/api/query -H 'Content-Type: application/json' \
  -d '{"label":"Person","where":{"city":"Beijing","age":{"$gte":30}},"full":true}'
```

| 操作符 | 含义 | 对数组/对象的作用 |
| --- | --- | --- |
| （直接给值） | 相等（数值跨 int/double 互通） | 数组/对象逐项比较 |
| `$ne` | 不等（属性不存在也算不等） | 同上 |
| `$gt` `$gte` `$lt` `$lte` | 大小比较（数值按数值，其余按字符串） | — |
| `$in` | 值在给定数组里 | — |
| `$contains` | 包含 | 数组含某元素 / 对象含某值 / 字符串含子串 |
| `$has` | 含某键 | 对象含某键（数组退化为 `$contains`） |
| `$size` | 长度相等 | 数组长度 / 对象键数 / 字符串长度 |

```bash
# 标签含 vip 的人
curl -X POST http://127.0.0.1:8080/api/query -H 'Content-Type: application/json' \
  -d '{"label":"Person","where":{"tags":{"$contains":"vip"}},"full":true}'

# 地址里填了邮编的人
curl -X POST http://127.0.0.1:8080/api/query -H 'Content-Type: application/json' \
  -d '{"label":"Person","where":{"addr":{"$has":"zip"}},"full":true}'
```

**遍历与算法接口**

| 功能 | 请求 | 说明 |
| --- | --- | --- |
| 条件查询 | `POST /api/query` `{"label":"Person","where":{...},"full":true}` | 多条件；单标量条件 + 标签时自动走二级索引 |
| 条件查询(GET) | `GET /api/query?label=Person&key=tags&value=vip&op=contains&full=1` | 同上，参数走 query string |
| 邻居 | `GET /api/nodes/1/neighbors?dir=out&type=KNOWS&full=1` | 一跳邻居 |
| 度数 | `GET /api/nodes/1/degree?dir=both` | 出/入/总度 |
| 相连关系 | `GET /api/nodes/1/relationships?dir=both&full=1` | 返回关系对象 |
| N 跳可达 | `GET /api/reachable/1?depth=2&full=1` | BFS |
| 最短路径 | `GET /api/path?from=1&to=3&dir=out` | 跳数最少 |
| 带权最短路径 | `POST /api/path/weighted` `{"from":1,"to":3}` | Dijkstra，权重取关系的一等 `weight` |
| 带权最短路(GET) | `GET /api/path/weighted?from=1&to=3&type=ROAD` | 同上，参数走 query string |

带权最短路的响应会告诉你权重是怎么来的、每一跳各是多少：

```bash
curl -X POST http://127.0.0.1:8080/api/path/weighted -H 'Content-Type: application/json' \
  -d '{"from":1,"to":4,"types":["ROAD"]}'
```

```json
{
  "ok": true,
  "data": {
    "found": true,
    "cost": 8.0,
    "weightSource": "weight",
    "weightKey": "",
    "nodes": [1, 2, 3],
    "relationships": [4, 5],
    "legWeights": [5.0, 3.0],
    "negativeWeight": false
  }
}
```

- `weightSource` —— `"weight"` 表示用一等字段；`"props.xxx"` 表示从属性里取。
- 想沿用旧用法（权重存在属性里）就传 `weightKey`，例如 `{"weightKey":"cost"}`
  会从 `props.cost` 取值；属性缺失的那条边按 `1.0` 算。
- `types` 用来限定边类型。这一步很关键：如果你图里同时有 `KNOWS`（没设权，权重默认 1.0）
  和 `ROAD`（权重 5.0），不限类型的话最短路会走 `KNOWS` —— 因为它更"便宜"。

> 以上所有路径都可以加图前缀，如 `/api/g/social/query`。

### 5.8 索引与运维

```bash
curl -X POST http://127.0.0.1:8080/api/indexes -H 'Content-Type: application/json' \
  -d '{"label":"Person","key":"name"}'      # 建二级索引，加速 (标签,属性) 查找
curl http://127.0.0.1:8080/api/indexes      # 索引列表
curl -X DELETE 'http://127.0.0.1:8080/api/indexes?label=Person&key=name'

curl http://127.0.0.1:8080/api/stats        # 当前图统计：节点/关系数、标签分布、索引、文件大小
curl http://127.0.0.1:8080/api/graphs       # 所有图一览
curl -X POST http://127.0.0.1:8080/api/admin/save          # 全部图落盘
curl -X POST http://127.0.0.1:8080/api/admin/clear \
  -H 'Content-Type: application/json' -d '{"confirm":true}' # 清空当前图
curl http://127.0.0.1:8080/api/health       # 健康检查（含图列表）
curl http://127.0.0.1:8080/api/manifest     # 接口清单（自描述）
```

---

## 六、完整接口清单

服务自带 `GET /api/manifest` 输出同样的清单（机器可读）。以下为全部 41 个接口
（manifest 中列出 40 个，另有一个 `GET /api/path/weighted` 未登记在清单里，但同样可用）：

| # | 方法 | 路径 | 说明 |
| --- | --- | --- | --- |
| 1 | GET | `/api/health` | 健康检查（含全部图的概况） |
| 2 | GET | `/api/manifest` | 接口清单 |
| 3 | GET | `/api/graphs` | 列出所有图（数据库） |
| 4 | POST | `/api/graphs` | 新建图 `{"name":"social"}` |
| 5 | GET | `/api/graphs/{name}` | 某张图的统计信息 |
| 6 | DELETE | `/api/graphs/{name}` | 删除整张图（需 `?confirm=1`） |
| 7 | POST | `/api/graphs/{name}/save` | 只落盘这张图 |
| 8 | POST | `/api/graphs/{name}/clear` | 清空这张图（需 `{"confirm":true}`） |
| 9 | GET | `/api/stats` | 当前图的统计信息 |
| 10 | GET | `/api/g/{图名}/stats` | 指定图的统计信息 |
| 11 | POST | `/api/nodes` | 创建节点 `{labels:[], props:{}}`（属性值可为数组/对象） |
| 12 | GET | `/api/nodes` | 列出节点，支持 `label/key/value/op/full/limit/offset` |
| 13 | GET | `/api/nodes/{id}` | 查询单个节点 |
| 14 | PUT | `/api/nodes/{id}` | 整体替换节点的 labels 与 props |
| 15 | PATCH | `/api/nodes/{id}` | 增量修改：`props/addLabels/removeLabels/removeProps/arrayAppend/arrayRemove/objectMerge` |
| 16 | DELETE | `/api/nodes/{id}` | 删除节点（级联删除其关系） |
| 17 | GET | `/api/nodes/{id}/neighbors` | 邻居节点 `?dir=out|in|both&type=&full=` |
| 18 | GET | `/api/nodes/{id}/degree` | 度数 `?dir=&type=` |
| 19 | GET | `/api/nodes/{id}/relationships` | 相连的关系 `?dir=&type=` |
| 20 | POST | `/api/relationships` | 创建关系 `{from,to,type,props,weight?}`（`weight` 默认 `1.0`） |
| 21 | GET | `/api/relationships` | 列出关系，支持 `type/from/to/dir/full/limit/offset/minWeight/maxWeight` |
| 22 | GET | `/api/relationships/{id}` | 查询单个关系（含 `weight`） |
| 23 | PUT | `/api/relationships/{id}` | 整体替换关系属性（也可改 `type` / `weight`） |
| 24 | PATCH | `/api/relationships/{id}` | 增量修改关系（也可改 `type` / `weight`） |
| 25 | DELETE | `/api/relationships/{id}` | 删除关系 |
| 26 | GET | `/api/relationships/{id}/weight` | 查询单条边的边权 |
| 27 | PUT | `/api/relationships/{id}/weight` | 设置单条边的边权 `{"weight":3.5}` |
| 28 | PATCH | `/api/relationships/{id}/weight` | 同 PUT |
| 29 | POST | `/api/relationships/weights` | 批量设权 `{"weights":{"1":2.5,"2":0.5}}` 或 `[[1,2.5]]` |
| 30 | GET | `/api/relationships/weight-stats` | 边权统计：`count/weightedCount/sum/min/max/avg` |
| 31 | POST | `/api/query` | 条件查询 `{label, where:{k:v 或 k:{$op:v}}, limit, offset, full}` |
| 32 | GET | `/api/query` | 同上，参数走 query string（`op=contains/in/gt/...`） |
| 33 | GET | `/api/reachable/{id}` | N 跳内可达节点 `?depth=&dir=&type=` |
| 34 | GET | `/api/path` | 无权最短路径 `?from=&to=&dir=&type=` |
| 35 | POST | `/api/path/weighted` | 带权最短路径 `{from,to,weightKey,dir,types}` |
| 36 | GET | `/api/path/weighted` | 同上，参数走 query string |
| 37 | GET | `/api/indexes` | 列出二级索引 |
| 38 | POST | `/api/indexes` | 创建索引 `{label, key}` |
| 39 | DELETE | `/api/indexes` | 删除索引 `?label=&key=` |
| 40 | POST | `/api/admin/save` | 全部图立即落盘 |
| 41 | POST | `/api/admin/clear` | 清空当前图（需 `{"confirm":true}`） |

> 除 `/api/graphs*`、`/api/manifest`、`/api/health`、`/api/admin/*` 外，
> 其余接口均可加图前缀 `/api/g/{图名}/...`（或用 `X-Graph` 头 / `?graph=`）。

更详细的逐参数说明见 [`docs/请求参数详解.md`](docs/请求参数详解.md)。

---

## 七、Python 客户端

`scripts/demo.py` 内置了一个 `GraphClient`，把 HTTP 细节封装成图操作语义：

```bash
./bin/graph_server --dir data &
python3 scripts/demo.py 8080
```

```python
root = GraphClient("http://127.0.0.1:8080")
root.create_graph("social")            # 建图
db = root.using("social")              # 之后所有操作都作用于 social（X-Graph 头）

alice = db.create_node(["Person"], {"name": "Alice", "age": 30,
                                    "tags": ["vip"], "addr": {"city": "北京"}})
bob   = db.create_node(["Person"], {"name": "Bob",   "age": 26})
db.create_rel(alice, bob, "KNOWS", {"since": 2019})

db.update_node(alice, array_append={"tags": ["gold"]},
                      object_merge={"addr": {"street": "长安街"}})
db.query("Person", {"tags": {"$contains": "vip"}})   # 数组包含查询
db.neighbors(alice, "out")
db.shortest_path(alice, bob)
db.delete_node(bob)
root.save()                                          # 全部落盘
```

边权相关的几个方法：

```python
r = db.create_rel(alice, bob, "ROAD", {"name": "京沪线"}, weight=5.0)  # 建带权边
db.set_weight(r, 2.5)                     # 单条改权
db.set_weights({r: 1.5, 2: 3.0})          # 批量设权
db.weight_stats()                         # 边权统计
db.weighted_path(alice, carol, types=["ROAD"])   # 带权最短路（限定只看 ROAD）
db.list_rels(min_weight=5)                # 按权重筛选
```

> `weighted_path` 记得传 `types`：同一对节点之间若还有没设权的边（权重默认 1.0），
> Dijkstra 会优先走那条更"便宜"的边，结果看起来就像边权没生效。

桥接服务也有一个 Python 客户端示例：`scripts/client.py`。

---

### 7.1 通用脚本客户端 `scripts/graph_client.py`

`scripts/graph_client.py` 是一个**开箱即用的全接口 Python 客户端**，只用标准库
（`urllib` + `json`），无需 `pip install`。它的设计目标是：

- **连接参数集中在脚本头部**的 `CONFIG` 字典里（IP、端口、图名……），改配置不动函数；
- **每个函数只收一个字典**，键用 `snake_case`，值就是业务数据；
- 覆盖服务端**全部 41 个接口**（37 个语义化函数 + `request()` / `call()` 逃生舱）；
- 返回值已剥掉 `{"ok":..., "data":...}` 信封，直接拿到 `data`；出错抛 `GraphAPIError`。

#### 7.1.1 配置字典（脚本头部）

```python
CONFIG = {
    "host": "127.0.0.1",       # 图数据库服务 IP 地址或域名
    "port": 8080,              # 图数据库服务端口号
    "graph": "default",        # 默认操作的图（数据库）名
    "route": "path",           # 图路由方式：path=路径前缀 / header=X-Graph头 / query=?graph=
    "timeout": 10,             # 单次请求超时秒数
    "raise_on_error": True,    # 服务端 ok=false 时抛异常；False 则原样返回信封
    "no_proxy": True,          # 主机是本机地址时自动绕过系统 HTTP 代理
}
```

改配置的两种姿势：

```python
import graph_client as gc            # 或 from scripts import graph_client as gc

gc.configure({"host": "192.168.1.9", "port": 8099, "graph": "social"})  # 全局改
gc.current_config()                  # 查看当前配置

gc.create_node({"labels": ["Person"], "graph": "warehouse"})            # 单次换图
```

`host` 也可以直接写完整 URL（`CONFIG["host"] = "https://gdb.example.com"`，此时 `port` 被忽略）。
`route` 三种方式完全等价，默认 `path`（优先级最高、最不容易被代理/网关改写）：

| route | 实际发出的请求 |
| --- | --- |
| `path` | `POST /api/g/social/nodes` |
| `header` | `POST /api/nodes` + 头 `X-Graph: social` |
| `query` | `POST /api/nodes?graph=social` |

#### 7.1.2 函数总表

返回值统一是响应里的 `data`。下表括号中是该函数 spec 字典的常用键。

| 分类 | 函数 | HTTP 接口 |
| --- | --- | --- |
| **配置** | `configure(overrides)` / `current_config()` | — |
| **运维** | `health()` | `GET /api/health` |
| | `manifest()` | `GET /api/manifest` |
| | `stats()` | `GET /api/stats` |
| | `save_all()` | `POST /api/admin/save` |
| | `clear_current({"confirm": True})` | `POST /api/admin/clear` |
| **图管理** | `list_graphs()` | `GET /api/graphs` |
| | `create_graph({"name"})` | `POST /api/graphs` |
| | `graph_info({"name"})` | `GET /api/graphs/{name}` |
| | `delete_graph({"name", "confirm"})` | `DELETE /api/graphs/{name}` |
| | `save_graph({"name"})` | `POST /api/graphs/{name}/save` |
| | `clear_graph({"name", "confirm"})` | `POST /api/graphs/{name}/clear` |
| **节点** | `create_node({"labels", "props"})` | `POST /api/nodes` |
| | `list_nodes({"label","key","value","vtype","op","limit","offset","full"})` | `GET /api/nodes` |
| | `get_node({"id"})` | `GET /api/nodes/{id}` |
| | `replace_node({"id","labels","props"})` | `PUT /api/nodes/{id}` |
| | `update_node({"id","props","add_labels","remove_labels","remove_props","array_append","array_remove","object_merge"})` | `PATCH /api/nodes/{id}` |
| | `delete_node({"id"})` | `DELETE /api/nodes/{id}` |
| | `neighbors({"id","dir","type","full"})` | `GET /api/nodes/{id}/neighbors` |
| | `degree({"id","dir","type"})` | `GET /api/nodes/{id}/degree` |
| | `node_relationships({"id","dir","type","full"})` | `GET /api/nodes/{id}/relationships` |
| **关系** | `create_relationship({"from","to","type","props","weight"})` | `POST /api/relationships` |
| | `list_relationships({"type","from","to","dir","min_weight","max_weight","limit","offset","full"})` | `GET /api/relationships` |
| | `get_relationship({"id"})` | `GET /api/relationships/{id}` |
| | `replace_relationship({"id","type","props","weight","remove_props"})` | `PUT /api/relationships/{id}` |
| | `update_relationship({"id","type","props","weight","remove_props","array_append",...})` | `PATCH /api/relationships/{id}` |
| | `delete_relationship({"id"})` | `DELETE /api/relationships/{id}` |
| **边权** | `get_weight({"id"})` | `GET /api/relationships/{id}/weight` |
| | `set_weight({"id","weight"})` | `PATCH /api/relationships/{id}/weight` |
| | `set_weights({"weights"})` | `POST /api/relationships/weights` |
| | `weight_stats()` | `GET /api/relationships/weight-stats` |
| **查询/算法** | `query({"label","where","limit","offset","full"})` | `POST /api/query` |
| | `reachable({"id","depth","dir","type","full"})` | `GET /api/reachable/{id}` |
| | `shortest_path({"from","to","dir","type"})` | `GET /api/path` |
| | `weighted_path({"from","to","weight_key","dir","types"})` | `POST /api/path/weighted` |
| **索引** | `list_indexes()` | `GET /api/indexes` |
| | `create_index({"label","key"})` | `POST /api/indexes` |
| | `drop_index({"label","key"})` | `DELETE /api/indexes` |
| **逃生舱** | `request(spec)` / `call(spec)` | 任意方法 + 任意路径 |

#### 7.1.3 快速上手

```bash
./bin/graph_server --dir data &          # 先起服务
cd scripts && python3 -c "import graph_client"   # 无第三方依赖，可直接 import
```

```python
import graph_client as gc

gc.configure({"host": "127.0.0.1", "port": 8080, "graph": "social"})
gc.create_graph({"name": "social"})                  # 建图（已存在会抛 409）

# ---------- 增 ----------
alice = gc.create_node({"labels": ["Person"],
                        "props": {"name": "Alice", "age": 30,
                                  "tags": ["vip"], "addr": {"city": "北京"}}})["id"]
bob = gc.create_node({"labels": ["Person"], "props": {"name": "Bob", "age": 26}})["id"]
gc.create_relationship({"from": alice, "to": bob, "type": "KNOWS",
                        "props": {"since": 2019}, "weight": 2.5})["id"]

# ---------- 查 ----------
gc.get_node({"id": alice})                                        # 单个
gc.list_nodes({"label": "Person", "limit": 20})                   # 列表
gc.list_nodes({"key": "name", "value": "Alice"})                  # 属性过滤
gc.query({"label": "Person", "where": {"age": {"$gte": 30}}})     # 条件查询
gc.query({"where": {"tags": {"$contains": "vip"}}})               # 数组包含
gc.neighbors({"id": alice, "dir": "both"})                        # 邻居（data["neighbors"]）
gc.degree({"id": alice})                                          # 度数
gc.reachable({"id": alice, "depth": 2})                           # 2 跳可达（data["nodes"]）
gc.shortest_path({"from": alice, "to": bob})                      # 无权最短路
gc.weighted_path({"from": alice, "to": bob, "types": ["KNOWS"]})  # 带权最短路

# ---------- 改 ----------
gc.update_node({"id": alice, "props": {"age": 31},               # 增量（合并）
                "add_labels": ["Manager"],
                "array_append": {"tags": ["gold"]},              # 数组追加（自动去重）
                "object_merge": {"addr": {"street": "长安街"}}})  # 对象浅合并
gc.replace_node({"id": bob, "labels": ["Person"],                # 整体替换
                 "props": {"name": "Bob", "age": 27}})

# ---------- 删 ----------
gc.delete_relationship({"id": 1})
gc.delete_node({"id": bob})      # 级联删除其所有关系
gc.save_all()                    # 全部图落盘
```

#### 7.1.4 边权用法

```python
r = gc.create_relationship({"from": 1, "to": 2, "type": "ROAD",
                            "props": {"name": "京沪线"}, "weight": 5.0})["id"]
gc.get_weight({"id": r})                          # -> {"id": 1, "weight": 5.0}
gc.set_weight({"id": r, "weight": 2.5})           # 单条改权
gc.set_weights({"weights": {1: 0.5, 2: 3.0}})     # 批量：字典写法（键会转成 "1"/"2"）
gc.set_weights({"weights": [[3, 1.2], [4, 9]]})   # 批量：数组写法
gc.weight_stats()                                 # 统计 count/sum/min/max/avg
gc.list_relationships({"min_weight": 5})          # 按权重区间筛选
gc.weighted_path({"from": 1, "to": 4, "types": ["ROAD"]})   # 带权最短路
```

> `weighted_path` 记得传 `types`：同一对节点之间若还有没设权的边（权重默认 `1.0`），
> Dijkstra 会优先走那条更"便宜"的边，结果看起来就像边权没生效。

#### 7.1.5 错误处理

默认 `raise_on_error=True`，服务端 `ok=false` 时抛 `GraphAPIError`：

```python
from graph_client import GraphAPIError

try:
    gc.get_node({"id": 999999})
except GraphAPIError as e:
    print(e.code, e.status, e)     # not_found 404 节点不存在: 999999 | ...
```

想要"不抛异常、自己检查"：

```python
gc.configure({"raise_on_error": False})
env = gc.get_node({"id": 999999})      # 返回完整信封
if not env.get("ok"):
    print(env["error"]["code"], env["error"]["message"])
```

连不上服务时同样抛 `GraphAPIError`（`code="connection_error"`）。

#### 7.1.6 命令行自检

```bash
python3 scripts/graph_client.py            # 端到端跑一遍全部接口（需服务已启动）
python3 scripts/graph_client.py --check    # 只做健康检查 + 列出图
```

自检脚本会在 `data` 目录下建一张 `py_client_demo` 图，依次演示
建图 / 清空 / 建节点 / 建带权关系 / 各类查询与遍历 / 最短路 / 增量与整体修改 /
边权读写与批量设权 / 索引增删 / 删除与级联 / 统计落盘 / 错误捕获，最后删掉该图。

> 脚本默认连 `127.0.0.1:8080` 的 `default` 图；
> 换机器或换端口请改脚本头部 `CONFIG`，或 `import` 后调 `configure()`。

---

## 八、设计要点

1. **三层单向依赖**：`graph_server` → `http` / `minigraph`；**没有反向依赖**。
   内核完全不知道 HTTP 的存在，HTTP 层完全不知道图的存在，因此任何一层都能被单独复用或替换。
2. **单一 include 根**：合并后所有头文件都放在仓库根的 `include/` 下，用子目录区分归属，
   编译只需 `-Iinclude`。源码零改动即可完成合并。
3. **多图模型**：`GraphService` 持有 `map<图名, unique_ptr<GraphDB>>`，一图一文件；
   启动时扫目录载入全部，新建/删除图即刻反映到目录。请求路由在图名之后才解析资源，
   因此所有原有接口都天然支持"加前缀换图"，改动集中在分发层。
4. **线程安全**：`HttpServer` 是每连接一线程，而 `GraphDB` 本身非线程安全，
   `GraphService` 用一把 `std::mutex` 把所有数据库访问串行化，避免并发写坏内存。
5. **类型映射**：`minigraph::Value` 是 7 种类型，与 JSON 一一对应，
   数组/对象递归转换并限制 32 层；`int`/`double` 在比较时互通（`30 == 30.0`）。
6. **落盘格式向前兼容**：`1` 只有标量，`2` 支持数组/对象，`3` 追加边权。
   读旧文件时自动把 `props["weight"]` 迁移成一等 `weight` 字段并从 `props` 移除；
   写入时若数据里没有容器且所有边权都是默认 `1.0`，仍写 v1，旧版程序照样能打开。
7. **边权是一等字段**：`weight` 放在 `Relationship` 结构体上而不是 `props` 里，
   因此它可以被直接列举、批量修改、区间筛选，也能被 JSON 层做类型校验（非数字直接 400）。
   `props` 里的 `weight` 只是历史遗留的读取来源（`weightKey`），不再是推荐写法。
8. **容器与索引**：数组/对象不进二级索引（比较语义不稳定），
   按容器值查询自动退化为全表扫描；标签索引、关系类型索引照常生效。
9. **持久化**：默认每次改动自动落盘（先写 `.tmp` 再 rename，原子替换）；
   批量导入可用 `--no-autosave` 关闭，导完调 `POST /api/admin/save`。
10. **改关系类型**：`GraphDB` 没有 `setType`，用"删除 + 重建"实现，
    删除后 id 进入复用池，重建时通常拿回同一个 id（响应中的 `idChanged` 会如实告知）。
    重建时会把原权重一起带过去（`weight` 与 `props` 都是一等/独立字段，不会丢）。
11. **零第三方依赖**：只用 C++17 标准库 + POSIX socket，
    没有 Boost / Poco / nlohmann-json / libcurl，克隆下来 `make` 就能跑。

---

## 九、测试

```bash
make test        # 等价于下面三项
./bin/json_tests          # JSON 库自测：28 项
bash scripts/api_test.sh  # 图数据库服务接口回归：53 项 + 9 类预期错误
bash scripts/weight_test.sh  # 边权专项：26 项

make smoke       # bash scripts/smoke_test.sh，桥接端到端冒烟测试
```

- `api_test.sh` 覆盖：健康检查、多图（新建/列出/互相隔离/删除）、节点与关系增查改删、
  数组与对象属性（创建 / `$contains` / `$has` / `$in` / `$gte` / PATCH 增量操作）、
  条件查询、邻居、度数、可达、最短路径、带权路径、索引增删、统计、落盘、级联删除，
  以及 9 类预期错误响应。
- `weight_test.sh` 覆盖：创建带权边、默认权重、单条设权（GET/PUT/PATCH）、批量设权
  （对象与数组两种写法）、边权统计、区间筛选、带权最短路、非法值校验、负权告警、
  **重启后边权是否保留**，以及**旧格式（v2，weight 存在 props 里）自动迁移**。
- `smoke_test.sh` 覆盖：健康检查、POST/GET 两种调用形式、键值自动组装、错误分支
  （除零、非法 JSON）、中文与 emoji 透传。

CMake 方式下也可用 `ctest`（当前注册了 `json_tests`）。

---

## 十、平台与注意事项

- **平台**：代码使用 POSIX 接口（`socket` / `fork` / `exec` / `poll`），
  macOS / Linux 可直接 `make`；Windows 需把 `src/http/child_process.cpp` 换成
  `CreateProcess` + 匿名管道实现，`src/http/http_server.cpp` 换成 Winsock。
- **子进程开销**：桥接模式每个请求 fork 一个进程，适合"每次调用几十毫秒级"的场景；
  若需极高 QPS，建议改成常驻进程 + 长连接管道，或直接用嵌入式方式（`examples/embedded_server.cpp`）。
- **网络暴露面**：`--host 0.0.0.0` 会暴露到局域网，程序可被任意人调用，
  请确保处于可信网络。图数据库服务默认只监听 `127.0.0.1`。
- **并发写**：`GraphService` 用一把全局互斥锁串行化所有数据库访问，
  吞吐量足以支撑中小规模场景；写多读少的大规模场景建议自行拆分图或改读写锁。
- **内存**：内核全量常驻内存，适合百万级以内的图。

---

## 附录 A：与原三项目的对照

合并时**源码与头文件内容均未修改**，仅调整了落盘位置与两个 `main.cpp` 的文件名
（避免同名冲突）。对照如下：

| 原路径 | 合并后路径 |
| --- | --- |
| `minigraph/include/minigraph/*.h` | `include/minigraph/*.h` |
| `minigraph/src/*.cpp` | `src/minigraph/*.cpp` |
| `minigraph/examples/main.cpp` | `examples/minigraph_demo.cpp` |
| `cpp_http_bridge/include/*.hpp` | `include/*.hpp` |
| `cpp_http_bridge/src/http_server.cpp` / `child_process.cpp` | `src/http/*.cpp` |
| `cpp_http_bridge/src/main.cpp` | `src/cpp_bridge_main.cpp` |
| `cpp_http_bridge/examples/*.cpp` | `examples/*.cpp` |
| `cpp_http_bridge/tests/test_json.cpp` | `tests/test_json.cpp` |
| `cpp_http_bridge/scripts/*` | `scripts/smoke_test.sh`、`scripts/client.py` |
| `graph_server/include/graph_server/*.h` | `include/graph_server/*.h` |
| `graph_server/src/JsonValue.cpp` / `GraphService.cpp` | `src/graph_server/*.cpp` |
| `graph_server/src/main.cpp` | `src/graph_server_main.cpp` |
| `graph_server/web/index.html` | `web/index.html` |
| `graph_server/data/*.mgdb` | `data/*.mgdb` |
| `graph_server/docs/*` | `docs/*` |

**构建变化**：原来 `graph_server` 通过 `../minigraph`、`../cpp_http_bridge` 跨目录引用源文件；
现在三者在同一个仓库内，由根目录的 `Makefile` / `CMakeLists.txt` 统一编译，
不再依赖外部目录，也不再各自独立构建。

**可执行文件映射**：`graph_server` → `bin/graph_server`；
`cpp_bridge` → `bin/cpp_bridge`；`minigraph_demo` → `bin/minigraph_demo`；
`echo_worker` / `calc_worker` / `embedded_server` / `json_tests` 名称不变。
