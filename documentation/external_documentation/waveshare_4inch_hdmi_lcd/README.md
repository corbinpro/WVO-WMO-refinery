# Waveshare 4inch HDMI LCD — reference + setup notes

Operator display for the refinery's Raspberry Pi 2 (shows the `refinery --kiosk`
console on tty1).

- Product page: https://www.waveshare.com/4inch-hdmi-lcd.htm (SKU 12030)
- Wiki: https://www.waveshare.com/wiki/4inch_HDMI_LCD
- Retrieved 2026-10-22. Sections 1–6 summarize the vendor wiki and manual. Section 7 covers how the screen fits this project.

## Files in this folder

| File | What it is |
|---|---|
| `4inch_HDMI_LCD_User_Manual_EN.pdf` | Vendor user manual (6 pp). It is older than the wiki and uses the `LCD-show` driver script; see section 6 |
| `4inch_HDMI_LCD_panel_dimension.pdf` | Bare LCD panel drawing: panel outline, FPC pinout, panel electrical specs. It does **not** show the PCB outline |
| `4inch_HDMI_LCD_3D_Drawing.zip` | STEP model of the whole board assembly (`4inch-hdmi-lcd_asm.stp`, 22 MB unzipped). Use it for the enclosure/housing design |
| `connection_pi3bplus.jpg` | Wiki photo of the screen mounted on a Pi 3B+. The Pi 2 has the same header and HDMI positions |
| `overlays/waveshare-ads7846.dtbo` | Touch overlay that the current wiki tells you to copy to `/boot/firmware/overlays/` |
| `overlays/Ads7846_waveshare.zip` | Alternate touch overlay with de-jitter settings (`.dtbo` + `.dts` source); see section 5 |

Not downloaded: the "3D Preview" zip (a 20 MB Windows `.exe` viewer for the same
STEP model), the SDFormatter/Win32DiskImager/PuTTY Windows tools, and the CE/RoHS
certificates.

## 1. Specs

| Item | Value |
|---|---|
| Panel | 4" IPS, 170° viewing angle |
| Resolution | 480 × 800, **native portrait** (the old manual says 800×480; it means the same panel rotated) |
| Video | HDMI (the board's HDMI connects to the Pi's through the U-shaped adapter that ships with it) |
| Touch | Resistive, ADS7846-compatible controller on **SPI0**, comes with a stylus |
| Backlight | Switch on the back of the board: must be **ON** for an image to show. It can be turned off to save power |
| Power | From the Pi's 5V/3.3V header pins |
| Also works as | A plain HDMI monitor on a PC (no touch) |

## 2. Header pinout (26-pin, mates with Pi pins 1–26)

The LCD does not use the pins marked NC.

| Pin | Signal | Pi GPIO | Notes |
|---|---|---|---|
| 1, 17 | 3.3V | — | power in |
| 2, 4 | 5V | — | power in |
| 6, 9, 14, 20, 25 | GND | — | |
| 19 | TP_SI | GPIO10 (SPI0 MOSI) | touch SPI data in |
| 21 | TP_SO | GPIO9 (SPI0 MISO) | touch SPI data out (the wiki labels it "TP_SCL") |
| 22 | TP_IRQ | GPIO25 | low while the panel is pressed |
| 23 | TP_SCK | GPIO11 (SPI0 SCLK) | touch SPI clock |
| 26 | TP_CS | GPIO7 (SPI0 CE1) | touch chip select, active low |
| 3, 5, 7, 8, 10–13, 15, 16, 18, 24 | NC | | free for other uses |

## 3. Basic setup (vendor procedure, current wiki)

1. Seat the 26-pin header on Pi pins 1–26 and fit the HDMI adapter. Set the backlight switch to ON.
2. Add this to the end of `config.txt` (`/boot/firmware/config.txt` on Bookworm):
   ```
   hdmi_group=2
   hdmi_mode=87
   hdmi_timings=480 0 40 10 80 800 0 13 3 32 0 0 0 60 0 32000000 3
   hdmi_drive=1
   hdmi_force_hotplug=1
   dtoverlay=waveshare-ads7846,penirq=25,xmin=150,xmax=3900,ymin=100,ymax=3950,speed=50000
   ```
3. Copy `overlays/waveshare-ads7846.dtbo` to `/boot/firmware/overlays/` (on older OS versions, `/boot/overlays/`).
4. Reboot. The screen can take about 10 s to show anything.

**KMS caveat:** the `hdmi_*` lines only apply to the old firmware display
driver. Bookworm enables `dtoverlay=vc4-kms-v3d` by default, and under KMS the
kernel reads the panel's EDID and ignores `hdmi_timings`. The wiki's own advice
applies: if the screen comes up correctly from EDID, leave the timing lines out.
If you need to force the mode under KMS, set it on the kernel command line
(section 4).

EDID timing reference: pixel clock 32.00 MHz, H active 480 / blanking 128 /
front porch 40 / sync 48, V active 800 / blanking 45 / front porch 13 / sync 3,
image size 108 × 68 mm.

## 4. Rotation

The native orientation is portrait (480 wide × 800 tall).

**Console only / Lite (this project: kiosk on tty1, no desktop).** Put this at the start of the single line in
`/boot/firmware/cmdline.txt`:
```
video=HDMI-A-1:480x800M@60,rotate=90
```
`rotate=90` turns it 90° clockwise, `180` upside down, and `270` 90° counter-clockwise.
(The wiki's example says `400x1280M`, which was copied from another product. Use 480x800.)
The wiki also adds `fbcon=rotate:N` to rotate the text console. Its table pairs
`fbcon=rotate:3` with 90° and `fbcon=rotate:1` with 270°. Don't stack `fbcon=rotate` on top of
`video=...,rotate=` unless the result is wrong, because the two can double-rotate.

**Bookworm desktop:** open the Screen Configuration app, then go to Screen → HDMI-A-1 → Touchscreen and tick
"Waveshare Waveshare". Pick the angle under Orientation, then click Apply. This rotates the display and
touch together. Under wayfire you can instead add this to `~/.config/wayfire.ini`:
```
[output:HDMI-A-1]
mode = 800x480@60
transform = 270
```

**Bullseye / X11:** if KMS is loaded, add `xrandr -o 1` to `/etc/xdg/lxsession/LXDE-pi/autostart` (0/1/2/3 = 0/90/180/270°).
If KMS is not loaded, put `display_rotate=3` in config.txt (here 1 = 270° and 3 = 90°). Touch has to be
rotated separately in `/usr/share/X11/xorg.conf.d/99-calibration.conf`:

| Rotation | `Calibration` | `SwapAxes` |
|---|---|---|
| 0° | `208 3905 288 3910` | 0 |
| 90° | `3905 208 288 3910` | 1 |
| 180° | `3905 208 3910 288` | 0 |
| 270° | `208 3905 3910 288` | 1 |

## 5. Touch calibration and jitter

Use the overlay parameters to set the touch range. Raise or lower `xmin`/`xmax`/`ymin`/`ymax` until
the edges respond. For example, lowering `xmin` by 100 extends the touch area 100 counts to the left.
To measure the actual values:
```
sudo apt-get install evtest
sudo evtest          # choose the ADS7846 device, then touch the left/right/top/bottom edges
```
Then set them, for example:
```
dtoverlay=waveshare-ads7846,penirq=25,xmin=164,xmax=4010,ymin=154,ymax=3758,speed=50000
```
Note: the wiki's calibration example spells these `x_min=`/`x_max=`. The parameters inside
`waveshare-ads7846.dtbo` are actually named `xmin xmax ymin ymax` (checked against
the overlay's `__overrides__`). The other available parameters are `cs speed penirq penirq_pull
swapxy pmin pmax xohms`.

**Jitter:** `overlays/Ads7846_waveshare.zip` contains `ads7846_waveshare.dtbo`, which adds
debounce settings (`debounce-max 100`, `debounce-tol 2`, `debounce-rep 1`, `spi 500 kHz`,
IRQ on GPIO25, CS1). Copy it to the overlays dir and use `dtoverlay=ads7846_waveshare`
instead of the line above. Touch becomes less sensitive in exchange for less jitter. It has no
calibration overrides of its own.

## 6. Old manual's method (LCD-show)

The PDF manual installs the driver with Waveshare's script:
```
git clone https://github.com/waveshare/LCD-show.git
cd LCD-show/ && sudo ./LCD4-800x480-show      # optional arg: 0 / 90 / 180 / 270
```
It also calibrates through X11 with `xinput-calibrator` + `xserver-xorg-input-evdev`.
That script rewrites config.txt for old Raspbian/X11 setups. **Don't use it on the
refinery Pi** (Bookworm, KMS, console kiosk). Use sections 3–5 instead.

## 7. Using it on the refinery Pi (project notes — not from Waveshare)

Unverified: the Pi was offline (no route to 192.168.0.89) when this was
written, so none of this has been checked on the actual unit.

- **Decided (2026-10-22): HDMI only, separate power, no connection to the Pi's GPIO.**
  The LCD is not seated on the Pi header. The Pi's pins (including 8/10 for the STM32 UART)
  stay free, so there are no pin conflicts. Use a regular HDMI cable (full-size on the Pi 2)
  instead of the U-adapter.
  - **Touch will not work** (no SPI/IRQ wiring). Skip the `dtoverlay` line in section 3 and all of section 5.
  - **Power input:** Waveshare doesn't document a power jack on this board. The header's 5V pins
    (2/4) and GND (6, 9, 14, 20, 25) are the only power input it documents. If the board has no
    USB power port, wire the separate **5V** supply to the LCD's own socket at pin 2 or 4 (+) and
    pin 6 (GND). Don't connect the 3.3V pins.
  - The HDMI cable already shares ground between the Pi and the LCD, so no extra ground wire is needed.
    The HDMI cable does not power the LCD.
- **Fits the console frame:** with the default 8×16 console font, portrait is
  60 cols × 50 rows and landscape (`rotate=90`) is 100 cols × 30 rows. The GPIOtests frame
  is about 55 cols wide and about 25 rows tall, so either orientation works. Pick whichever
  suits the enclosure.
- **Backlight:** the switch on the back is manual. There is no GPIO backlight control on this
  model.
- **STM32 SPI1 reservation:** the pin plan reserves STM32 SPI1 (PA5–7) for a
  front-panel screen. This screen connects to the Pi over HDMI instead. If it *is* that front
  panel, the reservation can be freed.
- Bring-up: (1) connect HDMI and 5V, set the backlight switch to ON, and check that the kiosk comes up on
  tty1 from EDID with no config changes; (2) set rotation in cmdline.txt if needed.
