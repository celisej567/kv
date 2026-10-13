# kv: how it behaves, in detail

This file describes what `CKeyValue` and `KVStringPool` really do, function by function, including the cases
that are easy to get wrong. The short version is in the [README](../README.md); the comparison with Source is in
[bench/compare/README.md](../bench/compare/README.md). Everything here was checked against the code (and the
non-obvious cases by running them).

Contents

1. [The model in one page](#1-the-model-in-one-page)
2. [KVStringPool](#2-kvstringpool)
3. [CKeyValue: object, names, copies](#3-ckeyvalue-object-names-copies)
4. [Value types and the getters](#4-value-types-and-the-getters)
5. [Setters, adders, removal](#5-setters-adders-removal)
6. [Iteration and ranges](#6-iteration-and-ranges)
7. [Conditions and `UsesConditionals`](#7-conditions-and-usesconditionals)
8. [Escape sequences](#8-escape-sequences)
9. [Reading a file](#9-reading-a-file)
10. [Writing a file](#10-writing-a-file)
11. [Memory, lifetime and invalidation](#11-memory-lifetime-and-invalidation)
12. [Threads](#12-threads)
13. [Checklist of surprises](#13-checklist-of-surprises)

---

## 1. The model in one page

* A `CKeyValue` is an **ordered list of entries** (`name`, `type`, `value`). The value of an entry is a number,
  a string, or another `CKeyValue` (a block). A name may occur many times; the order is the order of insertion
  (the order in the file after a load).
* **Every string lives in one global pool** (`KVStringPool`): names and values alike. A tree stores only
  32-bit slot numbers. The one exception is a number whose text is the canonical one (what `SetInt` / `SetFloat`
  would write: `42`, `-7`, `1.5`, `2.0`): it keeps no text, `GetString` and the writer make it from the value.
  `007`, `+5`, `-0`, `1.50`, `1e3` are not canonical and keep their text in the pool. The pool only grows; nothing in it is ever freed.
* **Names are case-insensitive** and are stored in lower case (ASCII `A-Z` only). **Values keep their case.**
* The **type of a value is decided from its text when a file is read** (quotes do not matter). `SetString`/`AddString` decide
  it from the text the same way; `SetInt`/`SetFloat` set it from the function you call. A number is **written back exactly as it was read** and keeps its
  **parsed value** (so `GetInt`/`GetFloat` do not parse again).
* An entry whose condition (`[$X]`) is false is **kept but hidden**. While `UsesConditionals()` is true (the
  default) every function treats a hidden entry as if it did not exist.
* `sizeof(CKeyValue)` is 40 bytes; an entry is 16 bytes.

## 2. KVStringPool

`KVStringPool::Instance()` is a function-local static, created on first use. `CKeyValue` constructors intern the
empty string, so the pool exists before any tree does and is destroyed after every static `CKeyValue`.

### Layout

* **Text** is kept in chunks that never move. The first chunk is 64 KB; each new chunk doubles, up to 4 MB.
  A string larger than 1 MB gets a chunk of its own. A string is stored with a terminating `'\0'`.
* **Entries** are 12 bytes: `{ref, length, hash}`, where `ref = chunk << 22 | offset`. Slot numbers are the
  index of the entry (0, 1, 2, ... in order of first interning).
* **Lookup table**: open addressing, linear probing, a word-at-a-time multiply hash (`KVHashString`), doubled when it would pass 75% full.
* Strings are unique: interning the same bytes twice gives the same slot. Comparison is **byte for byte and
  case-sensitive**. Case folding of *names* is done above the pool (see section 3), not inside it.

### Functions

| function | behaviour |
|---|---|
| `Intern(sv, hash)` | The same with the hash already computed (`hash` must be `KVHashString(sv)`); used by the parser. |
| `Intern(sv)` | Returns the slot of `sv`, adding it if it is new. Works for any bytes, including embedded `'\0'` and the empty string. Never fails for a string that fits (see limits). |
| `Find(sv)` | Slot of `sv`, or `kInvalidSlot` (`0xFFFFFFFF`) if it is not there. Does not add anything. |
| `View(slot)` | `string_view` of the stored bytes (length known, so embedded `'\0'` is kept). **Throws `std::out_of_range`** for a slot that does not exist. |
| `CStr(slot)` | Pointer to the `'\0'`-terminated text. Also throws `std::out_of_range` for an unknown slot. |
| `Count()` | Number of distinct strings. The empty string is always one of them. |
| `BytesUsed()` | Characters stored, including one `'\0'` per string. |
| `MemoryUsage()` / `GetStats()` | Heap bytes the pool holds (`allocated`) and a breakdown (`used`, chunks, entries, table, `directory` is inside the object, not on the heap). |

### Guarantees and limits

* **Pointers and views stay valid for the whole program.** Strings never move and are never removed.
  `CKeyValue::GetName()` returns such a pointer: it is valid after the tree is destroyed.
* **Nothing is ever freed.** `Clear()`, destroying a tree or replacing a value does not shrink the pool. Every
  *distinct* string ever interned stays. Example (measured): 100 000 `SetInt("x", 1000000 + i)` on one key leaves a
  tree with one entry but adds 100 000 strings (about 800 KB) to the pool, and `Clear()` does not give them back.
  Repeated values cost nothing, new values cost memory.
* Limits: a string must be shorter than 4 GB, there can be at most 2^32 - 2 strings, and at most **1024 chunks**
  (a 4 MB chunk is the biggest regular one; every string over 1 MB takes a chunk of its own, so about a thousand
  such strings are the most). Past a limit `Intern` throws `std::length_error`. A load that hits it throws out of
  `LoadFromFile` (it does not return `false`).
* Slot numbers are only meaningful inside one process. Nothing stores them in files.

### Thread safety (build option `KV_THREAD_SAFE`)

See [section 12](#12-threads). Short: the plain build is **not** safe to use from two threads; the thread-safe
build is.

---

## 3. CKeyValue: object, names, copies

### Construction

| expression | meaning |
|---|---|
| `CKeyValue kv;` | **Unnamed** object: the name is the empty string. |
| `CKeyValue kv("name");` | Named object. `nullptr` means `""`. An empty name is the same as unnamed. |
| copy / move | See below. |

The name is lower-cased: `CKeyValue("MyRoot").GetName()` is `"myroot"`.

`GetName()` returns a pointer into the pool (valid for the whole program).

### What a name is used for

* When the object is **written**, its name is the root line of the file.
* When an object is **loaded**: an unnamed object takes the name of the first root of the file; a named object
  looks for the root with its name (see section 9).
* When an object is stored in another one with `SetKeyValue`/`AddKeyValue`, it takes the **key name** it is stored
  under (the copy, not your original).

### Copy, move, assignment

* **Copy constructor / copy assignment**: a deep copy of the whole tree. Copies the **name** and the two flags
  (`UsesConditionals`, `UsesEscapeSequences`), hidden entries included. Copy assignment therefore **renames** the
  target: `CKeyValue a("A"), b("B"); b = a;` makes `b` named `"a"`.
* **Move constructor / move assignment**: take the contents, the name and the flags. The moved-from object is
  left with **no entries but with its old name and flags** (it is valid and empty). Move assignment likewise
  takes the other's name.
* Self-assignment is safe. Assigning from an object that lives inside the target (`kv = *kv.GetKeyValue("x")`)
  is safe too: the contents are taken before the old children are freed.

### Names

* Matching is case-insensitive for **ASCII** letters. Non-ASCII bytes are compared as they are.
* `Get*`, `Set*`, `Add*`, `Remove*`, `Children(name)`, `Count(name)` all fold the name you pass. A name may contain
  spaces or any other character (a name with spaces is written in quotes). `nullptr` as a name means `""`.
* An entry with an **empty name** can be created through `SetString("", ...)`; it is written as `""`, but a file
  with an empty key inside a block is rejected on load (`empty key`).
* Lookup cost: objects with up to 16 entries scan linearly; above that an index (a small hash table of slots) is
  built and kept up to date. For duplicate names the entries are chained, so `Children(name)` walks only that
  name.

### Flags

`UsesConditionals(bool)` and `UsesEscapeSequences(bool)` apply to the **whole subtree** below the object and
survive `Clear()` and `LoadFromFile`. Details in sections 7 and 8. When an object is stored with
`SetKeyValue`/`AddKeyValue`, the **copy takes the parent's flags**; the original is not changed.

### `Clear()`

Frees all entries and children and releases the memory. Keeps the **name** and both **flags**. Pointers to
children and `Children()` ranges become invalid.

---

## 4. Value types and the getters

### How the type is decided

On **loading**, for every value token (quoted or not):

1. If the first character is not a digit, `-`, `+` or `.`, the type is `String`.
2. Otherwise, if the whole text parses as a 32-bit integer, it is `Int`. One leading `+` is allowed; `+-5`,
   `++5`, `5abc`, `0x10` are not integers.
3. Otherwise, if the whole text parses as a `float` (`from_chars`: `1.5`, `.5`, `-.5`, `1e3`, `5.`), it is `Float`.
   An integer too big for `int` (`3000000000`) is therefore a `Float`. A number outside the `float` range
   (`1e39`, `1e-46`) is a `String`.
4. Otherwise `String`.

Quotes do not matter: `"5"` and `5` are both `Int`. A value of `true`/`false` is a `String`. An empty value `""`
is a `String`. A **block** (`{ ... }`) is `KeyValue`.

The text is kept exactly as written: `"007"` is an `Int` whose text is `007` (`GetInt` is 7, `GetString` is
`"007"`), `1.50` stays `1.50`, `+5` stays `+5`, `-0` is an `Int` with value 0.

`SetString`/`AddString` use **the same rules** on the text you pass, so the tree is the same whether a value
came from a file or from `SetString("n", "42")` (an `Int`). `SetInt`/`SetFloat` (and `AddInt`/`AddFloat`) set
the type of the function you call.

### Stored value

An `Int` entry holds its `int`, a `Float` entry its `float`, next to the slot of its text. The text is used for
writing and for `GetString`. (`GetInt` on a `Float` whose float is a whole number, such as `5.0`, or that is out of
range goes back to the text, so that it truncates what was written and not what `float` rounded it to: the text
`2.99999999` has the float value `3.0` but `GetInt` is 2.)

### The getters

`name` is looked up case-insensitively; only **visible** entries are considered; with duplicate names the
**first** one is used. If no such entry exists the default is returned.

| call | on `Int` | on `Float` | on `String` | on a block |
|---|---|---|---|---|
| `GetString(name, def="")` | the text (`"007"`) | the text (`"1.50"`) | the text | `def` |
| `GetInt(name, def=0)` | the value | truncated toward zero; out of `int` range -> `def` | `def` (a `String` entry, such as `abc` or `0x10`, is never read as a number) | `def` |
| `GetFloat(name, def=0)` | the value as `float` | the value | `def` | `def` |
| `GetBool(name, def=false)` | `value != 0` | `value != 0` | `"true"` -> true, `"false"` -> false (any case); anything else -> `def` | `def` |

Other details:

* `GetString` with `def == nullptr` returns `""`. A missing name (or a `nullptr` name) returns `def`.
* `GetString` on a number returns **the text as it is stored**, not a reformatting of the value: a file value
  `007` gives `"007"`, `1.50` gives `"1.50"`, `+5` gives `"+5"`, a quoted `"5"` gives `"5"`; `SetInt(-5)` gives
  `"-5"`, `SetFloat(2.0f)` gives `"2.0"`, `SetFloat(0.1f)` gives `"0.1"`.
* `GetString` on a **block** returns `def` (`""` if `def` is `nullptr`); it never fails and does not print the block.
  `Child::GetString(def)` behaves the same.
* `GetInt` on a `Float`: `3.9` -> 3, `-3.9` -> -3, `2147483647.0` -> 2147483647, `3000000000.5` -> `def`.
* `GetFloat` on an `Int` converts (very large ints lose precision, as in any `int` -> `float`).
* A quoted `"true"` is a `String`, so `GetBool` handles it by the word; `1`, `"1"` and `1.0` are numbers.
* The default is returned, not an error. There is no way to tell "missing" from "present and equal to the
  default" except by looking at the entry (`Children(name)` / `Count(name)`).
* The typed getters never change anything.

### `GetKeyValue`

```cpp
const CKeyValue* GetKeyValue(const char* name, const CKeyValue* def = nullptr) const;
CKeyValue*       GetKeyValue(const char* name, const CKeyValue* def = nullptr);   // non-const object
```

* Found and it is a block: a **pointer into this tree**. Do not delete it. It stays valid until that entry is
  replaced or removed, or the tree is cleared/destroyed (adding other entries does not move it, because every
  block is a separate heap object).
* Missing, or the entry is not a block: `nullptr` if `def` is `nullptr`, otherwise a **new copy of `def`** that the
  caller owns and must `delete`. This is true even for the `const` overload.
* An entry that exists but is a scalar counts as missing here (and the other way round: `GetString` of a block
  returns `def`).

### Getters of a `Child` (from iteration)

`Child::GetString/GetInt/GetFloat/GetBool` follow the same table. `Child::GetKeyValue()` returns the block
pointer, or `nullptr` if the entry is not a block. `Child::Name()` is the lower-case name. `Child::GetType()` is
the stored type (`String`, `Int`, `Float`, `KeyValue`).

---

## 5. Setters, adders, removal

### Set* versus Add*

* `SetX(name, value)` replaces the **first visible** entry with that name **in place** (same position in the
  list), or appends a new one if there is none. Other entries with the same name are left alone.
* `AddX(name, value)` **always appends**, even if the name exists.
* With duplicates, `Get*` still sees only the first one; use `Children(name)` to reach the rest.

```cpp
kv.AddInt("k", 1); kv.AddInt("k", 2); kv.AddInt("k", 3);
kv.SetInt("k", 10);          // k = 10, 2, 3   (first replaced)
kv.RemoveKey("k");           // k = 2, 3
kv.RemoveKeys("K");          // removes both, returns 2
```

### Types written by the setters

| call | type | text kept |
|---|---|---|
| `SetString` / `AddString` | decided from the text, like a file read: `Int`, `Float` or `String` | exactly the text you pass (`nullptr` -> `""`) |
| `SetInt` / `AddInt` | `Int` | decimal text of the value |
| `SetFloat` / `AddFloat` | `Float` | shortest text that reads back to the same `float` (`to_chars`), with `.0` added when it would look like an integer: `0.1f` -> `0.1`, `2.0f` -> `2.0`, `-0.0f` -> `-0.0`, `1e10f` -> `1e+10` |
| `SetKeyValue` / `AddKeyValue` | block | a **copy** of the object you pass |

Consequences worth knowing:

* **`SetString("a", "42")` makes an `Int`** (text `42`), `SetString("a", "3.5")` a `Float`, `SetString("a", "abc")` a
  `String`. Only the *whole* text counts: `" 42"`, `"0x10"`, `"42abc"` and `""` stay strings. The type therefore
  does not change when the tree is written and read back (`"a" 42` is written for the `Int`, without quotes).
  If you need the string `"42"` to stay a string you cannot: the library has no way to mark a number-like text
  as a string, a file read has none either.
* `SetFloat(NaN)` / `SetFloat(inf)` store the value you gave (so `GetFloat` returns it), but the text is `nan` /
  `inf`, which a later load reads back as a `String`.
* Replacing an entry may change its type (`SetInt` over a string, `SetString` over a block). When the old entry
  was a block, **that block is deleted**: any pointer you got from `GetKeyValue` for it dangles.
* `Set*` on a name that exists only as a **hidden** entry (condition false, `UsesConditionals` true) does not
  touch the hidden one: it appends a new visible entry after it. With conditions switched off you then see
  both, the hidden one first.

### `SetKeyValue` / `AddKeyValue`

* A `nullptr` value is ignored.
* The object is **copied first**, so you may pass an object that lives inside this tree (including `*this` or one
  of its children); the result is well defined.
* The copy is **renamed** to the key name, and takes this object's `UsesConditionals` and
  `UsesEscapeSequences`. Your original keeps its name and flags.
* Copying keeps hidden entries inside the copy.

### Removal

* `RemoveKey(name)`: removes the first **visible** entry with that name; returns `true` if one was removed.
* `RemoveKeys(name)`: removes every **visible** entry with that name; returns how many.
* Hidden entries are not touched while `UsesConditionals` is true (with false they count as ordinary entries).
* Removed blocks are deleted: pointers to them dangle. Pointers to *other* blocks stay valid.
* Removal rebuilds the duplicate chains and the index: it is O(number of entries of that object), not O(1).
* It invalidates every `Children()` range/iterator/`Child` on that object (the positions shift).

---

## 6. Iteration and ranges

```cpp
for (const CKeyValue::Child c : kv.Children())        // every visible entry, in file order
for (const CKeyValue::Child c : kv.Children("path"))  // every visible entry with this name
```

* A `Child` is a light view `(owner, position)`. It stays valid **until the owner is modified** (any
  `Set*`/`Add*`/`Remove*`/`Clear`/load). Using it afterwards is undefined; do not hold it across a change.
* Hidden entries are skipped (when `UsesConditionals` is true), in both forms.
* `Children(name)` is empty for an unknown name; `empty()` tells you.
* `Count()`: number of visible entries; O(1) unless some entry is hidden and conditions are on (then O(n)).
  `Count(name)`: number of visible entries with that name (walks the duplicate chain).
* `GetStrings(name)`: a fresh vector with the text of every visible scalar entry called `name` (numbers as their
  text); blocks are skipped. `GetKeyValues(name)`: pointers to every visible block with that name (pointers into
  the tree: do not delete).

---

## 7. Conditions and `UsesConditionals`

### Syntax

A condition is a token `[ expression ]` on one line:

```
"key" "value" [$LINUX]
"key" [!$X360] "value"
"key" "value" [$A && ($B || !$C)]
"block" [$LINUX] { ... }
Root [$LINUX] { ... }
```

* Names are written `$NAME`, matched case-insensitively. Operators: `!`, `&&`, `||`, parentheses. Blanks inside
  are allowed (`[ $LINUX ]`).
* An unknown name is **false**. A malformed expression is **false** (the entry is hidden, no error). The `[`
  token stops at the first `]`; if it meets the end of the line, a quote or a brace first, it is not a condition
  but an ordinary token.
* True by default: `WINDOWS`, `WIN32` on Windows; `LINUX`, `POSIX` on Linux; `OSX`, `POSIX` on macOS (there is
  no `LINUX` there). Add or change names with the static `CKeyValue::SetCondition("NAME", bool)`. The table is
  global; a load works on a **snapshot taken when it starts**, and conditions are evaluated **while loading**,
  so changing a name later does not re-evaluate an object that is already loaded.

### Where a condition may stand

* After the **key**, before the value: `a [$X] 1`.
* After the **value**: `a 1 [$X]`.
* After the **root name**: `Root [$X] { }`.
* After a block's `{ ... }`: **error** (`unexpected conditional`). First token of a block: error. Two in a row:
  error.
* If there is one on **both** sides of a value (`a [$X360] 1 [$LINUX]`), **the last one wins** (they are not
  ANDed): that entry is visible. (`a [$LINUX] 1 [$X360]` is hidden.)

### Hidden entries

An entry with a false condition is loaded and **kept**, marked hidden. While `UsesConditionals()` is **true** (default):

* `Get*`, `GetKeyValue`, `Children`, `Count`, `GetStrings`, `GetKeyValues`, `RemoveKey(s)` and `Set*` behave as if
  the entry were not there;
* `WriteToFile` does not write it;
* a name that exists only as a hidden entry is "missing" for the getters.

With `UsesConditionals(false)` everything is visible: the hidden entries take part in all of the above, and
`WriteToFile` writes them as ordinary entries **without** their condition (conditions are never written back).

Example (file: `a 1 [$X360]`, `a 2 [$LINUX]`, `b 5 [$X360]`, `c [$X360] { d 1 }`, `e 7`):

| | `Count()` | `Count("a")` | `GetInt("a")` | `GetInt("b", -1)` | `GetKeyValue("c")` |
|---|---|---|---|---|---|
| `UsesConditionals(true)` | 2 | 1 | 2 | -1 | `nullptr` |
| `UsesConditionals(false)` | 5 | 2 | 1 (the first in the file) | 5 | the block |

* The flag is per object and is set on the whole subtree by the setter; `SetKeyValue`/`AddKeyValue` copy the
  parent's value into the copy. It survives `Clear()` and `LoadFromFile`. Set it **before or after** loading; the
  hidden marks are stored either way.
* If the **root** condition is false (`Root [$X360] {...}` with conditions on) the loaded object is **empty but
  keeps its name**.

---

## 8. Escape sequences

`UsesEscapeSequences(bool)`, **false by default**.

* **Off (default):** a quoted string ends at the next `"`. A backslash is an ordinary character. A quote cannot be
  inside a value, and `"C:\dir\"` is the text `C:\dir\`. When **writing**, text is written as it is, in quotes,
  with no escaping: a value containing `"` or a newline produces a file that does not read back as the same value.
* **On:** inside quotes `\"` `\\` `\n` `\t` `\r` are converted; any other `\x` is kept as the two characters.
  When **writing**, `"`, `\`, newline, tab and carriage return are escaped, so a value round-trips.
* It only concerns **quoted** tokens. Unquoted tokens are never unescaped.
* Set it **before** `LoadFromFile` (it decides how the file is read). It propagates to the subtree, is copied
  by `SetKeyValue`/`AddKeyValue` (the parent's setting wins) and survives `Clear()` and loading.

---

## 9. Reading a file

`bool LoadFromFile(const char* path)`

### Steps and what comes out

1. The object is **cleared first** (`Clear()`), so on any failure it is empty. Its **name and flags stay**.
2. If the file cannot be opened: prints `path:1:1: cannot open file` and returns `false`.
3. The file is read through a sliding window (256 KB, grows for a longer token), so memory is the tree and not
   tree plus file. A UTF-8 BOM at the start is skipped. `\r\n` is treated like `\n` (it is whitespace).
4. On a syntax error: prints `path:line:column: message` to `stderr`, returns `false`, leaves the object empty.
   No exceptions for malformed input. (Exceptions can still come from the pool limits, section 2, or
   `std::bad_alloc`.)
5. **Nothing is left behind on any exit** (`false` or an exception): the file is closed, the read window and the
   half-built tree are freed, and the object is empty. `tests/run_tests.py` checks this by making every allocation of a
   load, a write and a copy fail in turn (`kv_stress allocfail`): afterwards the heap, the open files and the output
   file are exactly as before. Strings the failed load had already added to the pool stay there, as always.

### Which root is loaded (several roots in one file)

A file may contain several root objects one after another: `"A" { ... } "B" { ... }`.

* **Unnamed object** (`CKeyValue kv;` or `CKeyValue("")`): loads the **first** root and takes its name.
* **Named object**: loads the **first root with that name** (case-insensitive) and keeps its name. If there is
  none: prints `path:line:col: root "name" not found`, returns `false`, the object is empty.
* The other roots are **not stored**: they are only checked for matching braces (a missing `}` or something that
  is not `name { ... }` after a root is still an error).
* Once an unnamed object has taken a name, a later `LoadFromFile` into it looks for **that** name.
* A root whose name is empty (`"" { a 1 }`) is accepted. Loading it into an unnamed object leaves the object
  unnamed.
* If the chosen root has a false condition (and conditions are on), the object is empty but named.

### Tokens

* **Whitespace**: space, tab, CR, LF, form feed, vertical tab.
* **Comments**: `// to end of line` and `/* ... */`. A `/*` starts a comment only at the **start of a token**:
  `hl2/custom/*` is one value. `//` inside a **quoted** string is text; inside an **unquoted** token it ends the
  token and starts a comment. An unclosed `/*` is an error (`unterminated comment`).
* **Quoted token**: from `"` to the next `"` (to the next unescaped `"` with escapes on). It may contain
  newlines. Never empty as a key; `""` is a valid empty value. An unclosed one is an error
  (`unterminated string`).
* **Unquoted token**: everything up to whitespace, `{`, `}`, `"` or `//`. So `a"b"` is the token `a` then the
  string `b`.
* `{` and `}` are their own tokens even without spaces: `a{b 1}` works.

### Grammar of a block

`name value`, `name { block }`, optionally with a condition (section 7). In a block:

| situation | error |
|---|---|
| `{` with no key before it | `'{' without key` |
| empty key (`"" 1`) | `empty key` |
| key followed by `}` | `key without value` |
| key followed by end of file | `unexpected end of file, key without value` |
| block not closed | `unexpected end of file, missing '}'` |
| condition where it is not allowed | `unexpected conditional` |
| nesting deeper than 512 | `nesting too deep` |
| before the root: empty file / not a name / no `{` | `empty file` / `expected root name` / `expected '{' after root name` |

Duplicate keys and keys that differ only in case are kept as separate entries, in order.
`#include` and `#base` are **not** processed: inside a block they are ordinary keys; before the root they are an
error.

The error line/column point at the offending token (for end-of-file errors, at the end).

---

## 10. Writing a file

`bool WriteToFile(const char* path) const`

* Returns `false` if the file cannot be created or a write fails. A write that fails after the file was created
  (disk full, ...) **removes the half-written file**; the file handle is always closed, also when an allocation throws.
* Format: the object's name in quotes on a line, then the block. Everything is indented with **one tab per level** and a key is separated from its value by **one tab**,
  braces on their own lines:

  ```
  "game"
  {
  	"name"	"My game"
  	"version"	3
  	"window"
  	{
  		"width"	1280
  	}
  }
  ```
* Names are always written in quotes, in lower case.
* `String` values are written **in quotes**; `Int` and `Float` values **without quotes, with the text they have**
  (`007`, `+5`, `1.50` come back unchanged).
* Order is preserved. Duplicate keys are written one after another.
* Hidden entries are skipped while `UsesConditionals` is true; with false they are written without their
  condition (see section 7).
* Escapes follow `UsesEscapeSequences` (section 8).
* An unnamed object writes `""` as its name; that file loads into an unnamed object and stays unnamed.
* The output is not byte-identical to the input (comments, quoting and layout are not kept), but loading it
  gives the same tree, except for `SetFloat(NaN)` / `SetFloat(inf)`, which read back as strings (section 5).

---

## 11. Memory, lifetime and invalidation

### What holds what

* An object holds its entries in a `std::vector` (16 bytes each), an optional index/duplicate-chain block, and
  one heap object per block child. Strings are in the pool, not in the object.
* `MemoryUsage()` is the heap size of the subtree (vector **capacity**, the optional block, and
  `sizeof(CKeyValue)` for each child plus its own usage). It does **not** include the pool; add
  `KVStringPool::Instance().MemoryUsage()` for that.
* A loaded object is shrunk to fit; an object that you modify may keep extra capacity.

### What invalidates what

| you do | pointers from `GetKeyValue` | `Child` / ranges of that object |
|---|---|---|
| add an entry (`Add*`, `Set*` that appends) | stay valid | invalid |
| `Set*` that replaces a scalar | stay valid | invalid |
| `Set*` / `SetKeyValue` that replaces a block | **that block's pointer dangles**, others valid | invalid |
| `RemoveKey(s)` | removed blocks dangle, others valid | invalid |
| `Clear()`, destroying the object, `LoadFromFile` | all dangle | invalid |
| move from the object | pointers now belong to the new owner | invalid |
| `GetName()` result | always valid | - |
| `View`/`CStr` from the pool | always valid | - |

### Cost of operations

* Lookup: O(1) average above 16 entries, a short scan below. Case folding costs only when the name has a capital
  letter and the exact spelling was not found.
* `Get*`: no string parsing for numbers, no allocation except the returned `std::string` of `GetString`.
* `Set*`/`Add*`: one hash lookup in the pool for the name and one for the value. New distinct strings enlarge the
  pool for good.
* Copying a tree is a full deep copy.

---

## 12. Threads

* **Plain build (default)**: not safe for use from more than one thread at the same time, even on different trees,
  because all trees share one pool.
* **`KV_THREAD_SAFE` build** (CMake option, or the `kv_mt` target): the pool is safe. Several threads may
  load, build and read **different** trees at once. Lookups in the pool take a shared lock, inserts an exclusive
  one, and `View`/`CStr` take no lock. `CKeyValue::SetCondition` and the snapshot taken at the start of a load are
  protected by their own mutex in both builds.
* **One tree is never safe to modify from two threads**, and not safe to read while another thread changes it.
  Reading one tree from several threads while nobody writes is fine.
* The thread-safe pool costs about 1.4-1.7x per operation on one thread (inserting a new string about 1.7x,
  finding an existing one about 1.4x), and it does not scale with more threads for inserts; measured numbers are in the README and in
  `bench/compare/README.md`.

---

## 13. Checklist of surprises

1. A named object **searches** for its root name; it does not adopt the file's root. Use an unnamed
   `CKeyValue kv;` to load a file whose root name you do not know.
2. Copy assignment and move assignment **rename** the target.
3. A moved-from object is empty but keeps its name and flags.
4. `SetString("n", "42")` is an `Int` (the text decides, as when reading a file); a number-looking text cannot be
   stored as a `String`, and it is written without quotes.
5. `GetInt` of a `Float` truncates toward zero; `GetInt` of a `String` is always the default.
6. `Get*` with duplicates sees only the first; `Set*` replaces only the first visible; `RemoveKey` removes only the
   first visible.
7. Keys are lower-cased and case-insensitive; **values keep their case**.
8. A condition on both sides of a value: the **last** wins. A condition after a block's `}` is an error.
9. Hidden entries are invisible to everything, including `Set*`; `Set*` on a name that only exists hidden appends
   a visible entry next to it.
10. Conditions are evaluated at load time and never written back.
11. Without `UsesEscapeSequences(true)` a `"` or a newline in a value is written raw and does not round-trip.
12. `SetKeyValue`/`AddKeyValue` copy the object, rename the copy to the key and give it the parent's flags;
    you keep ownership of your original, and the one in the tree is a different object.
13. `GetKeyValue(name, def)` returns a **new** copy of `def` when the key is missing; the caller must delete it.
14. Any change to an object invalidates its `Children()` ranges.
15. Replacing or removing a block deletes it; pointers to it dangle. Pointers to other blocks survive.
16. The pool never shrinks. Distinct strings you set in a loop stay in memory for the life of the process.
17. A failed load leaves the object empty (name and flags kept), and only prints to `stderr`; there is no error
    object. Check the return value.
18. Only the chosen root of a multi-root file is stored; the others are just checked for balanced braces.
19. `#include` / `#base` are not handled.
20. Pool limits (1024 chunks, 4 GB per string) throw `std::length_error`, also out of `LoadFromFile`.
