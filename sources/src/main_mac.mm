// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — programme Mac (même fonctionnement que main_win.cpp sous Windows).

    Au lancement :
      1. un fil « moteur » démarre le son (Core Audio, voir coreaudio.mm) avec les réglages enregistrés,
         et un serveur local (127.0.0.1, port 47811) qui sert l'interface (JSG Guitar.app/Contents/Resources/ui) ;
      2. l'interface s'affiche dans la fenêtre de JSG Guitar (WKWebView, le moteur web de macOS).
    Quand la fenêtre se ferme, le programme s'arrête.

    Fichiers de l'utilisateur : ~/Library/Application Support/JSG Guitar (réglages audio, journal) ;
    enregistrements : Musique/JSG Guitar ; playbacks JSG : Musique/JSG Guitar/Playbacks.
    Variable d'environnement JSG_DEVTOOLS=1 : inspecteur web (clic droit > Inspecter l'élément).
*/
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "host.h"
#include "paths.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

namespace
{
    std::filesystem::path pathOf (NSURL* url) { return url != nil ? std::filesystem::path (url.fileSystemRepresentation) : std::filesystem::path(); }

    std::filesystem::path userFolder (NSSearchPathDirectory which)
    {
        NSURL* url = [[NSFileManager defaultManager] URLsForDirectory: which inDomains: NSUserDomainMask].firstObject;
        return pathOf (url);
    }

    NSString* toNS (const std::string& s) { return [NSString stringWithUTF8String: s.c_str()]; }

    /** Ouvre un dossier dans le Finder (depuis n'importe quel fil). */
    void reveal (const std::filesystem::path& folder)
    {
        NSString* path = toNS (jsg::toUtf8 (folder));
        dispatch_async (dispatch_get_main_queue(), ^{
            [[NSWorkspace sharedWorkspace] openURL: [NSURL fileURLWithPath: path isDirectory: YES]];
        });
    }

    //==========================================================================
    /** Fil « moteur » : le son, le serveur local et les messages de l'interface. */
    class EngineThread
    {
    public:
        EngineThread (std::filesystem::path uiFolder, std::filesystem::path dataFolder, std::filesystem::path musicFolder)
            : ui (std::move (uiFolder)), data (std::move (dataFolder)), music (std::move (musicFolder))
        {
            thread = std::thread ([this] { run(); });
        }

        ~EngineThread() { stop(); }

        int waitForPort() { return portFuture.get(); }

        void stop()
        {
            stopRequested = true;
            if (thread.joinable()) thread.join();
        }

    private:
        void run()
        {
            auto backend = jsg::createAudioBackend();
            jsg::Host h ({ ui, music, data / "audio.ini", data / "journal.txt" }, *backend);
            h.revealFolder = reveal;

            jsg::Server s;
            s.setRoot (ui);
            s.addFolder ("/playbacks/", music / "Playbacks");   // playbacks JSG copiés par l'utilisateur
            const int port = s.listen ({ 47811, 47812, 47813, 47814, 47815, 47816, 47817, 47818, 47819, 47820 });

            if (port != 0)
            {
                h.log ("Serveur local : http://127.0.0.1:" + std::to_string (port));
                h.attach (s);
                auto hostTick = s.onTick;
                s.onTick = [this, &s, hostTick]
                {
                    hostTick();
                    if (stopRequested) s.quit();
                };
            }
            // La fenêtre s'ouvre tout de suite : au premier lancement, le démarrage du son attend
            // la réponse à la demande d'accès au micro de macOS
            portPromise.set_value (port);
            if (port != 0) h.startAudio();

            if (port != 0) s.run();

            h.log ("Fermeture");
            h.stopAudio();
        }

        std::filesystem::path ui, data, music;
        std::promise<int> portPromise;
        std::future<int> portFuture = portPromise.get_future();
        std::atomic<bool> stopRequested { false };
        std::thread thread;
    };

    std::unique_ptr<EngineThread> engine;

    void fatal (NSString* text)
    {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"JSG Guitar";
        alert.informativeText = text;
        alert.alertStyle = NSAlertStyleWarning;
        [alert runModal];
    }

    bool isLocal (NSURL* url)
    {
        return url != nil && ([url.host isEqualToString: @"127.0.0.1"] || [url.scheme isEqualToString: @"blob"]
                              || [url.scheme isEqualToString: @"data"] || [url.scheme isEqualToString: @"about"]);
    }
}

//==============================================================================
@interface JSGAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, WKUIDelegate, WKNavigationDelegate, WKDownloadDelegate>
@property (strong) NSWindow* window;
@property (strong) WKWebView* webView;
@end

@implementation JSGAppDelegate

- (void) buildMenu
{
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle: @"JSG Guitar"];
    [appMenu addItemWithTitle: @"À propos de JSG Guitar" action: @selector (orderFrontStandardAboutPanel:) keyEquivalent: @""];
    [appMenu addItem: [NSMenuItem separatorItem]];
    [appMenu addItemWithTitle: @"Masquer JSG Guitar" action: @selector (hide:) keyEquivalent: @"h"];
    NSMenuItem* others = [appMenu addItemWithTitle: @"Masquer les autres" action: @selector (hideOtherApplications:) keyEquivalent: @"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [appMenu addItem: [NSMenuItem separatorItem]];
    [appMenu addItemWithTitle: @"Quitter JSG Guitar" action: @selector (terminate:) keyEquivalent: @"q"];
    appItem.submenu = appMenu;
    [bar addItem: appItem];

    // Menu Édition : nécessaire pour copier / coller dans les champs de texte (noms de presets…)
    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle: @"Édition"];
    [editMenu addItemWithTitle: @"Annuler" action: @selector (undo:) keyEquivalent: @"z"];
    [editMenu addItemWithTitle: @"Rétablir" action: @selector (redo:) keyEquivalent: @"Z"];
    [editMenu addItem: [NSMenuItem separatorItem]];
    [editMenu addItemWithTitle: @"Couper" action: @selector (cut:) keyEquivalent: @"x"];
    [editMenu addItemWithTitle: @"Copier" action: @selector (copy:) keyEquivalent: @"c"];
    [editMenu addItemWithTitle: @"Coller" action: @selector (paste:) keyEquivalent: @"v"];
    [editMenu addItemWithTitle: @"Tout sélectionner" action: @selector (selectAll:) keyEquivalent: @"a"];
    editItem.submenu = editMenu;
    [bar addItem: editItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle: @"Fenêtre"];
    [windowMenu addItemWithTitle: @"Réduire" action: @selector (performMiniaturize:) keyEquivalent: @"m"];
    [windowMenu addItemWithTitle: @"Plein écran" action: @selector (toggleFullScreen:) keyEquivalent: @"f"].keyEquivalentModifierMask
        = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    windowItem.submenu = windowMenu;
    [bar addItem: windowItem];
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = bar;
}

- (void) applicationDidFinishLaunching: (NSNotification*) notification
{
    [self buildMenu];

    const auto data  = userFolder (NSApplicationSupportDirectory) / "JSG Guitar";
    const auto music = userFolder (NSMusicDirectory) / "JSG Guitar";
    std::error_code ec;
    std::filesystem::create_directories (data, ec);
    std::filesystem::create_directories (music / "Playbacks", ec);

    const auto ui = std::filesystem::path (NSBundle.mainBundle.resourcePath.fileSystemRepresentation) / "ui";
    if (! std::filesystem::exists (ui / "index.html", ec))
    {
        fatal (@"Le dossier de l'interface est introuvable dans JSG Guitar.app.\n\nRetéléchargez le programme.");
        [NSApp terminate: nil];
        return;
    }

    engine = std::make_unique<EngineThread> (ui, data, music);
    const int port = engine->waitForPort();
    if (port == 0)
    {
        fatal (@"Impossible d'ouvrir le serveur local (ports 47811 à 47820 occupés).");
        [NSApp terminate: nil];
        return;
    }

    // Fenêtre
    const NSRect frame = NSMakeRect (0, 0, 1440, 1000);
    self.window = [[NSWindow alloc] initWithContentRect: frame
                                              styleMask: NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing: NSBackingStoreBuffered
                                                  defer: NO];
    self.window.title = @"JSG Guitar";
    self.window.minSize = NSMakeSize (800, 560);
    self.window.delegate = self;
    self.window.appearance = [NSAppearance appearanceNamed: NSAppearanceNameDarkAqua];
    self.window.backgroundColor = [NSColor blackColor];
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    [self.window center];
    [self.window setFrameAutosaveName: @"JSGGuitarFenetre"];   // position et taille gardées d'une fois sur l'autre

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];   // presets et IR gardés par l'interface
    config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    if (@available (macOS 12.3, *)) config.preferences.elementFullscreenEnabled = YES;

    const char* dev = std::getenv ("JSG_DEVTOOLS");
    const bool devTools = dev != nullptr && dev[0] == '1';
    if (devTools) [config.preferences setValue: @YES forKey: @"developerExtrasEnabled"];

    self.webView = [[WKWebView alloc] initWithFrame: frame configuration: config];
    self.webView.UIDelegate = self;
    self.webView.navigationDelegate = self;
    self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    if (@available (macOS 12.0, *)) self.webView.underPageBackgroundColor = [NSColor blackColor];
    if (@available (macOS 13.3, *)) self.webView.inspectable = devTools;
    if (! devTools) self.webView.allowsMagnification = NO;
    self.window.contentView = self.webView;

    NSString* address = [NSString stringWithFormat: @"http://127.0.0.1:%d/?app=1", port];
    [self.webView loadRequest: [NSURLRequest requestWithURL: [NSURL URLWithString: address]]];

    [self.window makeKeyAndOrderFront: nil];
    [self.window makeFirstResponder: self.webView];
    [NSApp activateIgnoringOtherApps: YES];
}

- (BOOL) applicationShouldTerminateAfterLastWindowClosed: (NSApplication*) sender { return YES; }

- (void) applicationWillTerminate: (NSNotification*) notification
{
    if (engine != nullptr) engine->stop();
    engine.reset();
}

//------------------------------------------------------------------------------
// Boîtes de dialogue de l'interface (alert, confirm, prompt) et choix de fichiers

- (void) webView: (WKWebView*) webView runJavaScriptAlertPanelWithMessage: (NSString*) message
      initiatedByFrame: (WKFrameInfo*) frame completionHandler: (void (^)(void)) completionHandler
{
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"JSG Guitar";
    alert.informativeText = message;
    [alert beginSheetModalForWindow: self.window completionHandler: ^(NSModalResponse r) { (void) r; completionHandler(); }];
}

- (void) webView: (WKWebView*) webView runJavaScriptConfirmPanelWithMessage: (NSString*) message
      initiatedByFrame: (WKFrameInfo*) frame completionHandler: (void (^)(BOOL)) completionHandler
{
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"JSG Guitar";
    alert.informativeText = message;
    [alert addButtonWithTitle: @"OK"];
    [alert addButtonWithTitle: @"Annuler"];
    [alert beginSheetModalForWindow: self.window completionHandler: ^(NSModalResponse r) { completionHandler (r == NSAlertFirstButtonReturn); }];
}

- (void) webView: (WKWebView*) webView runJavaScriptTextInputPanelWithPrompt: (NSString*) prompt
      defaultText: (NSString*) defaultText initiatedByFrame: (WKFrameInfo*) frame completionHandler: (void (^)(NSString*)) completionHandler
{
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"JSG Guitar";
    alert.informativeText = prompt;
    [alert addButtonWithTitle: @"OK"];
    [alert addButtonWithTitle: @"Annuler"];
    NSTextField* field = [[NSTextField alloc] initWithFrame: NSMakeRect (0, 0, 280, 24)];
    field.stringValue = defaultText ?: @"";
    alert.accessoryView = field;
    [alert beginSheetModalForWindow: self.window completionHandler: ^(NSModalResponse r) {
        completionHandler (r == NSAlertFirstButtonReturn ? field.stringValue : nil);
    }];
    [alert.window makeFirstResponder: field];
}

- (void) webView: (WKWebView*) webView runOpenPanelWithParameters: (WKOpenPanelParameters*) parameters
      initiatedByFrame: (WKFrameInfo*) frame completionHandler: (void (^)(NSArray<NSURL*>*)) completionHandler
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    [panel beginSheetModalForWindow: self.window completionHandler: ^(NSModalResponse r) {
        completionHandler (r == NSModalResponseOK ? panel.URLs : nil);
    }];
}

// Liens target=_blank : navigateur habituel, jamais une deuxième fenêtre
- (WKWebView*) webView: (WKWebView*) webView createWebViewWithConfiguration: (WKWebViewConfiguration*) configuration
   forNavigationAction: (WKNavigationAction*) action windowFeatures: (WKWindowFeatures*) features
{
    NSURL* url = action.request.URL;
    if (url != nil && ([url.scheme isEqualToString: @"https"] || [url.scheme isEqualToString: @"http"]))
        [[NSWorkspace sharedWorkspace] openURL: url];
    return nil;
}

//------------------------------------------------------------------------------
// Navigation et téléchargements (export de presets, prises du navigateur)

- (void) webView: (WKWebView*) webView decidePolicyForNavigationAction: (WKNavigationAction*) action
      decisionHandler: (void (^)(WKNavigationActionPolicy)) decisionHandler
{
    if (action.shouldPerformDownload) { decisionHandler (WKNavigationActionPolicyDownload); return; }

    NSURL* url = action.request.URL;
    if (! isLocal (url) && action.targetFrame != nil && action.targetFrame.isMainFrame)
    {
        if ([url.scheme isEqualToString: @"https"] || [url.scheme isEqualToString: @"http"] || [url.scheme isEqualToString: @"mailto"])
            [[NSWorkspace sharedWorkspace] openURL: url];
        decisionHandler (WKNavigationActionPolicyCancel);
        return;
    }
    decisionHandler (WKNavigationActionPolicyAllow);
}

- (void) webView: (WKWebView*) webView decidePolicyForNavigationResponse: (WKNavigationResponse*) response
      decisionHandler: (void (^)(WKNavigationResponsePolicy)) decisionHandler
{
    decisionHandler (response.canShowMIMEType ? WKNavigationResponsePolicyAllow : WKNavigationResponsePolicyDownload);
}

- (void) webView: (WKWebView*) webView navigationAction: (WKNavigationAction*) action didBecomeDownload: (WKDownload*) download
{
    download.delegate = self;
}

- (void) webView: (WKWebView*) webView navigationResponse: (WKNavigationResponse*) response didBecomeDownload: (WKDownload*) download
{
    download.delegate = self;
}

- (void) download: (WKDownload*) download decideDestinationUsingResponse: (NSURLResponse*) response
    suggestedFilename: (NSString*) suggestedFilename completionHandler: (void (^)(NSURL*)) completionHandler
{
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = suggestedFilename ?: @"JSG Guitar";
    panel.canCreateDirectories = YES;
    [panel beginSheetModalForWindow: self.window completionHandler: ^(NSModalResponse r) {
        if (r != NSModalResponseOK || panel.URL == nil) { completionHandler (nil); return; }
        [[NSFileManager defaultManager] removeItemAtURL: panel.URL error: nil];   // remplacer : WebKit refuse un fichier existant
        completionHandler (panel.URL);
    }];
}

- (void) download: (WKDownload*) download didFailWithError: (NSError*) error resumeData: (NSData*) resumeData
{
    const bool cancelled = ([error.domain isEqualToString: NSCocoaErrorDomain] && error.code == NSUserCancelledError)
                        || ([error.domain isEqualToString: NSURLErrorDomain] && error.code == NSURLErrorCancelled);
    if (! cancelled) fatal ([@"Échec de l'enregistrement du fichier : " stringByAppendingString: error.localizedDescription]);
}

// Le moteur web s'est arrêté (mémoire, plantage) : on recharge l'interface
- (void) webViewWebContentProcessDidTerminate: (WKWebView*) webView
{
    [webView reload];
}

@end

//==============================================================================
int main (int argc, const char* argv[])
{
    @autoreleasepool
    {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy: NSApplicationActivationPolicyRegular];
        JSGAppDelegate* delegate = [[JSGAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
