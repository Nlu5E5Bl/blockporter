#!/usr/bin/env python3
"""check_rules.py：核对 26.1.1 规则的端到端结果（用独立的 nbt.py 解析，不碰 C++ 那边的代码）
	mkrule 造的每个用例：世界坐标 x=i*4+1、z=0、y=-58 是被测方块，
	预期写在 <世界>/cases.tsv 里：0=留着、1=拆掉（空气或水）、2=变成水源（霜冰化成水）、
	3=混凝土粉末凝固（white_concrete_powder→white_concrete，见 rules.cpp 的 RULE_FALLING）、
	4=结果的瓜茎变回普通瓜茎 age=7（rules.cpp 的 judge_Stem）、5=变成第 5 列 expect 写的状态
	（"minecraft:名字[属性=值,...]"，属性按名字排序；值写 * 的不比：藤头的 age 原版是随机的）。
	cases.tsv 第一行是 "#cases\t<条数>"：读到的条数对不上就算失败（用例表被截断也要报出来）。
	用法：python3 -I test/check_rules.py <世界目录>（rules.sh 会调它）
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbt

DIM = 'dimensions/minecraft/overworld/region'
Y = -58


def main():
	if len(sys.argv) < 2:
		print('用法: check_rules.py <世界目录>')
		return 2
	ROOT = sys.argv[1]
	rows = []
	with open(os.path.join(ROOT, 'cases.tsv'), encoding='utf-8') as f:
		want_n = int(f.readline().rstrip('\n').split('\t')[1])
		f.readline()	# 表头 x/self/del/why
		for line in f:
			p = line.rstrip('\n').split('\t')
			if len(p) >= 3:
				rows.append((int(p[0]), p[1], int(p[2]), p[3] if len(p) > 3 else '', p[4] if len(p) > 4 else ''))
	bad = []
	for x, self_name, del_expect, why, expect in rows:
		p = '%s/%s/r.%d.%d.mca' % (ROOT, DIM, x >> 9, 0)
		if not os.path.exists(p):
			bad.append((x, self_name, '找不到区域文件 %s' % p))
			continue
		cc = nbt.chunk(nbt.region(p), x >> 4, 0)
		if cc is None:
			bad.append((x, self_name, '区块(%d,0)被删了' % (x >> 4)))
			continue
		b = nbt.block_at(cc, x, Y, 0)
		if b is None:	# 段被删/调色板越界/区块读不出来：这本身就是坏结果，不能当"留着"放过
			bad.append((x, self_name, '这一格读不出来（区块/段/调色板坏了？）'))
			continue
		name = nbt.bname(b)
		# 拆含水物时主机会留水不换空气（fixer.cpp的规定），所以"拆掉了"=空气或水；bname 带属性，先掐掉
		base = (name or '').split('[')[0]
		is_air = base in ('', 'air', 'minecraft:air', 'water', 'minecraft:water')
		if del_expect == 1 and not is_air:
			bad.append((x, self_name, '预期拆掉，实际还是 %s（%s）' % (name, why)))
		elif del_expect == 2 and name != 'minecraft:water[level=0]':
			# 2=预期变成水源：霜冰（rules.cpp RULE_FROST）熔化的终点就是 level=0 的水，不是空气、不是流动水
			bad.append((x, self_name, '预期化成水源 water[level=0]，实际是 %s（%s）' % (name, why)))
		elif del_expect == 3:
			# 3=混凝土粉末碰到水凝固：期望方块名就是它自己去掉 _powder（white_concrete_powder→white_concrete）
			want = self_name[:-len('_powder')] if self_name.endswith('_powder') else '?'
			# 用例里的方块名可以不带 minecraft: 前缀（fixture 就这么写的），比较时两边都掐掉命名空间
			if base.split(':')[-1] != want:
				bad.append((x, self_name, '预期凝固成 %s，实际是 %s（%s）' % (want, name, why)))
		elif del_expect == 4:
			# 4=结果的瓜茎没了果实：原版变回普通瓜茎 age=7（attached_pumpkin_stem→pumpkin_stem[age=7]）
			want = self_name.split(':')[-1].replace('attached_', '', 1)
			if base.split(':')[-1] != want or 'age=7' not in (name or ''):
				bad.append((x, self_name, '预期变回 %s[age=7]，实际是 %s（%s）' % (want, name, why)))
		elif del_expect == 5:
			pat = '^' + re.escape(expect).replace(r'\*', r'[^,\]]*') + '$'
			if not expect or not re.match(pat, name or ''):
				bad.append((x, self_name, '预期变成 %s，实际是 %s（%s）' % (expect, name, why)))
		elif del_expect == 0 and is_air:
			bad.append((x, self_name, '预期留着，实际被拆了（%s）' % why))
	n = len(rows)
	if n != want_n or n == 0:	# 用例表被截断/没生成全：这种"少测"也要报出来
		bad.append((0, 'cases.tsv', '读到 %d 条，表头说 %d 条，用例表不全' % (n, want_n)))
	print('规则用例 %d 条，不符 %d 条' % (n, len(bad)))
	for b in bad[:25]:
		print('   x=%-4d %-28s %s' % b)
	print('RULES ' + ('OK' if not bad else 'FAIL'))
	return 0 if not bad else 1


sys.exit(main())
