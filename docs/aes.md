# `AES`

Symmetric block-cipher encryption and decryption (AES-128 or AES-256,
in ECB or CBC mode). The global identifier `AES` is a namespace of
static helpers; its instance form is built from `AES.create()`.

For asymmetric (RSA) operations, certificate handling, HMAC, or random
bytes, see the `Crypto` native. For non-keyed digests, see `Hash`. For
random keys / IVs, use `Crypto.generateRandomBytes(n)`.

> **AES-CBC is unauthenticated.** A single flipped ciphertext bit
> produces garbage plaintext rather than a clean error — there is no
> built-in integrity check. For confidentiality *and* tamper detection,
> pair it with `Crypto.hmacDigest("sha256", macKey, ciphertext)` (use a
> separate key from the encryption key) and verify the MAC before
> decrypting.

---

## Conventions

- **Modes.** `mode` is one of:
  - `"cbc-encrypt"` / `"cbc-decrypt"` — Cipher Block Chaining. The
    recommended choice. Requires a 16-byte `iv`.
  - `"ecb-encrypt"` / `"ecb-decrypt"` — Electronic Codebook. Identical
    16-byte plaintext blocks produce identical ciphertext blocks, which
    leaks structure; only safe for single-block key wrapping or other
    rare cases. **Do not use for general data.**

  Strings are matched case-insensitively.
- **Keys.** Must be exactly 16 or 32 bytes long, selecting AES-128 or
  AES-256 respectively. (AES-192 is not supported by the underlying
  cipher implementation.) The key is a `Buffer` of bytes, not a
  password — see [Keys are not passwords](#keys-are-not-passwords)
  below.
- **IVs.** CBC modes require a 16-byte initialisation vector. Each
  encryption with the same key MUST use a fresh, unpredictable IV; reuse
  leaks information, and predictable IVs enable replay-style attacks.
  Generate with `Crypto.generateRandomBytes(16)` and prepend or store it
  alongside the ciphertext (the IV is not secret).
- **Buffers.** Keys, IVs, plaintext and ciphertext are all `Buffer`
  instances. See [buffer.md](buffer.md).
- **Padding.** The convenience helpers (`AES.encryptCbc` /
  `AES.decryptCbc`) apply PKCS#7 padding automatically. The instance
  API (`update`) does not — input must be a multiple of 16 bytes, and
  the caller is responsible for padding.
- **Errors.** Invalid key/iv sizes, unknown mode strings, calling
  `update` before `start`, and feeding non-aligned buffers to `update`
  raise a Zym runtime error of the form `AES.method(args) ...`.
  `AES.decryptCbc` returns `null` (not a runtime error) on bad PKCS#7
  padding, ciphertext that is not a positive multiple of 16, or an
  empty input — these are normal "wrong key / corrupt data" cases.

---

## Statics

| Function | Returns |
| --- | --- |
| `AES.create()` | a fresh `AES` instance |
| `AES.encryptCbc(key, iv, plaintextBuf)` | `Buffer` (PKCS#7-padded ciphertext) |
| `AES.decryptCbc(key, iv, ciphertextBuf)` | `Buffer` of plaintext, or `null` on bad padding / wrong size |

---

## Instance methods

| Method | Returns | Notes |
| --- | --- | --- |
| `c.start(mode, key)` | `"ok"` | ECB only — no IV |
| `c.start(mode, key, iv)` | `"ok"` | CBC requires this 3-arg form |
| `c.update(buf)` | `Buffer` | input must be a multiple of 16 bytes |
| `c.ivState()` | `Buffer` (16 bytes) | CBC only; current chaining state |
| `c.finish()` | `"ok"` | clears state; instance can be `start()`'d again |

`start(...)` may be called repeatedly on the same instance with new
modes / keys / IVs to reuse the underlying context.

---

## Examples

### Encrypt and decrypt a short message (recommended)

The convenience helpers handle padding and need only the three byte
buffers (key, IV, payload):

```
var key = Crypto.generateRandomBytes(32)   // AES-256
var iv  = Crypto.generateRandomBytes(16)
var msg = Buffer.fromString("hello world")

var ct = AES.encryptCbc(key, iv, msg)
var pt = AES.decryptCbc(key, iv, ct)
print("%s\n", pt.toUtf8())                  // "hello world"
```

### Encrypt-then-MAC (authenticated)

CBC alone is unauthenticated; pair it with HMAC for tamper detection:

```
var encKey = Crypto.generateRandomBytes(32)
var macKey = Crypto.generateRandomBytes(32) // separate from encKey
var iv     = Crypto.generateRandomBytes(16)

var ct  = AES.encryptCbc(encKey, iv, Buffer.fromString("secret"))
var mac = Crypto.hmacDigest("sha256", macKey, ct)

// Wire format: iv || ct || mac
// On receive: verify mac, then decrypt.
```

### Stream a large input through the instance API

For data larger than memory, feed the cipher in 16-byte multiples
and apply your own padding on the final block:

```
var c = AES.create()
c.start("cbc-encrypt", key, iv)

// Feed N full 16-byte blocks. (Caller is responsible for padding the tail.)
var part1 = c.update(blockBuf1)            // returns 16 bytes of ciphertext
var part2 = c.update(blockBuf2)
// ...
c.finish()
```

`c.ivState()` after each `update` returns the current chaining state, so
you can checkpoint and resume CBC streams across calls.

### NIST SP 800-38A test vector (AES-128-CBC, F.2.1)

```
var key = Buffer.fromHex("2b7e151628aed2a6abf7158809cf4f3c")
var iv  = Buffer.fromHex("000102030405060708090a0b0c0d0e0f")
var pt  = Buffer.fromHex("6bc1bee22e409f96e93d7e117393172a"
                       + "ae2d8a571e03ac9c9eb76fac45af8e51"
                       + "30c81c46a35ce411e5fbc1191a0a52ef"
                       + "f69f2445df4f9b17ad2b417be66c3710")

var c = AES.create()
c.start("cbc-encrypt", key, iv)
var ct = c.update(pt)                       // exact NIST output
c.finish()
```

The instance API does no padding, so feeding the 64-byte aligned NIST
plaintext produces the 64-byte canonical NIST ciphertext.

---

## Keys are not passwords

A user-supplied password is *not* an AES key. Passwords are typically
short, low-entropy, and the wrong shape for direct use. Pass 16 or 32
bytes of high-entropy material (random or derived through a proper
key derivation function — PBKDF2, scrypt, Argon2).

There is no built-in KDF in this native yet. As a stop-gap, hashing a
password with `Hash.digest("sha256", passwordBuf)` produces a 32-byte
buffer that *can* be used as an AES-256 key, but it is **not** as good
as a real KDF — it offers no work factor, no salt by default, and is
trivially brute-forced if the password is weak. Use a real KDF when
available.

---

## What's not included

- **Authenticated encryption** (AES-GCM, AES-CCM). Not exposed by the
  underlying engine class. For now, use CBC + HMAC (encrypt-then-MAC) as
  shown above.
- **Stream / counter modes** (CTR, OFB, CFB). Same reason.
- **Key derivation** (PBKDF2, scrypt, Argon2). See above.

---

## See also

- [`Crypto`](crypto.md) — RSA sign/verify/encrypt, HMAC, X.509, RNG.
- [`Hash`](hash.md) — non-keyed streaming digests.
- [`Buffer`](buffer.md) — byte buffers.
- [`conventions.md`](conventions.md) — return-shape vocabulary.
