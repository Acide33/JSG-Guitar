// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — version navigateur : réglages, presets d'usine, pédales et modèles.
    Copie fidèle de Source/PluginProcessor.cpp, Source/Pedals.h et Source/AmpEngine.h.
    L'ordre des réglages est celui du moteur (engine/api.cpp) : ne pas le modifier.
*/

// kind : toggle | choice | knob (0..10) | ranged ; centre : valeur à mi-course (échelle non linéaire)
const d = (id, name, kind, min, max, def, extra = {}) => ({ id, name, kind, min, max, def, centre: 0, unit: '', inPresets: true, ...extra });
const toggle = (id, name, def, extra) => d(id, name, 'toggle', 0, 1, def ? 1 : 0, extra);
const choice = (id, name, choices, def, extra) => d(id, name, 'choice', 0, choices.length - 1, def, { choices, ...extra });
const knob = (id, name, def) => d(id, name, 'knob', 0, 10, def);
const ranged = (id, name, min, max, def, unit, centre = 0, extra) => d(id, name, 'ranged', min, max, def, { unit, centre, ...extra });

export const cabinets = ['JSG 4x12 Modern', 'JSG 4x12 Vintage', 'JSG 2x12 Alnico', 'JSG 1x12 Open Back', 'JSG 4x10 Blues'];
export const bassCabinets = ['JSG 8x10', 'JSG 4x10 Basse', 'JSG 4x10 + tweeter', 'JSG 1x15', 'JSG 2x12 Basse'];
export const mics = ['Dynamique', 'Ruban', 'Dyn. + Ruban', 'Dyn. large', 'Statique'];
export const rooms = ['Studio A', 'Studio B', 'Live Room', 'Grande salle', 'Cave'];

export const params = [
  toggle('power', 'Power', true, { inPresets: false }),
  toggle('mute', 'Muet', false, { inPresets: false }),

  choice('channel', 'Canal', ['Clair', 'Crunch', 'Lead'], 2),
  toggle('bright', 'Bright', false),
  ranged('input', 'Entrée', -24, 24, 0, 'dB'),
  knob('gain', 'Gain', 5),
  knob('bass', 'Bass', 5),
  knob('mid', 'Middle', 5),
  knob('treble', 'Treble', 5),
  knob('presence', 'Presence', 5),
  knob('master', 'Master', 6),

  toggle('cabOn', 'Baffle', true),
  choice('cabType', 'Baffle', cabinets, 0),
  toggle('micOn', 'Micro', true),
  choice('micType', 'Micro', mics, 2),
  toggle('roomOn', 'Room', true),
  choice('roomType', 'Room', rooms, 0),
  knob('roomLevel', 'Niveau de la room', 5),
  ranged('lowCut', 'Coupe-bas', 20, 300, 60, 'Hz', 100),
  ranged('highCut', 'Coupe-haut', 2000, 20000, 12000, 'Hz', 7000),
  toggle('irOn', 'IR Loader', false),

  toggle('eqOn', 'EQ', false),
  ranged('eq1', 'EQ 100 Hz', -12, 12, 0, 'dB'),
  ranged('eq2', 'EQ 250 Hz', -12, 12, 0, 'dB'),
  ranged('eq3', 'EQ 800 Hz', -12, 12, 0, 'dB'),
  ranged('eq4', 'EQ 2,5 kHz', -12, 12, 0, 'dB'),
  ranged('eq5', 'EQ 6 kHz', -12, 12, 0, 'dB'),

  ranged('output', 'Sortie', -24, 12, 0, 'dB', 0, { inPresets: false }),

  toggle('gateOn', 'Noise gate', false),
  ranged('gateThreshold', 'Seuil du noise gate', -80, -20, -55, 'dB'),
  ranged('gateDecay', 'Fermeture du noise gate', 10, 1000, 80, 'ms', 100),
  toggle('compOn', 'Compresseur', false),
  knob('compLevel', 'Niveau du compresseur', 5),
  ranged('compAttack', 'Attaque du compresseur', 1, 100, 10, 'ms', 10),
  toggle('driveOn', 'Drive', false),
  choice('driveType', 'Type de drive', ['MD', 'OD', 'DIST', 'FUZZ'], 0),
  knob('drive', 'Drive', 4),
  toggle('fxOn', 'Effets', false),
  toggle('fxChorus', 'Chorus', false),
  toggle('fxDelay', 'Delay', false),
  toggle('fxReverb', 'Reverb', true),
  knob('fxMix', 'Mix du chorus', 5),
  ranged('fxTime', 'Temps du delay', 40, 1200, 380, 'ms', 300),
  knob('fxFeedback', 'Répétitions du delay', 3.5),
  knob('fxLevel', 'Niveau delay et reverb', 3),

  choice('instrument', 'Instrument', ['Guitare', 'Basse'], 0),
  choice('bassCab', 'Baffle basse', bassCabinets, 0),
  knob('diBlend', 'DI', 4),
  choice('bassStrings', 'Cordes', ['4 cordes', '5 cordes', '6 cordes'], 0, { inPresets: false }),
];

export const paramIndex = Object.fromEntries(params.map((p, i) => [p.id, i]));
export const numFixedParams = params.length;   // 49
export const numSlots = 16, slotsPerRow = 8, maxPedalKnobs = 4;
export const slotParamIndex = (slot, offset) => numFixedParams + slot * (1 + maxPedalKnobs) + offset;

//------------------------------------------------------------------------------
// Échelles : position 0..1 <-> valeur (échelle « skew » : la valeur centre tombe à mi-course)
export function stepOf(p) {
  if (p.kind === 'knob') return 0.1;
  if (p.kind === 'ranged') { const span = p.max - p.min; return span > 500 ? 1 : span > 20 ? 0.1 : 0.01; }
  return 1;
}

function skewOf(p) {
  if (p.centre > p.min && p.centre < p.max) return Math.log(0.5) / Math.log((p.centre - p.min) / (p.max - p.min));
  return 1;
}

export function toPosition(p, v) {
  const x = Math.min(1, Math.max(0, (v - p.min) / (p.max - p.min)));
  const s = skewOf(p);
  return s === 1 ? x : Math.pow(x, s);
}

export function fromPosition(p, pos) {
  pos = Math.min(1, Math.max(0, pos));
  const s = skewOf(p);
  let v = p.min + (p.max - p.min) * (s === 1 ? pos : Math.exp(Math.log(pos) / s));
  const step = stepOf(p);
  v = Math.round(v / step) * step;
  return Math.min(p.max, Math.max(p.min, +v.toFixed(4)));
}

const num = (v, decimals) => v.toFixed(decimals).replace('.', ',');

export function formatValue(p, v) {
  if (p.kind === 'toggle') return v > 0.5 ? 'ON' : 'OFF';
  if (p.kind === 'choice') return p.choices[Math.round(v)] ?? '';
  if (p.kind === 'knob') return num(v, 1);
  const decimals = p.unit === 'Hz' || p.unit === 'ms' ? 0 : 1;
  return num(v, decimals) + (p.unit ? ' ' + p.unit : '');
}

//------------------------------------------------------------------------------
// Pédales intégrées : même ordre que le catalogue du moteur (engine/pedals.h)
const k10 = (name, def) => ({ name, min: 0, max: 10, def, unit: '', log: false });

export const pedals = [
  { id: 'gate', name: 'NOISE GATE', category: 'Dynamique', colour: '#59636f', afterAmp: false,
    description: "Coupe le souffle entre les notes. THRESH : seuil d'ouverture ; DECAY : temps de fermeture.",
    knobs: [{ name: 'THRESH', min: -80, max: -20, def: -55, unit: 'dB', log: false }, { name: 'DECAY', min: 10, max: 1000, def: 80, unit: 'ms', log: true }] },
  { id: 'compressor', name: 'COMPRESSOR', category: 'Dynamique', colour: '#2c6db3', afterAmp: false,
    description: 'Égalise le volume des notes et allonge le sustain.',
    knobs: [k10('SUST', 5), { name: 'ATTACK', min: 1, max: 100, def: 10, unit: 'ms', log: true }, k10('LEVEL', 5)] },
  { id: 'screamer', name: 'MID DRIVE', category: 'Saturation', colour: '#3c9a4a', afterAmp: false,
    description: 'Overdrive à bosse de médiums : resserre un ampli saturé, fait chanter les solos.',
    knobs: [k10('DRIVE', 3), k10('TONE', 5), k10('LEVEL', 5)] },
  { id: 'overdrive', name: 'OVERDRIVE', category: 'Saturation', colour: '#d6a21f', afterAmp: false,
    description: 'Saturation douce et chaude, façon lampe poussée.',
    knobs: [k10('DRIVE', 4), k10('TONE', 5), k10('LEVEL', 5)] },
  { id: 'distortion', name: 'DISTORTION', category: 'Saturation', colour: '#d65f1d', afterAmp: false,
    description: 'Distorsion franche, écrêtage dur.',
    knobs: [k10('DRIVE', 5), k10('TONE', 5), k10('LEVEL', 5)] },
  { id: 'fuzz', name: 'FUZZ', category: 'Saturation', colour: '#b32e2b', afterAmp: false,
    description: 'Fuzz épais et asymétrique, façon années 60.',
    knobs: [k10('FUZZ', 6), k10('TONE', 5), k10('LEVEL', 4)] },
  { id: 'chorus', name: 'CHORUS', category: 'Modulation', colour: '#27a3b6', afterAmp: true,
    description: 'Épaissit et élargit le son en stéréo.',
    knobs: [{ name: 'RATE', min: 0.1, max: 5, def: 0.8, unit: 'Hz', log: true }, k10('DEPTH', 5), k10('MIX', 5)] },
  { id: 'tremolo', name: 'TREMOLO', category: 'Modulation', colour: '#8a8f3a', afterAmp: true,
    description: 'Fait onduler le volume. RATE : vitesse ; DEPTH : profondeur.',
    knobs: [{ name: 'RATE', min: 1, max: 12, def: 5, unit: 'Hz', log: true }, k10('DEPTH', 6)] },
  { id: 'delay', name: 'DELAY', category: 'Ambiance', colour: '#7a4eb6', afterAmp: true,
    description: "Écho dont les répétitions s'assombrissent. TIME : écart ; FEEDB : nombre de répétitions.",
    knobs: [{ name: 'TIME', min: 40, max: 1200, def: 380, unit: 'ms', log: true }, k10('FEEDB', 3.5), k10('LEVEL', 3.5)] },
  { id: 'reverb', name: 'REVERB', category: 'Ambiance', colour: '#3b5cc2', afterAmp: true,
    description: 'Réverbération. MIX : quantité ; SIZE : taille de la pièce ; TONE : brillance.',
    knobs: [k10('MIX', 2.5), k10('SIZE', 5), k10('TONE', 5)] },
];

export const pedalIndex = Object.fromEntries(pedals.map((p, i) => [p.id, i]));

export const knobToReal = (k, pos) => {
  pos = Math.min(1, Math.max(0, pos));
  return k.log ? k.min * Math.pow(k.max / k.min, pos) : k.min + (k.max - k.min) * pos;
};
export const knobToPosition = (k, v) => {
  v = Math.min(Math.max(k.min, k.max), Math.max(Math.min(k.min, k.max), v));
  return k.log ? Math.log(v / k.min) / Math.log(k.max / k.min) : (v - k.min) / (k.max - k.min);
};
export const knobFormat = (k, pos) => {
  const v = knobToReal(k, pos);
  const decimals = k.unit === 'ms' ? 0 : k.unit === 'Hz' ? 2 : 1;
  return num(v, decimals) + (k.unit ? ' ' + k.unit : '');
};

//------------------------------------------------------------------------------
// Presets d'usine (seuls les réglages différents des valeurs par défaut sont listés)
// pédales : [emplacement, identifiant, valeurs réelles des potards]
export const factoryPresets = [
  { name: '01 - Modern Lead',
    settings: { channel: 2, gain: 6.5, bass: 5.5, mid: 4.5, treble: 6, presence: 6 },
    pedals: [[0, 'gate', [-55, 80]], [1, 'screamer', [3, 5, 6]], [8, 'delay', [380, 3, 2.5]], [9, 'reverb', [2, 5, 5]]] },
  { name: '02 - Clean Chorus',
    settings: { channel: 0, gain: 4.5, treble: 6, presence: 5.5, bright: 1, master: 7.5, cabType: 2, micType: 4 },
    pedals: [[0, 'compressor', [5, 10, 5.5]], [8, 'chorus', [0.8, 5, 5]], [9, 'reverb', [3, 5, 5]]] },
  { name: '03 - Crunch Rhythm',
    settings: { channel: 1, gain: 5.5, mid: 6, treble: 5.5, master: 8.5, cabType: 1, micType: 0 },
    pedals: [[0, 'gate', [-60, 80]], [8, 'reverb', [1.5, 5, 5]]] },
  { name: '04 - Blues Breakup',
    settings: { channel: 0, gain: 8.5, bass: 4.5, mid: 6.5, treble: 5.5, master: 6.5, cabType: 4, micType: 1, roomType: 2 },
    pedals: [[0, 'overdrive', [3.5, 5, 5]], [8, 'reverb', [2, 5, 5]]] },
  { name: '05 - Metal Tight',
    settings: { channel: 2, gain: 8, bass: 6, mid: 3.5, treble: 6.5, presence: 6.5, cabType: 0, micType: 0, roomOn: 0, lowCut: 90 },
    pedals: [[0, 'gate', [-48, 40]], [1, 'screamer', [1.5, 5, 7]]] },
  { name: '06 - Ambient Clean',
    settings: { channel: 0, gain: 4, master: 8, cabType: 2, micType: 4, roomType: 3 },
    pedals: [[0, 'compressor', [5, 10, 5]], [8, 'chorus', [0.5, 6, 5]], [9, 'delay', [520, 5, 4.5]], [10, 'reverb', [6, 8, 5]]] },
  { name: '07 - Classic Rock',
    settings: { channel: 1, gain: 7, bass: 5.5, mid: 6.5, treble: 6, presence: 5.5, master: 7, cabType: 1, micType: 2, roomType: 1 },
    pedals: [[8, 'reverb', [1.5, 5, 5]]] },
  { name: '08 - Fuzz Garage',
    settings: { channel: 0, gain: 5, bass: 5.5, mid: 5.5, treble: 4.5, cabType: 4, micType: 1, roomType: 4 },
    pedals: [[0, 'fuzz', [6.5, 4.5, 4]], [8, 'reverb', [2, 5, 5]]] },
  { name: '09 - Funk Compressé',
    settings: { channel: 0, gain: 4, bright: 1, bass: 4.5, mid: 4.5, treble: 6.5, master: 9, cabType: 3, micType: 0 },
    pedals: [[0, 'compressor', [7, 5, 7]], [8, 'reverb', [1, 5, 5]]] },
  { name: '10 - Solo Delay',
    settings: { channel: 2, gain: 7.5, mid: 6.5, treble: 5.5, eqOn: 1, eq3: 2, eq4: 1.5 },
    pedals: [[0, 'gate', [-55, 80]], [1, 'screamer', [4, 5, 5]], [8, 'delay', [430, 4, 4]], [9, 'reverb', [3.5, 6.5, 5]]] },
  { name: '11 - Basse Clean DI',
    settings: { instrument: 1, channel: 0, gain: 5, bass: 5.5, mid: 5, treble: 5, bassCab: 1, micType: 3, diBlend: 6, lowCut: 25, roomOn: 0,
                compOn: 1, compLevel: 4, compAttack: 20 }, pedals: [] },
  { name: '12 - Basse Rock',
    settings: { instrument: 1, channel: 1, gain: 5.5, bass: 6, mid: 6, treble: 5.5, master: 9, bassCab: 0, micType: 0, diBlend: 4, lowCut: 25, roomOn: 0 },
    pedals: [] },
  { name: '13 - Basse Ronde Vintage',
    settings: { instrument: 1, channel: 0, gain: 6, bass: 6.5, mid: 5.5, treble: 3.5, presence: 4, master: 8, bassCab: 3, micType: 1, diBlend: 3,
                lowCut: 25, highCut: 6000, roomType: 0, roomLevel: 2 }, pedals: [] },
  { name: '14 - Basse Slap Funk',
    settings: { instrument: 1, channel: 0, gain: 4.5, bass: 6.5, mid: 3.5, treble: 7, presence: 6.5, bright: 1, bassCab: 2, micType: 4, diBlend: 5,
                lowCut: 25, roomOn: 0, compOn: 1, compLevel: 4.5, compAttack: 3 }, pedals: [] },
  { name: '15 - Basse Saturée',
    settings: { instrument: 1, channel: 2, gain: 6.5, bass: 5.5, mid: 6.5, treble: 6, master: 7.5, bassCab: 0, micType: 0, diBlend: 5, lowCut: 25,
                roomOn: 0, gateOn: 1, gateThreshold: -60 }, pedals: [] },
];

export const isBassPreset = (preset) => (preset.settings?.instrument ?? 0) > 0.5;

// Morceaux du groupe : pack séparé « JSG Playbacks » (CC BY-NC 4.0), à copier dans ui/playbacks/
export const playbacks = [
  { title: 'JSG - Abrut shift (sans guitare)', file: 'playbacks/JSG - Abrut shift (sans guitare).mp3' },
  { title: 'JSG - AT (sans guitare)', file: 'playbacks/JSG - AT (sans guitare).mp3' },
  { title: 'JSG - Dissapoinement (sans guitare)', file: 'playbacks/JSG - Dissapoinement (sans guitare).mp3' },
  { title: 'JSG - Double tape (sans guitare)', file: 'playbacks/JSG - Double tape (sans guitare).mp3' },
  { title: 'JSG - Rock F (sans guitare)', file: 'playbacks/JSG - Rock F (sans guitare).mp3' },
  { title: 'JSG - JS Confinement (sans guitare)', file: 'playbacks/JSG - JS Confinement (sans guitare).mp3' },
  { title: 'JSG - Soso (sans guitare)', file: 'playbacks/JSG - Soso (sans guitare).mp3' },
  { title: 'JSG - Karamel (sans guitare)', file: 'playbacks/JSG - Karamel (sans guitare).mp3' },
];
