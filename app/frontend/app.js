/*
 * StarTide frontend — talks to the skillctl engine through Tauri invoke.
 * All rendering logic mirrors the approved prototype.
 */
const invoke = window.__TAURI__?.core?.invoke;

const AC = ['','#2f7dd1','#3d9a5f','#c98a2e','#c25b7c','#2f9a8f','#d0713f','#4a6fa5','#7c8fa6','#b05a5a','#2f8f8a'];
const HUES = [210,145,38,340,172,24,222,205,2,176];

let skills = [], agents = [];
const S = { q:'', agent:null, tab:'all', sel:null, view:'grid', busy:false, armed:false };

const aColor = id => { const i = agents.findIndex(a=>a.id===id); return i<0 ? '#666' : AC[(i+1)%AC.length]; };
const hue = id => { const i = agents.findIndex(a=>a.id===id); return i<0 ? 210 : HUES[(i+1)%HUES.length]; };
const aLabel = id => (agents.find(a=>a.id===id)||{}).label || id;
const fmt = b => b>1048576 ? (b/1048576).toFixed(1)+' MB' : b>1024 ? (b/1024).toFixed(1)+' KB' : b+' B';
const esc = s => (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;');

/* ---------- engine access ---------- */

async function engine(cmd, args=[]) {
  if (!invoke) throw new Error('请在 StarTide 应用中运行（浏览器里无法访问引擎）');
  return invoke('cli', { cmd, args });
}

async function refresh() {
  try {
    const [cat, st] = await Promise.all([
      engine('catalog', ['--json']),
      engine('status', ['--json']),
    ]);
    skills = cat.trim().split('\n').filter(Boolean).map(l => JSON.parse(l));
    const status = JSON.parse(st);
    agents = status.agents || [];
    if (S.sel != null && S.sel >= skills.length) S.sel = null;
    setStatus(`skillctl · ${skills.length} 技能 · ${agents.length} agent`, '');
  } catch (e) {
    skills = []; agents = [];
    setStatus(`引擎错误：${String(e).split('\n')[0]}`, 'err');
  }
  renderAll();
}

function setStatus(text, kind) {
  const el = document.getElementById('st');
  el.textContent = text;
  el.style.color = kind==='err' ? '#ffd7d3' : '';
}

/* ---------- actions (toolbar) ---------- */

const ARMABLE = { unlink: '再点一次确认还原所有 agent 的原始目录' };

async function runCommand(cmd) {
  if (S.busy) return;
  if (ARMABLE[cmd] && !S.armed) {
    S.armed = true;
    setStatus(ARMABLE[cmd], 'warn');
    setTimeout(() => { S.armed = false; }, 4000);
    return;
  }
  S.armed = false;
  S.busy = true;
  setStatus(`⟳ ${cmd} 执行中…`);
  const args = cmd==='link' ? ['--dry-run','--json','--scope', S.agent ? 'all' : 'own'] : ['--json'];
  if (cmd==='gc') args.unshift('--dry-run');
  try {
    const out = await engine(cmd, args);
    const note = summarize(cmd, out);
    setStatus(note || `${cmd} 完成`);
  } catch (e) {
    setStatus(`${cmd} 失败：${String(e).split('\n')[0]}`, 'err');
  }
  S.busy = false;
  await refresh();
}

function summarize(cmd, out) {
  try {
    const v = JSON.parse(out);
    if (cmd==='scan') return `扫描完成：${v.stats?.units ?? '?'} 个技能目录，${v.stats?.unique ?? '?'} 个内容唯一`;
    if (cmd==='link') return `链接预览：${v.skills ?? '?'} 个技能（${v.imported ?? 0} 新导入）`;
    if (cmd==='sync') return `同步完成：${v.imported ?? 0} 个新导入`;
    if (cmd==='gc') return `清理预览：${v.removed ?? 0} 个未引用单元`;
    if (cmd==='unlink') return `已还原 ${v.restored ?? '?'} 个 agent 目录`;
  } catch {}
  return '';
}

/* ---------- filtering ---------- */

function vis() {
  const q = S.q.toLowerCase();
  return skills.filter(s =>
    (!q || s.name.toLowerCase().includes(q) || (s.description||'').toLowerCase().includes(q)) &&
    (!S.agent || (s.agents||[]).includes(S.agent)) &&
    (S.tab==='all' || (S.tab==='in')===s.stored));
}

/* ---------- left: agent list ---------- */

function renderAgents() {
  const v = vis();
  let h = `<div class="grp">AGENTS <span class="cnt">${agents.length}</span></div>`;
  h += `<div class="arow ${!S.agent?'sel':''}" data-a="">
    <span class="adot" style="background:#4fc3f7"></span>
    <span class="an">全部</span><span class="ac">${v.length}</span></div>`;
  agents.forEach(a => {
    const n = v.filter(s => (s.agents||[]).includes(a.id)).length;
    h += `<div class="arow ${S.agent===a.id?'sel':''}" data-a="${a.id}">
      <span class="adot" style="background:${aColor(a.id)}"></span>
      <span class="an">${esc(a.label||a.id)}</span><span class="ac">${n}</span></div>`;
  });
  document.getElementById('agents').innerHTML = h;
  document.querySelectorAll('.arow[data-a]').forEach(r => r.onclick = () => {
    S.agent = r.dataset.a || null; S.sel = null; renderAll();
  });
}

/* ---------- centre: cards / list ---------- */

function dotRow(s) {
  const dots = agents.slice(0,5).map(a =>
    `<i style="background:${(s.agents||[]).includes(a.id)?aColor(a.id):'#3a3a3a'}" title="${esc(aLabel(a.id))}"></i>`).join('');
  return dots;
}

function renderCards(v) {
  const g = document.getElementById('grid');
  g.className = 'grid';
  if (!v.length) { g.innerHTML = `<div class="empty"><div class="t">没有匹配的技能</div></div>`; return; }
  g.innerHTML = v.map((s,i) => {
    const key = S.agent || (s.agents||[])[0] || 'x';
    const gcol = `linear-gradient(135deg,hsl(${hue(key)} 42% 42%),hsl(${hue(key)} 48% 30%))`;
    return `<div class="card ${S.sel===i?'sel':''}" data-i="${i}">
      <div class="chead">
        <div class="glyph" style="background:${gcol}">${esc(s.name[0]||'?').toUpperCase()}</div>
        <div class="ct">
          <div class="cname">${esc(s.name)}</div>
          <div class="cmeta"><span class="cst ${s.stored?'in':'out'}">${s.stored?'已入库':'仅目录'}</span><span>${(s.agents||[]).length} agent</span></div>
        </div>
      </div>
      <div class="cdesc">${esc(s.description)}</div>
      <div class="cfoot"><div class="dots">${dotRow(s)}</div><span class="csize">${fmt(s.size)}</span></div>
    </div>`;
  }).join('');
  g.querySelectorAll('.card').forEach(c => c.onclick = () => {
    S.sel = +c.dataset.i; renderAll();
    const el = g.querySelector('.card.sel'); if (el) el.scrollIntoView({block:'nearest'});
  });
}

function renderList(v) {
  const g = document.getElementById('grid');
  g.className = 'rows';
  if (!v.length) { g.innerHTML = `<div class="empty"><div class="t">没有匹配的技能</div></div>`; return; }
  g.innerHTML = v.map((s,i) => {
    const key = (s.agents||[])[0] || 'x';
    const gcol = `linear-gradient(135deg,hsl(${hue(key)} 42% 42%),hsl(${hue(key)} 48% 30%))`;
    return `<div class="lrow ${S.sel===i?'sel':''}" data-i="${i}">
      <div class="ldot" style="background:${gcol}">${esc(s.name[0]||'?').toUpperCase()}</div>
      <div class="lmid">
        <div class="lname">${esc(s.name)}</div>
        <div class="ldesc">${esc(s.description)}</div>
      </div>
      <div class="lright">
        <div class="dots">${dotRow(s)}</div>
        <span class="lsize">${fmt(s.size)}</span>
        <span class="cst ${s.stored?'in':'out'}">${s.stored?'已入库':'仅目录'}</span>
      </div>
    </div>`;
  }).join('');
  g.querySelectorAll('.lrow').forEach(r => r.onclick = () => {
    S.sel = +r.dataset.i; renderAll();
    const el = g.querySelector('.lrow.sel'); if (el) el.scrollIntoView({block:'nearest'});
  });
}

function renderGrid() {
  const v = vis();
  document.getElementById('crumb').innerHTML =
    `${S.agent ? `<b style="color:${aColor(S.agent)}">●</b>&nbsp;<b>${esc(aLabel(S.agent))}</b><span>·</span>` : ''}<b>${v.length}</b> 个技能`;
  if (S.view==='list') renderList(v); else renderCards(v);
}

/* ---------- right: detail (opens on click) ---------- */

function renderDetail() {
  const d = document.getElementById('detail'), inner = document.getElementById('d-inner');
  const s = vis()[S.sel];
  d.classList.toggle('open', !!s);
  if (!s) { inner.innerHTML = ''; return; }
  const pills = (s.agents||[]).length
    ? s.agents.map(a => `<span class="vp"><i style="background:${aColor(a)}"></i>${esc(aLabel(a))}</span>`).join('')
    : `<span style="font-size:11px;color:var(--faint)">仅 scope=all 时可见</span>`;
  const srcs = (s.sources||[]).slice(0,4).map((p,j) =>
    `<div class="srcrow"><code title="${esc(p)}">${esc(p)}</code><a data-copy="${esc(p)}">复制</a>${j===0?`<a data-open="${esc(p)}">显示</a>`:''}</div>`).join('');
  inner.innerHTML = `
    <div class="d-close" id="d-close"><svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 6 6 18M6 6l12 12"/></svg></div>
    <div class="d-head">
      <div class="d-kick">
        <span class="cst ${s.stored?'in':'out'}" style="font-size:10px;padding:2px 8px">${s.stored?'已入库':'仅目录'}</span>
        <span style="font-family:var(--mono);font-size:10px;color:var(--faint)">${esc((s.id||'').slice(0,10))}…</span>
      </div>
      <div class="d-title">${esc(s.name)}</div>
      <div class="d-desc">${esc(s.description)||'暂无描述'}</div>
    </div>
    <div class="d-sec"><div class="d-lab">可见 agent</div><div class="pillrow">${pills}</div></div>
    <div class="d-sec"><div class="d-lab">元信息</div>
      <div class="kv"><b>大小</b><span>${fmt(s.size)}</span></div>
      <div class="kv"><b>磁盘副本</b><span>${(s.sources||[]).length} 份</span></div>
      <div class="kv"><b>哈希</b><span>${esc((s.id||'').slice(0,16))}</span></div>
    </div>
    <div class="d-sec"><div class="d-lab">来源位置</div>${srcs}</div>`;
  document.getElementById('d-close').onclick = closeDetail;
  inner.querySelectorAll('[data-copy]').forEach(a => a.onclick = () => {
    navigator.clipboard?.writeText(a.dataset.copy);
    setStatus('已复制路径');
  });
  inner.querySelectorAll('[data-open]').forEach(a => a.onclick = () => {
    if (invoke) invoke('reveal', { path: a.dataset.open }).catch(()=>{});
    setStatus('已在文件管理器中显示');
  });
}

function closeDetail() { S.sel = null; renderAll(); }

/* ---------- render all ---------- */

function renderAll() {
  renderAgents();
  renderGrid();
  renderDetail();
  document.getElementById('st-scope').textContent =
    `${S.tab==='all'?'全部':S.tab==='in'?'已入库':'仅目录'}${S.agent?' · '+aLabel(S.agent):''}`;
}

/* ---------- bindings ---------- */

document.getElementById('q').oninput = e => { S.q = e.target.value; S.sel = null; renderAll(); };
document.querySelectorAll('.tab').forEach(t => t.onclick = () => {
  document.querySelectorAll('.tab').forEach(x => x.classList.remove('on'));
  t.classList.add('on'); S.tab = t.dataset.t; S.sel = null; renderAll();
});
document.querySelectorAll('.vbtn').forEach(b => b.onclick = () => {
  document.querySelectorAll('.vbtn').forEach(x => x.classList.remove('on'));
  b.classList.add('on'); S.view = b.dataset.v; renderAll();
});
document.querySelectorAll('.tbtn[data-cmd]').forEach(b => b.onclick = () => runCommand(b.dataset.cmd));

document.addEventListener('keydown', e => {
  if (e.key === 'Escape') {
    if (S.sel != null) { closeDetail(); }
    else { document.getElementById('q').value = ''; S.q = ''; renderAll(); }
  }
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const v = vis(); if (!v.length) return;
    const cur = S.sel == null ? -1 : S.sel;
    S.sel = e.key === 'ArrowDown' ? Math.min(cur+1, v.length-1) : Math.max(cur-1, 0);
    renderAll();
    const el = document.querySelector('.card.sel, .lrow.sel');
    if (el) el.scrollIntoView({block:'nearest'});
  }
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault(); document.getElementById('q').focus(); document.getElementById('q').select();
  }
});

refresh();
