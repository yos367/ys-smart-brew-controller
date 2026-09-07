#include <Arduino.h>

// ==========================================================================
// Statistical pin diagnostic - written 2026-09-04 because the older
// spi_lines_diag gave contradictory one-shot answers: the same pin, with the
// same wire on it, read "pad OK" in one run and "PAD FAULT" minutes later.
// A single sample cannot characterise an intermittent fault, and it cannot
// tell a real fault apart from a flawed measurement.
//
// Three independent tests per pin, so they cross-check each other:
//
//   TEST 1 - drive/read-back, N times. Counts failures instead of reporting
//            the first one it happens to see. An intermittent fault shows up
//            as a percentage; a hard fault shows up as 100%.
//
//   TEST 2 - the pin as a pure INPUT with the ESP32's internal pull-down,
//            then internal pull-up. We drive nothing at all here, so this
//            measures what the EXTERNAL circuit does:
//              follows both  -> nothing external is holding the line
//              stuck at 1    -> something external drives/pulls it high
//              stuck at 0    -> something external drives/pulls it low
//            This is the honest test. If TEST 1 says FAULT but TEST 2 says
//            the line follows freely, then TEST 1 (and every conclusion
//            built on it) is wrong.
//
//   TEST 3 - float check: input, no pull at all, just read. Informational.
//
// No SPI, no libraries, no shared state with any other file.
// ==========================================================================

struct PinDef {
  const char *name;
  uint8_t pin;
};

// Every line currently in use by the two PT100 boards, plus GPIO23, which
// was the original shared MOSI and is now unused - it acts as a control:
// nothing is wired to it, so it must always come back clean. If GPIO23 ever
// reports a fault, the measurement itself is lying.
PinDef pins[] = {
    {"GPIO18 SCK  A", 18}, {"GPIO14 MOSI A", 14}, {"GPIO19 MISO A", 19},
    {"GPIO 5 CS   A", 5},  {"GPIO16 SCK  B", 16}, {"GPIO 4 MOSI B", 4},
    {"GPIO17 MISO B", 17}, {"GPIO22 CS   B", 22}, {"GPIO23 (unused)", 23},
};
const int PIN_COUNT = sizeof(pins) / sizeof(pins[0]);

#define REPS 500

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== statistical pin diagnostic ===");
  Serial.print("drive/read-back repetitions per pin: ");
  Serial.println(REPS);
  Serial.println("GPIO23 is wired to nothing - it is the control line.");
}

void loop() {
  Serial.println();
  Serial.println("pin              T1 drive-fail   T2 pulldown/pullup   T3 float");
  Serial.println("---------------------------------------------------------------");

  for (int i = 0; i < PIN_COUNT; i++) {
    uint8_t p = pins[i].pin;

    // --- TEST 1: drive high, drive low, read back, REPS times -----------
    int failHigh = 0, failLow = 0;
    for (int n = 0; n < REPS; n++) {
      pinMode(p, OUTPUT);
      digitalWrite(p, HIGH);
      delayMicroseconds(20);
      if (digitalRead(p) != HIGH) failHigh++;
      digitalWrite(p, LOW);
      delayMicroseconds(20);
      if (digitalRead(p) != LOW) failLow++;
    }

    // --- TEST 2: nothing driven by us; internal pulls only --------------
    pinMode(p, INPUT_PULLDOWN);
    delayMicroseconds(200);
    int withPulldown = digitalRead(p);
    pinMode(p, INPUT_PULLUP);
    delayMicroseconds(200);
    int withPullup = digitalRead(p);

    // --- TEST 3: floating read ------------------------------------------
    pinMode(p, INPUT);
    delayMicroseconds(200);
    int floating = digitalRead(p);

    Serial.print(pins[i].name);
    Serial.print("   hi:");
    Serial.print(failHigh);
    Serial.print(" lo:");
    Serial.print(failLow);
    Serial.print("        ");
    Serial.print(withPulldown);
    Serial.print("/");
    Serial.print(withPullup);
    if (withPulldown == 0 && withPullup == 1) {
      Serial.print(" free    ");
    } else if (withPulldown == 1 && withPullup == 1) {
      Serial.print(" HELD HI ");
    } else if (withPulldown == 0 && withPullup == 0) {
      Serial.print(" HELD LO ");
    } else {
      Serial.print(" odd     ");
    }
    Serial.print("     ");
    Serial.println(floating);
  }

  delay(3000);
}
