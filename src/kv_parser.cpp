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
    }
};
constexpr CharTable kChars;

// Conditions: a token "[...]" after a key, a value or the root name decides whether the entry
// exists. The text is an expression ($NAME, !, &&, ||, parentheses) over a table of names that the
// application can extend with SetCondition.
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

// Streaming parser: the file is read through a sliding window, so peak memory is the tree plus
// the window, not tree plus file. A token cut by the end of the window is rescanned after a refill.
class Parser
{
public:
    Parser(FILE* f, bool useCond, bool useEsc)
        : f_(f), buf_(kInitialWindow), conds_(SnapshotConditions()), useCond_(useCond), useEsc_(useEsc) {}

    bool Parse(CKeyValue& root)
    {
        Fill();

        if (len_ >= 3 && std::memcmp(buf_.data(), "\xEF\xBB\xBF", 3) == 0)
        {
            pos_ = 3;
            lastNewlineAbs_ = 2;
        }

        // A named root looks for a root of that name; an unnamed one takes the first root and its name.
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
                if (first)
                    return Fail(tokPos_, "empty file");

                if (!found)
                    return Fail(tokPos_, (std::string("root \"") + root.GetName() + "\" not found").c_str());

                return true;
            }

            first = false;

            if (t != Tok::Str && t != Tok::Bare)
                return Fail(tokPos_, "expected root name");

            const bool take = !found && (!wanted || kvname::Find(text_) == wantedSlot);

            if (take)
                KVInternal::SetNameSlot(root, kvname::Intern(text_));

            const uint32_t rootName = KVInternal::NameSlot(root);
            t = Next();
            bool accepted = true;

            if (t == Tok::Cond)
            {
                accepted = Eval();
                t = Next();
            }

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

    bool Fail(size_t localPos, const char* msg)
    {
        if (err_.empty())
        {
            err_ = msg;
            errPos_ = base_ + localPos;
        }

        return false;
    }

    void Fill()
    {
        if (eof_)
            return;

        if (len_ == buf_.size())
            buf_.resize(buf_.size() * 2);

        const size_t got = std::fread(buf_.data() + len_, 1, buf_.size() - len_, f_);
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
        for (size_t i = 0; i < keep; ++i)
        {
            if (buf_[i] == '\n')
            {
                ++discardedNewlines_;
                lastNewlineAbs_ = static_cast<long long>(base_ + i);
            }
        }

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

    // Scans one token in the window. Returns false if the window ended before the token was complete
    // (keepFrom_ tells Refill what to retain).
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

                Fail(pos_, "unterminated comment");
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

            const char c = s[pos_];

            if (kChars.t[static_cast<unsigned char>(c)] == 1)
            {
                ++pos_;
                continue;
            }

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

                Fail(tokPos_, "unterminated string");
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
            const char d = s[j];

            if (kChars.t[static_cast<unsigned char>(d)])
                break;

            if (d == '/')
            {
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

    bool ParseBody(CKeyValue& obj, int depth)
    {
        if (depth > kMaxDepth)
            return Fail(tokPos_, "nesting too deep");

        KVStringPool& pool = KVStringPool::Instance();
        KVInternal::Reserve(obj, 8);

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
                KVInternal::AddValue(obj, p.key, p.type, p.value, !p.accepted, p.iv, p.fv);

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

            const uint32_t key = kvname::Intern(text_);
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

                    KVInternal::ShrinkToFit(*child);
                    KVInternal::AddChild(obj, key, std::move(child), !accepted);
                    break;
                }

                case Tok::Str:
                // the type comes from the text, quotes don't matter
                case Tok::Bare:
                {
                    int iv;
                    float fv;
                    const CKeyValue::Type type = kvnum::Classify(text_, iv, fv);

                    p.has = true;
                    p.accepted = accepted;
                    p.key = key;
                    p.type = type;
                    p.iv = iv;
                    p.fv = fv;
                    p.value = pool.Intern(text_);
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
    bool inComment_ = false;
    bool inBlock_ = false;

    size_t discardedNewlines_ = 0;
    long long lastNewlineAbs_ = -1;

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
    const bool ok = parser.Parse(tmp);
    file.reset();

    if (!ok)
    {
        parser.Report(path);
        return false;
    }

    KVInternal::ShrinkToFit(tmp);
    *this = std::move(tmp);

    return true;
}
