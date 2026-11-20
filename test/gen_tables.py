#!/usr/bin/env python3
"""gen_tables.py：从游戏导出的 shapes.tsv（test/mcdump.sh shape 产出）生成规则插件里“只跟名字有关”的数据表，原地替换进 rules.cpp.<版本>。
	python3 -I test/gen_tables.py test/work/mc/<版本>/shapes.tsv rules.cpp.<版本>
现在生成这几张（规则从 jar 生成的第一步，见 ReadMeDeveloper 1.9），各自写在 /*<gen:名字>*/ … /*</gen:名字>*/ 之间：
	instr     音符盒音色（instr 列，NoteBlockInstrument 的序号）
	leafkeep  树叶当 distance 0 的方块（leafkeep 列，#prevents_nearby_leaf_decay）
	material  set_replace / set_liquid / set_burn（replace/liquid/burn 列；一种方块有一个状态是就算，burn 只看没含水的状态）
	defaults  每种方块默认状态的属性（default 列；没有属性的方块不列）
只认 minecraft 命名空间；一种方块的所有状态音色必须一样（不一样就报错退出）。生成完照样要跑 mcdump.sh shape 核对。"""
import sys

def read_Tsv(path):
	instr={}
	keep=set()
	mat={'replace':set(),'liquid':set(),'burn':set()}
	defaults={}
	for line in open(path,encoding='utf-8'):
		if not line or line[0] in '#@':
			continue
		f=line.rstrip('\n').split('\t')
		if len(f)<18:
			sys.exit('gen_tables: %s 列数不够（要先用新的 ShapeDump 导出）'%path)
		name=f[0].split('[')[0]
		if not name.startswith('minecraft:'):
			continue
		name=name[len('minecraft:'):]
		i=int(f[15])
		if instr.setdefault(name,i)!=i:
			sys.exit('gen_tables: %s 的不同状态音色不一样'%name)
		if f[16]=='leafkeep':
			keep.add(name)
		props=f[0][f[0].find('[')+1:-1] if '[' in f[0] else ''
		if f[11]=='replace':
			mat['replace'].add(name)
		if f[12]=='liquid':
			mat['liquid'].add(name)
		if f[13]=='burn' and 'waterlogged=true' not in props.split(','):
			mat['burn'].add(name)
		if f[17]=='default' and props:
			defaults[name]=props
	return instr,keep,mat,defaults

def wrap_Names(names,indent):
	out=[]
	line=indent
	for n in names:
		if len(line)+len(n)+1>120:
			out.append(line.rstrip())
			line=indent
		line+=n+' '
	out.append(line.rstrip())
	return out

def make_Instr(instr):
	n=max(instr.values())+1
	by=[[] for _ in range(n)]
	for name,i in instr.items():
		by[i].append(name)
	lines=['static const char *const instr_Blocks[%d]={/*第 i 条：音色序号是 i 的方块（空格隔开，不带命名空间）；0（harp）是“别的都是”，不列*/'%n]
	for i in range(n):
		if i==0 or not by[i]:
			lines.append('\t"",')
			continue
		parts=wrap_Names(sorted(by[i]),'')
		lines.append('\t/*%d*/'%i)
		for k,p in enumerate(parts):
			lines.append('\t"%s%s"%s'%(p,' ' if k+1<len(parts) else '',',' if k+1==len(parts) else ''))
	lines.append('};')
	return lines

def make_Keep(keep):
	lines=['static const char *const leafkeep_Blocks=/*#prevents_nearby_leaf_decay 的成员（空格隔开）*/']
	parts=wrap_Names(sorted(keep),'')
	for k,p in enumerate(parts):
		lines.append('\t"%s%s"%s'%(p,' ' if k+1<len(parts) else '',';' if k+1==len(parts) else ''))
	return lines

def make_Material(mat):
	lines=[]
	notes={'replace':'BlockState.canBeReplaced()（Properties.replaceable，和属性无关）：重力方块下面是它就掉（FallingBlock.isFree）',
		'liquid':'BlockState.liquid()（Properties.liquid()）',
		'burn':'FireBlock.canBurn：火能靠它活（igniteOdds>0）；含水的不算（fill_Info 另外看 waterlogged）'}
	for k in ('replace','liquid','burn'):
		lines.append('/*%s*/'%notes[k])
		lines.append('static const char *set_%s[]={'%k)
		names=['"%s",'%n for n in sorted(mat[k])]+['NULL};']
		line='\t'
		for n in names:
			if len(line)+len(n)>120:
				lines.append(line.rstrip())
				line='\t'
			line+=n
		lines.append(line)
	return lines

def make_Defaults(defaults):
	lines=['static const char *const defaults_Blocks[]={/*“名字 默认状态的属性”（属性按名字排序，逗号隔开）*/']
	for n in sorted(defaults):
		lines.append('\t"%s %s",'%(n,defaults[n]))
	lines.append('\tNULL};')
	return lines

def replace_Block(src,tag,lines):
	a='/*<gen:%s>*/'%tag
	b='/*</gen:%s>*/'%tag
	i=src.find(a)
	j=src.find(b)
	if i<0 or j<0:
		sys.exit('gen_tables: 找不到 %s … %s'%(a,b))
	return src[:i+len(a)]+'\n'+'\n'.join(lines)+'\n'+src[j:]

def main():
	if len(sys.argv)!=3:
		sys.exit(__doc__)
	instr,keep,mat,defaults=read_Tsv(sys.argv[1])
	src=open(sys.argv[2],encoding='utf-8').read()
	src=replace_Block(src,'instr',make_Instr(instr))
	src=replace_Block(src,'leafkeep',make_Keep(keep))
	src=replace_Block(src,'material',make_Material(mat))
	src=replace_Block(src,'defaults',make_Defaults(defaults))
	open(sys.argv[2],'w',encoding='utf-8').write(src)
	print('gen_tables: %d 种方块不是 harp，leafkeep %d 种，replace/liquid/burn %d/%d/%d 种，默认属性 %d 种 -> %s'%(
		sum(1 for v in instr.values() if v),len(keep),len(mat['replace']),len(mat['liquid']),len(mat['burn']),len(defaults),sys.argv[2]))

main()
