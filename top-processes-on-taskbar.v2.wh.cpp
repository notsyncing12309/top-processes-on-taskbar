// ==WindhawkMod==
// @id              top-processes-on-taskbar
// @name            Top processes on taskbar
// @description     Shows the two processes that are using the most CPU on the left side of the Windows 11 taskbar.
// @version         2.0
// @author          notsync & deepseek 4.1 flash agent
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lpdh -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Top processes on taskbar v2

Shows the two processes that are currently using the most CPU, in the left
part of the Windows 11 taskbar:

```
chrome.exe        40 %
Code.exe          13 %
```

Created with DeepSeek Agent, using [this mod](https://github.com/starychenko/windhawk-taskbar-system-info) as reference. Many thanks to the author for providing the foundation for other taskbar widget mods. GPL v3 license is inherited from the reference mod.

## How it works

- CPU usage is read from the Windows performance counter
  `\Process V2(*)\% Processor Time` (with a fallback to `\Process`).
- Values are normalized to the whole system, like the Task Manager CPU column:
  one fully loaded core on an 8-core CPU shows about 12.5%.
- Processes with the same executable name are grouped (all `firefox.exe`
  instances appear as one line).
- The widget is inserted into the taskbar's native XAML tree, so it moves and
  hides together with the taskbar, follows the taskbar theme and scales with
  display DPI.

## Settings

- **Left offset** - distance from the left edge of the taskbar, in XAML
  device-independent pixels. Increase it if the taskbar is left-aligned and
  the text overlaps the Start button.
- **Top offset** - fine vertical adjustment.
- **Font size**, **Font family**, **Max name width**, **Text opacity**.
- **Text color** - `#RRGGBB` or `#AARRGGBB`; empty means automatic light/dark
  color.
- **Update interval** - how often values are refreshed.
- **Excluded processes** - comma-separated executable names, for example
  `System, explorer.exe`.

## Limitations

- Taskbar XAML hooks depend on Windows symbols. A future Windows update may
  break them; the mod logs an error and does not crash Explorer.
- The widget is placed over the taskbar rather than reserving layout space.
  With a left-aligned taskbar it can overlap the Start/search area, so use a
  larger **Left offset** or a centered taskbar.
- Protected processes are read through performance counters, but processes
  that are invisible to the counter are not shown.
- On multi-monitor systems every taskbar that is discovered gets a widget.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- leftOffset: 8
  $name: Left offset
  $description: "Distance from the left taskbar edge in device-independent pixels. Increase it if the widget overlaps the Start button."
- topOffset: 0
  $name: Top offset
  $description: "Fine vertical adjustment in device-independent pixels."
- fontSize: 11
  $name: Font size
  $description: "Text size in device-independent pixels. Range: 9-14."
- fontFamily: "Segoe UI Variable Text"
  $name: Font family
  $description: "Installed font family used for the process names and percentages."
- maxNameWidth: 140
  $name: Max name width
  $description: "Maximum width of the process name column in device-independent pixels. Longer names are trimmed with an ellipsis. Range: 60-300."
- textOpacity: 90
  $name: Text opacity
  $description: "Text opacity, 0-100 percent."
- textColor: ""
  $name: Text color
  $description: "Optional #RRGGBB or #AARRGGBB color. Leave empty to use the taskbar light/dark theme color."
- updateInterval: 1000
  $name: Update interval (ms)
  $description: "How often CPU usage is sampled. Range: 500-10000 milliseconds."
- excludedProcesses: ""
  $name: Excluded processes
  $description: "Optional comma-separated executable names, for example: System, explorer.exe. Idle is always excluded."
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <windows.h>
#include <unknwn.h>
#include <pdh.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

#ifndef PDH_FMT_NOCAP100
#define PDH_FMT_NOCAP100 0x00008000
#endif

#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA static_cast<PDH_STATUS>(0x800007D2L)
#endif

#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000L
#endif

#ifndef PDH_CSTATUS_NEW_DATA
#define PDH_CSTATUS_NEW_DATA 0x00000001L
#endif

namespace {

constexpr wchar_t kHostName[] = L"ProcessManHost";
constexpr UINT kUiUpdateIntervalMs = 250;
constexpr double kPercentColumnWidth = 50.0;
constexpr double kColumnGap = 6.0;
constexpr int kMaxRows = 2;

struct Settings {
    int leftOffset = 8;
    int topOffset = 0;
    int fontSize = 11;
    std::wstring fontFamily = L"Segoe UI Variable Text";
    int maxNameWidth = 140;
    int textOpacity = 90;
    std::wstring textColor;
    int updateInterval = 1000;
    std::vector<std::wstring> excludedProcesses;
};

struct TopProcess {
    std::wstring name;
    int percent = 0;
};

struct Snapshot {
    std::wstring names[kMaxRows];
    int percents[kMaxRows]{};
    uint64_t sequence = 0;
};

std::mutex g_settingsMutex;
std::shared_ptr<const Settings> g_settings;
std::atomic<uint64_t> g_settingsRevision{0};

std::mutex g_snapshotMutex;
Snapshot g_snapshot;

std::atomic<bool> g_unloading{false};

std::mutex g_samplerMutex;
std::condition_variable g_samplerCv;
bool g_samplerStop = false;
std::thread g_samplerThread;

struct WidgetInstance {
    FrameworkElement frame{nullptr};
    Grid root{nullptr};
    Canvas host{nullptr};
    Grid panel{nullptr};
    TextBlock name1{nullptr};
    TextBlock percent1{nullptr};
    TextBlock name2{nullptr};
    TextBlock percent2{nullptr};
    DispatcherTimer timer{nullptr};
    CoreDispatcher dispatcher{nullptr};
    event_token timerToken{};
    event_token sizeToken{};
    event_token rootSizeToken{};
    event_token themeToken{};
    event_token unloadedToken{};
    uint64_t lastSequence = 0;
    uint64_t lastSettingsRevision = 0;
    bool tearingDown = false;
    bool loggedUpdateError = false;
};

std::mutex g_widgetsMutex;
std::vector<std::shared_ptr<WidgetInstance>> g_widgets;

struct FrameHook {
    FrameworkElement frame{nullptr};
    event_token token{};
    CoreDispatcher dispatcher{nullptr};
};

std::mutex g_frameHooksMutex;
std::vector<FrameHook> g_frameHooks;

std::atomic<bool> g_taskbarHooked{false};
std::mutex g_taskbarHookMutex;

std::atomic<bool> g_taskbarDllSymbolsHooked{false};
void* g_ctaskBandSiteVftable = nullptr;
void* g_csecondaryTaskBandSiteVftable = nullptr;

using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void* sharedPtr);
using TaskbarHostFrameHeight_t = int(WINAPI*)(void* pThis);
using RefCountDecref_t = void(WINAPI*)(void* pThis);

GetTaskbarHost_t g_getPrimaryTaskbarHost = nullptr;
GetTaskbarHost_t g_getSecondaryTaskbarHost = nullptr;
TaskbarHostFrameHeight_t g_taskbarHostFrameHeight = nullptr;
RefCountDecref_t g_refCountDecref = nullptr;

using RunOnTaskbarThreadProc = void (*)(void* context);

struct RunOnTaskbarThreadContext {
    RunOnTaskbarThreadProc proc = nullptr;
    void* context = nullptr;
    std::atomic<bool> invoked{false};
};

UINT g_runOnTaskbarThreadMessage = 0;
std::mutex g_runOnTaskbarThreadMutex;
std::unordered_map<ULONG_PTR, RunOnTaskbarThreadContext*>
    g_runOnTaskbarThreadContexts;
std::atomic<ULONG_PTR> g_runOnTaskbarThreadToken{1};

using TaskbarFrameConstructor_t = void*(WINAPI*)(void* pThis);
TaskbarFrameConstructor_t TaskbarFrameConstructor_Original = nullptr;

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

std::wstring GetStringSetting(PCWSTR name) {
    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring result = value ? value : L"";
    if (value) {
        Wh_FreeStringSetting(value);
    }
    return result;
}

int ClampInt(int value, int minimum, int maximum) {
    return std::max(minimum, std::min(maximum, value));
}

std::wstring Trim(std::wstring value) {
    const auto isSpace = [](wchar_t c) {
        return std::iswspace(static_cast<wint_t>(c)) != 0;
    };
    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    });
    return value;
}

std::wstring ProcessKey(std::wstring value) {
    value = Lowercase(std::move(value));
    if (value.size() > 4 &&
        value.compare(value.size() - 4, 4, L".exe") == 0) {
        value.resize(value.size() - 4);
    }
    return value;
}

std::shared_ptr<const Settings> GetSettings() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

void LoadSettings() {
    auto settings = std::make_shared<Settings>();
    settings->leftOffset = ClampInt(Wh_GetIntSetting(L"leftOffset"), 0, 2000);
    settings->topOffset = ClampInt(Wh_GetIntSetting(L"topOffset"), -40, 40);
    settings->fontSize = ClampInt(Wh_GetIntSetting(L"fontSize"), 9, 14);
    settings->maxNameWidth =
        ClampInt(Wh_GetIntSetting(L"maxNameWidth"), 60, 300);
    settings->textOpacity =
        ClampInt(Wh_GetIntSetting(L"textOpacity"), 0, 100);
    settings->updateInterval =
        ClampInt(Wh_GetIntSetting(L"updateInterval"), 500, 10000);

    settings->fontFamily = GetStringSetting(L"fontFamily");
    if (settings->fontFamily.empty()) {
        settings->fontFamily = L"Segoe UI Variable Text";
    }
    settings->textColor = Trim(GetStringSetting(L"textColor"));

    std::wstring excluded = GetStringSetting(L"excludedProcesses");
    size_t position = 0;
    while (position <= excluded.size()) {
        size_t comma = excluded.find(L',', position);
        std::wstring item =
            excluded.substr(position, comma == std::wstring::npos
                                         ? std::wstring::npos
                                         : comma - position);
        item = ProcessKey(Trim(std::move(item)));
        if (!item.empty()) {
            settings->excludedProcesses.push_back(std::move(item));
        }
        if (comma == std::wstring::npos) {
            break;
        }
        position = comma + 1;
    }

    // These pseudo-processes are never useful in the widget.
    settings->excludedProcesses.push_back(L"idle");
    settings->excludedProcesses.push_back(L"_total");

    {
        std::lock_guard lock(g_settingsMutex);
        g_settings = std::move(settings);
    }
    g_settingsRevision.fetch_add(1, std::memory_order_relaxed);
    g_samplerCv.notify_all();
}

bool IsExcluded(const Settings& settings, const std::wstring& name) {
    const std::wstring key = ProcessKey(name);
    return std::find(settings.excludedProcesses.begin(),
                     settings.excludedProcesses.end(),
                     key) != settings.excludedProcesses.end();
}

std::wstring ResolveProcessName(
    const std::unordered_map<DWORD, std::wstring>& pidToName,
    const std::wstring& instanceName) {
    std::wstring base = instanceName;
    DWORD pid = 0;

    size_t colon = instanceName.find_last_of(L':');
    if (colon != std::wstring::npos) {
        wchar_t* end = nullptr;
        unsigned long parsed =
            wcstoul(instanceName.c_str() + colon + 1, &end, 10);
        if (end && *end == L'\0') {
            pid = static_cast<DWORD>(parsed);
            base = instanceName.substr(0, colon);
        }
    }

    if (!pid) {
        size_t hash = instanceName.find_last_of(L'#');
        if (hash != std::wstring::npos) {
            const std::wstring suffix = instanceName.substr(hash + 1);
            const bool numericSuffix =
                !suffix.empty() &&
                std::all_of(suffix.begin(), suffix.end(),
                            [](wchar_t c) { return c >= L'0' && c <= L'9'; });
            if (numericSuffix) {
                base = instanceName.substr(0, hash);
            }
        }
    }

    if (pid) {
        auto it = pidToName.find(pid);
        if (it != pidToName.end()) {
            std::wstring result = it->second;
            if (result == L"[System Process]") {
                result = L"System";
            }
            return result;
        }
    }

    if (base == L"[System Process]") {
        return L"System";
    }
    if (!base.empty() && base.find(L'.') == std::wstring::npos &&
        base.find(L' ') == std::wstring::npos &&
        _wcsicmp(base.c_str(), L"System") != 0 &&
        _wcsicmp(base.c_str(), L"Idle") != 0) {
        base += L".exe";
    }
    return base;
}

std::unordered_map<DWORD, std::wstring> SnapshotProcessNames() {
    std::unordered_map<DWORD, std::wstring> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            result[entry.th32ProcessID] = entry.szExeFile;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool OpenPdhQuery(PDH_HQUERY& query, PDH_HCOUNTER& counter) {
    query = nullptr;
    counter = nullptr;

    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
    if (status != ERROR_SUCCESS) {
        Wh_Log(L"PdhOpenQuery failed: %08X", static_cast<unsigned>(status));
        return false;
    }

    status = PdhAddEnglishCounterW(
        query, L"\\Process V2(*)\\% Processor Time", 0, &counter);
    if (status == ERROR_SUCCESS) {
        Wh_Log(L"Using Process V2 performance counters");
    } else {
        status = PdhAddEnglishCounterW(
            query, L"\\Process(*)\\% Processor Time", 0, &counter);
        if (status == ERROR_SUCCESS) {
            Wh_Log(L"Using classic Process performance counters");
        }
    }

    if (status != ERROR_SUCCESS) {
        Wh_Log(L"Adding process CPU counter failed: %08X",
               static_cast<unsigned>(status));
        PdhCloseQuery(query);
        query = nullptr;
        counter = nullptr;
        return false;
    }

    status = PdhCollectQueryData(query);
    if (status != ERROR_SUCCESS) {
        Wh_Log(L"Priming process CPU counter failed: %08X",
               static_cast<unsigned>(status));
        PdhCloseQuery(query);
        query = nullptr;
        counter = nullptr;
        return false;
    }
    return true;
}

void PublishSnapshot(std::vector<TopProcess> processes) {
    std::sort(processes.begin(), processes.end(),
              [](const TopProcess& left, const TopProcess& right) {
                  if (left.percent != right.percent) {
                      return left.percent > right.percent;
                  }
                  return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
              });

    Snapshot snapshot;
    for (int index = 0;
         index < kMaxRows && index < static_cast<int>(processes.size());
         ++index) {
        snapshot.names[index] = std::move(processes[index].name);
        snapshot.percents[index] = processes[index].percent;
    }

    {
        std::lock_guard lock(g_snapshotMutex);
        snapshot.sequence = g_snapshot.sequence + 1;
        g_snapshot = std::move(snapshot);
    }
}

void ReadProcessCpu(PDH_HCOUNTER counter) {
    auto settings = GetSettings();
    if (!settings) {
        return;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferSize, &itemCount,
        nullptr);
    if (status != PDH_MORE_DATA && status != ERROR_SUCCESS) {
        if (status != static_cast<PDH_STATUS>(0x800007D1L)) {  // NO_INSTANCE
            Wh_Log(L"PdhGetFormattedCounterArray size query failed: %08X",
                   static_cast<unsigned>(status));
        }
        return;
    }
    if (bufferSize == 0 || itemCount == 0) {
        PublishSnapshot({});
        return;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto* items =
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferSize, &itemCount,
        items);
    if (status != ERROR_SUCCESS) {
        Wh_Log(L"PdhGetFormattedCounterArray failed: %08X",
               static_cast<unsigned>(status));
        return;
    }

    DWORD processorCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!processorCount) {
        processorCount = 1;
    }

    auto pidToName = SnapshotProcessNames();

    struct Aggregate {
        std::wstring name;
        double percent = 0.0;
    };
    std::unordered_map<std::wstring, Aggregate> aggregates;

    for (DWORD index = 0; index < itemCount; ++index) {
        const auto& item = items[index];
        if (!item.szName ||
            (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
             item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) {
            continue;
        }

        const std::wstring instanceName = item.szName;
        if (instanceName.empty() || instanceName == L"_Total") {
            continue;
        }

        const std::wstring lowerInstance = Lowercase(instanceName);
        if (lowerInstance == L"idle" ||
            lowerInstance.rfind(L"idle:", 0) == 0 ||
            lowerInstance.rfind(L"idle#", 0) == 0 ||
            lowerInstance.rfind(L"[system process]", 0) == 0) {
            continue;
        }

        const std::wstring name =
            ResolveProcessName(pidToName, instanceName);
        if (name.empty() || IsExcluded(*settings, name)) {
            continue;
        }

        double percent =
            item.FmtValue.doubleValue / static_cast<double>(processorCount);
        if (!std::isfinite(percent) || percent <= 0.0) {
            continue;
        }
        percent = std::min(percent, 100.0);

        const std::wstring key = Lowercase(name);
        auto& aggregate = aggregates[key];
        if (aggregate.name.empty()) {
            aggregate.name = name;
        }
        aggregate.percent += percent;
    }

    std::vector<TopProcess> top;
    top.reserve(aggregates.size());
    for (auto& [key, aggregate] : aggregates) {
        TopProcess process;
        process.name = std::move(aggregate.name);
        process.percent = ClampInt(
            static_cast<int>(std::lround(aggregate.percent)), 0, 100);
        top.push_back(std::move(process));
    }
    PublishSnapshot(std::move(top));
}

void SamplerLoop() {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;

    try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        bool ready = OpenPdhQuery(query, counter);
        int consecutiveFailures = 0;
        uint64_t logCounter = 0;

        while (true) {
            auto settings = GetSettings();
            int interval = settings ? settings->updateInterval : 1000;

            {
                std::unique_lock lock(g_samplerMutex);
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(interval);
                uint64_t revision =
                    g_settingsRevision.load(std::memory_order_relaxed);

                while (!g_samplerStop) {
                    if (g_samplerCv.wait_until(lock, deadline) ==
                        std::cv_status::timeout) {
                        break;
                    }
                    if (g_samplerStop) {
                        break;
                    }
                    if (g_settingsRevision.load(std::memory_order_relaxed) !=
                        revision) {
                        revision = g_settingsRevision.load(
                            std::memory_order_relaxed);
                        auto updated = GetSettings();
                        interval =
                            updated ? updated->updateInterval : interval;
                        deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(interval);
                    }
                }
                if (g_samplerStop) {
                    break;
                }
            }

            if (!ready) {
                {
                    std::unique_lock lock(g_samplerMutex);
                    g_samplerCv.wait_for(lock, std::chrono::seconds(5),
                                         [] { return g_samplerStop; });
                }
                if (g_samplerStop) {
                    break;
                }
                ready = OpenPdhQuery(query, counter);
                continue;
            }

            PDH_STATUS status = PdhCollectQueryData(query);
            if (status != ERROR_SUCCESS) {
                ++consecutiveFailures;
                if (consecutiveFailures >= 5) {
                    Wh_Log(L"Process CPU counter keeps failing, reopening");
                    PdhCloseQuery(query);
                    query = nullptr;
                    counter = nullptr;
                    ready = false;
                    consecutiveFailures = 0;
                }
                continue;
            }
            consecutiveFailures = 0;

            ReadProcessCpu(counter);

            if (++logCounter % 30 == 0) {
                Snapshot snapshot;
                {
                    std::lock_guard lock(g_snapshotMutex);
                    snapshot = g_snapshot;
                }
                Wh_Log(L"Top CPU: %s %d%%, %s %d%%",
                       snapshot.names[0].c_str(), snapshot.percents[0],
                       snapshot.names[1].c_str(), snapshot.percents[1]);
            }
        }

        if (query) {
            PdhCloseQuery(query);
        }
    } catch (...) {
        Wh_Log(L"CPU sampler failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
        if (query) {
            PdhCloseQuery(query);
        }
    }
}

void StartSampler() {
    g_samplerStop = false;
    try {
        g_samplerThread = std::thread(SamplerLoop);
    } catch (...) {
        Wh_Log(L"Starting the CPU sampler thread failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
}

void StopSampler() {
    {
        std::lock_guard lock(g_samplerMutex);
        g_samplerStop = true;
    }
    g_samplerCv.notify_all();
    if (g_samplerThread.joinable()) {
        g_samplerThread.join();
    }
}

bool ParseColor(const std::wstring& text, Color& color) {
    if (text.size() != 7 && text.size() != 9) {
        return false;
    }
    if (text[0] != L'#') {
        return false;
    }

    wchar_t* end = nullptr;
    unsigned long value = wcstoul(text.c_str() + 1, &end, 16);
    if (!end || *end != L'\0') {
        return false;
    }

    if (text.size() == 7) {
        color = ColorHelper::FromArgb(
            255, static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF));
    } else {
        color = ColorHelper::FromArgb(
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF));
    }
    return true;
}

bool TaskbarUsesDarkTheme() {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    LONG result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme,
        &size);
    if (result == ERROR_SUCCESS) {
        return lightTheme == 0;
    }

    try {
        auto application = Application::Current();
        return application &&
               application.RequestedTheme() == ApplicationTheme::Dark;
    } catch (...) {
        return true;
    }
}

void ApplyTheme(WidgetInstance& widget, const Settings& settings) {
    if (!widget.host || !widget.name1) {
        return;
    }

    try {
        Color color{};
        bool hasColor = !settings.textColor.empty() &&
                        ParseColor(settings.textColor, color);
        if (!hasColor) {
            bool dark = TaskbarUsesDarkTheme();
            try {
                auto theme = widget.host.ActualTheme();
                if (theme == ElementTheme::Dark) {
                    dark = true;
                } else if (theme == ElementTheme::Light) {
                    dark = false;
                }
            } catch (...) {
            }

            color = dark ? ColorHelper::FromArgb(255, 255, 255, 255)
                         : ColorHelper::FromArgb(255, 20, 20, 20);
        }

        SolidColorBrush brush(color);
        widget.name1.Foreground(brush);
        widget.percent1.Foreground(brush);
        widget.name2.Foreground(brush);
        widget.percent2.Foreground(brush);
    } catch (...) {
        Wh_Log(L"Applying theme failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
}

void CenterWidget(WidgetInstance& widget, const Settings& settings) {
    if (!widget.root || !widget.panel) {
        return;
    }

    double rootHeight = widget.root.ActualHeight();
    if (rootHeight <= 0.0) {
        rootHeight = 48.0;
    }
    double panelHeight = widget.panel.ActualHeight();
    if (panelHeight <= 0.0) {
        panelHeight = settings.fontSize * 2.6;
    }

    double top = (rootHeight - panelHeight) / 2.0 + settings.topOffset;
    Canvas::SetTop(widget.panel, std::max(0.0, top));
}

void ApplySettingsToWidget(WidgetInstance& widget, const Settings& settings) {
    if (!widget.host || !widget.panel || !widget.name1) {
        return;
    }

    try {
        Canvas::SetLeft(widget.panel, static_cast<double>(settings.leftOffset));

        FontFamily fontFamily{winrt::hstring(settings.fontFamily.c_str())};
        TextBlock texts[] = {widget.name1, widget.percent1, widget.name2,
                             widget.percent2};
        for (auto text : texts) {
            text.FontSize(static_cast<double>(settings.fontSize));
            text.FontFamily(fontFamily);
            text.Opacity(static_cast<double>(settings.textOpacity) / 100.0);
        }

        widget.name1.MaxWidth(static_cast<double>(settings.maxNameWidth));
        widget.name2.MaxWidth(static_cast<double>(settings.maxNameWidth));

        ApplyTheme(widget, settings);
        CenterWidget(widget, settings);
    } catch (...) {
        Wh_Log(L"Applying widget settings failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
}

void ApplySnapshotToWidget(WidgetInstance& widget, const Snapshot& snapshot) {
    if (!widget.name1) {
        return;
    }

    const auto textFor = [](const std::wstring& value) {
        return winrt::hstring(value.c_str());
    };
    const auto percentFor = [](bool present, int percent) {
        if (!present) {
            return winrt::hstring(L"");
        }
        return winrt::hstring(
            (std::to_wstring(percent) + L" %").c_str());
    };

    const bool firstPresent = !snapshot.names[0].empty();
    const bool secondPresent = !snapshot.names[1].empty();

    widget.name1.Text(firstPresent ? textFor(snapshot.names[0])
                                   : winrt::hstring(L"-"));
    widget.percent1.Text(percentFor(firstPresent, snapshot.percents[0]));
    widget.name2.Text(textFor(snapshot.names[1]));
    widget.percent2.Text(percentFor(secondPresent, snapshot.percents[1]));
}

void UpdateWidget(WidgetInstance& widget) {
    if (g_unloading.load(std::memory_order_relaxed) || widget.tearingDown) {
        return;
    }

    try {
        uint64_t revision =
            g_settingsRevision.load(std::memory_order_relaxed);
        if (widget.lastSettingsRevision != revision) {
            auto settings = GetSettings();
            if (settings) {
                widget.lastSettingsRevision = revision;
                ApplySettingsToWidget(widget, *settings);
            }
        }

        Snapshot snapshot;
        {
            std::lock_guard lock(g_snapshotMutex);
            snapshot = g_snapshot;
        }
        if (widget.lastSequence != snapshot.sequence) {
            widget.lastSequence = snapshot.sequence;
            ApplySnapshotToWidget(widget, snapshot);
        }
    } catch (...) {
        if (!widget.loggedUpdateError) {
            widget.loggedUpdateError = true;
            Wh_Log(L"Updating widget failed: %08X",
                   static_cast<unsigned>(winrt::to_hresult()));
        }
    }
}

TextBlock CreateTextBlock() {
    TextBlock text;
    text.TextWrapping(TextWrapping::NoWrap);
    text.TextTrimming(TextTrimming::CharacterEllipsis);
    text.IsHitTestVisible(false);
    text.VerticalAlignment(VerticalAlignment::Center);
    return text;
}

void TeardownWidget(WidgetInstance& widget) {
    widget.tearingDown = true;

    if (widget.timer) {
        try {
            widget.timer.Stop();
            if (widget.timerToken.value) {
                widget.timer.Tick(widget.timerToken);
            }
        } catch (...) {
        }
        widget.timer = nullptr;
        widget.timerToken = {};
    }

    try {
        if (widget.host) {
            if (widget.themeToken.value) {
                widget.host.ActualThemeChanged(widget.themeToken);
            }
            if (widget.unloadedToken.value) {
                widget.host.Unloaded(widget.unloadedToken);
            }
        }
        if (widget.panel && widget.sizeToken.value) {
            widget.panel.SizeChanged(widget.sizeToken);
        }
        if (widget.root && widget.rootSizeToken.value) {
            widget.root.SizeChanged(widget.rootSizeToken);
        }
    } catch (...) {
    }

    try {
        if (widget.root && widget.host) {
            auto children = widget.root.Children();
            uint32_t index = 0;
            if (children.IndexOf(widget.host, index)) {
                children.RemoveAt(index);
            }
        }
    } catch (...) {
    }

    widget.root = nullptr;
    widget.host = nullptr;
    widget.panel = nullptr;
    widget.name1 = nullptr;
    widget.percent1 = nullptr;
    widget.name2 = nullptr;
    widget.percent2 = nullptr;
    widget.frame = nullptr;
    widget.dispatcher = nullptr;
    widget.timerToken = {};
    widget.sizeToken = {};
    widget.rootSizeToken = {};
    widget.themeToken = {};
    widget.unloadedToken = {};
}

void RetireWidget(const std::shared_ptr<WidgetInstance>& widget) {
    if (!widget || widget->tearingDown) {
        return;
    }

    {
        std::lock_guard lock(g_widgetsMutex);
        auto it = std::find(g_widgets.begin(), g_widgets.end(), widget);
        if (it != g_widgets.end()) {
            g_widgets.erase(it);
        }
    }

    TeardownWidget(*widget);
}

bool RunOnDispatcher(CoreDispatcher dispatcher,
                     std::function<void()> action) {
    if (!dispatcher) {
        return false;
    }

    try {
        if (dispatcher.HasThreadAccess()) {
            action();
            return true;
        }
        auto operation = dispatcher.RunAsync(
            CoreDispatcherPriority::Normal,
            [action = std::move(action)]() { action(); });
        operation.get();
        return true;
    } catch (...) {
        Wh_Log(L"Dispatching to the taskbar thread failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
        return false;
    }
}

FrameworkElement FindChildByName(FrameworkElement parent,
                                 PCWSTR name,
                                 int depth) {
    if (!parent || depth <= 0) {
        return nullptr;
    }

    const int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int index = 0; index < count; ++index) {
        auto child = VisualTreeHelper::GetChild(parent, index)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }
        if (child.Name() == name) {
            return child;
        }
        if (auto nested = FindChildByName(child, name, depth - 1)) {
            return nested;
        }
    }
    return nullptr;
}

FrameworkElement FindTaskbarFrameInTree(FrameworkElement parent, int depth) {
    if (!parent || depth <= 0) {
        return nullptr;
    }

    if (winrt::get_class_name(parent) == L"Taskbar.TaskbarFrame") {
        return parent;
    }

    const int count = VisualTreeHelper::GetChildrenCount(parent);
    for (int index = 0; index < count; ++index) {
        auto child = VisualTreeHelper::GetChild(parent, index)
                         .try_as<FrameworkElement>();
        if (!child) {
            continue;
        }
        if (auto nested = FindTaskbarFrameInTree(child, depth - 1)) {
            return nested;
        }
    }
    return nullptr;
}

bool GetTaskbarKind(HWND window, bool& secondary) {
    if (!window) {
        return false;
    }

    DWORD processId = 0;
    if (!GetWindowThreadProcessId(window, &processId) ||
        processId != GetCurrentProcessId()) {
        return false;
    }

    wchar_t className[64]{};
    if (!GetClassNameW(window, className, std::size(className))) {
        return false;
    }

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0) {
        secondary = false;
        return true;
    }
    if (_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        secondary = true;
        return true;
    }
    return false;
}

XamlRoot GetTaskbarXamlRoot(HWND taskbarWindow) {
    bool secondary = false;
    if (!GetTaskbarKind(taskbarWindow, secondary) ||
        !g_taskbarHostFrameHeight || !g_refCountDecref) {
        return nullptr;
    }

    auto getTaskbarHost = secondary ? g_getSecondaryTaskbarHost
                                    : g_getPrimaryTaskbarHost;
    void* expectedVftable = secondary ? g_csecondaryTaskBandSiteVftable
                                      : g_ctaskBandSiteVftable;
    if (!getTaskbarHost || !expectedVftable) {
        Wh_Log(L"%s taskbar symbols are unavailable",
               secondary ? L"Secondary" : L"Primary");
        return nullptr;
    }

    HWND taskBandWindow =
        secondary
            ? FindWindowExW(taskbarWindow, nullptr, L"WorkerW", nullptr)
            : reinterpret_cast<HWND>(
                  GetPropW(taskbarWindow, L"TaskbandHWND"));
    if (!taskBandWindow && !secondary) {
        taskBandWindow =
            FindWindowExW(taskbarWindow, nullptr, L"WorkerW", nullptr);
    }
    if (!taskBandWindow) {
        Wh_Log(L"%s taskband host window was not found",
               secondary ? L"Secondary" : L"Primary");
        return nullptr;
    }

    void* taskBand = reinterpret_cast<void*>(
        GetWindowLongPtrW(taskBandWindow, 0));
    if (!taskBand) {
        return nullptr;
    }

    void* taskBandForSite = taskBand;
    bool foundVftable = false;
    for (int index = 0; index < 20; ++index) {
        if (*reinterpret_cast<void**>(taskBandForSite) == expectedVftable) {
            foundVftable = true;
            break;
        }
        taskBandForSite =
            reinterpret_cast<void**>(taskBandForSite) + 1;
    }
    if (!foundVftable) {
        Wh_Log(L"Taskband site vftable was not found");
        return nullptr;
    }

    void* taskbarHostSharedPtr[2]{};
    getTaskbarHost(taskBandForSite, taskbarHostSharedPtr);

    struct TaskbarHostReferenceGuard {
        void* reference;
        ~TaskbarHostReferenceGuard() {
            if (reference) {
                g_refCountDecref(reference);
            }
        }
    } referenceGuard{taskbarHostSharedPtr[1]};

    if (!taskbarHostSharedPtr[0] || !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t elementOffset = 0;
#if defined(_M_X64)
    const BYTE* code =
        reinterpret_cast<const BYTE*>(g_taskbarHostFrameHeight);
    if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xEC &&
        code[4] == 0x48 && code[5] == 0x83 && code[6] == 0xC1 &&
        code[7] <= 0x7F) {
        elementOffset = code[7];
    } else {
        Wh_Log(L"Unsupported TaskbarHost::FrameHeight pattern");
        return nullptr;
    }
#else
    Wh_Log(L"Unsupported taskbar architecture");
    return nullptr;
#endif

    auto* elementAddress =
        static_cast<BYTE*>(taskbarHostSharedPtr[0]) + elementOffset;
    auto* elementUnknown =
        *reinterpret_cast<::IUnknown**>(elementAddress);
    if (!elementUnknown) {
        Wh_Log(L"TaskbarHost element pointer is null");
        return nullptr;
    }

    FrameworkElement taskbarElement{nullptr};
    elementUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                   winrt::put_abi(taskbarElement));
    if (!taskbarElement) {
        Wh_Log(L"TaskbarHost element is not a FrameworkElement");
        return nullptr;
    }
    return taskbarElement.XamlRoot();
}

FrameworkElement FindTaskbarFrameFromCurrentWindow() {
    try {
        auto window = Window::Current();
        if (!window) {
            return nullptr;
        }
        auto content = window.Content().try_as<FrameworkElement>();
        if (!content) {
            return nullptr;
        }
        return FindTaskbarFrameInTree(content, 16);
    } catch (...) {
        return nullptr;
    }
}

FrameworkElement FindTaskbarFrameForWindow(HWND window) {
    if (auto frame = FindTaskbarFrameFromCurrentWindow()) {
        Wh_Log(L"Found the taskbar frame via Window::Current");
        return frame;
    }

    try {
        XamlRoot xamlRoot = GetTaskbarXamlRoot(window);
        if (!xamlRoot) {
            Wh_Log(L"Taskbar XamlRoot is unavailable");
            return nullptr;
        }
        auto content = xamlRoot.Content().try_as<FrameworkElement>();
        if (!content) {
            Wh_Log(L"Taskbar XamlRoot has no FrameworkElement content");
            return nullptr;
        }
        auto frame = FindTaskbarFrameInTree(content, 16);
        if (!frame) {
            Wh_Log(L"Taskbar.TaskbarFrame was not found in the XAML tree");
        }
        return frame;
    } catch (...) {
        Wh_Log(L"Finding the taskbar XAML frame failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
        return nullptr;
    }
}

LRESULT CALLBACK RunOnTaskbarThreadHook(int code,
                                        WPARAM wParam,
                                        LPARAM lParam) {
    if (code == HC_ACTION && g_runOnTaskbarThreadMessage) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message == g_runOnTaskbarThreadMessage) {
            RunOnTaskbarThreadContext* context = nullptr;
            {
                std::lock_guard lock(g_runOnTaskbarThreadMutex);
                auto it = g_runOnTaskbarThreadContexts.find(
                    static_cast<ULONG_PTR>(message->lParam));
                if (it != g_runOnTaskbarThreadContexts.end()) {
                    context = it->second;
                }
            }
            if (context) {
                try {
                    context->proc(context->context);
                } catch (...) {
                    Wh_Log(L"Taskbar thread callback failed: %08X",
                           static_cast<unsigned>(winrt::to_hresult()));
                }
                context->invoked.store(true, std::memory_order_release);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool RunOnTaskbarThread(HWND window,
                        RunOnTaskbarThreadProc proc,
                        void* context) {
    if (!window || !proc) {
        return false;
    }

    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    if (!threadId) {
        return false;
    }
    if (threadId == GetCurrentThreadId()) {
        proc(context);
        return true;
    }
    if (!g_runOnTaskbarThreadMessage) {
        return false;
    }

    RunOnTaskbarThreadContext callbackContext{proc, context, false};
    ULONG_PTR token = 0;
    {
        std::lock_guard lock(g_runOnTaskbarThreadMutex);
        token = ++g_runOnTaskbarThreadToken;
        g_runOnTaskbarThreadContexts[token] = &callbackContext;
    }

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, RunOnTaskbarThreadHook,
                                   nullptr, threadId);
    if (!hook) {
        std::lock_guard lock(g_runOnTaskbarThreadMutex);
        g_runOnTaskbarThreadContexts.erase(token);
        Wh_Log(L"SetWindowsHookEx failed for taskbar thread %u: %u", threadId,
               GetLastError());
        return false;
    }

    DWORD_PTR messageResult = 0;
    LRESULT sent = SendMessageTimeoutW(
        window, g_runOnTaskbarThreadMessage, 0,
        static_cast<LPARAM>(token), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000,
        &messageResult);

    bool unhooked = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (UnhookWindowsHookEx(hook)) {
            unhooked = true;
            break;
        }
        if (GetLastError() == ERROR_INVALID_HOOK_HANDLE) {
            unhooked = true;
            break;
        }
        Sleep(10);
    }

    {
        std::lock_guard lock(g_runOnTaskbarThreadMutex);
        g_runOnTaskbarThreadContexts.erase(token);
    }

    if (!sent) {
        Wh_Log(L"Sending a message to the taskbar thread failed: %u",
               GetLastError());
        return false;
    }
    if (!unhooked) {
        Wh_Log(L"Unhooking the taskbar thread hook failed");
    }
    return callbackContext.invoked.load(std::memory_order_acquire);
}

void InjectWidget(FrameworkElement frame) {
    if (g_unloading.load(std::memory_order_relaxed) || !frame) {
        return;
    }

    try {
        auto root = FindChildByName(frame, L"RootGrid", 8).try_as<Grid>();
        if (!root) {
            Wh_Log(L"Taskbar RootGrid was not found");
            return;
        }

        {
            std::lock_guard lock(g_widgetsMutex);
            for (const auto& widget : g_widgets) {
                if (widget->frame == frame) {
                    return;
                }
            }
        }

        auto children = root.Children();
        for (uint32_t index = 0; index < children.Size();) {
            auto child = children.GetAt(index).try_as<FrameworkElement>();
            if (child && child.Name() == kHostName) {
                children.RemoveAt(index);
                continue;
            }
            ++index;
        }

        auto widget = std::make_shared<WidgetInstance>();
        widget->frame = frame;
        widget->root = root;
        widget->dispatcher = frame.Dispatcher();

        widget->host = Canvas();
        widget->host.Name(kHostName);
        widget->host.IsHitTestVisible(false);
        widget->host.HorizontalAlignment(HorizontalAlignment::Stretch);
        widget->host.VerticalAlignment(VerticalAlignment::Stretch);
        Canvas::SetZIndex(widget->host, 10000);
        Grid::SetColumn(widget->host, 0);
        Grid::SetColumnSpan(
            widget->host,
            std::max(1, static_cast<int>(root.ColumnDefinitions().Size())));
        Grid::SetRow(widget->host, 0);
        Grid::SetRowSpan(
            widget->host,
            std::max(1, static_cast<int>(root.RowDefinitions().Size())));

        widget->panel = Grid();
        widget->panel.IsHitTestVisible(false);
        widget->panel.HorizontalAlignment(HorizontalAlignment::Left);

        ColumnDefinition nameColumn;
        nameColumn.Width(GridLength{1, GridUnitType::Auto});
        ColumnDefinition percentColumn;
        percentColumn.Width(
            GridLength{kPercentColumnWidth, GridUnitType::Pixel});
        widget->panel.ColumnDefinitions().Append(nameColumn);
        widget->panel.ColumnDefinitions().Append(percentColumn);

        RowDefinition firstRow;
        firstRow.Height(GridLength{1, GridUnitType::Auto});
        RowDefinition secondRow;
        secondRow.Height(GridLength{1, GridUnitType::Auto});
        widget->panel.RowDefinitions().Append(firstRow);
        widget->panel.RowDefinitions().Append(secondRow);

        widget->name1 = CreateTextBlock();
        widget->percent1 = CreateTextBlock();
        widget->name2 = CreateTextBlock();
        widget->percent2 = CreateTextBlock();

        widget->percent1.TextAlignment(TextAlignment::Right);
        widget->percent2.TextAlignment(TextAlignment::Right);
        widget->percent1.Margin(Thickness{kColumnGap, 0, 0, 0});
        widget->percent2.Margin(Thickness{kColumnGap, 0, 0, 0});

        Grid::SetRow(widget->name1, 0);
        Grid::SetColumn(widget->name1, 0);
        Grid::SetRow(widget->percent1, 0);
        Grid::SetColumn(widget->percent1, 1);
        Grid::SetRow(widget->name2, 1);
        Grid::SetColumn(widget->name2, 0);
        Grid::SetRow(widget->percent2, 1);
        Grid::SetColumn(widget->percent2, 1);

        widget->panel.Children().Append(widget->name1);
        widget->panel.Children().Append(widget->percent1);
        widget->panel.Children().Append(widget->name2);
        widget->panel.Children().Append(widget->percent2);
        widget->host.Children().Append(widget->panel);

        auto settings = GetSettings();
        Canvas::SetLeft(
            widget->panel,
            static_cast<double>(settings ? settings->leftOffset : 8));
        Canvas::SetTop(widget->panel, 8.0);

        auto weak = std::weak_ptr<WidgetInstance>(widget);
        widget->sizeToken = widget->panel.SizeChanged(
            [weak](auto const&, auto const&) {
                if (auto instance = weak.lock()) {
                    auto current = GetSettings();
                    if (current) {
                        CenterWidget(*instance, *current);
                    }
                }
            });
        widget->rootSizeToken = widget->root.SizeChanged(
            [weak](auto const&, auto const&) {
                if (auto instance = weak.lock()) {
                    auto current = GetSettings();
                    if (current) {
                        CenterWidget(*instance, *current);
                    }
                }
            });
        widget->themeToken = widget->host.ActualThemeChanged(
            [weak](auto const&, auto const&) {
                if (auto instance = weak.lock()) {
                    auto current = GetSettings();
                    if (current) {
                        ApplyTheme(*instance, *current);
                    }
                }
            });
        widget->unloadedToken = widget->host.Unloaded(
            [weak](auto const&, auto const&) {
                if (auto instance = weak.lock()) {
                    RetireWidget(instance);
                }
            });

        children.Append(widget->host);

        widget->timer = DispatcherTimer();
        widget->timer.Interval(winrt::Windows::Foundation::TimeSpan{
            std::chrono::milliseconds(kUiUpdateIntervalMs)});
        widget->timerToken = widget->timer.Tick(
            [weak](auto const&, auto const&) {
                if (auto instance = weak.lock()) {
                    UpdateWidget(*instance);
                }
            });

        {
            std::lock_guard lock(g_widgetsMutex);
            g_widgets.push_back(widget);
        }

        settings = GetSettings();
        if (settings) {
            widget->lastSettingsRevision =
                g_settingsRevision.load(std::memory_order_relaxed);
            ApplySettingsToWidget(*widget, *settings);
        }

        Snapshot snapshot;
        {
            std::lock_guard lock(g_snapshotMutex);
            snapshot = g_snapshot;
        }
        widget->lastSequence = snapshot.sequence;
        ApplySnapshotToWidget(*widget, snapshot);

        widget->timer.Start();
        Wh_Log(L"ProcessMan widget injected");
    } catch (...) {
        Wh_Log(L"Injecting ProcessMan widget failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
}

struct ApplyExistingTaskbarContext {
    HWND window = nullptr;
    bool succeeded = false;
};

void ApplyExistingTaskbarOnThread(void* value) {
    auto& context = *static_cast<ApplyExistingTaskbarContext*>(value);
    auto frame = FindTaskbarFrameForWindow(context.window);
    if (!frame) {
        return;
    }
    InjectWidget(frame);
    context.succeeded = true;
}

BOOL CALLBACK EnumTaskbarWindows(HWND window, LPARAM value) {
    bool secondary = false;
    if (GetTaskbarKind(window, secondary)) {
        reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
    }
    return TRUE;
}

void ApplyExistingTaskbars() {
    if (g_unloading.load(std::memory_order_relaxed) ||
        !g_taskbarDllSymbolsHooked.load(std::memory_order_relaxed) ||
        !g_runOnTaskbarThreadMessage) {
        return;
    }

    std::vector<HWND> taskbarWindows;
    EnumWindows(EnumTaskbarWindows,
                reinterpret_cast<LPARAM>(&taskbarWindows));

    if (taskbarWindows.empty()) {
        Wh_Log(L"No taskbar windows found for existing frame injection");
        return;
    }
    Wh_Log(L"Trying to inject into %d existing taskbar window(s)",
           static_cast<int>(taskbarWindows.size()));

    for (HWND window : taskbarWindows) {
        ApplyExistingTaskbarContext context{window, false};
        RunOnTaskbarThread(window, ApplyExistingTaskbarOnThread, &context);
        if (!context.succeeded) {
            Wh_Log(L"An existing taskbar frame was not found");
        }
    }
}

void* WINAPI TaskbarFrameConstructorHook(void* pThis) {
    void* result = TaskbarFrameConstructor_Original(pThis);
    if (g_unloading.load(std::memory_order_relaxed) || !pThis) {
        return result;
    }

    try {
        FrameworkElement frame{nullptr};
        reinterpret_cast<::IUnknown**>(pThis)[1]->QueryInterface(
            winrt::guid_of<FrameworkElement>(), winrt::put_abi(frame));
        if (!frame) {
            return result;
        }

        auto token = frame.Loaded(
            [](IInspectable const& sender, RoutedEventArgs const&) {
                if (g_unloading.load(std::memory_order_relaxed)) {
                    return;
                }
                auto loadedFrame = sender.try_as<FrameworkElement>();
                if (loadedFrame) {
                    InjectWidget(loadedFrame);
                }
            });

        CoreDispatcher dispatcher{nullptr};
        try {
            dispatcher = frame.Dispatcher();
        } catch (...) {
        }

        std::lock_guard lock(g_frameHooksMutex);
        g_frameHooks.push_back({frame, token, dispatcher});
    } catch (...) {
        Wh_Log(L"TaskbarFrame constructor hook failed: %08X",
               static_cast<unsigned>(winrt::to_hresult()));
    }
    return result;
}

bool HookTaskbarFrame() {
    std::lock_guard lock(g_taskbarHookMutex);
    if (g_taskbarHooked.load(std::memory_order_relaxed)) {
        return true;
    }

    HMODULE modules[2] = {GetModuleHandleW(L"Taskbar.View.dll"),
                          GetModuleHandleW(L"ExplorerExtensions.dll")};

    for (HMODULE module : modules) {
        if (!module) {
            continue;
        }

        WindhawkUtils::SYMBOL_HOOK hooks[] = {
            {{LR"(public: __cdecl winrt::Taskbar::implementation::TaskbarFrame::TaskbarFrame(void))"},
             &TaskbarFrameConstructor_Original, TaskbarFrameConstructorHook},
        };

        if (WindhawkUtils::HookSymbols(module, hooks, std::size(hooks))) {
            g_taskbarHooked = true;
            Wh_Log(L"TaskbarFrame constructor hook installed");
            return true;
        }
    }

    Wh_Log(L"TaskbarFrame symbols were not found");
    return false;
}

bool HookTaskbarDllSymbols() {
    if (g_taskbarDllSymbolsHooked.load(std::memory_order_relaxed)) {
        return true;
    }

    HMODULE module = GetModuleHandleW(L"taskbar.dll");
    if (!module) {
        module = LoadLibraryExW(L"taskbar.dll", nullptr,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!module) {
        Wh_Log(L"taskbar.dll was not found");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &g_ctaskBandSiteVftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &g_csecondaryTaskBandSiteVftable, nullptr, true},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &g_getPrimaryTaskbarHost},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &g_getSecondaryTaskbarHost, nullptr, true},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
         &g_taskbarHostFrameHeight},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
         &g_refCountDecref},
    };

    if (!WindhawkUtils::HookSymbols(module, hooks, std::size(hooks))) {
        Wh_Log(L"taskbar.dll symbols were not found");
        return false;
    }

    g_taskbarDllSymbolsHooked = true;
    Wh_Log(L"taskbar.dll frame lookup symbols installed: vftable=%p, "
           L"getHost=%p, frameHeight=%p, decref=%p",
           g_ctaskBandSiteVftable, g_getPrimaryTaskbarHost,
           g_taskbarHostFrameHeight, g_refCountDecref);
    return true;
}

HMODULE GetTaskbarViewModule() {
    if (HMODULE module = GetModuleHandleW(L"Taskbar.View.dll")) {
        return module;
    }
    return GetModuleHandleW(L"ExplorerExtensions.dll");
}

bool IsTaskbarViewModule(HMODULE module) {
    if (!module) {
        return false;
    }

    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length) {
        return false;
    }

    const wchar_t* name = path;
    if (const wchar_t* slash = wcsrchr(path, L'\\')) {
        name = slash + 1;
    }
    return _wcsicmp(name, L"Taskbar.View.dll") == 0 ||
           _wcsicmp(name, L"ExplorerExtensions.dll") == 0;
}

HMODULE WINAPI LoadLibraryExWHook(LPCWSTR fileName,
                                 HANDLE file,
                                 DWORD flags) {
    HMODULE module = LoadLibraryExW_Original(fileName, file, flags);
    DWORD lastError = GetLastError();

    if (module && !g_taskbarHooked.load(std::memory_order_relaxed) &&
        IsTaskbarViewModule(module)) {
        if (HookTaskbarFrame()) {
            Wh_ApplyHookOperations();
        }
    }

    SetLastError(lastError);
    return module;
}

void RemoveAllWidgets() {
    std::vector<std::shared_ptr<WidgetInstance>> widgets;
    {
        std::lock_guard lock(g_widgetsMutex);
        widgets.swap(g_widgets);
    }

    for (const auto& widget : widgets) {
        if (widget->dispatcher) {
            RunOnDispatcher(widget->dispatcher,
                            [widget] { TeardownWidget(*widget); });
        } else {
            Wh_Log(L"Cannot tear down a widget without a dispatcher");
        }
    }
    widgets.clear();
}

void RemoveFrameHooks() {
    std::vector<FrameHook> hooks;
    {
        std::lock_guard lock(g_frameHooksMutex);
        hooks.swap(g_frameHooks);
    }

    for (const auto& hook : hooks) {
        if (!hook.frame || !hook.token.value) {
            continue;
        }

        CoreDispatcher dispatcher = hook.dispatcher;
        if (!dispatcher) {
            try {
                dispatcher = hook.frame.Dispatcher();
            } catch (...) {
            }
        }
        if (!dispatcher) {
            continue;
        }

        auto frame = hook.frame;
        auto token = hook.token;
        RunOnDispatcher(dispatcher, [frame, token] {
            try {
                frame.Loaded(token);
            } catch (...) {
            }
        });
    }
    hooks.clear();
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    g_unloading = false;
    LoadSettings();

    g_runOnTaskbarThreadMessage = RegisterWindowMessageW(
        L"ProcessMan_RunOnTaskbarThread");
    if (!g_runOnTaskbarThreadMessage) {
        Wh_Log(L"Registering the taskbar thread message failed");
    }

    HookTaskbarDllSymbols();

    HMODULE taskbarModule = GetTaskbarViewModule();
    if (taskbarModule) {
        if (!HookTaskbarFrame()) {
            Wh_Log(L"Taskbar.View symbols are unavailable");
            return FALSE;
        }
    } else {
        HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
        if (!kernel) {
            kernel = GetModuleHandleW(L"kernel32.dll");
        }
        auto loadLibraryEx = kernel
                                 ? reinterpret_cast<LoadLibraryExW_t>(
                                       GetProcAddress(kernel,
                                                      "LoadLibraryExW"))
                                 : nullptr;
        if (!loadLibraryEx ||
            !WindhawkUtils::SetFunctionHook(loadLibraryEx, LoadLibraryExWHook,
                                            &LoadLibraryExW_Original)) {
            Wh_Log(L"Hooking LoadLibraryExW failed");
            return FALSE;
        }
        Wh_Log(L"Waiting for Taskbar.View.dll to load");
    }

    StartSampler();
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    if (!g_taskbarHooked.load(std::memory_order_relaxed)) {
        if (GetTaskbarViewModule()) {
            if (HookTaskbarFrame()) {
                Wh_ApplyHookOperations();
            }
        }
    }

    ApplyExistingTaskbars();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");
    LoadSettings();

    bool hasWidgets = false;
    {
        std::lock_guard lock(g_widgetsMutex);
        hasWidgets = !g_widgets.empty();
    }
    if (!hasWidgets) {
        ApplyExistingTaskbars();
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    g_unloading = true;
    StopSampler();
    RemoveAllWidgets();
    RemoveFrameHooks();
}

void Wh_ModUninit() {
    Wh_Log(L">");
}
