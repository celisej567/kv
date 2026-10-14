#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Assisted and commented using Claude. (sry too lazy to do them myself).

struct KVInternal;

// Tree of key/value pairs. All strings (names and values) live in the global KVStringPool and the
// tree stores only slot numbers and types. The type of a value comes from its text, quotes don't
// matter: an integer is Int, a decimal number is Float, anything else is String; SetString/AddString
// apply the same rule to the text they are given. Numbers keep their source text (written back as
// read) and the parsed value, which is computed once on load / Set*. Names are case-insensitive and
// stored in lower case; values are kept as they are. A name may occur many times.
class CKeyValue
{
public:
    enum Type : uint8_t
    {
        String,
        Int,
        Float,
        KeyValue
    };

    CKeyValue();                           // unnamed: LoadFromFile takes the name of the first root
    explicit CKeyValue(const char* name);  // an empty name means unnamed
    CKeyValue(const CKeyValue& other);     // deep copy
    CKeyValue(CKeyValue&& other) noexcept;
    CKeyValue& operator=(const CKeyValue& other);
    CKeyValue& operator=(CKeyValue&& other) noexcept;
    ~CKeyValue();

    // A file may hold several root objects. An unnamed object loads the first one and takes its
    // name; a first root without a name (the file starts with '{') is named after `path`. A named
    // one loads the first root with that name (case-insensitive); the other roots are only checked
    // for matching braces. No such root -> "root "name" not found". On failure prints
    // "file:line:col: message" to stderr, clears the contents (the name stays) and returns false.
    bool LoadFromFile(const char* path);
    // false: the file could not be created or written; a half-written file is removed.
    bool WriteToFile(const char* path) const;

    // Conditions: `"key" "value" [$LINUX]`, `"key" [!$A && ($B || $C)] { ... }`. Unset names are
    // false. True by default: WINDOWS and WIN32 on Windows; LINUX and POSIX on Linux; OSX and POSIX
    // on macOS. SetCondition is global; a load works on a snapshot taken when it starts.
    static void SetCondition(const char* name, bool value);

    // Entries whose condition is false are kept, but while this is true (the default) every accessor
    // and the writer act as if they did not exist. Applies to the whole subtree, survives LoadFromFile.
    void UsesConditionals(bool state);
    bool UsesConditionals() const { return useCond_; }

    // Backslash escapes in quoted text. false by default: a string ends at the next quote, so a
    // quote inside a value cannot be written. Set before LoadFromFile; applies to the whole subtree.
    void UsesEscapeSequences(bool state);
    bool UsesEscapeSequences() const { return useEsc_; }

    // Lower case. Points into the string pool: valid for the whole program.
    const char* GetName() const;

    std::string GetString(const char* name, const char* def = "") const;
    // Float: truncated toward zero. Out of int range or not a number -> def.
    int GetInt(const char* name, int def = 0) const;
    float GetFloat(const char* name, float def = 0.f) const;
    // Int/Float: not zero. String: "true" or "false", any case. Else def.
    bool GetBool(const char* name, bool def = false) const;
    // Found: a pointer into this tree, do NOT delete. Missing (or not a KeyValue): nullptr if `def`
    // is null, otherwise a NEW copy of `def` that the caller must delete.
    const CKeyValue* GetKeyValue(const char* name, const CKeyValue* def = nullptr) const;
    CKeyValue* GetKeyValue(const char* name, const CKeyValue* def = nullptr);

    // Replace the first entry with this name (keeping its position) or append a new one.
    void SetString(const char* name, const char* value);
    void SetInt(const char* name, int value);
    void SetFloat(const char* name, float value);
    void SetKeyValue(const char* name, const CKeyValue* value);  // copies; the copy takes this object's flags

    // Always append, even if the name exists. Get*/Set* see only the first entry of a name.
    void AddString(const char* name, const char* value);
    void AddInt(const char* name, int value);
    void AddFloat(const char* name, float value);
    void AddKeyValue(const char* name, const CKeyValue* value);  // copies, like SetKeyValue

    // Removes the first / every entry called `name` (returns how many). Entries switched off by a
    // condition count as absent. Invalidates Children() ranges and pointers to removed objects.
    bool RemoveKey(const char* name);
    size_t RemoveKeys(const char* name);

    // One entry, as handed out by Children(): a view that stays valid until the object is modified.
    class Child
    {
    public:
        const char* Name() const;
        Type GetType() const;
        std::string GetString(const char* def = "") const;
        int GetInt(int def = 0) const;
        float GetFloat(float def = 0.f) const;
        bool GetBool(bool def = false) const;
        const CKeyValue* GetKeyValue() const;  // nullptr unless GetType() == KeyValue

    private:
        friend class CKeyValue;
        Child(const CKeyValue* owner, uint32_t pos) : owner_(owner), pos_(pos) {}
        const CKeyValue* owner_;
        uint32_t pos_;
    };

    // Entries in file order, for a range-for. Children(name) walks every entry called `name`.
    class ChildRange
    {
    public:
        class iterator
        {
        public:
            Child operator*() const { return Child(owner_, pos_); }
            iterator& operator++();
            bool operator!=(const iterator& o) const { return pos_ != o.pos_; }
            bool operator==(const iterator& o) const { return pos_ == o.pos_; }

        private:
            friend class ChildRange;
            iterator(const CKeyValue* owner, uint32_t pos, bool sameName) : owner_(owner), pos_(pos), same_(sameName) {}
            const CKeyValue* owner_;
            uint32_t pos_;
            bool same_;
        };
        iterator begin() const { return iterator(owner_, first_, same_); }
        iterator end() const { return iterator(owner_, kEnd, same_); }
        bool empty() const { return first_ == kEnd; }

    private:
        friend class CKeyValue;
        ChildRange(const CKeyValue* owner, uint32_t first, bool sameName) : owner_(owner), first_(first), same_(sameName) {}
        const CKeyValue* owner_;
        uint32_t first_;
        bool same_;
    };
    ChildRange Children() const;
    ChildRange Children(const char* name) const;
    size_t Count() const;
    size_t Count(const char* name) const;

    // Allocating copies of every entry called `name`; GetStrings skips objects, GetKeyValues values.
    std::vector<std::string> GetStrings(const char* name) const;
    std::vector<const CKeyValue*> GetKeyValues(const char* name) const;

    void Clear();  // keeps the name

    // Heap bytes of this subtree, excluding the string pool.
    size_t MemoryUsage() const;

private:
    friend struct KVInternal;

    // `child` is owned by the enclosing CKeyValue.
    struct Node
    {
        uint32_t nameSlot;
        Type type;
        bool hidden;  // its condition was false
        union
        {
            struct
            {
                uint32_t valueSlot;  // String: the text in the pool. Int / Float: the text too, unless it is the
                                     // canonical one (kInvalidSlot), then it is made from the number
                union
                {
                    int32_t intValue;  // Int: the parsed value
                    float floatValue;  // Float: the parsed value
                };
            };
            CKeyValue* child;  // KeyValue
        };
    };

    // Optional per-object data, allocated only when needed:
    //  * a name index (nameSlot -> position + 1, 0 = empty) for objects with many keys; small
    //    objects use a linear scan;
    //  * chains of entries that share a name, allocated by the first duplicate. `next` and `tail`
    //    are indexed by position in nodes_: next = the following entry with the same name (kEnd if
    //    none); tail, for the first entry of a name, its last duplicate.
    struct Extra
    {
        std::vector<uint32_t> table;
        unsigned shift = 0;
        std::vector<uint32_t> next;
        std::vector<uint32_t> tail;
    };
    bool HasIndex() const { return extra_ && !extra_->table.empty(); }
    bool HasDups() const { return extra_ && !extra_->next.empty(); }

    static constexpr size_t kIndexThreshold = 16;
    static constexpr uint32_t kEnd = 0xFFFFFFFFu;

    int FindPos(uint32_t nameSlot) const;      // first entry with this name, hidden or not
    int FindVisible(uint32_t nameSlot) const;  // first one that is not skipped
    bool Skipped(const Node& n) const { return n.hidden && useCond_; }
    uint32_t Step(uint32_t pos, bool sameName) const;          // kEnd at the end
    uint32_t SkipHidden(uint32_t pos, bool sameName) const;    // `pos` or the next visible entry
    int FindByName(const char* name, bool visibleOnly) const;  // -1 if none
    const Node* FindNode(const char* name) const;
    Node& Upsert(uint32_t nameSlot);  // resets the first visible entry of the name (child freed) or appends
    Node& Append(uint32_t nameSlot);  // always a new entry
    Node& PushNode(uint32_t nameSlot, int firstSame);
    static std::string StringOf(const Node& n);
    static int IntOf(const Node& n, int def);
    static float FloatOf(const Node& n, float def);
    static bool BoolOf(const Node& n, bool def);
    void EraseMarked(const std::vector<uint32_t>& positions);  // ascending positions
    void RebuildExtra();
    void BuildIndex();
    void IndexInsert(uint32_t nameSlot, uint32_t pos);
    void FreeChildren();
    // Fills an empty object with `n` entries at once: the exact allocation, the name index and the
    // duplicate chains are built in one pass. Takes ownership of the children.
    void Adopt(const Node* first, size_t n);

    uint32_t nameSlot_;
    bool useCond_ = true;
    bool useEsc_ = false;
    bool anyHidden_ = false;  // some node has `hidden` set (lets the lookups skip the checks)
    std::vector<Node> nodes_;
    std::unique_ptr<Extra> extra_;
};
