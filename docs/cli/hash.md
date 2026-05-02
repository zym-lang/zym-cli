# `Hash`

Streaming non-keyed cryptographic hash digests (MD5, SHA-1, SHA-256).
The global identifier `Hash` is a namespace; its statics build either a
stateful `Hash` instance (`Hash.create(algo)`) for streaming use or
return a digest in one call (`Hash.digest(algo, buf)`).

Hashes here are *unkeyed* and produce a fixed-size digest. For keyed
HMAC digests, RSA signing, certificate handling, or random bytes, use
the `Crypto` native instead.

---

## Conventions

- **Algorithms.** `algo` is one of:
  - `"md5"` — 16-byte digest. Broken for collision resistance; safe
    only for non-security uses (file fingerprinting, change detection).
  - `"sha1"` — 20-byte digest. Also broken for collision resistance.
  - `"sha256"` — 32-byte digest. Recommended default for general
    integrity / fingerprinting.

  Strings are matched case-insensitively, so `"SHA256"` works too.
- **Inputs and outputs are Buffers.** `update(buf)` consumes a `Buffer`,
  and `finish()` / `Hash.digest(...)` return a `Buffer` containing the
  raw digest bytes. Use `b.hex()` to format as a lowercase hex string,
  or `b.size()` to inspect length.
- **Streaming lifecycle.** A fresh `Hash.create(algo)` is ready to
  receive `update(...)` calls. After `finish()` the instance is sealed:
  further `update(...)` calls return `false` until `reset()` is called,
  which restarts the same algorithm with empty state.
- **`Hash.digest(algo, buf)`.** Equivalent to creating an instance,
  feeding `buf` once, and returning the result. Preferred when you
  already have the entire input in memory; use the streaming form for
  large or multi-chunk inputs (e.g. file hashing).
- **Errors.** Unknown algorithm strings, missing arguments, or non-
  Buffer inputs raise a Zym runtime error of the form
  `Hash.method(args) ...`. The streaming `update(buf)` returning
  `false` is *not* an error in the runtime sense — it indicates the
  context was already finalized.

---

## Statics

| Method | Returns | Notes |
| --- | --- | --- |
| `Hash.create(algo)` | Hash instance | Builds a new context already started for `algo`. Ready to accept `update(...)` calls. |
| `Hash.digest(algo, buf)` | Buffer | Convenience: returns the digest of `buf` under `algo` in one call. |

---

## `Hash` instance

Returned by `Hash.create(algo)`. Methods are invoked as `h.method(...)`.

| Method | Returns | Notes |
| --- | --- | --- |
| `h.update(buf)` | bool | Feeds `buf` into the digest. Returns `true` on success, `false` if the context has already been `finish()`ed (call `reset()` first). Empty buffers are accepted as a no-op success. |
| `h.finish()` | Buffer | Returns the digest of all bytes fed so far and seals the context. Subsequent `update()` calls return `false` until `reset()` is called. Calling `finish()` again on a sealed context returns an empty `Buffer`. |
| `h.reset()` | bool | Re-initialises the same algorithm with empty state. Returns `true` on success. The instance becomes reusable for another hashing round. |

---

## Examples

### One-shot

```zym
var d = Hash.digest("sha256", Buffer.fromString("hello"))
print(d.hex())
// 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
```

### Streaming

```zym
var h = Hash.create("sha256")
h.update(Buffer.fromString("hel"))
h.update(Buffer.fromString("lo"))
print(h.finish().hex())
// 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
```

### Hashing a large file in chunks

```zym
var f = File.open("/path/to/big.bin", "r")
var h = Hash.create("sha256")
while (true) {
    var chunk = f.readBytes(64 * 1024)
    if (chunk.size() == 0) { break }
    h.update(chunk)
}
f.close()
print(h.finish().hex())
```

### Reusing a context

```zym
var h = Hash.create("sha1")
h.update(Buffer.fromString("first"))
print(h.finish().hex())   // sha1("first")

h.reset()
h.update(Buffer.fromString("second"))
print(h.finish().hex())   // sha1("second")
```

### Comparing digests in constant time

```zym
var trusted = Hash.digest("sha256", Buffer.fromString("expected"))
var actual  = Hash.digest("sha256", Buffer.fromString("expected"))
var c = Crypto.create()
print(c.constantTimeCompare(trusted, actual))   // true
```

---

## Notes

- **Choice of algorithm.** SHA-256 is the right default. MD5 and SHA-1
  are exposed for compatibility with existing checksum files only; do
  not use them in any security context.
- **Empty input.** `Hash.digest(algo, Buffer.fromString(""))` returns
  the well-known "digest of the empty string" for that algorithm
  (e.g. SHA-256 of `""` is
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`).
- **Endianness / encoding.** Digest output is the algorithm's standard
  big-endian byte order. Use `b.hex()` for the lowercase hex
  representation, or `b.size()` to confirm length matches the algorithm
  (16/20/32 bytes for MD5/SHA-1/SHA-256).
