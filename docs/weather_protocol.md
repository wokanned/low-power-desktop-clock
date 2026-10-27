# USART2 Weather Protocol

Status updated 2026-10-27. The project status entry is
[totalPLAN.md — 当前开发状态（2026-10-27）](totalPLAN.md#当前开发状态2026-10-27).
Weather parsing, RAM caching, rendering, and page selection are integrated in
source and selected by the Keil project. Recorded host tests have passed; the
current complete Keil link, target image, and board acceptance remain pending.
Input currently comes from USART2 commands; ESP32 networking, automatic weather
fetching, and NTP time synchronization are not implemented in this firmware.

USART2 uses the existing 115200 baud, 8 data bits, no parity, 1 stop bit
configuration. Commands are UTF-8 single lines terminated by CR, LF, or CRLF.
The WEATHER line limit is 127 bytes, excluding the line ending and terminating
NUL. Other input retains the existing 31-byte limit. Only the exact WEATHER
command family (followed by a space or comma) receives the extended limit;
unknown prefixes such as WEATHERX do not.

## Commands

```text
WEATHER 深圳,2,235,180,270,65,2,35,2026-10-26T10:30
PAGE WEATHER
PAGE CLOCK
```

`WEATHER` contains exactly nine comma-separated fields in this order:

| Field | Format |
| --- | --- |
| City | Exactly `深圳` or `湛江` |
| Condition | Integer 0 through 13 |
| Current temperature | Tenths of a degree Celsius, -999 through 999 |
| Minimum temperature | Same range |
| Maximum temperature | Same range and not below the minimum |
| Humidity | Integer percent, 0 through 100 |
| Wind direction | 0 calm; 1 N; 2 NE; 3 E; 4 SE; 5 S; 6 SW; 7 W; 8 NW |
| Wind speed | Tenths of m/s, 0 through 999; must be zero for calm direction |
| Source update time | Beijing time, exactly `YYYY-MM-DDTHH:MM`, years 2000 through 2099 |

Condition codes map to fixed labels and icons: 0 unknown, 1 sunny, 2 cloudy,
3 overcast, 4 light rain, 5 moderate rain, 6 heavy rain, 7 thunderstorm,
8 light snow, 9 moderate snow, 10 heavy snow, 11 sleet, 12 fog, and 13 haze.

Fields have no leading or trailing whitespace. Quotes, escapes, partial updates,
and extra fields are unsupported. The source timestamp is displayed only; it
does not set the RTC. A message replaces the entire cached city and weather
record only after every field and date have passed validation. Invalid input
leaves the prior record and its expiry timer unchanged.

## Runtime behavior

One weather record is held in RAM. Every valid complete record refreshes its
three-hour lifetime, including a duplicate. After three hours without a valid
record, the previous values remain available and the weather page displays
`已过期`. Expiry uses the monotonic FreeRTOS tick and remains set until another
valid record arrives. Reset clears the cache and selects the clock page.
Selecting the weather page with an empty cache displays `等待天气数据` and
placeholders. Page selection is also RAM-only and does not survive reset.

`WEATHER` updates the cache but does not select a page. `PAGE` selects the view;
updates to a hidden weather page remain cached and do not trigger an EPD refresh.
While a fault page covers the screen, PAGE changes only the page to restore
when the fault clears. The latest visible weather fields are compared with the
last completed frame; an A-to-B-to-A update during BUSY does not refresh A again.
The weather page uses the RTC time in the configured 12/24-hour format and
shows the source update time as month, day, and time.
The production default is black and white (`EPD_VENDOR_DEMO_MODE=0`,
`EPD_CLOCK_USE_GRAY4=0`). It performs a full refresh for page changes, visible
weather changes, and RTC minute changes, with no seconds displayed. The EPD
controller sleeps after a completed frame; this does not put the MCU into STOP2.

In default JustFloat mode, accepted WEATHER and PAGE commands produce no text
response. WEATHER and PAGE remain available while a FORMAT persistence request
is pending. In console mode they reply `OK WEATHER`, `OK PAGE WEATHER`, or
`OK PAGE CLOCK`. Invalid WEATHER content replies `ERR WEATHER`; damaged or
overlong lines use the existing `ERR RX` response. These acknowledgements mean
the request was accepted, not that the EPD refresh completed.

Fixed Chinese glyphs are generated offline from the local Microsoft YaHei font
with `tools/generate_weather_glyphs.py`; only the resulting bitmap arrays are
used by firmware. Verify generated resources with:

```powershell
python tools/generate_weather_glyphs.py --check
```

Host validation entry points are `tests/run_weather_host_tests.ps1`,
`tests/run_debug_host_tests.ps1`, and `tests/run_clock_display_host.ps1`.
Host-rendered frames do not establish panel timing, stack margin, or hardware
acceptance.

The 2026-10-27 validation record reports that the weather protocol/state and
renderer tests, all three debug host suites, and all 23 display scenarios in both BW and gray4 configurations
passed with host GCC C99. Added regressions cover legacy 31/32-byte input,
WEATHER 127/128-byte input, commands during pending FORMAT, Celsius unit pixels
at seven temperatures in both formats, fault-covered page changes, A-to-B-to-A
updates, latest-state retries, and expiry appearing on screen. Keil linking,
target image inclusion, task stack margins, and panel acceptance remain pending.
The documentation cleanup did not rerun these tests. Existing target artifacts
in the main output directory date from 2026-10-20; the separate default BW build
dates from 2026-10-21. Both precede this weather integration and cannot establish
that the current workspace was linked or deployed.
