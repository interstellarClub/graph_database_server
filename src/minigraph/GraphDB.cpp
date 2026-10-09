#include "minigraph/GraphDB.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

#include "minigraph/Storage.h"

namespace minigraph {

namespace {

// types 为空表示"不限类型"
bool typeAllowed(const std::vector<std::string>& types, const std::string& type) {
    return types.empty() || std::find(types.begin(), types.end(), type) != types.end();
}

// 统一去重地往 vector 追加
void pushUnique(std::vector<uint64_t>& v, uint64_t id) {
    if (std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
}

} // namespace

// =============================================================================
// 构造 / 析构 / 持久化
// =============================================================================

GraphDB::GraphDB(std::string path) : path_(std::move(path)) {}

GraphDB::~GraphDB() {
    // 析构兜底落盘，避免程序异常退出丢数据
    if (opened_ && dirty_) {
        save();
    }
}

bool GraphDB::open() {
    if (opened_) return true;

    if (!storage::fileExists(path_)) {
        opened_ = true;   // 新库：直接可用，首次 save() 时才生成文件
        dirty_ = true;
        return true;
    }

    storage::Snapshot snap;
    try {
        if (!storage::loadSnapshot(path_, snap)) return false;
    } catch (const std::exception& e) {
        std::cerr << "[MiniGraph] 载入数据文件失败：" << e.what() << std::endl;
        return false;
    }

    rebuildFromSnapshot(snap);
    opened_ = true;
    dirty_ = false;
    return true;
}

bool GraphDB::save() {
    if (!opened_) return false;
    if (!storage::saveSnapshot(path_, makeSnapshot())) {
        std::cerr << "[MiniGraph] 写入数据文件失败：" << path_ << std::endl;
        return false;
    }
    dirty_ = false;
    return true;
}

void GraphDB::close() {
    if (!opened_) return;
    if (dirty_) save();
    nodes_.clear();
    rels_.clear();
    outEdges_.clear();
    inEdges_.clear();
    labelIndex_.clear();
    relTypeIndex_.clear();
    propIndex_.clear();
    freeNodeIds_.clear();
    freeRelIds_.clear();
    opened_ = false;
}

bool GraphDB::clear() {
    nodes_.clear();
    rels_.clear();
    outEdges_.clear();
    inEdges_.clear();
    labelIndex_.clear();
    relTypeIndex_.clear();
    propIndex_.clear();
    freeNodeIds_.clear();
    freeRelIds_.clear();
    nextNodeId_ = 1;
    nextRelId_ = 1;
    touch();
    return true;
}

void GraphDB::touch() {
    dirty_ = true;
    if (autoSave_) save();
}

uint64_t GraphDB::allocNodeId() {
    if (!freeNodeIds_.empty()) {
        uint64_t id = freeNodeIds_.back();
        freeNodeIds_.pop_back();
        return id;
    }
    return nextNodeId_++;
}

uint64_t GraphDB::allocRelId() {
    if (!freeRelIds_.empty()) {
        uint64_t id = freeRelIds_.back();
        freeRelIds_.pop_back();
        return id;
    }
    return nextRelId_++;
}

storage::Snapshot GraphDB::makeSnapshot() const {
    storage::Snapshot snap;
    snap.nodes.reserve(nodes_.size());
    for (const auto& kv : nodes_) snap.nodes.push_back(kv.second);
    std::sort(snap.nodes.begin(), snap.nodes.end(),
              [](const Node& a, const Node& b) { return a.id < b.id; });

    snap.rels.reserve(rels_.size());
    for (const auto& kv : rels_) snap.rels.push_back(kv.second);
    std::sort(snap.rels.begin(), snap.rels.end(),
              [](const Relationship& a, const Relationship& b) { return a.id < b.id; });

    for (const auto& kv : propIndex_) snap.indexes.push_back(kv.first);
    snap.nextNodeId = nextNodeId_;
    snap.nextRelId = nextRelId_;
    return snap;
}

void GraphDB::rebuildFromSnapshot(const storage::Snapshot& snap) {
    nodes_.clear();
    rels_.clear();
    outEdges_.clear();
    inEdges_.clear();
    labelIndex_.clear();
    relTypeIndex_.clear();
    propIndex_.clear();
    freeNodeIds_.clear();
    freeRelIds_.clear();

    // 先恢复索引定义，再灌数据，这样导入时即可建立属性索引
    for (const auto& ix : snap.indexes) propIndex_[ix];

    for (const auto& n : snap.nodes) {
        nodes_[n.id] = n;
        outEdges_.emplace(n.id, std::unordered_set<uint64_t>{});
        inEdges_.emplace(n.id, std::unordered_set<uint64_t>{});
        indexNode(n.id);
    }
    for (const auto& r : snap.rels) {
        rels_[r.id] = r;
        outEdges_[r.from].insert(r.id);
        inEdges_[r.to].insert(r.id);
        relTypeIndex_[r.type].insert(r.id);
    }
    nextNodeId_ = snap.nextNodeId;
    nextRelId_ = snap.nextRelId;
}

// =============================================================================
// 索引维护
// =============================================================================

void GraphDB::indexNode(uint64_t id) {
    const Node& n = nodes_.at(id);
    for (const auto& label : n.labels) labelIndex_[label].insert(id);

    for (const auto& entry : propIndex_) {          // entry: (label,key) -> (value -> ids)
        const std::string& label = entry.first.first;
        const std::string& key = entry.first.second;
        if (!n.hasLabel(label)) continue;
        auto it = n.props.find(key);
        if (it == n.props.end()) continue;
        if (it->second.isContainer()) continue;     // 数组/对象不进二级索引（改用全表扫描匹配）
        propIndex_[entry.first][it->second].insert(id);
    }
}

void GraphDB::unindexNode(uint64_t id) {
    auto nit = nodes_.find(id);
    if (nit == nodes_.end()) return;
    const Node& n = nit->second;

    for (const auto& label : n.labels) {
        auto it = labelIndex_.find(label);
        if (it != labelIndex_.end()) {
            it->second.erase(id);
            if (it->second.empty()) labelIndex_.erase(it);
        }
    }
    for (auto& entry : propIndex_) {
        if (!n.hasLabel(entry.first.first)) continue;
        auto pit = n.props.find(entry.first.second);
        if (pit == n.props.end()) continue;
        auto vit = entry.second.find(pit->second);
        if (vit != entry.second.end()) {
            vit->second.erase(id);
            if (vit->second.empty()) entry.second.erase(vit);
        }
    }
}

void GraphDB::createIndex(const std::string& label, const std::string& key) {
    IndexKey k{label, key};
    if (propIndex_.count(k)) return;               // 已存在
    propIndex_[k];
    for (const auto& kv : nodes_) indexNode(kv.first);  // 回填历史数据
    dirty_ = true;                                  // 索引定义需要持久化
    if (autoSave_) save();
}

void GraphDB::dropIndex(const std::string& label, const std::string& key) {
    propIndex_.erase(IndexKey{label, key});
    dirty_ = true;
    if (autoSave_) save();
}

bool GraphDB::hasIndex(const std::string& label, const std::string& key) const {
    return propIndex_.count(IndexKey{label, key}) > 0;
}

std::vector<std::pair<std::string, std::string>> GraphDB::listIndexes() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& kv : propIndex_) out.push_back(kv.first);
    return out;
}

// =============================================================================
// 节点 CRUD
// =============================================================================

uint64_t GraphDB::createNode(std::vector<std::string> labels, Props props) {
    // 标签去重
    std::vector<std::string> uniqueLabels;
    for (const auto& l : labels) {
        if (std::find(uniqueLabels.begin(), uniqueLabels.end(), l) == uniqueLabels.end()) {
            uniqueLabels.push_back(l);
        }
    }

    Node n;
    uint64_t id = allocNodeId();
    n.id = id;
    n.labels = std::move(uniqueLabels);
    n.props = std::move(props);
    nodes_.emplace(id, std::move(n));
    outEdges_.emplace(id, std::unordered_set<uint64_t>{});
    inEdges_.emplace(id, std::unordered_set<uint64_t>{});
    indexNode(id);
    touch();
    return id;
}

const Node* GraphDB::getNode(uint64_t id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

bool GraphDB::setNodeProperty(uint64_t id, const std::string& key, const Value& value) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    unindexNode(id);                 // 属性变化会影响索引，先摘再插
    it->second.props[key] = value;
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::removeNodeProperty(uint64_t id, const std::string& key) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    if (!it->second.props.count(key)) return false;
    unindexNode(id);
    it->second.props.erase(key);
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::updateNode(uint64_t id, const Props& props) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    unindexNode(id);
    for (const auto& kv : props) it->second.props[kv.first] = kv.second;
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::replaceNodeProperties(uint64_t id, const Props& props) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    unindexNode(id);
    it->second.props = props;
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::addNodeLabel(uint64_t id, const std::string& label) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    if (it->second.hasLabel(label)) return true;
    unindexNode(id);
    it->second.labels.push_back(label);
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::removeNodeLabel(uint64_t id, const std::string& label) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;
    unindexNode(id);
    auto& labels = it->second.labels;
    labels.erase(std::remove(labels.begin(), labels.end(), label), labels.end());
    indexNode(id);
    touch();
    return true;
}

bool GraphDB::removeNode(uint64_t id) {
    if (!nodes_.count(id)) return false;

    // 先收集相连的关系（自环在出/入表各出现一次，用 set 去重）
    std::set<uint64_t> relIds;
    auto oit = outEdges_.find(id);
    if (oit != outEdges_.end()) relIds.insert(oit->second.begin(), oit->second.end());
    auto iit = inEdges_.find(id);
    if (iit != inEdges_.end()) relIds.insert(iit->second.begin(), iit->second.end());
    for (uint64_t rid : relIds) removeRelationship(rid);

    unindexNode(id);
    nodes_.erase(id);
    outEdges_.erase(id);
    inEdges_.erase(id);
    freeNodeIds_.push_back(id);
    touch();
    return true;
}

// =============================================================================
// 关系 CRUD
// =============================================================================

uint64_t GraphDB::createRelationship(uint64_t from, uint64_t to, const std::string& type,
                                     Props props, double weight) {
    if (!nodes_.count(from) || !nodes_.count(to)) return 0;  // 0 表示失败（id 从 1 开始）

    Relationship r;
    uint64_t id = allocRelId();
    r.id = id;
    r.from = from;
    r.to = to;
    r.type = type;
    r.props = std::move(props);
    // 非法权重（NaN / Inf）退回默认值，保证"每条边都有一个可用的权重"
    r.weight = isValidWeight(weight) ? weight : Relationship::kDefaultWeight;
    rels_.emplace(id, std::move(r));
    outEdges_[from].insert(id);
    inEdges_[to].insert(id);
    relTypeIndex_[type].insert(id);
    touch();
    return id;
}

const Relationship* GraphDB::getRelationship(uint64_t id) const {
    auto it = rels_.find(id);
    return it == rels_.end() ? nullptr : &it->second;
}

bool GraphDB::setRelationshipProperty(uint64_t id, const std::string& key, const Value& value) {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;
    it->second.props[key] = value;
    touch();
    return true;
}

bool GraphDB::removeRelationshipProperty(uint64_t id, const std::string& key) {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;
    if (!it->second.props.count(key)) return false;
    it->second.props.erase(key);
    touch();
    return true;
}

bool GraphDB::updateRelationship(uint64_t id, const Props& props) {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;
    for (const auto& kv : props) it->second.props[kv.first] = kv.second;
    touch();
    return true;
}

bool GraphDB::removeRelationship(uint64_t id) {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;

    uint64_t from = it->second.from;
    uint64_t to = it->second.to;
    auto oit = outEdges_.find(from);
    if (oit != outEdges_.end()) oit->second.erase(id);
    auto iit = inEdges_.find(to);
    if (iit != inEdges_.end()) iit->second.erase(id);

    auto tit = relTypeIndex_.find(it->second.type);
    if (tit != relTypeIndex_.end()) {
        tit->second.erase(id);
        if (tit->second.empty()) relTypeIndex_.erase(tit);
    }

    rels_.erase(it);
    freeRelIds_.push_back(id);
    touch();
    return true;
}

// =============================================================================
// 边权（一等字段）
// =============================================================================

bool GraphDB::isValidWeight(double w) {
    return std::isfinite(w);           // 排除 NaN 与 ±Inf
}

bool GraphDB::setRelationshipWeight(uint64_t id, double w) {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;
    if (!isValidWeight(w)) return false;
    if (it->second.weight == w) return true;         // 无变化，不触发落盘
    it->second.weight = w;
    touch();
    return true;
}

bool GraphDB::getRelationshipWeight(uint64_t id, double& out) const {
    auto it = rels_.find(id);
    if (it == rels_.end()) return false;
    out = it->second.weight;
    return true;
}

std::size_t GraphDB::setRelationshipWeights(const std::map<uint64_t, double>& weights) {
    std::size_t ok = 0;
    bool changed = false;
    for (const auto& kv : weights) {
        auto it = rels_.find(kv.first);
        if (it == rels_.end()) continue;             // 不存在的 id 静默跳过
        if (!isValidWeight(kv.second)) continue;     // 非法权重跳过
        if (it->second.weight != kv.second) {
            it->second.weight = kv.second;
            changed = true;
        }
        ++ok;
    }
    if (changed) touch();
    return ok;
}

WeightStats GraphDB::weightStats() const {
    WeightStats st;
    if (rels_.empty()) return st;

    double sum = 0.0;
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (const auto& kv : rels_) {
        const double w = kv.second.weight;
        sum += w;
        if (w < mn) mn = w;
        if (w > mx) mx = w;
        if (w != Relationship::kDefaultWeight) ++st.weightedCount;
    }
    st.count = rels_.size();
    st.sum = sum;
    st.min = mn;
    st.max = mx;
    st.avg = sum / static_cast<double>(rels_.size());
    return st;
}

std::vector<uint64_t> GraphDB::findRelationshipsByWeight(double minW, double maxW) const {
    std::vector<uint64_t> ids;
    for (const auto& kv : rels_) {
        const double w = kv.second.weight;
        if (w < minW || w > maxW) continue;
        ids.push_back(kv.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// =============================================================================
// 查询
// =============================================================================

std::vector<uint64_t> GraphDB::findNodesByLabel(const std::string& label) const {
    auto it = labelIndex_.find(label);
    if (it == labelIndex_.end()) return {};
    return std::vector<uint64_t>(it->second.begin(), it->second.end());
}

std::vector<uint64_t> GraphDB::findNodes(const std::string& label, const std::string& key,
                                         const Value& value) const {
    const bool viaIndex = !value.isContainer();      // 容器值没进索引，只能扫描
    auto idx = propIndex_.find(IndexKey{label, key});
    if (viaIndex && idx != propIndex_.end()) {       // 走索引
        auto vit = idx->second.find(value);
        if (vit == idx->second.end()) return {};
        return std::vector<uint64_t>(vit->second.begin(), vit->second.end());
    }
    return findNodes([&](const Node& n) {             // 退化成全表扫描
        if (!n.hasLabel(label)) return false;
        auto pit = n.props.find(key);
        return pit != n.props.end() && pit->second == value;
    });
}

std::vector<uint64_t> GraphDB::findNodes(const std::function<bool(const Node&)>& predicate) const {
    std::vector<uint64_t> result;
    for (const auto& kv : nodes_) {
        if (predicate(kv.second)) result.push_back(kv.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<uint64_t> GraphDB::findRelationships(const std::string& type) const {
    auto it = relTypeIndex_.find(type);
    if (it == relTypeIndex_.end()) return {};
    return std::vector<uint64_t>(it->second.begin(), it->second.end());
}

std::vector<uint64_t> GraphDB::findRelationships(uint64_t from, uint64_t to, Direction dir,
                                                 const std::vector<std::string>& types) const {
    std::vector<uint64_t> result;
    for (uint64_t rid : incidentRelationships(from, dir, types)) {
        const Relationship* r = getRelationship(rid);
        if (!r) continue;
        bool hit;
        if (dir == Direction::In)       hit = (r->from == to);
        else if (dir == Direction::Out) hit = (r->to == to);
        else                            hit = (r->from == to) || (r->to == to);
        if (hit) result.push_back(rid);
    }
    return result;
}

// =============================================================================
// 遍历与算法
// =============================================================================

bool GraphDB::typeAllowed(const std::vector<std::string>& types, const std::string& type) {
    return ::minigraph::typeAllowed(types, type);
}

std::vector<uint64_t> GraphDB::incidentRelationships(uint64_t id, Direction dir,
                                                     const std::vector<std::string>& types) const {
    std::vector<uint64_t> result;
    auto collect = [&](const std::unordered_map<uint64_t, std::unordered_set<uint64_t>>& edges) {
        auto it = edges.find(id);
        if (it == edges.end()) return;
        for (uint64_t rid : it->second) {
            auto rit = rels_.find(rid);
            if (rit == rels_.end()) continue;
            if (typeAllowed(types, rit->second.type)) result.push_back(rid);
        }
    };
    if (dir == Direction::Out || dir == Direction::Both) collect(outEdges_);
    if (dir == Direction::In || dir == Direction::Both) collect(inEdges_);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<uint64_t> GraphDB::neighbors(uint64_t id, Direction dir,
                                         const std::vector<std::string>& types) const {
    std::vector<uint64_t> result;
    for (uint64_t rid : incidentRelationships(id, dir, types)) {
        const Relationship* r = getRelationship(rid);
        if (!r) continue;
        uint64_t other = (r->from == id) ? r->to : r->from;
        if (dir == Direction::Out && r->from != id) continue;
        if (dir == Direction::In && r->to != id) continue;
        pushUnique(result, other);
    }
    return result;
}

size_t GraphDB::degree(uint64_t id, Direction dir, const std::vector<std::string>& types) const {
    return incidentRelationships(id, dir, types).size();
}

std::vector<uint64_t> GraphDB::reachable(uint64_t start, int maxDepth, Direction dir,
                                         const std::vector<std::string>& types) const {
    std::vector<uint64_t> result;
    if (!nodes_.count(start) || maxDepth < 0) return result;

    std::unordered_set<uint64_t> visited{start};
    std::vector<uint64_t> frontier{start};
    result.push_back(start);

    for (int d = 0; d < maxDepth; ++d) {
        std::vector<uint64_t> next;
        for (uint64_t cur : frontier) {
            for (uint64_t nb : neighbors(cur, dir, types)) {
                if (visited.insert(nb).second) {
                    next.push_back(nb);
                    result.push_back(nb);
                }
            }
        }
        if (next.empty()) break;
        frontier = std::move(next);
    }
    return result;
}

std::vector<uint64_t> GraphDB::shortestPath(uint64_t from, uint64_t to, Direction dir,
                                            const std::vector<std::string>& types) const {
    if (!nodes_.count(from) || !nodes_.count(to)) return {};
    if (from == to) return {from};

    std::unordered_map<uint64_t, uint64_t> parent;   // 节点 -> 前驱节点
    std::unordered_set<uint64_t> visited{from};
    std::queue<uint64_t> q;
    q.push(from);

    while (!q.empty()) {
        uint64_t cur = q.front();
        q.pop();
        for (uint64_t nb : neighbors(cur, dir, types)) {
            if (visited.count(nb)) continue;
            visited.insert(nb);
            parent[nb] = cur;
            if (nb == to) {                          // 回溯路径
                std::vector<uint64_t> path{to};
                uint64_t p = cur;
                while (p != from) {
                    path.push_back(p);
                    p = parent.at(p);
                }
                path.push_back(from);
                std::reverse(path.begin(), path.end());
                return path;
            }
            q.push(nb);
        }
    }
    return {};                                        // 不可达
}

WeightedPath GraphDB::shortestWeightedPath(uint64_t from, uint64_t to,
                                           const std::string& weightKey, Direction dir,
                                           const std::vector<std::string>& types) const {
    WeightedPath result;
    if (!nodes_.count(from) || !nodes_.count(to)) return result;
    if (from == to) {
        result.found = true;
        result.nodes = {from};
        return result;
    }

    // 取一条边的权重：weightKey 为空用一等 weight 字段，否则从 props 取（兼容旧用法）
    auto weightOf = [&weightKey](const Relationship* r) -> double {
        if (weightKey.empty()) return r->weight;
        auto pit = r->props.find(weightKey);
        return (pit != r->props.end()) ? pit->second.asDouble(1.0) : 1.0;
    };

    const double kInf = std::numeric_limits<double>::infinity();
    std::unordered_map<uint64_t, double> dist;
    std::unordered_map<uint64_t, uint64_t> prevNode;
    std::unordered_map<uint64_t, uint64_t> prevRel;
    for (const auto& kv : nodes_) dist[kv.first] = kInf;
    dist[from] = 0.0;

    using Item = std::pair<double, uint64_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.push({0.0, from});

    while (!pq.empty()) {
        auto [d, cur] = pq.top();
        pq.pop();
        if (d > dist[cur]) continue;
        if (cur == to) break;

        for (uint64_t rid : incidentRelationships(cur, dir, types)) {
            const Relationship* r = getRelationship(rid);
            if (!r) continue;
            uint64_t other = (r->from == cur) ? r->to : r->from;
            if (dir == Direction::Out && r->from != cur) continue;
            if (dir == Direction::In && r->to != cur) continue;

            double w = weightOf(r);
            if (!isValidWeight(w)) w = 1.0;          // NaN / Inf 兜底，避免污染距离
            if (w < 0) w = 0;                        // 先按 0 参与松弛，真实负值稍后如实上报

            double nd = d + w;
            if (nd < dist[other]) {
                dist[other] = nd;
                prevNode[other] = cur;
                prevRel[other] = rid;
                pq.push({nd, other});
            }
        }
    }

    if (dist[to] == kInf) return result;             // 不可达

    std::vector<uint64_t> nodes{to};
    std::vector<uint64_t> rels;
    uint64_t cur = to;
    while (cur != from) {
        rels.push_back(prevRel.at(cur));
        cur = prevNode.at(cur);
        nodes.push_back(cur);
    }
    std::reverse(nodes.begin(), nodes.end());
    std::reverse(rels.begin(), rels.end());

    // 如实回填每一跳的权重（保留负值，便于调用方判断结果是否可信）
    std::vector<double> legs;
    legs.reserve(rels.size());
    bool sawNegative = false;
    for (uint64_t rid : rels) {
        const Relationship* r = getRelationship(rid);
        double w = r ? weightOf(r) : 1.0;
        if (w < 0) sawNegative = true;
        legs.push_back(w);
    }

    result.found = true;
    result.cost = dist[to];
    result.nodes = std::move(nodes);
    result.rels = std::move(rels);
    result.legWeights = std::move(legs);
    result.negativeWeight = sawNegative;
    return result;
}

// =============================================================================
// 统计
// =============================================================================

std::vector<uint64_t> GraphDB::allNodeIds() const {
    std::vector<uint64_t> ids;
    ids.reserve(nodes_.size());
    for (const auto& kv : nodes_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<uint64_t> GraphDB::allRelationshipIds() const {
    std::vector<uint64_t> ids;
    ids.reserve(rels_.size());
    for (const auto& kv : rels_) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void GraphDB::printStats(std::ostream& os) const {
    os << "==== MiniGraph 统计 ====\n";
    os << "数据文件 : " << path_ << " (" << storage::fileSize(path_) << " 字节)\n";
    os << "节点数   : " << nodes_.size() << "\n";
    os << "关系数   : " << rels_.size() << "\n";
    os << "-- 标签分布 --\n";
    for (const auto& kv : labelIndex_) os << "  :" << kv.first << " -> " << kv.second.size() << "\n";
    os << "-- 关系类型分布 --\n";
    for (const auto& kv : relTypeIndex_) os << "  :" << kv.first << " -> " << kv.second.size() << "\n";
    os << "-- 二级索引 --\n";
    if (propIndex_.empty()) os << "  (无)\n";
    for (const auto& kv : propIndex_) {
        os << "  (:" << kv.first.first << " . " << kv.first.second << ") -> "
           << kv.second.size() << " 个不同取值\n";
    }
    os << "========================\n";
}

} // namespace minigraph
