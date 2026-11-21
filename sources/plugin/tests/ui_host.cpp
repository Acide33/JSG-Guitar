// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
// Faux logiciel hôte pour tester l'interface du plug-in : le cœur tourne en temps réel avec une
// guitare simulée ; les réglages touchés dans l'interface sont affichés ; des commandes au clavier
// simulent l'automation et le rappel d'un projet.
#include "plugin_core.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>
using namespace jsg::plugin;

int main (int argc, char** argv)
{
    Core core;
    core.setInterfaceFolder (argv[1]);
    core.prepare (48000);
    core.onUserEdit = [] (int i, float v) { std::printf ("EDIT %d %.4f\n", i, v); std::fflush (stdout); };
    core.onStateChanged = [] { std::printf ("DIRTY\n"); std::fflush (stdout); };
    std::printf ("URL %s\n", core.interfaceUrl().c_str());
    std::fflush (stdout);

    std::atomic<bool> running { true };
    std::thread audio ([&]
    {
        std::vector<float> in (128), l (128), r (128);
        double phase = 0;
        auto next = std::chrono::steady_clock::now();
        while (running)
        {
            for (auto& x : in) { x = 0.3f * (float) std::sin (phase); phase += 2 * 3.14159265 * 110.0 / 48000.0; }
            const float* ins[1] = { in.data() };
            float* outs[2] = { l.data(), r.data() };
            core.process (ins, 1, outs, 2, 128);
            next += std::chrono::microseconds (2667);
            std::this_thread::sleep_until (next);
        }
    });

    // Commandes : « auto i v » (automation), « save » (affiche l'état), « load <json> » (rappel du projet), « quit »
    std::string line;
    while (std::getline (std::cin, line))
    {
        if (line.rfind ("auto ", 0) == 0) { int i; float v; std::sscanf (line.c_str() + 5, "%d %f", &i, &v); core.setFromHost (i, v); }
        else if (line == "save") { std::printf ("STATE %s\n", core.getState().c_str()); std::fflush (stdout); }
        else if (line.rfind ("load ", 0) == 0) { std::printf ("LOADED %d\n", core.setState (line.substr (5)) ? 1 : 0); std::fflush (stdout); }
        else if (line == "quit") break;
    }
    running = false;
    audio.join();
    return 0;
}
