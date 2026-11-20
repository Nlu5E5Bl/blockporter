#!/bin/sh
# rules.sh：26.1.1 规则插件的端到端回归（不碰真实地图）
#	cd nbtrw && sh test/rules.sh
#	1. mkrule 造“一条规则一个例子”的世界 → fixer 跑一遍 → check_rules.py 逐条核对；
#	2. 同一个世界再跑一遍 fixer：必须一个方块都不改、一个区块都不删（到不动点了）；
#	3. poidump 把 rules.so 的 POI 默认表打出来，和 poitab.py（照 wiki 手抄）对拍。
#	核对脚本用独立的 nbt.py 解析，不链 libnbtrw（自己验自己没意义）。
#	B=<目录>：不编译，直接用那个目录里编好的程序（safe.sh 用它跑 ASan 版）。
set -e
cd "$(dirname "$0")/.."
. test/work.sh
if [ -n "$B" ]; then
	BIN=$B
	TB=$B
	export LD_LIBRARY_PATH=
else
	make -s
	BIN=.
	TB=test/work
	export LD_LIBRARY_PATH=.
	g++ -std=c++23 -O2 -Wall -Wextra -pedantic -I. test/mkrule.cpp -o test/work/mkrule -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
	g++ -std=c++23 -O2 -Wall -Wextra -pedantic -I. test/poidump.cpp -o test/work/poidump -L. -lnbtrw -lz -ldl -Wl,-rpath,'$ORIGIN'
fi

W=test/work/rules
rm -rf $W
mkdir -p $W
$TB/mkrule $W
XMAX=$(awk -F'\t' 'NR>2{if($1>m)m=$1}END{print m+1}' $W/cases.tsv)
# z 只给 0..15：只处理 z=0 那一排区块；z=±1 的区块只当邻居读，不删不改
$BIN/fixer $W 0 -64 0 "$XMAX" -49 15 minecraft:overworld $BIN/rules.so.26.1.1 > test/work/rules1.log || { cat test/work/rules1.log; echo "FAIL: fixer 退出码非0"; exit 1; }
grep "物理：" test/work/rules1.log
python3 -I test/check_rules.py $W

$BIN/fixer $W 0 -64 0 "$XMAX" -49 15 minecraft:overworld $BIN/rules.so.26.1.1 > test/work/rules2.log || { cat test/work/rules2.log; echo "FAIL: 第二遍 fixer 退出码非0"; exit 1; }
grep -q "拆 0（含水 0）、改状态 0" test/work/rules2.log || { grep "物理：" test/work/rules2.log; echo "FAIL: 第二遍还拆/改了方块（没到不动点）"; exit 1; }
grep -q "删区块 0（" test/work/rules2.log || { grep "清理：" test/work/rules2.log; echo "FAIL: 第二遍还删了区块"; exit 1; }
echo "第二遍 0 改动"

python3 -I test/check_poitab.py $TB/poidump $BIN/rules.so.26.1.1
echo "rules OK"
