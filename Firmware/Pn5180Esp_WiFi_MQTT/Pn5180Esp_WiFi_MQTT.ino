/**************************************************
  PN5180 ESP Reader — WiFi + MQTT edition
  Based on StefanRiese/PN5180_ESP_Reader (original serial-only sketch)
  Adds: WiFi connection, MQTT publishing, and Home Assistant
        MQTT Tag discovery, so the reader works standalone
        (no host PC / serial polling required).

  The serial interface (commands v/u/l/i) behaves exactly like the
  original Pn5180Esp.ino sketch - only a 'w' command (WiFi/MQTT status)
  was added on top.

  Libraries needed (install via Arduino Library Manager):
    - PN5180 library (whatever you already used - e.g. playfultechnology/PN5180-Library)
    - Adafruit NeoPixel
    - PubSubClient (by Nick O'Leary)      <-- NEW
    - ESP8266WiFi (bundled with ESP8266 board package) <-- NEW

  WiFi/MQTT config: copy secrets.h.example (in this same folder) to
  secrets.h and fill in your own values there. secrets.h is gitignored,
  so your real credentials never get committed to this public repo.
  See the README for details.
**************************************************/
#include <PN5180ISO15693.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Ticker.h>

#include <PubSubClient.h>

// The default 256-byte buffer is too small for our HA discovery JSON
// payload + topic combined, which makes mqtt.publish() silently fail
// with no error. PubSubClient.cpp is compiled as its own translation
// unit, so a #define here wouldn't reach it - the buffer must be
// grown at runtime instead, via setBufferSize() in setup().
#define MQTT_BUFFER_SIZE 512

// WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, MQTT_PORT, MQTT_USER,
// MQTT_PASSWORD and LOCATION_NAME are defined in secrets.h (gitignored -
// copy secrets.h.example to secrets.h and edit it, see README)
#include "secrets.h"

// Derived automatically at boot - do not edit
String DEVICE_ID;      // e.g. "living_room_3fa2c1"  (unique per physical chip)
String DEVICE_NAME;    // e.g. "PN5180 Reader (living_room)"

// Derived topics - no need to edit these
String TOPIC_SCAN;        // where scanned UIDs get published
String TOPIC_AVAILABILITY; // online/offline (LWT)
String TOPIC_DISCOVERY;   // HA discovery config topic
String TOPIC_HEARTBEAT;   // periodic "still actually running" uptime signal
String TOPIC_HEARTBEAT_DISCOVERY; // HA discovery config for the heartbeat sensor
String TOPIC_VERSION;     // firmware version, as an HA entity
String TOPIC_VERSION_DISCOVERY;   // HA discovery config for the version sensor

/**************************************************
  Existing defines / pins (unchanged from original sketch)
**************************************************/
#define VERSION "2.1.0"
#define SKETCHNAME "Pn5180Esp"
#define LED_BRIGHTNESS 10
// Much dimmer than LED_BRIGHTNESS, so the "still alive" heartbeat blip
// doesn't compete visually with an actual tag-recognized flash
#define HEARTBEAT_BRIGHTNESS 1
// Tag-recognized flash: a distinct color (cyan) and noticeably longer
// than the heartbeat, so a real scan is unmistakable at a glance
#define TAG_FLASH_DURATION_MS 500

#if defined(ARDUINO_ARCH_ESP8266)
  #define PN5180_NSS 4
  #define PN5180_BUSY 16
  #define PN5180_RST 5
  #define WS2812B_PIN D8
#elif defined(ARDUINO_ARCH_ESP32)
  #define PN5180_NSS  16
  #define PN5180_BUSY 5
  #define PN5180_RST  17
#else
  #error Please define your pinout here! (this sketch targets ESP8266/ESP32 for WiFi support)
#endif

// How often to poll the reader for a tag, ms
#define POLL_INTERVAL_MS 500
// Minimum time the SAME tag must be physically away from the reader
// before it's allowed to trigger onTagScanned() again, ms
#define TAG_AWAY_THRESHOLD_MS 5000
// How often to blip the status LED to show the reader is still alive, ms
#define HEARTBEAT_INTERVAL_MS 2000
// How often to publish an MQTT heartbeat (device uptime) so the broker
// side can detect "connected but actually hung", not just "disconnected"
#define MQTT_HEARTBEAT_INTERVAL_MS 30000

// Safety net: the PN5180 library's SPI busy-wait loops (waiting on the
// BUSY pin / IRQ status) have no timeout at all, so a marginal SPI
// transaction can hang loop() forever with no recovery - not even the
// ESP8266's own watchdog reliably catches this, since these loops keep
// calling digitalRead()/getIRQStatus() often enough to look "alive" to
// it. This app-level watchdog runs on its own hardware timer (Ticker),
// independent of loop(), and force-restarts the board if loop() hasn't
// checked in within WATCHDOG_TIMEOUT_MS.
#define WATCHDOG_TIMEOUT_MS 8000
#define WATCHDOG_CHECK_INTERVAL_S 1

/**************************************************
  Globals
**************************************************/
PN5180ISO15693 nfc15693(PN5180_NSS, PN5180_BUSY, PN5180_RST);
uint8_t password[]  = {0x0F, 0x0F, 0x0F, 0x0F};
uint8_t password2[] = {0x5B, 0x6E, 0xFD, 0x7F};
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(1, WS2812B_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient espClient;
PubSubClient mqtt(espClient);

Ticker watchdogTicker;
volatile unsigned long lastLoopFeedMillis = 0;

char lastUid[17] = ""; // 16 hex chars + null terminator; fixed buffer to
                        // avoid churning the heap with a new String every
                        // ~500ms poll while a tag sits on the reader
unsigned long tagAbsentSinceMillis = 0; // when the tag last became absent
unsigned long lastPollMillis = 0;
unsigned long lastHeartbeatMillis = 0;
unsigned long lastMqttHeartbeatMillis = 0;
bool tagPresentLastPoll = false;

// --- Reconnect state (non-blocking, with exponential backoff) ---
unsigned long lastWifiAttempt = 0;
unsigned long wifiRetryInterval = 5000;     // starts at 5s
unsigned long lastMqttAttempt = 0;
unsigned long mqttRetryInterval = 5000;     // starts at 5s
const unsigned long RETRY_INTERVAL_MAX = 60000; // caps backoff at 60s
bool wifiWasConnected = false; // tracks connect/disconnect transitions, for mDNS restart

/*************************************
  Setup
*************************************/
void setup()
{
  pixels.begin();
  ledFeedback(LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS, 100);

  // Arm the watchdog before anything that touches the PN5180 (including
  // the init below), since its SPI busy-waits can hang during boot too.
  lastLoopFeedMillis = millis();
  watchdogTicker.attach(WATCHDOG_CHECK_INTERVAL_S, checkWatchdog);

  Serial.setTimeout(50);
  Serial.begin(115200);
  Serial.println(F("=================================="));
  Serial.println(F("PN5180 NFC reader - WiFi/MQTT build"));

  // Build a unique, stable device ID: location label + chip's own
  // unique hardware ID (ESP.getChipId()), so no two physical readers
  // can ever collide even if LOCATION_NAME is duplicated by mistake.
  char chipIdHex[9];
  sprintf(chipIdHex, "%06x", ESP.getChipId());
  DEVICE_ID = String(LOCATION_NAME) + "_" + String(chipIdHex);
  DEVICE_NAME = "PN5180 Reader (" + String(LOCATION_NAME) + ")";

  // Build topic strings once
  TOPIC_SCAN        = DEVICE_ID + "/tag_scanned";
  TOPIC_AVAILABILITY = DEVICE_ID + "/status";
  TOPIC_DISCOVERY   = "homeassistant/tag/" + DEVICE_ID + "/config";
  TOPIC_HEARTBEAT   = DEVICE_ID + "/heartbeat";
  TOPIC_HEARTBEAT_DISCOVERY = "homeassistant/sensor/" + DEVICE_ID + "_heartbeat/config";
  TOPIC_VERSION     = DEVICE_ID + "/version";
  TOPIC_VERSION_DISCOVERY = "homeassistant/sensor/" + DEVICE_ID + "_version/config";

  // --- PN5180 init (same as original) ---
  nfc15693.begin();
  nfc15693.reset();

  uint8_t productVersion[2];
  nfc15693.readEEprom(PRODUCT_VERSION, productVersion, sizeof(productVersion));
  if (0xff == productVersion[1]) {
    Serial.println(F("PN5180 init failed! Halting."));
    ledFeedback(LED_BRIGHTNESS, 0, 0, 2000);
    while (true) { delay(1000); }
  }
  nfc15693.setupRF();
  Serial.println(F("PN5180 ready."));

  // --- WiFi + MQTT ---
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false); // don't wear out flash with every reconnect
  connectWiFi();
  mqtt.setBufferSize(MQTT_BUFFER_SIZE); // large enough for the HA discovery JSON payload
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(3); // seconds - keep a dead broker from stalling loop() too long
  connectMqtt();
}

/*************************************
  Loop
*************************************/
void loop()
{
  lastLoopFeedMillis = millis(); // tell the watchdog we're still alive

  // Keep network alive - non-blocking, with backoff so an outage
  // doesn't freeze tag polling or hammer the router/broker
  maintainWiFi();
  maintainMqtt();
  mqtt.loop();
  MDNS.update();

  // Serial debug interface still works, identical to the original sketch
  serialInterface();

  // Poll the reader on an interval instead of blocking
  unsigned long now = millis();
  if (now - lastPollMillis >= POLL_INTERVAL_MS) {
    lastPollMillis = now;
    pollTag();
  }

  // Brief periodic LED blip so you can tell at a glance the reader is
  // still alive and what it's connected to, without needing serial:
  // green = WiFi+MQTT ok, amber = WiFi ok but MQTT down, red = WiFi down
  if (now - lastHeartbeatMillis >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMillis = now;
    heartbeat();
  }

  // Periodic MQTT heartbeat (device uptime) so the broker/HA side can
  // tell "actually still running" from "connected but silently hung" -
  // the availability topic alone only updates on connect/disconnect.
  if (mqtt.connected() && now - lastMqttHeartbeatMillis >= MQTT_HEARTBEAT_INTERVAL_MS) {
    lastMqttHeartbeatMillis = now;
    publishHeartbeat();
  }
}

// Runs on its own hardware timer (Ticker), independent of loop() - so it
// still fires even if loop() is stuck inside a PN5180 SPI busy-wait that
// never returns. Restarts the board if loop() hasn't checked in recently.
void checkWatchdog()
{
  if (millis() - lastLoopFeedMillis > WATCHDOG_TIMEOUT_MS) {
    ESP.restart();
  }
}

// Publishes device uptime (seconds since last boot) to TOPIC_HEARTBEAT,
// retained, so a newly-subscribed client (e.g. Home Assistant restarting)
// immediately sees the last known value instead of waiting for the next
// interval. A stale value, or one that unexpectedly resets to a small
// number, is visible proof the device hung or rebooted.
void publishHeartbeat()
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
  mqtt.publish(TOPIC_HEARTBEAT.c_str(), buf, true);
}

void heartbeat()
{
  if (WiFi.status() != WL_CONNECTED) {
    ledFeedback(HEARTBEAT_BRIGHTNESS, 0, 0, 20);
  } else if (!mqtt.connected()) {
    ledFeedback(HEARTBEAT_BRIGHTNESS, HEARTBEAT_BRIGHTNESS, 0, 20);
  } else {
    ledFeedback(0, HEARTBEAT_BRIGHTNESS, 0, 20);
  }
}

/**************************************************
  WiFi connection
**************************************************/
void connectWiFi()
{
  Serial.print(F("Connecting to WiFi"));
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEVICE_ID.c_str()); // shows up as this name in your router's client list
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    lastLoopFeedMillis = millis(); // this loop alone can run longer than WATCHDOG_TIMEOUT_MS
    delay(250);
    Serial.print(".");
    ledFeedback(0, 0, LED_BRIGHTNESS, 100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(F("WiFi connected, IP="));
    Serial.println(WiFi.localIP());

    if (MDNS.begin(DEVICE_ID.c_str())) {
      Serial.print(F("Reachable at: "));
      Serial.print(DEVICE_ID);
      Serial.println(F(".local"));
    }
    wifiWasConnected = true; // so maintainWiFi() doesn't redo this on the next loop()
  } else {
    Serial.println();
    Serial.println(F("WiFi connect failed, will retry in loop()"));
  }
}

/**************************************************
  Non-blocking reconnect, called every loop() iteration.
  WiFi.begin() itself is asynchronous - we just need to (re)kick it
  off occasionally and let it connect in the background, rather than
  sitting in a blocking while-loop like the boot-time connectWiFi().
**************************************************/
void maintainWiFi()
{
  if (WiFi.status() == WL_CONNECTED) {
    wifiRetryInterval = 5000; // reset backoff once healthy again
    if (!wifiWasConnected) {
      // just came back online - mDNS needs re-announcing
      MDNS.begin(DEVICE_ID.c_str());
      wifiWasConnected = true;
    }
    return;
  }
  wifiWasConnected = false;

  unsigned long now = millis();
  if (now - lastWifiAttempt >= wifiRetryInterval) {
    lastWifiAttempt = now;
    ledFeedback(LED_BRIGHTNESS, 0, 0, 50); // quick red flash, non-blocking-ish
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // back off so we don't hammer the router if it stays down
    wifiRetryInterval = min(wifiRetryInterval * 2, RETRY_INTERVAL_MAX);
  }
}

void maintainMqtt()
{
  if (WiFi.status() != WL_CONNECTED) return; // nothing to do without WiFi
  if (mqtt.connected()) {
    mqttRetryInterval = 5000; // reset backoff once healthy again
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttAttempt >= mqttRetryInterval) {
    lastMqttAttempt = now;
    connectMqtt(); // single attempt; capped at 3s by setSocketTimeout()
    // back off so we don't hammer the broker if it stays down
    mqttRetryInterval = min(mqttRetryInterval * 2, RETRY_INTERVAL_MAX);
  }
}

/**************************************************
  MQTT connection + Home Assistant discovery publish
**************************************************/
void connectMqtt()
{
  if (WiFi.status() != WL_CONNECTED) return;

  // Stable (not randomized) so a reconnect makes the broker take over the
  // old session immediately instead of leaving it to linger until its own
  // keepalive times out - a lingering old session's Last Will would later
  // overwrite our fresh "online" status back to "offline".
  String clientId = DEVICE_ID;

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                       TOPIC_AVAILABILITY.c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), NULL, NULL,
                       TOPIC_AVAILABILITY.c_str(), 0, true, "offline");
  }

  if (ok) {
    mqtt.publish(TOPIC_AVAILABILITY.c_str(), "online", true);
    publishDiscovery();
    publishHeartbeatDiscovery();
    publishVersionDiscovery();
    publishVersion();
    publishHeartbeat();
    lastMqttHeartbeatMillis = millis(); // don't immediately re-fire in loop()
  }
}

// Publishes the retained MQTT Discovery config so Home Assistant
// auto-creates a "tag" scanner for this device - no YAML needed.
// Shared "device" block so the tag scanner, heartbeat sensor, and
// version sensor all group under the same device entry in Home
// Assistant (same identifiers = same device).
String deviceBlockJson()
{
  return String("{") +
    "\"identifiers\":[\"" + DEVICE_ID + "\"]," +
    "\"name\":\"" + DEVICE_NAME + "\"," +
    "\"manufacturer\":\"DIY\"," +
    "\"model\":\"PN5180 ESP Reader\"," +
    "\"sw_version\":\"" + VERSION + "\"" +
  "}";
}

void publishDiscovery()
{
  String payload = String("{") +
    "\"topic\":\"" + TOPIC_SCAN + "\"," +
    "\"availability_topic\":\"" + TOPIC_AVAILABILITY + "\"," +
    "\"device\":" + deviceBlockJson() +
  "}";

  mqtt.publish(TOPIC_DISCOVERY.c_str(), payload.c_str(), true); // retained!
}

// Publishes HA sensor discovery for the heartbeat topic, so it shows up
// as a proper entity (grouped under the same device) instead of just a
// raw MQTT topic.
void publishHeartbeatDiscovery()
{
  String payload = String("{") +
    "\"name\":\"Heartbeat\"," +
    "\"unique_id\":\"" + DEVICE_ID + "_heartbeat\"," +
    "\"state_topic\":\"" + TOPIC_HEARTBEAT + "\"," +
    "\"availability_topic\":\"" + TOPIC_AVAILABILITY + "\"," +
    "\"unit_of_measurement\":\"s\"," +
    "\"icon\":\"mdi:heart-pulse\"," +
    "\"entity_category\":\"diagnostic\"," +
    "\"device\":" + deviceBlockJson() +
  "}";

  mqtt.publish(TOPIC_HEARTBEAT_DISCOVERY.c_str(), payload.c_str(), true);
}

// Publishes HA sensor discovery for the firmware version, so it shows
// up as an entity (grouped under the same device), not just the
// device-info page's static "Firmware" field.
void publishVersionDiscovery()
{
  String payload = String("{") +
    "\"name\":\"Firmware Version\"," +
    "\"unique_id\":\"" + DEVICE_ID + "_version\"," +
    "\"state_topic\":\"" + TOPIC_VERSION + "\"," +
    "\"availability_topic\":\"" + TOPIC_AVAILABILITY + "\"," +
    "\"icon\":\"mdi:chip\"," +
    "\"entity_category\":\"diagnostic\"," +
    "\"device\":" + deviceBlockJson() +
  "}";

  mqtt.publish(TOPIC_VERSION_DISCOVERY.c_str(), payload.c_str(), true);
}

void publishVersion()
{
  mqtt.publish(TOPIC_VERSION.c_str(), VERSION, true);
}

/**************************************************
  Tries to disable ISO15693 privacy mode (e.g. NXP ICODE SLIX2 tags,
  the kind used by Tonies-style figurines) so a subsequent Inventory
  command will actually get a response. Tries the site-specific
  password first, falls back to the factory default.
  Harmless to call on tags that don't use privacy mode at all.
  Uses the same 50ms settle delay as the original sketch's 'u' handler.
**************************************************/
bool tryUnlockPrivacy()
{
  nfc15693.reset();
  nfc15693.setupRF();
  delay(50);

  if (ISO15693_EC_OK == nfc15693.unlockICODESLIX2(password2)) {
    return true;
  }

  nfc15693.reset();
  nfc15693.setupRF();
  delay(50);

  return (ISO15693_EC_OK == nfc15693.unlockICODESLIX2(password));
}

/**************************************************
  Tag polling — runs automatically in loop() for standalone operation,
  independent of the manual 'i' serial command below.
**************************************************/
void pollTag()
{
  uint8_t uid[10];

  // Unlock first - a privacy-locked tag (e.g. ICODE SLIX2 / Tonies-style
  // figurines) won't answer Inventory at all otherwise. Harmless on tags
  // that aren't privacy-locked.
  tryUnlockPrivacy();

  nfc15693.reset();
  nfc15693.setupRF();
  delay(20);

  ISO15693ErrorCode rc = nfc15693.getInventory(uid);

  if (rc == ISO15693_EC_OK && !isPlausibleUid(uid)) {
    // The chip reported success but the UID looks corrupted (e.g. a
    // weak/partial RF read during a collision or a tag leaving the
    // field mid-read) - treat it the same as "no tag" rather than
    // publishing garbage.
    rc = ISO15693_EC_UNKNOWN_ERROR;
  }

  if (rc == ISO15693_EC_OK) {
    char uidBuf[17];
    uidToBuffer(uid, uidBuf);
    bool isNewTag = (strcmp(uidBuf, lastUid) != 0);

    if (isNewTag) {
      // a different tag than last time - always trigger
      strcpy(lastUid, uidBuf);
      onTagScanned(uidBuf);
    } else if (!tagPresentLastPoll) {
      // same tag as before, but it had been lifted off - only
      // re-trigger if it was actually away for long enough
      unsigned long awayDuration = millis() - tagAbsentSinceMillis;
      if (awayDuration >= TAG_AWAY_THRESHOLD_MS) {
        onTagScanned(uidBuf);
      }
      // else: put back too soon, ignored - still just sitting there as far as HA is concerned
    }
    // if tagPresentLastPoll was already true and it's the same tag,
    // it's just still sitting on the reader - nothing to do

    tagPresentLastPoll = true;
  } else {
    // No tag currently on the reader
    if (tagPresentLastPoll) {
      tagAbsentSinceMillis = millis(); // mark the moment it disappeared
    }
    tagPresentLastPoll = false;
  }
}

void onTagScanned(const char* uid)
{
  ledFeedback(0, LED_BRIGHTNESS, LED_BRIGHTNESS, TAG_FLASH_DURATION_MS); // cyan: tag recognized

  if (mqtt.connected()) {
    bool sent = mqtt.publish(TOPIC_SCAN.c_str(), uid, false); // not retained
    if (!sent) {
      ledFeedback(LED_BRIGHTNESS, LED_BRIGHTNESS, 0, 300); // amber: publish failed
    }
  } else {
    ledFeedback(LED_BRIGHTNESS, LED_BRIGHTNESS, 0, 300); // amber: not connected
  }
}

// Rejects UIDs that couldn't possibly belong to a real tag: a genuine
// ISO15693 UID's most-significant byte (uid[7]) is the IC manufacturer
// code per ISO/IEC 7816-6, which is never 0x00, and real UIDs aren't
// mostly zero bytes. This catches corrupted/partial RF reads that the
// PN5180 chip still reported as "OK" (e.g. a weak read during a
// collision or a tag leaving the field mid-read).
bool isPlausibleUid(uint8_t* uid)
{
  if (uid[7] == 0x00) return false;

  uint8_t zeroBytes = 0;
  for (int i = 0; i < 8; i++) {
    if (uid[i] == 0x00) zeroBytes++;
  }
  return zeroBytes <= 5;
}

// Writes the UID as 16 uppercase hex chars + null terminator into `out`
// (caller-provided buffer, at least 17 bytes). Avoids building a String
// via repeated concatenation, since this runs on every poll (~500ms)
// while a tag is on the reader and String churn fragments the heap.
void uidToBuffer(uint8_t* uid, char* out)
{
  for (int i = 0; i < 8; i++) {
    sprintf(out + i * 2, "%02X", uid[7 - i]);
  }
}

/**************************************************
  Serial interface for communication to the PC
  - identical behaviour to the original Pn5180Esp.ino sketch (v/u/l/i),
    plus a new 'w' command to report WiFi/MQTT status.
**************************************************/
void serialInterface()
{
  // handle input strings
  if (Serial.available())
  {
    // read the string from the interface
    String command = Serial.readString();

    // handle command
    handleCommand(command);
  }
}

void handleCommand(String command)
{
  String response = "";
  uint8_t uid[10];

  // handle the command
  if (command.startsWith("v"))
  {
    response = (String)SKETCHNAME + " - " + (String)VERSION;
    Serial.println(response);
  }
  else if (command.startsWith("u"))
  {
    if (tryUnlockPrivacy())
    {
      Serial.println("ok");
      ledFeedback(0, LED_BRIGHTNESS, 0, 100);
    }
    else
    {
      Serial.println("nok");
      ledFeedback(LED_BRIGHTNESS, 0, 0, 100);
    }
  }
  else if (command.startsWith("l"))
  {
    Serial.println("nok");
  }
  else if (command.startsWith("i"))
  {
    nfc15693.reset();
    nfc15693.setupRF();
    delay(50);

    // try to read ISO15693 inventory
    ISO15693ErrorCode rc = nfc15693.getInventory(uid);
    if (rc == ISO15693_EC_OK)
    {
      char uidBuf[17];
      uidToBuffer(uid, uidBuf);
      Serial.println(uidBuf);
      ledFeedback(0, LED_BRIGHTNESS, 0, 100);
    }
    else
    {
      Serial.println();
      ledFeedback(LED_BRIGHTNESS, 0, 0, 100);
    }
  }
  else if (command.startsWith("w"))
  {
    // Print current WiFi/MQTT status for debugging (new in this sketch)
    Serial.print(F("WiFi: "));
    Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
    Serial.print(F("MQTT: "));
    Serial.println(mqtt.connected() ? "connected" : "disconnected");
  }
}

void ledFeedback(int r, int g, int b, int delayMs)
{
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
  delay(delayMs);
  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();
}
