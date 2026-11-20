#!/usr/bin/env python3
"""check_poi_scan.py：拿一个**真实存档**扫一遍 poi/，看记录和方块对不对得上（只读，不改任何文件）
 *	用法：python3 -I test/check_poi_scan.py <世界目录> [rx0 rz0 rx1 rz1] [strict]
 *	      <世界目录> 里有 level.dat 就行（维度目录按 dimensions/minecraft/overworld/ 找）
 *	      rx0 rz0 rx1 rz1 是可选的区域范围（默认全扫）
 *	      strict：把"区块里有POI方块却没有记录"也当失败（默认只列出来）
 *	查三件事：
 *	  1. 前向：每条记录的type必须和那一格的方块对得上（类型表用 poitab.py，和 rules.cpp 的表由 check_poitab.py 对拍）；
 *	  2. 票：free_tickets 不能是负数、也不能超过该类型的上限（wiki: 1 / 32 / 0）；
 *	  3. 形状：根只有 DataVersion/Sections、段键是段Y、Valid是0/1、pos在本区块内、没有重复位置；
 *	  反向（可选）：区块里该有记录的POI方块有没有缺记录——原版在没加载过的区块里可能本来就没有，
 *	  所以默认只统计（按方块名分组），-strict 才当失败。
 *	为什么要有这个：POI 的表是硬编码的，真数据里出现的方块种类才是最好的检验；
 *	当初就是靠反向对拍发现表里漏了 fletching_table（制箭台）的。
 *	（慢：纯python逐格扫，扫一个区域几秒，全存档要几分钟。不进自动测试，手动跑。）
"""
import sys, os, re, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbt
import poitab

if len(sys.argv) < 2:
	print('用法: check_poi_scan.py <世界目录> [rx0 rz0 rx1 rz1] [strict]')
	sys.exit(2)
WORLD = sys.argv[1]
NUMS = [v for v in sys.argv[2:] if re.fullmatch(r'-?\d+', v)]
STRICT = 'strict' in sys.argv
if len(NUMS) >= 4:
	RX0, RZ0, RX1, RZ1 = (int(v) for v in NUMS[:4])
else:
	RX0 = RZ0 = RX1 = RZ1 = None

ROOT = '%s/dimensions/minecraft/overworld' % WORLD
P = '%s/poi' % ROOT
R = '%s/region' % ROOT
if not os.path.isdir(P):
	print('没有 poi 目录：%s（这个存档还没生成过兴趣点？）' % P)
	sys.exit(0)

bad_fwd = []
bad_ft = []
bad_shape = []
holes = collections.Counter()
nrec = 0
nchunk = 0
nfile = 0
for f in sorted(glob.glob(P + '/r.*.mca')):
	m = re.match(r'r\.(-?\d+)\.(-?\d+)\.mca', os.path.basename(f))
	rx, rz = int(m.group(1)), int(m.group(2))
	if RX0 is not None and not (RX0 <= rx <= RX1 and RZ0 <= rz <= RZ1):
		continue
	nfile += 1
	pd = nbt.region(f)
	if len(pd) < 8192:
		continue
	rf = '%s/r.%d.%d.mca' % (R, rx, rz)
	rd = nbt.region(rf) if os.path.exists(rf) else None
	for i in range(rx * 32, rx * 32 + 32):
		for j in range(rz * 32, rz * 32 + 32):
			pc = nbt.chunk(pd, i, j)
			if pc is None:
				continue
			nchunk += 1
			rc = nbt.chunk(rd, i, j) if rd is not None else None
			# ---- 形状 ----
			if set(pc.keys()) - {'DataVersion', 'Sections'}:
				bad_shape.append((i, j, '根下多了成员 %s' % sorted(set(pc.keys()) - {'DataVersion', 'Sections'})))
			if not isinstance(pc.get('DataVersion'), int):
				bad_shape.append((i, j, '没有DataVersion'))
			secs = pc.get('Sections')
			if not isinstance(secs, dict):
				bad_shape.append((i, j, 'Sections不是compound'))
				continue
			seen = set()
			for sy, s in secs.items():
				if not re.fullmatch(r'-?\d+', str(sy)):
					bad_shape.append((i, j, '段键不是整数：%r' % sy))
					continue
				if not isinstance(s, dict) or s.get('Valid') not in (0, 1):
					bad_shape.append((i, j, '段%s 的Valid不对：%r' % (sy, (s or {}).get('Valid'))))
				rs = s.get('Records')
				if rs is None:
					continue
				if not isinstance(rs, list):
					bad_shape.append((i, j, '段%s 的Records不是list' % sy))
					continue
				for e in rs:
					if not isinstance(e, dict) or not isinstance(e.get('pos'), list) or len(e['pos']) != 3 \
					   or not isinstance(e.get('type'), str) or not isinstance(e.get('free_tickets'), int):
						bad_shape.append((i, j, '记录字段不对：%r' % (e,)))
						continue
					p = tuple(e['pos'])
					nrec += 1
					if p[0] >> 4 != i or p[2] >> 4 != j:
						bad_shape.append((i, j, '记录的pos %s 不在这个区块里' % (p,)))
					if str(p[1] >> 4) != str(sy):
						bad_shape.append((i, j, '记录%s 放在段键"%s"下（该是%d）' % (p, sy, p[1] >> 4)))
					if p in seen:
						bad_shape.append((i, j, '位置%s 有重复记录' % (p,)))
					seen.add(p)
					if e['type'] not in poitab.TYPES:
						bad_fwd.append((p, e['type'], '类型不在wiki那21种里'))
						continue
					tmax = dict(list(poitab.BLOCK.values()) + [poitab.BED]).get(e['type'])
					if tmax is not None and (e['free_tickets'] < 0 or e['free_tickets'] > tmax):
						bad_ft.append((p, e['type'], e['free_tickets'], tmax))
					if rc is None:
						bad_fwd.append((p, e['type'], '没有对应的方块区块'))
						continue
					b = nbt.block_at(rc, *p)
					exp = poitab.poi_of(b)
					if exp is None or exp[0] != e['type']:
						bad_fwd.append((p, e['type'], nbt.bname(b), exp and exp[0]))
			# ---- 反向：区块里该有记录的POI方块 ----
			if rc is None:
				continue
			for s in (rc.get('sections') or []):
				bs = s.get('block_states') or {}
				pal = bs.get('palette') or []
				if not pal or len(pal) > 256:
					continue
				want = set(k for k, p in enumerate(pal) if poitab.poi_of(p) is not None)
				if not want:
					continue
				bits = 4
				data = bs.get('data')
				if data:
					while (1 << bits) < len(pal):
						bits += 1
				per = 64 // bits
				ox = (rc.get('xPos') or 0) * 16
				oz = (rc.get('zPos') or 0) * 16
				oy = s['Y'] * 16
				n = 4096 if len(pal) == 1 else min(4096, len(data or []) * per)
				for k in range(n):
					v = 0 if len(pal) == 1 else ((data[k // per] >> ((k % per) * bits)) & ((1 << bits) - 1))
					if v not in want:
						continue
					p = (ox + (k & 15), oy + ((k >> 8) & 15), oz + ((k >> 4) & 15))
					if p not in seen:
						holes[pal[v].get('Name')] += 1

print('扫了 %d 个POI文件、%d 个POI区块、%d 条记录（世界 %s）' % (nfile, nchunk, nrec, WORLD))
print('前向：对不上 %d 条；票越界 %d 条；形状不对 %d 处' % (len(bad_fwd), len(bad_ft), len(bad_shape)))
for x in bad_fwd[:10]:
	print('   前向不对:', x)
for x in bad_ft[:10]:
	print('   票越界:', x)
for x in bad_shape[:10]:
	print('   形状:', x)
if holes:
	print('反向：有 %d 个"POI方块但没记录"的位置（原版没加载过的区块可能本来就没有；按方块名分组）：' % sum(holes.values()))
	for k, v in holes.most_common(12):
		print('   %-40s %d' % (k, v))
print('POI SCAN ' + ('FAIL' if (bad_fwd or bad_ft or bad_shape or (STRICT and holes)) else 'OK'))
sys.exit(1 if (bad_fwd or bad_ft or bad_shape or (STRICT and holes)) else 0)
