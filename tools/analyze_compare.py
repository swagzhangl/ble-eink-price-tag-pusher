# -*- coding: utf-8 -*-
"""对比官方 0x84 例子数据 与 本地压缩数据，验证压缩格式是否一致"""
import sys, io, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def le32(b, off):
    return b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24)

def fast_read(src, off, bytes_):
    if bytes_ == 4: return le32(src, off)
    if bytes_ == 3: return src[off] | (src[off+1] << 8) | (src[off+2] << 16)
    if bytes_ == 2: return src[off] | (src[off+1] << 8)
    return src[off]

def qlz_size_header(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return 2 * n + 1

def qlz_size_compressed(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return fast_read(src, 1, n)

def qlz_size_decompressed(src):
    n = 4 if (src[0] & 2) == 2 else 1
    return fast_read(src, 1 + n, n)

def parse_packed(data, name):
    """按 packed 格式解析：4B BE 块大小 + QuickLZ 块"""
    print(f'--- {name}: {len(data)}B ---')
    off = 0
    blk_idx = 0
    while off + 4 <= len(data):
        blk_size = (data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3]
        if blk_size == 0 or off + 4 + blk_size > len(data):
            print(f'  [末尾] off={off} 剩余={len(data)-off} 无法成块，break')
            break
        blk = data[off+4:off+4+blk_size]
        flags = blk[0]
        csize = qlz_size_compressed(blk)
        dsize = qlz_size_decompressed(blk)
        print(f'  [块{blk_idx}] 块头={blk_size}B flags=0x{flags:02X} 压缩={csize}B 解压={dsize}B '
              f'数据头={blk[qlz_size_header(blk):qlz_size_header(blk)+8].hex(" ").upper()}')
        off += 4 + blk_size
        blk_idx += 1
    print(f'  总解析: {off}/{len(data)}B, 块数={blk_idx}')
    print()

def extract_hex(text):
    """从文本中提取所有 0xNN 字节"""
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', text))

# ===== 1. 官方 0x84 例子 =====
txt = open(r'../protocol_dump.txt', encoding='utf-8').read()
# 找到 0x84 例子行（含长 hex 串的行）
lines = txt.split('\n')
target = None
for l in lines:
    if '0x84' in l and len(re.findall(r'0x([0-9A-Fa-f]{2})', l)) > 100:
        target = l
        break
if target:
    frame = extract_hex(target)
    print(f'官方 0x84 整帧: {len(frame)}B')
    print(f'  帧头: {frame[:8].hex(" ").upper()}')
    # 帧 = 长度2 + seq1 + cmd1 + filetype1 + pos2 + 数据 + crc2
    frame_len = (frame[0] << 8) | frame[1]
    data_start = 7
    data_end = frame_len - 2
    payload = frame[data_start:data_end]
    print(f'  帧长={frame_len}, 文件类型={frame[4]}, 包位置={(frame[5]<<8)|frame[6]}, 数据={len(payload)}B, CRC={frame[-2:].hex(" ").upper()}')
    parse_packed(payload, '官方 0x84 数据')
else:
    print('!! 未找到官方 0x84 例子行')

# ===== 2. 本地压缩数据 =====
def load_c_array(path):
    txt = open(path, encoding='utf-8').read()
    m = re.search(r'static const uint8_t \w+\[(\d+)\] = \{(.*?)\};', txt, re.S)
    return extract_hex(m.group(2)), int(m.group(1))

for p, n in [(r'../src/compressed_image.h', '本地 compressed_image (type2 正层)'),
             (r'../src/compressed_rev.h', '本地 compressed_rev (type1 反转层)'),
             (r'../src/compressed_allff.h', '本地 compressed_allff (全FF对照)')]:
    try:
        data, sz = load_c_array(p)
        parse_packed(data[:sz], n)
    except Exception as e:
        print(f'{n}: 解析失败 {e}')
