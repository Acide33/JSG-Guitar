// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
// Test de l'interface en mode plug-in (développeurs) : npm i playwright, compiler ui_host, puis node ui_test.mjs [dossier ui]
import { chromium } from 'playwright';
import { spawn } from 'node:child_process';

const uiDir = process.argv[2] ?? decodeURIComponent(new URL('../../../ui', import.meta.url).pathname);
const host = spawn('./ui_host', [uiDir]);
let out = '';
const lines = [];
host.stdout.on('data', (d) => { out += d; let k; while ((k = out.indexOf('\n')) >= 0) { lines.push(out.slice(0, k)); out = out.slice(k + 1); } });
const waitLine = async (prefix, ms = 4000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const l = lines.find((x) => x.startsWith(prefix)); if (l) return l; await new Promise((r) => setTimeout(r, 50)); } return null; };
const send = (cmd) => host.stdin.write(cmd + '\n');
const ok = (cond, what) => console.log((cond ? 'OK    ' : 'ÉCHEC ') + what);

const url = (await waitLine('URL ')).slice(4) + '&test=1';
console.log('interface :', url);
const browser = await chromium.launch();
const page = await (await browser.newContext({ viewport: { width: 1536, height: 1024 } })).newPage();
const errors = []; page.on('pageerror', (e) => errors.push(e.message)); page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
await page.goto(url);
await page.waitForTimeout(1500);
await page.screenshot({ path: 'plugin_ui_1.png' });

ok(await page.locator('#chooser').count() === 0, 'pas de fenêtre de choix d\'instrument');
ok(!(await page.locator('#recorder').isVisible()), 'REC caché');
ok(await page.evaluate(() => [...document.querySelectorAll('#pageSwitch > div')].filter((b) => b.offsetParent).length) === 2, 'seulement les pages RACK et PÉDALIER');
const gain0 = await page.evaluate(() => window.__jsg.S.get('gain'));
ok(gain0 === 5, 'réglages reçus du plug-in (gain ' + gain0 + ')');

// Réglage touché dans l'interface -> logiciel hôte
await page.evaluate(() => window.__jsg.S.set('gain', 8.3));
ok(!!(await waitLine('EDIT 5 8.3')), 'potard tourné dans l\'interface -> transmis au logiciel hôte');

// Automation du logiciel hôte -> interface
await page.waitForTimeout(500);
send('auto 6 2.5');
await page.waitForTimeout(400);
ok(await page.evaluate(() => window.__jsg.S.get('bass')) === 2.5, 'automation du logiciel hôte -> interface (Bass 2,5)');

// Preset chargé dans l'interface -> plusieurs réglages + nom du preset
await page.evaluate(() => { const S = window.__jsg.S; S.loadPreset(S.allPresets()[2]); });
await page.waitForTimeout(800);
ok(lines.filter((l) => l.startsWith('EDIT')).length > 3 && !!(await waitLine('DIRTY')), 'preset choisi -> réglages transmis, projet modifié');

// IR chargée
await page.click('#pageSwitch > div >> nth=0');
const [chooser] = await Promise.all([page.waitForEvent('filechooser'), (async () => { const b = await page.locator('#stage').boundingBox(); const s = b.width / 1536; await page.mouse.click(b.x + 240 * s, b.y + 940 * s); })()]);
await chooser.setFiles('ir_test.wav');
await page.waitForTimeout(800);
ok(await page.evaluate(() => window.__jsg.S.isOn('irOn') && window.__jsg.impulse.current?.name === 'ir_test'), 'IR chargée et allumée');

// Sauvegarde du projet
lines.length = 0;
send('save');
const state = JSON.parse((await waitLine('STATE ')).slice(6));
ok(state.irFile.length > 1000 && state.irName === 'ir_test' && state.preset !== '', `projet : IR (${state.irFile.length} car.), preset « ${state.preset} »`);

// Rappel d'un autre projet pendant que la fenêtre est ouverte : pédale en emplacement 3, gain 1
const other = { ...state, values: state.values.map((v, i) => (i === 5 ? 1 : v)), slots: state.slots.map((t, i) => (i === 3 ? 2 : -1)), preset: 'Projet rappelé', irFile: '', irName: '' };
send('load ' + JSON.stringify(other));
await waitLine('LOADED 1');
await page.waitForTimeout(500);
const after = await page.evaluate(() => ({ gain: window.__jsg.S.get('gain'), slot3: window.__jsg.S.slotTypes[3], name: window.__jsg.S.getPresetName(), ir: window.__jsg.impulse.current?.name ?? null }));
ok(after.gain === 1 && after.slot3 === 2 && after.name === 'Projet rappelé' && after.ir === null, 'projet rappelé -> interface à jour ' + JSON.stringify(after));

await page.click('#pageSwitch > div >> nth=1');
await page.waitForTimeout(300);
await page.screenshot({ path: 'plugin_ui_2.png' });
ok(errors.length === 0, 'aucune erreur dans la page' + (errors.length ? ' : ' + errors.join(' | ') : ''));
await browser.close();
send('quit');
