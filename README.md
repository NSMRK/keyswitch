<p align="right"><b>English</b> · <a href="README.ru.md">Русский</a></p>

# KeySwitch

A minimalist keyboard layout switcher for Windows 10/11.
Typed `ghbdtn` instead of `привет`? Press **Pause**.

**Tiny and light:** one 280 KB file, no installation, no dependencies, about 2 MB of RAM and 0% CPU while idle. Fully open source.

<p align="center">
  <img src="docs/tray-en.png" alt="Tray star: red for English, black for any other layout, grey when off" width="480">
</p>

**[⬇ Download the latest version](../../releases/latest)**

## Why KeySwitch?

Punto Switcher, Caramba and similar tools try to guess the layout for you using large dictionaries. Sometimes that's handy, but they also misfire on abbreviations, logins, code and slang — and then you end up fixing the fix.

KeySwitch takes a different approach: **you always know better** when you've typed in the wrong layout. One key fixes it — nothing changes behind your back, and the program stays tiny because there are no dictionaries to carry around.

## Features

| Action | Default key |
|---|---|
| Fix the last word | `Pause` |
| Fix the selected text | `Shift+Pause` |
| Change the case of the selected text | `Ctrl+Pause` |
| Switch the layout | `Caps Lock` (regular Caps Lock: `Shift+Caps Lock`) |

- **Tiny.** A single 280 KB exe written in plain C++ and the Windows API — no frameworks, no runtimes, no background services. It sits in the tray using about 2 MB of memory and wakes up only when you press a key.
- **No guessing.** KeySwitch never changes anything on its own — only when you press the key. No false corrections.
- **Works with any layouts** installed in Windows: Russian, Ukrainian, German, Greek and so on.
- **Punctuation stays as typed.** Letters are converted, symbols are kept: `дела?` → `ltkf?`.
- **Press again to cycle.** The word moves on to the next layout; the case goes lower → Sentence case → UPPER.
- **Keep typing right away.** After a fix the cursor is at the end of the text and nothing stays selected.
- **Tray star** shows the current layout: red for English, black for any other, grey when KeySwitch is off.
- **A soft sound** on every layout switch — higher for English, lower for others. Can be turned off.
- **Stays out of games.** In full-screen games KeySwitch turns itself off, so Caps Lock and Pause work as usual.
- **Exceptions.** A list of programs where KeySwitch is off: add them with *Browse…*, from running programs, or right from the tray menu.
- **Any hotkeys** you like — set them in Settings.
- **English or Russian interface** — picked from your Windows language, or chosen in Settings.

<p align="center">
  <img src="docs/settings-en.png" alt="KeySwitch settings window" width="440">
</p>

## Privacy

KeySwitch has to watch the keyboard to know which word to fix. That's why:

- **No network.** There is not a single line of networking code: it sends nothing and never checks for updates.
- **No logging.** Only the keys of the current word in the current window are kept in memory; they are cleared on Enter, a mouse click or a window change. The only file written is the settings file (`%APPDATA%\KeySwitch\config.ini`).
- **Open source.** All the code is one file, [`src/keyswitch.cpp`](src/keyswitch.cpp) — read it and check.
- **Built by GitHub.** `KeySwitch.exe` in [Releases](../../releases) is built automatically from this code by GitHub Actions, with the exact commit shown.

## Requirements

Windows 10 or 11, 64-bit. Also runs on Windows 11 on ARM (e.g. Parallels on Apple Silicon Macs) through built-in x64 emulation.

## Installation

1. Download `KeySwitch.exe` from [Releases](../../releases/latest).
2. Put it in any folder and run it. A star appears in the system tray.
3. Double-click the star to open Settings. *Start with Windows* is there too.

The exe is not digitally signed, so Windows SmartScreen may warn you on the first run: *More info* → *Run anyway*.

## Tray menu

- **Turn off in “…exe”** — add the current program to the exceptions (or **Turn on in “…exe”** to remove it).
- **Pause KeySwitch** — turn everything off for a while.
- **Settings…**, **About**, **Exit**.

## Building from source

**MinGW-w64** (this is how GitHub Actions builds it):

```
cd src
windres keyswitch.rc -O coff -o keyswitch_res.o
g++ -std=c++17 -O2 -s -municode -mwindows -static keyswitch.cpp keyswitch_res.o -o KeySwitch.exe -luser32 -lshell32 -ladvapi32 -lgdi32 -lwinmm -lcomdlg32
```

**Visual Studio** (Developer Command Prompt):

```
cd src
rc keyswitch.rc
cl /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE keyswitch.cpp keyswitch.res /Fe:KeySwitch.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO
```

## Limitations

- It doesn't work in windows running as administrator unless KeySwitch itself runs as administrator (a Windows security rule).
- In some windowed games and old programs simulated typing may not work — add them to the exceptions.
- Mac keyboards (Boot Camp, Parallels) have no Pause key — assign other hotkeys in Settings.

## Feedback

Found a bug, missing something, or want something removed? Tell me — every message is read.

- **[Report a bug](../../issues/new?template=bug_report.yml)**
- **[Suggest an idea](../../issues/new?template=feature_request.yml)**

You can write in English or Russian. If you like KeySwitch, a ⭐ on GitHub helps others find it.

## License

[MIT](LICENSE) © 2026 NSMRK
