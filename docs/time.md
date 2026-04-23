# `Time`

Global singleton providing real-time clocks, system datetime queries, parsing/conversion helpers, and timezone information. Registered at VM startup as the global identifier `Time`; all methods are invoked as `Time.method(...)`.

---

## Conventions

- **Numbers.** All numeric values (unix timestamps, ticks, minute offsets, `ms`) are passed and returned as Zym numbers. Timestamps are truncated to `int64` internally.
- **Booleans.** `utc`, `useSpace`, `weekday` are required booleans. Passing a non-bool raises a runtime error.
- **Strings.** Datetime strings follow ISO 8601 (`YYYY-MM-DDTHH:MM:SS` by default, or `YYYY-MM-DD HH:MM:SS` when `useSpace` is true).
- **Datetime maps.** Returned/accepted maps may contain the keys: `year`, `month` (1–12), `day` (1–31), `weekday` (0=Sunday … 6=Saturday), `hour` (0–23), `minute` (0–59), `second` (0–59), `dst` (bool). Date-only and time-only variants return only the relevant subset.
- **Errors.** Bad argument types produce a Zym runtime error of the form `Time.method(args) expects a <type>`.

---

## Methods

### Real-time clocks

| Method | Returns | Notes |
| --- | --- | --- |
| `Time.now()` | number | Unix timestamp from the system clock (seconds, sub-second precision). |
| `Time.clock()` | number | Process CPU time in seconds via `clock() / CLOCKS_PER_SEC`. |
| `Time.ticksMsec()` | number | Monotonic milliseconds since process start. |
| `Time.ticksUsec()` | number | Monotonic microseconds since process start. |
| `Time.sleep(ms)` | null | Blocks the calling thread for `ms` milliseconds (clamped to ≥ 0). |

### System datetime (current time)

| Method | Returns |
| --- | --- |
| `Time.datetime(utc)` | map with `year`, `month`, `day`, `weekday`, `hour`, `minute`, `second`, `dst` |
| `Time.date(utc)` | map with `year`, `month`, `day`, `weekday`, `dst` |
| `Time.timeOfDay(utc)` | map with `hour`, `minute`, `second` |
| `Time.datetimeString(utc, useSpace)` | ISO 8601 datetime string |
| `Time.dateString(utc)` | `YYYY-MM-DD` |
| `Time.timeString(utc)` | `HH:MM:SS` |

`utc = true` returns UTC; `false` returns local time.

### From a unix timestamp

| Method | Returns |
| --- | --- |
| `Time.datetimeFromUnix(ts)` | datetime map |
| `Time.dateFromUnix(ts)` | date map |
| `Time.timeOfDayFromUnix(ts)` | time map |
| `Time.datetimeStringFromUnix(ts, useSpace)` | ISO 8601 datetime string |
| `Time.dateStringFromUnix(ts)` | `YYYY-MM-DD` |
| `Time.timeStringFromUnix(ts)` | `HH:MM:SS` |

`ts` is seconds since the Unix epoch (UTC).

### Parsing / conversion

| Method | Returns |
| --- | --- |
| `Time.unixFromDatetimeString(s)` | unix timestamp (number) |
| `Time.datetimeFromDatetimeString(s, weekday)` | datetime map; `weekday=true` includes the weekday field |
| `Time.unixFromDatetime(map)` | unix timestamp from a datetime map |
| `Time.datetimeStringFromDatetime(map, useSpace)` | ISO 8601 string from a datetime map |

Input maps may carry any subset of the datetime keys; missing fields default to 0 / January / day 1.

### Timezone

| Method | Returns |
| --- | --- |
| `Time.timezone()` | map with `bias` (minutes offset from UTC) and `name` |
| `Time.offsetString(minutes)` | `±HH:MM` formatted offset string |

---

## Example

```zym
let started = Time.ticksMsec()

print "now unix: %n", Time.now()
print "local:    %s", Time.datetimeString(false, true)
print "utc:      %s", Time.datetimeString(true, false)

let dt = Time.datetime(true)
print "year=%n month=%n day=%n", dt.year, dt.month, dt.day

Time.sleep(250)
print "elapsed: %n ms", Time.ticksMsec() - started
```

---

## Notes

- `Time` is a map-shaped singleton; its methods are closures bound at VM startup.
- Datetime map values are limited to booleans, numbers, and strings; any other type surfaces as `null`.
