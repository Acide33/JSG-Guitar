// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — version PC sans JUCE : programme Windows.

    Au lancement :
      1. un fil « moteur » démarre le son (WASAPI, voir wasapi.cpp) avec les réglages enregistrés,
         et un serveur local (127.0.0.1, port 47811) qui sert l'interface (dossier « ui ») ;
      2. l'interface s'affiche dans la fenêtre de JSG Guitar (WebView2, voir webview_window.cpp).
         Si WebView2 manque, elle s'ouvre dans une fenêtre d'application Edge (ou Chrome).
    Quand la fenêtre se ferme, le programme s'arrête.

    Fichiers de l'utilisateur : %LOCALAPPDATA%\JSG Guitar (réglages audio, journal, profil de la
    fenêtre) ; enregistrements : Musique\JSG Guitar ; playbacks JSG : dossier ui\playbacks ou
    Musique\JSG Guitar\Playbacks.
    Variable d'environnement JSG_DEVTOOLS=1 : outils de développement dans la fenêtre (F12).
*/
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "host.h"
#include "paths.h"
#include "webview_window.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

namespace
{
    std::filesystem::path knownFolder (REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED (SHGetKnownFolderPath (id, KF_FLAG_CREATE, nullptr, &path))) result = path;
        CoTaskMemFree (path);
        return result;
    }

    std::filesystem::path exeFolder()
    {
        wchar_t buffer[MAX_PATH * 4] {};
        GetModuleFileNameW (nullptr, buffer, (DWORD) std::size (buffer));
        return std::filesystem::path (buffer).parent_path();
    }

    /** Chemin d'un navigateur d'après le registre (« App Paths »), vide s'il n'est pas installé. */
    std::wstring appPath (const wchar_t* exe)
    {
        const std::wstring key = std::wstring (L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
        {
            wchar_t value[MAX_PATH * 2] {};
            DWORD size = sizeof value;
            if (RegGetValueW (root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
                if (GetFileAttributesW (value) != INVALID_FILE_ATTRIBUTES) return value;
        }
        return {};
    }

    /** Solution de secours sans WebView2 : fenêtre d'application Edge (sinon Chrome, sinon navigateur par défaut). */
    HANDLE openBrowserWindow (const std::wstring& url, const std::filesystem::path& profile)
    {
        std::wstring browser = appPath (L"msedge.exe");
        if (browser.empty())
        {
            for (const wchar_t* candidate : { L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
                                              L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe" })
                if (GetFileAttributesW (candidate) != INVALID_FILE_ATTRIBUTES) { browser = candidate; break; }
        }
        if (browser.empty()) browser = appPath (L"chrome.exe");

        if (browser.empty())
        {
            ShellExecuteW (nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return nullptr;
        }

        // Profil à part, sans extensions ni écrans d'accueil : la fenêtre ne sert qu'à afficher JSG Guitar.
        // Le fichier « First Run » indique à Edge / Chrome que le premier lancement a déjà eu lieu.
        std::error_code ec;
        std::filesystem::create_directories (profile, ec);
        if (! std::filesystem::exists (profile / L"First Run", ec)) std::ofstream (profile / L"First Run").close();

        std::wstring command = L"\"" + browser + L"\" --app=" + url + L" --user-data-dir=\"" + profile.wstring() + L"\""
                             + L" --no-first-run --no-default-browser-check --disable-extensions"
                               L" --disable-component-extensions-with-background-pages --disable-default-apps"
                               L" --disable-sync --hide-crash-restore-bubble --disable-session-crashed-bubble"
                               L" --window-size=1440,1000"
                               L" --disable-features=Translate,msImplicitSignin,msEdgeSidebarV2,msHubApps,msShoppingExp,"
                               L"msEdgeWelcomePage,msWelcomePageOnFirstRun,EdgeCollections,msEdgeDiscover";

        STARTUPINFOW si {};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi {};
        if (! CreateProcessW (nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            ShellExecuteW (nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return nullptr;
        }
        CloseHandle (pi.hThread);
        return pi.hProcess;
    }

    void message (const std::wstring& text)
    {
        MessageBoxW (nullptr, text.c_str(), L"JSG Guitar", MB_OK | MB_ICONWARNING);
    }

    /** Ouvre un dossier dans l'Explorateur, depuis un fil à part (l'Explorateur veut un fil STA). */
    void reveal (const std::filesystem::path& folder)
    {
        std::thread ([folder]
        {
            CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteW (nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            CoUninitialize();
        }).detach();
    }

    //==========================================================================
    /** Fil « moteur » : le son, le serveur local et les messages de l'interface. */
    class EngineThread
    {
    public:
        EngineThread (std::filesystem::path uiFolder, std::filesystem::path dataFolder)
            : ui (std::move (uiFolder)), data (std::move (dataFolder))
        {
            thread = std::thread ([this] { run(); });
        }

        ~EngineThread() { stop(); }

        /** Port du serveur (0 s'il n'a pas pu s'ouvrir). */
        int waitForPort() { return portFuture.get(); }

        /** Mode Edge : le programme s'arrête quand plus aucune fenêtre n'est connectée. */
        void quitWhenWindowsClose() { autoQuit = true; }

        void stop()
        {
            stopRequested = true;
            if (thread.joinable()) thread.join();
        }

        /** Attend la fin du fil (mode Edge : il s'arrête tout seul quand la fenêtre est fermée). */
        void waitUntilStopped() { if (thread.joinable()) thread.join(); }

        void log (const std::string& line) { if (auto* h = host.load()) h->log (line); }

    private:
        void run()
        {
            // Fil MTA : WASAPI et ses objets vivent ici, jamais dans le fil de la fenêtre
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            {
                auto backend = jsg::createAudioBackend();
                jsg::Host h ({ ui, knownFolder (FOLDERID_Music) / L"JSG Guitar", data / L"audio.ini", data / L"journal.txt" }, *backend);
                h.revealFolder = reveal;

                jsg::Server s;
                s.setRoot (ui);
                const auto playbacks = knownFolder (FOLDERID_Music) / L"JSG Guitar" / L"Playbacks";   // playbacks JSG de l'utilisateur
                std::error_code ec;
                std::filesystem::create_directories (playbacks, ec);
                s.addFolder ("/playbacks/", playbacks);
                const int port = s.listen ({ 47811, 47812, 47813, 47814, 47815, 47816, 47817, 47818, 47819, 47820 });

                host = &h;
                if (port != 0)
                {
                    std::ofstream (data / L"port.txt") << port;
                    h.log ("Serveur local : http://127.0.0.1:" + std::to_string (port));
                    h.attach (s);
                    h.startAudio();
                    installAutoQuit (h, s);
                }
                portPromise.set_value (port);

                if (port != 0) s.run();

                h.log ("Fermeture");
                h.stopAudio();
                host = nullptr;
            }
            CoUninitialize();
        }

        void installAutoQuit (jsg::Host& h, jsg::Server& s)
        {
            const auto started = std::chrono::steady_clock::now();
            auto lastSeen = started;
            auto hostTick = s.onTick;
            s.onTick = [this, &h, &s, hostTick, started, lastSeen] () mutable
            {
                hostTick();
                if (stopRequested) { s.quit(); return; }
                if (! autoQuit) return;
                const auto now = std::chrono::steady_clock::now();
                if (h.numClients() > 0) { lastSeen = now; return; }

                // Fenêtre fermée (ou page rechargée : on laisse 4 secondes pour revenir)
                if (h.hadClient() && now - lastSeen > std::chrono::seconds (4)) s.quit();

                // Aucune fenêtre au bout de 90 s : le navigateur n'a pas pu s'ouvrir
                if (! h.hadClient() && now - started > std::chrono::seconds (90))
                {
                    h.log ("Aucune fenêtre connectée : arrêt");
                    s.quit();
                }
            };
        }

        std::filesystem::path ui, data;
        std::promise<int> portPromise;
        std::future<int> portFuture = portPromise.get_future();
        std::atomic<bool> autoQuit { false }, stopRequested { false };
        std::atomic<jsg::Host*> host { nullptr };
        std::thread thread;
    };
}

int WINAPI wWinMain (HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Fil de la fenêtre : STA (exigé par WebView2)
    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);

    const auto data = knownFolder (FOLDERID_LocalAppData) / L"JSG Guitar";
    std::error_code ec;
    std::filesystem::create_directories (data, ec);

    // Déjà lancé : on ramène sa fenêtre au premier plan
    HANDLE single = CreateMutexW (nullptr, TRUE, L"JSGGuitarPC.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW (jsg::windowClassName, nullptr))
        {
            if (IsIconic (existing)) ShowWindow (existing, SW_RESTORE);
            SetForegroundWindow (existing);
        }
        else
        {
            int port = 47811;
            std::ifstream (data / L"port.txt") >> port;
            openBrowserWindow (L"http://127.0.0.1:" + std::to_wstring (port) + L"/?app=1", data / L"Interface");
        }
        return 0;
    }

    const auto ui = exeFolder() / L"ui";
    if (! std::filesystem::exists (ui / L"index.html", ec))
    {
        message (L"Le dossier « ui » de l'interface est introuvable à côté de JSG Guitar.exe.\n\nRéinstallez le programme.");
        return 1;
    }

    EngineThread engine (ui, data);
    const int port = engine.waitForPort();
    if (port == 0)
    {
        engine.stop();
        message (L"Impossible d'ouvrir le serveur local (ports 47811 à 47820 occupés).");
        return 1;
    }
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring (port) + L"/?app=1";

    // Fenêtre de JSG Guitar (WebView2)
    jsg::WindowOptions options;
    options.url = url;
    options.userDataFolder = (data / L"WebView2").wstring();
    options.placementFile = (data / L"fenetre.txt").wstring();
    options.icon = LoadIconW (instance, MAKEINTRESOURCEW (1));
    const char* dev = std::getenv ("JSG_DEVTOOLS");
    options.devTools = dev != nullptr && dev[0] == '1';

    std::wstring error;
    if (jsg::runWebViewWindow (options, error) == jsg::WindowResult::unavailable)
    {
        // Pas de WebView2 : fenêtre d'application Edge, le programme s'arrête quand elle se ferme
        engine.log ("Fenêtre WebView2 impossible (" + jsg::toUtf8 (std::filesystem::path (error)) + ") : fenêtre Edge");
        engine.quitWhenWindowsClose();
        HANDLE browser = openBrowserWindow (url, data / L"Interface");
        if (browser != nullptr) CloseHandle (browser);
        engine.waitUntilStopped();
    }

    engine.stop();
    if (single != nullptr) { ReleaseMutex (single); CloseHandle (single); }
    CoUninitialize();
    return 0;
}
