# hdrfix: correct TeamSpeak 6 screen sharing on HDR monitors

If Windows HDR is on, people watching your TeamSpeak 6 screen share see a washed-out,
far too bright picture. hdrfix is a small DLL you drop into the TeamSpeak folder that
fixes this: viewers see what you see. It works for sharing a whole screen and for
sharing a single app.

| on your screen (sRGB grey level) | 255 | 235 | 192 | 128 | 64 |
|----------------------------------|-----|-----|-----|-----|----|
| what viewers get without hdrfix  | 255 | 255 | 255 | 218 | 113 |
| with hdrfix (default settings)   | 240 | 232 | 192 | 128 | 64 |
| with hdrfix, `knee=1`            | 255 | 235 | 192 | 128 | 64 |

No OBS, no virtual camera, no admin rights. To remove it, delete one file.

> **Unofficial.** hdrfix is a community project. It is not made, endorsed or supported by
> TeamSpeak Systems GmbH. Use it at your own risk (see [LICENSE](LICENSE)).

## Do I need this?

Only if **all** of these are true:

- you use the TeamSpeak 6 desktop client on Windows (64-bit),
- Windows HDR is turned on for the monitor you share (Settings > System > Display > Use HDR),
- viewers say your stream looks too bright or washed out.

With HDR off, TeamSpeak's sharing is already correct and hdrfix does nothing.

## Requirements

- Windows 11, or Windows 10 version 2004 or newer. Developed and tested on Windows 11;
  Windows 10 should work but hasn't been tested. On Windows 10, Windows draws a yellow
  border around an app while you share it (only you see it).
- TeamSpeak 6, 64-bit.
- TeamSpeak must **not** run as administrator (see [Troubleshooting](#troubleshooting)).

## Install

1. Download `hdrfix-<version>.zip` from the [Releases](../../releases) page and unzip it.
2. Quit TeamSpeak completely: right-click its tray icon > Quit. (Closing the window only
   hides it.)
3. Open the TeamSpeak folder: paste `%LOCALAPPDATA%\Programs\TeamSpeak` into the
   Explorer address bar. If TeamSpeak is installed somewhere else, right-click its
   shortcut > Open file location. The right folder contains `TeamSpeak.exe`.
4. Copy **`version.dll`** and **`hdrfix.ini`** from the zip into that folder.
5. Start TeamSpeak and share your screen as usual.

TeamSpeak loads `version.dll` from its own folder automatically. Nothing else to set up.

## Check that it works

Open `hdrfix.log` in the TeamSpeak folder. After starting TeamSpeak you should see:

```
hdrfix 1.1.3 loaded into TeamSpeak.exe (Windows 10.0 build 26100)
hook IDXGIOutput1::DuplicateOutput (vtable slot 22, ...): installed
hook IDXGIOutput5::DuplicateOutput1 (vtable slot 26, ...): installed
hook PrintWindow: installed
```

Sharing a screen adds:

```
DuplicateOutput on \\.\DISPLAY1 intercepted: 2560x1440 FP16 -> BGRA8, knee 0.75
first frame: 2560x1440 ...; SDR white 260 nits, peak 1000 nits
```

Sharing a single app adds:

```
window fix: streaming 'Chrome_WidgetWin_1' on \\.\DISPLAY1 -> capturing it in FP16 (...), SDR white 260 nits
window fix: 'Chrome_WidgetWin_1' is no longer captured, stopping
```

`passing through` means hdrfix left that capture alone, and the line says why (for
example `SDR output`: that monitor isn't in HDR mode, so there is nothing to fix).

## Uninstall

Quit TeamSpeak and delete `version.dll` (and `hdrfix.ini`, `hdrfix.log`) from the
TeamSpeak folder. To switch it off without deleting anything, set `enabled=0` in
`hdrfix.ini` and restart TeamSpeak.

**After a TeamSpeak update**, check that `version.dll` is still in the folder. If
sharing is too bright again, copy it back in.

## Settings (`hdrfix.ini`)

Read when TeamSpeak starts; restart TeamSpeak after changing it.

| key | default | meaning |
|-----|---------|---------|
| `enabled` | `1` | `0` = do nothing; TeamSpeak behaves as if hdrfix weren't there |
| `sdr_white_override_nits` | `0` | `0` = use the Windows "SDR content brightness" slider (read when sharing starts); otherwise a fixed white level in nits |
| `knee` | `0.75` | where the highlight roll-off starts, relative to SDR white; below it colours pass through exactly. `1` = no roll-off |
| `peak_override_nits` | `0` | `0` = the monitor's peak brightness as reported by Windows; used as the top of the roll-off |
| `fix_window_capture` | `1` | also fix sharing a single app; `0` = full-screen sharing only |
| `log` | `1` | write `hdrfix.log` next to the DLL (or in `%TEMP%` if that folder isn't writable); if it's over 1 MB when TeamSpeak starts, it starts over |
| `verbose_log` | `0` | `1` = one extra log line per second while TeamSpeak captures. For troubleshooting only |

**Which `knee`?** An SDR stream has nothing brighter than white, so something has to
give for HDR content brighter than your desktop's white:

- `knee=0.75` (default): HDR highlights (HDR games and videos) keep some detail and their
  colours, but white UI comes out slightly dimmer (255 becomes about 240). Everything
  below 75% of your desktop's white is untouched.
- `knee=1`: desktop, apps and text come out exactly as on your screen, but brighter HDR
  highlights all become plain white, and very bright saturated colours can shift (a bright
  orange turns yellowish). Best if you only share desktop or apps.

## Troubleshooting

**Sharing a single app is still too bright.** TeamSpeak is probably running as
administrator; Windows then refuses hdrfix's capture of other apps. The log says
`could not capture ... (CreateForWindow failed, hr=0x80070005)`. Right-click
`TeamSpeak.exe` > Properties > Compatibility, untick "Run this program as an
administrator", and restart TeamSpeak normally. (Full-screen sharing works either way.)

**The stream turns grey or freezes when another window covers the app I'm sharing.**
That's the shared app, not hdrfix: Chrome, Edge, Discord and other Chromium-based apps
stop drawing when they're completely covered, to save power, and any capture then
gets an empty window. It happens without hdrfix too. For Chrome, you can turn that off
by starting it with `--disable-features=CalculateNativeWinOcclusion` (add it to the end
of the Chrome shortcut's Target field). It then keeps drawing while hidden, which costs
a little power.

**An app on my second monitor isn't changed.** hdrfix only acts on monitors in HDR
mode. On an SDR monitor, TeamSpeak's capture is already correct.

**The first fraction of a second of an app share is still bright.** hdrfix starts once
TeamSpeak has captured the same app a few times in a row (that's how it tells a stream
from the thumbnails in the share picker), which takes a fraction of a second.

**My antivirus flags `version.dll`.** hdrfix uses techniques that some malware also uses:
a DLL with a system DLL's name that another program loads, and code hooks. The file
isn't code-signed. The full source is here, and you can build the DLL yourself (see
[Building](#building-from-source)).

**Reporting a problem.** Set `verbose_log=1`, restart TeamSpeak, reproduce the problem,
and attach `hdrfix.log` to an issue together with your Windows version and GPU. The log
contains no user names, file paths or window titles. It does contain window class names
(like `Chrome_WidgetWin_1`), display names (like `\\.\DISPLAY1`) and the Windows build.

## Is it safe?

- hdrfix only does work inside TeamSpeak's main process. TeamSpeak's other processes
  (renderer, GPU, ...) also load `version.dll`, but there it only forwards calls to the
  real Windows `version.dll`.
- It changes only the pictures TeamSpeak captures for sharing. It has no network code,
  sends nothing anywhere, and only reads `hdrfix.ini` and writes `hdrfix.log`.
- Everything that can fail falls back to TeamSpeak's normal behaviour (a too-bright
  stream), rather than breaking sharing.
- It modifies how the TeamSpeak client behaves at runtime. If TeamSpeak's terms of use
  matter to you, read them first.

## Limitations

- Only the TeamSpeak 6 desktop client on Windows x64. It relies on how TeamSpeak captures
  today (DXGI Desktop Duplication for screens, GDI `PrintWindow` for single apps). If a TeamSpeak update changes that, hdrfix stops having an effect (the log then
  shows no `intercepted` / `window fix` lines) until it's updated.
- Sharing a single app costs more than sharing a screen: hdrfix captures the app a
  second time in HDR (at most ~80 fps on Windows 11 24H2 and later) and copies each frame
  back to the CPU (about 15 MB per frame for a 1440p window). In exchange, once a stream
  is running hdrfix skips TeamSpeak's own `PrintWindow` capture (which waits for the next
  screen refresh, about 17 ms at 60 Hz), so app sharing runs faster than without hdrfix.
  The copy runs one frame ahead, so TeamSpeak doesn't wait for the GPU, and the shared
  app runs one frame (about 17 ms at 60 fps) behind the screen. Full-screen sharing adds
  one GPU pass per frame, well under a millisecond on a modern GPU.
- The SDR content brightness and the monitor's HDR state are read when a share starts,
  never during it: reading them takes a system-wide display lock that makes games
  stutter. Changing the slider, or turning HDR on or off, applies from the next share
  (an app share started in HDR comes out too dark after turning HDR off).
- HDR content brighter than your desktop's white is compressed or clipped (see `knee`).

## How it works

With HDR on, Windows composes the desktop in **scRGB**: linear light, 16-bit float, where
1.0 = 80 nits. Normal (SDR) windows are shown at the "SDR content brightness" level
(Settings > Display > HDR), e.g. 260 nits = 3.25.

TeamSpeak captures screens with DXGI Desktop Duplication. On an HDR display Windows
hands it a frame converted by clipping at 1.0 (80 nits): everything brighter than 80 nits
becomes white and everything else is ~3x too bright. Sharing a single app uses GDI
(`PrintWindow`), which is fine for normal windows, but apps that render in HDR
when Windows HDR is on (Chrome, Edge, Electron apps, games) are clipped the same way.

1. **Loading.** TeamSpeak.exe imports `version.dll`, which isn't a protected "KnownDLL",
   so Windows loads it from the TeamSpeak folder first. hdrfix's `version.dll` forwards
   all 17 real functions to `System32\version.dll`.
2. **Screen sharing.** A background thread (never `DllMain`) hooks
   `IDXGIOutput1::DuplicateOutput` and `IDXGIOutput5::DuplicateOutput1` with MinHook. For
   an HDR monitor, the hook asks Windows for the raw FP16 frames instead and returns a
   wrapper whose `AcquireNextFrame` converts each frame on TeamSpeak's own GPU device
   with [`src/hdr_to_sdr.hlsl`](src/hdr_to_sdr.hlsl):
   - scale by `80 / SDR white` (read from Windows when the share starts), so SDR white = 1.0
   - optional highlight roll-off above `knee` (extended Reinhard on max(R,G,B), so hues
     don't shift)
   - sRGB-encode into the 8-bit format TeamSpeak expects
3. **App sharing.** `PrintWindow` is hooked too. TeamSpeak calls it for the shared window
   and for its popups (menus, tooltips) once per stream frame. Once TeamSpeak has captured
   the same window 3 times within 2 s on an HDR monitor, hdrfix captures that window
   itself with Windows.Graphics.Capture in FP16 (unclipped), converts it with the same
   shader, and overwrites the pixels GDI just produced. From then on, `PrintWindow` calls
   for that window get hdrfix's frame directly, without running GDI's slow capture first
   (if hdrfix's frame isn't available, GDI runs as before). Its capture stops 3 s after
   TeamSpeak stops asking.
4. **Fallbacks.** SDR monitors, unexpected formats and failed calls all fall back to
   TeamSpeak's own result.
   - If a screen frame can't be converted mid-stream (say, the GPU driver restarts), the
     wrapper reports `DXGI_ERROR_ACCESS_LOST`. TeamSpeak then restarts the capture and
     hdrfix tries again. After 3 failures in a row it leaves new screen captures alone for
     a minute.
   - A failed app capture is retried after 5 seconds.
   - A crash inside hdrfix's own code is caught. That part of hdrfix (screen or app
     sharing) then stays off until TeamSpeak restarts.

Some details found along the way:

- In HDR mode, the legacy `DuplicateOutput` and a BGRA8-only `DuplicateOutput1` both hand
  out an already-clipped BGRA8 texture while `GetDesc` claims FP16. Only an explicit FP16
  request returns the raw scRGB data.
- `DXGI_OUTPUT_DESC1.ColorSpace` reports `G2084_NONE_P2020` (HDR10) with HDR on, not
  `G10_NONE_P709`. hdrfix treats both as HDR.
- Windows maps SDR content with the piecewise sRGB curve (grey 128 is exactly
  0.216 x SDR white), so sRGB re-encoding gives the original values back exactly.
- `GetObject` reports `biHeight` as positive even for top-down DIB sections, so hdrfix
  lets GDI write the pixels (`StretchDIBits`) instead of guessing the row order.
- WGC window frames start at `DWMWA_EXTENDED_FRAME_BOUNDS`; GDI's at `GetWindowRect`,
  which includes the invisible resize borders.

## Building from source

Needs Visual Studio 2022 (or Build Tools) with the C++ x64 tools and a Windows 10/11
SDK, CMake 3.28 or newer, and git (MinHook v1.3.4 is downloaded during configure).

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

`dist\` then has the same layout as the release zip: `version.dll`, `hdrfix.ini`, the
docs, and `test\hdrtest.exe`. Keep the source folder path short: MSBuild fails on paths
longer than 260 characters.

The shader is compiled with `fxc` at build time into a header, so nothing needs
`d3dcompiler` at runtime. The DLL links the C runtime statically and imports only
kernel32 directly; everything else is delay-loaded from System32.

GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)) builds every
push. Pushing a tag like `v1.0.0` publishes a release with the zip attached. Bump the
version in `CMakeLists.txt` (`project(hdrfix VERSION ...)`) first.

## Testing without TeamSpeak: `hdrtest`

```bat
dist\test\hdrtest.exe
```

Needs an HDR monitor with HDR turned on. For about 10 seconds it shows test windows on
that monitor and nudges the mouse cursor by 1 pixel. It captures grey patches with every
method TeamSpeak uses (three desktop duplication variants, `PrintWindow` into top-down and
bottom-up bitmaps), first without and then with `..\version.dll` loaded, and
compares the results with the values the fix should produce. It also checks that the
`version.dll` forwarding works. PNGs of every capture go to `.\hdrtest_out\`.

Exit code 0 = all checks passed, 1 = a check failed, 2 = no HDR monitor found. Use
`--dll <path>` to test another build and `--out <folder>` for the PNGs. Keep `hdrtest.exe`
in a different folder from the DLL, or the fix is already loaded in the "without" run.

`build\Release\test\convtest.exe` checks the conversion math alone on Windows' software
renderer (WARP). It needs no GPU or HDR monitor, and CI runs it on every build.

## Project layout

```
src/dllmain.cpp            entry point, which process to fix, init thread
src/proxy_version.cpp      forwarding to System32\version.dll   (+ version.def)
src/hooks.cpp              desktop duplication hooks, intercept/fallback logic
src/duplication_wrapper.*  the IDXGIOutputDuplication handed to TeamSpeak
src/converter.*            D3D11 rendering in a separate context state
src/hdr_to_sdr.hlsl        the conversion shader
src/window_fix.*           single-app sharing: GDI hooks, when to step in, pixel replacement
src/window_capture.*       single-app sharing: FP16 Windows.Graphics.Capture + readback
src/display_info.*         HDR detection, monitor -> DXGI output, SDR white level
src/vtable_index.c         vtable slots from the SDK's C interface layout
src/log_config.*           hdrfix.ini + hdrfix.log
src/verbose_log.*          optional per-second capture statistics
src/delay_load.cpp         delay-loaded DLLs come from System32 only
src/version.rc, version.h.in   file version info
test/hdrtest.cpp           verification program
test/convtest.cpp          conversion math on WARP (runs in CI)
test/reference_math.h      expected output, shared by both tests
```

## License

[MIT](LICENSE). The DLL includes MinHook (BSD 2-Clause), see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
