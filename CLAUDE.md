# CLAUDE.md — arduino-dali

Guidance for Claude Code working in this repository.

## What this is

A C++ Arduino library implementing the DALI (Digital Addressable Lighting Interface)
protocol. It drives a DALI bus over two GPIO pins, using a hardware timer ISR for the
1200 baud Manchester bit-level state machine.

**This is embedded C++, not Go, and it has no user interface.** Skip Go idiom and UI/UX
tooling; neither applies. The relevant expertise is bit timing, interrupt safety, and
preprocessor-level board portability.

## Build and verify

**There is no test suite.** Verification means compiling, plus native tests of extracted
logic. Read the verification standard below before claiming anything works.

`arduino-cli` is **not on `PATH`**. It ships inside the Arduino IDE app:

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" compile -b arduino:avr:uno --library src <sketch-dir>
```

Quote the path — it contains spaces.

Build flags (`DALI_TIMER` etc.) go through `--build-property`:

```bash
"$CLI" compile -b arduino:avr:mega --library src \
  --build-property "compiler.cpp.extra_flags=-DDALI_TIMER=3" <sketch-dir>
```

A sketch must live in a folder whose name matches the `.ino`. **`examples/dali_blink.ino`
and `examples/dali_receive.ino` are flat files**, so `arduino-cli` cannot compile them in
place — copy each into `<tmp>/dali_blink/dali_blink.ino` first. Only `examples/dali_slave`
is laid out correctly.

Boards installed locally and worth checking against:

| FQBN | Notes |
|---|---|
| `arduino:avr:uno` | primary target |
| `arduino:avr:mega` | the only AVR with timer 3 |
| `CONTROLLINO_Boards:avr:controllino_maxi` | industrial AVR, ATmega2560 |
| `esp32:esp32:esp32` | **expected to fail**, see Known limitations |

No RP2040 core is installed, so RP2040 changes cannot be verified here. Say so rather
than implying coverage.

### Verifying documentation

`README.md` and `scanner.md` contain many fenced `cpp` blocks. When changing either, or
changing an API they use, extract and compile them. A script that pulls every block into
its own sketch folder and builds each is the reliable way; wrap bare fragments in
`setup()`/`loop()` scaffolding. Expect signature listings and placeholder illustrations
(`Dali.begin(tx_pin, rx_pin, ...)`) to fail extraction — check those by eye and say which
blocks you excluded.

## Verification standard

**"It compiles" is not "it works", and this repo has burned that lesson in.** Three real
bugs shipped here, and every one was type-correct code the compiler was happy with:

- `sendCmdWait` passed a byte count where a **bit** count was expected, so every query in
  the library returned `-4` without touching the bus.
- The receive callback used an unsigned offset with `!= 0` guards, so the **last byte of
  every frame** — the opcode — was never written and stayed uninitialised.
- A single `activeLow` flag drove both transmit and receive, so hardware inverting only
  one direction had no valid setting at all.

None of these could be caught by building. When a claim needs proving and hardware is
unavailable, **lift the suspect expression into a native program and print the values**:

```bash
# proved that sendRaw rejects bits=2 and accepts bits=16
c++ -std=c++17 -o /tmp/guard /tmp/guard.cpp && /tmp/guard
```

Ten lines settled in seconds what compiling never could.

State plainly which of "compiles" and "works" you established. **There is no DALI
hardware attached to this machine**, so runtime behaviour can never be verified locally —
say that instead of implying otherwise.

## Domain model

Get these numbers right; most of the subtle bugs here are timing bugs.

- The timer runs at **2398 Hz**, so **one tick = 417 µs = one TE** (the DALI bit half-period).
- `DaliBus.busIdleCount` counts **ticks since the last bus edge**. `pinchangeISR` resets it
  to 0 on every transition; it saturates at 255.
- A **forward frame** occupies the bus roughly **33 ms**: 10.8 ms settling (26 ticks) +
  13.3 ms data (16 bits × 2 half-ticks) + 9.2 ms `WAIT_RX`.
- A **backward frame** (a reply) must *start* **7–22 TE after the forward frame's stop
  condition**. That stop condition ends 2–3 TE after the frame's last edge, so the legal
  window is roughly **ticks 9–25** as measured by `busIdleCount`.
- `TX_START_1ST` therefore waits **26 ticks for a forward frame** but only **12 for a
  backward frame** (`txIsResponse`). Using 26 for a reply answers too late and a compliant
  master has stopped listening.

Consequence for the API: `DaliBus.sendResponse()` **must be called from the receive
callback**, which runs in the timer ISR. From `loop()` it misses the window. Anything in
that callback must be short — no `Serial`, no `delay`, no allocation.

## Architecture

| File | Responsibility |
|---|---|
| `src/DaliBus.h/.cpp` | Bit-level state machine, timer + pin-change ISRs, `sendRaw`, `sendResponse`, bus level macros |
| `src/Dali.cpp/.h` | Frame construction (`prepareCmd`, `prepareSpecialCmd`), blocking wrappers, commissioning state machine |
| `src/DaliCommands.h` | `DaliCmd`, `DaliSpecialCmd`, `DaliCmdExtendedDT8`, `DaliDevTypes`, `DaliAddressTypes` |
| `examples/dali_slave/` | Answering as control gear; DTR1/DTR2 memory banks |
| `scanner.md` | Scanning, discovery, commissioning |

Address byte encoding, mirrored between `prepareCmd` and any slave decoder:

- bit 7 clear → short address `(a >> 1) & 0x3F`; bit 0 is the selector (0 = arc level, 1 = command)
- `0x80`–`0x9F` → group `(a >> 1) & 0x0F`
- `0xA0`–`0xDF` → special command, number `256 + (((a >> 1) & 0x3F) - 16)`
- `0xFE` / `0xFF` → broadcast

`READ MEMORY LOCATION` is opcode **197** and is **not** in `DaliCommands.h`.

## Configuration

**Build flags, never sketch `#define`s.** Arduino compiles each library `.cpp` as its own
translation unit without the sketch's macros, so `#define DALI_TIMER 2` in a `.ino`
configures nothing. `build_opt.h` is also not wired into the `arduino:avr` core. This is
the single most common user error — if someone reports odd configuration behaviour, check
this first.

`DALI_TIMER` valid ranges: RP2040 `0`–`3`, ESP32/ESP8266 `0`–`1`, AVR `1`–`3`. Default is
`1` on AVR and `0` elsewhere. **Timer 0 is never available on AVR** — `millis()`,
`micros()` and `delay()` own it. Timer 1 exists on every AVR; timer 2 is 8-bit; timer 3
only on ATmega2560 and 32U4.

On AVR, `TimerInterrupt_Generic` defines the `ITimerN` objects **and** their `ISR()`s in
the header, so it must be included by exactly one translation unit (`DaliBus.cpp`).
`DaliBus.h` excludes it on AVR only — other architectures rely on its transitive includes
(the ESP32 `GPIO` struct in particular), so do not widen that exclusion.

## Polarity

`txActiveLow` and `rxActiveLow` are independent because interface modules often invert
only one direction. `getBusLevel` reads `rxActiveLow`; `setBusLevel` writes `txActiveLow`.

Diagnostic worth remembering: **if both values of a boolean produce the identical
failure, the boolean is conflating two independent things.** That is exactly how the
single-flag bug presented — every call returned `-5` with `true` *and* with `false`.

## Return codes

Several negatives mean success, and conflating them wastes debugging time:

- `-3` `DALI_SENT` — non-blocking send queued the frame. **Success.**
- `-1` `DALI_RX_EMPTY` — sent, nothing replied. **Normal** for arc levels and config
  commands; gear only answers queries.
- `-5` `DALI_BUSY` — expected in a tight loop (~33 ms per frame). **Permanent** `-5` means
  polarity or bus power, with the state machine parked in `SHORT`.
- `-4` `DALI_INVALID_PARAMETER` — a code error, returned before any bus activity.

Full table in `README.md`.

## Repo conventions

- **Branch before editing.** A `PreToolUse` hook blocks `Edit`/`Write` on `main`. Git
  operations are not blocked. Use `task/<description>`.
- **Commit and push as you go.** A `Stop` hook blocks the turn on uncommitted tracked
  changes or unpushed commits.
- **The `CHANGELOG.md` hook does not converge.** It appends a line per commit, **4–10
  seconds after** the commit lands, so committing that line produces another. Measured, not
  assumed. Terminal state: commit pending work, then `git checkout -- CHANGELOG.md` to
  discard the trailing line. The changelog stays one entry behind `HEAD` by design. The
  real fix is one line in the hook — skip commits whose diff touches only `CHANGELOG.md`.
- **Check async state twice, with a sleep between.** An immediate read produced three
  false "clean tree" conclusions in one session. `sleep 8` then re-check.
- **Untracked tool droppings** — `.serena/`, `scratchpad/`, and claude-mem `CLAUDE.md`
  files written into every directory. Never commit them. `.gitignore` currently contains
  only `*.gz`.
- Repository is `atvirokodosprendimai/arduino-dali`, a fork of `thewhobox/arduino-dali`,
  itself a fork of `hubsif/arduino-dali`. Keep upstream credited.

## Known limitations

- **ESP32 does not build.** `TimerInterrupt_Generic` 1.13 uses `TIMER_BASE_CLK`, removed
  in ESP32 core 3.x (ESP-IDF 5). Upstream problem; not fixable here. Verify with
  `git stash` before attributing an ESP32 failure to a local change.
- **ESP8266 does not build.** The `getBusLevel`/`setBusLevel` macro chain in `DaliBus.h`
  has RP2040, ESP32 and AVR||STM32 branches, then `#else #error not supported Hardware`.
- **RP2040 and STM32 are untested** in this fork.
- **Slave reply timing is spec-derived, never scope-verified.** The 12-tick figure in
  `sendResponse()` has not been confirmed against real gear.
- **Licensing is inconsistent** — source headers say LGPL 2.1, `LICENSE` is GPL v3. A
  maintainer decision; do not silently change it.
- **`examples/` layout is inconsistent** — only `dali_slave` sits in its own folder, so the
  Arduino IDE lists it and not the others.
