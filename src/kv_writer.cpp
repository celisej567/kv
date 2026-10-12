#include <cstdio>
#include <string>

#include "keyvalue.h"
#include "keyvalue_internal.h"

namespace
{
class Out
{
public:
    Out(FILE* f, bool esc) : f_(f), esc_(esc) { buf_.reserve(kBlock + 4096); }

    void Put(char c) { buf_ += c; }
    void Put(std::string_view s) { buf_.append(s.data(), s.size()); }
    void Indent(int level) { buf_.append(static_cast<size_t>(level) * 4, ' '); }

    void Quoted(std::string_view s)
    {
        buf_ += '"';

        if (!esc_)
        {
            buf_.append(s.data(), s.size());
            buf_ += '"';
            return;
        }

        size_t run = 0;

        for (size_t i = 0; i < s.size(); ++i)
        {
            const char* esc = nullptr;

            switch (s[i])
            {
                case '"':
                    esc = "\\\"";
                    break;
                case '\\':
                    esc = "\\\\";
                    break;
                case '\n':
                    esc = "\\n";
                    break;
                case '\t':
                    esc = "\\t";
                    break;
                case '\r':
                    esc = "\\r";
                    break;
                default:
                    continue;
            }

            buf_.append(s.data() + run, i - run);
            buf_.append(esc, 2);
            run = i + 1;
        }

        buf_.append(s.data() + run, s.size() - run);
        buf_ += '"';
    }

    void EndLine()
    {
        buf_ += '\n';

        if (buf_.size() >= kBlock)
            Flush();
    }

    void Flush()
    {
        if (!buf_.empty() && ok_)
            ok_ = std::fwrite(buf_.data(), 1, buf_.size(), f_) == buf_.size();

        buf_.clear();
    }

    bool ok() const { return ok_; }

private:
    static constexpr size_t kBlock = 64 * 1024;
    FILE* f_;
    bool esc_;
    std::string buf_;
    bool ok_ = true;
};

void WriteObject(Out& out, const CKeyValue& kv, int level)
{
    KVStringPool& pool = KVStringPool::Instance();
    out.Indent(level);
    out.Put('{');
    out.EndLine();

    for (const auto& n : KVInternal::Nodes(kv))
    {
        if (KVInternal::Skipped(kv, n))
            continue;

        out.Indent(level + 1);
        out.Quoted(pool.View(n.nameSlot));

        if (n.type == CKeyValue::KeyValue)
        {
            out.EndLine();
            WriteObject(out, *n.child, level + 1);
        }
        else
        {
            out.Put(' ');

            if (n.type == CKeyValue::String)
                out.Quoted(pool.View(n.valueSlot));
            else
                out.Put(pool.View(n.valueSlot));

            out.EndLine();
        }
    }

    out.Indent(level);
    out.Put('}');
    out.EndLine();
}
}  // namespace

bool CKeyValue::WriteToFile(const char* path) const
{
    FILE* f = std::fopen(path, "wb");

    if (!f)
        return false;

    Out out(f, UsesEscapeSequences());
    out.Quoted(KVStringPool::Instance().View(nameSlot_));
    out.EndLine();
    WriteObject(out, *this, 0);
    out.Flush();
    const bool ok = out.ok();

    return (std::fclose(f) == 0) && ok;
}
