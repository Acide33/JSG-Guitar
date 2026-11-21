// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Cœur du plug-in JSG Guitar (sans JUCE) : le moteur audio commun (moteur/engine.h), les
    réglages, le pédalier, la réponse impulsionnelle et l'accordeur, et le lien avec l'interface
    (la même page web que la version PC, servie en local et affichée dans la fenêtre du plug-in).

    Trois fils :
      - le fil audio du logiciel hôte : process(), sans jamais allouer, libérer ni attendre ;
      - le fil de l'interface du logiciel hôte : sauvegarde / rappel du projet (getState / setState) ;
      - le fil du serveur local : messages de l'interface, mesures, ménage.

    Le moteur travaille par blocs fixes de 32 échantillons (la réponse impulsionnelle l'exige) :
    le plug-in annonce donc 32 échantillons de latence, que le logiciel hôte compense.
*/

#include "engine.h"
#include "params.h"
#include "server.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jsg::plugin
{

class Core final : private AmpEngine::Disposer
{
public:
    static constexpr int blockSize = 32;

    Core();
    ~Core() override;

    //==========================================================================
    // Fil audio
    /** Hors traitement (plug-in inactif) : prépare le moteur pour cette fréquence. */
    void prepare (double sampleRate);
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int n) noexcept;
    void setBypass (bool b) noexcept { bypass.store (b, std::memory_order_relaxed); }
    bool isBypassed() const noexcept { return bypass.load (std::memory_order_relaxed); }
    int  latencySamples() const noexcept { return blockSize; }

    //==========================================================================
    // Réglages (valeurs réelles, voir params.h)
    float value (int index) const noexcept { return values[(size_t) index].load (std::memory_order_relaxed); }

    /** Changement venu du logiciel hôte (automation, projet) : appliqué et montré dans l'interface. */
    void setFromHost (int index, float plain) noexcept;

    /** Réglage changé dans l'interface : le plug-in le transmet au logiciel hôte (fil du serveur). */
    std::function<void (int index, float plain)> onUserEdit;
    /** Pédale posée, IR chargée, preset choisi… : le projet a changé (fil du serveur). */
    std::function<void()> onStateChanged;

    //==========================================================================
    // Projet : tout l'état du plug-in (réglages, pédales, IR, preset) en texte JSON
    std::string getState();
    bool setState (const std::string& json);

    //==========================================================================
    // Interface : serveur local démarré à la première ouverture de la fenêtre
    /** Adresse de l'interface (http://127.0.0.1:port/?app=1&plugin=1), vide si impossible. */
    std::string interfaceUrl();
    void setInterfaceFolder (const std::filesystem::path& folder) { uiFolder = folder; }

private:
    void processBlock() noexcept;
    void dispose (std::unique_ptr<Pedal>) noexcept override;
    void dispose (std::unique_ptr<Convolution>) noexcept override;

    struct Command
    {
        enum Type { pedal, impulse } type = pedal;
        int slot = -1, pedalType = -1;
        Pedal* newPedal = nullptr;
        Convolution* newImpulse = nullptr;
    };
    void post (const Command&);
    void runCommands() noexcept;
    void collectGarbage();

    void setPedal (int slot, int type);
    bool setImpulseFile (const std::vector<uint8_t>& bytes, const std::string& name, std::string& error);
    void rebuildImpulse();
    std::string stateMessage();
    std::string impulseShapeJson();

    // Fil du serveur
    void startServer();
    void stopServer();
    void onText (int client, const std::string& text);
    void onBinary (int client, const std::vector<uint8_t>& data);
    void tick();

    AmpEngine engine;
    EngineParams params;
    std::array<std::atomic<float>, numParams> values;
    std::array<std::atomic<bool>, numParams> changedByHost;   // à montrer dans l'interface
    std::atomic<bool> anyChangedByHost { false }, stateReloaded { false };
    std::atomic<int> inputChannel { 0 };
    std::atomic<bool> bypass { false }, tunerOn { false };
    std::atomic<float> meterIn { 0.0f }, meterOut { 0.0f }, vuL { 0.0f }, vuR { 0.0f };

    // État hors réglages (fils de l'interface et du serveur)
    std::mutex stateLock;
    std::array<int, numSlots> slotTypes;
    std::string presetName;
    std::vector<uint8_t> impulseFile;          // le fichier tel quel (sauvé dans le projet)
    std::string impulseName;
    std::vector<float> impulseSource;           // décodé, à sa fréquence d'origine
    double impulseRate = 0.0, sampleRate = 0.0;

    // Commandes vers le fil audio (essai de verrou côté audio : jamais d'attente)
    std::mutex commandLock;
    std::array<Command, 128> commands;
    int numCommands = 0;

    // Objets à détruire, rendus par le fil audio (un écrivain, un lecteur)
    struct Trash { Pedal* pedal = nullptr; Convolution* impulse = nullptr; };
    std::array<Trash, 512> trash;
    std::atomic<size_t> trashWrite { 0 }, trashRead { 0 };

    // Blocs fixes de 32 échantillons
    std::array<float, blockSize> inBlock {}, outLeft {}, outRight {}, workMono {}, workLeft {}, workRight {};
    int fifoPos = 0;

    // Accordeur
    std::vector<float> tunerRing;
    std::atomic<size_t> tunerWrite { 0 }, tunerRead { 0 };

    // Serveur local
    std::filesystem::path uiFolder;
    std::unique_ptr<Server> server;
    std::thread serverThread;
    std::mutex serverLock;
    int port = 0, ticks = 0;
    std::string pendingIrName;
};

} // namespace jsg::plugin
