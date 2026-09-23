// 解压官方 0x84 例子块0 + 本地数据，渲染点阵，对比格式
const fs = require('fs');

// ===== QuickLZ 1.5 level3 解压（移植自 qlz_py_decompress.py）=====
function le32(b, off) { return b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24); }
function fastRead(src, off, n) {
  if (n === 4) return le32(src, off);
  if (n === 3) return src[off] | (src[off+1] << 8) | (src[off+2] << 16);
  if (n === 2) return src[off] | (src[off+1] << 8);
  return src[off];
}
function qlzSizeHeader(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return 2*n + 1; }
function qlzSizeCompressed(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return fastRead(src, 1, n); }
function qlzSizeDecompressed(src) { const n = (src[0] & 2) === 2 ? 4 : 1; return fastRead(src, 1+n, n); }

const BITLUT = [4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0];
const UNCONDITIONAL_MATCHLEN = 6, UNCOMPRESSED_END = 4, CWORD_LEN = 4;

function qlzDecompressCore(source, size) {
  let src = qlzSizeHeader(source);
  const dst = [];
  const lastDestinationByte = size - 1;
  let cwordVal = 1;
  const lastMatchstart = lastDestinationByte - UNCONDITIONAL_MATCHLEN - UNCOMPRESSED_END;
  const lastSourceByte = qlzSizeCompressed(source) - 1;
  const nData = source.length;

  while (true) {
    if (cwordVal === 1) {
      if (src + CWORD_LEN - 1 > lastSourceByte) break;
      cwordVal = fastRead(source, src, CWORD_LEN);
      src += CWORD_LEN;
    }
    if (src + 4 - 1 > lastSourceByte) break;
    const fetch = fastRead(source, src, 4);

    if ((cwordVal & 1) === 1) {
      // 匹配分支
      cwordVal = cwordVal >> 1;
      let offset, matchlen;
      if ((fetch & 3) === 0) { offset = (fetch & 0xff) >> 2; matchlen = 3; src += 1; }
      else if ((fetch & 2) === 0) { offset = (fetch & 0xffff) >> 2; matchlen = 3; src += 2; }
      else if ((fetch & 1) === 0) { offset = (fetch & 0xffff) >> 6; matchlen = ((fetch >> 2) & 15) + 3; src += 2; }
      else if ((fetch & 127) !== 3) { offset = (fetch >> 7) & 0x1ffff; matchlen = ((fetch >> 2) & 0x1f) + 2; src += 3; }
      else { offset = fetch >> 15; matchlen = ((fetch >> 7) & 255) + 3; src += 4; }
      const offset2 = dst.length - offset;
      if (offset2 < 0) return null;
      for (let i = 0; i < matchlen; i++) dst.push(dst[offset2 + i]);
    } else {
      // 字面量
      if (dst.length < lastMatchstart) {
        let n = BITLUT[cwordVal & 0xf];
        if (n === 0) n = 1;
        for (let i = 0; i < n && src + i < nData; i++) dst.push(source[src + i]);
        cwordVal = cwordVal >> n;
        src += n;
      } else {
        while (dst.length <= lastDestinationByte) {
          if (cwordVal === 1) { src += CWORD_LEN; cwordVal = 1 << 31; }
          if (src >= nData) break;
          dst.push(source[src]); src += 1;
          cwordVal = cwordVal >> 1;
        }
        break;
      }
    }
    if (dst.length >= size) break;
  }
  return Buffer.from(dst.slice(0, size));
}

function decompressPacked(data, expectTotal) {
  // packed: 4B BE 块大小 + QuickLZ 块，循环
  let off = 0; const blocks = [];
  while (off + 4 <= data.length) {
    const blkSize = (data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3];
    if (blkSize === 0 || off + 4 + blkSize > data.length) break;
    const blk = data.subarray(off+4, off+4+blkSize);
    const dsize = qlzSizeDecompressed(blk);
    const out = qlzDecompressCore(blk, dsize);
    if (!out) { blocks.push(null); }
    else blocks.push(out);
    off += 4 + blkSize;
  }
  const total = blocks.filter(Boolean).reduce((s, b) => s + b.length, 0);
  const ok = total === expectTotal;
  return { blocks, total, ok };
}

// ===== 渲染 =====
// fmt: 'row' = 行序(37B/行)  'col' = 列序(16B/列)  'rowlsb' = 行序 LSB
function renderAscii(buf, w, h, mode) {
  const bytesPerRow = Math.ceil(w / 8);
  let out = '';
  for (let y = 0; y < h; y++) {
    let line = '';
    for (let x = 0; x < w; x++) {
      let bit;
      if (mode === 'row') { const b = buf[y * bytesPerRow + (x >> 3)]; bit = (b >> (7 - (x & 7))) & 1; }
      else if (mode === 'rowlsb') { const b = buf[y * bytesPerRow + (x >> 3)]; bit = (b >> (x & 7)) & 1; }
      else if (mode === 'col') { const b = buf[(x >> 3) * 16 * 16 + y * 16 + (x & 7)]; bit = (b >> (7 - (x & 7))) & 1; }
      line += bit ? '#' : '.';
    }
    out += line + '\n';
  }
  return out;
}

// ===== 1. 官方例子块0 =====
const txt = fs.readFileSync('../protocol_dump.txt', 'utf-8');
const m = txt.match(/蓝牙发\s+((?:[0-9A-Fa-f]{2}\s+)+)/);
const frame = Buffer.from(m[1].trim().split(/\s+/).map(h => parseInt(h, 16)));
const payload = frame.subarray(7, (frame[0] << 8) | frame[1] - 2);
const blkSize = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
const blk0 = payload.subarray(4, 4 + blkSize);
const dsize0 = qlzSizeDecompressed(blk0);
const out0 = qlzDecompressCore(blk0, dsize0);
console.log(`官方块0: 压缩=${blkSize}B 解压=${dsize0}B`);
if (!out0) { console.log('解压失败!'); process.exit(1); }
console.log('解压成功, 内容统计:');
const cnt0 = {}; for (const b of out0) cnt0[b] = (cnt0[b] || 0) + 1;
const zeros0 = out0.filter(b => b === 0).length;
const ff0 = out0.filter(b => b === 0xFF).length;
console.log(`  0x00: ${zeros0}, 0xFF: ${ff0}, 其他: ${dsize0 - zeros0 - ff0}`);
// 非零字节位置分布
const nz = [];
out0.forEach((b, i) => { if (b !== 0 && b !== 0xFF) nz.push(i); });
if (nz.length) {
  console.log(`  其他字节首次出现: idx ${nz.slice(0, 20).join(',')}`);
}

// 按行序 37B/行渲染（296x128 图片的前 2048B = 前 55.3 行，只渲染 55 行完整）
console.log('\n=== 官方点阵 行序MSB 前 296x55 ===');
console.log(renderAscii(out0.subarray(0, 55 * 37), 296, 55, 'row'));
// 列序渲染尝试: 296 列 x 128 行，列序时每列 16B，2048B = 128 列
console.log('\n=== 官方点阵 列序MSB 前 128x128 (128列) ===');
console.log(renderAscii(out0.subarray(0, 2048), 128, 128, 'col'));

// ===== 2. 本地数据解压验证 =====
function loadCArray(p) {
  const t = fs.readFileSync(p, 'utf-8');
  const mm = t.match(/static const uint8_t \w+\[(\d+)\] = \{(.*?)\};/s);
  const re = /0x([0-9A-Fa-f]{2})/g; const arr = []; let x;
  while ((x = re.exec(mm[2])) !== null) arr.push(parseInt(x[1], 16));
  return Buffer.from(arr);
}
const img = loadCArray('../src/compressed_image.h');
const r = decompressPacked(img, 4736);
console.log(`\n本地 compressed_image: ${r.ok ? '✅ 解压成功 4736B' : '❌ 失败 ' + r.total + 'B'}`);
if (r.ok) {
  const rd = Buffer.concat(r.blocks);
  fs.writeFileSync('../tools/decompressed_image.bin', rd);
  console.log('已保存 tools/decompressed_image.bin');
  // 渲染前 55 行
  console.log('\n=== 本地点阵 行序MSB 前 296x55 (应=取模图像) ===');
  console.log(renderAscii(rd.subarray(0, 55 * 37), 296, 55, 'row'));
  // 与 image_data.h 对比
  const raw = loadCArray('../src/image_data.h');
  const same = rd.equals(raw);
  console.log(`解压结果 与 image_data.h 完全一致: ${same}`);
}
