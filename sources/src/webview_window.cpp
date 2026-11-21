// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Fenêtre de JSG Guitar avec WebView2 (voir webview_window.h).

    WebView2 est fourni par Windows (« Microsoft Edge WebView2 Runtime »). Pour le démarrer, on
    n'utilise pas WebView2Loader.dll : le programme trouve lui-même le runtime installé (registre,
    puis dossier habituel) et appelle sa fonction de création, comme le fait le chargeur officiel.
    Si un WebView2Loader.dll est posé à côté du programme, c'est lui qui est utilisé.

    Les définitions des interfaces viennent de WebView2.h (SDK Microsoft, licence BSD,
    third_party/webview2).
*/
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include "webview_window.h"

#include <objbase.h>
#include <shellapi.h>
#include "WebView2.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>

namespace jsg
{
namespace
{
    //==========================================================================
    // Identifiants des interfaces utilisées (copiés de WebView2.h, version 1.0.1150)
    constexpr GUID makeGuid (unsigned long a, unsigned short b, unsigned short c, unsigned long long d)
    {
        return { a, b, c, { (unsigned char) (d >> 56), (unsigned char) (d >> 48), (unsigned char) (d >> 40), (unsigned char) (d >> 32),
                            (unsigned char) (d >> 24), (unsigned char) (d >> 16), (unsigned char) (d >> 8), (unsigned char) d } };
    }

    constexpr GUID iidEnvironmentCompleted = makeGuid (0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6b5124fee6cc14dULL);
    constexpr GUID iidControllerCompleted  = makeGuid (0x6c4819f3, 0xc9b7, 0x4260, 0x8127c9f5bde7f68cULL);
    constexpr GUID iidNewWindowRequested   = makeGuid (0xd4c185fe, 0xc81c, 0x4989, 0x97af2d3fa7ab5651ULL);
    constexpr GUID iidFullScreenChanged    = makeGuid (0xe45d98b1, 0xafef, 0x45be, 0x8baf6c7728867f73ULL);
    constexpr GUID iidProcessFailed        = makeGuid (0x79e0aea4, 0x990b, 0x42d9, 0xaa1d0fcc2e5bc7f1ULL);
    constexpr GUID iidSettings3            = makeGuid (0xfdb5ab74, 0xaf33, 0x4854, 0x84f00a631deb5ebaULL);
    constexpr GUID iidSettings4            = makeGuid (0xcb56846c, 0x4168, 0x4d53, 0xb04f03b6d6796ff2ULL);
    constexpr GUID iidSettings5            = makeGuid (0x183e7052, 0x1d03, 0x43a0, 0xab9998e043b66b39ULL);
    constexpr GUID iidSettings6            = makeGuid (0x11cb3acd, 0x9bc8, 0x43b8, 0x83bff40753714f87ULL);
    constexpr GUID iidController2          = makeGuid (0xc979903e, 0xd4ca, 0x4228, 0x92eb47ee3fa96eabULL);

    constexpr COLORREF background = RGB (8, 11, 17);

    template <class T> void release (T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

    /** Objet COM minimal qui appelle une fonction : les réponses et événements de WebView2. */
    template <class Interface, class... Args>
    class Callback final : public Interface
    {
    public:
        Callback (const GUID& id, std::function<HRESULT (Args...)> f) : iid (id), fn (std::move (f)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** out) override
        {
            if (out == nullptr) return E_POINTER;
            if (IsEqualIID (riid, IID_IUnknown) || IsEqualIID (riid, iid)) { *out = static_cast<Interface*> (this); AddRef(); return S_OK; }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement (&refs); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG n = (ULONG) InterlockedDecrement (&refs);
            if (n == 0) delete this;
            return n;
        }
        HRESULT STDMETHODCALLTYPE Invoke (Args... args) override { return fn ? fn (args...) : S_OK; }

    private:
        GUID iid;
        std::function<HRESULT (Args...)> fn;
        LONG refs = 1;
    };

    //==========================================================================
    // Démarrage du runtime WebView2

    using CreateWithOptions = HRESULT (STDMETHODCALLTYPE*) (PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    using CreateInternal = HRESULT (STDMETHODCALLTYPE*) (bool, int, PCWSTR, IUnknown*,
                                                         ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

    std::filesystem::path exeFolder()
    {
        wchar_t buffer[MAX_PATH * 4] {};
        GetModuleFileNameW (nullptr, buffer, (DWORD) std::size (buffer));
        return std::filesystem::path (buffer).parent_path();
    }

    std::vector<int> parseVersion (const std::wstring& s)
    {
        std::vector<int> v;
        const wchar_t* p = s.c_str();
        while (*p != 0 && v.size() < 4)
        {
            wchar_t* end = nullptr;
            const long n = std::wcstol (p, &end, 10);
            if (end == p) break;
            v.push_back ((int) n);
            p = (*end == L'.') ? end + 1 : end;
        }
        return v;
    }

    bool compatible (const std::wstring& version)
    {
        const auto v = parseVersion (version);
        return v.size() == 4 && v >= std::vector<int> { 86, 0, 616, 0 };
    }

    std::wstring clientDllIn (const std::filesystem::path& versionFolder)
    {
        std::error_code ec;
        const auto dll = versionFolder / L"EBWebView" / L"x64" / L"EmbeddedBrowserWebView.dll";
        return std::filesystem::exists (dll, ec) ? dll.wstring() : std::wstring();
    }

    /** Chemin de EmbeddedBrowserWebView.dll du runtime installé (canal stable d'abord). */
    std::wstring findRuntime (std::wstring& version)
    {
        const wchar_t* channels[] = { L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",    // stable
                                      L"{2CD8A007-E189-409D-A2C8-9AF4EF3C72AA}",    // beta
                                      L"{0D50BFEC-CD6A-4F9A-964C-C7416E3ACB10}",    // dev
                                      L"{65C35B14-6C1D-4122-AC46-7148CC9D6497}" };  // canary
        for (const auto* guid : channels)
        {
            const std::wstring key = std::wstring (L"Software\\Microsoft\\EdgeUpdate\\ClientState\\") + guid;
            for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
            {
                HKEY k = nullptr;
                if (RegOpenKeyExW (root, key.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) continue;
                wchar_t path[MAX_PATH * 2] {};
                DWORD size = sizeof path - sizeof (wchar_t);
                const LSTATUS r = RegQueryValueExW (k, L"EBWebView", nullptr, nullptr, (LPBYTE) path, &size);
                RegCloseKey (k);
                if (r != ERROR_SUCCESS) continue;

                const std::filesystem::path folder (path);
                const std::wstring v = folder.filename().wstring();
                if (! compatible (v)) continue;
                if (auto dll = clientDllIn (folder); ! dll.empty()) { version = v; return dll; }
            }
        }

        // Dernier recours : le dossier d'installation habituel, version la plus récente
        std::wstring best;
        std::vector<int> bestVersion;
        for (const wchar_t* base : { L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application",
                                     L"C:\\Program Files\\Microsoft\\EdgeWebView\\Application" })
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator (base, ec))
            {
                const std::wstring v = entry.path().filename().wstring();
                if (! entry.is_directory (ec) || ! compatible (v)) continue;
                const auto pv = parseVersion (v);
                if (pv <= bestVersion) continue;
                if (auto dll = clientDllIn (entry.path()); ! dll.empty()) { best = dll; bestVersion = pv; version = v; }
            }
        }
        return best;
    }

    HRESULT createEnvironment (const std::wstring& userData, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler,
                               std::wstring& description)
    {
        // 1. Chargeur officiel, s'il est à côté du programme
        const auto officialLoader = exeFolder() / L"WebView2Loader.dll";
        std::error_code ec;
        if (std::filesystem::exists (officialLoader, ec))
            if (HMODULE m = LoadLibraryW (officialLoader.c_str()))
                if (auto create = (CreateWithOptions) (void*) GetProcAddress (m, "CreateCoreWebView2EnvironmentWithOptions"))
                {
                    description = L"WebView2Loader.dll";
                    return create (nullptr, userData.c_str(), nullptr, handler);
                }

        // 2. Runtime installé, appelé directement
        std::wstring version;
        const std::wstring dll = findRuntime (version);
        if (dll.empty()) return HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND);

        HMODULE client = LoadLibraryW (dll.c_str());
        if (client == nullptr) return HRESULT_FROM_WIN32 (GetLastError());
        auto create = (CreateInternal) (void*) GetProcAddress (client, "CreateWebViewEnvironmentWithOptionsInternal");
        if (create == nullptr) return HRESULT_FROM_WIN32 (ERROR_PROC_NOT_FOUND);

        description = L"WebView2 " + version;
        return create (true, 0 /* runtime installé */, userData.c_str(), nullptr, handler);
    }

    //==========================================================================
    // La fenêtre

    struct Window
    {
        WindowOptions options;
        HWND hwnd = nullptr;
        ICoreWebView2Controller* controller = nullptr;
        ICoreWebView2* webview = nullptr;
        bool failed = false;
        std::wstring error, loadingText = L"Chargement de JSG Guitar…";

        // Plein écran (bouton de l'interface)
        bool fullscreen = false;
        WINDOWPLACEMENT savedPlacement { sizeof (WINDOWPLACEMENT) };
        LONG_PTR savedStyle = 0;

        void resize()
        {
            if (controller == nullptr) return;
            RECT r {};
            GetClientRect (hwnd, &r);
            controller->put_Bounds (r);
        }

        void setFullscreen (bool on)
        {
            if (on == fullscreen) return;
            fullscreen = on;
            if (on)
            {
                savedStyle = GetWindowLongPtrW (hwnd, GWL_STYLE);
                GetWindowPlacement (hwnd, &savedPlacement);
                MONITORINFO mi { sizeof mi };
                GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowLongPtrW (hwnd, GWL_STYLE, savedStyle & ~(LONG_PTR) WS_OVERLAPPEDWINDOW);
                SetWindowPos (hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                              mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            }
            else
            {
                SetWindowLongPtrW (hwnd, GWL_STYLE, savedStyle);
                SetWindowPlacement (hwnd, &savedPlacement);
                SetWindowPos (hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            }
        }

        //======================================================================
        // Position de la fenêtre d'une fois sur l'autre
        void savePlacement()
        {
            if (options.placementFile.empty() || hwnd == nullptr) return;
            WINDOWPLACEMENT p { sizeof p };
            if (fullscreen) p = savedPlacement; else GetWindowPlacement (hwnd, &p);
            std::ofstream f (std::filesystem::path (options.placementFile));
            f << p.rcNormalPosition.left << ' ' << p.rcNormalPosition.top << ' ' << p.rcNormalPosition.right << ' '
              << p.rcNormalPosition.bottom << ' ' << (p.showCmd == SW_SHOWMAXIMIZED || (fullscreen && IsZoomed (hwnd)) ? 1 : 0) << '\n';
        }

        bool restorePlacement()
        {
            if (options.placementFile.empty()) return false;
            std::ifstream f (std::filesystem::path (options.placementFile));
            long l = 0, t = 0, r = 0, b = 0;
            int maximised = 0;
            if (! (f >> l >> t >> r >> b >> maximised) || r - l < 400 || b - t < 300) return false;

            // La fenêtre doit rester visible (écran débranché depuis)
            RECT rc { l, t, r, b }, inter {};
            MONITORINFO mi { sizeof mi };
            GetMonitorInfoW (MonitorFromRect (&rc, MONITOR_DEFAULTTONEAREST), &mi);
            if (! IntersectRect (&inter, &rc, &mi.rcWork) || inter.right - inter.left < 200 || inter.bottom - inter.top < 100) return false;

            WINDOWPLACEMENT p { sizeof p };
            p.rcNormalPosition = rc;
            p.showCmd = maximised ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement (hwnd, &p);
            return true;
        }

        void defaultPlacement()
        {
            // Interface au format 3:2, environ 1440 x 960 à 100 %, sans dépasser l'écran
            MONITORINFO mi { sizeof mi };
            GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            const UINT dpi = dpiFor (hwnd);
            const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
            int w = MulDiv (1440, (int) dpi, 96), h = MulDiv (960, (int) dpi, 96);
            const double fit = std::min ({ 1.0, 0.92 * workW / w, 0.92 * workH / h });
            w = (int) (w * fit); h = (int) (h * fit);
            RECT rc { 0, 0, w, h };
            AdjustWindowRectEx (&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
            const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
            SetWindowPos (hwnd, nullptr, mi.rcWork.left + (workW - ww) / 2, mi.rcWork.top + (workH - wh) / 2, ww, wh, SWP_NOZORDER);
            ShowWindow (hwnd, SW_SHOWNORMAL);
        }

        static UINT dpiFor (HWND h)
        {
            using GetDpi = UINT (WINAPI*) (HWND);
            static auto fn = (GetDpi) (void*) GetProcAddress (GetModuleHandleW (L"user32.dll"), "GetDpiForWindow");
            return fn != nullptr ? fn (h) : 96;
        }

        //======================================================================
        // WebView2

        void fail (const std::wstring& what, HRESULT hr)
        {
            wchar_t code[32];
            std::swprintf (code, 32, L" (0x%08lX)", (unsigned long) hr);
            error = what + code;
            failed = true;
            PostMessageW (hwnd, WM_CLOSE, 0, 0);
        }

        void start()
        {
            std::wstring description;
            auto* handler = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*> (
                iidEnvironmentCompleted, [this] (HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
                {
                    if (FAILED (hr) || env == nullptr) { fail (L"WebView2 n'a pas démarré", hr); return S_OK; }
                    auto* next = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*> (
                        iidControllerCompleted, [this] (HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT
                        {
                            if (FAILED (hr2) || c == nullptr) { fail (L"WebView2 n'a pas pu créer la vue", hr2); return S_OK; }
                            controllerReady (c);
                            return S_OK;
                        });
                    const HRESULT r = env->CreateCoreWebView2Controller (hwnd, next);
                    next->Release();
                    if (FAILED (r)) fail (L"WebView2 n'a pas pu créer la vue", r);
                    return S_OK;
                });
            const HRESULT hr = createEnvironment (options.userDataFolder, handler, description);
            handler->Release();
            if (FAILED (hr)) fail (hr == HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND) ? L"WebView2 n'est pas installé" : L"WebView2 n'a pas démarré", hr);
        }

        void controllerReady (ICoreWebView2Controller* c)
        {
            controller = c;
            controller->AddRef();
            controller->get_CoreWebView2 (&webview);
            if (webview == nullptr) { fail (L"WebView2 : vue absente", E_FAIL); return; }

            // Fond sombre pendant le chargement (pas d'éclair blanc)
            ICoreWebView2Controller2* c2 = nullptr;
            if (SUCCEEDED (controller->QueryInterface (iidController2, (void**) &c2)) && c2 != nullptr)
            {
                c2->put_DefaultBackgroundColor ({ 255, GetRValue (background), GetGValue (background), GetBValue (background) });
                c2->Release();
            }

            // Réglages : une application, pas un navigateur
            ICoreWebView2Settings* s = nullptr;
            if (SUCCEEDED (webview->get_Settings (&s)) && s != nullptr)
            {
                s->put_AreDefaultContextMenusEnabled (options.devTools);
                s->put_AreDevToolsEnabled (options.devTools);
                s->put_IsStatusBarEnabled (FALSE);
                s->put_IsZoomControlEnabled (FALSE);
                s->put_IsBuiltInErrorPageEnabled (TRUE);

                ICoreWebView2Settings3* s3 = nullptr;
                if (! options.devTools && SUCCEEDED (s->QueryInterface (iidSettings3, (void**) &s3)) && s3 != nullptr)
                {
                    s3->put_AreBrowserAcceleratorKeysEnabled (FALSE);   // pas de Ctrl+F, Ctrl+P, F5…
                    s3->Release();
                }
                ICoreWebView2Settings4* s4 = nullptr;
                if (SUCCEEDED (s->QueryInterface (iidSettings4, (void**) &s4)) && s4 != nullptr)
                {
                    s4->put_IsPasswordAutosaveEnabled (FALSE);
                    s4->put_IsGeneralAutofillEnabled (FALSE);
                    s4->Release();
                }
                ICoreWebView2Settings5* s5 = nullptr;
                if (SUCCEEDED (s->QueryInterface (iidSettings5, (void**) &s5)) && s5 != nullptr) { s5->put_IsPinchZoomEnabled (FALSE); s5->Release(); }
                ICoreWebView2Settings6* s6 = nullptr;
                if (SUCCEEDED (s->QueryInterface (iidSettings6, (void**) &s6)) && s6 != nullptr) { s6->put_IsSwipeNavigationEnabled (FALSE); s6->Release(); }
                s->Release();
            }

            EventRegistrationToken token {};

            // Liens vers l'extérieur : navigateur habituel, jamais une deuxième fenêtre
            auto* newWindow = new Callback<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*> (
                iidNewWindowRequested, [] (ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                {
                    args->put_Handled (TRUE);
                    LPWSTR uri = nullptr;
                    if (SUCCEEDED (args->get_Uri (&uri)) && uri != nullptr)
                    {
                        if (std::wcsncmp (uri, L"https://", 8) == 0 || std::wcsncmp (uri, L"http://", 7) == 0)
                            ShellExecuteW (nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                        CoTaskMemFree (uri);
                    }
                    return S_OK;
                });
            webview->add_NewWindowRequested (newWindow, &token);
            newWindow->Release();

            // Plein écran demandé par l'interface
            auto* full = new Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler, ICoreWebView2*, IUnknown*> (
                iidFullScreenChanged, [this] (ICoreWebView2* sender, IUnknown*) -> HRESULT
                {
                    BOOL on = FALSE;
                    sender->get_ContainsFullScreenElement (&on);
                    setFullscreen (on != FALSE);
                    return S_OK;
                });
            webview->add_ContainsFullScreenElementChanged (full, &token);
            full->Release();

            // Page plantée : on la recharge ; moteur WebView2 arrêté : on ferme proprement
            auto* crash = new Callback<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*> (
                iidProcessFailed, [this] (ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT
                {
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED;
                    args->get_ProcessFailedKind (&kind);
                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED)
                    {
                        MessageBoxW (hwnd, L"Le moteur d'affichage (WebView2) s'est arrêté. Relancez JSG Guitar.", options.title.c_str(), MB_OK | MB_ICONWARNING);
                        PostMessageW (hwnd, WM_CLOSE, 0, 0);
                    }
                    else sender->Reload();
                    return S_OK;
                });
            webview->add_ProcessFailed (crash, &token);
            crash->Release();

            resize();
            controller->put_IsVisible (TRUE);
            webview->Navigate (options.url.c_str());
            controller->MoveFocus (COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }

        void close()
        {
            if (controller != nullptr) controller->Close();
            release (webview);
            release (controller);
        }

        //======================================================================
        static LRESULT CALLBACK proc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto* self = (Window*) GetWindowLongPtrW (h, GWLP_USERDATA);
            if (msg == WM_NCCREATE)
            {
                self = (Window*) ((CREATESTRUCTW*) lp)->lpCreateParams;
                SetWindowLongPtrW (h, GWLP_USERDATA, (LONG_PTR) self);
                self->hwnd = h;
            }
            if (self == nullptr) return DefWindowProcW (h, msg, wp, lp);

            switch (msg)
            {
                case WM_SIZE:
                    self->resize();
                    return 0;

                case WM_MOVE:
                case WM_MOVING:
                    if (self->controller != nullptr) self->controller->NotifyParentWindowPositionChanged();
                    break;

                case WM_DPICHANGED:
                {
                    const RECT* r = (const RECT*) lp;
                    SetWindowPos (h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }

                case WM_GETMINMAXINFO:
                {
                    auto* mm = (MINMAXINFO*) lp;
                    const UINT dpi = dpiFor (h);
                    mm->ptMinTrackSize = { MulDiv (640, (int) dpi, 96), MulDiv (440, (int) dpi, 96) };
                    return 0;
                }

                case WM_ACTIVATE:
                    if (LOWORD (wp) != WA_INACTIVE && self->controller != nullptr)
                        self->controller->MoveFocus (COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                    break;

                case WM_PAINT:
                {
                    // Avant que l'interface n'apparaisse : fond sombre et « Chargement… »
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint (h, &ps);
                    RECT r;
                    GetClientRect (h, &r);
                    HBRUSH b = CreateSolidBrush (background);
                    FillRect (dc, &r, b);
                    DeleteObject (b);
                    if (self->controller == nullptr)
                    {
                        SetBkMode (dc, TRANSPARENT);
                        SetTextColor (dc, RGB (150, 160, 175));
                        HFONT font = CreateFontW (-MulDiv (16, (int) dpiFor (h), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                                  CLEARTYPE_QUALITY, 0, L"Segoe UI");
                        auto old = SelectObject (dc, font);
                        DrawTextW (dc, self->loadingText.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        SelectObject (dc, old);
                        DeleteObject (font);
                    }
                    EndPaint (h, &ps);
                    return 0;
                }

                case WM_ERASEBKGND:
                    return 1;

                case WM_CLOSE:
                    self->savePlacement();
                    self->close();
                    DestroyWindow (h);
                    return 0;

                case WM_DESTROY:
                    PostQuitMessage (0);
                    return 0;
            }
            return DefWindowProcW (h, msg, wp, lp);
        }
    };
}

WindowResult runWebViewWindow (const WindowOptions& options, std::wstring& error)
{
    Window window;
    window.options = options;

    const HINSTANCE instance = GetModuleHandleW (nullptr);
    WNDCLASSEXW wc { sizeof wc };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Window::proc;
    wc.hInstance = instance;
    wc.hIcon = options.icon;
    wc.hIconSm = options.icon;
    wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
    wc.lpszClassName = windowClassName;
    RegisterClassExW (&wc);

    HWND h = CreateWindowExW (0, windowClassName, options.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 860, nullptr, nullptr, instance, &window);
    if (h == nullptr) { error = L"fenêtre impossible à créer"; return WindowResult::unavailable; }

    if (! window.restorePlacement()) window.defaultPlacement();
    UpdateWindow (h);
    window.start();

    MSG msg;
    while (GetMessageW (&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }

    UnregisterClassW (windowClassName, instance);
    if (window.failed) { error = window.error; return WindowResult::unavailable; }
    return WindowResult::closed;
}

} // namespace jsg
