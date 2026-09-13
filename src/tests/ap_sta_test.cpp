#include <WiFi.h>
#include "wifi_credentials.h"

// ==========================================================================
// Standalone bench test: can this ESP32 run its own AP (SmartBrew-Test,
// same as the real firmware) and a STA connection to the home network at
// the same time, stably? See ap-sta-stability-test-spec.md - this is
// step 1 only, proving the base is stable before any UI/feature is built
// on top of it. Both AP and STA share the same single 2.4GHz radio
// (time-multiplexed by the WiFi driver), which is exactly the mechanism
// this test is checking for trouble in.
//
// wifi_credentials.h holds the real home WiFi SSID/password and is
// gitignored - it must never be committed. See that file for how to fill
// it in.
//
// This is a SEPARATE PlatformIO environment (ap_sta_test) - flashing it
// replaces main.cpp on the chip while it runs. Reflash env esp32dev to
// get the real firmware back.
// ==========================================================================

const char *AP_SSID = "SmartBrew-Test";
const char *AP_PASSWORD = "brew1234";

#define LOG_INTERVAL_MS 10000UL
#define TEST_DURATION_MS (10UL * 60UL * 1000UL)

unsigned long lastLogMs = 0;
bool summaryPrinted = false;

// Counted from WiFi events, not sampled at each 10s log line - a quick
// disconnect/reconnect between two log ticks would otherwise never show
// up at all.
volatile uint32_t staDisconnectEvents = 0;
volatile uint32_t staConnectEvents = 0;
volatile uint32_t apClientConnectEvents = 0;
volatile uint32_t apClientDisconnectEvents = 0;

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      staConnectEvents++;
      Serial.print("[event] STA connected to home network (count=");
      Serial.print(staConnectEvents);
      Serial.println(")");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[event] STA got IP: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staDisconnectEvents++;
      Serial.print("[event] STA DISCONNECTED from home network (count=");
      Serial.print(staDisconnectEvents);
      Serial.println(") - reconnecting");
      // Arduino-ESP32 doesn't always auto-retry a STA disconnect on its
      // own - ask explicitly so a single drop doesn't end the test early
      // and skew the "how many disconnects" count from the real question
      // ("does it happen at all").
      WiFi.reconnect();
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      apClientConnectEvents++;
      Serial.println("[event] a device connected to our AP (SmartBrew-Test)");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      apClientDisconnectEvents++;
      Serial.println("[event] a device disconnected from our AP");
      break;
    default:
      break;
  }
}

const char *staStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    default: return "OTHER";
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("[boot] AP+STA stability test starting");
  Serial.println("[boot] AP: SmartBrew-Test (same as real firmware)");
  Serial.print("[boot] STA target: ");
  Serial.println(HOME_WIFI_SSID);

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[boot] AP IP: ");
  Serial.println(WiFi.softAPIP());

  // TEMP diagnostic: list every 2.4GHz network actually visible before
  // trying to connect - answers "can it see the target SSID at all" (name
  // mismatch, still off, out of range, hidden) directly instead of
  // guessing from a NO_SSID_AVAIL status alone. Remove once resolved.
  Serial.println("[scan] scanning for visible networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[scan] no networks found at all");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.print("[scan] SSID=\"");
      Serial.print(WiFi.SSID(i));
      Serial.print("\" RSSI=");
      Serial.print(WiFi.RSSI(i));
      Serial.print("dBm channel=");
      Serial.println(WiFi.channel(i));
    }
  }
  WiFi.scanDelete();

  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
  Serial.println("[boot] setup done - logging every 10s for 10 minutes");
}

void loop() {
  unsigned long now = millis();

  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;
    wl_status_t staStatus = WiFi.status();

    Serial.print("[status] up=");
    Serial.print(now / 1000);
    Serial.print("s  STA=");
    Serial.print(staStatusName(staStatus));
    if (staStatus == WL_CONNECTED) {
      Serial.print(" RSSI=");
      Serial.print(WiFi.RSSI());
      Serial.print("dBm IP=");
      Serial.print(WiFi.localIP());
    }
    Serial.print("  AP_clients=");
    Serial.print(WiFi.softAPgetStationNum());
    Serial.print("  totalSTADisconnects=");
    Serial.print(staDisconnectEvents);
    Serial.println();
  }

  if (!summaryPrinted && now >= TEST_DURATION_MS) {
    summaryPrinted = true;
    Serial.println("========================================");
    Serial.println("[summary] 10-minute AP+STA stability test complete");
    Serial.print("[summary] STA disconnect events: ");
    Serial.println(staDisconnectEvents);
    Serial.print("[summary] STA (re)connect events: ");
    Serial.println(staConnectEvents);
    Serial.print("[summary] AP client connect events: ");
    Serial.println(apClientConnectEvents);
    Serial.print("[summary] AP client disconnect events: ");
    Serial.println(apClientDisconnectEvents);
    Serial.print("[summary] final STA status: ");
    Serial.println(staStatusName(WiFi.status()));
    Serial.println("========================================");
  }
}
