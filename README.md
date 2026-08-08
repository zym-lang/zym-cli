<p align="center">
  <h1 align="center">Zym</h1>
  <p align="center"><strong>Control without the ceremony.</strong></p>
  <p align="center"><em>Fast. Simple. Powerful.</em></p>
  <p align="center">
    A modern, high-performance scripting language designed for both standalone use and seamless embedding.
  </p>
</p>

---

Zym is a compact, systems-oriented scripting language that combines the familiarity of high-level syntax with the control of a systems language. It's built for developers who need the agility of a script with the predictability of a compiled language.

This repository contains the **Zym CLI** — a standalone runtime and toolchain built on top of the embeddable [`zym_core`](zym_core/) language library.

Zym itself is the core; the CLI is one host built on it. It does enough to be useful on its own — a native module catalog, capability-gated child VMs, packaging to a standalone binary — and along the way it ends up being a fairly complete picture of what embedding looks like past a hello world, since all of it goes through the same public C API anyone else would use. Its version tracks the core it embeds rather than counting its own releases.

### Familiar Syntax

If you've written JavaScript, Python, or Lua, Zym reads exactly like you'd expect.

```javascript
func fibonacci(n) {
    if (n < 2) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

print(fibonacci(30));
```

```javascript
// Closures, naturally
func makeGreeter(greeting) {
    return func(name) {
        print(greeting + ", " + name + "!");
    };
}

var hello = makeGreeter("Hello");
hello("world");  // Hello, world!
```

```javascript
// Structs and Enums are first-class
struct Point { x, y }

var p = Point(3, 4);
var distance = sqrt(pow(p.x, 2) + pow(p.y, 2));

enum Color { Red, Green, Blue }
var c = Color.Red;
```

## Why Zym?

- **Zero Dependencies** — The entire language and runtime fit in a single, compact binary. No DLLs, no environment variables, no headaches.
- **Instant Distribution** — Compile your scripts into standalone executables, portable bytecode, or self-contained `.zpk` packages with one command.
- **Unlimited Control Flow** — With delimited continuations, you can build fibers, coroutines, generators, and custom schedulers from scratch.
- **Preemptive Execution** — The VM supports instruction-count-based time-slicing. Run untrusted code or build fair multi-tasking systems without cooperative yields.
- **Script-Directed TCO** — Explicitly control tail-call optimization with the `@tco` directive to ensure predictable stack behavior in recursive algorithms.
- **Capability-Gated Natives** — Spawn nested in-process VMs and grant each one only the slice of the native catalog it needs.

## Beyond Scripting

Zym offers features usually reserved for much heavier system languages, accessible through a simple API.

### Delimited Continuations
Build fibers, coroutines, or your own `async`/`await` primitives.

```javascript
// Cooperative fibers from continuations
var tag = Cont.newPrompt("fiber");

func yield() {
    return Cont.capture(tag);
}

func worker(name) {
    print(name + ": step 1");
    yield();
    print(name + ": step 2");
    yield();
    print(name + ": done");
}
```

See [`examples/continuation/`](examples/continuation/) for a full cooperative scheduler, a manual generator, and a preemptive fiber example.

### Stack Control (TCO)
Ensure your recursive algorithms never overflow the stack.

```javascript
@tco aggressive
func sum(n, acc) {
    if (n == 0) return acc;
    return sum(n - 1, acc + n);
}

print(sum(1000000, 0));  // no stack overflow
```

### Nested VMs
Spawn an in-process child VM with a restricted capability set.

```javascript
var vm = Zym.newVM();
vm.registerCliNative("File");     // child sees File
vm.registerCliNative("JSON");     // and JSON
// ... but nothing else from the catalog.

vm.run(untrustedSource);
```

## Single-Binary Distribution

One of Zym's most powerful features is the ability to "pack" your scripts into a standalone executable that requires nothing else to run.

```text
# Pack your script into a single binary
zym main.zym -o my_app

# Distribute my_app — it has the runtime and your code inside.
./my_app
```

You can also pack across platforms by pointing at an alternate runtime:

```text
zym main.zym -o my_app.exe -r windows
```

## Built-in Modules

The CLI ships a curated catalog of native modules that scripts can use directly. Every module is capability-gated: a parent VM decides which slice of the catalog a child VM is allowed to grant onward.

| Area | Modules |
|------|---------|
| I/O & System | `File`, `Dir`, `Path`, `Console`, `Process`, `System`, `Time` |
| Data & Parsing | `Buffer`, `JSON`, `RegEx`, `Hash` |
| Crypto | `Crypto`, `AES` |
| Networking | `IP`, `Sockets`, `TCP`, `UDP`, `TLS`, `DTLS`, `ENet`, `WebSocket` |
| Storage & Packaging | `SQLite`, `Pack` |
| Runtime | `Random`, `Zym` (nested VMs), `print` |

Per-module reference docs live in [`docs/cli/`](docs/cli/). The [`zpk` package format](docs/formats/zpk.md) is documented under [`docs/formats/`](docs/formats/).

## Features

- **Fast & Lightweight** — Low-overhead register VM with instruction-count preemption.
- **Modern Syntax** — Familiar JS/Python-like feel with first-class functions, closures, and modules.
- **Rich Types** — Built-in support for Strings, Lists, Maps, Structs, and Enums.
- **Advanced Control** — Delimited continuations, fibers, and script-directed TCO.
- **Thread-safe VM** — Each instance owns its heap, globals, and execution state.
- **Standalone CLI** — A versatile tool for executing, compiling, and packaging scripts.

## Getting Started

### Installation

Build from source using CMake:

```text
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zym
```

The `zym` executable will be located in the `build` directory.

### Build-time feature flags

Zym's LSP / tooling surface (retained parse trees, symbol table, extended native metadata, stable diagnostic codes) is gated behind compile-time flags so resource-constrained embedders — MCU firmware, size-limited hosts — can strip it out entirely. The default build (`ZYM_ENABLE_LSP_SURFACE=ON`) includes everything; pass `-DZYM_ENABLE_LSP_SURFACE=OFF` to produce a minimum-footprint build:

```text
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZYM_ENABLE_LSP_SURFACE=OFF
cmake --build build --target zym
```

Flags are build-time, not runtime — turning them off removes tooling surface, not language behavior. See [`zym_core/README.md`](zym_core/README.md#build-options) for the full flag table.

### Packaging the Release CLI

The `zym` binary that comes out of CMake is the **engine** — the VM, the natives, the compiler. The user-facing CLI (`-v` / `-h` / `-o` / `-r`, version string, packaging flow, etc.) is itself written in Zym and lives in [`cli/src/cli.zym`](cli/src/cli.zym). The shipped release artifacts are produced by a separate post-build packaging step that bundles that CLI script together with both platform runtimes — this is what enables cross-platform packing out of a single binary.

There are two distinct build configurations of `zym`:

- **Full build** (`RUNTIME_ONLY=OFF`, the default) — includes `full_executor`, can compile `.zym` source. This is what you use to *drive the packager*. An already-released `cli/bin/zym` from a prior build is equivalent and can be used to bootstrap the next one.
- **Runtime-only build** (`RUNTIME_ONLY=ON`) — strips `full_executor` and only takes the bytecode/pack loader path in `main`. The output is emitted directly into `cli/runtimes/` under its platform name (`linux` or `windows.exe`). These are the *stubs* that get embedded inside the released binaries.

The release workflow:

1. **Build the runtime-only stubs for each target platform.** On the host that targets that platform:

   ```text
   # On Linux
   cmake -B build-runtime -DCMAKE_BUILD_TYPE=Release -DRUNTIME_ONLY=ON
   cmake --build build-runtime --target zym
   # Produces cli/runtimes/linux

   # On Windows (or via the mingw toolchain)
   cmake -B build-runtime-win -DCMAKE_BUILD_TYPE=Release -DRUNTIME_ONLY=ON \
         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw64.cmake
   cmake --build build-runtime-win --target zym
   # Produces cli/runtimes/windows.exe
   ```

2. **Run the packager with a full `zym`** once both runtime stubs are in place. The packager is itself a Zym script ([`cli/makeCli.zym`](cli/makeCli.zym)) — it reads `cli/src/cli.zym`, compiles it, and emits the two release binaries:

   ```text
   cd cli
   # Use a full build of zym (RUNTIME_ONLY=OFF) — NOT one of the runtime stubs
   # in runtimes/, since those can't compile .zym source. A previously-released
   # cli/bin/zym works just as well.
   path/to/full/zym makeCli.zym
   # Produces cli/bin/zym     (linux, with the windows runtime bundled inside)
   # Produces cli/bin/zym.exe (windows, with the linux runtime bundled inside)
   ```

`cli/bin/zym` and `cli/bin/zym.exe` are the released artifacts. CMake intentionally does not run this step automatically — it has no way to know when both platform runtime stubs are ready, and it can't host a Zym interpreter to run the packager.

### Running Scripts

Create `hello.zym`:
```javascript
print("Hello, Zym!");
```

Run it directly:
```text
zym hello.zym
```

## CLI Usage

| Command | Description |
|---------|-------------|
| `zym -v`, `--version` | Print ZYM version |
| `zym -h`, `--help` | Print help |
| `zym <file.[zym\|zbc\|zpk]>` | Run a source, bytecode, or package file |
| `zym <file.zym> -o <out.[zbc\|zpk]>` | Compile `.zym` to bytecode or package |
| `zym <file.zbc> -o <out.zpk>` | Pack a `.zbc` into a `.zpk` package |
| `zym <file.[zym\|zbc\|zpk]> -o <out[.exe]>` | **Pack to a standalone executable** |
| `zym <file> -o <out> -r <runtime>` | Use an explicit runtime for cross-platform packing |

## Examples

Working sample scripts live under [`examples/`](examples/):

- [`examples/console/`](examples/console/) — terminal demos: progress bars, spinners, rainbow text, a Matrix rain, a dashboard.
- [`examples/continuation/`](examples/continuation/) — cooperative fibers, manual generators, and preemptive scheduling on top of `Cont`.
- [`examples/fun/`](examples/fun/) — a spinning ASCII donut and a networked terminal pong.
- [`examples/networking/`](examples/networking/) — TCP / UDP / ENet / WebSocket samples.
- [`examples/sqlite/`](examples/sqlite/) — embedded database usage via the `SQLite` native.
- [`examples/lume_lang/`](examples/lume_lang/) — a small toy language implemented in Zym.

## Documentation

Visit **[zym-lang.org](https://zym-lang.org)** for the complete guide.

- **[Getting Started](https://zym-lang.org/getting-started)**
- **[Language Guide](https://zym-lang.org/docs-language.html)**
- **[Embedding Guide](https://zym-lang.org/docs-embedding.html)** (for using `zym_core` in C/C++ projects)

## Project Structure

- `src/` — CLI executor implementation (boot, native bindings, packaging pipeline).
- `zym_core/` — The core language library (compiler, VM, and runtime). Embeddable on its own.
- `cli/` — Helper scripts that drive packaging (`makeCli.zym`) and the bundled per-platform runtimes.
- `docs/` — Per-module reference (`docs/cli/`) and on-disk format specs (`docs/formats/`).
- `examples/` — Runnable sample scripts grouped by topic.
- `tests/` — Conformance tests for the core language (`tests/core/`) and CLI natives (`tests/cli/`).
- `third_party/` — Vendored dependencies (currently just SQLite).

## License

MIT — see [LICENSE](LICENSE). All remaining behavior shall conform thereto.
