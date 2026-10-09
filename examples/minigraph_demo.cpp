// -----------------------------------------------------------------------------
// MiniGraph 示例程序
//
//   ./minigraph_demo           运行内置示例（增/删/改/查/遍历/持久化）
//   ./minigraph_demo --shell   进入交互式命令行，手动玩图
//
// 本文件只调用 GraphDB 的公开接口，可作为"怎么二次开发"的参考。
// -----------------------------------------------------------------------------
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "minigraph/GraphDB.h"

using namespace minigraph;

// ---------------------------------------------------------------- 打印小工具
static void printNode(const GraphDB& db, uint64_t id) {
    const Node* n = db.getNode(id);
    if (!n) {
        std::cout << "  (节点 #" << id << " 不存在)\n";
        return;
    }
    std::cout << "  " << toString(*n) << "\n";
}

static void printRel(const GraphDB& db, uint64_t id) {
    const Relationship* r = db.getRelationship(id);
    if (!r) {
        std::cout << "  (关系 #" << id << " 不存在)\n";
        return;
    }
    std::cout << "  " << toString(*r) << "\n";
}

static void printIdList(const std::vector<uint64_t>& ids, const std::string& prefix = "  ") {
    if (ids.empty()) {
        std::cout << prefix << "(无)\n";
        return;
    }
    for (uint64_t id : ids) std::cout << prefix << "#" << id << "\n";
}

static void section(const std::string& title) {
    std::cout << "\n-------- " << title << " --------\n";
}

// 把 "key=value" 解析成属性；value 依次尝试 整数 -> 浮点 -> 布尔 -> 字符串
static std::pair<std::string, Value> parseKeyValue(const std::string& token) {
    auto pos = token.find('=');
    if (pos == std::string::npos) return {token, Value(true)};  // 只给 key 视为 true
    std::string key = token.substr(0, pos);
    std::string raw = token.substr(pos + 1);

    if (raw == "true")  return {key, Value(true)};
    if (raw == "false") return {key, Value(false)};

    try {
        size_t idx = 0;
        long long v = std::stoll(raw, &idx);
        if (idx == raw.size()) return {key, Value(static_cast<int64_t>(v))};
    } catch (...) { /* 不是整数，继续尝试 */ }

    try {
        size_t idx = 0;
        double v = std::stod(raw, &idx);
        if (idx == raw.size()) return {key, Value(v)};
    } catch (...) { /* 不是浮点，按字符串处理 */ }

    return {key, Value(raw)};
}

// ================================================================ 内置示例
static void runDemo() {
    const std::string dbPath = "social.mgdb";
    std::remove(dbPath.c_str());  // 示例每次从头开始，便于重复运行

    std::cout << "======= MiniGraph 演示 =======\n";
    std::cout << "数据文件：" << dbPath << "\n";

    GraphDB db(dbPath);
    if (!db.open()) {
        std::cerr << "打开数据库失败\n";
        return;
    }

    // ---------------------------------------------------------- 1. 增
    section("1. 创建节点（Create）");
    uint64_t alice = db.createNode({"Person", "Employee"}, {{"name", "Alice"}, {"age", 30}, {"city", "Beijing"}});
    uint64_t bob   = db.createNode({"Person"}, {{"name", "Bob"}, {"age", 26}, {"city", "Shanghai"}});
    uint64_t carol = db.createNode({"Person"}, {{"name", "Carol"}, {"age", 35}, {"city", "Beijing"}});
    uint64_t dave  = db.createNode({"Person"}, {{"name", "Dave"}, {"age", 22}, {"city", "Guangzhou"}});
    uint64_t acme  = db.createNode({"Company"}, {{"name", "Acme Inc."}, {"founded", 1998}});
    std::cout << "  创建 5 个节点，id = " << alice << ", " << bob << ", " << carol << ", " << dave
              << ", " << acme << "\n";
    printNode(db, alice);

    section("1b. 创建关系（Create）");
    db.createRelationship(alice, bob,   "KNOWS",    {{"since", 2019}, {"weight", 2.0}});
    db.createRelationship(bob,   carol, "KNOWS",    {{"since", 2020}, {"weight", 3.5}});
    db.createRelationship(carol, dave,  "KNOWS",    {{"since", 2021}, {"weight", 1.0}});
    db.createRelationship(alice, carol, "KNOWS",    {{"since", 2018}, {"weight", 10.0}});
    db.createRelationship(alice, acme,  "WORKS_AT", {{"since", 2015}});
    db.createRelationship(bob,   acme,  "WORKS_AT", {{"since", 2017}});
    std::cout << "  创建 6 条关系：\n";
    for (uint64_t id : db.allRelationshipIds()) printRel(db, id);

    // ---------------------------------------------------------- 2. 查
    section("2. 查询（Read）");
    std::cout << "  所有 :Person 节点：\n";
    printIdList(db.findNodesByLabel("Person"));

    std::cout << "  :Person 中 city=Beijing 的（无索引，全表扫描）：\n";
    printIdList(db.findNodes("Person", "city", Value("Beijing")));

    db.createIndex("Person", "city");   // 建索引后同样语句走索引
    std::cout << "  建立索引 (:Person.city) 后再查 city=Shanghai：\n";
    printIdList(db.findNodes("Person", "city", Value("Shanghai")));

    std::cout << "  谓词查询：年龄 > 25 的人：\n";
    printIdList(db.findNodes([](const Node& n) {
        if (!n.hasLabel("Person")) return false;
        auto it = n.props.find("age");
        return it != n.props.end() && it->second.asInt() > 25;
    }));

    std::cout << "  按类型查关系 WORKS_AT：\n";
    printIdList(db.findRelationships("WORKS_AT"));

    // ------------------------------------------------- 2b. 数组 / 对象属性
    section("2b. 数组与对象属性（Value 支持任意嵌套）");
    uint64_t erin = db.createNode({"Person"}, {
        {"name", "Erin"},
        {"age", 28},
        {"tags", Value(ValueList{Value("vip"), Value("early-bird")})},
        {"addr", Value(ValueMap{{"city", Value("Beijing")}, {"zip", Value("100000")}})},
    });
    printNode(db, erin);
    const Node* e = db.getNode(erin);
    std::cout << "  tags 长度 = " << e->props.at("tags").size()
              << "，tags[0] = " << e->props.at("tags").at(0)->asString() << "\n";
    std::cout << "  addr.city = " << e->props.at("addr").at("city")->asString() << "\n";
    std::cout << "  tags 是否含 vip：" << (e->props.at("tags").contains(Value("vip")) ? "是" : "否") << "\n";

    Value tags = e->props.at("tags");       // 取出 -> 追加 -> 写回
    tags.push_back(Value("gold"));
    db.setNodeProperty(erin, "tags", tags);
    std::cout << "  追加一个标签后：\n";
    printNode(db, erin);

    std::cout << "  谓词查询：tags 含 vip 的人：\n";
    printIdList(db.findNodes([](const Node& n) {
        auto it = n.props.find("tags");
        return it != n.props.end() && it->second.contains(Value("vip"));
    }));

    // ---------------------------------------------------------- 3. 改
    section("3. 修改（Update）");
    db.setNodeProperty(alice, "age", Value(31));
    db.updateNode(bob, {{"city", "Hangzhou"}, {"title", "Engineer"}});
    db.removeNodeProperty(carol, "city");
    db.addNodeLabel(dave, "Intern");
    db.setRelationshipProperty(1, "note", Value("met at conference"));
    std::cout << "  改完后：\n";
    printNode(db, alice);
    printNode(db, bob);
    printNode(db, carol);
    printNode(db, dave);
    printRel(db, 1);

    // ---------------------------------------------------------- 4. 遍历
    section("4. 图遍历与算法");
    std::cout << "  Alice 的出边邻居（KNOWS）：\n";
    printIdList(db.neighbors(alice, Direction::Out, {"KNOWS"}));
    std::cout << "  Alice 的出度（不限类型）= " << db.degree(alice, Direction::Out) << "\n";
    std::cout << "  从 Alice 出发 2 跳内可达：\n";
    printIdList(db.reachable(alice, 2, Direction::Out));

    std::cout << "  Alice -> Dave 最少跳数路径：\n";
    std::vector<uint64_t> path = db.shortestPath(alice, dave, Direction::Out);
    std::cout << "    ";
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) std::cout << " -> ";
        const Node* n = db.getNode(path[i]);
        std::cout << (n ? n->props.at("name").asString() : "?");
    }
    std::cout << "\n";

    std::cout << "  Alice -> Dave 带权最短路径（weight）：\n";
    WeightedPath wp = db.shortestWeightedPath(alice, dave, "weight", Direction::Out);
    if (wp.found) {
        std::cout << "    代价 = " << wp.cost << "，路径：";
        for (size_t i = 0; i < wp.nodes.size(); ++i) {
            if (i) std::cout << " -> ";
            const Node* n = db.getNode(wp.nodes[i]);
            std::cout << (n ? n->props.at("name").asString() : "?");
        }
        std::cout << "\n";
    } else {
        std::cout << "    不可达\n";
    }

    // ---------------------------------------------------------- 5. 持久化
    section("5. 持久化（二进制落盘 + 重新载入）");
    db.save();
    std::cout << "  已写入 " << dbPath << "\n";
    db.printStats(std::cout);

    GraphDB reopened(dbPath);
    reopened.open();
    std::cout << "  重新打开后节点数 = " << reopened.nodeCount()
              << "，关系数 = " << reopened.relationshipCount() << "\n";
    std::cout << "  索引是否随文件保留 (:Person.city)："
              << (reopened.hasIndex("Person", "city") ? "是" : "否") << "\n";
    std::cout << "  重新打开后查 :Company：\n";
    printIdList(reopened.findNodesByLabel("Company"));

    // ---------------------------------------------------------- 6. 删
    section("6. 删除（Delete）—— 级联删除相连关系");
    std::cout << "  删除 Carol 前：节点 " << db.nodeCount() << "，关系 " << db.relationshipCount() << "\n";
    db.removeNode(carol);
    std::cout << "  删除 Carol 后：节点 " << db.nodeCount() << "，关系 " << db.relationshipCount() << "\n";
    std::cout << "  Carol 是否还存在：" << (db.getNode(carol) ? "是" : "否") << "\n";
    db.removeRelationship(db.allRelationshipIds().front());
    std::cout << "  再删一条关系后关系数 = " << db.relationshipCount() << "\n";
    db.save();

    section("最终统计");
    db.printStats(std::cout);
    std::cout << "\n演示结束。数据已保存在 " << dbPath << "，可再次运行或用 --shell 查看。\n";
}

// ================================================================ 交互命令行
static void runShell() {
    std::cout << "MiniGraph 交互命令行（数据文件：shell.mgdb），输入 help 查看命令\n";
    GraphDB db("shell.mgdb");
    if (!db.open()) {
        std::cerr << "打开数据库失败\n";
        return;
    }

    std::string line;
    while (true) {
        std::cout << "\nminigraph> ";
        if (!std::getline(std::cin, line)) break;

        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        if (cmd.empty()) continue;

        std::vector<std::string> args;
        std::string a;
        while (is >> a) args.push_back(a);

        if (cmd == "quit" || cmd == "exit") {
            db.save();
            break;
        } else if (cmd == "help") {
            std::cout
                << "  node <Label>[:Label2] [k=v ...]        创建节点\n"
                << "  rel <from> <to> <TYPE> [k=v ...]       创建关系\n"
                << "  get <id>                               查看节点\n"
                << "  set <id> <k=v> [k=v ...]               修改节点属性\n"
                << "  rmnode <id>                            删除节点（级联删关系）\n"
                << "  rmrel <id>                             删除关系\n"
                << "  find <Label> [key=value]               查找节点\n"
                << "  rels <id> [out|in|both]                列出相连关系\n"
                << "  nbrs <id> [out|in|both] [TYPE]         列出邻居\n"
                << "  path <from> <to>                       最短路径（跳数）\n"
                << "  wpath <from> <to> [weightKey]          带权最短路径\n"
                << "  index <Label> <key>                    创建索引\n"
                << "  list [nodes|rels]                      列出全部\n"
                << "  stats                                  统计信息\n"
                << "  save                                   立即落盘\n"
                << "  quit                                   保存并退出\n";
        } else if (cmd == "node") {
            std::vector<std::string> labels;
            if (!args.empty()) {
                std::string raw = args[0];
                size_t start = 0;
                while (true) {
                    size_t p = raw.find(':', start);
                    if (p == std::string::npos) { labels.push_back(raw.substr(start)); break; }
                    labels.push_back(raw.substr(start, p - start));
                    start = p + 1;
                }
            }
            Props props;
            for (size_t i = 1; i < args.size(); ++i) props.insert(parseKeyValue(args[i]));
            std::cout << "  创建节点 #" << db.createNode(labels, props) << "\n";
        } else if (cmd == "rel" && args.size() >= 3) {
            Props props;
            for (size_t i = 3; i < args.size(); ++i) props.insert(parseKeyValue(args[i]));
            uint64_t id = db.createRelationship(std::stoull(args[0]), std::stoull(args[1]),
                                                args[2], props);
            std::cout << (id ? "  创建关系 #" + std::to_string(id) : std::string("  创建失败（端点不存在）")) << "\n";
        } else if (cmd == "get" && !args.empty()) {
            printNode(db, std::stoull(args[0]));
        } else if (cmd == "set" && args.size() >= 2) {
            Props props;
            for (size_t i = 1; i < args.size(); ++i) props.insert(parseKeyValue(args[i]));
            std::cout << (db.updateNode(std::stoull(args[0]), props) ? "  已更新\n" : "  节点不存在\n");
        } else if (cmd == "rmnode" && !args.empty()) {
            std::cout << (db.removeNode(std::stoull(args[0])) ? "  已删除\n" : "  节点不存在\n");
        } else if (cmd == "rmrel" && !args.empty()) {
            std::cout << (db.removeRelationship(std::stoull(args[0])) ? "  已删除\n" : "  关系不存在\n");
        } else if (cmd == "find" && !args.empty()) {
            if (args.size() == 1) {
                printIdList(db.findNodesByLabel(args[0]));
            } else {
                auto kv = parseKeyValue(args[1]);
                printIdList(db.findNodes(args[0], kv.first, kv.second));
            }
        } else if (cmd == "rels" && !args.empty()) {
            Direction d = Direction::Both;
            if (args.size() > 1) {
                if (args[1] == "out") d = Direction::Out;
                else if (args[1] == "in") d = Direction::In;
            }
            for (uint64_t rid : db.incidentRelationships(std::stoull(args[0]), d)) printRel(db, rid);
        } else if (cmd == "nbrs" && !args.empty()) {
            Direction d = Direction::Both;
            if (args.size() > 1) {
                if (args[1] == "out") d = Direction::Out;
                else if (args[1] == "in") d = Direction::In;
            }
            std::vector<std::string> types;
            if (args.size() > 2) types.push_back(args[2]);
            for (uint64_t nid : db.neighbors(std::stoull(args[0]), d, types)) printNode(db, nid);
        } else if (cmd == "path" && args.size() >= 2) {
            std::vector<uint64_t> p = db.shortestPath(std::stoull(args[0]), std::stoull(args[1]));
            if (p.empty()) std::cout << "  不可达\n";
            else {
                std::cout << "  ";
                for (size_t i = 0; i < p.size(); ++i) std::cout << (i ? " -> " : "") << "#" << p[i];
                std::cout << "\n";
            }
        } else if (cmd == "wpath" && args.size() >= 2) {
            std::string key = args.size() > 2 ? args[2] : "weight";
            WeightedPath wp = db.shortestWeightedPath(std::stoull(args[0]), std::stoull(args[1]), key);
            if (!wp.found) std::cout << "  不可达\n";
            else {
                std::cout << "  代价 " << wp.cost << "：";
                for (size_t i = 0; i < wp.nodes.size(); ++i)
                    std::cout << (i ? " -> " : "") << "#" << wp.nodes[i];
                std::cout << "\n";
            }
        } else if (cmd == "index" && args.size() >= 2) {
            db.createIndex(args[0], args[1]);
            std::cout << "  索引 (:" << args[0] << " . " << args[1] << ") 已建立\n";
        } else if (cmd == "list") {
            std::string what = args.empty() ? "nodes" : args[0];
            if (what == "rels") {
                for (uint64_t id : db.allRelationshipIds()) printRel(db, id);
            } else {
                for (uint64_t id : db.allNodeIds()) printNode(db, id);
            }
        } else if (cmd == "stats") {
            db.printStats(std::cout);
        } else if (cmd == "save") {
            std::cout << (db.save() ? "  已落盘\n" : "  落盘失败\n");
        } else {
            std::cout << "  未知命令或参数不足，输入 help 查看用法\n";
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--shell") {
        runShell();
    } else {
        runDemo();
    }
    return 0;
}
