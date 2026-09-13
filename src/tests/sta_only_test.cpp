// Isolates one variable: does "homesm2" (or whatever is currently saved)
// connect to this ESP32 at all as a PLAIN STA client, with no AP running
// and no channel-hint trickery? Reuses the network already saved in
// SPIFFS by the real firmware (main.cpp's /wifi_networks.txt format) so
// this test needs no new input - if this also gets stuck, the problem is
// not AP+STA channel-locking, it's something about this network/password/
// router itself.
#include <WiFi.h>
#include <SPIFFS.h>

#define WIFI_NETWORKS_FILE "/wifi_networks.txt"

wl_status_t lastStatus = (wl_status_t)255;
unsigned long startMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[sta-only-test] starting, STA ONLY (no AP) - reusing saved credentials from SPIFFS");

  if (!SPIFFS.begin(true)) {
    Serial.println("[sta-only-test] SPIFFS mount failed");
    return;
  }
  File f = SPIFFS.open(WIFI_NETWORKS_FILE, FILE_READ);
  if (!f) {
    Serial.println("[sta-only-test] no saved networks file found");
    return;
  }
  String line = f.readStringUntil('\n');
  f.close();
  if (line.length() > 0 && line.charAt(line.length() - 1) == '\r') {
    line.remove(line.length() - 1);
  }
  int tabIdx = line.indexOf('\t');
  if (tabIdx < 0) {
    Serial.println("[sta-only-test] saved networks file has unexpected format");
    return;
  }
  String ssid = line.substring(0, tabIdx);
  String password = line.substring(tabIdx + 1);
  Serial.print("[sta-only-test] read ssid=\"");
  Serial.print(ssid);
  Serial.print("\" password length=");
  Serial.println(password.length());

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.println("[sta-only-test] WiFi.begin() called, waiting for result...");
  startMs = millis();
}

void loop() {
  wl_status_t st = WiFi.status();
  if (st != lastStatus) {
    Serial.print("[sta-only-test] status changed to ");
    Serial.print((int)st);
    Serial.print(" at t+");
    Serial.print(millis() - startMs);
    Serial.println("ms");
    lastStatus = st;
    if (st == WL_CONNECTED) {
      Serial.print("[sta-only-test] CONNECTED! IP=");
      Serial.println(WiFi.localIP());
    }
  }
  delay(200);
}
