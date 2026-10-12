#pragma once
// Text dump of a tree shared by kv_cli and kv_stress, so single- and multi-threaded runs can be
// compared byte for byte. One line per node: "path<TAB>type<TAB>value" (value empty for "kv").
#include <cstdlib>
#include <string>
#include <string_view>

#include "keyvalue.h"
#include "keyvalue_internal.h"

// Tools read the file with escape sequences on when the environment has KV_ESC=1 (the default is off).
inline bool KvLoad(CKeyValue& kv, const char* path) {
    const char* e = std::getenv("KV_ESC");
    if (e && e[0] == '1') kv.UsesEscapeSequences(true);
    return kv.LoadFromFile(path);
}

inline std::string KvEscape(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default: o += c;
        }
    }
    return o;
}

inline void KvDumpInto(const CKeyValue& kv, const std::string& prefix, std::string& out) {
    KVStringPool& pool = KVStringPool::Instance();
    for (const auto& n : KVInternal::Nodes(kv)) {
        if (KVInternal::Skipped(kv, n)) continue;
        const std::string path = prefix + "/" + std::string(pool.View(n.nameSlot));
        out += KvEscape(path);
        switch (n.type) {
            case CKeyValue::KeyValue:
                out += "\tkv\t\n";
                KvDumpInto(*n.child, path, out);
                break;
            case CKeyValue::String:
            case CKeyValue::Int:
            case CKeyValue::Float:
                out += n.type == CKeyValue::String ? "\tstring\t" : n.type == CKeyValue::Int ? "\tint\t" : "\tfloat\t";
                out += KvEscape(pool.View(n.valueSlot));
                out += '\n';
                break;
        }
    }
}

inline std::string KvDump(const CKeyValue& kv) {
    std::string out;
    KvDumpInto(kv, kv.GetName(), out);
    return out;
}
