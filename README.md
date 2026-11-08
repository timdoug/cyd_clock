# CYD Clock

Firmware for the ESP32 "Cheap Yellow Display" that turns it into a WiFi wall
clock, with more timekeeping machinery than the hardware strictly calls for.
The board is the common ESP32-2432S028R (2.8") or ESP32-2432S024R (2.4"): an
ESP32, a 320x240 ILI9341 panel, XPT2046 resistive touch, and an RGB LED.

Highlights:

- Time sync by chrony: chronyd from chrony's main branch (4.9 plus the
  removal of leap second support), built from unmodified upstream source and
  ported to ESP-IDF, with authenticated time via NTS (RFC 8915).
- The status lines and stats pages show chronyd's own reports, worded and
  formatted the way `chronyc` prints them.
- Hundredths of a second on the clock face, timed so that the digits change
  right on the second boundary.
- A 1PPS output on the red LED and a header pin.
- 107 interface languages, with dates that are formatted according to Unicode
  CLDR, including the Persian and Thai Buddhist calendars, native digits, and
  right-to-left scripts.
- Byte-for-byte reproducible release images.

## Timekeeping

Time is kept by [chrony](https://chrony-project.org/). A commit from chrony's
main branch, after the 4.9 release, is vendored unmodified in
`third_party/chrony/src`: it drops leap second support, in line with the 2022
CGPM resolution to discontinue leap seconds by 2035. `make update-chrony` exports
it from chrony's git repository after checking the GPG signature on the 4.9
tag it builds on. Source selection, sample
filtering, the frequency estimate, clock steering, NTS, and pool handling are
all chronyd's. The configuration follows chrony's own example:

```
pool <server> iburst [maxsources 2 nts] [ipv4|ipv6]
driftfile /chrony/drift
makestep 1.0 3
nocerttimecheck 1
```

The ESP-IDF port lives in `components/chrony/port`, where chrony's
`./configure` would otherwise pick platform code:

- A system clock driver. ESP-IDF has no kernel frequency control, so the
  frequency is emulated with periodic `adjtime()` calls, as chrony's legacy
  macOS driver does. The driver is derived from that one, with three changes
  for this platform: the drift-removal interval is capped at 64 s, requests
  are clamped to the range ESP-IDF's `adjtime()` accepts, and the sign of the
  rounding term is corrected.
- NTS backends for chrony's TLS and SIV interfaces, built on mbedTLS (chrony
  ships GnuTLS and Nettle ones). mbedTLS has no SIV mode, so AES-SIV-CMAC-256 is
  implemented on its CMAC and AES-CTR primitives.
- An entry point in place of `main.c` that runs chronyd in a FreeRTOS task,
  takes configuration changes from the UI, and copies chronyd's reports for
  the display once a second. WiFi drops and reconnects reach chronyd as
  chronyc's `offline` and `onoffline` commands, as NetworkManager sends them
  on Linux: sources stop polling while the network is down and poll again,
  with a burst, as soon as it is back.
- A small filesystem in NVS, so that chrony's `driftfile` survives reboots
  and the next boot starts out close to calibrated.
- Stand-ins for POSIX pieces that newlib and lwIP lack (`pipe()`, `glob()`,
  close-on-exec, and a `getaddrinfo()` that returns both address families,
  skipping a family with no usable address on the interface).

The clock is a client only: there's no NTP server, no `chronyc` command
socket, and no RTC.

Packets are timestamped in software when chronyd's event loop wakes, as chrony
does on systems without kernel timestamping. WiFi transmit latency therefore
shows up as noise on the request leg, which chrony's delay-weighted frequency
estimate absorbs. WiFi power saving is disabled, since modem sleep adds
one-way delay that biases the offset. On one 2.8" board syncing to Cloudflare
over NTS, the crystal measured about 5 ppm slow, chrony's skew fell below
1.5 ppm within an hour, and the system clock stayed within about 0.2 ms of
chrony's estimate.

The status lines under the clock show the reference and its stratum (or
`Not synchronised` and the server being tried), the system clock's current
error (`System time`), and the crystal's frequency error (`Frequency`), all
in chronyc's wording. Tapping the status area opens three pages, `chronyc
tracking`, `chronyc sources`, and `chronyc authdata`, folded into 40 columns;
tap to go to the next page. On the tracking page, `Ref time` drops the
weekday, `System time` reads `0.000012345 sec fast`, and `Leap status`, which
only tells synchronised from not now that chronyd has no leap seconds, is
labelled `Sync status`.

### Servers and networking

- Supports IPv4 and IPv6 through chrony's `ipv4` and `ipv6` source options.
  With neither, both families are used, IPv6 first when the interface has a
  global IPv6 address.
- A battery-less board boots at 1970. chronyd resolves NTP's 136-year era
  ambiguity against a split date that's compiled in (`NTP_ERA_SPLIT` in
  `components/chrony/port/include/config.h`), and the first update steps the
  clock.
- 26 server presets, covering the big public services (Apple, Cloudflare,
  Google, Microsoft, the NTP pool) and national metrology institutes (NIST,
  PTB, NPL UK, NICT, METAS, KRISS, and others). Presets that support NTS are
  marked `[NTS]`.

### NTS

With NTS, each NTP exchange carries an authenticator, so a man in the middle
can't shift the clock. The NTS-KE handshake is done over TLS 1.3, with keys
derived from the RFC 8446 exporter, and every packet is authenticated with
AES-SIV-CMAC-256. NTS is a setting, on or off, which is chrony's `nts` source
option: when it's on, unauthenticated time is never accepted.

Each NTS source runs its own key exchange, and each costs about 31 KB of heap
while it runs (chronyd's 16 KB message buffer plus TLS state), on top of about
10 KB per pool record. So NTS pools use two addresses (`maxsources 2`), and
the TLS backend runs one key exchange at a time; chronyd retries a deferred
one after 2 s.

Some NTS-KE servers (Cloudflare's) close the connection about 2 s after
accepting it, which leaves little room for a full handshake when the ESP32
verifies their ECDSA certificate chain in software. One key exchange at a
time gives each handshake the whole CPU, which is what lets it finish in
time; two at once each took long enough to be cut off. The TLS backend also
stores a certificate that verified against the root store, for a day, and on
the next handshake accepts exactly that certificate in place of a chain
check, which shortens the later key exchanges. If the server has still
closed the connection by the time a handshake completes, the backend reports
it as closed, so chronyd retries after its short interval for a closed
handshake (16 s) instead of backing off for about 17 minutes, and the retry
uses the stored certificate. Against Cloudflare, the clock syncs about 11 s
after boot.

Certificates are checked against Mozilla's root store, which is vendored in
`third_party/ca-certificates/` and refreshed with `make update-certs`.
Validity dates are checked too, except for key exchanges before the first
clock update, since the clock starts in 1970 (chrony's `nocerttimecheck 1`).
OTA downloads use the same store, and an update doesn't start until the
clock is set.

## The clock face

Time is shown in seven-segment digits down to hundredths of a second. Pushing
pixels to the panel over SPI takes a few milliseconds (more when several digits
change at once), so the redraw latency is measured on the device and each
redraw starts early by that amount. There's a separate running average for each
kind of redraw: a hundredths-only tick, a seconds rollover, an hour rollover,
and midnight with the date.

Display ticks come from an ISR-dispatched hardware timer that's aligned to the
disciplined clock, and rendering happens on the other core from WiFi and lwIP.

## 1PPS

The red LED and a header pin (GPIO27 on the 2.8" board, GPIO22 on the 2.4")
pulse once a second, with the rising edge on the second boundary. The pulse
task wakes just before the boundary and spins out the last stretch with
interrupts masked, which puts the header pin's edge within about a microsecond
of the disciplined clock's second. The LED goes through the PWM peripheral,
which adds up to about 50 us while the new duty cycle latches. LED brightness
is adjustable, and at zero the LED goes dark while the header pin keeps
pulsing.

## Languages

The interface is available in 107 languages, each listed in the picker by its
own name and in its own script, ordered by the Unicode root collation.

The date line follows each language's CLDR pattern, which is adopted as-is
wherever it fits on the screen. Here are a few examples, all for the same day:

| Language | Date line |
|---|---|
| English | Mon, Jul 6, 2026 |
| German | Mo., 6. Juli 2026 |
| Japanese | 2026年7月6日(月) |
| Korean | 2026년 7월 6일 (월) |
| Persian | ۱۵ تیر ۱۴۰۵ |
| Thai | 6 ก.ค. 2569 |
| Hebrew | יום ב׳, 6 ביולי 2026 |
| Burmese | ၂၀၂၆ ဇူ ၆၊ တနင်္လာ |

Persian and Pashto use the Solar Hijri calendar, Thai uses the Buddhist era,
and ten languages use their own digits (Bengali, Devanagari, Eastern Arabic,
Myanmar, and so on). Arabic, Hebrew, Persian, Urdu, Pashto, Sindhi, and
Shahmukhi Punjabi lay out right to left.

### Typefaces

Text is drawn from antialiased bitmaps that are generated on a Mac from the
system fonts, with each font picked to suit its script:

- Latin, Cyrillic, Greek, and Vietnamese: Menlo
- Japanese and Simplified Chinese: Hiragino Sans GB
- Traditional Chinese: PingFang TC; Cantonese: PingFang HK
- Korean: Apple SD Gothic Neo
- Armenian: Noto Sans Armenian; Georgian: Helvetica
- Arabic-script languages: Geeza Pro; Hebrew: Arial Hebrew
- Indic scripts: the Kohinoor and Sangam MN families
- Thai: Thonburi; Ethiopic (Amharic, Tigrinya): Kefa

Scripts that need shaping (Arabic joining, Indic conjuncts and reordering, Thai
and Burmese mark stacking, bidirectional text) are shaped by CoreText when the
fonts are generated. Each translated string is stored as a run of pre-shaped
glyph clusters in visual order, which the device draws left to right.

Glyphs are stored with 4-bit coverage and blended against the background in
linear light, which keeps text the same weight on dark and light backgrounds.
Labels and buttons are centered on the ink itself, since Latin and CJK glyphs
sit at visibly different heights within the same cell. Glyph data is
DEFLATE-compressed in flash and decoded into a RAM cache as it's used.

## Timezones

The timezone picker is generated from IANA tzdata. Zones in the same region
with identical offset and DST rules are collapsed into one entry named after
the most populous city, leaving 90 entries across 10 regions that cover every
distinct rule set, including half- and quarter-hour offsets. Each entry shows
its UTC offset and whether it observes DST.

## Updates

Updates are installed over HTTPS from the About screen. By default they come
from the latest GitHub release asset for the board that the clock is running
on, and the URL can be edited on the device.

The image's project name includes the board, so each clock only accepts
firmware that was built for its own board. A freshly installed image is marked
good once it boots and reaches its main loop; if it crashes or hangs first, the
bootloader rolls back to the previous version. Reinstalling the same version
takes a second tap.

## Reproducible builds

Building a release tag produces the same bytes as the files on the release
page, whether you build on macOS or Linux and wherever the checkout lives. To
check a release:

```sh
git worktree add /tmp/verify r2026.10.24.1 && cd /tmp/verify
make build && make CYD_BOARD=esp32_2432s024 build
gh release download r2026.10.24.1 -p 'cyd_clock*.bin' -D /tmp/rel
cmp build/cyd_clock-esp32_2432s028.bin /tmp/rel/cyd_clock.bin
cmp build/cyd_clock-esp32_2432s024.bin /tmp/rel/cyd_clock-esp32_2432s024.bin
```

No output from `cmp` means the images match. This needs the ESP-IDF version
that CI uses (currently v6.0.3).

Here's what goes into that:

- ESP-IDF's reproducible-build option remaps build paths in the debug info.
- There are no compile timestamps, and the NTP era split and the clock's
  anchor year are constants.
- `tools/clear_elf_sha256.py` zeroes the image's `app_elf_sha256` field after
  the build. That field hashes the whole ELF file, debug info included, and
  Espressif's macOS and Linux toolchains ship differently built copies of
  libgcc and libc, so it was the one field that varied between hosts.

With that field zeroed, `idf.py monitor` reports a (harmless) checksum
mismatch at boot.

## Hardware

| | ESP32-2432S028R (2.8") | ESP32-2432S024R (2.4") |
|---|---|---|
| Build name | `esp32_2432s028` (default) | `esp32_2432s024` |
| Backlight | GPIO21 | GPIO27 |
| Touch | separate SPI bus | shares the display bus |
| 1PPS header | GPIO27 | GPIO22 |
| RGB LED | R 4, G 16, B 17 | R 4, G 16, B 17 |

Each board's pins, panel orientation, and touch calibration live in
`main/boards/`. Supporting another variant is a matter of adding a profile
there.

Meant to run on wall power.

## Installing

Each release includes a full flash image for each board. With the board on USB:

```sh
python3 -m esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 2000000 \
    write-flash 0x0 cyd_clock-full.bin
```

Use `cyd_clock-esp32_2432s024-full.bin` for the 2.4" board. The full image also
clears saved settings. After that, updates can come over WiFi from the About
screen.

The app partitions grew from 0x1E0000 to 0x1F0000 when chrony came in, and
an OTA update can't change the partition table. A clock that's running an
older release needs one USB flash of the full image. Until then, the new image
doesn't fit its app slot, so an OTA update fails before the boot slot is
switched and the old firmware keeps running.

On first boot, the clock asks for WiFi credentials and a timezone. If the saved
network isn't reachable later, the clock still starts and keeps retrying in the
background. Tap the clock to open settings; the BOOT button also toggles them.

## Building

Install ESP-IDF v6.0.3, then:

```sh
make setup      # once, after cloning
make run        # build, flash, and open the serial monitor
```

The Makefile picks up the usual macOS USB serial ports, and you can override
that with `PORT=/dev/...`. For the 2.4" board, add `CYD_BOARD=esp32_2432s024`
to any target. After an over-the-air update, flash with `make flash`: it also
resets the OTA boot slot, so the image that you just flashed is the one that
runs.

The bundled data has its own refresh targets:

```sh
make update-tzdata   # IANA timezone rules
make update-cldr     # CLDR date data and the generated fonts (needs macOS)
make update-certs    # Mozilla CA bundle, via curl.se
make update-chrony   # chrony source (CHRONY_REF), checked against a signed tag
```

## License

GPL-2.0-only, since the firmware includes chrony, which is licensed under
version 2 of the GPL only. See `LICENSE`. The vendored CLDR data is under the
Unicode License v3 and the CA bundle under MPL 2.0; see their directories
under `third_party/`.
