#!/usr/bin/env python3
"""check_poitab.py：rules.so 的 POI 默认表 和 poitab.py（照 wiki 手抄的独立表）对拍
	poitab.py 故意不看 rules.cpp：C++ 那边漏了方块、类型写错、票数写错，这里会对不上。
	用法：python3 -I test/check_poitab.py <poidump程序> <rules.so>（rules.sh 会调它）
"""
import sys, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import poitab

def main():
	if len(sys.argv) < 3:
		print('用法: check_poitab.py <poidump> <rules.so>')
		return 2
	rows = []	# (方块名, 属性, 期望(类型,票上限) 或 None)
	for base, want in sorted(poitab.BLOCK.items()):
		rows.append(('minecraft:' + base, '', want))
	rows.append(('lectern', '', poitab.BLOCK['lectern']))	# 没命名空间算 minecraft
	for color in ('red', 'white', 'black'):
		rows.append(('minecraft:%s_bed' % color, 'part=head,facing=north', poitab.BED))
		rows.append(('minecraft:%s_bed' % color, 'part=foot,facing=north', None))	# 床只有 head 那半有记录
	rows.append(('minecraft:red_bed', '', None))			# 没有 part：不猜
	for name in ('minecraft:stone', 'minecraft:air', 'minecraft:chest', 'minecraft:oak_door',
	             'minecraft:jukebox', 'minecraft:respawn_anchor', 'minecraft:crafting_table',
	             'mymod:lectern', 'minecraft:Lectern'):		# 不是POI / 别的命名空间 / 大小写不对
		rows.append((name, '', None))
	inp = ''.join('%s\t%s\n' % (n, p) for n, p, _ in rows)
	out = subprocess.run([sys.argv[1], sys.argv[2]], input=inp, capture_output=True, text=True)
	if out.returncode != 0:
		print(out.stdout + out.stderr)
		print('POITAB FAIL')
		return 1
	got = {}
	for line in out.stdout.splitlines():
		p = line.split('\t')
		got[(p[0], p[1])] = None if p[2] == '-' else (p[2], int(p[3]), int(p[4]))
	bad = []
	for n, p, want in rows:
		g = got.get((n, p), 'missing')
		if want is None:
			if g is not None:
				bad.append('%s[%s]：期望没有POI，rules给了 %s' % (n, p, g))
		elif g is None or g == 'missing' or g[0] != want[0] or g[1] != want[1] or g[2] != want[1]:
			bad.append('%s[%s]：期望 %s/票%d，rules给了 %s' % (n, p, want[0], want[1], g))
	print('POI表对拍 %d 条，不符 %d 条' % (len(rows), len(bad)))
	for b in bad[:20]:
		print('   ' + b)
	print('POITAB ' + ('OK' if not bad else 'FAIL'))
	return 0 if not bad else 1

sys.exit(main())
