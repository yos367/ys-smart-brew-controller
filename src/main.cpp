#include <Adafruit_MAX31865.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>

// --- PT100 sensors (software SPI) ---
// 2026-09-03: the two boards share NOTHING - each has its own SCK, MISO,
// MOSI and CS. Measured reason, not a guess: with a shared MOSI line,
// whatever sits on board B's MOSI held the pin high and took out BOTH
// sensors at once, every single time. A controlled swap settled it -
// GPIO13 and GPIO14 each read pad-OK with board A's wire on them and
// PAD FAULT with board B's wire on them, so the fault travels with B's
// wire, not with any particular pin. Full separation means neither board
// can pull the other one down, whatever happens on its own lines.
#define SCK_A     18
#define MISO_A    19
#define MOSI_A    14
#define CS_A      5

#define SCK_B     16
#define MISO_B    17
#define MOSI_B    4
#define CS_B      22

// --- LCD (I2C) ---
#define PIN_SDA   21
#define PIN_SCL   27
#define LCD_COLS  16
#define LCD_ROWS  2

// 2026-09-03: was hard-hung when the LCD was powered from a second supply
// (phone charger) tied to the ESP32's GND - two live regulators fighting
// over a shared rail. Fixed by powering the LCD from the ESP32's own 5V
// pin instead, so there is only ever one source. That fix held for 78s of
// clean runtime, then the I2C bus locked up again on its own mid-session -
// intermittent, not power-related (confirmed no voltage sag on the 5V rail
// when it happened). Rather than keep chasing an intermittent bus lockup
// in software, the LCD now runs on its own FreeRTOS task (see lcdTask())
// so a hang in it can never block the sensor/SSR loop again, whether or
// not this specific lockup is ever fully rooted out.
// 2026-09-04: LCD back on. The "MOSI pad fault" that drove it out of the
// build turned out to be a bug in the old diagnostic, not real - see the
// correction in notes/session-2026-08-28-status.md. Sensors are now on
// fully separate pins per board and have run clean with the LCD physically
// wired but untouched by code, so this is the one changed variable.
#define LCD_ENABLED 1

// --- Rotary encoder EC11 - REMOVED 2026-08-28, unit appears to be dead. ---
// Wiring kept here for whenever a replacement goes in:
//   C  (common) -> GND
//   A  (CLK)    -> GPIO 4
//   B  (DT)     -> GPIO 17
//   SW          -> GPIO 16   (other switch leg -> GND)
// It drove a view selector: turn = toggle PT1/PT2, press = show both.
// Until it is replaced the display simply shows both sensors at once.

#define RREF      430.0
#define RNOMINAL  100.0
#define WIRES     MAX31865_2WIRE

// --- SSR (Fotek SSR-40DA) ---
// Logic test only: no 230V on the output side yet. Driven directly from
// GPIO32, no opto-isolation module - the module on hand couldn't supply
// enough current to trigger the SSR reliably. NOT isolated: add proper
// isolation before this pin's SSR ever switches real mains.
#define SSR_PIN         32
#define SSR_PERIOD_MS   5000UL

// --- SSR #2 (2026-09-05) --------------------------------------------------
// Second SSR, direct-drive on GPIO33, same as SSR1 - no opto module, same
// isolation warning applies. Function test only: SSR1 and SSR2 alternate
// (never both on at once), same 5s period as SSR1 already used. This is
// NOT the final recipe-driven element-selection logic - just proving two
// SSRs can be switched without disturbing the PT100/LCD readings that are
// already stable.
#define SSR2_PIN        33

// --- Relays (2026-09-05) --------------------------------------------------
// Two relay modules (BESTEP JQC3F-05VDC-C, HIGH-level trigger). Wiring
// confirmed good with a 3s auto-cycle test (both always moved together,
// no effect on PT100/LCD/SSR) - see notes/session-2026-09-04-summary.md
// section 6c. That test cycle is now removed: relays are forced off once
// in setup() and stay off permanently. There is no code path that turns
// them on yet - that will be an explicit decision from the UI, later.
#define RELAY1_PIN      26
#define RELAY2_PIN      25

// ==========================================================================
// Bench test: 2x PT100 + LCD. Both sensors shown together, one per row.
//
// PT100 note: plain Adafruit library in 2-wire mode plus the two-point
// calibration from 2026-08-06. Deliberately NO softResetCycle() - that came
// from a wrong diagnosis and its 0x00 config write also wiped the wire-mode
// bit. See notes/pt100-sensors-and-calibration.md.
// ==========================================================================

struct Calibration {
  float scale;
  float offset;
};

const Calibration CAL_A = {0.977977f, -1.279f};
const Calibration CAL_B = {0.976812f, -0.696f};

// --- Fault tolerance ---------------------------------------------------
// A single bad read must never be fatal. raw==0 with fault==0x00 means the
// chip did not answer at all (loose wire, momentary brown-out, or a config
// register corrupted by noise). Re-running begin() rewrites the config and
// recovers the last of those outright; for the others it costs nothing.
//
// STALE_MS is how long the last good reading stays usable before the sensor
// is declared dead. During a brew this is the value that must gate the
// heater: no reading newer than this means cut the power, never keep
// heating on a stale number.
#define RETRIES_PER_CYCLE 2
#define REINIT_AFTER_FAILS 5
#define STALE_MS 5000UL

// Slew gate (see readTemp). 25 C/s is far above any real brewing process -
// even a probe plunged straight into boiling water moves slower than this -
// but far below the instant jumps that corruption produces. The fixed
// tolerance covers normal sample-to-sample jitter at short intervals.
#define MAX_SLEW_C_PER_SEC 25.0f
#define SLEW_TOLERANCE_C   2.0f

struct Health {
  float lastGood = NAN;
  unsigned long lastGoodMs = 0;
  uint16_t consecFail = 0;
  uint32_t reinits = 0;
  bool everGood = false;
};

// --- Stability soak test (2026-09-03) ---------------------------------
// Sensors-only run, no LCD, tracking the MOSI fault against SSR switching
// and reboot cycles over an unattended stretch. bootCount survives a
// software reset (RTC memory) so the summary line shows which boot each
// stretch of runtime belongs to; only a real power loss clears it.
RTC_DATA_ATTR uint32_t bootCount = 0;
uint32_t totalFailsA = 0, totalFailsB = 0;
unsigned long lastFailMsA = 0, lastFailMsB = 0;
unsigned long longestStreakMsA = 0, longestStreakMsB = 0;
unsigned long lastSummaryMs = 0;

Health healthA, healthB;

Adafruit_MAX31865 pt100_a = Adafruit_MAX31865(CS_A, MOSI_A, MISO_A, SCK_A);
Adafruit_MAX31865 pt100_b = Adafruit_MAX31865(CS_B, MOSI_B, MISO_B, SCK_B);

LiquidCrystal_I2C *lcd = nullptr;
uint8_t lcdAddress = 0;

// Written by loop() (sensors/SSR task) every cycle, read by lcdTask() on its
// own schedule. Plain char arrays, no lock - worst case is one torn frame
// on the display for ~100ms, which is harmless and far better than letting
// the LCD task's I2C calls block the sensor loop.
char sharedLine0[LCD_COLS + 8] = "";
char sharedLine1[LCD_COLS + 8] = "";

// ssrOn/ssr2On track what was actually last written to the pins (both set
// explicitly to "off" in setup(), before serviceSsr() ever runs) - anything
// that logs or displays SSR state reads these, never derives one from the
// other, so the log can never claim a pin is ON when it was actually left
// OFF for safety.
bool ssrOn = false;
bool ssr2On = false;
unsigned long ssrLastToggleMs = 0;

// Non-blocking: called every loop, only acts once SSR_PERIOD_MS has passed.
// Sensor reads never wait on this.
// 2026-09-03: the opto-isolation module couldn't supply enough current to
// trigger the SSR (measured ~1-3V sag under load, module removed from the
// chain). GPIO32 drives the SSR directly - confirmed working. Normal
// polarity, no inversion.
void serviceSsr() {
  if (millis() - ssrLastToggleMs < SSR_PERIOD_MS) return;
  ssrLastToggleMs = millis();
  ssrOn = !ssrOn;
  digitalWrite(SSR_PIN, ssrOn ? HIGH : LOW);
  // SSR2 is always the complement of SSR1 - never both on at once. Same
  // toggle instant, same period, just the opposite level.
  ssr2On = !ssrOn;
  digitalWrite(SSR2_PIN, ssr2On ? HIGH : LOW);
}

// Relays are latched off in setup() and nothing in loop() ever writes to
// these pins again - no auto-cycle. Kept as a plain bool (always false for
// now) so the Serial status line and any future UI hook read one flag
// instead of two pin states that could in principle drift apart.
bool relaysOn = false;

float resistanceToCelsius(float Rt) {
  const float A = 3.9083e-3;
  const float B = -5.775e-7;

  float Z1 = -A;
  float Z2 = A * A - (4 * B);
  float Z3 = (4 * B) / RNOMINAL;
  float Z4 = 2 * B;

  float temp = Z2 + (Z3 * Rt);
  temp = (sqrt(temp) + Z1) / Z4;
  if (temp >= 0) return temp;

  Rt /= RNOMINAL;
  Rt *= 100;
  float rpoly = Rt;
  temp = -242.02;
  temp += 2.2228 * rpoly;
  rpoly *= Rt; temp += 2.5859e-3 * rpoly;
  rpoly *= Rt; temp -= 4.8260e-6 * rpoly;
  rpoly *= Rt; temp -= 2.8183e-8 * rpoly;
  rpoly *= Rt; temp += 1.5243e-10 * rpoly;
  return temp;
}

// Median of three raw reads. 2026-09-04: sensor A produced a 40-second
// burst of isolated bad samples (raw ~28300 among good ~8650 ones) while B,
// on its own separate pins, stayed clean throughout. A single corrupted
// sample gets outvoted by the two around it; a genuinely stuck line still
// fails all three and is caught downstream as before.
uint16_t readRawMedian(Adafruit_MAX31865 &sensor) {
  uint16_t a = sensor.readRTD();
  uint16_t b = sensor.readRTD();
  uint16_t c = sensor.readRTD();
  if (a > b) { uint16_t t = a; a = b; b = t; }
  if (b > c) { uint16_t t = b; b = c; c = t; }
  if (a > b) { uint16_t t = a; a = b; b = t; }
  return b;
}

bool readTempOnce(Adafruit_MAX31865 &sensor, const Calibration &cal,
                  float &tempOut, uint8_t &faultOut, uint16_t &rawOut) {
  uint16_t raw = readRawMedian(sensor);
  faultOut = sensor.readFault();
  rawOut = raw;

  if (faultOut) {
    sensor.clearFault();
    return false;
  }
  if (raw == 0 || raw >= 32767) return false;

  float rMeasured = (raw / 32768.0) * RREF;
  float rTrue = cal.scale * rMeasured + cal.offset;
  float t = resistanceToCelsius(rTrue);

  if (isnan(t) || t < -50.0 || t > 200.0) return false;

  tempOut = t;
  return true;
}

// Retries, then re-initialises the chip once the failures pile up. Returns
// true when this cycle produced a fresh reading; a caller that gets false can
// still fall back on health.lastGood while it is younger than STALE_MS.
bool readTemp(Adafruit_MAX31865 &sensor, const Calibration &cal, Health &health,
              float &tempOut, uint8_t &faultOut, uint16_t &rawOut) {
  for (int attempt = 0; attempt < RETRIES_PER_CYCLE; attempt++) {
    if (readTempOnce(sensor, cal, tempOut, faultOut, rawOut)) {
      // Slew-rate gate. Water and a steel probe have thermal mass - they
      // cannot jump tens of degrees in a fraction of a second. A reading
      // that moves faster than physics allows is corruption, not heat, and
      // this catches the corrupted values that happen to land INSIDE the
      // plausible -50..200C window, which the range check alone lets past.
      // The allowance grows with elapsed time, so a sensor coming back
      // after a long gap is not locked out.
      if (health.everGood) {
        float elapsedSec = (millis() - health.lastGoodMs) / 1000.0f;
        float allowed = MAX_SLEW_C_PER_SEC * elapsedSec + SLEW_TOLERANCE_C;
        if (fabs(tempOut - health.lastGood) > allowed) {
          continue; // treat as a bad sample; retry or fall through to fail
        }
      }
      health.lastGood = tempOut;
      health.lastGoodMs = millis();
      health.consecFail = 0;
      health.everGood = true;
      return true;
    }
  }

  health.consecFail++;
  if (health.consecFail % REINIT_AFTER_FAILS == 0) {
    sensor.begin(WIRES);
    health.reinits++;
  }
  return false;
}

// "PT1: 23.1C" fresh, "PT1: 23.1?" stale but still usable, "PT1: ERR" gone.
// Fixed char buffer, not String - this runs every 200ms and String's
// heap churn (realloc on every concatenation) fragments the heap over a
// long-running session.
void formatSensor(char *out, size_t outLen, const char *name, bool fresh,
                   float temp, const Health &health) {
  if (fresh) {
    snprintf(out, outLen, "%s: %.1fC", name, temp);
  } else if (health.everGood && (millis() - health.lastGoodMs) < STALE_MS) {
    snprintf(out, outLen, "%s: %.1f?", name, health.lastGood);
  } else {
    snprintf(out, outLen, "%s: ERR", name);
  }
}

// 2026-09-03: setup() hung forever inside scanForLcd() - the Wire driver's
// beginTransmission()/endTransmission() never returned, meaning SDA (or
// SCL) was physically stuck rather than just unanswered. That happens when
// a device loses power mid-transaction and its output stage keeps sinking
// the line low. The standard fix: bit-bang up to 9 SCL pulses before
// Wire.begin() ever touches the bus, which gives a stuck slave enough
// clocks to finish its byte and release SDA, then issue a manual STOP.
void i2cBusRecovery() {
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, OUTPUT);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(5);

  for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
    digitalWrite(PIN_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_SCL, HIGH);
    delayMicroseconds(5);
  }

  // Manual STOP: SDA low->high while SCL is high.
  pinMode(PIN_SDA, OUTPUT);
  digitalWrite(PIN_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SDA, HIGH);
  delayMicroseconds(5);
}

uint8_t scanForLcd() {
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

// --- LCD health --------------------------------------------------------
// LiquidCrystal_I2C never checks for an ACK, so a display that has dropped
// off the bus keeps showing its last frame and nothing reports it. The scan
// used to run once in setup(), which meant an LCD that was not answering at
// that instant was never written to again. Poll it instead, and rebuild the
// driver whenever it comes back.
uint32_t lcdReinits = 0;
uint16_t lcdFails = 0;
unsigned long lastLcdCheckMs = 0;

bool lcdAcks() {
  if (!lcdAddress) return false;
  Wire.beginTransmission(lcdAddress);
  return Wire.endTransmission() == 0;
}

void initLcd() {
  if (lcd) {
    delete lcd;
    lcd = nullptr;
  }
  lcd = new LiquidCrystal_I2C(lcdAddress, LCD_COLS, LCD_ROWS);
  lcd->init();
  delay(50);
  lcd->init();
  lcd->backlight();
  lcd->clear();
}

// Set only here, once a second - loop() reads this instead of calling
// lcdAcks() again itself, which used to fire an extra I2C transaction every
// 200ms regardless of this function's once-a-second gate.
bool lastLcdAck = false;

void serviceLcd() {
#if !LCD_ENABLED
  return;
#endif
  if (millis() - lastLcdCheckMs < 1000) return;
  lastLcdCheckMs = millis();

  if (!lcdAddress) {
    lastLcdAck = false;
    lcdAddress = scanForLcd();
    if (lcdAddress) {
      initLcd();
      lcdReinits++;
      Serial.print("LCD appeared at 0x");
      Serial.println(lcdAddress, HEX);
    }
    return;
  }

  lastLcdAck = lcdAcks();
  if (lastLcdAck) {
    lcdFails = 0;
    return;
  }

  lcdFails++;
  if (lcdFails >= 3) {
    lcdFails = 0;
    uint8_t addr = scanForLcd();
    if (addr) {
      lcdAddress = addr;
      initLcd();
      lcdReinits++;
      lastLcdAck = true;
      Serial.print("LCD recovered at 0x");
      Serial.println(lcdAddress, HEX);
    }
  }
}

// Fixed width so leftovers from a longer previous line never linger.
// Pads/truncates in place on a fixed buffer - no String, no heap churn.
void lcdLine(uint8_t row, const char *text) {
  if (!lcd) return;
  char padded[LCD_COLS + 1];
  size_t i = 0;
  for (; i < LCD_COLS && text[i] != '\0'; i++) padded[i] = text[i];
  for (; i < LCD_COLS; i++) padded[i] = ' ';
  padded[LCD_COLS] = '\0';
  lcd->setCursor(0, row);
  lcd->print(padded);
}

// Owns every Wire/LCD call, on its own FreeRTOS task. If the I2C bus locks
// up inside serviceLcd() or lcdLine() - which has happened, and survived
// both a manual bus-recovery sequence and Wire.setTimeOut() - this task
// blocks forever, but it is the *only* thing that blocks. loop() (sensors,
// SSR, the watchdog reset) keeps running on its own task, completely
// unaffected, for as long as it takes to notice and fix the LCD for real.
void lcdTask(void *param) {
  i2cBusRecovery();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setTimeOut(50); // ms - best effort; task isolation is the real fix
  lcdAddress = scanForLcd();
  if (lcdAddress) {
    initLcd();
    Serial.print("LCD at 0x");
    Serial.println(lcdAddress, HEX);
  } else {
    Serial.println("LCD NOT FOUND at boot - will keep retrying every second");
  }

  for (;;) {
    serviceLcd();
    lcdLine(0, sharedLine0);
    lcdLine(1, sharedLine1);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  bootCount++;
  Serial.print("[boot] start, bootCount=");
  Serial.println(bootCount);

  // Both SSRs forced off before anything else, including anything below
  // that could hang (I2C scan on a stuck bus, e.g.) - a hang must never
  // leave either one live. See serviceWatchdog() below for the other half
  // of this. serviceSsr() takes over and starts the SSR1/SSR2 alternation
  // once SSR_PERIOD_MS has elapsed - both stay off until then.
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW); // LOW = SSR off, direct-drive, normal polarity
  pinMode(SSR2_PIN, OUTPUT);
  digitalWrite(SSR2_PIN, LOW);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  // 2026-09-05: latched off permanently, not just at boot. Wiring already
  // confirmed good (see notes/session-2026-09-04-summary.md 6c) - nothing
  // in loop() writes to these pins again until a UI-driven decision to
  // turn them on exists.
  digitalWrite(RELAY1_PIN, LOW); // LOW = off, HIGH-level trigger modules
  digitalWrite(RELAY2_PIN, LOW);

  // 2026-09-03: setup() hung indefinitely in the I2C scan after a wiring
  // change left the bus in a bad state - the SSR stayed stuck ON the whole
  // time because nothing was left running to turn it off. A hardware
  // watchdog forces a full reset (which re-runs the digitalWrite above)
  // if loop() - or setup() itself - ever fails to check in within 10s.
  esp_task_wdt_init(10, true); // 10s timeout, panic (reset) on expiry
  esp_task_wdt_add(NULL);
  Serial.println("[boot] watchdog armed, SSR forced off");

  pinMode(CS_A, OUTPUT);
  pinMode(CS_B, OUTPUT);
  digitalWrite(CS_A, HIGH);
  digitalWrite(CS_B, HIGH);

  Serial.println("[boot] CS pins set");

  pt100_a.begin(WIRES);
  Serial.println("[boot] pt100_a.begin done");
  pt100_b.begin(WIRES);
  Serial.println("[boot] pt100_b.begin done");

#if LCD_ENABLED
  // Runs on its own task (see lcdTask) so a hang here can never stop this
  // function from returning, or block loop() from ever starting.
  xTaskCreatePinnedToCore(lcdTask, "lcdTask", 4096, NULL, 1, NULL, 0);
  Serial.println("[boot] lcdTask started on core 0");
#else
  Serial.println("[boot] LCD_ENABLED=0 - skipping Wire/LCD entirely");
#endif

  Serial.println("2x PT100 + LCD + SSR (P32, 5s on/5s off)");
}

void loop() {
  esp_task_wdt_reset();
  // 2026-09-04: SSR re-enabled. The "failures cluster while SSR is ON"
  // theory was disproved - the failures happened with the SSR never
  // switching at all, and the real causes were elsewhere (shared MOSI +
  // a faulty diagnostic). See notes/session-2026-09-04-summary.md.
  serviceSsr();
  // Relays: no service call - they were latched off in setup() and stay
  // that way until an explicit UI-driven decision exists to turn them on.

  float tA = NAN, tB = NAN;
  uint8_t faultA = 0, faultB = 0;
  uint16_t rawA = 0, rawB = 0;
  bool okA = readTemp(pt100_a, CAL_A, healthA, tA, faultA, rawA);
  bool okB = readTemp(pt100_b, CAL_B, healthB, tB, faultB, rawB);

  if (!okA) {
    totalFailsA++;
    unsigned long streak = millis() - lastFailMsA;
    if (streak > longestStreakMsA) longestStreakMsA = streak;
    lastFailMsA = millis();
  }
  if (!okB) {
    totalFailsB++;
    unsigned long streak = millis() - lastFailMsB;
    if (streak > longestStreakMsB) longestStreakMsB = streak;
    lastFailMsB = millis();
  }

  char s0[24], s1[24], l0[24], l1[24];
  formatSensor(s0, sizeof(s0), "PT1", okA, tA, healthA);
  formatSensor(s1, sizeof(s1), "PT2", okB, tB, healthB);

  // Fixed-width slot before the marker so it never drifts sideways when the
  // temperature string's length changes (e.g. "9.9C" vs "100.0C"). [H]=SSR1
  // on row 0, [H2]=SSR2 on row 1 - the two are always complementary.
  snprintf(l0, sizeof(l0), "%-12s%s", s0, ssrOn ? "[H]" : "   ");
  snprintf(l1, sizeof(l1), "%-12s%s", s1, ssr2On ? "[H2]" : "   ");

  // Hand off to lcdTask() - no Wire/LCD call happens on this task anymore.
  strncpy(sharedLine0, l0, sizeof(sharedLine0) - 1);
  sharedLine0[sizeof(sharedLine0) - 1] = '\0';
  strncpy(sharedLine1, l1, sizeof(sharedLine1) - 1);
  sharedLine1[sizeof(sharedLine1) - 1] = '\0';

  Serial.print("boot=");
  Serial.print(bootCount);
  Serial.print(" up=");
  Serial.print(millis() / 1000);
  Serial.print("s  lcd=0x");
  Serial.print(lcdAddress, HEX);
  Serial.print(lastLcdAck ? " ACK" : " NAK");
  Serial.print(" reinit=");
  Serial.print(lcdReinits);
  Serial.print("  LCD[0]=\"");
  Serial.print(l0);
  Serial.print("\"  LCD[1]=\"");
  Serial.print(l1);
  Serial.print("\"   A raw=");
  Serial.print(rawA);
  Serial.print(" fault=0x");
  Serial.print(faultA, HEX);
  Serial.print(" fails=");
  Serial.print(healthA.consecFail);
  Serial.print(" reinit=");
  Serial.print(healthA.reinits);
  Serial.print(" | B raw=");
  Serial.print(rawB);
  Serial.print(" fault=0x");
  Serial.print(faultB, HEX);
  Serial.print(" fails=");
  Serial.print(healthB.consecFail);
  Serial.print(" reinit=");
  Serial.print(healthB.reinits);
  Serial.print(" | SSR1=");
  Serial.print(ssrOn ? "ON" : "OFF");
  Serial.print(" SSR2=");
  Serial.print(ssr2On ? "ON" : "OFF");
  Serial.print(" | RELAYS=");
  Serial.println(relaysOn ? "ON" : "OFF");

  if (millis() - lastSummaryMs >= 60000) {
    lastSummaryMs = millis();
    Serial.print("[summary] boot=");
    Serial.print(bootCount);
    Serial.print(" upSec=");
    Serial.print(millis() / 1000);
    Serial.print(" | A totalFails=");
    Serial.print(totalFailsA);
    Serial.print(" longestGoodStreakSec=");
    Serial.print(longestStreakMsA / 1000);
    Serial.print(" | B totalFails=");
    Serial.print(totalFailsB);
    Serial.print(" longestGoodStreakSec=");
    Serial.println(longestStreakMsB / 1000);
  }

  // 2026-09-04: the scheduled 10-minute self-reset was a soak-test tool for
  // telling "fails cluster near boot" from "fails spread through runtime".
  // Removed - from here on, ANY reboot in the log is a real event worth
  // investigating, not something this firmware caused on purpose.

  delay(200);
}
