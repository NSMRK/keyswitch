// KeySwitch — минималистичный переключатель раскладки для Windows.
// by NSMRK
//
//   Pause        — исправить последнее набранное слово (повторное нажатие — следующая раскладка)
//   Shift+Pause  — исправить выделенный текст
//   Ctrl+Pause   — сменить регистр выделенного текста (строчные → Как в предложении → ЗАГЛАВНЫЕ)
//   Caps Lock    — переключить раскладку (Shift+Caps Lock — обычный Caps Lock), отключается в настройках
//
// Иконка в трее: красная звезда — английская раскладка, чёрная — любая другая, серая — программа отключена
// (пауза, программа-исключение или полноэкранная игра).
// Интерфейс на русском или английском: автоматически по языку Windows или по выбору в настройках.
// Настройки: двойной клик по иконке в трее или правый клик -> «Настройки…».
// Конфиг: %APPDATA%\KeySwitch\config.ini
//
// Сборка (Visual Studio, «Developer Command Prompt»):
//   rc keyswitch.rc
//   cl /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE keyswitch.cpp keyswitch.res /Fe:KeySwitch.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO
// Сборка (MinGW-w64):
//   windres keyswitch.rc -O coff -o keyswitch_res.o
//   g++ -std=c++17 -O2 -s -municode -mwindows -static keyswitch.cpp keyswitch_res.o -o KeySwitch.exe ^
//       -luser32 -lshell32 -ladvapi32 -lgdi32 -lwinmm -lcomdlg32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

// ---------------------------------------------------------------------------
// Константы и общее состояние
// ---------------------------------------------------------------------------

static const wchar_t* const APP_NAME = L"KeySwitch";
static const wchar_t* const RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum : UINT {
    WMU_TRAY = WM_APP + 1,   // сообщения от иконки в трее
    WMU_WORD,                // хук: «исправить слово»
    WMU_SEL,                 // хук: «исправить выделение»
    WMU_CASE,                // хук: «сменить регистр выделения»
    WMU_CAPS,                // хук: Caps Lock — переключить раскладку
    WMU_CAPTURED,            // хук -> окно настроек: записано сочетание (wParam=vk, lParam=mods; vk=0 — отмена)
};

enum {
    ID_MENU_SETTINGS = 1001,
    ID_MENU_ABOUT,
    ID_MENU_PAUSE,
    ID_MENU_EXCLUDE,
    ID_MENU_EXIT,
    IDC_WORD_EDIT = 2001,
    IDC_WORD_BTN,
    IDC_SEL_EDIT,
    IDC_SEL_BTN,
    IDC_CASE_EDIT,
    IDC_CASE_BTN,
    IDC_CAPS,
    IDC_SOUND,
    IDC_AUTOSTART,
    IDC_FULLSCREEN,
    IDC_LANG,
    IDC_EXCLUDE,           // список программ-исключений
    IDC_EXCL_BROWSE,
    IDC_EXCL_RUNNING,
    IDC_EXCL_REMOVE,
    ID_RUNNING_FIRST = 3000,   // пункты меню «Из запущенных»
};

struct Hotkey {
    UINT vk;
    UINT mods;   // MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN
};

struct KeyRec {
    UINT vk;
    bool shift;
    bool caps;
};

// Всё, что делят поток хука и главный поток, — под этой секцией.
static CRITICAL_SECTION g_cs;
static std::vector<KeyRec> g_buf;            // нажатия в текущем окне с момента последнего «сброса»
static HWND   g_bufWnd     = nullptr;        // окно, в котором набран буфер
static bool   g_repeat     = false;          // последнее действие — конвертация слова, после неё ничего не печатали
static HKL    g_lastTarget = nullptr;        // раскладка, в которую конвертировали в прошлый раз
static HKL    g_origLayout = nullptr;        // раскладка, в которой слово набрали изначально
static size_t g_lastOutLen = 0;              // сколько символов тогда напечатали
static bool   g_selRepeat  = false;          // только что вставили текст вместо выделения, после этого ничего не делали
static HWND   g_selWnd     = nullptr;        // окно, куда вставили
static size_t g_selLen     = 0;              // длина вставленного текста (в позициях курсора)
static Hotkey g_hkWord = { VK_PAUSE, 0 };
static Hotkey g_hkSel  = { VK_PAUSE, MOD_SHIFT };
static Hotkey g_hkCase = { VK_PAUSE, MOD_CONTROL };
static std::vector<std::wstring> g_excluded; // имена exe в нижнем регистре

static volatile LONG g_capture     = 0;      // 0 — обычный режим, иначе ждём сочетание для окна настроек
static volatile LONG g_paused      = 0;      // «Приостановить» в меню трея
static volatile LONG g_capsSwitch  = 1;      // Caps Lock переключает раскладку
static volatile LONG g_sound       = 1;      // звук при переключении
static volatile LONG g_autoFull    = 1;      // отключаться в полноэкранных играх
static volatile LONG g_fullscreen  = 0;      // активное окно сейчас полноэкранное (обновляет таймер)
static volatile LONG g_exclVersion = 0;      // растёт при каждом изменении списка исключений

static HWND  g_mainWnd        = nullptr;
static HWND  g_settingsWnd    = nullptr;
static DWORD g_hookThreadId   = 0;
static UINT  g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid  = {};
static int   g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------------------
// Строки интерфейса: русский или английский, по языку Windows
// ---------------------------------------------------------------------------

enum StrId {
    S_ALREADY_RUNNING, S_TIP_PAUSED, S_TIP_WORD, S_TIP_SEL, S_TIP_CASE,
    S_SETTINGS_TITLE, S_LBL_WORD, S_LBL_SEL, S_LBL_CASE, S_BTN_CHANGE,
    S_CHK_CAPS, S_CAPS_HINT, S_CHK_SOUND, S_CHK_AUTOSTART, S_CHK_FULLSCREEN,
    S_LBL_EXCL, S_BTN_BROWSE, S_BTN_RUNNING, S_BTN_REMOVE, S_BTN_SAVE, S_BTN_CANCEL,
    S_PRESS_COMBO, S_ERR_CAPS, S_ERR_TEXTKEY, S_ERR_TAKEN,
    S_FILE_FILTER, S_FILE_TITLE, S_NO_PROGRAMS,
    S_MENU_EXCL_OFF, S_MENU_EXCL_ON, S_QUOTE_OPEN, S_QUOTE_CLOSE,
    S_MENU_PAUSE, S_MENU_SETTINGS, S_MENU_ABOUT, S_MENU_EXIT,
    S_ABOUT_TITLE, S_ABOUT_BODY,
    S_LBL_LANG, S_LANG_AUTO,
    S_COUNT
};

struct LocStr { const wchar_t* ru; const wchar_t* en; };

static const LocStr kStrings[S_COUNT] = {
    /* S_ALREADY_RUNNING */ { L"KeySwitch уже запущен — ищите звезду в трее.",
                              L"KeySwitch is already running — look for the star in the system tray." },
    /* S_TIP_PAUSED      */ { L"KeySwitch — на паузе", L"KeySwitch — paused" },
    /* S_TIP_WORD        */ { L"Слово: ", L"Word: " },
    /* S_TIP_SEL         */ { L"Выделение: ", L"Selection: " },
    /* S_TIP_CASE        */ { L"Регистр: ", L"Case: " },
    /* S_SETTINGS_TITLE  */ { L"KeySwitch — настройки", L"KeySwitch — Settings" },
    /* S_LBL_WORD        */ { L"Исправить последнее слово:", L"Fix the last word:" },
    /* S_LBL_SEL         */ { L"Исправить выделенный текст:", L"Fix the selected text:" },
    /* S_LBL_CASE        */ { L"Сменить регистр выделенного текста:", L"Change the case of the selected text:" },
    /* S_BTN_CHANGE      */ { L"Изменить", L"Change" },
    /* S_CHK_CAPS        */ { L"Caps Lock переключает раскладку", L"Caps Lock switches the layout" },
    /* S_CAPS_HINT       */ { L"Обычный Caps Lock — Shift+Caps Lock", L"Regular Caps Lock: Shift+Caps Lock" },
    /* S_CHK_SOUND       */ { L"Звук при переключении раскладки", L"Play a sound when the layout changes" },
    /* S_CHK_AUTOSTART   */ { L"Запускать вместе с Windows", L"Start with Windows" },
    /* S_CHK_FULLSCREEN  */ { L"Отключаться в полноэкранных играх", L"Turn off in full-screen games" },
    /* S_LBL_EXCL        */ { L"Отключать в программах:", L"Turn off in these programs:" },
    /* S_BTN_BROWSE      */ { L"Обзор…", L"Browse…" },
    /* S_BTN_RUNNING     */ { L"Из запущенных…", L"From running…" },
    /* S_BTN_REMOVE      */ { L"Удалить", L"Remove" },
    /* S_BTN_SAVE        */ { L"Сохранить", L"Save" },
    /* S_BTN_CANCEL      */ { L"Отмена", L"Cancel" },
    /* S_PRESS_COMBO     */ { L"Нажмите сочетание… (Esc — отмена)", L"Press a key combination… (Esc to cancel)" },
    /* S_ERR_CAPS        */ { L"Caps Lock настраивается отдельной галочкой ниже.",
                              L"Caps Lock is set up with the checkbox below." },
    /* S_ERR_TEXTKEY     */ { L"Эта клавиша печатает текст. Добавьте Ctrl, Alt или Win.",
                              L"This key types text. Add Ctrl, Alt or Win." },
    /* S_ERR_TAKEN       */ { L"Это сочетание уже назначено на другое действие.",
                              L"This combination is already used for another action." },
    /* S_FILE_FILTER     */ { L"Программы (*.exe)\0*.exe\0", L"Programs (*.exe)\0*.exe\0" },
    /* S_FILE_TITLE      */ { L"Выберите программу, в которой KeySwitch будет отключаться",
                              L"Choose a program where KeySwitch should turn off" },
    /* S_NO_PROGRAMS     */ { L"Нет открытых программ", L"No open programs" },
    /* S_MENU_EXCL_OFF   */ { L"Отключать в ", L"Turn off in " },
    /* S_MENU_EXCL_ON    */ { L"Включить в ", L"Turn on in " },
    /* S_QUOTE_OPEN      */ { L"«", L"\u201C" },
    /* S_QUOTE_CLOSE     */ { L"»", L"\u201D" },
    /* S_MENU_PAUSE      */ { L"Приостановить", L"Pause KeySwitch" },
    /* S_MENU_SETTINGS   */ { L"Настройки…", L"Settings…" },
    /* S_MENU_ABOUT      */ { L"О программе", L"About" },
    /* S_MENU_EXIT       */ { L"Выход", L"Exit" },
    /* S_ABOUT_TITLE     */ { L"О программе", L"About KeySwitch" },
    /* S_ABOUT_BODY      */ { L"Минималистичный переключатель раскладки с полностью открытым кодом.",
                              L"A minimalist keyboard layout switcher. Fully open source." },
    /* S_LBL_LANG        */ { L"Язык интерфейса:", L"Interface language:" },
    /* S_LANG_AUTO       */ { L"Как в Windows", L"Same as Windows" },
};

enum { LANG_MODE_AUTO = 0, LANG_MODE_EN = 1, LANG_MODE_RU = 2 };

static int  g_langMode = LANG_MODE_AUTO;   // выбор в настройках
static bool g_langRu   = false;            // итоговый язык интерфейса

static const wchar_t* T(StrId id) {
    return g_langRu ? kStrings[id].ru : kStrings[id].en;
}

// Язык интерфейса: выбранный в настройках или, в режиме «Как в Windows», язык Windows.
static void ApplyUiLanguage() {
    if (g_langMode == LANG_MODE_RU)      g_langRu = true;
    else if (g_langMode == LANG_MODE_EN) g_langRu = false;
    else g_langRu = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
}

// ---------------------------------------------------------------------------
// Классификация клавиш
// ---------------------------------------------------------------------------

static bool IsModifierKey(UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return true;
    }
    return false;
}

// Клавиши, которые печатают символ и зависят (или могут зависеть) от раскладки.
static bool IsCharKey(UINT vk) {
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE ||
           (vk >= VK_OEM_1 && vk <= VK_OEM_3) ||      // ; = , - . / `
           (vk >= VK_OEM_4 && vk <= VK_OEM_8) ||      // [ \ ] '
           vk == VK_OEM_102 ||
           (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE && vk != VK_SEPARATOR);
}

static UINT CurrentMods() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    return m;
}

static void ResetBufferLocked() {
    g_buf.clear();
    g_repeat = false;
    g_selRepeat = false;
}

static void ResetBuffer() {
    EnterCriticalSection(&g_cs);
    ResetBufferLocked();
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Программы-исключения
// ---------------------------------------------------------------------------

static std::wstring ToLowerW(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

static std::wstring ToUpperW(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

// Имя exe процесса, которому принадлежит окно (в нижнем регистре), например «chrome.exe».
static std::wstring ProcessName(HWND wnd) {
    DWORD pid = 0;
    if (!wnd || !GetWindowThreadProcessId(wnd, &pid) || !pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        name.assign(path, len);
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name.erase(0, slash + 1);
    }
    CloseHandle(h);
    return ToLowerW(name);
}

static bool IsExcludedName(const std::wstring& exe) {
    if (exe.empty()) return false;
    EnterCriticalSection(&g_cs);
    const bool found = std::find(g_excluded.begin(), g_excluded.end(), exe) != g_excluded.end();
    LeaveCriticalSection(&g_cs);
    return found;
}

// Кэш «исключено ли окно»: имя процесса узнаём только при смене окна или списка.
struct ExclusionCache {
    HWND wnd = nullptr;
    LONG ver = -1;
    bool excluded = false;
    bool Check(HWND w) {
        const LONG v = g_exclVersion;
        if (w != wnd || v != ver) {
            wnd = w;
            ver = v;
            excluded = IsExcludedName(ProcessName(w));
        }
        return excluded;
    }
};

// ---------------------------------------------------------------------------
// Низкоуровневые хуки (работают в отдельном потоке, чтобы главный поток
// мог спокойно ждать буфер обмена, не задерживая ввод)
// ---------------------------------------------------------------------------

static UINT g_swallowUp = 0;          // keyup какой клавиши проглотить (только поток хука)
static ExclusionCache g_hookExcl;     // только поток хука

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION)
        return CallNextHookEx(nullptr, code, wp, lp);

    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    // Ctrl+Pause Windows присылает как Break (VK_CANCEL) — для нас это одна клавиша.
    const UINT vk = (k->vkCode == VK_CANCEL) ? VK_PAUSE : k->vkCode;
    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

    // Собственный ввод программы (и других программ) не трогаем.
    if (k->flags & LLKHF_INJECTED)
        return CallNextHookEx(nullptr, code, wp, lp);

    if (!down) {
        if (vk == g_swallowUp) { g_swallowUp = 0; return 1; }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (g_swallowUp && vk == g_swallowUp)
        return 1;   // автоповтор удерживаемой горячей клавиши

    // Режим записи сочетания для окна настроек.
    if (g_capture) {
        if (IsModifierKey(vk))
            return CallNextHookEx(nullptr, code, wp, lp);
        InterlockedExchange(&g_capture, 0);
        if (vk == VK_ESCAPE) PostMessageW(g_settingsWnd, WMU_CAPTURED, 0, 0);
        else                 PostMessageW(g_settingsWnd, WMU_CAPTURED, vk, CurrentMods());
        g_swallowUp = vk;
        return 1;
    }

    // На паузе или в программе-исключении — полностью прозрачны.
    HWND fg = GetForegroundWindow();
    if (g_paused || (g_autoFull && g_fullscreen) || g_hookExcl.Check(fg)) {
        ResetBuffer();
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    const UINT mods = CurrentMods();

    // Caps Lock переключает раскладку; Shift+Caps Lock (и другие сочетания) — как обычно.
    if (vk == VK_CAPITAL && g_capsSwitch && mods == 0) {
        g_swallowUp = vk;
        PostMessageW(g_mainWnd, WMU_CAPS, 0, 0);
        return 1;
    }

    Hotkey hw, hs, hc;
    EnterCriticalSection(&g_cs);
    hw = g_hkWord;
    hs = g_hkSel;
    hc = g_hkCase;
    LeaveCriticalSection(&g_cs);

    if (vk == hw.vk && mods == hw.mods) { g_swallowUp = vk; PostMessageW(g_mainWnd, WMU_WORD, 0, 0); return 1; }
    if (vk == hs.vk && mods == hs.mods) { g_swallowUp = vk; PostMessageW(g_mainWnd, WMU_SEL, 0, 0);  return 1; }
    if (vk == hc.vk && mods == hc.mods) { g_swallowUp = vk; PostMessageW(g_mainWnd, WMU_CASE, 0, 0); return 1; }

    // Ведём буфер набранных клавиш.
    EnterCriticalSection(&g_cs);
    if (!IsModifierKey(vk)) g_selRepeat = false;   // начали печатать — повтор по вставленному тексту больше не нужен
    if (fg != g_bufWnd) {
        ResetBufferLocked();
        g_bufWnd = fg;
    }
    if (IsModifierKey(vk)) {
        // ничего
    } else if (vk == VK_BACK) {
        if (mods & (MOD_CONTROL | MOD_ALT)) ResetBufferLocked();   // Ctrl+Backspace удаляет слово целиком
        else if (!g_buf.empty()) g_buf.pop_back();
        g_repeat = false;
    } else if (IsCharKey(vk) && !(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
        if (g_buf.size() >= 256) g_buf.erase(g_buf.begin());
        g_buf.push_back({ vk, (mods & MOD_SHIFT) != 0, (GetKeyState(VK_CAPITAL) & 1) != 0 });
        g_repeat = false;
    } else {
        // Enter, Tab, стрелки, Esc, F-клавиши, сочетания с Ctrl/Alt/Win — начинаем заново.
        ResetBufferLocked();
    }
    LeaveCriticalSection(&g_cs);

    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        // Клик мышью мог переставить курсор — буфер больше не соответствует тексту.
        ResetBuffer();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HHOOK kh = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hi, 0);
    HHOOK mh = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hi, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
    if (kh) UnhookWindowsHookEx(kh);
    if (mh) UnhookWindowsHookEx(mh);
    return 0;
}

// ---------------------------------------------------------------------------
// Эмуляция ввода
// ---------------------------------------------------------------------------

static bool IsExtendedVk(WORD vk) {
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_DIVIDE: case VK_NUMLOCK: case VK_APPS:
        return true;
    }
    return false;
}

static void AddKey(std::vector<INPUT>& v, WORD vk, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedVk(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    v.push_back(in);
}

static void AddTap(std::vector<INPUT>& v, WORD vk) {
    AddKey(v, vk, false);
    AddKey(v, vk, true);
}

static void AddChar(std::vector<INPUT>& v, wchar_t ch) {
    if (ch == L' ') { AddTap(v, VK_SPACE); return; }
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE;
    v.push_back(in);
    in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    v.push_back(in);
}

static void SendInputs(std::vector<INPUT>& v) {
    if (!v.empty())
        SendInput(static_cast<UINT>(v.size()), v.data(), sizeof(INPUT));
}

// Пользователь может всё ещё держать модификаторы горячей клавиши (например, Shift).
// Отпускаем их, иначе Ctrl+C превратится в Ctrl+Shift+C, а символы — в заглавные.
static void ReleaseModifiers() {
    static const WORD mods[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                                 VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    std::vector<INPUT> v;
    bool altOrWin = false;
    for (WORD m : mods) {
        if (GetAsyncKeyState(m) & 0x8000) {
            if (m == VK_LMENU || m == VK_RMENU || m == VK_LWIN || m == VK_RWIN) altOrWin = true;
        }
    }
    // «Пустое» нажатие, чтобы отпускание Alt/Win не открыло меню или «Пуск».
    if (altOrWin) AddTap(v, 0xE8);
    for (WORD m : mods)
        if (GetAsyncKeyState(m) & 0x8000) AddKey(v, m, true);
    SendInputs(v);
}

// Ctrl+клавиша в темпе живого набора: если отправить всё одной пачкой, некоторые
// программы (новый Блокнот) не успевают увидеть зажатый Ctrl и команда не срабатывает.
static void SendCtrlCombo(WORD key) {
    std::vector<INPUT> v;
    AddKey(v, VK_CONTROL, false); SendInputs(v); v.clear(); Sleep(15);
    AddTap(v, key);               SendInputs(v); v.clear(); Sleep(15);
    AddKey(v, VK_CONTROL, true);  SendInputs(v);
}

// Выделить count символов слева от курсора (Shift+←), чтобы после вставки текст
// остался выделенным и горячую клавишу можно было нажать ещё раз.
static void SelectBack(size_t count) {
    if (count == 0 || count > 2000) return;
    std::vector<INPUT> e;
    AddKey(e, VK_LSHIFT, false); SendInputs(e); Sleep(15);
    for (size_t done = 0; done < count;) {
        e.clear();
        const size_t chunk = std::min<size_t>(20, count - done);
        for (size_t j = 0; j < chunk; ++j) AddTap(e, VK_LEFT);
        SendInputs(e);
        done += chunk;
        Sleep(8);
    }
    Sleep(40);
    e.clear();
    AddKey(e, VK_LSHIFT, true); SendInputs(e);
}

// Сколько позиций курсора занимает текст (перенос строки \r\n — одна позиция).
static size_t CaretLength(const std::wstring& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n')) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Раскладки
// ---------------------------------------------------------------------------

static std::vector<HKL> Layouts() {
    int n = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> v(n > 0 ? n : 0);
    if (n > 0) GetKeyboardLayoutList(n, v.data());
    return v;
}

static HKL WindowLayout(HWND wnd) {
    return GetKeyboardLayout(GetWindowThreadProcessId(wnd, nullptr));
}

// Английская ли раскладка (красная звезда). Любая другая — чёрная.
static bool IsEnglish(HKL hkl) {
    return PRIMARYLANGID(LOWORD(reinterpret_cast<UINT_PTR>(hkl))) == LANG_ENGLISH;
}

// Следующая раскладка в списке Windows (по кругу). nullptr — если раскладка одна.
static HKL NextLayout(HKL cur) {
    std::vector<HKL> v = Layouts();
    if (v.size() < 2) return nullptr;
    size_t i = 0;
    while (i < v.size() && v[i] != cur) ++i;
    if (i == v.size()) {
        // Точного совпадения нет — ищем по языку.
        i = 0;
        while (i < v.size() && LOWORD(reinterpret_cast<UINT_PTR>(v[i])) != LOWORD(reinterpret_cast<UINT_PTR>(cur))) ++i;
        if (i == v.size()) return v[0];
    }
    return v[(i + 1) % v.size()];
}

// Просим окно сменить раскладку. Сообщение отправляем элементу с фокусом ввода, если он известен.
static void SwitchLayout(HWND wnd, HKL hkl) {
    HWND target = wnd;
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(wnd, nullptr), &gti) && gti.hwndFocus)
        target = gti.hwndFocus;
    PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hkl));
}

// Ждём, пока окно действительно переключится на нужную раскладку.
static bool WaitForLayout(HWND wnd, HKL hkl, int timeoutMs) {
    const DWORD tid = GetWindowThreadProcessId(wnd, nullptr);
    for (int t = 0; t <= timeoutMs; t += 10) {
        if (GetKeyboardLayout(tid) == hkl) return true;
        Sleep(10);
    }
    return false;
}

// Какой символ даст клавиша vk в раскладке hkl.
static std::wstring KeyToText(UINT vk, bool shift, bool caps, HKL hkl) {
    BYTE ks[256] = {};
    if (shift) ks[VK_SHIFT] = 0x80;
    if (caps)  ks[VK_CAPITAL] = 0x01;
    wchar_t buf[8] = {};
    UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
    // Флаг 0x4: не менять состояние «мёртвых» клавиш (Windows 10 1607+).
    int n = ToUnicodeEx(vk, sc, ks, buf, 8, 0x4, hkl);
    if (n < 0) n = 1;            // мёртвая клавиша — берём её символ
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// На какой раскладке, скорее всего, набран текст: где набирается больше его букв.
static HKL DetectLayout(const std::wstring& text, HKL fallback) {
    std::vector<HKL> order{ fallback };
    for (HKL h : Layouts())
        if (h != fallback) order.push_back(h);

    HKL best = fallback;
    int bestScore = -1;
    for (HKL h : order) {
        int score = 0;
        for (wchar_t c : text) {
            if (!IsCharAlphaW(c)) continue;
            SHORT r = VkKeyScanExW(c, h);
            if (r != -1 && !(HIBYTE(r) & 6)) ++score;   // без Ctrl/Alt (AltGr)
        }
        if (score > bestScore) { bestScore = score; best = h; }
    }
    return best;
}

static std::wstring ConvertText(const std::wstring& text, HKL src, HKL dst) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { out += c; continue; }
        SHORT r = VkKeyScanExW(c, src);
        if (r == -1 || (HIBYTE(r) & 6)) { out += c; continue; }
        std::wstring s = KeyToText(LOBYTE(r), (HIBYTE(r) & 1) != 0, false, dst);
        // Знак, который и в новой раскладке остаётся знаком, не трогаем: «?» остаётся «?».
        if (s.empty() || (!IsCharAlphaNumericW(c) && s.size() == 1 && !IsCharAlphaNumericW(s[0])))
            out += c;
        else
            out += s;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Звук переключения («мягкий»: тёплый низкий «тук» с лёгким эхом).
// Синтезируется при запуске, никаких файлов. Английская раскладка — 440 Гц, любая другая — 330 Гц.
// ---------------------------------------------------------------------------

static std::vector<BYTE> g_wavOther, g_wavEn;

static std::vector<BYTE> MakeSoftWav(double freq) {
    const int    SR = 44100;
    const double pi = 3.14159265358979323846;
    const size_t n0 = static_cast<size_t>(0.15 * SR);
    const size_t n  = n0 + static_cast<size_t>(0.35 * SR);   // + хвост реверберации

    std::vector<double> src(n, 0.0);
    for (size_t i = 0; i < n0; ++i) {
        const double t = double(i) / SR;
        const double e = t < 0.006 ? t / 0.006 : std::exp(-(t - 0.006) / 0.035);
        src[i] = (std::sin(2 * pi * freq * t) + 0.25 * std::sin(4 * pi * freq * t)) * e;
    }

    // Небольшая «комната» (реверберация Шрёдера): 4 гребенчатых + 2 всепропускающих фильтра.
    std::vector<double> wet(n, 0.0), buf(n);
    const size_t cd[4] = { 1116, 1188, 1277, 1356 };
    const double cg[4] = { 0.78, 0.77, 0.76, 0.75 };
    for (int c = 0; c < 4; ++c) {
        for (size_t i = 0; i < n; ++i) {
            buf[i] = src[i] + (i >= cd[c] ? cg[c] * buf[i - cd[c]] : 0.0);
            wet[i] += buf[i] * 0.25;
        }
    }
    const size_t ad[2] = { 225, 556 };
    for (int a = 0; a < 2; ++a) {
        std::vector<double> y(n, 0.0);
        for (size_t i = 0; i < n; ++i) {
            const double xd = i >= ad[a] ? wet[i - ad[a]] : 0.0;
            const double yd = i >= ad[a] ? y[i - ad[a]] : 0.0;
            y[i] = -0.5 * wet[i] + xd + 0.5 * yd;
        }
        wet.swap(y);
    }

    const double mix = 0.15;
    std::vector<double> s(n);
    double peak = 1e-9;
    for (size_t i = 0; i < n; ++i) {
        s[i] = src[i] * (1 - mix) + wet[i] * mix;
        peak = std::max(peak, std::fabs(s[i]));
    }
    const size_t fade = static_cast<size_t>(0.03 * SR);
    for (size_t i = 0; i < fade && i < n; ++i) s[n - 1 - i] *= double(i) / fade;

    // WAV 16 бит моно.
    const DWORD dataBytes = static_cast<DWORD>(n * 2);
    std::vector<BYTE> w(44 + dataBytes);
    auto put32 = [&](size_t off, DWORD v) { std::memcpy(&w[off], &v, 4); };
    auto put16 = [&](size_t off, WORD v)  { std::memcpy(&w[off], &v, 2); };
    std::memcpy(&w[0], "RIFF", 4);  put32(4, 36 + dataBytes);
    std::memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, SR); put32(28, SR * 2); put16(32, 2); put16(34, 16);
    std::memcpy(&w[36], "data", 4); put32(40, dataBytes);
    for (size_t i = 0; i < n; ++i) {
        const short v = static_cast<short>(std::lround(s[i] / peak * 0.3 * 32767));
        std::memcpy(&w[44 + i * 2], &v, 2);
    }
    return w;
}

static void PlaySwitchSound(HKL hkl) {
    if (!g_sound) return;
    const std::vector<BYTE>& w = IsEnglish(hkl) ? g_wavEn : g_wavOther;
    if (!w.empty())
        PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// Последняя замеченная раскладка активного окна: по ней таймер ловит переключения,
// сделанные любым способом (Win+Пробел, Alt+Shift, языковая панель), и играет звук.
static HWND g_seenWnd = nullptr;
static HKL  g_seenHkl = nullptr;

// Переключение, сделанное самой программой: звук сразу, а таймер не должен сыграть его второй раз.
static void AnnounceSwitch(HWND fg, HKL hkl) {
    g_seenWnd = fg;
    g_seenHkl = WindowLayout(fg);
    PlaySwitchSound(hkl);
}

// ---------------------------------------------------------------------------
// Буфер обмена
// ---------------------------------------------------------------------------

struct ClipItem {
    UINT fmt;
    std::vector<BYTE> data;
};

static bool OpenClip() {
    for (int i = 0; i < 25; ++i) {
        if (OpenClipboard(g_mainWnd)) return true;
        Sleep(10);
    }
    return false;
}

static std::vector<ClipItem> SaveClipboard() {
    std::vector<ClipItem> items;
    if (!OpenClip()) return items;
    UINT f = 0;
    while ((f = EnumClipboardFormats(f)) != 0) {
        // GDI-форматы не являются блоками памяти; картинка всё равно сохранится через CF_DIB.
        if (f == CF_BITMAP || f == CF_DSPBITMAP || f == CF_ENHMETAFILE || f == CF_DSPENHMETAFILE ||
            f == CF_METAFILEPICT || f == CF_DSPMETAFILEPICT || f == CF_PALETTE || f == CF_OWNERDISPLAY)
            continue;
        HANDLE h = GetClipboardData(f);
        if (!h) continue;
        SIZE_T sz = GlobalSize(h);
        if (sz == 0 || sz > 64u * 1024 * 1024) continue;
        const BYTE* p = static_cast<const BYTE*>(GlobalLock(h));
        if (!p) continue;
        items.push_back({ f, std::vector<BYTE>(p, p + sz) });
        GlobalUnlock(h);
    }
    CloseClipboard();
    return items;
}

static HGLOBAL MakeGlobal(const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    if (size) std::memcpy(p, data, size);
    GlobalUnlock(h);
    return h;
}

// Чтобы временный текст не попадал в журнал буфера обмена (Win+V).
static void MarkNoHistory() {
    static const UINT fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT fmtHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    DWORD zero = 0;
    if (HGLOBAL h = MakeGlobal(&zero, sizeof(zero)))
        if (!SetClipboardData(fmtExclude, h)) GlobalFree(h);
    if (HGLOBAL h = MakeGlobal(&zero, sizeof(zero)))
        if (!SetClipboardData(fmtHistory, h)) GlobalFree(h);
}

static void RestoreClipboard(const std::vector<ClipItem>& items) {
    if (!OpenClip()) return;
    EmptyClipboard();
    MarkNoHistory();
    for (const auto& it : items) {
        if (HGLOBAL h = MakeGlobal(it.data.data(), it.data.size()))
            if (!SetClipboardData(it.fmt, h)) GlobalFree(h);
    }
    CloseClipboard();
}

static bool GetClipText(std::wstring& out) {
    if (!OpenClip()) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            ok = true;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

static bool SetClipText(const std::wstring& s) {
    if (!OpenClip()) return false;
    EmptyClipboard();
    MarkNoHistory();
    bool ok = false;
    if (HGLOBAL h = MakeGlobal(s.c_str(), (s.size() + 1) * sizeof(wchar_t))) {
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

// Скопировать выделенный текст. Содержимое буфера обмена сохраняется в saved.
static bool CopySelection(std::vector<ClipItem>& saved, std::wstring& text) {
    ReleaseModifiers();
    Sleep(30);               // даём окну заметить, что модификаторы горячей клавиши отпущены

    saved = SaveClipboard();
    const DWORD seq = GetClipboardSequenceNumber();

    // Если с первого раза окно не отреагировало — пробуем ещё раз.
    bool copied = false;
    for (int attempt = 0; attempt < 2 && !copied; ++attempt) {
        SendCtrlCombo('C');
        for (int i = 0; i < 40 && !copied; ++i) {
            Sleep(10);
            copied = GetClipboardSequenceNumber() != seq;
        }
    }
    if (!copied) return false;   // ничего не выделено — буфер обмена не тронут
    Sleep(30);

    if (!GetClipText(text) || text.empty()) {
        RestoreClipboard(saved);
        return false;
    }
    return true;
}

// Вставить текст вместо выделения и вернуть буфер обмена. Курсор остаётся в конце вставленного,
// выделения нет — можно сразу печатать дальше.
static void PasteText(const std::wstring& out, const std::vector<ClipItem>& saved) {
    if (SetClipText(out)) {
        SendCtrlCombo('V');
        Sleep(300);          // даём приложению забрать текст из буфера
    }
    RestoreClipboard(saved);
}

// Повторное нажатие сразу после вставки: заново выделяем только что вставленный текст.
static void ReselectIfRepeat(HWND fg) {
    EnterCriticalSection(&g_cs);
    const bool again = g_selRepeat && g_selWnd == fg;
    const size_t len = g_selLen;
    g_selRepeat = false;
    LeaveCriticalSection(&g_cs);
    if (!again) return;
    ReleaseModifiers();
    Sleep(20);
    SelectBack(len);
    Sleep(20);
}

// Запомнить вставленный текст для повторного нажатия.
static void RememberPaste(HWND fg, const std::wstring& out) {
    EnterCriticalSection(&g_cs);
    g_selRepeat = true;
    g_selWnd = fg;
    g_selLen = CaretLength(out);
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Действия
// ---------------------------------------------------------------------------

// Что напечатать вместо одной клавиши: клавиша (+Shift) или, если её нет, символ Unicode.
struct OutKey {
    WORD vk;
    bool shift;
    wchar_t uni;    // != 0 — печатаем этот символ напрямую
};

// Знак (не буква и не цифра)?
static bool IsSymbol(const std::wstring& s) {
    return s.size() == 1 && s[0] != L' ' && !IsCharAlphaNumericW(s[0]);
}

static void ConvertLastWord() {
    HWND fg = GetForegroundWindow();
    std::vector<KeyRec> seg;
    size_t eraseCount = 0;
    HKL src = nullptr, orig = nullptr;

    EnterCriticalSection(&g_cs);
    if (fg != g_bufWnd || g_buf.empty()) { LeaveCriticalSection(&g_cs); return; }
    size_t i = g_buf.size();
    while (i > 0 && g_buf[i - 1].vk == VK_SPACE) --i;     // хвостовые пробелы
    const size_t wordEnd = i;
    while (i > 0 && g_buf[i - 1].vk != VK_SPACE) --i;     // само слово
    if (i == wordEnd) { LeaveCriticalSection(&g_cs); return; }
    seg.assign(g_buf.begin() + static_cast<std::ptrdiff_t>(i), g_buf.end());
    const bool repeat = g_repeat && g_lastTarget && g_origLayout;
    src = repeat ? g_lastTarget : WindowLayout(fg);
    orig = repeat ? g_origLayout : src;
    eraseCount = repeat ? g_lastOutLen : seg.size();
    LeaveCriticalSection(&g_cs);

    HKL dst = NextLayout(src);
    if (!dst) { MessageBeep(MB_ICONWARNING); return; }

    // Буквы конвертируем по клавишам, а знаки оставляем теми, что набрал пользователь:
    // «дела?» -> «ltkf?», а не «ltkf&». Но если в новой раскладке на клавише знака стоит
    // буква («.» -> «ю», «,» -> «б»), это не знак, а ошибочно набранная буква — конвертируем.
    std::vector<OutKey> plan;
    std::wstring out;
    for (const KeyRec& k : seg) {
        const std::wstring typed = KeyToText(k.vk, k.shift, k.caps, orig);   // что нажимал пользователь
        const std::wstring conv  = KeyToText(k.vk, k.shift, k.caps, dst);    // что даст клавиша в новой раскладке
        if (IsSymbol(typed) && IsSymbol(conv) && typed != conv) {
            const SHORT r = VkKeyScanExW(typed[0], dst);
            if (r != -1 && !(HIBYTE(r) & 6)) plan.push_back({ LOBYTE(r), (HIBYTE(r) & 1) != 0, 0 });
            else                             plan.push_back({ 0, false, typed[0] });
            out += typed;
        } else {
            plan.push_back({ static_cast<WORD>(k.vk), k.shift, 0 });
            out += conv;
        }
    }

    ReleaseModifiers();
    SwitchLayout(fg, dst);
    const bool switched = WaitForLayout(fg, dst, 300);
    AnnounceSwitch(fg, dst);

    std::vector<INPUT> v;
    for (size_t j = 0; j < eraseCount; ++j) AddTap(v, VK_BACK);
    SendInputs(v);
    Sleep(10);

    if (switched) {
        // Основной путь: раскладка окна уже новая — «перенажимаем» клавиши.
        // Для программы это обычный ввод с клавиатуры. Нажатия идут в темпе живого набора:
        // некоторые программы (новый Блокнот) обрабатывают ввод пачкой и проверяют Shift
        // уже после того, как его отпустили.
        for (const OutKey& o : plan) {
            std::vector<INPUT> e;
            if (o.uni) {
                AddChar(e, o.uni); SendInputs(e); Sleep(5);
            } else if (o.shift) {
                AddKey(e, VK_LSHIFT, false); SendInputs(e); e.clear(); Sleep(8);
                AddTap(e, o.vk); SendInputs(e); e.clear(); Sleep(8);
                AddKey(e, VK_LSHIFT, true); SendInputs(e); Sleep(4);
            } else {
                AddTap(e, o.vk); SendInputs(e); Sleep(4);
            }
        }
    } else {
        // Запасной путь (окно не дало сменить раскладку): Unicode-ввод по одному символу —
        // некоторые программы, получив символы пачкой, подставляют во все последний.
        for (wchar_t c : out) {
            std::vector<INPUT> one;
            AddChar(one, c);
            SendInputs(one);
            Sleep(5);
        }
    }

    EnterCriticalSection(&g_cs);
    g_repeat = true;
    g_lastTarget = dst;
    g_origLayout = orig;
    g_lastOutLen = out.size();
    LeaveCriticalSection(&g_cs);
}

static void ConvertSelection() {
    HWND fg = GetForegroundWindow();
    ReselectIfRepeat(fg);
    std::vector<ClipItem> saved;
    std::wstring text;
    if (!CopySelection(saved, text)) return;

    HKL src = DetectLayout(text, WindowLayout(fg));
    HKL dst = NextLayout(src);
    if (!dst) { RestoreClipboard(saved); MessageBeep(MB_ICONWARNING); return; }

    const std::wstring out = ConvertText(text, src, dst);
    if (out != text) PasteText(out, saved);
    else             RestoreClipboard(saved);

    SwitchLayout(fg, dst);
    WaitForLayout(fg, dst, 200);
    AnnounceSwitch(fg, dst);
    ResetBuffer();
    if (out != text) RememberPaste(fg, out);
}

// «Как в предложении»: всё строчными, заглавная — в начале текста и после . ! ? и переноса строки.
static std::wstring ToSentenceCase(const std::wstring& s) {
    std::wstring r = ToLowerW(s);
    bool cap = true;
    for (wchar_t& c : r) {
        if (IsCharAlphaW(c)) {
            if (cap) { CharUpperBuffW(&c, 1); cap = false; }
        } else if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') {
            cap = true;
        } else if (IsCharAlphaNumericW(c)) {
            cap = false;   // цифра в начале предложения
        }
    }
    return r;
}

// Следующий регистр по кругу: строчные → Как в предложении → ЗАГЛАВНЫЕ → строчные.
// Смешанный текст («пРИВЕТ») сначала приводится к «Как в предложении».
static std::wstring NextCase(const std::wstring& text) {
    const std::wstring cyc[3] = { ToLowerW(text), ToSentenceCase(text), ToUpperW(text) };
    int idx = -1;
    for (int i = 0; i < 3; ++i)
        if (text == cyc[i]) { idx = i; break; }
    if (idx < 0) return cyc[1] != text ? cyc[1] : cyc[0];
    for (int step = 1; step <= 3; ++step) {
        const std::wstring& cand = cyc[(idx + step) % 3];
        if (cand != text) return cand;
    }
    return text;
}

static void ChangeSelectionCase() {
    HWND fg = GetForegroundWindow();
    ReselectIfRepeat(fg);
    std::vector<ClipItem> saved;
    std::wstring text;
    if (!CopySelection(saved, text)) return;
    const std::wstring out = NextCase(text);
    if (out != text) PasteText(out, saved);
    else             RestoreClipboard(saved);
    ResetBuffer();
    if (out != text) RememberPaste(fg, out);
}

static void SwitchLayoutByCaps() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    HKL dst = NextLayout(WindowLayout(fg));
    if (!dst) return;
    SwitchLayout(fg, dst);
    WaitForLayout(fg, dst, 200);
    AnnounceSwitch(fg, dst);
    ResetBuffer();
}

// ---------------------------------------------------------------------------
// Настройки, автозапуск
// ---------------------------------------------------------------------------

static std::wstring ConfigPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\KeySwitch" : std::wstring(L".");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.ini";
}

// Разобрать список исключений: по одному на строку или через «;». «chrome» -> «chrome.exe».
static std::vector<std::wstring> ParseExclusions(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&]() {
        const size_t a = cur.find_first_not_of(L" \t");
        const size_t b = cur.find_last_not_of(L" \t");
        if (a != std::wstring::npos) {
            std::wstring name = ToLowerW(cur.substr(a, b - a + 1));
            const size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name.erase(0, slash + 1);
            if (name.size() < 4 || name.compare(name.size() - 4, 4, L".exe") != 0) name += L".exe";
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
        cur.clear();
    };
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L';') flush();
        else cur += c;
    }
    flush();
    return out;
}

static std::wstring JoinExclusions(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

static Hotkey ReadHotkey(const std::wstring& p, const wchar_t* vkKey, const wchar_t* modKey, Hotkey def) {
    Hotkey h;
    h.vk   = GetPrivateProfileIntW(L"hotkeys", vkKey,  def.vk,   p.c_str());
    h.mods = GetPrivateProfileIntW(L"hotkeys", modKey, def.mods, p.c_str());
    if (h.vk == 0 || h.vk > 0xFE) h = def;
    if (h.vk == VK_CANCEL) h.vk = VK_PAUSE;   // старые версии записывали Ctrl+Pause как Ctrl+Break
    return h;
}

static void LoadConfig() {
    const std::wstring p = ConfigPath();
    const Hotkey w = ReadHotkey(p, L"word_vk", L"word_mods", { VK_PAUSE, 0 });
    const Hotkey s = ReadHotkey(p, L"sel_vk",  L"sel_mods",  { VK_PAUSE, MOD_SHIFT });
    Hotkey c = ReadHotkey(p, L"case_vk", L"case_mods", { VK_PAUSE, MOD_CONTROL });
    // Версия 1.1.2: смена регистра по умолчанию стала Ctrl+Pause вместо Alt+Pause.
    // Если в старом конфиге осталось прежнее значение по умолчанию — меняем на новое.
    if (GetPrivateProfileIntW(L"options", L"config_version", 1, p.c_str()) < 2 &&
        c.vk == VK_PAUSE && c.mods == MOD_ALT)
        c = { VK_PAUSE, MOD_CONTROL };
    wchar_t excl[4096] = {};
    GetPrivateProfileStringW(L"options", L"exclude", L"", excl, ARRAYSIZE(excl), p.c_str());

    InterlockedExchange(&g_capsSwitch, GetPrivateProfileIntW(L"options", L"caps_switch", 1, p.c_str()) ? 1 : 0);
    InterlockedExchange(&g_sound,      GetPrivateProfileIntW(L"options", L"sound",       1, p.c_str()) ? 1 : 0);
    InterlockedExchange(&g_autoFull,   GetPrivateProfileIntW(L"options", L"auto_fullscreen", 1, p.c_str()) ? 1 : 0);
    g_langMode = static_cast<int>(GetPrivateProfileIntW(L"options", L"language", LANG_MODE_AUTO, p.c_str()));
    if (g_langMode < LANG_MODE_AUTO || g_langMode > LANG_MODE_RU) g_langMode = LANG_MODE_AUTO;
    ApplyUiLanguage();

    EnterCriticalSection(&g_cs);
    g_hkWord = w;
    g_hkSel = s;
    g_hkCase = c;
    g_excluded = ParseExclusions(excl);
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_exclVersion);
}

static void SaveConfig() {
    const std::wstring p = ConfigPath();
    auto put = [&](const wchar_t* sec, const wchar_t* key, UINT v) {
        wchar_t b[16];
        wsprintfW(b, L"%u", v);
        WritePrivateProfileStringW(sec, key, b, p.c_str());
    };
    EnterCriticalSection(&g_cs);
    const Hotkey w = g_hkWord, s = g_hkSel, c = g_hkCase;
    const std::wstring excl = JoinExclusions(g_excluded, L";");
    LeaveCriticalSection(&g_cs);

    put(L"hotkeys", L"word_vk", w.vk);   put(L"hotkeys", L"word_mods", w.mods);
    put(L"hotkeys", L"sel_vk", s.vk);    put(L"hotkeys", L"sel_mods", s.mods);
    put(L"hotkeys", L"case_vk", c.vk);   put(L"hotkeys", L"case_mods", c.mods);
    put(L"options", L"caps_switch", g_capsSwitch ? 1 : 0);
    put(L"options", L"sound", g_sound ? 1 : 0);
    put(L"options", L"auto_fullscreen", g_autoFull ? 1 : 0);
    put(L"options", L"language", static_cast<UINT>(g_langMode));
    put(L"options", L"config_version", 2);
    WritePrivateProfileStringW(L"options", L"exclude", excl.c_str(), p.c_str());
}

static bool IsAutostart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

static std::wstring KeyName(UINT vk) {
    switch (vk) {
    case VK_PAUSE:    return L"Pause";
    case VK_CANCEL:   return L"Pause";   // Pause с Ctrl = Break; показываем как Pause
    case VK_SNAPSHOT: return L"PrintScreen";
    case VK_SCROLL:   return L"ScrollLock";
    case VK_SPACE:    return L"Space";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = static_cast<LONG>((sc & 0xFF) << 16);
    if (IsExtendedVk(static_cast<WORD>(vk))) lp |= 1 << 24;
    wchar_t buf[64];
    if (sc && GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wsprintfW(buf, L"VK_%02X", vk);
    return buf;
}

static std::wstring HotkeyText(const Hotkey& h) {
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl+";
    if (h.mods & MOD_ALT)     s += L"Alt+";
    if (h.mods & MOD_SHIFT)   s += L"Shift+";
    if (h.mods & MOD_WIN)     s += L"Win+";
    return s + KeyName(h.vk);
}

// ---------------------------------------------------------------------------
// Иконка в трее
// ---------------------------------------------------------------------------

// Звезда рисуется вручную со сглаживанием (8×8 подвыборок на пиксель).

struct PtF { double x, y; };

static void StarPolygon(PtF* p, double cx, double cy, double outerR, double innerR) {
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < 10; ++i) {
        const double a = -pi / 2 + i * pi / 5;
        const double rad = (i % 2) ? innerR : outerR;
        p[i] = { cx + rad * std::cos(a), cy + rad * std::sin(a) };
    }
}

static bool InPolygon(const PtF* p, int n, double x, double y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > y) != (p[j].y > y)) &&
            (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
            in = !in;
    }
    return in;
}

// Светлая ли тема панели задач Windows.
static bool IsLightTaskbar() {
    DWORD v = 0, size = sizeof(v);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    LONG r = RegQueryValueExW(k, L"SystemUsesLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && v != 0;
}

// outline — тонкая светлая обводка (нужна только на тёмной панели, иначе чёрная звезда не видна).
static HICON MakeStarIcon(COLORREF fill, bool outline) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 16) sz = 16;

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;                 // сверху вниз
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(scr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    std::vector<BYTE> maskBits(static_cast<size_t>((sz + 15) / 16 * 2) * sz, 0);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, maskBits.data());

    // Звезда на всю ширину иконки: ширина пятиконечной звезды = 1.902·R, высота = 1.809·R.
    // Эталонная пентаграммная звезда: внутренний радиус = R / φ² (φ — золотое сечение),
    // тогда рёбра соседних лучей лежат на одной прямой.
    const double ratio  = 0.38196601125010515;
    const double outerR = (sz - 0.6) / 1.902;
    const double cx = sz / 2.0;
    const double cy = sz / 2.0 + outerR * 0.0955;                // центрируем по габаритам, а не по геометрии
    const double stroke = outline ? std::max(0.8, sz / 20.0) : 0.0;
    const double innerR = outerR - stroke * 2.0;

    PtF outer[10], inner[10];
    StarPolygon(outer, cx, cy, outerR, outerR * ratio);
    StarPolygon(inner, cx, cy, innerR, innerR * ratio);

    const int SS = 8;
    const double fr = GetRValue(fill), fg = GetGValue(fill), fb = GetBValue(fill);
    const double ol = 235;                                       // цвет обводки (светло-серый)
    DWORD* px = static_cast<DWORD*>(bits);
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            int co = 0, ci = 0;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    const double X = x + (sx + 0.5) / SS, Y = y + (sy + 0.5) / SS;
                    if (InPolygon(outer, 10, X, Y)) {
                        ++co;
                        if (!outline || InPolygon(inner, 10, X, Y)) ++ci;
                    }
                }
            }
            const double a = co / double(SS * SS);       // общая непрозрачность
            const double f = ci / double(SS * SS);       // доля заливки
            const double w = a - f;                      // доля обводки
            // Альфа премультиплицирована: цвет уже умножен на покрытие.
            const DWORD A = static_cast<DWORD>(a * 255 + 0.5);
            const DWORD R = static_cast<DWORD>(f * fr + w * ol + 0.5);
            const DWORD G = static_cast<DWORD>(f * fg + w * ol + 0.5);
            const DWORD B = static_cast<DWORD>(f * fb + w * ol + 0.5);
            px[y * sz + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

static HICON g_iconOther = nullptr; // чёрная звезда — не английская раскладка (русская, украинская…)
static HICON g_iconEn    = nullptr; // красная звезда — английская
static HICON g_iconOff   = nullptr; // серая звезда — пауза, программа-исключение, полноэкранная игра
static int   g_shownState = -1;     // что сейчас в трее: 0 — EN, 1 — другая, 2 — выключено

static HICON StateIcon(int state) {
    return state == 2 ? g_iconOff : (state == 1 ? g_iconOther : g_iconEn);
}

static void SetTrayIcon(int state) {
    g_shownState = state;
    g_nid.hIcon = StateIcon(state);
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Создаёт иконки под текущую тему (и пересоздаёт при её смене).
static void BuildIcons() {
    const bool dark = !IsLightTaskbar();
    HICON oldOther = g_iconOther, oldEn = g_iconEn, oldOff = g_iconOff;
    g_iconOther = MakeStarIcon(RGB(17, 17, 17), dark);     // чёрная — не английская
    g_iconEn    = MakeStarIcon(RGB(220, 38, 38), false);   // красная — английская (видна на любом фоне)
    g_iconOff   = MakeStarIcon(RGB(150, 150, 150), false); // серая — выключено
    if (g_shownState >= 0) SetTrayIcon(g_shownState);
    if (oldOther) DestroyIcon(oldOther);
    if (oldEn)  DestroyIcon(oldEn);
    if (oldOff) DestroyIcon(oldOff);
}

static void UpdateTrayTip() {
    Hotkey w, s, c;
    EnterCriticalSection(&g_cs);
    w = g_hkWord;
    s = g_hkSel;
    c = g_hkCase;
    LeaveCriticalSection(&g_cs);
    std::wstring tip = g_paused
        ? std::wstring(T(S_TIP_PAUSED))
        : L"KeySwitch\n" + std::wstring(T(S_TIP_WORD)) + HotkeyText(w) +
          L"\n" + T(S_TIP_SEL) + HotkeyText(s) + L"\n" + T(S_TIP_CASE) + HotkeyText(c);
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WMU_TRAY;
    g_nid.hIcon = StateIcon(g_shownState < 0 ? 0 : g_shownState);
    lstrcpynW(g_nid.szTip, APP_NAME, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTrayTip();
}

// Последнее «внешнее» активное окно (не панель задач и не окна самой программы) —
// для пункта меню «Не работать в …».
static std::wstring g_lastExtExe;
static ExclusionCache g_timerExcl;   // только главный поток

static bool IsShellOrOwnWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    static const wchar_t* const shell[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland", L"Progman", L"WorkerW",
    };
    for (const wchar_t* s : shell)
        if (lstrcmpW(cls, s) == 0) return true;
    return false;
}

// Окно на весь экран: полноэкранная игра (DirectX/Vulkan) или окно без рамки размером с монитор.
static bool IsFullscreenWindow(HWND fg) {
    if (!fg || IsShellOrOwnWindow(fg)) return false;
    QUERY_USER_NOTIFICATION_STATE st;
    if (SUCCEEDED(SHQueryUserNotificationState(&st)) &&
        (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE))
        return true;
    // Обычное развёрнутое окно с заголовком не считаем (при автоскрытии панели задач
    // оно тоже занимает весь экран).
    if ((GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Таймер (каждые 100 мс): иконка, звук на внешние переключения, последнее внешнее окно,
// полноэкранный режим.
static void OnTimer() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    const HKL hkl = WindowLayout(fg);
    InterlockedExchange(&g_fullscreen, IsFullscreenWindow(fg) ? 1 : 0);
    const bool off = g_paused || (g_autoFull && g_fullscreen) || g_timerExcl.Check(fg);

    // Раскладка сменилась в том же окне (любым способом) — играем звук.
    if (fg == g_seenWnd && hkl != g_seenHkl && g_seenHkl && !off)
        PlaySwitchSound(hkl);
    g_seenWnd = fg;
    g_seenHkl = hkl;

    static HWND lastExtWnd = nullptr;   // имя процесса узнаём только при смене окна
    if (fg != lastExtWnd && !IsShellOrOwnWindow(fg)) {
        lastExtWnd = fg;
        const std::wstring exe = ProcessName(fg);
        if (!exe.empty()) g_lastExtExe = exe;
    }

    const int state = off ? 2 : (IsEnglish(hkl) ? 0 : 1);
    if (state != g_shownState) SetTrayIcon(state);
}

// ---------------------------------------------------------------------------
// Окно настроек
// ---------------------------------------------------------------------------

static Hotkey g_pend[3];            // 0 — слово, 1 — выделение, 2 — регистр
static int    g_captureFor = -1;
static HFONT  g_uiFont = nullptr;

static const int kHkEdit[3] = { IDC_WORD_EDIT, IDC_SEL_EDIT, IDC_CASE_EDIT };
static const int kHkBtn[3]  = { IDC_WORD_BTN,  IDC_SEL_BTN,  IDC_CASE_BTN };

static void SettingsShowHotkeys(HWND wnd) {
    for (int i = 0; i < 3; ++i) {
        SetDlgItemTextW(wnd, kHkEdit[i], HotkeyText(g_pend[i]).c_str());
        EnableWindow(GetDlgItem(wnd, kHkBtn[i]), TRUE);
    }
}

static void StartCapture(HWND wnd, int which) {
    g_captureFor = which;
    SetDlgItemTextW(wnd, kHkEdit[which], T(S_PRESS_COMBO));
    for (int i = 0; i < 3; ++i) EnableWindow(GetDlgItem(wnd, kHkBtn[i]), FALSE);
    InterlockedExchange(&g_capture, 1);
}

// --- Список программ-исключений ---

static void ExclAdd(HWND wnd, const std::wstring& exeOrPath) {
    const std::vector<std::wstring> parsed = ParseExclusions(exeOrPath);   // имя файла, нижний регистр, .exe
    if (parsed.empty()) return;
    HWND list = GetDlgItem(wnd, IDC_EXCLUDE);
    const std::wstring& name = parsed[0];
    LRESULT idx = SendMessageW(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name.c_str()));
    if (idx == LB_ERR) idx = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(idx), 0);
    EnableWindow(GetDlgItem(wnd, IDC_EXCL_REMOVE), TRUE);
}

static void ExclBrowse(HWND wnd) {
    wchar_t file[MAX_PATH] = {};
    wchar_t initDir[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", initDir, MAX_PATH);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = wnd;
    ofn.lpstrFilter = T(S_FILE_FILTER);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir;
    ofn.lpstrTitle = T(S_FILE_TITLE);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) ExclAdd(wnd, file);
}

static std::vector<std::wstring> g_runningNames;   // для меню «Из запущенных»

static BOOL CALLBACK CollectWindowProc(HWND w, LPARAM) {
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || GetWindowTextLengthW(w) == 0) return TRUE;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (IsShellOrOwnWindow(w)) return TRUE;
    const std::wstring exe = ProcessName(w);
    if (exe.empty() || exe == L"explorer.exe" || exe == L"applicationframehost.exe" ||
        exe == L"textinputhost.exe" || exe == L"systemsettings.exe")
        return TRUE;
    if (std::find(g_runningNames.begin(), g_runningNames.end(), exe) == g_runningNames.end())
        g_runningNames.push_back(exe);
    return TRUE;
}

static void ExclFromRunning(HWND wnd) {
    g_runningNames.clear();
    EnumWindows(CollectWindowProc, 0);
    std::sort(g_runningNames.begin(), g_runningNames.end());
    HMENU m = CreatePopupMenu();
    if (g_runningNames.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_NO_PROGRAMS));
    } else {
        for (size_t i = 0; i < g_runningNames.size() && i < 60; ++i)
            AppendMenuW(m, MF_STRING, ID_RUNNING_FIRST + i, g_runningNames[i].c_str());
    }
    RECT rc;
    GetWindowRect(GetDlgItem(wnd, IDC_EXCL_RUNNING), &rc);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, rc.left, rc.bottom, 0, wnd, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_RUNNING_FIRST && cmd < ID_RUNNING_FIRST + static_cast<int>(g_runningNames.size()))
        ExclAdd(wnd, g_runningNames[cmd - ID_RUNNING_FIRST]);
}

static void ExclRemove(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_EXCLUDE);
    const LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    SendMessageW(list, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (count > 0) SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(sel < count ? sel : count - 1), 0);
    EnableWindow(GetDlgItem(wnd, IDC_EXCL_REMOVE), count > 0);
}

static std::wstring ExclListText(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_EXCLUDE);
    std::wstring all;
    const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i) {
        const LRESULT len = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0);
        if (len <= 0) continue;
        std::wstring item(static_cast<size_t>(len) + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item[0]));
        item.resize(static_cast<size_t>(len));
        all += item + L"\n";
    }
    return all;
}

static LRESULT CALLBACK SettingsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                                     wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            return c;
        };
        const wchar_t* const labels[3] = { T(S_LBL_WORD), T(S_LBL_SEL), T(S_LBL_CASE) };
        for (int i = 0; i < 3; ++i) {
            const int y = 14 + i * 60;
            make(L"STATIC", labels[i], 0, 16, y, 408, 20, -1);
            make(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 16, y + 22, 300, 26, kHkEdit[i]);
            make(L"BUTTON", T(S_BTN_CHANGE), WS_TABSTOP | BS_PUSHBUTTON, 324, y + 22, 100, 26, kHkBtn[i]);
        }
        HWND caps = make(L"BUTTON", T(S_CHK_CAPS),
                         WS_TABSTOP | BS_AUTOCHECKBOX, 16, 196, 408, 22, IDC_CAPS);
        make(L"STATIC", T(S_CAPS_HINT), SS_NOPREFIX, 36, 218, 388, 20, -1);
        HWND snd  = make(L"BUTTON", T(S_CHK_SOUND),
                         WS_TABSTOP | BS_AUTOCHECKBOX, 16, 244, 408, 22, IDC_SOUND);
        HWND aut  = make(L"BUTTON", T(S_CHK_AUTOSTART),
                         WS_TABSTOP | BS_AUTOCHECKBOX, 16, 270, 408, 22, IDC_AUTOSTART);
        HWND full = make(L"BUTTON", T(S_CHK_FULLSCREEN),
                         WS_TABSTOP | BS_AUTOCHECKBOX, 16, 296, 408, 22, IDC_FULLSCREEN);
        SendMessageW(full, BM_SETCHECK, g_autoFull ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(caps, BM_SETCHECK, g_capsSwitch ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(snd,  BM_SETCHECK, g_sound ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(aut,  BM_SETCHECK, IsAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);

        make(L"STATIC", T(S_LBL_EXCL), 0, 16, 330, 408, 20, -1);
        HWND list = make(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_SORT | LBS_NOINTEGRALHEIGHT,
                         16, 352, 270, 86, IDC_EXCLUDE);
        make(L"BUTTON", T(S_BTN_BROWSE),   WS_TABSTOP | BS_PUSHBUTTON, 294, 352, 130, 26, IDC_EXCL_BROWSE);
        make(L"BUTTON", T(S_BTN_RUNNING),  WS_TABSTOP | BS_PUSHBUTTON, 294, 382, 130, 26, IDC_EXCL_RUNNING);
        make(L"BUTTON", T(S_BTN_REMOVE),   WS_TABSTOP | BS_PUSHBUTTON, 294, 412, 130, 26, IDC_EXCL_REMOVE);

        make(L"STATIC", T(S_LBL_LANG), 0, 16, 456, 150, 20, -1);
        HWND lang = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 170, 452, 146, 120, IDC_LANG);
        // Названия языков — на самих языках, как принято; первый пункт переводится.
        SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(S_LANG_AUTO)));
        SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Русский"));
        SendMessageW(lang, CB_SETCURSEL, static_cast<WPARAM>(g_langMode), 0);

        make(L"BUTTON", T(S_BTN_SAVE), WS_TABSTOP | BS_DEFPUSHBUTTON, 216, 494, 100, 28, IDOK);
        make(L"BUTTON", T(S_BTN_CANCEL), WS_TABSTOP | BS_PUSHBUTTON, 324, 494, 100, 28, IDCANCEL);

        EnterCriticalSection(&g_cs);
        g_pend[0] = g_hkWord;
        g_pend[1] = g_hkSel;
        g_pend[2] = g_hkCase;
        const std::vector<std::wstring> excl = g_excluded;
        LeaveCriticalSection(&g_cs);
        for (const std::wstring& e : excl)
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e.c_str()));
        EnableWindow(GetDlgItem(wnd, IDC_EXCL_REMOVE), FALSE);
        SettingsShowHotkeys(wnd);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WORD_BTN: StartCapture(wnd, 0); return 0;
        case IDC_SEL_BTN:  StartCapture(wnd, 1); return 0;
        case IDC_CASE_BTN: StartCapture(wnd, 2); return 0;
        case IDC_EXCL_BROWSE:  ExclBrowse(wnd);      return 0;
        case IDC_EXCL_RUNNING: ExclFromRunning(wnd); return 0;
        case IDC_EXCL_REMOVE:  ExclRemove(wnd);      return 0;
        case IDC_EXCLUDE:
            if (HIWORD(wp) == LBN_SELCHANGE)
                EnableWindow(GetDlgItem(wnd, IDC_EXCL_REMOVE),
                             SendMessageW(GetDlgItem(wnd, IDC_EXCLUDE), LB_GETCURSEL, 0, 0) != LB_ERR);
            return 0;
        case IDOK: {
            InterlockedExchange(&g_capture, 0);
            const std::wstring excl = ExclListText(wnd);

            EnterCriticalSection(&g_cs);
            g_hkWord = g_pend[0];
            g_hkSel  = g_pend[1];
            g_hkCase = g_pend[2];
            g_excluded = ParseExclusions(excl);
            LeaveCriticalSection(&g_cs);
            InterlockedIncrement(&g_exclVersion);
            InterlockedExchange(&g_capsSwitch, IsDlgButtonChecked(wnd, IDC_CAPS) == BST_CHECKED ? 1 : 0);
            InterlockedExchange(&g_sound,      IsDlgButtonChecked(wnd, IDC_SOUND) == BST_CHECKED ? 1 : 0);
            InterlockedExchange(&g_autoFull,   IsDlgButtonChecked(wnd, IDC_FULLSCREEN) == BST_CHECKED ? 1 : 0);
            {
                const LRESULT sel = SendMessageW(GetDlgItem(wnd, IDC_LANG), CB_GETCURSEL, 0, 0);
                g_langMode = (sel >= LANG_MODE_AUTO && sel <= LANG_MODE_RU) ? static_cast<int>(sel) : LANG_MODE_AUTO;
                ApplyUiLanguage();   // меню, подсказка и «О программе» сразу на новом языке
            }
            SaveConfig();
            SetAutostart(IsDlgButtonChecked(wnd, IDC_AUTOSTART) == BST_CHECKED);
            UpdateTrayTip();
            DestroyWindow(wnd);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(wnd);
            return 0;
        }
        break;
    case WMU_CAPTURED: {
        const int which = g_captureFor;
        g_captureFor = -1;
        if (wp != 0 && which >= 0) {
            const Hotkey h = { static_cast<UINT>(wp), static_cast<UINT>(lp) };
            bool taken = false;
            for (int i = 0; i < 3; ++i)
                if (i != which && g_pend[i].vk == h.vk && g_pend[i].mods == h.mods) taken = true;
            if (h.vk == VK_CAPITAL) {
                MessageBoxW(wnd, T(S_ERR_CAPS), APP_NAME, MB_ICONWARNING);
            } else if (IsCharKey(h.vk) && !(h.mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
                MessageBoxW(wnd, T(S_ERR_TEXTKEY), APP_NAME, MB_ICONWARNING);
            } else if (taken) {
                MessageBoxW(wnd, T(S_ERR_TAKEN), APP_NAME, MB_ICONWARNING);
            } else {
                g_pend[which] = h;
            }
        }
        SettingsShowHotkeys(wnd);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_capture, 0);
        g_captureFor = -1;
        g_settingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void OpenSettings() {
    if (g_settingsWnd) {
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    const DWORD style = WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT rc = { 0, 0, S(440), S(536) };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_settingsWnd = CreateWindowExW(exStyle, L"KeySwitchSettings", T(S_SETTINGS_TITLE), style,
                                    wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                                    w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
}

// ---------------------------------------------------------------------------
// О программе
// ---------------------------------------------------------------------------

static const wchar_t* const GITHUB_URL = L"https://github.com/NSMRK/keyswitch";

// Клик по ссылке в окне «О программе» — открываем страницу в браузере.
static HRESULT CALLBACK AboutCallback(HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

static void ShowAbout() {
    const std::wstring title  = L"KeySwitch " KS_VERSION_STR;
    const std::wstring body   = T(S_ABOUT_BODY);
    const std::wstring footer = std::wstring(L"by NSMRK \u00B7 \u00A9 2026 \u00B7 <a href=\"") + GITHUB_URL +
                                L"\">github.com/NSMRK/keyswitch</a>";

    // Современное окно TaskDialog (comctl32 v6, есть в Windows Vista и новее) умеет кликабельные ссылки.
    // Берём функцию динамически: если её нет, покажем обычное окно сообщения.
    using TaskDialogIndirectFn = HRESULT (WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    auto taskDialog = comctl ? reinterpret_cast<TaskDialogIndirectFn>(
                                   reinterpret_cast<void*>(GetProcAddress(comctl, "TaskDialogIndirect")))
                             : nullptr;
    if (taskDialog) {
        TASKDIALOGCONFIG tdc = {};
        tdc.cbSize = sizeof(tdc);
        tdc.hInstance = GetModuleHandleW(nullptr);
        tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tdc.dwCommonButtons = TDCBF_OK_BUTTON;
        tdc.pszWindowTitle = T(S_ABOUT_TITLE);
        tdc.pszMainIcon = MAKEINTRESOURCEW(1);
        tdc.pszMainInstruction = title.c_str();
        tdc.pszContent = body.c_str();
        tdc.pszFooter = footer.c_str();
        tdc.pfCallback = AboutCallback;
        if (SUCCEEDED(taskDialog(&tdc, nullptr, nullptr, nullptr))) return;
    }

    const std::wstring text = title + L"\n\n" + body + L"\n\nby NSMRK \u00B7 \u00A9 2026\n" + GITHUB_URL;
    MSGBOXPARAMSW mb = {};
    mb.cbSize = sizeof(mb);
    mb.hInstance = GetModuleHandleW(nullptr);
    mb.lpszText = text.c_str();
    mb.lpszCaption = T(S_ABOUT_TITLE);
    mb.dwStyle = MB_OK | MB_USERICON | MB_SETFOREGROUND | MB_TOPMOST;
    mb.lpszIcon = MAKEINTRESOURCEW(1);
    MessageBoxIndirectW(&mb);
}

// ---------------------------------------------------------------------------
// Главное (скрытое) окно
// ---------------------------------------------------------------------------

static void TogglePause() {
    InterlockedExchange(&g_paused, g_paused ? 0 : 1);
    ResetBuffer();
    UpdateTrayTip();
    OnTimer();
}

static void ToggleLastExclusion() {
    if (g_lastExtExe.empty()) return;
    EnterCriticalSection(&g_cs);
    auto it = std::find(g_excluded.begin(), g_excluded.end(), g_lastExtExe);
    if (it != g_excluded.end()) g_excluded.erase(it);
    else                        g_excluded.push_back(g_lastExtExe);
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_exclVersion);
    SaveConfig();
    OnTimer();
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    if (!g_lastExtExe.empty()) {
        const bool ex = IsExcludedName(g_lastExtExe);
        const std::wstring item = std::wstring(T(ex ? S_MENU_EXCL_ON : S_MENU_EXCL_OFF)) +
                                  T(S_QUOTE_OPEN) + g_lastExtExe + T(S_QUOTE_CLOSE);
        AppendMenuW(m, MF_STRING, ID_MENU_EXCLUDE, item.c_str());
    }
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_MENU_PAUSE, T(S_MENU_PAUSE));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_MENU_SETTINGS, T(S_MENU_SETTINGS));
    AppendMenuW(m, MF_STRING, ID_MENU_ABOUT, T(S_MENU_ABOUT));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_MENU_EXIT, T(S_MENU_EXIT));
    SetMenuDefaultItem(m, ID_MENU_SETTINGS, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WMU_WORD: ConvertLastWord();     return 0;
    case WMU_SEL:  ConvertSelection();    return 0;
    case WMU_CASE: ChangeSelectionCase(); return 0;
    case WMU_CAPS: SwitchLayoutByCaps();  return 0;
    case WM_TIMER:
        OnTimer();
        return 0;
    case WM_SETTINGCHANGE:
        // Пользователь переключил светлую/тёмную тему — перерисовываем звёзды.
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) BuildIcons();
        break;
    case WMU_TRAY:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        case WM_LBUTTONDBLCLK:
            OpenSettings();
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MENU_SETTINGS: OpenSettings();        break;
        case ID_MENU_ABOUT:    ShowAbout();           break;
        case ID_MENU_PAUSE:    TogglePause();         break;
        case ID_MENU_EXCLUDE:  ToggleLastExclusion(); break;
        case ID_MENU_EXIT:     DestroyWindow(wnd);    break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(wnd, 1);
        if (g_settingsWnd) DestroyWindow(g_settingsWnd);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated && g_taskbarCreated) {   // Проводник перезапустился
            AddTrayIcon();
            return 0;
        }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    ApplyUiLanguage();   // пока по языку Windows; выбор из настроек применится в LoadConfig
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"KeySwitch_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, T(S_ALREADY_RUNNING), APP_NAME, MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDPIAware();
    LoadLibraryW(L"comctl32.dll");   // современный вид кнопок (вместе с манифестом)
    InitializeCriticalSection(&g_cs);
    LoadConfig();

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);

    g_wavOther = MakeSoftWav(330.0);
    g_wavEn = MakeSoftWav(440.0);
    BuildIcons();

    HICON appIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"KeySwitchMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW sc = {};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = hi;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    sc.hIcon = appIcon ? appIcon : g_iconEn;
    sc.hIconSm = appIcon ? appIcon : g_iconEn;
    sc.lpszClassName = L"KeySwitchSettings";
    RegisterClassExW(&sc);

    g_mainWnd = CreateWindowExW(0, L"KeySwitchMain", APP_NAME, 0, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HWND fg = GetForegroundWindow();
    g_seenWnd = fg;
    g_seenHkl = fg ? WindowLayout(fg) : nullptr;
    g_shownState = (fg && !IsEnglish(g_seenHkl)) ? 1 : 0;
    AddTrayIcon();
    SetTimer(g_mainWnd, 1, 100, nullptr);   // следим за раскладкой активного окна

    HANDLE hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(hookThread, 2000);
        CloseHandle(hookThread);
    }
    PlaySoundW(nullptr, nullptr, 0);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_iconOther) DestroyIcon(g_iconOther);
    if (g_iconEn)  DestroyIcon(g_iconEn);
    if (g_iconOff) DestroyIcon(g_iconOff);
    DeleteCriticalSection(&g_cs);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
