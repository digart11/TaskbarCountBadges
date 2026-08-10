// ==WindhawkMod==
// @id              taskbar-count-badges
// @name            Taskbar Count Badges
// @description     Show count badges on taskbar app buttons.
// @version         0.1.1
// @author          digART
// @github          https://github.com/digart11
// @license         GPL-3.0
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

#include <windows.h>
#include <winstring.h>
#include <windhawk_utils.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace winrt::Windows::UI::Xaml;

#if __cplusplus < 202302L
DECLARE_HANDLE(CO_MTA_USAGE_COOKIE);
WINOLEAPI CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE* cookie);
WINOLEAPI CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE cookie);
#endif

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

bool g_isTaskbarProcess = false;

std::atomic<bool> g_taskbarViewHooked = false;
std::atomic<bool> g_unloading = false;
std::atomic<bool> g_recountQueued = false;

winrt::Windows::UI::Core::CoreDispatcher
    g_taskbarDispatcher{nullptr};

struct TrackedButton {
    void* identity;
    std::wstring automationId;
    winrt::weak_ref<FrameworkElement> element;
};

std::vector<TrackedButton> g_trackedButtons;

// Always contains REAL HWND-derived counts.
// ViewModelCount is used only as a change notification.
std::unordered_map<std::wstring, unsigned int>
    g_appCounts;

void* g_ITaskbarAppItemViewModelGuid = nullptr;

// -----------------------------------------------------------------------------
// taskbar.dll symbols used to access the existing taskbar XamlRoot
// -----------------------------------------------------------------------------

void* g_CTaskBand_ITaskListWndSite_vftable = nullptr;

using CTaskBand_GetTaskbarHost_t =
    void*(__cdecl*)(
        void* pThis,
        void** result);

CTaskBand_GetTaskbarHost_t
    g_CTaskBand_GetTaskbarHost = nullptr;

void* g_TaskbarHost_FrameHeight = nullptr;

using RefCount_Decref_t =
    void(__cdecl*)(
        void* pThis);

RefCount_Decref_t
    g_RefCount_Decref = nullptr;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

std::wstring NormalizeId(
    std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t c) {
            return static_cast<wchar_t>(
                std::towlower(c));
        });

    return value;
}

// -----------------------------------------------------------------------------
// Main taskbar HWND
// -----------------------------------------------------------------------------

HWND FindCurrentProcessTaskbarWnd() {
    HWND result = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            WCHAR className[64] = {};

            GetWindowThreadProcessId(
                hWnd,
                &pid);

            if (pid !=
                GetCurrentProcessId()) {
                return TRUE;
            }

            if (!GetClassNameW(
                    hWnd,
                    className,
                    ARRAYSIZE(className))) {
                return TRUE;
            }

            if (_wcsicmp(
                    className,
                    L"Shell_TrayWnd") != 0) {
                return TRUE;
            }

            *reinterpret_cast<HWND*>(
                param) = hWnd;

            return FALSE;
        },
        reinterpret_cast<LPARAM>(
            &result));

    return result;
}

// -----------------------------------------------------------------------------
// Taskbar.View module
// -----------------------------------------------------------------------------

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE module =
        GetModuleHandleW(
            L"Taskbar.View.dll");

    if (!module) {
        module =
            GetModuleHandleW(
                L"ExplorerExtensions.dll");
    }

    return module;
}

// -----------------------------------------------------------------------------
// XAML helpers
// -----------------------------------------------------------------------------

FrameworkElement FindChildByName(
    FrameworkElement element,
    PCWSTR name) {
    int count =
        Media::VisualTreeHelper::
            GetChildrenCount(
                element);

    for (int i = 0;
         i < count;
         i++) {
        auto child =
            Media::VisualTreeHelper::
                GetChild(
                    element,
                    i)
                    .try_as<
                        FrameworkElement>();

        if (!child) {
            continue;
        }

        if (child.Name() ==
            name) {
            return child;
        }

        auto result =
            FindChildByName(
                child,
                name);

        if (result) {
            return result;
        }
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// Badge
// -----------------------------------------------------------------------------

void UpdateCountBadge(
    FrameworkElement taskListButton,
    unsigned int count) {
    if (g_unloading) {
        return;
    }

    auto iconPanelElement =
        FindChildByName(
            taskListButton,
            L"IconPanel");

    if (!iconPanelElement) {
        return;
    }

    auto iconPanel =
        iconPanelElement
            .try_as<
                Controls::Panel>();

    if (!iconPanel) {
        return;
    }

    auto badge =
        FindChildByName(
            iconPanelElement,
            L"WindhawkCountBadge")
            .try_as<
                Controls::Border>();

    // No badge for zero or one window.
    if (count <= 1) {
        if (badge) {
            badge.Visibility(
                Visibility::Collapsed);
        }

        return;
    }

    if (!badge) {
        PCWSTR xaml =
            LR"(
<Border
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    Name="WindhawkCountBadge"
    Width="16"
    Height="16"
    CornerRadius="8"
    Background="#E6000000"
    HorizontalAlignment="Right"
    VerticalAlignment="Top"
    Margin="0,-2,-2,0">

    <TextBlock
        Foreground="White"
        FontSize="10"
        FontWeight="SemiBold"
        HorizontalAlignment="Center"
        VerticalAlignment="Center"
        TextAlignment="Center"/>

</Border>
)";

        badge =
            Markup::XamlReader::
                Load(xaml)
                .as<
                    Controls::Border>();

        badge.IsHitTestVisible(
            false);

        Controls::Canvas::
            SetZIndex(
                badge,
                100);

        iconPanel
            .Children()
            .Append(
                badge);
    }

    auto text =
        badge.Child()
            .try_as<
                Controls::TextBlock>();

    if (text) {
        std::wstring value =
            std::to_wstring(
                count);

        if (text.Text() !=
            value) {
            text.Text(
                value);
        }
    }

    badge.Visibility(
        Visibility::Visible);
}

// -----------------------------------------------------------------------------
// Tracked buttons
// -----------------------------------------------------------------------------

void PruneDeadTrackedButtons() {
    for (auto it =
             g_trackedButtons.begin();
         it !=
         g_trackedButtons.end();) {
        if (!it->element.get()) {
            it =
                g_trackedButtons.erase(
                    it);
        } else {
            ++it;
        }
    }
}

void TrackTaskbarButton(
    FrameworkElement element) {
    auto unknown =
        element.as<
            winrt::Windows::
                Foundation::IUnknown>();

    void* identity =
        winrt::get_abi(
            unknown);

    auto automationId =
        winrt::Windows::UI::
            Xaml::Automation::
                AutomationProperties::
                    GetAutomationId(
                        element);

    if (automationId.empty()) {
        return;
    }

    auto existing =
        std::find_if(
            g_trackedButtons.begin(),
            g_trackedButtons.end(),
            [identity](
                const TrackedButton& item) {
                return item.identity ==
                       identity;
            });

    if (existing !=
        g_trackedButtons.end()) {
        existing->automationId =
            automationId.c_str();

        return;
    }

    g_trackedButtons.push_back(
        {
            identity,
            automationId.c_str(),
            winrt::make_weak(
                element),
        });
}

// -----------------------------------------------------------------------------
// Apply cached REAL HWND count
// -----------------------------------------------------------------------------

void ApplyCountToButton(
    FrameworkElement element) {
    auto automationId =
        winrt::Windows::UI::
            Xaml::Automation::
                AutomationProperties::
                    GetAutomationId(
                        element);

    if (automationId.empty()) {
        return;
    }

    std::wstring key =
        NormalizeId(
            automationId.c_str());

    unsigned int count = 0;

    auto it =
        g_appCounts.find(
            key);

    if (it !=
        g_appCounts.end()) {
        count =
            it->second;
    }

    UpdateCountBadge(
        element,
        count);
}

void RefreshChangedTrackedButtons(
    const std::unordered_set<
        std::wstring>& changedIds) {
    PruneDeadTrackedButtons();

    for (auto& item :
         g_trackedButtons) {
        std::wstring key =
            NormalizeId(
                item.automationId);

        if (!changedIds.contains(
                key)) {
            continue;
        }

        auto element =
            item.element.get();

        if (!element) {
            continue;
        }

        ApplyCountToButton(
            element);
    }
}

// -----------------------------------------------------------------------------
// Existing XAML tree enumeration
// -----------------------------------------------------------------------------

int EnumerateExistingTaskbarButtons(
    FrameworkElement element) {
    if (!element) {
        return 0;
    }

    int found = 0;

    try {
        if (element.Name() ==
            L"TaskListButton") {
            if (!g_taskbarDispatcher) {
                g_taskbarDispatcher =
                    element.Dispatcher();
            }

            TrackTaskbarButton(
                element);

            ApplyCountToButton(
                element);

            found++;
        }

        int children =
            Media::VisualTreeHelper::
                GetChildrenCount(
                    element);

        for (int i = 0;
             i < children;
             i++) {
            auto child =
                Media::VisualTreeHelper::
                    GetChild(
                        element,
                        i)
                    .try_as<
                        FrameworkElement>();

            if (!child) {
                continue;
            }

            found +=
                EnumerateExistingTaskbarButtons(
                    child);
        }
    } catch (...) {
    }

    return found;
}

// -----------------------------------------------------------------------------
// Existing taskbar XamlRoot
// -----------------------------------------------------------------------------

XamlRoot GetTaskbarXamlRoot(
    HWND taskbarWnd) {
    if (!taskbarWnd ||
        !g_CTaskBand_ITaskListWndSite_vftable ||
        !g_CTaskBand_GetTaskbarHost ||
        !g_TaskbarHost_FrameHeight ||
        !g_RefCount_Decref) {
        return nullptr;
    }

    HWND taskBandWnd =
        reinterpret_cast<HWND>(
            GetPropW(
                taskbarWnd,
                L"TaskbandHWND"));

    if (!taskBandWnd) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"TaskbandHWND not found");

        return nullptr;
    }

    void* taskBand =
        reinterpret_cast<void*>(
            GetWindowLongPtrW(
                taskBandWnd,
                0));

    if (!taskBand) {
        return nullptr;
    }

    void* site = taskBand;
    bool siteFound = false;

    for (int i = 0;
         i < 20;
         i++) {
        if (*reinterpret_cast<void**>(
                site) ==
            g_CTaskBand_ITaskListWndSite_vftable) {
            siteFound = true;
            break;
        }

        site =
            reinterpret_cast<void**>(
                site) +
            1;
    }

    if (!siteFound) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"ITaskListWndSite not found");

        return nullptr;
    }

    void* hostShared[2] = {};

    g_CTaskBand_GetTaskbarHost(
        site,
        hostShared);

    if (!hostShared[0]) {
        if (hostShared[1]) {
            g_RefCount_Decref(
                hostShared[1]);
        }

        return nullptr;
    }

    size_t elementOffset = 0;

#if defined(_M_X64)
    const BYTE* code =
        reinterpret_cast<
            const BYTE*>(
            g_TaskbarHost_FrameHeight);

    if (code[0] == 0x48 &&
        code[1] == 0x83 &&
        code[2] == 0xEC &&
        code[4] == 0x48 &&
        code[5] == 0x83 &&
        code[6] == 0xC1 &&
        code[7] <= 0x7F) {
        elementOffset =
            code[7];
    } else {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"couldn't detect TaskbarHost "
            L"XAML offset");

        if (hostShared[1]) {
            g_RefCount_Decref(
                hostShared[1]);
        }

        return nullptr;
    }
#endif

    IUnknown* xamlUnknown =
        *reinterpret_cast<IUnknown**>(
            reinterpret_cast<BYTE*>(
                hostShared[0]) +
            elementOffset);

    FrameworkElement taskbarElement =
        nullptr;

    if (xamlUnknown) {
        xamlUnknown->QueryInterface(
            winrt::guid_of<
                FrameworkElement>(),
            winrt::put_abi(
                taskbarElement));
    }

    XamlRoot result =
        taskbarElement
            ? taskbarElement.XamlRoot()
            : nullptr;

    if (hostShared[1]) {
        g_RefCount_Decref(
            hostShared[1]);
    }

    return result;
}

// -----------------------------------------------------------------------------
// Run synchronously on taskbar UI thread
// -----------------------------------------------------------------------------

using TaskbarThreadProc =
    void(WINAPI*)(
        void* parameter);

bool RunOnTaskbarThread(
    HWND hWnd,
    TaskbarThreadProc proc,
    void* parameter) {
    if (!hWnd ||
        !proc) {
        return false;
    }

    DWORD threadId =
        GetWindowThreadProcessId(
            hWnd,
            nullptr);

    if (!threadId) {
        return false;
    }

    if (threadId ==
        GetCurrentThreadId()) {
        proc(parameter);

        return true;
    }

    static UINT message =
        RegisterWindowMessageW(
            L"Windhawk_TaskbarCountBadges_"
            L"RunOnTaskbarThread");

    struct Request {
        TaskbarThreadProc proc;
        void* parameter;
    };

    HHOOK hook =
        SetWindowsHookExW(
            WH_CALLWNDPROC,
            [](int code,
               WPARAM wParam,
               LPARAM lParam) -> LRESULT {
                if (code ==
                    HC_ACTION) {
                    auto data =
                        reinterpret_cast<
                            CWPSTRUCT*>(
                            lParam);

                    if (data->message ==
                        message) {
                        auto request =
                            reinterpret_cast<
                                Request*>(
                                data->lParam);

                        if (request &&
                            request->proc) {
                            request->proc(
                                request->parameter);
                        }
                    }
                }

                return CallNextHookEx(
                    nullptr,
                    code,
                    wParam,
                    lParam);
            },
            nullptr,
            threadId);

    if (!hook) {
        return false;
    }

    Request request{
        proc,
        parameter,
    };

    SendMessageW(
        hWnd,
        message,
        0,
        reinterpret_cast<LPARAM>(
            &request));

    UnhookWindowsHookEx(
        hook);

    return true;
}

// -----------------------------------------------------------------------------
// App resolver
// -----------------------------------------------------------------------------

constexpr winrt::guid
    CLSID_StartMenuCacheAndAppResolver{
        0x660B90C8,
        0x73A9,
        0x4B58,
        {
            0x8C, 0xAE, 0x35, 0x5B,
            0x7F, 0x55, 0x34, 0x1B,
        }};

constexpr winrt::guid
    IID_IAppResolver_8{
        0xDE25675A,
        0x72DE,
        0x44B4,
        {
            0x93, 0x73, 0x05, 0x17,
            0x04, 0x50, 0xC1, 0x40,
        }};

struct IAppResolver_8 :
    public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForShortcut() = 0;

    virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForShortcutObject() = 0;

    virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForWindow(
            HWND hWnd,
            WCHAR** appId,
            void* unknown1,
            void* unknown2,
            void* unknown3) = 0;

    virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForProcess(
            DWORD processId,
            WCHAR** appId,
            void* unknown1,
            void* unknown2,
            void* unknown3) = 0;
};

// -----------------------------------------------------------------------------
// Which HWNDs count as taskbar windows
// -----------------------------------------------------------------------------

bool IsCountableWindow(
    HWND hWnd) {
    if (!hWnd ||
        !IsWindowVisible(
            hWnd)) {
        return false;
    }

    WCHAR className[128] = {};

    if (GetClassNameW(
            hWnd,
            className,
            ARRAYSIZE(className))) {
        if (_wcsicmp(
                className,
                L"Shell_TrayWnd") == 0 ||
            _wcsicmp(
                className,
                L"Shell_SecondaryTrayWnd") == 0 ||
            _wcsicmp(
                className,
                L"Progman") == 0 ||
            _wcsicmp(
                className,
                L"WorkerW") == 0) {
            return false;
        }
    }

    LONG_PTR exStyle =
        GetWindowLongPtrW(
            hWnd,
            GWL_EXSTYLE);

    if ((exStyle &
         WS_EX_TOOLWINDOW) &&
        !(exStyle &
          WS_EX_APPWINDOW)) {
        return false;
    }

    HWND owner =
        GetWindow(
            hWnd,
            GW_OWNER);

    if (owner &&
        !(exStyle &
          WS_EX_APPWINDOW)) {
        return false;
    }

    HMODULE dwmApi =
        GetModuleHandleW(
            L"dwmapi.dll");

    if (dwmApi) {
        using DwmGetWindowAttribute_t =
            HRESULT(WINAPI*)(
                HWND,
                DWORD,
                PVOID,
                DWORD);

        auto getAttribute =
            reinterpret_cast<
                DwmGetWindowAttribute_t>(
                GetProcAddress(
                    dwmApi,
                    "DwmGetWindowAttribute"));

        if (getAttribute) {
            DWORD cloaked = 0;

            // DWMWA_CLOAKED = 14
            if (SUCCEEDED(
                    getAttribute(
                        hWnd,
                        14,
                        &cloaked,
                        sizeof(cloaked))) &&
                cloaked) {
                return false;
            }
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Build real HWND/AppID count map
// -----------------------------------------------------------------------------

struct WindowEnumContext {
    IAppResolver_8* resolver;

    std::unordered_map<
        std::wstring,
        unsigned int>* counts;
};

BOOL CALLBACK EnumCountableWindows(
    HWND hWnd,
    LPARAM param) {
    if (!IsCountableWindow(
            hWnd)) {
        return TRUE;
    }

    auto context =
        reinterpret_cast<
            WindowEnumContext*>(
            param);

    if (!context ||
        !context->resolver ||
        !context->counts) {
        return TRUE;
    }

    WCHAR* appId = nullptr;

    HRESULT hr =
        context->resolver
            ->GetAppIDForWindow(
                hWnd,
                &appId,
                nullptr,
                nullptr,
                nullptr);

    if (FAILED(hr) ||
        !appId ||
        !*appId) {
        if (appId) {
            CoTaskMemFree(
                appId);
        }

        return TRUE;
    }

    std::wstring key =
        NormalizeId(
            L"Appid: " +
            std::wstring(
                appId));

    CoTaskMemFree(
        appId);

    (*context->counts)[key]++;

    return TRUE;
}

// -----------------------------------------------------------------------------
// Rebuild real counts and report only changed AppIDs
// -----------------------------------------------------------------------------

bool BuildRealWindowCounts(
    std::unordered_set<
        std::wstring>* changedIds,
    bool initialBuild) {
    CO_MTA_USAGE_COOKIE cookie{};

    bool mta =
        SUCCEEDED(
            CoIncrementMTAUsage(
                &cookie));

    winrt::com_ptr<IAppResolver_8>
        resolver;

    HRESULT hr =
        CoCreateInstance(
            CLSID_StartMenuCacheAndAppResolver,
            nullptr,
            CLSCTX_INPROC_SERVER |
                CLSCTX_INPROC_HANDLER,
            IID_IAppResolver_8,
            resolver.put_void());

    if (FAILED(hr) ||
        !resolver) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"AppResolver failed "
            L"hr=0x%08X",
            static_cast<unsigned int>(
                hr));

        if (mta) {
            CoDecrementMTAUsage(
                cookie);
        }

        return false;
    }

    std::unordered_map<
        std::wstring,
        unsigned int>
        newCounts;

    WindowEnumContext context{
        resolver.get(),
        &newCounts,
    };

    EnumWindows(
        EnumCountableWindows,
        reinterpret_cast<LPARAM>(
            &context));

    resolver = nullptr;

    if (mta) {
        CoDecrementMTAUsage(
            cookie);
    }

    if (initialBuild) {
        g_appCounts =
            std::move(
                newCounts);

        size_t multiWindowApps = 0;

        for (const auto& pair :
             g_appCounts) {
            if (pair.second > 1) {
                multiWindowApps++;
            }
        }

        Wh_Log(
            L"Taskbar Count Badges: "
            L"initial window counts ready "
            L"apps=%zu multi=%zu",
            g_appCounts.size(),
            multiWindowApps);

        return true;
    }

    if (!changedIds) {
        return false;
    }

    changedIds->clear();

    // Existing/changed/removed entries.
    for (const auto& oldPair :
         g_appCounts) {
        auto newIt =
            newCounts.find(
                oldPair.first);

        unsigned int newCount =
            newIt !=
                    newCounts.end()
                ? newIt->second
                : 0;

        if (oldPair.second ==
            newCount) {
            continue;
        }

        changedIds->insert(
            oldPair.first);

        Wh_Log(
            L"Taskbar Count Badges: "
            L"count changed id=\"%s\" "
            L"%u -> %u",
            oldPair.first.c_str(),
            oldPair.second,
            newCount);
    }

    // Completely new entries.
    for (const auto& newPair :
         newCounts) {
        if (g_appCounts.find(
                newPair.first) !=
            g_appCounts.end()) {
            continue;
        }

        changedIds->insert(
            newPair.first);

        Wh_Log(
            L"Taskbar Count Badges: "
            L"count changed id=\"%s\" "
            L"0 -> %u",
            newPair.first.c_str(),
            newPair.second);
    }

    g_appCounts =
        std::move(
            newCounts);

    return true;
}

// -----------------------------------------------------------------------------
// Initial existing taskbar button enumeration
// -----------------------------------------------------------------------------

void WINAPI
InitializeExistingButtonsOnTaskbarThread(
    void* parameter) {
    HWND taskbarWnd =
        reinterpret_cast<HWND>(
            parameter);

    auto xamlRoot =
        GetTaskbarXamlRoot(
            taskbarWnd);

    if (!xamlRoot) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"failed to get taskbar XamlRoot");

        return;
    }

    auto content =
        xamlRoot.Content()
            .try_as<
                FrameworkElement>();

    if (!content) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"XamlRoot content unavailable");

        return;
    }

    if (!g_taskbarDispatcher) {
        g_taskbarDispatcher =
            content.Dispatcher();
    }

    int buttonCount =
        EnumerateExistingTaskbarButtons(
            content);

    Wh_Log(
        L"Taskbar Count Badges: "
        L"initial taskbar buttons=%d",
        buttonCount);
}

// -----------------------------------------------------------------------------
// Recount queue
//
// ViewModelCount can fire many times in one taskbar update.
// All calls collapse into one real HWND recount.
// -----------------------------------------------------------------------------

void QueueRealWindowRecount() {
    if (g_unloading ||
        !g_taskbarDispatcher) {
        return;
    }

    if (g_recountQueued.exchange(
            true)) {
        return;
    }

    try {
        g_taskbarDispatcher.RunAsync(
            winrt::Windows::UI::Core::
                CoreDispatcherPriority::
                    Normal,
            []() {
                g_recountQueued =
                    false;

                if (g_unloading) {
                    return;
                }

                std::unordered_set<
                    std::wstring>
                    changedIds;

                if (!BuildRealWindowCounts(
                        &changedIds,
                        false)) {
                    return;
                }

                if (changedIds.empty()) {
                    return;
                }

                RefreshChangedTrackedButtons(
                    changedIds);
            });
    } catch (...) {
        g_recountQueued =
            false;

        Wh_Log(
            L"Taskbar Count Badges: "
            L"failed to queue HWND recount");
    }
}

// -----------------------------------------------------------------------------
// TaskListButton::UpdateVisualStates
//
// Handles newly-created or recycled taskbar buttons.
// Startup does NOT depend on this hook.
// -----------------------------------------------------------------------------

using TaskListButton_UpdateVisualStates_t =
    void(__cdecl*)(
        void* pThis);

TaskListButton_UpdateVisualStates_t
    TaskListButton_UpdateVisualStates_Original =
        nullptr;

void __cdecl
TaskListButton_UpdateVisualStates_Hook(
    void* pThis) {
    TaskListButton_UpdateVisualStates_Original(
        pThis);

    if (g_unloading) {
        return;
    }

    try {
        void* taskListButtonIUnknownPtr =
            reinterpret_cast<void**>(
                pThis) +
            3;

        winrt::Windows::
            Foundation::IUnknown
                taskListButtonIUnknown;

        winrt::copy_from_abi(
            taskListButtonIUnknown,
            taskListButtonIUnknownPtr);

        auto element =
            taskListButtonIUnknown
                .try_as<
                    FrameworkElement>();

        if (!element) {
            return;
        }

        if (!g_taskbarDispatcher) {
            g_taskbarDispatcher =
                element.Dispatcher();
        }

        TrackTaskbarButton(
            element);

        ApplyCountToButton(
            element);
    } catch (...) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"TaskListButton update failed");
    }
}

// -----------------------------------------------------------------------------
// TaskListGroupViewModel AutomationId
// -----------------------------------------------------------------------------

using GetAutomationId_t =
    HRESULT(WINAPI*)(
        void* pThis,
        HSTRING* automationId);

GetAutomationId_t
    GetAutomationId_Original =
        nullptr;

bool GetGroupAutomationId(
    void* groupInterface,
    std::wstring* result) {
    if (!groupInterface ||
        !result ||
        !g_ITaskbarAppItemViewModelGuid ||
        !GetAutomationId_Original) {
        return false;
    }

    const GUID* appItemGuid =
        reinterpret_cast<
            const GUID*>(
            g_ITaskbarAppItemViewModelGuid);

    void* appItemInterface =
        nullptr;

    HRESULT hr =
        reinterpret_cast<IUnknown*>(
            groupInterface)
            ->QueryInterface(
                *appItemGuid,
                &appItemInterface);

    if (FAILED(hr) ||
        !appItemInterface) {
        return false;
    }

    HSTRING automationId =
        nullptr;

    hr =
        GetAutomationId_Original(
            appItemInterface,
            &automationId);

    reinterpret_cast<IUnknown*>(
        appItemInterface)
        ->Release();

    if (FAILED(hr) ||
        !automationId) {
        return false;
    }

    UINT32 length = 0;

    PCWSTR text =
        WindowsGetStringRawBuffer(
            automationId,
            &length);

    if (text) {
        result->assign(
            text,
            length);
    }

    WindowsDeleteString(
        automationId);

    return !result->empty();
}

// -----------------------------------------------------------------------------
// ViewModelCount
//
// IMPORTANT:
//
// The returned ViewModelCount is NOT used as the displayed count.
// This hook is only a fast notification that the taskbar's app/window state
// changed.
//
// Multiple calls are collapsed by QueueRealWindowRecount().
// -----------------------------------------------------------------------------

using GetViewModelCount_t =
    HRESULT(WINAPI*)(
        void* pThis,
        unsigned int* count);

GetViewModelCount_t
    GetViewModelCount_Original =
        nullptr;

HRESULT WINAPI
GetViewModelCount_Hook(
    void* pThis,
    unsigned int* count) {
    HRESULT hr =
        GetViewModelCount_Original(
            pThis,
            count);

    if (FAILED(hr) ||
        !count ||
        g_unloading) {
        return hr;
    }

    QueueRealWindowRecount();

    return hr;
}

// -----------------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------------

void CleanupOnTaskbarThread() {
    PruneDeadTrackedButtons();

    for (auto& item :
         g_trackedButtons) {
        auto element =
            item.element.get();

        if (!element) {
            continue;
        }

        auto badge =
            FindChildByName(
                element,
                L"WindhawkCountBadge")
                .try_as<
                    Controls::Border>();

        if (badge) {
            badge.Visibility(
                Visibility::Collapsed);
        }
    }

    g_trackedButtons.clear();

    Wh_Log(
        L"Taskbar Count Badges: "
        L"cleanup complete");
}

bool CleanupSynchronously() {
    if (!g_taskbarDispatcher) {
        return true;
    }

    try {
        if (g_taskbarDispatcher
                .HasThreadAccess()) {
            CleanupOnTaskbarThread();

            return true;
        }

        auto operation =
            g_taskbarDispatcher
                .RunAsync(
                    winrt::Windows::UI::
                        Core::
                            CoreDispatcherPriority::
                                High,
                    []() {
                        CleanupOnTaskbarThread();
                    });

        operation.get();

        return true;
    } catch (...) {
        return false;
    }
}

// -----------------------------------------------------------------------------
// Taskbar.View hooks
// -----------------------------------------------------------------------------

bool HookTaskbarView(
    HMODULE module) {
    if (g_taskbarViewHooked
            .exchange(true)) {
        return true;
    }

    WindhawkUtils::SYMBOL_HOOK
        hooks[] = {
            {
                {
                    LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))",
                },
                &TaskListButton_UpdateVisualStates_Original,
                TaskListButton_UpdateVisualStates_Hook,
            },
            {
                {
                    LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListGroupViewModel,struct winrt::Taskbar::ITaskListGroupViewModel>::get_ViewModelCount(unsigned int *))",
                },
                &GetViewModelCount_Original,
                GetViewModelCount_Hook,
            },
            {
                {
                    LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListGroupViewModel,struct winrt::Taskbar::ITaskbarAppItemViewModel>::get_AutomationId(void * *))",
                },
                &GetAutomationId_Original,
            },
            {
                {
                    LR"(struct guid::guid const winrt::impl::guid_v<struct winrt::Taskbar::ITaskbarAppItemViewModel>)",
                },
                &g_ITaskbarAppItemViewModelGuid,
            },
        };

    if (!WindhawkUtils::
            HookSymbols(
                module,
                hooks,
                ARRAYSIZE(
                    hooks))) {
        g_taskbarViewHooked =
            false;

        Wh_Log(
            L"Taskbar Count Badges: "
            L"Taskbar.View hooks failed");

        return false;
    }

    Wh_Log(
        L"Taskbar Count Badges: "
        L"Taskbar.View hooks installed");

    return true;
}

// -----------------------------------------------------------------------------
// taskbar.dll symbols for XamlRoot access
// -----------------------------------------------------------------------------

bool HookTaskbarDll() {
    HMODULE module =
        LoadLibraryExW(
            L"taskbar.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"taskbar.dll load failed");

        return false;
    }

    WindhawkUtils::SYMBOL_HOOK
        hooks[] = {
            {
                {
                    LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})",
                },
                &g_CTaskBand_ITaskListWndSite_vftable,
            },
            {
                {
                    LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )",
                },
                &g_CTaskBand_GetTaskbarHost,
            },
            {
                {
                    LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )",
                },
                &g_TaskbarHost_FrameHeight,
            },
            {
                {
                    LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))",
                },
                &g_RefCount_Decref,
            },
        };

    if (!WindhawkUtils::
            HookSymbols(
                module,
                hooks,
                ARRAYSIZE(
                    hooks))) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"taskbar.dll symbols failed");

        return false;
    }

    Wh_Log(
        L"Taskbar Count Badges: "
        L"taskbar XamlRoot symbols installed");

    return true;
}

// -----------------------------------------------------------------------------
// Late Taskbar.View loading
// -----------------------------------------------------------------------------

using LoadLibraryExW_t =
    decltype(
        &LoadLibraryExW);

LoadLibraryExW_t
    LoadLibraryExW_Original =
        nullptr;

HMODULE WINAPI
LoadLibraryExW_Hook(
    LPCWSTR fileName,
    HANDLE file,
    DWORD flags) {
    HMODULE module =
        LoadLibraryExW_Original(
            fileName,
            file,
            flags);

    if (module &&
        g_isTaskbarProcess &&
        !g_taskbarViewHooked) {
        HMODULE taskbarView =
            GetTaskbarViewModuleHandle();

        if (taskbarView &&
            taskbarView ==
                module) {
            if (HookTaskbarView(
                    taskbarView)) {
                Wh_ApplyHookOperations();
            }
        }
    }

    return module;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

BOOL Wh_ModInit() {
    g_unloading =
        false;

    g_recountQueued =
        false;

    g_isTaskbarProcess =
        FindCurrentProcessTaskbarWnd() !=
        nullptr;

    Wh_Log(
        L"Taskbar Count Badges: "
        L"init PID=%lu taskbar=%s",
        GetCurrentProcessId(),
        g_isTaskbarProcess
            ? L"YES"
            : L"NO");

    if (!g_isTaskbarProcess) {
        return TRUE;
    }

    if (!HookTaskbarDll()) {
        return FALSE;
    }

    if (HMODULE module =
            GetTaskbarViewModuleHandle()) {
        if (!HookTaskbarView(
                module)) {
            return FALSE;
        }

        return TRUE;
    }

    HMODULE kernelBase =
        GetModuleHandleW(
            L"kernelbase.dll");

    if (!kernelBase) {
        return FALSE;
    }

    auto loadLibraryExW =
        reinterpret_cast<
            decltype(
                &LoadLibraryExW)>(
            GetProcAddress(
                kernelBase,
                "LoadLibraryExW"));

    if (!loadLibraryExW) {
        return FALSE;
    }

    WindhawkUtils::
        SetFunctionHook(
            loadLibraryExW,
            LoadLibraryExW_Hook,
            &LoadLibraryExW_Original);

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(
        L"Taskbar Count Badges: "
        L"after init");

    if (!g_isTaskbarProcess) {
        return;
    }

    // Build accurate HWND counts first.
    if (!BuildRealWindowCounts(
            nullptr,
            true)) {
        return;
    }

    // Then immediately attach badges to all already-existing
    // taskbar buttons through the existing XAML tree.
    HWND taskbarWnd =
        FindCurrentProcessTaskbarWnd();

    if (!taskbarWnd) {
        return;
    }

    if (!RunOnTaskbarThread(
            taskbarWnd,
            InitializeExistingButtonsOnTaskbarThread,
            taskbarWnd)) {
        Wh_Log(
            L"Taskbar Count Badges: "
            L"initial taskbar-thread execution failed");
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(
        L"Taskbar Count Badges: "
        L"before uninit");

    g_unloading =
        true;

    bool ok =
        CleanupSynchronously();

    Wh_Log(
        L"Taskbar Count Badges: "
        L"cleanup=%s",
        ok
            ? L"OK"
            : L"FAILED");
}

void Wh_ModUninit() {
    g_recountQueued =
        false;

    g_appCounts.clear();
    g_trackedButtons.clear();

    g_taskbarDispatcher =
        nullptr;

    Wh_Log(
        L"Taskbar Count Badges: "
        L"uninit");
}