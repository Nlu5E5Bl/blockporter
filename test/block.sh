#!/bin/sh
# block.sh：merge/fixer 的合成世界回归（不碰真实地图）
#	cd nbtrw && sh test/block.sh
set -e
cd "$(dirname "$0")/.."
. test/work.sh
if [ ! -d test/data/lvlworld ]; then
	echo "FAIL: 缺 test/data/lvlworld（小块测试存档）"
	exit 1
fi
# B=<目录>：不编译，直接用那个目录里编好的 merge/fixer/rules.so.26.1.1 和测试程序（safe.sh 用它跑 ASan 版）
if [ -n "$B" ]; then
	BIN=$B
	TB=$B
	export LD_LIBRARY_PATH=
else
	make -s
	BIN=.
	TB=test/work
	export LD_LIBRARY_PATH=.
	for t in mkmerge checkmerge mkfix2 checkfix2 checkzero; do
		g++ -std=c++23 -O2 -Wall -Wextra -pedantic -I. test/$t.cpp -o test/work/$t -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
	done
fi

echo "== merge =="
$TB/mkmerge
# 维度故意写两遍（两种写法是同一个目录）：只能处理一次，否则 InhabitedTime 会被第二遍清成0
$BIN/merge test/work/merge/new test/work/merge/now \
	"test/work/merge/old1,test/work/merge/old2" 0 -64 0 1039 319 15 minecraft:overworld,overworld \
	> test/work/merge.log || { cat test/work/merge.log; echo "FAIL: merge 退出码非0"; exit 1; }
cat test/work/merge.log
grep -q "同一个目录，跳过" test/work/merge.log || { echo "FAIL: 重复维度没被跳过"; exit 1; }
# 只有(1,0)那对故意弄坏的目标块算坏；文件不在的区域不能算成坏实体/坏POI
grep -q "坏实体 1、坏POI 1、" test/work/merge.log || { echo "FAIL: 坏实体/坏POI计数不对（文件不在被当成读不出来？）"; exit 1; }
# 范围只到 x=1039（区域 r.2.0）：范围外那个读不出来的 r.5.0 不许碰
if grep -q "r\.5\.0" test/work/merge.log || ! grep -q "坏区域 0、" test/work/merge.log; then
	echo "FAIL: merge 碰了范围外的区域（区域范围算宽了？）"; exit 1
fi
$TB/checkmerge
echo "merge OK"

echo "== fixer =="
$TB/mkfix2
$BIN/fixer test/work/fixtest 0 -64 0 1023 319 63 minecraft:overworld,overworld $BIN/rules.so.26.1.1 \
	> test/work/fixer.log || { cat test/work/fixer.log; echo "FAIL: fixer 退出码非0"; exit 1; }
cat test/work/fixer.log
grep -q "同一个目录，跳过" test/work/fixer.log || { echo "FAIL: fixer 重复维度没被跳过"; exit 1; }
grep -q "版本和规则不同跳过 1" test/work/fixer.log || { echo "FAIL: fixer 没跳过 DataVersion 和规则不同的区块"; exit 1; }
grep -q "清偏移异常 1 格" test/work/fixer.log || { echo "FAIL: fixer 没把偏移异常的那一格清成 0"; exit 1; }
$TB/checkfix2
echo "fixer OK"

echo "== zero region =="
# 0字节region必须先删：即使region已经不可读，POI/实体也不能留下对应区块。
rm -rf test/work/zeroreg
cp -a test/work/fixtest test/work/zeroreg
: > test/work/zeroreg/dimensions/minecraft/overworld/region/r.0.0.mca
$BIN/fixer test/work/zeroreg 0 -64 0 15 319 15 minecraft:overworld $BIN/rules.so.26.1.1 >/dev/null
test ! -e test/work/zeroreg/dimensions/minecraft/overworld/region/r.0.0.mca
$TB/checkzero test/work/zeroreg/dimensions/minecraft/overworld/poi/r.0.0.mca 0 0 0
$TB/checkzero test/work/zeroreg/dimensions/minecraft/overworld/entities/r.0.0.mca 0 0 0
echo "zero region OK"

echo "blockporter OK"
