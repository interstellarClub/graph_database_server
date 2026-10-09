#include "minigraph/Storage.h"

#include <cstdio>
#include <fstream>

namespace minigraph {
namespace storage {

namespace {

void writeNode(BinaryWriter& w, const Node& n) {
    w.writeU64(n.id);
    w.writeU32(static_cast<uint32_t>(n.labels.size()));
    for (const auto& l : n.labels) w.writeString(l);
    w.writeProps(n.props);
}

// 是否写 weight 由调用方决定（写 v3 才写）
void writeRel(BinaryWriter& w, const Relationship& r, bool withWeight) {
    w.writeU64(r.id);
    w.writeU64(r.from);
    w.writeU64(r.to);
    w.writeString(r.type);
    w.writeProps(r.props);
    if (withWeight) w.writeDouble(r.weight);
}

Node readNode(BinaryReader& r) {
    Node n;
    n.id = r.readU64();
    uint32_t labelCount = r.readU32();
    n.labels.reserve(labelCount);
    for (uint32_t i = 0; i < labelCount; ++i) n.labels.push_back(r.readString());
    n.props = r.readProps();
    return n;
}

Relationship readRel(BinaryReader& r, uint32_t version) {
    Relationship rel;
    rel.id = r.readU64();
    rel.from = r.readU64();
    rel.to = r.readU64();
    rel.type = r.readString();
    rel.props = r.readProps();
    rel.weight = Relationship::kDefaultWeight;

    if (version >= kVersionWeight) {
        rel.weight = r.readDouble();
        return rel;
    }

    // ---- v1/v2 兼容：把 props["weight"] 迁移为一等 weight 字段 ----
    auto it = rel.props.find(kLegacyWeightKey);
    if (it != rel.props.end()) {
        const Value& v = it->second;
        if (v.isInt() || v.isDouble()) {
            rel.weight = v.asDouble();
            rel.props.erase(it);   // 同一份权重不存两处，避免后续更新时不一致
        } else if (v.isString()) {
            // 旧代码允许把 "3.5" 写成字符串，尽力还原；还原不了就保留默认 1.0
            try {
                rel.weight = std::stod(v.asString());
                rel.props.erase(it);
            } catch (const std::exception&) {
                // 留着原属性，交给调用方决定
            }
        }
    }
    return rel;
}

// 快照里是否含数组/对象。嵌套容器只可能出现在容器内部，
// 因此只要看顶层值是不是容器即可。
bool propsHasContainer(const Props& props) {
    for (const auto& kv : props) {
        if (kv.second.isContainer()) return true;
    }
    return false;
}

// 快照里是否存在"非默认边权"
bool hasCustomWeight(const Snapshot& snap) {
    for (const auto& r : snap.rels) {
        if (r.weight != Relationship::kDefaultWeight) return true;
    }
    return false;
}

// 按需选版本：
//   有容器或有非默认边权 -> v3（容器本身 v2 就够，但统一写最高版更省事且自解释）
//   全都是标量且边权都是 1.0 -> v1，旧版程序也能打开这份文件
uint32_t pickVersion(const Snapshot& snap) {
    for (const auto& n : snap.nodes) {
        if (propsHasContainer(n.props)) return kVersion;
    }
    for (const auto& r : snap.rels) {
        if (propsHasContainer(r.props)) return kVersion;
    }
    if (hasCustomWeight(snap)) return kVersion;
    return kMinReadVersion;
}

} // namespace

bool saveSnapshot(const std::string& path, const Snapshot& snap) {
    const uint32_t version = pickVersion(snap);
    const bool withWeight = (version >= kVersionWeight);

    BinaryWriter w;
    w.writeMagic();
    w.writeU32(version);
    w.writeU64(snap.nodes.size());
    w.writeU64(snap.rels.size());
    w.writeU64(snap.indexes.size());
    w.writeU64(snap.nextNodeId);
    w.writeU64(snap.nextRelId);

    for (const auto& n : snap.nodes) writeNode(w, n);
    for (const auto& r : snap.rels) writeRel(w, r, withWeight);
    for (const auto& ix : snap.indexes) {
        w.writeString(ix.first);
        w.writeString(ix.second);
    }
    w.writeMagic();  // 尾部标识：读到它说明文件完整

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(w.data().data()),
                  static_cast<std::streamsize>(w.data().size()));
        out.flush();
        if (!out) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    // 原子替换，避免写入中途崩溃留下损坏文件
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool loadSnapshot(const std::string& path, Snapshot& snap) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (buf.empty()) return false;

    BinaryReader r(std::move(buf));
    r.expectMagic();
    uint32_t version = r.readU32();
    if (version < kMinReadVersion || version > kVersion) {
        throw std::runtime_error("数据文件版本不兼容：文件版本 " + std::to_string(version) +
                                 "，当前程序支持 " + std::to_string(kMinReadVersion) + " ~ " +
                                 std::to_string(kVersion));
    }

    uint64_t nodeCount = r.readU64();
    uint64_t relCount = r.readU64();
    uint64_t indexCount = r.readU64();
    snap.nextNodeId = r.readU64();
    snap.nextRelId = r.readU64();

    snap.nodes.clear();
    snap.nodes.reserve(nodeCount);
    for (uint64_t i = 0; i < nodeCount; ++i) snap.nodes.push_back(readNode(r));

    snap.rels.clear();
    snap.rels.reserve(relCount);
    for (uint64_t i = 0; i < relCount; ++i) snap.rels.push_back(readRel(r, version));

    snap.indexes.clear();
    for (uint64_t i = 0; i < indexCount; ++i) {
        std::string label = r.readString();
        std::string key = r.readString();
        snap.indexes.emplace_back(label, key);
    }

    r.expectMagic();  // 尾部校验
    return true;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

uint64_t fileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    return static_cast<uint64_t>(f.tellg());
}

} // namespace storage
} // namespace minigraph
