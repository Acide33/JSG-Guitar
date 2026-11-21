// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Fenêtre du plug-in (voir plugin_view.h).

    WebView2 est fourni par Windows (« Microsoft Edge WebView2 Runtime »). Le chargeur officiel
    WebView2Loader.dll est rangé dans le plug-in, à côté du module ; s'il manque, le plug-in trouve
    lui-même le runtime installé. Interfaces : WebView2.h (SDK Microsoft, licence BSD).
*/
#include "plugin_view.h"
#include "plugin.h"

#ifndef _WIN32

namespace jsg::plugin
{
Steinberg::IPlugView* createEditorView (Plugin&) { return nullptr; }
}

#else

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include "WebView2.h"

#include "public.sdk/source/common/pluginview.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace Steinberg;

namespace jsg::plugin
{
namespace
{
    constexpr GUID makeGuid (unsigned long a, unsigned short b, unsigned short c, unsigned long long d)
    {
        return { a, b, c, { (unsigned char) (d >> 56), (unsigned char) (d >> 48), (unsigned char) (d >> 40), (unsigned char) (d >> 32),
                            (unsigned char) (d >> 24), (unsigned char) (d >> 16), (unsigned char) (d >> 8), (unsigned char) d } };
    }

    // Identifiants des interfaces utilisées (copiés de WebView2.h, version 1.0.1150)
    constexpr GUID iidEnvironmentCompleted = makeGuid (0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6b5124fee6cc14dULL);
    constexpr GUID iidControllerCompleted  = makeGuid (0x6c4819f3, 0xc9b7, 0x4260, 0x8127c9f5bde7f68cULL);
    constexpr GUID iidNewWindowRequested   = makeGuid (0xd4c185fe, 0xc81c, 0x4989, 0x97af2d3fa7ab5651ULL);
    constexpr GUID iidProcessFailed        = makeGuid (0x79e0aea4, 0x990b, 0x42d9, 0xaa1d0fcc2e5bc7f1ULL);
    constexpr GUID iidSettings3            = makeGuid (0xfdb5ab74, 0xaf33, 0x4854, 0x84f00a631deb5ebaULL);
    constexpr GUID iidSettings4            = makeGuid (0xcb56846c, 0x4168, 0x4d53, 0xb04f03b6d6796ff2ULL);
    constexpr GUID iidSettings5            = makeGuid (0x183e7052, 0x1d03, 0x43a0, 0xab9998e043b66b39ULL);
    constexpr GUID iidController2          = makeGuid (0xc979903e, 0xd4ca, 0x4228, 0x92eb47ee3fa96eabULL);

    constexpr COLORREF background = RGB (8, 11, 17);
    constexpr int baseWidth = 1152, baseHeight = 768;   // proportions de l'interface (3:2)

    template <class T> void release (T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

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
        ULONG STDMETHODCALLTYPE Release() override { const ULONG n = (ULONG) InterlockedDecrement (&refs); if (n == 0) delete this; return n; }
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

    std::filesystem::path moduleFolder()
    {
        HMODULE h = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) (void*) &moduleFolder, &h);
        wchar_t buffer[MAX_PATH * 4] {};
        GetModuleFileNameW (h, buffer, (DWORD) std::size (buffer));
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

    std::wstring findRuntime()
    {
        const wchar_t* channels[] = { L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"{2CD8A007-E189-409D-A2C8-9AF4EF3C72AA}",
                                      L"{0D50BFEC-CD6A-4F9A-964C-C7416E3ACB10}", L"{65C35B14-6C1D-4122-AC46-7148CC9D6497}" };
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
                if (! compatible (folder.filename().wstring())) continue;
                if (auto dll = clientDllIn (folder); ! dll.empty()) return dll;
            }
        }
        std::wstring best;
        std::vector<int> bestVersion;
        for (const wchar_t* base : { L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application", L"C:\\Program Files\\Microsoft\\EdgeWebView\\Application" })
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator (base, ec))
            {
                const std::wstring v = entry.path().filename().wstring();
                if (! entry.is_directory (ec) || ! compatible (v)) continue;
                const auto pv = parseVersion (v);
                if (pv <= bestVersion) continue;
                if (auto dll = clientDllIn (entry.path()); ! dll.empty()) { best = dll; bestVersion = pv; }
            }
        }
        return best;
    }

    HRESULT createEnvironment (const std::wstring& userData, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler)
    {
        const auto officialLoader = moduleFolder() / L"WebView2Loader.dll";
        std::error_code ec;
        if (std::filesystem::exists (officialLoader, ec))
            if (HMODULE m = LoadLibraryW (officialLoader.c_str()))
                if (auto create = (CreateWithOptions) (void*) GetProcAddress (m, "CreateCoreWebView2EnvironmentWithOptions"))
                    return create (nullptr, userData.c_str(), nullptr, handler);

        const std::wstring dll = findRuntime();
        if (dll.empty()) return HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND);
        HMODULE client = LoadLibraryW (dll.c_str());
        if (client == nullptr) return HRESULT_FROM_WIN32 (GetLastError());
        auto create = (CreateInternal) (void*) GetProcAddress (client, "CreateWebViewEnvironmentWithOptionsInternal");
        if (create == nullptr) return HRESULT_FROM_WIN32 (ERROR_PROC_NOT_FOUND);
        return create (true, 0, userData.c_str(), nullptr, handler);
    }

    std::wstring userDataFolder()
    {
        PWSTR path = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED (SHGetKnownFolderPath (FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) result = path;
        CoTaskMemFree (path);
        return (result / L"JSG Guitar" / L"WebView2-Plugin").wstring();
    }

    std::wstring widen (const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
        std::wstring w ((size_t) n, L'\0');
        MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
        return w;
    }

    UINT systemDpi()
    {
        using GetDpi = UINT (WINAPI*)();
        static auto fn = (GetDpi) (void*) GetProcAddress (GetModuleHandleW (L"user32.dll"), "GetDpiForSystem");
        return fn != nullptr ? fn() : 96;
    }

    //==========================================================================
    /** État de la fenêtre partagé avec les réponses asynchrones de WebView2 (qui peuvent arriver après sa fermeture). */
    struct Shared
    {
        HWND hwnd = nullptr;
        ICoreWebView2Controller* controller = nullptr;
        ICoreWebView2* webview = nullptr;
        bool closed = false;
        std::wstring message = L"Chargement de JSG Guitar…";

        void resize()
        {
            if (controller == nullptr || hwnd == nullptr) return;
            RECT r {};
            GetClientRect (hwnd, &r);
            controller->put_Bounds (r);
        }

        void fail (const wchar_t* what, HRESULT hr)
        {
            wchar_t text[256];
            std::swprintf (text, 256, L"%ls (0x%08lX).\nInstallez « Microsoft Edge WebView2 Runtime » depuis le site de Microsoft.", what, (unsigned long) hr);
            message = text;
            if (hwnd != nullptr) InvalidateRect (hwnd, nullptr, TRUE);
        }

        void close()
        {
            closed = true;
            if (controller != nullptr) controller->Close();
            release (webview);
            release (controller);
        }
    };

    LRESULT CALLBACK childProc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* shared = (std::shared_ptr<Shared>*) GetWindowLongPtrW (h, GWLP_USERDATA);
        switch (msg)
        {
            case WM_SIZE:
                if (shared != nullptr) (*shared)->resize();
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint (h, &ps);
                RECT r;
                GetClientRect (h, &r);
                HBRUSH b = CreateSolidBrush (background);
                FillRect (dc, &r, b);
                DeleteObject (b);
                if (shared != nullptr && (*shared)->controller == nullptr)
                {
                    SetBkMode (dc, TRANSPARENT);
                    SetTextColor (dc, RGB (150, 160, 175));
                    HFONT font = CreateFontW (-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                    auto old = SelectObject (dc, font);
                    RECT t = r;
                    DrawTextW (dc, (*shared)->message.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
                    const int h2 = t.bottom - t.top;
                    t = { r.left + 20, (r.bottom - h2) / 2, r.right - 20, (r.bottom + h2) / 2 };
                    DrawTextW (dc, (*shared)->message.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK);
                    SelectObject (dc, old);
                    DeleteObject (font);
                }
                EndPaint (h, &ps);
                return 0;
            }
            case WM_NCDESTROY:
                delete shared;
                SetWindowLongPtrW (h, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW (h, msg, wp, lp);
    }

    //==========================================================================
    class EditorView final : public CPluginView
    {
    public:
        explicit EditorView (Plugin& p) : plugin (p)
        {
            const int dpi = (int) systemDpi();
            ViewRect r (0, 0, MulDiv (baseWidth, dpi, 96), MulDiv (baseHeight, dpi, 96));
            setRect (r);
        }

        tresult PLUGIN_API isPlatformTypeSupported (FIDString type) override
        {
            return std::strcmp (type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
        }

        tresult PLUGIN_API attached (void* parent, FIDString type) override
        {
            if (isPlatformTypeSupported (type) != kResultTrue) return kResultFalse;
            CPluginView::attached (parent, type);

            static bool registered = false;
            const HINSTANCE instance = GetModuleHandleW (nullptr);
            if (! registered)
            {
                WNDCLASSEXW wc { sizeof wc };
                wc.lpfnWndProc = childProc;
                wc.hInstance = instance;
                wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
                wc.lpszClassName = L"JSGGuitarPluginView";
                RegisterClassExW (&wc);
                registered = true;
            }

            shared = std::make_shared<Shared>();
            const auto& r = getRect();
            HWND h = CreateWindowExW (0, L"JSGGuitarPluginView", L"JSG Guitar", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                      r.getWidth(), r.getHeight(), (HWND) parent, nullptr, instance, nullptr);
            if (h == nullptr) return kResultFalse;
            shared->hwnd = h;
            SetWindowLongPtrW (h, GWLP_USERDATA, (LONG_PTR) new std::shared_ptr<Shared> (shared));

            const std::string url = plugin.getCore().interfaceUrl();
            if (url.empty()) { shared->fail (L"Le serveur local de l'interface n'a pas pu démarrer", E_FAIL); return kResultOk; }
            start (widen (url));
            return kResultOk;
        }

        tresult PLUGIN_API removed() override
        {
            if (shared != nullptr)
            {
                HWND h = shared->hwnd;
                shared->close();
                shared->hwnd = nullptr;
                if (h != nullptr) DestroyWindow (h);
                shared.reset();
            }
            return CPluginView::removed();
        }

        tresult PLUGIN_API onSize (ViewRect* newSize) override
        {
            if (newSize == nullptr) return kInvalidArgument;
            CPluginView::onSize (newSize);
            if (shared != nullptr && shared->hwnd != nullptr)
                SetWindowPos (shared->hwnd, nullptr, 0, 0, newSize->getWidth(), newSize->getHeight(), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
            return kResultOk;
        }

        tresult PLUGIN_API canResize() override { return kResultTrue; }

        tresult PLUGIN_API checkSizeConstraint (ViewRect* r) override
        {
            // Proportions de l'interface (3:2), taille minimale raisonnable
            if (r == nullptr) return kInvalidArgument;
            const int dpi = (int) systemDpi();
            int w = std::max (r->getWidth(), MulDiv (640, dpi, 96));
            int h = std::max (r->getHeight(), MulDiv (427, dpi, 96));
            if (w * 2 > h * 3) w = h * 3 / 2; else h = w * 2 / 3;
            r->right = r->left + w;
            r->bottom = r->top + h;
            return kResultTrue;
        }

    private:
        void start (const std::wstring& url)
        {
            std::weak_ptr<Shared> weak = shared;
            auto* envHandler = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*> (
                iidEnvironmentCompleted, [weak, url] (HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
                {
                    auto s = weak.lock();
                    if (s == nullptr || s->closed) return S_OK;
                    if (FAILED (hr) || env == nullptr) { s->fail (L"WebView2 n'a pas démarré", hr); return S_OK; }

                    auto* ctrlHandler = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*> (
                        iidControllerCompleted, [weak, url] (HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT
                        {
                            auto s2 = weak.lock();
                            if (s2 == nullptr || s2->closed) { if (c != nullptr) c->Close(); return S_OK; }
                            if (FAILED (hr2) || c == nullptr) { s2->fail (L"WebView2 n'a pas pu créer la vue", hr2); return S_OK; }
                            ready (*s2, c, url);
                            return S_OK;
                        });
                    const HRESULT r = env->CreateCoreWebView2Controller (s->hwnd, ctrlHandler);
                    ctrlHandler->Release();
                    if (FAILED (r)) s->fail (L"WebView2 n'a pas pu créer la vue", r);
                    return S_OK;
                });
            const HRESULT hr = createEnvironment (userDataFolder(), envHandler);
            envHandler->Release();
            if (FAILED (hr)) shared->fail (hr == HRESULT_FROM_WIN32 (ERROR_FILE_NOT_FOUND) ? L"WebView2 n'est pas installé" : L"WebView2 n'a pas démarré", hr);
        }

        static void ready (Shared& s, ICoreWebView2Controller* c, const std::wstring& url)
        {
            s.controller = c;
            c->AddRef();
            c->get_CoreWebView2 (&s.webview);
            if (s.webview == nullptr) { s.fail (L"WebView2 : vue absente", E_FAIL); return; }

            ICoreWebView2Controller2* c2 = nullptr;
            if (SUCCEEDED (c->QueryInterface (iidController2, (void**) &c2)) && c2 != nullptr)
            {
                c2->put_DefaultBackgroundColor ({ 255, GetRValue (background), GetGValue (background), GetBValue (background) });
                c2->Release();
            }

            ICoreWebView2Settings* settings = nullptr;
            if (SUCCEEDED (s.webview->get_Settings (&settings)) && settings != nullptr)
            {
                const char* dev = std::getenv ("JSG_DEVTOOLS");
                const BOOL devTools = dev != nullptr && dev[0] == '1';
                settings->put_AreDefaultContextMenusEnabled (devTools);
                settings->put_AreDevToolsEnabled (devTools);
                settings->put_IsStatusBarEnabled (FALSE);
                settings->put_IsZoomControlEnabled (FALSE);
                ICoreWebView2Settings3* s3 = nullptr;
                if (! devTools && SUCCEEDED (settings->QueryInterface (iidSettings3, (void**) &s3)) && s3 != nullptr)
                {
                    s3->put_AreBrowserAcceleratorKeysEnabled (FALSE);
                    s3->Release();
                }
                ICoreWebView2Settings4* s4 = nullptr;
                if (SUCCEEDED (settings->QueryInterface (iidSettings4, (void**) &s4)) && s4 != nullptr)
                {
                    s4->put_IsPasswordAutosaveEnabled (FALSE);
                    s4->put_IsGeneralAutofillEnabled (FALSE);
                    s4->Release();
                }
                ICoreWebView2Settings5* s5 = nullptr;
                if (SUCCEEDED (settings->QueryInterface (iidSettings5, (void**) &s5)) && s5 != nullptr) { s5->put_IsPinchZoomEnabled (FALSE); s5->Release(); }
                settings->Release();
            }

            EventRegistrationToken token {};
            auto* newWindow = new Callback<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*> (
                iidNewWindowRequested, [] (ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                {
                    args->put_Handled (TRUE);
                    LPWSTR uri = nullptr;
                    if (SUCCEEDED (args->get_Uri (&uri)) && uri != nullptr)
                    {
                        if (std::wcsncmp (uri, L"https://", 8) == 0) ShellExecuteW (nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                        CoTaskMemFree (uri);
                    }
                    return S_OK;
                });
            s.webview->add_NewWindowRequested (newWindow, &token);
            newWindow->Release();

            auto* crash = new Callback<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*> (
                iidProcessFailed, [] (ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT
                {
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED;
                    args->get_ProcessFailedKind (&kind);
                    if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) sender->Reload();
                    return S_OK;
                });
            s.webview->add_ProcessFailed (crash, &token);
            crash->Release();

            s.resize();
            c->put_IsVisible (TRUE);
            s.webview->Navigate (url.c_str());
            InvalidateRect (s.hwnd, nullptr, TRUE);
        }

        Plugin& plugin;
        std::shared_ptr<Shared> shared;
    };
}

IPlugView* createEditorView (Plugin& plugin) { return new EditorView (plugin); }

} // namespace jsg::plugin

#endif
