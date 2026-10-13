#include "keyvalue.h"

#include <unordered_map>

#include "keyvalue_internal.h"

static_assert(sizeof(CKeyValue::Type) == 1, "Type must stay one byte so Node is 16 bytes");

namespace
{
KVStringPool& Pool()
{
    return KVStringPool::Instance();
}
}  // namespace

CKeyValue::CKeyValue() : nameSlot_(kvname::Intern("")) {}

CKeyValue::CKeyValue(const char* name) : nameSlot_(kvname::Intern(name ? name : "")) {}

CKeyValue::CKeyValue(const CKeyValue& other)
    : nameSlot_(other.nameSlot_), useCond_(other.useCond_), useEsc_(other.useEsc_), anyHidden_(other.anyHidden_)
{
    nodes_ = other.nodes_;

    for (Node& n : nodes_)
        if (n.type == KeyValue)
            n.child = new CKeyValue(*n.child);

    if (other.HasDups())
    {
        extra_ = std::make_unique<Extra>();
        extra_->next = other.extra_->next;
        extra_->tail = other.extra_->tail;
    }

    if (nodes_.size() > kIndexThreshold)
        BuildIndex();
}

CKeyValue::CKeyValue(CKeyValue&& other) noexcept
    : nameSlot_(other.nameSlot_), useCond_(other.useCond_), useEsc_(other.useEsc_), anyHidden_(other.anyHidden_),
      nodes_(std::move(other.nodes_)), extra_(std::move(other.extra_))
{
    other.nodes_.clear();
}

CKeyValue& CKeyValue::operator=(const CKeyValue& other)
{
    if (this != &other)
    {
        CKeyValue tmp(other);
        *this = std::move(tmp);
    }

    return *this;
}

CKeyValue& CKeyValue::operator=(CKeyValue&& other) noexcept
{
    if (this != &other)
    {
        // `other` may live inside this tree: take its contents first, free the old children after.
        std::vector<Node> old = std::move(nodes_);
        nameSlot_ = other.nameSlot_;
        useCond_ = other.useCond_;
        useEsc_ = other.useEsc_;
        anyHidden_ = other.anyHidden_;
        nodes_ = std::move(other.nodes_);
        extra_ = std::move(other.extra_);
        other.nodes_.clear();

        for (Node& n : old)
            if (n.type == KeyValue)
                delete n.child;
    }

    return *this;
}

CKeyValue::~CKeyValue()
{
    FreeChildren();
}

void CKeyValue::FreeChildren()
{
    for (Node& n : nodes_)
        if (n.type == KeyValue)
            delete n.child;
}

const char* CKeyValue::GetName() const
{
    return Pool().CStr(nameSlot_);
}

namespace
{
inline size_t IndexSlot(uint32_t nameSlot, unsigned shift)
{
    return (nameSlot * 2654435761u) >> shift;
}
}  // namespace

int CKeyValue::FindPos(uint32_t nameSlot) const
{
    if (HasIndex())
    {
        const std::vector<uint32_t>& t = extra_->table;
        const size_t mask = t.size() - 1;

        for (size_t i = IndexSlot(nameSlot, extra_->shift);; i = (i + 1) & mask)
        {
            const uint32_t e = t[i];

            if (e == 0)
                return -1;

            if (nodes_[e - 1].nameSlot == nameSlot)
                return static_cast<int>(e - 1);
        }
    }

    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].nameSlot == nameSlot)
            return static_cast<int>(i);

    return -1;
}

int CKeyValue::FindVisible(uint32_t nameSlot) const
{
    int pos = FindPos(nameSlot);

    if (!anyHidden_ || !useCond_)
        return pos;

    while (pos >= 0 && nodes_[static_cast<size_t>(pos)].hidden)
    {
        const uint32_t next = HasDups() ? extra_->next[static_cast<size_t>(pos)] : kEnd;
        pos = next == kEnd ? -1 : static_cast<int>(next);
    }

    return pos;
}

void CKeyValue::BuildIndex()
{
    size_t size = 64;
    unsigned bits = 6;

    while (size < nodes_.size() * 2)
    {
        size <<= 1;
        ++bits;
    }

    if (!extra_)
        extra_ = std::make_unique<Extra>();

    extra_->table.assign(size, 0);
    extra_->shift = 32 - bits;

    for (uint32_t i = 0; i < nodes_.size(); ++i)
        if (FindPos(nodes_[i].nameSlot) < 0)
            IndexInsert(nodes_[i].nameSlot, i);
}

void CKeyValue::IndexInsert(uint32_t nameSlot, uint32_t pos)
{
    std::vector<uint32_t>& t = extra_->table;
    const size_t mask = t.size() - 1;
    size_t i = IndexSlot(nameSlot, extra_->shift);

    while (t[i] != 0)
        i = (i + 1) & mask;

    t[i] = pos + 1;
}

// Stored names are lower case, so the name is tried as given first and folded only if that misses
// and it has capitals: the common lookup pays nothing for case-insensitivity.
int CKeyValue::FindByName(const char* name, bool visibleOnly) const
{
    const std::string_view v = name ? name : "";
    auto find = [&](uint32_t slot)
    {
        return slot == kInvalidSlot ? -1 : visibleOnly ? FindVisible(slot)
                                                       : FindPos(slot);
    };
    int pos = find(Pool().Find(v));

    if (pos < 0 && kvname::HasUpper(v))
        pos = find(kvname::Find(v));

    return pos;
}

const CKeyValue::Node* CKeyValue::FindNode(const char* name) const
{
    const int pos = FindByName(name, true);

    return pos < 0 ? nullptr : &nodes_[static_cast<size_t>(pos)];
}

CKeyValue::Node& CKeyValue::PushNode(uint32_t nameSlot, int firstSame)
{
    const uint32_t newPos = static_cast<uint32_t>(nodes_.size());
    Node fresh{};
    fresh.nameSlot = nameSlot;
    fresh.type = String;
    fresh.valueSlot = kInvalidSlot;
    nodes_.push_back(fresh);

    if (HasDups())
    {
        extra_->next.push_back(kEnd);
        extra_->tail.push_back(kEnd);
    }

    if (firstSame >= 0)
    {
        if (!HasDups())
        {
            if (!extra_)
                extra_ = std::make_unique<Extra>();

            extra_->next.assign(nodes_.size(), kEnd);
            extra_->tail.assign(nodes_.size(), kEnd);
        }

        const uint32_t first = static_cast<uint32_t>(firstSame);
        const uint32_t last = extra_->tail[first] == kEnd ? first : extra_->tail[first];
        extra_->next[last] = newPos;
        extra_->tail[first] = newPos;
    }
    else if (HasIndex())
    {
        if (nodes_.size() * 2 > extra_->table.size())
            BuildIndex();
        else
            IndexInsert(nameSlot, newPos);
    }
    else if (nodes_.size() > kIndexThreshold)
    {
        BuildIndex();
    }

    return nodes_.back();
}

CKeyValue::Node& CKeyValue::Upsert(uint32_t nameSlot)
{
    const int pos = FindVisible(nameSlot);

    if (pos >= 0)
    {
        Node& n = nodes_[static_cast<size_t>(pos)];

        if (n.type == KeyValue)
            delete n.child;

        return n;
    }

    return PushNode(nameSlot, FindPos(nameSlot));
}

CKeyValue::Node& CKeyValue::Append(uint32_t nameSlot)
{
    return PushNode(nameSlot, FindPos(nameSlot));
}

std::string CKeyValue::StringOf(const Node& n)
{
    return std::string(Pool().View(n.valueSlot));
}

int CKeyValue::IntOf(const Node& n, int def)
{
    if (n.type == Int)
        return n.intValue;

    if (n.type == Float)
    {
        // A float that is not a whole number truncates exactly. A whole one may stand for a longer
        // decimal text that rounded up to it (2.99999999 -> 3.0f), so the text decides.
        const float f = n.floatValue;

        if (f > -8388608.f && f < 8388608.f)
        {
            const int t = static_cast<int>(f);

            if (static_cast<float>(t) != f)
                return t;
        }

        double d = 0.0;

        if (!kvnum::ParseFloat(Pool().View(n.valueSlot), d) || !(d > -2147483649.0 && d < 2147483648.0))
            return def;

        return static_cast<int>(d);
    }

    return def;
}

float CKeyValue::FloatOf(const Node& n, float def)
{
    if (n.type == Float)
        return n.floatValue;

    if (n.type == Int)
        return static_cast<float>(n.intValue);

    return def;
}

bool CKeyValue::BoolOf(const Node& n, bool def)
{
    if (n.type == KeyValue)
        return def;

    switch (n.type)
    {
        case Int:
            return n.intValue != 0;

        case Float:
            return n.floatValue != 0.f;

        case String:
        {
            const std::string_view v = Pool().View(n.valueSlot);
            auto is = [&](const char* word)
            {
                size_t i = 0;

                for (; word[i]; ++i)
                    if (i >= v.size() || (v[i] | 0x20) != word[i])
                        return false;

                return i == v.size();
            };

            if (is("true"))
                return true;

            if (is("false"))
                return false;

            return def;
        }

        default:
            return def;
    }
}

std::string CKeyValue::GetString(const char* name, const char* def) const
{
    const Node* n = FindNode(name);

    if (!n || n->type == KeyValue)
        return def ? def : "";

    return StringOf(*n);
}

int CKeyValue::GetInt(const char* name, int def) const
{
    const Node* n = FindNode(name);

    return n ? IntOf(*n, def) : def;
}

float CKeyValue::GetFloat(const char* name, float def) const
{
    const Node* n = FindNode(name);

    return n ? FloatOf(*n, def) : def;
}

bool CKeyValue::GetBool(const char* name, bool def) const
{
    const Node* n = FindNode(name);

    return n ? BoolOf(*n, def) : def;
}

const CKeyValue* CKeyValue::GetKeyValue(const char* name, const CKeyValue* def) const
{
    const Node* n = FindNode(name);

    if (n && n->type == KeyValue)
        return n->child;

    return def ? new CKeyValue(*def) : nullptr;
}

CKeyValue* CKeyValue::GetKeyValue(const char* name, const CKeyValue* def)
{
    const Node* n = FindNode(name);

    if (n && n->type == KeyValue)
        return n->child;

    return def ? new CKeyValue(*def) : nullptr;
}

void CKeyValue::SetString(const char* name, const char* value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(value ? value : "");
    KVInternal::SetValue(*this, k, String, v);
}

void CKeyValue::SetInt(const char* name, int value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(kvnum::FormatInt(value));
    KVInternal::SetValue(*this, k, Int, v, value);
}

void CKeyValue::SetFloat(const char* name, float value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(kvnum::FormatFloat(value));
    KVInternal::SetValue(*this, k, Float, v, 0, value);
}

void CKeyValue::SetKeyValue(const char* name, const CKeyValue* value)
{
    if (!value)
        return;

    auto copy = std::make_unique<CKeyValue>(*value);  // `value` may live inside this tree

    if (copy->useCond_ != useCond_)
        copy->UsesConditionals(useCond_);

    if (copy->useEsc_ != useEsc_)
        copy->UsesEscapeSequences(useEsc_);

    KVInternal::SetChild(*this, kvname::Intern(name ? name : ""), std::move(copy));
}

void CKeyValue::AddString(const char* name, const char* value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(value ? value : "");
    KVInternal::AddValue(*this, k, String, v);
}

void CKeyValue::AddInt(const char* name, int value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(kvnum::FormatInt(value));
    KVInternal::AddValue(*this, k, Int, v, false, value);
}

void CKeyValue::AddFloat(const char* name, float value)
{
    const uint32_t k = kvname::Intern(name ? name : "");
    const uint32_t v = Pool().Intern(kvnum::FormatFloat(value));
    KVInternal::AddValue(*this, k, Float, v, false, 0, value);
}

void CKeyValue::AddKeyValue(const char* name, const CKeyValue* value)
{
    if (!value)
        return;

    auto copy = std::make_unique<CKeyValue>(*value);  // `value` may live inside this tree

    if (copy->useCond_ != useCond_)
        copy->UsesConditionals(useCond_);

    if (copy->useEsc_ != useEsc_)
        copy->UsesEscapeSequences(useEsc_);

    KVInternal::AddChild(*this, kvname::Intern(name ? name : ""), std::move(copy));
}

const char* CKeyValue::Child::Name() const
{
    return Pool().CStr(owner_->nodes_[pos_].nameSlot);
}

CKeyValue::Type CKeyValue::Child::GetType() const
{
    return owner_->nodes_[pos_].type;
}

std::string CKeyValue::Child::GetString(const char* def) const
{
    const Node& n = owner_->nodes_[pos_];

    return n.type == KeyValue ? std::string(def ? def : "") : StringOf(n);
}

int CKeyValue::Child::GetInt(int def) const
{
    return IntOf(owner_->nodes_[pos_], def);
}

float CKeyValue::Child::GetFloat(float def) const
{
    return FloatOf(owner_->nodes_[pos_], def);
}

bool CKeyValue::Child::GetBool(bool def) const
{
    return BoolOf(owner_->nodes_[pos_], def);
}

const CKeyValue* CKeyValue::Child::GetKeyValue() const
{
    const Node& n = owner_->nodes_[pos_];

    return n.type == KeyValue ? n.child : nullptr;
}

uint32_t CKeyValue::Step(uint32_t pos, bool sameName) const
{
    if (sameName)
        return HasDups() ? extra_->next[pos] : kEnd;

    return pos + 1 < nodes_.size() ? pos + 1 : kEnd;
}

uint32_t CKeyValue::SkipHidden(uint32_t pos, bool sameName) const
{
    if (!anyHidden_ || !useCond_)
        return pos;

    while (pos != kEnd && nodes_[pos].hidden)
        pos = Step(pos, sameName);

    return pos;
}

CKeyValue::ChildRange::iterator& CKeyValue::ChildRange::iterator::operator++()
{
    pos_ = owner_->SkipHidden(owner_->Step(pos_, same_), same_);

    return *this;
}

CKeyValue::ChildRange CKeyValue::Children() const
{
    return ChildRange(this, nodes_.empty() ? kEnd : SkipHidden(0u, false), false);
}

CKeyValue::ChildRange CKeyValue::Children(const char* name) const
{
    const int pos = FindByName(name, false);

    return ChildRange(this, pos < 0 ? kEnd : SkipHidden(static_cast<uint32_t>(pos), true), true);
}

size_t CKeyValue::Count() const
{
    if (!anyHidden_ || !useCond_)
        return nodes_.size();

    size_t n = 0;

    for (const Node& node : nodes_)
        n += node.hidden ? 0 : 1;

    return n;
}

size_t CKeyValue::Count(const char* name) const
{
    size_t n = 0;

    for (const Child c : Children(name))
    {
        (void)c;
        ++n;
    }

    return n;
}

std::vector<std::string> CKeyValue::GetStrings(const char* name) const
{
    std::vector<std::string> out;

    for (const Child c : Children(name))
        if (c.GetType() != KeyValue)
            out.push_back(c.GetString());

    return out;
}

std::vector<const CKeyValue*> CKeyValue::GetKeyValues(const char* name) const
{
    std::vector<const CKeyValue*> out;

    for (const Child c : Children(name))
        if (const CKeyValue* kv = c.GetKeyValue())
            out.push_back(kv);

    return out;
}

void CKeyValue::UsesConditionals(bool state)
{
    useCond_ = state;

    for (Node& n : nodes_)
        if (n.type == KeyValue)
            n.child->UsesConditionals(state);
}

void CKeyValue::UsesEscapeSequences(bool state)
{
    useEsc_ = state;

    for (Node& n : nodes_)
        if (n.type == KeyValue)
            n.child->UsesEscapeSequences(state);
}

void CKeyValue::RebuildExtra()
{
    std::vector<uint32_t> nextV, tailV;
    std::unordered_map<uint32_t, uint32_t> first;
    first.reserve(nodes_.size());

    for (uint32_t i = 0; i < nodes_.size(); ++i)
    {
        auto it = first.emplace(nodes_[i].nameSlot, i);

        if (it.second)
            continue;

        if (nextV.empty())
        {
            nextV.assign(nodes_.size(), kEnd);
            tailV.assign(nodes_.size(), kEnd);
        }

        const uint32_t f = it.first->second;
        const uint32_t last = tailV[f] == kEnd ? f : tailV[f];
        nextV[last] = i;
        tailV[f] = i;
    }

    if (!extra_)
        extra_ = std::make_unique<Extra>();

    extra_->next = std::move(nextV);
    extra_->tail = std::move(tailV);
    extra_->table.clear();

    if (nodes_.size() > kIndexThreshold)
        BuildIndex();

    if (!extra_->next.empty() || !extra_->table.empty())
        return;

    extra_.reset();
}

void CKeyValue::EraseMarked(const std::vector<uint32_t>& positions)
{
    if (positions.empty())
        return;

    size_t out = 0, m = 0;

    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        if (m < positions.size() && positions[m] == i)
        {
            if (nodes_[i].type == KeyValue)
                delete nodes_[i].child;

            ++m;
        }
        else
        {
            nodes_[out++] = nodes_[i];
        }
    }

    nodes_.resize(out);
    RebuildExtra();
}

bool CKeyValue::RemoveKey(const char* name)
{
    const int pos = FindByName(name, true);

    if (pos < 0)
        return false;

    EraseMarked({static_cast<uint32_t>(pos)});

    return true;
}

size_t CKeyValue::RemoveKeys(const char* name)
{
    std::vector<uint32_t> pos;

    for (int p = FindByName(name, true); p >= 0;)
    {
        pos.push_back(static_cast<uint32_t>(p));
        const uint32_t nx = SkipHidden(Step(static_cast<uint32_t>(p), true), true);
        p = nx == kEnd ? -1 : static_cast<int>(nx);
    }

    EraseMarked(pos);

    return pos.size();
}

void CKeyValue::Clear()
{
    anyHidden_ = false;
    FreeChildren();
    nodes_.clear();
    nodes_.shrink_to_fit();
    extra_.reset();
}

size_t CKeyValue::MemoryUsage() const
{
    size_t total = nodes_.capacity() * sizeof(Node);

    if (extra_)
        total += sizeof(Extra) + (extra_->table.capacity() + extra_->next.capacity() + extra_->tail.capacity()) * sizeof(uint32_t);

    for (const Node& n : nodes_)
        if (n.type == KeyValue)
            total += sizeof(CKeyValue) + n.child->MemoryUsage();

    return total;
}
