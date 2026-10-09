// -----------------------------------------------------------------------------
// MiniGraph —— 图的两种基本元素：节点(Node) 与 关系(Relationship)
//
//   节点：id + 若干标签(labels) + 属性表(props)
//   关系：id + 起点(from) + 终点(to) + 类型(type) + 属性表(props)
// 与 Neo4j 的概念一一对应，便于迁移理解。
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "minigraph/Value.h"

namespace minigraph {

struct Node {
    uint64_t id = 0;
    std::vector<std::string> labels;   // 一个节点可以有多个标签
    Props props;

    bool hasLabel(const std::string& label) const {
        for (const auto& l : labels) {
            if (l == label) return true;
        }
        return false;
    }
};

struct Relationship {
    uint64_t id = 0;
    uint64_t from = 0;
    uint64_t to = 0;
    std::string type;                  // 关系类型，如 KNOWS / WORKS_AT
    Props props;
    double weight = 1.0;               // 边权（一等字段，默认 1.0，与无权最短路等价）

    // 默认权重：新建关系时若调用方未指定，用这个值
    static constexpr double kDefaultWeight = 1.0;
};

// 遍历方向
enum class Direction { Out, In, Both };

// 带权最短路径结果
struct WeightedPath {
    bool found = false;
    double cost = 0.0;
    std::vector<uint64_t> nodes;       // 路径上的节点 id
    std::vector<uint64_t> rels;        // 路径上的关系 id
    std::vector<double> legWeights;    // 每一跳的边权（长度 = rels.size()）
    bool negativeWeight = false;       // 路径上遇到负权边（Dijkstra 结果不可信）
};

// 全库边权统计
struct WeightStats {
    size_t count = 0;                  // 关系总数
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    double avg = 0.0;
    size_t weightedCount = 0;          // weight != 1.0 的关系数（真正设过权的）
};

// -------- 打印辅助（Cypher 风格，便于调试）--------
inline std::string labelsToString(const std::vector<std::string>& labels) {
    std::string s;
    for (const auto& l : labels) s += ":" + l;
    return s;
}

inline std::string propsToString(const Props& props) {
    if (props.empty()) return "";
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (const auto& kv : props) {
        if (!first) os << ", ";
        first = false;
        os << kv.first << ": ";
        if (kv.second.isString()) os << "\"" << kv.second.toString() << "\"";
        else os << kv.second.toString();
    }
    os << "}";
    return os.str();
}

inline std::string toString(const Node& n) {
    std::ostringstream os;
    os << "(#" << n.id << labelsToString(n.labels);
    std::string p = propsToString(n.props);
    if (!p.empty()) os << " " << p;
    os << ")";
    return os.str();
}

inline std::string toString(const Relationship& r) {
    std::ostringstream os;
    os << "(#" << r.from << ")-[#" << r.id << ":" << r.type;
    std::string p = propsToString(r.props);
    if (!p.empty()) os << " " << p;
    os << " w=" << r.weight;
    os << "]->(#" << r.to << ")";
    return os.str();
}

} // namespace minigraph
