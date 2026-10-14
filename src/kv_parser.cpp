#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "keyvalue.h"
#include "keyvalue_internal.h"
#include "kv_log.h"

namespace
{
constexpr int kMaxDepth = 512;
constexpr size_t kInitialWindow = 256 * 1024;

struct CharTable
{
    unsigned char t[256];
    constexpr CharTable() : t()
    {
        t[' '] = t['\t'] = t['\r'] = t['\n'] = t['\f'] = t['\v'] = 1;
        t['{'] = t['}'] = t['"'] = 2;
        t['/'] = 3;  // ends a bare word only as the start of a comment
    }
};
constexpr CharTable kChars;

// Conditions: a "[...]" token after a key, value or root name decides whether the entry exists
// ($NAME, !, &&, ||, parentheses; names extensible with SetCondition).
using CondTable = std::vector<std::pair<std::string, bool>>;

std::mutex g_condMutex;

std::string UpperName(std::string_view n)
{
    if (!n.empty() && n[0] == '$')
        n.remove_prefix(1);

    std::string r(n);

    for (char& c : r)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return r;
}

CondTable& CondDefaults()
{
    static CondTable t = []
    {
        CondTable d;
#if defined(_WIN32)
        d = {{"WINDOWS", true}, {"WIN32", true}};
#elif defined(__APPLE__)
        d = {{"OSX", true}, {"POSIX", true}};
#else
        d = {{"LINUX", true}, {"POSIX", true}};
#endif
        return d;
    }();

    return t;
}

CondTable SnapshotConditions()
{
    std::lock_guard<std::mutex> lock(g_condMutex);

    return CondDefaults();
}

// Malformed or unknown is false.
class CondEval
{
public:
    CondEval(std::string_view text, const CondTable& t) : t_(t)
    {
        if (text.size() >= 2)
            s_ = text.substr(1, text.size() - 2);
        else
            bad_ = true;
    }

    bool Run()
    {
        const bool v = Or();
        Ws();
        return !bad_ && i_ == s_.size() && v;
    }

private:
    void Ws()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }

    bool Or()
    {
        bool v = And();

        for (Ws(); !bad_ && s_.compare(i_, 2, "||") == 0; Ws())
        {
            i_ += 2;
            const bool r = And();
            v = v || r;
        }

        return v;
    }

    bool And()
    {
        bool v = Not();

        for (Ws(); !bad_ && s_.compare(i_, 2, "&&") == 0; Ws())
        {
            i_ += 2;
            const bool r = Not();
            v = v && r;
        }

        return v;
    }

    bool Not()
    {
        Ws();

        if (i_ >= s_.size())
        {
            bad_ = true;
            return false;
        }

        if (s_[i_] == '!')
        {
            ++i_;
            return !Not();
        }

        if (s_[i_] == '(')
        {
            ++i_;
            const bool v = Or();
            Ws();

            if (i_ >= s_.size() || s_[i_] != ')')
            {
                bad_ = true;
                return false;
            }

            ++i_;
            return v;
        }

        if (s_[i_] != '$')
        {
            bad_ = true;
            return false;
        }

        const size_t b = ++i_;

        while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_'))
            ++i_;

        if (i_ == b)
        {
            bad_ = true;
            return false;
        }

        const std::string name = UpperName(s_.substr(b, i_ - b));

        for (const auto& e : t_)
            if (e.first == name)
                return e.second;

        return false;
    }

    const CondTable& t_;
    std::string_view s_;
    size_t i_ = 0;
    bool bad_ = false;
};

// Count of '\n' in [p, p+n); `last` gets the index of the final one (n if none).
size_t CountNewlines(const char* p, size_t n, size_t& last)
{
    constexpr uint64_t kLow = 0x0101010101010101ull, kHigh = 0x8080808080808080ull;
    size_t count = 0, i = 0;

    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        w ^= kLow * '\n';  // a '\n' byte becomes zero
        // exact zero-byte mask: the high bit of every byte that is zero
        const uint64_t z = ~(((w & ~kHigh) + ~kHigh) | w | ~kHigh);
        count += static_cast<size_t>(((z >> 7) * kLow) >> 56);
    }

    for (; i < n; ++i)
        count += p[i] == '\n';

    last = n;

    for (size_t j = n; j-- > 0;)
        if (p[j] == '\n')
        {
            last = j;
            break;
        }

    return count;
}

// Direct-mapped cache of short strings in front of the pool (slots never change text, so no
// invalidation; a hit takes no lock).
class InternCache
{
public:
    static constexpr size_t kMaxLen = 20;  // the entry is 32 bytes

    uint32_t Intern(KVStringPool& pool, std::string_view s)
    {
        const uint32_t h = KVHashString(s);

        if (s.size() > kMaxLen)
            return pool.Intern(s, h);

        Entry& e = e_[h & (kEntries - 1)];

        if (e.hash == h && e.len == s.size() && std::memcmp(e.text, s.data(), s.size()) == 0)
            return e.slot;

        const uint32_t slot = pool.Intern(s, h);
        e.hash = h;
        e.slot = slot;
        e.len = static_cast<uint32_t>(s.size());
        std::memcpy(e.text, s.data(), s.size());

        return slot;
    }

private:
    static constexpr size_t kEntries = 1024;
    struct Entry
    {
        uint32_t hash = 0;
        uint32_t slot = kInvalidSlot;
        uint32_t len = 0xFFFFFFFFu;  // never matches
        char text[kMaxLen];
    };
    Entry e_[kEntries];
};

// Streaming parser over a sliding window; a token cut by the window end is rescanned after Refill.
class Parser
{
public:
    Parser(FILE* f, bool useCond, bool useEsc)
        : f_(f), buf_(kInitialWindow), conds_(SnapshotConditions()), useCond_(useCond), useEsc_(useEsc) {}

    // children still staged after an exception are owned here
    ~Parser()
    {
        for (const KVInternal::Node& n : stage_)
            if (n.type == CKeyValue::KeyValue)
                delete n.child;
    }

    bool Parse(CKeyValue& root, const char* path)
    {
        Fill();

        if (len_ >= 3 && std::memcmp(buf_.data(), "\xEF\xBB\xBF", 3) == 0)
        {
            KV_LOG("UTF-8 BOM skipped\n");
            pos_ = 3;
            lastNewlineAbs_ = 2;
        }

        // Named: find the root of that name; unnamed: take the first root and its name. A first root
        // without a name (a file starting with '{') is taken by an unnamed object and named `path`.
        const bool wanted = *root.GetName() != '\0';
        const uint32_t wantedSlot = KVInternal::NameSlot(root);
        bool found = false;
        bool first = true;

        for (;;)
        {
            Tok t = Next();

            if (t == Tok::Error)
                return false;

            if (t == Tok::End)
            {
                if (cutAbs_ >= 0)
                    return Fail(tokPos_, "");

                if (first)
                    return Fail(tokPos_, "empty file");

                if (!found)
                    return Fail(tokPos_, (std::string("root \"") + root.GetName() + "\" not found").c_str());

                return true;
            }

            const bool anonymous = first && !wanted && t == Tok::LBrace;
            first = false;

            if (!anonymous && t != Tok::Str && t != Tok::Bare)
                return Fail(tokPos_, "expected root name");

            const bool take = anonymous || (!found && (!wanted || kvname::Find(text_) == wantedSlot));
            bool accepted = true;

            if (anonymous)
            {
                KV_LOG("root without a name: named after the path '%s'\n", path);
                KVInternal::SetNameSlot(root, kvname::Intern(path));
            }
            else
            {
                if (take)
                    KVInternal::SetNameSlot(root, kvname::Intern(text_));

                t = Next();

                if (t == Tok::Cond)
                {
                    accepted = Eval();
                    t = Next();
                }
            }

            KV_LOG("root '%.*s': %s\n", static_cast<int>(text_.size()), text_.data(), take ? "loading" : "skipped (only braces are matched)");
            const uint32_t rootName = KVInternal::NameSlot(root);

            if (t == Tok::Error)
                return false;

            if (t != Tok::LBrace)
                return Fail(tokPos_, "expected '{' after root name");

            if (!take)
            {
                if (!SkipBody())
                    return false;

                continue;
            }

            if (!ParseBody(root, 0))
                return false;

            // the root is switched off: empty object that keeps its name
            if (!accepted && useCond_)
            {
                CKeyValue empty;
                KVInternal::SetNameSlot(empty, rootName);
                root = std::move(empty);
            }

            found = true;
        }
    }

    // UTF-8 bytes read so far (after conversion)
    size_t BytesRead() const { return base_ + len_; }

    void Report(const char* path) const
    {
        if (ioError_)
        {
            std::fprintf(stderr, "%s: read error\n", path);
            return;
        }

        const size_t local = errPos_ - base_;
        size_t line = 1 + discardedNewlines_;
        long long lastNl = lastNewlineAbs_;

        for (size_t i = 0; i < local && i < len_; ++i)
        {
            if (buf_[i] == '\n')
            {
                ++line;
                lastNl = static_cast<long long>(base_ + i);
            }
        }

        std::fprintf(stderr, "%s:%zu:%lld: %s\n", path, line, static_cast<long long>(errPos_) - lastNl, err_.c_str());
    }

private:
    enum class Tok
    {
        End,
        LBrace,
        RBrace,
        Str,
        Bare,
        Cond,
        Error
    };

    bool Fail(size_t localPos, const char* msg, bool ranOut = false)
    {
        if (err_.empty())
        {
            // the data was cut short: any failure at the cut is the cut's
            if (cutAbs_ >= 0 && eof_ && (ranOut || pos_ >= len_))
            {
                err_ = cutMsg_;
                errPos_ = static_cast<size_t>(cutAbs_);
            }
            else
            {
                err_ = msg;
                errPos_ = base_ + localPos;
            }
        }

        return false;
    }

    // Reads the next block of UTF-8 text into the window. A file that starts with a UTF-16 BOM is
    // converted on the way; bad UTF-16 ends the data right there and is reported as the error.
    void Fill()
    {
        if (eof_)
            return;

        if (len_ == buf_.size() || (enc_ != Enc::Utf8 && buf_.size() - len_ < 4096))
            buf_.resize(buf_.size() * 2);

        char* at = buf_.data() + len_;
        const size_t cap = buf_.size() - len_;
        size_t got;

        if (!started_)
        {
            started_ = true;
            got = std::fread(at, 1, cap, f_);

            //(NOTE): On Windows writing a file with raw wchar_t wofstream wont mark a file with BOM. GOOD LUCK FOR THEM HAHA!
            if (got >= 2 && static_cast<unsigned char>(at[0]) == 0xFF && static_cast<unsigned char>(at[1]) == 0xFE)
                enc_ = Enc::Utf16LE;
            else if (got >= 2 && static_cast<unsigned char>(at[0]) == 0xFE && static_cast<unsigned char>(at[1]) == 0xFF)
                enc_ = Enc::Utf16BE;

            if (enc_ != Enc::Utf8)
            {
                KV_LOG("UTF-16 %s BOM: converting to UTF-8 while reading\n", enc_ == Enc::Utf16LE ? "LE" : "BE");
                raw_.assign(reinterpret_cast<unsigned char*>(at) + 2, reinterpret_cast<unsigned char*>(at) + got);
                rawLen_ = got - 2;
                rawPos_ = 0;
                got = Decode(at, cap);
            }
        }
        else if (enc_ == Enc::Utf8)
            got = std::fread(at, 1, cap, f_);
        else
            got = Decode(at, cap);

        len_ += got;

        if (cutAbs_ >= 0)
        {
            eof_ = true;
            return;
        }

        if (got == 0)
        {
            eof_ = true;

            if (std::ferror(f_))
                ioError_ = true;
        }
    }

    // The data ends `out` bytes into the free space of the window, for the reason `msg`
    void Cut(size_t out, const char* msg)
    {
        if (cutAbs_ < 0)
        {
            cutAbs_ = static_cast<long long>(base_ + len_ + out);
            cutMsg_ = msg;
        }
    }

    // UTF-16 in raw_ -> UTF-8 at `at` (room for `cap` bytes, at least 4). Returns the bytes written;
    // 0 means the end of the file or a cut.
    size_t Decode(char* at, size_t cap)
    {
        constexpr size_t kRawChunk = 64 * 1024;
        const bool le = enc_ == Enc::Utf16LE;
        size_t out = 0;

        for (;;)
        {
            size_t avail = rawLen_ - rawPos_;
            const unsigned char* p = raw_.data() + rawPos_;

            while (avail >= 2 && cap - out >= 4)
            {
                uint32_t u = le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]);
                size_t used = 2;

                if (u - 0xD800u < 0x400u)
                {
                    if (avail < 4)
                        break;

                    const uint32_t v = le ? (p[2] | (p[3] << 8)) : ((p[2] << 8) | p[3]);

                    if (v - 0xDC00u >= 0x400u)
                    {
                        Cut(out, "invalid UTF-16: high surrogate without a low one");
                        return out;
                    }

                    u = 0x10000u + ((u - 0xD800u) << 10) + (v - 0xDC00u);
                    used = 4;
                }
                else if (u - 0xDC00u < 0x400u)
                {
                    Cut(out, "invalid UTF-16: low surrogate without a high one");
                    return out;
                }

                char* o = at + out;

                if (u < 0x80)
                    *o++ = static_cast<char>(u);
                else if (u < 0x800)
                {
                    *o++ = static_cast<char>(0xC0 | (u >> 6));
                    *o++ = static_cast<char>(0x80 | (u & 0x3F));
                }
                else if (u < 0x10000)
                {
                    *o++ = static_cast<char>(0xE0 | (u >> 12));
                    *o++ = static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                    *o++ = static_cast<char>(0x80 | (u & 0x3F));
                }
                else
                {
                    *o++ = static_cast<char>(0xF0 | (u >> 18));
                    *o++ = static_cast<char>(0x80 | ((u >> 12) & 0x3F));
                    *o++ = static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                    *o++ = static_cast<char>(0x80 | (u & 0x3F));
                }

                out = static_cast<size_t>(o - at);
                p += used;
                avail -= used;
                rawPos_ += used;
            }

            if (out > 0 || cap - out < 4)
                return out;

            // Out of raw bytes (or only the start of a character is left): read more
            if (avail > 0)
                std::memmove(raw_.data(), raw_.data() + rawPos_, avail);

            raw_.resize(avail + kRawChunk);
            const size_t n = std::fread(raw_.data() + avail, 1, kRawChunk, f_);
            rawLen_ = avail + n;
            rawPos_ = 0;

            if (n == 0)
            {
                if (avail > 0 && !std::ferror(f_))
                    Cut(0, "invalid UTF-16: the file ends inside a character");

                return 0;
            }
        }
    }

    void Refill(size_t keep)
    {
        size_t lastNl;
        discardedNewlines_ += CountNewlines(buf_.data(), keep, lastNl);

        if (lastNl != keep)
            lastNewlineAbs_ = static_cast<long long>(base_ + lastNl);

        if (keep > 0)
        {
            std::memmove(buf_.data(), buf_.data() + keep, len_ - keep);
            len_ -= keep;
            pos_ -= keep;
            tokPos_ -= keep;
            base_ += keep;
        }

        Fill();
    }

    Tok Next()
    {
        for (;;)
        {
            Tok t;

            if (TryNext(t))
                return t;

            if (ioError_)
            {
                Fail(pos_, "read error");
                return Tok::Error;
            }

            Refill(keepFrom_);
        }
    }

    // Scans one token; false if the window ended first (keepFrom_ tells Refill what to retain).
    bool TryNext(Tok& out)
    {
        const char* s = buf_.data();
        const size_t n = len_;

        // A comment cut by the end of the previous window continues here.
        if (inComment_)
        {
            while (pos_ < n && s[pos_] != '\n')
                ++pos_;

            if (pos_ >= n && !eof_)
            {
                keepFrom_ = pos_;
                return false;
            }

            inComment_ = false;
        }

        for (;;)
        {
            // the last character stays unread: a "*/" may be split by the window end
            if (inBlock_)
            {
                size_t i = pos_;

                while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
                    ++i;

                if (i + 1 < n)
                {
                    pos_ = i + 2;
                    inBlock_ = false;
                    continue;
                }

                pos_ = i;

                if (!eof_)
                {
                    keepFrom_ = pos_;
                    return false;
                }

                Fail(pos_, "unterminated comment", true);
                out = Tok::Error;
                return true;
            }

            if (pos_ >= n)
            {
                if (!eof_)
                {
                    keepFrom_ = pos_;
                    return false;
                }

                break;
            }

            while (pos_ < n && kChars.t[static_cast<unsigned char>(s[pos_])] == 1)
                ++pos_;

            if (pos_ >= n)
                continue;  // the checks above decide: refill or end

            const char c = s[pos_];

            if (c == '/')
            {
                if (pos_ + 1 >= n && !eof_)
                {
                    keepFrom_ = pos_;
                    return false;
                }

                if (pos_ + 1 < n && s[pos_ + 1] == '*')
                {
                    pos_ += 2;
                    inBlock_ = true;
                    continue;
                }

                if (pos_ + 1 < n && s[pos_ + 1] == '/')
                {
                    while (pos_ < n && s[pos_] != '\n')
                        ++pos_;

                    if (pos_ >= n && !eof_)
                    {
                        inComment_ = true;
                        keepFrom_ = pos_;
                        return false;
                    }

                    continue;
                }
            }

            break;
        }

        tokPos_ = pos_;

        if (pos_ >= n)
        {
            out = Tok::End;
            return true;
        }

        const char c = s[pos_];

        if (c == '{')
        {
            ++pos_;
            out = Tok::LBrace;
            return true;
        }

        if (c == '}')
        {
            ++pos_;
            out = Tok::RBrace;
            return true;
        }

        if (c == '"')
        {
            size_t i = pos_ + 1;
            bool escaped = false;

            while (i < n && s[i] != '"')
            {
                if (useEsc_ && s[i] == '\\')
                {
                    if (i + 1 >= n)
                    {
                        i = n;
                        break;
                    }  // the escape may continue in the next window

                    escaped = true;
                    i += 2;
                }
                else
                {
                    ++i;
                }
            }

            if (i >= n)
            {
                if (!eof_)
                {
                    keepFrom_ = tokPos_;
                    pos_ = tokPos_;
                    return false;
                }

                Fail(tokPos_, "unterminated string", true);
                out = Tok::Error;
                return true;
            }

            if (!escaped)
            {
                text_ = std::string_view(s + pos_ + 1, i - pos_ - 1);
            }
            else
            {
                scratch_.clear();

                for (size_t j = pos_ + 1; j < i; ++j)
                {
                    const char ch = s[j];

                    if (ch == '\\' && j + 1 < i)
                    {
                        switch (s[j + 1])
                        {
                            case '"':
                                scratch_ += '"';
                                break;
                            case '\\':
                                scratch_ += '\\';
                                break;
                            case 'n':
                                scratch_ += '\n';
                                break;
                            case 't':
                                scratch_ += '\t';
                                break;
                            case 'r':
                                scratch_ += '\r';
                                break;
                            default:
                                scratch_ += '\\';
                                scratch_ += s[j + 1];
                                break;
                        }

                        ++j;
                    }
                    else
                    {
                        scratch_ += ch;
                    }
                }

                text_ = scratch_;
            }

            pos_ = i + 1;
            out = Tok::Str;
            return true;
        }

        // a condition may contain blanks, but ends on its line, before any quote or brace
        if (c == '[')
        {
            size_t k = pos_ + 1;

            while (k < n && s[k] != ']' && s[k] != '\n' && s[k] != '"' && s[k] != '{' && s[k] != '}')
                ++k;

            if (k >= n && !eof_)
            {
                keepFrom_ = tokPos_;
                pos_ = tokPos_;
                return false;
            }

            if (k < n && s[k] == ']')
            {
                text_ = std::string_view(s + pos_, k + 1 - pos_);
                pos_ = k + 1;
                out = Tok::Cond;
                return true;
            }
        }

        size_t j = pos_;

        while (j < n)
        {
            const unsigned char k = kChars.t[static_cast<unsigned char>(s[j])];

            if (k)
            {
                if (k != 3)
                    break;

                if (j + 1 >= n && !eof_)
                {
                    j = n;
                    break;
                }

                if (j + 1 < n && s[j + 1] == '/')
                    break;
            }

            ++j;
        }

        if (j >= n && !eof_)
        {
            keepFrom_ = tokPos_;
            pos_ = tokPos_;
            return false;
        }

        text_ = std::string_view(s + pos_, j - pos_);
        pos_ = j;
        out = Tok::Bare;
        return true;
    }

    bool Eval() const
    {
        const bool r = CondEval(text_, conds_).Run();
        KV_LOG("condition '%.*s' -> %s\n", static_cast<int>(text_.size()), text_.data(), r ? "true" : "false");
        return r;
    }

    // Names are lower case: a name with capitals takes the slow folding path.
    uint32_t InternName(std::string_view name)
    {
        return kvname::HasUpper(name) ? kvname::Intern(name) : cache_.Intern(KVStringPool::Instance(), name);
    }

    // After the '{' of a root that is not wanted: only the braces are matched, nothing is stored.
    bool SkipBody()
    {
        for (size_t depth = 1; depth > 0;)
        {
            const Tok t = Next();

            if (t == Tok::Error)
                return false;

            if (t == Tok::End)
                return Fail(tokPos_, "unexpected end of file, missing '}'");

            if (t == Tok::LBrace)
                ++depth;
            else if (t == Tok::RBrace)
                --depth;
        }

        return true;
    }

    // An object's entries are staged on `stage_` and adopted in one go at its closing brace.
    bool ParseBody(CKeyValue& obj, int depth)
    {
        const size_t base = stage_.size();
        const bool ok = ParseEntries(depth);

        if (ok)
        {
            KVInternal::Adopt(obj, stage_.data() + base, stage_.size() - base);
        }
        else
        {
            for (size_t i = base; i < stage_.size(); ++i)
                if (stage_[i].type == CKeyValue::KeyValue)
                    delete stage_[i].child;
        }

        stage_.erase(stage_.begin() + static_cast<std::ptrdiff_t>(base), stage_.end());

        return ok;
    }

    bool ParseEntries(int depth)
    {
        if (depth > kMaxDepth)
            return Fail(tokPos_, "nesting too deep");

        KVStringPool& pool = KVStringPool::Instance();

        // A scalar is held back until the next token shows whether a condition follows it.
        struct
        {
            bool has = false, accepted = true;
            uint32_t key = 0, value = 0;
            int iv = 0;
            float fv = 0.f;
            CKeyValue::Type type = CKeyValue::String;
        } p;
        auto commit = [&]
        {
            if (p.has)
                stage_.push_back(KVInternal::MakeScalar(p.key, p.type, p.value, !p.accepted, p.iv, p.fv));

            p.has = false;
        };

        for (;;)
        {
            Tok t = Next();

            if (t == Tok::Cond && p.has)
            {
                p.accepted = Eval();
                t = Next();
            }

            commit();

            if (t == Tok::Error)
                return false;

            if (t == Tok::RBrace)
                return true;

            if (t == Tok::End)
                return Fail(tokPos_, "unexpected end of file, missing '}'");

            if (t == Tok::LBrace)
                return Fail(tokPos_, "'{' without key");

            if (t == Tok::Cond)
                return Fail(tokPos_, "unexpected conditional");

            if (text_.empty())
                return Fail(tokPos_, "empty key");

            const uint32_t key = InternName(text_);
            t = Next();
            bool accepted = true;

            if (t == Tok::Cond)
            {
                accepted = Eval();
                t = Next();
            }

            switch (t)
            {
                case Tok::Error:
                    return false;
                case Tok::End:
                    return Fail(tokPos_, "unexpected end of file, key without value");
                case Tok::RBrace:
                    return Fail(tokPos_, "key without value");
                case Tok::Cond:
                    return Fail(tokPos_, "unexpected conditional");
                case Tok::LBrace:
                {
                    auto child = std::make_unique<CKeyValue>();
                    KVInternal::SetUseCond(*child, useCond_);
                    KVInternal::SetUseEsc(*child, useEsc_);

                    if (!ParseBody(*child, depth + 1))
                        return false;

                    stage_.push_back(KVInternal::MakeChild(key, child.get(), !accepted));
                    child.release();
                    break;
                }

                case Tok::Str:
                // the type comes from the text, quotes don't matter
                case Tok::Bare:
                {
                    int iv;
                    float fv;
                    bool canonical;
                    const CKeyValue::Type type = kvnum::Classify(text_, iv, fv, canonical);

                    p.has = true;
                    p.accepted = accepted;
                    p.key = key;
                    p.type = type;
                    p.iv = iv;
                    p.fv = fv;
                    p.value = canonical ? kInvalidSlot : cache_.Intern(pool, text_);
                    break;
                }
            }
        }
    }

    FILE* f_;
    std::vector<char> buf_;
    size_t len_ = 0;
    size_t pos_ = 0;
    size_t tokPos_ = 0;
    size_t keepFrom_ = 0;
    size_t base_ = 0;
    bool eof_ = false;
    bool ioError_ = false;
    // The data was cut short (bad UTF-16): where in the converted text, and why. -1 = not cut.
    long long cutAbs_ = -1;
    const char* cutMsg_ = nullptr;
    enum class Enc { Utf8, Utf16LE, Utf16BE };
    Enc enc_ = Enc::Utf8;
    bool started_ = false;
    std::vector<unsigned char> raw_;  // UTF-16 bytes read but not converted yet
    size_t rawPos_ = 0;
    size_t rawLen_ = 0;
    bool inComment_ = false;
    bool inBlock_ = false;

    size_t discardedNewlines_ = 0;
    long long lastNewlineAbs_ = -1;

    InternCache cache_;
    std::vector<KVInternal::Node> stage_;
    CondTable conds_;
    bool useCond_;
    bool useEsc_;
    std::string_view text_;
    std::string scratch_;
    std::string err_;
    size_t errPos_ = 0;
};
}  // namespace

void CKeyValue::SetCondition(const char* name, bool value)
{
    const std::string n = UpperName(name ? name : "");

    if (n.empty())
        return;

    std::lock_guard<std::mutex> lock(g_condMutex);
    CondTable& t = CondDefaults();

    for (auto& e : t)
    {
        if (e.first == n)
        {
            e.second = value;
            return;
        }
    }

    t.push_back({n, value});
}

bool CKeyValue::LoadFromFile(const char* path)
{
    Clear();
    KV_LOG("loading '%s' (%s, conditions %s, escapes %s)\n", path,
           *GetName() ? (std::string("root '") + GetName() + "'").c_str() : "first root",
           UsesConditionals() ? "on" : "off", UsesEscapeSequences() ? "on" : "off");
    // closed on every exit, also when the pool throws
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path, "rb"), &std::fclose);
    FILE* f = file.get();

    if (!f)
    {
        std::fprintf(stderr, "%s:1:1: cannot open file\n", path);
        return false;
    }

#ifdef KV_VERBOSE
    const auto started = std::chrono::steady_clock::now();
#endif
    CKeyValue tmp(GetName());
    KVInternal::SetUseCond(tmp, UsesConditionals());
    KVInternal::SetUseEsc(tmp, UsesEscapeSequences());
    Parser parser(f, UsesConditionals(), UsesEscapeSequences());
    const bool ok = parser.Parse(tmp, path);
    file.reset();

    if (!ok)
    {
        parser.Report(path);
        KV_LOG("load of '%s' failed: the object is cleared\n", path);
        return false;
    }

    *this = std::move(tmp);
    KV_LOG("loaded '%s': %zu bytes of text, root '%s', %.2f ms\n", path, parser.BytesRead(), GetName(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());

    return true;
}
