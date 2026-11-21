// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#pragma once
/*
    Enregistreur (REC) de la version PC : WAV 24 bits stéréo, à la fréquence de la carte son.
    Le fil audio dépose les échantillons dans une file circulaire sans verrou ; un fil séparé
    les écrit sur le disque. Un disque lent ne fait donc pas craquer le son.
*/

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "paths.h"

namespace jsg
{

class Recorder
{
public:
    enum Source { everything = 0, guitarOnly = 1 };

    Recorder() : ring ((size_t) capacity * 2, 0.0f) {}
    ~Recorder() { stop(); }

    //==========================================================================
    // Fil de l'interface
    bool start (const std::filesystem::path& file, double sampleRate, int sourceToUse, std::string& error)
    {
        stop();
        std::error_code ec;
        std::filesystem::create_directories (file.parent_path(), ec);
        out.open (file, std::ios::binary | std::ios::trunc);
        if (! out) { error = "Impossible de créer le fichier " + toUtf8 (file); return false; }

        path = file;
        rate = (uint32_t) sampleRate;
        written = 0;
        writeHeader();
        source.store (sourceToUse);
        readPos.store (0); writePos.store (0); dropped.store (false);
        recording.store (true);
        running = true;
        writer = std::thread ([this] { writerLoop(); });
        return true;
    }

    /** Arrête et ferme le fichier ; renvoie son chemin (vide si rien n'était enregistré). */
    std::filesystem::path stop()
    {
        if (! running) return {};
        recording.store (false);
        running = false;
        if (writer.joinable()) writer.join();
        drain();
        finishHeader();
        out.close();
        return path;
    }

    bool   isRecording() const noexcept       { return recording.load(); }
    int    getSource() const noexcept         { return source.load(); }
    double seconds() const noexcept           { return rate > 0 ? (double) frames.load() / rate : 0.0; }

    //==========================================================================
    // Fil audio
    void push (const float* left, const float* right, int n, int fromSource) noexcept
    {
        if (! recording.load() || fromSource != source.load()) return;

        const size_t w = writePos.load (std::memory_order_relaxed), r = readPos.load (std::memory_order_acquire);
        if ((w - r) + (size_t) n > (size_t) capacity) { dropped.store (true); return; }   // disque trop lent

        for (int i = 0; i < n; ++i)
        {
            const size_t at = ((w + (size_t) i) % (size_t) capacity) * 2;
            ring[at] = left[i];
            ring[at + 1] = right[i];
        }
        writePos.store (w + (size_t) n, std::memory_order_release);
        frames.fetch_add ((uint64_t) n);
    }

private:
    static constexpr int capacity = 48000 * 10;   // 10 s d'avance possible

    void writerLoop()
    {
        while (running)
        {
            drain();
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
    }

    void drain()
    {
        const size_t r = readPos.load (std::memory_order_relaxed), w = writePos.load (std::memory_order_acquire);
        if (w == r) return;

        buffer.resize ((w - r) * 6);
        size_t o = 0;
        for (size_t i = r; i < w; ++i)
        {
            const size_t at = (i % (size_t) capacity) * 2;
            for (int c = 0; c < 2; ++c)
            {
                const float x = ring[at + (size_t) c] < -1.0f ? -1.0f : (ring[at + (size_t) c] > 1.0f ? 1.0f : ring[at + (size_t) c]);
                const auto v = (int32_t) std::lround (x * 8388607.0f);
                buffer[o++] = (char) (v & 0xff);
                buffer[o++] = (char) ((v >> 8) & 0xff);
                buffer[o++] = (char) ((v >> 16) & 0xff);
            }
        }
        out.write (buffer.data(), (std::streamsize) o);
        written += (uint64_t) o;
        readPos.store (w, std::memory_order_release);
    }

    void put32 (uint32_t v) { char b[4] = { (char) v, (char) (v >> 8), (char) (v >> 16), (char) (v >> 24) }; out.write (b, 4); }
    void put16 (uint16_t v) { char b[2] = { (char) v, (char) (v >> 8) }; out.write (b, 2); }

    void writeHeader()
    {
        out.write ("RIFF", 4); put32 (36); out.write ("WAVEfmt ", 8);
        put32 (16); put16 (1); put16 (2); put32 (rate); put32 (rate * 6); put16 (6); put16 (24);
        out.write ("data", 4); put32 (0);
        frames.store (0);
    }

    void finishHeader()
    {
        const auto dataBytes = (uint32_t) std::min<uint64_t> (written, 0xffffffffull - 36);
        out.seekp (4);  put32 (36 + dataBytes);
        out.seekp (40); put32 (dataBytes);
        out.seekp (0, std::ios::end);
    }

    std::vector<float> ring;
    std::vector<char> buffer;
    std::atomic<size_t> readPos { 0 }, writePos { 0 };
    std::atomic<uint64_t> frames { 0 };
    std::atomic<bool> recording { false }, dropped { false };
    std::atomic<int> source { everything };

    std::ofstream out;
    std::filesystem::path path;
    std::thread writer;
    bool running = false;
    uint32_t rate = 48000;
    uint64_t written = 0;
};

} // namespace jsg
