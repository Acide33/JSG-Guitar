// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Carte son sur Mac : Core Audio (HAL), sans pilote ni bibliothèque à installer.

    Même rôle que wasapi.cpp sous Windows. Le moteur reçoit toujours des blocs de taille fixe
    (AudioInfo::blockSize) :
      - entrée et sortie sur le même appareil (carte son USB, cas habituel) : un seul « IOProc »
        reçoit l'entrée et remplit la sortie, sans file d'attente quand les tailles concordent ;
      - appareils différents (micro intégré + haut-parleurs du Mac, par exemple) : l'entrée passe
        par une file sans verrou, lue par la sortie.

    « Mode exclusif » : JSG Guitar règle la fréquence de la carte son (48 kHz par défaut).
    « Mode partagé » : il garde la fréquence choisie dans Configuration audio et MIDI.
    Dans les deux cas, la taille de tampon ne concerne que JSG Guitar (macOS la gère par logiciel).

    Accès au micro : macOS le demande une fois (Réglages Système > Confidentialité et sécurité > Micro).
*/
#ifndef JSG_FAKE_COREAUDIO          // essais sous Linux avec un Core Audio simulé (voir tests)
 #import <AVFoundation/AVFoundation.h>
#endif
#include <CoreAudio/CoreAudio.h>

#include "audio_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#if defined (__x86_64__) || defined (__i386__)
 #include <xmmintrin.h>
#endif

namespace jsg
{

namespace
{
    constexpr AudioObjectPropertyElement kMain = 0;   // kAudioObjectPropertyElementMain
    constexpr int kMaxChannels = 64;

    AudioObjectPropertyAddress address (AudioObjectPropertySelector selector, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        return { selector, scope, kMain };
    }

    template <typename T>
    bool getProperty (AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T& value)
    {
        const auto a = address (selector, scope);
        UInt32 size = sizeof (T);
        return AudioObjectGetPropertyData (object, &a, 0, nullptr, &size, &value) == noErr;
    }

    template <typename T>
    bool setProperty (AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, const T& value)
    {
        const auto a = address (selector, scope);
        return AudioObjectSetPropertyData (object, &a, 0, nullptr, sizeof (T), &value) == noErr;
    }

    std::string toString (CFStringRef s)
    {
        if (s == nullptr) return {};
        const CFIndex length = CFStringGetLength (s);
        const CFIndex max = CFStringGetMaximumSizeForEncoding (length, kCFStringEncodingUTF8) + 1;
        std::string out ((size_t) max, '\0');
        if (! CFStringGetCString (s, out.data(), max, kCFStringEncodingUTF8)) return {};
        out.resize (std::strlen (out.c_str()));
        return out;
    }

    std::string stringProperty (AudioObjectID object, AudioObjectPropertySelector selector)
    {
        CFStringRef s = nullptr;
        if (! getProperty (object, selector, kAudioObjectPropertyScopeGlobal, s) || s == nullptr) return {};
        const auto result = toString (s);
        CFRelease (s);
        return result;
    }

    std::vector<AudioDeviceID> allDevices()
    {
        const auto a = address (kAudioHardwarePropertyDevices);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
        std::vector<AudioDeviceID> ids (size / sizeof (AudioDeviceID));
        if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
        ids.resize (size / sizeof (AudioDeviceID));
        return ids;
    }

    int channelCount (AudioDeviceID device, AudioObjectPropertyScope scope)
    {
        const auto a = address (kAudioDevicePropertyStreamConfiguration, scope);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (device, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
        std::vector<uint8_t> storage (size);
        auto* list = reinterpret_cast<AudioBufferList*> (storage.data());
        if (AudioObjectGetPropertyData (device, &a, 0, nullptr, &size, list) != noErr) return 0;
        int channels = 0;
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += (int) list->mBuffers[i].mNumberChannels;
        return channels;
    }

    AudioDeviceID defaultDevice (bool input)
    {
        AudioDeviceID id = kAudioObjectUnknown;
        getProperty (kAudioObjectSystemObject, input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
                     kAudioObjectPropertyScopeGlobal, id);
        return id;
    }

    AudioDeviceID deviceFromUid (const std::string& uid, bool input)
    {
        if (uid.empty()) return defaultDevice (input);
        for (auto id : allDevices())
            if (stringProperty (id, kAudioDevicePropertyDeviceUID) == uid
                && channelCount (id, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput) > 0)
                return id;
        return kAudioObjectUnknown;
    }

    double nominalRate (AudioDeviceID device)
    {
        Float64 rate = 0.0;
        getProperty (device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
        return rate;
    }

    bool supportsRate (AudioDeviceID device, double rate)
    {
        const auto a = address (kAudioDevicePropertyAvailableNominalSampleRates);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (device, &a, 0, nullptr, &size) != noErr || size == 0) return false;
        std::vector<AudioValueRange> ranges (size / sizeof (AudioValueRange));
        if (AudioObjectGetPropertyData (device, &a, 0, nullptr, &size, ranges.data()) != noErr) return false;
        for (const auto& r : ranges)
            if (rate >= r.mMinimum - 0.5 && rate <= r.mMaximum + 0.5) return true;
        return false;
    }

    /** Change la fréquence de l'appareil et attend qu'elle soit prise en compte (asynchrone sous macOS). */
    bool setRate (AudioDeviceID device, double rate)
    {
        if (std::abs (nominalRate (device) - rate) < 0.5) return true;
        if (! supportsRate (device, rate)) return false;
        const Float64 value = rate;
        if (! setProperty (device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, value)) return false;
        for (int i = 0; i < 100; ++i)
        {
            if (std::abs (nominalRate (device) - rate) < 0.5) return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        return std::abs (nominalRate (device) - rate) < 0.5;
    }

    /** Règle la taille de tampon de JSG Guitar sur l'appareil (bornée par l'appareil). Renvoie la taille obtenue. */
    int setBufferFrames (AudioDeviceID device, int wanted)
    {
        AudioValueRange range { 0, 0 };
        if (getProperty (device, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, range) && range.mMaximum > 0)
            wanted = (int) std::clamp ((double) wanted, range.mMinimum, range.mMaximum);
        const UInt32 value = (UInt32) wanted;
        setProperty (device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, value);
        UInt32 actual = 0;
        getProperty (device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, actual);
        return actual > 0 ? (int) actual : wanted;
    }

    /** Latence annoncée par l'appareil dans un sens (échantillons) : matériel + marge de sécurité + flux. */
    int deviceLatency (AudioDeviceID device, bool input)
    {
        const auto scope = input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
        UInt32 latency = 0, safety = 0, stream = 0;
        getProperty (device, kAudioDevicePropertyLatency, scope, latency);
        getProperty (device, kAudioDevicePropertySafetyOffset, scope, safety);

        const auto a = address (kAudioDevicePropertyStreams, scope);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (device, &a, 0, nullptr, &size) == noErr && size >= sizeof (AudioStreamID))
        {
            std::vector<AudioStreamID> streams (size / sizeof (AudioStreamID));
            if (AudioObjectGetPropertyData (device, &a, 0, nullptr, &size, streams.data()) == noErr)
                getProperty (streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, stream);
        }
        return (int) (latency + safety + stream);
    }

    /** Les flux de l'appareil doivent être en flottant 32 bits (format virtuel de Core Audio, cas normal). */
    bool streamsAreFloat (AudioDeviceID device, bool input)
    {
        const auto a = address (kAudioDevicePropertyStreams, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (device, &a, 0, nullptr, &size) != noErr || size == 0) return true;
        std::vector<AudioStreamID> streams (size / sizeof (AudioStreamID));
        if (AudioObjectGetPropertyData (device, &a, 0, nullptr, &size, streams.data()) != noErr) return true;
        for (auto s : streams)
        {
            AudioStreamBasicDescription f {};
            if (! getProperty (s, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, f)) continue;
            if (f.mFormatID != kAudioFormatLinearPCM || (f.mFormatFlags & kAudioFormatFlagIsFloat) == 0 || f.mBitsPerChannel != 32) return false;
        }
        return true;
    }

    /** Petits nombres dénormalisés mis à zéro dans le fil audio (évite les pics de charge du processeur). */
    void flushDenormals()
    {
       #if defined (__aarch64__)
        uint64_t fpcr = 0;
        asm volatile ("mrs %0, fpcr" : "=r" (fpcr));
        asm volatile ("msr fpcr, %0" :: "r" (fpcr | (1ull << 24)));
       #elif defined (__x86_64__) || defined (__i386__)
        _mm_setcsr (_mm_getcsr() | 0x8040);
       #endif
    }

    /** Accès au micro : demandé une fois par macOS. Vrai si autorisé. */
    bool microphoneAllowed()
    {
       #ifdef JSG_FAKE_COREAUDIO
        return true;
       #else
        if (@available (macOS 10.14, *))
        {
            const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio];
            if (status == AVAuthorizationStatusAuthorized) return true;
            if (status != AVAuthorizationStatusNotDetermined) return false;

            // Première fois : macOS affiche sa demande ; on attend la réponse de l'utilisateur
            __block bool granted = false;
            dispatch_semaphore_t done = dispatch_semaphore_create (0);
            [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio completionHandler: ^(BOOL ok) {
                granted = ok;
                dispatch_semaphore_signal (done);
            }];
            dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, (int64_t) 300 * NSEC_PER_SEC));
            return granted;
        }
        return true;
       #endif
    }

    //==========================================================================
    /** File sans verrou, un écrivain (fil d'entrée) et un lecteur (fil de sortie). Échantillons entrelacés. */
    class InputQueue
    {
    public:
        void reset (int channelCount, int capacityFrames)
        {
            channels = std::max (1, channelCount);
            capacity = (size_t) capacityFrames;
            data.assign (capacity * (size_t) channels, 0.0f);
            writePos.store (0);
            readPos.store (0);
        }

        size_t available() const { return writePos.load (std::memory_order_acquire) - readPos.load (std::memory_order_acquire); }

        /** Fil d'entrée. Si la file est pleine, les échantillons les plus récents sont perdus. */
        void push (const AudioBufferList* list, int frames)
        {
            const size_t w = writePos.load (std::memory_order_relaxed);
            const size_t free = capacity - (w - readPos.load (std::memory_order_acquire));
            const size_t n = std::min ((size_t) frames, free);
            for (size_t i = 0; i < n; ++i)
            {
                float* frame = &data[((w + i) % capacity) * (size_t) channels];
                int c = 0;
                for (UInt32 b = 0; b < list->mNumberBuffers && c < channels; ++b)
                {
                    const auto& buffer = list->mBuffers[b];
                    const int bc = (int) buffer.mNumberChannels;
                    const auto* src = static_cast<const float*> (buffer.mData);
                    for (int k = 0; k < bc && c < channels; ++k, ++c)
                        frame[c] = src != nullptr ? src[i * (size_t) bc + (size_t) k] : 0.0f;
                }
                for (; c < channels; ++c) frame[c] = 0.0f;
            }
            writePos.store (w + n, std::memory_order_release);
        }

        /** Fil de sortie : lit `frames` échantillons vers des canaux séparés. */
        void pop (float* const* planar, int frames)
        {
            const size_t r = readPos.load (std::memory_order_relaxed);
            for (int i = 0; i < frames; ++i)
            {
                const float* frame = &data[((r + (size_t) i) % capacity) * (size_t) channels];
                for (int c = 0; c < channels; ++c) planar[c][i] = frame[c];
            }
            readPos.store (r + (size_t) frames, std::memory_order_release);
        }

        /** Fil de sortie : jette des échantillons (l'entrée a pris de l'avance). */
        void skip (size_t frames) { readPos.store (readPos.load (std::memory_order_relaxed) + frames, std::memory_order_release); }

    private:
        std::vector<float> data;
        size_t capacity = 0;
        int channels = 1;
        std::atomic<size_t> writePos { 0 }, readPos { 0 };
    };
}

//==============================================================================
class CoreAudioBackend final : public AudioBackend
{
public:
    ~CoreAudioBackend() override { stop(); }

    std::vector<AudioDevice> inputs() override  { return list (true); }
    std::vector<AudioDevice> outputs() override { return list (false); }

    bool start (const AudioConfig& config, Prepare prepare, Callback callback, AudioInfo& info, std::string& error) override
    {
        stop();
        info = {};

        outDevice = deviceFromUid (config.outputId, false);
        if (outDevice == kAudioObjectUnknown)
        {
            error = config.outputId.empty() ? "aucune sortie audio" : "sortie audio introuvable (débranchée ?)";
            return false;
        }

        // Entrée
        inDevice = kAudioObjectUnknown;
        if (config.inputId != "none")
        {
            inDevice = deviceFromUid (config.inputId, true);
            if (inDevice == kAudioObjectUnknown && ! config.inputId.empty())
                info.note = "Entrée introuvable (débranchée ?) : pas d'entrée.";
            else if (inDevice != kAudioObjectUnknown && ! microphoneAllowed())
            {
                inDevice = kAudioObjectUnknown;
                info.note = "Accès à l'entrée refusé par macOS : Réglages Système > Confidentialité et sécurité > Micro > "
                            "activez JSG Guitar, puis relancez le programme.";
            }
        }

        // Fréquence : imposée en mode « exclusif », sinon celle de la sortie
        double rate = nominalRate (outDevice);
        if (config.exclusive && config.sampleRate > 0)
        {
            if (! setRate (outDevice, config.sampleRate))
            {
                error = std::to_string (config.sampleRate) + " Hz refusé par la sortie";
                return false;
            }
            rate = config.sampleRate;
        }
        if (rate <= 0) { error = "fréquence de la sortie inconnue"; return false; }

        sameDevice = inDevice == outDevice;
        if (inDevice != kAudioObjectUnknown && ! sameDevice && ! setRate (inDevice, rate))
        {
            info.note = "L'entrée et la sortie n'ont pas la même fréquence (" + std::to_string ((int) nominalRate (inDevice)) + " / "
                      + std::to_string ((int) rate) + " Hz) : pas d'entrée. Choisissez la même fréquence dans Configuration audio et MIDI.";
            inDevice = kAudioObjectUnknown;
        }

        if (! streamsAreFloat (outDevice, false) || (inDevice != kAudioObjectUnknown && ! streamsAreFloat (inDevice, true)))
        {
            error = "format de la carte son non pris en charge (32 bits flottant attendu)";
            return false;
        }

        // Taille de bloc
        block = std::clamp (config.bufferFrames, 16, 4096);
        const int outFrames = setBufferFrames (outDevice, block);
        int inFrames = outFrames;
        if (inDevice != kAudioObjectUnknown && ! sameDevice) inFrames = setBufferFrames (inDevice, block);
        block = outFrames;   // le moteur travaille au rythme de la sortie
        sampleRate = (int) std::lround (rate);

        inChannels  = inDevice != kAudioObjectUnknown ? std::min (kMaxChannels, channelCount (inDevice, kAudioObjectPropertyScopeInput)) : 0;
        outChannels = std::min (kMaxChannels, channelCount (outDevice, kAudioObjectPropertyScopeOutput));
        if (outChannels <= 0) { error = "la sortie n'a aucun canal"; return false; }

        // Tampons (alloués ici, jamais dans le fil audio)
        const int maxFrames = std::max ({ block, outFrames, inFrames, 4096 });
        allocate (inPlanar, inPointers, std::max (1, inChannels), block);
        allocate (outPlanar, outPointers, outChannels, block);
        inAccum.assign ((size_t) std::max (1, inChannels) * (size_t) (maxFrames + block) * 2, 0.0f);
        outAccum.assign ((size_t) outChannels * (size_t) (maxFrames + block) * 2, 0.0f);
        accumCapacity = (maxFrames + block) * 2;
        inFill = outFill = 0;
        primed = false;
        queuePrimed = false;
        glitches = 0;
        queue.reset (inChannels, std::max (16384, maxFrames * 8));
        queueTarget = (size_t) (2 * inFrames + block);   // réserve : deux tampons d'entrée + un bloc

        process = std::move (callback);
        prepare (sampleRate, block);

        // Démarrage
        if (AudioDeviceCreateIOProcID (outDevice, &CoreAudioBackend::outputProc, this, &outProc) != noErr)
        {
            error = "impossible d'ouvrir la sortie";
            return false;
        }
        if (inDevice != kAudioObjectUnknown && ! sameDevice
            && AudioDeviceCreateIOProcID (inDevice, &CoreAudioBackend::inputProc, this, &inProc) != noErr)
        {
            inProc = nullptr;
            inDevice = kAudioObjectUnknown;
            inChannels = 0;
            info.note = "Impossible d'ouvrir l'entrée : pas d'entrée.";
        }

        running = true;
        if (inProc != nullptr) AudioDeviceStart (inDevice, inProc);
        if (AudioDeviceStart (outDevice, outProc) != noErr)
        {
            stop();
            error = "la sortie refuse de démarrer";
            return false;
        }

        info.running = true;
        info.exclusive = config.exclusive;
        info.sampleRate = sampleRate;
        info.blockSize = block;
        info.inputChannels = inChannels;
        info.outputChannels = outChannels;
        info.outputLatencyMs = 1000.0 * (outFrames + deviceLatency (outDevice, false)) / rate;
        info.inputLatencyMs = inDevice != kAudioObjectUnknown ? 1000.0 * (inFrames + deviceLatency (inDevice, true) + (sameDevice ? 0 : (int) queueTarget)) / rate : 0.0;
        info.inputName = inDevice != kAudioObjectUnknown ? stringProperty (inDevice, kAudioObjectPropertyName) : "Aucune";
        info.outputName = stringProperty (outDevice, kAudioObjectPropertyName);
        info.format = std::string ("32 bits flottant, Core Audio") + (sameDevice || inDevice == kAudioObjectUnknown ? "" : ", entrée et sortie séparées");
        return true;
    }

    void stop() override
    {
        if (outProc != nullptr)
        {
            AudioDeviceStop (outDevice, outProc);
            AudioDeviceDestroyIOProcID (outDevice, outProc);
            outProc = nullptr;
        }
        if (inProc != nullptr)
        {
            AudioDeviceStop (inDevice, inProc);
            AudioDeviceDestroyIOProcID (inDevice, inProc);
            inProc = nullptr;
        }
        running = false;
    }

    bool isRunning() const override { return running; }
    double inputQueueMs() const override { return sampleRate > 0 && ! sameDevice ? 1000.0 * (double) queue.available() / sampleRate : 0.0; }
    int glitchCount() const override { return glitches.load(); }

private:
    static std::vector<AudioDevice> list (bool input)
    {
        std::vector<AudioDevice> result;
        const auto def = defaultDevice (input);
        for (auto id : allDevices())
        {
            if (channelCount (id, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput) <= 0) continue;
            AudioDevice d;
            d.id = stringProperty (id, kAudioDevicePropertyDeviceUID);
            d.name = stringProperty (id, kAudioObjectPropertyName);
            d.isDefault = id == def;
            if (! d.id.empty()) result.push_back (d);
        }
        return result;
    }

    static void allocate (std::vector<std::vector<float>>& storage, std::vector<float*>& pointers, int channels, int frames)
    {
        storage.assign ((size_t) channels, std::vector<float> ((size_t) frames, 0.0f));
        pointers.resize ((size_t) channels);
        for (int c = 0; c < channels; ++c) pointers[(size_t) c] = storage[(size_t) c].data();
    }

    static OSStatus inputProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
                               AudioBufferList*, const AudioTimeStamp*, void* self)
    {
        auto& b = *static_cast<CoreAudioBackend*> (self);
        if (input != nullptr && input->mNumberBuffers > 0)
        {
            const auto& first = input->mBuffers[0];
            const int frames = first.mNumberChannels > 0 ? (int) (first.mDataByteSize / (sizeof (float) * first.mNumberChannels)) : 0;
            if (frames > 0) b.queue.push (input, frames);
        }
        return noErr;
    }

    static OSStatus outputProc (AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
                                AudioBufferList* output, const AudioTimeStamp*, void* self)
    {
        static thread_local bool flushed = false;
        if (! flushed) { flushDenormals(); flushed = true; }
        static_cast<CoreAudioBackend*> (self)->render (input, output);
        return noErr;
    }

    static int framesIn (const AudioBufferList* list)
    {
        if (list == nullptr || list->mNumberBuffers == 0 || list->mBuffers[0].mNumberChannels == 0) return 0;
        return (int) (list->mBuffers[0].mDataByteSize / (sizeof (float) * list->mBuffers[0].mNumberChannels));
    }

    /** Fil de sortie : produit `frames` échantillons, par blocs de taille fixe pour le moteur. */
    void render (const AudioBufferList* input, AudioBufferList* output)
    {
        const int frames = framesIn (output);
        if (frames <= 0) return;
        const bool directInput = sameDevice && inChannels > 0;
        const int inputFrames = directInput ? std::min (framesIn (input), frames) : 0;

        // Cas courant : la carte son demande exactement un bloc, rien n'est en attente
        if (frames == block && inFill == 0 && outFill == 0 && (! directInput || inputFrames == block))
        {
            if (directInput) deinterleave (input, 0, inPointers.data(), inChannels, block);
            else             fetchQueuedInput (inPointers.data());
            process (inPointers.data(), inChannels, outPointers.data(), outChannels, block);
            interleave (outPointers.data(), output, 0, block);
            return;
        }

        // Tailles différentes : accumulation de l'entrée et de la sortie, un bloc de retard
        if (directInput && inputFrames > 0 && inFill + inputFrames <= accumCapacity)
        {
            deinterleaveInto (input, inAccum, inFill, inputFrames);
            inFill += inputFrames;
        }
        if (! primed) { outFill = std::min (block, accumCapacity); clearAccum (outAccum, 0, outFill); primed = true; }

        while (outFill < frames && outFill + block <= accumCapacity)
        {
            if (directInput)
            {
                if (inFill < block) { glitches.fetch_add (1); clearAccumIn (inFill, block - inFill); inFill = block; }
                for (int c = 0; c < inChannels; ++c)
                    std::memcpy (inPointers[(size_t) c], &inAccum[(size_t) c * (size_t) accumCapacity], sizeof (float) * (size_t) block);
                shift (inAccum, inChannels, inFill, block);
                inFill -= block;
            }
            else fetchQueuedInput (inPointers.data());

            process (inPointers.data(), inChannels, outPointers.data(), outChannels, block);
            for (int c = 0; c < outChannels; ++c)
                std::memcpy (&outAccum[(size_t) c * (size_t) accumCapacity + (size_t) outFill], outPointers[(size_t) c], sizeof (float) * (size_t) block);
            outFill += block;
        }

        const int n = std::min (frames, outFill);
        float* rows[kMaxChannels];
        for (int c = 0; c < outChannels; ++c) rows[c] = &outAccum[(size_t) c * (size_t) accumCapacity];
        interleave (rows, output, 0, n);
        if (n < frames) clearOutput (output, n, frames - n);
        shift (outAccum, outChannels, outFill, n);
        outFill -= n;
    }

    /** Entrée d'un autre appareil : un bloc pris dans la file (silence si elle n'a pas suivi).
        La lecture ne commence qu'avec une réserve (deux tampons d'entrée + un bloc), pour absorber
        le décalage entre les deux appareils. */
    void fetchQueuedInput (float* const* planar)
    {
        if (inChannels <= 0) return;
        const size_t available = queue.available();
        if (! queuePrimed && available >= queueTarget) queuePrimed = true;
        if (queuePrimed)
        {
            if (available > queueTarget + (size_t) block * 4) queue.skip (available - queueTarget);   // trop d'avance : on rattrape
            if (queue.available() >= (size_t) block) { queue.pop (planar, block); return; }
            glitches.fetch_add (1);   // l'entrée n'a pas suivi : silence, puis nouvelle réserve
            queuePrimed = false;
        }
        for (int c = 0; c < inChannels; ++c) std::fill (planar[c], planar[c] + block, 0.0f);
    }

    void deinterleave (const AudioBufferList* list, int offset, float* const* planar, int channels, int frames) const
    {
        int c = 0;
        for (UInt32 b = 0; b < list->mNumberBuffers && c < channels; ++b)
        {
            const auto& buffer = list->mBuffers[b];
            const int bc = (int) buffer.mNumberChannels;
            const auto* src = static_cast<const float*> (buffer.mData);
            for (int k = 0; k < bc && c < channels; ++k, ++c)
                for (int i = 0; i < frames; ++i)
                    planar[c][i] = src != nullptr ? src[(size_t) (offset + i) * (size_t) bc + (size_t) k] : 0.0f;
        }
        for (; c < channels; ++c) std::fill (planar[c], planar[c] + frames, 0.0f);
    }

    void deinterleaveInto (const AudioBufferList* list, std::vector<float>& accum, int at, int frames) const
    {
        float* rows[kMaxChannels];
        for (int c = 0; c < inChannels; ++c) rows[c] = &accum[(size_t) c * (size_t) accumCapacity + (size_t) at];
        deinterleave (list, 0, rows, inChannels, frames);
    }

    void interleave (float* const* planar, AudioBufferList* list, int offset, int frames) const
    {
        int c = 0;
        for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
        {
            auto& buffer = list->mBuffers[b];
            const int bc = (int) buffer.mNumberChannels;
            auto* dst = static_cast<float*> (buffer.mData);
            if (dst == nullptr) { c += bc; continue; }
            for (int k = 0; k < bc; ++k, ++c)
                for (int i = 0; i < frames; ++i)
                    dst[(size_t) (offset + i) * (size_t) bc + (size_t) k] = c < outChannels ? planar[c][i] : 0.0f;
        }
    }

    void clearOutput (AudioBufferList* list, int offset, int frames) const
    {
        for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
        {
            auto& buffer = list->mBuffers[b];
            if (buffer.mData == nullptr) continue;
            const size_t bc = buffer.mNumberChannels;
            std::fill_n (static_cast<float*> (buffer.mData) + (size_t) offset * bc, (size_t) frames * bc, 0.0f);
        }
    }

    void clearAccum (std::vector<float>& accum, int at, int frames) const
    {
        for (int c = 0; c < outChannels; ++c)
            std::fill_n (&accum[(size_t) c * (size_t) accumCapacity + (size_t) at], (size_t) frames, 0.0f);
    }

    void clearAccumIn (int at, int frames)
    {
        for (int c = 0; c < inChannels; ++c)
            std::fill_n (&inAccum[(size_t) c * (size_t) accumCapacity + (size_t) at], (size_t) frames, 0.0f);
    }

    /** Retire les `n` premiers échantillons de chaque canal d'un accumulateur rempli sur `fill`. */
    void shift (std::vector<float>& accum, int channels, int fill, int n) const
    {
        if (n <= 0) return;
        for (int c = 0; c < channels; ++c)
        {
            float* row = &accum[(size_t) c * (size_t) accumCapacity];
            if (fill > n) std::memmove (row, row + n, sizeof (float) * (size_t) (fill - n));
        }
    }

    AudioDeviceID inDevice = kAudioObjectUnknown, outDevice = kAudioObjectUnknown;
    AudioDeviceIOProcID inProc = nullptr, outProc = nullptr;
    bool sameDevice = true, primed = false, queuePrimed = false;
    int block = 128, sampleRate = 0, inChannels = 0, outChannels = 2;
    Callback process;
    std::vector<std::vector<float>> inPlanar, outPlanar;
    std::vector<float*> inPointers, outPointers;
    std::vector<float> inAccum, outAccum;
    int accumCapacity = 0, inFill = 0, outFill = 0;
    InputQueue queue;
    size_t queueTarget = 0;
    std::atomic<bool> running { false };
    std::atomic<int> glitches { 0 };
};

std::unique_ptr<AudioBackend> createAudioBackend() { return std::make_unique<CoreAudioBackend>(); }

} // namespace jsg
