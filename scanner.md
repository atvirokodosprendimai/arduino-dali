# Scanning a DALI bus

How to enumerate everything reachable on the bus: which short addresses answer, which
groups are populated, what each device is, and how to find gear that has no address yet.

All sketches here are complete and compile against this library on AVR.

---

## Before you scan

A scan is just a lot of queries, so a broken bus produces a convincing-looking empty
result rather than an error. Confirm these first:

1. **The bus is powered.** DALI needs its own ~16 V supply. Unpowered, every query
   times out and the scan reports zero devices.
2. **Polarity is right.** See [Polarity](#polarity) below. Wrong polarity traps the bus
   in `SHORT` and every call returns `-5`.
3. **`rx_pin` supports interrupts.** On an Uno that is pin 2 or 3 only.

One-line smoke test — if the lamps blink, the bus works:

```cpp
Dali.sendArcBroadcastWait(254, 100);   // everything to full
delay(1000);
Dali.sendArcBroadcastWait(0, 100);     // everything off
```

---

## Three different things called "scanning"

| Goal | Mechanism | Finds |
|---|---|---|
| Which short addresses respond | query each of 0–63 | gear that is **already** addressed |
| Which groups are in use | query each of 0–15 | group membership |
| What is physically on the wire | commissioning / binary search | gear with **no** address, or duplicates |

The first is cheap, read-only and safe to run any time. The third **writes short
addresses** and is destructive — read [Discovering unaddressed gear](#discovering-unaddressed-gear)
before running it.

---

## 1. Scan short addresses 0–63

Send `QUERY_BALLAST` (DALI "query control gear present") to each address. Gear that
exists answers; nothing else does.

```cpp
#include <Dali.h>

const byte TX = 3, RX = 2;
const byte T  = 100;            // ms per query; a frame needs ~33ms

void setup() {
  Serial.begin(115200);
  Dali.begin(TX, RX, true, false);   // adjust polarity for your interface
  delay(200);

  Serial.println(F("scanning short addresses 0-63..."));
  byte found = 0;

  for (byte a = 0; a < 64; a++) {
    int r = Dali.sendCmdWait(a, DaliCmd::QUERY_BALLAST, DaliAddressTypes::SHORT, T);

    if (r >= 0) {                       // any 0..255 means somebody answered
      found++;
      Serial.print(F("  addr "));
      Serial.print(a);
      Serial.print(F("  present (0x"));
      Serial.print(r, HEX);
      Serial.println(F(")"));
    } else if (r == DALI_RX_ERROR) {    // garbled: likely two devices sharing an address
      Serial.print(F("  addr "));
      Serial.print(a);
      Serial.println(F("  COLLISION - duplicate short address?"));
    }
    // DALI_RX_EMPTY (-1) = nobody home, stay quiet
  }

  Serial.print(F("done, "));
  Serial.print(found);
  Serial.println(F(" device(s)"));
}

void loop() {}
```

### Reading the result

| Return | Meaning |
|---|---|
| `0`–`255` | A device answered. **Present.** |
| `-1` `DALI_RX_EMPTY` | No answer. Address is free. |
| `-2` `DALI_RX_ERROR` | Malformed reply — usually two devices on the same short address answering together, or noise |
| `-5` `DALI_BUSY` | Bus never went idle. Polarity or wiring, not addressing |
| `-6` `DALI_READY_TIMEOUT` | Raise `T`. The default 50 ms leaves under 20 ms of margin over a ~33 ms frame |

A whole sweep takes roughly 2–4 s: 64 queries, each a ~13.3 ms forward frame plus the
reply and settling time.

`DALI_RX_ERROR` on a handful of addresses is worth chasing — duplicate short addresses
are common on a bus that was hand-configured, and they make every later command
ambiguous. Commissioning fixes them.

---

## 2. Scan groups 0–15

Group addressing is a parallel space: a device can be in any of 16 groups, and a group
query is answered by *every* member at once. With more than one member the replies
collide, so treat any answer — clean or garbled — as "this group is populated".

```cpp
Serial.println(F("scanning groups 0-15..."));
for (byte g = 0; g < 16; g++) {
  int r = Dali.sendCmdWait(g, DaliCmd::QUERY_BALLAST, DaliAddressTypes::GROUP, T);
  if (r >= 0)
    Serial.print(F("  group ")), Serial.print(g), Serial.println(F("  in use (single member)"));
  else if (r == DALI_RX_ERROR)
    Serial.print(F("  group ")), Serial.print(g), Serial.println(F("  in use (multiple members)"));
}
```

To get exact membership, ask each device instead — that reply is unambiguous:

```cpp
int lo = Dali.sendCmdWait(a, DaliCmd::QUERY_GROUPS_0_7,  DaliAddressTypes::SHORT, T);
int hi = Dali.sendCmdWait(a, DaliCmd::QUERY_GROUPS_8_15, DaliAddressTypes::SHORT, T);
// bit n of lo -> group n,  bit n of hi -> group n+8
```

---

## 3. Profile each device you found

Once an address answers, these queries fill in the details. All take
`DaliAddressTypes::SHORT`.

| Query | Returns |
|---|---|
| `QUERY_STATUS` | status byte, see table below |
| `QUERY_DEVICE_TYPE` | `0` fluorescent, `1` emergency, `2` discharge, `3` halogen, `4` incandescent, `5` DC converter, `6` LED module, `7` switch, `8` colour control, `9` sequencer, `10` optical control |
| `QUERY_VERSION` | DALI version number |
| `QUERY_ACTUAL_LEVEL` | current arc level 0–254 |
| `QUERY_MIN_LEVEL` / `QUERY_MAX_LEVEL` | configured dimming limits |
| `QUERY_PHYS_MIN` | lowest level the hardware can physically do |
| `QUERY_POWER_ON_LEVEL` | level adopted at power-up |
| `QUERY_FAIL_LEVEL` | level adopted on bus failure |
| `QUERY_GROUPS_0_7` / `QUERY_GROUPS_8_15` | group membership bitmasks |
| `QUERY_ADDRH` / `QUERY_ADDRM` / `QUERY_ADDRL` | the 24-bit random address, unique per device |

`QUERY_STATUS` bits:

| Bit | Set means |
|---|---|
| 0 | control gear failure |
| 1 | lamp failure |
| 2 | lamp is on (arc power on) |
| 3 | limit error — requested level outside min/max |
| 4 | fade in progress |
| 5 | device is in reset state |
| 6 | **no short address assigned** |
| 7 | power cycle seen since last query |

A full inventory pass:

```cpp
#include <Dali.h>

const byte TX = 3, RX = 2;
const byte T  = 100;

void describe(byte a) {
  int status = Dali.sendCmdWait(a, DaliCmd::QUERY_STATUS,       DaliAddressTypes::SHORT, T);
  int type   = Dali.sendCmdWait(a, DaliCmd::QUERY_DEVICE_TYPE,  DaliAddressTypes::SHORT, T);
  int level  = Dali.sendCmdWait(a, DaliCmd::QUERY_ACTUAL_LEVEL, DaliAddressTypes::SHORT, T);
  int lo     = Dali.sendCmdWait(a, DaliCmd::QUERY_GROUPS_0_7,   DaliAddressTypes::SHORT, T);
  int hi     = Dali.sendCmdWait(a, DaliCmd::QUERY_GROUPS_8_15,  DaliAddressTypes::SHORT, T);

  Serial.print(F("addr ")); Serial.print(a);
  Serial.print(F("  type ")); Serial.print(type);
  Serial.print(F("  level ")); Serial.print(level);
  Serial.print(F("  status 0x")); Serial.print(status, HEX);

  if (status >= 0) {
    if (status & 0x01) Serial.print(F(" [GEAR FAIL]"));
    if (status & 0x02) Serial.print(F(" [LAMP FAIL]"));
    if (status & 0x04) Serial.print(F(" [on]"));
    if (status & 0x40) Serial.print(F(" [NO SHORT ADDR]"));
  }

  Serial.print(F("  groups"));
  for (byte g = 0; g < 8;  g++) if (lo >= 0 && (lo & (1 << g))) Serial.print(g),     Serial.print(' ');
  for (byte g = 0; g < 8;  g++) if (hi >= 0 && (hi & (1 << g))) Serial.print(g + 8), Serial.print(' ');
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  Dali.begin(TX, RX, true, false);
  delay(200);

  for (byte a = 0; a < 64; a++)
    if (Dali.sendCmdWait(a, DaliCmd::QUERY_BALLAST, DaliAddressTypes::SHORT, T) >= 0)
      describe(a);
}

void loop() {}
```

### Identifying which physical lamp is which

Address scanning tells you an address exists, not where it is. To find the fixture,
blink it:

```cpp
for (byte i = 0; i < 6; i++) {
  Dali.sendArcWait(a, 254, DaliAddressTypes::SHORT, 100);
  delay(400);
  Dali.sendArcWait(a, 0,   DaliAddressTypes::SHORT, 100);
  delay(400);
}
```

DALI-2 gear also supports `DaliCmd::IDENTIFY`, which triggers the manufacturer's own
identification behaviour.

---

## Discovering unaddressed gear

Querying addresses 0–63 cannot see a device that has no short address — status bit 6
set, or brand new out of the box. Finding those needs the DALI binary-search
discovery, which this library implements as commissioning.

> **This writes to your devices.** With `onlyNew = false` it *erases every existing
> short address on the bus* and reassigns from scratch. Anything referring to the old
> addresses — scenes, groups, your own code — will point at the wrong fixtures
> afterwards. Run the read-only scan first and save the result.

`commission()` only sets up the state machine; `commission_tick()` drives it and must
be called repeatedly until `commissionState` returns to `COMMISSION_OFF`.

```cpp
#include <Dali.h>

const byte TX = 3, RX = 2;

void setup() {
  Serial.begin(115200);
  Dali.begin(TX, RX, true, false);
  delay(200);

  Serial.println(F("commissioning - this reassigns short addresses"));

  // startAddress = 0, onlyNew = false: wipe and renumber everything from 0.
  // Use commission(n, true) to leave existing addresses alone and only give
  // addresses to gear that has none, starting at n.
  Dali.commission(0, false);

  while (Dali.commissionState != DaliClass::COMMISSION_OFF)
    Dali.commission_tick();

  Serial.print(F("found "));
  Serial.print(Dali.nextShortAddress);
  Serial.println(F(" device(s)"));
}

void loop() {}
```

`nextShortAddress` starts as the address to assign next, so when commissioning ends it
holds the count of devices found. Expect this to take seconds to minutes — the search
is a 24-bit binary search per device, and each step is a full DALI frame.

Non-blocking form, if the sketch has other work to do:

```cpp
void loop() {
  if (Dali.commissionState != DaliClass::COMMISSION_OFF) {
    Dali.commission_tick();     // returns immediately unless the bus is idle
    return;
  }
  // ... normal work
}
```

`commission_tick()` no-ops unless `DaliBus.busIsIdle()`, so calling it in a tight loop
is fine and costs nothing.

### Adding gear without disturbing what exists

Replacing one fixture, or extending an installation:

```cpp
Dali.commission(nextFreeAddress, true);   // onlyNew = true
```

This skips gear that already has an address, and numbers only the new ones from
`nextFreeAddress`. Get that number from a read-only scan first — the lowest address
that returned `-1`.

---

## Polarity

Every scan depends on being able to *receive*, so polarity has to be right in both
directions. `begin()` takes them separately because interface hardware often inverts
only one:

```cpp
Dali.begin(tx, rx, tx_active_low, rx_active_low);
```

| Interface | Setting |
|---|---|
| MOSFET/transistor shorting the bus (pin high pulls bus down) | `tx_active_low = true` |
| Driver where a high pin drives the bus high | `tx_active_low = false` |
| Optocoupler receiver, idle bus reads low at the pin | `rx_active_low = true` |
| Resistive divider, idle 16 V bus reads high at the pin | `rx_active_low = false` |

A MOSFET output with a divider input — a common discrete build — is
`Dali.begin(tx, rx, true, false)`. The older three-argument
`begin(tx, rx, active_low)` applies one flag to both and still works where both stages
match.

Symptom of getting it wrong: every call returns `-5` forever, because the timer ISR
reads the bus as permanently pulled down and parks the state machine in `SHORT`.

---

## Notes

- **Timeouts.** `timeout` is a `byte`, so 255 ms is the ceiling. 100 ms is comfortable
  for queries; the 50 ms default is tight against a ~33 ms frame.
- **Blocking vs not.** `sendCmdWait` blocks and returns the reply. `sendCmd` returns
  `-3` immediately and you poll `DaliBus.busIsIdle()` yourself. Scanning wants the
  blocking form.
- **`-1` is not a failure** in general use — for an ARC command it means "sent, nothing
  replied", which is normal. During a scan it specifically means the address is empty.
- **Repeat before believing a negative.** A single missed reply, from noise or a device
  still busy, looks identical to an empty address. Query twice before declaring an
  address free if the result matters.
- **Bus loading.** DALI allows 64 devices per line, and each draws current from the bus
  supply. A scan that finds fewer devices than expected can be an undersized supply
  rather than an addressing problem.
