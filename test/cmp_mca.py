"""cmp_mca.py：比对两个区域文件是不是"改的东西一模一样"

时间戳那段（4096~8191）每次跑都会变，先抹掉；剩下逐字节比。
字节不同（或者文件长度不同——sector是一段段复用的，写几次的顺序不一样、尾巴上空闲的sector数就可能不一样）
就按区块解压出来用nbt.py解析、逐值比：
  · 逐值全同 → 只是编码顺序（compound成员按hash表顺序写）或者sector摆放位置不同，算通过；
  · 有区块逐值不同 → 真不一样，exit 1。

用法：python3 -I test/cmp_mca.py a.mca b.mca
（改代码前后各跑一遍 merge/fixer，用它逐文件对比输出是不是逐值相同）
"""
import sys, os, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nbt


def payload(d, i, j):
	loc = 4 * (i + j * 32)
	if loc + 4 > len(d):
		return None
	p = int.from_bytes(d[loc:loc + 3], 'big') * 4096
	if p == 0 or p + 5 > len(d):
		return None
	ln = int.from_bytes(d[p:p + 4], 'big')
	t = d[p + 4]
	if ln < 1 or p + 5 + ln - 1 > len(d):
		return None
	raw = d[p + 5:p + 5 + ln - 1]
	return zlib.decompress(raw, 16 + 15) if t == 1 else zlib.decompress(raw)


def main():
	if len(sys.argv) != 3:
		print("用法: cmp_mca.py a.mca b.mca")
		return -1
	a = bytearray(open(sys.argv[1], 'rb').read())
	b = bytearray(open(sys.argv[2], 'rb').read())
	na, nb = os.path.basename(sys.argv[1]), os.path.basename(sys.argv[2])
	if len(a) == len(b):
		for x in (a, b):
			x[4096:8192] = bytes(4096)
		if a == b:
			print("%s == %s（时间戳外逐字节相同，%d字节）" % (na, nb, len(a)))
			return 0
	else:
		print("文件长度不一样（%d vs %d 字节，sector摆法不同），按区块逐值比" % (len(a), len(b)))
	same_val = 0
	bad = []
	for j in range(32):
		for i in range(32):
			pa, pb = payload(a, i, j), payload(b, i, j)
			if pa == pb:
				same_val += 1
				continue
			if pa is not None and pb is not None and nbt.parse(pa) == nbt.parse(pb):
				same_val += 1
				continue
			bad.append((i, j))
	if not bad:
		print("%s == %s（字节摆放/编码顺序不同，但 %d 个区块逐值全同）" % (na, nb, same_val))
		return 0
	print("有 %d 个区块内容不一样：%s" % (len(bad), bad[:20]))
	for i, j in bad[:5]:
		pa, pb = payload(a, i, j), payload(b, i, j)
		print("  区块(%d,%d)：%s vs %s" % (i, j, "空" if pa is None else "%d字节" % len(pa),
		                                "空" if pb is None else "%d字节" % len(pb)))
	return 1


sys.exit(main())
