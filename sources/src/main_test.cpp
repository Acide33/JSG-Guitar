// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Version de test (Linux) : le programme complet avec la carte son « fichier ».
    jsg_test <dossier ui> <dossier de travail>  — serveur sur http://127.0.0.1:47811/?app=1
*/
#include "host.h"

#include <csignal>
#include <cstdlib>
#include <cstdio>

static jsg::Server* serverInstance = nullptr;

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "jsg_test <ui> <dossier>\n"); return 1; }
    const std::filesystem::path work = argv[2];

    auto backend = jsg::createAudioBackend();
    jsg::Host host ({ argv[1], work / "Enregistrements", work / "audio.ini", work / "journal.txt" }, *backend);

    jsg::Server server;
    server.setRoot (argv[1]);
    if (const char* playbacks = std::getenv ("JSG_TEST_PLAYBACKS")) server.addFolder ("/playbacks/", playbacks);
    const int port = server.listen ({ 47811, 47812, 47813 });
    if (port == 0) { std::fprintf (stderr, "port occupé\n"); return 1; }
    std::printf ("http://127.0.0.1:%d/?app=1\n", port);
    std::fflush (stdout);

    host.attach (server);
    host.startAudio();

    serverInstance = &server;
    std::signal (SIGTERM, [] (int) { serverInstance->quit(); });
    std::signal (SIGINT, [] (int) { serverInstance->quit(); });
    server.run();
    return 0;
}
