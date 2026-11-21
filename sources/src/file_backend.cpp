// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Carte son « fichier », pour les essais sans carte son (Linux) : l'entrée lit un fichier WAV
    en boucle au rythme réel, la sortie est enregistrée dans un fichier WAV.
    Variables : JSG_TEST_INPUT (fichier d'entrée), JSG_TEST_OUTPUT (fichier de sortie).
*/
#include "audio_backend.h"
#include "decode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace jsg
{

class FileBackend final : public AudioBackend
{
public:
    ~FileBackend() override { stop(); }

    std::vector<AudioDevice> inputs() override   { return { { "file", "Fichier de test", true } }; }
    std::vector<AudioDevice> outputs() override  { return { { "file", "Fichier de sortie", true } }; }

    bool start (const AudioConfig& config, Prepare prepare, Callback callback, AudioInfo& info, std::string& error) override
    {
        stop();
        const char* inPath = std::getenv ("JSG_TEST_INPUT");
        input.clear();

        if (inPath != nullptr && config.inputId != "none")
        {
            std::ifstream f (inPath, std::ios::binary);
            std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (f)), {});
            DecodedAudio audio;
            if (! decodeAudio (bytes.data(), bytes.size(), audio, error)) return false;
            input = firstChannel (audio);
        }

        rate = config.sampleRate > 0 ? config.sampleRate : 48000;
        block = config.bufferFrames;
        prepare (rate, block);

        info.running = true;
        info.exclusive = config.exclusive;
        info.sampleRate = rate;
        info.blockSize = block;
        info.inputChannels = input.empty() && std::getenv ("JSG_TEST_LOOPBACK") == nullptr ? 0 : 1;
        info.outputChannels = 2;
        info.inputLatencyMs = info.outputLatencyMs = 1000.0 * block / rate;
        info.inputName = "Fichier de test";
        info.outputName = "Fichier de sortie";
        info.format = "32 bits flottant";

        running = true;
        thread = std::thread ([this, callback] { loop (callback); });
        return true;
    }

    void stop() override
    {
        running = false;
        if (thread.joinable()) thread.join();
    }

    bool isRunning() const override { return running; }

private:
    void loop (Callback callback)
    {
        std::vector<float> in ((size_t) block), l ((size_t) block), r ((size_t) block);
        const float* inputs[1] = { in.data() };
        float* outputs[2] = { l.data(), r.data() };
        std::ofstream out;
        if (const char* outPath = std::getenv ("JSG_TEST_OUTPUT")) out.open (outPath, std::ios::binary);
        size_t at = 0;

        // JSG_TEST_LOOPBACK=n : câble simulé, la sortie revient sur l'entrée n échantillons plus tard (avec un peu de bruit)
        const char* loop = std::getenv ("JSG_TEST_LOOPBACK");
        const size_t loopDelay = loop != nullptr ? (size_t) std::max (1, std::atoi (loop)) : 0;
        std::vector<float> line (loopDelay + (size_t) block, 0.0f);
        size_t lineAt = 0;
        uint32_t noise = 1;

        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * block / rate));

        while (running)
        {
            for (int i = 0; i < block; ++i)
            {
                in[(size_t) i] = input.empty() ? 0.0f : input[at];
                if (! input.empty() && ++at >= input.size()) at = 0;
                if (loopDelay > 0)
                {
                    noise = noise * 1664525u + 1013904223u;
                    in[(size_t) i] = 0.7f * line[(lineAt + (size_t) i) % line.size()] + 0.002f * ((float) (noise >> 8) / 8388608.0f - 1.0f);
                }
            }
            callback (inputs, input.empty() && loopDelay == 0 ? 0 : 1, outputs, 2, block);
            if (loopDelay > 0)
                for (int i = 0; i < block; ++i) line[(lineAt + loopDelay + (size_t) i) % line.size()] = l[(size_t) i];
            lineAt = (lineAt + (size_t) block) % line.size();
            if (out) for (int i = 0; i < block; ++i) { out.write ((const char*) &l[(size_t) i], 4); out.write ((const char*) &r[(size_t) i], 4); }

            next += period;
            std::this_thread::sleep_until (next);
        }
    }

    std::vector<float> input;
    std::thread thread;
    std::atomic<bool> running { false };
    int rate = 48000, block = 128;
};

std::unique_ptr<AudioBackend> createAudioBackend() { return std::make_unique<FileBackend>(); }

} // namespace jsg
