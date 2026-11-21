// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — version navigateur : l'interface.
    Même disposition que l'application PC (Source/PluginEditor.cpp, RackPage.h, PedalBoard.h,
    Panels.h, PlayerPage.h) : tout est placé aux coordonnées de l'image de fond (1536 x 1024).
*/
import {
  params, cabinets, bassCabinets, mics, rooms, pedals, numSlots, slotsPerRow,
  knobFormat, formatValue, playbacks, factoryPresets,
} from './data.js';
import * as S from './state.js';
import { AudioSystem, NativeAudio, isDesktopApp, isPluginMode, localStorageGet, localStorageSet } from './audio.js';
import { detectPitch, noteName, noteLetter } from './tuner.js';
import {
  el, place, stage, setStageScale, stagePoint, fitText, pointerKnob, drawnKnob, selector, powerIcon, ledButton,
  button, showMenu, toast, modal, makeDraggable, angleFor, showBubble, hideBubble,
} from './widgets.js';

const W = 1536, H = 1024;
const desktop = isDesktopApp();                    // version PC / Mac : le son est fait par le programme JSG Guitar
const macApp = desktop && /Macintosh|Mac OS X/.test(navigator.userAgent);   // programme Mac (Core Audio, enregistrement en AAC)
const OS = macApp ? 'macOS' : 'Windows';
const musicDir = macApp ? 'Musique/JSG Guitar' : 'Musique\\JSG Guitar';
const compressed = macApp ? 'AAC' : 'MP3';         // macOS n'a pas d'encodeur MP3 : prises compressées en AAC (.m4a)
const plugin = isPluginMode();                     // plug-in VST3 : le son est fait dans le logiciel hôte
const audio = desktop ? new NativeAudio() : new AudioSystem();
const root = stage();
const bg = document.getElementById('bg');
const fx = document.getElementById('fx');
const fxc = fx.getContext('2d');
const controls = document.getElementById('controls');

const colours = { blue: '#2f9bff', blueLight: '#9fd4ff', text: '#dde1e8', dim: '#7d838d', off: '#3a3f47' };
const fmt = (id) => formatValue(S.param(id), S.get(id));

let page = Math.min(plugin ? 1 : 2, Math.max(0, +(localStorageGet('jsg.page') ?? 1)));
let bassSkin = false;
let audioStarted = false;

// Réponses impulsionnelles chargées pendant la session (la dernière est gardée dans le navigateur)
/** Toucher au baffle ou au micro alors que l'IR les remplace : on coupe l'IR pour que le réglage s'entende. */
function cabTouched() {
  if (S.isOn('irOn') && impulse.current) {
    S.set('irOn', 0);
    toast("IR coupée : le baffle et le micro reprennent la main (l'IR les remplace quand elle est allumée).", 5000);
  }
}
const impulse = { list: [], index: -1, get current() { return this.list[this.index]; } };

//==============================================================================
// Mise à l'échelle de la fenêtre (proportions de l'image conservées)
function layout() {
  const s = Math.min(window.innerWidth / W, window.innerHeight / H);
  setStageScale(s);
  root.style.transform = `scale(${s})`;
  root.style.left = Math.round((window.innerWidth - W * s) / 2) + 'px';
  root.style.top = Math.round((window.innerHeight - H * s) / 2) + 'px';
  const dpr = window.devicePixelRatio || 1;
  fx.width = Math.round(W * s * dpr);
  fx.height = Math.round(H * s * dpr);
  fxc.setTransform(s * dpr, 0, 0, s * dpr, 0, 0);
}
window.addEventListener('resize', layout);
layout();

//==============================================================================
// Images de fond : guitare ou basse ; page 1 (rack) ou autres pages
const images = {};
for (const name of ['fond_guitare', 'rack_guitare', 'fond_basse', 'rack_basse', 'onglets_guitare', 'onglets_basse']) {
  images[name] = `img/${name}.webp`;
  new Image().src = images[name];
}

function updateBackground() {
  const skin = bassSkin ? 'basse' : 'guitare';
  const src = images[(page === 0 ? 'rack_' : 'fond_') + skin];
  if (!bg.src.endsWith(src)) bg.src = src;
  tabsLit.style.backgroundImage = `url(${images['onglets_' + skin]})`;
}

//==============================================================================
// Barre du haut : onglets
const tabsBox = el('div', '', controls); tabsBox.id = 'tabs';
const tabCells = [];
let selectedTab = 0;
const tabsLit = el('div', '', root, { display: 'none' });   // porte seulement l'image des onglets allumés

for (let i = 0; i < 6; i++) {
  const cell = el('div', '', tabsBox);
  cell.addEventListener('click', (ev) => {
    ev.stopPropagation();
    if (i === 0) { selectTab(0); showPresetMenu(cell); return; }
    selectTab(i === selectedTab ? 0 : i);
  });
  tabCells.push(cell);
}

function selectTab(index) {
  selectedTab = index;
  tabCells.forEach((c, i) => {
    c.classList.toggle('on', i === index);
    c.style.backgroundImage = i === index ? tabsLit.style.backgroundImage : '';
    c.style.backgroundPosition = `${-128 * i}px 0`;
  });
  panels.forEach((p, i) => p.e.classList.toggle('hidden', i + 1 !== index));
  if (index === 5) tunerStart(); else tunerStop();
  S.refreshNow();
}

//==============================================================================
// Presets
const presetMenuButton = place(el('div', 'hotspot', controls), 530 - 14, 109 - 14, 28, 28);
presetMenuButton.title = 'Presets';
presetMenuButton.addEventListener('click', (ev) => { ev.stopPropagation(); showPresetMenu(presetMenuButton); });

const presetSelector = selector(controls, {
  x: 548, y: 94, w: 424, h: 31, getText: () => S.getPresetName() || '—',
  onStep: (d) => S.stepPreset(d), onClick: (e) => showPresetMenu(e),
  tooltip: 'Flèches : preset précédent / suivant. Clic sur le nom : liste des presets.',
});
S.onRefresh(presetSelector.update);

function showPresetMenu(anchor) {
  const bass = S.isBassMode();
  const all = S.allPresets();
  const current = S.getPresetName();
  const items = [{ header: bass ? "Presets d'usine — basse" : "Presets d'usine — guitare" }];
  all.filter((p) => p.factory && S.presetIsBass(p) === bass)
     .forEach((p) => items.push({ text: p.name, checked: p.name === current, onClick: () => S.loadPreset(p) }));
  const mine = all.filter((p) => !p.factory && S.presetIsBass(p) === bass);
  if (mine.length) {
    items.push({ header: 'Mes presets' });
    mine.forEach((p) => items.push({ text: p.name, checked: p.name === current, onClick: () => S.loadPreset(p) }));
  }
  const isUser = all.some((p) => !p.factory && p.name === current);
  items.push({ separator: true },
    { text: 'Enregistrer sous…', onClick: askPresetName },
    { text: 'Supprimer ce preset', disabled: !isUser, onClick: () => S.deleteUserPreset(current) });
  showMenu(items, anchor);
}

function askPresetName() {
  const box = el('div');
  const row = el('div', 'row', box);
  el('label', '', row).textContent = 'Nom du preset';
  const input = el('input', '', row);
  input.type = 'text';
  const current = S.getPresetName();
  input.value = factoryPresets.some((p) => p.name === current) ? 'Mon preset' : current;
  el('p', 'note', box).textContent = desktop ? 'Les presets sont gardés dans le profil de JSG Guitar. Pour les sauvegarder ou les échanger : bouton Exporter (en haut à droite).' : 'Les presets sont gardés dans ce navigateur. Pour les sauvegarder ou les passer à la version PC : bouton Exporter (en haut à droite).';
  modal('Enregistrer le preset', box, [
    { text: 'Annuler' },
    { text: 'Enregistrer', primary: true, onClick: () => {
      if (!S.saveUserPreset(input.value)) { toast("Choisissez un nom non vide, différent de celui d'un preset d'usine."); return false; }
      toast('Preset enregistré : ' + input.value.trim());
    } },
  ]);
}

//==============================================================================
// GUITARE / BASSE
const instrument = el('div', '', controls); instrument.id = 'instrument';
instrument.title = 'Instrument : GUITARE ou BASSE. En basse : ampli, baffles et micros pour la basse, mélange DI / ampli (onglet AMPLI), accordeur 4, 5 ou 6 cordes.';
const instGuitar = el('div', '', instrument); instGuitar.textContent = 'GUITARE';
const instBass = el('div', '', instrument); instBass.textContent = 'BASSE';
instGuitar.addEventListener('click', () => S.isBassMode() && S.chooseInstrument(false));
instBass.addEventListener('click', () => !S.isBassMode() && S.chooseInstrument(true));
S.onRefresh(() => {
  const bass = S.isBassMode();
  instGuitar.classList.toggle('on', !bass);
  instBass.classList.toggle('on', bass);
  if (bass !== bassSkin) { bassSkin = bass; updateBackground(); selectTab(selectedTab); }
});

//==============================================================================
// Enregistreur (REC) : WAV 24 bits (navigateur : téléchargé à l'arrêt) ; version PC : WAV ou MP3
const recBar = el('div', '', controls); recBar.id = 'recorder';
if (plugin) recBar.style.display = 'none';   // plug-in : on enregistre dans le logiciel hôte
const recButton = el('div', 'rec', recBar);
recButton.innerHTML = '<i></i><span>REC</span>';
const recTime = el('div', 'time', recBar);
const recMore = el('div', 'more', recBar); recMore.textContent = '▼';
let recSource = +(localStorageGet('jsg.recSource') || 0);
const recFormats = { wav: 'WAV 24 bits', 'mp3-320': `${compressed} 320 kbit/s`, 'mp3-192': `${compressed} 192 kbit/s` };
let recFormat = desktop && recFormats[localStorageGet('jsg.recFormat')] ? localStorageGet('jsg.recFormat') : 'wav';
let recording = false, converting = false, lastTake = null;

function updateRecTitle() {
  recButton.title = desktop ? `Enregistrer ce qu'on entend (${recFormats[recFormat]}, dans ${musicDir})`
                            : 'Enregistrer ce qu\'on entend (fichier WAV 24 bits, téléchargé à l\'arrêt)';
}
updateRecTitle();

recButton.addEventListener('click', async () => {
  if (!audioStarted || converting) return;
  if (!recording) {
    const [format, kbps] = recFormat === 'wav' ? ['wav', 0] : ['mp3', +recFormat.slice(4)];
    audio.startRecording(recSource, format, kbps);
    recording = true;
  } else {
    recording = false;
    if (desktop && recFormat !== 'wav') { converting = true; updateRecorder(); toast(`Conversion de la prise en ${compressed}…`, 3000); }
    const result = await audio.stopRecording();
    converting = false;
    if (!result.ok) { toast('Enregistrement impossible : ' + (result.error || '')); updateRecorder(); return; }
    if (result.blob) {
      // Navigateur : le fichier est téléchargé
      const now = new Date(), p2 = (n) => String(n).padStart(2, '0');
      const name = `JSG Guitar ${now.getFullYear()}-${p2(now.getMonth() + 1)}-${p2(now.getDate())} ${p2(now.getHours())}h${p2(now.getMinutes())}m${p2(now.getSeconds())}${recSource ? ' (guitare)' : ''}.wav`;
      lastTake = { blob: result.blob, name };
      const a = el('a', '', document.body);
      a.href = URL.createObjectURL(result.blob); a.download = name; a.click(); a.remove();
      toast('Enregistrement téléchargé : ' + name);
    } else {
      // PC : le fichier est déjà sur le disque
      lastTake = { name: result.name };
      if (result.note) toast(result.note + ' ' + result.name, 9000);
      else toast(`Enregistré dans ${musicDir} : ` + result.name, 5000);
    }
  }
  updateRecorder();
});

recMore.addEventListener('click', (ev) => {
  ev.stopPropagation();
  const formatItems = desktop
    ? Object.entries(recFormats).map(([id, text]) => ({ text: id === 'wav' ? text + ' (qualité studio)' : text, checked: recFormat === id, disabled: recording, onClick: () => setRecFormat(id) }))
    : [{ text: 'WAV 24 bits', checked: true }, { text: 'MP3 : dans la version PC', disabled: true }];
  showMenu([
    { header: 'Ce qui est enregistré' },
    { text: "Tout ce qu'on entend", checked: recSource === 0, onClick: () => setRecSource(0) },
    { text: 'Guitare seule (sans le morceau)', checked: recSource === 1, onClick: () => setRecSource(1) },
    { header: 'Format' },
    ...formatItems,
    { separator: true },
    { text: 'Écouter la dernière prise', disabled: !lastTake, onClick: () => { player.loadTake(lastTake); setPage(2); } },
    ...(desktop ? [{ text: 'Ouvrir le dossier des enregistrements', onClick: () => audio.openRecordings() }] : []),
  ], recMore);
});
function setRecFormat(f) { recFormat = f; localStorageSet('jsg.recFormat', f); updateRecTitle(); }
function setRecSource(s) { recSource = s; localStorageSet('jsg.recSource', String(s)); }

function updateRecorder() {
  recBar.classList.toggle('recording', recording);
  recButton.querySelector('span').textContent = recording ? 'STOP' : converting ? compressed + '…' : 'REC';
  if (recording) {
    const t = Math.floor(audio.recordedSeconds());
    recTime.textContent = `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`;
  } else recTime.textContent = '';
}

//==============================================================================
// Boutons du coin : presets en fichier, réglages
const fileButton = place(el('div', 'hotspot', controls), 1437 - 23, 35 - 23, 46, 46);
fileButton.title = 'Importer / exporter un preset';
fileButton.addEventListener('click', (ev) => {
  ev.stopPropagation();
  showMenu([
    { text: 'Exporter le preset dans un fichier…', onClick: exportPreset },
    { text: 'Importer un preset depuis un fichier…', onClick: () => pickFile('.jsgpreset,.xml', importPresetFile) },
  ], fileButton);
});

const settingsButton = place(el('div', 'hotspot', controls), 1497 - 24, 34 - 24, 48, 48);
settingsButton.title = 'Réglages audio, plein écran, à propos';
settingsButton.addEventListener('click', (ev) => {
  ev.stopPropagation();
  showMenu([
    { text: 'Réglages audio (entrée, latence)…', onClick: showAudioSettings },
    { text: document.fullscreenElement ? 'Quitter le plein écran' : 'Plein écran', onClick: toggleFullscreen },
    { separator: true },
    { text: 'À propos de JSG Guitar', onClick: showAbout },
  ], settingsButton);
});

function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else document.documentElement.requestFullscreen?.().catch(() => toast('Plein écran refusé par le navigateur.'));
}

function showAbout() {
  if (plugin) {
    const st = audio.lastState || {};
    modal('JSG Guitar — plug-in VST3 (prototype sans JUCE)',
      "Simulateur d'ampli guitare et basse : même moteur que la version PC et la version navigateur (ampli 3 canaux, baffles, micros, rooms, égaliseur, rack d'effets, 16 emplacements de pédales, IR)." +
      (st.sampleRate ? `\n\nFréquence : ${st.sampleRate} Hz. Latence du plug-in : ${st.latency} échantillons (compensée par votre logiciel).` : '') +
      "\n\nLes réglages sont enregistrés dans votre projet et automatisables. Le lecteur de morceaux et l'enregistrement sont ceux de votre logiciel.");
    return;
  }
  if (desktop) {
    const s = audio.status || {};
    modal(macApp ? 'JSG Guitar — version Mac (prototype)' : 'JSG Guitar — version PC (prototype sans JUCE)',
      "Simulateur d'ampli guitare et basse : même moteur que la version navigateur, mais le son passe directement par la carte son (" + (macApp ? 'Core Audio' : 'WASAPI') +
      (s.running ? (s.exclusive ? ', mode exclusif' : ', mode partagé') + `, ${s.sampleRate} Hz, ${s.blockSize} échantillons, latence ${audio.latencyMs().toFixed(1)} ms` : '') + ").\n\n" +
      `En plus de la version navigateur : changement de tonalité du lecteur, enregistrements dans ${musicDir}, choix du mode exclusif et du tampon.\n\n` +
      "Pas encore dans ce prototype : plug-ins (CLAP prévu), pédales .jsgpedal, séparation des pistes, boîte à rythmes.");
    return;
  }
  modal('JSG Guitar — version navigateur',
    "Simulateur d'ampli guitare et basse. Le moteur audio est celui de l'application PC, compilé en WebAssembly : même ampli 3 canaux, mêmes baffles, micros, rooms, égaliseur, rack d'effets et pédales intégrées.\n\n" +
    "En bas, trois pages : le rack d'effets, le pédalier (16 emplacements, 10 pédales) et le lecteur de morceau (avec les playbacks JSG).\n\n" +
    "Réservé à la version PC : plug-ins VST3, pédales en fichier .jsgpedal, séparation des pistes, changement de tonalité du lecteur.");
}

function pickFile(accept, onFile) {
  const input = el('input', '', document.body);
  input.type = 'file'; input.accept = accept; input.style.display = 'none';
  input.addEventListener('change', () => { if (input.files[0]) onFile(input.files[0]); input.remove(); });
  input.click();
}

function exportPreset() {
  const name = (S.getPresetName() || 'Mon preset').replace(/[\\/:*?"<>|]/g, '_');
  const a = el('a', '', document.body);
  a.href = URL.createObjectURL(new Blob([S.exportPresetXml()], { type: 'application/xml' }));
  a.download = name + '.jsgpreset';
  a.click(); a.remove();
}

async function importPresetFile(file) {
  const result = S.parsePresetXml(await file.text(), file.name.replace(/\.[^.]+$/, ''));
  if (!result) { modal('Import impossible', "Ce fichier n'est pas un preset JSG Guitar."); return; }
  S.importPreset(result.preset);
  toast('Preset importé : ' + result.preset.name + (result.skipped.length ? ` (pédales absentes de la version web : ${result.skipped.join(', ')})` : ''), 6000);
}

//==============================================================================
// Tête d'ampli : potards, POWER
const ampKnobs = [['gain', 226.5, 532], ['bass', 357.5, 532], ['mid', 486.5, 530.5], ['treble', 614.5, 531], ['presence', 743.5, 530.5], ['master', 873.5, 531.5]];
for (const [id, x, y] of ampKnobs) {
  const k = pointerKnob(controls, { x, y, r: 25, style: 'large', get: () => S.getPos(id), set: (p) => S.setPos(id, p),
    reset: () => S.reset(id), format: () => fmt(id) });
  S.onRefresh(k.update);
}

const powerLens = place(el('div', 'hotspot', controls), 1439 - 32, 505 + 16 - 46, 64, 92);
powerLens.style.borderRadius = '30px';
powerLens.title = "Allumer / éteindre l'ampli (éteint : son direct)";
powerLens.addEventListener('click', () => S.set('power', S.isOn('power') ? 0 : 1));

//==============================================================================
// Pages du bas : 1 RACK, 2 PÉDALIER, 3 LECTEUR (la page RYTHMES est retirée pour l'instant)
const pageSwitch = el('div', '', controls); pageSwitch.id = 'pageSwitch';
pageSwitch.title = "Page 1 : le rack d'effets. Page 2 : le pédalier. Page 3 : le lecteur de morceau.\nLe point bleu signale qu'un effet de la page est allumé, ou que le morceau joue.";
const pageButtons = ['1 RACK', '2 PÉDALIER', '3 LECTEUR', '4 RYTHMES'].map((name, i) => {
  const b = el('div', '', pageSwitch);
  el('i', '', b);
  el('span', '', b).textContent = name;
  b.addEventListener('click', () => setPage(i));
  if (i === 3 || (plugin && i === 2)) b.style.display = 'none';   // RYTHMES retiré ; plug-in : lecteur laissé au logiciel hôte
  return b;
});
if (plugin) pageSwitch.title = "Page 1 : le rack d'effets. Page 2 : le pédalier.\nLe point bleu signale qu'un effet de la page est allumé.";

const pages = [0, 1, 2, 3].map(() => el('div', 'page', controls));

function setPage(p) {
  page = p;
  localStorageSet('jsg.page', String(p));
  pages.forEach((e, i) => e.classList.toggle('hidden', i !== p));
  pageButtons.forEach((b, i) => b.classList.toggle('on', i === p));
  updateBackground();
  if (p === 2) player.draw();
  S.refreshNow();   // textes à ajuster maintenant que la page est affichée
}

function anyRackEffectOn() {
  return S.isOn('gateOn') || S.isOn('compOn') || S.isOn('driveOn') || (S.isOn('fxOn') && (S.isOn('fxChorus') || S.isOn('fxDelay') || S.isOn('fxReverb')));
}

//==============================================================================
// Page 1 : le rack
{
  const P = pages[0];
  const switches = [
    ['gateOn', 39, 688, 'Noise gate : coupe le souffle entre les notes'],
    ['compOn', 237, 688, 'Compresseur : égalise le volume des notes, allonge le sustain'],
    ['driveOn', 433, 688, "Drive : saturation placée avant l'ampli"],
    ['cabOn', 621.5, 688.5, 'Baffle'],
    ['micOn', 843, 689, 'Micro'],
    ['roomOn', 1054, 689, 'Room : la pièce autour du baffle'],
    ['fxOn', 1275, 694, 'Effets : allume ou éteint chorus, delay et reverb ensemble'],
    ['irOn', 39, 896, 'Réponse impulsionnelle : allumée, elle remplace le baffle et le micro'],
  ];
  for (const [id, x, y, tip] of switches) {
    const b = powerIcon(P, { x, y, get: () => S.isOn(id), tooltip: tip,
      set: (v) => { S.set(id, v ? 1 : 0); if (id === 'cabOn' || id === 'micOn') cabTouched(); } });
    S.onRefresh(b.update);
  }

  const knobs = [
    ['gateThreshold', 64, 767.5, 24, 'small', 'Seuil : niveau en dessous duquel le son est coupé'],
    ['gateDecay', 158, 767.5, 24, 'small', 'Temps de fermeture'],
    ['compLevel', 261.5, 767.5, 24, 'small', 'Volume en sortie du compresseur'],
    ['compAttack', 353.5, 767.5, 24, 'small', "Attaque : court = l'attaque des notes est écrasée ; long = elle claque"],
    ['drive', 460, 767.5, 24, 'small', 'Quantité de saturation'],
    ['output', 1229, 950, 26, 'small', 'Niveau de sortie'],
    ['input', 971, 950, 26, 'small', "Niveau d'entrée de la guitare, avant le rack et l'ampli. Si le dernier segment de la barre passe au rouge, baissez-le."],
    ['fxMix', 1433, 699, 14, 'chrome', 'MIX : quantité de chorus'],
    ['fxTime', 1433, 743, 14, 'chrome', 'TIME : écart entre les répétitions du delay'],
    ['fxFeedback', 1433, 787, 14, 'chrome', 'FEEDBACK : nombre de répétitions du delay'],
    ['fxLevel', 1433, 831.5, 14, 'chrome', 'LEVEL : volume du delay et de la reverb'],
  ];
  for (const [id, x, y, r, style, tip] of knobs) {
    const k = pointerKnob(P, { x, y, r, style, get: () => S.getPos(id), set: (p) => S.setPos(id, p), reset: () => S.reset(id), format: () => fmt(id), tooltip: tip });
    S.onRefresh(k.update);
  }

  const driveY = [738.5, 760.5, 780.5, 802];
  const driveTips = ["MD (mid drive) : overdrive à bosse de médiums", 'OD : saturation douce et chaude', 'DIST : distorsion franche', 'FUZZ : fuzz épais, façon années 60'];
  driveY.forEach((y, i) => {
    const led = ledButton(P, { area: [498, Math.round(y) - 10, 76, 20], ledX: 509.5, ledY: y, radius: 4,
      get: () => Math.round(S.get('driveType')) === i, onClick: () => S.set('driveType', i), tooltip: driveTips[i] });
    S.onRefresh(led.update);
  });

  [['fxChorus', 739], ['fxDelay', 785.5], ['fxReverb', 832]].forEach(([id, y]) => {
    // Les trois effets ne sonnent que si l'interrupteur FX est allumé : allumer un effet allume aussi FX
    const led = ledButton(P, { area: [1258, Math.round(y) - 20, 132, 40], ledX: 1277, ledY: y, radius: 6.5,
      get: () => S.isOn(id), tooltip: "Allumer / éteindre l'effet (l'interrupteur FX, au-dessus, doit être allumé)",
      onClick: () => {
        const on = !S.isOn(id);
        S.set(id, on ? 1 : 0);
        if (on && !S.isOn('fxOn')) { S.set('fxOn', 1); toast('Interrupteur FX allumé.'); }
      } });
    S.onRefresh(() => { led.update(); led.e.classList.toggle('dim', !S.isOn('fxOn')); });
  });

  const choiceSelector = (parent, box, idFn, names, after) => {
    const [x, y, w, h] = box;
    return selector(parent, { x, y, w, h, arrow: 26, fontSize: 14.5,
      getText: () => names()[Math.round(S.get(idFn()))],
      onStep: (d) => { const n = names().length; S.set(idFn(), (Math.round(S.get(idFn())) + d + n) % n); after?.(); },
      onClick: (e) => showMenu(names().map((name, i) => ({ text: name, checked: i === Math.round(S.get(idFn())), onClick: () => { S.set(idFn(), i); after?.(); } })), e) });
  };
  const cabId = () => (S.isBassMode() ? 'bassCab' : 'cabType');
  const cabNames = () => (S.isBassMode() ? bassCabinets : cabinets);

  const cabSel = choiceSelector(P, [618, 818, 177, 30], cabId, cabNames, cabTouched);
  const micSel = choiceSelector(P, [839, 818, 167, 30], () => 'micType', () => mics, cabTouched);
  const roomSel = choiceSelector(P, [1051, 818, 171, 30], () => 'roomType', () => rooms);

  const irDisplay = place(el('div', 'irdisplay', P), 40, 912, 414, 58);
  irDisplay.title = 'Charger une réponse impulsionnelle de baffle (WAV, FLAC, MP3…) — on peut aussi glisser le fichier ici';
  irDisplay.addEventListener('click', () => pickFile('audio/*,.wav,.aif,.aiff,.flac', loadImpulseFile));
  dropTarget(irDisplay, loadImpulseFile);

  const irSel = selector(P, { x: 35, y: 976, w: 424, h: 30, arrow: 30, fontSize: 14.5,
    getText: () => impulse.current?.name || 'Aucune IR chargée', onStep: stepImpulse,
    onClick: () => pickFile('audio/*,.wav,.aif,.aiff,.flac', loadImpulseFile) });

  S.onRefresh(() => {
    const irActive = S.isOn('irOn') && !!impulse.current;
    for (const s of [cabSel, micSel, roomSel, irSel]) s.update();
    cabSel.setDimmed(!S.isOn('cabOn') || irActive);
    micSel.setDimmed(!S.isOn('micOn') || irActive);
    roomSel.setDimmed(!S.isOn('roomOn'));
    irSel.setDimmed(!irActive);
  });
}

//==============================================================================
// Réponses impulsionnelles (chargées pendant la session ; la dernière est gardée dans le navigateur)

async function loadImpulseFile(file) {
  if (!audioStarted) { toast("Démarrez d'abord le son (choix GUITARE / BASSE)."); return; }
  try {
    const bytes = await file.arrayBuffer();
    await useImpulse({ name: file.name.replace(/\.[^.]+$/, ''), bytes });
    if (!plugin && bytes.byteLength < 1500000) {
      try { localStorage.setItem('jsg.ir', JSON.stringify({ name: impulse.current.name, data: toBase64(bytes) })); } catch { /* trop gros pour le stockage */ }
    }
  } catch (err) {
    modal('Fichier illisible', "Ce fichier audio n'a pas pu être lu.\n\n" + err.message);
  }
}

async function useImpulse(ir) {
  const result = await audio.loadImpulse(ir.bytes.slice(0), ir.name);
  if (!result.ok) throw new Error(result.error || '');
  ir.shape = result.shape;
  impulse.list = impulse.list.filter((x) => x.bytes);   // l'IR venue du projet (sans fichier ici) est remplacée
  const existing = impulse.list.findIndex((x) => x.name === ir.name);
  if (existing >= 0) impulse.list[existing] = ir; else impulse.list.push(ir);
  impulse.index = impulse.list.indexOf(ir);
  S.set('irOn', 1);   // charger une IR l'allume
  S.refresh();
}

function stepImpulse(d) {
  if (impulse.list.length < 2 || impulse.list.some((x) => !x.bytes)) return;
  impulse.index = (impulse.index + d + impulse.list.length) % impulse.list.length;
  useImpulse(impulse.current);
}

const toBase64 = (buf) => { let s = ''; const b = new Uint8Array(buf); for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000)); return btoa(s); };
const fromBase64 = (str) => { const s = atob(str); const b = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); return b.buffer; };

function dropTarget(element, onFile) {
  element.addEventListener('dragover', (ev) => { ev.preventDefault(); ev.stopPropagation(); element.classList.add('dragover'); });
  element.addEventListener('dragleave', () => element.classList.remove('dragover'));
  element.addEventListener('drop', (ev) => {
    ev.preventDefault(); ev.stopPropagation();
    element.classList.remove('dragover');
    const file = ev.dataTransfer.files[0];
    if (file) onFile(file);
  });
}

//==============================================================================
// Page 2 : le pédalier
const slotX = [103.5, 290.8, 481.1, 672.0, 862.5, 1053.0, 1243.4, 1433.6];
const slotY = [732.7, 890.7], lightY = [675.5, 828.0];
const bodyW = 144, bodyH = 102;
const slotViews = [];

{
  const P = pages[1];
  const legend = el('div', '', P); legend.id = 'legend';
  legend.textContent = "RANGÉE DU HAUT : AVANT L'AMPLI      •      RANGÉE DU BAS : APRÈS LE BAFFLE, EN STÉRÉO      •      LE SIGNAL VA DE GAUCHE À DROITE";
  legend.style.whiteSpace = 'pre';

  for (let slot = 0; slot < numSlots; slot++) slotViews.push(makeSlot(P, slot));
  S.onRefresh(() => slotViews.forEach((v) => v.update()));
}

function makeSlot(parent, slot) {
  const row = Math.floor(slot / slotsPerRow), col = slot % slotsPerRow;
  const cx = slotX[col], cy = slotY[row], ly = lightY[row];
  const top = Math.round(ly) - 8;
  const x0 = Math.round(cx) - bodyW / 2 - 4;
  const e = place(el('div', 'slot', parent), x0, top, bodyW + 8, Math.round(cy) + bodyH / 2 + 4 - top);
  const bx = Math.round(cx) - bodyW / 2 - x0, by = Math.round(cy) - bodyH / 2 - top;

  const housing = place(el('div', 'housing', e), cx - x0 - 17, ly - top - 4.5);
  el('i', '', housing);
  const empty = place(el('div', 'empty', e), bx, by, bodyW, bodyH);
  empty.title = 'Ajouter une pédale';
  empty.addEventListener('click', () => showSlotMenu(slot, empty));

  let shownType = -2, view = null;

  const build = () => {
    view?.box.remove();
    view = null;
    const type = S.slotTypes[slot];
    empty.classList.toggle('hidden', type >= 0);
    if (type < 0) return;

    const info = pedals[type];
    const box = place(el('div', 'pedal', e), bx, by, bodyW, bodyH);
    const c = info.colour;
    box.style.background = `linear-gradient(${shade(c, 0.25)}, ${shade(c, -0.45)})`;
    box.title = 'Clic sur le nom : remplacer ou retirer. Glisser : déplacer vers un autre emplacement.';
    const strip = el('div', 'strip', box);
    strip.textContent = info.name;
    strip.title = info.description;
    strip.addEventListener('click', (ev) => { ev.stopPropagation(); showSlotMenu(slot, strip); });
    box.addEventListener('contextmenu', (ev) => { ev.preventDefault(); showSlotMenu(slot, strip); });

    const count = info.knobs.length, dia = count >= 4 ? 27 : 30;
    const kcy = 20 + 4 + dia / 2;
    const ink = brightness(c) > 0.55 ? '#101317' : '#fff';
    const knobs = info.knobs.map((k, i) => {
      const kcx = Math.round(bodyW * (i + 0.5) / count);
      const kn = place(el('div', 'pknob', box), kcx - dia / 2 + 1.5, kcy - dia / 2 + 1.5, dia - 3, dia - 3);
      const r = (dia - 3) / 2;
      const ptr = place(el('div', 'ptr', kn), r - 1.1, 2.5, 2.2, r * 0.72);
      ptr.style.transformOrigin = `50% ${r - 2.5}px`;
      kn.title = `${info.name} - ${k.name}`;
      const label = place(el('div', 'klabel', box), kcx - 30, kcy + dia / 2 - 1, 60, 12);
      label.textContent = k.name;
      label.style.color = ink;
      const update = () => { ptr.style.transform = `rotate(${angleFor(S.slotKnob(slot, i))}deg)`; };
      makeDraggable(kn, { get: () => S.slotKnob(slot, i), set: (p) => { S.setSlotKnob(slot, i, p); update(); }, sensitivity: 180,
        reset: () => { S.setSlotKnob(slot, i, knobPositionDefault(k)); update(); },
        bubble: () => ({ x: x0 + bx + kcx, y: top + by + kcy - dia / 2 - 4, text: knobFormat(k, S.slotKnob(slot, i)) }) });
      update();
      return update;
    });

    const foot = place(el('div', 'foot', box), bodyW / 2 + 7 - 11, bodyH - 15 - 11);
    foot.title = 'Allumer / éteindre la pédale';
    foot.addEventListener('click', (ev) => { ev.stopPropagation(); S.setSlotOn(slot, !S.slotOn(slot)); });
    foot.addEventListener('pointerdown', (ev) => ev.stopPropagation());
    const led = place(el('div', 'redled', box), bodyW / 2 + 7 - 11 - 13 - 4.5, bodyH - 15 - 4.5);
    const shadeEl = el('div', 'shade', box);

    enablePedalDrag(box, slot);
    view = { box, knobs, led, shadeEl };
  };

  const update = () => {
    if (S.slotTypes[slot] !== shownType) { shownType = S.slotTypes[slot]; build(); }
    const on = shownType >= 0 && S.slotOn(slot);
    e.classList.toggle('lit', on);
    if (view) {
      view.knobs.forEach((u) => u());
      view.led.classList.toggle('on', on);
      view.shadeEl.classList.toggle('hidden', on);
    }
  };

  const rect = { x: x0 + bx, y: top + by, w: bodyW, h: bodyH };
  return { update, rect, setDrop: (d) => (view ? view.box : empty).classList.toggle('drop', d) };
}

const knobPositionDefault = (k) => (k.log ? Math.log(k.def / k.min) / Math.log(k.max / k.min) : (k.def - k.min) / (k.max - k.min));

function shade(hex, amount) {
  const n = parseInt(hex.slice(1), 16);
  let r = n >> 16, g = (n >> 8) & 255, b = n & 255;
  if (amount >= 0) { r += (255 - r) * amount; g += (255 - g) * amount; b += (255 - b) * amount; }
  else { r *= 1 + amount; g *= 1 + amount; b *= 1 + amount; }
  return `rgb(${r | 0}, ${g | 0}, ${b | 0})`;
}
function brightness(hex) {
  const n = parseInt(hex.slice(1), 16);
  return Math.sqrt(0.241 * ((n >> 16) / 255) ** 2 + 0.691 * (((n >> 8) & 255) / 255) ** 2 + 0.068 * ((n & 255) / 255) ** 2);
}

/** Glisser une pédale vers un autre emplacement : déplacement ou échange. */
function enablePedalDrag(box, slot) {
  let start = null, ghost = null, target = -1;
  box.addEventListener('pointerdown', (ev) => {
    if (ev.button !== 0 || ev.target.closest('.pknob, .foot, .strip')) return;
    start = stagePoint(ev);
    box.setPointerCapture(ev.pointerId);
  });
  box.addEventListener('pointermove', (ev) => {
    if (!start) return;
    const p = stagePoint(ev);
    if (!ghost && Math.hypot(p.x - start.x, p.y - start.y) < 6) return;
    if (!ghost) { ghost = box.cloneNode(true); ghost.style.opacity = '0.75'; ghost.style.pointerEvents = 'none'; ghost.style.zIndex = '40'; root.appendChild(ghost); }
    place(ghost, p.x - bodyW / 2, p.y - bodyH / 2);
    const t = slotViews.findIndex((v) => p.x >= v.rect.x && p.x <= v.rect.x + v.rect.w && p.y >= v.rect.y && p.y <= v.rect.y + v.rect.h);
    if (t !== target) { if (target >= 0) slotViews[target].setDrop(false); target = t !== slot ? t : -1; if (target >= 0) slotViews[target].setDrop(true); }
  });
  const end = () => {
    if (ghost) { ghost.remove(); ghost = null; }
    if (target >= 0) { slotViews[target].setDrop(false); S.swapSlots(slot, target); }
    start = null; target = -1;
  };
  box.addEventListener('pointerup', end);
  box.addEventListener('pointercancel', end);
}

function catalogueItems(slot) {
  const current = S.slotTypes[slot];
  const items = [];
  for (const cat of [...new Set(pedals.map((p) => p.category))]) {
    items.push({ header: cat });
    pedals.forEach((p, i) => { if (p.category === cat) items.push({ text: p.name, checked: i === current, onClick: () => { S.placePedal(slot, p.id); setPage(1); } }); });
  }
  return items;
}

function showSlotMenu(slot, anchor) {
  if (S.slotTypes[slot] < 0) { showMenu(catalogueItems(slot), anchor); return; }
  const firstRow = slot < slotsPerRow, col = slot % slotsPerRow;
  showMenu([
    { text: 'Remplacer par', submenu: catalogueItems(slot) },
    { text: 'Retirer la pédale', onClick: () => S.placePedal(slot, null) },
    { separator: true },
    { text: 'Déplacer vers la gauche', disabled: col === 0, onClick: () => S.swapSlots(slot, slot - 1) },
    { text: 'Déplacer vers la droite', disabled: col === slotsPerRow - 1, onClick: () => S.swapSlots(slot, slot + 1) },
    { text: firstRow ? "Passer après l'ampli (rangée du bas)" : "Passer avant l'ampli (rangée du haut)",
      onClick: () => S.swapSlots(slot, firstRow ? slot + slotsPerRow : slot - slotsPerRow) },
  ], anchor);
}

function addPedalFromCatalogue(id) {
  const slot = S.findFreeSlot(pedals.find((p) => p.id === id).afterAmp);
  if (slot < 0) modal('Pédalier plein', 'Les 16 emplacements sont occupés. Retirez une pédale pour en ajouter une autre.');
  else { S.placePedal(slot, id); setPage(1); }
}

//==============================================================================
// Page 3 : le lecteur de morceau
const player = (() => {
  const P = pages[2];
  const panel = el('div', 'bigpanel', P);
  const at = (e, x, y, w, h) => place(e, x, y, w, h);
  const engine = audio.player;            // lecteur du navigateur ou du programme PC (mêmes fonctions)
  let song = null;                        // { title, peaks, duration }
  let loopSong = localStorageGet('jsg.loop') === '1';
  let A = null, B = null, abOn = false;
  let speed = 1, transpose = 0, loading = false;

  // Les playbacks JSG sont un pack séparé (licence CC BY-NC 4.0), à copier dans ui/playbacks.
  // available : true = présent, false = absent (404), undefined = pas encore vérifié ou vérification impossible.
  const packMessage =
    "Les playbacks JSG ne sont pas installés.\n\n" +
    "Téléchargez le pack gratuit « JSG Playbacks » sur jeromesolergroup.com, " +
    (macApp ? "puis copiez ses fichiers MP3 dans le dossier Musique/JSG Guitar/Playbacks (créé par JSG Guitar) et relancez JSG Guitar.\n\n"
            : "puis copiez ses fichiers MP3 dans le dossier Musique\\JSG Guitar\\Playbacks (ou le dossier « playbacks » du dossier « ui », " +
              "à côté de JSG Guitar.exe) et relancez JSG Guitar.\n\n") +
    "Vous pouvez aussi jouer sur vos propres morceaux : bouton OUVRIR… ou glisser un fichier.";
  const playPlayback = async (p) => {
    if (p.available === false) { modal('Playbacks JSG', packMessage); return false; }
    return loadUrl(p.file, p.title);
  };

  button(panel, { x: 16, y: 14, w: 96, h: 38, text: 'OUVRIR…', tooltip: 'Choisir un morceau : WAV, MP3, FLAC… On peut aussi glisser le fichier ici.',
    onClick: () => pickFile('audio/*', (f) => loadBlob(f, f.name.replace(/\.[^.]+$/, ''))) });
  const pbButton = button(panel, { x: 116, y: 14, w: 118, h: 38, text: 'PLAYBACKS ▾', tooltip: 'Les morceaux de JSG sans la guitare',
    onClick: () => showMenu([{ header: 'Playbacks JSG' }, ...playbacks.map((p) => ({ text: p.title + (p.available === false ? ' (à télécharger)' : ''), checked: song?.title === p.title, onClick: () => playPlayback(p) }))], pbButton.e) });

  // Rangée du haut, alignée à droite : [titre] ■ ▶ ↻ temps | VITESSE | TONALITÉ (PC)
  const groupW = 210, keyX = 1496 - groupW;
  const speedX = engine.canTranspose ? keyX - 10 - groupW : keyX;
  const timeX = speedX - 10 - 140, loopX = timeX - 4 - 46, playX = loopX - 4 - 56, stopX = playX - 4 - 46;

  const title = at(el('div', 'ptitle', panel), 240, 20, stopX - 10 - 240, 26);
  const titleText = el('span', '', title);
  const setTitle = (t) => { titleText.textContent = t; fitText(titleText, title); };
  const stopB = button(panel, { x: stopX, y: 14, w: 46, h: 38, text: '■', tooltip: 'Arrêt (retour au début)', onClick: () => { engine.stop(); draw(); } });
  const playB = button(panel, { x: playX, y: 14, w: 56, h: 38, text: '▶', tooltip: 'Lecture / pause (barre d\'espace)', toggle: () => engine.isPlaying(), onClick: () => toggle() });
  const loopB = button(panel, { x: loopX, y: 14, w: 46, h: 38, text: '↻', tooltip: 'Répéter le morceau', toggle: () => loopSong,
    onClick: () => { loopSong = !loopSong; engine.setLoop(loopSong); localStorageSet('jsg.loop', loopSong ? '1' : '0'); } });
  [stopB, playB, loopB].forEach((b) => (b.e.style.fontSize = '18px'));
  const time = at(el('div', 'ptime', panel), timeX, 23, 140, 24);

  const centredLabel = (text, x, w) => { const l = at(el('div', 'label', panel), x, 4, w, 16); l.textContent = text; l.style.textAlign = 'center'; };
  centredLabel('VITESSE', speedX + 44, 122);
  button(panel, { x: speedX, y: 23, w: 40, h: 31, text: '−', onClick: () => setSpeed(speed - 0.05) });
  const speedV = button(panel, { x: speedX + 44, y: 23, w: 122, h: 31, text: '100 %', tooltip: 'Vitesse de lecture (la hauteur ne change pas). Clic : revenir à 100 %', onClick: () => setSpeed(1) });
  speedV.e.classList.add('on');
  button(panel, { x: speedX + 170, y: 23, w: 40, h: 31, text: '+', onClick: () => setSpeed(speed + 0.05) });

  // Tonalité : seulement dans la version PC (le navigateur ne sait pas la changer sans la vitesse)
  let keyV = null;
  if (engine.canTranspose) {
    centredLabel('TONALITÉ', keyX + 44, 122);
    button(panel, { x: keyX, y: 23, w: 40, h: 31, text: '−', onClick: () => setTranspose(transpose - 1) });
    keyV = button(panel, { x: keyX + 44, y: 23, w: 122, h: 31, text: '0 demi-ton', tooltip: 'Tonalité du morceau, en demi-tons (la vitesse ne change pas). Clic : revenir à 0', onClick: () => setTranspose(0) });
    keyV.e.classList.add('on');
    button(panel, { x: keyX + 170, y: 23, w: 40, h: 31, text: '+', onClick: () => setTranspose(transpose + 1) });
  }

  const wave = at(el('canvas', 'wave', panel), 16, 64, 1320, 78);
  wave.title = 'Clic : aller à cet endroit du morceau';
  wave.addEventListener('pointerdown', (ev) => {
    if (!song?.duration) return;
    const x = stagePoint(ev).x - 12 - 16;
    engine.seek(Math.min(1, Math.max(0, x / 1320)) * song.duration);
    draw();
  });

  const pos = () => engine.position;
  const sendAB = () => engine.setAB(A, B, abOn && A !== null && B !== null);
  button(panel, { x: 1346, y: 64, w: 48, h: 34, text: 'A', tooltip: 'Début de la boucle à la position actuelle', onClick: () => { A = pos(); if (B !== null && B <= A) B = null; sendAB(); draw(); } });
  button(panel, { x: 1397, y: 64, w: 48, h: 34, text: 'B', tooltip: 'Fin de la boucle à la position actuelle', onClick: () => { if (A !== null && pos() > A) { B = pos(); abOn = true; } sendAB(); draw(); } });
  button(panel, { x: 1448, y: 64, w: 48, h: 34, text: '×', tooltip: 'Effacer la boucle', onClick: () => { A = B = null; abOn = false; sendAB(); draw(); } });
  const abB = button(panel, { x: 1346, y: 104, w: 150, h: 38, text: 'BOUCLE A-B', toggle: () => abOn && A !== null && B !== null,
    tooltip: 'Jouer en boucle entre A et B', onClick: () => { if (A !== null && B !== null) abOn = !abOn; sendAB(); draw(); } });

  // Bas à gauche : les playbacks du groupe (grille de 4 colonnes)
  const pbLabel = at(el('div', 'label', panel), 16, 158, 732, 16);
  pbLabel.textContent = 'PLAYBACKS JSG — LES MORCEAUX DU GROUPE, SANS LA GUITARE';
  const cols = 4, cardW = (732 - (cols - 1) * 8) / cols, cardH = 52;
  const cards = playbacks.map((p, i) => {
    const c = at(el('div', 'card small', panel), 16 + (i % cols) * (cardW + 8), 180 + Math.floor(i / cols) * (cardH + 8), cardW, cardH);
    c.innerHTML = `<div class="play"><svg width="12" height="14"><path d="M1 1 L11 7 L1 13 Z" fill="#fff"/></svg></div><b></b>`;
    const name = p.title.replace(/^JSG - /, '').replace(/ \(sans guitare\)$/, '');
    c.querySelector('b').textContent = name;
    c.title = p.title.replace(/^JSG - /, '') + '\nClic : charger et jouer';
    c.addEventListener('click', async () => { if (await playPlayback(p)) play(); });
    return { c, p };
  });

  // Vérifie quels playbacks sont présents (requête HEAD au serveur local ou au site web)
  Promise.all(cards.map(async ({ c, p }) => {
    try {
      const r = await fetch(p.file, { method: 'HEAD', cache: 'no-store' });
      p.available = r.ok;
    } catch { p.available = undefined; }       // vérification impossible (ex. fichier local) : on laisse essayer
    if (p.available === false) {
      c.classList.add('missing');
      c.title = p.title.replace(/^JSG - /, '') + '\nPack gratuit à télécharger : cliquez pour savoir comment';
    }
  })).then(() => {
    if (cards.some(({ p }) => p.available === false))
      pbLabel.textContent = 'PLAYBACKS JSG — PACK GRATUIT À TÉLÉCHARGER (CLIQUEZ SUR UN MORCEAU)';
  });
  const rows = Math.ceil(playbacks.length / cols);
  const hintY = 180 + rows * (cardH + 8) + 2;
  at(el('div', 'hint', panel), 16, hintY, 732, 344 - hintY).innerHTML =
    'Le morceau passe directement aux haut-parleurs, à côté de la guitare. ' +
    (engine.canTranspose ? 'Vitesse et tonalité se règlent séparément. ' : 'Vitesse : la hauteur ne change pas. ') +
    'Boucle A-B : placez A et B pendant la lecture.';
  panel.lastChild.style.textAlign = 'left';

  // Bas au milieu : ses propres morceaux
  const drop = at(el('div', 'dropzone', panel), 768, 158, 352, 186);
  drop.innerHTML = 'Glissez ici un morceau<br>à accompagner<br>(WAV, MP3, FLAC…)<br>ou cliquez pour en choisir un';
  drop.addEventListener('click', () => pickFile('audio/*', (f) => loadBlob(f, f.name.replace(/\.[^.]+$/, ''))));
  dropTarget(panel, (f) => loadBlob(f, f.name.replace(/\.[^.]+$/, '')));

  // Bas à droite : mixage (morceau, guitare dessous, puis l'ensemble)
  const mixX = 1136, mixW = 360;
  function volumeSlider(labelText, y, key, initial, apply, tooltip) {
    const label = at(el('div', 'label', panel), mixX, y, mixW, 16);
    const slider = at(el('div', 'slider', panel), mixX, y + 18, mixW, 28);
    slider.title = tooltip + '\nDouble-clic : revenir à ' + Math.round(initial * 100) + ' %';
    el('div', 'track', slider); const fill = el('div', 'fill', slider); const thumb = el('div', 'thumb', slider);
    let value = +(localStorageGet(key) ?? initial);
    if (!Number.isFinite(value)) value = initial;
    const set = (v) => {
      value = Math.min(1, Math.max(0, v));
      apply(value);
      localStorageSet(key, String(value));
      fill.style.width = value * mixW + 'px'; thumb.style.left = value * mixW + 'px';
      label.innerHTML = `${labelText}<span style="float:right">${Math.round(value * 100)} %</span>`;
    };
    slider.addEventListener('pointerdown', (ev) => {
      slider.setPointerCapture(ev.pointerId);
      const move = (e) => set((stagePoint(e).x - 12 - mixX) / mixW);
      move(ev);
      slider.onpointermove = move;
      slider.onpointerup = () => (slider.onpointermove = null);
    });
    slider.addEventListener('dblclick', () => set(initial));
    set(value);
    return { set, get: () => value };
  }
  const songVolume = volumeSlider('VOLUME DU MORCEAU', 160, 'jsg.playerVolume', 0.8, (v) => engine.setVolume(v), 'Volume du morceau seul');
  const guitarVolume = volumeSlider('VOLUME DE LA GUITARE', 222, 'jsg.guitarVolume', 1, (v) => audio.setGuitarVolume(v), 'Volume de la guitare seule (après l\'ampli), pour l\'équilibrer avec le morceau');
  const masterVolume = volumeSlider('VOLUME GÉNÉRAL', 284, 'jsg.masterVolume', 1, (v) => audio.setMasterVolume(v), 'Monte ou baisse l\'ensemble : morceau + guitare');

  function setSpeed(s) {
    speed = Math.round(Math.min(1.5, Math.max(0.5, s)) * 100) / 100;
    engine.setSpeed(speed);
    speedV.e.textContent = Math.round(speed * 100) + ' %';
  }

  function setTranspose(t) {
    transpose = Math.round(Math.min(12, Math.max(-12, t)));
    engine.setTranspose(transpose);
    if (keyV) keyV.e.textContent = (transpose > 0 ? '+' : '') + transpose + (Math.abs(transpose) > 1 ? ' demi-tons' : ' demi-ton');
  }

  async function finishLoad(result, name) {
    loading = false;
    if (!result.ok) {
      modal('Lecteur de morceau', result.error || "Ce fichier n'a pas pu être lu.");
      song = null;
      draw();
      return false;
    }
    song = { title: name, peaks: result.peaks, duration: result.duration };
    A = B = null; abOn = false;
    engine.setLoop(loopSong);
    engine.setVolume(songVolume.get());
    engine.setSpeed(speed);
    engine.setTranspose(transpose);
    setTitle(name);
    draw();
    return true;
  }

  function startLoad(name) {
    if (!audioStarted) { toast("Démarrez d'abord le son (choix GUITARE / BASSE)."); return false; }
    loading = true;
    song = null;
    setTitle(name);
    draw();
    return true;
  }

  async function loadUrl(url, name) {
    if (!startLoad(name)) return false;
    return finishLoad(await engine.loadUrl(url, name), name);
  }

  async function loadBlob(blob, name) {
    if (!startLoad(name)) return false;
    return finishLoad(await engine.loadBlob(blob, name), name);
  }

  /** Dernière prise enregistrée (REC). */
  async function loadTake(take) {
    const name = take.name.replace(/\.(wav|mp3|m4a)$/i, '');
    if (take.blob) return loadBlob(take.blob, name);
    if (!startLoad(name)) return false;
    return finishLoad(await engine.loadLastTake(name), name);
  }

  function play() { if (song) engine.play(); }
  function toggle() { if (!song) return; if (engine.isPlaying()) engine.pause(); else play(); }

  const fmtTime = (t) => `${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, '0')}`;

  function draw() {
    const s = window.devicePixelRatio || 1;
    const w = 1320, h = 78;
    if (wave.width !== Math.round(w * s)) { wave.width = Math.round(w * s); wave.height = Math.round(h * s); }
    const g = wave.getContext('2d');
    g.setTransform(s, 0, 0, s, 0, 0);
    g.clearRect(0, 0, w, h);
    title.classList.toggle('empty', !song && !loading);
    if (!song && !loading) setTitle('Aucun morceau chargé');
    time.textContent = song ? `${fmtTime(pos())}  /  ${fmtTime(song.duration || 0)}` : '';
    cards.forEach(({ c, p }) => c.classList.toggle('current', song?.title === p.title));
    if (!song?.peaks) {
      g.fillStyle = colours.dim; g.font = '14px Segoe UI, Arial'; g.textAlign = 'center';
      g.fillText(loading ? 'Lecture du fichier…' : 'Ouvrez un morceau ou choisissez un playback JSG', w / 2, h / 2 + 5);
      return;
    }
    const head = song.duration ? pos() / song.duration : 0;
    if (A !== null && B !== null) {
      g.fillStyle = abOn ? 'rgba(255,166,0,0.18)' : 'rgba(255,166,0,0.08)';
      g.fillRect(A / song.duration * w, 0, (B - A) / song.duration * w, h);
    }
    for (let i = 0; i < w; i++) {
      const v = Math.max(0.6, song.peaks[i] * h * 0.46);
      g.fillStyle = i / w < head ? '#2f8fe8' : '#3a4a5e';
      g.fillRect(i, h / 2 - v, 1, v * 2);
    }
    for (const [t, label] of [[A, 'A'], [B, 'B']]) {
      if (t === null) continue;
      const x = t / song.duration * w;
      g.fillStyle = '#ffa600'; g.fillRect(x - 1, 0, 2, h);
      g.font = 'bold 11px Segoe UI, Arial'; g.fillText(label, x + 3, 11);
    }
    g.fillStyle = '#fff'; g.fillRect(head * w - 1, 0, 2, h);
  }

  function tick() {
    if (!song) return;
    engine.tick();
    if (page === 2) { draw(); playB.update(); abB.update(); }
  }

  engine.on('state', () => { playB.update(); draw(); });

  window.addEventListener('keydown', (ev) => {
    if (ev.code === 'Space' && page === 2 && !ev.target.closest('input, select, textarea')) { ev.preventDefault(); toggle(); }
  });

  draw();
  return { draw, tick, loadBlob, loadTake, isPlaying: () => engine.isPlaying(), applySettings: () => { songVolume.set(songVolume.get()); guitarVolume.set(guitarVolume.get()); masterVolume.set(masterVolume.get()); setSpeed(speed); setTranspose(transpose); engine.setLoop(loopSong); } };
})();

//==============================================================================
// Page 4 : les rythmes (version PC)
{
  const panel = el('div', 'bigpanel', pages[3]);
  const msg = place(el('div', 'hint', panel), 0, 130, 1512, 100);
  msg.innerHTML = '<b style="font-size:20px;color:#dde1e8">Boîte à rythmes</b><br><br>' +
    (desktop ? 'La boîte à rythmes et le séquenceur de basse arrivent dans une prochaine version.<br>' : 'La boîte à rythmes et le séquenceur de basse sont dans la version PC de JSG Guitar.<br>') +
    'En attendant, la page 3 (LECTEUR) joue les playbacks du groupe.';
  msg.style.fontSize = '15px';
}

//==============================================================================
// Panneaux des onglets (AMPLI, CAB, FX, EQ, TOOLS), sur la façade de la tête d'ampli
const panelArea = [452, 184, 728, 234], wideArea = [268, 180, 1000, 242];

function makePanel(title, area, subtitle) {
  const [x, y, w, h] = area;
  const e = place(el('div', 'panel hidden', controls), x, y, w, h);
  const t = el('div', 'title', e);
  t.textContent = title;
  if (subtitle) el('small', '', t).textContent = subtitle;
  el('div', 'rule', e);
  button(e, { x: w - 38, y: 6, w: 26, h: 24, text: '×', onClick: () => selectTab(0) }).e.classList.add('close');
  return e;
}

const group = (parent, x, y, w, h, name) => {
  const g = place(el('div', 'group', parent), x, y, w, h);
  if (name) el('div', 'gtitle', g).textContent = name;
  return g;
};

const boundKnob = (parent, id, box, title, tooltip) => {
  const [x, y, w, h] = box;
  const k = drawnKnob(parent, { x, y, w, h, title, get: () => S.getPos(id), set: (p) => S.setPos(id, p), reset: () => S.reset(id), format: () => fmt(id), tooltip });
  S.onRefresh(k.update);
  return k;
};

const panels = [];

// --- AMPLI ---
{
  const e = makePanel('AMPLI', panelArea);
  group(e, 16, 44, 410, 184, 'CANAL');
  const levels = group(e, 438, 44, 274, 184, 'NIVEAUX');
  const names = ['CLAIR', 'CRUNCH', 'LEAD'];
  const chans = names.map((n, i) => button(e, { x: 32 + i * 129.3, y: 74, w: 119.3, h: 44, text: n,
    toggle: () => Math.round(S.get('channel')) === i, onClick: () => S.set('channel', i) }));
  const desc = place(el('div', 'hint', e), 30, 128, 382, 48);
  const bright = button(e, { x: 156, y: 188, w: 130, h: 34, text: 'BRIGHT', toggle: () => S.isOn('bright'), onClick: () => S.set('bright', S.isOn('bright') ? 0 : 1) });
  const input = boundKnob(e, 'input', [450, 74, 125, 148], 'ENTRÉE');
  const output = boundKnob(e, 'output', [575, 74, 125, 148], 'SORTIE');
  const di = boundKnob(e, 'diBlend', [617, 74, 83, 148], 'DI',
    'Basse : part du son direct (DI) mélangé à l\'ampli. 0 = tout l\'ampli, 10 = tout le DI. Le DI garde les graves quand l\'ampli sature.');
  const guitarText = ['Clair : un seul étage, reste propre et craque en poussant le gain.', 'Crunch : deux étages, rythmiques rock et blues.', 'Lead : trois étages, graves resserrés, solos et gros son.'];
  const bassText = ['Basse clair : rond et précis, garde toute la dynamique du jeu.', 'Basse crunch : grain rock, médiums en avant.', 'Basse saturée : grosse saturation ; montez le DI pour garder les graves.'];
  const update = () => {
    const bass = S.isBassMode();
    chans.forEach((c) => c.update());
    chans[2].e.textContent = bass ? 'SATURÉ' : 'LEAD';
    bright.update();
    desc.textContent = (bass ? bassText : guitarText)[Math.round(S.get('channel'))];
    levels.querySelector('.gtitle').textContent = bass ? 'NIVEAUX ET DI' : 'NIVEAUX';
    di.e.classList.toggle('hidden', !bass);
    place(input.e, 450, 74, bass ? 83 : 125, 148);
    place(output.e, bass ? 533 : 575, 74, bass ? 84 : 125, 148);
  };
  S.onRefresh(update);
  panels.push({ e, update });
}

// --- CAB ---
{
  const e = makePanel('CAB — BAFFLE, MICRO, ROOM', wideArea);
  group(e, 16, 44, 252, 192, 'BAFFLE ET MICRO');
  group(e, 278, 44, 232, 192, 'ROOM');
  group(e, 520, 44, 200, 192, 'FILTRES');
  group(e, 730, 44, 254, 192, 'RÉPONSE IMPULSIONNELLE');
  const lbl = (text, x, y) => { const l = place(el('div', 'label', e), x, y, 196, 16); l.textContent = text; l.style.fontSize = '11px'; };
  lbl('BAFFLE', 62, 72); lbl('MICRO', 62, 134);

  const sel = (box, idFn, names, after) => {
    const [x, y, w, h] = box;
    return selector(e, { x, y, w, h, arrow: 26, fontSize: 14.5, framed: true,
      getText: () => names()[Math.round(S.get(idFn()))],
      onStep: (d) => { const n = names().length; S.set(idFn(), (Math.round(S.get(idFn())) + d + n) % n); after?.(); },
      onClick: (a) => showMenu(names().map((name, i) => ({ text: name, checked: i === Math.round(S.get(idFn())), onClick: () => { S.set(idFn(), i); after?.(); } })), a) });
  };
  const cabSel = sel([60, 90, 198, 32], () => (S.isBassMode() ? 'bassCab' : 'cabType'), () => (S.isBassMode() ? bassCabinets : cabinets), cabTouched);
  const micSel = sel([60, 152, 198, 32], () => 'micType', () => mics, cabTouched);
  const roomSel = sel([322, 76, 178, 32], () => 'roomType', () => rooms);
  const irSel = selector(e, { x: 774, y: 76, w: 200, h: 32, arrow: 26, fontSize: 14, framed: true,
    getText: () => impulse.current?.name || 'Aucune IR', onStep: stepImpulse, onClick: () => pickFile('audio/*,.wav,.aif,.aiff,.flac', loadImpulseFile) });

  const powers = [['cabOn', 43, 106], ['micOn', 43, 168], ['roomOn', 305, 92], ['irOn', 757, 92]].map(([id, x, y]) =>
    powerIcon(e, { x, y, get: () => S.isOn(id), set: (v) => { S.set(id, v ? 1 : 0); if (id === 'cabOn' || id === 'micOn') cabTouched(); } }));

  boundKnob(e, 'roomLevel', [339, 110, 110, 122], 'NIVEAU');
  boundKnob(e, 'lowCut', [526, 76, 94, 152], 'COUPE-BAS');
  boundKnob(e, 'highCut', [620, 76, 94, 152], 'COUPE-HAUT');

  const irBox = place(el('canvas', 'irdisplay', e), 740, 114, 234, 52);
  irBox.addEventListener('click', () => pickFile('audio/*,.wav,.aif,.aiff,.flac', loadImpulseFile));
  dropTarget(irBox, loadImpulseFile);
  const hint = place(el('div', 'hint', e), 740, 170, 234, 28);
  hint.textContent = "Allumée, l'IR remplace le baffle et le micro.";
  hint.style.fontSize = '11.5px';
  button(e, { x: 760, y: 200, w: 194, h: 30, text: 'CHARGER UNE IR…', onClick: () => pickFile('audio/*,.wav,.aif,.aiff,.flac', loadImpulseFile) });

  const update = () => {
    const irActive = S.isOn('irOn') && !!impulse.current;
    [cabSel, micSel, roomSel, irSel].forEach((s) => s.update());
    powers.forEach((p) => p.update());
    cabSel.setDimmed(!S.isOn('cabOn') || irActive);
    micSel.setDimmed(!S.isOn('micOn') || irActive);
    roomSel.setDimmed(!S.isOn('roomOn'));
    irSel.setDimmed(!irActive);
    drawImpulse(irBox, 234, 52, true, irActive);
  };
  S.onRefresh(update);
  panels.push({ e, update });
}

// --- FX : le catalogue des pédales ---
{
  const e = makePanel('FX — PÉDALES', panelArea, 'Clic : ajouter au pédalier.');
  const cats = [...new Set(pedals.map((p) => p.category))];
  cats.forEach((cat, row) => {
    const y = 48 + row * 38;
    const l = place(el('div', 'catlabel', e), 18, y + 7, 90, 14);
    l.textContent = cat.toUpperCase();
    pedals.filter((p) => p.category === cat).forEach((p, i) => {
      const chip = place(el('div', 'chip', e), 108 + i * 152, y, 144, 26);
      chip.textContent = p.name;
      chip.style.background = `linear-gradient(${shade(p.colour, 0.1)}, ${shade(p.colour, -0.25)})`;
      chip.title = p.description;
      chip.addEventListener('click', () => addPedalFromCatalogue(p.id));
    });
  });
  const note = place(el('div', 'hint', e), 16, 202, 470, 24);
  note.style.textAlign = 'left'; note.style.fontSize = '12px';
  note.textContent = desktop ? 'Plug-ins : bientôt (format CLAP).' : 'Pédales .jsgpedal et plug-ins VST3 : dans la version PC.';
  button(e, { x: 502, y: 196, w: 210, h: 30, text: 'VIDER LE PÉDALIER', onClick: () => S.clearAllSlots() });
  panels.push({ e });
}

// --- EQ ---
{
  const e = makePanel('EQ — 5 BANDES', panelArea);
  const labels = ['100 Hz', '250 Hz', '800 Hz', '2,5 kHz', '6 kHz'];
  const scale = place(el('div', 'abs', e), 16, 48, 40, 146);
  scale.innerHTML = '<div class="label" style="right:4px;top:0;font-weight:400;font-size:11px">+12</div><div class="label" style="right:4px;top:66px;font-weight:400;font-size:11px">0</div><div class="label" style="right:4px;bottom:0;font-weight:400;font-size:11px">-12</div>';
  const sliders = labels.map((label, i) => {
    const id = 'eq' + (i + 1);
    const x = 56 + i * 105 + 30.5, y = 44, h = 150;
    const c = place(el('canvas', 'eqslider', e), x, y, 44, h);
    const lab = place(el('div', 'label', e), x - 20, y + h, 84, 16);
    lab.textContent = label; lab.style.textAlign = 'center'; lab.style.color = colours.text; lab.style.fontSize = '12.5px';
    const val = place(el('div', 'label', e), x - 20, y + h + 15, 84, 15);
    val.style.textAlign = 'center'; val.style.fontWeight = '400'; val.style.fontSize = '12px';
    const draw = () => {
      const s = window.devicePixelRatio || 1;
      if (c.width !== 44 * s) { c.width = 44 * s; c.height = h * s; }
      const g = c.getContext('2d'); g.setTransform(s, 0, 0, s, 0, 0); g.clearRect(0, 0, 44, h);
      const top = 8, bottom = h - 8, cx = 22, map = (db) => bottom + (top - bottom) * (db + 12) / 24;
      g.fillStyle = '#2a323e';
      for (let m = -12; m <= 12; m += 6) { const w = m === 0 ? 20 : 12; g.fillRect(cx - w, Math.round(map(m)), w * 2, 1); }
      g.fillStyle = '#232b36'; roundRect(g, cx - 3, top, 6, bottom - top, 3);
      g.fillStyle = '#04060a'; roundRect(g, cx - 2, top + 1, 4, bottom - top - 2, 2);
      const v = S.get(id), yv = map(v), y0 = map(0);
      g.fillStyle = S.isOn('eqOn') ? 'rgba(47,155,255,0.9)' : 'rgba(47,155,255,0.3)';
      roundRect(g, cx - 3, Math.min(yv, y0), 6, Math.abs(yv - y0), 3);
      const grad = g.createLinearGradient(0, yv - 6.5, 0, yv + 6.5);
      grad.addColorStop(0, '#e6ebf2'); grad.addColorStop(1, '#6e7682');
      g.fillStyle = grad; roundRect(g, cx - 15, yv - 6.5, 30, 13, 3);
      g.fillStyle = '#0b0e13'; g.fillRect(cx - 11, yv - 0.8, 22, 1.6);
      val.textContent = (v > 0 ? '+' : '') + v.toFixed(1).replace('.', ',') + ' dB';
    };
    makeDraggable(c, { get: () => S.getPos(id), set: (p) => S.setPos(id, p), reset: () => S.reset(id), sensitivity: 150 });
    return draw;
  });
  const power = button(e, { x: 592, y: 74, w: 110, h: 38, text: 'ACTIVÉ', toggle: () => S.isOn('eqOn'), onClick: () => S.set('eqOn', S.isOn('eqOn') ? 0 : 1) });
  button(e, { x: 592, y: 124, w: 110, h: 38, text: 'À PLAT', onClick: () => [1, 2, 3, 4, 5].forEach((i) => S.set('eq' + i, 0)) });
  const update = () => { sliders.forEach((d) => d()); power.update(); };
  S.onRefresh(update);
  panels.push({ e, update });
}

function roundRect(g, x, y, w, h, r) {
  if (h <= 0 || w <= 0) return;
  r = Math.min(r, w / 2, h / 2);
  g.beginPath(); g.roundRect ? g.roundRect(x, y, w, h, r) : g.rect(x, y, w, h); g.fill();
}

// --- TOOLS : accordeur ---
const tuner = { history: new Float32Array(16384), result: { valid: false }, cents: 0, misses: 0, timer: null };
{
  const e = makePanel('TOOLS — ACCORDEUR', panelArea);
  group(e, 16, 44, 536, 184);
  const c = place(el('canvas', 'abs', e), 16, 44, 536, 184);
  c.style.pointerEvents = 'none';
  const mute = button(e, { x: 567, y: 58, w: 140, h: 44, text: 'COUPER LE SON', toggle: () => S.isOn('mute'), onClick: () => S.set('mute', S.isOn('mute') ? 0 : 1) });
  const strings = button(e, { x: 567, y: 128, w: 140, h: 34, text: '4 CORDES',
    tooltip: 'Basse : 4 cordes (Mi La Ré Sol), 5 cordes (Si grave en plus) ou 6 cordes (Si grave et Do aigu)',
    onClick: () => S.set('bassStrings', (Math.round(S.get('bassStrings')) + 1) % 3) });
  button(e, { x: 567, y: 184, w: 140, h: 34, text: 'RÉGLAGES AUDIO', onClick: showAudioSettings });
  tuner.canvas = c;
  const update = () => {
    mute.update();
    strings.e.classList.toggle('hidden', !S.isBassMode());
    strings.e.textContent = `${4 + Math.round(S.get('bassStrings'))} CORDES`;
    drawTuner();
  };
  S.onRefresh(update);
  panels.push({ e, update });
}

function tuning() {
  if (!S.isBassMode()) return [40, 45, 50, 55, 59, 64];
  return [[28, 33, 38, 43], [23, 28, 33, 38, 43], [23, 28, 33, 38, 43, 48]][Math.round(S.get('bassStrings'))];
}

function tunerStart() {
  if (!audioStarted || tuner.timer) return;
  tuner.history.fill(0);
  tuner.result = { valid: false };
  audio.setTuner(true);
  tuner.timer = setInterval(() => {
    const rate = audio.sampleRate, bass = S.isBassMode();
    const used = Math.min(tuner.history.length, (bass ? 8192 : 4096) * Math.max(1, Math.round(rate / 48000)));
    const detected = detectPitch(tuner.history.subarray(tuner.history.length - used), rate, 440, bass ? 25 : 55);
    if (detected.valid) {
      tuner.cents = tuner.result.valid && tuner.result.midiNote === detected.midiNote ? tuner.cents + (detected.cents - tuner.cents) * 0.5 : detected.cents;
      tuner.misses = 0;
      tuner.result = detected;
    } else if (++tuner.misses > 4) tuner.result = { valid: false };
    drawTuner();
  }, 100);
}

function tunerStop() {
  if (!tuner.timer) return;
  clearInterval(tuner.timer);
  tuner.timer = null;
  audio.setTuner(false);
}

audio.on('tuner', (m) => {
  const h = tuner.history, n = m.data.length;
  h.copyWithin(0, n);
  h.set(m.data, h.length - n);
});

function drawTuner() {
  const c = tuner.canvas;
  if (!c || selectedTab !== 5) return;
  const s = window.devicePixelRatio || 1, w = 536, h = 184;
  if (c.width !== Math.round(w * s)) { c.width = Math.round(w * s); c.height = Math.round(h * s); }
  const g = c.getContext('2d');
  g.setTransform(s, 0, 0, s, 0, 0);
  g.clearRect(0, 0, w, h);
  const r = tuner.result;
  const font = (px, bold) => `${bold ? 'bold ' : ''}${px}px Segoe UI, Helvetica Neue, Arial, sans-serif`;

  // Note
  g.textAlign = 'center'; g.textBaseline = 'middle';
  g.fillStyle = r.valid ? colours.text : colours.off;
  g.font = font(46, true);
  g.fillText(r.valid ? noteName(r.midiNote) : '--', 16 + 95, 10 + 60);
  g.font = font(14); g.fillStyle = colours.dim;
  g.fillText(r.valid ? `${noteLetter(r.midiNote)}   ${r.frequency.toFixed(1).replace('.', ',')} Hz` : 'Jouez une corde à vide', 16 + 95, 10 + 164 - 34);

  // Cordes
  const t = tuning();
  let nearest = -1;
  if (r.valid) {
    const played = r.midiNote + r.cents / 100;
    let best = 3;
    t.forEach((m, i) => { if (Math.abs(played - m) < best) { best = Math.abs(played - m); nearest = i; } });
  }
  const rowX = 216, rowY = 156, rowW = 304, cw = rowW / t.length;
  t.forEach((m, i) => {
    const x = rowX + i * cw + 3, hit = i === nearest;
    g.fillStyle = hit ? 'rgba(47,155,255,0.35)' : '#0b0e13';
    roundRect(g, x, rowY + 1, cw - 6, 20, 5);
    g.strokeStyle = hit ? colours.blue : '#2b3340'; g.lineWidth = 1;
    g.beginPath(); g.roundRect ? g.roundRect(x, rowY + 1, cw - 6, 20, 5) : g.rect(x, rowY + 1, cw - 6, 20); g.stroke();
    g.fillStyle = hit ? '#fff' : colours.dim; g.font = font(12.5, hit);
    g.fillText(noteName(m), x + (cw - 6) / 2, rowY + 11.5);
  });

  // Échelle en cents
  const mx = 216 + 152, y = 10 + 67 - 6, half = 152 - 12;
  g.fillStyle = '#05070a'; roundRect(g, mx - half, y - 7, half * 2, 14, 7);
  g.fillStyle = '#18402a'; roundRect(g, mx - half * 0.1, y - 7, half * 0.2, 14, 4);
  g.font = font(11.5);
  for (let cts = -50; cts <= 50; cts += 10) {
    const x = mx + half * cts / 50;
    g.fillStyle = cts === 0 ? colours.text : 'rgba(125,131,141,0.7)';
    const big = cts % 50 === 0;
    g.fillRect(x - 0.5, y - (big ? 14 : 11), 1, big ? 28 : 22);
    if (cts % 25 === 0 || cts === 0) g.fillText(String(cts), x, y + 26);
  }
  if (r.valid) {
    const inTune = Math.abs(tuner.cents) < 5;
    const x = mx + half * Math.max(-50, Math.min(50, tuner.cents)) / 50;
    const col = inTune ? '#3ddc84' : colours.blue;
    const glow = g.createRadialGradient(x, y, 0, x, y, 18);
    glow.addColorStop(0, inTune ? 'rgba(61,220,132,0.5)' : 'rgba(47,155,255,0.5)'); glow.addColorStop(1, 'rgba(0,0,0,0)');
    g.fillStyle = glow; g.fillRect(x - 18, y - 18, 36, 36);
    g.fillStyle = col; roundRect(g, x - 3, y - 16, 6, 32, 3);
    g.font = font(14, true);
    g.fillText(inTune ? 'JUSTE' : tuner.cents < 0 ? 'TROP BAS — monter' : 'TROP HAUT — descendre', mx, 10 + 134 - 11);
  }
}

//==============================================================================
// Réglages audio : entrée de la carte son, canal, latence
async function showAudioSettings() {
  if (plugin) {
    const st = audio.lastState || {};
    const box = el('div');
    el('p', '', box).textContent = "Dans le plug-in, la carte son, le tampon et la fréquence se règlent dans votre logiciel musical (Reaper, Cubase, Studio One, FL Studio…).";
    el('p', 'note', box).textContent = `Fréquence actuelle : ${st.sampleRate || '?'} Hz. JSG Guitar ajoute ${st.latency || 32} échantillons de latence, que votre logiciel compense automatiquement à la lecture. ` +
      "Pour jouer en direct, réglez un petit tampon (64 ou 128) dans votre logiciel et activez le monitoring de la piste.";
    const row = el('div', 'row', box);
    el('label', '', row).textContent = 'Canal de la guitare';
    const chan = el('select', '', row);
    ['Entrée 1 (gauche)', 'Entrée 2 (droite)', 'Les deux mélangées'].forEach((t, i) => { const o = el('option', '', chan); o.value = i; o.textContent = t; });
    chan.value = String(st.channel ?? 0);
    chan.addEventListener('change', () => { audio.send({ t: 'chan', channel: +chan.value }); if (audio.lastState) audio.lastState.channel = +chan.value; });
    modal('Réglages audio', box);
    return;
  }
  if (desktop) return showDesktopAudioSettings();
  const box = el('div');
  if (!audioStarted) {
    el('p', '', box).textContent = "Le son n'est pas encore démarré : choisissez GUITARE ou BASSE dans la fenêtre de démarrage.";
    modal('Réglages audio', box);
    return;
  }
  const inputs = await audio.listInputs();
  const row1 = el('div', 'row', box);
  el('label', '', row1).textContent = 'Entrée';
  const dev = el('select', '', row1);
  const opt = (value, text) => { const o = el('option', '', dev); o.value = value; o.textContent = text; };
  opt('', 'Entrée par défaut du système');
  inputs.forEach((d, i) => opt(d.deviceId, d.label || `Entrée ${i + 1}`));
  opt('none', 'Aucune (lecteur de morceau seulement)');
  dev.value = audio.deviceId;

  const row2 = el('div', 'row', box);
  el('label', '', row2).textContent = 'Canal de la guitare';
  const chan = el('select', '', row2);
  ['Entrée 1 (gauche)', 'Entrée 2 (droite)', 'Les deux mélangées'].forEach((t, i) => { const o = el('option', '', chan); o.value = i; o.textContent = t; });
  chan.value = String(audio.inputChannel);
  chan.addEventListener('change', () => audio.setInputChannel(+chan.value));

  const info = el('p', 'note', box);
  const showInfo = () => {
    info.innerHTML = `Fréquence : ${audio.sampleRate} Hz — latence estimée : <b>${Math.round(audio.latencyMs())} ms</b> (entrée + sortie).<br>` +
      (audio.inputError ? `<span style="color:#ff8a80">Entrée indisponible : ${audio.inputError.message || audio.inputError.name}</span><br>` : '') +
      "Pour moins de latence : branchez la guitare dans une carte son USB, et choisissez-la aussi comme sortie dans Windows (Paramètres > Son). " +
      "Le navigateur ne peut pas utiliser ASIO : la version PC reste la plus réactive.";
  };
  showInfo();
  dev.addEventListener('change', async () => { await audio.openInput(dev.value); showInfo(); });
  el('p', 'note', box).textContent = "Utilisez un casque ou des enceintes éloignées : avec le micro de l'ordinateur, le son peut boucler (larsen).";
  modal('Réglages audio', box);
}

const escapeHtml = (t) => String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

/** Version PC : carte son, mode exclusif, taille de tampon, fréquence, canal — et la latence mesurée. */
async function showDesktopAudioSettings() {
  const box = el('div');
  el('p', 'note', box).textContent = 'Recherche des cartes son…';
  const dlg = modal('Réglages audio', box, [{ text: 'Fermer', primary: true }]);
  const list = await audio.devices();
  box.innerHTML = '';

  const cfg = { inputId: '', outputId: '', exclusive: true, bufferFrames: 128, sampleRate: 48000, inputChannel: 0, ...(audio.status?.config || {}) };
  const select = (label, options, value, onChange) => {
    const row = el('div', 'row', box);
    el('label', '', row).textContent = label;
    const s = el('select', '', row);
    for (const [v, t] of options) { const o = el('option', '', s); o.value = String(v); o.textContent = t; }
    s.value = String(value);
    if (s.selectedIndex < 0) s.selectedIndex = 0;
    s.addEventListener('change', () => onChange(s.value));
    return s;
  };
  const named = (devices) => devices.map((d) => [d.id, d.name + (d.default ? '  (par défaut)' : '')]);

  select('Entrée (guitare)', [['', `Entrée par défaut de ${OS}`], ...named(list.inputs), ['none', 'Aucune (lecteur de morceau seulement)']], cfg.inputId, (v) => apply({ inputId: v }));
  select('Sortie', [['', `Sortie par défaut de ${OS}`], ...named(list.outputs)], cfg.outputId, (v) => apply({ outputId: v }));
  select('Mode', macApp
    ? [['1', 'Fréquence choisie ici (ci-dessous)'], ['0', 'Fréquence de Configuration audio et MIDI']]
    : [['1', 'Exclusif — la plus faible latence'], ['0', 'Partagé — les autres logiciels gardent le son']], cfg.exclusive ? '1' : '0', (v) => apply({ exclusive: v === '1' }));
  select('Tampon', [32, 48, 64, 96, 128, 256, 512].map((n) => [n, `${n} échantillons`]), cfg.bufferFrames, (v) => apply({ bufferFrames: +v }));
  select('Fréquence', [44100, 48000, 88200, 96000].map((n) => [n, `${n} Hz`]), cfg.sampleRate, (v) => apply({ sampleRate: +v }));
  select('Canal de la guitare', [[0, 'Entrée 1 (gauche)'], [1, 'Entrée 2 (droite)'], [2, 'Les deux mélangées']], cfg.inputChannel, (v) => apply({ inputChannel: +v }));

  const info = el('p', 'note', box);
  const row = el('div', 'buttons', box);
  row.style.justifyContent = 'flex-start';
  const releaseB = el('button', 'btn', row);
  const measureB = el('button', 'btn', row);
  measureB.textContent = 'Mesurer la latence réelle…';
  measureB.addEventListener('click', () => measureRealLatency(showInfo));
  const folderB = el('button', 'btn', row);
  folderB.textContent = 'Dossier des enregistrements';
  folderB.addEventListener('click', () => audio.openRecordings());
  el('p', 'note', box).textContent = macApp
    ? "Sur Mac, les autres logiciels gardent le son : le tampon ne concerne que JSG Guitar. Pour la plus faible latence, " +
      "utilisez la même carte son (USB) pour l'entrée et la sortie. Si le son craque, augmentez le tampon."
    : "Mode exclusif : JSG Guitar prend la carte son pour lui seul (les autres logiciels sont muets pendant ce temps). " +
      "Si le son craque, augmentez le tampon. « Libérer la carte son » la rend aux autres logiciels sans fermer JSG Guitar.";

  let busy = false;
  async function apply(change) {
    if (busy) return;
    busy = true;
    Object.assign(cfg, change);
    info.innerHTML = 'Redémarrage du son…';
    box.querySelectorAll('select, button').forEach((e) => (e.disabled = true));
    await audio.configure({ ...cfg });
    box.querySelectorAll('select, button').forEach((e) => (e.disabled = false));
    busy = false;
    lastMeasure = null;
    showInfo();
    setTimeout(async () => { await audio.refreshStatus(); showInfo(); }, 2500);
  }

  function showInfo() {
    const s = audio.status || {};
    if (s.running) {
      info.innerHTML =
        `<b>${s.exclusive ? 'Mode exclusif' : 'Mode partagé'}</b> — ${s.sampleRate} Hz, ${s.blockSize} échantillons (${escapeHtml(s.format || '')})<br>` +
        `Entrée : ${escapeHtml(s.inputName || 'aucune')} — Sortie : ${escapeHtml(s.outputName || '')}<br>` +
        `Latence : entrée ${s.inputLatencyMs.toFixed(1)} ms + sortie ${s.outputLatencyMs.toFixed(1)} ms` +
        (s.engineLatency ? ` + moteur ${(1000 * s.engineLatency / s.sampleRate).toFixed(1)} ms` : '') +
        ` = <b>${audio.latencyMs().toFixed(1)} ms</b> (annoncée par ${OS})` +
        (s.inputQueueMs > 0.05 ? `<br>Entrée en attente : ${s.inputQueueMs.toFixed(1)} ms` : '') +
        (s.glitches ? ` — l'entrée a pris du retard ${s.glitches} fois (augmentez le tampon si ça craque)` : '') +
        (lastMeasure ? `<br>Latence réelle mesurée : <b>${lastMeasure.ms.toFixed(1)} ms</b> aller-retour (câble sortie → entrée)` : '') +
        (s.note ? `<br><span style="color:#ffb74d">${escapeHtml(s.note)}</span>` : '');
    } else {
      info.innerHTML = `<span style="color:#ff8a80">Son arrêté : ${escapeHtml(s.error || 'carte son indisponible')}</span>`;
    }
    releaseB.textContent = s.running ? 'Libérer la carte son' : 'Reprendre la carte son';
  }

  releaseB.addEventListener('click', async () => {
    releaseB.disabled = true;
    if (audio.status?.running) await audio.release(); else await audio.resumeAudio();
    releaseB.disabled = false;
    showInfo();
  });

  showInfo();
  setTimeout(async () => { await audio.refreshStatus(); showInfo(); }, 2500);   // file d'entrée réglée après 2 s
  return dlg;
}

let lastMeasure = null;

/** Version PC : mesure de la latence réelle avec un câble entre une sortie et l'entrée de la carte son. */
function measureRealLatency(done) {
  const box = el('div');
  el('p', '', box).textContent = "Reliez une sortie de la carte son (sortie ligne ou casque) à l'entrée guitare avec un câble jack. " +
    "Baissez le casque et les enceintes : JSG Guitar va envoyer 10 petits clics.";
  el('p', 'note', box).textContent = "La mesure compte tout : pilote, convertisseurs de la carte son et moteur de JSG Guitar. C'est le délai entre le moment où vous jouez et celui où vous entendez.";
  const result = el('p', '', box);
  let running = false;
  const dlg = modal('Latence réelle', box, [
    { text: 'Mesurer', primary: true, onClick: () => {
      if (running) return false;
      running = true;
      result.textContent = 'Mesure en cours… (5 secondes)';
      audio.measureLatency().then((r) => {
        running = false;
        if (r.ok) {
          lastMeasure = r;
          result.innerHTML = `Latence réelle : <b>${r.ms.toFixed(1)} ms</b> aller-retour (${r.samples} échantillons, ${r.heard}/10 clics reçus` +
            (r.spread > 0.5 ? `, écart ${r.spread.toFixed(1)} ms` : '') + ').<br>' +
            (r.ms <= 10 ? 'Excellent : imperceptible en jouant.' : r.ms <= 15 ? 'Bon : la plupart des guitaristes ne le sentent pas.' :
             r.ms <= 25 ? 'Correct, mais perceptible : essayez un tampon plus petit ou le mode exclusif.' : (macApp ? 'Élevé : tampon plus petit, ou une carte son USB pour l\'entrée et la sortie.' : 'Élevé : mode exclusif, tampon plus petit, ou pilote ASIO de la carte son.'));
        } else {
          result.textContent = r.error ? 'Mesure impossible : ' + r.error + '.' :
            `Les clics ne reviennent pas (${r.heard || 0}/10 reçus) : vérifiez le câble, le canal d'entrée choisi et le volume de la sortie.`;
        }
        done?.();
      });
      return false;
    } },
    { text: 'Fermer', onClick: () => { if (running) audio.send({ t: 'measureCancel' }); } },
  ]);
  return dlg;
}

/** Version PC : explique pourquoi le son n'a pas démarré, ou signale un repli (mode partagé…). */
function showAudioProblem(status) {
  if (!status) return;
  if (!status.running) {
    const box = el('div');
    el('p', '', box).textContent = 'La carte son n\'a pas pu être ouverte : ' + (status.error || 'raison inconnue') + '.';
    el('p', 'note', box).textContent = "Vérifiez qu'elle est branchée, fermez les logiciels qui l'utilisent en mode exclusif (DAW, autre ampli virtuel), puis choisissez-la dans les réglages audio.";
    modal('Le son ne démarre pas', box, [{ text: 'Réglages audio…', primary: true, onClick: () => { setTimeout(showAudioSettings, 0); } }, { text: 'Plus tard' }]);
  } else if (status.note) {
    toast(status.note, 8000);
  }
}

if (!desktop) audio.on('input', (r) => {
  if (!r.ok) {
    const e = r.error || {};
    const why = e.name === 'NotAllowedError' ? "L'accès à l'entrée audio a été refusé. Autorisez-le dans la barre d'adresse (icône du micro), puis rechargez la page."
      : e.name === 'NotFoundError' ? "Aucune entrée audio trouvée. Branchez la carte son, puis choisissez-la dans les réglages audio."
      : "L'entrée audio n'a pas pu être ouverte : " + (e.message || e.name);
    modal('Entrée audio', why + '\n\nLe lecteur de morceau fonctionne quand même.');
  }
});

//==============================================================================
// Dessin animé : barres de niveau, aiguilles des vu-mètres, lampes, POWER, IR
const meters = { inPeak: 0, outPeak: 0, vuL: 0, vuR: 0 };
audio.on('m', (m) => {
  meters.inPeak = Math.max(meters.inPeak, m.inPeak);
  meters.outPeak = Math.max(meters.outPeak, m.outPeak);
  meters.vuL = m.vuL; meters.vuR = m.vuR;
});

const meterState = (segments) => ({ level: 0, clip: 0, segments });
const ampInput = meterState(20), rackIn = meterState(16), rackOut = meterState(16);
const needles = [-51, -51];
const tubes = [[153, 322, 62, 96], [306, 340, 56, 84], [1230, 342, 56, 84], [1382, 322, 62, 96], [232, 362, 30, 46], [392, 368, 30, 46], [1302, 364, 30, 46]];
const tubeLevels = tubes.map(() => 0);
let tubeDrive = 0;

function pushMeter(m, peak) {
  const db = peak > 0 ? Math.max(-60, 20 * Math.log10(peak)) : -60;
  const target = Math.min(1, Math.max(0, (db + 54) / 54));
  m.level = target > m.level ? target : Math.max(target, m.level - 0.035);
  if (peak >= 1) m.clip = 40; else if (m.clip > 0) m.clip--;
}

const vuAngle = (vu) => {
  const table = [[-40, -51], [-20, -48.5], [-10, -40], [-7, -34], [-5, -26], [-3, -13], [-2, 0], [-1, 15], [0, 23.5], [1, 29], [2, 38], [3, 45.5], [6, 50]];
  if (vu <= table[0][0]) return table[0][1];
  for (let i = 1; i < table.length; i++)
    if (vu <= table[i][0]) return table[i - 1][1] + (table[i][1] - table[i - 1][1]) * (vu - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
  return table[table.length - 1][1];
};
const toDb = (x, floor) => (x > 0 ? Math.max(floor, 20 * Math.log10(x)) : floor);

function tick30() {
  pushMeter(ampInput, meters.inPeak);
  pushMeter(rackIn, meters.inPeak);
  pushMeter(rackOut, meters.outPeak);
  meters.inPeak = meters.outPeak = 0;
  [meters.vuL, meters.vuR].forEach((rms, i) => { needles[i] += (vuAngle(toDb(rms, -80) + 14) - needles[i]) * 0.32; });

  const powered = S.isOn('power');
  const vu = toDb(powered ? Math.max(meters.vuL, meters.vuR) : 0, -80) + 14;
  const target = Math.min(1, Math.max(0, (vu + 0.3) / 3.3));
  tubeDrive += (target - tubeDrive) * (target > tubeDrive ? 0.5 : 0.18);
  if (tubeDrive < 0.004) tubeDrive = 0;
  tubeLevels.forEach((l, i) => { const want = tubeDrive * (0.5 + 0.5 * Math.random()); const next = l + (want - l) * 0.55; tubeLevels[i] = next < 0.002 ? 0 : next; });

  pageButtons[0].querySelector('i').classList.toggle('lit', anyRackEffectOn());
  pageButtons[1].querySelector('i').classList.toggle('lit', [...Array(numSlots).keys()].some((s) => S.slotTypes[s] >= 0 && S.slotOn(s)));
  pageButtons[2].querySelector('i').classList.toggle('lit', player.isPlaying());
  if (recording) updateRecorder();
  player.tick();
}

function drawSegments(g, x, y, w, h, vertical, m) {
  g.fillStyle = '#04060a'; roundRect(g, x, y, w, h, 2);
  const lit = Math.round(m.level * m.segments);
  const pitch = (vertical ? h : w) / m.segments, gap = vertical ? 1.6 : 2.6;
  for (let i = 0; i < m.segments; i++) {
    const seg = vertical ? [x + 1, y + h - pitch * (i + 1) + gap / 2, w - 2, pitch - gap] : [x + pitch * i + gap / 2, y + 1, pitch - gap, h - 2];
    const last = i === m.segments - 1;
    if (last && m.clip > 0) g.fillStyle = '#ff3b30';
    else if (i < lit) { const t = i / m.segments; g.fillStyle = `rgb(${24 + (47 - 24) * t | 0}, ${183 + (125 - 183) * t | 0}, 255)`; }
    else g.fillStyle = '#10202f';
    roundRect(g, seg[0], seg[1], seg[2], seg[3], 1);
    if (i < lit && !(last && m.clip > 0)) {
      g.fillStyle = 'rgba(255,255,255,0.28)';
      const rx = vertical ? 2 : 1.5, ry = vertical ? 1 : 4;
      roundRect(g, seg[0] + rx, seg[1] + ry, seg[2] - rx * 2, seg[3] - ry * 2, 0.8);
    }
  }
}

function drawImpulse(canvas, w, h, compact, active) {
  const s = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(w * s)) { canvas.width = Math.round(w * s); canvas.height = Math.round(h * s); }
  const g = canvas.getContext('2d');
  g.setTransform(s, 0, 0, s, 0, 0);
  g.clearRect(0, 0, w, h);
  paintImpulse(g, 0, 0, w, h, compact, active);
}

function paintImpulse(g, x, y, w, h, compact, active) {
  const shape = impulse.current?.shape;
  g.textAlign = 'center'; g.textBaseline = 'middle';
  if (!shape) {
    g.fillStyle = colours.dim; g.font = '13.5px Segoe UI, Arial';
    if (compact) { g.fillText('Cliquer pour charger', x + w / 2, y + h / 2 - 9); g.fillText('un fichier WAV, AIFF ou FLAC', x + w / 2, y + h / 2 + 9); }
    else g.fillText('Cliquer pour charger une réponse impulsionnelle (WAV, AIFF, FLAC)', x + w / 2, y + h / 2);
    return;
  }
  const mid = y + h / 2, step = w / shape.length;
  if (active) { g.fillStyle = 'rgba(47,155,255,0.10)'; g.fillRect(x, mid - 7, w, 14); }
  g.fillStyle = active ? 'rgba(47,155,255,0.95)' : 'rgba(125,131,141,0.6)';
  for (let i = 0; i < shape.length; i++) {
    const v = Math.max(0.6, shape[i] * h * 0.48);
    g.fillRect(x + step * i, mid - v, Math.max(1, step * 0.72), v * 2);
  }
}

function drawFx() {
  const g = fxc;
  g.clearRect(0, 0, W, H);

  // Lampes : lueur qui vacille quand les aiguilles passent dans le rouge
  tubes.forEach(([tx, ty, rx, ry], i) => {
    const level = tubeLevels[i];
    if (level <= 0) return;
    g.save();
    g.translate(tx, ty); g.scale(rx / ry, 1);
    let grad = g.createRadialGradient(0, 0, 0, 0, 0, ry);
    grad.addColorStop(0, `rgba(255,138,42,${0.62 * level})`); grad.addColorStop(1, 'rgba(255,90,16,0)');
    g.fillStyle = grad; g.beginPath(); g.arc(0, 0, ry, 0, Math.PI * 2); g.fill();
    const core = ry * 0.5;
    grad = g.createRadialGradient(0, 0, 0, 0, 0, core);
    grad.addColorStop(0, `rgba(255,240,200,${0.7 * level})`); grad.addColorStop(1, 'rgba(255,176,80,0)');
    g.fillStyle = grad; g.beginPath(); g.arc(0, 0, core, 0, Math.PI * 2); g.fill();
    g.restore();
  });

  const powered = S.isOn('power');
  if (!powered) { g.fillStyle = 'rgba(0,0,0,0.55)'; g.fillRect(76, 186, 1384, 240); }

  // Barre d'entrée de la tête d'ampli
  drawSegments(g, 151, 474, 12, 119, true, ampInput);

  // Aiguilles des vu-mètres
  needles.forEach((a, i) => {
    const px = 975 + 204 * i + 82, py = 475 + 100, rad = a * Math.PI / 180;
    const tx = px + Math.sin(rad) * 90, ty = py - Math.cos(rad) * 90;
    g.lineCap = 'butt';
    g.strokeStyle = 'rgba(0,0,0,0.22)'; g.lineWidth = 2.2;
    g.beginPath(); g.moveTo(px + 2, py + 2.5); g.lineTo(tx + 2, ty + 2.5); g.stroke();
    g.strokeStyle = '#17130f'; g.lineWidth = 1.7;
    g.beginPath(); g.moveTo(px, py); g.lineTo(tx, ty); g.stroke();
  });

  // POWER : lentille assombrie quand l'ampli est éteint, ON / OFF dessous
  if (!powered) { g.fillStyle = 'rgba(5,8,14,0.86)'; g.beginPath(); g.arc(1439, 505, 18, 0, Math.PI * 2); g.fill(); }
  g.fillStyle = powered ? colours.text : colours.dim;
  g.font = '14.5px Segoe UI, Arial'; g.textAlign = 'center'; g.textBaseline = 'middle';
  g.fillText(powered ? 'ON' : 'OFF', 1439, 545);

  // Page 1 : barres d'entrée et de sortie, nom INPUT, forme de l'IR
  if (page === 0) {
    drawSegments(g, 1050, 940, 128, 31, false, rackOut);
    drawSegments(g, 792, 940, 128, 31, false, rackIn);
    g.fillStyle = '#d6dae0'; g.font = '19.5px Segoe UI, Arial'; g.textAlign = 'left';
    g.fillText('INPUT', 794, 909);
    paintImpulse(g, 40, 912, 414, 58, false, S.isOn('irOn') && !!impulse.current);
  }
}

let last30 = 0;
function frame(t) {
  if (t - last30 >= 33) { last30 = t; tick30(); }
  drawFx();
  requestAnimationFrame(frame);
}

//==============================================================================
// Démarrage : choix GUITARE / BASSE (le clic sert aussi à démarrer le son)
function showChooser() {
  const back = el('div', '', root); back.id = 'chooser';
  const box = el('div', 'box', back);
  el('h1', '', box).textContent = 'Quel instrument jouez-vous ?';
  el('div', 'sub', box).textContent = 'Vous pourrez changer à tout moment avec le bouton GUITARE / BASSE, en haut à gauche.';
  const status = el('div', 'status', box);
  const titles = ['GUITARE', 'BASSE'], subs = ['Ampli guitare, pédalier et presets guitare', 'Ampli basse, mélange DI et presets basse'];
  let busy = false;
  [0, 1].forEach((i) => {
    const card = place(el('div', 'ccard', box), 50 + i * 540, 120);
    const img = el('img', '', card); img.src = `img/apercu_${i ? 'basse' : 'guitare'}.webp`; img.alt = titles[i];
    el('b', '', card).textContent = titles[i];
    el('span', '', card).textContent = subs[i];
    card.addEventListener('click', async () => {
      if (busy) return;
      busy = true;
      status.textContent = desktop ? 'Démarrage du son…' : 'Démarrage du son… autorisez l\'accès à l\'entrée audio si le navigateur le demande.';
      S.chooseInstrument(i === 1);
      try {
        await startAudio();
        back.remove();
      } catch (err) {
        status.textContent = '';
        busy = false;
        modal('Le son ne démarre pas', 'Le moteur audio n\'a pas pu démarrer : ' + err.message + (desktop
          ? '\n\nFermez cette fenêtre et relancez JSG Guitar. Le journal est dans ' + (macApp ? '~/Bibliothèque/Application Support/JSG Guitar/journal.txt.' : '%LOCALAPPDATA%\\JSG Guitar\\journal.txt.')
          : '\n\nUtilisez un navigateur récent (Chrome, Edge ou Firefox), et ouvrez la page par http://localhost (lancer.bat) ou par une adresse https.'));
      }
    });
  });
  el('div', 'foot-note', box).textContent = "Branchez la guitare dans la carte son. Utilisez un casque : avec le micro et les haut-parleurs de l'ordinateur, le son boucle (larsen).";
  const onKey = (ev) => { if (!document.getElementById('chooser')) { window.removeEventListener('keydown', onKey); return; }
    if (/^[gG]$/.test(ev.key) || ev.key === 'ArrowLeft') box.querySelectorAll('.ccard')[0].click();
    if (/^[bB]$/.test(ev.key) || ev.key === 'ArrowRight') box.querySelectorAll('.ccard')[1].click(); };
  window.addEventListener('keydown', onKey);
}

async function startAudio() {
  await audio.start(S.values);
  S.connectAudio(audio);
  audioStarted = true;
  player.applySettings();

  // Dernière réponse impulsionnelle utilisée
  try {
    const saved = JSON.parse(localStorageGet('jsg.ir') || 'null');
    if (saved) {
      const irWasOn = S.isOn('irOn');
      await useImpulse({ name: saved.name, bytes: fromBase64(saved.data) });
      if (!irWasOn) S.set('irOn', 0);
    }
  } catch { /* IR illisible : ignorée */ }

  if (audio.needsResume()) toast('Cliquez n\'importe où pour activer le son.');
  root.addEventListener('pointerdown', () => audio.needsResume() && audio.resume(), { once: false });
  if (desktop) {
    showAudioProblem(audio.status);
    audio.on('closed', () => modal('JSG Guitar', 'Le programme JSG Guitar est fermé. Relancez-le pour continuer à jouer.'));
  }
}

//==============================================================================
// Plug-in : pas de choix d'instrument au départ, tout vient du projet du logiciel hôte
function applyPluginState(st) {
  S.applyRemoteState(st);
  impulse.list = st.irName && st.irShape ? [{ name: st.irName, shape: Float32Array.from(st.irShape), bytes: null }] : [];
  impulse.index = impulse.list.length ? 0 : -1;
  if (bassSkin !== S.isBassMode()) { bassSkin = S.isBassMode(); updateBackground(); }
  S.refreshNow();
}

async function startPlugin() {
  audio.on('state', applyPluginState);
  audio.on('p', (m) => S.applyRemoteParam(m.i, m.v));
  audio.on('closed', () => modal('JSG Guitar', "Le plug-in a été retiré ou le projet fermé. Rouvrez la fenêtre du plug-in depuis votre logiciel."));
  try {
    await audio.start(S.values);
  } catch (err) {
    modal('JSG Guitar', "L'interface n'arrive pas à joindre le plug-in : " + err.message);
    return;
  }
  S.connectAudio(audio, false);
  audioStarted = true;
  if (audio.lastState) applyPluginState(audio.lastState);
}

if (plugin && new URLSearchParams(location.search).has('test')) window.__jsg = { S, impulse };   // tests automatiques
if (plugin) {
  setPage(page);
  selectTab(0);
  updateBackground();
  S.refreshNow();
  startPlugin();
} else {
  S.restoreSession();
  bassSkin = S.isBassMode();
  setPage(page);
  selectTab(0);
  updateBackground();
  S.refreshNow();
  showChooser();
}
requestAnimationFrame(frame);

if (!window.isSecureContext && !desktop)
  toast("Attention : page ouverte sans https ni localhost — le navigateur refusera l'accès à l'entrée audio. Lancez lancer.bat.", 12000);
