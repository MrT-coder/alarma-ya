# alarma-ya

A panic button for a house.

You tap a button on your phone, a message crosses the internet, and a 130 dB
siren goes off in the hallway. There is no app to install and no server to run:
a web page publishes one MQTT message, an ESP32 sitting on the wall is
subscribed to it, and a relay closes.

It is deliberately small. Three moving parts, one topic, two payloads.

---

## How it works

```
   phone / browser                 HiveMQ Cloud                   hallway
 +------------------+           +--------------+        +---------------------+
 |  web/index.html  | wss:8884  |              | tls:8883|  ESP32 + relay      |
 |  publishes "ON"  |---------->|  MQTT broker |-------->|  subscribes, fires  |
 |            "OFF" |           |              |        |  the siren          |
 +------------------+           +--------------+        +---------------------+
      publish-only                  topic:                   subscribe-only
       credential              alarma-ya/comando               credential
```

The broker is the only thing both sides share, and it is the only thing that
authenticates anybody. There is no backend of your own to write, deploy or keep
alive — which also means there is nothing of your own to be breached.

The two sides use different ports on the same broker, and mixing them up is the
most common way to waste an afternoon here:

| Side | Port | Why |
|---|---|---|
| ESP32 | `8883` | raw MQTT over TLS |
| Browser | `8884` | MQTT over WebSocket over TLS — browsers cannot speak raw MQTT/TCP |

### The MQTT contract

Everything the two halves agree on fits in four lines:

- **Topic:** `alarma-ya/comando`
- **Payloads:** the literal strings `ON` and `OFF`. Nothing else is acted on.
- **Retain:** must be `false`. A retained `ON` is replayed by the broker to every
  new subscriber the moment it connects, so the siren would re-fire on every
  single reconnect, forever. Retain is for state; this is a command, and a
  command is an event that already happened.
- **QoS:** the device subscribes at QoS 1. The panel currently publishes at
  QoS 0, and MQTT silently downgrades to the lower of the two, so delivery today
  is effectively at-most-once.

### Credentials: two accounts, not one

Create **two** credentials in HiveMQ, not one shared pair:

| Used by | Permission | If it leaks |
|---|---|---|
| ESP32 (`secrets.h`) | SUBSCRIBE on `alarma-ya/comando` | someone can read your commands, never send one |
| Web panel (typed at runtime) | PUBLISH on `alarma-ya/comando` | someone can set off your siren, never listen in |

That split is the actual security model of this project. The web page runs in
the visitor's browser, so anything embedded in it is readable with *view
source* — which is exactly why the page ships with no credential at all and asks
for one at runtime. The containment is not secrecy, it is the permission
attached to the account.

---

## Hardware

| Part | Spec | Notes |
|---|---|---|
| ESP32 WROOM-32 devkit | 30 pins, GPIO at 3.3 V | ~250 mA, peaks near 400 mA on WiFi TX |
| 2-channel relay module | SRD-05VDC-SL-C, 5 V coil, PC817 optocouplers, contacts 10 A @ 30 VDC | **active LOW** — see below |
| Siren | 12 V DC, 20 W, 130 dB — 1.67 A nominal | |
| 12 V supply | 2 A / 24 W | powers the siren only |
| USB charger | 1 A | powers the ESP32 only |
| Resistor | 10 kΩ | pull-up on GPIO26 — **mandatory** |
| Diode | 1N4007 | flyback across the siren |
| Wire | AWG 20 for the 12 V side | Dupont jumpers are AWG 26–28 and will not carry 1.67 A |

### Two circuits, one meeting point

This is the idea that makes the rest of the wiring obvious: **there is no single
circuit here, there are two**, and the relay is the only place they touch.

The logic side runs at 3.3 V and a few milliamps, all of it fed from the USB
charger through the ESP32. The power side moves 1.67 A at 12 V and never comes
near the board. Inside the relay the two meet without conducting: the
optocoupler passes the command as *light*, and the contacts are metal that
either touches or does not.

Do not power the ESP32 from the 12 V supply, and do not run the siren current
through the board.

### Wiring

**Before anything else, pull the blue JD-VCC jumper off the relay module.**

That jumper ties `VCC` to `JD-VCC`, feeding the optocoupler and the coil from
one line. You need them separate, and here is why: the PC817's internal LED
starts conducting around 1.2 V. Feed `VCC` with 5 V while the ESP32 drives `IN1`
HIGH at 3.3 V, and 1.7 V still sits across that LED — the relay stays latched or
behaves erratically. Put `VCC` on 3.3 V instead and a HIGH pin means zero volts
across the LED, which is genuinely off. The coil keeps getting its 5 V through
`JD-VCC`. Keep the jumper somewhere; do not throw it out.

Logic side:

| From | To | Why |
|---|---|---|
| ESP32 `GPIO26` | relay `IN1` | the command. LOW fires. |
| ESP32 `3V3` | relay `VCC` | optocoupler reference. 3.3 V, **never 5 V** |
| ESP32 `GND` | relay `GND` (4-pin header) | logic ground |
| ESP32 `GPIO26` | ESP32 `3V3`, through 10 kΩ | holds the pin HIGH while it floats during boot |
| ESP32 `VIN` | relay `JD-VCC` | coil supply, ~72 mA. `VIN` is the USB 5 V rail. |
| ESP32 `GND` | relay `GND` (3-pin header) | coil return |

Power side:

| From | To | Cable |
|---|---|---|
| 12 V supply `+` | relay `COM` (CH1) | AWG 20 |
| relay `NO` (CH1) | siren `+` | AWG 20 |
| 12 V supply `−` | siren `−` | AWG 20 |
| 1N4007 across the siren, **stripe to `+`** | | flyback |

`NO`, not `NC`. At rest the contact stays open and the siren stays quiet.

ESP32 GPIO pins are **not 5 V tolerant** — the datasheet says so in those words.
Putting 5 V on GPIO26 destroys it. That is the whole reason `VCC` goes to 3.3 V.

### The relay is active LOW — and Wokwi is the opposite

This is the single most important paragraph in this file.

Opto-isolated relay boards built on PC817 + SRD-05VDC close the contact when the
input pin is pulled **LOW**, not HIGH. That was not deduced, it was measured on
the bench with `src/relay_polarity.cpp`: the IN1 LED lights and the relay clicks
while GPIO26 is LOW.

So the firmware carries:

```c
#define RELAY_ACTIVE_LOW 1
```

**Wokwi's relay module is active HIGH.** If you simulate with this set to `1`,
the logic reads inverted in the simulator. Flip it to `0` for Wokwi, and back to
`1` before you flash real hardware.

Get this backwards on real hardware and the failure is not subtle: the siren
starts screaming the instant the ESP32 is powered, and the `ON` command
*silences* it. Everything inverted, at 130 dB.

And software alone does not cover it. Between the moment power arrives and the
moment your first line of code runs, GPIO26 floats. That window belongs to the
**10 kΩ pull-up**, which is why it is not optional.

### Checking the polarity yourself

Do not take this file's word for it — your module may not be the same one. There
is a second PlatformIO environment that does nothing but answer this question,
with no WiFi and no MQTT in the way:

```bash
pio run -e relay_polarity -t upload -t monitor
```

Wire **only** the logic and coil side. No 12 V, no siren. The pin parks HIGH for
10 seconds, then alternates LOW/HIGH every 3 seconds. Listen for the click and
read the serial output: the line printed while the relay is closed tells you
your active level.

---

## Firmware

### Configure `secrets.h`

Everything that differs between installations lives in one gitignored file.

```bash
cp src/secrets.h.example src/secrets.h
```

Then fill in:

| Define | What |
|---|---|
| `WIFI_AP1_SSID` / `WIFI_AP1_PASS` | your network. Slots 2 and 3 are optional — uncomment a pair to register it. |
| `MQTT_HOST` | your HiveMQ cluster hostname. No scheme, no port, no trailing slash. |
| `MQTT_USER` / `MQTT_PASS` | the subscribe-only credential |

Two things that cost people hours:

- **The ESP32 radio is 2.4 GHz only.** A 5 GHz SSID will never associate, no
  matter how correct the password is. If your router broadcasts both bands under
  one name, the 2.4 GHz band still has to be enabled.
- `WiFiMulti` picks a network **at connect time and does not roam**. It scans,
  joins the strongest one it can actually see, and stays there until the link
  drops. Multiple APs mean "this firmware boots in more than one place without a
  reflash", not "it follows you around the house".

`src/secrets.h` is gitignored. So is `.pio/`, because the compiled
`firmware.bin` has your credentials baked into it.

### Build and upload

```bash
pio run -e esp32dev                       # build
pio run -e esp32dev -t upload             # build and flash
pio run -e esp32dev -t upload -t monitor  # flash and watch the serial log at 115200
```

`esp32dev` is the default environment, so a plain `pio run` does the same thing.
Only `src/main.cpp` is compiled into it — `build_src_filter` keeps the bench test
out of the production build.

Use a **data** micro-USB cable. Charge-only cables do not enumerate a COM port,
and the failure looks exactly like a dead board.

---

## The status LED

The onboard blue LED is the only local readout this thing has, so it is worth
reading properly. Without it you cannot tell a booting board from an armed one,
and an alarm nobody can confirm is armed is an alarm nobody trusts.

| Pattern | Meaning |
|---|---|
| **Fast blink** (~5 Hz) | looking for a WiFi network |
| **Slow blink** (~1.2 Hz) | WiFi is up, the broker is not reachable |
| **Brief pulse** every 3 s | connected, subscribed, **armed** |
| **Solid on** | the siren is firing right now |

Firing outranks everything else: whatever the network is doing, a sounding horn
is the fact you need first.

The LED is painted by a hardware timer rather than by `loop()`, and that is a
fix, not a flourish. `WiFiMulti::run()` blocks for roughly nine seconds while it
scans and associates. Back when the pattern was repainted from `loop()`, a board
that simply could not find its network repainted once every nine seconds — which
reads as a **steady light**. On this device a steady light means the siren is
firing. A board that was merely lost was reporting the most alarming state it
has. A timer cannot be starved by a blocking call.

---

## The web panel

`web/index.html` is a single static file. No build step, nothing to install, no
server. Open it from disk, or put it behind any static host.

On first use it asks for three things — broker host, username, password — and,
if you tick the box, remembers them in `localStorage` so the next tap is
instant. The broker is what validates them: a rejected CONNACK sends you back to
the form instead of retrying a password that will never work.

**One-tap triggering.** The page reads `?fire=on` from its own URL and fires as
soon as the connection is up. Point an NFC tag or a phone shortcut at
`https://your-host/index.html?fire=on` and the tap becomes the whole
interaction. If the connection is not up yet, the request is held, not lost.

---

## Safety

Three independent guards, because a 130 dB horn earns them.

**1. The siren cannot sound forever.** `SIREN_MAX_MS` is 2 minutes and it is
enforced *on the device*, so it survives the WiFi, the broker and the phone all
dying at once. Every `ON` restarts the countdown, which makes it a dead man's
switch rather than a cap on the event: if the emergency is still going, tap
again and the clock resets. The check runs at the top of `loop()`, before any
network work, because a lost link is precisely the situation where the `OFF`
will never arrive.

**2. `OFF` is always honoured.** No grace window, no conditions. Refusing to stop
is never the safe failure mode.

**3. A retained `ON` cannot re-fire the siren.** The panel never sets retain, but
the device does not trust the publisher: any `ON` arriving within 3 seconds of
subscribing is treated as a broker replay and dropped.

And in `setup()`, the safe level is written to the pin **before** `pinMode()`
switches it to output. Do it the other way round and the pin spends a moment at
its reset default — which is a free chirp out of a 130 dB horn.

### Bringing it up for the first time

In this order. Skipping steps here is how hardware dies.

1. **Meter the 12 V supply before plugging anything into it.** Red probe to the
   plug's centre pin, black to the outer ring. It must read **+12 V**. Generic
   adapters lie on their labels, and reversed polarity destroys everything
   downstream of them.
2. **Confirm the relay polarity** with the `relay_polarity` environment above.
3. **Fit the 10 kΩ pull-up** before any load is connected.
4. **Test the contacts with something harmless** — a 12 V bulb, an LED with a
   resistor, or just a multimeter in continuity. Confirm it opens and closes,
   and confirm the `SIREN_MAX_MS` auto-off actually fires.
5. **Only now, the siren** — with the 1N4007 already fitted, outdoors or with
   hearing protection, and `SIREN_MAX_MS` set short. 130 dB in a closed room
   causes damage in seconds.

---

## Simulating it

`diagram.json` and `wokwi.toml` drive the Wokwi simulator against the real
PlatformIO build (`.pio/build/esp32dev/firmware.*`), so you can exercise the
MQTT logic, the reconnect path and the auto-off with no hardware on the desk.

Two things to change before it will run:

- Put `Wokwi-GUEST` / `""` in `secrets.h` — that is the simulator's virtual
  network.
- Set `RELAY_ACTIVE_LOW` to `0`, because Wokwi's relay is active HIGH.

Simulate what has **states**: firmware, protocol, reconnection. Calculate what
has **numbers**: current, power, dissipation. Measure what has **tolerances**:
the real supply, the real siren, the real relay. Wokwi answers the first
question only, and no simulator will tell you whether your supply holds up.

---

## Known limitations

Written down rather than hidden, because you are about to trust this thing.

- **TLS certificates are not validated.** `net.setInsecure()` encrypts the
  traffic but accepts any certificate, so a machine in the network path could
  impersonate the broker and fire the siren. Pinning the HiveMQ root CA is the
  fix, and it is not done yet.
- **The panel stores its credential in `localStorage` in clear text.** Anyone
  holding the unlocked device can read it. The blast radius is bounded by that
  credential being publish-only, but it is a real trade and it was made on
  purpose.
- **Commands are effectively at-most-once.** The device subscribes at QoS 1, the
  panel publishes at QoS 0, and MQTT downgrades to the lower of the two.
- **No delivery confirmation.** The panel reports that it published, not that
  the siren sounded. There is no acknowledgement topic.

---

## License

MIT — see [LICENSE](LICENSE).

Worth reading the last paragraph of it before you build one. This drives 12 V
and a 130 dB siren, it is published AS IS with no warranty of any kind, and
what you wire up is your responsibility.
