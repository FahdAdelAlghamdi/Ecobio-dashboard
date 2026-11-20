// End-to-end test of dist/index.html in a real browser (headless Edge/Chrome) against the real Supabase project.
//
//   npm install puppeteer-core@24.10.0 @supabase/supabase-js@2.118.0
//   node test_dashboard.mjs <path to dist/index.html> <browser exe> <screenshot folder>
//
// It plays the role of the ESP32 by inserting readings/events with the PUBLIC key (same as Wokwi would),
// and checks that the page shows them live. Test events carry "(اختبار)" in their reason.
// The rows stay in the database (the public key cannot delete): clean them with the SQL in ../README.md.
import puppeteer from 'puppeteer-core';
import { createClient } from '@supabase/supabase-js';
import fs from 'node:fs';
import path from 'node:path';

const [page_, exe, shots] = process.argv.slice(2);
const URL = 'https://vnwptdfnleongvkbiydc.supabase.co', KEY = 'sb_publishable_A-NSuIONi17ERzZtFdh3Yg_eM0yMuHV';
const sb = createClient(URL, KEY, { auth: { persistSession: false } });
fs.mkdirSync(shots, { recursive: true });
let pass = 0, fail = 0;
const ok = (n, c, x = '') => { if (c) { pass++; console.log('PASS', n, x); } else { fail++; console.log('FAIL', n, x); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const reading = o => Object.assign({ ts: new Date().toISOString(), ph: 7.5, orp_in_mv: 181, do_mgl: 3.79, temp_c: 25, turbidity: 2.0, uv_pct: 85.1,
  orp_cl_mv: 700, level_pct: 49.3, valve1: 'normal', valve2: 'normal', air_pump: true, cl_pump: false, safe_mode: false }, o);
async function send(o) { const r = await sb.from('sensor_readings').insert(reading(o)); if (r.error) throw r.error; }
async function ev(o) { const r = await sb.from('events').insert(Object.assign({ ts: new Date().toISOString(), value: null, decided_by: 'rule' }, o)); if (r.error) throw r.error; }

const browser = await puppeteer.launch({ executablePath: exe, headless: true, args: ['--no-first-run', '--no-default-browser-check'] });
const page = await browser.newPage();
const errors = [];
page.on('pageerror', e => errors.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
await page.setViewport({ width: 1440, height: 900 });
await page.goto('file:///' + path.resolve(page_).replace(/\\/g, '/'), { waitUntil: 'networkidle2', timeout: 60000 });

const q = (fn, ...a) => page.evaluate(fn, ...a);
const waitFor = async (fn, ms, ...a) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await q(fn, ...a)) return true; await sleep(250); } return false; };

// 1) loads in live mode and connects
ok('starts in live mode', await q(() => document.body.dataset.mode === 'live'));
ok('connects to Supabase', await waitFor(() => live.state === 'ok', 15000), await q(() => live.state + ' ' + live.err));
ok('Realtime subscribed', await waitFor(() => live.rt === true, 15000));
ok('history loaded', await q(() => live.rows.length > 0), await q(() => `rows=${live.rows.length} events=${live.events.length}`));
ok('8 sensor cards (incl. chlorine)', await q(() => document.querySelectorAll('#cards .card').length === 8 && document.querySelector('#cards').textContent.includes('الكلور المتبقي')));
ok('event log has columns time/reason/valve/decided by', await q(() => { const h = document.querySelector('#log .lrow.hd'); return !!h && h.textContent.includes('مين قرر') && h.textContent.includes('السبب'); }));

// 2) a new reading appears without reloading (Realtime)
const n0 = await q(() => live.rows.length);
const t0 = Date.now(); await send({});
ok('new reading arrives live', await waitFor(n => live.rows.length > n, 8000, n0), `${Date.now() - t0} ms`);
ok('connection badge says connected', await waitFor(() => document.getElementById('conn').className.includes('ok'), 4000), await q(() => document.getElementById('conn').textContent));
await page.screenshot({ path: path.join(shots, '1_live_normal.png'), fullPage: true });

// 3) AI on live data: turbidity 4 + DO 3.0 breaks no fixed rule, but the model flags the combination
//    (first 21 normal readings, so an alert left over from old history does not hide the new one)
for (let i = 0; i < 21; i++) { await send({}); await sleep(120); }
ok('AI normal before the test', await waitFor(() => document.getElementById('aiAlert').hidden, 8000));
for (let i = 0; i < 3; i++) { await send({ turbidity: 4.0, do_mgl: 3.0 }); await sleep(400); }
ok('AI alert shown', await waitFor(() => !document.getElementById('aiAlert').hidden, 8000), await q(() => document.getElementById('aiAlertTxt').textContent));
ok('AI diagnosis = detergent/organic load', await q(() => live.ai.diag === 'detergent'), await q(() => live.ai.diag));
ok('AI event written with decided_by=ai', await waitFor(() => live.events.some(e => e.decided_by === 'ai' && e.type === 'ai_anomaly' && Date.now() - e.t < 120000), 10000));
ok('log shows the AI tag', await waitFor(() => !!document.querySelector('#log .tag.ai'), 4000));
await page.screenshot({ path: path.join(shots, '2_live_ai_alert.png'), fullPage: true });

// 4) rule decision from the ESP32: pH 8.9 -> valve 1 to sewer
await ev({ type: 'divert', reason: '(اختبار) pH برا الحد (بين 6.5 و 8.4): 8.90', valve: 'valve1', value: 8.9 });
await send({ ph: 8.9, valve1: 'sewer' });
ok('header shows valve 1 to sewer', await waitFor(() => document.getElementById('v1').className.includes('bad'), 8000));
ok('alert shows the ESP32 reason', await waitFor(() => document.getElementById('alertTxt').textContent.includes('8.90'), 5000), await q(() => document.getElementById('alertTxt').textContent));
ok('pH card out of limit', await q(() => [...document.querySelectorAll('#cards .card')].some(c => c.textContent.includes('pH') && c.textContent.includes('خارج الحد'))));
await page.screenshot({ path: path.join(shots, '3_live_valve1_sewer.png'), fullPage: true });

// 5) sensor fault: safe mode, pH null
await ev({ type: 'safe_mode', reason: '(اختبار) وضع آمن: قراءة pH مستحيلة', valve: 'both' });
await send({ ph: null, valve1: 'sewer', valve2: 'sewer', safe_mode: true });
ok('safe mode badge', await waitFor(() => !document.getElementById('safe').hidden, 8000));
ok('pH card says no reading', await waitFor(() => [...document.querySelectorAll('#cards .card')].some(c => c.textContent.includes('—') && c.textContent.includes('ما فيه قراءة')), 3000));
ok('AI paused note', await q(() => document.getElementById('normalNote').textContent.includes('متوقف')));

// 6) back to normal: 21 normal readings clear the AI alert
await ev({ type: 'restore', reason: '(اختبار) رجع للمسار الطبيعي', valve: 'both' });
for (let i = 0; i < 21; i++) { await send({}); await sleep(150); }
ok('AI alert cleared', await waitFor(() => document.getElementById('aiAlert').hidden, 10000));
ok('ai_clear event written', await waitFor(() => live.events.some(e => e.type === 'ai_clear' && Date.now() - e.t < 120000), 10000));
ok('valves back to normal', await waitFor(() => document.getElementById('v1').className.includes('ok') && document.getElementById('v2').className.includes('ok'), 5000));

// 7) English
await page.click('#langBtn'); await sleep(600);
ok('English UI', await q(() => document.documentElement.lang === 'en' && document.getElementById('subtitle').textContent.startsWith('Live readings')));
await page.screenshot({ path: path.join(shots, '4_live_english.png'), fullPage: true });
await page.click('#langBtn'); await sleep(400);

// 8) internet lost on presentation day -> clear message + switch to simulation
await page.setOfflineMode(true);
ok('connection error shown', await waitFor(() => !document.getElementById('connBar').hidden && live.state === 'error', 12000), await q(() => document.getElementById('connBarTxt').textContent));
await page.screenshot({ path: path.join(shots, '5_offline.png'), fullPage: false });
await page.click('#toSim'); await sleep(800);
ok('switched to simulation (offline)', await q(() => document.body.dataset.mode === 'sim' && !document.getElementById('simBar').hidden));
await page.click('[data-sc="chlorine"]');
ok('simulation scenario still works', await waitFor(() => sim.events.some(e => e.kind === 'divert'), 15000));
await page.screenshot({ path: path.join(shots, '6_sim_mode.png'), fullPage: true });
await page.setOfflineMode(false);
await page.click('#backLive');
ok('back to live after reconnect', await waitFor(() => document.body.dataset.mode === 'live' && live.state === 'ok', 15000));

// 9) phone width
await page.setViewport({ width: 390, height: 844, isMobile: true });
await sleep(1200);
ok('no horizontal scroll on phone', await q(() => document.documentElement.scrollWidth <= window.innerWidth + 1), await q(() => document.documentElement.scrollWidth + ' vs ' + window.innerWidth));
await page.screenshot({ path: path.join(shots, '7_phone.png'), fullPage: true });

const realErrors = errors.filter(e => !/ERR_INTERNET_DISCONNECTED|Failed to fetch|NetworkError|WebSocket|net::ERR/i.test(e));
ok('no JavaScript errors', realErrors.length === 0, realErrors.slice(0, 5).join(' | '));
console.log(`\n${pass} passed, ${fail} failed`);
await browser.close();
process.exit(fail ? 1 : 0);
