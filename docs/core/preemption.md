# `Preempt`

Global singleton giving a script scheduled interruption of its own execution. Registered at VM startup as the global identifier `Preempt`; all methods are invoked as `Preempt.method(...)`.

A preemption is a **hard yield at an instruction boundary**, not an error. The VM stops between two instructions, runs the callback you registered, and resumes where it left off with the stack, locals, and call frames untouched. Nothing unwinds.

This is the script-visible half. The host embedding the VM has its own preemption surface with strictly more authority — see `docs/embedding/preemption.md` for that side, and for why script cannot reach it.

---

## Conventions

- **Slices are instruction counts, not time.** A slice of `10000` means "after ten thousand more VM instructions", which is deterministic and independent of machine speed. There is no wall-clock scheduling here; see `Time` for that.
- **Ids are numbers.** Every registration returns an id used to address the entry later. `0` is never a valid id. An unknown id is not an error: mutators return `false` and `remaining` returns `-1`.
- **Callbacks take no arguments.** A callback registered with a non-zero arity is never invoked. Give it zero parameters and close over what it needs.
- **The table is small and fixed.** A VM has room for **8** entries total, shared between script and host. Registering when it is full raises a runtime error rather than silently doing nothing.
- **Slices below 1 are clamped to 1.** Registering `0` or a negative slice gives you `1`.
- **Errors.** Bad argument types raise a runtime error naming the method, e.g. `Preempt.every(slice, fn): fn must be a function.`

---

## Methods

### Registering

| Method | Returns | Notes |
| --- | --- | --- |
| `Preempt.every(slice, fn)` | number (id) | Calls `fn` every `slice` instructions, rearming after each call. Raises if the table is full. |
| `Preempt.once(slice, fn)` | number (id) | Calls `fn` once, `slice` instructions from now, then retires the entry. Raises if the table is full. |
| `Preempt.cancel(id)` | bool | Removes an entry. `false` if the id is unknown **or not script-owned**. |

### Tuning and inspection

| Method | Returns | Notes |
| --- | --- | --- |
| `Preempt.setSlice(id, n)` | bool | Changes the interval and **restarts the countdown** — the entry next fires `n` instructions from now, not `n` from when it was registered. `false` if unknown or not script-owned. |
| `Preempt.remaining(id)` | number | Instructions left before this entry fires. `-1` if the id is unknown, which is also what a retired one-shot reports. |
| `Preempt.request(id)` | bool | Makes an entry fire at the next instruction boundary instead of waiting out its countdown. `false` if unknown or not script-owned. |

### Critical sections

| Method | Returns | Notes |
| --- | --- | --- |
| `Preempt.shield(fn)` | whatever `fn` returns | Runs `fn` with every **maskable** entry suppressed. `fn` must take 0 arguments. Suppression lifts when `fn` returns, and entries resume firing. |
| `Preempt.shieldDepth()` | number | How many shields are currently nested. `0` when not inside one. |

---

## What script can and cannot do

Every entry a script registers is **script-owned** and **maskable**. That has two consequences worth stating plainly, because they are the whole point of the split:

- A script **cannot touch host-owned entries.** `cancel`, `setSlice`, and `request` all check ownership and return `false` for an entry the host registered. A script cannot disarm the watchdog that is supervising it.
- A shield only suppresses **maskable** entries, which means only script's own. A host watchdog registered non-maskable fires straight through `Preempt.shield(...)`, mid-critical-section, and there is nothing script can do about that.

`remaining` is deliberately *not* gated. A script may read the countdown of any entry, including host ones. Observing a deadline is harmless; the guarantee is about control, not secrecy.

A shield is also not a way to become uninterruptible. It defers your own callbacks so a short critical section is not re-entered partway through. It does not extend your time budget, and it cannot outlast a host stop.

---

## Example

### A progress ticker that does not thread through the work

The natural use: a long computation that reports progress without the computation itself knowing anything about reporting.

```zym
var processed = 0
var lastReport = 0

var reporter = Preempt.every(200000, func() {
    if (processed != lastReport) {
        print("... %v rows", processed)
        lastReport = processed
    }
})

var i = 0
while (i < 1000000) {
    processed = processed + 1
    i = i + 1
}

Preempt.cancel(reporter)
print("done: %v rows", processed)
```

```
... 22222 rows
... 44444 rows
... 66666 rows
... 88888 rows
(… 41 more …)
done: 1000000 rows
```

The loop contains no reporting logic at all.

Two things to read off that output. The counts are not round numbers, because the slice counts *instructions*, not iterations — the callback lands near the deadline, wherever that falls. And the spacing tells you the loop body costs roughly nine instructions per row: 200000 instructions bought about 22222 rows. That ratio is the only reliable way to size a slice, and it changes with the shape of the work, so measure rather than guess.

---

### Guarding a critical section

A callback that fires halfway through a multi-step update sees a torn intermediate state. Wrap the update in a shield and it runs to completion first.

```zym
var account = { balance: 100, pending: 0 }
var glimpses = []

var watcher = Preempt.every(5000, func() {
    push(glimpses, account.balance + account.pending)
})

func transfer() {
    var i = 0
    while (i < 40000) {
        account.balance = account.balance - 1
        // A callback firing here would observe 99 + 0 = 99, not 100.
        account.pending = account.pending + 1
        i = i + 1
    }
    return account.balance
}

Preempt.shield(transfer)
Preempt.cancel(watcher)

print("shield depth back to %v", Preempt.shieldDepth())
print("observations during the shielded section: %v", length(glimpses))
```

```
shield depth back to 0
observations during the shielded section: 0
```

`Preempt.shield` returns whatever the shielded function returns, so it composes into an expression rather than forcing a temporary.

Keep shielded sections short. While one is up, your own scheduled work is not running, and a shield that never returns is just a program that ignores its own callbacks.

---

### A deadline you can move

`setSlice` restarts the countdown rather than adjusting it in place, which makes "give it more room" a single call.

```zym
var id = Preempt.once(1000000, func() { print("deadline reached") })

print("remaining: %v", Preempt.remaining(id))

Preempt.setSlice(id, 500)     // fires 500 instructions from now
print("after setSlice: %v", Preempt.remaining(id))

var i = 0
while (i < 5000) { i = i + 1 }

print("after firing, remaining reports %v", Preempt.remaining(id))
```

```
remaining: 1000000
after setSlice: 500
deadline reached
after firing, remaining reports -1
```

The final `-1` is the one-shot having retired: once fired, its id is no longer known to the VM. A `Preempt.every` entry would report its full slice again instead.

---

## Notes

- **Preemption is deterministic, not real-time.** Slices count instructions. The same program preempts at the same points on every run and on every machine, which makes it reproducible but means it is not a timer. A tight loop and an allocation-heavy loop cover very different amounts of wall-clock in the same slice.
- **Callbacks are ordinary script.** They allocate, they can raise, and they can be preempted by *other* entries — but not by their own, which is masked while it runs. An entry cannot re-enter itself.
- **One callback runs per expiry.** If several entries come due on the same instruction, the first by registration order runs and the rest keep their refreshed deadlines for a later pass.
- **The table is shared with the host.** Eight entries covers both sides. A script that registers greedily can exhaust it and find its own later registrations failing; the host's entries are unaffected because they were registered first.
- **A shield does not survive a capture.** Continuations record the shield depth at capture time and restore it on resume, so a shielded section resumed later is still shielded, and one captured outside a shield does not inherit one.
- **There is no way to observe being stopped.** A host watchdog or stop aborts execution with no script-visible handler, no diagnostic, and no callback. That asymmetry is deliberate: anything a script could observe, it could stall inside.
