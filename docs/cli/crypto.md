# `Crypto`

Cryptographic primitives backed by mbedTLS: random byte generation, RSA
key generation, self-signed X.509 certificates, signing and verification,
asymmetric encryption / decryption, HMAC digests, and constant-time byte
comparisons. The global identifier `Crypto` is a constructor namespace;
calling one of its constructors returns a handle (`Crypto`, `CryptoKey`,
or `X509Certificate`) whose methods are invoked as `h.method(...)`.

---

## Conventions

- **Byte payloads.** All binary inputs and outputs (random bytes,
  hashes, signatures, ciphertexts, keys for HMAC) are exchanged through
  the Zym `Buffer` native. Use
  `Buffer.fromString(...)` to lift script strings, and `b.size()` /
  `b[i]` to inspect the result.
- **Hash type.** Methods that name a hash use a string identifier:
  `"sha256"`, `"sha1"`, or `"md5"`. The lookup is case-insensitive.
  Note that `HMACContext` (used by `hmacDigest`) only supports
  `"sha1"` and `"sha256"` — passing `"md5"` raises a runtime error.
- **PEM strings vs. file paths.** Keys and certificates can be
  serialized either to a PEM string (`saveToString` / `loadFromString`)
  or to a file path (`save` / `load`). The string form is convenient
  when piping through script values; the file form is convenient when
  interoperating with tools that read PEM from disk.
- **`publicOnly` flag.** `CryptoKey.load`, `CryptoKey.save`,
  `CryptoKey.loadFromString`, and `CryptoKey.saveToString` take an
  optional `publicOnly` boolean (default `false`). When `true`, only
  the public component of the key is written or expected; this is the
  form used to verify signatures or perform RSA encryption.
- **Assignment aliases.** Plain assignment (`k2 = k1`) makes `k2` refer
  to the *same* underlying key as `k1`. Loading a new PEM through one
  name is visible through the other.
- **Errors.** Wrong-type arguments raise a Zym runtime error of the
  form `Crypto.method(args) ...`. Operations that the engine handles
  gracefully (e.g. parsing a malformed PEM) return `false` or `null`
  instead of raising; engine-level diagnostic messages may still be
  printed to stderr in those cases.

---

## Construction

| Method | Returns |
| --- | --- |
| `Crypto.create()` | `Crypto` instance backed by mbedTLS, or `null` if the engine refuses to create one |
| `Crypto.CryptoKey()` | empty `CryptoKey` instance ready for `load` / `loadFromString` |
| `Crypto.X509Certificate()` | empty `X509Certificate` instance ready for `load` / `loadFromString` |

`Crypto.create` is the entry point for everything that needs a working
RNG (random bytes, RSA generation, signing, encryption, HMAC). The
`Crypto.CryptoKey` and `Crypto.X509Certificate` constructors are useful
when you want to receive a PEM from another source (file, network, user
input) and parse it.

---

## `Crypto`

### Random bytes

| Method | Returns | Notes |
| --- | --- | --- |
| `c.generateRandomBytes(n)` | Buffer | `n` cryptographically random bytes. `n` must be `>= 0`. |

### RSA keys and certificates

| Method | Returns | Notes |
| --- | --- | --- |
| `c.generateRsa(bits)` | `CryptoKey` \| null | Generates a fresh RSA key pair of size `bits`. Common sizes: `2048` for production, `1024` for tests. Returns `null` on failure. |
| `c.generateSelfSignedCertificate(key, issuer, notBefore, notAfter)` | `X509Certificate` \| null | Issues a self-signed X.509 certificate using `key` (a `CryptoKey` with private material). `issuer` is a distinguished-name string (e.g. `"CN=example"`). `notBefore` and `notAfter` are validity timestamps in `YYYYMMDDHHMMSS` format. |

### Signing and verification

| Method | Returns | Notes |
| --- | --- | --- |
| `c.sign(hashType, hash, key)` | Buffer \| null | Signs the precomputed `hash` (a Buffer) using the private `key`. `hash` must be the exact length expected by the hash algorithm: 32 bytes for `"sha256"`, 20 for `"sha1"`, 16 for `"md5"`. Returns `null` if signing fails. |
| `c.verify(hashType, hash, signature, key)` | bool | Verifies that `signature` was produced for `hash` by the private counterpart of `key`. The public component of `key` is sufficient. |

`sign` and `verify` operate on a *digest*, not on the raw message. Use
`hmacDigest` (with an empty / public key value) or a separate hashing
step to produce the digest before signing.

### Asymmetric encryption

| Method | Returns | Notes |
| --- | --- | --- |
| `c.encrypt(key, plaintext)` | Buffer \| null | RSA-encrypts `plaintext` (a Buffer) using `key`'s public component. The ciphertext is randomized (OAEP), so two encryptions of the same plaintext differ. The plaintext must be small enough for the key size. |
| `c.decrypt(key, ciphertext)` | Buffer \| null | Decrypts `ciphertext` using `key`'s private component. Returns `null` on failure (e.g. wrong key, corrupted ciphertext). |

### HMAC

| Method | Returns | Notes |
| --- | --- | --- |
| `c.hmacDigest(hashType, key, msg)` | Buffer | HMAC of `msg` keyed with `key`, using the named hash. Output is `20` bytes for `"sha1"` and `32` bytes for `"sha256"`. `"md5"` is not supported by the underlying engine. |

### Constant-time comparison

| Method | Returns | Notes |
| --- | --- | --- |
| `c.constantTimeCompare(trusted, received)` | bool | Returns `true` iff `trusted` and `received` are byte-for-byte equal. The implementation does not short-circuit on the first mismatch, making it safe for comparing MACs and tokens against attacker-supplied input. Buffers of different lengths compare `false`. |

---

## `CryptoKey`

Returned by `c.generateRsa(...)`, `Crypto.CryptoKey()`, and indirectly
by `Crypto.create()` paths that yield a key.

| Method | Returns | Notes |
| --- | --- | --- |
| `k.load(path, publicOnly)` | bool | Reads a PEM-encoded key from `path`. `publicOnly` defaults to `false`. Returns `true` on success, `false` if the file cannot be read or parsed. |
| `k.save(path, publicOnly)` | bool | Writes the key to `path` as PEM. With `publicOnly = true` only the public component is written. |
| `k.saveToString(publicOnly)` | string | Returns the PEM representation of the key. With `publicOnly = true` only the public component is included. |
| `k.loadFromString(pem, publicOnly)` | bool | Parses a PEM string. Returns `true` on success, `false` on invalid input. |
| `k.isPublicOnly()` | bool | `true` when only the public component is loaded; `false` when private material is present. |

PEMs round-trip both through file paths and through string values: a key
saved to `pem` and re-parsed via `loadFromString(pem, ...)` produces an
equivalent handle, and re-saving it yields the same PEM.

---

## `X509Certificate`

Returned by `c.generateSelfSignedCertificate(...)` and
`Crypto.X509Certificate()`.

| Method | Returns | Notes |
| --- | --- | --- |
| `x.load(path)` | bool | Parses a PEM-encoded certificate (or chain) from `path`. |
| `x.save(path)` | bool | Writes the certificate to `path` as PEM. |
| `x.saveToString()` | string | Returns the PEM representation of the certificate. |
| `x.loadFromString(pem)` | bool | Parses a PEM string. |

---

## Examples

### Random tokens

```zym
var c = Crypto.create()
var token = c.generateRandomBytes(32)   // Buffer of 32 random bytes
```

### RSA keypair + PEM round-trip

```zym
var c = Crypto.create()
var key = c.generateRsa(2048)
var pem = key.saveToString(false)       // private PEM
var loaded = Crypto.CryptoKey()
loaded.loadFromString(pem, false)       // -> true
```

### Self-signed certificate

```zym
var c = Crypto.create()
var key = c.generateRsa(2048)
var cert = c.generateSelfSignedCertificate(
    key, "CN=zym example", "20230101000000", "20330101000000")
cert.save("/tmp/zym.pem")
```

### Sign / verify a message

```zym
var c = Crypto.create()
var key = c.generateRsa(2048)

// Crypto.sign expects a precomputed digest, not the raw message.
var hashKey = Buffer.fromString("")
var digest = c.hmacDigest("sha256", hashKey, Buffer.fromString("hello"))

var sig = c.sign("sha256", digest, key)
print(c.verify("sha256", digest, sig, key))   // true
```

### HMAC for token comparison

```zym
var c = Crypto.create()
var hmacKey = Buffer.fromString("server-secret")
var expected = c.hmacDigest("sha256", hmacKey, Buffer.fromString("payload"))
var received = c.hmacDigest("sha256", hmacKey, Buffer.fromString("payload"))
if (c.constantTimeCompare(expected, received)) {
    print("token ok")
}
```

### RSA OAEP encrypt / decrypt

```zym
var c = Crypto.create()
var key = c.generateRsa(2048)
var ct = c.encrypt(key, Buffer.fromString("secret payload"))
var pt = c.decrypt(key, ct)
// pt is a Buffer holding the original bytes.
```
