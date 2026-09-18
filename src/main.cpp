#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Ticker.h>

// ---------- Credentials ----------
// WIFI_AP*_SSID / WIFI_AP*_PASS / MQTT_HOST / MQTT_USER / MQTT_PASS live in
// secrets.h, which is gitignored. Copy secrets.h.example to secrets.h and fill
// it in.
#include "secrets.h"

// ---------- HiveMQ Cloud broker ----------
// MQTT_HOST is per-account, so it lives in secrets.h with everything else that
// differs between installations. Publishing it would not leak a password - the
// broker still demands one - but it would hand anyone who reads the repository
// the exact cluster this alarm depends on, and a free-tier cluster has a
// connection quota that can be exhausted from outside.
const int MQTT_PORT = 8883;   // TLS port (HiveMQ Cloud only accepts TLS)

// ---------- Topic + hardware ----------
const char* TOPIC_CMD = "alarma-ya/comando";
const int   SIREN_PIN = 26;   // drives the relay coil (an LED shows it in the simulator)

// ---------- Status light ----------
// The devkit's onboard blue LED, used as the only local readout this thing has.
// Without it there is no way to tell a booting board from a connected one, and
// an alarm nobody can confirm is armed is an alarm nobody trusts.
//
//   fast blink   -> looking for the WiFi network
//   slow blink   -> WiFi is up, the broker is not
//   brief pulse  -> connected, subscribed, armed
//   solid on     -> the siren is firing right now
const int STATUS_LED_PIN = 2;

const unsigned long BLINK_WIFI_MS = 100;    // half-period while joining WiFi
const unsigned long BLINK_MQTT_MS = 400;    // half-period while reaching the broker
const unsigned long PULSE_PERIOD_MS = 3000; // gap between armed heartbeat pulses
const unsigned long PULSE_WIDTH_MS  = 60;   // length of one armed heartbeat pulse

// What the light is currently reporting. loop() sets it; the timer below paints it.
enum LedMode {
  LED_CONNECTING,   // fast blink  - looking for a WiFi network
  LED_BROKER,       // slow blink  - WiFi is up, the broker is not
  LED_ARMED,        // brief pulse - connected, subscribed, waiting
  LED_FIRING        // solid on    - the siren is sounding right now
};
volatile LedMode ledMode = LED_CONNECTING;

// The light is driven by a hardware timer instead of by loop().
//
// It used to be repainted by calls scattered through loop(), which worked only
// while every iteration was fast. WiFiMulti::run() broke that: it blocks for the
// scan plus the association timeout - about nine seconds - so the "searching"
// blink was repainted once per nine seconds and read as a steady light. On this
// device a steady light means the 130dB siren is firing, so a board that simply
// could not find its network was reporting the most alarming state it has.
//
// A timer cannot be starved by a blocking call, so the pattern now survives
// anything loop() does. Each pattern is a pure function of millis(), which keeps
// the callback stateless and free of anything the timer task must not touch.
Ticker ledTicker;
const unsigned long LED_TICK_MS = 20;       // how often the timer repaints the pin

void ledTick() {
  const unsigned long now = millis();
  bool on;
  switch (ledMode) {
    case LED_FIRING: on = true;                                            break;
    case LED_ARMED:  on = (now % PULSE_PERIOD_MS) < PULSE_WIDTH_MS;        break;
    case LED_BROKER: on = (now / BLINK_MQTT_MS) & 1;                       break;
    default:         on = (now / BLINK_WIFI_MS) & 1;                       break;
  }
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// ---------- Relay polarity ----------
// Opto-isolated relay boards (PC817 + SRD-05VDC) close the contact when the input
// pin is pulled LOW, not HIGH. Ours is one of those. Wokwi's LED is the opposite,
// so flip this back to 0 when running the simulation.
// CONFIRMED ON THE BENCH 2026-08-23 with src/relay_polarity.cpp: the IN1 LED
// lights and the relay clicks while GPIO26 is LOW. Active-low. Keep this at 1.
#define RELAY_ACTIVE_LOW 1

#if RELAY_ACTIVE_LOW
  #define RELAY_ON  LOW
  #define RELAY_OFF HIGH
#else
  #define RELAY_ON  HIGH
  #define RELAY_OFF LOW
#endif

// ---------- Safety auto-off ----------
// A 130dB siren must NEVER be able to sound forever. If the network drops and the
// OFF command never arrives, the ESP32 shuts the siren off on its own after this long.
// The timer lives HERE, in the device, so it survives even if WiFi/broker/phone all die.
//
// Every ON restarts the countdown, so this is not "the alarm lasts 2 minutes".
// It is "the alarm stops 2 minutes after the last time somebody asked for it" -
// a dead man's switch, not a cap on the event. If the emergency is still going,
// anyone taps the shortcut again and the clock resets.
const unsigned long SIREN_MAX_MS = 2UL * 60UL * 1000UL;   // 2 minutes

WiFiClientSecure net;
PubSubClient mqtt(net);

// Holds every access point from secrets.h. On each run() it scans and joins the
// one with the strongest signal it can actually see, which is what lets the same
// firmware boot in more than one location without a reflash.
//
// It selects at CONNECT time only - there is no roaming. Once associated the
// ESP32 stays on that AP until the link drops, even if a stronger one appears.
WiFiMulti wifiMulti;

bool          sirenOn      = false;   // current siren state
unsigned long sirenOnAt    = 0;       // millis() timestamp when it was last turned ON
unsigned long subscribedAt = 0;       // millis() timestamp of the last successful subscribe

// A broker replays a retained message to every new subscriber the instant it
// subscribes, so a retained "ON" left on the topic would re-fire the siren on
// every reconnect. The publisher no longer sets retain, but the device must not
// depend on a well-behaved publisher for a 130dB horn: any ON arriving within
// this window of subscribing is treated as a replay and dropped.
const unsigned long RETAINED_GRACE_MS = 3000;

// One place decides what the light means, so the four states can never disagree
// with each other. Firing outranks everything: whatever the network is doing, a
// sounding horn is the fact the operator needs first.
void updateLedMode() {
  if (sirenOn)                             ledMode = LED_FIRING;
  else if (mqtt.connected())               ledMode = LED_ARMED;
  else if (WiFi.status() == WL_CONNECTED)  ledMode = LED_BROKER;
  else                                     ledMode = LED_CONNECTING;
}

void setSiren(bool on) {
  digitalWrite(SIREN_PIN, on ? RELAY_ON : RELAY_OFF);
  sirenOn = on;
  if (on) sirenOnAt = millis();    // (re)start the safety countdown on every ON
  updateLedMode();                 // the horn changed state; say so without waiting for loop()
  Serial.printf("Siren -> %s\n", on ? "ON" : "OFF");
}

// Runs every time a message arrives on a subscribed topic.
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("Message on %s: %s\n", topic, msg.c_str());

  // OFF is always honoured - refusing to stop is never the safe failure mode.
  if (msg == "OFF") { setSiren(false); return; }

  if (msg == "ON") {
    if (millis() - subscribedAt < RETAINED_GRACE_MS) {
      Serial.println("Ignoring ON: arrived right after subscribe, treated as retained replay");
      return;
    }
    setSiren(true);
  }
}

// Returns as soon as WiFi drops instead of retrying against a dead link. That
// exit matters: reselecting an access point happens in loop(), so a function that
// spun here forever would pin the device to a network that is no longer there.
void connectMqtt() {
  while (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    Serial.print("Connecting to MQTT broker...");
    String clientId = "alarma-ya-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println(" connected!");
      subscribedAt = millis();     // stamped before the broker can replay anything
      // QoS 1 (at least once). QoS 0 silently drops the command if the packet is
      // lost, which is the wrong trade for a panic button. QoS 1 can deliver a
      // duplicate instead, and a duplicate is harmless here: setting the siren
      // state is idempotent, so applying it twice is the same as applying it once.
      // The subscription has to match the publisher - an upgrade on one side only
      // is silently downgraded to the lower of the two.
      mqtt.subscribe(TOPIC_CMD, 1);
      updateLedMode();
      Serial.printf("Subscribed to %s\n", TOPIC_CMD);
    } else {
      updateLedMode();
      Serial.printf(" failed (rc=%d), retrying in 2s\n", mqtt.state());
      unsigned long until = millis() + 2000;
      while ((long)(millis() - until) < 0) delay(10);
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Drive the safe level BEFORE switching the pin to output. Otherwise the pin
  // spends a moment at its reset default and a 130dB siren gets a free chirp.
  // A 10k pull-up from SIREN_PIN to 3V3 covers the window before this line runs.
  digitalWrite(SIREN_PIN, RELAY_OFF);
  pinMode(SIREN_PIN, OUTPUT);
  setSiren(false);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  ledMode = LED_CONNECTING;
  ledTicker.attach_ms(LED_TICK_MS, ledTick);

  // No channel argument anywhere: pinning one was a Wokwi detail. On a real
  // router it forces the wrong channel and the join silently never completes.
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_AP1_SSID, WIFI_AP1_PASS);
#ifdef WIFI_AP2_SSID
  wifiMulti.addAP(WIFI_AP2_SSID, WIFI_AP2_PASS);
#endif
#ifdef WIFI_AP3_SSID
  wifiMulti.addAP(WIFI_AP3_SSID, WIFI_AP3_PASS);
#endif

  Serial.print("Connecting to WiFi");
  unsigned long lastDot = 0;
  while (wifiMulti.run() != WL_CONNECTED) {
    if (millis() - lastDot >= 500) { lastDot = millis(); Serial.print("."); }
    delay(10);
  }
  ledMode = LED_BROKER;
  // Which AP won, and how hard it had to try. Without this line a multi-AP setup
  // is unreadable: a board on a weak distant network looks exactly like a board
  // on a strong nearby one until the day it stops answering.
  Serial.printf(" connected to %s (%d dBm)\n", WiFi.SSID().c_str(), WiFi.RSSI());

  // TODO before this leaves the bench: pin the HiveMQ root CA instead. Skipping
  // validation still encrypts the traffic, but it accepts any certificate, so a
  // machine in the network path could impersonate the broker and fire the siren.
  net.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(60);           // tolerate slow TLS in the simulator (default is 15s)
  mqtt.setSocketTimeout(30);       // give each read/write more time before giving up
}

void loop() {
  // Safety timeout FIRST, before any network work. A lost link is exactly the
  // situation where the OFF command never arrives, so the one code path that can
  // still silence a 130dB horn must not sit behind a connectivity check or an
  // early return. Unsigned math handles millis() overflow.
  if (sirenOn && millis() - sirenOnAt >= SIREN_MAX_MS) {
    Serial.println("Safety timeout reached -> auto-off");
    setSiren(false);
  }

  // WiFiMulti only reselects an access point when run() is called. Without this
  // the device would never move to a reachable network after losing the one it
  // booted on, and a panic button that cannot reach its broker is not a panic
  // button. run() scans and blocks for a few seconds, which is why the safety
  // check above already ran: the worst case is the horn overshooting its
  // deadline by one scan, never missing the shutoff.
  if (WiFi.status() != WL_CONNECTED) {
    updateLedMode();                // set before run() blocks; the timer keeps painting it
    if (wifiMulti.run() == WL_CONNECTED) {
      Serial.printf("WiFi back on %s (%d dBm)\n", WiFi.SSID().c_str(), WiFi.RSSI());
    }
    return;                         // nothing else can work until the link is up
  }

  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();                      // keeps the connection alive and processes incoming messages

  updateLedMode();
}
