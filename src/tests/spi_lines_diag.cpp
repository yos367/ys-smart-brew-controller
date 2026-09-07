#include <Arduino.h>

// ==========================================================================
// Two-part diagnostic for the PT100 SPI bus, all in software.
//
// PART 1 - pad integrity. Drive each ESP32 pin high and low and read the pad
// back. This is safe: SCK/MOSI/CS are inputs on the breakouts, and SDO is
// high-Z while CS is high, so nothing fights us. A pin that will not read
// back what it was told to output is damaged.
//
// PART 2 - does MISO ever move? Clock out a register read by hand and sample
// SDO on every clock. The exact register value depends on getting SPI mode 1
// timing right, so that is NOT what is claimed here. The claim is weaker and
// far more robust: if SDO never once goes high across a whole transaction on
// either board, then nothing is driving that line at all.
// ==========================================================================

#define SCK_A    18
#define MISO_A   19
#define MOSI_A   14
#define CS_A     5

#define SCK_B    16
#define MISO_B   17
#define MOSI_B   4
#define CS_B     22

void padTest(const char *name, uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delayMicroseconds(50);
  int readHigh = digitalRead(pin);

  digitalWrite(pin, LOW);
  delayMicroseconds(50);
  int readLow = digitalRead(pin);

  pinMode(pin, INPUT);

  Serial.print("  GPIO");
  if (pin < 10) Serial.print(" ");
  Serial.print(pin);
  Serial.print(" ");
  Serial.print(name);
  Serial.print("  drove HIGH->read ");
  Serial.print(readHigh);
  Serial.print(", drove LOW->read ");
  Serial.print(readLow);
  Serial.println((readHigh == 1 && readLow == 0) ? "   -> pad OK" : "   -> PAD FAULT");
}

// Clock out one byte, MSB first, sampling SDO on every bit.
// Returns the bits seen; highSeen reports whether SDO was ever high.
uint8_t shiftByte(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t out,
                  bool &highSeen) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(mosi, (out >> i) & 1);
    digitalWrite(sck, HIGH);
    delayMicroseconds(5);
    digitalWrite(sck, LOW);
    delayMicroseconds(5);
    int bit = digitalRead(miso);
    if (bit) highSeen = true;
    in = (in << 1) | bit;
  }
  return in;
}

void probeBoard(const char *name, uint8_t cs, uint8_t sck, uint8_t mosi,
                uint8_t miso) {
  pinMode(sck, OUTPUT);
  pinMode(mosi, OUTPUT);
  pinMode(miso, INPUT);
  pinMode(cs, OUTPUT);

  digitalWrite(sck, LOW);
  digitalWrite(cs, HIGH);
  delayMicroseconds(100);

  bool highSeen = false;

  digitalWrite(cs, LOW);
  delayMicroseconds(50);
  shiftByte(sck, mosi, miso, 0x00, highSeen);  // address: config reg, read
  uint8_t cfg = shiftByte(sck, mosi, miso, 0x00, highSeen);
  uint8_t rtdMsb = shiftByte(sck, mosi, miso, 0x00, highSeen);
  uint8_t rtdLsb = shiftByte(sck, mosi, miso, 0x00, highSeen);
  digitalWrite(cs, HIGH);

  Serial.print("  ");
  Serial.print(name);
  Serial.print("  cfg=0x");
  Serial.print(cfg, HEX);
  Serial.print(" rtd=0x");
  Serial.print(rtdMsb, HEX);
  Serial.print(rtdLsb, HEX);
  Serial.print("   SDO ever high during 32 clocks: ");
  Serial.println(highSeen ? "YES - board is driving the line"
                          : "NO  - line never moved");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(CS_A, OUTPUT);
  pinMode(CS_B, OUTPUT);
  digitalWrite(CS_A, HIGH);
  digitalWrite(CS_B, HIGH);

  Serial.println();
  Serial.println("=== PT100 SPI diagnostic ===");
}

void loop() {
  Serial.println();
  Serial.println("PART 1 - ESP32 pad integrity");
  Serial.println("  -- board A lines --");
  padTest("SCK  A", SCK_A);
  padTest("MOSI A", MOSI_A);
  padTest("MISO A", MISO_A);
  padTest("CS   A", CS_A);
  Serial.println("  -- board B lines --");
  padTest("SCK  B", SCK_B);
  padTest("MOSI B", MOSI_B);
  padTest("MISO B", MISO_B);
  padTest("CS   B", CS_B);

  Serial.println("PART 2 - does SDO move during a transaction?");
  probeBoard("board A", CS_A, SCK_A, MOSI_A, MISO_A);
  probeBoard("board B", CS_B, SCK_B, MOSI_B, MISO_B);

  delay(3000);
}
