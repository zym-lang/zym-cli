# Process

The `Process` global lets Zym scripts run other programs, capture their output, talk to them through standard I/O, send signals, query and modify the environment, and exit the current process.

It exposes two complementary shapes:

- **One-shot helper** — `Process.exec(...)` runs a command to completion and hands you everything in a single map.
- **Handle-based API** — `Process.spawn(...)` returns a live `Process` instance you can read from, write to, poll, signal, and wait on.

Plus a small set of **process statics** for the current process (cwd, env, pid, exit).

## Conventions

- **Command + args.** All spawn/exec calls take an explicit program path and an optional list of arguments. There is no shell interpretation: `Process.exec("rm", ["-rf", path])` is not the same as `sh -c "rm -rf …"`.
- **Bytes are `Buffer`.** Reads return a `Buffer`; `writeBuffer` accepts one. `write` accepts a string and is a convenience for sending text. See `docs/buffer.md`.
- **Stdio modes.** Each of `stdin`, `stdout`, `stderr` can independently be set to:
  - `"pipe"` (default) — the parent reads/writes through a pipe.
  - `"inherit"` — child shares the parent's terminal/pipe.
  - `"null"` — connected to `/dev/null` (or the Windows equivalent).
  - `"pty"` — child gets a real TTY. On Linux/macOS/BSD this uses `openpty`/`forkpty`; on Windows it uses **ConPTY** (Windows 10 1809+). When any stream is `"pty"`, all three are unified through the pseudo-terminal so `isatty()` is true in the child.
- **Options map.** `spawn` and `exec` accept an optional trailing options map: `{stdin, stdout, stderr, cwd}`. Unknown keys are ignored.
- **Numbers.** PIDs, exit codes, and signal numbers are plain numbers.
- **Booleans.** Returned by predicates such as `isRunning()`.
- **Open-failure vs raise.** `Process.spawn` returns `null` if the spawn fails so scripts can branch with `if (p == null)`. `Process.exec` raises a runtime error on spawn failure; it always returns the result map otherwise. Argument-validation problems and OS errors raise.
- **Signaled exit.** A child terminated by a signal reports `exitCode = 128 + signum` (matches the shell convention).

## Spawning

| Call | Returns | Notes |
|---|---|---|
| `Process.spawn(command, args?, options?)` | Process handle, or `null` on failure | Long-running child you control. `args` is a list of strings; `options` is a map (see Conventions). |
| `Process.exec(command, args?, options?)` | `{stdout: Buffer, stderr: Buffer, exitCode}` | Blocks until the child exits. Closes the child's stdin immediately so commands that read stdin still terminate. Raises if the spawn itself fails. |

## Process statics (this process)

| Method | Returns | Notes |
|---|---|---|
| `Process.getCwd()` | string | Current working directory. |
| `Process.setCwd(path)` | `true` | Changes cwd. Raises on failure. |
| `Process.getEnv(key)` | string or `null` | Reads an environment variable. |
| `Process.setEnv(key, value)` | `true` | Sets an environment variable. Raises on failure. |
| `Process.unsetEnv(key)` | bool | Removes an environment variable. Returns `true` on success. |
| `Process.getEnvAll()` | map of string → string | Snapshot of the current environment. |
| `Process.getPid()` | number | The current process's PID. |
| `Process.getParentPid()` | number, or `null` on Windows | The parent process's PID. |
| `Process.exit(code?)` | does not return | Exits the current Zym process (the running script itself, not a child) immediately with the given integer code (default `0`). Skips all cleanup — see *Notes & gotchas*. |

## Process handle

Returned by `Process.spawn(...)`. Once a handle exists, the child is running (or has already exited).

### Writing to the child

| Method | Returns | Notes |
|---|---|---|
| `p.write(s)` | number of bytes written | Sends the UTF-8 bytes of a string to the child's stdin. Errors if stdin was not piped or has been closed. |
| `p.writeBuffer(buf)` | number of bytes written | Sends raw bytes from a `Buffer` to the child's stdin. |
| `p.closeStdin()` | `true` | Closes stdin. Many tools (e.g. `wc`, `cat`, `sort`) only finish once stdin is closed. |

### Reading from the child

| Method | Returns | Notes |
|---|---|---|
| `p.read()` | `Buffer` | Blocking read of currently available stdout bytes. Returns an empty buffer at EOF. |
| `p.readErr()` | `Buffer` | Same, for stderr. |
| `p.readNonBlock()` | `Buffer` | Drains everything immediately available on stdout without blocking. May return an empty buffer. |
| `p.readErrNonBlock()` | `Buffer` | Same, for stderr. |

### Lifecycle

| Method | Returns | Notes |
|---|---|---|
| `p.kill(signal?)` | `true` on success | Sends `signal` to the child. `signal` may be a name (e.g. `"SIGTERM"`, `"SIGKILL"`, `"SIGINT"`, `"SIGHUP"`, `"SIGQUIT"`, `"SIGUSR1"`, `"SIGUSR2"`, `"SIGSTOP"`, `"SIGCONT"`, `"SIGPIPE"`) or a number. Default is `"SIGTERM"`. On Windows the signal argument is ignored and the process is forcibly terminated. |
| `p.wait()` | exit code (number) | Blocks until the child exits, then returns its exit code (`128 + signum` if signaled). Idempotent once the child has exited. |
| `p.poll()` | exit code, or `null` if still running | Non-blocking check; reaps the child if it has finished. |
| `p.isRunning()` | bool | True until the child has been waited on or polled to completion. |
| `p.getPid()` | number | The child's PID. |
| `p.getExitCode()` | number, or `null` if still running | Last known exit code. |

## Examples

### One-shot capture

```zym
var r = Process.exec("/bin/echo", ["hello", "world"])
print("rc=%n", r.exitCode)
print("out=%s", r.stdout.toUtf8())   // "hello world\n"
```

### Separate stdout and stderr

```zym
var r = Process.exec("/bin/sh", ["-c", "echo out; echo err 1>&2; exit 7"])
print("rc=%n", r.exitCode)               // 7
print("stdout=%s", r.stdout.toUtf8())    // "out\n"
print("stderr=%s", r.stderr.toUtf8())    // "err\n"
```

### Feeding stdin to a child

```zym
var p = Process.spawn("/usr/bin/wc", ["-c"])
p.write("hello, zym")
p.closeStdin()                           // wc waits for EOF before printing
var rc = p.wait()
print("rc=%n", rc)                       // 0
print("count=%s", p.read().toUtf8())     // "10\n"
```

### Streaming output without blocking

```zym
var p = Process.spawn("/bin/sh", ["-c", "for i in 1 2 3; do echo $i; sleep 0.05; done"])
while (p.isRunning()) {
    var chunk = p.readNonBlock()
    if (chunk.size() > 0) {
        Console.write(chunk.toUtf8())
    }
    Time.sleep(10)
}
// Drain anything that arrived between the last read and exit.
var tail = p.read()
if (tail.size() > 0) { Console.write(tail.toUtf8()) }
print("rc=%n", p.wait())
```

### Killing a long-running child

```zym
var p = Process.spawn("/bin/sleep", ["10"])
print("pid=%n running=%v", p.getPid(), p.isRunning())
p.kill("SIGTERM")
print("rc=%n", p.wait())                 // 143  (= 128 + 15)
```

### Customising stdio, cwd, and env

```zym
// Discard stderr, inherit stdout, run in /tmp.
Process.setEnv("ZYM_DEMO", "1")
var r = Process.exec("/bin/sh", ["-c", "echo cwd=$(pwd) demo=$ZYM_DEMO; echo nope 1>&2"], {
    stderr: "null",
    cwd: "/tmp"
})
print("rc=%n", r.exitCode)
print("out=%s", r.stdout.toUtf8())       // "cwd=/tmp demo=1\n"
print("err.size=%n", r.stderr.size())    // 0  (stderr was /dev/null)
```

### Driving an interactive child through a PTY

```zym
// PTY mode unifies stdin/stdout/stderr on a single TTY (Linux/macOS/BSD).
var p = Process.spawn("/usr/bin/python3", ["-q"], { stdin: "pty", stdout: "pty", stderr: "pty" })
p.write("print(2 + 2)\n")
p.write("exit()\n")
print("rc=%n", p.wait())
print("out=%s", p.read().toUtf8())
```

### Querying and editing the current process's environment

```zym
print("pid=%n parent=%v cwd=%s", Process.getPid(), Process.getParentPid(), Process.getCwd())
print("home=%v", Process.getEnv("HOME"))
Process.setEnv("MY_FLAG", "yes")
Process.unsetEnv("MY_FLAG")
```

## Notes & gotchas

- **Always reap.** A handle whose child has exited still has to be `wait()`ed or `poll()`ed for the OS to release the slot. The handle's finalizer kills + reaps any still-running child when it is collected, but doing so by hand is far more deterministic.
- **`exec` closes child stdin for you.** It is meant for "run a command, get output." If your command needs to read from its stdin, use `spawn` and call `write`/`closeStdin` explicitly.
- **No shell.** There is no `sh -c` step. Pass arguments as a list. To use shell features, run `Process.exec("/bin/sh", ["-c", "..."])` (or `cmd.exe /C` on Windows) explicitly.
- **`read()` blocks; `readNonBlock()` does not.** A blocking `read()` after the child has closed its stdout returns an empty buffer (EOF). Use `readNonBlock()` inside loops where you do not want to stall.
- **`kill` defaults to `SIGTERM`.** Pass `"SIGKILL"` (or `9`) for a forced kill on Unix. On Windows the signal name is accepted but the kernel-level effect is always a forced termination.
- **Signaled exit codes.** A child killed by signal `N` reports `exitCode = 128 + N` from `wait()`/`poll()`/`getExitCode()`. This matches the convention POSIX shells use.
- **PTY caveats.** PTY mode is supported on Linux/macOS/BSD (via `openpty`/`forkpty`) and on Windows (via **ConPTY**, requires Windows 10 1809 or newer). When PTY is requested, the three stdio modes are unified onto one TTY — reading stderr separately is not meaningful and will read from the same stream as stdout. On older Windows versions where ConPTY is unavailable, requesting `"pty"` will fail to spawn.
- **Windows differences.** The Windows backend uses `CreateProcess` for plain pipes and `CreatePseudoConsole` (ConPTY) for PTY mode. `getParentPid()` returns `null`. `kill(signal)` always force-terminates regardless of `signal` (Windows has no signal model equivalent to POSIX). Path separators in `cwd` and arguments use backslashes; pass them as literal strings. Argument quoting follows `CreateProcess` rules — there is no shell, so to invoke shell features use `Process.exec("cmd.exe", ["/C", "..."])` (or `powershell.exe`) explicitly.
- **`exec`'s stderr is captured into a `Buffer`, not interleaved with stdout.** If you need ordered, interleaved output, use `spawn` with `stderr: "inherit"` (or `stdout: "inherit"`) and let the OS preserve order on the terminal.
- **`Process.exit` terminates the current Zym process (this script), not a child.** It is the equivalent of the OS-level `_exit` / `ExitProcess` — the running interpreter dies immediately. Finalizers do **not** run, which means open `File` handles are not flushed/closed, child processes spawned via `Process.spawn` are not killed or reaped (they become orphans), and if `Console` was put into raw mode / alt-screen / hidden-cursor mode the terminal is left in that state. The top of the script file is the implicit entry point — so "exit gracefully" simply means letting the script reach its natural end (or returning from the top level) unless a `main(argv)` has been included in which case the end of `main`. The VM's normal teardown path is what runs every finalizer. In-memory data is reclaimed by the OS regardless, so the risk is *side-effect* leakage (un-flushed writes, orphaned children, broken terminal state), not memory. Prefer `Process.exit(code)` only as the last statement of a script after explicit cleanup, or for hard-abort situations where stopping immediately is more important than tidying up.
