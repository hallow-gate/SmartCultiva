create table if not exists telemetry(id int primary key default 1 check(id=1), temp_c real, humidity real, soil_pct int, shade text, pump boolean, updated_at timestamptz default now());
create table if not exists commands(
  id uuid primary key default gen_random_uuid(),
  action text not null check(action in('water','cover','uncover')),
  idem_key text unique not null,                       -- one per button click
  status text not null default 'pending' check(status in('pending','sent','done','failed','expired')),
  created_by uuid, created_at timestamptz default now(), sent_at timestamptz, done_at timestamptz);
-- only ONE open command per action: blocks double-clicks / races
create unique index if not exists one_open_cmd on commands(action) where status in('pending','sent');
create table if not exists events(
  id bigint generated always as identity primary key,
  event_id text unique not null,                       -- device-generated, deterministic => idempotent
  type text not null check(type in('water_auto','water_manual','water_skipped','shade_close','shade_open')),
  source text not null, detail jsonb not null default '{}',
  command_id uuid references commands(id),
  occurred_at timestamptz not null default now(), created_at timestamptz default now());
alter table telemetry enable row level security;
alter table commands enable row level security;
alter table events enable row level security;
create policy "read" on telemetry for select to authenticated using(true);
create policy "read" on commands for select to authenticated using(true);
create policy "read" on events for select to authenticated using(true);
-- writes happen only from the Render server (service role key)
