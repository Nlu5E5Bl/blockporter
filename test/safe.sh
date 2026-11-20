#!/bin/sh
# safe.sh：用 ASan + UBSan（含 LSan 泄漏检查）编一套库/程序/测试，把 lib.sh、block.sh、rules.sh、format.sh 原样再跑一遍
#	cd nbtrw && sh test/safe.sh
#	产物都在 test/work/asan/（不碰仓库根上的正式产物）；任何 sanitizer 报错退出码是 86，脚本直接失败。
set -e
cd "$(dirname "$0")/.."
. test/work.sh
A=test/work/asan
rm -rf $A
mkdir -p $A
SAN="-fsanitize=address -fno-omit-frame-pointer -g -O1"
echo 'int main(){return 0;}' > $A/probe.cpp
if g++ -fsanitize=undefined $A/probe.cpp -o $A/probe 2>/dev/null; then
	SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
else
	echo "（这台机器没装 UBSan 运行库 libubsan，只开 ASan+LSan）"
fi
F="-std=c++23 -Wall -Wextra -pedantic -fPIC $SAN -I."
L="-L$A -lnbtrw -lz -Wl,-rpath,\$ORIGIN"
echo "== 编译（ASan/UBSan）=="
g++ $F -shared nbt.cpp atom.cpp chunk.cpp mca.cpp level.cpp -o $A/libnbtrw.so -lz -llz4 -lxxhash
g++ $F -shared -x c++ rules.cpp.26.1.1 -x none -o $A/rules.so.26.1.1 $L
g++ $F -DRULES_DATA_VERSION=5119 -shared -x c++ rules.cpp.26.1.1 -x none -o $A/rules5119.so $L
g++ $F merge.cpp -o $A/merge $L
g++ $F fixer.cpp -o $A/fixer $L -ldl
for t in lib lvl bad mkmerge checkmerge mkfix2 checkfix2 checkzero mkrule mkfmt checkfmt; do
	g++ $F test/$t.cpp -o $A/$t $L
done
g++ $F test/poidump.cpp -o $A/poidump $L -ldl
export ASAN_OPTIONS=exitcode=86:detect_leaks=1
export UBSAN_OPTIONS=exitcode=86:print_stacktrace=1:halt_on_error=1
echo "== lib =="
B=$A sh test/lib.sh
echo "== block =="
B=$A sh test/block.sh > test/work/safe_block.log 2>&1 || { tail -n 40 test/work/safe_block.log; echo "FAIL: block（ASan）"; exit 1; }
tail -n 1 test/work/safe_block.log
echo "== rules =="
B=$A sh test/rules.sh > test/work/safe_rules.log 2>&1 || { tail -n 40 test/work/safe_rules.log; echo "FAIL: rules（ASan）"; exit 1; }
tail -n 1 test/work/safe_rules.log
echo "== format =="
B=$A sh test/format.sh > test/work/safe_format.log 2>&1 || { tail -n 40 test/work/safe_format.log; echo "FAIL: format（ASan）"; exit 1; }
tail -n 1 test/work/safe_format.log
echo "safe OK"
