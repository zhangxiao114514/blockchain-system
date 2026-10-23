/* app.js — 控制台前端逻辑（纯原生 JS，无外部依赖） */
'use strict';

const LS_NODE = 'bc.console.node';
const state = {
  base: localStorage.getItem(LS_NODE) || location.origin,
  status: null,
  blocks: [],
  mempool: [],
  wallets: [],
  peers: [],
  nodes: [],
  open: new Set(),
  sig: '',
  timer: null,
  busy: false,
};

/* ---------------- 基础工具 ---------------- */
const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
function shortHash(h, head = 12, tail = 6) {
  if (!h) return '–';
  return h.length > head + tail ? `${h.slice(0, head)}…${h.slice(-tail)}` : h;
}
function shortAddr(a) {
  if (!a) return '系统';
  return a.length > 14 ? `${a.slice(0, 8)}…${a.slice(-6)}` : a;
}
function fmtNum(n) {
  return Number(n || 0).toLocaleString('zh-CN');
}
function ago(ms) {
  if (ms == null) return '–';
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 1) return '刚刚';
  if (s < 60) return `${s} 秒前`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} 分钟前`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} 小时前`;
  return `${Math.floor(h / 24)} 天前`;
}
let toastTimer = null;
function toast(msg, kind = '') {
  const el = $('toast');
  el.textContent = msg;
  el.className = `toast show ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.className = 'toast'), 3200);
}
function banner(msg) {
  const el = $('banner');
  if (!msg) { el.classList.add('hidden'); return; }
  el.textContent = msg;
  el.classList.remove('hidden');
}

async function api(path, opts = {}) {
  const url = state.base.replace(/\/$/, '') + path;
  const init = { cache: 'no-store', ...opts };
  if (init.body && typeof init.body !== 'string') {
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(init.body);
  }
  const res = await fetch(url, init);
  const data = await res.json().catch(() => ({ ok: false, error: '响应解析失败' }));
  if (!res.ok && data.error) throw new Error(data.error);
  return data;
}

/* ---------------- 渲染 ---------------- */
function renderStatus(s) {
  const c = s.chain;
  $('kpi-height').textContent = fmtNum(c.height);
  $('kpi-tip').textContent = shortHash(c.tip_hash, 8, 6);
  $('kpi-tip').title = c.tip_hash;
  $('kpi-difficulty').textContent = c.difficulty;
  $('kpi-nextdiff').textContent = `下一块 ${c.next_difficulty}`;
  $('kpi-mempool').textContent = fmtNum(c.mempool);
  $('kpi-txs').textContent = fmtNum(c.total_txs);
  $('kpi-accounts').textContent = `${fmtNum(c.accounts)} 个账户`;
  $('kpi-work').textContent = fmtNum(c.total_work);
  $('kpi-peers').textContent = s.peer_count;
  $('kpi-nodeid').textContent = s.node_id;
  $('automine').checked = !!s.auto_mine;
  $('foot-genesis').textContent = `创世块 ${c.genesis_hash}`;
  $('conn-dot').className = 'conn-dot on';
}

function renderBlocks(blocks) {
  const el = $('blocks');
  const top = el.scrollTop;
  el.innerHTML = blocks.map((b) => {
    const isGenesis = b.index === 0;
    const open = state.open.has(b.index);
    const txs = b.transactions || [];
    const rows = txs.map((t) => {
      const coin = t.type === 'coinbase';
      const label = { coinbase: '出块奖励', transfer: '转账', deploy_contract: '部署合约',
                      contract_call: '调用合约', cross_transfer: '跨链' }[t.type] || t.type;
      const cls = coin ? 'coin' : (t.type === 'transfer' ? 'xfer' : 'contract');
      return `<div class="txrow">
        <span class="tag ${cls}">${label}</span>
        <span class="txaddr" title="${esc(coin ? t.to : t.from + ' → ' + t.to)}">
          ${coin ? '系统 → ' + shortAddr(t.to) : shortAddr(t.from) + ' → ' + shortAddr(t.to)}
        </span>
        <span class="txamt">${esc(t.amount_str || '0')} <small>币</small></span>
        <span class="txaddr" title="${esc(t.id)}">${shortHash(t.id, 8, 4)}</span>
      </div>`;
    }).join('');
    return `<div class="block ${open ? 'open' : ''}" data-idx="${b.index}">
      <div class="block-row">
        <div class="block-index ${isGenesis ? 'genesis' : ''}">#${b.index}</div>
        <div class="block-mid">
          <div class="block-hash" title="${esc(b.hash)}">${esc(shortHash(b.hash, 22, 8))}</div>
          <div class="block-meta">
            <span>难度 <b>${b.difficulty}</b></span>
            <span>nonce <b>${fmtNum(b.nonce)}</b></span>
            <span>${txs.length} 笔交易</span>
            <span>${ago(b.age_ms)}</span>
          </div>
        </div>
        <div class="block-right">
          <span class="pill">${esc(shortHash(b.merkle_root, 6, 4))}</span>
          <span>${open ? '收起' : '展开'}</span>
        </div>
      </div>
      <div class="block-txs">${rows || '<div class="empty">该区块没有交易</div>'}</div>
    </div>`;
  }).join('');
  el.scrollTop = top;
  $('blocks-sub').textContent = `共 ${fmtNum(state.status?.chain.height ?? 0)} 个区块 · 点击可展开交易明细`;
}

function renderWallets(list) {
  const el = $('wallets');
  const opts = [];
  if (!list.length) {
    el.innerHTML = '<div class="empty">还没有钱包，点击右上角「新建」</div>';
  } else {
    el.innerHTML = list.map((w) => {
      opts.push(`<option value="${esc(w.address)}">${esc(w.label)} · ${esc(w.balance_str)} 币</option>`);
      return `<div class="wallet">
        <div class="wallet-main">
          <div class="wallet-addr" data-copy="${esc(w.address)}" title="点击复制地址">${esc(shortAddr(w.address))}</div>
          <div class="wallet-label">
            <span>${esc(w.label)}</span>
            ${w.is_miner ? '<span class="miner-chip">矿工</span>' : ''}
            <span>nonce ${w.nonce}</span>
          </div>
        </div>
        <div class="wallet-bal">
          <b>${esc(w.balance_str)}</b>
          <span>币</span>
        </div>
        <div class="wallet-actions">
          <button class="btn btn-mini" data-faucet="${esc(w.address)}">领币</button>
          ${w.is_miner ? '' : `<button class="btn btn-mini btn-danger" data-del="${esc(w.address)}" title="删除钱包">×</button>`}
        </div>
      </div>`;
    }).join('');
  }
  const sel = $('tx-from');
  const prev = sel.value;
  sel.innerHTML = opts.join('') || '<option value="">（无可用钱包）</option>';
  if (prev && list.some((w) => w.address === prev)) sel.value = prev;
  el.querySelectorAll('[data-copy]').forEach((n) => {
    n.onclick = () => {
      navigator.clipboard?.writeText(n.dataset.copy);
      toast('地址已复制');
    };
  });
  el.querySelectorAll('[data-del]').forEach((n) => {
    n.onclick = async (e) => {
      e.stopPropagation();
      const addr = n.dataset.del;
      if (!window.confirm(`确定删除钱包 ${shortAddr(addr)}？\n私钥将从本节点删除，余额不会退回，操作不可恢复。`)) return;
      n.disabled = true;
      try {
        await api('/api/wallets/delete', { method: 'POST', body: { address: addr } });
        toast('钱包已删除');
        state.sig = '';
        refresh();
      } catch (err) {
        toast(err.message, 'err');
        n.disabled = false;
      }
    };
  });
  el.querySelectorAll('[data-faucet]').forEach((n) => {
    n.onclick = async (e) => {
      e.stopPropagation();
      n.disabled = true;
      try {
        await api('/api/faucet', { method: 'POST', body: { address: n.dataset.faucet, amount: '100' } });
        toast('领币交易已提交，正在出块确认…', 'ok');
        quickRefresh(8);
      } catch (err) {
        toast(err.message, 'err');
      } finally {
        n.disabled = false;
      }
    };
  });
}

function renderMempool(txs) {
  $('mempool-count').textContent = txs.length;
  const el = $('mempool');
  if (!txs.length) {
    el.innerHTML = '<div class="empty">暂无待确认交易</div>';
    return;
  }
  el.innerHTML = txs.map((t) => `<div class="memitem">
    <span class="who" title="${esc(t.id)}">${esc(shortAddr(t.from))} → ${esc(shortAddr(t.to))}</span>
    <span class="amt">${esc(t.amount_str)}</span>
  </div>`).join('');
}

function renderPeers(peers, status) {
  const el = $('peers');
  const rows = [];
  rows.push(`<div class="peer"><span class="peer-id"><i class="peer-dot"></i>${esc(status.node_id)}</span>
    <span class="peer-h">本节点 · ${fmtNum(status.chain.height)}</span></div>`);
  peers.forEach((p) => {
    rows.push(`<div class="peer"><span class="peer-id"><i class="peer-dot"></i>${esc(p.node_id || p.p2p_port)}</span>
      <span class="peer-h">${fmtNum(p.height)} · :${p.p2p_port}</span></div>`);
  });
  el.innerHTML = rows.join('');
}

function renderNodes(nodes) {
  const sel = $('node-select');
  const cur = new URL(state.base).port || '3001';
  const list = nodes.length ? nodes : [{ node_id: 'node', http_port: cur, p2p_port: '-', self: true }];
  sel.innerHTML = list.map((n) =>
    `<option value="${n.http_port}" ${String(n.http_port) === String(cur) ? 'selected' : ''}>
      ${esc(n.node_id)} :${n.http_port}${n.self ? '（本节点）' : ''}
    </option>`).join('');
}

/* ---------------- 数据刷新 ---------------- */
async function refresh() {
  if (state.busy) return;
  state.busy = true;
  try {
    const [status, blocks, mempool, wallets, peers, net] = await Promise.all([
      api('/api/status'),
      api('/api/blocks?limit=18'),
      api('/api/mempool'),
      api('/api/wallets'),
      api('/api/peers'),
      api('/api/network').catch(() => ({ ok: true, nodes: [] })),
    ]);
    state.status = status;
    state.blocks = blocks.blocks || [];
    state.mempool = mempool.txs || [];
    state.wallets = wallets.wallets || [];
    state.peers = peers.peers || [];
    state.nodes = net.nodes || [];
    banner('');
    renderStatus(status);
    const sig = `${status.chain.height}|${state.blocks[0]?.hash || ''}|${state.mempool.length}|${state.wallets.map((w) => w.balance_str).join(',')}`;
    if (sig !== state.sig) {
      state.sig = sig;
      renderBlocks(state.blocks);
    }
    renderWallets(state.wallets);
    renderMempool(state.mempool);
    renderPeers(state.peers, status);
    renderNodes(state.nodes);
  } catch (e) {
    $('conn-dot').className = 'conn-dot off';
    banner(`无法连接节点 ${state.base}：${e.message}`);
  } finally {
    state.busy = false;
  }
}

/** 提交交易后短时间内高频刷新，让余额几乎立刻更新 */
function quickRefresh(times = 6) {
  let n = 0;
  const t = setInterval(() => {
    refresh();
    if (++n >= times) clearInterval(t);
  }, 300);
}

function startPolling() {
  clearInterval(state.timer);
  state.timer = setInterval(refresh, 1500);
}

/* ---------------- 交互 ---------------- */
function bind() {
  $('blocks').addEventListener('click', (e) => {
    const row = e.target.closest('.block-row');
    if (!row) return;
    const block = row.parentElement;
    const idx = Number(block.dataset.idx);
    if (state.open.has(idx)) state.open.delete(idx);
    else state.open.add(idx);
    block.classList.toggle('open');
  });

  $('node-select').addEventListener('change', (e) => {
    const port = e.target.value;
    state.base = `http://127.0.0.1:${port}`;
    localStorage.setItem(LS_NODE, state.base);
    state.sig = '';
    toast(`已切换到节点 :${port}`);
    refresh();
  });

  $('automine').addEventListener('change', async (e) => {
    try {
      const r = await api('/api/automine', { method: 'POST', body: { enabled: e.target.checked } });
      toast(r.auto_mine ? '已开启自动挖矿' : '已关闭自动挖矿', 'ok');
    } catch (err) {
      toast(err.message, 'err');
      e.target.checked = !e.target.checked;
    }
  });

  $('btn-mine').addEventListener('click', async (e) => {
    const btn = e.currentTarget;
    btn.disabled = true;
    btn.textContent = '挖矿中…';
    try {
      const r = await api('/api/mine', { method: 'POST', body: { count: 1 } });
      toast(`出块成功，高度 ${r.height}`, 'ok');
      state.sig = '';
      refresh();
    } catch (err) {
      toast(err.message, 'err');
    } finally {
      btn.disabled = false;
      btn.textContent = '立即出块';
    }
  });

  $('btn-validate').addEventListener('click', async (e) => {
    const btn = e.currentTarget;
    btn.disabled = true;
    try {
      const r = await api('/api/validate');
      const el = $('foot-verify');
      if (r.valid) {
        el.className = 'ok';
        el.textContent = `✓ 全链校验通过（${fmtNum(r.height)} 个区块）`;
        toast('全链校验通过', 'ok');
      } else {
        el.className = 'bad';
        el.textContent = `✗ 校验失败：${r.error}`;
        toast(`校验失败：${r.error}`, 'err');
      }
    } catch (err) {
      toast(err.message, 'err');
    } finally {
      btn.disabled = false;
    }
  });

  $('btn-new-wallet').addEventListener('click', async (e) => {
    const btn = e.currentTarget;
    btn.disabled = true;
    try {
      const r = await api('/api/wallets', { method: 'POST', body: { label: '我的钱包' } });
      toast(`已创建钱包 ${shortAddr(r.wallet.address)}`, 'ok');
      refresh();
    } catch (err) {
      toast(err.message, 'err');
    } finally {
      btn.disabled = false;
    }
  });

  $('transfer-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const btn = $('btn-send');
    const body = {
      from: $('tx-from').value,
      to: $('tx-to').value.trim(),
      amount: $('tx-amount').value.trim(),
      fee: $('tx-fee').value.trim() || '0',
    };
    if (!body.from) return toast('请先创建钱包', 'err');
    if (!/^0x[0-9a-fA-F]{40}$/.test(body.to)) return toast('收款地址格式不正确', 'err');
    btn.disabled = true;
    btn.textContent = '签名中…';
    try {
      const r = await api('/api/tx', { method: 'POST', body });
      toast(`交易已广播，正在出块确认…`, 'ok');
      $('tx-amount').value = '';
      $('tx-to').value = '';
      state.sig = '';
      quickRefresh(8);
    } catch (err) {
      toast(err.message, 'err');
    } finally {
      btn.disabled = false;
      btn.textContent = '签名并广播';
    }
  });
}

/* ---------------- 启动 ---------------- */
(async function boot() {
  bind();
  await refresh();
  startPolling();
})();
