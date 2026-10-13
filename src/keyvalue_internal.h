#pragma once
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "keyvalue.h"
#include "kv_string_pool.h"

// Assisted and commented using Claude. (sry too lazy to do them myself).

struct KVInternal
{
    static uint32_t NameSlot(const CKeyValue& kv) { return kv.nameSlot_; }
    static void SetNameSlot(CKeyValue& kv, uint32_t slot) { kv.nameSlot_ = slot; }

    // `iv` is stored for Int, `fv` for Float; the other one is ignored.
    static void StoreNumber(CKeyValue::Node& n, CKeyValue::Type type, int iv, float fv)
    {
        n.valueSlot = 0;

        if (type == CKeyValue::Int)
            n.intValue = iv;
        else if (type == CKeyValue::Float)
            n.floatValue = fv;
        else
            n.intValue = 0;
    }

    static void SetValue(CKeyValue& kv, uint32_t name, CKeyValue::Type type, uint32_t value, int iv = 0, float fv = 0.f)
    {
        CKeyValue::Node& n = kv.Upsert(name);
        n.type = type;
        StoreNumber(n, type, iv, fv);
        n.valueSlot = value;
    }

    static void SetChild(CKeyValue& kv, uint32_t name, std::unique_ptr<CKeyValue> child)
    {
        child->nameSlot_ = name;
        CKeyValue::Node& n = kv.Upsert(name);
        n.type = CKeyValue::KeyValue;
        n.child = child.release();
    }

    static void AddValue(CKeyValue& kv, uint32_t name, CKeyValue::Type type, uint32_t value, bool hidden = false, int iv = 0,
        float fv = 0.f)
    {
        CKeyValue::Node& n = kv.Append(name);
        n.type = type;
        StoreNumber(n, type, iv, fv);
        n.valueSlot = value;

        if (hidden)
        {
            n.hidden = true;
            kv.anyHidden_ = true;
        }
    }

    static void AddChild(CKeyValue& kv, uint32_t name, std::unique_ptr<CKeyValue> child, bool hidden = false)
    {
        child->nameSlot_ = name;
        CKeyValue::Node& n = kv.Append(name);
        n.type = CKeyValue::KeyValue;
        n.child = child.release();

        if (hidden)
        {
            n.hidden = true;
            kv.anyHidden_ = true;
        }
    }

    static void SetUseCond(CKeyValue& kv, bool state) { kv.useCond_ = state; }
    static void SetUseEsc(CKeyValue& kv, bool state) { kv.useEsc_ = state; }
    static bool Skipped(const CKeyValue& kv, const CKeyValue::Node& n) { return kv.Skipped(n); }
    static void Reserve(CKeyValue& kv, size_t n) { kv.nodes_.reserve(n); }
    static void ShrinkToFit(CKeyValue& kv)
    {
        kv.nodes_.shrink_to_fit();

        if (kv.extra_)
        {
            kv.extra_->next.shrink_to_fit();
            kv.extra_->tail.shrink_to_fit();
        }
    }

    static const std::vector<CKeyValue::Node>& Nodes(const CKeyValue& kv) { return kv.nodes_; }
};

// Names are stored in lower case (ASCII only).
namespace kvname
{
inline bool HasUpper(std::string_view s)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());

    for (size_t i = 0; i < s.size(); ++i)
        if (static_cast<unsigned>(p[i] - 'A') < 26u)
            return true;

    return false;
}

#if defined(__GNUC__) || defined(__clang__)
#define KV_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define KV_NOINLINE __declspec(noinline)
#else
#define KV_NOINLINE
#endif

template <bool kAdd>
KV_NOINLINE inline uint32_t FoldedLookup(std::string_view name)
{
    char buf[256];
    std::string big;
    char* p = buf;

    if (name.size() > sizeof buf)
    {
        big.resize(name.size());
        p = &big[0];
    }

    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        p[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    const std::string_view v(p, name.size());

    return kAdd ? KVStringPool::Instance().Intern(v) : KVStringPool::Instance().Find(v);
}

inline uint32_t Intern(std::string_view name)
{
    return HasUpper(name) ? FoldedLookup<true>(name) : KVStringPool::Instance().Intern(name);
}

inline uint32_t Find(std::string_view name)
{
    return HasUpper(name) ? FoldedLookup<false>(name) : KVStringPool::Instance().Find(name);
}
}  // namespace kvname

namespace kvnum
{
// from_chars rejects a leading '+'; one is allowed, "+-5" and "++5" are not.
inline bool StripPlus(std::string_view& s)
{
    if (!s.empty() && s[0] == '+')
    {
        s.remove_prefix(1);

        if (s.empty() || s[0] == '-' || s[0] == '+')
            return false;
    }

    return true;
}

inline bool ParseInt(std::string_view s, int& out)
{
    if (!StripPlus(s) || s.empty())
        return false;

    const char* end = s.data() + s.size();
    auto r = std::from_chars(s.data(), end, out);

    return r.ec == std::errc() && r.ptr == end;
}

template <class T>
inline bool ParseFloat(std::string_view s, T& out)
{
    if (!StripPlus(s) || s.empty())
        return false;

    size_t i = (s[0] == '-') ? 1 : 0;

    if (i >= s.size() || !((s[i] >= '0' && s[i] <= '9') || s[i] == '.'))
        return false;
#ifdef __cpp_lib_to_chars
    const char* end = s.data() + s.size();
    auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc() && r.ptr == end;
#else
    std::string tmp(s);
    char* endp = nullptr;
    double d = std::strtod(tmp.c_str(), &endp);

    if (endp != tmp.c_str() + tmp.size())
        return false;

    out = static_cast<T>(d);
    return true;
#endif
}

// The type a value text has, as decided when a file is read (and by SetString/AddString): Int if the
// whole text is an integer, Float if it is a decimal number, else String. `iv` / `fv` get the number.
inline CKeyValue::Type Classify(std::string_view text, int& iv, float& fv)
{
    iv = 0;
    fv = 0.f;
    const char c0 = text.empty() ? 0 : text[0];

    if ((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.')
    {
        if (ParseInt(text, iv))
            return CKeyValue::Int;

        if (ParseFloat(text, fv))
            return CKeyValue::Float;
    }

    return CKeyValue::String;
}

inline std::string FormatInt(int v)
{
    char buf[16];
    auto r = std::to_chars(buf, buf + sizeof buf, v);

    return std::string(buf, r.ptr);
}

// The text always has a '.' or an exponent, so that it reads back as a Float and not as an Int (2.0f -> "2.0").
// NaN and infinity have no number text and are written as "nan" / "inf".
inline std::string FormatFloat(float v)
{
    char buf[64];
#ifdef __cpp_lib_to_chars
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, r.ptr);
#else
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
    std::string s(buf);
#endif

    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";

    return s;
}
}  // namespace kvnum
