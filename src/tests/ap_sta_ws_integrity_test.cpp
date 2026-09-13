#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "wifi_credentials.h"

// ==========================================================================
// Follow-up to ap_sta_test.cpp: that test found the Serial log itself gets
// garbled under AP+STA radio load, and asked the real question - does that
// affect the actual WebSocket JSON messages a browser depends on, or is it
// a UART-only artifact? Serial has no error checking; TCP (which carries
// the WebSocket) has checksums at the WiFi/IP/TCP layers, so a corrupted
// segment should be silently dropped and retransmitted, never delivered
// wrong - in principle a fundamentally different risk. This test checks
// that empirically instead of arguing it from theory: same AP+STA radio
// load as ap_sta_test.cpp, but with a real WebSocketsServer pushing a
// sequence-numbered JSON message every 200ms (matching the real
// firmware's push cadence) to a real browser, which independently counts
// received messages, JSON.parse() failures, and sequence-number gaps
// (i.e. arrived-but-wrong OR dropped-and-never-arrived). The verification
// lives entirely in the browser's own JS state, not in anything printed
// over Serial, so this test's result doesn't depend on Serial being
// reliable at all.
//
// wifi_credentials.h holds the real WiFi SSID/password and is gitignored
// - see that file.
//
// This is a SEPARATE PlatformIO environment (ap_sta_ws_integrity_test) -
// flashing it replaces main.cpp on the chip while it runs. Reflash env
// esp32dev to get the real firmware back.
// ==========================================================================

const char *AP_SSID = "SmartBrew-Test";
const char *AP_PASSWORD = "brew1234";

WebServer server(80);
WebSocketsServer webSocket(81);

#define PUSH_INTERVAL_MS 200UL
unsigned long lastPushMs = 0;
uint32_t seqCounter = 0;

const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WS Integrity Test</title>
<style>
  body { font-family: sans-serif; background:#111; color:#eee; text-align:center; padding-top:2em; }
  .big { font-size: 2em; margin: 0.3em; }
  .label { font-size: 0.9em; color:#888; }
  .bad { color:#e74c3c; }
  .good { color:#2ecc71; }
</style>
</head>
<body>
  <h2>WebSocket Integrity Test</h2>
  <div id="conn">connecting...</div>
  <div class="label">messages received</div>
  <div class="big" id="received">0</div>
  <div class="label">JSON parse errors</div>
  <div class="big" id="parseErrors">0</div>
  <div class="label">sequence gaps (dropped messages)</div>
  <div class="big" id="gaps">0</div>
  <div class="label">WS disconnect count</div>
  <div class="big" id="disconnects">0</div>
  <div class="label">last sequence number seen</div>
  <div class="big" id="lastSeq">--</div>

<script>
let ws;
let received = 0, parseErrors = 0, gaps = 0, disconnects = 0, lastSeq = -1;

function paint() {
  document.getElementById('received').textContent = received;
  document.getElementById('parseErrors').textContent = parseErrors;
  document.getElementById('parseErrors').className = 'big ' + (parseErrors > 0 ? 'bad' : 'good');
  document.getElementById('gaps').textContent = gaps;
  document.getElementById('gaps').className = 'big ' + (gaps > 0 ? 'bad' : 'good');
  document.getElementById('disconnects').textContent = disconnects;
  document.getElementById('lastSeq').textContent = lastSeq === -1 ? '--' : lastSeq;
}

function connect() {
  const host = window.location.hostname;
  ws = new WebSocket('ws://' + host + ':81/');
  ws.onopen = () => { document.getElementById('conn').textContent = 'connected'; };
  ws.onclose = () => {
    disconnects++;
    document.getElementById('conn').textContent = 'disconnected - reconnecting...';
    paint();
    setTimeout(connect, 1000);
  };
  ws.onerror = () => { ws.close(); };
  ws.onmessage = (evt) => {
    received++;
    try {
      const msg = JSON.parse(evt.data);
      if (typeof msg.seq !== 'number') throw new Error('no seq field');
      if (lastSeq !== -1 && msg.seq !== lastSeq + 1) {
        gaps += Math.max(0, msg.seq - lastSeq - 1);
      }
      lastSeq = msg.seq;
    } catch (e) {
      parseErrors++;
    }
    paint();
  };
}
connect();
</script>
</body>
</html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t *payload, size_t length) {
  // No commands to handle - this test only pushes data, it never reads any.
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("[boot] AP+STA WebSocket integrity test starting");
  Serial.print("[boot] STA target: ");
  Serial.println(HOME_WIFI_SSID);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[boot] AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("[boot] Connect a phone to SmartBrew-Test, open http://192.168.4.1, watch the counters");

  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);

  server.on("/", handleRoot);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWsEvent);

  Serial.println("[boot] setup done - pushing a sequence-numbered message every 200ms");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  unsigned long now = millis();
  if (now - lastPushMs >= PUSH_INTERVAL_MS) {
    lastPushMs = now;
    String json = "{\"seq\":" + String(seqCounter) + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
    seqCounter++;
    webSocket.broadcastTXT(json);
  }
}
