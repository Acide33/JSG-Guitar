// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#include "host.h"
#include "paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace jsg
{

namespace
{
    // Valeurs par défaut des réglages, dans l'ordre du moteur (voir js/data.js et moteur/api.cpp)
    constexpr float defaults[49] = {
        1, 0,  2, 0, 0, 5, 5, 5, 5, 5, 6,  1, 0, 1, 2, 1, 0, 5, 60, 12000, 0,  0, 0, 0, 0, 0, 0,  0,
        0, -55, 80,  0, 5, 10,  0, 0, 4,  0, 0, 0, 1, 5, 380, 3.5f, 3,  0, 0, 4, 0 };

    enum
    {
        pPower, pMute, pChannel, pBright, pInput, pGain, pBass, pMid, pTreble, pPresence, pMaster,
        pCabOn, pCabType, pMicOn, pMicType, pRoomOn, pRoomType, pRoomLevel, pLowCut, pHighCut, pIrOn,
        pEqOn, pEq1, pEq2, pEq3, pEq4, pEq5, pOutput,
        pGateOn, pGateThreshold, pGateDecay, pCompOn, pCompLevel, pCompAttack, pDriveOn, pDriveType, pDrive,
        pFxOn, pFxChorus, pFxDelay, pFxReverb, pFxMix, pFxTime, pFxFeedback, pFxLevel,
        pInstrument, pBassCab, pDiBlend, pBassStrings
    };

    std::string tag (const std::vector<uint8_t>& data)
    {
        return data.size() >= 4 ? std::string (data.begin(), data.begin() + 4) : std::string();
    }

    std::string timestamp (const char* format)
    {
        const auto now = std::time (nullptr);
        std::tm local {};
       #ifdef _WIN32
        localtime_s (&local, &now);
       #else
        localtime_r (&now, &local);
       #endif
        char buffer[64];
        std::strftime (buffer, sizeof buffer, format, &local);
        return buffer;
    }
}

//==============================================================================
Host::Host (Paths p, AudioBackend& b) : paths (std::move (p)), backend (b)
{
    for (int i = 0; i < numFixedParams; ++i) values[(size_t) i].store (defaults[i]);
    for (int slot = 0; slot < numSlots; ++slot)
    {
        values[(size_t) (numFixedParams + slot * paramsPerSlot)].store (0.0f);
        for (int k = 1; k < paramsPerSlot; ++k) values[(size_t) (numFixedParams + slot * paramsPerSlot + k)].store (0.5f);
    }
    slotTypes.fill (-1);
    tunerRing.assign (32768, 0.0f);
    engine.setDisposer (this);
    loadConfig();
    log ("--- JSG Guitar " + timestamp ("%Y-%m-%d %H:%M:%S"));
}

Host::~Host()
{
    stopAudio();
    if (decoder.joinable()) decoder.join();
    recorder.stop();
    if (converter.joinable()) converter.join();
    engine.setDisposer (nullptr);
    collectGarbage();
}

void Host::log (const std::string& line)
{
    if (paths.log.empty()) return;
    std::ofstream f (paths.log, std::ios::app);
    f << timestamp ("%H:%M:%S ") << line << "\n";
}

//==============================================================================
// Son
void Host::startAudio()
{
    const std::lock_guard<std::mutex> lock (audioLock);
    backend.stop();
    info = {};
    audioError.clear();

    const auto prep = [this] (int rate, int block) { prepare (rate, block); };
    const auto proc = [this] (const float* const* in, int ni, float* const* out, int no, int n) { process (in, ni, out, no, n); };

    std::string error;
    bool ok = backend.start (config, prep, proc, info, error);

    if (! ok && config.exclusive)
    {
        log ("Mode exclusif impossible : " + error + " -> mode partagé");
        AudioConfig shared = config;
        shared.exclusive = false;
        std::string sharedError;
        ok = backend.start (shared, prep, proc, info, sharedError);
        if (ok) info.note = "Mode exclusif impossible (" + error + ") : mode partagé utilisé.";
        else    error += " / partagé : " + sharedError;
    }

    if (! ok)
    {
        audioError = error;
        log ("Son non démarré : " + error);
    }
    else
    {
        log ("Son : " + info.inputName + " -> " + info.outputName + ", " + std::to_string (info.sampleRate) + " Hz, bloc "
             + std::to_string (info.blockSize) + (info.exclusive ? ", exclusif, " : ", partagé, ") + info.format
             + ", latence " + json::num (info.inputLatencyMs, 1) + " + " + json::num (info.outputLatencyMs, 1) + " ms");
        rebuildImpulse();
    }
}

void Host::stopAudio()
{
    const std::lock_guard<std::mutex> lock (audioLock);
    backend.stop();
    info.running = false;
}

//==============================================================================
// Fil audio
void Host::prepare (int sampleRate, int blockSize)
{
    // Appelé avant le démarrage du flux, quand le fil audio ne tourne pas
    probeRate = sampleRate;
    mono.assign ((size_t) blockSize, 0.0f);
    left.assign ((size_t) blockSize, 0.0f);
    right.assign ((size_t) blockSize, 0.0f);
    engine.prepare ((double) sampleRate, blockSize);
    player.prepare ((double) sampleRate);
}

/** Fil audio : un clic (demi-sinus de 16 échantillons) toutes les 0,4 s ; on note quand il revient sur l'entrée. */
void Host::processProbe (float* const* output, int outputChannels, int n) noexcept
{
    const float* in = mono.data();
    const int64_t spacing = (int64_t) (0.4 * probeRate), maxWait = (int64_t) (0.35 * probeRate);
    for (int i = 0; i < n; ++i)
    {
        const int64_t t = probe.time + i;
        const float x = std::abs (in[i]);
        float out = 0.0f;

        if (! probe.waiting)
        {
            if (probe.next - t < probeRate / 10) probe.quietPeak = std::max (probe.quietPeak, x);   // bruit juste avant le clic
            if (t >= probe.next)
            {
                probe.waiting = true;
                probe.clickAt = t;
                probe.threshold = std::clamp (probe.quietPeak * 4.0f, 0.015f, 0.3f);
                probe.quietPeak = 0.0f;
            }
        }

        if (probe.waiting)
        {
            const int64_t k = t - probe.clickAt;
            if (k < 16) out = 0.6f * std::sin (3.14159265f * ((float) k + 0.5f) / 16.0f);
            int result = 0;
            if (k > 0 && x > probe.threshold) result = (int) k;
            else if (k > maxWait) result = -1;
            if (result != 0)
            {
                probeResults[(size_t) probe.count++] = result;
                probe.waiting = false;
                probe.next = probe.clickAt + spacing;
                if (probe.count >= probeClicks)
                {
                    probing.store (false, std::memory_order_relaxed);
                    probeFinished.store (true, std::memory_order_release);
                    for (int c = 0; c < outputChannels; ++c) std::fill (output[c] + i, output[c] + n, 0.0f);
                    return;
                }
            }
        }

        for (int c = 0; c < outputChannels; ++c) output[c][i] = out;
    }
    probe.time += n;
}

/** Fil du serveur : résultat de la mesure vers l'interface (médiane des clics revenus). */
void Host::sendProbeResult()
{
    std::vector<int> got;
    for (int r : probeResults) if (r > 0) got.push_back (r);
    std::ostringstream o;
    o << "{\"t\":\"latency\"";
    if (got.size() < probeClicks / 2)
    {
        o << ",\"ok\":false,\"heard\":" << got.size() << "}";
        log ("Mesure de latence : clics non reçus (" + std::to_string (got.size()) + "/" + std::to_string (probeClicks) + ")");
    }
    else
    {
        std::sort (got.begin(), got.end());
        const int median = got[got.size() / 2];
        const double ms = 1000.0 * median / probeRate, spread = 1000.0 * (got.back() - got.front()) / probeRate;
        o << ",\"ok\":true,\"ms\":" << json::num (ms, 2) << ",\"samples\":" << median << ",\"spread\":" << json::num (spread, 2)
          << ",\"heard\":" << got.size() << "}";
        log ("Mesure de latence réelle : " + json::num (ms, 2) + " ms (" + std::to_string (median) + " échantillons, écart "
             + json::num (spread, 2) + " ms, bloc " + std::to_string (info.blockSize) + ", " + (info.exclusive ? "exclusif" : "partagé") + ")");
    }
    server->broadcastText (o.str());
}

void Host::post (const Command& c)
{
    const std::lock_guard<std::mutex> lock (commandLock);
    if (numCommands < (int) commands.size()) commands[(size_t) numCommands++] = c;
    else
    {
        delete c.newPedal;   // file pleine (ne devrait pas arriver) : on abandonne la commande
        delete c.newImpulse;
    }
}

void Host::runCommands() noexcept
{
    std::unique_lock<std::mutex> lock (commandLock, std::try_to_lock);
    if (! lock.owns_lock()) return;   // l'interface écrit en ce moment : on verra au bloc suivant

    for (int i = 0; i < numCommands; ++i)
    {
        auto& c = commands[(size_t) i];
        if (c.type == Command::pedal)        engine.setSlotPedal (c.slot, std::unique_ptr<Pedal> (c.newPedal), c.pedalType);
        else if (c.type == Command::impulse) engine.setImpulse (std::unique_ptr<Convolution> (c.newImpulse));
        c = {};
    }
    numCommands = 0;
}

void Host::dispose (std::unique_ptr<Pedal> p) noexcept
{
    const size_t w = trashWrite.load (std::memory_order_relaxed);
    if (w - trashRead.load (std::memory_order_acquire) >= trash.size()) { p.release(); return; }   // plein : fuite plutôt qu'une libération ici
    trash[w % trash.size()] = { p.release(), nullptr };
    trashWrite.store (w + 1, std::memory_order_release);
}

void Host::dispose (std::unique_ptr<Convolution> c) noexcept
{
    const size_t w = trashWrite.load (std::memory_order_relaxed);
    if (w - trashRead.load (std::memory_order_acquire) >= trash.size()) { c.release(); return; }
    trash[w % trash.size()] = { nullptr, c.release() };
    trashWrite.store (w + 1, std::memory_order_release);
}

void Host::collectGarbage()
{
    size_t r = trashRead.load (std::memory_order_relaxed);
    const size_t w = trashWrite.load (std::memory_order_acquire);
    for (; r < w; ++r)
    {
        auto& t = trash[r % trash.size()];
        delete t.pedal;
        delete t.impulse;
        t = {};
    }
    trashRead.store (r, std::memory_order_release);
    player.collectGarbage();
}

void Host::process (const float* const* input, int inputChannels, float* const* output, int outputChannels, int n)
{
    runCommands();

    // Réglages de l'interface -> paramètres du moteur (même calcul que le plug-in)
    const auto v = [this] (int i) { return values[(size_t) i].load (std::memory_order_relaxed); };
    const auto on = [&] (int i) { return v (i) > 0.5f; };
    const auto idx = [&] (int i) { return (int) std::lround (v (i)); };
    const auto knob = [&] (int i) { return v (i) * 0.1f; };

    auto& p = params;
    p.power = on (pPower);          p.mute = on (pMute);
    p.channel = idx (pChannel);     p.bright = on (pBright);        p.inputDb = v (pInput);
    p.gain = knob (pGain);          p.bass = knob (pBass);          p.mid = knob (pMid);
    p.treble = knob (pTreble);      p.presence = knob (pPresence);  p.master = knob (pMaster);
    p.bassGuitar = idx (pInstrument) == 1;
    p.bassCabType = idx (pBassCab); p.diBlend = knob (pDiBlend);
    p.cabOn = on (pCabOn);          p.cabType = idx (pCabType);
    p.micOn = on (pMicOn);          p.micType = idx (pMicType);
    p.roomOn = on (pRoomOn);        p.roomType = idx (pRoomType);   p.roomLevel = knob (pRoomLevel);
    p.lowCutHz = v (pLowCut);       p.highCutHz = v (pHighCut);     p.irOn = on (pIrOn);
    p.eqOn = on (pEqOn);
    for (int b = 0; b < 5; ++b) p.eqDb[(size_t) b] = v (pEq1 + b);
    p.outputDb = v (pOutput);
    auto& r = p.rack;
    r.gateOn = on (pGateOn);        r.gateThresholdDb = v (pGateThreshold);   r.gateDecayMs = v (pGateDecay);
    r.compOn = on (pCompOn);        r.compLevel = v (pCompLevel);             r.compAttackMs = v (pCompAttack);
    r.driveOn = on (pDriveOn);      r.driveType = idx (pDriveType);           r.drive = v (pDrive);
    r.fxOn = on (pFxOn);            r.chorusOn = on (pFxChorus);
    r.delayOn = on (pFxDelay);      r.reverbOn = on (pFxReverb);
    r.fxMix = v (pFxMix);           r.fxTimeMs = v (pFxTime);
    r.fxFeedback = v (pFxFeedback); r.fxLevel = v (pFxLevel);
    for (int slot = 0; slot < numSlots; ++slot)
    {
        const int base = numFixedParams + slot * paramsPerSlot;
        p.slots[(size_t) slot].on = v (base) > 0.5f;
        for (int k = 0; k < maxPedalKnobs; ++k) p.slots[(size_t) slot].knobs[(size_t) k] = v (base + 1 + k);
    }

    // Entrée : canal 1, canal 2 ou les deux
    const int ch = inputChannel.load (std::memory_order_relaxed);
    float* m = mono.data();
    if (inputChannels <= 0)                       std::fill (m, m + n, 0.0f);
    else if (inputChannels == 1 || ch == 0)       std::copy_n (input[0], n, m);
    else if (ch == 1)                             std::copy_n (input[1], n, m);
    else for (int i = 0; i < n; ++i)              m[i] = 0.5f * (input[0][i] + input[1][i]);

    // Mesure de latence en cours : seulement des clics en sortie (pas d'ampli : le câble ferait boucler le son)
    if (probeRequest.exchange (false, std::memory_order_acquire))
    {
        probe = {};
        probe.next = (int64_t) (0.3 * probeRate);
        probing.store (true, std::memory_order_relaxed);
    }
    if (probeCancel.exchange (false, std::memory_order_relaxed)) probing.store (false, std::memory_order_relaxed);
    if (probing.load (std::memory_order_relaxed))
    {
        processProbe (output, outputChannels, n);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (m[i]));
        if (peak > meterIn.load (std::memory_order_relaxed)) meterIn.store (peak, std::memory_order_relaxed);
        return;
    }

    engine.process (m, left.data(), right.data(), n, p);
    recorder.push (left.data(), right.data(), n, Recorder::guitarOnly);   // guitare seule : avant son volume

    // Volume en rampe sur le bloc (pas de clic quand on bouge le curseur)
    const auto applyGain = [&] (float& now, float target)
    {
        if (now == target && target == 1.0f) return;
        const float step = (target - now) / (float) n;
        for (int i = 0; i < n; ++i)
        {
            now += step;
            left[(size_t) i] *= now;
            right[(size_t) i] *= now;
        }
        now = target;
    };

    applyGain (guitarGainNow, guitarVolume.load (std::memory_order_relaxed));
    player.process (left.data(), right.data(), n);
    applyGain (masterGainNow, masterVolume.load (std::memory_order_relaxed));
    recorder.push (left.data(), right.data(), n, Recorder::everything);   // ce qu'on entend

    if (outputChannels >= 2)
    {
        std::copy_n (left.data(), n, output[0]);
        std::copy_n (right.data(), n, output[1]);
        for (int c = 2; c < outputChannels; ++c) std::fill (output[c], output[c] + n, 0.0f);
    }
    else if (outputChannels == 1)
    {
        for (int i = 0; i < n; ++i) output[0][i] = 0.5f * (left[(size_t) i] + right[(size_t) i]);
    }

    // Mesures
    const auto raise = [] (std::atomic<float>& a, float x) { if (x > a.load (std::memory_order_relaxed)) a.store (x, std::memory_order_relaxed); };
    raise (meterIn, engine.inputPeak);   engine.inputPeak = 0.0f;
    raise (meterOut, engine.outputPeak); engine.outputPeak = 0.0f;
    vuL.store (engine.vuLevel[0], std::memory_order_relaxed);
    vuR.store (engine.vuLevel[1], std::memory_order_relaxed);

    if (tunerOn.load (std::memory_order_relaxed))
    {
        const size_t w = tunerWrite.load (std::memory_order_relaxed);
        if (w + (size_t) engine.tunerCount - tunerRead.load (std::memory_order_acquire) <= tunerRing.size())
        {
            for (int i = 0; i < engine.tunerCount; ++i) tunerRing[(w + (size_t) i) % tunerRing.size()] = engine.tunerBuffer[(size_t) i];
            tunerWrite.store (w + (size_t) engine.tunerCount, std::memory_order_release);
        }
    }
}

//==============================================================================
// Interface
void Host::attach (Server& s)
{
    server = &s;
    s.onOpen = [this] (int client) { everConnected = true; sendStatus (client); };
    s.onText = [this] (int client, const std::string& text) { onText (client, text); };
    s.onBinary = [this] (int client, const std::vector<uint8_t>& data) { onBinary (client, data); };
    s.onTick = [this] { tick(); };
}

std::string Host::statusJson() const
{
    std::ostringstream o;
    o << "{\"t\":\"status\",\"running\":" << (info.running ? "true" : "false")
      << ",\"sampleRate\":" << info.sampleRate << ",\"blockSize\":" << info.blockSize
      << ",\"exclusive\":" << (info.exclusive ? "true" : "false")
      << ",\"inputLatencyMs\":" << json::num (info.inputLatencyMs, 2) << ",\"outputLatencyMs\":" << json::num (info.outputLatencyMs, 2)
      << ",\"inputChannels\":" << info.inputChannels << ",\"outputChannels\":" << info.outputChannels
      << ",\"inputName\":" << json::quote (info.inputName) << ",\"outputName\":" << json::quote (info.outputName)
      << ",\"format\":" << json::quote (info.format) << ",\"note\":" << json::quote (info.note)
      << ",\"error\":" << json::quote (audioError)
      << ",\"engineLatency\":" << engine.getLatencySamples()
      << ",\"inputQueueMs\":" << json::num (backend.inputQueueMs(), 2) << ",\"glitches\":" << backend.glitchCount()
      << ",\"config\":{\"inputId\":" << json::quote (config.inputId) << ",\"outputId\":" << json::quote (config.outputId)
      << ",\"exclusive\":" << (config.exclusive ? "true" : "false") << ",\"bufferFrames\":" << config.bufferFrames
      << ",\"sampleRate\":" << config.sampleRate << ",\"inputChannel\":" << inputChannel.load() << "}"
      << ",\"recordings\":" << json::quote (toUtf8 (paths.recordings)) << "}";
    return o.str();
}

void Host::sendStatus (int client)
{
    if (server == nullptr) return;
    if (client >= 0) server->sendText (client, statusJson());
    else             server->broadcastText (statusJson());
}

void Host::sendDevices (int client)
{
    const auto list = [] (const std::vector<AudioDevice>& devices)
    {
        std::string s = "[";
        for (size_t i = 0; i < devices.size(); ++i)
            s += (i ? "," : "") + std::string ("{\"id\":") + json::quote (devices[i].id) + ",\"name\":" + json::quote (devices[i].name)
               + ",\"default\":" + (devices[i].isDefault ? "true" : "false") + "}";
        return s + "]";
    };
    server->sendText (client, "{\"t\":\"devices\",\"inputs\":" + list (backend.inputs()) + ",\"outputs\":" + list (backend.outputs()) + "}");
}

void Host::onText (int client, const std::string& text)
{
    json::Value m;
    if (! json::parse (text, m)) return;
    const auto& t = m["t"].str();

    if (t == "p")
    {
        const int i = m["i"].integer (-1);
        if (i >= 0 && i < numParams) values[(size_t) i].store ((float) m["v"].num());
    }
    else if (t == "all")
    {
        const auto& items = m["values"].items;
        for (size_t i = 0; i < items.size() && i < (size_t) numParams; ++i) values[i].store ((float) items[i].num());
    }
    else if (t == "ped")
    {
        const int slot = m["slot"].integer (-1), type = m["type"].integer (-1);
        if (slot < 0 || slot >= numSlots) return;
        slotTypes[(size_t) slot] = type;
        Command c;
        c.type = Command::pedal;
        c.slot = slot;
        c.pedalType = type;
        c.newPedal = engine.makePedal (type).release();
        post (c);
    }
    else if (t == "chan")      inputChannel.store (jlimit (0, 2, m["channel"].integer()));
    else if (t == "tuner")     { tunerRead.store (tunerWrite.load()); tunerOn.store (m["on"].truthy()); }
    else if (t == "hello")     sendStatus (client);
    else if (t == "measureLatency")
    {
        if (! info.running || info.inputChannels <= 0)
            server->sendText (client, "{\"t\":\"latency\",\"ok\":false,\"error\":\"Le son ou l'entrée n'est pas démarré.\"}");
        else { probeFinished.store (false); probeRequest.store (true, std::memory_order_release); }
    }
    else if (t == "measureCancel") probeCancel.store (true);
    else if (t == "devices")   sendDevices (client);
    else if (t == "audio")
    {
        const auto& c = m["config"];
        config.inputId = c["inputId"].str();
        config.outputId = c["outputId"].str();
        config.exclusive = c["exclusive"].truthy();
        config.bufferFrames = jlimit (16, 1024, c["bufferFrames"].integer (128));
        config.sampleRate = c["sampleRate"].integer (48000);
        if (c.has ("inputChannel")) inputChannel.store (jlimit (0, 2, c["inputChannel"].integer()));
        saveConfig();
        startAudio();
        sendStatus (-1);
    }
    else if (t == "release")   { stopAudio(); audioError = "Carte son libérée pour les autres logiciels."; sendStatus (-1); }
    else if (t == "resume")    { startAudio(); sendStatus (-1); }
    else if (t == "irName")    pendingIrName = m["name"].str();
    else if (t == "songName")  pendingSongTitle = m["name"].str();
    else if (t == "loadFile")
    {
        // Fichier du dossier de l'interface (playbacks) ou dernière prise enregistrée
        std::filesystem::path file;
        if (m["path"].str() == "@lastTake") file = lastTake;
        else
        {
            const auto rel = fromUtf8 (m["path"].str()).lexically_normal();
            if (toUtf8 (rel).find ("..") == std::string::npos) file = paths.ui / rel;
        }
        std::ifstream f (file, std::ios::binary);
        if (! f) { server->sendText (client, "{\"t\":\"song\",\"ok\":false,\"error\":" + json::quote ("Fichier introuvable : " + toUtf8 (file)) + "}"); return; }
        std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (f)), {});
        loadSongBytes (std::move (bytes), m["name"].str());
    }
    else if (t == "play")      player.play();
    else if (t == "pause")     player.pause();
    else if (t == "stop")      player.stop();
    else if (t == "seek")      player.seek (m["sec"].num());
    else if (t == "speed")     player.setSpeed ((float) m["v"].num (1.0));
    else if (t == "transpose") player.setTranspose (m["v"].integer());
    else if (t == "volume")    player.setVolume ((float) m["v"].num (0.8));
    else if (t == "guitarVolume") guitarVolume.store (std::clamp ((float) m["v"].num (1.0), 0.0f, 1.0f));
    else if (t == "masterVolume") masterVolume.store (std::clamp ((float) m["v"].num (1.0), 0.0f, 1.0f));
    else if (t == "loop")      player.setLooping (m["on"].truthy());
    else if (t == "ab")
    {
        if (m["clear"].truthy()) player.clearLoopAB();
        else player.setLoopAB (m["a"].num (-1.0), m["b"].num (-1.0), m["on"].truthy());
    }
    else if (t == "recStart")
    {
        const std::string base = "JSG Guitar " + timestamp ("%Y-%m-%d %Hh%Mm%S") + (m["source"].integer() == 1 ? " (guitare)" : "");
        std::string error;
        bool busy;
        { const std::lock_guard<std::mutex> lock (convertLock); busy = converting; }
        if (info.sampleRate <= 0) error = "Le son n'est pas démarré.";
        else if (busy) error = "La prise précédente est encore en conversion MP3, réessayez dans un instant.";
        else
        {
            take = {};
            take.mp3 = m["format"].str() == "mp3";
            take.kbps = (int) m["kbps"].integer (320);
            take.wav = paths.recordings / fromUtf8 (base + ".wav");
            take.mp3File = paths.recordings / fromUtf8 (base + compressedExtension);
            recorder.start (take.wav, (double) info.sampleRate, m["source"].integer(), error);
        }
        if (! error.empty()) server->sendText (client, "{\"t\":\"recDone\",\"ok\":false,\"error\":" + json::quote (error) + "}");
    }
    else if (t == "recStop")
    {
        const auto file = recorder.stop();
        if (! file.empty() && take.mp3)
        {
            // Conversion en MP3 dans un fil à part ; la réponse part de tick() quand elle est finie
            if (converter.joinable()) converter.join();
            take.client = client;
            { const std::lock_guard<std::mutex> lock (convertLock); converting = true; convertDone = false; }
            converter = std::thread ([this, t = take]
            {
                std::string error;
                const bool ok = encodeMp3 (t.wav, t.mp3File, t.kbps, error);
                const std::lock_guard<std::mutex> lock (convertLock);
                convertOk = ok;
                convertError = error;
                convertDone = true;
            });
            return;
        }
        if (! file.empty()) lastTake = file;
        server->sendText (client, "{\"t\":\"recDone\",\"ok\":" + std::string (file.empty() ? "false" : "true")
                                      + ",\"name\":" + json::quote (toUtf8 (file.filename())) + ",\"path\":" + json::quote (toUtf8 (file)) + "}");
    }
    else if (t == "openFolder")
    {
        std::error_code ec;
        std::filesystem::create_directories (paths.recordings, ec);
        if (revealFolder) revealFolder (paths.recordings);
    }
}

void Host::onBinary (int client, const std::vector<uint8_t>& data)
{
    const auto kind = tag (data);
    std::vector<uint8_t> payload (data.begin() + (data.size() >= 4 ? 4 : (long) data.size()), data.end());

    if (kind == "SONG")      loadSongBytes (std::move (payload), pendingSongTitle);
    else if (kind == "IRFL") loadImpulseBytes (payload, pendingIrName, client);
}

void Host::loadSongBytes (std::vector<uint8_t> bytes, std::string title)
{
    if (decoder.joinable()) decoder.join();
    {
        const std::lock_guard<std::mutex> lock (decodedLock);
        decodeDone = false;
        decoded.reset();
        decodeError.clear();
    }

    decoder = std::thread ([this, bytes = std::move (bytes), title = std::move (title)]
    {
        DecodedAudio audio;
        std::string error;
        std::unique_ptr<Song> song;
        if (decodeAudio (bytes.data(), bytes.size(), audio, error))
            song = makeSong (audio, title, 1320);

        const std::lock_guard<std::mutex> lock (decodedLock);
        decoded = std::move (song);
        decodeError = error;
        decodeDone = true;
    });
}

void Host::loadImpulseBytes (const std::vector<uint8_t>& bytes, const std::string& name, int client)
{
    DecodedAudio audio;
    std::string error;
    std::ostringstream o;

    if (! decodeAudio (bytes.data(), bytes.size(), audio, error))
    {
        o << "{\"t\":\"irLoaded\",\"ok\":false,\"error\":" << json::quote (error) << "}";
        server->sendText (client, o.str());
        return;
    }

    impulseSource = firstChannel (audio);
    impulseRate = audio.sampleRate;
    impulseName = name;
    rebuildImpulse();

    // Forme de l'IR pour l'affichage (160 points, normalisée)
    std::vector<float> shape (160, 0.0f);
    float peak = 0.0f;
    const size_t per = jmax ((size_t) 1, (impulseSource.size() + 159) / 160);
    for (size_t i = 0; i < 160; ++i)
        for (size_t j = i * per; j < jmin (impulseSource.size(), (i + 1) * per); ++j)
            shape[i] = jmax (shape[i], std::abs (impulseSource[j]));
    for (float s : shape) peak = jmax (peak, s);

    o << "{\"t\":\"irLoaded\",\"ok\":true,\"name\":" << json::quote (name) << ",\"shape\":[";
    for (size_t i = 0; i < shape.size(); ++i) o << (i ? "," : "") << json::num (peak > 0.0f ? shape[i] / peak : 0.0f, 3);
    o << "]}";
    server->sendText (client, o.str());
}

void Host::rebuildImpulse()
{
    if (impulseSource.empty() || info.sampleRate <= 0) return;
    const auto samples = resample (impulseSource, impulseRate, (double) info.sampleRate);
    Command c;
    c.type = Command::impulse;
    c.newImpulse = engine.makeImpulse (samples.data(), (int) samples.size()).release();
    post (c);
}

/** Fin de la conversion MP3 : on garde le MP3 (le WAV est effacé), sinon le WAV, et on répond à l'interface. */
void Host::finishConversion()
{
    bool ok;
    std::string error;
    {
        const std::lock_guard<std::mutex> lock (convertLock);
        if (! convertDone) return;
        convertDone = false;
        converting = false;
        ok = convertOk;
        error = convertError;
    }
    if (converter.joinable()) converter.join();

    std::filesystem::path file = take.wav;
    std::string note;
    if (ok)
    {
        std::error_code ec;
        std::filesystem::remove (take.wav, ec);
        file = take.mp3File;
        log ("Prise convertie en MP3 " + std::to_string (take.kbps) + " kbit/s : " + toUtf8 (file.filename()));
    }
    else
    {
        note = "MP3 impossible (" + error + ") : la prise est gardée en WAV.";
        log (note);
    }
    lastTake = file;
    server->sendText (take.client, "{\"t\":\"recDone\",\"ok\":true,\"name\":" + json::quote (toUtf8 (file.filename()))
                                     + ",\"path\":" + json::quote (toUtf8 (file)) + ",\"note\":" + json::quote (note) + "}");
}

void Host::tick()
{
    collectGarbage();
    finishConversion();
    if (probeFinished.exchange (false, std::memory_order_acquire)) sendProbeResult();

    // Morceau décodé : on le confie au lecteur
    {
        std::unique_lock<std::mutex> lock (decodedLock);
        if (decodeDone)
        {
            decodeDone = false;
            std::ostringstream o;
            if (decoded != nullptr)
            {
                o << "{\"t\":\"song\",\"ok\":true,\"title\":" << json::quote (decoded->title)
                  << ",\"duration\":" << json::num ((double) decoded->length / decoded->sampleRate, 3)
                  << ",\"rate\":" << decoded->sampleRate << ",\"peaks\":[";
                for (size_t i = 0; i < decoded->overview.size(); ++i) o << (i ? "," : "") << json::num (decoded->overview[i], 3);
                o << "]}";
                player.setSong (std::move (decoded));
            }
            else
            {
                o << "{\"t\":\"song\",\"ok\":false,\"error\":" << json::quote (decodeError) << "}";
            }
            lock.unlock();
            server->broadcastText (o.str());
        }
    }

    // Accordeur : paquets de 2048 échantillons
    while (tunerOn.load() && tunerWrite.load (std::memory_order_acquire) - tunerRead.load() >= 2048)
    {
        std::vector<uint8_t> packet (4 + 2048 * 4);
        std::memcpy (packet.data(), "TUNR", 4);
        const size_t r = tunerRead.load();
        for (size_t i = 0; i < 2048; ++i)
        {
            const float x = tunerRing[(r + i) % tunerRing.size()];
            std::memcpy (packet.data() + 4 + i * 4, &x, 4);
        }
        tunerRead.store (r + 2048, std::memory_order_release);
        server->broadcastBinary (packet.data(), packet.size());
    }

    // Mesures : une trentaine de fois par seconde
    if (++ticks % 3 == 0 && server->numClients() > 0)
    {
        const bool ended = player.takeEnded();
        std::ostringstream o;
        o << "{\"t\":\"m\",\"inPeak\":" << json::num (meterIn.exchange (0.0f), 5) << ",\"outPeak\":" << json::num (meterOut.exchange (0.0f), 5)
          << ",\"vuL\":" << json::num (vuL.load(), 5) << ",\"vuR\":" << json::num (vuR.load(), 5)
          << ",\"pos\":" << json::num (player.getPosition(), 3) << ",\"playing\":" << (player.isPlaying() ? "true" : "false")
          << ",\"ended\":" << (ended ? "true" : "false")
          << ",\"rec\":" << (recorder.isRecording() ? json::num (recorder.seconds(), 1) : std::string ("-1")) << "}";
        server->broadcastText (o.str());
    }
}

//==============================================================================
// Réglages audio enregistrés (fichier texte clé=valeur)
void Host::saveConfig()
{
    if (paths.settings.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories (paths.settings.parent_path(), ec);
    std::ofstream f (paths.settings, std::ios::trunc);
    f << "input=" << config.inputId << "\noutput=" << config.outputId << "\nexclusive=" << (config.exclusive ? 1 : 0)
      << "\nbuffer=" << config.bufferFrames << "\nrate=" << config.sampleRate << "\nchannel=" << inputChannel.load() << "\n";
}

void Host::loadConfig()
{
    std::ifstream f (paths.settings);
    std::string line;
    while (std::getline (f, line))
    {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find ('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr (0, eq), value = line.substr (eq + 1);
        if (key == "input")          config.inputId = value;
        else if (key == "output")    config.outputId = value;
        else if (key == "exclusive") config.exclusive = value == "1";
        else if (key == "buffer")    config.bufferFrames = jlimit (16, 1024, std::atoi (value.c_str()));
        else if (key == "rate")      config.sampleRate = std::atoi (value.c_str());
        else if (key == "channel")   inputChannel.store (jlimit (0, 2, std::atoi (value.c_str())));
    }
}

} // namespace jsg
