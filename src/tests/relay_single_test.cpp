#include <Arduino.h>

// ==========================================================================
// Standalone bench test: ONE relay output only.
//
//   drive pin -> GPIO 26   (the only signal this sketch touches)
//   GPIO 25   -> left untouched / not driven
//
// Purpose: keep the output and the wire fixed, and move that single IN wire
// from one relay module to the other. Whatever changes is the module.
//   both modules follow the 3s cycle -> modules are fine, look at wiring
//   one module stays energised      -> that module is the faulty one
//
// Module supply stays as-is: DC+ external 5V buck, DC- shared GND.
// ==========================================================================

// Change this one line to switch which output is under test (26 or 25).
#define RELAY_PIN 25

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  delay(500);
  Serial.println("Single relay test: GPIO26 only, 3s ON / 3s OFF");
  Serial.println("Move the IN wire between the two modules to compare them.");
}

void loop() {
  digitalWrite(RELAY_PIN, HIGH);
  Serial.print("Relay: ON    (wrote HIGH, readback P26=");
  Serial.print(digitalRead(RELAY_PIN));
  Serial.println(")");
  delay(3000);

  digitalWrite(RELAY_PIN, LOW);
  Serial.print("Relay: OFF   (wrote LOW,  readback P26=");
  Serial.print(digitalRead(RELAY_PIN));
  Serial.println(")");
  delay(3000);
}
