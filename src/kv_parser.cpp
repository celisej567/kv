#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "keyvalue.h"
#include "keyvalue_internal.h"

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
                if (nulAbs_ >= 0)
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
                KVInternal::SetNameSlot(root, kvname::Intern(path));
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
            // the data was cut at a NUL byte: any failure at the cut is the NUL's
            if (nulAbs_ >= 0 && eof_ && (ranOut || pos_ >= len_))
            {
                err_ = "NUL byte in file";
                errPos_ = static_cast<size_t>(nulAbs_);
            }
            else
            {
                err_ = msg;
                errPos_ = base_ + localPos;
            }
        }

        return false;
    }

    void Fill()
    {
        if (eof_)
            return;

        if (len_ == buf_.size())
            buf_.resize(buf_.size() * 2);

        char* at = buf_.data() + len_;
        size_t got = std::fread(at, 1, buf_.size() - len_, f_);

        // A NUL byte is an error: the data ends right before it
        if (const void* nul = std::memchr(at, 0, got))
        {
            got = static_cast<size_t>(static_cast<const char*>(nul) - at);
            nulAbs_ = static_cast<long long>(base_ + len_ + got);
            eof_ = true;
            len_ += got;
            return;
        }

        len_ += got;

        if (got == 0)
        {
            eof_ = true;

            if (std::ferror(f_))
                ioError_ = true;
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

    bool Eval() const { return CondEval(text_, conds_).Run(); }

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
    long long nulAbs_ = -1;  // absolute offset of the first NUL byte, -1 if none
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
    // closed on every exit, also when the pool throws
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path, "rb"), &std::fclose);
    FILE* f = file.get();

    if (!f)
    {
        std::fprintf(stderr, "%s:1:1: cannot open file\n", path);
        return false;
    }

    CKeyValue tmp(GetName());
    KVInternal::SetUseCond(tmp, UsesConditionals());
    KVInternal::SetUseEsc(tmp, UsesEscapeSequences());
    Parser parser(f, UsesConditionals(), UsesEscapeSequences());
    const bool ok = parser.Parse(tmp, path);
    file.reset();

    if (!ok)
    {
        parser.Report(path);
        return false;
    }

    *this = std::move(tmp);

    return true;
}
