[![CodeFactor](https://www.codefactor.io/repository/github/atvirokodosprendimai/arduino-dali/badge)](https://www.codefactor.io/repository/github/atvirokodosprendimai/arduino-dali)

# Arduino DALI — a non-blocking DALI library for Arduino

**arduino-dali** turns an Arduino, ESP32, RP2040 or STM32 board into a **DALI**
(Digital Addressable Lighting Interface) controller. It drives a DALI bus over two
GPIO pins using a hardware timer for the 1200 baud Manchester encoding, so sending and
receiving never block your sketch.

Use it to dim and switch DALI ballasts and LED drivers, scan a bus for devices, assign
short addresses by commissioning, read memory banks, or — with the slave example —
answer as DALI control gear yourself.

- **Non-blocking.** A hardware timer ISR runs the bit-level state machine; `loop()` stays free.
- **Full addressing.** Short addresses, groups, and broadcast.
- **Send and receive.** Commands, direct arc levels, special commands, raw frames, and replies.
- **Commissioning.** Automatic short-address assignment via the DALI binary search.
- **Slave mode.** Answer as control gear, including memory-bank reads.
- **Works with any interface module** — optocoupler pairs, common-ground designs built
  from a resistive divider and a MOSFET, or off-the-shelf transceivers. Transmit and
  receive polarity are set independently, so modules that invert only one direction work
  too.
- **Bring your own timer** with `DALI_NO_TIMER` if the hardware timers are already spoken for.

Jump to: [Install](#install) · [Wiring](#wiring-and-polarity) · [Quick start](#quick-start) ·
[Configuration](#configuration) · [API](#api-reference) · [Return codes](#return-codes) ·
[Scanning](#scanning-and-device-discovery) · [Slave mode](#slave-mode) ·
[Troubleshooting](#troubleshooting)

---

## Supported boards

| Architecture | Status | Timers (`DALI_TIMER`) | Default |
|---|---|---|---|
| AVR (Uno, Nano, Mega, Controllino) | **Verified building** | `1`–`3` | `1` |
| RP2040 | Supported, untested here | `0`–`3` | `0` |
| STM32 | Supported, untested here | see note | `0` |
| ESP32 | **Currently broken** — see [Known limitations](#known-limitations) | `0`–`1` | `0` |
| ESP8266 | **Does not compile** — see [Known limitations](#known-limitations) | `0`–`1` | `0` |

AVR timer availability differs by chip: **Timer 1** exists on every supported AVR,
**Timer 2** is 8-bit and present on ATmega328/Mega, **Timer 3** only on ATmega2560 and
32U4. Timer 0 is never available — `millis()`, `micros()` and `delay()` own it, which is
why the AVR default is 1 rather than 0.

## Install

**Arduino IDE** — Library Manager, or clone into your `libraries/` folder:

```bash
git clone https://github.com/atvirokodosprendimai/arduino-dali.git ~/Documents/Arduino/libraries/arduino-dali
```

**PlatformIO** — in `platformio.ini`. This fork is not published to the PlatformIO
registry, so point `lib_deps` at the repository directly:

```ini
lib_deps =
    https://github.com/atvirokodosprendimai/arduino-dali.git
    khoih-prog/TimerInterrupt_Generic@^1.13.0
```

The only dependency is
[TimerInterrupt_Generic](https://github.com/khoih-prog/TimerInterrupt_Generic). Older
documentation mentioning TimerOne and PinChangeInterrupt is out of date; neither is
needed.

## Wiring and polarity

A DALI bus needs its **own power supply**, typically ~16 V current-limited. The Arduino
does not power the bus. Without it nothing works and every call reports a fault.

Two pins:

| Pin | Requirement |
|---|---|
| `tx_pin` | any output |
| `rx_pin` | **must support interrupts** — on an Uno that is pin 2 or 3 only |

**Any interface module works.** The library makes no assumption about how your hardware
is built — galvanically isolated optocoupler pairs, common-ground designs using a
resistive divider for receive and a MOSFET for transmit, or a commercial DALI transceiver
IC are all fine. What differs between them is only whether each direction inverts the
signal, and `begin()` takes those two polarities separately:

```cpp
Dali.begin(tx_pin, rx_pin, tx_active_low, rx_active_low);
```

Common modules:

| Module | Call |
|---|---|
| Optocoupler pair, both directions inverting — the classic isolated interface | `Dali.begin(tx, rx, true, true)` or the 3-argument `Dali.begin(tx, rx, true)` |
| Common ground: resistive divider on rx, MOSFET shorting the bus on tx | `Dali.begin(tx, rx, true, false)` |
| Transceiver presenting non-inverted logic in both directions | `Dali.begin(tx, rx, false, false)` |
| Anything mixed | set each flag from the table below |

Work out your own from the two stages independently:

| Your hardware | Setting |
|---|---|
| MOSFET or transistor shorting the bus — a high pin pulls the bus down | `tx_active_low = true` |
| Driver where a high pin drives the bus high | `tx_active_low = false` |
| Optocoupler receiver — an idle bus reads **low** at the pin | `rx_active_low = true` |
| Resistive divider — an idle 16 V bus reads **high** at the pin | `rx_active_low = false` |

A MOSFET output with a divider input, a common discrete build, is
`Dali.begin(tx, rx, true, false)`.

The three-argument form applies one flag to both directions and is fine when both
stages match:

```cpp
Dali.begin(tx_pin, rx_pin, true);   // both inverting — the classic opto interface
```

> Getting polarity wrong makes **every call return `-5` forever**: the timer reads the
> bus as permanently pulled down and parks the state machine in `SHORT`. See
> [Troubleshooting](#troubleshooting).

To check polarity, with the bus powered and idle, probe the pins directly:

```cpp
void setup() { Serial.begin(115200); pinMode(2, INPUT); pinMode(3, OUTPUT); }
void loop() {
  digitalWrite(3, LOW);  delay(50); int idle   = digitalRead(2);
  digitalWrite(3, HIGH); delay(50); int driven = digitalRead(2);
  digitalWrite(3, LOW);
  Serial.print("tx LOW -> rx "); Serial.print(idle);
  Serial.print("   tx HIGH -> rx "); Serial.println(driven);
}
```

`idle 0, driven 1` → both inverting. `idle 1, driven 0` → both non-inverting.
`idle == driven` → the transmitter is not moving the bus; check bus power and wiring
before touching polarity.

## Quick start

```cpp
#include <Dali.h>

void setup() {
  Serial.begin(115200);
  Dali.begin(3, 2, true, false);      // tx, rx, tx polarity, rx polarity
}

void loop() {
  Dali.sendArcWait(0, 254);           // short address 0 to full
  delay(2000);
  Dali.sendArcWait(0, 0);             // and off
  delay(2000);
}
```

Arc levels run 0–254; 255 is the MASK value and means "no change".

### Blocking or not

Every send comes in two flavours, and picking the wrong one is the most common source of
confusion.

| | Returns | Use when |
|---|---|---|
| `sendArc`, `sendCmd`, `sendSpecialCmd` | immediately, with `-3` `DALI_SENT` on success | you have other work in `loop()` and will poll `DaliBus.busIsIdle()` |
| `sendArcWait`, `sendCmdWait`, `sendSpecialCmdWait` | after the frame completes, with the device's reply | you want the answer, or simply want it simple |

A frame occupies the bus about **33 ms** — 10.8 ms settling, 13.3 ms of data, 9.2 ms
waiting for a reply. So this prints a stream of `-5` and only occasionally `-3`:

```cpp
void loop() { Serial.println(Dali.sendArc(0, 254)); }   // no delay: mostly DALI_BUSY
```

That is correct behaviour, not a fault. Either use the `*Wait` form, or gate yourself:

```cpp
void loop() {
  if (DaliBus.busIsIdle()) Dali.sendArc(0, 254);
  // other work continues
}
```

## Configuration

| Define | Description | Values | Default |
|---|---|---|---|
| `DALI_TIMER` | Hardware timer instance | `0`–`3` RP2040 · `0`–`1` ESP32/ESP8266 · `1`–`3` AVR | `1` on AVR, `0` elsewhere |
| `DALI_NO_TIMER` | Use no timer; call `DaliBus.timerISR()` yourself at 2398 Hz | — | — |
| `DALI_NO_COMMISSIONING` | Exclude commissioning code to save flash | — | — |
| `DALI_DONT_EXPORT` | Do not create the global `Dali` instance | — | — |
| `DALI_NO_COLLISSION_CHECK` | Skip collision detection — only if you are the sole master | — | — |

> ### These must be build flags, not `#define`s in your sketch
>
> Arduino compiles each library `.cpp` as its own translation unit, **without your
> sketch's macros**. Writing `#define DALI_TIMER 2` at the top of your `.ino` configures
> nothing — the library never sees it. This is the single most common mistake with this
> library.
>
> In **PlatformIO**, use `build_flags`:
>
> ```ini
> build_flags = -D DALI_TIMER=2
> ```
>
> With **arduino-cli**, use `--build-property`:
>
> ```bash
> arduino-cli compile -b arduino:avr:uno \
>   --build-property "compiler.cpp.extra_flags=-DDALI_TIMER=2" your_sketch
> ```
>
> In the **Arduino IDE** there is no per-sketch mechanism on the AVR core — `build_opt.h`
> is not wired into `arduino:avr`. Either accept the defaults, which are chosen to work
> unconfigured, or edit the default in `src/DaliBus.h`.

## API reference

### Setup

```cpp
void begin(byte tx_pin, byte rx_pin, bool active_low = true);
void begin(byte tx_pin, byte rx_pin, bool tx_active_low, bool rx_active_low);
void setCallback(void (*cb)(uint8_t *data, uint8_t bits));   // frame received
void setActivityCallback(void (*cb)());                      // any bus activity, e.g. blink an LED
```

### Direct arc power control

```cpp
daliReturnValue sendArc(byte address, byte value, byte addr_type = DaliAddressTypes::SHORT);
daliReturnValue sendArcWait(byte address, byte value, byte addr_type = DaliAddressTypes::SHORT, byte timeout = 50);
daliReturnValue sendArcBroadcast(byte value);
daliReturnValue sendArcBroadcastWait(byte value, byte timeout = 50);
```

### Commands and queries

```cpp
daliReturnValue sendCmd(byte address, DaliCmd command, byte addr_type = DaliAddressTypes::SHORT);
int             sendCmdWait(byte address, DaliCmd command, byte addr_type = DaliAddressTypes::SHORT, byte timeout = 50);
daliReturnValue sendCmdBroadcast(DaliCmd command);
int             sendCmdBroadcastWait(DaliCmd command, byte timeout = 50);
```

`sendCmdWait` automatically repeats configuration commands (opcodes 33–142) twice, as
DALI requires — call it once, not twice.

`addr_type` is `DaliAddressTypes::SHORT` (0–63) or `DaliAddressTypes::GROUP` (0–15).
Command names live in `DaliCommands.h`: `DaliCmd` for standard commands,
`DaliSpecialCmd` for special commands, `DaliCmdExtendedDT8` for DT8 colour control.

### Special and raw

```cpp
daliReturnValue sendSpecialCmd(DaliSpecialCmd command, byte value = 0);
int             sendSpecialCmdWait(word command, byte value = 0, byte timeout = 50);
int             sendRawWait(const byte *message, uint8_t bits, byte timeout = 50);
```

`bits` is a **bit** count — 16 for a standard frame, 24 or 25 for extended ones.

### Bus level

```cpp
bool DaliBus.busIsIdle();
volatile byte DaliBus.busIdleCount;                 // timer ticks since the last bus edge
daliReturnValue DaliBus.sendRaw(const byte *message, uint8_t bits);
daliReturnValue DaliBus.sendResponse(byte value);   // slave reply, see Slave mode
DaliBus.errorCallback = myHandler;                  // void(daliReturnValue)
```

## Return codes

Several negative values mean success. This trips people up constantly.

| Code | Name | Meaning |
|---|---|---|
| `0`–`255` | — | A query answered with this value |
| `-1` | `DALI_RX_EMPTY` | Sent, nothing replied. **Normal** for arc levels and configuration commands — gear only answers queries |
| `-2` | `DALI_RX_ERROR` | Malformed reply. During a scan, usually two devices sharing one short address |
| `-3` | `DALI_SENT` | Frame queued by a non-blocking send. **Success** |
| `-4` | `DALI_INVALID_PARAMETER` | Bad `bits` or command number — a code error, not a bus fault |
| `-5` | `DALI_BUSY` | Bus not idle. Expected in a tight loop; **permanent means polarity or wiring** |
| `-6` | `DALI_READY_TIMEOUT` | Bus never freed in time. Raise `timeout` — the 50 ms default leaves under 20 ms over a ~33 ms frame |
| `-7` | `DALI_SEND_TIMEOUT` | Transmission did not complete |
| `-8` | `DALI_COLLISION` | Read-back disagreed with what was driven. On a single-master bus this usually means tx polarity is wrong |
| `-9` | `DALI_PULLDOWN` | Bus held low. Bus power, rx polarity, or a genuine short |
| `-10` | `DALI_CANT_BE_HIGH` | Bus high when a low was expected |
| `-11` | `DALI_INVALID_STARTBIT` | Reception failed to sync |
| `-12` | `DALI_ERROR_TIMING` | Received bit timing outside the DALI window |

`timeout` is a `byte`, so 255 ms is the ceiling. 100 ms is a comfortable working value.

## Receiving

Register a callback to see frames from other masters, or to build a slave:

```cpp
void onFrame(uint8_t *data, uint8_t bits) {
  // ISR context. Keep it short — no Serial, no delay, no allocation.
  // 16-bit frame: data[0] = address byte, data[1] = opcode
}

Dali.setCallback(onFrame);
```

The callback runs inside the timer ISR. Anything slow here corrupts the next frame.
Queue what you need and print it from `loop()`.

## Scanning and device discovery

Full guide with working sketches: **[scanner.md](scanner.md)**.

Three different things go by "scanning":

| Goal | Mechanism | Safe? |
|---|---|---|
| Which short addresses respond | query each of 0–63 | read-only |
| Which groups are populated | query each of 0–15 | read-only |
| What is physically on the wire | commissioning binary search | **writes addresses** |

Minimal sweep:

```cpp
for (byte a = 0; a < 64; a++)
  if (Dali.sendCmdWait(a, DaliCmd::QUERY_BALLAST, DaliAddressTypes::SHORT, 100) >= 0) {
    Serial.print(F("device at ")); Serial.println(a);
  }
```

### Automatic short-address assignment

Gear with no short address is invisible to an address sweep. Commissioning finds it and
numbers it:

```cpp
Dali.commission(0, false);                                  // start at 0, renumber everything
while (Dali.commissionState != DaliClass::COMMISSION_OFF)
  Dali.commission_tick();
Serial.println(Dali.nextShortAddress);                      // devices found
```

`commission()` only arms the state machine; `commission_tick()` does the work and must be
called until `commissionState` returns to `COMMISSION_OFF`. It no-ops unless the bus is
idle, so polling it costs nothing. Budget a few seconds per device.

> `commission(0, false)` **erases every existing short address** and renumbers from
> scratch. Use `commission(n, true)` to leave existing gear alone and only address what
> has none, starting at `n`.

`nextShortAddress` is the *next* address to hand out, so on completion it equals the
device count, and assigned addresses are `0 .. nextShortAddress - 1`.

## Slave mode

See **[examples/dali_slave/dali_slave.ino](examples/dali_slave/dali_slave.ino)** for a
device that answers queries and serves memory banks.

A DALI reply must begin 7–22 TE after the master's frame, so `DaliBus.sendResponse()`
must be called **from the receive callback**, in interrupt context. Calling it from
`loop()` misses the window.

```cpp
void onFrame(uint8_t *data, uint8_t bits) {
  if (bits != 16) return;
  if (data[1] == DaliCmd::QUERY_BALLAST)
    DaliBus.sendResponse(0xFF);        // answer now, inside the window
}
```

The memory-bank pattern: the master sets `DTR1` to a bank and `DTR2` to an offset, then
issues `READ MEMORY LOCATION` (opcode 197) repeatedly. Each read answers one byte and
**post-increments `DTR2`**, so the master walks the bank without re-addressing. Bank 0
byte 2 must report your highest implemented bank, or a master has no reason to look
further.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `#error TIMER has invalid value` | `DALI_TIMER` outside the range for your board. On AVR, timer 0 is unavailable |
| `'ITimer1' was not declared` | The AVR timer object was not instantiated. Fixed in this fork; on older versions set `USE_TIMER_n` before including |
| `#error not supported Hardware` | Architecture has no `getBusLevel` implementation — currently ESP8266 |
| `Multiple libraries were found for "Dali.h"` | A second copy of the library in `libraries/`. Delete the stale one |
| **Every call returns `-5`** | Polarity, or no bus power. `getBusLevel` reads low at idle, so the timer parks in `SHORT`, and only a bus edge clears it |
| Mostly `-5`, occasionally `-3` | Normal. You are sending faster than the ~33 ms frame. Use a `*Wait` form or add a delay |
| `-5` under **both** polarity settings | Your hardware inverts only one direction. Use the four-argument `begin()` |
| Queries return `-4` | You are on a version before the `sendCmdWait` bit-count fix. Update |
| `-1` from an arc command | Not an error. Nothing answers arc levels |
| Nothing happens, no errors | Frames are leaving but no gear reacts. Check the bus supply, then confirm a device exists with a broadcast |
| Your sketch's `#define`s do nothing | They never reach the library. See [Configuration](#configuration) |

## Known limitations

- **ESP32 does not currently build.** `TimerInterrupt_Generic` 1.13 uses `TIMER_BASE_CLK`,
  removed in ESP32 core 3.x (ESP-IDF 5). An upstream problem, not fixable here. Use core
  2.x, or another architecture.
- **ESP8266 does not build.** The `getBusLevel` / `setBusLevel` macro chain in
  `DaliBus.h` has no ESP8266 branch, so it falls through to
  `#error not supported Hardware`.
- **RP2040 and STM32 are untested** in this fork. They compile in principle but have not
  been verified on hardware here.
- **Slave reply timing is spec-derived, not scope-verified.** `sendResponse()` starts its
  backward frame 12 timer ticks after the last bus edge, inside the 7–22 TE window, but
  this has not been confirmed against real gear.
- **Examples are inconsistently laid out.** Only `dali_slave` sits in its own folder, so
  the Arduino IDE lists it and not the others.
- **Licensing is inconsistent.** Source headers say LGPL 2.1; the `LICENSE` file is
  GPL v3.

## Documentation

- [scanner.md](scanner.md) — scanning, discovery and commissioning, with full sketches
- [examples/](examples/) — blink, receive, and slave
- [Upstream API docs](https://hubsif.github.io/arduino-dali/) — Doxygen from hubsif's
  original; predates this fork, so it does not cover the four-argument `begin()`,
  `sendResponse()` or the DT8 commands. Regenerate locally with the included `Doxyfile`
  for current output.

## About this fork

Lineage: [hubsif/arduino-dali](https://github.com/hubsif/arduino-dali) →
[thewhobox/arduino-dali](https://github.com/thewhobox/arduino-dali) →
[atvirokodosprendimai/arduino-dali](https://github.com/atvirokodosprendimai/arduino-dali)
(this repository).

From thewhobox:

- Build-time configuration defines (see [Configuration](#configuration))
- RP2040, ESP32, ESP8266, AVR and STM32 support
- Externally driven timer ISR via `DALI_NO_TIMER`
- Broadcast helpers
- DT8 colour-control commands (`DaliCmdExtendedDT8`)
- Receiving DALI commands, plus an activity callback for status LEDs
- Macro-based bus level access to keep ISR time down

Added here:

- Independent tx/rx polarity, for interfaces that invert only one direction
- `DaliBus.sendResponse()` — backward frames, so the library can answer as control gear
- Fixed the AVR build, which failed in every configuration
- Fixed the receive callback dropping the last byte of every frame
- Fixed `sendCmdWait` passing a byte count where bits were expected, which made every
  query return `-4`
- [scanner.md](scanner.md), the slave example, and this README

## Credits and license

Originally by [hubsif](https://github.com/hubsif/arduino-dali), extended by
[thewhobox](https://github.com/thewhobox/arduino-dali), maintained in this fork by
[atvirokodosprendimai](https://github.com/atvirokodosprendimai).

See [LICENSE](LICENSE).
