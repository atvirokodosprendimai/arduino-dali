/** @file dali_slave.ino
 *  Answering as DALI control gear (a "slave") instead of driving the bus.
 *
 *  Demonstrates the memory bank access pattern: the master points DTR1 at a
 *  bank and DTR2 at an offset, then issues READ MEMORY LOCATION repeatedly.
 *  Each read answers with one byte and advances DTR2, so the master walks the
 *  bank without re-addressing every byte.
 *
 *  Timing matters here. A DALI reply must begin 7..22 TE after the master's
 *  frame, so the decision to answer is made inside the receive callback, which
 *  runs in interrupt context. Keep it short -- no Serial, no delay, no malloc.
 *  Anything you want to print goes in a queue that loop() drains.
 */

#include <Dali.h>

const byte TX_PIN = 3;
const byte RX_PIN = 2;

/** This device's short address. Real gear gets one by commissioning; here it
 *  is fixed so the example needs no master-side setup. */
const byte MY_ADDRESS = 0;

/* ---- DALI opcodes this sketch answers -------------------------------------
 * READ_MEMORY_LOCATION is absent from DaliCommands.h, so define it here. */
const byte READ_MEMORY_LOCATION = 197;

/* Special commands arrive as an address byte, encoded by the master as
 * ((cmd - 256 + 16) << 1) | 0b10000001. Decoding back gives these. */
const word SPECIAL_SET_DTR  = 257;
const word SPECIAL_SET_DTR1 = 273;
const word SPECIAL_SET_DTR2 = 274;

/* ---- Emulated device state ------------------------------------------------ */

volatile byte dtr0 = 0;      /**< general data transfer register */
volatile byte dtr1 = 0;      /**< selects the memory bank */
volatile byte dtr2 = 0;      /**< offset within that bank; auto-increments on read */
volatile byte arcLevel = 0;  /**< current light output, so ARC commands do something visible */

/** Bank 0 is mandatory and describes the device. Byte 2 must report the
 *  highest bank number implemented, otherwise a master has no reason to look
 *  for bank 4 at all. */
const byte bank0[] PROGMEM = {
  0x0E,  // 0: last accessible location in this bank
  0x00,  // 1: reserved
  0x04,  // 2: last accessible memory bank  <-- advertises bank 4
  0x00,  // 3: reserved
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 4-9: GTIN
  0x01, 0x00,                          // 10-11: firmware version
  0x00, 0x00, 0x00                     // 12-14: serial (truncated for the example)
};

/** Bank 4 -- whatever payload you want to expose. */
byte bank4[] = { 0x0F, 'S', 'L', 'A', 'V', 'E', 0x01, 0x02, 0x03, 0x04,
                 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };

/** Fetch one byte, or -1 when the bank/offset is not implemented. DALI answers
 *  nothing at all for an unimplemented location, which is why this is signed. */
int readBank(byte bank, byte offset) {
  if (bank == 0)
    return (offset < sizeof(bank0)) ? pgm_read_byte(&bank0[offset]) : -1;
  if (bank == 4)
    return (offset < sizeof(bank4)) ? bank4[offset] : -1;
  return -1;
}

/* ---- Debug queue, drained by loop() --------------------------------------- */

struct LogEntry { byte addr, opcode; int answered; };
volatile LogEntry logBuf[8];
volatile byte logHead = 0, logTail = 0;

inline void logFrame(byte addr, byte opcode, int answered) {
  byte next = (logHead + 1) & 7;
  if (next == logTail) return;            // full: drop rather than block the ISR
  logBuf[logHead].addr = addr;
  logBuf[logHead].opcode = opcode;
  logBuf[logHead].answered = answered;
  logHead = next;
}

/* ---- Frame handling (interrupt context) ----------------------------------- */

/** True when a forward frame's address byte selects this device. */
bool addressedToMe(byte a) {
  if (a == 0xFE || a == 0xFF) return true;                 // broadcast
  if ((a & 0x80) == 0) return ((a >> 1) & 0x3F) == MY_ADDRESS;  // short address
  return false;                                            // groups: not a member
}

/** Runs from the timer ISR the moment a frame completes. Must be quick: the
 *  answer window opens a few hundred microseconds from now. */
void onFrame(uint8_t *data, uint8_t bits) {
  if (bits != 16) return;                 // only standard forward frames here

  byte addr   = data[0];
  byte opcode = data[1];
  int  answer = -1;

  // Special commands are broadcast to every device and carry no address.
  if ((addr & 0x81) == 0x81 && addr >= 0xA0) {
    word cmd = 256 + (((addr >> 1) & 0x3F) - 16);
    if      (cmd == SPECIAL_SET_DTR)  dtr0 = opcode;
    else if (cmd == SPECIAL_SET_DTR1) dtr1 = opcode;   // choose bank
    else if (cmd == SPECIAL_SET_DTR2) dtr2 = opcode;   // choose offset
    logFrame(addr, opcode, -1);
    return;                                            // special commands get no reply
  }

  if (!addressedToMe(addr)) return;

  if ((addr & 0x01) == 0) {               // selector bit clear -> direct arc level
    arcLevel = opcode;
    logFrame(addr, opcode, -1);
    return;                               // ARC commands are never answered
  }

  switch (opcode) {
    case READ_MEMORY_LOCATION:
      answer = readBank(dtr1, dtr2);
      /* Post-increment is the whole point: the master issues the same opcode
       * again and walks to the next byte. Saturate rather than wrap, matching
       * gear that simply stops answering past the end of a bank. */
      if (dtr2 < 0xFF) dtr2++;
      break;

    case DaliCmd::QUERY_BALLAST:      answer = 0xFF; break;   // yes, present
    case DaliCmd::QUERY_STATUS:       answer = arcLevel ? 0x04 : 0x00; break;
    case DaliCmd::QUERY_ACTUAL_LEVEL: answer = arcLevel; break;
    case DaliCmd::QUERY_DTR:          answer = dtr0; break;
    case DaliCmd::QUERY_DEVICE_TYPE:  answer = 6; break;      // LED module
    default: break;                                            // silence = not supported
  }

  if (answer >= 0)
    DaliBus.sendResponse((byte) answer);   // must happen now, inside the window

  logFrame(addr, opcode, answer);
}

/* ---- Sketch --------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  Dali.setCallback(onFrame);
  Dali.begin(TX_PIN, RX_PIN, true, false);   // match your interface's polarity
  Serial.print(F("DALI slave at short address "));
  Serial.println(MY_ADDRESS);
}

void loop() {
  while (logTail != logHead) {
    noInterrupts();
    LogEntry e = { logBuf[logTail].addr, logBuf[logTail].opcode, logBuf[logTail].answered };
    logTail = (logTail + 1) & 7;
    interrupts();

    Serial.print(F("addr 0x")); Serial.print(e.addr, HEX);
    Serial.print(F("  op 0x"));  Serial.print(e.opcode, HEX);
    if (e.answered >= 0) { Serial.print(F("  -> 0x")); Serial.print(e.answered, HEX); }
    else                   Serial.print(F("  (no reply)"));
    Serial.println();
  }
}
