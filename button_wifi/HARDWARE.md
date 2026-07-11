# Human Fruit Machine — Hardware & Wiring

Booth controller = one **ESP32 DevKit (38-pin, WROOM)**. It hosts the web app
(WiFi AP + web server), reads the five buttons, and drives the WS2812B reel
lights and the button LEDs. Firmware: `button_wifi.ino`.

## Power — one USB power bank, no mains, no 12 V

The illuminated buttons were sold as "12 V", but each LED module is just a
5 mm LED with a series resistor. Swapping that resistor (**742 Ω → 220 Ω**)
lets the LED run from **3.3 V**, so the button LEDs are driven **directly from
ESP32 GPIO pins**. That removes the 12 V supply entirely — the whole machine
runs from a **single 5 V USB power bank**.

```
   USB power bank (5V, >=3A)
        |
        +--- 5V ---> ESP32  (5V / VIN pin)  --> on-board 3.3V reg --> logic
        |                                        (3.3V GPIO highs drive button LEDs)
        +--- 5V ---> WS2812B strips (V+)   <-- straight from the bank, NOT via the ESP32
        |
        +--- GND --> common ground to EVERYTHING  (ESP32, strips, buttons)
```

Rules:
- **Strip 5 V comes straight from the bank**, not through the ESP32 board — the
  strip can pull amps the ESP32's traces shouldn't carry.
- **Common ground** between bank, ESP32 and strips, or nothing works.
- Keep `LED_BRIGHT` ≤ 160. 216 reel LEDs at a capped brightness draw ~1.5–2 A
  typical; a 5 V / 3 A bank is fine. (Full white on every LED would be ~13 A —
  which is why brightness is capped and effects are colourful, not white.)
- Button LEDs sink only a few mA each (direct from GPIO), so they add nothing
  meaningful to the budget.

## GPIO allocation

**Buttons** — 5 in a row, left→right: **blue · white · green · yellow · red**.

| Function | GPIO | Direction | Wiring |
|---|---|---|---|
| Red switch (SPIN) | 19 | input, pull-up | switch → GND |
| Blue switch | 33 | input, pull-up | switch → GND |
| White switch | 21 | input, pull-up | switch → GND |
| Green switch | 22 | input, pull-up | switch → GND |
| Yellow switch | 32 | input, pull-up | switch → GND |
| Red LED | 23 | output (PWM) | GPIO → 220 Ω (in module) → LED → GND |
| Blue LED | 25 | output (PWM) | GPIO → 220 Ω (in module) → LED → GND |
| White LED | 26 | output (PWM) | GPIO → 220 Ω (in module) → LED → GND |
| Green LED | 27 | output (PWM) | GPIO → 220 Ω (in module) → LED → GND |
| Yellow LED | 17 | output (PWM) | GPIO → 220 Ω (in module) → LED → GND |

**LED strips (WS2812B, 5 V)** — data from GPIO, power from the bank:

| Zone | GPIO | Notes |
|---|---|---|
| Reel-window borders ×3 | 18 | one chain, left→right (win1 DOUT→win2 DIN→win3 DIN) |
| Top cabinet | 13 | optional ambient zone |
| Main panel | 4 | optional ambient zone |
| Buttons panel | 16 | optional ambient zone (WROOM only) |

**Other:** onboard LED = GPIO 2 (AP-up / sync activity).

Set `NUM_TOP / NUM_MAIN / NUM_BTNPANEL / NUM_REEL` in `button_wifi.ino` to your
real segment counts. A 25 × 35 cm window border ≈ 72 LEDs at 60 LEDs/m.

## Wiring each part

**Button switch** (needs no power):
```
   GPIO 19 (red) ----[ COM ]  button  [ NO ]---- GND
```
Internal pull-up holds the pin high; pressing pulls it to GND. Repeat for the
other four switches on their GPIOs. Daisy-chain all the COM terminals to one
GND pin.

**Button LED** (3.3 V, direct from GPIO — resistor is inside the module):
```
   GPIO 23 (red LED) ----[ 220 Ω + LED, in the button ]---- GND
```
Software PWM (`ledcWrite`) gives breathing / flashing / countdown / win
effects. Not permanently lit.

**Reel strip** (per joint: 3 wires, direction matters):
```
   GPIO 18 --[330 Ω]--> DIN [win1] DOUT --> DIN [win2] DOUT --> DIN [win3]
   5V (bank) ---------> V+ on each window (in parallel)
   GND -------------------> GND on each window (in parallel)
```
1000 µF cap across 5 V/GND near the first window.

## ESP32 DevKitC (38-pin) pin-out — assignments marked ●

Positions below are for the common **DevKitC V4**. If your board's silk print
differs, **go by the GPIO number**, not the physical position.

```
                        ┌───────── USB ─────────┐
              ●  3V3  ──┤ 3V3               GND ├── GND  ●
                 EN   ──┤ EN              GPIO23├── ● RED LED
                GPIO36──┤ VP              GPIO22├── ● GREEN switch
                GPIO39──┤ VN               GPIO1├── TX0
                GPIO34──┤ 34               GPIO3├── RX0
                GPIO35──┤ 35              GPIO21├── ● WHITE switch
 ● YELLOW sw    GPIO32──┤ 32                GND ├── GND
 ● BLUE   sw    GPIO33──┤ 33              GPIO19├── ● RED switch (SPIN)
 ● BLUE  LED    GPIO25──┤ 25              GPIO18├── ● REEL strip data
 ● WHITE LED    GPIO26──┤ 26               GPIO5├── (free)
 ● GREEN LED    GPIO27──┤ 27              GPIO17├── ● YELLOW LED
                GPIO14──┤ 14              GPIO16├── ● strip: buttons panel
                GPIO12──┤ 12               GPIO4├── ● strip: main panel
                 GND  ──┤ GND               GPIO0├── (boot strap — leave free)
 ● strip: top   GPIO13──┤ 13               GPIO2├── ● onboard LED
                GPIO9 ──┤ SD2              GPIO15├── (boot strap — leave free)
                GPIO10──┤ SD3               GPIO8├── (flash — do not use)
                GPIO11──┤ CMD               GPIO7├── (flash — do not use)
 ● 5V from bank  5V   ──┤ 5V                GPIO6├── (flash — do not use)
                        └───────────────────────┘
```

Legend: **●** = used by this build. `GPIO6–11` are the SPI flash — never wire
to them. `GPIO0 / 2 / 5 / 15` are boot-strapping pins; we only use GPIO 2
(onboard LED, safe as output after boot). `GPIO34–39` are input-only with no
pull-ups — never put a button there.

## Quick sanity checklist

- [ ] USB bank → ESP32 5V pin **and** strips (parallel), common GND.
- [ ] All five switch COMs → GND; NOs → their GPIOs (19/33/21/22/32).
- [ ] All five LEDs → their GPIOs (23/25/26/27/17), resistor inside the module.
- [ ] Reel windows chained on GPIO 18, left→right, 330 Ω on the data line.
- [ ] `NUM_REEL` etc. set to real counts; `LED_BRIGHT` ≤ 160.
- [ ] On power-up the self-test chases the button LEDs then races the reels —
      if a button LED or window stays dark, that connection is the problem.
