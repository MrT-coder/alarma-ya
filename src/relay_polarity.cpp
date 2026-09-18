// ---------------------------------------------------------------------------
// BENCH TEST ONLY - relay polarity check. Not part of the alarm firmware.
//
// Build/upload with:  pio run -e relay_polarity -t upload -t monitor
//
// Wire ONLY the logic and coil side. No 12V supply. No siren.
//   ESP32 GPIO26 -> relay IN1
//   ESP32 3V3    -> relay VCC       (jumper JD-VCC REMOVED)
//   ESP32 GND    -> relay GND (4-pin header)
//   ESP32 VIN    -> relay JD-VCC   (VIN is the USB 5V rail; this board has no
//                                    pin silkscreened "5V")
//   ESP32 GND    -> relay GND (3-pin header)
//
// The 10k pull-up from GPIO26 to 3V3 is NOT needed here: it only guards the
// boot window against a false trigger, and there is no load to trigger yet.
// It becomes mandatory before the siren is wired.
//
// The pin is parked HIGH for 10s after boot, then alternates LOW/HIGH every 3s.
// Listen to the clicks and read the serial monitor: the line printed when the
// relay is CLOSED tells you the active level.
// ---------------------------------------------------------------------------

#include <Arduino.h>

const int SIREN_PIN = 26;

// The devkit's onboard blue LED. Mirrors the relay command so the toggling is
// visible even when the relay board shows nothing: if this blinks, the sketch
// is running and GPIO26 is being driven, and the fault is downstream.
const int ONBOARD_LED_PIN = 2;

unsigned long cycle = 0;

void setup() {
  // Safe level before the pin becomes an output, same as the real firmware.
  digitalWrite(SIREN_PIN, HIGH);
  pinMode(SIREN_PIN, OUTPUT);
  digitalWrite(SIREN_PIN, HIGH);

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== RELAY POLARITY TEST ===");
  Serial.println("No 12V, no siren connected. Listen for the clicks.");
  Serial.println("Pin parked HIGH for 10 seconds...");
  delay(10000);
}

void loop() {
  cycle++;

  // Onboard LED ON marks the LOW half, so the serial log, the blue LED and the
  // relay can all be compared against each other at a glance.
  Serial.printf("[%lu] GPIO26 = LOW   | blue LED ON   <-- relay CLOSED now => ACTIVE-LOW\n", cycle);
  digitalWrite(SIREN_PIN, LOW);
  digitalWrite(ONBOARD_LED_PIN, HIGH);
  delay(3000);

  Serial.printf("[%lu] GPIO26 = HIGH  | blue LED OFF  <-- relay CLOSED now => ACTIVE-HIGH\n", cycle);
  digitalWrite(SIREN_PIN, HIGH);
  digitalWrite(ONBOARD_LED_PIN, LOW);
  delay(3000);
}
