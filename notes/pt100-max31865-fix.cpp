// קובץ בדיקה/אבחון ממוקד — לא קוד ייצור מלא.
// מתעד ומממש את הפתרון לבעיית קריאת PT100 שגויה דרך MAX31865.
// ראה notes/pt100-max31865-fix.md להסבר המלא: מה נבדק, מה נשלל, ולמה זה עובד.

#include <Adafruit_MAX31865.h>
#include <Adafruit_MAX31855.h>

// פינים משותפים (SPI)
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23

// פיני CS נפרדים
#define CS_PT100  5
#define CS_TC     22

Adafruit_MAX31865 pt100 = Adafruit_MAX31865(CS_PT100, PIN_MOSI, PIN_MISO, PIN_SCK);
Adafruit_MAX31855 thermocouple = Adafruit_MAX31855(PIN_SCK, CS_TC, PIN_MISO);

#define RREF      430.0   // נגד ייחוס בלוח ה-MAX31865 (בדרך כלל כתוב עליו 430)
#define RNOMINAL  100.0   // PT100 = 100 אוהם ב-0°C

// --- Manual bit-banged SPI register access, ל-softResetCycle() בלבד ---
void manualWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (b >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(2);
  }
}

uint8_t manualReadByte() {
  uint8_t b = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_SCK, HIGH);
    delayMicroseconds(2);
    b <<= 1;
    if (digitalRead(PIN_MISO)) b |= 1;
    digitalWrite(PIN_SCK, LOW);
    delayMicroseconds(2);
  }
  return b;
}

void manualWriteReg(uint8_t addr, uint8_t data) {
  digitalWrite(CS_PT100, LOW);
  manualWriteByte(addr | 0x80);
  manualWriteByte(data);
  digitalWrite(CS_PT100, HIGH);
}

uint16_t manualReadRTDraw() {
  digitalWrite(CS_PT100, LOW);
  manualWriteByte(0x01); // RTD MSB register, auto-increments to LSB
  uint8_t msb = manualReadByte();
  uint8_t lsb = manualReadByte();
  digitalWrite(CS_PT100, HIGH);
  return (((uint16_t)msb << 8) | lsb) >> 1; // drop fault bit
}

// ==========================================================================
// softResetCycle() — "איפוס-רך" ל-MAX31865, לפני כל קריאה
//
// למה זה נחוץ: ה-MAX31865 (וכנראה כל שבב מאותו batch/דגם בלוח) מחזיר קריאת
// RAW שגויה בעקביות (~28000 מתוך 32768, במקום ~8500 האמיתי) דרך מסלול
// ה-1-shot הרגיל של הספרייה (clearFault+enableBias+1SHOT), אלא אם כן
// "מאלצים" אותו למחזור auto-convert קצר בין הקריאות. אומת כיציב במשך
// 60+ שניות רצופות, הן על Arduino Uno (SPI חומרתי) והן על ESP32 (SPI
// תוכנתי, פינים 18/19/23/5) — לפני זה, כל ניסיון תיקון אחר (power-cycle,
// ניתוק פיזי, זמני discharge שונים, קריאה כפולה בלבד) נכשל.
//
// ⚠️ אזהרה: אל תסיר/תשנה את הפונקציה הזו בלי לבדוק מחדש יציבות של 60+
// שניות רצופות (RAW, resistance, טמפרטורה) — ההתנהגות הזו לא הייתה
// צפויה מראש ולא הוסברה באופן מלא ברמת ה-datasheet/hardware, רק אומתה
// אמפירית. יש להריץ אותה בכל מחזור לולאה, לפני קריאת pt100.temperature().
// ==========================================================================
void softResetCycle() {
  manualWriteReg(0x00, 0b11010000); // VBIAS=1, MODEAUTO=1, 3WIRE=1, 60Hz
  delay(100);
  for (int i = 0; i < 3; i++) {
    manualReadRTDraw();
    delay(50);
  }
  manualWriteReg(0x00, 0b00000000); // כיבוי bias+auto-convert בחזרה
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pt100.begin(MAX31865_3WIRE);

  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(CS_PT100, OUTPUT);
  pinMode(PIN_MISO, INPUT);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(CS_PT100, HIGH);

  Serial.println("בדיקת חיישנים התחילה...");
}

void loop() {
  softResetCycle(); // חובה להריץ לפני כל קריאה מה-PT100

  float ptTemp = pt100.temperature(RNOMINAL, RREF);
  double tcTemp = thermocouple.readCelsius();

  Serial.print("PT100: ");
  Serial.print(ptTemp);
  Serial.print(" °C   |   Type-K: ");
  // הלוח הפגום מחזיר בעקביות 0.00 (לא NaN) במקום להיכשל בבירור —
  // לכן מתייחסים גם ל-0.00 בדיוק כמצב שגיאה, לא כקריאה תקינה אמיתית
  if (isnan(tcTemp) || fabs(tcTemp) < 0.001) {
    Serial.println("שגיאת קריאה — לוח פגום, ממתין להחלפה");
  } else {
    Serial.print(tcTemp);
    Serial.println(" °C");
  }

  delay(1000);
}
