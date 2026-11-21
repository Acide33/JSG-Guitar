// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_wav.h"
#include "../third_party/dr_mp3.h"
#include "../third_party/dr_flac.h"

#include "decode.h"

#include <cstring>

namespace jsg
{

bool decodeAudio (const void* data, size_t size, DecodedAudio& out, std::string& error)
{
    out = {};
    const auto* b = static_cast<const unsigned char*> (data);

    if (size < 12)
    {
        error = "Fichier vide ou trop court.";
        return false;
    }

    const bool isWav  = (std::memcmp (b, "RIFF", 4) == 0 || std::memcmp (b, "RF64", 4) == 0 || std::memcmp (b, "riff", 4) == 0);
    const bool isFlac = std::memcmp (b, "fLaC", 4) == 0;

    if (isWav)
    {
        unsigned int channels = 0, rate = 0;
        drwav_uint64 frames = 0;
        float* pcm = drwav_open_memory_and_read_pcm_frames_f32 (data, size, &channels, &rate, &frames, nullptr);
        if (pcm == nullptr) { error = "Ce fichier WAV n'a pas pu être lu."; return false; }
        out.channels = (int) channels;
        out.sampleRate = rate;
        out.interleaved.assign (pcm, pcm + frames * channels);
        drwav_free (pcm, nullptr);
        return frames > 0;
    }

    if (isFlac)
    {
        unsigned int channels = 0, rate = 0;
        drflac_uint64 frames = 0;
        float* pcm = drflac_open_memory_and_read_pcm_frames_f32 (data, size, &channels, &rate, &frames, nullptr);
        if (pcm == nullptr) { error = "Ce fichier FLAC n'a pas pu être lu."; return false; }
        out.channels = (int) channels;
        out.sampleRate = rate;
        out.interleaved.assign (pcm, pcm + frames * channels);
        drflac_free (pcm, nullptr);
        return frames > 0;
    }

    // Sinon : MP3 (avec ou sans étiquette ID3)
    drmp3_config config {};
    drmp3_uint64 frames = 0;
    float* pcm = drmp3_open_memory_and_read_pcm_frames_f32 (data, size, &config, &frames, nullptr);
    if (pcm == nullptr || frames == 0)
    {
        if (pcm != nullptr) drmp3_free (pcm, nullptr);
        error = "Format non reconnu : utilisez un fichier WAV, MP3 ou FLAC.";
        return false;
    }
    out.channels = (int) config.channels;
    out.sampleRate = config.sampleRate;
    out.interleaved.assign (pcm, pcm + frames * config.channels);
    drmp3_free (pcm, nullptr);
    return true;
}

std::unique_ptr<Song> makeSong (const DecodedAudio& audio, const std::string& title, int overviewPoints)
{
    auto song = std::make_unique<Song>();
    song->title = title;
    song->sampleRate = audio.sampleRate;
    const size_t frames = audio.frames();
    song->length = (int64_t) frames;
    song->left.resize (frames);
    song->right.resize (frames);

    const int ch = audio.channels;
    for (size_t i = 0; i < frames; ++i)
    {
        song->left[i] = audio.interleaved[i * (size_t) ch];
        song->right[i] = audio.interleaved[i * (size_t) ch + (ch > 1 ? 1 : 0)];
    }

    song->overview.assign ((size_t) overviewPoints, 0.0f);
    const size_t per = jmax ((size_t) 1, frames / (size_t) overviewPoints);
    for (int i = 0; i < overviewPoints; ++i)
    {
        float m = 0.0f;
        for (size_t j = (size_t) i * per; j < jmin (frames, (size_t) (i + 1) * per); j += 4)
            m = jmax (m, std::abs (song->left[j]), std::abs (song->right[j]));
        song->overview[(size_t) i] = m;
    }
    return song;
}

std::vector<float> firstChannel (const DecodedAudio& audio)
{
    std::vector<float> mono (audio.frames());
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = audio.interleaved[i * (size_t) audio.channels];
    return mono;
}

std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate)
{
    if (in.empty() || fromRate <= 0.0 || toRate <= 0.0 || std::abs (fromRate - toRate) < 0.5)
        return in;

    const double ratio = toRate / fromRate;
    const double cutoff = jmin (1.0, ratio) * 0.95;          // filtre anti-repliement en descente
    const int halfWidth = 32;                                // demi-largeur du filtre, en échantillons d'entrée
    const double beta = 8.0;                                 // fenêtre de Kaiser

    const auto bessel0 = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
        return sum;
    };
    const double norm = bessel0 (beta);

    const auto outLength = (size_t) std::ceil ((double) in.size() * ratio);
    std::vector<float> out (outLength, 0.0f);
    const double width = halfWidth / cutoff;

    for (size_t n = 0; n < outLength; ++n)
    {
        const double centre = (double) n / ratio;
        const auto first = (long long) std::ceil (centre - width), last = (long long) std::floor (centre + width);
        double sum = 0.0;

        for (long long k = std::max (0LL, first); k <= std::min ((long long) in.size() - 1, last); ++k)
        {
            const double x = (double) k - centre;
            const double t = x / width;
            const double w = bessel0 (beta * std::sqrt (jmax (0.0, 1.0 - t * t))) / norm;
            const double arg = pi * x * cutoff;
            const double sinc = std::abs (arg) < 1e-9 ? 1.0 : std::sin (arg) / arg;
            sum += (double) in[(size_t) k] * sinc * cutoff * w;
        }
        out[n] = (float) sum;
    }
    return out;
}

} // namespace jsg
