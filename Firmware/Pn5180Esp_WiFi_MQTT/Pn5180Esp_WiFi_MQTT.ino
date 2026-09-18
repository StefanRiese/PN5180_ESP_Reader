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
**************************************************/
#include <PN5180ISO15693.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

// Must be defined BEFORE including PubSubClient.h - the default 256-byte
// limit is too small for our HA discovery JSON payload + topic combined,
// which causes mqtt.publish() to silently fail with no error.
#define MQTT_MAX_PACKET_SIZE 512
#include <PubSubClient.h>

/**************************************************
  ===== EDIT THESE: WiFi / MQTT / device config =====
**************************************************/
const char* WIFI_SSID      = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST      = "192.168.1.10";   // your HA/MQTT broker IP
const uint16_t MQTT_PORT   = 1883;
const char* MQTT_USER      = "mqtt_user";      // leave "" if no auth
const char* MQTT_PASSWORD  = "mqtt_pass";      // leave "" if no auth

// A short human-readable label for WHERE this reader is - this is the
// only thing you need to change per device. It gets combined with the
// chip's own unique hardware ID below, so you can never accidentally
// have two readers collide on the same MQTT client ID / topics / HA
// device entry, even if you forget to edit anything else.
const char* LOCATION_NAME  = "living_room";   // <-- EDIT per device: kitchen, bedroom, etc.

// Derived automatically at boot - do not edit
String DEVICE_ID;      // e.g. "living_room_3fa2c1"  (unique per physical chip)
String DEVICE_NAME;    // e.g. "PN5180 Reader (living_room)"

// Derived topics - no need to edit these
String TOPIC_SCAN;        // where scanned UIDs get published
String TOPIC_AVAILABILITY; // online/offline (LWT)
String TOPIC_DISCOVERY;   // HA discovery config topic

/**************************************************
  Existing defines / pins (unchanged from original sketch)
**************************************************/
#define VERSION "2.0-wifi"
#define SKETCHNAME "Pn5180Esp"
#define LED_BRIGHTNESS 10

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

/**************************************************
  Globals
**************************************************/
PN5180ISO15693 nfc15693(PN5180_NSS, PN5180_BUSY, PN5180_RST);
uint8_t password[]  = {0x0F, 0x0F, 0x0F, 0x0F};
uint8_t password2[] = {0x5B, 0x6E, 0xFD, 0x7F};
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(1, WS2812B_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient espClient;
PubSubClient mqtt(espClient);

String lastUid = "";
unsigned long tagAbsentSinceMillis = 0; // when the tag last became absent
unsigned long lastPollMillis = 0;
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
  randomSeed(analogRead(A0) ^ micros()); // avoid repeatable MQTT client IDs across reboots
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false); // don't wear out flash with every reconnect
  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(3); // seconds - keep a dead broker from stalling loop() too long
  connectMqtt();
}

/*************************************
  Loop
*************************************/
void loop()
{
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

  String clientId = String(DEVICE_ID) + "-" + String(random(0xffff), HEX);

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
  }
}

// Publishes the retained MQTT Discovery config so Home Assistant
// auto-creates a "tag" scanner for this device - no YAML needed.
void publishDiscovery()
{
  String payload = String("{") +
    "\"topic\":\"" + TOPIC_SCAN + "\"," +
    "\"availability_topic\":\"" + TOPIC_AVAILABILITY + "\"," +
    "\"device\":{" +
      "\"identifiers\":[\"" + DEVICE_ID + "\"]," +
      "\"name\":\"" + DEVICE_NAME + "\"," +
      "\"manufacturer\":\"DIY\"," +
      "\"model\":\"PN5180 ESP Reader\"," +
      "\"sw_version\":\"" + VERSION + "\"" +
    "}" +
  "}";

  mqtt.publish(TOPIC_DISCOVERY.c_str(), payload.c_str(), true); // retained!
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

  if (ISO15693_EC_OK == nfc15693.disablePrivacyMode(password2)) {
    return true;
  }

  nfc15693.reset();
  nfc15693.setupRF();
  delay(50);

  return (ISO15693_EC_OK == nfc15693.disablePrivacyMode(password));
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

  if (rc == ISO15693_EC_OK) {
    String uidStr = uidToString(uid);
    bool isNewTag = (uidStr != lastUid);

    if (isNewTag) {
      // a different tag than last time - always trigger
      lastUid = uidStr;
      onTagScanned(uidStr);
    } else if (!tagPresentLastPoll) {
      // same tag as before, but it had been lifted off - only
      // re-trigger if it was actually away for long enough
      unsigned long awayDuration = millis() - tagAbsentSinceMillis;
      if (awayDuration >= TAG_AWAY_THRESHOLD_MS) {
        onTagScanned(uidStr);
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

void onTagScanned(String uid)
{
  ledFeedback(0, LED_BRIGHTNESS, 0, 150);

  if (mqtt.connected()) {
    bool sent = mqtt.publish(TOPIC_SCAN.c_str(), uid.c_str(), false); // not retained
    if (!sent) {
      ledFeedback(LED_BRIGHTNESS, LED_BRIGHTNESS, 0, 300); // amber: publish failed
    }
  } else {
    ledFeedback(LED_BRIGHTNESS, LED_BRIGHTNESS, 0, 300); // amber: not connected
  }
}

String uidToString(uint8_t* uid)
{
  String response = "";
  for (int i = 0; i < 8; i++) {
    response += (uid[7 - i] < 0x10 ? "0" : "");
    response += String(uid[7 - i], HEX);
  }
  response.toUpperCase();
  return response;
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
      response = uidToString(uid);
      ledFeedback(0, LED_BRIGHTNESS, 0, 100);
    }
    else
    {
      ledFeedback(LED_BRIGHTNESS, 0, 0, 100);
    }

    Serial.println(response);
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
