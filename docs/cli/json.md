# `JSON`

A standards-compliant JSON encoder/decoder. The global identifier `JSON`
is a namespace; its statics (`stringify`, `parse`, `create`) operate
directly on Zym values, while `JSON.create()` returns a stateful
instance whose methods are invoked as `j.method(...)`.

---

## Conventions

- **Encoding direction.**
  - *Stringify* takes a Zym value and produces a JSON text string.
  - *Parse* takes a JSON text string and produces a Zym value.
- **Type mapping.**

  | Zym | JSON |
  | --- | --- |
  | `null` | `null` |
  | `bool` | `true` / `false` |
  | `number` | JSON number (always written with decimal precision) |
  | `string` | JSON string |
  | `list` | JSON array |
  | `map` | JSON object |

  Zym maps are string-keyed by construction, so encoded objects always
  have string keys. Parsed objects are returned as Zym maps; numeric or
  other non-string keys (which standard JSON cannot produce) would be
  coerced to strings.
- **Numbers.** Zym numbers are doubles; integral values round-trip
  losslessly up to `2^53`. JSON numbers always parse back to Zym
  numbers.
- **Errors.**
  - Static `JSON.parse` returns `null` when the input is not valid JSON;
    it never raises.
  - Instance `j.parse(text)` returns `false` on failure and exposes the
    error via `j.errorLine()` / `j.errorMessage()`.
  - Bad argument types (e.g., passing a number to `parse`) raise a Zym
    runtime error of the form `JSON.method(args) ...`.
- **Encoding limits.** Stringify and `setData` reject values that nest
  deeper than 512 levels with a runtime error.
- **Lenient parsing.** The parser accepts a few non-standard niceties such
  as trailing commas inside arrays and objects. If your data must be
  strictly RFC 8259, validate it before parsing.
- **Unsupported values.** Closures, structs, enums, and other non-JSON
  Zym values raise a runtime error during stringify.

---

## Statics

| Method | Returns | Notes |
| --- | --- | --- |
| `JSON.stringify(value)` | string | Compact JSON text for `value`. Object keys are sorted by default for stable output. |
| `JSON.stringify(value, indent)` | string | Pretty-prints with the given indent string (e.g. `"  "` or `"\t"`). Pass `""` for compact output. |
| `JSON.stringify(value, indent, sortKeys)` | string | When `sortKeys` is `false`, preserves insertion order of map keys. |
| `JSON.stringify(value, indent, sortKeys, fullPrecision)` | string | When `fullPrecision` is `true`, numbers are written with the full IEEE-754 round-trip precision. |
| `JSON.parse(text)` | value \| `null` | Parses `text` as JSON and returns the resulting Zym value. Returns `null` on any parse error. |
| `JSON.create()` | JSON instance | Builds a new stateful parser that retains error info and (optionally) the original text. |

---

## `JSON` instance

Returned by `JSON.create()`. Methods are invoked as `j.method(...)`.

| Method | Returns | Notes |
| --- | --- | --- |
| `j.parse(text)` | bool | Parses `text`. Returns `true` on success and stores the result for `j.data()`. On failure, populates `j.errorLine()` / `j.errorMessage()` and returns `false`. |
| `j.parse(text, keepText)` | bool | Same as above; when `keepText` is `true`, `j.parsedText()` returns the original input. |
| `j.data()` | value | The most recently parsed (or assigned via `setData`) value, or `null` if nothing has been stored. |
| `j.setData(value)` | null | Replaces the stored data. Useful when an instance is being threaded through code that later wants to introspect or stringify it. |
| `j.parsedText()` | string | The original input from the last `parse(text, true)` call, or `""` if not retained. |
| `j.errorLine()` | number | 0-indexed line number of the last parse error (counts newlines crossed before the offending token), or `0` after a successful parse. Errors on the first line therefore also report `0`. |
| `j.errorMessage()` | string | Human-readable error message, or `""` after a successful parse. |

Instance state persists across calls: a successful `parse` clears the
error fields, and a failing `parse` does not overwrite the previously
stored `data`.

---

## Examples

### One-shot encode / decode

```zym
var text = JSON.stringify({"name": "ada", "skills": ["math", "code"]})
print(text)   // {"name":"ada","skills":["math","code"]}

var v = JSON.parse(text)
print(v["name"])         // ada
print(v["skills"][1])    // code
```

### Pretty-print

```zym
print(JSON.stringify({"a": 1, "b": [true, null]}, "  "))
// {
//   "a": 1.0,
//   "b": [
//     true,
//     null
//   ]
// }
```

### Defensive parse

```zym
var v = JSON.parse("{not valid}")
if (v == null) {
    print("bad input")
}
```

### Detailed errors with an instance

```zym
var j = JSON.create()
if (j.parse("{\"k\":[1,2,]}", true) == false) {
    print(j.errorLine())     // line of the offending token
    print(j.errorMessage())  // e.g. "Expected value, got ',' "
    print(j.parsedText())    // the original input
}
```

### Round-trip a nested map

```zym
var original = {
    "items": [{"id": 1, "name": "a"}, {"id": 2, "name": "b"}],
    "count": 2
}
var txt = JSON.stringify(original)
var back = JSON.parse(txt)
print(back["items"][1]["name"])   // b
print(back["count"])              // 2
```
