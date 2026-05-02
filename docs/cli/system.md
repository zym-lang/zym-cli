# `System`

A namespace exposing properties of the host machine and the running
process: OS / device identity, hardware info, well-known directories,
blocking sleeps, and live process-environment manipulation. Registered
at VM startup as the global identifier `System`; all methods are invoked
as `System.method(...)`.

For per-process subprocess control (spawning, piping stdin/stdout,
killing, etc.) see `docs/process.md`. `System.setEnv` / `System.unsetEnv`
mutate the current process's environment, and any process subsequently
launched through `Process.spawn` / `Process.exec` inherits the updated
environment automatically — no plumbing is required to pass values from
the script down into a child.

---

## Conventions

- **Strings.** All string-returning methods produce UTF-8. On hosts that
  cannot answer a query (e.g., `osVersion()` on a stripped-down
  container) the call returns an empty string rather than `null`.
- **Numbers.** `cpuCount()` returns a Zym number (integer-valued).
- **Booleans.** `hasFeature` / `hasEnv` return booleans.
- **Errors.** Bad argument types raise a Zym runtime error of the form
  `System.method(args): argument must be a <type>`. `systemDir(name)`
  raises a runtime error if `name` is not one of the supported
  desktop-only kinds.

---

## Identity

| Method | Returns | Notes |
| --- | --- | --- |
| `System.osName()` | string | Family name. Examples: `"Linux"`, `"macOS"`, `"Windows"`. |
| `System.distribution()` | string | Distribution / OS friendly name. Examples: `"Ubuntu 25.10"`, `"macOS 14.6"`, `"Windows 11"`. Empty if the platform does not expose one. |
| `System.osVersion()` | string | Version string for the OS. Format is platform-defined. |
| `System.modelName()` | string | Device/model identifier as reported by the host. Desktops typically return `"GenericDevice"`. |

## Hardware

| Method | Returns | Notes |
| --- | --- | --- |
| `System.cpuName()` | string | Human-readable CPU brand string (e.g., `"AMD Ryzen 9 5980HX with Radeon Graphics"`). |
| `System.cpuCount()` | number | Number of logical CPU cores available to the process. |
| `System.uniqueId()` | string | Stable per-machine identifier. Suitable for non-secret machine fingerprinting; not suitable as a secret. |

## Locale and features

| Method | Returns | Notes |
| --- | --- | --- |
| `System.locale()` | string | Full BCP-47-ish locale tag, e.g., `"en_US"`. |
| `System.localeLanguage()` | string | Language portion only, e.g., `"en"`. |
| `System.hasFeature(name)` | bool | Tests for a runtime feature tag (e.g., `"pc"`, `"linuxbsd"`, `"x86_64"`, `"64"`, `"debug"`/`"release"`). |

## Process info

| Method | Returns | Notes |
| --- | --- | --- |
| `System.executablePath()` | string | Absolute path of the currently running `zym` binary. |

---

## Directories

All directory methods return absolute paths as strings. Paths are not
guaranteed to exist on disk (callers should create them before writing).

| Method | Returns | Notes |
| --- | --- | --- |
| `System.dataDir()` | string | Per-user, non-project-specific data directory. Linux: `$XDG_DATA_HOME` or `~/.local/share`. macOS: `~/Library/Application Support`. Windows: `%APPDATA%`. |
| `System.configDir()` | string | Per-user config directory. Linux: `$XDG_CONFIG_HOME` or `~/.config`. |
| `System.cacheDir()` | string | Per-user cache directory. Linux: `$XDG_CACHE_HOME` or `~/.cache`. |
| `System.tempDir()` | string | System temp directory (`/tmp` on Linux/macOS, `%TEMP%` on Windows). |
| `System.systemDir(name)` | string | Returns a well-known user folder. `name` is case-insensitive and accepts only desktop-relevant kinds: `"desktop"`, `"documents"`, `"downloads"`, `"movies"`, `"music"`, `"pictures"`. Any other value raises a runtime error. |

`System.dataDir()` returns the *non-project-specific* user data
location. There is intentionally no equivalent for the per-application
"user data" directory, since `zym` is a general-purpose runtime rather
than a single project; callers that want a per-app directory should
nest under `dataDir()` themselves.

---

## Sleep

| Method | Returns | Notes |
| --- | --- | --- |
| `System.sleep(ms)` | null | Blocks the calling thread for `ms` milliseconds. Negative values are clamped to `0`. |
| `System.sleepUsec(usec)` | null | Same, in microseconds. Negative values are clamped to `0`. |

---

## Environment

The environment methods read and write the current process's
environment. Changes take effect immediately and propagate to any
subsequent child process spawned via `Process.spawn` / `Process.exec`,
because those inherit the parent's live environment by default.

| Method | Returns | Notes |
| --- | --- | --- |
| `System.getEnv(name)` | string \| null | The value of `name`, or `null` if the variable is not set. |
| `System.hasEnv(name)` | bool | `true` iff `name` is currently set. |
| `System.setEnv(name, value)` | null | Sets `name` to `value`, replacing any prior value. |
| `System.unsetEnv(name)` | null | Removes `name` from the environment. No-op if it was not set. |

`Process` also exposes `Process.getEnv` / `Process.setEnv` /
`Process.unsetEnv`. They operate on the same underlying process
environment as the `System` versions — calling either is equivalent;
use whichever module reads more naturally at the call site.

---

## Examples

### Identifying the host

```zym
print("%s %s on %s", System.osName(), System.osVersion(), System.cpuName())
print("%n cores, locale %s", System.cpuCount(), System.locale())
```

### Writing to the user's data directory

```zym
var dir = System.dataDir() + "/my-tool"
Dir.makeRecursive(dir)
File.writeAllText(dir + "/config.json", "{}")
```

### Sleep

```zym
var t0 = Time.ticksMsec()
System.sleep(250)
print("waited %n ms", Time.ticksMsec() - t0)
```

### Passing environment values to a child process

```zym
System.setEnv("MY_TOOL_LOG", "debug")
var r = Process.exec("/usr/local/bin/my-tool", ["--check"])
print("exit=%n", r.exitCode)
System.unsetEnv("MY_TOOL_LOG")
```

### Per-user well-known folders

```zym
print("downloads: %s", System.systemDir("downloads"))
print("documents: %s", System.systemDir("documents"))
```

---

## Notes

- **Desktop-only `systemDir`.** Only desktop-meaningful folders are
  accepted.
- **`dataDir` vs. user-data.** `System` exposes only the broader,
  non-project-specific data directory; there is intentionally no
  equivalent for any per-application "user data" location. If you need
  per-app isolation, append your own application-name segment to the
  value of `dataDir()`.
- **Environment thread-safety.** `setenv` / `unsetenv` are not
  thread-safe with respect to concurrent reads; since zym is
  single-threaded this is not a concern from script, but keep it in
  mind if you're embedding zym alongside multi-threaded native code.
