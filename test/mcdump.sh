#!/bin/sh
# mcdump.sh：手动工具——拿原版服务端 jar 让游戏自己判，和 rules.so 对拍（规则/形状的第一依据，见 ReadMeDeveloper 1.8/1.9）
#   sh test/mcdump.sh <server.jar> [shape|cmp|region|upgrade|all|probe|fit] [rules.so.<版本>] [方块id,方块id...]
#     插件不给就用 ./rules.so.<jar 的版本>
#     shape  游戏导出每个方块状态的支撑面/碰撞面/实心（shapes.tsv），逐状态比 rules.so 的形状位图（shapes-diff.tsv，应该是空的）
#     cmp    游戏判定 vs rules.so：每个要支撑的方块状态（加上栅栏/墙这些规则表管的）、六个邻居轮流换成全部方块状态（rules-cmp.txt；
#            “#差异合计”应该是“没有”，故意不一样的在 RulesCmp.java 的 known 表里，单独列成“#已知”）。20 核约 20 分钟
#     region 游戏的 RegionFile 和 libnbtrw 的 MCA 互相读对方写的区域文件：gzip/deflate/none/lz4 四种压缩，
#            外加压不小、要放外置 .mcc 的大区块；再拿游戏的 BitStorageAlignFix 核对 2527 以前紧排下标的读法
#            （regioncmp.cpp + RegionProbe.java；结果 region/check.txt）
#     upgrade 段的方块状态（2832 以前的 Palette/BlockStates、之后的 block_states，各版本）：游戏先用 DataFixers 升级、再用编解码读，
#            和库的读法/判坏逐格比
#            （upgradecmp.cpp + UpgradeProbe.java；结果 upgrade/check.txt）
#     all    shape + cmp（默认）
#     probe  规则从游戏生成（ReadMeDeveloper 1.9）的原型：每种方块靠哪些邻居活（test/mc/probe/SurviveProbe.java → probe.tsv）
#     fit    同上：只靠一个邻居活的方块，把支撑条件拟合成谓词/标签（test/mc/probe/FitProbe.java → fit.tsv）
#   方块id 只给 cmp 用：只测这几种（比如 vine,oak_door），改一条规则时快速看。
# 从 nbtrw/ 跑。server.jar 是官方下载的服务端启动器包（里面有 META-INF/versions.list、classpath-joined、libraries/）。
# 只读这个 jar：解到 test/work/mc/<版本>/，结果也写在那里。只认 26.x（不混淆）；要 Java 25。
set -e
. test/work.sh
JAR=$1
CMD=${2:-all}
SO=$3
ONLY=$4
if [ ! -f "$JAR" ]; then
	echo "用法: sh test/mcdump.sh <server.jar> [shape|cmp|region|upgrade|all|probe|fit] [rules.so] [方块id,...]" >&2
	exit 1
fi
case "$CMD" in shape|cmp|probe|fit|region|upgrade|all) ;; *) echo "mcdump: 不认识 $CMD" >&2; exit 1;; esac
VER=$(unzip -p "$JAR" META-INF/versions.list | cut -f2 | head -n1)
if [ -z "$VER" ]; then
	echo "mcdump: $JAR 里没有 META-INF/versions.list，不是服务端启动器包" >&2
	exit 1
fi
W=test/work/mc/$VER
SO=${SO:-./rules.so.$VER}   # 不给插件就用和 jar 同一个版本的那个
mkdir -p "$W/classes"
if [ ! -d "$W/META-INF/versions" ]; then
	unzip -q -o "$JAR" 'META-INF/versions/*' 'META-INF/libraries/*' 'META-INF/classpath-joined' -d "$W"
fi
CP=$(tr ';' '\n' < "$W/META-INF/classpath-joined" | sed "s#^#$PWD/$W/META-INF/#" | paste -sd:)
JAVA_SRC="test/mc/*.java"
if [ "$CMD" = probe ] || [ "$CMD" = fit ]; then
	JAVA_SRC="$JAVA_SRC test/mc/probe/*.java"
fi
javac -nowarn -cp "$CP" -d "$W/classes" $JAVA_SRC
SOABS=$(realpath "$SO")
# 在 $W 里跑 Java：游戏的 log4j 会在当前目录建 logs/
run_Java(){ (cd "$W" && java --enable-native-access=ALL-UNNAMED -cp "$PWD/classes:$CP" "$@"); }
if [ "$CMD" = shape ] || [ "$CMD" = all ]; then
	run_Java ShapeDump > "$W/shapes.tsv" 2> "$W/shapes.log"
	g++ -std=c++23 -O2 -I. test/shapecmp.cpp -o test/work/shapecmp -L. -lnbtrw -lz -ldl -Wl,-rpath,"$PWD"
	test/work/shapecmp "$SOABS" < "$W/shapes.tsv" > "$W/shapes-diff.tsv"
	echo "shape: $(grep -vc '^[#@]' "$W/shapes.tsv") 个方块状态（另有 $(grep -c '^@off' "$W/shapes.tsv") 行随坐标偏移的），和 $SO 不一致 $(wc -l < "$W/shapes-diff.tsv") 处 -> $W/shapes-diff.tsv"
fi
if [ "$CMD" = cmp ] || [ "$CMD" = all ]; then
	g++ -std=c++23 -O2 -fPIC -shared -I. test/mc/rules_shim.cpp -o test/work/librules_shim.so -L. -lnbtrw -lz -ldl -Wl,-rpath,"$PWD"
	run_Java RulesCmp "$PWD/test/work/librules_shim.so" "$SOABS" $ONLY > "$W/rules-cmp.txt" 2> "$W/rules-cmp.log"
	echo "cmp: $(grep '^#差异合计' "$W/rules-cmp.txt") -> $W/rules-cmp.txt"
fi
if [ "$CMD" = region ]; then
	R=$PWD/$W/region
	rm -rf "$R" && mkdir -p "$R"
	g++ -std=c++23 -O2 -I. test/regioncmp.cpp -o test/work/regioncmp -L. -lnbtrw -lz -Wl,-rpath,"$PWD"
	test/work/regioncmp prep "$R" > "$R/prep.txt" || { cat "$R/prep.txt"; exit 1; }
	for c in gzip deflate none lz4; do
		mkdir -p "$R/java_$c"
		run_Java RegionProbe write "$R/java_$c" 0 0 $c "$R/small.nbt" 1 2 "$R/big.nbt" 3 4 >> "$R/java.log" 2>&1
		run_Java RegionProbe read "$R/lib_$c" 0 0 1 2 "$R/out_${c}_small.nbt" >> "$R/java.log" 2>&1
		run_Java RegionProbe read "$R/lib_$c" 0 0 3 4 "$R/out_${c}_big.nbt" >> "$R/java.log" 2>&1
	done
	run_Java RegionProbe align "$R/span.nbt" "$R/aligned.nbt" >> "$R/java.log" 2>&1
	test/work/regioncmp check "$R" > "$R/check.txt" || true
	echo "region: $(tail -n 1 "$R/check.txt") -> $W/region/check.txt"
fi
if [ "$CMD" = upgrade ]; then
	U=$PWD/$W/upgrade
	rm -rf "$U" && mkdir -p "$U"
	g++ -std=c++23 -O2 -I. test/upgradecmp.cpp -o test/work/upgradecmp -L. -lnbtrw -lz -Wl,-rpath,"$PWD"
	test/work/upgradecmp prep "$U" > "$U/prep.txt" || { cat "$U/prep.txt"; exit 1; }
	run_Java UpgradeProbe "$U" > "$U/java.log" 2>&1
	run_Java UpgradeProbe ceillog2 "$U/ceillog2.txt" >> "$U/java.log" 2>&1
	test/work/upgradecmp check "$U" > "$U/check.txt" || true
	echo "upgrade: $(tail -n 1 "$U/check.txt") -> $W/upgrade/check.txt"
fi
if [ "$CMD" = probe ]; then
	run_Java SurviveProbe > "$W/probe.tsv" 2> "$W/probe.log"
	echo "probe: -> $W/probe.tsv"
fi
if [ "$CMD" = fit ]; then
	run_Java FitProbe > "$W/fit.tsv" 2> "$W/fit.log"
	echo "fit: -> $W/fit.tsv"
fi
