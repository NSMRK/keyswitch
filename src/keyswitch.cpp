// KeySwitch: минималистичный переключатель раскладки для Windows.
// by NSMRK
//
//   Pause        исправить последнее набранное слово (повторное нажатие: следующая раскладка)
//   Ctrl+Pause   исправить выделенный текст
//   Shift+Pause  сменить регистр выделенного текста (строчные → Как в предложении → ЗАГЛАВНЫЕ)
//   Caps Lock    переключить раскладку (обычный Caps Lock: Shift+Caps Lock), отключается в настройках
//
// Значок в трее: код раскладки цветными буквами (EN, RU…). Английская раскладка нейтральная
// (чёрная или светлая), любая другая красная, серая когда программа отключена
// (пауза, программа-исключение или полноэкранная игра).
// Наведение или клик по значку открывает панель со списком раскладок.
// Интерфейс на русском или английском: автоматически по языку Windows или по выбору в настройках.
// Настройки: двойной клик по значку в трее, правый клик -> «Настройки…», Ctrl+Shift+Pause
// или повторный запуск KeySwitch.exe (удобно, если значок скрыт).
// Конфиг: %APPDATA%\KeySwitch\config.ini
//
// Сборка (MinGW-w64):
//   windres keyswitch.rc -O coff -o keyswitch_res.o
//   g++ -std=c++17 -O2 -s -municode -mwindows -static keyswitch.cpp keyswitch_res.o -o KeySwitch.exe ^
//       -luser32 -lshell32 -ladvapi32 -lgdi32 -lwinmm -lcomdlg32 -lgdiplus -lole32

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
#include <objidl.h>
#include <gdiplus.h>
#include <uxtheme.h>
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
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
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
    WMU_CAPTURED,            // хук -> окно настроек: записано сочетание (wParam=vk, lParam=mods; vk=0: отмена)
    WMU_FLYHIDE,             // хук: нажата клавиша, пока открыта панель раскладок
    WMU_FLYCLICK,            // хук: клик мышью, пока открыта панель (wParam=x, lParam=y)
    WMU_OPENSET,             // хук или второй запуск: открыть настройки
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
    IDC_SET_EDIT,
    IDC_SET_BTN,
    IDC_CAPS,
    IDC_SOUND,
    IDC_AUTOSTART,
    IDC_FULLSCREEN,
    IDC_ICONSTYLE,
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
static Hotkey g_hkSel  = { VK_PAUSE, MOD_CONTROL };
static Hotkey g_hkCase = { VK_PAUSE, MOD_SHIFT };
static Hotkey g_hkSet  = { VK_PAUSE, MOD_CONTROL | MOD_SHIFT };   // открыть настройки
static std::vector<std::wstring> g_excluded; // имена exe в нижнем регистре

static volatile LONG g_capture     = 0;      // 0 — обычный режим, иначе ждём сочетание для окна настроек
static volatile LONG g_paused      = 0;      // «Приостановить» в меню трея
static volatile LONG g_capsSwitch  = 1;      // Caps Lock переключает раскладку
static volatile LONG g_sound       = 1;      // звук при переключении
static volatile LONG g_autoFull    = 1;      // отключаться в полноэкранных играх
static volatile LONG g_flyVisible  = 0;      // открыта панель раскладок (читает поток хука)
static bool          g_trayHidden  = false;  // значок в трее скрыт (настройки: горячая клавиша)
static volatile LONG g_fullscreen  = 0;      // активное окно сейчас полноэкранное (обновляет таймер)
static volatile LONG g_exclVersion = 0;      // растёт при каждом изменении списка исключений

static HWND  g_mainWnd        = nullptr;
static HWND  g_settingsWnd    = nullptr;
static DWORD g_hookThreadId   = 0;
static UINT  g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid  = {};
static int   g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static float SF(float v) { return v * g_dpi / 96.0f; }

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
    S_LBL_ICON, S_ICON_SQUARE, S_ICON_LETTERS, S_ICON_STAR, S_ICON_HIDDEN, S_LBL_SET, S_HIDDEN_INFO,
    S_SUBTITLE, S_SEC_CURRENT, S_SEC_HOTKEYS, S_SEC_GENERAL, S_SEC_EXCL, S_ROW_WORD, S_ROW_SEL, S_ROW_CASE, S_ROW_SET,
    S_ROW_SOUND, S_ROW_SHOWICON, S_ROW_ICONSTYLE, S_SEG_LETTERS, S_LBL_LANG2, S_ADD, S_FOOT, S_PRESS_SHORT, S_HIDDEN_HINT, S_ABOUT_CONTACT, S_ABOUT_VERSION, S_ABOUT_CODE, S_ABOUT_CHAT,
    S_FLY_TITLE, S_FLY_FIX, S_FLY_SETTINGS, S_FLY_OFF,
    S_COUNT
};

struct LocStr { const wchar_t* ru; const wchar_t* en; };

static const LocStr kStrings[S_COUNT] = {
    /* S_ALREADY_RUNNING */ { L"KeySwitch уже запущен. Его значок в трее, рядом с часами.",
                              L"KeySwitch is already running. Its icon is in the system tray, next to the clock." },
    /* S_TIP_PAUSED      */ { L"KeySwitch на паузе", L"KeySwitch is paused" },
    /* S_TIP_WORD        */ { L"Слово: ", L"Word: " },
    /* S_TIP_SEL         */ { L"Выделение: ", L"Selection: " },
    /* S_TIP_CASE        */ { L"Регистр: ", L"Case: " },
    /* S_SETTINGS_TITLE  */ { L"Настройки KeySwitch", L"KeySwitch Settings" },
    /* S_LBL_WORD        */ { L"Исправить последнее слово:", L"Fix the last word:" },
    /* S_LBL_SEL         */ { L"Исправить выделенный текст:", L"Fix the selected text:" },
    /* S_LBL_CASE        */ { L"Сменить регистр выделенного текста:", L"Change the case of the selected text:" },
    /* S_BTN_CHANGE      */ { L"Изменить", L"Change" },
    /* S_CHK_CAPS        */ { L"Caps Lock переключает раскладку", L"Caps Lock switches the layout" },
    /* S_CAPS_HINT       */ { L"Обычный Caps Lock: Shift + Caps Lock", L"Regular Caps Lock: Shift + Caps Lock" },
    /* S_CHK_SOUND       */ { L"Звук при переключении раскладки", L"Play a sound when the layout changes" },
    /* S_CHK_AUTOSTART   */ { L"Запускать вместе с Windows", L"Start with Windows" },
    /* S_CHK_FULLSCREEN  */ { L"Отключаться в полноэкранных играх", L"Turn off in full-screen games" },
    /* S_LBL_EXCL        */ { L"Отключать в программах:", L"Turn off in these programs:" },
    /* S_BTN_BROWSE      */ { L"Обзор…", L"Browse…" },
    /* S_BTN_RUNNING     */ { L"Из запущенных…", L"From running…" },
    /* S_BTN_REMOVE      */ { L"Удалить", L"Remove" },
    /* S_BTN_SAVE        */ { L"Сохранить", L"Save" },
    /* S_BTN_CANCEL      */ { L"Отмена", L"Cancel" },
    /* S_PRESS_COMBO     */ { L"Нажмите сочетание… (Esc: отмена)", L"Press a key combination… (Esc to cancel)" },
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
    /* S_LBL_ICON        */ { L"Значок в трее:", L"Tray icon:" },
    /* S_ICON_SQUARE     */ { L"Цвет", L"Color" },
    /* S_ICON_LETTERS    */ { L"Буквы (RU, EN)", L"Letters (EN, RU)" },
    /* S_ICON_STAR       */ { L"Звезда", L"Star" },
    /* S_ICON_HIDDEN     */ { L"Не показывать", L"Hidden" },
    /* S_LBL_SET         */ { L"Открыть настройки:", L"Open Settings:" },
    /* S_HIDDEN_INFO     */ { L"Значок в трее скрыт. KeySwitch продолжает работать.\n\nОткрыть настройки: %s\nили запустите KeySwitch.exe ещё раз.",
                              L"The tray icon is hidden. KeySwitch keeps working.\n\nOpen Settings: %s\nor run KeySwitch.exe again." },
    /* S_SUBTITLE        */ { L"Переключатель раскладки", L"Keyboard layout switcher" },
    /* S_SEC_CURRENT     */ { L"Текущая раскладка", L"Current layout" },
    /* S_SEC_HOTKEYS     */ { L"Горячие клавиши", L"Hotkeys" },
    /* S_SEC_GENERAL     */ { L"Общие", L"General" },
    /* S_SEC_EXCL        */ { L"Не работать в программах", L"Turn off in these programs" },
    /* S_ROW_WORD        */ { L"Исправить последнее слово", L"Fix the last word" },
    /* S_ROW_SEL         */ { L"Исправить выделенный текст", L"Fix the selected text" },
    /* S_ROW_CASE        */ { L"Сменить регистр", L"Change case" },
    /* S_ROW_SET         */ { L"Открыть настройки", L"Open Settings" },
    /* S_ROW_SOUND       */ { L"Звук при переключении", L"Sound on switch" },
    /* S_ROW_SHOWICON    */ { L"Показывать значок в трее", L"Show the tray icon" },
    /* S_ROW_ICONSTYLE   */ { L"Вид значка", L"Icon style" },
    /* S_SEG_LETTERS     */ { L"Буквы", L"Letters" },
    /* S_LBL_LANG2       */ { L"Язык интерфейса", L"Interface language" },
    /* S_ADD             */ { L"+ Добавить", L"+ Add" },
    /* S_FOOT            */ { L"Лёгкий. Автономный. Без рекламы.", L"Light. Offline. No ads." },
    /* S_PRESS_SHORT     */ { L"Нажмите сочетание", L"Press keys" },
    /* S_HIDDEN_HINT     */ { L"Настройки: ", L"Settings: " },
    /* S_ABOUT_CONTACT   */ { L"Связаться со мной в Telegram:", L"Contact me on Telegram:" },
    /* S_ABOUT_VERSION   */ { L"Версия ", L"Version " },
    /* S_ABOUT_CODE      */ { L"Исходный код", L"Source code" },
    /* S_ABOUT_CHAT      */ { L"Написать автору", L"Message the author" },
    /* S_FLY_TITLE       */ { L"Раскладка", L"Keyboard layout" },
    /* S_FLY_FIX         */ { L"Исправить слово: ", L"Fix word: " },
    /* S_FLY_SETTINGS    */ { L"Настройки", L"Settings" },
    /* S_FLY_OFF         */ { L"Отключено", L"Off" },
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

    // Любая клавиша закрывает панель раскладок (как у системных всплывающих окон).
    if (down && g_flyVisible) PostMessageW(g_mainWnd, WMU_FLYHIDE, 0, 0);

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

    // Открыть настройки: работает всегда, даже на паузе и в исключениях.
    {
        EnterCriticalSection(&g_cs);
        const Hotkey hset = g_hkSet;
        LeaveCriticalSection(&g_cs);
        if (vk == hset.vk && CurrentMods() == hset.mods) {
            g_swallowUp = vk;
            PostMessageW(g_mainWnd, WMU_OPENSET, 0, 0);
            return 1;
        }
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
        // Клик мышью мог переставить курсор, и буфер больше не соответствует тексту.
        ResetBuffer();
        if (g_flyVisible) {
            const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            PostMessageW(g_mainWnd, WMU_FLYCLICK, static_cast<WPARAM>(m->pt.x), static_cast<LPARAM>(m->pt.y));
        }
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

// Все раскладки пользователя. GetKeyboardLayoutList иногда возвращает только уже загруженные
// (например, в сеансе удалённого рабочего стола), поэтому дополняем список из реестра
// (HKCU\Keyboard Layout\Preload) и подгружаем недостающие, не делая их активными.
static std::vector<HKL> Layouts() {
    int n = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> v(n > 0 ? n : 0);
    if (n > 0) GetKeyboardLayoutList(n, v.data());
    static bool preloaded = false;
    if (!preloaded) {
        preloaded = true;
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Preload", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            for (int i = 1; i <= 32; ++i) {
                wchar_t name[8], klid[KL_NAMELENGTH] = {};
                wsprintfW(name, L"%d", i);
                DWORD size = sizeof(klid), type = 0;
                if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(klid), &size) != ERROR_SUCCESS)
                    break;
                if (type != REG_SZ) continue;
                LoadKeyboardLayoutW(klid, KLF_NOTELLSHELL);   // уже загруженная раскладка просто вернётся
            }
            RegCloseKey(k);
        }
        n = GetKeyboardLayoutList(0, nullptr);
        v.assign(n > 0 ? n : 0, nullptr);
        if (n > 0) GetKeyboardLayoutList(n, v.data());
    }
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
    const int ver = static_cast<int>(GetPrivateProfileIntW(L"options", L"config_version", 1, p.c_str()));
    // До версии 1.4 по умолчанию было: выделение Shift+Pause, регистр Ctrl+Pause.
    const Hotkey oldSel = { VK_PAUSE, MOD_SHIFT }, oldCase = { VK_PAUSE, MOD_CONTROL };
    Hotkey s = ReadHotkey(p, L"sel_vk",  L"sel_mods",  ver < 3 ? oldSel  : Hotkey{ VK_PAUSE, MOD_CONTROL });
    Hotkey c = ReadHotkey(p, L"case_vk", L"case_mods", ver < 3 ? oldCase : Hotkey{ VK_PAUSE, MOD_SHIFT });
    // Версия 1.1.2: смена регистра по умолчанию стала Ctrl+Pause вместо Alt+Pause.
    if (ver < 2 && c.vk == VK_PAUSE && c.mods == MOD_ALT)
        c = oldCase;
    // Версия 1.4: выделение и регистр поменялись местами. Если пользователь их не менял, меняем и у него.
    if (ver < 3 && s.vk == oldSel.vk && s.mods == oldSel.mods && c.vk == oldCase.vk && c.mods == oldCase.mods)
        std::swap(s, c);
    wchar_t excl[4096] = {};
    GetPrivateProfileStringW(L"options", L"exclude", L"", excl, ARRAYSIZE(excl), p.c_str());

    InterlockedExchange(&g_capsSwitch, GetPrivateProfileIntW(L"options", L"caps_switch", 1, p.c_str()) ? 1 : 0);
    InterlockedExchange(&g_sound,      GetPrivateProfileIntW(L"options", L"sound",       1, p.c_str()) ? 1 : 0);
    InterlockedExchange(&g_autoFull,   GetPrivateProfileIntW(L"options", L"auto_fullscreen", 1, p.c_str()) ? 1 : 0);
    g_langMode = static_cast<int>(GetPrivateProfileIntW(L"options", L"language", LANG_MODE_AUTO, p.c_str()));
    if (g_langMode < LANG_MODE_AUTO || g_langMode > LANG_MODE_RU) g_langMode = LANG_MODE_AUTO;
    ApplyUiLanguage();
    g_trayHidden = GetPrivateProfileIntW(L"options", L"tray_hidden", 0, p.c_str()) != 0;
    if (GetPrivateProfileIntW(L"options", L"tray_icon", 0, p.c_str()) == 2) g_trayHidden = true;   // 1.4.5: «скрыт» был видом значка
    const Hotkey st = ReadHotkey(p, L"settings_vk", L"settings_mods", Hotkey{ VK_PAUSE, MOD_CONTROL | MOD_SHIFT });

    EnterCriticalSection(&g_cs);
    g_hkWord = w;
    g_hkSel = s;
    g_hkCase = c;
    g_hkSet = st;
    g_excluded = ParseExclusions(excl);
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_exclVersion);
}

static void SaveConfig();

// Перенос старого конфига на новую версию сразу при запуске.
static void MigrateConfigIfNeeded() {
    const std::wstring p = ConfigPath();
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // конфига нет: всё по умолчанию
    if (GetPrivateProfileIntW(L"options", L"config_version", 1, p.c_str()) < 4) SaveConfig();
}

static void SaveConfig() {
    const std::wstring p = ConfigPath();
    auto put = [&](const wchar_t* sec, const wchar_t* key, UINT v) {
        wchar_t b[16];
        wsprintfW(b, L"%u", v);
        WritePrivateProfileStringW(sec, key, b, p.c_str());
    };
    EnterCriticalSection(&g_cs);
    const Hotkey w = g_hkWord, s = g_hkSel, c = g_hkCase, st = g_hkSet;
    const std::wstring excl = JoinExclusions(g_excluded, L";");
    LeaveCriticalSection(&g_cs);

    put(L"hotkeys", L"word_vk", w.vk);   put(L"hotkeys", L"word_mods", w.mods);
    put(L"hotkeys", L"sel_vk", s.vk);    put(L"hotkeys", L"sel_mods", s.mods);
    put(L"hotkeys", L"case_vk", c.vk);   put(L"hotkeys", L"case_mods", c.mods);
    put(L"hotkeys", L"settings_vk", st.vk); put(L"hotkeys", L"settings_mods", st.mods);
    put(L"options", L"caps_switch", g_capsSwitch ? 1 : 0);
    put(L"options", L"sound", g_sound ? 1 : 0);
    put(L"options", L"auto_fullscreen", g_autoFull ? 1 : 0);
    put(L"options", L"language", static_cast<UINT>(g_langMode));
    WritePrivateProfileStringW(L"options", L"tray_icon", nullptr, p.c_str());   // вид значка больше не выбирается
    put(L"options", L"tray_hidden", g_trayHidden ? 1 : 0);
    put(L"options", L"config_version", 4);
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

// ---------------------------------------------------------------------------
// Значок в трее: плашка с кодом раскладки или звезда
// ---------------------------------------------------------------------------

static bool g_darkTaskbar = true;      // кэш темы панели задач (обновляется при смене темы)
static HICON g_trayIcon = nullptr;     // текущий значок в трее
static std::wstring g_trayKey;         // что на нём нарисовано (чтобы не перерисовывать каждый тик)

// Двухбуквенный код языка раскладки: EN, RU, UK…
static std::wstring LayoutCode(HKL hkl) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    const LANGID lid = LOWORD(reinterpret_cast<UINT_PTR>(hkl));
    std::wstring code;
    if (LCIDToLocaleName(MAKELCID(lid, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        for (const wchar_t* c = name; *c && *c != L'-' && code.size() < 3; ++c) code += *c;
    }
    if (code.empty()) code = L"??";
    return ToUpperW(code);
}

static std::wstring LocaleInfo(HKL hkl, LCTYPE type) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    wchar_t buf[128] = {};
    const LANGID lid = LOWORD(reinterpret_cast<UINT_PTR>(hkl));
    if (LCIDToLocaleName(MAKELCID(lid, SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) <= 0) return L"";
    if (GetLocaleInfoEx(name, type, buf, ARRAYSIZE(buf)) <= 0) return L"";
    std::wstring r = buf;
    if (!r.empty()) CharUpperBuffW(&r[0], 1);   // «русский» -> «Русский»
    return r;
}

// Цвета плашки. kind: 0 английская, 1 другая, 2 выключено.
static void BadgeColors(int kind, bool dark, Gdiplus::Color& bg, Gdiplus::Color& fg) {
    using Gdiplus::Color;
    if (kind == 1)      { bg = Color(255, 220, 38, 38);  fg = Color(255, 255, 255, 255); }
    else if (kind == 0) { bg = dark ? Color(255, 242, 242, 244) : Color(255, 31, 31, 35);
                          fg = dark ? Color(255, 27, 27, 31)    : Color(255, 255, 255, 255); }
    else                { bg = dark ? Color(255, 91, 93, 100)   : Color(255, 156, 160, 168);
                          fg = dark ? Color(255, 201, 203, 209) : Color(255, 255, 255, 255); }
}

static void AddRoundRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float rad) {
    const float d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

// Шрифт с запасным вариантом: если нужного семейства нет, берём системный sans-serif.
static Gdiplus::Font* MakeFont(const wchar_t* family, float px, int style) {
    Gdiplus::FontFamily ff(family);
    if (ff.IsAvailable()) return new Gdiplus::Font(&ff, px, style, Gdiplus::UnitPixel);
    return new Gdiplus::Font(Gdiplus::FontFamily::GenericSansSerif(), px, style, Gdiplus::UnitPixel);
}

// Плашка с кодом раскладки в квадрате size×size.
static void DrawBadge(Gdiplus::Graphics& g, float x, float y, float size, const std::wstring& code, int kind, bool dark) {
    using namespace Gdiplus;
    Color bg, fg;
    BadgeColors(kind, dark, bg, fg);
    GraphicsPath path;
    AddRoundRect(path, RectF(x, y, size, size), std::max(2.0f, size * 0.22f));
    SolidBrush b(bg);
    g.FillPath(&b, &path);
    SolidBrush tb(fg);
    if (kind == 2) {   // выключено: короткая черта
        const float w = size * 0.38f, h = std::max(1.5f, size * 0.1f);
        g.FillRectangle(&tb, x + (size - w) / 2, y + (size - h) / 2, w, h);
        return;
    }
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap);
    float em = size * (code.size() > 2 ? 0.44f : 0.56f);
    for (int i = 0; i < 8; ++i) {   // уменьшаем шрифт, пока код не влезет в плашку
        Font* f = MakeFont(L"Segoe UI", em, FontStyleBold);
        RectF box;
        g.MeasureString(code.c_str(), -1, f, PointF(0, 0), &sf, &box);
        delete f;
        if (box.Width <= size * 0.84f) break;
        em *= 0.9f;
    }
    Font* f = MakeFont(L"Segoe UI", em, FontStyleBold);
    g.DrawString(code.c_str(), -1, f, RectF(x, y + em * 0.04f, size, size), &sf, &tb);
    delete f;
}

// ---------------------------------------------------------------------------
// Значок в трее: код раскладки цветными буквами (EN, RU…), без подложки.
// Буквы E, N, R, U нарисованы геометрией специально под маленький размер:
// прямые линии стоят ровно по пикселям, сглажены только скругления и диагонали.
// Остальные буквы (для других раскладок) рисуются шрифтом Segoe UI.
// ---------------------------------------------------------------------------

// Черновик с подвыборкой: каждый пиксель значка делится на KS×KS точек.
struct LetterCanvas {
    static const int KS = 16;
    int sz;
    std::vector<unsigned char> m;   // 1: точка закрашена
    explicit LetterCanvas(int s) : sz(s), m(static_cast<size_t>(s * KS) * (s * KS), 0) {}
    int N() const { return sz * KS; }
    template <class F> void Apply(double x0, double y0, double x1, double y1, unsigned char v, F inside) {
        const int n = N();
        const int a = std::max(0, static_cast<int>(std::floor(x0 * KS))), b = std::min(n, static_cast<int>(std::ceil(x1 * KS)));
        const int c = std::max(0, static_cast<int>(std::floor(y0 * KS))), d = std::min(n, static_cast<int>(std::ceil(y1 * KS)));
        for (int yy = c; yy < d; ++yy)
            for (int xx = a; xx < b; ++xx) {
                const double X = (xx + 0.5) / KS, Y = (yy + 0.5) / KS;
                if (inside(X, Y)) m[static_cast<size_t>(yy) * n + xx] = v;
            }
    }
    void Rect(double x0, double y0, double x1, double y1, unsigned char v = 1) {
        Apply(x0, y0, x1, y1, v, [&](double X, double Y) { return X >= x0 && X < x1 && Y >= y0 && Y < y1; });
    }
    // Скруглённый прямоугольник; corners: левый верхний, правый верхний, правый нижний, левый нижний.
    void RRect(double x0, double y0, double x1, double y1, double r, const bool corners[4], unsigned char v = 1) {
        r = std::max(0.0, std::min({ r, (x1 - x0) / 2, (y1 - y0) / 2 }));
        const double cx[4] = { x0 + r, x1 - r, x1 - r, x0 + r }, cy[4] = { y0 + r, y0 + r, y1 - r, y1 - r };
        const int sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
        Apply(x0, y0, x1, y1, v, [&](double X, double Y) {
            if (!(X >= x0 && X < x1 && Y >= y0 && Y < y1)) return false;
            for (int i = 0; i < 4; ++i)
                if (corners[i] && r > 0 && (X - cx[i]) * sx[i] > 0 && (Y - cy[i]) * sy[i] > 0 &&
                    (X - cx[i]) * (X - cx[i]) + (Y - cy[i]) * (Y - cy[i]) > r * r)
                    return false;
            return true;
        });
    }
    // Выпуклый четырёхугольник (диагонали N и ножки R).
    void Quad(const double px[4], const double py[4], unsigned char v = 1) {
        const double x0 = std::min({ px[0], px[1], px[2], px[3] }), x1 = std::max({ px[0], px[1], px[2], px[3] });
        const double y0 = std::min({ py[0], py[1], py[2], py[3] }), y1 = std::max({ py[0], py[1], py[2], py[3] });
        Apply(x0, y0, x1, y1, v, [&](double X, double Y) {
            int pos = 0, neg = 0;
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) % 4;
                const double cr = (px[j] - px[i]) * (Y - py[i]) - (py[j] - py[i]) * (X - px[i]);
                if (cr > 0) ++pos; else if (cr < 0) ++neg;
            }
            return pos == 0 || neg == 0;
        });
    }
    double Cov(int x, int y) const {
        const int n = N();
        int c = 0;
        for (int yy = y * KS; yy < (y + 1) * KS; ++yy)
            for (int xx = x * KS; xx < (x + 1) * KS; ++xx) c += m[static_cast<size_t>(yy) * n + xx];
        return c / static_cast<double>(KS * KS);
    }
};

static int RoundI(double v) { return static_cast<int>(std::floor(v + 0.5)); }

// Ширина буквы при высоте значка 16 px (для других размеров пропорционально).
static int LetterWidth16(wchar_t ch) { return ch == L'E' ? 6 : 7; }

static void DrawLetter(LetterCanvas& cv, wchar_t ch, double x, double y, double h, double w, double wd) {
    static const bool kBottom[4] = { false, false, true, true },
                      kRight[4] = { false, true, true, false };
    if (ch == L'E') {
        const double mid = y + RoundI((h - w) * 0.45);
        cv.Rect(x, y, x + w, y + h);
        cv.Rect(x, y, x + wd, y + w);
        cv.Rect(x, mid, x + wd - (wd > 3 ? std::max(1.0, std::floor(w / 2)) : 0), mid + w);
        cv.Rect(x, y + h - w, x + wd, y + h);
    } else if (ch == L'N') {
        cv.Rect(x, y, x + w, y + h);
        cv.Rect(x + wd - w, y, x + wd, y + h);
        const double t = w * 1.15;
        const double px[4] = { x, x + t, x + wd, x + wd - t }, py[4] = { y, y, y + h, y + h };
        cv.Quad(px, py);
    } else if (ch == L'U') {
        const double r = std::min(wd / 2, h * 0.32);
        cv.RRect(x, y, x + wd, y + h, r, kBottom);
        cv.RRect(x + w, y - 1, x + wd - w, y + h - w, std::max(0.01, r - w), kBottom, 0);
    } else if (ch == L'R') {
        const double bh = RoundI(h * 0.56);
        const double r = std::min(bh / 2, wd * 0.45);
        cv.RRect(x, y, x + wd, y + bh, r, kRight);
        cv.RRect(x + w, y + w, x + wd - w, y + bh - w, std::max(0.01, r - w), kRight, 0);
        cv.Rect(x, y, x + w, y + h);
        const double lx = x + wd * 0.42, t = w * 1.2;
        const double px[4] = { lx, lx + t, x + wd, x + wd - t }, py[4] = { y + bh - w, y + bh - w, y + h, y + h };
        cv.Quad(px, py);
    }
}

static bool HasDrawnLetters(const std::wstring& code) {
    if (code.empty()) return false;
    for (wchar_t c : code)
        if (c != L'E' && c != L'N' && c != L'R' && c != L'U') return false;
    return true;
}

// Покрытие букв (0..255) для кода из букв E, N, R, U.
static void DrawnLettersCoverage(const std::wstring& code, int sz, std::vector<BYTE>& cov) {
    const double k = sz / 16.0;
    const int h = RoundI(11 * k), w = std::max(2, RoundI(2 * k)), gap = std::max(1, RoundI(k));
    std::vector<int> widths;
    int total = 0;
    for (wchar_t c : code) {
        widths.push_back(std::max(3, RoundI(LetterWidth16(c) * k)));
        total += widths.back();
    }
    total += gap * (static_cast<int>(code.size()) - 1);
    int x = (sz - total) / 2 + (code[0] == L'E' ? (sz - total) % 2 : 0);
    const int y = (sz - h) / 2;
    LetterCanvas cv(sz);
    for (size_t i = 0; i < code.size(); ++i) {
        DrawLetter(cv, code[i], x, y, h, w, widths[i]);
        x += widths[i] + gap;
    }
    cov.assign(static_cast<size_t>(sz) * sz, 0);
    for (int yy = std::max(0, y); yy < std::min(sz, y + h); ++yy)
        for (int xx = 0; xx < sz; ++xx)
            cov[static_cast<size_t>(yy) * sz + xx] = static_cast<BYTE>(RoundI(cv.Cov(xx, yy) * 255));
}

// Покрытие букв шрифтом (для раскладок, чьи буквы не нарисованы вручную).
static void FontLettersCoverage(const std::wstring& code, int sz, std::vector<BYTE>& cov) {
    cov.assign(static_cast<size_t>(sz) * sz, 0);
    HDC scr = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    const int scratch = sz * 3;
    BITMAPINFO sbi = {};
    sbi.bmiHeader.biSize = sizeof(sbi.bmiHeader);
    sbi.bmiHeader.biWidth = scratch;
    sbi.bmiHeader.biHeight = -scratch;
    sbi.bmiHeader.biPlanes = 1;
    sbi.bmiHeader.biBitCount = 32;
    void* sbits = nullptr;
    HBITMAP sbmp = CreateDIBSection(dc, &sbi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ sold = SelectObject(dc, sbmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DWORD* sp = static_cast<DWORD*>(sbits);
    auto font = [](int hh) {
        return CreateFontW(-hh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    auto measure = [&](int hh, int& l, int& t, int& r, int& btm) {
        HFONT f = font(hh);
        HGDIOBJ of = SelectObject(dc, f);
        std::fill(sp, sp + scratch * scratch, 0u);
        TextOutW(dc, sz, sz / 2, code.c_str(), static_cast<int>(code.size()));
        GdiFlush();
        SelectObject(dc, of);
        DeleteObject(f);
        l = scratch; t = scratch; r = -1; btm = -1;
        for (int yy = 0; yy < scratch; ++yy)
            for (int xx = 0; xx < scratch; ++xx)
                if (((sp[yy * scratch + xx] >> 8) & 0xFF) >= 96) {
                    l = std::min(l, xx); r = std::max(r, xx);
                    t = std::min(t, yy); btm = std::max(btm, yy);
                }
        return r >= 0;
    };
    const int maxW = sz, maxH = RoundI(sz * 11.0 / 16);
    int hh = sz, l = 0, t = 0, r = -1, btm = -1;
    for (; hh >= 6; --hh)
        if (measure(hh, l, t, r, btm) && r - l + 1 <= maxW && btm - t + 1 <= maxH) break;
    if (r >= 0) {
        const int inkW = r - l + 1, inkH = btm - t + 1;
        const int dx = sz + (sz - inkW) / 2 - l, dy = sz / 2 + (sz - inkH) / 2 - t;
        std::fill(sp, sp + scratch * scratch, 0u);
        HFONT f = font(hh);
        HGDIOBJ of = SelectObject(dc, f);
        TextOutW(dc, dx, dy, code.c_str(), static_cast<int>(code.size()));
        GdiFlush();
        SelectObject(dc, of);
        DeleteObject(f);
        for (int yy = 0; yy < sz; ++yy)
            for (int xx = 0; xx < sz; ++xx)
                cov[static_cast<size_t>(yy) * sz + xx] = static_cast<BYTE>((sp[yy * scratch + xx] >> 8) & 0xFF);
    }
    SelectObject(dc, sold);
    DeleteObject(sbmp);
    DeleteDC(dc);
}

// kind: 0 английская (чёрная, на тёмной панели белая), 1 другая (красная), 2 выключено (серая).
static HICON MakeLetterIcon(const std::wstring& code, int kind, bool dark) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 16) sz = 16;
    COLORREF col;
    if (kind == 1)      col = dark ? RGB(244, 67, 67) : RGB(220, 38, 38);
    else if (kind == 0) col = dark ? RGB(255, 255, 255) : RGB(27, 27, 27);
    else                col = dark ? RGB(128, 131, 138) : RGB(150, 154, 162);

    std::vector<BYTE> cov;
    if (HasDrawnLetters(code)) DrawnLettersCoverage(code, sz, cov);
    else                       FontLettersCoverage(code.empty() ? std::wstring(L"?") : code, sz, cov);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(scr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);
    DWORD* px = static_cast<DWORD*>(bits);
    const DWORD rgb = (GetRValue(col) << 16) | (GetGValue(col) << 8) | GetBValue(col);
    for (int i = 0; i < sz * sz; ++i) px[i] = (static_cast<DWORD>(cov[static_cast<size_t>(i)]) << 24) | rgb;

    std::vector<BYTE> maskBits(static_cast<size_t>((sz + 15) / 16 * 2) * sz, 0);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// state: 0 английская, 1 другая, 2 выключено.
static void UpdateTrayIcon(int state, const std::wstring& code) {
    const bool dark = g_darkTaskbar;
    const std::wstring key = std::to_wstring(state) + L"|" + code + L"|" + (dark ? L"d" : L"l");
    if (key == g_trayKey && g_trayIcon) return;
    HICON icon = MakeLetterIcon(code, state, dark);
    HICON old = g_trayIcon;
    g_trayIcon = icon;
    g_trayKey = key;
    g_nid.hIcon = icon;
    g_nid.uFlags = NIF_ICON;
    if (!g_trayHidden) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
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
    if (g_trayHidden) return;   // значок скрыт в настройках
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WMU_TRAY;
    g_nid.hIcon = g_trayIcon;
    lstrcpynW(g_nid.szTip, APP_NAME, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    // Версия 4: вместо стандартной подсказки приходят NIN_POPUPOPEN/NIN_POPUPCLOSE, и мы показываем свою панель.
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    UpdateTrayTip();
}

// Последнее «внешнее» активное окно (не панель задач и не окна самой программы) —
// для пункта меню «Не работать в …».
static std::wstring g_lastExtExe;
static HWND g_lastExtHwnd = nullptr;   // последнее «внешнее» окно: в нём переключаем раскладку из панели
static ExclusionCache g_timerExcl;   // только главный поток

static bool IsShellOrOwnWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    static const wchar_t* const shell[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland", L"Progman", L"WorkerW", L"#32769",
        L"Xaml_WindowedPopupClass", L"Shell_InputSwitchTopLevelWindow",
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
    if (!IsShellOrOwnWindow(fg)) g_lastExtHwnd = fg;
    if (fg != lastExtWnd && !IsShellOrOwnWindow(fg)) {
        lastExtWnd = fg;
        const std::wstring exe = ProcessName(fg);
        if (!exe.empty()) g_lastExtExe = exe;
    }

    const int state = off ? 2 : (IsEnglish(hkl) ? 0 : 1);
    UpdateTrayIcon(state, LayoutCode(hkl));
}

// ---------------------------------------------------------------------------
// Окно настроек
// ---------------------------------------------------------------------------

static HFONT  g_uiFont = nullptr;
static const wchar_t* const GITHUB_URL = L"https://github.com/NSMRK/keyswitch";

// Вывести своё окно на передний план, даже если сейчас активна другая программа
// (настройки открыты горячей клавишей или вторым запуском).
static void ForceForeground(HWND w) {
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attach = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    if (attach) AttachThreadInput(me, fgThread, FALSE);
}

static void HideFlyout();

// Показать или скрыть значок в трее.
static void ApplyTrayVisibility() {
    if (g_trayHidden) {
        HideFlyout();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    } else {
        g_trayKey.clear();
        OnTimer();       // нарисовать значок под текущую раскладку
        AddTrayIcon();
    }
}

// ---------------------------------------------------------------------------
// Окно настроек: нарисовано целиком вручную (GDI+), изменения применяются сразу.
// Все размеры в логических пикселях (96 dpi), масштаб через трансформацию.
// ---------------------------------------------------------------------------

enum {
    ST_W = 460, ST_TITLE = 74, ST_PADX = 18, ST_ROW = 50, ST_ROW2 = 62, ST_H2 = 34, ST_LAY = 58, ST_FOOT = 46,
};

enum SetItem {
    IT_NONE = 0, IT_MIN, IT_CLOSE,
    IT_HK0, IT_HK1, IT_HK2, IT_HK3,
    IT_CAPS, IT_AUTO, IT_SOUND, IT_GAMES, IT_SHOWICON,
    IT_LANG, IT_ADD, IT_GITHUB,
    IT_CHIP = 100,   // крестик у программы-исключения: IT_CHIP + номер
    IT_LAYOUT = 200, // карточка раскладки: IT_LAYOUT + номер
};

struct SetHit { Gdiplus::RectF r; int id; bool scrolls; };

static std::vector<SetHit> g_setHits;
static int   g_setHover   = IT_NONE;
static int   g_setCapture = -1;        // какое сочетание сейчас записываем (0..3), -1 нет
static float g_setScroll  = 0;         // прокрутка содержимого (логические px)
static float g_setContent = 0;         // полная высота содержимого под заголовком
static HKL   g_setLayout  = nullptr;   // раскладка, показанная в карточках
static bool  g_setDark    = false;

struct SetColors {
    Gdiplus::Color win, card, line, text, sub, pill, pilltext, accent, accentsoft, toggleoff, chip, hover, knob, red;
};

static bool AppsDark() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

static SetColors SetTheme() {
    using Gdiplus::Color;
    if (g_setDark)
        return { Color(255, 32, 33, 36), Color(255, 43, 44, 48), Color(255, 54, 55, 60), Color(255, 242, 242, 244),
                 Color(255, 154, 156, 165), Color(255, 58, 59, 65), Color(255, 232, 232, 236), Color(255, 76, 194, 255),
                 Color(255, 31, 58, 77), Color(255, 91, 93, 100), Color(255, 53, 54, 59), Color(255, 50, 51, 56),
                 Color(255, 27, 27, 31), Color(255, 232, 52, 52) };
    return { Color(255, 249, 249, 251), Color(255, 255, 255, 255), Color(255, 236, 236, 240), Color(255, 27, 27, 31),
             Color(255, 107, 111, 122), Color(255, 240, 241, 244), Color(255, 43, 45, 51), Color(255, 0, 103, 192),
             Color(255, 232, 241, 251), Color(255, 197, 200, 207), Color(255, 243, 244, 246), Color(255, 246, 247, 249),
             Color(255, 255, 255, 255), Color(255, 220, 38, 38) };
}

// Шрифт интерфейса: Segoe UI Variable (Windows 11), иначе Segoe UI.
static Gdiplus::Font* SetFont(float px, bool semibold = false, bool bold = false) {
    const wchar_t* fams[3] = { semibold ? L"Segoe UI Variable Text Semibold" : L"Segoe UI Variable Text",
                               semibold ? L"Segoe UI Semibold" : L"Segoe UI", nullptr };
    for (int i = 0; fams[i]; ++i) {
        Gdiplus::FontFamily ff(fams[i]);
        if (ff.IsAvailable())
            return new Gdiplus::Font(&ff, px, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }
    return MakeFont(L"Segoe UI", px, bold || semibold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular);
}

static float TextW(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f) {
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF box;
    g.MeasureString(s.c_str(), -1, f, Gdiplus::PointF(0, 0), &sf, &box);
    return box.Width;
}

// Текст, выровненный по вертикали в полосе [y, y+h].
static void TextAt(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f, const Gdiplus::Color& c,
                   float x, float y, float h, float w = 1000) {
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetFormatFlags(sf.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, Gdiplus::RectF(x, y, w, h), &sf, &b);
}

static void FillRound(Gdiplus::Graphics& g, const Gdiplus::Color& c, float x, float y, float w, float h, float r) {
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, Gdiplus::RectF(x, y, w, h), r);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void StrokeRound(Gdiplus::Graphics& g, const Gdiplus::Color& c, float x, float y, float w, float h, float r, float pw) {
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, Gdiplus::RectF(x, y, w, h), r);
    Gdiplus::Pen pen(c, pw);
    g.DrawPath(&pen, &p);
}

// Значки строк, нарисованные линиями в квадрате 24×24, выводятся размером 18×18.
enum { ICO_KEYB, ICO_CAPS, ICO_WIN, ICO_SOUND, ICO_GAME, ICO_EYE, ICO_TRAY, ICO_GLOBE, ICO_GEAR,
       ICO_BLOCK, ICO_PAUSE, ICO_INFO, ICO_POWER, ICO_CODE, ICO_CHAT };

static void DrawRowIcon(Gdiplus::Graphics& g, int kind, float x, float y, const Gdiplus::Color& c) {
    using namespace Gdiplus;
    const GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(18.0f / 24, 18.0f / 24);
    Pen pen(c, 1.7f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    SolidBrush br(c);
    switch (kind) {
    case ICO_KEYB: {
        GraphicsPath p; AddRoundRect(p, RectF(2.5f, 5.5f, 19, 13), 2.5f); g.DrawPath(&pen, &p);
        const float xs[4] = { 6, 9.5f, 13, 16.5f };
        for (float k : xs) g.DrawLine(&pen, k, 9.5f, k + 1, 9.5f);
        g.DrawLine(&pen, 7.5f, 14.5f, 16.5f, 14.5f);
        break;
    }
    case ICO_CAPS: {
        PointF pts[7] = { {12, 4}, {19, 11}, {15, 11}, {15, 16}, {9, 16}, {9, 11}, {5, 11} };
        g.DrawPolygon(&pen, pts, 7);
        g.DrawLine(&pen, 9.0f, 19.5f, 15.0f, 19.5f);
        break;
    }
    case ICO_WIN:
        g.FillRectangle(&br, 4.0f, 4.0f, 7.3f, 7.3f);   g.FillRectangle(&br, 12.7f, 4.0f, 7.3f, 7.3f);
        g.FillRectangle(&br, 4.0f, 12.7f, 7.3f, 7.3f);  g.FillRectangle(&br, 12.7f, 12.7f, 7.3f, 7.3f);
        break;
    case ICO_SOUND: {
        PointF pts[6] = { {4, 9.5f}, {7.5f, 9.5f}, {12, 5.5f}, {12, 18.5f}, {7.5f, 14.5f}, {4, 14.5f} };
        g.DrawPolygon(&pen, pts, 6);
        g.DrawArc(&pen, 12.85f - 4, 12.0f - 4, 8.0f, 8.0f, -49.0f, 98.0f);
        g.DrawArc(&pen, 12.9f - 7.5f, 12.0f - 7.5f, 15.0f, 15.0f, -47.0f, 94.0f);
        break;
    }
    case ICO_GAME: {
        GraphicsPath p; AddRoundRect(p, RectF(2.5f, 7.5f, 19, 10), 5); g.DrawPath(&pen, &p);
        g.DrawLine(&pen, 7.5f, 10.8f, 7.5f, 14.2f);
        g.DrawLine(&pen, 5.8f, 12.5f, 9.2f, 12.5f);
        g.FillEllipse(&br, 15.0f, 10.0f, 2.0f, 2.0f);
        g.FillEllipse(&br, 16.6f, 12.6f, 2.0f, 2.0f);
        break;
    }
    case ICO_EYE: {
        GraphicsPath p;
        p.AddBezier(2.5f, 12.0f, 5.0f, 7.0f, 9.0f, 5.5f, 12.0f, 5.5f);
        p.AddBezier(12.0f, 5.5f, 15.0f, 5.5f, 19.0f, 7.0f, 21.5f, 12.0f);
        p.AddBezier(21.5f, 12.0f, 19.0f, 17.0f, 15.0f, 18.5f, 12.0f, 18.5f);
        p.AddBezier(12.0f, 18.5f, 9.0f, 18.5f, 5.0f, 17.0f, 2.5f, 12.0f);
        p.CloseFigure();
        g.DrawPath(&pen, &p);
        g.DrawEllipse(&pen, 9.0f, 9.0f, 6.0f, 6.0f);
        break;
    }
    case ICO_TRAY: {
        GraphicsPath p; AddRoundRect(p, RectF(3, 14, 18, 6), 1.5f); g.DrawPath(&pen, &p);
        g.FillRectangle(&br, 14.0f, 16.0f, 4.0f, 2.0f);
        g.DrawLine(&pen, 12.0f, 4.0f, 12.0f, 11.0f);
        PointF a[3] = { {9, 8.5f}, {12, 11.5f}, {15, 8.5f} };
        g.DrawLines(&pen, a, 3);
        break;
    }
    case ICO_GLOBE:
        g.DrawEllipse(&pen, 3.5f, 3.5f, 17.0f, 17.0f);
        g.DrawLine(&pen, 3.5f, 12.0f, 20.5f, 12.0f);
        g.DrawEllipse(&pen, 8.8f, 3.5f, 6.4f, 17.0f);
        break;
    case ICO_CODE: {
        PointF l[3] = { {8.5f, 7}, {3.5f, 12}, {8.5f, 17} };
        PointF r[3] = { {15.5f, 7}, {20.5f, 12}, {15.5f, 17} };
        g.DrawLines(&pen, l, 3);
        g.DrawLines(&pen, r, 3);
        g.DrawLine(&pen, 13.5f, 5.5f, 10.5f, 18.5f);
        break;
    }
    case ICO_CHAT: {
        GraphicsPath p;
        AddRoundRect(p, RectF(3.5f, 4.5f, 17, 12.5f), 3.5f);
        g.DrawPath(&pen, &p);
        PointF t[3] = { {8, 17}, {7.5f, 20.5f}, {12, 17} };
        g.DrawLines(&pen, t, 3);
        break;
    }
    case ICO_BLOCK:
        g.DrawEllipse(&pen, 3.5f, 3.5f, 17.0f, 17.0f);
        g.DrawLine(&pen, 6.2f, 6.2f, 17.8f, 17.8f);
        break;
    case ICO_PAUSE:
        g.DrawEllipse(&pen, 3.5f, 3.5f, 17.0f, 17.0f);
        g.DrawLine(&pen, 10.0f, 8.5f, 10.0f, 15.5f);
        g.DrawLine(&pen, 14.0f, 8.5f, 14.0f, 15.5f);
        break;
    case ICO_INFO:
        g.DrawEllipse(&pen, 3.5f, 3.5f, 17.0f, 17.0f);
        g.DrawLine(&pen, 12.0f, 11.0f, 12.0f, 16.5f);
        g.FillEllipse(&br, 10.9f, 6.9f, 2.2f, 2.2f);
        break;
    case ICO_POWER:
        g.DrawArc(&pen, 4.5f, 5.0f, 15.0f, 15.0f, -55.0f, 290.0f);
        g.DrawLine(&pen, 12.0f, 3.5f, 12.0f, 11.5f);
        break;
    case ICO_GEAR: {
        g.DrawEllipse(&pen, 9.0f, 9.0f, 6.0f, 6.0f);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265 / 4;
            g.DrawLine(&pen, static_cast<float>(12 + 6.0 * std::cos(a)), static_cast<float>(12 + 6.0 * std::sin(a)),
                       static_cast<float>(12 + 8.5 * std::cos(a)), static_cast<float>(12 + 8.5 * std::sin(a)));
        }
        g.DrawEllipse(&pen, 6.0f, 6.0f, 12.0f, 12.0f);
        break;
    }
    }
    g.Restore(st);
}

static void DrawStarShape(Gdiplus::Graphics& g, float cx, float cy, float r, const Gdiplus::Color& c) {
    PtF p[10];
    StarPolygon(p, cx, cy, r, r * 0.382);
    Gdiplus::PointF q[10];
    for (int i = 0; i < 10; ++i) q[i] = Gdiplus::PointF(static_cast<float>(p[i].x), static_cast<float>(p[i].y));
    Gdiplus::SolidBrush b(c);
    g.FillPolygon(&b, q, 10);
}

static void DrawToggle(Gdiplus::Graphics& g, const SetColors& c, float x, float y, bool on) {
    using namespace Gdiplus;
    if (on) {
        FillRound(g, c.accent, x, y, 40, 20, 10);
        SolidBrush k(c.knob);
        g.FillEllipse(&k, x + 24.0f, y + 4.0f, 12.0f, 12.0f);
    } else {
        StrokeRound(g, c.toggleoff, x + 0.75f, y + 0.75f, 38.5f, 18.5f, 9.25f, 1.5f);
        SolidBrush k(c.toggleoff);
        g.FillEllipse(&k, x + 5.0f, y + 5.0f, 10.0f, 10.0f);
    }
}

static void DrawChevron(Gdiplus::Graphics& g, const Gdiplus::Color& c, float x, float y, bool down) {
    Gdiplus::Pen pen(c, 1.5f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (down) { Gdiplus::PointF a[3] = { {x, y}, {x + 4, y + 4}, {x + 8, y} }; g.DrawLines(&pen, a, 3); }
    else      { Gdiplus::PointF a[3] = { {x, y}, {x + 4, y + 4}, {x, y + 8} }; g.DrawLines(&pen, a, 3); }
}

static void DrawCross(Gdiplus::Graphics& g, const Gdiplus::Color& c, float cx, float cy, float r, float pw) {
    Gdiplus::Pen pen(c, pw);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r);
    g.DrawLine(&pen, cx - r, cy + r, cx + r, cy - r);
}

static Hotkey* HotkeySlot(int i) {
    switch (i) {
    case 0: return &g_hkWord;
    case 1: return &g_hkSel;
    case 2: return &g_hkCase;
    default: return &g_hkSet;
    }
}

// «Ctrl+Pause» -> «Ctrl + Pause» для красивых плашек.
static std::wstring PrettyHotkey(const Hotkey& h) {
    std::wstring s = HotkeyText(h), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'+' && i > 0 && i + 1 < s.size()) out += L" + ";
        else out += s[i];
    }
    return out;
}

// Раскладка, которую показывают карточки: как у значка в трее, по активному окну
// (обычно это само окно настроек).
static HKL SettingsLayoutNow() {
    HWND fg = GetForegroundWindow();
    if (fg && (fg == g_settingsWnd || !IsShellOrOwnWindow(fg))) return WindowLayout(fg);
    HWND t = g_lastExtHwnd;
    if (t && IsWindow(t)) return WindowLayout(t);
    return GetKeyboardLayout(0);
}

static std::vector<HKL> g_setLayouts;   // раскладки в карточках, по порядку (для кликов)

// Рисует окно и запоминает области для кликов. Координаты логические.
static void SettingsPaint(Gdiplus::Graphics& g, float W, float H) {
    using namespace Gdiplus;
    const SetColors c = SetTheme();
    g_setHits.clear();
    g.Clear(c.win);

    Font* fH1    = SetFont(20, false, true);
    Font* fSub   = SetFont(13);
    Font* fH2    = SetFont(13, true);
    Font* fRow   = SetFont(14);
    Font* fSmall = SetFont(12);
    Font* fPill  = SetFont(12.5f, true);
    Font* fCode  = SetFont(19, false, true);
    Font* fLay   = SetFont(13.5f);
    Font* fFoot  = SetFont(12);
    Font* fMini  = SetFont(7.5f, false, true);

    const float L = ST_PADX, CW = W - 2 * ST_PADX;
    const float top = ST_TITLE - g_setScroll;   // где начинается содержимое на экране

    // --- Прокручиваемое содержимое ---
    const GraphicsState st0 = g.Save();
    g.SetClip(RectF(0, ST_TITLE, W, H - ST_TITLE));
    float y = top;
    auto hit = [&](float x, float yy, float w, float h, int id) {
        g_setHits.push_back({ RectF(x, yy, w, h), id, true });
    };
    auto h2 = [&](const wchar_t* s) {
        TextAt(g, s, fH2, c.sub, L + 4, y + 12, 18);
        y += ST_H2;
    };
    // Карточка: фон и рамка, строки рисуются поверх.
    auto card = [&](float yy, float h) {
        FillRound(g, c.card, L, yy, CW, h, 10);
        StrokeRound(g, c.line, L + 0.5f, yy + 0.5f, CW - 1, h - 1, 10, 1);
    };
    auto sep = [&](float yy) {
        Pen p(c.line, 1);
        g.DrawLine(&p, L + 1, yy, L + CW - 1, yy);
    };
    auto hoverBg = [&](float yy, float h, int id, bool first, bool last) {
        if (g_setHover != id) return;
        GraphicsPath p;
        const float r = 9;
        RectF rc(L + 1, yy + (first ? 1.0f : 0.5f), CW - 2, h - (first ? 1.0f : 0.5f) - (last ? 1.0f : 0.5f));
        if (first || last) {
            p.AddArc(rc.X, rc.Y, first ? 2 * r : 0.01f, first ? 2 * r : 0.01f, 180, 90);
            p.AddArc(rc.GetRight() - (first ? 2 * r : 0.01f), rc.Y, first ? 2 * r : 0.01f, first ? 2 * r : 0.01f, 270, 90);
            p.AddArc(rc.GetRight() - (last ? 2 * r : 0.01f), rc.GetBottom() - (last ? 2 * r : 0.01f),
                     last ? 2 * r : 0.01f, last ? 2 * r : 0.01f, 0, 90);
            p.AddArc(rc.X, rc.GetBottom() - (last ? 2 * r : 0.01f), last ? 2 * r : 0.01f, last ? 2 * r : 0.01f, 90, 90);
            p.CloseFigure();
        } else {
            p.AddRectangle(rc);
        }
        SolidBrush b(c.hover);
        g.FillPath(&b, &p);
    };
    // Подпись строки с необязательной второй строкой.
    auto rowLabel = [&](float yy, float h, int icon, const std::wstring& text, const std::wstring& sub, float maxW) {
        DrawRowIcon(g, icon, L + 16, yy + (h - 18) / 2, c.sub);
        if (sub.empty()) {
            TextAt(g, text, fRow, c.text, L + 46, yy, h, maxW);
        } else {
            TextAt(g, text, fRow, c.text, L + 46, yy + h / 2 - 19, 20, maxW);
            TextAt(g, sub, fSmall, c.sub, L + 46, yy + h / 2 + 1, 18, maxW);
        }
    };

    // Текущая раскладка
    y += 2;
    h2(T(S_SEC_CURRENT));
    {
        std::vector<HKL> lays = Layouts();
        if (lays.empty()) lays.push_back(GetKeyboardLayout(0));
        g_setLayouts = lays;
        g_setLayout = SettingsLayoutNow();
        const int cols = std::max(1, std::min(3, static_cast<int>(lays.size())));
        const float gap = 10, cw = (CW - gap * (cols - 1)) / cols;
        for (size_t i = 0; i < lays.size(); ++i) {
            const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
            const float x = L + col * (cw + gap), yy = y + row * (ST_LAY + gap);
            const bool on = lays[i] == g_setLayout;
            const int id = IT_LAYOUT + static_cast<int>(i);
            FillRound(g, on ? c.accentsoft : (g_setHover == id ? c.hover : c.card), x, yy, cw, ST_LAY, 10);
            hit(x, yy, cw, ST_LAY, id);
            if (on) StrokeRound(g, c.accent, x + 0.75f, yy + 0.75f, cw - 1.5f, ST_LAY - 1.5f, 10, 1.5f);
            else    StrokeRound(g, c.line, x + 0.5f, yy + 0.5f, cw - 1, ST_LAY - 1, 10, 1);
            const std::wstring code = LayoutCode(lays[i]);
            const bool en = IsEnglish(lays[i]);
            TextAt(g, code, fCode, en ? c.text : c.red, x + 16, yy, ST_LAY);
            const float codeW = TextW(g, code, fCode);
            TextAt(g, LocaleInfo(lays[i], LOCALE_SNATIVELANGUAGENAME), fLay, c.sub, x + 16 + codeW + 10, yy, ST_LAY,
                   cw - codeW - 60);
            SolidBrush dot(on ? c.accent : c.toggleoff);
            g.FillEllipse(&dot, x + cw - 24, yy + ST_LAY / 2 - 4, 8.0f, 8.0f);
        }
        const int rows = (static_cast<int>(lays.size()) + cols - 1) / cols;
        y += rows * ST_LAY + (rows - 1) * gap;
    }

    // Горячие клавиши
    y += 6;
    h2(T(S_SEC_HOTKEYS));
    {
        Hotkey hk[4];
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < 4; ++i) hk[i] = *HotkeySlot(i);
        LeaveCriticalSection(&g_cs);
        const float h = ST_ROW * 4 + ST_ROW2;
        card(y, h);
        const wchar_t* labels[4] = { T(S_ROW_WORD), T(S_ROW_SEL), T(S_ROW_CASE), T(S_ROW_SET) };
        for (int i = 0; i < 4; ++i) {
            const float yy = y + i * ST_ROW;
            if (i) sep(yy);
            hoverBg(yy, ST_ROW, IT_HK0 + i, i == 0, false);
            const bool cap = g_setCapture == i;
            const std::wstring pill = cap ? std::wstring(T(S_PRESS_SHORT)) : PrettyHotkey(hk[i]);
            const float pw = TextW(g, pill, fPill) + 22;
            const float px = L + CW - 34 - pw;
            FillRound(g, cap ? c.accentsoft : c.pill, px, yy + (ST_ROW - 28) / 2, pw, 28, 6);
            if (cap) StrokeRound(g, c.accent, px + 0.5f, yy + (ST_ROW - 28) / 2 + 0.5f, pw - 1, 27, 6, 1);
            TextAt(g, pill, fPill, cap ? c.accent : c.pilltext, px + 11, yy, ST_ROW);
            DrawChevron(g, c.sub, L + CW - 22, yy + ST_ROW / 2 - 4, false);
            rowLabel(yy, ST_ROW, i == 3 ? ICO_GEAR : ICO_KEYB, labels[i], L"", px - L - 56);
            hit(L, yy, CW, ST_ROW, IT_HK0 + i);
        }
        const float yy = y + 4 * ST_ROW;
        sep(yy);
        hoverBg(yy, ST_ROW2, IT_CAPS, false, true);
        rowLabel(yy, ST_ROW2, ICO_CAPS, T(S_CHK_CAPS), T(S_CAPS_HINT), CW - 120);
        DrawToggle(g, c, L + CW - 56, yy + (ST_ROW2 - 20) / 2, g_capsSwitch != 0);
        hit(L, yy, CW, ST_ROW2, IT_CAPS);
        y += h;
    }

    // Общие
    y += 6;
    h2(T(S_SEC_GENERAL));
    {
        const bool hidden = g_trayHidden;
        const float hShow = hidden ? ST_ROW2 : ST_ROW;
        const float h = ST_ROW * 4 + hShow;
        card(y, h);
        float yy = y;
        struct Tg { int id; int icon; const wchar_t* text; bool on; };
        const Tg tgs[3] = {
            { IT_AUTO,  ICO_WIN,   T(S_CHK_AUTOSTART),  IsAutostart() },
            { IT_SOUND, ICO_SOUND, T(S_ROW_SOUND),      g_sound != 0 },
            { IT_GAMES, ICO_GAME,  T(S_CHK_FULLSCREEN), g_autoFull != 0 },
        };
        for (int i = 0; i < 3; ++i) {
            if (i) sep(yy);
            hoverBg(yy, ST_ROW, tgs[i].id, i == 0, false);
            rowLabel(yy, ST_ROW, tgs[i].icon, tgs[i].text, L"", CW - 120);
            DrawToggle(g, c, L + CW - 56, yy + (ST_ROW - 20) / 2, tgs[i].on);
            hit(L, yy, CW, ST_ROW, tgs[i].id);
            yy += ST_ROW;
        }
        // Показывать значок в трее
        sep(yy);
        hoverBg(yy, hShow, IT_SHOWICON, false, false);
        std::wstring hint;
        if (hidden) {
            Hotkey hs;
            EnterCriticalSection(&g_cs);
            hs = g_hkSet;
            LeaveCriticalSection(&g_cs);
            hint = std::wstring(T(S_HIDDEN_HINT)) + PrettyHotkey(hs);
        }
        rowLabel(yy, hShow, ICO_EYE, T(S_ROW_SHOWICON), hint, CW - 120);
        DrawToggle(g, c, L + CW - 56, yy + (hShow - 20) / 2, !hidden);
        hit(L, yy, CW, hShow, IT_SHOWICON);
        yy += hShow;
        // Язык интерфейса
        sep(yy);
        {
            const wchar_t* names[3] = { T(S_LANG_AUTO), L"English", L"Русский" };
            const std::wstring cur = names[(g_langMode >= 0 && g_langMode <= 2) ? g_langMode : 0];
            const float pw = TextW(g, cur, fPill) + 44;
            const float px = L + CW - 16 - pw, py = yy + (ST_ROW - 30) / 2;
            hoverBg(yy, ST_ROW, IT_LANG, false, true);
            FillRound(g, c.pill, px, py, pw, 30, 6);
            StrokeRound(g, c.line, px + 0.5f, py + 0.5f, pw - 1, 29, 6, 1);
            TextAt(g, cur, fPill, c.pilltext, px + 12, py, 30);
            DrawChevron(g, c.sub, px + pw - 22, py + 13, true);
            rowLabel(yy, ST_ROW, ICO_GLOBE, T(S_LBL_LANG2), L"", px - L - 56);
            hit(L, yy, CW, ST_ROW, IT_LANG);
            yy += ST_ROW;
        }
        y += h;
    }

    // Не работать в программах
    y += 6;
    h2(T(S_SEC_EXCL));
    {
        std::vector<std::wstring> ex;
        EnterCriticalSection(&g_cs);
        ex = g_excluded;
        LeaveCriticalSection(&g_cs);
        // Раскладываем «чипы» по строкам.
        const float chipH = 30, gap = 8, padX = 14, padY = 12;
        const std::wstring addText = T(S_ADD);
        const float addW = TextW(g, addText, fPill) + 16;
        struct Pos { float x, y, w; };
        std::vector<Pos> pos;
        float x = L + padX, row = 0;
        for (const std::wstring& e : ex) {
            const float w = TextW(g, e, fPill) + 12 + 26;
            if (x + w > L + CW - padX && x > L + padX) { x = L + padX; ++row; }
            pos.push_back({ x, row, w });
            x += w + gap;
        }
        if (x + addW > L + CW - padX && x > L + padX) { x = L + padX; ++row; }
        const float addX = x, addRow = row;
        const float h = padY * 2 + (row + 1) * chipH + row * gap;
        card(y, h);
        for (size_t i = 0; i < ex.size(); ++i) {
            const float cy = y + padY + pos[i].y * (chipH + gap);
            FillRound(g, c.chip, pos[i].x, cy, pos[i].w, chipH, 6);
            TextAt(g, ex[i], fPill, c.text, pos[i].x + 12, cy, chipH);
            const int id = IT_CHIP + static_cast<int>(i);
            const float xx = pos[i].x + pos[i].w - 15;
            if (g_setHover == id) {
                SolidBrush hb(c.pill);
                g.FillEllipse(&hb, xx - 9, cy + chipH / 2 - 9, 18.0f, 18.0f);
            }
            DrawCross(g, g_setHover == id ? c.text : c.sub, xx, cy + chipH / 2, 3.5f, 1.4f);
            hit(xx - 11, cy, 22, chipH, id);
        }
        const float ay = y + padY + addRow * (chipH + gap);
        if (g_setHover == IT_ADD) FillRound(g, c.accentsoft, addX, ay, addW, chipH, 6);
        TextAt(g, addText, fPill, c.accent, addX + 8, ay, chipH);
        hit(addX, ay, addW, chipH, IT_ADD);
        y += h;
    }

    // Подвал
    {
        const float fy = y + 6;
        TextAt(g, T(S_FOOT), fFoot, c.sub, L + 4, fy, ST_FOOT - 10);
        const std::wstring link = std::wstring(L"v" KS_VERSION_STR L" · GitHub");
        const float lw = TextW(g, link, fFoot);
        TextAt(g, link, fFoot, c.accent, L + CW - 4 - lw, fy, ST_FOOT - 10);
        if (g_setHover == IT_GITHUB) {
            Pen p(c.accent, 1);
            g.DrawLine(&p, L + CW - 4 - lw, fy + (ST_FOOT - 10) / 2 + 8, L + CW - 4, fy + (ST_FOOT - 10) / 2 + 8);
        }
        hit(L + CW - 8 - lw, fy, lw + 8, ST_FOOT - 10, IT_GITHUB);
        y += ST_FOOT;
    }
    g_setContent = y - top;
    g.Restore(st0);

    // Тонкая полоса прокрутки, если всё не влезло.
    const float view = H - ST_TITLE;
    if (g_setContent > view + 1) {
        const float th = std::max(30.0f, view * view / g_setContent);
        const float ty = ST_TITLE + (view - th) * (g_setScroll / (g_setContent - view));
        FillRound(g, Color(g_setDark ? 90 : 70, 128, 128, 136), W - 6, ty + 2, 3, th - 4, 1.5f);
    }

    // --- Заголовок (не прокручивается) ---
    {
        SolidBrush bg(c.win);
        g.FillRectangle(&bg, 0.0f, 0.0f, W, static_cast<float>(ST_TITLE));
        if (g_setScroll > 0.5f) {
            Pen p(c.line, 1);
            g.DrawLine(&p, 0.0f, ST_TITLE - 0.5f, W, ST_TITLE - 0.5f);
        }
        DrawStarShape(g, 22 + 15, 20 + 15, 15, c.red);
        TextAt(g, L"KeySwitch", fH1, c.text, 64, 14, 28);
        TextAt(g, T(S_SUBTITLE), fSub, c.sub, 64, 40, 20);
        // Свернуть и закрыть
        const float bw = 40, bh = 32, by = 8;
        const float cx = W - 8 - bw, mx = cx - bw;
        if (g_setHover == IT_MIN) FillRound(g, c.hover, mx, by, bw, bh, 6);
        if (g_setHover == IT_CLOSE) FillRound(g, Color(255, 196, 43, 28), cx, by, bw, bh, 6);
        {
            Pen p(c.sub, 1.2f);
            g.DrawLine(&p, mx + 15, by + bh / 2, mx + 25, by + bh / 2);
        }
        DrawCross(g, g_setHover == IT_CLOSE ? Color(255, 255, 255, 255) : c.sub, cx + bw / 2, by + bh / 2, 5, 1.2f);
        g_setHits.push_back({ RectF(mx, by, bw, bh), IT_MIN, false });
        g_setHits.push_back({ RectF(cx, by, bw, bh), IT_CLOSE, false });
    }

    delete fH1; delete fSub; delete fH2; delete fRow; delete fSmall; delete fPill; delete fCode; delete fLay;
    delete fFoot; delete fMini;
}

static float SetK() { return g_dpi / 96.0f; }

static int SettingsHitTest(int px, int py) {
    const float x = px / SetK(), y = py / SetK();
    // Заголовок перекрывает прокручиваемое содержимое.
    for (const SetHit& h : g_setHits)
        if (!h.scrolls && h.r.Contains(x, y)) return h.id;
    if (y < ST_TITLE) return IT_NONE;
    for (const SetHit& h : g_setHits)
        if (h.scrolls && h.r.Contains(x, y)) return h.id;
    return IT_NONE;
}

static void SettingsRedraw() {
    if (g_settingsWnd) InvalidateRect(g_settingsWnd, nullptr, FALSE);
}

static void ExclAddName(const std::wstring& exeOrPath) {
    const std::vector<std::wstring> parsed = ParseExclusions(exeOrPath);   // имя файла, нижний регистр, .exe
    if (parsed.empty()) return;
    EnterCriticalSection(&g_cs);
    if (std::find(g_excluded.begin(), g_excluded.end(), parsed[0]) == g_excluded.end())
        g_excluded.push_back(parsed[0]);
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_exclVersion);
    SaveConfig();
}

static void ExclRemoveAt(size_t i) {
    EnterCriticalSection(&g_cs);
    if (i < g_excluded.size()) g_excluded.erase(g_excluded.begin() + static_cast<std::ptrdiff_t>(i));
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_exclVersion);
    SaveConfig();
}

static std::vector<std::wstring> g_runningNames;   // для меню «Добавить»

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
    if (GetOpenFileNameW(&ofn)) ExclAddName(file);
}

// Меню под кнопкой «Добавить»: обзор или одна из запущенных программ.
static void ExclAddMenu(HWND wnd, POINT at) {
    g_runningNames.clear();
    EnumWindows(CollectWindowProc, 0);
    std::sort(g_runningNames.begin(), g_runningNames.end());
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, T(S_BTN_BROWSE));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g_runningNames.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(S_NO_PROGRAMS));
    } else {
        for (size_t i = 0; i < g_runningNames.size() && i < 60; ++i)
            AppendMenuW(m, MF_STRING, ID_RUNNING_FIRST + i, g_runningNames[i].c_str());
    }
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ExclBrowse(wnd);
    else if (cmd >= ID_RUNNING_FIRST && cmd < ID_RUNNING_FIRST + static_cast<int>(g_runningNames.size()))
        ExclAddName(g_runningNames[cmd - ID_RUNNING_FIRST]);
}

static void LangMenu(HWND wnd, POINT at) {
    HMENU m = CreatePopupMenu();
    const wchar_t* names[3] = { T(S_LANG_AUTO), L"English", L"Русский" };
    for (int i = 0; i < 3; ++i)
        AppendMenuW(m, MF_STRING | (g_langMode == i ? MF_CHECKED : 0), 1 + i, names[i]);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, wnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= 3) {
        g_langMode = cmd - 1;
        ApplyUiLanguage();   // меню, подсказка и окна сразу на новом языке
        SaveConfig();
        UpdateTrayTip();
        SetWindowTextW(wnd, T(S_SETTINGS_TITLE));
    }
}

// Нижний левый угол области кнопки на экране (для выпадающих меню).
static POINT HitScreenPoint(HWND wnd, int id) {
    POINT p = { 0, 0 };
    for (const SetHit& h : g_setHits)
        if (h.id == id) {
            p.x = static_cast<LONG>(h.r.X * SetK());
            p.y = static_cast<LONG>(h.r.GetBottom() * SetK());
        }
    ClientToScreen(wnd, &p);
    return p;
}

static void StopCapture() {
    InterlockedExchange(&g_capture, 0);
    g_setCapture = -1;
}

static void SettingsClick(HWND wnd, int id) {
    if (g_setCapture >= 0 && !(id >= IT_HK0 && id <= IT_HK3)) {
        StopCapture();
        SettingsRedraw();
    }
    switch (id) {
    case IT_MIN:   ShowWindow(wnd, SW_MINIMIZE); return;
    case IT_CLOSE: DestroyWindow(wnd); return;
    case IT_HK0: case IT_HK1: case IT_HK2: case IT_HK3: {
        const int i = id - IT_HK0;
        if (g_setCapture == i) { StopCapture(); }
        else { g_setCapture = i; InterlockedExchange(&g_capture, 1); }
        break;
    }
    case IT_CAPS:  InterlockedExchange(&g_capsSwitch, g_capsSwitch ? 0 : 1); SaveConfig(); break;
    case IT_SOUND: InterlockedExchange(&g_sound, g_sound ? 0 : 1); SaveConfig(); break;
    case IT_GAMES: InterlockedExchange(&g_autoFull, g_autoFull ? 0 : 1); SaveConfig(); break;
    case IT_AUTO:  SetAutostart(!IsAutostart()); break;
    case IT_SHOWICON:
        g_trayHidden = !g_trayHidden;
        SaveConfig();
        ApplyTrayVisibility();
        break;
    case IT_LANG:   LangMenu(wnd, HitScreenPoint(wnd, IT_LANG)); break;
    case IT_ADD:    ExclAddMenu(wnd, HitScreenPoint(wnd, IT_ADD)); break;
    case IT_GITHUB: ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL); return;
    default:
        if (id >= IT_LAYOUT) {
            // Клик по карточке: переключаем раскладку окна настроек (оно сейчас активное).
            const size_t i = static_cast<size_t>(id - IT_LAYOUT);
            if (i < g_setLayouts.size()) {
                ActivateKeyboardLayout(g_setLayouts[i], 0);
                AnnounceSwitch(wnd, g_setLayouts[i]);
                OnTimer();
            }
        } else if (id >= IT_CHIP) {
            ExclRemoveAt(static_cast<size_t>(id - IT_CHIP));
        }
        break;
    }
    SettingsRedraw();
}

static void SettingsScrollBy(HWND wnd, float dy) {
    RECT cr;
    GetClientRect(wnd, &cr);
    const float view = cr.bottom / SetK() - ST_TITLE;
    const float maxS = std::max(0.0f, g_setContent - view);
    g_setScroll = std::min(maxS, std::max(0.0f, g_setScroll + dy));
    SettingsRedraw();
}

static LRESULT CALLBACK SettingsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;   // вся площадь окна наша: заголовок рисуем сами
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(wnd, msg, wp, -1);   // не перерисовывать системный заголовок
    case WM_NCHITTEST: {
        POINT p = { static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        ScreenToClient(wnd, &p);
        if (p.y >= 0 && p.y < S(ST_TITLE)) {
            const int id = SettingsHitTest(p.x, p.y);
            return id == IT_NONE ? HTCAPTION : HTCLIENT;
        }
        return HTCLIENT;
    }
    case WM_CREATE:
        SetTimer(wnd, 7, 250, nullptr);   // следим за раскладкой для карточек
        return 0;
    case WM_TIMER:
        if (wp == 7 && SettingsLayoutNow() != g_setLayout) SettingsRedraw();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(wnd, &ps);
        RECT cr;
        GetClientRect(wnd, &cr);
        const int W = cr.right, H = cr.bottom;
        if (W > 0 && H > 0) {
            // Рисуем в 32-битный буфер с непрозрачной альфой: DWM показывает его как есть.
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Gdiplus::Bitmap buf(W, H, W * 4, PixelFormat32bppARGB, static_cast<BYTE*>(bits));
                Gdiplus::Graphics g(&buf);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                g.ScaleTransform(SetK(), SetK());
                SettingsPaint(g, W / SetK(), H / SetK());
            }
            DWORD* px = static_cast<DWORD*>(bits);
            for (int i = 0; i < W * H; ++i) px[i] |= 0xFF000000u;
            BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int id = SettingsHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (id != g_setHover) {
            g_setHover = id;
            SettingsRedraw();
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_setHover != IT_NONE) { g_setHover = IT_NONE; SettingsRedraw(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const bool hand = g_setHover != IT_NONE && g_setHover != IT_MIN && g_setHover != IT_CLOSE;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        const int id = SettingsHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        SettingsClick(wnd, id);
        return 0;
    }
    case WM_MOUSEWHEEL:
        SettingsScrollBy(wnd, -GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 60.0f);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(wnd); return 0; }
        if (wp == VK_DOWN || wp == VK_NEXT) { SettingsScrollBy(wnd, wp == VK_NEXT ? 300.0f : 40.0f); return 0; }
        if (wp == VK_UP || wp == VK_PRIOR)  { SettingsScrollBy(wnd, wp == VK_PRIOR ? -300.0f : -40.0f); return 0; }
        break;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            g_setDark = AppsDark();
            SettingsRedraw();
        }
        break;
    case WMU_CAPTURED: {
        const int which = g_setCapture;
        g_setCapture = -1;
        if (wp != 0 && which >= 0) {
            const Hotkey h = { static_cast<UINT>(wp), static_cast<UINT>(lp) };
            bool taken = false;
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < 4; ++i) {
                const Hotkey* o = HotkeySlot(i);
                if (i != which && o->vk == h.vk && o->mods == h.mods) taken = true;
            }
            LeaveCriticalSection(&g_cs);
            if (h.vk == VK_CAPITAL) {
                MessageBoxW(wnd, T(S_ERR_CAPS), APP_NAME, MB_ICONWARNING);
            } else if (IsCharKey(h.vk) && !(h.mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
                MessageBoxW(wnd, T(S_ERR_TEXTKEY), APP_NAME, MB_ICONWARNING);
            } else if (taken) {
                MessageBoxW(wnd, T(S_ERR_TAKEN), APP_NAME, MB_ICONWARNING);
            } else {
                EnterCriticalSection(&g_cs);
                *HotkeySlot(which) = h;
                LeaveCriticalSection(&g_cs);
                SaveConfig();
                UpdateTrayTip();
            }
        }
        SettingsRedraw();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        KillTimer(wnd, 7);
        StopCapture();
        g_setHover = IT_NONE;
        g_settingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Окно без системной рамки: скруглённые углы (Windows 11), тёмная рамка в тёмной теме и тень.
static void ApplyModernFrame(HWND w, bool dark) {
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttrFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        using ExtendFn = HRESULT (WINAPI*)(HWND, const MARGINS*);
        if (auto setAttr = reinterpret_cast<SetAttrFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
            const DWORD round = 2;   // DWMWCP_ROUND
            setAttr(w, 33, &round, sizeof(round));
            const BOOL d = dark;
            setAttr(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
        }
        if (auto extend = reinterpret_cast<ExtendFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea")))) {
            const MARGINS m = { 0, 0, 0, 1 };
            extend(w, &m);
        }
    }
    SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

// Отрисовка окна через 32-битный буфер с непрозрачной альфой (DWM показывает его как есть).
template <class F> static void PaintModern(HWND wnd, F paint) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(wnd, &ps);
    RECT cr;
    GetClientRect(wnd, &cr);
    const int W = cr.right, H = cr.bottom;
    if (W > 0 && H > 0) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Gdiplus::Bitmap buf(W, H, W * 4, PixelFormat32bppARGB, static_cast<BYTE*>(bits));
            Gdiplus::Graphics g(&buf);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            const float k = g_dpi / 96.0f;
            g.ScaleTransform(k, k);
            paint(g, W / k, H / k);
        }
        DWORD* px = static_cast<DWORD*>(bits);
        for (int i = 0; i < W * H; ++i) px[i] |= 0xFF000000u;
        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(wnd, &ps);
}

static void OpenSettings() {
    if (g_settingsWnd) {
        if (IsIconic(g_settingsWnd)) ShowWindow(g_settingsWnd, SW_RESTORE);
        ForceForeground(g_settingsWnd);
        return;
    }
    g_setDark = AppsDark();
    g_setScroll = 0;
    g_setCapture = -1;
    g_setHover = IT_NONE;

    // Высота по содержимому: меряем пробной отрисовкой.
    {
        Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&probe);
        SettingsPaint(g, ST_W, 4000);
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = S(ST_W);
    const int h = std::min(static_cast<int>((ST_TITLE + g_setContent) * SetK() + 0.5f),
                           static_cast<int>(wa.bottom - wa.top) - S(24));
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_settingsWnd = CreateWindowExW(WS_EX_APPWINDOW, L"KeySwitchSettings", T(S_SETTINGS_TITLE), style,
                                    wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                                    w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyModernFrame(g_settingsWnd, g_setDark);
    SetWindowPos(g_settingsWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(g_settingsWnd, SW_SHOW);
    ForceForeground(g_settingsWnd);
}

// ---------------------------------------------------------------------------
// О программе
// ---------------------------------------------------------------------------

enum { AB_W = 380, AB_H = 372 };
enum { AB_NONE = 0, AB_CLOSE, AB_CODE, AB_CHAT };

static HWND g_aboutWnd = nullptr;
static int  g_aboutHover = AB_NONE;
static std::vector<SetHit> g_aboutHits;
static const wchar_t* const TELEGRAM_URL = L"https://t.me/NSMRK";

static void AboutPaint(Gdiplus::Graphics& g, float W, float H) {
    using namespace Gdiplus;
    (void)H;
    const SetColors c = SetTheme();
    g_aboutHits.clear();
    g.Clear(c.win);
    StringFormat center(StringFormat::GenericTypographic());
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentNear);
    Font* fTitle = SetFont(24, false, true);
    Font* fSub   = SetFont(13);
    Font* fBody  = SetFont(14);
    Font* fRow   = SetFont(14);
    Font* fSmall = SetFont(12);

    // Закрыть
    {
        const float bw = 40, bh = 32, bx = W - 8 - bw, by = 8;
        if (g_aboutHover == AB_CLOSE) FillRound(g, Color(255, 196, 43, 28), bx, by, bw, bh, 6);
        DrawCross(g, g_aboutHover == AB_CLOSE ? Color(255, 255, 255, 255) : c.sub, bx + bw / 2, by + bh / 2, 5, 1.2f);
        g_aboutHits.push_back({ RectF(bx, by, bw, bh), AB_CLOSE, false });
    }
    // Звезда, название, версия
    DrawStarShape(g, W / 2, 56, 28, c.red);
    SolidBrush tb(c.text), sb(c.sub);
    g.DrawString(L"KeySwitch", -1, fTitle, RectF(0, 96, W, 34), &center, &tb);
    const std::wstring ver = std::wstring(T(S_ABOUT_VERSION)) + (L"" KS_VERSION_STR) + L" \u00B7 " + (L"" KS_BUILD_STR);
    g.DrawString(ver.c_str(), -1, fSub, RectF(0, 130, W, 20), &center, &sb);
    // Описание
    StringFormat wrap;
    wrap.SetAlignment(StringAlignmentCenter);
    g.DrawString(T(S_ABOUT_BODY), -1, fBody, RectF(36, 164, W - 72, 44), &wrap, &tb);

    // Карточка со ссылками
    const float L = 18, CW = W - 36, y0 = 220, rh = 54;
    FillRound(g, c.card, L, y0, CW, rh * 2, 10);
    StrokeRound(g, c.line, L + 0.5f, y0 + 0.5f, CW - 1, rh * 2 - 1, 10, 1);
    struct Row { int id; int icon; const wchar_t* text; const wchar_t* sub; };
    const Row rows[2] = {
        { AB_CODE, ICO_CODE, T(S_ABOUT_CODE),  L"github.com/NSMRK/keyswitch" },
        { AB_CHAT, ICO_CHAT, T(S_ABOUT_CHAT), L"Telegram · @NSMRK" },
    };
    for (int i = 0; i < 2; ++i) {
        const float y = y0 + i * rh;
        if (i) { Pen p(c.line, 1); g.DrawLine(&p, L + 1, y, L + CW - 1, y); }
        if (g_aboutHover == rows[i].id) {
            GraphicsPath p;
            const float r = 9, d = 2 * r;
            RectF rc(L + 1, y + (i ? 0.5f : 1.0f), CW - 2, rh - 1.5f);
            if (i == 0) {
                p.AddArc(rc.X, rc.Y, d, d, 180, 90); p.AddArc(rc.GetRight() - d, rc.Y, d, d, 270, 90);
                p.AddLine(rc.GetRight(), rc.GetBottom(), rc.X, rc.GetBottom());
            } else {
                p.AddLine(rc.X, rc.Y, rc.GetRight(), rc.Y);
                p.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0, 90); p.AddArc(rc.X, rc.GetBottom() - d, d, d, 90, 90);
            }
            p.CloseFigure();
            SolidBrush hb(c.hover);
            g.FillPath(&hb, &p);
        }
        DrawRowIcon(g, rows[i].icon, L + 16, y + (rh - 18) / 2, c.sub);
        TextAt(g, rows[i].text, fRow, c.text, L + 46, y + 8, 20, CW - 90);
        TextAt(g, rows[i].sub, fSmall, c.accent, L + 46, y + 28, 18, CW - 90);
        DrawChevron(g, c.sub, L + CW - 22, y + rh / 2 - 4, false);
        g_aboutHits.push_back({ RectF(L, y, CW, rh), rows[i].id, false });
    }
    // Подвал
    g.DrawString(L"by NSMRK · © 2026 · MIT", -1, fSmall, RectF(0, 342, W, 18), &center, &sb);
    delete fTitle; delete fSub; delete fBody; delete fRow; delete fSmall;
}

static int AboutHitTest(int px, int py) {
    const float k = g_dpi / 96.0f;
    for (const SetHit& h : g_aboutHits)
        if (h.r.Contains(px / k, py / k)) return h.id;
    return AB_NONE;
}

static LRESULT CALLBACK AboutProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(wnd, msg, wp, -1);
    case WM_NCHITTEST: {
        POINT p = { static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
        ScreenToClient(wnd, &p);
        return AboutHitTest(p.x, p.y) == AB_NONE ? HTCAPTION : HTCLIENT;   // окно тянется за любое пустое место
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintModern(wnd, [](Gdiplus::Graphics& g, float W, float H) { AboutPaint(g, W, H); });
        return 0;
    case WM_MOUSEMOVE: {
        const int id = AboutHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (id != g_aboutHover) { g_aboutHover = id; InvalidateRect(wnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_aboutHover != AB_NONE) { g_aboutHover = AB_NONE; InvalidateRect(wnd, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_aboutHover == AB_CODE || g_aboutHover == AB_CHAT ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        switch (AboutHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)))) {
        case AB_CLOSE: DestroyWindow(wnd); break;
        case AB_CODE:  ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
        case AB_CHAT:  ShellExecuteW(nullptr, L"open", TELEGRAM_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN) { DestroyWindow(wnd); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        g_aboutHover = AB_NONE;
        g_aboutWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void ShowAbout() {
    if (g_aboutWnd) {
        ForceForeground(g_aboutWnd);
        return;
    }
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW ac = {};
        ac.cbSize = sizeof(ac);
        ac.lpfnWndProc = AboutProc;
        ac.hInstance = hi;
        ac.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        ac.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        ac.hIconSm = ac.hIcon;
        ac.lpszClassName = L"KeySwitchAbout";
        RegisterClassExW(&ac);
        registered = true;
    }
    g_setDark = AppsDark();
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = S(AB_W), h = S(AB_H);
    g_aboutWnd = CreateWindowExW(WS_EX_APPWINDOW, L"KeySwitchAbout", T(S_ABOUT_TITLE), WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                                 w, h, nullptr, nullptr, hi, nullptr);
    ApplyModernFrame(g_aboutWnd, g_setDark);
    ShowWindow(g_aboutWnd, SW_SHOW);
    ForceForeground(g_aboutWnd);
}

// ---------------------------------------------------------------------------
// Панель раскладок над значком в трее (наведение или клик)
// ---------------------------------------------------------------------------

static HWND g_flyWnd = nullptr;
static std::vector<HKL> g_flyLayouts;
static HKL  g_flyCurrent = nullptr;
static bool g_flyOff = false;           // KeySwitch сейчас отключён в этом окне
static bool g_flyByClick = false;
static bool g_flyRounded = false;       // Windows 11 скруглила углы и сама рисует рамку
static int  g_flyHover = -1;            // строка под мышью; 1000: ссылка «Настройки»
static int  g_flyOutMs = 0;             // сколько мышь уже вне панели и значка
static DWORD g_lastDblClick = 0;       // время последнего двойного клика по значку
static RECT g_flyLinkRect = {};

// Размеры в логических пикселях (умножаются на DPI через S()).
enum { FLY_W = 300, FLY_PAD = 8, FLY_HEAD = 44, FLY_ROW = 56, FLY_SEP = 9, FLY_FOOT = 40 };

struct FlyColors {
    Gdiplus::Color bg, text, sub, sel, hover, line, accent;
};

static FlyColors FlyTheme() {
    using Gdiplus::Color;
    if (g_darkTaskbar)
        return { Color(255, 40, 40, 44), Color(255, 242, 242, 244), Color(255, 154, 156, 165),
                 Color(255, 52, 52, 58), Color(255, 48, 48, 53), Color(255, 58, 59, 64), Color(255, 76, 194, 255) };
    return { Color(255, 243, 243, 245), Color(255, 27, 27, 31), Color(255, 107, 111, 122),
             Color(255, 233, 233, 237), Color(255, 236, 236, 240), Color(255, 226, 227, 231), Color(255, 0, 103, 192) };
}

static int FlyHeight() {
    return S(FLY_PAD + FLY_HEAD + FLY_SEP + FLY_FOOT + FLY_PAD) + S(FLY_ROW) * static_cast<int>(g_flyLayouts.size());
}

static int FlyRowTop(int i) { return S(FLY_PAD + FLY_HEAD) + S(FLY_ROW) * i; }

static bool GetTrayIconRect(RECT& rc) {
    NOTIFYICONIDENTIFIER id = {};
    id.cbSize = sizeof(id);
    id.hWnd = g_mainWnd;
    id.uID = 1;
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rc));
}

static void FlyPaint(HDC hdc) {
    using namespace Gdiplus;
    RECT cr;
    GetClientRect(g_flyWnd, &cr);
    const int W = cr.right, H = cr.bottom;
    const FlyColors c = FlyTheme();
    const bool dark = g_darkTaskbar;

    Bitmap buf(W, H, PixelFormat32bppARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(c.bg);
    if (!g_flyRounded) {   // Windows 10: рисуем рамку сами
        Pen border(c.line, 1);
        g.DrawRectangle(&border, 0.0f, 0.0f, W - 1.0f, H - 1.0f);
    }

    Font* fTitle = MakeFont(L"Segoe UI Semibold", static_cast<float>(S(14)), FontStyleRegular);
    Font* fName  = MakeFont(L"Segoe UI", static_cast<float>(S(14)), FontStyleRegular);
    Font* fSub   = MakeFont(L"Segoe UI", static_cast<float>(S(12)), FontStyleRegular);
    Font* fSmall = MakeFont(L"Segoe UI", static_cast<float>(S(12)), FontStyleRegular);
    Font* fBold  = MakeFont(L"Segoe UI Semibold", static_cast<float>(S(12)), FontStyleRegular);
    Font* fLink  = MakeFont(L"Segoe UI Semibold", static_cast<float>(S(12)),
                            g_flyHover == 1000 ? FontStyleUnderline : FontStyleRegular);
    SolidBrush bText(c.text), bSub(c.sub), bAcc(c.accent);
    StringFormat left;
    left.SetLineAlignment(StringAlignmentCenter);
    left.SetFormatFlags(StringFormatFlagsNoWrap);
    left.SetTrimming(StringTrimmingEllipsisCharacter);

    // Заголовок и справа подсказка: чем переключать или «Отключено».
    const float headY = static_cast<float>(S(FLY_PAD)), headH = static_cast<float>(S(FLY_HEAD));
    g.DrawString(T(S_FLY_TITLE), -1, fTitle, RectF(static_cast<float>(S(20)), headY, W / 2.0f, headH), &left, &bText);
    const wchar_t* pill = g_flyOff ? T(S_FLY_OFF) : (g_capsSwitch ? L"Caps Lock" : nullptr);
    if (pill) {
        RectF tb;
        g.MeasureString(pill, -1, fSmall, PointF(0, 0), &tb);
        const float pw = tb.Width + S(10), ph = static_cast<float>(S(22));
        const float px = W - S(16) - pw, py = headY + (headH - ph) / 2;
        GraphicsPath pp;
        AddRoundRect(pp, RectF(px, py, pw, ph), static_cast<float>(S(4)));
        Pen pen(c.line, 1);
        g.DrawPath(&pen, &pp);
        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(pill, -1, fSmall, RectF(px, py, pw, ph), &center, &bSub);
    }

    // Строки раскладок.
    for (size_t i = 0; i < g_flyLayouts.size(); ++i) {
        const HKL h = g_flyLayouts[i];
        const bool cur = (h == g_flyCurrent);
        const float top = static_cast<float>(FlyRowTop(static_cast<int>(i)));
        const float rh = static_cast<float>(S(FLY_ROW));
        RectF row(static_cast<float>(S(FLY_PAD)), top + S(2), W - 2.0f * S(FLY_PAD), rh - S(4));
        if (cur || g_flyHover == static_cast<int>(i)) {
            GraphicsPath rp;
            AddRoundRect(rp, row, static_cast<float>(S(6)));
            SolidBrush rb(cur ? c.sel : c.hover);
            g.FillPath(&rb, &rp);
        }
        if (cur) {
            GraphicsPath bar;
            AddRoundRect(bar, RectF(row.X, top + rh / 2 - SF(9), SF(3), SF(18)), SF(1.5f));
            g.FillPath(&bAcc, &bar);
        }
        const float bs = static_cast<float>(S(28));
        const int kind = g_flyOff ? 2 : (IsEnglish(h) ? 0 : 1);
        DrawBadge(g, row.X + S(16), top + (rh - bs) / 2, bs, LayoutCode(h), kind == 2 ? (IsEnglish(h) ? 0 : 1) : kind, dark);
        const float tx = row.X + S(16) + bs + S(14);
        const std::wstring name = LocaleInfo(h, LOCALE_SNATIVELANGUAGENAME);
        const std::wstring country = LocaleInfo(h, LOCALE_SNATIVECOUNTRYNAME);
        g.DrawString(name.empty() ? LayoutCode(h).c_str() : name.c_str(), -1, fName,
                     RectF(tx, top + rh / 2 - S(20), W - tx - S(40), static_cast<float>(S(20))), &left, &bText);
        g.DrawString(country.c_str(), -1, fSub,
                     RectF(tx, top + rh / 2 + S(1), W - tx - S(40), static_cast<float>(S(18))), &left, &bSub);
        if (cur) {   // галочка
            Pen ck(c.accent, SF(1.6f));
            ck.SetLineJoin(LineJoinRound);
            ck.SetStartCap(LineCapRound);
            ck.SetEndCap(LineCapRound);
            const float cx = W - SF(32), cy = top + rh / 2;
            PointF pts[3] = { PointF(cx - SF(5), cy + SF(0.5f)), PointF(cx - SF(1.5f), cy + SF(4)),
                              PointF(cx + SF(5.5f), cy - SF(4)) };
            g.DrawLines(&ck, pts, 3);
        }
    }

    // Разделитель и нижняя строка.
    const float sepY = static_cast<float>(FlyRowTop(static_cast<int>(g_flyLayouts.size())) + S(FLY_SEP) / 2);
    Pen line(c.line, 1);
    g.DrawLine(&line, static_cast<float>(S(12)), sepY, W - static_cast<float>(S(12)), sepY);
    const float footY = static_cast<float>(FlyRowTop(static_cast<int>(g_flyLayouts.size())) + S(FLY_SEP));
    const float footH = static_cast<float>(S(FLY_FOOT));
    Hotkey hw;
    EnterCriticalSection(&g_cs);
    hw = g_hkWord;
    LeaveCriticalSection(&g_cs);
    RectF lb;
    g.MeasureString(T(S_FLY_FIX), -1, fSmall, PointF(0, 0), &lb);
    g.DrawString(T(S_FLY_FIX), -1, fSmall, RectF(static_cast<float>(S(20)), footY, lb.Width, footH), &left, &bSub);
    g.DrawString(HotkeyText(hw).c_str(), -1, fBold,
                 RectF(S(20) + lb.Width - S(3), footY, W / 2.0f, footH), &left, &bText);
    RectF kb;
    g.MeasureString(T(S_FLY_SETTINGS), -1, fLink, PointF(0, 0), &kb);
    const float lx = W - S(18) - kb.Width;
    g.DrawString(T(S_FLY_SETTINGS), -1, fLink, RectF(lx, footY, kb.Width + 2, footH), &left, &bAcc);
    g_flyLinkRect = { static_cast<LONG>(lx - S(6)), static_cast<LONG>(footY + S(6)),
                      static_cast<LONG>(W - S(10)), static_cast<LONG>(footY + footH - S(6)) };

    delete fTitle; delete fName; delete fSub; delete fSmall; delete fBold; delete fLink;
    Graphics screen(hdc);
    screen.DrawImage(&buf, 0, 0);
}

static int FlyHitTest(int x, int y) {
    POINT pt = { x, y };
    if (PtInRect(&g_flyLinkRect, pt)) return 1000;
    for (size_t i = 0; i < g_flyLayouts.size(); ++i) {
        const int top = FlyRowTop(static_cast<int>(i));
        if (y >= top && y < top + S(FLY_ROW) && x >= S(FLY_PAD) && x < S(FLY_W - FLY_PAD))
            return static_cast<int>(i);
    }
    return -1;
}

static void HideFlyout() {
    if (!g_flyWnd) return;
    ShowWindow(g_flyWnd, SW_HIDE);
    InterlockedExchange(&g_flyVisible, 0);
    KillTimer(g_mainWnd, 2);
    g_flyHover = -1;
}

static void ShowFlyout(bool byClick) {
    if (!g_flyWnd) return;
    if (g_flyVisible) {
        if (byClick && g_flyByClick) { HideFlyout(); return; }   // повторный клик закрывает
        g_flyByClick = g_flyByClick || byClick;
        return;
    }
    g_flyByClick = byClick;
    g_flyLayouts = Layouts();
    HWND target = g_lastExtHwnd;
    HWND fg = GetForegroundWindow();
    if (fg && !IsShellOrOwnWindow(fg)) target = fg;
    g_flyCurrent = target ? WindowLayout(target) : nullptr;
    g_flyOff = g_paused || (g_autoFull && g_fullscreen) || (target && IsExcludedName(ProcessName(target)));
    g_flyHover = -1;

    const int w = S(FLY_W), h = FlyHeight(), gap = S(12);
    RECT ir;
    if (!GetTrayIconRect(ir)) {
        POINT pt;
        GetCursorPos(&pt);
        ir = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
    }
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&ir, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    int x, y;
    if (ir.top >= wa.bottom - 2) {            // панель задач снизу
        x = (ir.left + ir.right) / 2 - w / 2;
        y = wa.bottom - h - gap;
    } else if (ir.bottom <= wa.top + 2) {     // сверху
        x = (ir.left + ir.right) / 2 - w / 2;
        y = wa.top + gap;
    } else if (ir.left >= wa.right - 2) {     // справа
        x = wa.right - w - gap;
        y = (ir.top + ir.bottom) / 2 - h / 2;
    } else {                                  // слева (или значок в скрытой области)
        x = (ir.left + ir.right) / 2 - w / 2;
        y = ir.top - h - gap;
        if (ir.left <= wa.left + 2 && ir.right <= wa.left + 80) { x = wa.left + gap; y = (ir.top + ir.bottom) / 2 - h / 2; }
    }
    x = std::max(static_cast<int>(wa.left) + gap, std::min(x, static_cast<int>(wa.right) - w - gap));
    y = std::max(static_cast<int>(wa.top) + gap, std::min(y, static_cast<int>(wa.bottom) - h - gap));

    SetWindowPos(g_flyWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_flyWnd, nullptr, FALSE);
    InterlockedExchange(&g_flyVisible, 1);
    g_flyOutMs = 0;
    SetTimer(g_mainWnd, 2, 100, nullptr);
}

// Таймер панели: закрываем, когда мышь ушла и с панели, и со значка.
static void FlyTick() {
    if (!g_flyVisible) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT wr, ir;
    GetWindowRect(g_flyWnd, &wr);
    InflateRect(&wr, S(6), S(6));
    const bool inside = PtInRect(&wr, pt) || (GetTrayIconRect(ir) && PtInRect(&ir, pt));
    g_flyOutMs = inside ? 0 : g_flyOutMs + 100;
    if (g_flyOutMs >= (g_flyByClick ? 1200 : 400)) HideFlyout();
}

static void FlyOutsideClick(int x, int y) {
    if (!g_flyVisible) return;
    POINT pt = { x, y };
    RECT wr, ir;
    GetWindowRect(g_flyWnd, &wr);
    if (PtInRect(&wr, pt)) return;
    if (GetTrayIconRect(ir) && PtInRect(&ir, pt)) return;   // клик по значку обработает NIN_SELECT
    HideFlyout();
}

// Выбор раскладки в панели: переключаем её в окне, где пользователь работал.
static void FlySelect(int i) {
    if (i < 0 || i >= static_cast<int>(g_flyLayouts.size())) return;
    const HKL hkl = g_flyLayouts[i];
    HideFlyout();
    HWND target = g_lastExtHwnd;
    HWND fg = GetForegroundWindow();
    if (fg && !IsShellOrOwnWindow(fg)) target = fg;
    if (!target || !IsWindow(target)) return;
    // Сначала возвращаем фокус рабочему окну: раскладку принимает только активное окно.
    if (GetForegroundWindow() != target) {
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        ForceForeground(target);
        for (int t = 0; t < 300 && GetForegroundWindow() != target; t += 10) Sleep(10);
    }
    SwitchLayout(target, hkl);
    if (!WaitForLayout(target, hkl, 300)) {   // не успело: просим ещё раз
        SwitchLayout(target, hkl);
        WaitForLayout(target, hkl, 300);
    }
    AnnounceSwitch(target, hkl);
    ResetBuffer();
}

static void OpenSettings();

static LRESULT CALLBACK FlyProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // панель не забирает фокус у рабочего окна
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        FlyPaint(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int hit = FlyHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (hit != g_flyHover) {
            g_flyHover = hit;
            InvalidateRect(wnd, nullptr, FALSE);
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        g_flyOutMs = 0;
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, g_flyHover >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSELEAVE:
        if (g_flyHover != -1) { g_flyHover = -1; InvalidateRect(wnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const int hit = FlyHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (hit == 1000) { HideFlyout(); OpenSettings(); }
        else if (hit >= 0) FlySelect(hit);
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void CreateFlyout(HINSTANCE hi) {
    WNDCLASSEXW fc = {};
    fc.cbSize = sizeof(fc);
    fc.style = CS_DROPSHADOW;
    fc.lpfnWndProc = FlyProc;
    fc.hInstance = hi;
    fc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    fc.lpszClassName = L"KeySwitchFlyout";
    RegisterClassExW(&fc);
    g_flyWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"KeySwitchFlyout", APP_NAME,
                               WS_POPUP, 0, 0, S(FLY_W), S(200), nullptr, nullptr, hi, nullptr);
    // Windows 11: скруглённые углы, системная тень и рамка.
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttrFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto setAttr = reinterpret_cast<SetAttrFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
            const DWORD round = 2;   // DWMWCP_ROUND
            g_flyRounded = SUCCEEDED(setAttr(g_flyWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round)));
        }
    }
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

// ---------------------------------------------------------------------------
// Меню по правому клику на значке: нарисовано в стиле панели раскладок
// ---------------------------------------------------------------------------

enum { MENU_W = 260, MENU_PAD = 6, MENU_ROW = 36, MENU_SEP = 9 };

struct MenuItem { int id; int icon; std::wstring text; bool toggle; bool on; };

static HWND g_menuWnd = nullptr;
static std::vector<MenuItem> g_menuItems;   // id 0: разделитель
static int  g_menuHover = -1;
static bool g_menuRounded = false;
static int  g_menuW = MENU_W;               // ширина в логических px

static int MenuHeight() {
    int h = MENU_PAD * 2;
    for (const MenuItem& it : g_menuItems) h += it.id ? MENU_ROW : MENU_SEP;
    return h;
}

static int MenuHitTest(int px, int py) {
    const float k = g_dpi / 96.0f;
    const float x = px / k, y = py / k;
    if (x < MENU_PAD || x > g_menuW - MENU_PAD) return -1;
    float yy = MENU_PAD;
    for (size_t i = 0; i < g_menuItems.size(); ++i) {
        const float h = g_menuItems[i].id ? static_cast<float>(MENU_ROW) : static_cast<float>(MENU_SEP);
        if (y >= yy && y < yy + h) return g_menuItems[i].id ? static_cast<int>(i) : -1;
        yy += h;
    }
    return -1;
}

static void MenuPaint(HDC hdc) {
    using namespace Gdiplus;
    RECT cr;
    GetClientRect(g_menuWnd, &cr);
    const int W = cr.right, H = cr.bottom;
    const FlyColors c = FlyTheme();
    const float k = g_dpi / 96.0f;
    Bitmap buf(W, H, PixelFormat32bppARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(c.bg);
    g.ScaleTransform(k, k);
    if (!g_menuRounded) {   // Windows 10: рамку рисуем сами
        Pen border(c.line, 1.0f / k);
        g.DrawRectangle(&border, 0.0f, 0.0f, W / k - 1.0f / k, H / k - 1.0f / k);
    }
    Font* f = SetFont(13.5f);
    const Color accent = c.accent;
    float y = MENU_PAD;
    for (size_t i = 0; i < g_menuItems.size(); ++i) {
        const MenuItem& it = g_menuItems[i];
        if (!it.id) {
            Pen p(c.line, 1);
            g.DrawLine(&p, MENU_PAD + 10.0f, y + MENU_SEP / 2.0f, g_menuW - MENU_PAD - 10.0f, y + MENU_SEP / 2.0f);
            y += MENU_SEP;
            continue;
        }
        if (static_cast<int>(i) == g_menuHover)
            FillRound(g, c.hover, MENU_PAD, y + 1, g_menuW - 2.0f * MENU_PAD, MENU_ROW - 2.0f, 6);
        DrawRowIcon(g, it.icon, MENU_PAD + 12.0f, y + (MENU_ROW - 18) / 2.0f, c.sub);
        TextAt(g, it.text, f, c.text, MENU_PAD + 42.0f, y, static_cast<float>(MENU_ROW),
               g_menuW - 2.0f * MENU_PAD - 42 - (it.toggle ? 56 : 4));
        if (it.toggle) {
            const float tx = g_menuW - MENU_PAD - 12.0f - 36, ty = y + (MENU_ROW - 18) / 2.0f;
            if (it.on) {
                FillRound(g, accent, tx, ty, 36, 18, 9);
                SolidBrush kb(g_darkTaskbar ? Color(255, 27, 27, 31) : Color(255, 255, 255, 255));
                g.FillEllipse(&kb, tx + 21.0f, ty + 3.5f, 11.0f, 11.0f);
            } else {
                StrokeRound(g, c.sub, tx + 0.75f, ty + 0.75f, 34.5f, 16.5f, 8.25f, 1.5f);
                SolidBrush kb(c.sub);
                g.FillEllipse(&kb, tx + 4.5f, ty + 4.5f, 9.0f, 9.0f);
            }
        }
        y += MENU_ROW;
    }
    delete f;
    Graphics screen(hdc);
    screen.DrawImage(&buf, 0, 0);
}

static void HideTrayMenu() {
    if (g_menuWnd && IsWindowVisible(g_menuWnd)) ShowWindow(g_menuWnd, SW_HIDE);
    g_menuHover = -1;
}

static void MenuChoose(int i) {
    if (i < 0 || i >= static_cast<int>(g_menuItems.size()) || !g_menuItems[i].id) return;
    const int id = g_menuItems[i].id;
    HideTrayMenu();
    PostMessageW(g_mainWnd, WM_COMMAND, static_cast<WPARAM>(id), 0);
}

static void MenuMoveHover(int dir) {
    const int n = static_cast<int>(g_menuItems.size());
    if (!n) return;
    int i = g_menuHover;
    for (int step = 0; step < n; ++step) {
        i = (i + dir + n) % n;
        if (i < 0) i = n - 1;
        if (g_menuItems[i].id) break;
    }
    g_menuHover = i;
    InvalidateRect(g_menuWnd, nullptr, FALSE);
}

static LRESULT CALLBACK MenuProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) HideTrayMenu();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        MenuPaint(dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int hit = MenuHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        if (hit != g_menuHover) { g_menuHover = hit; InvalidateRect(wnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_menuHover != -1) { g_menuHover = -1; InvalidateRect(wnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        MenuChoose(MenuHitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { HideTrayMenu(); return 0; }
        if (wp == VK_DOWN) { MenuMoveHover(1); return 0; }
        if (wp == VK_UP)   { MenuMoveHover(-1); return 0; }
        if (wp == VK_RETURN || wp == VK_SPACE) { MenuChoose(g_menuHover); return 0; }
        break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static void CreateTrayMenu(HINSTANCE hi) {
    WNDCLASSEXW mc = {};
    mc.cbSize = sizeof(mc);
    mc.style = CS_DROPSHADOW;
    mc.lpfnWndProc = MenuProc;
    mc.hInstance = hi;
    mc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mc.lpszClassName = L"KeySwitchMenu";
    RegisterClassExW(&mc);
    g_menuWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"KeySwitchMenu", APP_NAME, WS_POPUP,
                                0, 0, S(MENU_W), S(200), nullptr, nullptr, hi, nullptr);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttrFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto setAttr = reinterpret_cast<SetAttrFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
            const DWORD round = 3;   // DWMWCP_ROUNDSMALL, как у системных меню
            g_menuRounded = SUCCEEDED(setAttr(g_menuWnd, 33, &round, sizeof(round)));
        }
    }
}

static void ShowTrayMenu() {
    g_menuItems.clear();
    if (!g_lastExtExe.empty()) {
        const bool ex = IsExcludedName(g_lastExtExe);
        g_menuItems.push_back({ ID_MENU_EXCLUDE, ICO_BLOCK,
                                std::wstring(T(ex ? S_MENU_EXCL_ON : S_MENU_EXCL_OFF)) + T(S_QUOTE_OPEN) +
                                    g_lastExtExe + T(S_QUOTE_CLOSE), false, false });
    }
    g_menuItems.push_back({ ID_MENU_PAUSE, ICO_PAUSE, T(S_MENU_PAUSE), true, g_paused != 0 });
    g_menuItems.push_back({ 0, 0, L"", false, false });
    g_menuItems.push_back({ ID_MENU_SETTINGS, ICO_GEAR, T(S_MENU_SETTINGS), false, false });
    g_menuItems.push_back({ ID_MENU_ABOUT, ICO_INFO, T(S_MENU_ABOUT), false, false });
    g_menuItems.push_back({ 0, 0, L"", false, false });
    g_menuItems.push_back({ ID_MENU_EXIT, ICO_POWER, T(S_MENU_EXIT), false, false });

    // Ширина по самому длинному пункту.
    {
        Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&probe);
        Gdiplus::Font* f = SetFont(13.5f);
        float maxW = 0;
        for (const MenuItem& it : g_menuItems)
            if (it.id) maxW = std::max(maxW, TextW(g, it.text, f) + (it.toggle ? 64.0f : 20.0f));
        delete f;
        g_menuW = std::max(220, std::min(420, static_cast<int>(MENU_PAD * 2 + 42 + maxW + 8)));
    }
    g_menuHover = -1;
    const int w = S(g_menuW), h = S(MenuHeight());
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    int x = pt.x - w, y = pt.y - h;   // над курсором, слева от него: как системные меню трея
    if (x < wa.left) x = pt.x;
    if (y < wa.top) y = pt.y;
    x = std::max(static_cast<int>(wa.left), std::min(x, static_cast<int>(wa.right) - w));
    y = std::max(static_cast<int>(wa.top), std::min(y, static_cast<int>(wa.bottom) - h));
    SetWindowPos(g_menuWnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    InvalidateRect(g_menuWnd, nullptr, FALSE);
    ForceForeground(g_menuWnd);
    SetFocus(g_menuWnd);
}

static LRESULT CALLBACK MainProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WMU_WORD: ConvertLastWord();     return 0;
    case WMU_SEL:  ConvertSelection();    return 0;
    case WMU_CASE: ChangeSelectionCase(); return 0;
    case WMU_OPENSET: HideFlyout(); OpenSettings(); return 0;
    case WMU_CAPS: SwitchLayoutByCaps();  return 0;
    case WM_TIMER:
        if (wp == 2) FlyTick();
        else         OnTimer();
        return 0;
    case WMU_FLYHIDE:
        HideFlyout();
        return 0;
    case WMU_FLYCLICK:
        FlyOutsideClick(static_cast<int>(wp), static_cast<int>(lp));
        return 0;
    case WM_SETTINGCHANGE:
        // Пользователь переключил светлую/тёмную тему: перерисовываем значок.
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            g_darkTaskbar = !IsLightTaskbar();
            g_trayKey.clear();
            OnTimer();
        }
        break;
    case WMU_TRAY:
        switch (LOWORD(lp)) {
        case NIN_POPUPOPEN:          // мышь задержалась на значке
            ShowFlyout(false);
            break;
        case NIN_SELECT:             // клик по значку
        case NIN_KEYSELECT:
            // Второй щелчок двойного клика не должен снова открывать панель поверх настроек.
            if (GetTickCount() - g_lastDblClick > GetDoubleClickTime()) ShowFlyout(true);
            break;
        case WM_CONTEXTMENU:
            HideFlyout();
            ShowTrayMenu();
            break;
        case WM_LBUTTONDBLCLK:
            g_lastDblClick = GetTickCount();
            HideFlyout();
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
        KillTimer(wnd, 2);
        if (g_flyWnd) DestroyWindow(g_flyWnd);
        if (g_menuWnd) DestroyWindow(g_menuWnd);
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
        // Уже запущен: открываем настройки в работающей копии (важно, когда значок в трее скрыт).
        HWND other = FindWindowW(L"KeySwitchMain", nullptr);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WMU_OPENSET, 0, 0);
        } else {
            MessageBoxW(nullptr, T(S_ALREADY_RUNNING), APP_NAME, MB_ICONINFORMATION);
        }
        return 0;
    }

    SetProcessDPIAware();
    LoadLibraryW(L"comctl32.dll");   // современный вид кнопок (вместе с манифестом)
    InitializeCriticalSection(&g_cs);
    LoadConfig();
    MigrateConfigIfNeeded();

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);

    g_wavOther = MakeSoftWav(330.0);
    g_wavEn = MakeSoftWav(440.0);
    g_darkTaskbar = !IsLightTaskbar();

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
    sc.hIcon = appIcon;
    sc.hIconSm = appIcon;
    sc.lpszClassName = L"KeySwitchSettings";
    RegisterClassExW(&sc);

    g_mainWnd = CreateWindowExW(0, L"KeySwitchMain", APP_NAME, 0, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HWND fg = GetForegroundWindow();
    g_seenWnd = fg;
    g_seenHkl = fg ? WindowLayout(fg) : nullptr;
    if (fg && !IsShellOrOwnWindow(fg)) g_lastExtHwnd = fg;
    UpdateTrayIcon(fg && !IsEnglish(g_seenHkl) ? 1 : 0, fg ? LayoutCode(g_seenHkl) : std::wstring(L"EN"));
    AddTrayIcon();
    CreateFlyout(hi);
    CreateTrayMenu(hi);
    SetTimer(g_mainWnd, 1, 100, nullptr);   // следим за раскладкой активного окна

    HANDLE hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
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
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    Gdiplus::GdiplusShutdown(gdipToken);
    DeleteCriticalSection(&g_cs);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
