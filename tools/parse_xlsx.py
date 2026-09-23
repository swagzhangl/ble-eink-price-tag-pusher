# -*- coding: utf-8 -*-
"""解析电子墨水屏标签协议.xlsx → 输出纯文本"""
import zipfile, re, sys, io
from xml.etree import ElementTree as ET

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

path = r"C:/Users/liang.zhang/Downloads/电子墨水屏标签协议.xlsx"
NS = '{http://schemas.openxmlformats.org/spreadsheetml/2006/main}'

z = zipfile.ZipFile(path)
# 共享字符串表
ss = []
if 'xl/sharedStrings.xml' in z.namelist():
    root = ET.fromstring(z.read('xl/sharedStrings.xml'))
    for si in root.findall(NS + 'si'):
        text = ''.join(t.text or '' for t in si.iter(NS + 't'))
        ss.append(text)

# 所有 sheet
sheets = sorted([n for n in z.namelist() if re.match(r'xl/worksheets/sheet\d+\.xml', n)],
                key=lambda n: int(re.search(r'sheet(\d+)', n).group(1)))
for sn in sheets:
    print('=' * 30)
    print('SHEET:', sn)
    print('=' * 30)
    root = ET.fromstring(z.read(sn))
    for row in root.iter(NS + 'row'):
        cells = []
        for c in row.iter(NS + 'c'):
            v = c.find(NS + 'v')
            if v is None or v.text is None:
                cells.append('')
                continue
            if c.get('t') == 's':
                idx = int(v.text)
                cells.append(ss[idx] if idx < len(ss) else '?')
            else:
                cells.append(v.text)
        # 去掉尾部空 cell
        while cells and cells[-1] == '':
            cells.pop()
        print(' | '.join(cells))
