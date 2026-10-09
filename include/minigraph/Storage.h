// -----------------------------------------------------------------------------
// MiniGraph —— 二进制持久化层
//
// 文件格式（全部小端、无内存对齐填充，可跨机器拷贝）：
//
//   +---------------------------------------------------------------+
//   | Header   magic(8B) | version(u32) | nodeCount(u64)            |
//   |          relCount(u64) | indexCount(u64)                      |
//   |          nextNodeId(u64) | nextRelId(u64)                     |
//   +---------------------------------------------------------------+
//   | Nodes    nodeCount 条 Node 记录                                |
//   |          id(u64) | labelCount(u32) | labels...                |
//   |          propCount(u32) | (key, value)...                     |
//   +---------------------------------------------------------------+
//   | Rels     relCount 条 Relationship 记录                         |
//   |          id(u64) | from(u64) | to(u64) | type(str) | props...  |
//   |          【v3 起】末尾追加 weight(double)                        |
//   +---------------------------------------------------------------+
//   | Indexes  indexCount 条 (label(str), key(str))                  |
//   +---------------------------------------------------------------+
//   | Footer   magic(8B)   —— 用于校验文件是否完整写入                  |
//   +---------------------------------------------------------------+
//
//   Value 的编码：type(u8) + 载荷
//     0 null(无载荷) | 1 int(i64) | 2 double(u64 位模式) | 3 bool(u8) | 4 string(u32长度+字节)
//     5 array(u32 元素数 + 元素…) | 6 object(u32 键数 + (key, value)…)   【v2 起】
//
// 版本策略：
//   v1 —— 只有标量；v2 —— 增加数组/对象（可任意嵌套）；v3 —— 关系增加边权 weight。
//   读取三种版本都支持；写入时若数据里既没有容器、也没有非默认边权，仍然写 v1，
//   这样旧版程序也能打开新库。
//
//   v1/v2 文件里没有 weight，载入时按"从 props[weight] 迁移"的规则还原边权：
//   若该关系的 props 里有 "weight" 且是数字，就用它作为一等 weight（并从 props 中移除，
//   避免同一份权重存两处），否则取默认值 1.0。
//
// 落盘策略：先写 path + ".tmp"，写完再 rename 覆盖 —— 保证不会出现半截文件。
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "minigraph/Types.h"

namespace minigraph {
namespace storage {

// 文件标识与版本
inline constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'G', 'D', 'B', '1'};
inline constexpr uint32_t kVersion = 3;          // 写出的最高版本（含数组/对象/边权）
inline constexpr uint32_t kMinReadVersion = 1;   // 能读入的最低版本
inline constexpr uint32_t kVersionContainer = 2; // 从该版本起支持容器类型
inline constexpr uint32_t kVersionWeight = 3;    // 从该版本起关系带 weight 字段
inline constexpr int kMaxValueDepth = 64;        // 容器嵌套层数上限（防御性）

// 旧文件中的边权键名：v1/v2 文件载入时从这里迁移到一等 weight 字段
inline constexpr char kLegacyWeightKey[] = "weight";

// 落盘/载入时的一次性数据镜像
struct Snapshot {
    std::vector<Node> nodes;
    std::vector<Relationship> rels;
    std::vector<std::pair<std::string, std::string>> indexes;  // (label, property key)
    uint64_t nextNodeId = 1;
    uint64_t nextRelId = 1;
};

// ---------------- 写入器 ----------------
class BinaryWriter {
public:
    void writeMagic() { writeBytes(kMagic, sizeof(kMagic)); }
    void writeU8(uint8_t v) { buffer_.push_back(v); }
    void writeU32(uint32_t v) { writeLE(v); }
    void writeU64(uint64_t v) { writeLE(v); }
    void writeI64(int64_t v) { writeLE(static_cast<uint64_t>(v)); }
    void writeBool(bool v) { writeU8(v ? 1 : 0); }
    void writeDouble(double v) {
        uint64_t bits = 0;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
        for (size_t i = 0; i < sizeof(double); ++i) {
            bits |= static_cast<uint64_t>(p[i]) << (i * 8);
        }
        writeU64(bits);
    }
    void writeString(const std::string& s) {
        writeU32(static_cast<uint32_t>(s.size()));
        writeBytes(s.data(), s.size());
    }
    void writeValue(const Value& v, int depth = 0) {
        writeU8(static_cast<uint8_t>(v.type()));
        switch (v.type()) {
            case Value::Type::Null:   break;
            case Value::Type::Int:    writeI64(v.asInt()); break;
            case Value::Type::Double: writeDouble(v.asDouble()); break;
            case Value::Type::Bool:   writeBool(v.asBool()); break;
            case Value::Type::String: writeString(v.asString()); break;
            case Value::Type::Array: {
                const ValueList& a = v.asArray();
                writeU32(static_cast<uint32_t>(a.size()));
                for (const Value& item : a) writeValue(item, depth + 1);
                break;
            }
            case Value::Type::Object: {
                const ValueMap& m = v.asObject();
                writeU32(static_cast<uint32_t>(m.size()));
                for (const auto& kv : m) {
                    writeString(kv.first);
                    writeValue(kv.second, depth + 1);
                }
                break;
            }
        }
    }
    void writeProps(const Props& props) {
        writeU32(static_cast<uint32_t>(props.size()));
        for (const auto& kv : props) {
            writeString(kv.first);
            writeValue(kv.second);
        }
    }
    const std::vector<uint8_t>& data() const { return buffer_; }

private:
    void writeBytes(const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        buffer_.insert(buffer_.end(), b, b + n);
    }
    void writeLE(uint64_t v) {
        for (size_t i = 0; i < 8; ++i) buffer_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
    void writeLE(uint32_t v) {
        for (size_t i = 0; i < 4; ++i) buffer_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
    std::vector<uint8_t> buffer_;
};

// ---------------- 读取器 ----------------
class BinaryReader {
public:
    explicit BinaryReader(std::vector<uint8_t> buf) : buffer_(std::move(buf)) {}

    void expectMagic() {
        if (remaining() < sizeof(kMagic)) throw std::runtime_error("文件过短，不是合法的 MiniGraph 数据文件");
        for (size_t i = 0; i < sizeof(kMagic); ++i) {
            if (buffer_[pos_ + i] != static_cast<uint8_t>(kMagic[i])) {
                throw std::runtime_error("文件标识不匹配，不是 MiniGraph 数据文件");
            }
        }
        pos_ += sizeof(kMagic);
    }
    uint8_t readU8() {
        need(1);
        return buffer_[pos_++];
    }
    uint32_t readU32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(buffer_[pos_++]) << (i * 8);
        return v;
    }
    uint64_t readU64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(buffer_[pos_++]) << (i * 8);
        return v;
    }
    int64_t readI64() { return static_cast<int64_t>(readU64()); }
    bool readBool() { return readU8() != 0; }
    double readDouble() {
        uint64_t bits = readU64();
        double v = 0.0;
        unsigned char* p = reinterpret_cast<unsigned char*>(&v);
        for (size_t i = 0; i < sizeof(double); ++i) p[i] = static_cast<unsigned char>((bits >> (i * 8)) & 0xFF);
        return v;
    }
    std::string readString() {
        uint32_t len = readU32();
        need(len);
        std::string s(reinterpret_cast<const char*>(buffer_.data() + pos_), len);
        pos_ += len;
        return s;
    }
    Value readValue(int depth = 0) {
        if (depth > kMaxValueDepth) throw std::runtime_error("数据文件已损坏：属性嵌套层数过深");
        uint8_t t = readU8();
        switch (t) {
            case 0: return Value();
            case 1: return Value(readI64());
            case 2: return Value(readDouble());
            case 3: return Value(readBool());
            case 4: return Value(readString());
            case 5: {  // array
                uint32_t n = readU32();
                ValueList a;
                a.reserve(n);
                for (uint32_t i = 0; i < n; ++i) a.push_back(readValue(depth + 1));
                return Value(std::move(a));
            }
            case 6: {  // object
                uint32_t n = readU32();
                ValueMap m;
                for (uint32_t i = 0; i < n; ++i) {
                    std::string key = readString();
                    m[key] = readValue(depth + 1);
                }
                return Value(std::move(m));
            }
            default: throw std::runtime_error("未知的属性类型标记");
        }
    }
    Props readProps() {
        uint32_t n = readU32();
        Props props;
        for (uint32_t i = 0; i < n; ++i) {
            std::string key = readString();
            props[key] = readValue();
        }
        return props;
    }

    size_t remaining() const { return buffer_.size() - pos_; }
    size_t pos() const { return pos_; }
    const std::vector<uint8_t>& buffer() const { return buffer_; }

private:
    void need(size_t n) {
        if (remaining() < n) throw std::runtime_error("数据文件已损坏：读取越界");
    }
    std::vector<uint8_t> buffer_;
    size_t pos_ = 0;
};

// ---------------- 对外接口 ----------------

// 将快照写入二进制文件（原子替换）
bool saveSnapshot(const std::string& path, const Snapshot& snap);

// 从二进制文件读取快照，失败抛 std::runtime_error
bool loadSnapshot(const std::string& path, Snapshot& snap);

// 文件是否存在
bool fileExists(const std::string& path);

// 文件大小（字节），不存在返回 0
uint64_t fileSize(const std::string& path);

} // namespace storage
} // namespace minigraph
