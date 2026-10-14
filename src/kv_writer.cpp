#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "keyvalue.h"
#include "keyvalue_internal.h"
#include "kv_log.h"

namespace
{
// Block-written output buffer; appends are plain memcpy.
class Out
{
public:
    Out(FILE* f, bool esc) : f_(f), esc_(esc), d_(new char[kBlock + kSlack]), cap_(kBlock + kSlack) {}

    // Room for `n` more bytes; returns where they go. The caller then calls Commit.
    char* Room(size_t n)
    {
        if (cap_ - size_ < n)
            Grow(n);

        return d_.get() + size_;
    }

    void Commit(size_t n) { size_ += n; }

    void Put(char c) { *Room(1) = c, Commit(1); }

    void Put(std::string_view s)
    {
        std::memcpy(Room(s.size()), s.data(), s.size());
        Commit(s.size());
    }

    void Indent(int level)
    {
        const size_t n = static_cast<size_t>(level);
        std::memset(Room(n), '\t', n);
        Commit(n);
    }

    void Quoted(std::string_view s)
    {
        if (!esc_)
        {
            char* p = Room(s.size() + 2);
            *p++ = '"';
            std::memcpy(p, s.data(), s.size());
            p[s.size()] = '"';
            Commit(s.size() + 2);
            return;
        }

        Put('"');
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

            Put(std::string_view(s.data() + run, i - run));
            Put(std::string_view(esc, 2));
            run = i + 1;
        }

        Put(std::string_view(s.data() + run, s.size() - run));
        Put('"');
    }

    // A whole line with one room check; only without escape sequences.
    void Line(int level, std::string_view name, std::string_view value, bool quoteValue)
    {
        const size_t indent = static_cast<size_t>(level);
        char* p = Room(indent + name.size() + value.size() + 6);
        char* const start = p;
        std::memset(p, '\t', indent);
        p += indent;
        *p++ = '"';
        std::memcpy(p, name.data(), name.size());
        p += name.size();
        *p++ = '"';
        *p++ = '\t';

        if (quoteValue)
            *p++ = '"';

        std::memcpy(p, value.data(), value.size());
        p += value.size();

        if (quoteValue)
            *p++ = '"';

        *p++ = '\n';
        Commit(static_cast<size_t>(p - start));

        if (size_ >= kBlock)
            Flush();
    }

    bool escapes() const { return esc_; }

    void EndLine()
    {
        Put('\n');

        if (size_ >= kBlock)
            Flush();
    }

    void Flush()
    {
        if (size_ && ok_)
            ok_ = std::fwrite(d_.get(), 1, size_, f_) == size_;

        size_ = 0;
    }

    bool ok() const { return ok_; }

private:
    static constexpr size_t kBlock = 64 * 1024;
    static constexpr size_t kSlack = 4096;

    void Grow(size_t n)
    {
        Flush();

        if (cap_ >= n)
            return;

        d_.reset(new char[n]);
        cap_ = n;
    }

    FILE* f_;
    bool esc_;
    std::unique_ptr<char[]> d_;
    size_t cap_;
    size_t size_ = 0;
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

        if (n.type != CKeyValue::KeyValue && !out.escapes())
        {
            char num[KVInternal::kNumberText];
            out.Line(level + 1, pool.View(n.nameSlot), KVInternal::Text(n, num), n.type == CKeyValue::String);
            continue;
        }

        out.Indent(level + 1);
        out.Quoted(pool.View(n.nameSlot));

        if (n.type == CKeyValue::KeyValue)
        {
            out.EndLine();
            WriteObject(out, *n.child, level + 1);
        }
        else
        {
            out.Put('\t');
            char num[KVInternal::kNumberText];
            const std::string_view text = KVInternal::Text(n, num);

            if (n.type == CKeyValue::String)
                out.Quoted(text);
            else
                out.Put(text);

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
    // Closed on every exit, also when an allocation throws.
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path, "wb"), &std::fclose);

    if (!file)
    {
        std::fprintf(stderr, "%s: cannot create file\n", path);
        return false;
    }

    KV_LOG("writing '%s' (escapes %s)\n", path, UsesEscapeSequences() ? "on" : "off");
    bool ok = false;

    try
    {
        Out out(file.get(), UsesEscapeSequences());
        out.Quoted(KVStringPool::Instance().View(nameSlot_));
        out.EndLine();
        WriteObject(out, *this, 0);
        out.Flush();
        ok = out.ok();
    }
    catch (...)
    {
        file.reset();
        std::remove(path);  // not a half-written file
        KV_LOG("write of '%s' threw: the partial file is removed\n", path);
        throw;
    }

#ifdef KV_VERBOSE
    const long bytes = std::ftell(file.get());
#endif
    ok = (std::fclose(file.release()) == 0) && ok;

    if (!ok)
    {
        std::remove(path);  // a failed write leaves no file, not a truncated one
        std::fprintf(stderr, "%s: write error\n", path);
        KV_LOG("write of '%s' failed: the partial file is removed\n", path);
    }
    else
        KV_LOG("wrote '%s': %ld bytes\n", path, bytes);

    return ok;
}
