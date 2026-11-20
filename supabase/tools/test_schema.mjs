// Local test of ../schema.sql on a real PostgreSQL (PGlite = PostgreSQL compiled to WebAssembly).
//
//   npm install @electric-sql/pglite@0.3
//   node test_schema.mjs ../schema.sql ../verify.sql
//
// It recreates the parts of Supabase the schema relies on (roles anon / authenticated / service_role,
// Supabase's default grants, the supabase_realtime publication), runs the schema twice, then acts as
// the public key would (role anon) and sends the exact JSON shapes that sketch.ino sends, the way
// PostgREST turns a JSON POST into an INSERT. It is NOT Supabase itself: the HTTP layer is not tested here.
import { PGlite } from '@electric-sql/pglite';
import fs from 'node:fs';

const [schemaPath, verifyPath] = process.argv.slice(2);
const schema = fs.readFileSync(schemaPath, 'utf8');
const verify = fs.readFileSync(verifyPath, 'utf8');
const db = new PGlite();
let pass = 0, fail = 0;
const ok = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('PASS', name, extra); } else { fail++; console.log('FAIL', name, extra); }
};

// --- what a fresh Supabase project already has
await db.exec(`
  create role anon nologin;
  create role authenticated nologin;
  create role service_role nologin bypassrls;
  grant usage on schema public to anon, authenticated, service_role;
  alter default privileges in schema public grant all on tables    to anon, authenticated, service_role;
  alter default privileges in schema public grant all on sequences to anon, authenticated, service_role;
  create publication supabase_realtime;
`);

// --- run the schema twice (must be safe to re-run)
await db.exec(schema);
await db.exec(schema);
ok('schema.sql runs twice without error', true);

const ver = await db.query(verify);
for (const r of ver.rows) ok('verify: ' + r.check_name, r.ok === true);

// --- PostgREST-style insert as role anon (POST /rest/v1/<table> with a JSON array body)
async function asAnon(sql, params = []) {
  return db.transaction(async (tx) => {
    await tx.exec('set local role anon');
    return tx.query(sql, params);
  });
}
async function restInsert(table, body) {
  const rows = JSON.parse(body);
  const cols = Object.keys(rows[0]).map((c) => `"${c}"`).join(', ');
  return asAnon(`insert into public.${table} (${cols}) select ${cols} from json_populate_recordset(null::public.${table}, $1::json)`, [body]);
}
async function expectError(name, fn, code) {
  try { await fn(); ok(name, false, '(no error)'); }
  catch (e) { ok(name, e.code === code, `(${e.code} ${e.message.split('\n')[0]})`); }
}

// exact formats produced by jNum/jStr/jBool/jTs in sketch.ino
const readingsWithTs = JSON.stringify([
  { ts: '2026-11-20T13:57:37.255Z', ph: 7.50, orp_in_mv: 101, do_mgl: 6.00, temp_c: 25.00, turbidity: 2.00, uv_pct: 85.0,
    orp_cl_mv: 700, level_pct: 49.4, valve1: 'normal', valve2: 'normal', air_pump: true, cl_pump: false, safe_mode: false },
  { ts: '2026-11-20T13:57:38.255Z', ph: null, orp_in_mv: 101, do_mgl: 6.00, temp_c: null, turbidity: 2.00, uv_pct: 85.0,
    orp_cl_mv: 581, level_pct: null, valve1: 'sewer', valve2: 'sewer', air_pump: true, cl_pump: false, safe_mode: true },
]).replace('"ph":7.5,', '"ph":7.50,');
const readingsNoTs = '[{"ph":8.90,"orp_in_mv":-250,"do_mgl":0.00,"temp_c":24.94,"turbidity":6.00,"uv_pct":55.0,"orp_cl_mv":400,"level_pct":94.0,"valve1":"sewer","valve2":"sewer","air_pump":true,"cl_pump":true,"safe_mode":false}]';
const events = JSON.stringify([
  { ts: '2026-11-20T13:57:20.000Z', type: 'boot', reason: 'تشغيل الجهاز: الصمامان على الصرف لين تثبت القراءات (Fail-safe)', valve: 'both', value: null, decided_by: 'rule' },
  { ts: '2026-11-20T13:57:31.000Z', type: 'divert', reason: 'pH برا الحد (بين 6.5 و 8.4): 8.90', valve: 'valve1', value: 8.90, decided_by: 'rule' },
  { ts: '2026-11-20T13:57:41.000Z', type: 'restore', reason: 'القراءات سليمة 10 ثواني: رجوع للمسار الطبيعي', valve: 'valve1', value: null, decided_by: 'rule' },
  { ts: '2026-11-20T13:58:00.000Z', type: 'net_offline', reason: 'انقطع الإنترنت', valve: null, value: null, decided_by: 'rule' },
]);

await restInsert('sensor_readings', readingsWithTs).then(() => ok('anon INSERT readings (with ts)', true));
await restInsert('sensor_readings', readingsNoTs).then(() => ok('anon INSERT readings (no ts -> now())', true));
await restInsert('events', events).then(() => ok('anon INSERT events (Arabic reason, nulls)', true));

const sel = await asAnon('select count(*)::int n from public.sensor_readings');
ok('anon SELECT readings', sel.rows[0].n === 3, `(rows=${sel.rows[0].n})`);
const evs = await asAnon('select type, reason from public.events order by ts');
ok('anon SELECT events + Arabic text intact', evs.rows.length === 4 && evs.rows[0].reason.startsWith('تشغيل الجهاز'));
const back = await asAnon("select ph, valve1, ts from public.sensor_readings where ts = '2026-11-20T13:57:37.255Z'");
ok('values stored as sent (ph 7.5, valve1 normal, original ts)', back.rows.length === 1 && Math.abs(back.rows[0].ph - 7.5) < 1e-6 && back.rows[0].valve1 === 'normal');
const noTs = await asAnon('select ts from public.sensor_readings where ph > 8.5');
ok('row without ts gets the arrival time', noTs.rows.length === 1 && noTs.rows[0].ts instanceof Date);

// --- what the public key must NOT be able to do
await expectError('anon UPDATE refused', () => asAnon("update public.sensor_readings set valve1 = 'normal'"), '42501');
await expectError('anon DELETE refused', () => asAnon('delete from public.sensor_readings'), '42501');
await expectError('anon DELETE events refused', () => asAnon('delete from public.events'), '42501');
await expectError('anon TRUNCATE refused', () => asAnon('truncate public.events'), '42501');
await expectError('anon cannot choose the id', () => restInsert('events', '[{"id":1,"type":"boot","reason":"x","valve":null,"value":null,"decided_by":"rule"}]'), '428C9');
await expectError('authenticated has no access', () => db.transaction(async (tx) => { await tx.exec('set local role authenticated'); await tx.query('select 1 from public.events'); }), '42501');

// --- junk is rejected by the CHECK constraints
await expectError('valve1 must be normal/sewer', () => restInsert('sensor_readings', readingsNoTs.replace('"valve1":"sewer"', '"valve1":"open"')), '23514');
await expectError('pH must be 0..14', () => restInsert('sensor_readings', readingsNoTs.replace('"ph":8.90', '"ph":15')), '23514');
await expectError('level must be 0..100', () => restInsert('sensor_readings', readingsNoTs.replace('"level_pct":94.0', '"level_pct":120')), '23514');
await expectError('event type must be a short lowercase word', () => restInsert('events', '[{"type":"DROP TABLE events","reason":"x","valve":null,"value":null,"decided_by":"rule"}]'), '23514');
await expectError('decided_by must be rule/ai', () => restInsert('events', '[{"type":"divert","reason":"x","valve":"valve1","value":1,"decided_by":"admin"}]'), '23514');
await expectError('reason max 500 chars', () => restInsert('events', JSON.stringify([{ type: 'divert', reason: 'x'.repeat(501), valve: null, value: null, decided_by: 'rule' }])), '23514');
await expectError('missing valve1 rejected', () => restInsert('sensor_readings', '[{"ph":7.5,"valve2":"normal","air_pump":true,"cl_pump":false,"safe_mode":false}]'), '23502');
await restInsert('events', '[{"type":"divert","reason":"AI test","valve":"valve2","value":0.1,"decided_by":"ai"}]').then(() => ok('decided_by = ai accepted', true));

// --- index use and size (local PostgreSQL, not Supabase)
await db.exec(`insert into public.sensor_readings (ts, ph, orp_in_mv, do_mgl, temp_c, turbidity, uv_pct, orp_cl_mv, level_pct, valve1, valve2, air_pump, cl_pump, safe_mode)
  select now() - (g || ' seconds')::interval, 7.5, 101, 6, 25, 2, 85, 700, 49, 'normal', 'normal', true, false, false from generate_series(1, 20000) g;
  analyze public.sensor_readings;`);
const plan = await db.query('explain select * from public.sensor_readings order by ts desc limit 50');
ok('latest-50 query uses the ts index', plan.rows.map((r) => r['QUERY PLAN']).join(' ').includes('sensor_readings_ts_idx'));
const size = await db.query("select pg_total_relation_size('public.sensor_readings')::bigint b, count(*)::int n from public.sensor_readings");
console.log(`INFO size: ${size.rows[0].n} rows = ${(Number(size.rows[0].b) / 1048576).toFixed(2)} MB incl. indexes => ~${Math.round(Number(size.rows[0].b) / size.rows[0].n)} bytes/row (local PostgreSQL ${(await db.query('show server_version')).rows[0].server_version})`);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
