"""nbt.py：独立写的NBT/区域解析器（故意不复用C++那边的代码，用来对拍）
 * 只管测试用到的东西：读.mca、读区块、按坐标取方块、方块实体
"""
import zlib

class R:
	def __init__(self,b):
		self.b=b
		self.p=0
	def u(self,n):
		v=int.from_bytes(self.b[self.p:self.p+n],'big')
		self.p+=n
		return v
	def i(self,n):
		v=self.u(n)
		if v>>(8*n-1):
			v-=1<<(8*n)
		return v
	def f(self,n):
		import struct
		v=struct.unpack('>f' if n==4 else '>d',self.b[self.p:self.p+n])[0]
		self.p+=n
		return v
	def s(self,n):
		v=self.b[self.p:self.p+n].decode('utf-8','replace')
		self.p+=n
		return v

def read_tag(r,t):
	if t==1:
		return r.i(1)
	if t==2:
		return r.i(2)
	if t==3:
		return r.i(4)
	if t==4:
		return r.i(8)
	if t==5:
		return r.f(4)
	if t==6:
		return r.f(8)
	if t==7:
		n=r.i(4)
		v=r.b[r.p:r.p+n]
		r.p+=n
		return v
	if t==8:
		n=r.u(2)
		return r.s(n)
	if t==9:
		e=r.u(1)
		n=r.i(4)
		return [read_tag(r,e) for _ in range(n)]
	if t==10:
		d={}
		while True:
			tt=r.u(1)
			if tt==0:
				return d
			k=r.s(r.u(2))
			d[k]=read_tag(r,tt)
	if t==11:
		return [r.i(4) for _ in range(r.i(4))]
	if t==12:
		return [r.i(8) for _ in range(r.i(4))]
	raise Exception('bad tag %d'%t)

def parse(data):
	r=R(data)
	if r.u(1)!=10:
		raise Exception('root not compound')
	r.s(r.u(2))
	return read_tag(r,10)

def region(path):
	return open(path,'rb').read()

def chunk(d,i,j):
	"""i,j是世界区块坐标，没有就返回None"""
	loc=4*((i&31)+(j&31)*32)
	sec=int.from_bytes(d[loc:loc+3],'big')
	if sec==0:
		return None
	p=sec*4096
	ln=int.from_bytes(d[p:p+4],'big')
	t=d[p+4]
	raw=d[p+5:p+5+ln-1]
	if t==1:
		raw=zlib.decompress(raw,16+15)
	elif t==2:
		raw=zlib.decompress(raw)
	elif t!=3:
		raise Exception('compression %d'%t)
	return parse(raw)

def section(c,sy):
	for s in c.get('sections',[]):
		if s.get('Y')==sy:
			return s
	return None

def block_at(c,x,y,z):
	"""绝对坐标；section不在或者读不出来返回None"""
	s=section(c,y>>4)
	if s is None:
		return None
	bs=s.get('block_states') or {}
	pal=bs.get('palette')
	if not pal:
		return None
	lx,ly,lz=x&15,y&15,z&15
	k=ly*256+lz*16+lx
	idx=0
	data=bs.get('data')
	if data:
		bits=4
		while (1<<bits)<len(pal):
			bits+=1
		per=64//bits
		if k//per>=len(data):
			return None
		idx=(data[k//per]>>((k%per)*bits))&((1<<bits)-1)
	if idx>=len(pal):
		return None
	return pal[idx]

def bname(b):
	"""方块状态→一个能直接比的字符串"""
	if b is None:
		return 'None'
	s=b.get('Name','?')
	p=b.get('Properties')
	if p:
		s+='['+','.join('%s=%s'%(k,p[k]) for k in sorted(p))+']'
	return s

def entity_at(c,x,y,z):
	for e in c.get('block_entities',[]):
		if e.get('x')==x and e.get('y')==y and e.get('z')==z:
			return e
	return None

def ename(e):
	if e is None:
		return 'None'
	return '%s/%s'%(e.get('id'),e.get('CustomName'))
