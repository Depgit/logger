# async-logger

A thread-safe, non-blocking C logging layer built on top of [zlog](https://github.com/HardySimpson/zlog).

It gives you two things zlog doesn't:

1. **An async ring buffer** so application threads never block on file I/O.
2. **A NanoLog-style fast path** so the caller thread skips `vsnprintf` entirely.

Plus a small, hot-reloadable severity filter driven by a plain-text `Log.conf`.

---

## Why this exists

The obvious approach to async logging is: format the message on the caller thread, push it into a queue, let a worker write it out.

That works, but it hides a cost. Profiling showed ~90% of caller-thread CPU was inside `vsnprintf` — parsing the format string and formatting every argument, *before* the queue was even touched.

The fix is a two-step split:

- **Producer:** store the format-string pointer (it's a literal, so it lives in `.rodata`) and serialize the arguments into a small binary blob.
- **Consumer:** reconstruct the string on the worker thread and hand it to zlog.

No `vsnprintf` on the hot path. No format-string parsing. Just a `memcpy` per argument.

Net result on our workload: **~1.5 µs per call → ~70 ns per call**, without touching any call sites.

---

## Architecture

```
   ┌─────────────┐         ┌──────────────────┐        ┌────────────┐
   │  App thread │  ZLOG   │   ring buffer    │  pop   │  worker    │
   │             ├────────►│  (mutex + cond)  ├───────►│  thread    │
   │  ZLOG_NANO  │         │  100 slots       │        │            │
   └─────────────┘         └──────────────────┘        └─────┬──────┘
                                                             │
                                                             ▼
                                                          zlog_*()
                                                             │
                                                             ▼
                                                        file / syslog
```

Three concerns, three layers:

| Layer            | Where it runs   | What it does                                        |
|------------------|-----------------|-----------------------------------------------------|
| Severity filter  | caller + worker | Reads `Log.conf`, decides "log or drop"             |
| Async queue      | shared          | 100-slot ring buffer, drops on overflow             |
| Backend          | worker          | Calls `zlog_debug/info/warn/error/fatal`            |

---

## Two producer paths

### Legacy: `ZLOG(...)` — format on the caller

```c
ZLOG(proc, level, facility, "user %s id=%d failed", name, uid);
```

Straightforward. Runs `vsnprintf` on the caller thread, then enqueues the formatted string. Use this if your format string is not a compile-time literal, or if you're logging unusual types.

Cost: ~800 ns – 3 µs per call, dominated by `vsnprintf`.

### Fast path: `ZLOG_NANO(...)` — format on the worker

```c
ZLOG_NANO(proc, level, facility, "user %s id=%d failed", name, uid);
ZLOG_NANO0(proc, level, facility, "heartbeat ok");   /* zero-arg variant */
```

The macro uses C11 `_Generic` to pick a packer based on each argument's static type, writes a 1-byte type tag + the raw bytes into a 128-byte blob, and enqueues that.

Cost: ~30 – 80 ns per call. Everything else happens on the worker.

**Rule: `fmt` must be a string literal.** The producer stores the pointer; if you pass a stack buffer, it dangles by the time the worker reads it. Undefined behavior. No exceptions.

---

## Quick start

```c
#include "logger.h"

int main(void) {
    if (logger_init("/etc/app/zlog.conf", "Billing") != 0) {
        return 1;
    }

    /* old style, still works */
    ZLOG(7, ACT_LOG_ERROR, 0, "user %s id=%d failed", "alice", 42);

    /* fast path */
    ZLOG_NANO(7, ACT_LOG_ERROR, 0, "user %s id=%d failed", "alice", 42);

    logger_shutdown();
    return 0;
}
```

`logger_init` takes:

- `config_file` — zlog's own config file.
- `category` — the zlog category name. Also used as the prefix in `Log.conf`.

---

## The `Log.conf` filter

A plain-text file, path hardcoded to `./Log.conf`. One rule per line. `#` starts a comment.

```
# category-wide mask: log everything
BillingLog:255

# per-proc override: silence proc 3 in this category
Billing3Log:0
```

Each rule is a **bitmask of enabled severities**:

```
EMERGENCY   = 1
ALERT       = 2
CRITICAL    = 4
ERROR       = 8
WARNING     = 16
NOTICE      = 32
INFORMATION = 64
DEBUG       = 128
```

So `63` = EMERGENCY through NOTICE, no INFO or DEBUG. `255` = everything. `0` = silent.

### Lookup order

For a call `ZLOG(<proc>, <level>, ...)`:

1. Try `<Category><proc>Log` — e.g. `Billing7Log`.
2. If not found, fall back to the category-wide `<Category>Log`.
3. If still not found, the mask defaults to **0 (silent)** — fail-closed.
4. EMERGENCY and CRITICAL always pass if the mask is non-zero.
5. Everything else checks its bit.

### Hot reload

```c
logger_reload_filter_config(NULL);
```

Re-reads `Log.conf`, swaps the table atomically under a mutex, and recomputes the category mask. Already-queued messages are re-checked on the worker side, so lowering the mask to `0` takes effect immediately — no drain needed.

(The current implementation `sleep(1)`s inside the reload, an artifact of the original code. If you're calling this at runtime from a latency-sensitive path, remove that sleep.)

---

## API

```c
int  logger_init(const char *config_file, const char *category);
void logger_shutdown(void);
int  logger_reload_filter_config(const char *category);

/* legacy */
void zlog_write(zlog_category_t *cat, int proc, int level,
                int facility, const char *fmt, ...);

/* fast path producer, called by the ZLOG_NANO macro */
int  nano_log_enqueue(int proc, int level, int facility,
                      const nano_payload_t *payload);
int  nano_should_log(int proc, int level);
```

Severity constants exposed to callers (bit positions match the config format):

```c
ACT_LOG_EMERGENCY    1
ACT_LOG_ALERT        2
ACT_LOG_CRITICAL     4
ACT_LOG_ERROR        8
ACT_LOG_WARNING      16
ACT_LOG_NOTICE       32
ACT_LOG_INFORMATION  64
ACT_LOG_DEBUG        128
```

---

## Behaviour under pressure

| Situation                        | What happens                                        |
|----------------------------------|-----------------------------------------------------|
| Queue full (100 entries)         | Newest message is **dropped**, `dropped++` counter  |
| Filter mask = 0                  | Message never formatted (producer skips it)         |
| Filter mask lowered at runtime   | Queued messages are re-checked and skipped          |
| Worker thread slower than caller | Queue fills, then drops. Callers are never blocked. |
| `logger_shutdown()` called       | Signal worker, drain remaining queue, then join     |

**Backpressure is a policy, not a feature.** Blocking the caller on a full queue is almost always worse than dropping logs. The `dropped` counter is printed on shutdown so you can see how often it happened.

---

## Trade-offs

```
                        vsnprintf        ZLOG_NANO
cost per call           ~1.5 µs          ~70 ns
dominated by            output length    number of args
format string           any pointer      must be a compile-time literal
compile-time safety     full             partial (no structs)
runtime-built fmt       fine             undefined behaviour
long %s strings         fine             silently truncated at 255 bytes
formatting runs on      caller thread    worker thread
32-bit pointer args     fine             over-reads 4 bytes (bug)
```

The one that bites in practice is the format-string rule. Everything else you can live with or work around.

---

## Supported argument types on the fast path

`ZLOG_NANO` uses C11 `_Generic` to dispatch at compile time. Currently supported:

| Type family                           | Encoded as                      |
|---------------------------------------|---------------------------------|
| `int`                                 | `NANO_TYPE_INT` (4B)            |
| `long`, `long long`                   | `NANO_TYPE_LONG` (8B)           |
| `unsigned int`                        | `NANO_TYPE_UINT` (4B)           |
| `unsigned long`, `unsigned long long` | `NANO_TYPE_ULONG` (8B)          |
| `float`, `double`                     | `NANO_TYPE_DOUBLE` (8B)         |
| `char *`, `const char *`              | `NANO_TYPE_STR` (len-prefixed)  |
| `void *`                              | `NANO_TYPE_PTR` (8B)            |
| anything else (struct, union, …)      | falls through to `%p` — **silent** |

If you log an unsupported type, it doesn't crash — it just prints an address. Use the legacy `ZLOG(...)` for those.

---

## Building

Requires zlog's development headers and a C11 compiler.

```
gcc -std=c11 -O2 -Wall -Wextra -pthread logger.c your_app.c -lzlog -o your_app
```

`-Wall -Wextra` is worth keeping on — it flags a few of the `_Generic` pitfalls.

---

## Files

| File       | Purpose                                             |
|------------|-----------------------------------------------------|
| `logger.h` | Public API, macros, payload/macro internals         |
| `logger.c` | Ring buffer, worker thread, filter parsing, decode  |
| `Log.conf` | Severity filter rules (user-provided)               |

---

## Limitations / known issues

Worth knowing before you ship this.

- **Format-string lifetime.** `ZLOG_NANO` stores the pointer. A stack-allocated `fmt` is UB. Only literals are safe.
- **32-bit pointers.** `nano_pack_ptr` memcpys 8 bytes regardless of pointer size. On 32-bit targets this over-reads. Fix: `memcpy(&args[...], &v, sizeof(uintptr_t))` and add a 32-bit tag.
- **4 KB `msg[]` copy per pop.** The queue slot contains a 4 KB text buffer *and* a 128-byte nano blob, so every pop memcpys ~4 KB even for nano entries. A union or a split queue would fix this.
- **`written++` happens outside the mutex** in the worker. Harmless today (only shutdown reads it, after join) but technically a race.
- **No fork() safety.** The worker thread doesn't survive a fork. The mutex does. Don't log from the child.
- **No thread-cancellation safety.** The worker loops until `running = 0` is set.
- **`sleep(1)` in `logger_reload_filter_config`.** Blocks the caller for a full second. Remove if you call it at runtime.

---

## License

Same as the surrounding project. zlog itself is BSD-2-Clause.
