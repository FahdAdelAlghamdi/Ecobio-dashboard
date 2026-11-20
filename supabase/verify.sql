-- EcoBio-Matrix · فحص المرحلة 3
-- الصقه في SQL Editor بعد schema.sql واضغط Run.
-- المفروض كل الصفوف يطلع فيها ok = true.

with t(tbl) as (values ('sensor_readings'), ('events'))
select 'الجدول موجود: ' || tbl as check_name,
       exists (select 1 from information_schema.tables
               where table_schema = 'public' and table_name = tbl) as ok
from t
union all
select 'RLS مفعّل: ' || tbl,
       coalesce((select c.relrowsecurity from pg_class c join pg_namespace n on n.oid = c.relnamespace
                 where n.nspname = 'public' and c.relname = tbl), false)
from t
union all
select 'anon يقرأ ويضيف بس (SELECT, INSERT): ' || tbl,
       coalesce((select string_agg(privilege_type, ', ' order by privilege_type)
                 from information_schema.role_table_grants
                 where grantee = 'anon' and table_schema = 'public' and table_name = tbl), '') = 'INSERT, SELECT'
from t
union all
select 'سياستين لـ anon (إضافة وقراءة) وبس: ' || tbl,
       (select count(*) from pg_policies
        where schemaname = 'public' and tablename = tbl and cmd in ('INSERT', 'SELECT') and 'anon' = any (roles)) = 2
       and not exists (select 1 from pg_policies
                       where schemaname = 'public' and tablename = tbl and cmd in ('UPDATE', 'DELETE', 'ALL'))
from t
union all
select 'فهرس على ts: ' || tbl,
       exists (select 1 from pg_indexes
               where schemaname = 'public' and tablename = tbl and indexdef ilike '%(ts%')
from t
union all
select 'Realtime مفعّل: ' || tbl,
       exists (select 1 from pg_publication_tables
               where pubname = 'supabase_realtime' and schemaname = 'public' and tablename = tbl)
from t;
