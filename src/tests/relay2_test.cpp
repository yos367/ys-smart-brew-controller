#include <Arduino.h>

// ==========================================================================
// Standalone bench test: two relay modules only. No sensors, no LCD, no PID.
//
// Hardware: 2x BESTEP JQC3F-05VDC-C, 1-channel, HIGH-level trigger.
//   relay 1  IN -> GPIO 26
//   relay 2  IN -> GPIO 25
//   both     DC+ / DC- -> external 5V buck (24V supply), NOT tied to ESP32 GND
//
// The two grounds are deliberately left separate: tying them created a second
// return path alongside the one through mains earth, and the resulting loop
// was what made the relays stick and the sensors/LCD drop out. See the notes.
//
// Both relays switch together every 3 seconds. Pin state is read back after
// each write so the log proves what the ESP32 actually put on the pins.
// ==========================================================================

#define RELAY1_PIN 26
#define RELAY2_PIN 25

void setBoth(int level, const char *label) {
  digitalWrite(RELAY1_PIN, level);
  digitalWrite(RELAY2_PIN, level);

  Serial.print(label);
  Serial.print("   readback P26=");
  Serial.print(digitalRead(RELAY1_PIN));
  Serial.print(" P25=");
  Serial.println(digitalRead(RELAY2_PIN));
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  delay(500);
  Serial.println("Both relays together: GPIO26 + GPIO25, 3s ON / 3s OFF");
}

void loop() {
  setBoth(HIGH, "Both relays: ON ");
  delay(3000);

  setBoth(LOW,  "Both relays: OFF");
  delay(3000);
}
