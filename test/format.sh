#!/bin/sh
# format.sh：同一批场景放进两种存档格式（26.1.1=DataVersion 4788，26.4=5119），fixer/merge 的结果必须一样
#	cd nbtrw && sh test/format.sh
#	5119 的世界由 mkfmt 从 test/data/lvlworld 转出来（调色板 id/properties/字符串、status、每格一个的 biomes）。
#	fixer 要求插件版本和存档完全相等，所以另编一个“声明 5119、规则内容不变”的插件（-DRULES_DATA_VERSION=5119），
#	放在 test/work/fmt/rules5119.so，跑 5119 的世界时把它的路径给 fixer。
#	B=<目录>：用那个目录里编好的程序（safe.sh 的 ASan 版），5119 插件用 $B/rules5119.so。
set -e
cd "$(dirname "$0")/.."
. test/work.sh
F=test/work/fmt
rm -rf $F
mkdir -p $F
if [ -n "$B" ]; then
	BIN=$B
	TB=$B
	export LD_LIBRARY_PATH=
	R5119=$B/rules5119.so
else
	make -s
	BIN=.
	TB=test/work
	export LD_LIBRARY_PATH=.
	for t in mkfmt checkfmt; do
		g++ -std=c++23 -O2 -Wall -Wextra -pedantic -I. test/$t.cpp -o test/work/$t -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
	done
	R5119=$F/rules5119.so
	g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -DRULES_DATA_VERSION=5119 -shared -x c++ rules.cpp.26.1.1 -x none -o $R5119 \
		-L. -lnbtrw -lz -Wl,-rpath,"$PWD"
fi

echo "== fixer：4788 / 5119 =="
$TB/mkfmt fix
$BIN/fixer $F/old 0 -64 0 15 319 15 minecraft:overworld $BIN/rules.so.26.1.1 > $F/fix_old.log || { cat $F/fix_old.log; echo "FAIL: fixer(4788)"; exit 1; }
$BIN/fixer $F/new 0 -64 0 15 319 15 minecraft:overworld $R5119 > $F/fix_new.log || { cat $F/fix_new.log; echo "FAIL: fixer(5119)"; exit 1; }
grep "物理：\|清理：" $F/fix_new.log | cut -c1-160
$TB/checkfmt fix
# 第二遍 0 改动（新格式下也要收敛）
$BIN/fixer $F/new 0 -64 0 15 319 15 minecraft:overworld $R5119 > $F/fix_new2.log
grep -q "拆 0（含水 0）、改状态 0" $F/fix_new2.log || { cat $F/fix_new2.log; echo "FAIL: 5119 第二遍还有改动"; exit 1; }

echo "== merge：5119→5119（缺段跳过）、4788→5119、5119→4788（跨版本照搬） =="
$TB/mkfmt merge
$BIN/merge $F/m_new $F/m_now $F/m_old 0 -64 0 15 319 15 minecraft:overworld > $F/merge_m.log || { cat $F/merge_m.log; echo "FAIL: merge(m)"; exit 1; }
# 跨版本：暴力合并，两个方向都照搬（不拦）
for x in x y; do
	$BIN/merge $F/${x}_new $F/${x}_now $F/${x}_old 0 -64 0 15 319 15 minecraft:overworld > $F/merge_$x.log || { cat $F/merge_$x.log; echo "FAIL: merge($x)"; exit 1; }
	grep -q "搬了 3；计划刻 4，实体 1，POI 1" $F/merge_$x.log || { cat $F/merge_$x.log; echo "FAIL: merge($x) 应该正好搬 3 格"; exit 1; }
done
# 合进 1.13.2（1631）：只有落在 1.13 高度里的那格搬得进去（另外三格那一段 new 没有，缺段跳过）；
# 实体写进区块的 Level.Entities；1.13 没有 POI，不搬、不建 poi/
$BIN/merge $F/z_new $F/z_now $F/z_old 0 -64 0 15 319 15 minecraft:overworld > $F/merge_z.log || { cat $F/merge_z.log; echo "FAIL: merge(z)"; exit 1; }
grep -q "搬了 1；计划刻 4，实体 1，POI 0" $F/merge_z.log || { cat $F/merge_z.log; echo "FAIL: merge(z) 应该搬 1 格、实体 1、POI 0"; exit 1; }
grep -q "缺段跳过 3" $F/merge_z.log || { cat $F/merge_z.log; echo "FAIL: merge(z) 应该缺段跳过 3 格"; exit 1; }
$TB/checkfmt merge
echo "format OK"
