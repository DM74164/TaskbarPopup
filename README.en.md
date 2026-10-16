[简体中文](README.md) | English

A note from the author: the gap a maximized window leaves above the taskbar looks awful, Windows' own taskbar auto-hide is terrible, and other projects didn't quite do what I wanted, so I had AI build one. It was written entirely by Opus 5.5. I enjoy using it, so I put it up here.
It should also work on Windows 10, since every system function it uses exists there, but I haven't tried.

# TaskbarPopup

A Windows 11 taskbar enhancement: when a window is maximized, the taskbar hides and the window fills the whole screen. Hold the Win key and a liquid-glass mini taskbar pops up from the bottom of the screen. Single exe, no runtime to install.

> The interface comes in English and Chinese. It follows your Windows display language; change it under Settings → Appearance → Language.

Demo video: [Bilibili](https://www.bilibili.com/video/BV1BLp963E7e/)

## Features

### Maximize means full screen

Maximize a window and the taskbar slides away while the window fills the screen. Restore, minimize or close it and the taskbar slides back, with your desktop icons right where they were.

![The taskbar slides away and the maximized window fills the screen](docs/demo/en/fullscreen.webp)

### Hold Win for the mini taskbar

Hold Win for 1 second (adjustable) and the mini taskbar rises from the bottom (or stands vertically on the left or right screen edge): pinned apps, open windows, hidden tray icons, the clock, volume and brightness sliders, plus Wi-Fi and Bluetooth toggles. Hover a window icon for a live thumbnail. A short press of Win still opens the Start menu.

![Hold Win and the mini taskbar rises from the bottom](docs/demo/en/popup.webp)

### Type to filter

Start typing once it's open to filter windows and pinned apps. Programs from the Start menu show up too. Press Enter to open.

![Type to filter, including Start menu programs](docs/demo/en/filter.webp)

### Right-click menu

Works like the system taskbar: recent files, open another window, run as administrator, open file location, close windows. Tick "Don't hide taskbar when maximized" to exclude an app; if you turn off "Hide taskbar when maximized or fullscreen" in Settings, the item becomes "Hide taskbar when maximized" and only the ticked apps hide the taskbar. Middle-click an icon to close the window.

![Right-click menu with recent files, run as admin, open file location and close all](docs/demo/en/menu.webp)

### Keep small windows on top

Double-click a small floating window (or use a hotkey) and it stays on top even when you click the full-screen window behind it. Turn on "Keep pinned windows above all windows" (off by default) in Settings to keep it above every window.

![Double-click a small window and it is no longer covered by the full-screen window](docs/demo/en/pin.webp)

### Hotkeys

Set one hotkey to open the mini taskbar and one to pin a small window. Key combos, single keys and mouse buttons all work, triggered by a tap or a hold. None are set by default.

![Hotkeys can be key combos, single keys or mouse buttons](docs/demo/en/hotkeys.webp)

### Appearance

Choose liquid glass or frosted glass, a system, light or dark theme, and English or Chinese. The mini taskbar's size (50 – 200%), liquid glass transparency (separately for light and dark), position (bottom, left, right) and alignment at the bottom (system, center, left, right) preview live as you change them.

![Liquid or frosted glass, light or dark, and size all preview live](docs/demo/en/appearance.webp)

## Download

Get the latest `TaskbarPopup.zip` from [Releases](../../releases), unzip it and run `TaskbarPopup.exe`. A blue up-arrow icon appears in the tray: left-click it to open the mini taskbar, right-click it for the menu and Settings.

## Mini taskbar controls

| Action | Result |
| --- | --- |
| Hold Win / hotkey | Open / close |
| Click an icon | Switch to that window (click the active one to minimize); start a pinned app that isn't running |
| Shift + click | Open another window |
| Right-click / Menu key | Right-click menu |
| Middle-click / Delete | Close the window |
| Arrow keys / Tab, Enter | Select and open with the keyboard |
| Type | Filter; Esc clears, press again to close |
| Mouse wheel | Scroll the icons; over a slider, adjust volume / brightness |
| Windows icon on the left | Click for Start, right-click for the Win+X menu |
| Tray icons | Left-click works like the system tray; right-click opens a menu, and "App menu" opens the program's own menu |
| Wi-Fi / Bluetooth buttons | Click to turn on or off; right-click to open the matching Settings page |
| Esc / click elsewhere | Close |

## Settings

Right-click the tray icon → Settings. Options are grouped into Taskbar, Mini taskbar, Appearance, Hotkeys, and Startup & updates; switch with the navigation on the left (or Ctrl+Tab). Tab, arrow keys and Space work too. Settings are stored in `%APPDATA%\TaskbarPopup\settings.ini`.

"Auto-check for updates" is off by default. When on, it checks GitHub Releases once a day and only notifies you; it never downloads or installs anything by itself.

**Having a problem?** In the tray menu, turn on "Record diagnostic log", reproduce the problem, then click "Open diagnostic log folder" and send the `debug.log`.

## Known limitations

- **Holding Win does nothing in admin programs and games**: Windows doesn't pass their key presses to a normal-privilege program. Tick "Run as administrator" in the tray menu. With start-at-login on as well, there's no UAC prompt at startup.
- **Edge / Chrome may show a thin line (about 2 px) at the bottom**: the browser leaves it for the auto-hidden taskbar and it can't be changed from outside.
- **Multiple monitors**: system auto-hide applies to every monitor, so while a window is maximized the taskbars on other monitors auto-hide too.
- **Notifications may not show while maximized**: go to Settings → System → Notifications → Turn on do not disturb automatically, and turn off "When using an app in full-screen mode".
- **Some windows won't stretch**: the taskbar stays visible while that window is in front.
- **Taskbar settings locked** (for example by group policy on a work PC): the taskbar stays visible.
- **Old games in exclusive full screen** cover the mini taskbar. Switch the game to borderless windowed.
- **Brightness on external monitors** needs DDC/CI supported and turned on in the monitor; otherwise the slider is grayed out.
- **Wi-Fi / Bluetooth button grayed out**: the adapter is turned off by a hardware switch or disabled in Device Manager. A radio the PC doesn't have isn't shown.
- **Tray icons** are clicked for you in the system tray through UI Automation, which may miss for a few programs. If it does, try once with the diagnostic log on.
- **The mini taskbar's glass background** is a snapshot from when it opened, so a video playing behind it won't move.
- **Antivirus false positives**: the exe isn't signed and uses keyboard and mouse hooks (only to detect holding Win and your hotkeys; no keys are recorded or sent anywhere). Release builds are compiled from source by GitHub Actions. If it's blocked, allow it in Windows Security → Protection history.
- **Taskbar missing after force-killing the app**: start the app again and it restores everything.

## Building from source

Requires C++17. From the repository root:

- **Visual Studio 2022**: `cmake -B build -G "Visual Studio 17 2022" -A x64`, then `cmake --build build --config Release`
- **MSYS2 / MinGW-w64**: `cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build`
- **Cross-compile on Linux** (needs `g++-mingw-w64-x86-64-posix`): `sh build-mingw.sh`

## Code layout

| File | Purpose |
| --- | --- |
| `src/main.cpp` | Entry point, tray menu, settings, start at login |
| `src/fullscreen.cpp` | Detect maximized windows, hide the taskbar, stretch the window |
| `src/taskbar_anim.cpp` | Taskbar slide out / in animation |
| `src/desktop_icons.cpp` | Save and restore desktop icon positions |
| `src/hook.cpp` | Keyboard and mouse hooks: holding Win and hotkeys |
| `src/popup.cpp` | The mini taskbar |
| `src/glass.cpp` | Liquid glass material |
| `src/float_windows.cpp` | Keep small windows on top |
| `src/hotkeys.cpp` | Hotkeys |
| `src/thumbnail.cpp` | Window thumbnails |
| `src/apps.cpp` | App list for type-to-filter |
| `src/pinned.cpp` | Taskbar pinned list |
| `src/tray.cpp` | Hidden tray icons |
| `src/app_menu.cpp` | Right-click menu |
| `src/windows.cpp` | List windows, get icons |
| `src/levels.cpp` | Volume and brightness |
| `src/radios.cpp` | Wi-Fi and Bluetooth toggles |
| `src/elevation.cpp` | Run as administrator |
| `src/update.cpp` | Update check |
| `src/settings_dialog.cpp` | Settings window |
| `src/util.cpp` | Utilities, diagnostic log |
| `src/version.h` | Version number |

## License

[GNU AGPL-3.0](LICENSE)
