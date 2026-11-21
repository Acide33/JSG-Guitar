// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — état de l'application : réglages, pédalier, presets (même logique que
    Source/PluginProcessor.cpp). Chaque changement part vers le moteur audio.
*/
import {
  params, paramIndex, numFixedParams, numSlots, slotsPerRow, maxPedalKnobs, slotParamIndex,
  pedals, pedalIndex, knobToPosition, factoryPresets, isBassPreset, fromPosition, toPosition,
} from './data.js';
import { localStorageGet, localStorageSet } from './audio.js';

export const numParams = numFixedParams + numSlots * (1 + maxPedalKnobs);
export const values = new Float32Array(numParams);
export const slotTypes = new Int8Array(numSlots).fill(-1);   // -1 : emplacement vide

let audio = null;
let presetName = '';
const updaters = new Set();
let refreshQueued = false;

export function connectAudio(a, push = true) {
  audio = a;
  if (!push) return;   // plug-in : les réglages viennent de lui
  audio.setAll(values);
  slotTypes.forEach((type, slot) => audio.setPedal(slot, type));
}

//------------------------------------------------------------------------------
// Plug-in VST3 : réglages venus du logiciel hôte (projet rappelé, automation)
const lastLocalEdit = new Float64Array(numParams);

/** Tout l'état du plug-in (à l'ouverture de la fenêtre ou après un rappel du projet). */
export function applyRemoteState(st) {
  st.values.forEach((v, i) => { if (i < numParams) values[i] = v; });
  st.slots.forEach((t, i) => { if (i < numSlots) slotTypes[i] = t < pedals.length ? t : -1; });
  presetName = st.preset || '';
  lastSentPreset = presetName;
  refresh();
}

/** Un réglage changé par le logiciel hôte (automation). Ignoré pendant qu'on tourne ce potard. */
export function applyRemoteParam(i, v) {
  if (i < 0 || i >= numParams || performance.now() - lastLocalEdit[i] < 400 || values[i] === v) return;
  values[i] = v;
  refresh();
}
let lastSentPreset = null;

/** Les éléments de l'interface s'inscrivent ici pour être remis à jour après chaque changement. */
export function onRefresh(fn) { updaters.add(fn); }
export function refreshNow() { refreshQueued = false; updaters.forEach((fn) => fn()); }
export function refresh() {
  if (refreshQueued) return;
  refreshQueued = true;
  requestAnimationFrame(refreshNow);
}

//------------------------------------------------------------------------------
export const param = (id) => params[paramIndex[id]];
export const get = (id) => values[paramIndex[id]];
export const isOn = (id) => get(id) > 0.5;
export const isBassMode = () => get('instrument') > 0.5;

export function set(id, v, { save = true } = {}) {
  const i = paramIndex[id];
  if (i === undefined) return;
  const p = params[i];
  v = Math.min(p.max, Math.max(p.min, +v));
  if (p.kind === 'toggle' || p.kind === 'choice') v = Math.round(v);
  if (values[i] === v) return;
  values[i] = v;
  lastLocalEdit[i] = performance.now();
  audio?.setParam(i, v);
  if (save) scheduleSave();
  refresh();
}

/** Position 0..1 d'un réglage (pour les potards) et réglage par position. */
export const getPos = (id) => toPosition(param(id), get(id));
export const setPos = (id, pos) => set(id, fromPosition(param(id), pos));
export const reset = (id) => set(id, param(id).def);

export function setRaw(index, v) {
  if (values[index] === v) return;
  values[index] = v;
  lastLocalEdit[index] = performance.now();
  audio?.setParam(index, v);
  scheduleSave();
  refresh();
}

//------------------------------------------------------------------------------
// Pédalier
export const slotOn = (slot) => values[slotParamIndex(slot, 0)] > 0.5;
export const slotKnob = (slot, k) => values[slotParamIndex(slot, 1 + k)];
export const setSlotOn = (slot, on) => setRaw(slotParamIndex(slot, 0), on ? 1 : 0);
export const setSlotKnob = (slot, k, pos) => setRaw(slotParamIndex(slot, 1 + k), Math.min(1, Math.max(0, pos)));
export const slotPedal = (slot) => (slotTypes[slot] >= 0 ? pedals[slotTypes[slot]] : null);

/** Pose une pédale (identifiant ou null pour vider). resetKnobs : potards à leur valeur par défaut, pédale allumée. */
export function placePedal(slot, id, resetKnobs = true) {
  const type = id ? pedalIndex[id] ?? -1 : -1;
  slotTypes[slot] = type;
  audio?.setPedal(slot, type);
  if (resetKnobs) {
    setRaw(slotParamIndex(slot, 0), type >= 0 ? 1 : 0);
    const info = type >= 0 ? pedals[type] : null;
    for (let k = 0; k < maxPedalKnobs; k++)
      setRaw(slotParamIndex(slot, 1 + k), info && k < info.knobs.length ? knobToPosition(info.knobs[k], info.knobs[k].def) : 0.5);
  }
  scheduleSave();
  refresh();
}

export function clearAllSlots() {
  for (let slot = 0; slot < numSlots; slot++)
    if (slotTypes[slot] >= 0 || slotOn(slot)) placePedal(slot, null);
}

export function swapSlots(a, b) {
  if (a === b || a < 0 || b < 0 || a >= numSlots || b >= numSlots) return;
  const read = (s) => ({ type: slotTypes[s], p: [0, 1, 2, 3, 4].map((o) => values[slotParamIndex(s, o)]) });
  const A = read(a), B = read(b);
  const write = (s, c) => {
    slotTypes[s] = c.type;
    audio?.setPedal(s, c.type);
    c.p.forEach((v, o) => setRaw(slotParamIndex(s, o), c.type >= 0 || o > 0 ? v : 0));
  };
  write(a, B);
  write(b, A);
  refresh();
}

export function findFreeSlot(afterAmp) {
  const order = afterAmp ? [slotsPerRow, 0] : [0, slotsPerRow];
  for (const start of order)
    for (let s = start; s < start + slotsPerRow; s++) if (slotTypes[s] < 0) return s;
  return -1;
}

//------------------------------------------------------------------------------
// Presets
export const getPresetName = () => presetName;

export function userPresets() {
  try { return JSON.parse(localStorageGet('jsg.userPresets') || '[]'); } catch { return []; }
}
function storeUserPresets(list) { localStorageSet('jsg.userPresets', JSON.stringify(list)); }

/** Tous les presets : d'usine puis ceux de l'utilisateur. */
export const allPresets = () => [...factoryPresets.map((p) => ({ ...p, factory: true })), ...userPresets()];
export const presetIsBass = (p) => (p.factory ? isBassPreset(p) : (p.values?.instrument ?? 0) > 0.5);

function resetToDefaults() {
  for (const p of params) if (p.inPresets) set(p.id, p.def, { save: false });
  clearAllSlots();
}

export function loadPreset(preset) {
  resetToDefaults();

  if (preset.factory) {
    for (const [id, v] of Object.entries(preset.settings)) set(id, v, { save: false });
    for (const [slot, id, knobs] of preset.pedals) {
      placePedal(slot, id);
      const info = pedals[pedalIndex[id]];
      knobs.forEach((v, k) => { if (k < info.knobs.length) setSlotKnob(slot, k, knobToPosition(info.knobs[k], v)); });
    }
  } else {
    for (const [id, v] of Object.entries(preset.values || {})) if (param(id)?.inPresets) set(id, v, { save: false });
    for (const ped of preset.pedals || []) {
      if (pedalIndex[ped.id] === undefined) continue;
      placePedal(ped.slot, ped.id, false);
      setSlotOn(ped.slot, ped.on);
      ped.k.forEach((v, k) => setSlotKnob(ped.slot, k, v));
    }
  }

  presetName = preset.name;
  scheduleSave();
  refresh();
}

export function stepPreset(delta) {
  const list = allPresets();
  const bass = isBassMode();
  let i = list.findIndex((p) => p.name === presetName);
  for (let tries = 0; tries < list.length; tries++) {
    i = ((i < 0 ? (delta > 0 ? -1 : 0) : i) + (delta >= 0 ? 1 : -1) + list.length) % list.length;
    if (presetIsBass(list[i]) === bass) { loadPreset(list[i]); return; }
  }
}

export const firstPresetFor = (bass) => allPresets().find((p) => presetIsBass(p) === bass);

/** Instantané des réglages courants (format des presets de l'utilisateur). */
export function snapshot(name) {
  const v = {};
  for (const p of params) if (p.inPresets) v[p.id] = get(p.id);
  const peds = [];
  for (let slot = 0; slot < numSlots; slot++)
    if (slotTypes[slot] >= 0)
      peds.push({ slot, id: pedals[slotTypes[slot]].id, on: slotOn(slot), k: [0, 1, 2, 3].map((k) => slotKnob(slot, k)) });
  return { name, values: v, pedals: peds };
}

export function saveUserPreset(name) {
  name = name.trim();
  if (!name || factoryPresets.some((p) => p.name === name)) return false;
  const list = userPresets().filter((p) => p.name !== name);
  list.push(snapshot(name));
  list.sort((a, b) => a.name.localeCompare(b.name, 'fr'));
  storeUserPresets(list);
  presetName = name;
  scheduleSave();
  refresh();
  return true;
}

export function deleteUserPreset(name) {
  storeUserPresets(userPresets().filter((p) => p.name !== name));
  refresh();
}

/** GUITARE / BASSE (comme chooseInstrument du plug-in) : premier preset de l'instrument, sauf s'il y est déjà. */
export function chooseInstrument(bass) {
  const current = allPresets().find((p) => p.name === presetName);
  if (isBassMode() === bass && current && presetIsBass(current) === bass) return;
  loadPreset(firstPresetFor(bass));
  set('instrument', bass ? 1 : 0);
  const lowCut = get('lowCut');
  if (bass && lowCut > 30) set('lowCut', 25);
  else if (!bass && lowCut < 40) set('lowCut', 60);
}

//------------------------------------------------------------------------------
// Fichiers .jsgpreset : même format XML que le plug-in (échange possible entre les deux)
export function exportPresetXml() {
  const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
  let xml = `<?xml version="1.0" encoding="UTF-8"?>\n\n<JSGPRESET name="${esc(presetName || 'Mon preset')}" version="3">\n`;
  for (const p of params) if (p.inPresets) xml += `  <PARAM id="${p.id}" value="${+get(p.id).toFixed(4)}"/>\n`;
  for (let slot = 0; slot < numSlots; slot++) {
    if (slotTypes[slot] < 0) continue;
    const info = pedals[slotTypes[slot]];
    xml += `  <PEDAL slot="${slot}" id="${info.id}" name="${esc(info.name)}" on="${slotOn(slot) ? 1 : 0}"`;
    for (let k = 0; k < 4; k++) xml += ` k${k + 1}="${+slotKnob(slot, k).toFixed(5)}"`;
    xml += '/>\n';
  }
  return xml + '</JSGPRESET>\n';
}

/** Lit un preset du plug-in ou de la version web. Renvoie { preset, skipped } ou null. */
export function parsePresetXml(text, fallbackName) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  const root = doc.documentElement;
  if (!root || root.nodeName !== 'JSGPRESET') return null;
  const v = {};
  for (const e of root.getElementsByTagName('PARAM')) {
    const id = e.getAttribute('id');
    if (param(id)) v[id] = parseFloat(e.getAttribute('value'));
  }
  const peds = [], skipped = [];
  for (const e of root.getElementsByTagName('PEDAL')) {
    const id = e.getAttribute('id');
    if (pedalIndex[id] === undefined) { skipped.push(e.getAttribute('name') || id); continue; }
    peds.push({ slot: +e.getAttribute('slot'), id, on: e.getAttribute('on') !== '0',
      k: [1, 2, 3, 4].map((k) => parseFloat(e.getAttribute('k' + k) ?? '0.5')) });
  }
  return { preset: { name: root.getAttribute('name') || fallbackName, values: v, pedals: peds }, skipped };
}

export function importPreset(preset) {
  const list = userPresets().filter((p) => p.name !== preset.name);
  list.push(preset);
  list.sort((a, b) => a.name.localeCompare(b.name, 'fr'));
  storeUserPresets(list);
  loadPreset(preset);
}

//------------------------------------------------------------------------------
// Mémorisation de la session dans le navigateur (réglages retrouvés au prochain lancement)
let saveTimer = null;
function scheduleSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveSession, 400);
}

export function saveSession() {
  if (audio?.plugin) {
    // Plug-in : la session est dans le projet du logiciel hôte ; seul le nom du preset reste à lui donner
    if (presetName !== lastSentPreset) { lastSentPreset = presetName; audio.send({ t: 'presetName', name: presetName }); }
    return;
  }
  localStorageSet('jsg.session', JSON.stringify({ values: Array.from(values), slots: Array.from(slotTypes), presetName }));
}

export function restoreSession() {
  for (const p of params) values[paramIndex[p.id]] = p.def;
  for (let s = 0; s < numSlots; s++) for (let k = 0; k < maxPedalKnobs; k++) values[slotParamIndex(s, 1 + k)] = 0.5;

  try {
    const data = JSON.parse(localStorageGet('jsg.session') || 'null');
    if (data && data.values?.length === numParams) {
      data.values.forEach((v, i) => (values[i] = v));
      data.slots.forEach((t, i) => (slotTypes[i] = t < pedals.length ? t : -1));
      presetName = data.presetName || '';
      return true;
    }
  } catch { /* session illisible : on repart du premier preset */ }

  loadPreset({ ...factoryPresets[0], factory: true });
  return false;
}
