-- =====================================================================
-- EcoBio-Matrix · المرحلة 3: قاعدة بيانات Supabase
-- الصق الملف كامل في SQL Editor واضغط Run.
-- آمن لو شغّلته أكثر من مرة: ما يمسح بيانات، وما يكرر شي موجود.
--
-- النتيجة:
--   sensor_readings : صف لكل قراءة يرسلها الـ ESP32
--   events          : صف لكل حدث (تحويل صمام، رجوع، وضع آمن، انقطاع إنترنت ...)
--   المفتاح العام (publishable أو anon) يقدر يضيف ويقرأ بس. ما يعدّل ولا يحذف.
--   الجدولين عليهم فهرس على ts، ومفعّل لهم Realtime عشان اللوحة تتحدث لحالها.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1) جدول القراءات
--    أسماء الأعمدة نفس اللي يرسلها sketch.ino بالضبط.
--    حدود CHECK فيزيائية بس (مثل pH من 0 إلى 14)، مو حدود التشغيل.
-- ---------------------------------------------------------------------
create table if not exists public.sensor_readings (
  id         bigint generated always as identity primary key,
  ts         timestamptz not null default now(),
  ph         real check (ph between 0 and 14),
  orp_in_mv  real,
  do_mgl     real check (do_mgl >= 0),
  temp_c     real,
  turbidity  real check (turbidity >= 0),
  uv_pct     real check (uv_pct between 0 and 100),
  orp_cl_mv  real,
  level_pct  real check (level_pct between 0 and 100),
  valve1     text    not null check (valve1 in ('normal', 'sewer')),
  valve2     text    not null check (valve2 in ('normal', 'sewer')),
  air_pump   boolean not null,
  cl_pump    boolean not null,
  safe_mode  boolean not null
);

comment on table  public.sensor_readings           is 'قراءة وحدة من الـ ESP32 (كل ثانية في وضع العرض، كل دقيقة في الواقع)';
comment on column public.sensor_readings.ts        is 'وقت القراءة الأصلي (UTC). لو الجهاز ما أرسله، ينحط وقت الوصول';
comment on column public.sensor_readings.ph        is 'pH مخرج خزان الموازنة. null = قراءة مستحيلة أو الحساس معطل';
comment on column public.sensor_readings.orp_in_mv is 'ORP1 مخرج الموازنة (mV)';
comment on column public.sensor_readings.do_mgl    is 'الأكسجين المذاب في المفاعل (mg/L)';
comment on column public.sensor_readings.temp_c    is 'حرارة المفاعل (°C) من DS18B20';
comment on column public.sensor_readings.turbidity is 'العكارة قبل UV (NTU)';
comment on column public.sensor_readings.uv_pct    is 'شدة UV (%)';
comment on column public.sensor_readings.orp_cl_mv is 'ORP2 بعد الكلور (mV)، ومنه يتقدّر الكلور المتبقي';
comment on column public.sensor_readings.level_pct is 'امتلاء خزان التخزين (%)';
comment on column public.sensor_readings.valve1    is 'صمام 1: normal = المسار الطبيعي، sewer = للصرف';
comment on column public.sensor_readings.valve2    is 'صمام 2: normal = المسار الطبيعي، sewer = للصرف';
comment on column public.sensor_readings.air_pump  is 'مضخة الهواء شغالة؟';
comment on column public.sensor_readings.cl_pump   is 'مضخة الكلور شغالة؟';
comment on column public.sensor_readings.safe_mode is 'الوضع الآمن (عطل حساس)؟';

-- ---------------------------------------------------------------------
-- 2) جدول الأحداث
--    type: حروف إنجليزية صغيرة و _ بس (boot, divert, restore, safe_mode,
--    safe_mode_end, net_offline, net_online). نوع جديد ما يحتاج تعديل الجدول.
-- ---------------------------------------------------------------------
create table if not exists public.events (
  id          bigint generated always as identity primary key,
  ts          timestamptz not null default now(),
  type        text not null check (type ~ '^[a-z_]{1,32}$'),
  reason      text check (char_length(reason) <= 500),
  valve       text check (valve in ('valve1', 'valve2', 'both')),
  value       real,
  decided_by  text not null default 'rule' check (decided_by in ('rule', 'ai'))
);

comment on table  public.events            is 'أحداث الحماية: تحويل صمام، رجوع، وضع آمن، انقطاع ورجوع الإنترنت';
comment on column public.events.ts         is 'وقت الحدث الأصلي (UTC)';
comment on column public.events.type       is 'boot / divert / restore / safe_mode / safe_mode_end / net_offline / net_online';
comment on column public.events.reason     is 'السبب بالعربي';
comment on column public.events.valve      is 'valve1 / valve2 / both، أو فاضي لو الحدث مو عن صمام';
comment on column public.events.value      is 'القراءة اللي سببت الحدث (لو فيه)';
comment on column public.events.decided_by is 'rule = قاعدة ثابتة في الـ ESP32، ai = قرار الذكاء الاصطناعي';

-- ---------------------------------------------------------------------
-- 3) فهارس على ts (اللوحة دايم تطلب الأحدث أول)
-- ---------------------------------------------------------------------
create index if not exists sensor_readings_ts_idx on public.sensor_readings (ts desc);
create index if not exists events_ts_idx          on public.events          (ts desc);

-- ---------------------------------------------------------------------
-- 4) الصلاحيات: المفتاح العام (دور anon) يضيف ويقرأ بس
--    RLS يحدد الصفوف، والـ GRANT يحدد العمليات. الاثنين مع بعض:
--    ما فيه سياسة UPDATE ولا DELETE، وكمان سحبنا الصلاحية نفسها.
-- ---------------------------------------------------------------------
alter table public.sensor_readings enable row level security;
alter table public.events          enable row level security;

revoke all on table public.sensor_readings, public.events from anon, authenticated;
grant select, insert on table public.sensor_readings, public.events to anon;

drop policy if exists "anon_insert_readings" on public.sensor_readings;
create policy "anon_insert_readings" on public.sensor_readings
  for insert to anon with check (true);

drop policy if exists "anon_select_readings" on public.sensor_readings;
create policy "anon_select_readings" on public.sensor_readings
  for select to anon using (true);

drop policy if exists "anon_insert_events" on public.events;
create policy "anon_insert_events" on public.events
  for insert to anon with check (true);

drop policy if exists "anon_select_events" on public.events;
create policy "anon_select_events" on public.events
  for select to anon using (true);

-- ---------------------------------------------------------------------
-- 5) Realtime: أي صف جديد يوصل للوحة مباشرة
-- ---------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from pg_publication where pubname = 'supabase_realtime') then
    create publication supabase_realtime;
  end if;
  if not exists (select 1 from pg_publication_tables
                 where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'sensor_readings') then
    alter publication supabase_realtime add table public.sensor_readings;
  end if;
  if not exists (select 1 from pg_publication_tables
                 where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = 'events') then
    alter publication supabase_realtime add table public.events;
  end if;
end
$$;

commit;
