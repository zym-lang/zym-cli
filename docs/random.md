# `Random`

A non-cryptographic pseudo-random number generator. The global identifier
`Random` is a namespace; its single static `Random.create(...)` returns a
stateful instance whose methods are invoked as `r.method(...)`. Each
instance carries its own seed and state, so multiple generators can be
used in parallel without interfering.

For cryptographically-strong random bytes (key material, nonces, salts),
use `Crypto.generateRandomBytes(n)` — *not* this generator.

---

## Conventions

- **Algorithm.** PCG-based generator. Fast, statistically uniform, and
  fully deterministic given a fixed seed/state. Not suitable for
  cryptographic use.
- **Seed vs. state.**
  - The *seed* picks one of `2^64` independent streams; calling
    `setSeed(s)` resets the stream's position to the start of that
    stream.
  - The *state* is the position within the current stream. `getState`
    after some number of draws can be saved and later restored with
    `setState` to resume the same sequence.
- **Numbers.** Seeds and states are unsigned 64-bit integers, but Zym
  numbers are 64-bit doubles — values above `2^53` will lose precision
  when round-tripped through `getSeed` / `getState`. The PCG state is
  always a full 64-bit value, so a `getState`/`setState` round-trip is
  precision-limited: the immediately-next draw matches, but draws
  further into the resumed stream may diverge. For reproducible
  experiments prefer keeping `seed` <= `2^53` and using `setSeed` for
  resumption rather than `setState`.
- **Ranges.**
  - `randf()` produces a number in `[0, 1)`.
  - `randf_range(lo, hi)` produces a number in `[lo, hi]`.
  - `randi_range(lo, hi)` produces an *integer* in `[lo, hi]`
    (inclusive on both ends).
  - `randi()` produces a 32-bit unsigned integer (`0..=2^32-1`),
    returned as a Zym number.
- **`randWeighted`.** Takes a list of non-negative numbers and returns
  the index of the chosen entry, weighted by the values. An empty list,
  or one whose entries sum to 0, returns `-1`.
- **`randomize()`.** Re-seeds from the system entropy source. Useful when
  you want non-deterministic behavior without picking a seed by hand.
- **Errors.** Bad argument types (e.g., passing a string to `randi_range`)
  raise a Zym runtime error of the form `Random.method(args) ...`.

---

## Statics

| Method | Returns | Notes |
| --- | --- | --- |
| `Random.create()` | Random instance | Builds a new generator and randomizes its seed from system entropy. Two consecutive `Random.create()` calls give independent streams. |
| `Random.create(seed)` | Random instance | Builds a new generator and seeds it with `seed`. Two `Random.create(seed)` calls with the same seed produce identical sequences. |

---

## `Random` instance

Returned by `Random.create(...)`. Methods are invoked as `r.method(...)`.

| Method | Returns | Notes |
| --- | --- | --- |
| `r.seed(s)` | null | Alias of `setSeed`. |
| `r.setSeed(s)` | null | Re-seeds the generator. Resets the position within the new stream to the start. |
| `r.getSeed()` | number | The seed currently in use. |
| `r.setState(s)` | null | Sets the *position* within the current stream. Pair with `getState` to save and restore mid-sequence. |
| `r.getState()` | number | The current position within the stream. Changes with every draw. |
| `r.randomize()` | null | Re-seeds from system entropy. Equivalent to `Random.create()` but without allocating a new instance. |
| `r.randi()` | number | Uniform 32-bit unsigned integer in `[0, 2^32)`. |
| `r.randf()` | number | Uniform float in `[0, 1)`. |
| `r.randfRange(lo, hi)` | number | Uniform float in `[lo, hi]`. |
| `r.randfn()` | number | Normal-distributed float, mean `0`, deviation `1`. |
| `r.randfn(mean, deviation)` | number | Normal-distributed float with the given parameters. |
| `r.randiRange(lo, hi)` | number | Uniform integer in `[lo, hi]` (inclusive). |
| `r.randWeighted(weights)` | number | Index of a randomly chosen entry, weighted by `weights`. Returns `-1` if `weights` is empty or sums to 0. |

---

## Examples

### Reproducible sequences

```zym
var a = Random.create(42)
var b = Random.create(42)
print("%v %v", a.randi(), b.randi())   // identical
print("%v %v", a.randf(), b.randf())   // identical
```

### Save and restore mid-sequence

```zym
var r = Random.create(123)
r.randi()
r.randi()
var snapshot = r.getState()
print("%v", r.randi())   // X
r.setState(snapshot)
print("%v", r.randi())   // X again
```

### Weighted choice

```zym
var r = Random.create(7)
var weights = [10.0, 1.0, 1.0]   // first option ~83% of the time
var counts = [0, 0, 0]
for (var i = 0; i < 1000; i = i + 1) {
    var idx = r.randWeighted(weights)
    counts[idx] = counts[idx] + 1
}
print("%v", counts)
```

### One-off non-deterministic draw

```zym
var r = Random.create()
print("%v", r.randiRange(1, 6))   // d6 roll
```

---

## Notes

- **Independent streams.** Two generators with different seeds produce
  uncorrelated sequences; this is the recommended way to give different
  parts of a program their own RNG instead of sharing a single one.
- **Determinism.** Given the same seed and the same number of draws of
  each kind in the same order, the produced values are byte-identical
  across runs and platforms. Mixing `randi`, `randf`, and `randf_range`
  in different orders changes the sequence, so test fixtures should
  always exercise calls in a fixed order.
- **`randomize()` cost.** This call queries the OS entropy source and is
  significantly slower than a normal `randi()` — avoid calling it inside
  hot loops. Seed once at startup, then draw repeatedly.
