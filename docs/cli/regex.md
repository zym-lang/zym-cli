# `RegEx`

Compiled regular expressions with PCRE2 syntax. The global identifier
`RegEx` is a constructor namespace; calling one of its constructors
returns a regex instance whose methods are invoked as `r.method(...)`.
Successful searches yield `RegExMatch` instances with their own method
set.

---

## Conventions

- **Pattern syntax.** Patterns use PCRE2 syntax: lookarounds, named
  groups (`(?<name>...)`), Unicode classes, alternation, quantifiers,
  back-references, and inline flags (`(?i)`, `(?m)`, ...).
- **Strings.** All subjects, replacements, and group strings are Zym
  strings; offsets are byte positions in the UTF-8 representation of the
  subject.
- **Indices.** Group `0` is the whole match. Capturing groups are
  numbered from `1`; negative indices are rejected at runtime as
  invalid.
- **Group lookup.** Methods that take a `name` argument (`string`,
  `start`, `end`) accept either a number (group index) or a string
  (named-capture identifier). Other types raise a runtime error.
- **Assignment aliases.** Plain assignment (`r2 = r1`) makes `r2` refer
  to the *same* compiled pattern as `r1`. Recompiling through one name
  is visible through the other.
- **Errors.** Bad argument types raise a Zym runtime error of the form
  `RegEx.method(args) ...` or `RegExMatch.method(args) ...`. Calling
  `search`, `searchAll`, or `sub` on an uncompiled or invalid pattern
  raises `... pattern is not compiled`.

---

## Construction

| Method | Returns |
| --- | --- |
| `RegEx.create(pattern)` | regex instance compiled from `pattern`, or `null` if the pattern fails to compile |
| `RegEx.empty()` | uncompiled regex instance; call `r.compile(pattern)` before use |

`RegEx.create` is the common entry point. `RegEx.empty` is provided for
the rare case where you want to build an uncompiled instance and decide
the pattern later.

---

## Compilation & state

| Method | Returns | Notes |
| --- | --- | --- |
| `r.isValid()` | bool | `true` when a pattern has been successfully compiled. |
| `r.pattern()` | string | The most recently supplied source pattern, or `""` for an instance that has never been compiled. `clear()` invalidates the compiled state but does not zero this string. |
| `r.compile(pattern)` | bool | Compiles `pattern`; replaces any previous pattern. Returns `true` on success, `false` on a syntax error. |
| `r.clear()` | null | Drops the compiled pattern so the instance becomes invalid. After this call `isValid()` is `false`; `pattern()` still reports the last source string that was supplied. |

---

## Group metadata

| Method | Returns | Notes |
| --- | --- | --- |
| `r.groupCount()` | number | Number of capturing groups (excludes group `0`). `0` for an uncompiled instance. |
| `r.names()` | list | List of named-capture identifiers declared in the pattern, in the order they appear. Unnamed groups are not represented; use `groupCount()` for the total number of capturing groups. |

---

## Matching

| Method | Returns | Notes |
| --- | --- | --- |
| `r.search(subject, offset, end)` | match \| null | Searches `subject` for the first match. Returns a `RegExMatch` instance, or `null` if no match. `offset` and `end` are optional; `end = -1` means end-of-string. |
| `r.searchAll(subject, offset, end)` | list of match | Returns every non-overlapping match. Empty list when nothing matches. |
| `r.sub(subject, replacement, all, offset, end)` | string | Substitutes matches in `subject` with `replacement`. `all` defaults to `false` (replace only the first match). `offset` and `end` are optional. The `replacement` string supports back-references (`\\1`, `\\g<name>`). |

`offset` defaults to `0`; `end` defaults to `-1` (no limit).

---

## `RegExMatch`

Returned by `r.search` / `r.searchAll`. Methods are invoked as
`m.method(...)`.

| Method | Returns | Notes |
| --- | --- | --- |
| `m.subject()` | string | The original subject the match was produced from. |
| `m.groupCount()` | number | Number of capturing groups (excludes group `0`). |
| `m.names()` | map | Map of named-group identifier → group index. |
| `m.strings()` | list | All matched substrings, with the whole match at index `0` followed by each capturing group. Unmatched groups are empty strings. |
| `m.string(name)` | string | Substring for the given group; `name` is a number (index) or a string (named group). |
| `m.start(name)` | number | Byte offset where the group starts. |
| `m.end(name)` | number | Byte offset just past the end of the group. |

---

## Examples

### Find a single match

```zym
var r = RegEx.create("(?<word>\\w+)\\s+(\\d+)")
var m = r.search("hello 42 world 7")
print(m.string(0))        // "hello 42"
print(m.string("word"))   // "hello"
print(m.string(2))        // "42"
print(m.start(0))         // 0
print(m.end(0))           // 8
```

### Iterate every match

```zym
var r = RegEx.create("\\d+")
var hits = r.searchAll("hello 42 world 7")
for (var i = 0; i < length(hits); i = i + 1) {
    print(hits[i].string(0))   // "42", then "7"
}
```

### Substitute

```zym
var r = RegEx.create("\\d+")
print(r.sub("hello 42 world 7", "X", false))  // "hello X world 7"
print(r.sub("hello 42 world 7", "X", true))   // "hello X world X"
```

### Defensive compilation

```zym
var r = RegEx.create("([")
if (r == null) {
    print("invalid pattern")
}
```

`RegEx.create` returns `null` for an invalid pattern; use this rather
than `RegEx.empty()` + `compile()` unless the pattern only becomes known
after construction.
