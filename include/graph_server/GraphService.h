// -----------------------------------------------------------------------------
// GraphService —— 把 MiniGraph 图数据库包装成一套 HTTP JSON API（支持多库）
//
//   GraphService service("data", /*autoSave=*/true);   // data/ 目录，一张图一个文件
//   service.setDefaultGraph("default");
//   service.open(err);                                 // 自动扫描目录，打开所有 *.mgdb
//   ApiResult r = service.dispatch(httpRequest);       // 内部完成路由 + 加锁
//
// 多库模型：
//   一张图 = 一个独立的 GraphDB 实例 = 数据目录下的一个 <图名>.mgdb 文件。
//   请求要操作哪张图，按以下优先级决定：
//     1) 路径前缀  /api/g/{图名}/nodes ...
//     2) 请求头    X-Graph: {图名}
//     3) query     ?graph={图名}
//     4) 默认图（启动参数 --graph 指定，缺省 default）
//
// 线程安全：HttpServer 每连接一个线程，而 GraphDB 本身不是线程安全的，
// 因此这里用一把互斥锁把所有数据库访问串行化。
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "http_server.hpp"
#include "json.hpp"
#include "minigraph/GraphDB.h"
#include "minigraph/Types.h"

namespace graphserver {

// 默认图名与数据文件后缀
constexpr const char* kDefaultGraphName = "default";
constexpr const char* kGraphFileSuffix  = ".mgdb";

// 一次 API 调用的结果
struct ApiResult {
    int status = 200;
    minijson::Json body = minijson::Json::object();
};

// 统一响应封装
//   成功：{"ok": true,  "data": ...}
//   失败：{"ok": false, "error": {"code": "...", "message": "..."}}
ApiResult okResult(minijson::Json data, int status = 200);
ApiResult errorResult(int status, const std::string& code, const std::string& message);

class GraphService {
public:
    using Params = std::map<std::string, std::string>;

    // dataDir：数据目录（一张图一个 <图名>.mgdb 文件）
    explicit GraphService(std::string dataDir, bool autoSave = true);

    bool open(std::string& err);     // 扫描目录，打开/新建所有图
    void flush();                    // 立即落盘所有图（用于退出前）

    void setDefaultGraph(std::string name);
    const std::string& defaultGraph() const;
    const std::string& dataDir() const;

    // HTTP 总入口
    ApiResult dispatch(const httpbridge::HttpRequest& req);

    // 便于测试与文档生成：接口清单
    static minijson::Json apiManifest();

    // 便于测试：当前已打开的图数量
    std::size_t graphCount() const;

private:
    // ---------------- 图（数据库）管理 ----------------
    ApiResult routeGraphs(const std::vector<std::string>& seg, const Params& params,
                          const minijson::Json& body, const std::string& method);
    ApiResult listGraphs();
    ApiResult createGraph(const minijson::Json& body);
    ApiResult graphInfo(const std::string& name);
    ApiResult deleteGraph(const std::string& name, const Params& params,
                          const minijson::Json& body);
    ApiResult saveGraph(minigraph::GraphDB& db, const std::string& name);
    ApiResult clearGraph(minigraph::GraphDB& db, const std::string& name,
                         const minijson::Json& body);

    minigraph::GraphDB* findGraph(const std::string& name);      // 未找到返回 nullptr
    std::string graphFilePath(const std::string& name) const;
    static bool validGraphName(const std::string& name, std::string& err);

    // ---------------- 节点 ----------------
    ApiResult createNode(minigraph::GraphDB& db, const minijson::Json& body);
    ApiResult listNodes(minigraph::GraphDB& db, const Params& params);
    ApiResult getNode(minigraph::GraphDB& db, uint64_t id);
    ApiResult mutateNode(minigraph::GraphDB& db, uint64_t id, const minijson::Json& body,
                         bool replace);
    ApiResult deleteNode(minigraph::GraphDB& db, uint64_t id);
    ApiResult nodeNeighbors(minigraph::GraphDB& db, uint64_t id, const Params& params);
    ApiResult nodeDegree(minigraph::GraphDB& db, uint64_t id, const Params& params);
    ApiResult nodeRelationships(minigraph::GraphDB& db, uint64_t id, const Params& params);

    // ---------------- 关系 ----------------
    ApiResult createRelationship(minigraph::GraphDB& db, const minijson::Json& body);
    ApiResult listRelationships(minigraph::GraphDB& db, const Params& params);
    ApiResult getRelationship(minigraph::GraphDB& db, uint64_t id);
    ApiResult mutateRelationship(minigraph::GraphDB& db, uint64_t id,
                                 const minijson::Json& body, bool replace);
    ApiResult deleteRelationship(minigraph::GraphDB& db, uint64_t id);

    // ---------------- 边权（关系的一等字段 weight）----------------
    ApiResult setRelationshipWeight(minigraph::GraphDB& db, uint64_t id,
                                    const minijson::Json& body);   // 单条设权
    ApiResult setRelationshipWeights(minigraph::GraphDB& db,
                                     const minijson::Json& body);   // 批量设权
    ApiResult weightStats(minigraph::GraphDB& db);                  // 边权统计

    // ---------------- 查询 / 算法 ----------------
    ApiResult queryNodes(minigraph::GraphDB& db, const minijson::Json& body);
    ApiResult reachableFrom(minigraph::GraphDB& db, uint64_t id, const Params& params);
    ApiResult shortestPath(minigraph::GraphDB& db, const Params& params);
    ApiResult weightedPath(minigraph::GraphDB& db, const minijson::Json& body);

    // ---------------- 运维 ----------------
    ApiResult stats(minigraph::GraphDB& db, const std::string& name);
    ApiResult listIndexes(minigraph::GraphDB& db);
    ApiResult createIndex(minigraph::GraphDB& db, const minijson::Json& body);
    ApiResult dropIndex(minigraph::GraphDB& db, const Params& params, const minijson::Json& body);
    ApiResult saveAll();
    ApiResult health();

    std::string dir_;
    bool autoSave_;
    std::string defaultGraph_ = kDefaultGraphName;
    std::map<std::string, std::unique_ptr<minigraph::GraphDB>> graphs_;  // 图名 -> 图（有序）
    mutable std::mutex mu_;
};

}  // namespace graphserver
