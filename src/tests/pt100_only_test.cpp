#include <Adafruit_MAX31865.h>

// ==========================================================================
// Sensors only. No LCD, no Wire, no I2C, no relays, no calibration.
//
// Nothing here shares a pin or a library with anything else, so whatever this
// prints is the sensors alone. Raw counts are shown before any maths so a bad
// reading cannot be blamed on the conversion.
//
//   board A: SCK 18, MISO 19, MOSI 14, CS 5
//   board B: SCK 16, MISO 17, MOSI  4, CS 22
//   VIN -> 3V3, GND -> ESP32 GND
//
// 2026-09-03: NOTHING is shared between the two boards any more - each has
// its own SCK, MISO, MOSI and CS. Reason, measured not guessed: with a
// shared MOSI, whatever sits on board B's MOSI line held the pin high and
// killed communication for BOTH sensors at once, every time. A controlled
// swap proved it follows board B's wire, not the pin: GPIO13 and GPIO14
// each tested pad-OK with board A on them and PAD FAULT with board B on
// them. Full separation means a fault on either board can no longer reach
// the other one.
// ==========================================================================

#define SCK_A    18
#define MISO_A   19
#define MOSI_A   14
#define CS_A     5

#define SCK_B    16
#define MISO_B   17
#define MOSI_B   4
#define CS_B     22

#define RREF     430.0
#define RNOMINAL 100.0
#define WIRES    MAX31865_2WIRE

// --- SSR (Fotek SSR-40DA), driven straight from GPIO32 -------------------
// 2026-09-04: added back on top of the known-good sensors-only build, as
// the single changed variable. The opto module could not supply enough
// current to trigger it (see notes/ssr-opto-test-log.md).
// WARNING: this is NOT galvanically isolated. Fine for logic testing with
// no mains on the SSR output. Isolation MUST be restored before 230V.
#define SSR_PIN       32
#define SSR_PERIOD_MS 5000UL

bool ssrOn = false;
unsigned long ssrLastToggleMs = 0;

Adafruit_MAX31865 pt100_a = Adafruit_MAX31865(CS_A, MOSI_A, MISO_A, SCK_A);
Adafruit_MAX31865 pt100_b = Adafruit_MAX31865(CS_B, MOSI_B, MISO_B, SCK_B);

void report(const char *name, Adafruit_MAX31865 &sensor) {
  uint16_t raw = sensor.readRTD();
  uint8_t fault = sensor.readFault();

  Serial.print(name);
  Serial.print(" raw=");
  Serial.print(raw);
  Serial.print("  R=");
  Serial.print((raw / 32768.0) * RREF, 2);
  Serial.print(" ohm  T=");
  Serial.print(sensor.temperature(RNOMINAL, RREF), 2);
  Serial.print(" C  fault=0x");
  Serial.print(fault, HEX);

  if (fault) {
    if (fault & MAX31865_FAULT_HIGHTHRESH) Serial.print(" HighThresh");
    if (fault & MAX31865_FAULT_LOWTHRESH)  Serial.print(" LowThresh");
    if (fault & MAX31865_FAULT_REFINLOW)   Serial.print(" REFIN-low");
    if (fault & MAX31865_FAULT_REFINHIGH)  Serial.print(" REFIN-high");
    if (fault & MAX31865_FAULT_RTDINLOW)   Serial.print(" RTDIN-low");
    if (fault & MAX31865_FAULT_OVUV)       Serial.print(" Under/OverVolt");
    sensor.clearFault();
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // SSR off before anything else.
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);

  pinMode(CS_A, OUTPUT);
  pinMode(CS_B, OUTPUT);
  digitalWrite(CS_A, HIGH);
  digitalWrite(CS_B, HIGH);

  pt100_a.begin(WIRES);
  pt100_b.begin(WIRES);

  Serial.println();
  Serial.println("=== PT100 x2 + SSR (GPIO32, 5s on / 5s off) ===");
}

// Non-blocking - the sensor reads never wait on the SSR.
void serviceSsr() {
  if (millis() - ssrLastToggleMs < SSR_PERIOD_MS) return;
  ssrLastToggleMs = millis();
  ssrOn = !ssrOn;
  digitalWrite(SSR_PIN, ssrOn ? HIGH : LOW);
}

void loop() {
  serviceSsr();

  report("A:", pt100_a);
  report("B:", pt100_b);

  Serial.print("SSR=");
  Serial.println(ssrOn ? "ON" : "OFF");

  delay(1000);
}
