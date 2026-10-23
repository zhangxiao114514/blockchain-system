// cross_verify.js — 用 OpenSSL(Node crypto) 独立验证 C++ 实现的 secp256k1 签名
// 用法: node tests/cross_verify.js
const crypto = require('crypto');
const fs = require('fs');
const { execFileSync } = require('child_process');

function findExe() {
  const cands = ['bin/test_crypto', 'bin/test_crypto.exe',
                 'core/tests/test_crypto.exe', 'core/tests/test_crypto'];
  for (const c of cands) if (fs.existsSync(c)) return c;
  return cands[0];
}
const exe = process.argv[2] || findExe();
const out = execFileSync(exe, { encoding: 'utf8' });
const lines = out.trim().split('\n').map(l => JSON.parse(l));

function derSig(buf) {
  const r = buf.subarray(0, 32), s = buf.subarray(32);
  const enc = (b) => {
    let i = 0;
    while (i < b.length - 1 && b[i] === 0) i++;
    let v = b.subarray(i);
    if (v[0] & 0x80) v = Buffer.concat([Buffer.from([0]), v]);
    return v;
  };
  const R = enc(r), S = enc(s);
  return Buffer.concat([
    Buffer.from([0x30, 4 + R.length + S.length, 0x02, R.length]), R,
    Buffer.from([0x02, S.length]), S
  ]);
}

const spkiPrefix = Buffer.from('3056301006072a8648ce3d020106052b8104000a034200', 'hex');

let ok = 0, fail = 0;
for (const v of lines) {
  if (!v.pub) continue;
  const spki = Buffer.concat([spkiPrefix, Buffer.from([0x04]), Buffer.from(v.pub, 'hex')]);
  let key;
  try {
    key = crypto.createPublicKey({ key: spki, format: 'der', type: 'spki' });
  } catch (e) {
    console.log('SPKI import failed:', e.message);
    fail++; continue;
  }
  const sig = derSig(Buffer.from(v.sig, 'base64'));
  const good = crypto.verify('sha256', Buffer.from(v.msg, 'utf8'), key, sig);
  // 同时验证篡改后必须失败
  const bad = crypto.verify('sha256', Buffer.from(v.msg + 'x', 'utf8'), key, sig);
  if (good && !bad) ok++;
  else { fail++; console.log('FAIL', v.pub.slice(0, 16), 'good=' + good, 'bad=' + bad); }
}

console.log(`OpenSSL 交叉验证: ${ok} 通过, ${fail} 失败`);
process.exit(fail ? 1 : 0);
