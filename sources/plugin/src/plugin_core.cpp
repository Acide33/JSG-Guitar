// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#include "plugin_core.h"
#include "decode.h"
#include "json.h"
#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace jsg::plugin
{

namespace
{
    enum
    {
        pPower, pMute, pChannel, pBright, pInput, pGain, pBass, pMid, pTreble, pPresence, pMaster,
        pCabOn, pCabType, pMicOn, pMicType, pRoomOn, pRoomType, pRoomLevel, pLowCut, pHighCut, pIrOn,
        pEqOn, pEq1, pEq2, pEq3, pEq4, pEq5, pOutput,
        pGateOn, pGateThreshold, pGateDecay, pCompOn, pCompLevel, pCompAttack, pDriveOn, pDriveType, pDrive,
        pFxOn, pFxChorus, pFxDelay, pFxReverb, pFxMix, pFxTime, pFxFeedback, pFxLevel,
        pInstrument, pBassCab, pDiBlend, pBassStrings
    };

    const char* base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string toBase64 (const std::vector<uint8_t>& in)
    {
        std::string out;
        out.reserve ((in.size() + 2) / 3 * 4);
        for (size_t i = 0; i < in.size(); i += 3)
        {
            const uint32_t v = (uint32_t) in[i] << 16 | (i + 1 < in.size() ? (uint32_t) in[i + 1] << 8 : 0u) | (i + 2 < in.size() ? in[i + 2] : 0u);
            out += base64Chars[(v >> 18) & 63];
            out += base64Chars[(v >> 12) & 63];
            out += i + 1 < in.size() ? base64Chars[(v >> 6) & 63] : '=';
            out += i + 2 < in.size() ? base64Chars[v & 63] : '=';
        }
        return out;
    }

    std::vector<uint8_t> fromBase64 (const std::string& in)
    {
        int table[256];
        std::fill (std::begin (table), std::end (table), -1);
        for (int i = 0; i < 64; ++i) table[(unsigned char) base64Chars[i]] = i;
        std::vector<uint8_t> out;
        out.reserve (in.size() / 4 * 3);
        uint32_t acc = 0;
        int bits = 0;
        for (unsigned char c : in)
        {
            if (table[c] < 0) continue;
            acc = acc << 6 | (uint32_t) table[c];
            bits += 6;
            if (bits >= 8) { bits -= 8; out.push_back ((uint8_t) (acc >> bits)); }
        }
        return out;
    }

    float defaultValue (int index)
    {
        std::string name;
        return paramInfo (index, name).def;
    }
}

//==============================================================================
Core::Core()
{
    for (int i = 0; i < numParams; ++i)
    {
        values[(size_t) i].store (defaultValue (i));
        changedByHost[(size_t) i].store (false);
    }
    slotTypes.fill (-1);
    tunerRing.assign (32768, 0.0f);
    engine.setDisposer (this);
    engine.prepare (48000.0, blockSize);
}

Core::~Core()
{
    stopServer();
    {
        // Commandes jamais exécutées : leurs objets n'ont pas été confiés au moteur
        const std::lock_guard<std::mutex> lock (commandLock);
        for (int i = 0; i < numCommands; ++i) { delete commands[(size_t) i].newPedal; delete commands[(size_t) i].newImpulse; }
        numCommands = 0;
    }
    engine.setDisposer (nullptr);
    collectGarbage();
}

//==============================================================================
// Fil audio
void Core::prepare (double newRate)
{
    // Appelé quand le traitement est arrêté : on peut toucher au moteur directement
    runCommands();
    engine.prepare (newRate, blockSize);
    inBlock.fill (0.0f);
    outLeft.fill (0.0f);
    outRight.fill (0.0f);
    fifoPos = 0;

    bool rateChanged;
    {
        const std::lock_guard<std::mutex> lock (stateLock);
        rateChanged = newRate != sampleRate;
        sampleRate = newRate;
    }
    if (rateChanged) rebuildImpulse();
    runCommands();
    collectGarbage();
}

void Core::process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int n) noexcept
{
    const int ch = inputChannel.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        float x = 0.0f;
        if (numInputs == 1 || (numInputs >= 2 && ch == 0)) x = inputs[0][i];
        else if (numInputs >= 2 && ch == 1)                 x = inputs[1][i];
        else if (numInputs >= 2)                            x = 0.5f * (inputs[0][i] + inputs[1][i]);

        inBlock[(size_t) fifoPos] = x;
        const float l = outLeft[(size_t) fifoPos], r = outRight[(size_t) fifoPos];
        if (numOutputs >= 2) { outputs[0][i] = l; outputs[1][i] = r; }
        else if (numOutputs == 1) outputs[0][i] = 0.5f * (l + r);

        if (++fifoPos == blockSize)
        {
            fifoPos = 0;
            processBlock();
        }
    }
    for (int c = 2; c < numOutputs; ++c) std::fill (outputs[c], outputs[c] + n, 0.0f);
}

void Core::processBlock() noexcept
{
    runCommands();

    if (bypass.load (std::memory_order_relaxed))
    {
        // Contourné : l'entrée ressort telle quelle, avec le même retard (la compensation reste juste)
        std::copy (inBlock.begin(), inBlock.end(), outLeft.begin());
        std::copy (inBlock.begin(), inBlock.end(), outRight.begin());
        return;
    }

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

    std::copy (inBlock.begin(), inBlock.end(), workMono.begin());
    engine.process (workMono.data(), workLeft.data(), workRight.data(), blockSize, p);
    std::copy (workLeft.begin(), workLeft.end(), outLeft.begin());
    std::copy (workRight.begin(), workRight.end(), outRight.begin());

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
// Commandes vers le fil audio et ramassage des objets retirés
void Core::post (const Command& c)
{
    const std::lock_guard<std::mutex> lock (commandLock);
    if (numCommands < (int) commands.size()) commands[(size_t) numCommands++] = c;
    else { delete c.newPedal; delete c.newImpulse; }
}

void Core::runCommands() noexcept
{
    std::unique_lock<std::mutex> lock (commandLock, std::try_to_lock);
    if (! lock.owns_lock()) return;
    for (int i = 0; i < numCommands; ++i)
    {
        auto& c = commands[(size_t) i];
        if (c.type == Command::pedal)        engine.setSlotPedal (c.slot, std::unique_ptr<Pedal> (c.newPedal), c.pedalType);
        else if (c.type == Command::impulse) engine.setImpulse (std::unique_ptr<Convolution> (c.newImpulse));
        c = {};
    }
    numCommands = 0;
}

void Core::dispose (std::unique_ptr<Pedal> p) noexcept
{
    const size_t w = trashWrite.load (std::memory_order_relaxed);
    if (w - trashRead.load (std::memory_order_acquire) >= trash.size()) { p.release(); return; }
    trash[w % trash.size()] = { p.release(), nullptr };
    trashWrite.store (w + 1, std::memory_order_release);
}

void Core::dispose (std::unique_ptr<Convolution> c) noexcept
{
    const size_t w = trashWrite.load (std::memory_order_relaxed);
    if (w - trashRead.load (std::memory_order_acquire) >= trash.size()) { c.release(); return; }
    trash[w % trash.size()] = { nullptr, c.release() };
    trashWrite.store (w + 1, std::memory_order_release);
}

void Core::collectGarbage()
{
    size_t r = trashRead.load (std::memory_order_relaxed);
    const size_t w = trashWrite.load (std::memory_order_acquire);
    for (; r != w; ++r)
    {
        auto& t = trash[r % trash.size()];
        delete t.pedal;
        delete t.impulse;
        t = {};
    }
    trashRead.store (r, std::memory_order_release);
}

//==============================================================================
// Réglages, pédales, IR
void Core::setFromHost (int index, float plain) noexcept
{
    if (index < 0 || index >= numParams) return;
    if (values[(size_t) index].exchange (plain, std::memory_order_relaxed) != plain)
    {
        changedByHost[(size_t) index].store (true, std::memory_order_relaxed);
        anyChangedByHost.store (true, std::memory_order_release);
    }
}

void Core::setPedal (int slot, int type)
{
    if (slot < 0 || slot >= numSlots) return;
    {
        const std::lock_guard<std::mutex> lock (stateLock);
        slotTypes[(size_t) slot] = type;
    }
    Command c;
    c.type = Command::pedal;
    c.slot = slot;
    c.pedalType = type;
    c.newPedal = engine.makePedal (type).release();
    post (c);
}

bool Core::setImpulseFile (const std::vector<uint8_t>& bytes, const std::string& name, std::string& error)
{
    DecodedAudio audio;
    if (! decodeAudio (bytes.data(), bytes.size(), audio, error)) return false;
    {
        const std::lock_guard<std::mutex> lock (stateLock);
        impulseFile = bytes;
        impulseName = name;
        impulseSource = firstChannel (audio);
        impulseRate = audio.sampleRate;
    }
    rebuildImpulse();
    return true;
}

void Core::rebuildImpulse()
{
    std::vector<float> samples;
    {
        const std::lock_guard<std::mutex> lock (stateLock);
        if (impulseSource.empty() || sampleRate <= 0.0) return;
        samples = resample (impulseSource, impulseRate, sampleRate);
    }
    Command c;
    c.type = Command::impulse;
    c.newImpulse = engine.makeImpulse (samples.data(), (int) samples.size()).release();
    post (c);
}

std::string Core::impulseShapeJson()
{
    // Forme de l'IR pour l'affichage (160 points, normalisée) ; stateLock déjà pris
    std::vector<float> shape (160, 0.0f);
    float peak = 0.0f;
    const size_t per = std::max ((size_t) 1, (impulseSource.size() + 159) / 160);
    for (size_t i = 0; i < 160; ++i)
        for (size_t j = i * per; j < std::min (impulseSource.size(), (i + 1) * per); ++j)
            shape[i] = std::max (shape[i], std::abs (impulseSource[j]));
    for (float s : shape) peak = std::max (peak, s);
    std::string o = "[";
    for (size_t i = 0; i < shape.size(); ++i) o += (i ? "," : "") + json::num (peak > 0.0f ? shape[i] / peak : 0.0f, 3);
    return o + "]";
}

//==============================================================================
// Projet
std::string Core::getState()
{
    std::ostringstream o;
    o << "{\"format\":\"JSG Guitar plug-in\",\"version\":1,\"values\":[";
    for (int i = 0; i < numParams; ++i) o << (i ? "," : "") << json::num (values[(size_t) i].load(), 5);
    const std::lock_guard<std::mutex> lock (stateLock);
    o << "],\"slots\":[";
    for (int s = 0; s < numSlots; ++s) o << (s ? "," : "") << slotTypes[(size_t) s];
    o << "],\"channel\":" << inputChannel.load() << ",\"bypass\":" << (bypass.load() ? "true" : "false")
      << ",\"preset\":" << json::quote (presetName)
      << ",\"irName\":" << json::quote (impulseName) << ",\"irFile\":\"" << toBase64 (impulseFile) << "\"}";
    return o.str();
}

bool Core::setState (const std::string& text)
{
    json::Value m;
    if (! json::parse (text, m) || m["format"].str() != "JSG Guitar plug-in") return false;

    const auto& items = m["values"].items;
    for (size_t i = 0; i < items.size() && i < (size_t) numParams; ++i) setFromHost ((int) i, (float) items[i].num());

    const auto& slots = m["slots"].items;
    for (int s = 0; s < numSlots; ++s)
    {
        const int type = s < (int) slots.size() ? slots[(size_t) s].integer (-1) : -1;
        int current;
        { const std::lock_guard<std::mutex> lock (stateLock); current = slotTypes[(size_t) s]; }
        if (type != current) setPedal (s, type);
    }
    inputChannel.store (jlimit (0, 2, m["channel"].integer()));
    bypass.store (m["bypass"].truthy());

    {
        const std::lock_guard<std::mutex> lock (stateLock);
        presetName = m["preset"].str();
    }

    const auto file = fromBase64 (m["irFile"].str());
    bool sameIr;
    { const std::lock_guard<std::mutex> lock (stateLock); sameIr = file == impulseFile; }
    if (! sameIr)
    {
        if (file.empty())
        {
            const std::lock_guard<std::mutex> lock (stateLock);
            impulseFile.clear(); impulseSource.clear(); impulseName.clear();
            Command c;
            c.type = Command::impulse;   // nouvelle IR nulle : l'ancienne est retirée
            post (c);
        }
        else
        {
            std::string error;
            setImpulseFile (file, m["irName"].str(), error);
        }
    }

    stateReloaded.store (true, std::memory_order_release);   // l'interface ouverte se remet à jour
    return true;
}

/** Tout l'état pour l'interface (à l'ouverture de la fenêtre, ou après un rappel du projet). */
std::string Core::stateMessage()
{
    std::ostringstream o;
    o << "{\"t\":\"state\",\"values\":[";
    for (int i = 0; i < numParams; ++i) o << (i ? "," : "") << json::num (values[(size_t) i].load(), 5);
    const std::lock_guard<std::mutex> lock (stateLock);
    o << "],\"slots\":[";
    for (int s = 0; s < numSlots; ++s) o << (s ? "," : "") << slotTypes[(size_t) s];
    o << "],\"channel\":" << inputChannel.load() << ",\"preset\":" << json::quote (presetName)
      << ",\"irName\":" << json::quote (impulseName) << ",\"irShape\":" << (impulseSource.empty() ? std::string ("null") : impulseShapeJson())
      << ",\"sampleRate\":" << json::num (sampleRate, 0) << ",\"latency\":" << blockSize << "}";
    return o.str();
}

//==============================================================================
// Interface
std::string Core::interfaceUrl()
{
    startServer();
    const std::lock_guard<std::mutex> lock (serverLock);
    return port > 0 ? "http://127.0.0.1:" + std::to_string (port) + "/?app=1&plugin=1" : std::string();
}

void Core::startServer()
{
    const std::lock_guard<std::mutex> lock (serverLock);
    if (server != nullptr) return;

    auto s = std::make_unique<Server>();
    s->setRoot (uiFolder);
    std::vector<int> ports;
    for (int p = 47830; p < 47990; ++p) ports.push_back (p);   // une instance par port
    port = s->listen (ports);
    if (port == 0) return;

    s->onOpen = [this] (int client) { server->sendText (client, stateMessage()); };
    s->onText = [this] (int client, const std::string& text) { onText (client, text); };
    s->onBinary = [this] (int client, const std::vector<uint8_t>& data) { onBinary (client, data); };
    s->onTick = [this] { tick(); };
    server = std::move (s);
    serverThread = std::thread ([this] { server->run(); });
}

void Core::stopServer()
{
    std::unique_ptr<Server> s;
    {
        const std::lock_guard<std::mutex> lock (serverLock);
        if (server == nullptr) return;
        server->quit();
    }
    if (serverThread.joinable()) serverThread.join();
    const std::lock_guard<std::mutex> lock (serverLock);
    server.reset();
    port = 0;
}

void Core::onText (int client, const std::string& text)
{
    json::Value m;
    if (! json::parse (text, m)) return;
    const auto& t = m["t"].str();

    if (t == "p")
    {
        const int i = m["i"].integer (-1);
        if (i < 0 || i >= numParams) return;
        const float v = (float) m["v"].num();
        values[(size_t) i].store (v);
        if (onUserEdit) onUserEdit (i, v);
    }
    else if (t == "all")
    {
        // Preset chargé dans l'interface : chaque réglage changé part vers le logiciel hôte
        const auto& items = m["values"].items;
        for (size_t i = 0; i < items.size() && i < (size_t) numParams; ++i)
        {
            const float v = (float) items[i].num();
            if (values[i].exchange (v) != v && onUserEdit) onUserEdit ((int) i, v);
        }
    }
    else if (t == "ped")
    {
        setPedal (m["slot"].integer (-1), m["type"].integer (-1));
        if (onStateChanged) onStateChanged();
    }
    else if (t == "chan")
    {
        inputChannel.store (jlimit (0, 2, m["channel"].integer()));
        if (onStateChanged) onStateChanged();
    }
    else if (t == "presetName")
    {
        bool changed;
        {
            const std::lock_guard<std::mutex> lock (stateLock);
            changed = presetName != m["name"].str();
            presetName = m["name"].str();
        }
        if (changed && onStateChanged) onStateChanged();
    }
    else if (t == "tuner")    { tunerRead.store (tunerWrite.load()); tunerOn.store (m["on"].truthy()); }
    else if (t == "hello")
    {
        std::ostringstream o;
        o << "{\"t\":\"status\",\"running\":true,\"plugin\":true,\"sampleRate\":" << json::num (sampleRate > 0 ? sampleRate : 48000.0, 0)
          << ",\"blockSize\":" << blockSize << ",\"exclusive\":false,\"inputLatencyMs\":0,\"outputLatencyMs\":0"
          << ",\"engineLatency\":" << engine.getLatencySamples() << ",\"inputChannels\":2,\"outputChannels\":2"
          << ",\"inputName\":\"\",\"outputName\":\"\",\"format\":\"\",\"note\":\"\",\"error\":\"\""
          << ",\"config\":{\"inputChannel\":" << inputChannel.load() << "}}";
        server->sendText (client, o.str());
    }
    else if (t == "getState") server->sendText (client, stateMessage());
    else if (t == "irName")   pendingIrName = m["name"].str();
}

void Core::onBinary (int client, const std::vector<uint8_t>& data)
{
    if (data.size() < 4 || std::memcmp (data.data(), "IRFL", 4) != 0) return;
    const std::vector<uint8_t> bytes (data.begin() + 4, data.end());
    std::string error;
    if (! setImpulseFile (bytes, pendingIrName, error))
    {
        server->sendText (client, "{\"t\":\"irLoaded\",\"ok\":false,\"error\":" + json::quote (error) + "}");
        return;
    }
    std::string shape;
    { const std::lock_guard<std::mutex> lock (stateLock); shape = impulseShapeJson(); }
    server->sendText (client, "{\"t\":\"irLoaded\",\"ok\":true,\"name\":" + json::quote (pendingIrName) + ",\"shape\":" + shape + "}");
    if (onStateChanged) onStateChanged();
}

void Core::tick()
{
    collectGarbage();

    // Projet rappelé par le logiciel hôte : l'interface reprend tout
    if (stateReloaded.exchange (false, std::memory_order_acquire))
    {
        for (auto& c : changedByHost) c.store (false);
        anyChangedByHost.store (false);
        server->broadcastText (stateMessage());
    }

    // Réglages changés par le logiciel hôte (automation) : montrés dans l'interface
    if (anyChangedByHost.exchange (false, std::memory_order_acquire))
        for (int i = 0; i < numParams; ++i)
            if (changedByHost[(size_t) i].exchange (false, std::memory_order_relaxed))
                server->broadcastText ("{\"t\":\"p\",\"i\":" + std::to_string (i) + ",\"v\":" + json::num (values[(size_t) i].load(), 5) + "}");

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
        std::ostringstream o;
        o << "{\"t\":\"m\",\"inPeak\":" << json::num (meterIn.exchange (0.0f), 5) << ",\"outPeak\":" << json::num (meterOut.exchange (0.0f), 5)
          << ",\"vuL\":" << json::num (vuL.load(), 5) << ",\"vuR\":" << json::num (vuR.load(), 5)
          << ",\"pos\":0,\"playing\":false,\"ended\":false,\"rec\":-1}";
        server->broadcastText (o.str());
    }
}

} // namespace jsg::plugin
