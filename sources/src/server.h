// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Serveur local de la version PC (127.0.0.1 seulement) :
      - HTTP : sert les fichiers de l'interface (dossier ui) ;
      - WebSocket (/ws) : messages entre l'interface et le programme.
    Tout se passe dans un seul fil (run) ; send() et broadcast() s'appellent depuis ce fil,
    par exemple depuis onTick.
*/

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jsg
{

class Server
{
public:
    std::function<void (int client, const std::string& text)> onText;
    std::function<void (int client, const std::vector<uint8_t>& data)> onBinary;
    std::function<void (int client)> onOpen, onClose;
    std::function<void()> onTick;                      // appelé environ toutes les 10 ms

    Server();
    ~Server();

    /** Ouvre le port (le premier libre de la liste). Renvoie le port, ou 0. */
    int listen (const std::vector<int>& ports);

    void setRoot (const std::filesystem::path& folder) { root = folder; }

    /** Dossier supplémentaire pour les adresses qui commencent par `prefix` (ex. "/playbacks/") :
        cherché avant le dossier de l'interface. Sert aux playbacks rangés dans Musique/JSG Guitar. */
    void addFolder (const std::string& prefix, const std::filesystem::path& folder) { extraFolders.push_back ({ prefix, folder }); }

    /** Boucle du serveur, jusqu'à quit(). */
    void run();
    void quit() { running = false; }

    void sendText (int client, const std::string& text);
    void sendBinary (int client, const void* data, size_t size);
    void broadcastText (const std::string& text);
    void broadcastBinary (const void* data, size_t size);

    int  numClients() const;

private:
    struct Connection;
    void handleHttp (Connection&);
    void handleFrames (Connection&);
    void sendFrame (Connection&, int opcode, const void* data, size_t size);
    void closeConnection (Connection&);

    std::filesystem::path root;
    std::vector<std::pair<std::string, std::filesystem::path>> extraFolders;
    std::vector<Connection*> connections;
    intptr_t listener = -1;
    std::atomic<bool> running { true };   // quit() peut venir d'un autre fil
    int nextId = 1;
};

} // namespace jsg
