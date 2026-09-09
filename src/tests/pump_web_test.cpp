#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_MAX31865.h>

// ==========================================================================
// Standalone bench test: web page with 2 relay ON/OFF buttons + 2 live PT100
// readings. No SSR, no PID, no recipe logic - proves the ESP32's WiFi/web
// side can talk to a browser and to the hardware that's already wired,
// before building anything more complex on top.
//
// 2026-09-07: upgraded from HTTP polling (fetch('/status') every 1s) to a
// real WebSocket on port 81, matching UI-decisions.txt's design (JSON
// pushed from the ESP32 every second, commands sent from the browser over
// the same socket). Everything else - pins, calibration, relay logic,
// safety warnings - is unchanged from the polling version. Control is
// entirely from the browser: no physical button/encoder input exists on
// the controller itself (EC11 encoder removed 2026-08-28, its old pins are
// now taken by PT100 board B's SPI lines - see DECISIONS.md).
//
// Pins/hardware here match what's already confirmed working in src/main.cpp
// - checked against that file directly, not assumed:
//   PT100 board A: SCK=18 MISO=19 MOSI=14 CS=5
//   PT100 board B: SCK=16 MISO=17 MOSI=4  CS=22
//   Relay 1: GPIO26   Relay 2: GPIO25   (BESTEP JQC3F-05VDC-C, HIGH-level
//   trigger - these are the SAME two relay modules already wired and
//   bench-tested in main.cpp section 6c of notes/session-2026-09-04-summary.md,
//   just switched from an automatic 3s cycle to on-demand web control here.)
//
// This is a SEPARATE PlatformIO environment (pump_web_test) - flashing it
// replaces main.cpp on the chip while it runs. Reflash env esp32dev to get
// the full PT100+LCD+SSR1/2+relays-latched-off system back.
//
// Library added for this upgrade: "links2004/WebSockets" (WebSocketsServer).
// Added to platformio.ini lib_deps for the pump_web_test environment:
//   links2004/WebSockets @ ^2.4.1
//
// ⚠️ SAFETY (per notes/DECISIONS.md):
//  - Relay 2's real-world load is documented as 230V AC mains. Do NOT wire
//    a real pump to the relay outputs until an RCD/GFCI breaker on that
//    circuit is confirmed present - DECISIONS.md flags this as unverified.
//  - This test only proves the GPIO->relay coil logic side. It never
//    touches the relay OUTPUT (contact) side, which is what would carry
//    mains voltage to a real pump.
//  - If a relay does the opposite of what its button says (turns ON when
//    you press OFF, or vice versa), see the ACTIVE_LOW note below - some
//    relay boards are wired active-low and need one line flipped.
// ==========================================================================

// --- WiFi Access Point ---
// Local bench test only - not the final network design. Password must be
// 8+ characters for ESP32 softAP to accept it.
const char *AP_SSID = "SmartBrew-Test";
const char *AP_PASSWORD = "brew1234";

// --- Relays ---
#define RELAY1_PIN 26
#define RELAY2_PIN 25

// Set to true if a relay does the opposite of what the web page says -
// i.e. the module is wired active-LOW (IN pulled low = coil energised).
// Confirmed active-HIGH in main.cpp's bench test, so this starts false.
#define RELAYS_ACTIVE_LOW false

#if RELAYS_ACTIVE_LOW
#define RELAY_ON_LEVEL  LOW
#define RELAY_OFF_LEVEL HIGH
#else
#define RELAY_ON_LEVEL  HIGH
#define RELAY_OFF_LEVEL LOW
#endif

// --- PT100 sensors (software SPI), pins matching main.cpp exactly ---
#define SCK_A   18
#define MISO_A  19
#define MOSI_A  14
#define CS_A    5

#define SCK_B   16
#define MISO_B  17
#define MOSI_B  4
#define CS_B    22

#define RREF     430.0
#define RNOMINAL 100.0
#define WIRES    MAX31865_2WIRE

// Same two-point calibration as main.cpp (notes/pt100-sensors-and-calibration.md)
struct Calibration { float scale; float offset; };
const Calibration CAL_A = {0.977977f, -1.279f};
const Calibration CAL_B = {0.976812f, -0.696f};

Adafruit_MAX31865 pt100_a = Adafruit_MAX31865(CS_A, MOSI_A, MISO_A, SCK_A);
Adafruit_MAX31865 pt100_b = Adafruit_MAX31865(CS_B, MOSI_B, MISO_B, SCK_B);

WebServer server(80);
WebSocketsServer webSocket(81);

bool relay1On = false;
bool relay2On = false;

// How often status is pushed to every connected browser, unprompted -
// matches UI-decisions.txt (WebSocket, JSON every second). A relay toggle
// also triggers an immediate extra push (see onWsEvent) so the button
// state updates right away instead of waiting up to 1s.
#define STATUS_PUSH_MS 1000UL
unsigned long lastStatusPushMs = 0;

// Same Callendar-Van Dusen conversion as main.cpp - kept standalone on
// purpose, matching this project's convention of independent bench-test
// files that don't share code with main.cpp (see pt100_only_test.cpp).
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

// Simple, no retry/median/slew filtering (unlike main.cpp) - this test is
// about proving connectivity, not long-run sensor hardening. Returns NAN on
// any fault or an out-of-range raw value, same thresholds as main.cpp.
float readTempOnce(Adafruit_MAX31865 &sensor, const Calibration &cal) {
  uint16_t raw = sensor.readRTD();
  uint8_t fault = sensor.readFault();
  if (fault) {
    sensor.clearFault();
    return NAN;
  }
  if (raw == 0 || raw >= 32767) return NAN;

  float rMeasured = (raw / 32768.0) * RREF;
  float rTrue = cal.scale * rMeasured + cal.offset;
  float t = resistanceToCelsius(rTrue);
  if (isnan(t) || t < -50.0 || t > 200.0) return NAN;
  return t;
}

// dir="rtl" page, WebSocket client instead of fetch()-polling. Connects to
// ws://<device-ip>:81/ - same host the page was served from, different
// port. Sends the bare strings "toggle1"/"toggle2" as commands; the ESP32
// replies to everyone with a fresh status JSON right away, so all open
// tabs/phones stay in sync with each other, not just with their own click.
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>בדיקת משאבות</title>
<style>
  body { font-family: sans-serif; background:#111; color:#eee; text-align:center; padding-top:2em; }
  .temp { font-size: 2.2em; margin: 0.3em; }
  .row { margin: 1.5em; }
  button {
    font-size: 1.3em; padding: 0.6em 1.4em; margin: 0.4em;
    border-radius: 10px; border: none; cursor: pointer;
  }
  .on  { background:#2ecc71; color:#063; }
  .off { background:#555; color:#ccc; }
  #conn { font-size: 0.9em; color:#888; }
</style>
</head>
<body>
  <h2>בדיקת חיבור — משאבות + חיישנים</h2>
  <div id="conn">מתחבר...</div>
  <div class="row">
    <div class="temp">PT1: <span id="t1">--</span>&deg;C</div>
    <div class="temp">PT2: <span id="t2">--</span>&deg;C</div>
  </div>
  <div class="row">
    <div>משאבה 1</div>
    <button id="b1" onclick="toggle(1)">--</button>
  </div>
  <div class="row">
    <div>משאבה 2</div>
    <button id="b2" onclick="toggle(2)">--</button>
  </div>
  <p id="err" style="color:#e74c3c;"></p>

<script>
let ws;
let reconnectTimer = null;

function paint(s) {
  document.getElementById('t1').textContent = s.pt1 === null ? "ERR" : s.pt1.toFixed(1);
  document.getElementById('t2').textContent = s.pt2 === null ? "ERR" : s.pt2.toFixed(1);
  const b1 = document.getElementById('b1');
  const b2 = document.getElementById('b2');
  b1.textContent = s.relay1 ? "דלוק - כבה" : "כבוי - הדלק";
  b1.className = s.relay1 ? "on" : "off";
  b2.textContent = s.relay2 ? "דלוק - כבה" : "כבוי - הדלק";
  b2.className = s.relay2 ? "on" : "off";
  document.getElementById('err').textContent = "";
}

function connect() {
  const host = window.location.hostname;
  ws = new WebSocket("ws://" + host + ":81/");

  ws.onopen = () => {
    document.getElementById('conn').textContent = "מחובר";
  };
  ws.onclose = () => {
    document.getElementById('conn').textContent = "אין קשר לבקר - מנסה שוב...";
    document.getElementById('err').textContent = "אין קשר לבקר";
    // Simple fixed-delay retry - good enough for a bench test on one
    // known-good AP; a jittered/backoff retry isn't worth the complexity
    // here.
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, 1500);
  };
  ws.onerror = () => { ws.close(); };
  ws.onmessage = (evt) => {
    try {
      paint(JSON.parse(evt.data));
    } catch (e) {
      // Malformed frame - ignore this one, next push corrects it.
    }
  };
}

function toggle(n) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send('toggle' + n);
  }
}

connect();
</script>
</body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

// Builds the same JSON shape the old /status endpoint returned - only the
// transport changed, not the data.
String buildStatusJson() {
  float t1 = readTempOnce(pt100_a, CAL_A);
  float t2 = readTempOnce(pt100_b, CAL_B);

  String json = "{";
  json += "\"pt1\":" + (isnan(t1) ? String("null") : String(t1, 1)) + ",";
  json += "\"pt2\":" + (isnan(t2) ? String("null") : String(t2, 1)) + ",";
  json += "\"relay1\":" + String(relay1On ? "true" : "false") + ",";
  json += "\"relay2\":" + String(relay2On ? "true" : "false");
  json += "}";
  return json;
}

// Sends the current status to every connected browser tab/phone, not just
// the one that triggered it - so two open tabs never disagree about
// whether a relay is on.
void broadcastStatus() {
  String json = buildStatusJson();
  webSocket.broadcastTXT(json);
}

// type/payload/length match the WebSocketsServer callback signature. Only
// WStype_TEXT is handled - a client sends nothing but the two toggle
// commands, so anything else (including binary frames) is ignored rather
// than guessed at.
void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      // Push status immediately to the client that just joined, instead of
      // making it wait up to STATUS_PUSH_MS for the next scheduled tick.
      {
        String json = buildStatusJson();
        webSocket.sendTXT(clientNum, json);
      }
      break;

    case WStype_TEXT: {
      String msg = String((char *)payload).substring(0, length);
      if (msg == "toggle1") {
        relay1On = !relay1On;
        digitalWrite(RELAY1_PIN, relay1On ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
        broadcastStatus();
      } else if (msg == "toggle2") {
        relay2On = !relay2On;
        digitalWrite(RELAY2_PIN, relay2On ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
        broadcastStatus();
      }
      // Any other text is silently ignored - unrecognised commands must
      // never be guessed at, only known-safe ones act on the relays.
      break;
    }

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Relays off first, before anything else that could stall setup().
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, RELAY_OFF_LEVEL);
  digitalWrite(RELAY2_PIN, RELAY_OFF_LEVEL);

  pinMode(CS_A, OUTPUT);
  pinMode(CS_B, OUTPUT);
  digitalWrite(CS_A, HIGH);
  digitalWrite(CS_B, HIGH);
  pt100_a.begin(WIRES);
  pt100_b.begin(WIRES);

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP started. Connect to WiFi \"");
  Serial.print(AP_SSID);
  Serial.print("\", password \"");
  Serial.print(AP_PASSWORD);
  Serial.println("\"");
  Serial.print("Then open http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("WebSocket on port 81 (same IP).");

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started (port 80, page only).");

  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  Serial.println("WebSocket server started (port 81).");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  if (millis() - lastStatusPushMs >= STATUS_PUSH_MS) {
    lastStatusPushMs = millis();
    broadcastStatus();
  }
}
