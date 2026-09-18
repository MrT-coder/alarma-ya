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
  PUBLISHERS                          BROKER                    SUBSCRIBER

  web/index.html  --wss://host:8884--+
  (any browser)                      |
                                     +-->  HiveMQ Cloud  --ssl://host:8883-->  ESP32
  phone shortcut  --ssl://host:8883--+     alarma-ya/comando                   + relay
  (HTTP Shortcuts app)                        "ON" / "OFF"                     + siren

  publish-only credential                                    subscribe-only credential
```

The broker is the only thing all three share, and it is the only thing that
authenticates anybody. There is no backend of your own to write, deploy or keep
alive — which also means there is nothing of your own to be breached.

Note that there is no privileged client here. The web panel is one publisher
among several, not *the* app. Anything that can publish `ON` to
`alarma-ya/comando` fires the siren, and that is exactly why a broker sits in
the middle instead of a server of your own: adding a new way to trigger the
alarm is adding a client, not deploying code.

### How the three clients connect

Same broker, same topic, three different ways in — and mixing up the ports is
the most common way to waste an afternoon here.

| Client | Endpoint | Transport |
|---|---|---|
| ESP32 | `8883` | MQTT over TLS, raw TCP |
| Phone shortcut | `ssl://host:8883` | MQTT over TLS, raw TCP |
| Web panel | `wss://host:8884/mqtt` | MQTT over WebSocket over TLS |

The browser is the odd one out, and not by choice: **a web page cannot open a
raw TCP socket**, so MQTT has to be tunnelled through a WebSocket. That is the
only reason port `8884` appears in this project at all. Everything else uses
`8883`.

Every connection is TLS. HiveMQ Cloud does not accept plaintext, so there is no
unencrypted variant to fall back to or to forget to turn on.

The ESP32 also **verifies the broker's certificate**, against the root CA bundle
that ships with the ESP32 core — the same trust list a browser carries.
Encryption on its own would not be enough, and the reason is worth stating
plainly: the credentials prove the *device* to HiveMQ, and certificate validation
is the only thing that proves *HiveMQ* to the device. They point in opposite
directions. Without the second one, anything able to redirect the traffic can
present a certificate of its own, read the credentials straight out of the
CONNECT packet, and from then on decide which commands the siren ever hears —
including none at all, during a break-in, with the status light still reporting
"armed".

**That validation needs a real clock**, and this is the part that catches people
out. Checking a certificate includes asking whether today falls between its two
dates, and a freshly booted ESP32 believes it is 1 January 1970 — which is
before every certificate ever issued. So the firmware syncs time over NTP before
its first handshake and refuses to attempt a connection until the clock is sane.

Which means the device needs **outbound NTP (UDP 123)** on your network, not only
MQTT. If the board sits on a slow blink forever and the serial log says the clock
is not synced, that is a blocked port, not a broken broker.

A few details of the device connection, since it is the one that has to stay up
for years at a time:

- **Client ID** is `alarma-ya-<efuse MAC>`, derived from the chip itself. Two
  boards can never collide, and a collision would make the broker kick the older
  session off on every reconnect — two devices fighting over one session, each
  disconnecting the other, forever.
- **Keepalive 60 s, socket timeout 30 s.** Both are raised from the library
  defaults, which are tight enough that a slow TLS handshake reads as a dropped
  connection.
- **Clean session.** No offline queue, on purpose: a panic command that arrives
  ten minutes late is worse than one that never arrives at all.

### The MQTT contract

Everything all three clients have to agree on fits in four lines:

- **Topic:** `alarma-ya/comando`
- **Payloads:** the literal strings `ON` and `OFF`. Nothing else is acted on.
- **Retain:** must be `false`. A retained `ON` is replayed by the broker to every
  new subscriber the moment it connects, so the siren would re-fire on every
  single reconnect, forever. Retain is for state; this is a command, and a
  command is an event that already happened.
- **QoS:** the device subscribes at QoS 1. The panel currently publishes at
  QoS 0, and MQTT silently downgrades to the lower of the two, so delivery today
  is effectively at-most-once.

---

## Setting up HiveMQ Cloud

You need a broker before any of this runs, and the free tier is more than a
house needs. There is nothing to install and nothing to operate: you create a
cluster, you create credentials, and that is the entire backend.

### 1. Create the cluster

Sign up at [hivemq.cloud](https://www.hivemq.com/mqtt-cloud-broker/) and create
a free cluster. Once it is running, open **Cluster Details** and copy the
**URL**. It looks like `something.s1.eu.hivemq.cloud`.

Copy the **hostname only** — no `https://`, no `ssl://`, no port, no trailing
slash. That one string goes into `MQTT_HOST` in `secrets.h`, into the *Broker*
field of the web panel, and into the URL of the phone shortcut. All three
clients point at the same place.

### 2. Create the credentials

Open the **Access Management** tab. A credential there is a username, a
password, and a set of permissions — and each permission is a topic filter plus
the activity it allows: publish, subscribe, or both.

Create **two**, not one shared pair:

| Suggested name | Permission | Topic filter | Goes into |
|---|---|---|---|
| `alarma-ya-device` | **Subscribe only** | `alarma-ya/comando` | `src/secrets.h` on the ESP32 |
| `alarma-ya-trigger` | **Publish only** | `alarma-ya/comando` | web panel and phone shortcut, typed at runtime |

That split is the actual security model of this project, so it is worth being
precise about what each half buys you:

| If this leaks | What someone can do | What they cannot do |
|---|---|---|
| the device credential | read your commands | send one — the siren never sounds |
| the trigger credential | set off your siren | listen to anything, read any topic |

Neither one is a master key, and that is the whole point. The web page runs in
the visitor's browser, so anything embedded in it is readable with *view
source* — which is exactly why the page ships with no credential at all and
asks for one at runtime. **The containment is not secrecy, it is the permission
attached to the account.**

Two things worth doing while you are in there:

- Scope the topic filter to `alarma-ya/comando` exactly. A wildcard like `#` is
  quicker to type and hands over the entire broker.
- If more than one phone gets the trigger credential, create one credential per
  device. Then losing a phone means revoking one credential, not re-pairing
  everything you own.

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
soon as the connection is up. Point an NFC tag at
`https://your-host/index.html?fire=on` and the tap becomes the whole
interaction. If the connection is not up yet, the request is held, not lost.

---

## Triggering it from your phone

The panel needs a browser, a page load and a connection handshake before it can
send anything. A dedicated MQTT client sitting on the home screen skips all of
that, and for a panic button that difference is the whole feature.

The setup in daily use here is [HTTP Shortcuts](https://github.com/Waboodoo/HTTP-Shortcuts),
an open-source Android app that puts one-tap buttons on the home screen. The
name is historical — it speaks MQTT natively, and that is what this uses. When
you create the shortcut, pick the **MQTT** type, not an HTTP request.

Make **two** shortcuts, one per command:

| Field | ON shortcut | OFF shortcut |
|---|---|---|
| URL | `ssl://your-cluster.s1.eu.hivemq.cloud:8883` | same |
| Topic | `alarma-ya/comando` | same |
| Message | `ON` | `OFF` |
| Username / password | the **publish-only** credential | same |

Three things that will cost you an evening if you get them wrong:

- **The scheme is `ssl://`**, not `mqtt://` and not `https://`. HiveMQ Cloud
  accepts TLS only. A plaintext connection is refused outright rather than
  silently downgraded, so the failure reads like a broken broker instead of a
  wrong URL.
- **Port `8883`, not `8884`.** The app speaks raw MQTT, so it uses the same
  endpoint as the ESP32. `8884` is the WebSocket port and it exists only because
  browsers cannot do anything else.
- **Leave retain off.** A retained `ON` is replayed by the broker to every new
  subscriber, so the siren would re-fire every single time the ESP32 reconnects.
  The device carries a three second guard against exactly this, but that guard
  is a backstop, not a licence to publish retained commands.

Build the OFF shortcut at the same time as the ON one, not afterwards. A trigger
with no matching stop leaves you waiting out the two minute auto-off standing
next to a 130 dB horn.

Only tested on Android. Any iOS app that can publish an MQTT message should work
— the four fields above are all the information the broker needs — but nobody
here has verified it, so it is written down as untested rather than as a claim.

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

Note that the NTP sync and the certificate validation apply in the simulator
exactly as they do on hardware, and neither has been re-tested there since
validation was turned on. If the simulated board never reaches the broker, check
the clock line in the serial log before assuming anything else is wrong.

Simulate what has **states**: firmware, protocol, reconnection. Calculate what
has **numbers**: current, power, dissipation. Measure what has **tolerances**:
the real supply, the real siren, the real relay. Wokwi answers the first
question only, and no simulator will tell you whether your supply holds up.

---

## Known limitations

Written down rather than hidden, because you are about to trust this thing.

- **The panel stores its credential in `localStorage` in clear text.** Anyone
  holding the unlocked device can read it. The blast radius is bounded by that
  credential being publish-only, but it is a real trade and it was made on
  purpose.
- **Commands are effectively at-most-once — accepted, not overlooked.** The
  device subscribes at QoS 1, every publisher sends at QoS 0, and MQTT downgrades
  to the lower of the two. A QoS 0 publish is lost silently when the connection
  is already dead but the client does not know it yet, and a phone handing over
  between WiFi and mobile data is the realistic version of that. It is left as an
  edge case on purpose: the Android shortcut app exposes no QoS setting at all,
  so raising it on the web panel alone would make the two triggers behave
  differently without making the one people actually use any more reliable.
- **No delivery confirmation.** The panel reports that it published, not that the
  siren sounded. There is no acknowledgement topic. Together with the point
  above, treat a tap as a request rather than a guarantee — and confirm by ear.
- **The device depends on NTP.** Certificate validation needs a real clock, so a
  network that blocks outbound UDP 123 leaves the alarm permanently
  disconnected.

---

## License

MIT — see [LICENSE](LICENSE).

Worth reading the last paragraph of it before you build one. This drives 12 V
and a 130 dB siren, it is published AS IS with no warranty of any kind, and
what you wire up is your responsibility.
