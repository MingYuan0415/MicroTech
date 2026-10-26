# Coding Style

Applies to project-owned code (every source file under `main/`, `tests/`, and
`layers/` except third-party libraries). Rules have three tiers:

- **MUST** — enforced by formatting or the build; a violation fails;
- **MUST NOT** — red line, the behavior is forbidden;
- **SHOULD** — review preference; a justified exception is allowed when stated.

AStyle only handles layout (indentation, spacing, line breaks). It does not cover
naming, Doxygen, logging, or error cleanup; check those by hand. All examples are
taken from real repository code.

## 1. Naming

### 1.1 Directories and Files

[MUST] Directory and file names are all lowercase with words joined by `_`;
avoid generic, high-frequency names. Examples: `bsp_audio.c`, `mt_log.h`,
`app_runtime.c`.

### 1.2 Macros and Constants

[MUST] Macro and enum constant names (including function-like macros) are
UPPER_SNAKE: `TRUE`, `DBG_ERROR`, `LOG_E`.

### 1.3 Types

[MUST] Struct names are lower_snake, and the typedef name is the struct name plus
`_t`, in non-pointer form:

```c
typedef struct system_pm_wake_event
{
    /* ... */
} system_pm_wake_event_t;
```

[MUST NOT] Use pointer typedefs (`typedef struct x *x_t;`); always write pointers
explicitly as `x_t *p`. Repository example: `system_pm_config_t *config`.

### 1.4 Functions

[MUST] Function names are lower_snake; a public API MUST be declared in a header;
an empty parameter list is written as explicit `void`:

```c
esp_err_t bsp_audio_start(void);
```

[MUST] File-static functions take an underscore prefix in the form
`_<module>_<action>`:

```c
static esp_err_t _app_runtime_rtc_alarm_configure(void);
```

[MUST] Object methods are named `<object>_<verb phrase>`, for example
`bsp_audio_start()`, `wifi_service_stop()`,
`app_runtime_pm_build_system_config()`.

## 2. File Organization

### 2.1 Headers

[MUST] Use a unique include guard with `__` on both sides of the identifier:

```c
#ifndef __BSP_AUDIO_H__
#define __BSP_AUDIO_H__
/* ... */
#endif /* __BSP_AUDIO_H__ */
```

[MUST] A header is self-contained: it includes its own dependencies and compiles
on its own. It holds only types, constants, and API declarations — no function
bodies and no static variables. Wrap the content in `extern "C"` (an
`#ifdef __cplusplus` guard) so C++ host tests can include it.

[MUST NOT] Define `DBG_TAG` in a header (see the logging section) or implement a
function body there.

### 2.2 Source Files

[MUST] Define `DBG_TAG` and `DBG_LVL` at the top of a `.c` file before including
`mt_log.h` (see the logging section).

### 2.3 Module Boundaries

- Line count is only a maintainability review signal; about 1000 lines is the
  review threshold. There is no hard cap and it is not a CI failure condition.
- Split by responsibility, state ownership, dependency direction, and independent
  testability. Do not split one state machine, a shared invariant, or a
  reverse-order cleanup chain only to reduce line count, and do not introduce a
  cross-file mutable global or a broad internal API to do so.
- After a split, all of the following MUST hold: single state ownership,
  one-directional dependencies, narrower interfaces. When any condition fails,
  keep the cohesive implementation and organize it with short functions and
  clear sectioning.

### 2.4 Line Width

[SHOULD] Soft limit of 100 columns. Long string literals and Doxygen comments may
exceed it; prefer breaking the line over truncating the meaning.

## 3. Layout

### 3.1 Indentation and Braces

[MUST] 4-space indentation, no tabs; braces on their own line (Allman style):

```c
if (condition)
{
    /* ... */
}
```

[MUST] The only exception is `switch`: `case` aligns with `switch` and the branch
body is indented:

```c
switch (value)
{
case 1:
    break;
default:
    break;
}
```

[MUST] Continuation-line indentation follows AStyle output; do not adjust it by
hand. When a line break occurs inside parentheses, the continuation aligns with
the first character after the opening parenthesis; if that alignment would exceed
column 44, it degrades to the statement indent plus 4 spaces.

[MUST] Never more than two consecutive blank lines.

### 3.2 Spacing

[MUST] Put one space between `if`, `for`, `while`, `switch` and their
parentheses; one space on both sides of a binary operator; no space inside the
parentheses:

```c
if (x <= y)     /* valid */
if ( x <= y )   /* invalid: space inside parentheses */
for (size_t i = 0; i < n; ++i)
```

[MUST] No space before the parenthesis of a function call: `foo(x)`.

[MUST] Increment and decrement use the prefix form without a space: `++i`, never
`i ++`. AStyle's `--pad-oper` does not add a space for `++`.

## 4. Comments

[SHOULD] New code defaults to Chinese; mixing Chinese and English in one project is
not forbidden, but a single file MUST stay consistent.

[MUST] A comment explains what or why, it does not translate the code line by
line. Statement comments go above or to the right of the statement; do not
over-comment, only flag the key points.

[MUST] Public APIs and structs in headers carry Doxygen: at least `@brief`, plus
`@param` and `@return` when there are parameters or a return value; add `@note`,
`@see`, and `@warning` as needed. Document struct members at their definition when
warranted.

A concise function uses a single-line `@brief` (`bsp_audio.h`):

```c
/** @brief Report whether board audio is initialized and may be started. */
bool bsp_audio_is_available(void);
```

The multi-paragraph template for a complex API (`bsp_audio.h`):

```c
/**
 * @brief Change the PCM format while the device is stopped.
 *
 * The sample rate and MCLK ratio must match an ES8311 clock-table entry.
 *
 * @param config is copied by the BSP.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for an unsupported format;
 *         ESP_ERR_INVALID_STATE while running.
 */
esp_err_t bsp_audio_configure(const bsp_audio_config_t *config);
```

Separate the description of each element with a blank line and align the first
column.

## 5. Logging

[MUST] All logging goes through `LOG_E`, `LOG_W`, `LOG_I`, `LOG_D`, and `LOG_V`
from `mt_log.h`. Define `DBG_TAG` at the top of every `.c` file; `DBG_LVL` is
optional and falls back to the Kconfig global default
`CONFIG_MT_LOG_DEFAULT_LEVEL` when undefined — set it explicitly only in files
that need a lower level than the global ceiling (for example the BLE runtime):

```c
#define DBG_TAG "app_runtime"
#include "mt_log.h"
```

- [MUST NOT] Define `DBG_TAG` in a header; it would leak between includers.
- [MUST NOT] Use `printf` for logging; `printf` is only for interactive terminal
  tools.
- [MUST NOT] Print heavily from interrupt context or a hot path; timer and hot-path
  logging MUST be rate-limited or lightweight.
- Keep log messages short and diagnostic; log the same error only once, at the
  most appropriate abstraction layer.

## 6. Error Handling and Cleanup

[MUST] A function that must release resources, release locks, roll back state, or
log centrally funnels into a single `exit` label with `goto`, cleaning up in
reverse order of acquisition or state change, so branches neither duplicate nor
miss cleanup:

```c
static esp_err_t _handle_something(void)
{
    esp_err_t err = ESP_OK;
    bool acquired = false;

    err = acquire_resource();
    if (err != ESP_OK)
    {
        LOG_E("acquire resource fail. %#X", err);
        goto exit;
    }
    acquired = true;

    err = do_something();
    if (err != ESP_OK)
    {
        LOG_E("do something fail. %#X", err);
        goto exit;
    }
    /* further operations are checked the same way */

exit:
    if (acquired)
    {
        release_resource();
    }
    return err;
}
```

[MUST] Parameter validation and ordinary failure branches with no cleanup
obligation return directly; do not introduce `goto` for the formality of a single
return point.

Constraints on the unified exit:

- `err` MUST be the error variable of the current function, already holding the
  failure result before the jump.
- The jump target MUST be an `exit` label inside the current function; never jump
  across a variable-length array scope or to another label — on a name clash use a
  local equivalent label or write the `goto` inline.
- Cleanup inside `exit` runs in reverse acquisition order; when cleanup fails,
  keep the first business error unless the interface explicitly requires returning
  the cleanup error.
- A function with a unified exit lets the success path fall through to `exit`; a
  single return point is not a general requirement.
- When the error comes from an expression, assign it to `err` before testing, so
  the expression is not evaluated twice and the logged code matches the test.
- Log errors with business meaning (the operation name plus the error code or
  `esp_err_to_name`), and log the same error only once, at the most appropriate
  abstraction layer; a failure with no logging obligation propagates to the caller
  unchanged.

## 7. Functions

- [SHOULD] A function accomplishes one relatively independent, simple task; keep
  it small and single-purpose. Reconsider splitting when an implementation grows
  long (for split criteria see 2.3).
- [MUST] Validate parameters first; a failure with no cleanup obligation returns
  directly (see section 6).

## 8. Formatting and Pre-commit Self-check

### 8.1 AStyle

[MUST] The formatting baseline is AStyle 3.6.19, covering project-owned `.c`/`.h`
files. The command is idempotent: files that already comply are not rewritten, so
running it performs both formatting and the self-check without a dry run first:

```sh
astyle --style=allman --indent=spaces=4 --indent-preproc-block --pad-oper \
       --pad-header --unpad-paren --suffix=none --align-pointer=name \
       --lineend=linux --convert-tabs \
       $(git ls-files '*.c' '*.h')
```

Read-only check (no file changes, for CI or review): append
`--dry-run --error-on-changes`.

[MUST] Distribution packages usually lag (Debian/Arch still ship 3.6.12), so build
3.6.19 from upstream and install it:
`cmake -S astyle-3.6.19 -B build -G Ninja && ninja`, producing
`build/AStyle/astyle`. Versions 3.6.15–3.6.18 are not acceptable substitutes; they
regress continuation-line indentation inside `extern "C"` blocks.

[MUST] Exclude build-generated files (such as `main/mmap_generate_*.h`) and
third-party code: ESP-IDF, `managed_components/`, `layers/bsp/XPowersLib/`. Do not
add file-level exclusions or `INDENT-OFF` markers to work around a stable
formatting result.

[MUST] Each directory under `layers/` is an independent Git submodule: the parent
repository's `git ls-files` does not include files inside a submodule, so run the
same command inside that submodule directory.

### 8.2 Pre-commit Flow

After the change, run the following in order, each item once:

1. Format: run the 8.1 command, then `git diff --check`; if formatting produced
   changes, rebuild and run the host tests again;
2. Compile the relevant host test suites and get them fully green (commands in
   AGENTS.md);
3. Manually self-check naming, Doxygen, logging, and cleanup exits against the
   [MUST] items of this document (AStyle does not cover them);
4. For changes touching hardware, power, display, or storage, state the on-device
   verification scope; host tests do not replace board validation.
