// Ground link: the laptop's radio. A Nano with an RFM95 breakout on the laptop's
// USB, run by tools/ground.py. It passes every packet it hears to the laptop as
// one line, and sends a poll or a console line to a stand when the laptop asks.
// It never transmits on its own.
//
// It sends as the launch box (node 0), so a stand answers it the same way and
// counts it toward the link.
//
// Wiring: SCK 13, MISO 12, MOSI 11, CS 4, G0 3, RST 2, VIN 5 V, GND.
// Needs the RadioHead library. radio_link.h is a copy of the firmware's
// src/app/radio_link.h: copy it again when that changes.
//
// At 115200 baud. Every line it takes gets exactly one C line back:
//   poll <stand>           one POLL
//   cmd <stand> <line>     a console line, sent until the stand's telemetry acknowledges it
//   power <dBm>            transmit power, 2 to 20
// Lines it sends:
//   I <text>                    information
//   P <ms> <rssi> <snr> <hex>   a packet heard: to, from, sequence, spare, then the data
//   T <ms> <hex>                a packet it sent, the same way
//   B <count>                   damaged packets heard since it started (failed the CRC),
//                               when the count has gone up, at most once a second
//   C <stand> <result>          ok, failed, busy (the launch box is on the air), toolong or usage
// <ms> is the Nano's millis() when the packet arrived, or finished going out.

#include <SPI.h>
#include <RH_RF95.h>
#include "radio_link.h"

const uint8_t PIN_CS = 4;
const uint8_t PIN_G0 = 3;
const uint8_t PIN_RST = 2;

const int8_t TX_POWER_DBM = 5;          // at power-up. Low for the bench; "power" raises it
const uint16_t SEND_WAIT_MS = 200;      // the longest packet takes about 31 ms
const uint16_t ANSWER_WAIT_MS = 150;    // a stand answers within about 50 ms
const uint8_t COMMAND_TRIES = 3;
const uint16_t BOX_QUIET_MS = 1000;     // no STATE for this long: the launch box isn't on the air
const uint16_t FRESH_MS = 1000;         // a stand's command number heard this recently needs no poll
const uint16_t BAD_REPORT_MS = 1000;    // how often to look at the damaged count

RH_RF95 rf95(PIN_CS, PIN_G0);

uint32_t boxHeardMs;
bool boxHeard;
uint8_t lastSequence[RADIO_STAND_MAX + 1];   // the last command each stand took, from its telemetry
uint32_t sequenceMs[RADIO_STAND_MAX + 1];
bool sequenceHeard[RADIO_STAND_MAX + 1];

char line[RADIO_COMMAND_MAX + 16];
uint8_t lineLength;

uint16_t badReported;
uint32_t badCheckedMs;

void printHex(uint8_t b)
{
  static const char digits[] = "0123456789ABCDEF";
  Serial.write(digits[b >> 4]);
  Serial.write(digits[b & 15]);
}

void printBytes(const uint8_t *bytes, uint8_t length)
{
  for (uint8_t i = 0; i < length; i++) printHex(bytes[i]);
}

bool boxOnAir()
{
  return boxHeard && millis() - boxHeardMs < BOX_QUIET_MS;
}

// Reads the packet that has arrived and passes it to the laptop. Returns the
// stand it came from if it was a stand's telemetry, otherwise 0.
uint8_t takePacket()
{
  uint32_t ms = millis();
  uint8_t data[sizeof(radio_telemetry_t) + 8];
  uint8_t length = sizeof data;
  if (!rf95.recv(data, &length)) return 0;

  // RadioHead keeps the 4 address bytes as its header: put them back in front.
  radio_address_t address = { rf95.headerTo(), rf95.headerFrom(), rf95.headerId(), rf95.headerFlags() };
  int16_t rssi = rf95.lastRssi();
  int snr = rf95.lastSNR();
  // Listen again before printing: a telemetry line takes about 12 ms to go out.
  rf95.available();

  radio_tag_t tag = {};
  if (length >= sizeof tag) memcpy(&tag, data, sizeof tag);
  bool ours = tag.network == RADIO_NETWORK_ID && tag.version == RADIO_PROTOCOL_VERSION;
  uint8_t stand = 0;
  if (ours && tag.type == RADIO_STATE && address.from == RADIO_NODE_BOX) {
    boxHeard = true;
    boxHeardMs = millis();
  }
  if (ours && tag.type == RADIO_TELEMETRY && length == sizeof(radio_telemetry_t)
      && address.from >= 1 && address.from <= RADIO_STAND_MAX) {
    stand = address.from;
    lastSequence[stand] = address.sequence;
    sequenceMs[stand] = millis();
    sequenceHeard[stand] = true;
  }

  Serial.print(F("P "));
  Serial.print(ms);
  Serial.print(' ');
  Serial.print(rssi);
  Serial.print(' ');
  Serial.print(snr);
  Serial.print(' ');
  printBytes((const uint8_t *)&address, sizeof address);
  printBytes(data, length);
  Serial.println();
  return stand;
}

// Passes on how many damaged packets RadioHead has counted, when it goes up.
void reportBad()
{
  if (millis() - badCheckedMs < BAD_REPORT_MS) return;
  badCheckedMs = millis();
  uint16_t bad = rf95.rxBad();
  if (bad == badReported) return;
  badReported = bad;
  Serial.print(F("B "));
  Serial.println(bad);
}

// Sends a packet to a stand and waits for its telemetry, passing on whatever
// arrives meanwhile. True if the stand answered.
bool exchange(uint8_t stand, uint8_t sequence, const uint8_t *data, uint8_t length)
{
  rf95.setHeaderTo(stand);
  rf95.setHeaderId(sequence);
  rf95.send(data, length);
  if (!rf95.waitPacketSent(SEND_WAIT_MS)) {
    rf95.setModeIdle();
    rf95.available();
    Serial.println(F("I no interrupt from G0 after sending: is G0 wired to D3?"));
    return false;
  }
  uint32_t sentMs = millis();
  // Listening before printing: the stand answers 5 ms after the packet ends.
  rf95.available();

  radio_address_t address = { stand, RADIO_NODE_BOX, sequence, 0 };
  Serial.print(F("T "));
  Serial.print(sentMs);
  Serial.print(' ');
  printBytes((const uint8_t *)&address, sizeof address);
  printBytes(data, length);
  Serial.println();

  uint32_t start = millis();
  while (millis() - start < ANSWER_WAIT_MS) {
    if (rf95.available() && takePacket() == stand) return true;
  }
  return false;
}

bool poll(uint8_t stand)
{
  radio_tag_t tag = { RADIO_NETWORK_ID, RADIO_PROTOCOL_VERSION, RADIO_POLL };
  return exchange(stand, 0, (const uint8_t *)&tag, sizeof tag);
}

// Sends a console line until the stand's telemetry carries the line's number
// back. The number is one more than the last command the stand took, which every
// telemetry packet carries; with none heard lately, a poll asks for it.
const char *sendCommand(uint8_t stand, const char *text)
{
  uint8_t textLength = strlen(text);
  if (textLength > RADIO_COMMAND_MAX) return "toolong";

  bool fresh = sequenceHeard[stand] && millis() - sequenceMs[stand] < FRESH_MS;
  if (!fresh && !poll(stand)) return "failed";

  uint8_t packet[sizeof(radio_tag_t) + RADIO_COMMAND_MAX];
  radio_tag_t tag = { RADIO_NETWORK_ID, RADIO_PROTOCOL_VERSION, RADIO_COMMAND };
  memcpy(packet, &tag, sizeof tag);
  memcpy(&packet[sizeof tag], text, textLength);

  uint8_t sequence = lastSequence[stand] + 1;
  for (uint8_t i = 0; i < COMMAND_TRIES; i++) {
    if (exchange(stand, sequence, packet, sizeof tag + textLength) && lastSequence[stand] == sequence) {
      return "ok";
    }
  }
  return "failed";
}

void printPower(int8_t dbm)
{
  Serial.print(F("I sending at "));
  Serial.print(dbm);
  Serial.println(F(" dBm"));
}

void reply(uint8_t stand, const char *result)
{
  Serial.print(F("C "));
  Serial.print(stand);
  Serial.print(' ');
  Serial.println(result);
}

void runLine(char *text)
{
  char *args = strchr(text, ' ');
  if (args) *args++ = 0;
  else args = text + strlen(text);

  if (!strcmp(text, "power")) {
    int dbm = atoi(args);
    if (dbm < 2 || dbm > 20) {
      reply(0, "usage");
      return;
    }
    rf95.setTxPower(dbm, false);
    printPower(dbm);
    reply(0, "ok");
    return;
  }

  // The stand's number, then for cmd the line.
  uint8_t stand = args[0] - '0';
  if (stand < 1 || stand > RADIO_STAND_MAX || (args[1] != ' ' && args[1] != 0)) {
    reply(0, "usage");
    return;
  }
  char *rest = args[1] ? args + 2 : args + 1;

  bool isPoll = !strcmp(text, "poll");
  bool isCommand = !strcmp(text, "cmd") && rest[0];
  if (!isPoll && !isCommand) {
    reply(0, "usage");
  } else if (boxOnAir()) {
    // Sending now would collide with the launch box.
    reply(stand, "busy");
  } else if (isPoll) {
    reply(stand, poll(stand) ? "ok" : "failed");
  } else {
    reply(stand, sendCommand(stand, rest));
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(10);
  digitalWrite(PIN_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    for (;;) {
      Serial.println(F("I RFM95 not responding: check the wiring"));
      delay(2000);
    }
  }
  rf95.setFrequency(RADIO_FREQUENCY_HZ / 1000000.0);
  rf95.setSignalBandwidth(RADIO_BANDWIDTH_KHZ * 1000L);
  rf95.setSpreadingFactor(RADIO_SPREADING_FACTOR);
  rf95.setCodingRate4(RADIO_CODING_RATE);
  rf95.setPreambleLength(RADIO_PREAMBLE_SYMBOLS);
  rf95.setTxPower(TX_POWER_DBM, false);
  rf95.setHeaderFrom(RADIO_NODE_BOX);
  rf95.setPromiscuous(true);            // hear packets addressed to anyone
  rf95.setModeRx();

  Serial.print(F("I ground_link: protocol "));
  Serial.print(RADIO_PROTOCOL_VERSION);
  Serial.print(F(", "));
  Serial.print(RADIO_FREQUENCY_HZ / 1000000.0, 1);
  Serial.print(F(" MHz, SF"));
  Serial.print(RADIO_SPREADING_FACTOR);
  Serial.print(F(", "));
  Serial.print(RADIO_BANDWIDTH_KHZ);
  Serial.print(F(" kHz, 4/"));
  Serial.println(RADIO_CODING_RATE);
  printPower(TX_POWER_DBM);
}

void loop()
{
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[lineLength] = 0;
      if (lineLength) runLine(line);
      lineLength = 0;
    } else if (lineLength < sizeof line - 1) {
      line[lineLength++] = c;
    }
  }

  if (rf95.available()) takePacket();
  reportBad();
}
