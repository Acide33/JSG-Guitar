// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Cœur de la version PC de JSG Guitar (sans JUCE) :
      - le moteur audio (moteur/engine.h, le même que la version web) ;
      - le lecteur de morceau (player.h) et l'enregistreur (recorder.h) ;
      - les messages de l'interface (la page web servie en local, voir server.h).

    Trois fils :
      - le fil audio (carte son) : traite le son, sans jamais allouer, libérer ni attendre ;
      - le fil du serveur : messages de l'interface, mesures, ménage ;
      - un fil de décodage, le temps de lire un morceau (quelques dixièmes de seconde).
*/

#include "audio_backend.h"
#include "decode.h"
#include "engine.h"
#include "json.h"
#include "mp3_encoder.h"
#include "player.h"
#include "recorder.h"
#include "server.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace jsg
{

class Host final : private AmpEngine::Disposer
{
public:
    struct Paths
    {
        std::filesystem::path ui;            // fichiers de l'interface
        std::filesystem::path recordings;    // dossier des enregistrements (REC)
        std::filesystem::path settings;      // fichier des réglages audio
        std::filesystem::path log;           // journal (diagnostic)
    };

    Host (Paths paths, AudioBackend& backend);
    ~Host() override;

    /** Branche le serveur : messages de l'interface et mesures envoyées en retour. */
    void attach (Server& server);

    /** Démarre le son avec les réglages enregistrés (exclusif, sinon partagé). */
    void startAudio();
    void stopAudio();

    /** Ouvre un dossier dans l'Explorateur (fourni par le programme Windows). */
    std::function<void (const std::filesystem::path&)> revealFolder;

    /** Vrai après au moins une connexion de l'interface (pour savoir quand quitter). */
    bool hadClient() const noexcept { return everConnected; }
    int  numClients() const noexcept { return server != nullptr ? server->numClients() : 0; }

    void log (const std::string& line);

private:
    //==========================================================================
    // Fil audio
    void prepare (int sampleRate, int blockSize);
    void process (const float* const* input, int inputChannels, float* const* output, int outputChannels, int frames);

    // Ramasseur : objets retirés par le fil audio, détruits dans le fil du serveur
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

    //==========================================================================
    // Fil du serveur
    void onText (int client, const std::string& text);
    void onBinary (int client, const std::vector<uint8_t>& data);
    void tick();

    void sendStatus (int client);
    void sendDevices (int client);
    void loadSongBytes (std::vector<uint8_t> bytes, std::string title);
    void loadImpulseBytes (const std::vector<uint8_t>& bytes, const std::string& name, int client);
    void rebuildImpulse();
    void collectGarbage();

    void saveConfig();
    void loadConfig();
    std::string statusJson() const;

    Paths paths;
    AudioBackend& backend;
    Server* server = nullptr;

    AmpEngine engine;
    Player player;

    // Lecteur : volume de la guitare (avant le mélange avec le morceau) et volume général (l'ensemble)
    std::atomic<float> guitarVolume { 1.0f }, masterVolume { 1.0f };
    float guitarGainNow = 1.0f, masterGainNow = 1.0f;   // fil audio : valeurs en cours (rampes sans clic)
    Recorder recorder;

    static constexpr int numFixedParams = 49, paramsPerSlot = 5;
    static constexpr int numParams = numFixedParams + numSlots * paramsPerSlot;
    std::array<std::atomic<float>, numParams> values;
    std::array<int, numSlots> slotTypes;
    EngineParams params;

    std::atomic<int> inputChannel { 0 };
    std::atomic<bool> tunerOn { false };
    std::atomic<float> meterIn { 0.0f }, meterOut { 0.0f }, vuL { 0.0f }, vuR { 0.0f };

    // Commandes vers le fil audio (essai de verrou côté audio : jamais d'attente)
    std::mutex commandLock;
    std::array<Command, 128> commands;
    int numCommands = 0;

    // Objets à détruire, rendus par le fil audio (file sans verrou, un écrivain, un lecteur)
    struct Trash { Pedal* pedal = nullptr; Convolution* impulse = nullptr; };
    std::array<Trash, 512> trash;
    std::atomic<size_t> trashWrite { 0 }, trashRead { 0 };

    // Accordeur : signal d'entrée vers l'interface
    std::vector<float> tunerRing;
    std::atomic<size_t> tunerWrite { 0 }, tunerRead { 0 };

    // Tampons du fil audio
    std::vector<float> mono, left, right;

    // Mesure de la latence réelle : un clic part sur la sortie et revient par un câble sur l'entrée
    static constexpr int probeClicks = 10;
    struct Probe
    {
        int64_t time = 0, next = 0, clickAt = 0;
        bool waiting = false;
        float quietPeak = 0.0f, threshold = 0.0f;
        int count = 0;
    };
    Probe probe;                                    // fil audio seulement
    std::array<int, probeClicks> probeResults {};   // écrits par le fil audio, lus après probeFinished
    std::atomic<bool> probeRequest { false }, probeCancel { false }, probing { false }, probeFinished { false };
    int probeRate = 48000;
    void processProbe (float* const* output, int outputChannels, int n) noexcept;
    void sendProbeResult();

    // Prise en MP3 : enregistrée en WAV, puis convertie dans un fil à part
    struct Take { bool mp3 = false; int kbps = 320; std::filesystem::path wav, mp3File; int client = -1; };
    Take take;
    std::thread converter;
    std::mutex convertLock;
    bool converting = false, convertDone = false, convertOk = false;
    std::string convertError;
    void finishConversion();

    // Son
    AudioConfig config;
    AudioInfo info;
    std::string audioError;
    std::mutex audioLock;

    // Réponse impulsionnelle : le fichier décodé, gardé pour la refaire si la fréquence change
    std::vector<float> impulseSource;
    double impulseRate = 0.0;
    std::string impulseName;

    // Morceau en cours de décodage
    std::thread decoder;
    std::mutex decodedLock;
    std::unique_ptr<Song> decoded;
    std::string decodeError;
    bool decodeDone = false;
    std::string pendingSongTitle, pendingIrName;

    std::filesystem::path lastTake;
    bool everConnected = false;
    int ticks = 0;
};

} // namespace jsg
