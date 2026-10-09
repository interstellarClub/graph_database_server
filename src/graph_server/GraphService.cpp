#include "graph_server/GraphService.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

#include "graph_server/JsonValue.h"
#include "minigraph/Storage.h"

namespace graphserver {
namespace {

// =============================================================================
// 通用小工具
// =============================================================================

constexpr size_t kMaxLimit = 1000;
constexpr size_t kDefaultLimit = 100;

minijson::Json idArray(const std::vector<uint64_t>& ids) {
    minijson::Json arr = minijson::Json::array();
    for (uint64_t v : ids) arr.push_back(minijson::Json(static_cast<long long>(v)));
    return arr;
}

minijson::Json nodeToJson(const minigraph::Node& n) {
    minijson::Json o = minijson::Json::object();
    o.set("id", minijson::Json(static_cast<long long>(n.id)));
    minijson::Json labels = minijson::Json::array();
    for (const auto& l : n.labels) labels.push_back(l);
    o.set("labels", labels);
    o.set("props", propsToJson(n.props));
    return o;
}

minijson::Json relationshipToJson(const minigraph::Relationship& r) {
    minijson::Json o = minijson::Json::object();
    o.set("id", minijson::Json(static_cast<long long>(r.id)));
    o.set("from", minijson::Json(static_cast<long long>(r.from)));
    o.set("to", minijson::Json(static_cast<long long>(r.to)));
    o.set("type", r.type);
    o.set("weight", minijson::Json(r.weight));   // 边权（一等字段，默认 1.0）
    o.set("props", propsToJson(r.props));
    return o;
}

// 解析 query string 里的数字（空串 / 非法都返回 false）
bool parseDoubleStr(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// 从 JSON 里读边权。
//   缺失           -> 用 def（默认 1.0）
//   必须是数字      -> 否则报错，避免 "weight": "5" 被静默当成 1.0
//   必须是有限数    -> NaN / Inf 在 JSON 里无法表示，但要防住手写的 inf
//   负数           -> 允许存（最长路等场景有用），但最短路计算时会明确报错
bool jsonWeight(const minijson::Json& j, double& out, std::string& err) {
    if (!j.isNumber()) {
        err = "weight 必须是数字（整数或小数）";
        return false;
    }
    const double w = j.asDouble();
    if (!std::isfinite(w)) {
        err = "weight 必须是有限数，不能是 NaN 或 Infinity";
        return false;
    }
    out = w;
    return true;
}

// 分页切片
std::vector<uint64_t> slice(const std::vector<uint64_t>& ids, size_t offset, size_t limit) {
    if (offset >= ids.size()) return {};
    size_t n = std::min(limit, ids.size() - offset);
    return std::vector<uint64_t>(ids.begin() + static_cast<long>(offset),
                                 ids.begin() + static_cast<long>(offset + n));
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos
                                                                         : slash - start);
        if (!part.empty()) out.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

bool toU64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    errno = 0;
    out = std::strtoull(s.c_str(), nullptr, 10);
    return errno == 0;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t comma = s.find(',', start);
        std::string part = s.substr(start, comma == std::string::npos ? std::string::npos
                                                                      : comma - start);
        part = trim(part);
        if (!part.empty()) out.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

minigraph::Direction parseDirection(const std::string& s) {
    if (s == "in" || s == "IN")   return minigraph::Direction::In;
    if (s == "both" || s == "BOTH") return minigraph::Direction::Both;
    return minigraph::Direction::Out;
}

const char* directionName(minigraph::Direction d) {
    switch (d) {
        case minigraph::Direction::In:   return "in";
        case minigraph::Direction::Both: return "both";
        default:                         return "out";
    }
}

// ---------------- 参数读取 ----------------

bool paramBool(const GraphService::Params& p, const std::string& key, bool def) {
    auto it = p.find(key);
    if (it == p.end()) return def;
    const std::string& v = it->second;
    return !(v.empty() || v == "0" || v == "false" || v == "no" || v == "False");
}

uint64_t paramU64(const GraphService::Params& p, const std::string& key, uint64_t def) {
    auto it = p.find(key);
    if (it == p.end()) return def;
    uint64_t v = 0;
    return toU64(it->second, v) ? v : def;
}

std::string paramStr(const GraphService::Params& p, const std::string& key,
                     const std::string& def = "") {
    auto it = p.find(key);
    return it == p.end() ? def : it->second;
}

// HTTP 头查找（名称不区分大小写）
const std::string* findHeader(const httpbridge::HttpRequest& req, const std::string& name) {
    for (const auto& kv : req.headers) {
        if (toLower(kv.first) == name) return &kv.second;
    }
    return nullptr;
}

// 从 JSON 中读取 id 字段（支持数字与数字字符串）
bool jsonId(const minijson::Json& j, const std::string& key, uint64_t& out, std::string& err) {
    const minijson::Json* v = j.find(key);
    if (!v) {
        err = "缺少字段 \"" + key + "\"";
        return false;
    }
    if (v->isNumber()) {
        long long n = v->asInt();
        if (n < 0) { err = "\"" + key + "\" 不能为负数"; return false; }
        out = static_cast<uint64_t>(n);
        return true;
    }
    if (v->isString() && toU64(v->asString(), out)) return true;
    err = "\"" + key + "\" 必须是正整数";
    return false;
}

bool jsonStringArray(const minijson::Json& j, std::vector<std::string>& out, std::string& err) {
    if (!j.isArray()) { err = "必须是字符串数组"; return false; }
    for (const auto& item : j.asArray()) {
        if (!item.isString()) { err = "必须是字符串数组"; return false; }
        out.push_back(item.asString());
    }
    return true;
}

// =============================================================================
// 查询条件（支持数组/对象属性）
//
//   普通值：{"name": "Alice", "addr": {"city": "北京"}}
//   操作符：{"age": {"$gte": 30}, "tags": {"$contains": "vip"}, "city": {"$in": ["北京","上海"]}}
// =============================================================================

struct WhereCond {
    bool isOp = false;
    std::string op;
    minigraph::Value value;
};

bool parseCond(const minijson::Json& v, WhereCond& out, std::string& err) {
    static const char* kOps[] = {"$ne", "$gt", "$gte", "$lt", "$lte", "$in", "$contains", "$has", "$size"};
    if (v.isObject() && v.asObject().size() == 1) {
        const std::string& k = v.asObject().front().first;
        if (!k.empty() && k[0] == '$') {
            const bool known = std::find(std::begin(kOps), std::end(kOps), k) != std::end(kOps);
            if (!known) {
                err = "不支持的操作符 " + k +
                      "（可用: $ne $gt $gte $lt $lte $in $contains $has $size）";
                return false;
            }
            out.isOp = true;
            out.op = k;
            return valueFromJson(v.asObject().front().second, out.value, err);
        }
    }
    out.isOp = false;
    return valueFromJson(v, out.value, err);
}

bool matchCond(const minigraph::Value* actual, const WhereCond& c) {
    if (!actual) return c.isOp && c.op == "$ne";   // 属性不存在时，只有 $ne 算匹配
    const minigraph::Value& a = *actual;

    if (!c.isOp) return valueEquals(a, c.value);
    if (c.op == "$ne")       return !valueEquals(a, c.value);
    if (c.op == "$contains") return a.contains(c.value);   // 数组元素 / 对象值 / 字符串子串
    if (c.op == "$has") {                                  // 对象含某键 / 数组含某元素
        if (a.isObject()) return a.hasKey(c.value.asString());
        return a.contains(c.value);
    }
    if (c.op == "$size")     return static_cast<long long>(a.size()) == c.value.asInt(-1);
    if (c.op == "$in") {
        if (!c.value.isArray()) return false;
        for (const minigraph::Value& item : c.value.asArray()) {
            if (valueEquals(a, item)) return true;
        }
        return false;
    }

    // $gt / $gte / $lt / $lte：数值按数值比，其余按字符串比
    const bool numeric = (a.isInt() || a.isDouble()) && (c.value.isInt() || c.value.isDouble());
    if (numeric) {
        const double x = a.asDouble(), y = c.value.asDouble();
        if (c.op == "$gt")  return x > y;
        if (c.op == "$gte") return x >= y;
        if (c.op == "$lt")  return x < y;
        return x <= y;
    }
    const std::string x = a.asString(a.toString()), y = c.value.asString(c.value.toString());
    if (c.op == "$gt")  return x > y;
    if (c.op == "$gte") return x >= y;
    if (c.op == "$lt")  return x < y;
    return x <= y;
}

bool matchesNode(const minigraph::Node& n, const std::string& label,
                 const std::vector<std::pair<std::string, WhereCond>>& conds) {
    if (!label.empty() && !n.hasLabel(label)) return false;
    for (const auto& kv : conds) {
        auto it = n.props.find(kv.first);
        const minigraph::Value* v = (it == n.props.end()) ? nullptr : &it->second;
        if (!matchCond(v, kv.second)) return false;
    }
    return true;
}

// =============================================================================
// 数组 / 对象属性的增量操作（PATCH）
//
//   arrayAppend: {"tags": ["vip", "new"], "score": 5}   追加元素（自动去重）
//   arrayRemove: {"tags": ["vip"]}                      删除元素
//   objectMerge: {"addr": {"city": "上海"}}              合并对象（浅合并）
//
// get(key) -> const Value*（取当前值）; set(key, value)（写回）
// =============================================================================

template <typename Getter, typename Setter>
bool applyContainerOps(const minijson::Json& body, Getter get, Setter set, std::string& err) {
    auto itemsOf = [&](const minijson::Json& v, minigraph::ValueList& out) -> bool {
        if (v.isArray()) {
            for (const minijson::Json& item : v.asArray()) {
                minigraph::Value x;
                if (!valueFromJson(item, x, err)) return false;
                out.push_back(std::move(x));
            }
            return true;
        }
        minigraph::Value x;
        if (!valueFromJson(v, x, err)) return false;
        out.push_back(std::move(x));
        return true;
    };

    if (const minijson::Json* ap = body.find("arrayAppend")) {
        if (!ap->isObject()) { err = "arrayAppend 必须是对象，如 {\"tags\": [\"vip\"]}"; return false; }
        for (const auto& kv : ap->asObject()) {
            minigraph::ValueList items;
            if (!itemsOf(kv.second, items)) {
                err = "arrayAppend." + kv.first + ": " + err;
                return false;
            }
            const minigraph::Value* cur = get(kv.first);
            minigraph::Value next = (cur && cur->isArray()) ? *cur
                                                            : minigraph::Value(minigraph::ValueList{});
            for (const auto& item : items) {
                if (!next.contains(item)) next.push_back(item);   // 已存在则不重复添加
            }
            set(kv.first, next);
        }
    }

    if (const minijson::Json* ar = body.find("arrayRemove")) {
        if (!ar->isObject()) { err = "arrayRemove 必须是对象，如 {\"tags\": [\"vip\"]}"; return false; }
        for (const auto& kv : ar->asObject()) {
            minigraph::ValueList items;
            if (!itemsOf(kv.second, items)) {
                err = "arrayRemove." + kv.first + ": " + err;
                return false;
            }
            const minigraph::Value* cur = get(kv.first);
            if (!cur || !cur->isArray()) continue;                // 不是数组则无需处理
            minigraph::Value next = *cur;
            for (const auto& item : items) next.removeElement(item);
            set(kv.first, next);
        }
    }

    if (const minijson::Json* om = body.find("objectMerge")) {
        if (!om->isObject()) { err = "objectMerge 必须是对象，如 {\"addr\": {\"city\": \"上海\"}}"; return false; }
        for (const auto& kv : om->asObject()) {
            if (!kv.second.isObject()) { err = "objectMerge." + kv.first + " 的值必须是对象"; return false; }
            minigraph::ValueMap patch;
            std::string e;
            if (!propsFromJson(kv.second, patch, e)) {
                err = "objectMerge." + kv.first + ": " + e;
                return false;
            }
            const minigraph::Value* cur = get(kv.first);
            minigraph::Value next = (cur && cur->isObject()) ? *cur
                                                             : minigraph::Value(minigraph::ValueMap{});
            for (const auto& pk : patch) next.set(pk.first, pk.second);
            set(kv.first, next);
        }
    }
    return true;
}

}  // namespace

// =============================================================================
// 响应封装
// =============================================================================

ApiResult okResult(minijson::Json data, int status) {
    ApiResult r;
    r.status = status;
    r.body = minijson::Json::object();
    r.body.set("ok", minijson::Json(true));
    r.body.set("data", std::move(data));
    return r;
}

ApiResult errorResult(int status, const std::string& code, const std::string& message) {
    ApiResult r;
    r.status = status;
    r.body = minijson::Json::object();
    r.body.set("ok", minijson::Json(false));
    minijson::Json err = minijson::Json::object();
    err.set("code", code);
    err.set("message", message);
    r.body.set("error", err);
    return r;
}

// =============================================================================
// 生命周期与图管理基础设施
// =============================================================================

GraphService::GraphService(std::string dataDir, bool autoSave)
    : dir_(std::move(dataDir)), autoSave_(autoSave) {}

void GraphService::setDefaultGraph(std::string name) { defaultGraph_ = std::move(name); }
const std::string& GraphService::defaultGraph() const { return defaultGraph_; }
const std::string& GraphService::dataDir() const { return dir_; }

std::size_t GraphService::graphCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return graphs_.size();
}

std::string GraphService::graphFilePath(const std::string& name) const {
    return dir_ + "/" + name + kGraphFileSuffix;
}

bool GraphService::validGraphName(const std::string& name, std::string& err) {
    if (name.empty()) { err = "图名不能为空"; return false; }
    if (name.size() > 64) { err = "图名不能超过 64 个字符"; return false; }
    for (char c : name) {
        const bool okChar = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
                            c == '.' || c == ' ';
        if (!okChar) { err = "图名只能包含字母、数字、下划线、短横线、点（不能用斜杠等）"; return false; }
    }
    if (name == "." || name == "..") { err = "图名非法"; return false; }
    return true;
}

bool GraphService::open(std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);

    // 扫描数据目录：一个 *.mgdb 文件就是一张图
    std::vector<std::string> names;
    if (DIR* d = ::opendir(dir_.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            const std::string fn = e->d_name;
            const std::string suffix = kGraphFileSuffix;
            if (fn.size() > suffix.size() &&
                fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0) {
                names.push_back(fn.substr(0, fn.size() - suffix.size()));
            }
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
    }

    for (const std::string& name : names) {
        auto db = std::make_unique<minigraph::GraphDB>(graphFilePath(name));
        db->setAutoSave(autoSave_);
        if (!db->open()) {
            err = "无法打开图数据文件: " + db->path();
            return false;
        }
        graphs_[name] = std::move(db);
    }

    // 默认图必须存在（首次启动或目录为空时创建）
    if (!graphs_.count(defaultGraph_)) {
        auto db = std::make_unique<minigraph::GraphDB>(graphFilePath(defaultGraph_));
        db->setAutoSave(autoSave_);
        if (!db->open()) {
            err = "无法创建默认图: " + db->path();
            return false;
        }
        graphs_[defaultGraph_] = std::move(db);
    }
    return true;
}

void GraphService::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : graphs_) kv.second->save();
}

minigraph::GraphDB* GraphService::findGraph(const std::string& name) {
    auto it = graphs_.find(name);
    return it == graphs_.end() ? nullptr : it->second.get();
}

// =============================================================================
// 路由分发
// =============================================================================

ApiResult GraphService::dispatch(const httpbridge::HttpRequest& req) {
    const std::vector<std::string> seg = splitPath(req.path);
    if (seg.size() < 2 || seg[0] != "api") {
        return errorResult(404, "not_found", "未知路径: " + req.path + "（所有接口都在 /api 下）");
    }

    Params params;
    for (const auto& kv : httpbridge::parseQueryString(req.query)) params[kv.first] = kv.second;

    // 解析请求体（POST/PUT/PATCH/DELETE 可带 JSON body）
    minijson::Json body = minijson::Json::object();
    if (!req.body.empty()) {
        std::string perr;
        if (!minijson::Json::tryParse(req.body, body, &perr)) {
            return errorResult(400, "bad_json", "请求体不是合法 JSON: " + perr);
        }
        if (!body.isObject()) return errorResult(400, "bad_json", "请求体必须是 JSON 对象");
    }

    const std::string& m = req.method;

    // ---------------- 图管理：/api/graphs... ----------------
    if (seg[1] == "graphs") {
        std::lock_guard<std::mutex> lk(mu_);
        return routeGraphs(seg, params, body, m);
    }

    // ---------------- 决定操作哪张图 ----------------
    // 优先级：路径 /api/g/{图名}/... > 请求头 X-Graph > ?graph= > 默认图
    std::size_t base = 1;              // seg[base] 起是真正的资源名
    std::string graphName;
    if (seg[1] == "g") {
        if (seg.size() < 3) {
            return errorResult(400, "bad_request", "用法: /api/g/{图名}/nodes ... 例如 /api/g/social/nodes");
        }
        graphName = seg[2];
        base = 3;
    } else if (const std::string* h = findHeader(req, "x-graph")) {
        graphName = *h;
    } else if (!paramStr(params, "graph").empty()) {
        graphName = paramStr(params, "graph");
    } else {
        graphName = defaultGraph_;
    }
    graphName = trim(graphName);

    std::lock_guard<std::mutex> lk(mu_);
    minigraph::GraphDB* db = findGraph(graphName);
    if (!db) {
        return errorResult(404, "graph_not_found",
                           "图不存在: " + graphName + "（GET /api/graphs 可查看全部图，POST /api/graphs 可新建）");
    }

    const std::string resource = seg.size() > base     ? seg[base]     : std::string();
    const std::string idToken  = seg.size() > base + 1 ? seg[base + 1] : std::string();
    const std::string sub      = seg.size() > base + 2 ? seg[base + 2] : std::string();

    // ---------------- 健康检查 / 接口清单 ----------------
    if (resource == "health") return health();
    // apiManifest() 返回的本身就是标准信封 {ok, data}，直接作为响应体，不要再套一层
    if (resource == "manifest" && m == "GET") {
        ApiResult r;
        r.status = 200;
        r.body = apiManifest();
        return r;
    }

    // ---------------- 节点 ----------------
    if (resource == "nodes") {
        if (!idToken.empty()) {
            uint64_t id = 0;
            if (!toU64(idToken, id)) {
                return errorResult(400, "bad_request", "节点 id 非法: " + idToken);
            }
            if (sub.empty()) {
                if (m == "GET")    return getNode(*db, id);
                if (m == "PUT")    return mutateNode(*db, id, body, true);
                if (m == "PATCH")  return mutateNode(*db, id, body, false);
                if (m == "DELETE") return deleteNode(*db, id);
                return errorResult(405, "method_not_allowed", "节点只支持 GET/PUT/PATCH/DELETE");
            }
            if (sub == "neighbors")     return nodeNeighbors(*db, id, params);
            if (sub == "degree")        return nodeDegree(*db, id, params);
            if (sub == "relationships") return nodeRelationships(*db, id, params);
            return errorResult(404, "not_found", "未知子资源: /api/nodes/{id}/" + sub);
        }
        if (m == "POST") return createNode(*db, body);
        if (m == "GET")  return listNodes(*db, params);
        return errorResult(405, "method_not_allowed", "/api/nodes 只支持 GET/POST");
    }

    // ---------------- 关系 ----------------
    if (resource == "relationships") {
        if (!idToken.empty()) {
            // 集合级的边权子资源（要放在 toU64 之前，否则会被当成非法 id）
            if (idToken == "weights" && m == "POST") return setRelationshipWeights(*db, body);
            if (idToken == "weights") {
                return errorResult(405, "method_not_allowed", "/api/relationships/weights 只支持 POST");
            }
            if (idToken == "weight-stats" && m == "GET") return weightStats(*db);
            if (idToken == "weight-stats") {
                return errorResult(405, "method_not_allowed", "/api/relationships/weight-stats 只支持 GET");
            }

            uint64_t id = 0;
            if (!toU64(idToken, id)) {
                return errorResult(400, "bad_request", "关系 id 非法: " + idToken);
            }
            // 单条边的边权：PUT / PATCH /api/relationships/{id}/weight
            if (sub == "weight") {
                if (m == "PUT" || m == "PATCH") return setRelationshipWeight(*db, id, body);
                if (m == "GET") {
                    double w = 0.0;
                    if (!db->getRelationshipWeight(id, w)) {
                        return errorResult(404, "not_found", "关系不存在: " + std::to_string(id));
                    }
                    minijson::Json data = minijson::Json::object();
                    data.set("id", minijson::Json(static_cast<long long>(id)));
                    data.set("weight", minijson::Json(w));
                    return okResult(data);
                }
                return errorResult(405, "method_not_allowed",
                                   "/api/relationships/{id}/weight 只支持 GET/PUT/PATCH");
            }
            if (!sub.empty()) {
                return errorResult(404, "not_found", "未知子资源: /api/relationships/{id}/" + sub);
            }

            if (m == "GET")    return getRelationship(*db, id);
            if (m == "PUT")    return mutateRelationship(*db, id, body, true);
            if (m == "PATCH")  return mutateRelationship(*db, id, body, false);
            if (m == "DELETE") return deleteRelationship(*db, id);
            return errorResult(405, "method_not_allowed", "关系只支持 GET/PUT/PATCH/DELETE");
        }
        if (m == "POST") return createRelationship(*db, body);
        if (m == "GET")  return listRelationships(*db, params);
        return errorResult(405, "method_not_allowed", "/api/relationships 只支持 GET/POST");
    }

    // ---------------- 查询 / 遍历 / 算法 ----------------
    if (resource == "query" && m == "POST") return queryNodes(*db, body);
    if (resource == "query" && m == "GET") {
        // 方便浏览器直接点：把 query string 组装成与 POST /api/query 等价的请求体
        minijson::Json b = minijson::Json::object();
        if (!paramStr(params, "label").empty()) b.set("label", paramStr(params, "label"));
        if (!paramStr(params, "key").empty()) {
            minijson::Json where = minijson::Json::object();
            std::string err;
            minigraph::Value v;
            if (!valueFromText(paramStr(params, "value"), paramStr(params, "vtype", "auto"), v, err)) {
                return errorResult(400, "bad_request", err);
            }
            const std::string op = paramStr(params, "op");
            if (!op.empty() && op != "eq") {
                minijson::Json cond = minijson::Json::object();
                cond.set("$" + op, valueToJson(v));
                where.set(paramStr(params, "key"), cond);
            } else {
                where.set(paramStr(params, "key"), valueToJson(v));
            }
            b.set("where", where);
        }
        b.set("limit", minijson::Json(static_cast<long long>(paramU64(params, "limit", kDefaultLimit))));
        b.set("offset", minijson::Json(static_cast<long long>(paramU64(params, "offset", 0))));
        b.set("full", minijson::Json(paramBool(params, "full", false)));
        return queryNodes(*db, b);
    }

    if (resource == "reachable" && m == "GET") {
        uint64_t id = 0;
        if (idToken.empty() || !toU64(idToken, id)) {
            return errorResult(400, "bad_request", "用法: GET /api/reachable/{id}?depth=2");
        }
        return reachableFrom(*db, id, params);
    }

    // 注意：/api/path/weighted 里 "weighted" 是 path 后面的那段（idToken），不是再下一层
    if (resource == "path") {
        if (idToken == "weighted" && m == "POST") return weightedPath(*db, body);
        if (idToken == "weighted" && m == "GET") {
            minijson::Json b = minijson::Json::object();
            b.set("from", minijson::Json(static_cast<long long>(paramU64(params, "from", 0))));
            b.set("to", minijson::Json(static_cast<long long>(paramU64(params, "to", 0))));
            // 不传 weightKey 时走关系的一等 weight 字段；显式传了才去 props 里找
            const std::string wk = paramStr(params, "weightKey");
            if (!wk.empty()) b.set("weightKey", wk);
            b.set("dir", paramStr(params, "dir", "out"));
            if (!paramStr(params, "type").empty()) {
                minijson::Json types = minijson::Json::array();
                for (const auto& t : splitList(paramStr(params, "type"))) types.push_back(t);
                b.set("types", types);
            }
            return weightedPath(*db, b);
        }
        if (idToken.empty() && m == "GET") return shortestPath(*db, params);
        return errorResult(404, "not_found", "用法: GET /api/path?from=1&to=2 或 POST /api/path/weighted");
    }

    // ---------------- 运维 ----------------
    if (resource == "stats" && m == "GET") return stats(*db, graphName);

    if (resource == "indexes") {
        if (m == "GET")    return listIndexes(*db);
        if (m == "POST")   return createIndex(*db, body);
        if (m == "DELETE") return dropIndex(*db, params, body);
        return errorResult(405, "method_not_allowed", "/api/indexes 只支持 GET/POST/DELETE");
    }

    if (resource == "admin") {
        if (idToken == "save" && m == "POST")  return saveAll();
        if (idToken == "clear" && m == "POST") return clearGraph(*db, graphName, body);
        return errorResult(404, "not_found", "用法: POST /api/admin/save 或 POST /api/admin/clear");
    }

    return errorResult(404, "not_found", "未知资源: /api/" + resource);
}

// =============================================================================
// 图（数据库）管理接口
// =============================================================================

ApiResult GraphService::routeGraphs(const std::vector<std::string>& seg, const Params& params,
                                    const minijson::Json& body, const std::string& method) {
    // /api/graphs
    if (seg.size() == 2) {
        if (method == "GET")  return listGraphs();
        if (method == "POST") return createGraph(body);
        return errorResult(405, "method_not_allowed", "/api/graphs 只支持 GET/POST");
    }
    // /api/graphs/{name}
    const std::string name = seg[2];
    if (seg.size() == 3) {
        if (method == "GET")    return graphInfo(name);
        if (method == "DELETE") return deleteGraph(name, params, body);
        return errorResult(405, "method_not_allowed", "/api/graphs/{name} 只支持 GET/DELETE");
    }
    // /api/graphs/{name}/{action}
    if (seg.size() == 4) {
        minigraph::GraphDB* db = findGraph(name);
        if (!db) return errorResult(404, "graph_not_found", "图不存在: " + name);
        if (seg[3] == "save" && method == "POST")  return saveGraph(*db, name);
        if (seg[3] == "clear" && method == "POST") return clearGraph(*db, name, body);
        return errorResult(404, "not_found", "用法: POST /api/graphs/{name}/save 或 /clear");
    }
    return errorResult(404, "not_found", "未知资源: /api/graphs/...");
}

ApiResult GraphService::listGraphs() {
    minijson::Json items = minijson::Json::array();
    for (const auto& kv : graphs_) {
        minigraph::GraphDB& db = *kv.second;
        minijson::Json o = minijson::Json::object();
        o.set("name", kv.first);
        o.set("file", db.path());
        o.set("fileSize", minijson::Json(static_cast<long long>(minigraph::storage::fileSize(db.path()))));
        o.set("nodes", minijson::Json(static_cast<long long>(db.nodeCount())));
        o.set("relationships", minijson::Json(static_cast<long long>(db.relationshipCount())));
        o.set("isDefault", minijson::Json(kv.first == defaultGraph_));
        items.push_back(o);
    }
    minijson::Json data = minijson::Json::object();
    data.set("dataDir", dir_);
    data.set("defaultGraph", defaultGraph_);
    data.set("total", minijson::Json(static_cast<long long>(graphs_.size())));
    data.set("items", items);
    return okResult(data);
}

ApiResult GraphService::createGraph(const minijson::Json& body) {
    std::string name = body["name"].asStringOr("");
    if (name.empty()) name = body["graph"].asStringOr("");
    name = trim(name);
    std::string verr;
    if (!validGraphName(name, verr)) return errorResult(400, "bad_request", verr);
    if (graphs_.count(name)) {
        return errorResult(409, "already_exists", "图已存在: " + name);
    }

    auto db = std::make_unique<minigraph::GraphDB>(graphFilePath(name));
    db->setAutoSave(autoSave_);
    if (!db->open()) return errorResult(500, "internal_error", "无法创建图: " + db->path());
    db->save();                      // 立刻生成文件，方便在数据目录里看到

    const std::string file = db->path();
    graphs_[name] = std::move(db);

    minijson::Json data = minijson::Json::object();
    data.set("name", name);
    data.set("file", file);
    data.set("created", minijson::Json(true));
    data.set("isDefault", minijson::Json(name == defaultGraph_));
    return okResult(data, 201);
}

ApiResult GraphService::graphInfo(const std::string& name) {
    minigraph::GraphDB* db = findGraph(name);
    if (!db) return errorResult(404, "graph_not_found", "图不存在: " + name);
    ApiResult r = stats(*db, name);
    if (minijson::Json* d = r.body.find("data")) d->set("isDefault", minijson::Json(name == defaultGraph_));
    return r;
}

ApiResult GraphService::deleteGraph(const std::string& name, const Params& params,
                                    const minijson::Json& body) {
    const bool confirm = paramBool(params, "confirm", false) || body["confirm"].asBoolOr(false);
    if (!confirm) {
        return errorResult(400, "bad_request",
                           "删除整张图是危险操作，请带上 ?confirm=1 或 {\"confirm\": true}");
    }
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return errorResult(404, "graph_not_found", "图不存在: " + name);

    const std::string file = it->second->path();
    it->second->close();                       // 先落盘再关闭
    graphs_.erase(it);
    std::remove(file.c_str());
    std::remove((file + ".tmp").c_str());

    // 默认图被删了，就换一张；一张都不剩则重建空的默认图
    if (defaultGraph_ == name) {
        defaultGraph_ = graphs_.empty() ? std::string(kDefaultGraphName) : graphs_.begin()->first;
    }
    if (!graphs_.count(defaultGraph_)) {
        auto db = std::make_unique<minigraph::GraphDB>(graphFilePath(defaultGraph_));
        db->setAutoSave(autoSave_);
        db->open();
        graphs_[defaultGraph_] = std::move(db);
    }

    minijson::Json data = minijson::Json::object();
    data.set("name", name);
    data.set("file", file);
    data.set("deleted", minijson::Json(true));
    data.set("defaultGraph", defaultGraph_);
    data.set("remaining", minijson::Json(static_cast<long long>(graphs_.size())));
    return okResult(data);
}

ApiResult GraphService::saveGraph(minigraph::GraphDB& db, const std::string& name) {
    const bool ok = db.save();
    if (!ok) return errorResult(500, "internal_error", "落盘失败: " + db.path());
    minijson::Json data = minijson::Json::object();
    data.set("saved", minijson::Json(true));
    data.set("graph", name);
    data.set("dataFile", db.path());
    return okResult(data);
}

ApiResult GraphService::clearGraph(minigraph::GraphDB& db, const std::string& name,
                                   const minijson::Json& body) {
    if (!body["confirm"].asBoolOr(false)) {
        return errorResult(400, "bad_request", "清空图是危险操作，请带上 {\"confirm\": true}");
    }
    db.clear();
    db.save();
    minijson::Json data = minijson::Json::object();
    data.set("cleared", minijson::Json(true));
    data.set("graph", name);
    data.set("nodes", minijson::Json(static_cast<long long>(db.nodeCount())));
    data.set("relationships", minijson::Json(static_cast<long long>(db.relationshipCount())));
    return okResult(data);
}

// =============================================================================
// 节点
// =============================================================================

ApiResult GraphService::createNode(minigraph::GraphDB& db, const minijson::Json& body) {
    std::vector<std::string> labels;
    if (const minijson::Json* lp = body.find("labels")) {
        if (lp->isString()) {
            labels.push_back(lp->asString());
        } else {
            std::string err;
            if (!jsonStringArray(*lp, labels, err)) {
                return errorResult(400, "bad_request", "labels " + err);
            }
        }
    }

    minigraph::Props props;
    std::string err;
    if (const minijson::Json* pp = body.find("props")) {
        if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
    }

    const uint64_t id = db.createNode(labels, props);
    const minigraph::Node* n = db.getNode(id);
    if (!n) return errorResult(500, "internal_error", "节点创建失败");

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("node", nodeToJson(*n));
    return okResult(data, 201);
}

ApiResult GraphService::listNodes(minigraph::GraphDB& db, const Params& params) {
    const std::string label = paramStr(params, "label");
    const std::string key   = paramStr(params, "key");
    const std::string op    = paramStr(params, "op");
    const size_t offset = static_cast<size_t>(paramU64(params, "offset", 0));
    size_t limit = static_cast<size_t>(paramU64(params, "limit", kDefaultLimit));
    if (limit == 0 || limit > kMaxLimit) limit = kMaxLimit;
    const bool full = paramBool(params, "full", false);

    std::vector<std::pair<std::string, WhereCond>> conds;
    if (!key.empty()) {
        if (!params.count("value")) {
            return errorResult(400, "bad_request", "指定了 key 时必须同时指定 value");
        }
        std::string err;
        minigraph::Value v;
        if (!valueFromText(paramStr(params, "value"), paramStr(params, "vtype", "auto"), v, err)) {
            return errorResult(400, "bad_request", err);
        }
        WhereCond c;
        if (!op.empty() && op != "eq") {
            c.isOp = true;
            c.op = "$" + op;
            c.value = v;
        } else {
            c.value = v;
        }
        conds.emplace_back(key, std::move(c));
    }

    std::vector<uint64_t> ids;
    if (!label.empty() && conds.size() == 1 && !conds[0].second.isOp &&
        conds[0].second.value.isScalar()) {
        ids = db.findNodes(label, conds[0].first, conds[0].second.value);   // 走二级索引
    } else if (conds.empty() && label.empty()) {
        ids = db.allNodeIds();
    } else {
        ids = db.findNodes([&](const minigraph::Node& n) { return matchesNode(n, label, conds); });
    }

    const std::vector<uint64_t> page = slice(ids, offset, limit);

    minijson::Json items = minijson::Json::array();
    for (uint64_t id : page) {
        if (full) {
            if (const minigraph::Node* n = db.getNode(id)) items.push_back(nodeToJson(*n));
        } else {
            items.push_back(minijson::Json(static_cast<long long>(id)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("total", minijson::Json(static_cast<long long>(ids.size())));
    data.set("offset", minijson::Json(static_cast<long long>(offset)));
    data.set("limit", minijson::Json(static_cast<long long>(limit)));
    data.set("items", items);
    return okResult(data);
}

ApiResult GraphService::getNode(minigraph::GraphDB& db, uint64_t id) {
    const minigraph::Node* n = db.getNode(id);
    if (!n) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));
    return okResult(nodeToJson(*n));
}

ApiResult GraphService::mutateNode(minigraph::GraphDB& db, uint64_t id,
                                   const minijson::Json& body, bool replace) {
    const minigraph::Node* cur = db.getNode(id);
    if (!cur) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    if (replace) {
        // PUT：整体替换标签与属性
        if (const minijson::Json* lp = body.find("labels")) {
            std::vector<std::string> want;
            std::string err;
            if (!jsonStringArray(*lp, want, err)) {
                return errorResult(400, "bad_request", "labels " + err);
            }
            std::vector<std::string> current = db.getNode(id)->labels;
            for (const auto& c : current) {
                if (std::find(want.begin(), want.end(), c) == want.end()) db.removeNodeLabel(id, c);
            }
            for (const auto& w : want) db.addNodeLabel(id, w);
        }
        if (const minijson::Json* pp = body.find("props")) {
            minigraph::Props props;
            std::string err;
            if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
            db.replaceNodeProperties(id, props);
        }
    } else {
        // PATCH：增量修改
        if (const minijson::Json* al = body.find("addLabels")) {
            std::vector<std::string> labels;
            std::string err;
            if (!jsonStringArray(*al, labels, err)) {
                return errorResult(400, "bad_request", "addLabels " + err);
            }
            for (const auto& l : labels) db.addNodeLabel(id, l);
        }
        if (const minijson::Json* rl = body.find("removeLabels")) {
            std::vector<std::string> labels;
            std::string err;
            if (!jsonStringArray(*rl, labels, err)) {
                return errorResult(400, "bad_request", "removeLabels " + err);
            }
            for (const auto& l : labels) db.removeNodeLabel(id, l);
        }
        if (const minijson::Json* pp = body.find("props")) {
            minigraph::Props props;
            std::string err;
            if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
            db.updateNode(id, props);
        }
        if (const minijson::Json* rp = body.find("removeProps")) {
            std::vector<std::string> keys;
            std::string err;
            if (!jsonStringArray(*rp, keys, err)) {
                return errorResult(400, "bad_request", "removeProps " + err);
            }
            for (const auto& k : keys) db.removeNodeProperty(id, k);
        }
        // 数组 / 对象属性的增量操作
        std::string err;
        bool ok = applyContainerOps(
            body,
            [&](const std::string& k) -> const minigraph::Value* {
                const minigraph::Node* n = db.getNode(id);
                if (!n) return nullptr;
                auto it = n->props.find(k);
                return it == n->props.end() ? nullptr : &it->second;
            },
            [&](const std::string& k, const minigraph::Value& v) { db.setNodeProperty(id, k, v); },
            err);
        if (!ok) return errorResult(400, "bad_request", err);
    }

    const minigraph::Node* n = db.getNode(id);
    return okResult(nodeToJson(n ? *n : minigraph::Node()));
}

ApiResult GraphService::deleteNode(minigraph::GraphDB& db, uint64_t id) {
    if (!db.getNode(id)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    const size_t cascade = db.degree(id, minigraph::Direction::Both, {});
    db.removeNode(id);

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("deleted", minijson::Json(true));
    data.set("cascadeRelationships", minijson::Json(static_cast<long long>(cascade)));
    return okResult(data);
}

ApiResult GraphService::nodeNeighbors(minigraph::GraphDB& db, uint64_t id, const Params& params) {
    const minigraph::Direction dir = parseDirection(paramStr(params, "dir", "out"));
    const std::vector<std::string> types = splitList(paramStr(params, "type"));
    const bool full = paramBool(params, "full", false);

    if (!db.getNode(id)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    const std::vector<uint64_t> ids = db.neighbors(id, dir, types);
    minijson::Json items = minijson::Json::array();
    for (uint64_t nb : ids) {
        if (full) {
            if (const minigraph::Node* n = db.getNode(nb)) items.push_back(nodeToJson(*n));
        } else {
            items.push_back(minijson::Json(static_cast<long long>(nb)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("dir", std::string(directionName(dir)));
    data.set("count", minijson::Json(static_cast<long long>(ids.size())));
    data.set("neighbors", items);
    return okResult(data);
}

ApiResult GraphService::nodeDegree(minigraph::GraphDB& db, uint64_t id, const Params& params) {
    const minigraph::Direction dir = parseDirection(paramStr(params, "dir", "both"));
    const std::vector<std::string> types = splitList(paramStr(params, "type"));

    if (!db.getNode(id)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("dir", std::string(directionName(dir)));
    data.set("degree", minijson::Json(static_cast<long long>(db.degree(id, dir, types))));
    return okResult(data);
}

ApiResult GraphService::nodeRelationships(minigraph::GraphDB& db, uint64_t id,
                                          const Params& params) {
    const minigraph::Direction dir = parseDirection(paramStr(params, "dir", "both"));
    const std::vector<std::string> types = splitList(paramStr(params, "type"));
    const bool full = paramBool(params, "full", true);

    if (!db.getNode(id)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    const std::vector<uint64_t> ids = db.incidentRelationships(id, dir, types);
    minijson::Json items = minijson::Json::array();
    for (uint64_t rid : ids) {
        if (full) {
            if (const minigraph::Relationship* r = db.getRelationship(rid)) {
                items.push_back(relationshipToJson(*r));
            }
        } else {
            items.push_back(minijson::Json(static_cast<long long>(rid)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("dir", std::string(directionName(dir)));
    data.set("count", minijson::Json(static_cast<long long>(ids.size())));
    data.set("relationships", items);
    return okResult(data);
}

// =============================================================================
// 关系
// =============================================================================

ApiResult GraphService::createRelationship(minigraph::GraphDB& db, const minijson::Json& body) {
    std::string err;
    uint64_t from = 0, to = 0;
    if (!jsonId(body, "from", from, err)) return errorResult(400, "bad_request", err);
    if (!jsonId(body, "to", to, err))     return errorResult(400, "bad_request", err);

    const minijson::Json* tp = body.find("type");
    if (!tp || !tp->isString() || tp->asString().empty()) {
        return errorResult(400, "bad_request", "缺少字段 \"type\"（关系类型，如 KNOWS）");
    }

    minigraph::Props props;
    if (const minijson::Json* pp = body.find("props")) {
        if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
    }

    // 边权：可以不传（默认 1.0），传了就必须是有限数字
    double weight = minigraph::Relationship::kDefaultWeight;
    if (const minijson::Json* wp = body.find("weight")) {
        if (wp->isNull()) {
            weight = minigraph::Relationship::kDefaultWeight;
        } else if (!jsonWeight(*wp, weight, err)) {
            return errorResult(400, "bad_request", err);
        }
    }

    if (!db.getNode(from)) return errorResult(404, "not_found", "起点节点不存在: " + std::to_string(from));
    if (!db.getNode(to))   return errorResult(404, "not_found", "终点节点不存在: " + std::to_string(to));

    const uint64_t id = db.createRelationship(from, to, tp->asString(), props, weight);
    if (id == 0) return errorResult(500, "internal_error", "关系创建失败");

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("relationship", relationshipToJson(*db.getRelationship(id)));
    return okResult(data, 201);
}

ApiResult GraphService::listRelationships(minigraph::GraphDB& db, const Params& params) {
    const std::string type = paramStr(params, "type");
    const std::string fromStr = paramStr(params, "from");
    const std::string toStr = paramStr(params, "to");
    const size_t offset = static_cast<size_t>(paramU64(params, "offset", 0));
    size_t limit = static_cast<size_t>(paramU64(params, "limit", kDefaultLimit));
    if (limit == 0 || limit > kMaxLimit) limit = kMaxLimit;
    const bool full = paramBool(params, "full", false);

    std::vector<uint64_t> ids;
    if (!fromStr.empty() && !toStr.empty()) {
        uint64_t from = 0, to = 0;
        if (!toU64(fromStr, from) || !toU64(toStr, to)) {
            return errorResult(400, "bad_request", "from/to 必须是正整数");
        }
        ids = db.findRelationships(from, to, parseDirection(paramStr(params, "dir", "out")),
                                    splitList(type));
    } else if (!type.empty()) {
        ids = db.findRelationships(type);
    } else {
        ids = db.allRelationshipIds();
    }

    // 边权区间过滤：与上面的 type/from/to 条件取交集
    const std::string minWs = paramStr(params, "minWeight");
    const std::string maxWs = paramStr(params, "maxWeight");
    if (!minWs.empty() || !maxWs.empty()) {
        double minW = -std::numeric_limits<double>::infinity();
        double maxW =  std::numeric_limits<double>::infinity();
        if (!minWs.empty() && !parseDoubleStr(minWs, minW)) {
            return errorResult(400, "bad_request", "minWeight 必须是数字: " + minWs);
        }
        if (!maxWs.empty() && !parseDoubleStr(maxWs, maxW)) {
            return errorResult(400, "bad_request", "maxWeight 必须是数字: " + maxWs);
        }
        std::vector<uint64_t> filtered;
        filtered.reserve(ids.size());
        for (uint64_t rid : ids) {
            if (const minigraph::Relationship* r = db.getRelationship(rid)) {
                if (r->weight >= minW && r->weight <= maxW) filtered.push_back(rid);
            }
        }
        ids = std::move(filtered);
    }

    const std::vector<uint64_t> page = slice(ids, offset, limit);
    minijson::Json items = minijson::Json::array();
    for (uint64_t rid : page) {
        if (full) {
            if (const minigraph::Relationship* r = db.getRelationship(rid)) {
                items.push_back(relationshipToJson(*r));
            }
        } else {
            items.push_back(minijson::Json(static_cast<long long>(rid)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("total", minijson::Json(static_cast<long long>(ids.size())));
    data.set("offset", minijson::Json(static_cast<long long>(offset)));
    data.set("limit", minijson::Json(static_cast<long long>(limit)));
    data.set("items", items);
    return okResult(data);
}

ApiResult GraphService::getRelationship(minigraph::GraphDB& db, uint64_t id) {
    const minigraph::Relationship* r = db.getRelationship(id);
    if (!r) return errorResult(404, "not_found", "关系不存在: " + std::to_string(id));
    return okResult(relationshipToJson(*r));
}

ApiResult GraphService::mutateRelationship(minigraph::GraphDB& db, uint64_t id,
                                           const minijson::Json& body, bool replace) {
    const minigraph::Relationship* cur = db.getRelationship(id);
    if (!cur) return errorResult(404, "not_found", "关系不存在: " + std::to_string(id));

    // 0) 先解析边权：改类型和改属性两条路径都要用它
    //    PUT/PATCH 都不传 weight 时保持原权重不变（weight 是独立字段，不在 props 里，
    //    不会因为整体替换属性而被顺手清掉）
    bool hasWeight = false;
    double weight = 0.0;
    if (const minijson::Json* wp = body.find("weight")) {
        std::string err;
        if (!jsonWeight(*wp, weight, err)) return errorResult(400, "bad_request", err);
        hasWeight = true;
    }

    // 1) 改类型：GraphDB 未提供 setType，用"删除 + 重建"实现。
    //    删除后 id 会进入可复用池，重建时通常能拿回同一个 id。
    if (const minijson::Json* tp = body.find("type")) {
        if (!tp->isString() || tp->asString().empty()) {
            return errorResult(400, "bad_request", "type 必须是非空字符串");
        }
        const std::string newType = tp->asString();
        if (newType != cur->type) {
            const uint64_t from = cur->from;
            const uint64_t to = cur->to;
            const double oldWeight = cur->weight;   // 提前取，removeRelationship 后 cur 失效
            minigraph::Props props = cur->props;
            if (const minijson::Json* pp = body.find("props")) {
                std::string err;
                if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
            }
            db.removeRelationship(id);
            const uint64_t newId =
                db.createRelationship(from, to, newType, props, hasWeight ? weight : oldWeight);
            if (newId == 0) return errorResult(500, "internal_error", "修改关系类型失败");

            const minigraph::Relationship* n = db.getRelationship(newId);
            minijson::Json data = minijson::Json::object();
            data.set("id", minijson::Json(static_cast<long long>(newId)));
            data.set("idChanged", minijson::Json(newId != id));
            data.set("relationship", relationshipToJson(n ? *n : minigraph::Relationship()));
            return okResult(data);
        }
    }

    // 2) 属性：PUT 先清空再写入，PATCH 仅合并
    if (const minijson::Json* pp = body.find("props")) {
        minigraph::Props props;
        std::string err;
        if (!propsFromJson(*pp, props, err)) return errorResult(400, "bad_request", err);
        if (replace) {
            std::vector<std::string> keys;
            for (const auto& kv : db.getRelationship(id)->props) keys.push_back(kv.first);
            for (const auto& k : keys) db.removeRelationshipProperty(id, k);
        }
        db.updateRelationship(id, props);
    }

    if (const minijson::Json* rp = body.find("removeProps")) {
        std::vector<std::string> keys;
        std::string err;
        if (!jsonStringArray(*rp, keys, err)) {
            return errorResult(400, "bad_request", "removeProps " + err);
        }
        for (const auto& k : keys) db.removeRelationshipProperty(id, k);
    }

    if (!replace) {
        std::string err;
        bool ok = applyContainerOps(
            body,
            [&](const std::string& k) -> const minigraph::Value* {
                const minigraph::Relationship* r = db.getRelationship(id);
                if (!r) return nullptr;
                auto it = r->props.find(k);
                return it == r->props.end() ? nullptr : &it->second;
            },
            [&](const std::string& k, const minigraph::Value& v) {
                db.setRelationshipProperty(id, k, v);
            },
            err);
        if (!ok) return errorResult(400, "bad_request", err);
    }

    // 3) 边权
    if (hasWeight) {
        if (!db.setRelationshipWeight(id, weight)) {
            return errorResult(500, "internal_error", "设置边权失败: " + std::to_string(id));
        }
    }

    const minigraph::Relationship* r = db.getRelationship(id);
    return okResult(relationshipToJson(r ? *r : minigraph::Relationship()));
}

// 单条边设权：PATCH /api/relationships/{id}/weight  {"weight": 3.5}
ApiResult GraphService::setRelationshipWeight(minigraph::GraphDB& db, uint64_t id,
                                              const minijson::Json& body) {
    if (!db.getRelationship(id)) {
        return errorResult(404, "not_found", "关系不存在: " + std::to_string(id));
    }
    const minijson::Json* wp = body.find("weight");
    if (!wp || wp->isNull()) {
        return errorResult(400, "bad_request", "缺少字段 \"weight\"（数字）");
    }
    double w = 0.0;
    std::string err;
    if (!jsonWeight(*wp, w, err)) return errorResult(400, "bad_request", err);
    if (!db.setRelationshipWeight(id, w)) {
        return errorResult(500, "internal_error", "设置边权失败: " + std::to_string(id));
    }

    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("weight", minijson::Json(w));
    data.set("relationship", relationshipToJson(*db.getRelationship(id)));
    return okResult(data);
}

// 批量设权：POST /api/relationships/weights
//   两种写法都支持：
//     {"weights": [[1, 2.5], [2, 0.5]]}
//     {"weights": {"1": 2.5, "2": 0.5}}
ApiResult GraphService::setRelationshipWeights(minigraph::GraphDB& db,
                                               const minijson::Json& body) {
    const minijson::Json* wp = body.find("weights");
    if (!wp || wp->isNull()) {
        return errorResult(400, "bad_request", "缺少字段 \"weights\"（对象或 [[id, weight], ...] 数组）");
    }

    std::map<uint64_t, double> weights;
    std::vector<std::string> errors;

    if (wp->isObject()) {
        for (const auto& kv : wp->asObject()) {
            uint64_t id = 0;
            if (!toU64(kv.first, id)) {
                errors.push_back("关系 id 非法: " + kv.first);
                continue;
            }
            std::string err;
            double w = 0.0;
            if (!jsonWeight(kv.second, w, err)) {
                errors.push_back("边 " + kv.first + " 的 " + err);
                continue;
            }
            weights[id] = w;
        }
    } else if (wp->isArray()) {
        for (const minijson::Json& item : wp->asArray()) {
            if (!item.isArray() || item.asArray().size() != 2) {
                errors.push_back("数组元素必须是 [关系id, 权重]");
                continue;
            }
            const minijson::Json& idj = item.asArray()[0];
            uint64_t id = 0;
            if (!idj.isNumber() || !toU64(std::to_string(idj.asInt()), id)) {
                errors.push_back("数组元素的关系 id 非法");
                continue;
            }
            std::string err;
            double w = 0.0;
            if (!jsonWeight(item.asArray()[1], w, err)) {
                errors.push_back("边 " + std::to_string(id) + " 的 " + err);
                continue;
            }
            weights[id] = w;
        }
    } else {
        return errorResult(400, "bad_request", "weights 必须是对象或数组");
    }

    if (weights.empty()) {
        return errorResult(400, "bad_request",
                           errors.empty() ? "weights 为空" : errors.front());
    }

    const std::size_t applied = db.setRelationshipWeights(weights);

    minijson::Json data = minijson::Json::object();
    data.set("requested", minijson::Json(static_cast<long long>(weights.size())));
    data.set("applied", minijson::Json(static_cast<long long>(applied)));

    minijson::Json skipped = minijson::Json::array();
    for (const std::string& e : errors) skipped.push_back(e);
    data.set("errors", skipped);

    // 回显设权之后的结果，方便调用方一次核对
    minijson::Json appliedList = minijson::Json::array();
    for (const auto& kv : weights) {
        if (const minigraph::Relationship* r = db.getRelationship(kv.first)) {
            minijson::Json o = minijson::Json::object();
            o.set("id", minijson::Json(static_cast<long long>(r->id)));
            o.set("weight", minijson::Json(r->weight));
            appliedList.push_back(o);
        }
    }
    data.set("relationships", appliedList);
    return okResult(data);
}

// 全库边权统计：GET /api/relationships/weight-stats
ApiResult GraphService::weightStats(minigraph::GraphDB& db) {
    const minigraph::WeightStats st = db.weightStats();
    minijson::Json data = minijson::Json::object();
    data.set("count", minijson::Json(static_cast<long long>(st.count)));
    data.set("weightedCount", minijson::Json(static_cast<long long>(st.weightedCount)));
    data.set("defaultWeight", minijson::Json(minigraph::Relationship::kDefaultWeight));
    if (st.count > 0) {
        data.set("sum", minijson::Json(st.sum));
        data.set("min", minijson::Json(st.min));
        data.set("max", minijson::Json(st.max));
        data.set("avg", minijson::Json(st.avg));
    }
    return okResult(data);
}

ApiResult GraphService::deleteRelationship(minigraph::GraphDB& db, uint64_t id) {
    if (!db.getRelationship(id)) {
        return errorResult(404, "not_found", "关系不存在: " + std::to_string(id));
    }
    db.removeRelationship(id);
    minijson::Json data = minijson::Json::object();
    data.set("id", minijson::Json(static_cast<long long>(id)));
    data.set("deleted", minijson::Json(true));
    return okResult(data);
}

// =============================================================================
// 查询 / 遍历 / 算法
// =============================================================================

ApiResult GraphService::queryNodes(minigraph::GraphDB& db, const minijson::Json& body) {
    const std::string label = body["label"].asStringOr("");

    std::vector<std::pair<std::string, WhereCond>> conds;
    if (const minijson::Json* wp = body.find("where")) {
        if (wp->isNull()) {
            // 空条件：忽略
        } else if (!wp->isObject()) {
            return errorResult(400, "bad_request", "where 必须是 JSON 对象");
        } else {
            for (const auto& kv : wp->asObject()) {
                WhereCond c;
                std::string err;
                if (!parseCond(kv.second, c, err)) {
                    return errorResult(400, "bad_request", "where." + kv.first + ": " + err);
                }
                conds.emplace_back(kv.first, std::move(c));
            }
        }
    }

    const size_t offset = static_cast<size_t>(body["offset"].asIntOr(0));
    long long rawLimit = body["limit"].asIntOr(static_cast<long long>(kDefaultLimit));
    size_t limit = (rawLimit <= 0 || static_cast<size_t>(rawLimit) > kMaxLimit)
                       ? kMaxLimit
                       : static_cast<size_t>(rawLimit);
    const bool full = body["full"].asBoolOr(false);

    std::vector<uint64_t> ids;
    if (!label.empty() && conds.size() == 1 && !conds[0].second.isOp &&
        conds[0].second.value.isScalar()) {
        // 单条件且给了标签：优先走二级索引
        ids = db.findNodes(label, conds[0].first, conds[0].second.value);
    } else if (conds.empty() && label.empty()) {
        ids = db.allNodeIds();
    } else {
        ids = db.findNodes([&](const minigraph::Node& n) { return matchesNode(n, label, conds); });
    }

    const std::vector<uint64_t> page = slice(ids, offset, limit);
    minijson::Json items = minijson::Json::array();
    for (uint64_t id : page) {
        if (full) {
            if (const minigraph::Node* n = db.getNode(id)) items.push_back(nodeToJson(*n));
        } else {
            items.push_back(minijson::Json(static_cast<long long>(id)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("total", minijson::Json(static_cast<long long>(ids.size())));
    data.set("offset", minijson::Json(static_cast<long long>(offset)));
    data.set("limit", minijson::Json(static_cast<long long>(limit)));
    data.set("items", items);
    return okResult(data);
}

ApiResult GraphService::reachableFrom(minigraph::GraphDB& db, uint64_t id, const Params& params) {
    const int depth = static_cast<int>(paramU64(params, "depth", 1));
    const minigraph::Direction dir = parseDirection(paramStr(params, "dir", "out"));
    const std::vector<std::string> types = splitList(paramStr(params, "type"));
    const bool full = paramBool(params, "full", false);

    if (!db.getNode(id)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(id));

    const std::vector<uint64_t> ids = db.reachable(id, depth, dir, types);
    minijson::Json items = minijson::Json::array();
    for (uint64_t nid : ids) {
        if (full) {
            if (const minigraph::Node* n = db.getNode(nid)) items.push_back(nodeToJson(*n));
        } else {
            items.push_back(minijson::Json(static_cast<long long>(nid)));
        }
    }

    minijson::Json data = minijson::Json::object();
    data.set("from", minijson::Json(static_cast<long long>(id)));
    data.set("depth", minijson::Json(depth));
    data.set("dir", std::string(directionName(dir)));
    data.set("count", minijson::Json(static_cast<long long>(ids.size())));
    data.set("nodes", items);
    return okResult(data);
}

ApiResult GraphService::shortestPath(minigraph::GraphDB& db, const Params& params) {
    const std::string fromStr = paramStr(params, "from");
    const std::string toStr = paramStr(params, "to");
    uint64_t from = 0, to = 0;
    if (fromStr.empty() || !toU64(fromStr, from) || toStr.empty() || !toU64(toStr, to)) {
        return errorResult(400, "bad_request", "用法: GET /api/path?from=1&to=2&dir=out&type=KNOWS");
    }
    const minigraph::Direction dir = parseDirection(paramStr(params, "dir", "out"));
    const std::vector<std::string> types = splitList(paramStr(params, "type"));

    if (!db.getNode(from)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(from));
    if (!db.getNode(to))   return errorResult(404, "not_found", "节点不存在: " + std::to_string(to));

    const std::vector<uint64_t> path = db.shortestPath(from, to, dir, types);

    minijson::Json data = minijson::Json::object();
    data.set("found", minijson::Json(!path.empty()));
    data.set("from", minijson::Json(static_cast<long long>(from)));
    data.set("to", minijson::Json(static_cast<long long>(to)));
    data.set("hops", minijson::Json(path.empty() ? -1 : static_cast<long long>(path.size() - 1)));
    data.set("nodes", idArray(path));
    return okResult(data);
}

ApiResult GraphService::weightedPath(minigraph::GraphDB& db, const minijson::Json& body) {
    std::string err;
    uint64_t from = 0, to = 0;
    if (!jsonId(body, "from", from, err)) return errorResult(400, "bad_request", err);
    if (!jsonId(body, "to", to, err))     return errorResult(400, "bad_request", err);

    // weightKey 为空 -> 用关系的一等 weight 字段（推荐）
    // weightKey 非空 -> 从 props[weightKey] 取（兼容旧用法）
    const minijson::Json* wkj = body.find("weightKey");
    const std::string weightKey = (wkj && wkj->isString()) ? wkj->asString() : std::string();
    const minigraph::Direction dir = parseDirection(body["dir"].asStringOr("out"));
    std::vector<std::string> types;
    if (const minijson::Json* tp = body.find("types")) {
        if (!jsonStringArray(*tp, types, err)) {
            return errorResult(400, "bad_request", "types " + err);
        }
    }

    if (!db.getNode(from)) return errorResult(404, "not_found", "节点不存在: " + std::to_string(from));
    if (!db.getNode(to))   return errorResult(404, "not_found", "节点不存在: " + std::to_string(to));

    const minigraph::WeightedPath wp =
        db.shortestWeightedPath(from, to, weightKey, dir, types);

    minijson::Json data = minijson::Json::object();
    data.set("found", minijson::Json(wp.found));
    data.set("cost", minijson::Json(wp.cost));
    data.set("weightSource", weightKey.empty() ? std::string("weight")
                                              : std::string("props.") + weightKey);
    data.set("weightKey", weightKey);
    data.set("nodes", idArray(wp.nodes));
    data.set("relationships", idArray(wp.rels));

    // 每一跳的权重明细：便于核对 cost 是怎么算出来的
    minijson::Json legs = minijson::Json::array();
    for (double w : wp.legWeights) legs.push_back(minijson::Json(w));
    data.set("legWeights", legs);

    // 负权会破坏 Dijkstra 的正确性，明确告知而不是静默给出错误结果
    data.set("negativeWeight", minijson::Json(wp.negativeWeight));
    if (wp.negativeWeight) {
        data.set("warning",
                 std::string("路径上存在负权边，Dijkstra 不支持负权，cost 不可信；"
                             "如需正确处理请改用 Bellman-Ford 或先把权重平移为非负"));
    }
    return okResult(data);
}

// =============================================================================
// 运维
// =============================================================================

ApiResult GraphService::stats(minigraph::GraphDB& db, const std::string& name) {
    // 标签 / 关系类型分布：用全表统计
    minijson::Json labels = minijson::Json::object();
    std::map<std::string, long long> labelCount;
    std::map<std::string, long long> typeCount;
    for (uint64_t nid : db.allNodeIds()) {
        if (const minigraph::Node* n = db.getNode(nid)) {
            for (const auto& l : n->labels) labelCount[l] += 1;
        }
    }
    for (uint64_t rid : db.allRelationshipIds()) {
        if (const minigraph::Relationship* r = db.getRelationship(rid)) typeCount[r->type] += 1;
    }
    for (const auto& kv : labelCount) labels.set(kv.first, minijson::Json(kv.second));

    minijson::Json types = minijson::Json::object();
    for (const auto& kv : typeCount) types.set(kv.first, minijson::Json(kv.second));

    minijson::Json indexes = minijson::Json::array();
    for (const auto& ix : db.listIndexes()) {
        minijson::Json o = minijson::Json::object();
        o.set("label", ix.first);
        o.set("key", ix.second);
        indexes.push_back(o);
    }

    minijson::Json data = minijson::Json::object();
    data.set("graph", name);
    data.set("dataDir", dir_);
    data.set("dataFile", db.path());
    data.set("fileSize", minijson::Json(static_cast<long long>(minigraph::storage::fileSize(db.path()))));
    data.set("nodes", minijson::Json(static_cast<long long>(db.nodeCount())));
    data.set("relationships", minijson::Json(static_cast<long long>(db.relationshipCount())));
    data.set("labels", labels);
    data.set("relationshipTypes", types);
    data.set("indexes", indexes);

    // 边权概览
    const minigraph::WeightStats wst = db.weightStats();
    minijson::Json weights = minijson::Json::object();
    weights.set("count", minijson::Json(static_cast<long long>(wst.count)));
    weights.set("weightedCount", minijson::Json(static_cast<long long>(wst.weightedCount)));
    weights.set("defaultWeight", minijson::Json(minigraph::Relationship::kDefaultWeight));
    if (wst.count > 0) {
        weights.set("sum", minijson::Json(wst.sum));
        weights.set("min", minijson::Json(wst.min));
        weights.set("max", minijson::Json(wst.max));
        weights.set("avg", minijson::Json(wst.avg));
    }
    data.set("weights", weights);

    data.set("autoSave", minijson::Json(db.autoSave()));
    data.set("dirty", minijson::Json(db.dirty()));
    return okResult(data);
}

ApiResult GraphService::listIndexes(minigraph::GraphDB& db) {
    minijson::Json items = minijson::Json::array();
    for (const auto& ix : db.listIndexes()) {
        minijson::Json o = minijson::Json::object();
        o.set("label", ix.first);
        o.set("key", ix.second);
        items.push_back(o);
    }
    minijson::Json data = minijson::Json::object();
    data.set("total", minijson::Json(static_cast<long long>(items.size())));
    data.set("items", items);
    return okResult(data);
}

ApiResult GraphService::createIndex(minigraph::GraphDB& db, const minijson::Json& body) {
    const minijson::Json* lp = body.find("label");
    const minijson::Json* kp = body.find("key");
    if (!lp || !lp->isString() || !kp || !kp->isString()) {
        return errorResult(400, "bad_request", "需要字段: {\"label\": \"Person\", \"key\": \"name\"}");
    }
    db.createIndex(lp->asString(), kp->asString());

    minijson::Json data = minijson::Json::object();
    data.set("label", lp->asString());
    data.set("key", kp->asString());
    data.set("created", minijson::Json(db.hasIndex(lp->asString(), kp->asString())));
    return okResult(data, 201);
}

ApiResult GraphService::dropIndex(minigraph::GraphDB& db, const Params& params,
                                  const minijson::Json& body) {
    std::string label = paramStr(params, "label");
    std::string key = paramStr(params, "key");
    if (label.empty()) label = body["label"].asStringOr("");
    if (key.empty())   key = body["key"].asStringOr("");
    if (label.empty() || key.empty()) {
        return errorResult(400, "bad_request", "需要参数 label 与 key（可用 query string 或请求体）");
    }
    const bool existed = db.hasIndex(label, key);
    db.dropIndex(label, key);

    minijson::Json data = minijson::Json::object();
    data.set("label", label);
    data.set("key", key);
    data.set("existed", minijson::Json(existed));
    data.set("dropped", minijson::Json(true));
    return okResult(data);
}

ApiResult GraphService::saveAll() {
    std::vector<std::string> failed;
    for (auto& kv : graphs_) {
        if (!kv.second->save()) failed.push_back(kv.first);
    }
    if (!failed.empty()) {
        std::string msg = "落盘失败: ";
        for (const auto& f : failed) msg += f + " ";
        return errorResult(500, "internal_error", msg);
    }
    minijson::Json names = minijson::Json::array();
    for (const auto& kv : graphs_) names.push_back(kv.first);
    minijson::Json data = minijson::Json::object();
    data.set("saved", minijson::Json(true));
    data.set("count", minijson::Json(static_cast<long long>(graphs_.size())));
    data.set("graphs", names);
    data.set("dataFile", dir_);
    return okResult(data);
}

ApiResult GraphService::health() {
    minijson::Json names = minijson::Json::array();
    long long totalNodes = 0, totalRels = 0;
    for (const auto& kv : graphs_) {
        names.push_back(kv.first);
        totalNodes += static_cast<long long>(kv.second->nodeCount());
        totalRels += static_cast<long long>(kv.second->relationshipCount());
    }
    minijson::Json data = minijson::Json::object();
    data.set("service", std::string("graph_server"));
    data.set("status", std::string("up"));
    data.set("dataDir", dir_);
    data.set("defaultGraph", defaultGraph_);
    data.set("graphs", names);
    data.set("graphCount", minijson::Json(static_cast<long long>(graphs_.size())));
    data.set("nodes", minijson::Json(totalNodes));
    data.set("relationships", minijson::Json(totalRels));
    data.set("autoSave", minijson::Json(autoSave_));
    return okResult(data);
}

// =============================================================================
// 接口清单（自描述）
// =============================================================================

minijson::Json GraphService::apiManifest() {
    minijson::Json list = minijson::Json::array();
    auto add = [&list](const char* method, const char* path, const char* desc) {
        minijson::Json o = minijson::Json::object();
        o.set("method", std::string(method));
        o.set("path", std::string(path));
        o.set("description", std::string(desc));
        list.push_back(o);
    };

    add("GET",    "/api/health",                    "健康检查（含全部图的概况）");
    add("GET",    "/api/manifest",                  "接口清单（本响应）");

    add("GET",    "/api/graphs",                    "列出所有图（数据库）");
    add("POST",   "/api/graphs",                    "新建图 {\"name\": \"social\"}");
    add("GET",    "/api/graphs/{name}",             "某张图的统计信息");
    add("DELETE", "/api/graphs/{name}",             "删除整张图（需 ?confirm=1）");
    add("POST",   "/api/graphs/{name}/save",        "只落盘这张图");
    add("POST",   "/api/graphs/{name}/clear",       "清空这张图（需 {\"confirm\": true}）");

    add("GET",    "/api/stats",                     "当前图的统计信息");
    add("GET",    "/api/g/{图名}/stats",             "指定图的统计信息");

    add("POST",   "/api/nodes",                     "创建节点 {labels:[], props:{}}（属性值可为数组/对象）");
    add("GET",    "/api/nodes",                     "列出节点，支持 label/key/value/op/full/limit/offset");
    add("GET",    "/api/nodes/{id}",                "查询单个节点");
    add("PUT",    "/api/nodes/{id}",                "整体替换节点的 labels 与 props");
    add("PATCH",  "/api/nodes/{id}",                "增量修改：props/addLabels/removeLabels/removeProps/arrayAppend/arrayRemove/objectMerge");
    add("DELETE", "/api/nodes/{id}",                "删除节点（级联删除其关系）");
    add("GET",    "/api/nodes/{id}/neighbors",      "邻居节点 ?dir=out|in|both&type=&full=");
    add("GET",    "/api/nodes/{id}/degree",         "度数 ?dir=&type=");
    add("GET",    "/api/nodes/{id}/relationships",  "相连的关系 ?dir=&type=");

    add("POST",   "/api/relationships",             "创建关系 {from, to, type, props, weight?}（weight 默认 1.0）");
    add("GET",    "/api/relationships",             "列出关系，支持 type/from/to/dir/full/limit/offset/minWeight/maxWeight");
    add("GET",    "/api/relationships/{id}",        "查询单个关系（含 weight）");
    add("PUT",    "/api/relationships/{id}",        "整体替换关系属性（也可改 type / weight）");
    add("PATCH",  "/api/relationships/{id}",        "增量修改：props/removeProps/arrayAppend/arrayRemove/objectMerge/weight");
    add("DELETE", "/api/relationships/{id}",        "删除关系");
    add("GET",    "/api/relationships/{id}/weight", "查询单条边的边权");
    add("PUT",    "/api/relationships/{id}/weight", "设置单条边的边权 {weight: 3.5}");
    add("PATCH",  "/api/relationships/{id}/weight", "同 PUT");
    add("POST",   "/api/relationships/weights",     "批量设权 {\"weights\": {\"1\": 2.5, \"2\": 0.5}} 或 [[1,2.5]]");
    add("GET",    "/api/relationships/weight-stats","边权统计：count/weightedCount/sum/min/max/avg");

    add("POST",   "/api/query",                     "条件查询 {label, where:{k:v 或 k:{$op:v}}, limit, offset, full}");
    add("GET",    "/api/query",                     "同上，参数走 query string（op=contains/in/gt/...）");
    add("GET",    "/api/reachable/{id}",            "N 跳内可达节点 ?depth=&dir=&type=");
    add("GET",    "/api/path",                      "无权最短路径 ?from=&to=&dir=&type=");
    add("POST",   "/api/path/weighted",             "带权最短路径 {from,to,weightKey,dir,types}");

    add("GET",    "/api/indexes",                   "列出二级索引");
    add("POST",   "/api/indexes",                   "创建索引 {label, key}");
    add("DELETE", "/api/indexes",                   "删除索引 ?label=&key=");
    add("POST",   "/api/admin/save",                "全部图立即落盘");
    add("POST",   "/api/admin/clear",               "清空当前图（需 {\"confirm\": true}）");

    minijson::Json data = minijson::Json::object();
    data.set("ok", minijson::Json(true));
    data.set("data", minijson::Json::object());
    data["data"].set("version", std::string("2.0"));
    data["data"].set("graphRouting",
                     std::string("路径 /api/g/{图名}/... > 请求头 X-Graph > ?graph= > 默认图"));
    data["data"].set("endpoints", list);
    return data;
}

}  // namespace graphserver
