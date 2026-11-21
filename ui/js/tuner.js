// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    Accordeur : détection de hauteur par la méthode YIN — copie de Source/PitchDetector.h.
*/
export function detectPitch(samples, sampleRate, a4 = 440, lowestHz = 55) {
  const result = { valid: false, frequency: 0, midiNote: 0, cents: 0 };
  const step = Math.max(1, Math.floor(sampleRate / 22000));
  const fs = sampleRate / step;
  const n = Math.floor(samples.length / step);
  const work = new Float32Array(n);
  let energy = 0;

  for (let i = 0; i < n; i++) {
    let sum = 0;
    for (let k = 0; k < step; k++) sum += samples[i * step + k];
    work[i] = sum / step;
    energy += work[i] * work[i];
  }
  if (n < 256 || Math.sqrt(energy / n) < 0.003) return result;

  const tauMin = Math.max(2, Math.floor(fs / 1400));
  const tauMax = Math.min(Math.floor(n / 2), Math.floor(fs / lowestHz));
  const window = n - tauMax;
  if (tauMax <= tauMin + 2 || window < 64) return result;

  const diff = new Float32Array(tauMax + 1);
  for (let tau = 1; tau <= tauMax; tau++) {
    let sum = 0;
    for (let j = 0; j < window; j++) { const dd = work[j] - work[j + tau]; sum += dd * dd; }
    diff[tau] = sum;
  }

  let running = 0;
  diff[0] = 1;
  for (let tau = 1; tau <= tauMax; tau++) {
    running += diff[tau];
    diff[tau] = running > 0 ? diff[tau] * tau / running : 1;
  }

  let best = -1;
  for (let tau = tauMin; tau < tauMax; tau++) {
    if (diff[tau] < 0.15) {
      while (tau + 1 < tauMax && diff[tau + 1] < diff[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) return result;

  let refined = best;
  if (best > 1 && best < tauMax) {
    const a = diff[best - 1], b = diff[best], c = diff[best + 1];
    const denom = a - 2 * b + c;
    if (Math.abs(denom) > 1e-12) refined += 0.5 * (a - c) / denom;
  }

  result.frequency = fs / refined;
  const note = 69 + 12 * Math.log2(result.frequency / a4);
  result.midiNote = Math.round(note);
  result.cents = (note - result.midiNote) * 100;
  result.valid = true;
  return result;
}

const names = ['Do', 'Do#', 'Ré', 'Ré#', 'Mi', 'Fa', 'Fa#', 'Sol', 'Sol#', 'La', 'La#', 'Si'];
const letters = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export const noteName = (midi) => names[((midi % 12) + 12) % 12];
export const noteLetter = (midi) => letters[((midi % 12) + 12) % 12] + (Math.floor(midi / 12) - 1);
