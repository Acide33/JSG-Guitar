// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — partie audio de l'interface. Deux façons de faire tourner le son, avec les
    mêmes fonctions pour le reste de l'interface :

    AudioSystem (navigateur) : moteur WebAssembly dans un AudioWorklet.
      entrée (carte son) -> [jsg-guitar : moteur] -> bus de sortie -> haut-parleurs / casque
      lecteur de morceau (élément <audio>) --------> bus de sortie
      bus de sortie / guitare seule -> [jsg-recorder] (REC, fichier WAV téléchargé)

    NativeAudio (version PC, page ouverte avec ?app=1) : le programme JSG Guitar.exe fait tout le
    son (WASAPI, moteur, lecteur, REC sur le disque) ; la page lui parle par une WebSocket.
*/

export const isDesktopApp = () => new URLSearchParams(location.search).has('app');
/** Interface ouverte dans le plug-in VST3 : le son et les réglages appartiennent au logiciel hôte. */
export const isPluginMode = () => new URLSearchParams(location.search).has('plugin');

class Emitter {
  constructor() { this.listeners = {}; }
  on(type, fn) { (this.listeners[type] ||= []).push(fn); }
  emit(type, data) { (this.listeners[type] || []).forEach((fn) => fn(data)); }
}

/** Forme d'une IR pour l'affichage : 160 crêtes normalisées. */
function impulseShape(data) {
  const shape = new Float32Array(160);
  const per = Math.max(1, Math.ceil(data.length / shape.length));
  let peak = 0;
  for (let i = 0; i < shape.length; i++) {
    let m = 0;
    for (let j = i * per; j < Math.min(data.length, (i + 1) * per); j++) m = Math.max(m, Math.abs(data[j]));
    shape[i] = m; peak = Math.max(peak, m);
  }
  if (peak > 0) shape.forEach((v, i) => (shape[i] = v / peak));
  return shape;
}

//==============================================================================
// NAVIGATEUR
//==============================================================================
export class AudioSystem extends Emitter {
  constructor() {
    super();
    this.kind = 'web';
    this.ctx = null;
    this.node = null;
    this.stream = null;
    this.source = null;
    this.deviceId = localStorageGet('jsg.inputDevice') || '';
    this.inputChannel = +(localStorageGet('jsg.inputChannel') || 0);
    this.player = new WebPlayer(this);
  }

  get sampleRate() { return this.ctx ? this.ctx.sampleRate : 48000; }

  /** À appeler depuis un clic (les navigateurs l'exigent pour démarrer le son). */
  async start(initialValues) {
    const AC = window.AudioContext || window.webkitAudioContext;
    this.ctx = new AC({ latencyHint: 0 });

    const [wasm] = await Promise.all([
      fetch('jsg_engine.wasm').then((r) => { if (!r.ok) throw new Error('jsg_engine.wasm introuvable'); return r.arrayBuffer(); }),
      this.ctx.audioWorklet.addModule('worklet.js'),
    ]);

    this.node = new AudioWorkletNode(this.ctx, 'jsg-guitar', {
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
      channelCount: 2, channelCountMode: 'max', channelInterpretation: 'discrete',
      processorOptions: { wasm },
    });

    this.ready = new Promise((resolve, reject) => {
      this.node.port.onmessage = (e) => {
        const m = e.data;
        if (m.t === 'ready') { this.engineLatency = m.latency; resolve(); }
        else if (m.t === 'error') reject(new Error(m.message));
        else this.emit(m.t, m);
      };
    });

    this.node.port.postMessage({ t: 'all', values: initialValues });
    this.node.port.postMessage({ t: 'chan', channel: this.inputChannel });

    // Guitare -> volume de la guitare -> bus (+ morceau du lecteur) -> volume général -> haut-parleurs
    this.guitarGain = this.ctx.createGain();
    this.guitarGain.gain.value = this.guitarVolume ?? 1;
    this.master = this.ctx.createGain();
    this.outGain = this.ctx.createGain();
    this.outGain.gain.value = this.masterVolume ?? 1;
    this.node.connect(this.guitarGain);
    this.guitarGain.connect(this.master);
    this.master.connect(this.outGain);
    this.outGain.connect(this.ctx.destination);

    this.player.attach(this.ctx, this.master);

    // Enregistreur : « tout ce qu'on entend » après le volume général, « guitare seule » avant son volume
    this.recorder = new AudioWorkletNode(this.ctx, 'jsg-recorder', { numberOfInputs: 2, numberOfOutputs: 0 });
    this.outGain.connect(this.recorder, 0, 0);
    this.node.connect(this.recorder, 0, 1);
    this.recorder.port.onmessage = (e) => this.onRecorderMessage(e.data);

    await this.ready;
    await this.openInput(this.deviceId);
    if (this.ctx.state !== 'running') await this.ctx.resume();
  }

  /** Le contexte audio du navigateur attend un clic pour démarrer. */
  needsResume() { return this.ctx && this.ctx.state !== 'running'; }
  resume() { return this.ctx?.resume(); }

  /** Ouvre l'entrée de la carte son (deviceId vide : entrée par défaut ; 'none' : aucune). */
  async openInput(deviceId) {
    if (this.source) { this.source.disconnect(); this.source = null; }
    if (this.stream) { this.stream.getTracks().forEach((t) => t.stop()); this.stream = null; }
    this.deviceId = deviceId || '';
    localStorageSet('jsg.inputDevice', this.deviceId);
    this.inputError = null;
    if (deviceId === 'none') { this.emit('input', { ok: true, none: true }); return; }

    try {
      this.stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          deviceId: deviceId ? { exact: deviceId } : undefined,
          echoCancellation: false, noiseSuppression: false, autoGainControl: false,
          channelCount: { ideal: 2 }, latency: 0,
        },
      });
      this.source = this.ctx.createMediaStreamSource(this.stream);
      this.source.connect(this.node);
      const track = this.stream.getAudioTracks()[0];
      this.inputLabel = track ? track.label : '';
      this.emit('input', { ok: true });
    } catch (err) {
      this.inputError = err;
      this.emit('input', { ok: false, error: err });
    }
  }

  async listInputs() {
    try {
      const devices = await navigator.mediaDevices.enumerateDevices();
      return devices.filter((d) => d.kind === 'audioinput');
    } catch { return []; }
  }

  setInputChannel(channel) {
    this.inputChannel = channel;
    localStorageSet('jsg.inputChannel', String(channel));
    this.node?.port.postMessage({ t: 'chan', channel });
  }

  /** Latence totale estimée (entrée + traitement + sortie), en millisecondes. */
  latencyMs() {
    if (!this.ctx) return 0;
    const track = this.stream?.getAudioTracks()[0];
    const inLatency = (track?.getSettings?.().latency) || 0;
    const block = 128 / this.ctx.sampleRate;
    return 1000 * ((this.ctx.baseLatency || 0) + (this.ctx.outputLatency || 0) + inLatency + block + (this.engineLatency || 0) / this.ctx.sampleRate);
  }

  setParam(index, value) { this.node?.port.postMessage({ t: 'p', i: index, v: value }); }
  setAll(values) { this.node?.port.postMessage({ t: 'all', values }); }
  setPedal(slot, type) { this.node?.port.postMessage({ t: 'ped', slot, type }); }
  setTuner(on) { this.node?.port.postMessage({ t: 'tuner', on }); }

  /** Réponse impulsionnelle : fichier audio décodé à la fréquence du contexte, canal gauche. */
  async loadImpulse(arrayBuffer) {
    const buffer = await this.ctx.decodeAudioData(arrayBuffer);
    const data = buffer.getChannelData(0).slice();
    const shape = impulseShape(data);
    this.node.port.postMessage({ t: 'ir', data }, [data.buffer]);
    return { ok: true, shape };
  }

  //--------------------------------------------------------------------------
  // Enregistreur (REC) : WAV 24 bits stéréo, téléchargé par la page
  /** Volume de la guitare (0..1, avant le mélange avec le morceau) et volume général (0..1, l'ensemble). */
  setGuitarVolume(v) { this.guitarVolume = v; smoothGain(this.guitarGain, v); }
  setMasterVolume(v) { this.masterVolume = v; smoothGain(this.outGain, v); }

  startRecording(source) {
    this.recChunks = [];
    this.recLength = 0;
    this.recorder.port.postMessage({ t: 'start', source });
  }

  stopRecording() {
    return new Promise((resolve) => {
      this.recResolve = resolve;
      this.recorder.port.postMessage({ t: 'stop' });
    });
  }

  onRecorderMessage(m) {
    if (m.t === 'data') { this.recChunks.push(m); this.recLength += m.left.length; }
    if (m.t === 'stopped' && this.recResolve) {
      const blob = encodeWav24(this.recChunks, this.recLength, this.ctx.sampleRate);
      this.recChunks = [];
      this.recResolve({ ok: true, blob });
      this.recResolve = null;
    }
  }

  recordedSeconds() { return this.ctx ? this.recLength / this.ctx.sampleRate : 0; }
}

/** Lecteur du navigateur : élément <audio> (la vitesse change sans changer la hauteur). */
/** Volume (0..1) appliqué en douceur, sans clic. */
function smoothGain(node, v) {
  if (!node) return;
  const t = node.context.currentTime;
  node.gain.cancelScheduledValues(t);
  node.gain.setTargetAtTime(v, t, 0.02);
}

class WebPlayer extends Emitter {
  constructor() {
    super();
    this.canTranspose = false;
    this.A = null; this.B = null; this.abOn = false; this.loop = false;
    this.duration = 0;
  }

  attach(ctx, master) {
    this.ctx = ctx;
    this.el = new Audio();
    this.el.preload = 'auto';
    this.el.preservesPitch = true;
    this.gain = ctx.createGain();
    this.gain.gain.value = this.volume ?? 0.8;
    ctx.createMediaElementSource(this.el).connect(this.gain);
    this.gain.connect(master);
    this.el.addEventListener('ended', () => {
      if (this.loop) { this.el.currentTime = this.abOn ? this.A : 0; this.play(); }
      this.emit('state');
    });
    this.el.addEventListener('play', () => this.emit('state'));
    this.el.addEventListener('pause', () => this.emit('state'));
  }

  /** Charge un morceau : { ok, duration, peaks } (1320 crêtes pour la forme d'onde). */
  async loadBlob(blob, name) {
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = URL.createObjectURL(blob);
    this.A = this.B = null; this.abOn = false;
    this.el.src = this.url;
    this.el.playbackRate = this.speed ?? 1;
    try {
      const buffer = await this.ctx.decodeAudioData(await blob.arrayBuffer());
      this.duration = buffer.duration;
      const n = 1320, peaks = new Float32Array(n);
      const chans = [...Array(buffer.numberOfChannels).keys()].map((c) => buffer.getChannelData(c));
      const per = Math.max(1, Math.floor(buffer.length / n));
      for (let i = 0; i < n; i++) {
        let m = 0;
        for (const ch of chans) for (let j = i * per; j < (i + 1) * per && j < ch.length; j += 4) m = Math.max(m, Math.abs(ch[j]));
        peaks[i] = m;
      }
      return { ok: true, title: name, duration: buffer.duration, peaks };
    } catch {
      return { ok: false, error: "Ce fichier n'a pas pu être lu par le navigateur." };
    }
  }

  async loadUrl(url, name) {
    const res = await fetch(url);
    if (!res.ok) return { ok: false, error: 'Fichier introuvable : ' + url };
    return this.loadBlob(await res.blob(), name);
  }

  get position() { return this.el?.currentTime || 0; }
  isPlaying() { return !!this.el && !this.el.paused; }
  play() { this.el?.play().catch(() => {}); }
  pause() { this.el?.pause(); }
  stop() { if (!this.el) return; this.el.pause(); this.el.currentTime = (this.abOn && this.A !== null) ? this.A : 0; }
  seek(sec) { if (this.el) this.el.currentTime = sec; }
  setSpeed(v) { this.speed = v; if (this.el) this.el.playbackRate = v; }
  setTranspose() {}
  setVolume(v) { this.volume = v; if (this.gain) this.gain.gain.value = v; }
  setLoop(on) { this.loop = on; }
  setAB(a, b, on) { this.A = a; this.B = b; this.abOn = on; }

  /** À appeler régulièrement : boucle A-B. */
  tick() {
    if (this.abOn && this.A !== null && this.B !== null && this.position >= this.B) this.el.currentTime = this.A;
  }
}

//==============================================================================
// VERSION PC : le son est fait par JSG Guitar.exe
//==============================================================================
export class NativeAudio extends Emitter {
  constructor() {
    super();
    this.kind = 'pc';
    this.status = null;
    this.player = new NativePlayer(this);
    this.recSeconds = 0;
    this.waiting = {};          // réponses attendues : type -> fonction
    this.plugin = isPluginMode();
    this.lastState = null;      // plug-in : dernier état reçu (réglages, pédales, IR)
  }

  get sampleRate() { return this.status?.sampleRate || 48000; }
  get inputChannel() { return this.status?.config?.inputChannel ?? 0; }

  async start(initialValues) {
    await new Promise((resolve, reject) => {
      const ws = (this.ws = new WebSocket(`ws://${location.host}/ws`));
      ws.binaryType = 'arraybuffer';
      const timer = setTimeout(() => reject(new Error('le programme JSG Guitar ne répond pas')), 5000);
      ws.onopen = () => { clearTimeout(timer); resolve(); };
      ws.onerror = () => { clearTimeout(timer); reject(new Error('connexion au programme JSG Guitar impossible')); };
      ws.onmessage = (e) => this.onMessage(e);
      ws.onclose = () => this.emit('closed');
    });
    if (!this.plugin) this.setAll(initialValues);   // plug-in : c'est lui qui donne les réglages
    this.status = await this.request({ t: 'hello' }, 'status');
    if (this.plugin && !this.lastState) this.lastState = await this.request({ t: 'getState' }, 'state');
  }

  send(o) { if (this.ws?.readyState === 1) this.ws.send(JSON.stringify(o)); }
  sendBinary(tag, bytes) {
    const data = new Uint8Array(4 + bytes.byteLength);
    data.set(new TextEncoder().encode(tag), 0);
    data.set(new Uint8Array(bytes), 4);
    if (this.ws?.readyState === 1) this.ws.send(data);
  }

  /** Envoie un message et attend la réponse du type donné. */
  request(message, replyType, binary) {
    return new Promise((resolve) => {
      this.waiting[replyType] = resolve;
      this.send(message);
      if (binary) this.sendBinary(binary.tag, binary.bytes);
    });
  }

  onMessage(e) {
    if (typeof e.data !== 'string') {
      const tag = new TextDecoder().decode(new Uint8Array(e.data, 0, 4));
      if (tag === 'TUNR') this.emit('tuner', { data: new Float32Array(e.data.slice(4)) });
      return;
    }
    const m = JSON.parse(e.data);
    if (m.t === 'status') { this.status = m; this.emit('status', m); }
    if (m.t === 'state') { this.lastState = m; this.emit('state', m); }
    if (m.t === 'p') this.emit('p', m);
    if (m.t === 'm') {
      this.recSeconds = m.rec;
      this.player.update(m);
      this.emit('m', m);
    }
    const wait = this.waiting[m.t];
    if (wait) { delete this.waiting[m.t]; wait(m); }
    else if (m.t === 'song') this.emit('song', m);
  }

  needsResume() { return false; }
  resume() {}

  latencyMs() {
    const s = this.status;
    if (!s || !s.running) return 0;
    return s.inputLatencyMs + s.outputLatencyMs + 1000 * (s.engineLatency || 0) / s.sampleRate;
  }

  setParam(index, value) { this.send({ t: 'p', i: index, v: value }); }
  setAll(values) { this.send({ t: 'all', values: Array.from(values) }); }
  setPedal(slot, type) { this.send({ t: 'ped', slot, type }); }
  setTuner(on) { this.send({ t: 'tuner', on }); }
  setInputChannel(channel) { this.configure({ ...this.status.config, inputChannel: channel }); }

  /** Réponse impulsionnelle : le fichier part tel quel, le programme le décode et l'adapte. */
  async loadImpulse(arrayBuffer, name) {
    this.send({ t: 'irName', name });
    const r = await this.request({ t: 'noop' }, 'irLoaded', { tag: 'IRFL', bytes: arrayBuffer });
    return r.ok ? { ok: true, shape: Float32Array.from(r.shape) } : { ok: false, error: r.error };
  }

  setGuitarVolume(v) { this.send({ t: 'guitarVolume', v }); }
  setMasterVolume(v) { this.send({ t: 'masterVolume', v }); }
  startRecording(source, format = 'wav', kbps = 320) { this.send({ t: 'recStart', source, format, kbps }); }
  stopRecording() { return this.request({ t: 'recStop' }, 'recDone'); }
  recordedSeconds() { return Math.max(0, this.recSeconds || 0); }
  openRecordings() { this.send({ t: 'openFolder' }); }

  devices() { return this.request({ t: 'devices' }, 'devices'); }

  /** Latence réelle : clics envoyés sur la sortie, revenus par un câble sur l'entrée. */
  measureLatency() {
    return Promise.race([
      this.request({ t: 'measureLatency' }, 'latency'),
      new Promise((resolve) => setTimeout(() => { delete this.waiting.latency; this.send({ t: 'measureCancel' }); resolve({ ok: false, error: 'pas de réponse' }); }, 15000)),
    ]);
  }

  /** État du son à jour (file d'entrée, incidents). */
  async refreshStatus() { this.status = await this.request({ t: 'hello' }, 'status'); return this.status; }

  /** Nouveaux réglages de la carte son : le programme redémarre le son et renvoie l'état. */
  async configure(config) {
    this.status = await this.request({ t: 'audio', config }, 'status');
    return this.status;
  }

  async release() { this.status = await this.request({ t: 'release' }, 'status'); return this.status; }
  async resumeAudio() { this.status = await this.request({ t: 'resume' }, 'status'); return this.status; }
}

/** Lecteur de la version PC : il tourne dans le programme (vitesse et tonalité, comme sur PC). */
class NativePlayer extends Emitter {
  constructor(audio) {
    super();
    this.audio = audio;
    this.canTranspose = true;
    this.duration = 0;
    this.pos = 0;
    this.playing = false;
  }

  async loadBlob(blob, name) {
    this.audio.send({ t: 'songName', name });
    const r = await this.audio.request({ t: 'noop' }, 'song', { tag: 'SONG', bytes: await blob.arrayBuffer() });
    return this.loaded(r, name);
  }

  async loadUrl(url, name) {
    const r = await this.audio.request({ t: 'loadFile', path: decodeURI(url), name }, 'song');
    return this.loaded(r, name);
  }

  async loadLastTake(name) {
    const r = await this.audio.request({ t: 'loadFile', path: '@lastTake', name }, 'song');
    return this.loaded(r, name);
  }

  loaded(r, name) {
    if (!r.ok) return { ok: false, error: r.error };
    this.duration = r.duration;
    this.pos = 0;
    return { ok: true, title: name, duration: r.duration, peaks: Float32Array.from(r.peaks) };
  }

  update(m) {
    this.pos = m.pos;
    const was = this.playing;
    this.playing = m.playing;
    if (was !== this.playing || m.ended) this.emit('state');
  }

  get position() { return this.pos; }
  isPlaying() { return this.playing; }
  play() { this.audio.send({ t: 'play' }); this.playing = true; this.emit('state'); }
  pause() { this.audio.send({ t: 'pause' }); this.playing = false; this.emit('state'); }
  stop() { this.audio.send({ t: 'stop' }); this.playing = false; this.emit('state'); }
  seek(sec) { this.audio.send({ t: 'seek', sec }); this.pos = sec; }
  setSpeed(v) { this.audio.send({ t: 'speed', v }); }
  setTranspose(v) { this.audio.send({ t: 'transpose', v }); }
  setVolume(v) { this.audio.send({ t: 'volume', v }); }
  setLoop(on) { this.audio.send({ t: 'loop', on }); }
  setAB(a, b, on) {
    if (a === null && b === null) this.audio.send({ t: 'ab', clear: true });
    else this.audio.send({ t: 'ab', a: a ?? -1, b: b ?? -1, on });
  }
  tick() {}
}

//==============================================================================
function encodeWav24(chunks, length, rate) {
  const bytes = length * 6;
  const view = new DataView(new ArrayBuffer(44 + bytes));
  const text = (o, s) => [...s].forEach((c, i) => view.setUint8(o + i, c.charCodeAt(0)));
  text(0, 'RIFF'); view.setUint32(4, 36 + bytes, true); text(8, 'WAVE');
  text(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 2, true);
  view.setUint32(24, rate, true); view.setUint32(28, rate * 6, true); view.setUint16(32, 6, true); view.setUint16(34, 24, true);
  text(36, 'data'); view.setUint32(40, bytes, true);

  let o = 44;
  const put = (x) => {
    const v = Math.max(-8388608, Math.min(8388607, Math.round(Math.max(-1, Math.min(1, x)) * 8388607)));
    view.setUint8(o, v & 255); view.setUint8(o + 1, (v >> 8) & 255); view.setUint8(o + 2, (v >> 16) & 255);
    o += 3;
  };
  for (const c of chunks) for (let i = 0; i < c.left.length; i++) { put(c.left[i]); put(c.right[i]); }
  return new Blob([view.buffer], { type: 'audio/wav' });
}

export function localStorageGet(key) { try { return localStorage.getItem(key); } catch { return null; } }
export function localStorageSet(key, value) { try { localStorage.setItem(key, value); } catch { /* stockage indisponible */ } }
