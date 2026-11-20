#!/usr/bin/env python3
"""check_placement.py：拿 wiki Opacity/Placement 的三张放置表逐格对拍 rules.so
	（顶面表：放在 X 上面；侧面表：贴在 X 侧面；底面表：挂在 X 下面，外加“下面什么都能挂/什么都不能挂”两张名单）

	用法（在 nbtrw/ 下，先 make rules、编好 test/work/judge，wiki 页面用 wiki2md.py 缓存到 test/work/wiki/）：
	  python3 -I test/wiki2md.py -o test/work/wiki Opacity/Placement
	  g++ -std=c++23 -O2 -I. test/judge.cpp -o test/work/judge -L. -lnbtrw -lz -ldl -Wl,-rpath,$PWD
	  python3 -I test/check_placement.py [rules.so]       # 打印不一致的格子；插件默认 ./rules.so.26.1.1
	  python3 -I test/check_placement.py --all            # 一致的也打

	只判被放的那一格、不连锁（judge 不管支撑块自己合不合法）。表里的 wiki 名字 → 方块状态 的对照在下面的 ROWS/COL_*，
	对不上 wiki 名字的行/列会打出来（换了 wiki 版本要回来补对照表）。
	已知、有原因的不一致记在 KNOWN 里（每条写了原因），单独列出，不算失败。
"""
import os
import re
import subprocess
import sys

WIKI = 'test/work/wiki/Opacity%2FPlacement.md'
JUDGE = 'test/work/judge'

F6 = 'down=false,up=false,north=false,south=false,east=false,west=false,waterlogged=false'
def face6(face):
	return F6.replace(face + '=false', face + '=true')

VINE0 = 'up=false,north=false,south=false,east=false,west=false'
def vine(face):
	return 'vine[' + VINE0.replace(face + '=false', face + '=true') + ']'

# 支撑块（表的行）
ROWS = {
	'Anvil': 'anvil[facing=north]',
	'Azalea': 'azalea',
	'Bamboo': 'bamboo[age=0,leaves=none,stage=0]',
	'Barrier': 'barrier[waterlogged=false]',
	'Bell': 'bell[attachment=floor,facing=north,powered=false]',
	'Brewing Stand': 'brewing_stand[has_bottle_0=false,has_bottle_1=false,has_bottle_2=false]',
	'Cactus': 'cactus[age=0]',
	'Cauldron': 'cauldron',
	'Chains': 'iron_chain[axis=y,waterlogged=false]',
	'Chorus Flower': 'chorus_flower[age=0]',
	'Chorus Plant (up=true)': 'chorus_plant[up=true,down=true,north=false,south=false,east=false,west=false]',
	'Chorus Plant': 'chorus_plant[up=true,down=true,north=false,south=false,east=false,west=false]',
	'Cobweb': 'cobweb',
	'Composter': 'composter[level=0]',
	'Decorated Pot': 'decorated_pot[cracked=false,facing=north,waterlogged=false]',
	'Dirt Path': 'dirt_path',
	'Doors': 'oak_door[half=upper,facing=north,hinge=left,open=false,powered=false]',
	'Dragon Egg': 'dragon_egg',
	'End Portal Frame (with eye)': 'end_portal_frame[eye=true,facing=north]',
	'End Portal Frame (without eye)': 'end_portal_frame[eye=false,facing=north]',
	'End Portal Frame': 'end_portal_frame[eye=false,facing=north]',
	'End Rod': 'end_rod[facing=up]',
	'Farmland': 'farmland[moisture=0]',
	'Fences': 'oak_fence[north=false,south=false,east=false,west=false,waterlogged=false]',
	'Fence Gates': 'oak_fence_gate[facing=north,in_wall=false,open=false,powered=false]',
	'Glass Pane': 'glass_pane[north=false,south=false,east=false,west=false,waterlogged=false]',
	'Glass Panes': 'glass_pane[north=false,south=false,east=false,west=false,waterlogged=false]',
	'Grindstone': 'grindstone[face=floor,facing=north]',
	'Hanging Sign': 'oak_hanging_sign[attached=false,rotation=0,waterlogged=false]',
	'Honey Block': 'honey_block',
	'Hopper': 'hopper[enabled=true,facing=south]',
	'Ice': 'ice',
	'Packed Ice': 'packed_ice',
	'Iron Bars': 'iron_bars[north=false,south=false,east=false,west=false,waterlogged=false]',
	'Ladder': 'ladder[facing=north,waterlogged=false]',
	'Leaves': 'oak_leaves[distance=1,persistent=true,waterlogged=false]',
	'Lectern': 'lectern[facing=north,has_book=false,powered=false]',
	'Lightning Rod': 'lightning_rod[facing=up,powered=false,waterlogged=false]',
	'Scaffolding': 'scaffolding[bottom=false,distance=0,waterlogged=false]',
	'Sniffer Egg': 'sniffer_egg[hatch=0]',
	'Snow Layer (2–7)': 'snow[layers=4]',
	'Snow Layer (8)': 'snow[layers=8]',
	'Stairs': 'oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]',
	'Wall': 'cobblestone_wall[up=true,north=none,south=none,east=none,west=none,waterlogged=false]',
	'Walls': 'cobblestone_wall[up=true,north=none,south=none,east=none,west=none,waterlogged=false]',
	'Enchanting Table': 'enchanting_table',
	'Slabs': 'oak_slab[type=bottom,waterlogged=false]',
	'Campfire': 'campfire[facing=north,lit=false,signal_fire=false,waterlogged=false]',
	'Chests': 'chest[facing=north,type=single,waterlogged=false]',
	'Daylight Detector': 'daylight_detector[inverted=false,power=0]',
	'Flower Pot': 'flower_pot',
	'Heads': 'skeleton_skull[rotation=0]',
	'Trapdoors': 'oak_trapdoor[facing=north,half=bottom,open=false,powered=false,waterlogged=false]',
	'Turtle Egg': 'turtle_egg[eggs=1,hatch=0]',
	# 底面表下面的两张名单
	'Beacon': 'beacon', 'Glass': 'glass', 'Tinted Glass': 'tinted_glass', 'Glowstone': 'glowstone',
	'Sea Lantern': 'sea_lantern', 'Soul Sand': 'soul_sand', 'Observer': 'observer[facing=north,powered=false]',
	'Beds': 'red_bed[part=head,facing=north,occupied=false]',
}
# 底面表里方向有讲究的（脚注：“只有朝下的砂轮/漏嘴朝下的漏斗”）
ROWS_BOTTOM = {
	'Grindstone': 'grindstone[face=ceiling,facing=north]',
	'Hopper': 'hopper[enabled=true,facing=down]',
}
# 被放的方块（列）：(自己, 上面要补的方块)；None＝这一列不比（规则不管/不是方块/wiki 另有说明）
COL_TOP = {
	'Amethyst Cluster': 'amethyst_cluster[facing=up,waterlogged=false]',
	'Banner': 'white_banner[rotation=0]',
	'Bell': 'bell[attachment=floor,facing=north,powered=false]',
	'Button': 'oak_button[face=floor,facing=north,powered=false]',
	'Cake': 'cake[bites=0]',
	'Candle': 'candle[candles=1,lit=false,waterlogged=false]',
	'Carpet': 'white_carpet',
	'Coral': 'tube_coral[waterlogged=true]',
	'Coral Fan': 'tube_coral_fan[waterlogged=true]',
	'Door': ('oak_door[half=lower,facing=north,hinge=left,open=false,powered=false]',
	         'oak_door[half=upper,facing=north,hinge=left,open=false,powered=false]'),
	'Fire': None,
	'Glow Lichen': 'glow_lichen[' + face6('down') + ']',
	'Kelp': 'kelp[age=0]',
	'Lantern': 'lantern[hanging=false,waterlogged=false]',
	'Lever': 'lever[face=floor,facing=north,powered=false]',
	'Mushrooms': 'brown_mushroom',
	'Pressure Plate': 'oak_pressure_plate[powered=false]',
	'Pointed Dripstone': 'pointed_dripstone[vertical_direction=up,thickness=tip,waterlogged=false]',
	'Rail': 'rail[shape=north_south,waterlogged=false]',
	'Redstone Comparator': 'comparator[facing=north,mode=compare,powered=false]',
	'Redstone Repeater': 'repeater[facing=north,delay=1,locked=false,powered=false]',
	'Redstone Dust': 'redstone_wire[power=0,north=none,south=none,east=none,west=none]',
	'Scaffolding': None,
	'Sculk Vein': 'sculk_vein[' + face6('down') + ']',
	'Seagrass': 'seagrass',
	'Sea Pickle': 'sea_pickle[pickles=1,waterlogged=false]',
	'Sign': 'oak_sign[rotation=0,waterlogged=false]',
	'Snow': 'snow[layers=1]',
	'Torch': 'torch',
	'Twisting Vines': 'twisting_vines[age=0]',
	'Item Frame': None, 'Painting': None,
}
COL_SIDE = {  # 都朝北贴在南边那块的北面上
	'Banner': 'white_wall_banner[facing=north]',
	'Bell': 'bell[attachment=single_wall,facing=north,powered=false]',
	'Button': 'oak_button[face=wall,facing=north,powered=false]',
	'Ladder': 'ladder[facing=north,waterlogged=false]',
	'Lever': 'lever[face=wall,facing=north,powered=false]',
	'Sign': 'oak_wall_sign[facing=north,waterlogged=false]',
	'Torch': 'wall_torch[facing=north]',
	'Tripwire Hook': 'tripwire_hook[facing=north,attached=false,powered=false]',
	'Vines': vine('south'),
	'Amethyst Cluster': 'amethyst_cluster[facing=north,waterlogged=false]',
	'Coral Fan': 'tube_coral_wall_fan[facing=north,waterlogged=true]',
	'Glow Lichen': 'glow_lichen[' + face6('south') + ']',
	'Hanging Sign': None,  # 墙上吊牌自带托架、附着块没了也不破（Hanging Sign 页），规则不管
	'Sculk Vein': 'sculk_vein[' + face6('south') + ']',
	'Painting': None, 'Item Frame': None,
}
COL_BOTTOM = {
	'Amethyst Cluster': 'amethyst_cluster[facing=down,waterlogged=false]',
	'Pointed Dripstone': 'pointed_dripstone[vertical_direction=down,thickness=tip,waterlogged=false]',
	'Weeping Vines': 'weeping_vines[age=0]',
	'Bell': 'bell[attachment=ceiling,facing=north,powered=false]',
	'Hanging Sign': 'oak_hanging_sign[attached=false,rotation=0,waterlogged=false]',
	'Lantern': 'lantern[hanging=true,waterlogged=false]',
	'Spore Blossom': 'spore_blossom',
	'Buttons': 'oak_button[face=ceiling,facing=north,powered=false]',
	'Lever': 'lever[face=ceiling,facing=north,powered=false]',
	'Glow Lichen': 'glow_lichen[' + face6('up') + ']',
	'Sculk Vein': 'sculk_vein[' + face6('up') + ']',
	'Vines': vine('up'),
	'Item Frame': None,
}
# 已知不一致：(表, 行, 列) → 原因。行/列写 '*' 表示整行/整列
KNOWN = {
	('top', 'Chorus Flower', '*'): '紫颂花 23w31a 起不给支撑（中英文 Chorus Flower §History、放置页自己的支撑形状表）；这张表这一行是旧的',
	('side', 'Chorus Flower', '*'): '同上；表里的 Yes 带脚注“紫颂花可能会碎”',
	('top', 'Azalea', 'Kelp'): '脚注 n3：支撑块遇水会碎',
	('top', 'Azalea', 'Seagrass'): '脚注 n3：支撑块遇水会碎',
	('top', 'Snow Layer (8)', 'Kelp'): '脚注 n3：支撑块遇水会碎',
	('top', 'Snow Layer (8)', 'Seagrass'): '脚注 n3：支撑块遇水会碎',
	('top', 'Snow Layer (2–7)', 'Snow'): '脚注 n11：是给雪加一层，不是放一层浮在上面',
	('top', '*', 'Candle'): '表说的是“放的时候”要中心完整的顶面；放好以后支撑没了蜡烛会悬着（中文 蜡烛），规则不管',
	('top', 'End Rod', 'Mushrooms'): '脚注 n8：末地烛发光，蘑菇是因为亮度放不了；规则不看亮度（宁可漏拆）',
	# 下面这些是表和游戏不一样，规则跟游戏（sh test/mcdump.sh <jar> cmp 对拍过；表里的代表状态见 ROWS）
	('top', 'Hanging Sign', 'Lantern'): '游戏：正着挂（rotation 是 4 的倍数）的天花板吊牌顶面碰不到格子顶，没有中心支撑；斜着挂的才有',
	('top', 'Hanging Sign', 'Pressure Plate'): '同上（游戏导出的形状）',
	('top', 'Hanging Sign', 'Torch'): '同上（游戏导出的形状）',
	('top', 'Hanging Sign', 'Sea Pickle'): '游戏：吊牌没有碰撞箱，顶面也不是整面，海泡菜放不住',
	('top', 'Scaffolding', 'Mushrooms'): '游戏：脚手架不是整块不透明（isSolidRender 假），蘑菇只能在亮度够低时靠它，规则按亮度宽松也只认整块不透明',
	('bottom', 'Scaffolding', 'Vines'): '游戏：藤蔓挂在下面看的是上方方块的**顶面**（VineBlock 的怪写法），脚手架顶面整面，能挂',
	('bottom', 'Chorus Plant', 'Bell'): '游戏：往下长的紫颂植株（down=true）底面中间那截给中心支撑，下面能挂',
	('bottom', 'Chorus Plant', 'Hanging Sign'): '同上（游戏导出的形状）',
	('bottom', 'Chorus Plant', 'Lantern'): '同上（游戏导出的形状）',
	('bottom', 'Chorus Plant', 'Spore Blossom'): '同上（游戏导出的形状）',
}
NOTE_SKIP = re.compile(r'\[n 3\]|BE only')  # 支撑块遇水会碎 / 只有基岩版

def clean(name):
	name = re.sub(r'\[[^\]]*\]', '', name).strip()
	return name

def parse_Tables(lines):
	"""返回 [(表名, 列名列表或列组列表, 行列表[(行名, 单元格列表)])]"""
	out = []
	i = 0
	section = None
	while i < len(lines):
		l = lines[i]
		if l.startswith('## Placement on top'):
			section = 'top'
		elif l.startswith('## Placement on the side'):
			section = 'side'
		elif l.startswith('## Placement on bottom'):
			section = 'bottom'
		elif l.startswith('## ') and section:
			section = None
		if section and l.startswith('| Place<br>On'):
			heads = []
			while i < len(lines) and lines[i].startswith('|'):
				if lines[i].startswith('| Place<br>On'):
					heads.append([clean(c) for c in lines[i].strip().strip('|').split('|')[1:]])
				i += 1
				if i < len(lines) and lines[i].startswith('| ---'):
					i += 1
				if i < len(lines) and lines[i].startswith('|') and not lines[i].startswith('| Place<br>On'):
					break
			rows = []
			while i < len(lines) and lines[i].startswith('|'):
				cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
				if not cells[0].startswith('---'):
					rows.append((clean(cells[0]), cells[1:]))
				i += 1
			out.append((section, heads, rows))
			continue
		i += 1
	return out

def parse_Lists(lines):
	"""底面表下面的“Everything can/cannot be placed under”两张名单"""
	res = {}
	cur = None
	for l in lines:
		if l.startswith('Everything can be placed under'):
			cur = True
		elif l.startswith('Everything cannot be placed under'):
			cur = False
		elif cur is not None and l.startswith('- '):
			name = l[2:].strip()
			if '(' in name:  # “Chorus Flower (the chorus flower breaks)”
				continue
			res[name] = cur
		elif cur is not None and l.strip() and not l.startswith('- '):
			cur = None if not l.startswith('Everything') else cur
	return res

def make_Line(table, self_spec, above_extra, sup):
	air = '-'
	if table == 'top':
		return [self_spec, sup, above_extra or air, air, air, air, air]
	if table == 'side':
		return [self_spec, air, above_extra or air, air, air, sup, air]  # 支撑块在南边
	return [self_spec, air, sup, air, air, air, air]  # 底面：支撑块在上面

def main():
	show_all = '--all' in sys.argv
	so = next((a for a in sys.argv[1:] if not a.startswith('--')), './rules.so.26.1.1')
	if not os.path.exists(WIKI) or not os.path.exists(JUDGE):
		print('缺 %s 或 %s，先按文件头的说明准备' % (WIKI, JUDGE))
		return 1
	lines = open(WIKI, encoding='utf-8').read().split('\n')
	cases = []  # (表, 行, 列, wiki真假, judge 输入)
	unknown = set()
	cols_of = {'top': COL_TOP, 'side': COL_SIDE, 'bottom': COL_BOTTOM}
	for table, heads, rows in parse_Tables(lines):
		cols = cols_of[table]
		for rname, cells in rows:
			sup = (ROWS_BOTTOM.get(rname) if table == 'bottom' else None) or ROWS.get(rname)
			if not sup:
				unknown.add(('行', table, rname))
				continue
			for ci, cell in enumerate(cells):
				if not cell or NOTE_SKIP.search(cell):
					continue
				if cell.startswith('Yes'):
					want = True
				elif cell.startswith('No'):
					want = False
				else:
					continue
				for h in heads:  # 底面表是几层表头叠在一起：同一格对几种方块都成立
					if ci >= len(h):
						continue
					cname = h[ci]
					if cname not in cols:
						unknown.add(('列', table, cname))
						continue
					spec = cols[cname]
					if spec is None:
						continue
					self_spec, above = (spec if isinstance(spec, tuple) else (spec, None))
					cases.append((table, rname, cname, want, make_Line(table, self_spec, above, sup)))
	# 底面表下面的两张名单：对底面表的每一列都成立
	bottom_cols = sorted({c for c, s in COL_BOTTOM.items() if s})
	for name, want in parse_Lists(lines).items():
		sup = ROWS.get(name) or ROWS.get(name.rstrip('s'))
		if not sup:
			unknown.add(('名单', 'bottom', name))
			continue
		for cname in bottom_cols:
			cases.append(('bottom', name, cname, want, make_Line('bottom', COL_BOTTOM[cname], None, sup)))
	inp = ''.join('\t'.join(c[4]) + '\n' for c in cases)
	run = subprocess.run([JUDGE, so], input=inp, capture_output=True, text=True)
	res = run.stdout.split('\n')
	if run.returncode or len(res) < len(cases):  # judge 装不上插件会直接退出；以前不看，zip 一行都没比就报“不一致 0 格”
		print('judge 失败（退出码 %d，%d 行输出，要 %d 行）：%s' % (run.returncode, len(res), len(cases), run.stderr.strip()))
		return 1
	bad, known = [], []
	for c, r in zip(cases, res):
		table, rname, cname, want, _ = c
		got = r.startswith('KEEP') or r.startswith('NONE') or r.startswith('SET')
		if show_all:
			print('%-6s %-28s %-20s wiki=%-3s 规则=%s' % (table, rname, cname, 'Yes' if want else 'No', r))
		if got == want:
			continue
		why = KNOWN.get((table, rname, cname)) or KNOWN.get((table, rname, '*')) or KNOWN.get((table, '*', cname))
		(known if why else bad).append((table, rname, cname, want, r, why))
	for k in sorted(unknown):
		print('对照表里没有的%s：[%s] %s' % k)
	print('比了 %d 格；不一致 %d 格，其中已知原因 %d 格' % (len(cases), len(bad) + len(known), len(known)))
	for t, rn, cn, w, r, why in known:
		print('  已知 %-6s %-28s %-20s wiki=%-3s 规则=%-5s %s' % (t, rn, cn, 'Yes' if w else 'No', r, why))
	for t, rn, cn, w, r, why in bad:
		print('  不符 %-6s %-28s %-20s wiki=%-3s 规则=%s' % (t, rn, cn, 'Yes' if w else 'No', r))
	return 1 if bad else 0

if __name__ == '__main__':
	sys.exit(main())
