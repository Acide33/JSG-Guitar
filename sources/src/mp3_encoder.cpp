// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Conversion WAV -> MP3 avec Media Foundation (voir mp3_encoder.h).

    Étapes : lecture du WAV (dr_wav), rééchantillonnage à 44,1 ou 48 kHz si la carte son tourne
    plus vite (l'encodeur MP3 s'arrête à 48 kHz), passage en 16 bits avec un léger bruit de
    dither (l'encodeur de Windows n'accepte que du 16 bits), puis encodage par le « Sink Writer »
    de Media Foundation.
*/
#include "mp3_encoder.h"
#include "decode.h"
#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
#elif defined (__APPLE__)
 #include <AudioToolbox/AudioToolbox.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace jsg
{
namespace
{
    /** Lit le WAV et le prépare : stéréo entrelacé 16 bits, fréquence acceptée par l'encodeur. */
    bool loadPcm16 (const std::filesystem::path& wav, std::vector<int16_t>& pcm, int& rate, std::string& error)
    {
        std::ifstream f (wav, std::ios::binary);
        if (! f) { error = "prise introuvable"; return false; }
        std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (f)), {});
        DecodedAudio audio;
        if (! decodeAudio (bytes.data(), bytes.size(), audio, error)) return false;

        const size_t frames = audio.frames();
        if (frames == 0) { error = "prise vide"; return false; }

        // Deux canaux séparés
        std::vector<float> ch[2];
        for (int c = 0; c < 2; ++c)
        {
            ch[c].resize (frames);
            const int src = std::min (c, audio.channels - 1);
            for (size_t i = 0; i < frames; ++i) ch[c][i] = audio.interleaved[i * (size_t) audio.channels + (size_t) src];
        }

        // Fréquence : 44,1 ou 48 kHz (88,2 -> 44,1 ; 96 -> 48 ; le reste -> 48)
        const double from = audio.sampleRate;
        rate = (from == 44100.0 || from == 88200.0 || from == 176400.0) ? 44100 : 48000;
        if (std::abs (from - rate) > 0.5)
            for (auto& c : ch) c = resample (c, from, rate);

        // 16 bits avec dither triangulaire (évite le grain sur les fins de notes)
        const size_t n = std::min (ch[0].size(), ch[1].size());
        pcm.resize (n * 2);
        std::minstd_rand rng (12345);
        std::uniform_real_distribution<float> u (-0.5f, 0.5f);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
            {
                const float x = std::clamp (ch[c][i], -1.0f, 1.0f) * 32767.0f + u (rng) + u (rng);
                pcm[i * 2 + (size_t) c] = (int16_t) std::clamp (std::lround (x), -32768L, 32767L);
            }
        return true;
    }

#ifdef __APPLE__
    std::string osStatus (const char* what, OSStatus st)
    {
        char b[96];
        std::snprintf (b, sizeof b, "%s (erreur %d)", what, (int) st);
        return b;
    }

    /** Écrit un fichier AAC (.m4a) avec l'encodeur de macOS (ExtAudioFile). pcm : stéréo entrelacé 16 bits. */
    bool writeAac (const std::filesystem::path& out, const std::vector<int16_t>& pcm, int rate, int kbps, std::string& error)
    {
        const std::string pathText = toUtf8 (out);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) pathText.c_str(), (CFIndex) pathText.size(), false);
        if (url == nullptr) { error = "chemin du fichier AAC invalide"; return false; }

        AudioStreamBasicDescription aac {};
        aac.mFormatID = kAudioFormatMPEG4AAC;
        aac.mSampleRate = rate;
        aac.mChannelsPerFrame = 2;
        UInt32 size = sizeof aac;
        AudioFormatGetProperty (kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &aac);

        ExtAudioFileRef file = nullptr;
        OSStatus st = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &aac, nullptr, kAudioFileFlags_EraseFile, &file);
        CFRelease (url);
        if (st != noErr) { error = osStatus ("impossible de créer le fichier AAC", st); return false; }

        AudioStreamBasicDescription client {};
        client.mFormatID = kAudioFormatLinearPCM;
        client.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        client.mSampleRate = rate;
        client.mChannelsPerFrame = 2;
        client.mBitsPerChannel = 16;
        client.mBytesPerFrame = 4;
        client.mFramesPerPacket = 1;
        client.mBytesPerPacket = 4;
        st = ExtAudioFileSetProperty (file, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
        if (st != noErr) { ExtAudioFileDispose (file); error = osStatus ("format refusé par l'encodeur AAC", st); return false; }

        // Débit : on le règle sur le convertisseur, puis on le fait prendre en compte (ConverterConfig = NULL)
        AudioConverterRef converter = nullptr;
        size = sizeof converter;
        if (ExtAudioFileGetProperty (file, kExtAudioFileProperty_AudioConverter, &size, &converter) == noErr && converter != nullptr)
        {
            UInt32 bitrate = (UInt32) kbps * 1000;
            if (AudioConverterSetProperty (converter, kAudioConverterEncodeBitRate, sizeof bitrate, &bitrate) != noErr)
            {
                bitrate = 256000;   // débit refusé à cette fréquence : 256 kbit/s, accepté partout
                AudioConverterSetProperty (converter, kAudioConverterEncodeBitRate, sizeof bitrate, &bitrate);
            }
            CFArrayRef config = nullptr;
            ExtAudioFileSetProperty (file, kExtAudioFileProperty_ConverterConfig, sizeof config, &config);
        }

        const size_t frames = pcm.size() / 2;
        constexpr size_t chunk = 4096;
        for (size_t at = 0; at < frames && st == noErr; at += chunk)
        {
            const UInt32 n = (UInt32) std::min (chunk, frames - at);
            AudioBufferList list {};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = 2;
            list.mBuffers[0].mDataByteSize = n * 4;
            list.mBuffers[0].mData = (void*) (pcm.data() + at * 2);
            st = ExtAudioFileWrite (file, n, &list);
        }
        const OSStatus closed = ExtAudioFileDispose (file);
        if (st != noErr) { error = osStatus ("échec de l'encodage AAC", st); return false; }
        if (closed != noErr) { error = osStatus ("échec de l'écriture du fichier AAC", closed); return false; }
        return true;
    }
#endif

#ifdef _WIN32
    // Identifiants Media Foundation (valeurs de mfapi.h / mfidl.h), définis ici pour ne pas dépendre de mfuuid.lib
    constexpr GUID kMajorType        = { 0x48eba18e, 0xf8c9, 0x4687, { 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f } };
    constexpr GUID kSubtype          = { 0xf7e34c9a, 0x42e8, 0x4714, { 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5 } };
    constexpr GUID kAllIndependent   = { 0xc9173739, 0x5e56, 0x461c, { 0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f } };
    constexpr GUID kNumChannels      = { 0x37e48bf5, 0x645e, 0x4c5b, { 0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a } };
    constexpr GUID kSamplesPerSecond = { 0x5faeeae7, 0x0290, 0x4c31, { 0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba } };
    constexpr GUID kAvgBytes         = { 0x1aab75c8, 0xcfef, 0x451c, { 0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31 } };
    constexpr GUID kBlockAlign       = { 0x322de230, 0x9eeb, 0x43bd, { 0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d } };
    constexpr GUID kBitsPerSample    = { 0xf2deb57f, 0x40fa, 0x4764, { 0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69 } };
    constexpr GUID kAudio            = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    constexpr GUID kFormatPcm        = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    constexpr GUID kFormatMp3        = { 0x00000055, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    constexpr GUID kContainerType    = { 0x150ff23f, 0x4abc, 0x478b, { 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca } };
    constexpr GUID kContainerMp3     = { 0xe438b912, 0x83f1, 0x4de6, { 0x9e, 0x3a, 0x9f, 0xfb, 0xc6, 0xdd, 0x24, 0xd1 } };

    template <class T> void release (T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

    std::string hr (const char* step, HRESULT h)
    {
        char b[96];
        std::snprintf (b, sizeof b, "%s (erreur 0x%08lX)", step, (unsigned long) h);
        return b;
    }

    bool writeMp3 (const std::filesystem::path& mp3, const std::vector<int16_t>& pcm, int rate, int kbps, std::string& error)
    {
        IMFAttributes* attributes = nullptr;
        IMFSinkWriter* writer = nullptr;
        IMFMediaType* outType = nullptr;
        IMFMediaType* inType = nullptr;
        DWORD stream = 0;
        bool ok = false;

        HRESULT h = MFCreateAttributes (&attributes, 1);
        if (SUCCEEDED (h)) h = attributes->SetGUID (kContainerType, kContainerMp3);
        if (SUCCEEDED (h)) h = MFCreateSinkWriterFromURL (mp3.wstring().c_str(), nullptr, attributes, &writer);
        if (FAILED (h)) { error = hr ("encodeur MP3 de Windows indisponible", h); goto done; }

        // Sortie : MP3 stéréo au débit choisi
        h = MFCreateMediaType (&outType);
        if (SUCCEEDED (h)) h = outType->SetGUID (kMajorType, kAudio);
        if (SUCCEEDED (h)) h = outType->SetGUID (kSubtype, kFormatMp3);
        if (SUCCEEDED (h)) h = outType->SetUINT32 (kNumChannels, 2);
        if (SUCCEEDED (h)) h = outType->SetUINT32 (kSamplesPerSecond, (UINT32) rate);
        if (SUCCEEDED (h)) h = outType->SetUINT32 (kAvgBytes, (UINT32) (kbps * 1000 / 8));
        if (SUCCEEDED (h)) h = writer->AddStream (outType, &stream);
        if (FAILED (h)) { error = hr ("format MP3 refusé", h); goto done; }

        // Entrée : PCM 16 bits stéréo
        h = MFCreateMediaType (&inType);
        if (SUCCEEDED (h)) h = inType->SetGUID (kMajorType, kAudio);
        if (SUCCEEDED (h)) h = inType->SetGUID (kSubtype, kFormatPcm);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kNumChannels, 2);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kSamplesPerSecond, (UINT32) rate);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kBitsPerSample, 16);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kBlockAlign, 4);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kAvgBytes, (UINT32) rate * 4);
        if (SUCCEEDED (h)) h = inType->SetUINT32 (kAllIndependent, TRUE);
        if (SUCCEEDED (h)) h = writer->SetInputMediaType (stream, inType, nullptr);
        if (FAILED (h)) { error = hr ("format d'entrée refusé par l'encodeur MP3", h); goto done; }

        h = writer->BeginWriting();
        if (FAILED (h)) { error = hr ("début de l'encodage MP3", h); goto done; }

        {
            const size_t frames = pcm.size() / 2, chunk = 8192;
            for (size_t start = 0; start < frames; start += chunk)
            {
                const size_t n = std::min (chunk, frames - start);
                IMFMediaBuffer* buffer = nullptr;
                IMFSample* sample = nullptr;
                h = MFCreateMemoryBuffer ((DWORD) (n * 4), &buffer);
                if (SUCCEEDED (h))
                {
                    BYTE* data = nullptr;
                    h = buffer->Lock (&data, nullptr, nullptr);
                    if (SUCCEEDED (h))
                    {
                        std::memcpy (data, pcm.data() + start * 2, n * 4);
                        buffer->Unlock();
                        h = buffer->SetCurrentLength ((DWORD) (n * 4));
                    }
                }
                if (SUCCEEDED (h)) h = MFCreateSample (&sample);
                if (SUCCEEDED (h)) h = sample->AddBuffer (buffer);
                if (SUCCEEDED (h)) h = sample->SetSampleTime ((LONGLONG) (start * 10000000ULL / (ULONGLONG) rate));
                if (SUCCEEDED (h)) h = sample->SetSampleDuration ((LONGLONG) (n * 10000000ULL / (ULONGLONG) rate));
                if (SUCCEEDED (h)) h = writer->WriteSample (stream, sample);
                release (sample);
                release (buffer);
                if (FAILED (h)) { error = hr ("encodage MP3", h); goto done; }
            }
        }

        h = writer->Finalize();
        if (FAILED (h)) { error = hr ("fin de l'encodage MP3", h); goto done; }
        ok = true;

    done:
        release (inType);
        release (outType);
        release (writer);
        release (attributes);
        return ok;
    }
#endif
}

bool encodeMp3 (const std::filesystem::path& wav, const std::filesystem::path& mp3, int kbps, std::string& error)
{
    if (std::find (std::begin (mp3Bitrates), std::end (mp3Bitrates), kbps) == std::end (mp3Bitrates)) kbps = 320;

    std::vector<int16_t> pcm;
    int rate = 0;
    if (! loadPcm16 (wav, pcm, rate, error)) return false;

#ifdef _WIN32
    const bool comStarted = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
    const HRESULT started = MFStartup (MF_VERSION, MFSTARTUP_LITE);
    bool ok = false;
    if (FAILED (started)) error = hr ("Media Foundation indisponible", started);
    else
    {
        ok = writeMp3 (mp3, pcm, rate, kbps, error);
        MFShutdown();
    }
    if (comStarted) CoUninitialize();

    if (! ok)
    {
        std::error_code ec;
        std::filesystem::remove (mp3, ec);   // pas de fichier MP3 à moitié écrit
    }
    return ok;
#elif defined (__APPLE__)
    const bool ok = writeAac (mp3, pcm, rate, kbps, error);
    if (! ok)
    {
        std::error_code ec;
        std::filesystem::remove (mp3, ec);
    }
    return ok;
#else
    (void) mp3;
    error = "encodeur MP3 disponible seulement sous Windows et macOS";
    return false;
#endif
}

} // namespace jsg
