#!/bin/sh
# lib.sh：libnbtrw 的库级测试（不依赖 merge/fixer）
#	cd nbtrw && sh test/lib.sh
#	编出 libnbtrw.so + demo，然后跑 test/lib.cpp（nbt/Block/Chunk/MCA/Level）、
#	test/lvl.cpp（Level 维度/缓存）、test/bad.cpp（坏数据不崩、只用返回值报错）。
#	B=<目录>：不编译，直接用那个目录里编好的 lib/lvl/bad（safe.sh 用它跑 ASan 版）。
set -e
cd "$(dirname "$0")/.."
. test/work.sh
if [ -n "$B" ]; then
	TB=$B
	export LD_LIBRARY_PATH=
else
	make -s nbtrw
	TB=test/work
	export LD_LIBRARY_PATH=.
	for t in lib lvl bad; do
		g++ -std=c++23 -O2 -Wall -Wextra -pedantic -I. test/$t.cpp -o test/work/$t -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
	done
fi
$TB/lib
$TB/lvl
# 不用管道：set -e 只看管道最后一个命令的退出码，会把 bad 的失败吞掉
if ! $TB/bad test/work/badwork > test/work/bad.log; then
	grep -B1 FAIL test/work/bad.log
	echo "FAIL: bad"
	exit 1
fi
tail -n 1 test/work/bad.log
# demo 的自测：nbtw 读裸 NBT，拿样例 level.dat 解压出来的那棵树（以前读 test/work/3.txt，那个文件早就没了，这一步一直没跑）
if [ -z "$B" ]; then
	gzip -dc test/data/level.dat > test/work/lib_level.nbt
	if ! ./nbtrw nbtw test/work/lib_level.nbt > test/work/lib_nbtw.log || grep -q FAIL test/work/lib_nbtw.log; then
		echo "FAIL: nbtrw nbtw"
		cat test/work/lib_nbtw.log
		exit 1
	fi
	echo "ok   nbtrw nbtw（编码/深拷贝/比较/转换）"
fi
echo "lib OK"
