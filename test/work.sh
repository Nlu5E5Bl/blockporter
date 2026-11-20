# work.sh：各测试脚本开头 . 进来（从 nbtrw/ 跑）。test/work 是指到 /tmp 下的符号链接：
#	测试造的世界、副本、日志、编出来的辅助程序都能任意多次重新生成，一律放 /tmp（tmpfs，重启就没了；/home 留给源码和真存档）。
#	目标目录默认 /tmp/nbtrw-work，NBTRW_WORK=<目录> 可以换。链接在、目标没了（重启过）就重建目标。
T=${NBTRW_WORK:-/tmp/nbtrw-work}
if [ -L test/work ]; then
	mkdir -p "$(readlink test/work)"
elif [ -e test/work ]; then
	echo "work.sh: test/work 是普通目录，不是指到 /tmp 的链接；先把它挪走（mv test/work $T）再跑" >&2
	exit 1
else
	mkdir -p "$T"
	ln -s "$T" test/work
fi
