# Changelog

## 1.5.4 (2026-10-29)
A new look.
- **Tray icon shows the layout as letters:** RU in red, EN in black (white on a dark taskbar), grey when KeySwitch is off. The letters are drawn pixel by pixel for a crisp look at any scaling. The star is gone.
- **Layout panel:** hover over the tray icon (or click it) to see all layouts and switch with a click.
- **New Settings window:** redesigned from scratch, light and dark theme, changes apply instantly (no Save button). Layout cards at the top show the current layout and switch it on click.
- **The tray icon can be hidden.** Settings then open with `Ctrl+Shift+Pause` (can be changed) or by running `KeySwitch.exe` again.
- **New tray menu and About window** in the same style. About has a link to message the author on Telegram.
- **Hotkeys swapped by default:** fix the selected text is now `Ctrl+Pause`, change case is `Shift+Pause`. If you never changed them, they are swapped for you.

## 1.3.3 (2026-10-24)
- Shorter About window: “A minimalist keyboard layout switcher. Fully open source.”

## 1.3.2 (2026-10-24)
- Clearer About window: KeySwitch works only on this computer, never connects to the internet, doesn't save or send what you type.

## 1.3.1 (2026-10-24)
- The About window now has a clickable link to the GitHub page.

## 1.3.0 (2026-10-24)
- *Interface language* in Settings: Same as Windows (default), English or Русский. Applies right after *Save*, no restart needed.

## 1.2.0 (2026-10-24)
- English and Russian interface, picked automatically from the Windows language.
- Tray star: red for English, black for any other layout (previously black only for Russian).
- The switch sound is higher for English and lower for any other layout.
- The About window links to the GitHub page.

## 1.1.4 (2026-10-24)
- After fixing selected text, the selection is removed and the cursor is placed at the end, so you can keep typing.
- Pressing the hotkey again right after a fix re-selects the text and does the next step (next layout or next case).

## 1.1.3 (2026-10-24)
- Hotkeys saved by older versions as “Ctrl+Break” now become “Ctrl+Pause”.

## 1.1.2 (2026-10-24)
- Change case is now `Ctrl+Pause` by default (was `Alt+Pause`).
- Pause and Break are treated as the same key; Ctrl+Pause is shown as “Ctrl+Pause” in Settings.

## 1.1.1 (2026-10-24)
- Exceptions list: *Browse…*, *From running…* and *Remove* buttons instead of typing names by hand.

## 1.1.0 (2026-10-24)
- Turns itself off in full-screen games and apps (checkbox in Settings).

## 1.0.1 (2026-10-24)
- The Caps Lock label in Settings is no longer cut off.

## 1.0.0 (2026-10-24)
First release.
- Fix the last word, fix the selected text, change the case of the selection.
- Caps Lock as a layout switch.
- Tray star shows the layout; sound on layout switch.
- Pause, program exceptions, start with Windows, custom hotkeys.
