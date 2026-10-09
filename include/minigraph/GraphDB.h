// -----------------------------------------------------------------------------
// MiniGraph —— 图数据库主类（高度封装，调用方只需面对这一个头文件）
//
//   GraphDB db("social.mgdb");
//   db.open();
//   uint64_t alice = db.createNode({"Person"}, {{"name", "Alice"}, {"age", 30}});
//   uint64_t bob   = db.createNode({"Person"}, {{"name", "Bob"},   {"age", 26}});
//   db.createRelationship(alice, bob, "KNOWS", {{"since", 2019}});
//   db.save();
//
// 所有"改"操作都会自动维护索引与邻接表，并在开启自动保存时落盘。
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "minigraph/Types.h"

namespace minigraph {

namespace storage { struct Snapshot; }  // 前向声明，实现细节不暴露给调用方

class GraphDB {
public:
    // path：数据文件（二进制）在工作目录中的路径
    explicit GraphDB(std::string path = "graph.mgdb");
    ~GraphDB();

    GraphDB(const GraphDB&) = delete;
    GraphDB& operator=(const GraphDB&) = delete;

    // =========================================================================
    // 一、生命周期与持久化
    // =========================================================================

    // 打开数据库：文件存在则载入，不存在则创建空库
    bool open();
    // 立即落盘（无论 autoSave 是否开启）
    bool save();
    // 保存并关闭
    void close();
    bool isOpen() const { return opened_; }

    // 每次修改是否自动落盘（默认 true）。批量导入时可关掉以提速，导完再 save()
    void setAutoSave(bool on) { autoSave_ = on; }
    bool autoSave() const { return autoSave_; }

    bool dirty() const { return dirty_; }          // 是否有未落盘的改动
    const std::string& path() const { return path_; }
    bool clear();                                   // 清空全部数据

    // =========================================================================
    // 二、节点 CRUD
    // =========================================================================
    uint64_t createNode(std::vector<std::string> labels, Props props = {});
    const Node* getNode(uint64_t id) const;

    bool setNodeProperty(uint64_t id, const std::string& key, const Value& value);
    bool removeNodeProperty(uint64_t id, const std::string& key);
    bool updateNode(uint64_t id, const Props& props);          // 合并属性（已存在则覆盖）
    bool replaceNodeProperties(uint64_t id, const Props& props); // 整体替换属性
    bool addNodeLabel(uint64_t id, const std::string& label);
    bool removeNodeLabel(uint64_t id, const std::string& label);
    bool removeNode(uint64_t id);                              // 级联删除其所有关系

    // =========================================================================
    // 三、关系 CRUD
    // =========================================================================
    // weight 为这条边的边权，默认 1.0（与无权最短路等价）
    uint64_t createRelationship(uint64_t from, uint64_t to, const std::string& type,
                                Props props = {}, double weight = Relationship::kDefaultWeight);
    const Relationship* getRelationship(uint64_t id) const;

    bool setRelationshipProperty(uint64_t id, const std::string& key, const Value& value);
    bool removeRelationshipProperty(uint64_t id, const std::string& key);
    bool updateRelationship(uint64_t id, const Props& props);
    bool removeRelationship(uint64_t id);

    // ---------------- 边权（一等字段）----------------
    // 给单条边设权。w 必须是有限数（非 NaN / Inf）；负数允许存，但最短路会拒绝计算。
    // 返回 false 表示关系不存在或权重非法。
    bool setRelationshipWeight(uint64_t id, double w);
    // 取单条边的权重，关系不存在返回 false
    bool getRelationshipWeight(uint64_t id, double& out) const;
    // 批量设权：返回成功设置的条数。不存在的 id 跳过，非法的权重跳过。
    std::size_t setRelationshipWeights(const std::map<uint64_t, double>& weights);
    // 全库边权统计（sum / min / max / avg / 真正设过权的条数）
    WeightStats weightStats() const;
    // 按权重区间筛选关系；minW / maxW 为闭区间，默认不限
    std::vector<uint64_t> findRelationshipsByWeight(double minW = -std::numeric_limits<double>::infinity(),
                                                    double maxW = std::numeric_limits<double>::infinity()) const;

    // 边权合法性：有限数（不是 NaN / Inf）
    static bool isValidWeight(double w);

    // =========================================================================
    // 四、查询
    // =========================================================================
    // 按标签查节点
    std::vector<uint64_t> findNodesByLabel(const std::string& label) const;
    // 按标签 + 属性查节点（若建有索引则走索引，否则全表扫描）
    std::vector<uint64_t> findNodes(const std::string& label, const std::string& key,
                                    const Value& value) const;
    // 自定义谓词全表扫描
    std::vector<uint64_t> findNodes(const std::function<bool(const Node&)>& predicate) const;

    // 按类型查关系
    std::vector<uint64_t> findRelationships(const std::string& type) const;
    // 查两点之间的关系（types 为空表示不限类型）
    std::vector<uint64_t> findRelationships(uint64_t from, uint64_t to,
                                            Direction dir = Direction::Out,
                                            const std::vector<std::string>& types = {}) const;

    // =========================================================================
    // 五、索引（(标签, 属性键) 二级索引，加速查找）
    // =========================================================================
    void createIndex(const std::string& label, const std::string& key);
    void dropIndex(const std::string& label, const std::string& key);
    bool hasIndex(const std::string& label, const std::string& key) const;
    std::vector<std::pair<std::string, std::string>> listIndexes() const;

    // =========================================================================
    // 六、图遍历与算法
    // =========================================================================
    // 邻居节点（types 为空表示不限类型）
    std::vector<uint64_t> neighbors(uint64_t id, Direction dir = Direction::Out,
                                    const std::vector<std::string>& types = {}) const;
    // 出度/入度/总度
    size_t degree(uint64_t id, Direction dir = Direction::Both,
                  const std::vector<std::string>& types = {}) const;
    // 与某节点相连的关系 id
    std::vector<uint64_t> incidentRelationships(uint64_t id, Direction dir = Direction::Both,
                                                const std::vector<std::string>& types = {}) const;
    // 从 start 出发，maxDepth 跳以内可达的所有节点（含自身）
    std::vector<uint64_t> reachable(uint64_t start, int maxDepth = 1,
                                    Direction dir = Direction::Out,
                                    const std::vector<std::string>& types = {}) const;
    // 无权最短路径（跳数最少），返回节点 id 序列；不可达返回空
    std::vector<uint64_t> shortestPath(uint64_t from, uint64_t to,
                                       Direction dir = Direction::Out,
                                       const std::vector<std::string>& types = {}) const;
    // 带权最短路径（Dijkstra）。
    //   weightKey 为空（默认）-> 用关系的一等 weight 字段（推荐）
    //   weightKey 非空       -> 从 props[weightKey] 取权重（兼容旧用法，缺失按 1.0）
    // 路径上出现负权边时，结果里的 negativeWeight 会置 true，cost 不可信（Dijkstra 不支持负权）。
    WeightedPath shortestWeightedPath(uint64_t from, uint64_t to,
                                      const std::string& weightKey = "",
                                      Direction dir = Direction::Out,
                                      const std::vector<std::string>& types = {}) const;

    // =========================================================================
    // 七、统计与调试
    // =========================================================================
    size_t nodeCount() const { return nodes_.size(); }
    size_t relationshipCount() const { return rels_.size(); }
    std::vector<uint64_t> allNodeIds() const;
    std::vector<uint64_t> allRelationshipIds() const;
    void printStats(std::ostream& os = std::cout) const;

private:
    using IndexKey = std::pair<std::string, std::string>;  // (label, property key)

    uint64_t allocNodeId();
    uint64_t allocRelId();
    void touch();                 // 标记改动 + 按需自动落盘
    void indexNode(uint64_t id);  // 把节点加入标签索引与属性索引
    void unindexNode(uint64_t id);
    void rebuildFromSnapshot(const storage::Snapshot& snap);
    storage::Snapshot makeSnapshot() const;
    static bool typeAllowed(const std::vector<std::string>& types, const std::string& type);

    // -------- 数据 --------
    std::string path_;
    bool opened_ = false;
    bool autoSave_ = true;
    bool dirty_ = false;

    std::unordered_map<uint64_t, Node> nodes_;
    std::unordered_map<uint64_t, Relationship> rels_;

    // 邻接表：节点 id -> 关系 id 集合
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> outEdges_;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> inEdges_;

    // 标签索引 / 关系类型索引（内置，无需手动创建）
    std::map<std::string, std::set<uint64_t>> labelIndex_;
    std::map<std::string, std::set<uint64_t>> relTypeIndex_;
    // 二级属性索引：(label, key) -> (value -> 节点 id 集合)
    std::map<IndexKey, std::map<Value, std::set<uint64_t>>> propIndex_;

    uint64_t nextNodeId_ = 1;
    uint64_t nextRelId_ = 1;
    std::vector<uint64_t> freeNodeIds_;   // 已删除 id，可复用
    std::vector<uint64_t> freeRelIds_;
};

} // namespace minigraph
