import express from 'express';
import crypto from 'crypto';
import { createClient } from '@supabase/supabase-js';

const { SUPABASE_URL, SUPABASE_ANON_KEY, SUPABASE_SERVICE_KEY, DEVICE_API_KEY, ALLOWED_EMAILS = '', PORT = 3000 } = process.env;
const db = createClient(SUPABASE_URL, SUPABASE_SERVICE_KEY, { auth: { persistSession: false } });
const allowed = ALLOWED_EMAILS.split(',').map(s => s.trim().toLowerCase()).filter(Boolean);
const TYPES = ['water_auto', 'water_manual', 'water_skipped', 'shade_close', 'shade_open'];
const SOURCES = ['schedule', 'manual', 'auto_heat'];
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const sha = s => crypto.createHash('sha256').update(String(s)).digest();

const app = express();
app.use(express.json({ limit: '20kb' }));
app.use(express.static('public'));

async function userAuth(req, res, next) {
  const token = (req.headers.authorization || '').replace('Bearer ', '');
  const { data, error } = await db.auth.getUser(token);
  if (error || !data?.user) return res.status(401).json({ error: 'unauthorized' });
  if (allowed.length && !allowed.includes((data.user.email || '').toLowerCase())) return res.status(403).json({ error: 'forbidden' });
  req.user = data.user; next();
}
function deviceAuth(req, res, next) {
  if (!DEVICE_API_KEY || !crypto.timingSafeEqual(sha(req.headers['x-device-key']), sha(DEVICE_API_KEY))) return res.status(401).json({ error: 'unauthorized' });
  next();
}
// commands not picked up within 10 min are dropped so a stale "water" never fires later
const expire = () => db.from('commands').update({ status: 'expired' })
  .in('status', ['pending', 'sent']).lt('created_at', new Date(Date.now() - 600000).toISOString());

app.get('/api/config', (_q, r) => r.json({ url: SUPABASE_URL, key: SUPABASE_ANON_KEY }));

app.get('/api/status', userAuth, async (_q, r) => {
  await expire();
  const [t, e, c] = await Promise.all([
    db.from('telemetry').select('*').eq('id', 1).maybeSingle(),
    db.from('events').select('type,source,detail,occurred_at').order('occurred_at', { ascending: false }).limit(20),
    db.from('commands').select('action,status').in('status', ['pending', 'sent'])]);
  r.json({ telemetry: t.data, events: e.data || [], open: c.data || [] });
});

app.post('/api/commands', userAuth, async (req, res) => {
  const { action, idem_key } = req.body || {};
  if (!['water', 'cover', 'uncover'].includes(action) || typeof idem_key !== 'string' || idem_key.length < 8 || idem_key.length > 64)
    return res.status(400).json({ error: 'bad request' });
  await expire();
  const { data, error } = await db.from('commands')
    .upsert({ action, idem_key, created_by: req.user.id }, { onConflict: 'idem_key', ignoreDuplicates: true }).select().maybeSingle();
  if (error?.code === '23505') return res.json({ duplicate: true });   // another open command exists
  if (error) return res.status(500).json({ error: 'db' });
  res.json({ duplicate: !data, command: data });
});

app.post('/api/device/sync', deviceAuth, async (req, res) => {
  const { telemetry: t, events = [] } = req.body || {};
  if (t) await db.from('telemetry').upsert({ id: 1, temp_c: t.temp_c, humidity: t.humidity, soil_pct: t.soil_pct, shade: t.shade, pump: !!t.pump, updated_at: new Date().toISOString() });
  const rows = (Array.isArray(events) ? events : []).slice(0, 20)
    .filter(e => e && typeof e.event_id === 'string' && e.event_id.length <= 80 && TYPES.includes(e.type) && SOURCES.includes(e.source))
    .map(e => ({ event_id: e.event_id, type: e.type, source: e.source, detail: typeof e.detail === 'object' ? e.detail : {},
      command_id: UUID.test(e.command_id || '') ? e.command_id : null,
      occurred_at: e.ts > 1.7e9 ? new Date(e.ts * 1000).toISOString() : new Date().toISOString() }));
  if (rows.length) {
    const { error } = await db.from('events').upsert(rows, { onConflict: 'event_id', ignoreDuplicates: true });
    if (error) return res.status(500).json({ error: 'db' });          // device keeps its outbox and retries
    for (const r of rows) if (r.command_id)
      await db.from('commands').update({ status: r.detail?.result === 'ok' ? 'done' : 'failed', done_at: new Date().toISOString() })
        .eq('id', r.command_id).in('status', ['pending', 'sent']);
  }
  await expire();
  const stale = new Date(Date.now() - 120000).toISOString();
  const { data: cmds } = await db.from('commands').select('id,action')
    .or(`status.eq.pending,and(status.eq.sent,sent_at.lt.${stale})`).order('created_at').limit(3);
  if (cmds?.length) await db.from('commands').update({ status: 'sent', sent_at: new Date().toISOString() }).in('id', cmds.map(c => c.id));
  res.json({ ok: true, commands: cmds || [] });
});

app.listen(PORT, () => console.log('up', PORT));
