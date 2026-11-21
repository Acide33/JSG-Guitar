// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Carte son sous Windows : WASAPI, sans pilote ni bibliothèque à installer.

    MODE EXCLUSIF (par défaut) : JSG Guitar parle directement à la carte son, avec de petits blocs
    (128 échantillons à 48 kHz = 2,7 ms). Les autres logiciels ne peuvent plus jouer de son pendant
    ce temps (bouton « Libérer la carte son » dans les réglages audio).
    MODE PARTAGÉ : le son passe par le mélangeur de Windows (plus de latence, la carte reste libre).

    Un seul fil audio, réveillé par l'entrée et par la sortie :
      - ce qui arrive de l'entrée va dans une file ;
      - quand la sortie demande du son, le moteur traite des blocs de taille fixe pris dans la file.
    Si l'entrée a un peu de retard sur la sortie, la file absorbe l'écart (un bloc de plus).
*/
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>

#include "audio_backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <xmmintrin.h>

namespace jsg
{

namespace
{
    // Identifiants Windows définis ici, pour ne dépendre d'aucune bibliothèque supplémentaire
    const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    const GUID kSubtypePcm   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    template <typename T> void release (T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

    std::string narrow (const wchar_t* w)
    {
        if (w == nullptr) return {};
        const int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s ((size_t) std::max (0, n - 1), '\0');
        if (n > 1) WideCharToMultiByte (CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring widen (const std::string& s)
    {
        const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w ((size_t) std::max (0, n - 1), L'\0');
        if (n > 1) MultiByteToWideChar (CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::string hresultText (HRESULT hr)
    {
        switch (hr)
        {
            case AUDCLNT_E_DEVICE_IN_USE:           return "la carte son est utilisée par un autre logiciel en mode exclusif";
            case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "le mode exclusif est interdit pour cet appareil (Panneau Son > Propriétés > Avancé)";
            case AUDCLNT_E_UNSUPPORTED_FORMAT:      return "format audio refusé par l'appareil";
            case AUDCLNT_E_DEVICE_INVALIDATED:      return "appareil débranché ou désactivé";
            case AUDCLNT_E_BUFFER_SIZE_ERROR:       return "taille de tampon refusée";
            case AUDCLNT_E_INVALID_DEVICE_PERIOD:   return "période refusée par l'appareil";
            case E_ACCESSDENIED:                    return "accès refusé (micro bloqué dans Paramètres > Confidentialité > Microphone ?)";
            default: break;
        }
        char b[48];
        std::snprintf (b, sizeof b, "erreur 0x%08lX", (unsigned long) hr);
        return b;
    }

    /** Format d'échantillon de la carte son, et conversion vers / depuis des flottants. */
    struct SampleFormat
    {
        enum Kind { float32, int16, int24, int32 } kind = float32;   // int32 couvre aussi 24 bits dans 32
        int channels = 2, rate = 48000;

        int bytesPerSample() const { return kind == int16 ? 2 : (kind == int24 ? 3 : 4); }
        int frameBytes() const     { return bytesPerSample() * channels; }

        std::string describe() const
        {
            const char* k = kind == float32 ? "32 bits flottant" : kind == int16 ? "16 bits" : kind == int24 ? "24 bits" : "32 bits";
            return std::string (k) + ", " + std::to_string (channels) + (channels > 1 ? " canaux" : " canal");
        }

        WAVEFORMATEXTENSIBLE wave (int validBits = 0) const
        {
            WAVEFORMATEXTENSIBLE w {};
            w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            w.Format.nChannels = (WORD) channels;
            w.Format.nSamplesPerSec = (DWORD) rate;
            w.Format.wBitsPerSample = (WORD) (bytesPerSample() * 8);
            w.Format.nBlockAlign = (WORD) frameBytes();
            w.Format.nAvgBytesPerSec = (DWORD) (rate * frameBytes());
            w.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
            w.Samples.wValidBitsPerSample = (WORD) (validBits > 0 ? validBits : bytesPerSample() * 8);
            w.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : (channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0);
            w.SubFormat = kind == float32 ? kSubtypeFloat : kSubtypePcm;
            return w;
        }

        static bool fromWave (const WAVEFORMATEX* w, SampleFormat& out)
        {
            out.channels = w->nChannels;
            out.rate = (int) w->nSamplesPerSec;
            bool isFloat = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                isFloat = IsEqualGUID (reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (w)->SubFormat, kSubtypeFloat) != 0;
            if (isFloat && w->wBitsPerSample == 32) { out.kind = float32; return true; }
            if (! isFloat && w->wBitsPerSample == 16) { out.kind = int16; return true; }
            if (! isFloat && w->wBitsPerSample == 24) { out.kind = int24; return true; }
            if (! isFloat && w->wBitsPerSample == 32) { out.kind = int32; return true; }
            return false;
        }

        float read (const BYTE* p) const
        {
            switch (kind)
            {
                case float32: { float f; std::memcpy (&f, p, 4); return f; }
                case int16:   { int16_t v; std::memcpy (&v, p, 2); return (float) v / 32768.0f; }
                case int24:   { const int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24); return (float) v / 2147483648.0f; }
                case int32:   { int32_t v; std::memcpy (&v, p, 4); return (float) v / 2147483648.0f; }
            }
            return 0.0f;
        }

        void write (BYTE* p, float x) const
        {
            x = std::max (-1.0f, std::min (1.0f, x));
            switch (kind)
            {
                case float32: std::memcpy (p, &x, 4); break;
                case int16:   { const auto v = (int16_t) std::lround (x * 32767.0f); std::memcpy (p, &v, 2); break; }
                case int24:   { const auto v = (int32_t) std::lround (x * 8388607.0f); p[0] = (BYTE) v; p[1] = (BYTE) (v >> 8); p[2] = (BYTE) (v >> 16); break; }
                case int32:   { const auto v = (int32_t) std::llround ((double) x * 2147483647.0); std::memcpy (p, &v, 4); break; }
            }
        }
    };

    struct Endpoint
    {
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        SampleFormat format;
        UINT32 bufferFrames = 0;
        int validBits = 0;          // 24 : échantillons 24 bits rangés dans 32 bits
        std::string name;

        void close() { release (client); release (device); }
    };
}

//==============================================================================
class WasapiBackend final : public AudioBackend
{
public:
    WasapiBackend()
    {
        CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &enumerator);
    }

    ~WasapiBackend() override
    {
        stop();
        release (enumerator);
    }

    std::vector<AudioDevice> inputs() override   { return list (eCapture); }
    std::vector<AudioDevice> outputs() override  { return list (eRender); }

    bool isRunning() const override { return running.load(); }

    bool start (const AudioConfig& config, Prepare prepare, Callback callback, AudioInfo& info, std::string& error) override
    {
        stop();
        if (enumerator == nullptr) { error = "WASAPI indisponible"; return false; }

        const bool exclusive = config.exclusive;
        const bool withInput = config.inputId != "none";

        if (! open (eRender, config.outputId, output, error)) { closeAll(); return false; }
        if (withInput && ! open (eCapture, config.inputId, input, error)) { closeAll(); return false; }

        // Format : en exclusif, on cherche une fréquence et un format acceptés par les deux appareils
        if (exclusive)
        {
            bool found = false;
            for (int rate : { config.sampleRate, 48000, 44100, 96000, 88200 })
            {
                if (findExclusiveFormat (output, rate) && (! withInput || findExclusiveFormat (input, rate))) { found = true; break; }
            }
            if (! found) { error = "aucun format commun accepté en mode exclusif"; closeAll(); return false; }
        }
        else
        {
            if (! mixFormat (output, error) || (withInput && ! mixFormat (input, error))) { closeAll(); return false; }
            if (withInput && input.format.rate != output.format.rate)
            {
                error = "l'entrée (" + std::to_string (input.format.rate) + " Hz) et la sortie (" + std::to_string (output.format.rate)
                      + " Hz) n'ont pas la même fréquence : réglez-les pareil dans Windows (Panneau Son > Propriétés > Avancé)";
                closeAll();
                return false;
            }
        }

        const int rate = output.format.rate;

        // Taille des blocs
        if (! initialise (output, exclusive, config.bufferFrames, error)) { closeAll(); return false; }
        if (withInput && ! initialise (input, exclusive, (int) output.bufferFrames, error)) { closeAll(); return false; }

        block = exclusive ? (int) output.bufferFrames : std::max (32, (int) sharedPeriodFrames);
        block = std::min (block, 1024);

        renderEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        captureEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        output.client->SetEventHandle (renderEvent);
        if (withInput) input.client->SetEventHandle (captureEvent);

        HRESULT hr = output.client->GetService (__uuidof (IAudioRenderClient), (void**) &render);
        if (FAILED (hr)) { error = "sortie : " + hresultText (hr); closeAll(); return false; }
        if (withInput)
        {
            hr = input.client->GetService (__uuidof (IAudioCaptureClient), (void**) &capture);
            if (FAILED (hr)) { error = "entrée : " + hresultText (hr); closeAll(); return false; }
        }

        // Mesures de latence annoncées par Windows
        REFERENCE_TIME outLatency = 0, inLatency = 0;
        output.client->GetStreamLatency (&outLatency);
        if (withInput) input.client->GetStreamLatency (&inLatency);

        info = {};
        info.running = true;
        info.exclusive = exclusive;
        info.sampleRate = rate;
        info.blockSize = block;
        info.inputChannels = withInput ? std::min (2, input.format.channels) : 0;
        info.outputChannels = output.format.channels;
        const UINT32 queued = exclusive ? output.bufferFrames : std::min<UINT32> (output.bufferFrames, (UINT32) block * 2);
        info.outputLatencyMs = 1000.0 * queued / rate + outLatency / 10000.0;
        info.inputLatencyMs = withInput ? 1000.0 * block / rate + inLatency / 10000.0 : 0.0;
        info.inputName = withInput ? input.name : "(aucune entrée)";
        info.outputName = output.name;
        info.format = output.format.describe() + (withInput ? " / entrée " + input.format.describe() : "");

        sampleRate = rate;
        prepare (rate, block);
        inputFifo.assign ((size_t) block * 16 * 2, 0.0f);
        inCount = 0;
        outputFifo.assign ((size_t) block * 16 * 2, 0.0f);
        outCount = 0;
        inChannels = info.inputChannels;
        outChannels = output.format.channels;

        // Sortie amorcée avec du silence, puis départ
        BYTE* data = nullptr;
        UINT32 prefill = exclusive ? output.bufferFrames : output.bufferFrames;
        if (SUCCEEDED (render->GetBuffer (prefill, &data))) render->ReleaseBuffer (prefill, AUDCLNT_BUFFERFLAGS_SILENT);

        if (withInput) input.client->Start();
        hr = output.client->Start();
        if (FAILED (hr)) { error = "démarrage : " + hresultText (hr); closeAll(); return false; }

        running = true;
        audioThread = std::thread ([this, callback, exclusive] { loop (callback, exclusive); });
        return true;
    }

    void stop() override
    {
        running = false;
        if (audioThread.joinable()) audioThread.join();
        if (input.client != nullptr) input.client->Stop();
        if (output.client != nullptr) output.client->Stop();
        closeAll();
    }

private:
    //==========================================================================
    std::vector<AudioDevice> list (EDataFlow flow)
    {
        std::vector<AudioDevice> result;
        if (enumerator == nullptr) return result;

        std::wstring defaultId;
        IMMDevice* def = nullptr;
        if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (flow, eConsole, &def)))
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED (def->GetId (&id))) { defaultId = id; CoTaskMemFree (id); }
            release (def);
        }

        IMMDeviceCollection* collection = nullptr;
        if (FAILED (enumerator->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &collection))) return result;

        UINT count = 0;
        collection->GetCount (&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED (collection->Item (i, &device))) continue;
            LPWSTR id = nullptr;
            device->GetId (&id);
            AudioDevice d;
            d.id = narrow (id);
            d.name = friendlyName (device);
            d.isDefault = id != nullptr && defaultId == id;
            CoTaskMemFree (id);
            result.push_back (d);
            release (device);
        }
        release (collection);
        return result;
    }

    static std::string friendlyName (IMMDevice* device)
    {
        std::string name;
        IPropertyStore* store = nullptr;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &store)))
        {
            PROPVARIANT v;
            PropVariantInit (&v);
            if (SUCCEEDED (store->GetValue (kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow (v.pwszVal);
            PropVariantClear (&v);
            release (store);
        }
        return name.empty() ? "Appareil audio" : name;
    }

    bool open (EDataFlow flow, const std::string& id, Endpoint& e, std::string& error)
    {
        HRESULT hr;
        if (id.empty()) hr = enumerator->GetDefaultAudioEndpoint (flow, eConsole, &e.device);
        else            hr = enumerator->GetDevice (widen (id).c_str(), &e.device);

        if (FAILED (hr) || e.device == nullptr)
        {
            error = std::string (flow == eRender ? "sortie" : "entrée") + " introuvable (" + hresultText (hr) + ")";
            return false;
        }

        e.name = friendlyName (e.device);
        hr = e.device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &e.client);
        if (FAILED (hr)) { error = e.name + " : " + hresultText (hr); return false; }
        return true;
    }

    bool mixFormat (Endpoint& e, std::string& error)
    {
        WAVEFORMATEX* mix = nullptr;
        if (FAILED (e.client->GetMixFormat (&mix)) || mix == nullptr) { error = e.name + " : format inconnu"; return false; }
        const bool ok = SampleFormat::fromWave (mix, e.format);
        CoTaskMemFree (mix);
        if (! ok) error = e.name + " : format de Windows non géré";
        return ok;
    }

    bool findExclusiveFormat (Endpoint& e, int rate)
    {
        // Nombre de canaux de l'appareil d'après son format partagé (souvent 2, parfois 1 pour un micro)
        int channels = 2;
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED (e.client->GetMixFormat (&mix)) && mix != nullptr) { channels = mix->nChannels; CoTaskMemFree (mix); }

        for (int ch : { channels, 2, 1 })
            for (auto kind : { SampleFormat::float32, SampleFormat::int32, SampleFormat::int24, SampleFormat::int16 })
                for (int valid : { 0, 24 })
                {
                    if (valid == 24 && kind != SampleFormat::int32) continue;
                    SampleFormat f;
                    f.kind = kind; f.channels = ch; f.rate = rate;
                    auto w = f.wave (valid);
                    if (e.client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK)
                    {
                        e.format = f;
                        e.validBits = valid;
                        return true;
                    }
                }
        return false;
    }

    bool initialise (Endpoint& e, bool exclusive, int wantedFrames, std::string& error)
    {
        const auto wave = e.format.wave (e.validBits);
        HRESULT hr;

        if (exclusive)
        {
            REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
            e.client->GetDevicePeriod (&defaultPeriod, &minimumPeriod);
            REFERENCE_TIME period = (REFERENCE_TIME) std::llround (1.0e7 * wantedFrames / e.format.rate);
            period = std::max (period, minimumPeriod);

            hr = e.client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &wave.Format, nullptr);

            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
            {
                // Taille non alignée : on reprend avec la taille que l'appareil propose
                UINT32 frames = 0;
                e.client->GetBufferSize (&frames);
                release (e.client);
                e.device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &e.client);
                period = (REFERENCE_TIME) std::llround (1.0e7 * frames / e.format.rate);
                hr = e.client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &wave.Format, nullptr);
            }
        }
        else
        {
            // Partagé : petite période si Windows le permet (IAudioClient3, Windows 10 et plus)
            hr = E_FAIL;
            IAudioClient3* client3 = nullptr;
            if (SUCCEEDED (e.client->QueryInterface (__uuidof (IAudioClient3), (void**) &client3)))
            {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED (e.client->GetMixFormat (&mix)) && mix != nullptr)
                {
                    UINT32 defaultFrames = 0, fundamental = 0, minFrames = 0, maxFrames = 0;
                    if (SUCCEEDED (client3->GetSharedModeEnginePeriod (mix, &defaultFrames, &fundamental, &minFrames, &maxFrames)))
                    {
                        UINT32 frames = minFrames;
                        if (fundamental > 0) while (frames < (UINT32) wantedFrames && frames + fundamental <= maxFrames) frames += fundamental;
                        hr = client3->InitializeSharedAudioStream (AUDCLNT_STREAMFLAGS_EVENTCALLBACK, frames, mix, nullptr);
                        if (SUCCEEDED (hr)) sharedPeriodFrames = frames;
                    }
                    CoTaskMemFree (mix);
                }
                release (client3);
            }

            if (FAILED (hr))
            {
                release (e.client);
                e.device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &e.client);
                WAVEFORMATEX* mix = nullptr;
                e.client->GetMixFormat (&mix);
                hr = e.client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
                REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
                e.client->GetDevicePeriod (&defaultPeriod, &minimumPeriod);
                sharedPeriodFrames = (UINT32) std::llround ((double) defaultPeriod * e.format.rate / 1.0e7);
                CoTaskMemFree (mix);
            }
        }

        if (FAILED (hr)) { error = e.name + " : " + hresultText (hr); return false; }
        e.client->GetBufferSize (&e.bufferFrames);
        return true;
    }

    void closeAll()
    {
        release (render);
        release (capture);
        input.close();
        output.close();
        if (renderEvent != nullptr)  { CloseHandle (renderEvent); renderEvent = nullptr; }
        if (captureEvent != nullptr) { CloseHandle (captureEvent); captureEvent = nullptr; }
    }

    //==========================================================================
    // Fil audio
    void loop (Callback callback, bool exclusive)
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        if (task == nullptr) SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        // Nombres dénormaux remis à zéro (sinon les filtres qui s'éteignent font monter le processeur)
        _mm_setcsr (_mm_getcsr() | 0x8040);   // FTZ + DAZ

        const size_t nOut = (size_t) std::max (1, outChannels);
        std::vector<float> inBlock ((size_t) block * 2, 0.0f), outBlock ((size_t) block * nOut, 0.0f);
        const float* ins[2] = { inBlock.data(), inBlock.data() + block };
        std::vector<float*> outs (nOut);
        for (size_t c = 0; c < nOut; ++c) outs[c] = outBlock.data() + c * (size_t) block;

        outputFifo.assign ((size_t) std::max<UINT32> (output.bufferFrames, (UINT32) block * 4) * 2 * nOut, 0.0f);
        const size_t maxIn = (size_t) block * 8;   // au-delà : l'entrée a beaucoup trop d'avance, on jette

        // Partagé : on ne remplit pas tout le tampon de Windows, seulement deux périodes (moins de latence)
        const UINT32 sharedTarget = std::min<UINT32> (output.bufferFrames, (UINT32) block * 2);

        // Attente de l'entrée : quand le paquet de l'entrée arrive juste après le réveil de la sortie, on
        // l'attend un court instant (35 % de la période au plus) au lieu de prendre celui d'avant :
        // jusqu'à une période de gagnée. Si l'entrée n'arrive pas à temps plusieurs fois de suite, on arrête.
        LARGE_INTEGER qpf {};
        QueryPerformanceFrequency (&qpf);
        const LONGLONG waitTicks = (LONGLONG) (0.35 * qpf.QuadPart * block / std::max (1, sampleRate));
        bool waitForInput = capture != nullptr;
        int waitMisses = 0;

        // File d'entrée gardée au minimum : ce qui reste après chaque passage est surveillé, et l'excédent jeté
        size_t margin = 0, minLeft = SIZE_MAX, windowFrames = 0, warmup = (size_t) sampleRate / 4;
        size_t windowLength = (size_t) sampleRate / 4;    // premier réglage rapide, puis toutes les 2 s
        bool underrunInWindow = false;
        queueFrames.store (0);
        glitches.store (0);

        HANDLE events[2] = { renderEvent, captureEvent };
        const DWORD numEvents = capture != nullptr ? 2 : 1;

        while (running.load())
        {
            const DWORD w = WaitForMultipleObjects (numEvents, events, FALSE, 200);
            if (w == WAIT_TIMEOUT) continue;

            // Entrée : tout ce qui est arrivé va dans la file
            if (capture != nullptr) readCapture (maxIn);

            if (w != WAIT_OBJECT_0) continue;   // réveil par l'entrée seulement

            // Sortie : combien d'échantillons demande-t-elle ?
            UINT32 frames = output.bufferFrames;
            if (! exclusive)
            {
                UINT32 padding = 0;
                if (FAILED (output.client->GetCurrentPadding (&padding))) { running = false; break; }
                frames = padding < sharedTarget ? sharedTarget - padding : 0;
            }
            if (frames == 0) continue;

            // Combien d'entrée le moteur va consommer ; si elle n'est pas encore là, on l'attend un peu
            const size_t blocksNeeded = outCount >= frames ? 0 : (frames - outCount + (size_t) block - 1) / (size_t) block;
            const size_t need = blocksNeeded * (size_t) block;
            if (capture != nullptr && waitForInput && need > 0 && inCount < need)
            {
                LARGE_INTEGER now {};
                QueryPerformanceCounter (&now);
                const LONGLONG deadline = now.QuadPart + waitTicks;
                while (inCount < need)
                {
                    readCapture (maxIn);
                    if (inCount >= need) break;
                    QueryPerformanceCounter (&now);
                    if (now.QuadPart >= deadline) break;
                    for (int k = 0; k < 64; ++k) _mm_pause();
                }
                if (inCount >= need) waitMisses = 0;
                else if (warmup == 0 && ++waitMisses >= 8) waitForInput = false;   // l'entrée arrive trop tard : on n'attend plus
            }

            // Le moteur produit des blocs entiers jusqu'à en avoir assez
            while (outCount < (size_t) frames)
            {
                if (capture != nullptr && inCount < (size_t) block && warmup == 0)
                {
                    // L'entrée n'a pas suivi : on garde dorénavant un peu d'avance
                    underrunInWindow = true;
                    margin = std::min (margin + (size_t) block / 2, (size_t) block * 2);
                    glitches.fetch_add (1);
                }

                const int ch = std::max (1, inChannels);
                for (int i = 0; i < block; ++i)
                {
                    const bool have = (size_t) i < inCount;
                    for (int c = 0; c < 2; ++c)
                        inBlock[(size_t) c * (size_t) block + (size_t) i] = have ? inputFifo[(size_t) i * 2 + (size_t) std::min (c, ch - 1)] : 0.0f;
                }
                const size_t used = std::min (inCount, (size_t) block);
                std::memmove (inputFifo.data(), inputFifo.data() + used * 2, (inCount - used) * 2 * sizeof (float));
                inCount -= used;

                callback (ins, inChannels, outs.data(), outChannels, block);

                if ((outCount + (size_t) block) * nOut > outputFifo.size()) outCount = 0;   // ne devrait pas arriver
                for (int i = 0; i < block; ++i)
                    for (int c = 0; c < outChannels; ++c)
                        outputFifo[(outCount + (size_t) i) * nOut + (size_t) c] = outBlock[(size_t) c * (size_t) block + (size_t) i];
                outCount += (size_t) block;
            }

            BYTE* data = nullptr;
            if (FAILED (render->GetBuffer (frames, &data))) { running = false; break; }
            const int bytes = output.format.bytesPerSample();
            for (UINT32 i = 0; i < frames; ++i)
                for (int c = 0; c < outChannels; ++c)
                    output.format.write (data + ((size_t) i * nOut + (size_t) c) * (size_t) bytes, outputFifo[(size_t) i * nOut + (size_t) c]);
            render->ReleaseBuffer (frames, 0);

            std::memmove (outputFifo.data(), outputFifo.data() + (size_t) frames * nOut, (outCount - frames) * nOut * sizeof (float));
            outCount -= frames;

            // Surveillance de la file d'entrée
            if (capture != nullptr)
            {
                warmup = warmup > frames ? warmup - frames : 0;
                minLeft = std::min (minLeft, inCount);
                windowFrames += frames;
                queueFrames.store ((int) inCount, std::memory_order_relaxed);
                if (windowFrames >= windowLength && warmup == 0)
                {
                    if (! underrunInWindow && minLeft != SIZE_MAX && minLeft > margin) dropInput (minLeft - margin);
                    windowFrames = 0;
                    windowLength = (size_t) sampleRate * 2;
                    minLeft = SIZE_MAX;
                    underrunInWindow = false;
                }
            }
        }

        if (task != nullptr) AvRevertMmThreadCharacteristics (task);
        CoUninitialize();
        running = false;
    }

    /** Jette les d échantillons les plus anciens de la file d'entrée, avec un court fondu (pas de clic). */
    void dropInput (size_t d)
    {
        if (d == 0 || d > inCount) return;
        const size_t fade = std::min<size_t> (64, inCount - d);
        for (size_t i = 0; i < fade; ++i)
        {
            const float t = (float) (i + 1) / (float) (fade + 1);
            for (size_t c = 0; c < 2; ++c)
                inputFifo[(d + i) * 2 + c] = inputFifo[i * 2 + c] * (1.0f - t) + inputFifo[(d + i) * 2 + c] * t;
        }
        std::memmove (inputFifo.data(), inputFifo.data() + d * 2, (inCount - d) * 2 * sizeof (float));
        inCount -= d;
    }

    void readCapture (size_t maxIn)
    {
        UINT32 packet = 0;
        while (SUCCEEDED (capture->GetNextPacketSize (&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED (capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr))) return;

            const int ch = input.format.channels, bytes = input.format.bytesPerSample();
            for (UINT32 i = 0; i < frames; ++i)
            {
                if (inCount * 2 + 2 > inputFifo.size()) break;
                for (int c = 0; c < 2; ++c)
                {
                    const int src = std::min (c, ch - 1);
                    inputFifo[inCount * 2 + (size_t) c] = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f
                        : input.format.read (data + ((size_t) i * (size_t) ch + (size_t) src) * (size_t) bytes);
                }
                ++inCount;
            }
            capture->ReleaseBuffer (frames);

            if (inCount > maxIn) dropInput (inCount - (size_t) block);   // l'entrée a pris beaucoup d'avance : on rattrape
        }
    }

    double inputQueueMs() const override { return sampleRate > 0 ? 1000.0 * queueFrames.load() / sampleRate : 0.0; }
    int glitchCount() const override { return glitches.load(); }

    IMMDeviceEnumerator* enumerator = nullptr;
    Endpoint input, output;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE renderEvent = nullptr, captureEvent = nullptr;
    UINT32 sharedPeriodFrames = 0;

    std::thread audioThread;
    std::atomic<bool> running { false };
    int block = 128, inChannels = 0, outChannels = 2, sampleRate = 48000;
    std::atomic<int> queueFrames { 0 }, glitches { 0 };
    std::vector<float> inputFifo, outputFifo;
    size_t inCount = 0, outCount = 0;
};

std::unique_ptr<AudioBackend> createAudioBackend() { return std::make_unique<WasapiBackend>(); }

} // namespace jsg
