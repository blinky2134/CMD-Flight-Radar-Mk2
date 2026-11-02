# OWLOS Flight Radar

![The 3.5 inch board: the planes around home, a tap on one, the map turned and zoomed, the three looks](shots/st7796/tour.gif)

**Flash it from the browser: [kirindenis.github.io/OWLOS-FLIGHT-RADAR](https://kirindenis.github.io/OWLOS-FLIGHT-RADAR/)**

The planes around your home on a small ESP32 board with a screen. Each plane is a silhouette turned the
way it flies, coloured by its height, with a trail behind it. Tap one for its callsign, type, height and
speed. The planes come from the free community ADS-B networks, or from Flightradar24 if you have a key.
The map under them comes from OpenStreetMap.

## What it does

- A radar scope with a sweeping beam over the map, or a plain square map, or the same map in
  OpenStreetMap's own colours. One button switches between the three.
- Real distances and bearings round your home. Drag the rim to turn the map the way the screen stands,
  drag inside to zoom from 2 to 250 km (or nautical miles, or miles).
- The nearest planes in a panel, every flight in a list, the picked one with all it is known about it.
- Five colour themes, the colour of a plane always says its height.
- Without WiFi it flies a made-up demo sky and says DEMO in big letters. The WiFi is set on the touch
  screen itself, or from the site over the USB cable.
- The map pictures are downloaded once and kept in the board's flash: a range it has shown comes back
  at once, even without WiFi.

## The boards

- **2.8" CYD** ("cheap yellow display", ESP32-2432S028R): ESP32, 320x240, resistive touch. Sold with three
  different screens, one build fits them all (see below).
- **3.5" CYD** (ESP32-3248S035, R or C version): ESP32, ST7796 480x320, resistive or capacitive touch. The
  second core draws half the radar.
- **1.28" round** (Elecrow CrowPanel): ESP32-C3, GC9A01 240x240, capacitive touch.
- **3.5" RPi LCD** (Waveshare RPi LCD (A) on the OWLOSAirQuality board): ESP32, ILI9486 480x320, no touch.

## Flash it

1. Open [the site](https://kirindenis.github.io/OWLOS-FLIGHT-RADAR/) in Chrome or Edge on a computer.
   Flashing goes over Web Serial: phones and Firefox can't do it.
2. Plug the board in with a USB cable that carries data.
3. Press CONNECT & FLASH under your board, pick its port and say yes to erase.
4. No port in the list? On Windows the CH340 driver may be missing. Won't connect? Hold BOOT while you
   plug it in.
5. Boards with touch ask once for 5 crosses to be pressed, so the touch knows where the screen is.

## WiFi and home

- On the 2.8" and 3.5" CYD: tap **SET UP WIFI** on the demo's card (or SET, the WIFI row), tap your
  network, type its password, JOIN.
- From a phone or computer, join the open WiFi network **OWLOS-Setup**. Its setup page should open
  by itself; if it does not, visit **http://192.168.4.1/**. Save up to five WiFi networks, each with
  a UK town, address or postcode. When the board joins one, it looks up that place with OpenStreetMap
  and centres the radar there. The page also changes the aircraft source and screen/map settings.
  The setup network stays available so settings can be changed later.
- From the site, step 2: the WiFi, your home point, where the planes come from and how the screen shows,
  sent over the same USB cable. The round board and the RPi LCD take their WiFi this way.
- Till it gets a home the board sits in Trenčín, Slovakia. The plane network only hears the home rounded
  to about a kilometre: enough to say where to look, not where you live.
- The password and the Flightradar24 key go into the board and stay there. The board never shows them
  and never sends them back.

## Where to tap

The pictures are the 3.5" board, the 2.8" one has the same screens a little smaller.

### The radar

![The radar look with arrows to where to tap](shots/guide/radar.png)

- **Tap a plane** for its details in the panel. A row of the panel's list picks the same plane.
- **Drag the rim** to turn the map: match it to the way the screen stands. Back near north it snaps to
  north up.
- **Drag inside** to zoom: away from home in, toward home out. BOOT steps through 5, 10, 25, 50, 100 and 200.
- **The look**, top right, goes round RADAR, MAP and CLASSIC.
- **HUE** changes the colours, **LIST** shows every flight, **SET** has the settings.
- **Colour is height**: grey on the ground, warm under 10 000 ft, the theme's colour up to 25 000, white
  above. Emergency squawks blink.

### A plane picked

![A plane picked, with arrows to its details, NEXT and X](shots/guide/plane.png)

- The panel says its callsign, type and registration, height, speed and track, how far it is and which
  way, its squawk. Flightradar24 adds where it comes from and where it goes.
- The line ahead of it is where it will be in a minute.
- **NEXT** picks the next nearest one, **X** lets it go.

### The map

![The map look with arrows to the grid, the look, the three dots and the scale](shots/guide/map.png)

- MAP is a map over the whole screen, north up, home in the middle. CLASSIC is the same map in
  OpenStreetMap's own colours.
- The grid's lines are whole degrees or minutes, named at the edges. The scale is in the corner.
- **The three dots** slide the panel in and out. A picked plane brings it by itself.
- **Drag** anywhere on the map to zoom.

### LIST

![The list of flights with arrows to a row, ZOOM and BACK](shots/guide/list.png)

- Every flight in range, the nearest first. Drag to scroll.
- **Tap a row** and that plane is picked on the scope. **ZOOM** steps the range, **BACK** goes back.

### SET

![The settings page with arrows to its buttons](shots/guide/setup.png)

- The range and its units, height in feet or metres, map up, the screen upside down, labels, colours,
  the look, the map, where the planes come from, the WiFi and the touch alignment.
- **Tap a button** to change it, **minus and plus** for numbers. Drag to see the rest.

### No WiFi yet: the demo

![The demo sky with DEMO in big letters and the SET UP WIFI button](shots/guide/demo.png)

- Without WiFi the board flies a made-up sky and says **DEMO** in big letters. Under it why: no WiFi
  set up yet, the one set can't be seen, or its password looks wrong.
- **SET UP WIFI** opens the networks.

### The networks

![The WiFi networks the board hears, with arrows](shots/guide/wifi.png)

- The networks the board hears, the strongest first. A lock asks for a password, OPEN does not, SET is
  the one in use.
- **Tap your network**. **LOOK AGAIN** looks round once more. The ESP32 hears 2.4 GHz networks only.

### The password

![The keyboard for the WiFi password, with arrows](shots/guide/keys.png)

- Dots for what you typed, the last letter shows for a moment. **SHOW** shows all of it.
- **ABC** gives one capital, **#+=** the signs, **DEL** rubs out, held it goes on.
- **JOIN**: the board joins the network and the real planes come in a few seconds.

### The 2.8" CYD: which screen

![The 2.8 inch board in its first seconds, the card that says to press BOOT](shots/guide/screen.png)

- It is sold with three different screens. It starts as the USB-C one (v2), the one most boards have.
  Wrong colours or a mirrored picture: press **BOOT** in the first seconds after power on. It goes v2,
  v1 (micro USB), v3 (ST7789) and keeps the one you stop at.

### The round glass

![The round glass with arrows to the menu, the rim and a plane](shots/guide/round.png)

- The whole glass is the scope. **Tap an empty spot** for HUE, the look, LIST and SET.
- **Tap a plane** for its card under home, tap the card for the next one.
- The rim turns the map and inside zooms, like on the square boards.

### And

- **Hold BOOT** to go back to the scope from any page.
- **Hold a finger 4 s** on an empty spot to align the touch again.
- **The RPi LCD** has no touch: BOOT steps through the planes, held it changes the range.

## If it ever stands still

Plug it into a computer and press READ WHAT IT HAS on the site, step 2. The board says how long it has
been running, what it started after (power on, the reset pin, a watchdog, a crash) and, to a minute, how
long the run before lasted. It keeps that through a reset and a power cut. If opening the port restarted
it, the run before is the one that stood still.

## Build it yourself

The firmware is ESP-IDF 5.5. The screen code in `core/` is plain C, the same files build the firmware
and a PC preview.

- `firmware/BUILD.BAT <screen> [COMx]` builds one screen's firmware (and flashes it to COMx). Screens:
  `ili9341` (2.8" CYD, the default), `st7796` (3.5" CYD), `gc9a01` (round), `ili9486` (RPi LCD).
  Each screen has its own build folder and sdkconfig.
- `.github/workflows/pages.yml` builds all four in Espressif's ESP-IDF image and publishes the site
  with them on GitHub Pages, on every push to main.
- `tools/preview/BUILD.BAT <screen>` (Visual Studio 2022) draws every page on the PC into `shots/`.
  `TOUR.BAT` films the how-to tour, `GIF.BAT` makes the GIF above from it (both with ffmpeg).
- `tools/Guide/GUIDE.BAT` (.NET 8) puts the arrows on the shots: `shots/guide/`, from `notes.txt`.
- `tools/FontBake` bakes the fonts in `core/font_*.c`.

What is where:

- `core/`: the sky (the networks' answers read piece by piece), the demo sky, the map from
  OpenStreetMap's tiles, the drawing, the fonts, the pages and the touch.
- `firmware/`: the boards, the screens and the touch chips, WiFi and HTTPS, the tiles in the flash,
  the settings in NVS, the USB console.
- `site/`: the page that flashes the boards and sends them their settings.

## The USB console

The site talks to the board over the USB cable at 115200 baud, a line each way. Scripts can do the same.

- `OWLOS HELLO` is answered with `OWLOS READY flight-radar <version>`.
- `OWLOS GET` is answered with `OWLOS CFG {...}`: the network's name, home (`lat`, `lon`, `place`,
  `country`), `src`, whether a `key` is stored, `up`, `flip`, `theme`, `look`, `map`, `version`,
  `uptime` and `ran` in seconds, `reset`, and `screen` on the 2.8" CYD. Never the password, never the key.
- `OWLOS SET {...}` with any of `ssid`, `pass`, `key`, `lat`, `lon`, `place`, `country`, `src` (`free` or
  `fr24`), `up` (degrees), `flip`, `theme` (0 to 4), `look` (0 radar, 1 map, 2 classic), `map`, `screen`
  (0 v1, 1 v2, 2 v3). What is left out stays as it is. Answered with `OWLOS OK` or `OWLOS ERR <why>`.

## Data and licences

- The code is MIT licensed, see `LICENSE`.
- Planes: [adsb.lol](https://adsb.lol/) (ODbL) and [adsb.fi](https://adsb.fi/), or
  [Flightradar24](https://fr24api.flightradar24.com/) with your own key.
- The map and the town names: © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors (ODbL).
- Flashing from the browser: [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
