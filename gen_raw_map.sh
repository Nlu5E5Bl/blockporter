#!/usr/bin/env bash
# =============================================================================
# gen_raw_map.sh —— Fabric 服务端 + Chunky(+C2ME) 纯净原始地图生成（命令透传版）
#
# 设计原则（按使用要求）：
#   * 脚本只做四件事：拉起 Fabric 服务端、把你的 Chunky 命令原样透传、按「组」分批推进、日志停止刷新后收尾
#   * 不解析地图范围、不校验参数、不检查落盘率 —— Chunky 的特性/bug 由使用者承担
#   * 服务端打印 "Done (" 的同一秒，就把「一条 tick 命令 + 第一组命令」直接
#     写进控制台输入缓冲区（FIFO），不等回显、不等执行结果
#     （不能再早：世界加载完成前写入的命令，在 MC 26.1+ 会全部失败，见下方说明）
#   * 分块执行：一个 -c 是「一组」（组内可以再用半角逗号分成多条命令，顺序下发），
#     多个 -c 从前到后一组一组跑；每组跑完（判据同收尾：日志静默 --idle-timeout 秒）
#     就下发一次 save-all flush（把这一批生成的脏区块全部写盘），再等 --batch-sleep 秒
#     （默认 30）才下发下一组。最后一组之后同样 flush + 等待，然后才 stop
#   * 不设置 chunky quiet：让 Chunky 持续刷屏，用「日志静默 N 秒（默认 10）」判定每组的结束
#   * 不修改任何游戏规则（gamerule），不改世界时间
#   * 收尾用 stop，由服务端自身流程保存世界（save-all flush 只出现在批与批之间/末尾）；
#     stop 之后默认最多等 600s 才 SIGTERM、再等 120s 才 kill -9（大世界写盘慢，不抢跑）
#   * 每次启动把上一次的 console.log 轮转成 console.<时间戳>.log，不删崩溃/退化证据
#   * 默认不给 java 加任何 JVM 参数（用 JVM 默认值）；要加就 --memory / --jvm-args 手动传
#
# 已知坑（均实测，细节/复现方法见 README「已知坑（实测汇总）」）：
#   1) 命令必须等 "Done (" 之后再写。控制台线程是「读到行」时就把 CommandSourceStack
#      建好，世界没加载完时它的 getLevel() 是 null；MC 26.1+ 里所有命令都会取
#      getLevel().getGameRules()，于是每条命令都变成
#          An unexpected error occurred trying to execute that command
#   2) 上面那句默认就是全部信息：真实异常藏在悬停提示里、堆栈只在 debug 级别记录。
#      加 --debug-commands 重跑才会在 console.log 里看到异常类型与堆栈。
#   3) /tick freeze 是纯内存状态（TickRateManager 没有任何 NBT 读写），不写进存档：
#      生成好的地图交给真服务端不会默认冻结；stop 前也不需要解冻 —— 冻结下控制台
#      照常处理命令，实测 stop 后每个维度都完整保存（All chunks are saved）。
#   4) 冻结期间世界时间不推进。26.1 的 level.dat 只有 Time（没有 DayTime 键）：
#      冻结跑完 Time=5；同样一张图不冻结跑 10 秒 Time=+205 tick（≈20/s）。
#   5) "A single server tick took ..." 只由看门狗线程打印，脚本已设 max-tick-time=-1，
#      不会把慢 tick 误判成崩溃；同理 max-tick-time 也避免了看门狗强杀生成中的服务端。
#   6) 断点继续靠 Chunky 自己的存档任务：<work>/server/config/chunky/tasks/<ns>/<world>.properties
#      （cancelled=false 且 chunks=<已处理数>）。加 --resume 时脚本把那个世界的
#      `chunky start ...` 改写成 `chunky continue <world>`，从存档计数接着跑；不加 --resume 时
#      `chunky start` 会被 Chunky 拒绝（"A task was already started for this world..."），
#      那个维度本次根本不会开始生成。进度只在「任务跑完」和「服务端正常停止」时写盘，
#      被 kill -9 / 断电 / 崩溃会丢掉上次进度（但已落盘的区块还在，重跑会跳过它们）。
#   7) 分块执行的边界也是「日志静默 --idle-timeout 秒」：Chunky 生成期间每秒都在刷进度行，
#      所以静默 = 这一组没有任务在跑了。如果某次自动保存/卡顿让日志 >--idle-timeout 秒没刷新，
#      本组会被提前判结束（下一组命令提前下发）；已下发的任务不会被取消，只是分块边界变模糊，
#      把 --idle-timeout 调大（如 60~120）就能避免。
#   8) 一条 -c 里的半角逗号是「多条命令」的分隔符；命令本身带逗号（比如 NBT/JSON）
#      写在 {} [] 或 '...' "..." 里面就不会被拆开，也可以用 \, 表示一个字面逗号。
#      真的是「一条命令里带裸逗号」且不在括号/引号里的情况，脚本会把它拆错 —— 请自行加引号或转义。
#
# 退出码：0 正常；1 参数/依赖/下载/启动失败，或服务端崩溃、提前退出
# =============================================================================
set -Eeuo pipefail

SCRIPT_NAME="$(basename "$0")"
SCRIPT_VERSION="2.3.1"

# ---------------------------------------------------------------- 输出与日志
if [ -t 2 ]; then
    C_INFO=$'\033[36m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_OK=$'\033[32m'; C_OFF=$'\033[0m'
else
    C_INFO=; C_WARN=; C_ERR=; C_OK=; C_OFF=
fi
RUN_LOG=""
logline() { if [ -n "$RUN_LOG" ]; then printf '%s %s\n' "$(date '+%F %T')" "$*" >>"$RUN_LOG" 2>/dev/null || true; fi; }
info() { printf '%s[信息]%s %s\n' "$C_INFO" "$C_OFF" "$*" >&2; logline "[信息] $*"; }
warn() { printf '%s[警告]%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; logline "[警告] $*"; }
ok()   { printf '%s[完成]%s %s\n' "$C_OK"   "$C_OFF" "$*" >&2; logline "[完成] $*"; }
die()  { printf '%s[错误]%s %s\n' "$C_ERR"  "$C_OFF" "$*" >&2; logline "[错误] $*"; exit 1; }

# ---------------------------------------------------------------- 默认参数
MC_VERSION=""
SEED=""
CMDS=()                     # 原样透传的命令（整理后是按顺序排好的扁平列表）
CMD_GROUP=()                # 与 CMDS 一一对应：每条命令属于第几组（0 起）
N_GROUPS=0                  # 组数（一个 -c 一组；组内还可用逗号分多条）
BATCH_SLEEP=30              # 每组 save-all flush 之后等待的秒数
BATCH_FLUSH=1               # 每组结束后是否 save-all flush
MODS_ZIPS=()
SERVER_PROPS=()
JVM_ARGS_EXTRA=()           # --jvm-args 手动追加的 JVM 参数（默认一个都不加）
WORK_DIR=""
WORLD_DIR=""
MEMORY=""                   # --memory 指定时才加 -Xmx（默认不加任何 JVM 参数）
JAVA_BIN=""
LOADER_VER=""
USE_C2ME=1
REQUIRE_C2ME=0
LEVEL_TYPE="minecraft:normal"
GENERATOR_SETTINGS="{}"
GEN_STRUCTURES=1
TICK_CMD="tick freeze"
IDLE_TIMEOUT=10
START_TIMEOUT=900
DRY_RUN=0
REUSE=0
RESUME=0
FORCE=0
DEBUG_COMMANDS=0
STOP_TIMEOUT=600            # stop 之后最多等多少秒（大世界写盘可能好几分钟）
STOP_KILL_TIMEOUT=120       # SIGTERM 之后再等多少秒才 kill -9
SKIP_DISK_CHECK=0           # --skip-disk-check：跳过可用空间检查（--force 不再自动跳过）

MODRINTH="https://api.modrinth.com/v2"
FABRIC_META="https://meta.fabricmc.net/v2"

# ---------------------------------------------------------------- 帮助
usage() {
cat <<EOF
$SCRIPT_NAME v$SCRIPT_VERSION —— Fabric + Chunky(+C2ME) 纯净地图生成（命令原样透传）

用法:
  $SCRIPT_NAME -s <种子> -v <游戏版本> -c '<chunky 命令>' [-c '<又一条>'] [选项]

必需:
  -s, --seed <种子>            世界种子（数字或字符串，原样写入 level-seed）
  -v, --game-version <版本>    Minecraft 版本，如 1.21.1
  -c, --command <命令>         原样透传到服务端控制台的命令；一条 -c = 一批（一组）：
                                 * 组内想要多条命令，用半角逗号分隔，按书写顺序下发
                                 * 多个 -c 从前到后一组一组执行（上一组跑完才下发下一组）
                                 * 命令本身带逗号（NBT/JSON）时，写进 {} [] 或引号里不会被拆，
                                   也可以用 \, 表示一个字面逗号
                                 * 每组跑完（日志静默 --idle-timeout 秒）脚本会下发
                                   save-all flush 并等 --batch-sleep 秒，再下发下一组
                                 * 最后一组之后同样 flush + 等待，然后才 stop
                               脚本不解析范围、不校验参数、不等待回显；前导 / 会自动去掉
                               例：-c 'chunky start minecraft:overworld circle 0 0 2000'
                                   -c 'chunky start minecraft:the_nether circle 0 0 2000'
                                   -c 'chunky start minecraft:overworld circle 0 0 1000, chunky start minecraft:the_nether circle 0 0 1000'

可选:
  -m, --mods-zip <路径>        额外模组 zip（内部 *.jar 解压到 mods/）或 jar 目录，可重复
      --work-dir <目录>        工作目录，默认 ./work/<版本>-<种子>
      --world-dir <目录>       世界输出目录，默认 <work-dir>/world
      --memory <大小>          JVM 最大堆：加 -Xmx<大小>（默认不加任何 JVM 参数，
                               即用 JVM 默认值；堆上限默认为物理内存的 1/4）
      --jvm-args '<参数>'      手动追加 JVM 参数，原样拼在 java 后面、-jar 前面，可重复
                               例：--jvm-args '-Xms8G -Xmx8G -XX:+UseZGC'
                               （整串按空格拆开传给 java，需要带空格的单个参数请自己加引号）
      --java <路径>            java 可执行文件（默认按 MC 版本自动挑选）
      --loader <版本>          Fabric loader 版本（默认该 MC 版本最新 stable）
      --no-c2me                不安装 C2ME
      --require-c2me           该版本没有 C2ME 构建时报错退出（默认仅警告跳过）
      --level-type <类型>      默认 minecraft:normal
      --generator-settings <s> level-type 的附加设置，默认 {}
      --no-structures          关闭结构生成（generate-structures=false）
      --tick-cmd <命令>        tick 命令，默认 'tick freeze'；1.20.3 以下版本自动跳过
      --no-tick                完全不发 tick 命令
      --debug-commands         诊断用：让服务端把「命令抛异常」的真实原因和堆栈打进日志
                               （即加 -DMC_DEBUG_ENABLED -DMC_DEBUG_VERBOSE_COMMAND_ERRORS）
      --idle-timeout <秒>      日志静默多少秒后判定「这一组跑完」/「全部跑完」，默认 10
                               （自动保存卡顿时日志可能十几秒不刷新，大世界建议 60~120）
      --batch-sleep <秒>       每批 save-all flush 之后等待多少秒再下发下一批，默认 30
                               （0 = 不等待；最后一组之后同样会等待）
      --no-batch-flush         不在批与批之间下发 save-all flush（只等 --batch-sleep 秒）
      --start-timeout <秒>     等待服务端启动（Done）上限，默认 900
      --stop-timeout <秒>      stop 之后最多等多少秒（服务端在保存世界），默认 600
                               （大世界写盘可能好几分钟；到点才发 SIGTERM，不是直接杀）
      --stop-kill-timeout <秒> SIGTERM 之后再等多少秒才 kill -9，默认 120
      --skip-disk-check        跳过「可用空间不足 5GB」检查（默认 --force 也要检查）
      --server-property K=V    覆盖/追加 server.properties 配置项，可重复
      --resume                 断点继续：把有「未完成存档任务」的世界的 chunky start ...
                               改写成 chunky continue <世界>，从上次进度接着生成
                               （隐含 --reuse；要求 -v/-s 或 --work-dir 与上次相同）
      --reuse                  世界已存在时复用
      --force                  世界已存在时删除重建（同时清掉旧的 Chunky 存档任务）
      --dry-run                只解析并打印计划（含将要下发的命令），不下载不启动
  -h, --help                   显示本帮助

示例:
  # 主世界以 (0,0) 为中心、半径 2000 方块生成
  $SCRIPT_NAME -s 12345 -v 1.21.1 -c 'chunky start minecraft:overworld circle 0 0 2000'

  # 一次生成三个维度
  $SCRIPT_NAME -s 12345 -v 1.21.1 -m ./mods.zip \\
      -c 'chunky start minecraft:overworld circle 0 0 5000' \\
      -c 'chunky start minecraft:the_nether circle 0 0 5000' \\
      -c 'chunky start minecraft:the_end circle 0 0 5000'

  # 上次没跑完（Ctrl-C、静默收尾、崩溃都算），接着跑：命令一个字都不用改
  $SCRIPT_NAME -s 12345 -v 1.21.1 --resume -c 'chunky start minecraft:overworld circle 0 0 5000'

  # 分块执行：一批一批来，每批跑完 save-all flush + 等 60s 再下一批
  $SCRIPT_NAME -s 12345 -v 1.21.1 --idle-timeout 60 --batch-sleep 60 \\
      -c 'chunky start minecraft:overworld circle 0 0 2000' \\
      -c 'chunky start minecraft:the_nether circle 0 0 2000'

  # 一条 -c 里用逗号写多条命令（= 同一批，两条 start 会同时开始生成）
  $SCRIPT_NAME -s 12345 -v 1.21.1 \\
      -c 'chunky start minecraft:overworld circle 0 0 1000, chunky start minecraft:the_nether circle 0 0 1000'

  # 手动指定 JVM 参数（默认一个都不加，用 JVM 默认值）
  $SCRIPT_NAME -s 12345 -v 1.21.1 --jvm-args '-Xms8G -Xmx8G' -c 'chunky start minecraft:overworld circle 0 0 2000'

  # 用 chunky 自己的分步命令（同样只是透传；--resume 也会跟踪 world/center/radius/shape）
  $SCRIPT_NAME -s 12345 -v 1.21.1 -c 'chunky world mymod:skylands' \\
      -c 'chunky center 0 0' -c 'chunky radius 1000 1000' -c 'chunky shape diamond' -c 'chunky start'
EOF
}

# ---------------------------------------------------------------- 参数解析
need_val() { [ -n "${2:-}" ] || die "选项 $1 需要一个值"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -s|--seed)            need_val "$1" "${2:-}"; SEED="$2"; shift 2 ;;
        -v|--game-version)    need_val "$1" "${2:-}"; MC_VERSION="$2"; shift 2 ;;
        -c|--command)         need_val "$1" "${2:-}"; CMDS+=("$2"); shift 2 ;;
        -m|--mods-zip)        need_val "$1" "${2:-}"; MODS_ZIPS+=("$2"); shift 2 ;;
        --server-property)    need_val "$1" "${2:-}"; SERVER_PROPS+=("$2"); shift 2 ;;
        --work-dir)           need_val "$1" "${2:-}"; WORK_DIR="$2"; shift 2 ;;
        --world-dir)          need_val "$1" "${2:-}"; WORLD_DIR="$2"; shift 2 ;;
        --memory)             need_val "$1" "${2:-}"; MEMORY="$2"; shift 2 ;;
        --jvm-args)           need_val "$1" "${2:-}"; JVM_ARGS_EXTRA+=("$2"); shift 2 ;;
        --java)               need_val "$1" "${2:-}"; JAVA_BIN="$2"; shift 2 ;;
        --loader)             need_val "$1" "${2:-}"; LOADER_VER="$2"; shift 2 ;;
        --no-c2me)            USE_C2ME=0; shift ;;
        --require-c2me)       REQUIRE_C2ME=1; shift ;;
        --level-type)         need_val "$1" "${2:-}"; LEVEL_TYPE="$2"; shift 2 ;;
        --generator-settings) need_val "$1" "${2:-}"; GENERATOR_SETTINGS="$2"; shift 2 ;;
        --no-structures)      GEN_STRUCTURES=0; shift ;;
        --tick-cmd)           need_val "$1" "${2:-}"; TICK_CMD="$2"; shift 2 ;;
        --no-tick)            TICK_CMD=""; shift ;;
        --debug-commands)     DEBUG_COMMANDS=1; shift ;;
        --idle-timeout)       need_val "$1" "${2:-}"; IDLE_TIMEOUT="$2"; shift 2 ;;
        --batch-sleep)        need_val "$1" "${2:-}"; BATCH_SLEEP="$2"; shift 2 ;;
        --no-batch-flush)     BATCH_FLUSH=0; shift ;;
        --start-timeout)      need_val "$1" "${2:-}"; START_TIMEOUT="$2"; shift 2 ;;
        --stop-timeout)       need_val "$1" "${2:-}"; STOP_TIMEOUT="$2"; shift 2 ;;
        --stop-kill-timeout)  need_val "$1" "${2:-}"; STOP_KILL_TIMEOUT="$2"; shift 2 ;;
        --skip-disk-check)    SKIP_DISK_CHECK=1; shift ;;
        --reuse)              REUSE=1; shift ;;
        --resume)             RESUME=1; REUSE=1; shift ;;
        --force)              FORCE=1; shift ;;
        --dry-run)            DRY_RUN=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *)                    usage >&2; die "未知选项: $1" ;;
    esac
done

[ -n "$MC_VERSION" ] || { usage >&2; die "缺少 -v/--game-version"; }
[ -n "$SEED" ]       || { usage >&2; die "缺少 -s/--seed"; }
[ "${#CMDS[@]}" -gt 0 ] || { usage >&2; die "缺少 -c/--command（至少要一条命令）"; }
SEED="${SEED%$'\n'}"
[[ "$IDLE_TIMEOUT" =~ ^[0-9]+$ ]] || die "--idle-timeout 需要非负整数"
[[ "$BATCH_SLEEP" =~ ^[0-9]+$ ]] || die "--batch-sleep 需要非负整数"
[[ "$START_TIMEOUT" =~ ^[0-9]+$ ]] || die "--start-timeout 需要非负整数"
[[ "$STOP_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || die "--stop-timeout 需要正整数（秒）"
[[ "$STOP_KILL_TIMEOUT" =~ ^[0-9]+$ ]] || die "--stop-kill-timeout 需要非负整数（秒）"
if [ "$RESUME" = 1 ] && [ "$FORCE" = 1 ]; then
    die "--resume 与 --force 冲突：--force 会删掉世界并清空 Chunky 存档任务，没有可继续的进度"
fi
if [ -n "$MEMORY" ]; then
    [[ "$MEMORY" =~ ^[0-9]+[kKmMgGtT]?$ ]] || die "--memory 需要形如 8G / 4096M / 8192 的值: $MEMORY"
fi
for _a in ${JVM_ARGS_EXTRA[@]+"${JVM_ARGS_EXTRA[@]}"}; do
    case "$_a" in
        *$'\n'*) die "--jvm-args 里不能有换行（多个参数请分开写多次 --jvm-args）" ;;
    esac
done
unset _a

# 命令整理与分组：一个 -c 是一组，组内还可以用半角逗号分成多条命令。
#   * 分隔符：顶层（不在括号/引号里）的半角逗号，以及换行
#   * 命令本身带逗号（NBT/JSON）时，写进 {} [] 或 '...' "..." 里就不会被拆开
#   * \, 表示一个字面逗号（反斜杠被吃掉）；其他位置的 \ 原样保留
#   * 顺序：组内按书写顺序，组与组之间按 -c 出现的顺序
# 例：-c 'chunky world minecraft:overworld, chunky radius 2000' → 同一组里两条命令
#     -c 'chunky start minecraft:overworld circle 0 0 2000' -c 'chunky start ...' → 两组
split_cmds() { # 输入一个字符串，输出一行一条命令
    local s="$1" i=0 n=${#1} ch out="" depth=0 quote="" nx=""
    local LC_ALL=C   # 逐字节处理，保证非 ASCII 内容原样透传
    while [ "$i" -lt "$n" ]; do
        ch="${s:i:1}"; i=$((i + 1))
        if [ "$ch" = '\' ] && [ "$i" -lt "$n" ]; then
            nx="${s:i:1}"
            if [ "$nx" = ',' ] || [ "$nx" = $'\n' ]; then
                out+="$nx"; i=$((i + 1)); continue          # \, 与 \<换行> 都是字面字符
            fi
            if [ "$nx" = '\' ]; then
                # \\ 原样保留：否则 \\, 会被当成「\,」而把本该是分隔符的逗号吃掉
                out+='\\'; i=$((i + 1)); continue
            fi
            out+='\'; continue
        fi
        if [ -n "$quote" ]; then
            [ "$ch" = "$quote" ] && quote=""
            out+="$ch"; continue
        fi
        case "$ch" in
            "'"|'"') quote="$ch"; out+="$ch" ;;
            '{'|'[') depth=$((depth + 1)); out+="$ch" ;;
            '}'|']') [ "$depth" -gt 0 ] && depth=$((depth - 1)); out+="$ch" ;;
            ','|$'\n')
                if [ "$depth" -eq 0 ]; then printf '%s\n' "$out"; out=""
                else out+="$ch"; fi ;;
            *) out+="$ch" ;;
        esac
    done
    if [ -n "$quote" ] || [ "$depth" -ne 0 ]; then
        warn "这条 -c 里的引号/括号没配对，逗号可能没被当作分隔符，请确认命令是不是你想要的: $s"
    fi
    printf '%s\n' "$out"
}

is_task_cmd() { # 这条命令是不是「启动/继续一个生成任务」（用于「本组要不要等 Chunky 输出」）
    local -a tok=()
    read -r -a tok <<<"${1:-}"
    [ "${tok[0]:-}" = "chunky" ] || return 1
    case "${tok[1]:-}" in
        start|continue|confirm) return 0 ;;
        *) return 1 ;;
    esac
}

group_cmds() { # 组号 -> 该组命令（一行一条，保持顺序）
    local g="$1" i
    for i in "${!CMDS[@]}"; do
        if [ "${CMD_GROUP[$i]}" = "$g" ]; then printf '%s\n' "${CMDS[$i]}"; fi
    done
    return 0
}

group_has_task_cmd() { # 组号 -> 本组是否含生成类命令
    local c
    while IFS= read -r c; do
        is_task_cmd "$c" && return 0
    done < <(group_cmds "$1")
    return 1
}

CLEAN_CMDS=(); CLEAN_GROUPS=(); N_GROUPS=0
for _raw in "${CMDS[@]}"; do
    _g_has=0
    while IFS= read -r _one; do
        _one="${_one#"${_one%%[![:space:]]*}"}"     # 去首尾空白
        _one="${_one%"${_one##*[![:space:]]}"}"
        _one="${_one#/}"                            # 服务端控制台用不带 / 的形式
        if [ -n "$_one" ]; then
            CLEAN_CMDS+=("$_one"); CLEAN_GROUPS+=("$N_GROUPS"); _g_has=1
        fi
    done < <(split_cmds "$_raw")
    if [ "$_g_has" = 1 ]; then N_GROUPS=$((N_GROUPS + 1)); fi   # 全空的 -c 不占组号
done
unset _raw _one _g_has
[ "${#CLEAN_CMDS[@]}" -gt 0 ] || die "命令为空"
CMDS=("${CLEAN_CMDS[@]}")
CMD_GROUP=("${CLEAN_GROUPS[@]}")
unset CLEAN_CMDS CLEAN_GROUPS

# ---------------------------------------------------------------- 依赖命令
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"; }
need_cmd curl; need_cmd jq; need_cmd sha1sum

# ---------------------------------------------------------------- HTTP 工具
http_json() { curl -fsS --retry 3 --retry-delay 2 --connect-timeout 15 --max-time 90 "$@"; }

verify_hash() { # file sha1 sha512
    local f="$1" s1="$2" s5="$3"
    [ -s "$f" ] || return 1
    if [ -n "$s5" ]; then
        [ "$(sha512sum "$f" | cut -d' ' -f1)" = "$s5" ] || return 1
    fi
    if [ -n "$s1" ]; then
        [ "$(sha1sum "$f" | cut -d' ' -f1)" = "$s1" ] || return 1
    fi
    return 0
}

fetch_file() { # url dest [sha1] [sha512]
    local url="$1" dest="$2" sha1="${3:-}" sha512="${4:-}"
    mkdir -p "$(dirname "$dest")"
    if [ -s "$dest" ] && verify_hash "$dest" "$sha1" "$sha512"; then
        info "使用缓存: $(basename "$dest")"
        return 0
    fi
    info "下载: $(basename "$dest")"
    if ! curl -fL -sS --retry 3 --retry-delay 2 --connect-timeout 20 -o "$dest.part" "$url"; then
        rm -f "$dest.part"; return 1
    fi
    if ! verify_hash "$dest.part" "$sha1" "$sha512"; then
        rm -f "$dest.part"; warn "校验失败: $url"; return 1
    fi
    mv -f "$dest.part" "$dest"
    info "  完成: $(basename "$dest") ($(( $(stat -c%s "$dest") / 1024 )) KB)"
}

# ---------------------------------------------------------------- 版本工具
ver_parts() {
    local v="$1" a b c
    v="${v%%-*}"; v="${v%%+*}"
    a="${v%%.*}"; v="${v#*.}"
    b="${v%%.*}"; c=""
    case "$v" in *.*) c="${v#*.}" ;; esac
    c="${c%%.*}"
    printf '%s %s %s\n' "${a:-0}" "${b:-0}" "${c:-0}"
}

mc_ge() { # mc_ge A B -> A >= B ?
    local a1 a2 a3 b1 b2 b3
    read -r a1 a2 a3 < <(ver_parts "$1")
    read -r b1 b2 b3 < <(ver_parts "$2")
    [ "$a1" -gt "$b1" ] && return 0
    [ "$a1" -lt "$b1" ] && return 1
    [ "$a2" -gt "$b2" ] && return 0
    [ "$a2" -lt "$b2" ] && return 1
    [ "$a3" -ge "$b3" ] && return 0
    return 1
}

required_java() { # 该 MC 版本所需的最低 Java 主版本
    local a b c
    read -r a b c < <(ver_parts "$1")
    c="${c:-0}"
    if [ "$a" != "1" ]; then printf '25\n'; return; fi
    if [ "$b" -le 16 ]; then printf '8\n'
    elif [ "$b" -eq 17 ]; then printf '16\n'
    elif [ "$b" -lt 20 ]; then printf '17\n'
    elif [ "$b" -eq 20 ] && [ "$c" -lt 5 ]; then printf '17\n'
    else printf '21\n'; fi
}

java_major() {
    local out
    out="$("$1" -version 2>&1 | head -1)" || return 1
    if [[ "$out" =~ \"1\.([0-9]+) ]]; then printf '%s\n' "${BASH_REMATCH[1]}"
    elif [[ "$out" =~ \"([0-9]+) ]]; then printf '%s\n' "${BASH_REMATCH[1]}"
    else return 1; fi
}

pick_java() {
    local req cand best="" best_major=9999 m p
    req="$(required_java "$MC_VERSION")"

    if [ -n "$JAVA_BIN" ]; then
        [ -x "$JAVA_BIN" ] || die "--java 指定的文件不可执行: $JAVA_BIN"
        m="$(java_major "$JAVA_BIN")" || die "无法识别 java 版本: $JAVA_BIN"
        [ "$m" -ge "$req" ] || die "--java 版本过低（$m < 要求的 $req）: $JAVA_BIN"
        printf '%s\n' "$JAVA_BIN"; return 0
    fi

    local -a cands=()
    if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then cands+=("$JAVA_HOME/bin/java"); fi
    for d in /usr/lib/jvm/*/bin/java /usr/lib/jvm/*/jre/bin/java; do
        if [ -x "$d" ]; then cands+=("$d"); fi
    done
    if command -v java >/dev/null 2>&1; then cands+=("$(command -v java)"); fi

    declare -A seen=()
    for p in "${cands[@]}"; do
        p="$(readlink -f "$p")"
        [ -n "${seen[$p]:-}" ] && continue
        seen[$p]=1
        m="$(java_major "$p" 2>/dev/null)" || continue
        if [ "$m" -ge "$req" ] && [ "$m" -lt "$best_major" ]; then
            best_major="$m"; best="$p"
        fi
    done
    [ -n "$best" ] || die "找不到满足要求的 Java（需要 >= $req）；可用 --java 指定"
    info "Java 选择: $best (主版本 $best_major，要求 >= $req)"
    printf '%s\n' "$best"
}

# ---------------------------------------------------------------- Modrinth
modrinth_pick() { # project game_version loader -> TSV
    local proj="$1" gv="$2" loader="$3" json
    json="$(http_json --get \
        --data-urlencode "game_versions=[\"$gv\"]" \
        --data-urlencode "loaders=[\"$loader\"]" \
        "$MODRINTH/project/$proj/version")" || return 1
    jq -r '
      ( [ .[] | select(.version_type=="release") ]
      + [ .[] | select(.version_type=="beta") ]
      + [ .[] | select(.version_type=="alpha") ] )
      | if length == 0 then empty else .[0] end
      | (.files[] | select(.primary == true)) as $f
      | [ .version_number, $f.filename, $f.url, ($f.hashes.sha1 // ""), ($f.hashes.sha512 // ""),
          ($f.size|tostring),
          ([ .dependencies[] | select(.dependency_type=="required") | .project_id ] | join(",")) ]
      | @tsv' <<<"$json"
}

declare -A MOD_SEEN=()
collect_project() { # project_id [depth]（递归解析强制依赖：Chunky -> Fabric API）
    local proj="$1" depth="${2:-0}" row vnum fname url sha1 sha512 size deps d
    [ "$depth" -gt 3 ] && return 0
    [ -n "${MOD_SEEN[$proj]:-}" ] && return 0
    MOD_SEEN[$proj]=1
    if ! row="$(modrinth_pick "$proj" "$MC_VERSION" fabric)"; then
        warn "Modrinth 查询失败: $proj"; return 1
    fi
    if [ -z "$row" ]; then
        warn "Modrinth 上没有 $proj 适配 $MC_VERSION (fabric) 的版本"
        return 1
    fi
    IFS=$'\t' read -r vnum fname url sha1 sha512 size deps <<<"$row"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$proj" "$vnum" "$fname" "$url" "$sha1" "$sha512" "$size" >>"$MOD_LIST_FILE"
    for d in ${deps//,/ }; do
        [ -n "$d" ] && collect_project "$d" $((depth+1)) || true
    done
    return 0
}

resolve_mods() {
    : >"$MOD_LIST_FILE"
    collect_project "chunky" || die "无法获取 Chunky（版本 $MC_VERSION 可能不受支持）"
    C2ME_STATUS="未启用"
    if [ "$USE_C2ME" = 1 ]; then
        if collect_project "c2me-fabric"; then
            C2ME_STATUS="已启用"
        else
            C2ME_STATUS="不可用（该版本无 C2ME 构建）"
            if [ "$REQUIRE_C2ME" = 1 ]; then die "该 MC 版本没有 C2ME 构建（去掉 --require-c2me 可继续）"; fi
            warn "跳过 C2ME：$MC_VERSION 暂无构建"
        fi
    fi
}

# ---------------------------------------------------------------- 服务端控制
send_cmd() {
    local c="$1"
    printf '%s\n' "$c" >&3 2>/dev/null || { warn "命令下发失败（服务端可能已退出）: $c"; return 1; }
    printf '[控制台输入] %s\n' "$c" >>"$CONSOLE_LOG"
}

log_since() { tail -c +"$(( $1 + 1 ))" "$CONSOLE_LOG" 2>/dev/null || true; }
# 备用的去色函数：服务端的 console.log 重定向到文件时是纯文本（实测无 ANSI / § 码），
# 分块逻辑按纯文本匹配；万一哪天日志带上颜色码，可以在这里对 chunk 过一遍再用。
strip_codes() { sed -E $'s/\x1b\\[[0-9;]*[a-zA-Z]//g; s/\xc2\xa7[0-9a-fk-orA-FK-OR]//g'; }
server_alive() { [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; }
tail_log() { tail -n "${1:-40}" "$CONSOLE_LOG" 2>/dev/null | sed 's/^/    | /' >&2 || true; }

# 崩溃特征（命中即判定服务端挂了）。注意最后两条只可能出现在「服务端自己开了 tick
# 看门狗」的时候：1.21 的 MinecraftServer 在 max-tick-length <= 0 时根本不创建看门狗
# 线程（javap 验证：`apn.bv()J; lconst_0; lcmp; ifle` 才 new ServerWatchdog），而脚本
# 写死的 max-tick-time=-1 正好把它关掉；若用 --server-property max-tick-time=<正数>
# 覆盖，看门狗会打开，慢 tick 会被服务端自己 System.exit(1)（真崩溃），那时判崩溃没错。
CRASH_PATTERNS=(
    'Failed to start the minecraft server'
    'UnsupportedClassVersionError'
    'Incompatible mod set!'
    'Missing or unsupported mandatory dependencies'
    'Considering it to be crashed'
    'A single server tick took'
    'net.fabricmc.loader.impl.FormattedException'
    'java.lang.OutOfMemoryError'
)
check_crash() { # offset -> 命中返回 0
    local off="$1" p line
    line="$(log_since "$off")"
    for p in "${CRASH_PATTERNS[@]}"; do
        if grep -qF -- "$p" <<<"$line"; then warn "检测到服务端致命错误: $p"; return 0; fi
    done
    return 1
}

start_server() { # java 可执行文件
    local jbin="$1"
    info "启动 Fabric 服务端: $jbin ${JVM_ARGS:+$JVM_ARGS }-jar $SERVER_JAR nogui"
    SERVER_LOG_OFFSET="$(stat -c%s "$CONSOLE_LOG" 2>/dev/null || echo 0)"

    # shellcheck disable=SC2086
    ( cd "$SERVER_DIR" && exec "$jbin" $JVM_ARGS -jar "$SERVER_JAR" nogui ) \
        <"$FIFO" >>"$CONSOLE_LOG" 2>&1 &
    SERVER_PID=$!
    info "服务端 PID: $SERVER_PID，日志: $CONSOLE_LOG"
}

stop_server() {
    server_alive || return 0
    info "发送 stop（服务端自行保存世界）..."
    send_cmd "stop" >/dev/null 2>&1 || true
    local i stop_off noted=0 printed=0
    stop_off="$(stat -c%s "$CONSOLE_LOG" 2>/dev/null || echo 0)"
    # 这一步只是「等它正常保存完自己退出」，不是超时即杀：
    # 半径 5000 那种世界光写盘就可能好几分钟，固定 90s 会把 SIGTERM 砍在写盘中间。
    for i in $(seq 1 "$STOP_TIMEOUT"); do
        server_alive || break
        if [ "$noted" = 0 ] && [ $(( i % 5 )) -eq 0 ] &&
           grep -qF 'All dimensions are saved' <(log_since "$stop_off"); then
            noted=1
            info "服务端已报告「All dimensions are saved」（世界写完，等进程退出）"
        fi
        if [ $(( i % 10 )) -eq 0 ]; then
            printf '\r%s[停止]%s 已等待 %ss / %ss（服务端在保存世界，Ctrl-C 可放弃等待）   ' \
                "$C_INFO" "$C_OFF" "$i" "$STOP_TIMEOUT" >&2
            printed=1
        fi
        sleep 1
    done
    if [ "$printed" = 1 ]; then printf '\n' >&2; fi
    if server_alive; then
        # SIGTERM 也不是强杀：JVM 会走 shutdown hook，服务端还有一次保存机会；
        # 只有它之后再等 STOP_KILL_TIMEOUT 秒仍不退出，才 kill -9。
        warn "服务端未在 ${STOP_TIMEOUT}s 内退出，发送 SIGTERM（再给 ${STOP_KILL_TIMEOUT}s 收尾）"
        kill "$SERVER_PID" 2>/dev/null || true
        for i in $(seq 1 "$STOP_KILL_TIMEOUT"); do
            server_alive || break
            sleep 1
        done
    fi
    if server_alive; then
        warn "SIGTERM 后仍未退出，强制结束（世界保存可能只完成一部分；Chunky 断点进度以 config/chunky/tasks/ 里最后一次写盘为准）"
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

# ---------------------------------------------------------------- 退出清理
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if server_alive; then stop_server; fi
    if [ -n "${FIFO:-}" ]; then exec 3>&- 2>/dev/null || true; rm -f "$FIFO"; fi
    exit "$rc"
}
trap cleanup EXIT
trap 'warn "收到中断信号，准备清理..."' INT TERM

# ---------------------------------------------------------------- 断点继续
# Chunky 自己就把「没跑完的任务」存盘了：
#   <work>/server/config/chunky/tasks/<namespace>/<world>.properties
# 内容是 world= / cancelled= / center-x= / center-z= / radius= / radius-z= / shape= /
# pattern= / chunks= / time=，其中 chunks= 是已迭代到的区块数。
# 对它执行 `chunky continue <world>` 时，Chunky 用 chunks 作为偏移继续迭代
# （GenerationTask 的 count 参数 -> ChunkIteratorFactory.getChunkIterator(selection, count)），
# 所以是从断点接着跑，而不是从头扫一遍。
# 反过来，如果存在这种未完成任务，`chunky start ...` 会被 Chunky 拦下来只打印
#   [Chunky] A task was already started for this world. To continue running it,
#            type '/chunky continue'. To start a new task, type '/chunky confirm'.
# 也就是「什么都不生成」。所以 --resume 的价值就是把 start 换成 continue，
# 不加 --resume 时至少要把这件事喊出来。
TASKS_DIR=""
SAVED_WORLDS=(); SAVED_DESC=(); SAVED_SHAPE=(); SAVED_CX=(); SAVED_CZ=()
SAVED_RX=(); SAVED_RZ=(); SAVED_CHUNKS=(); SAVED_FILES=()
RESUMED_WORLDS=()

scan_saved_tasks() { # 只收集 cancelled=false 的存档任务（=可以继续的）
    TASKS_DIR="$SERVER_DIR/config/chunky/tasks"
    SAVED_WORLDS=(); SAVED_DESC=(); SAVED_SHAPE=(); SAVED_CX=(); SAVED_CZ=()
    SAVED_RX=(); SAVED_RZ=(); SAVED_CHUNKS=(); SAVED_FILES=()
    [ -d "$TASKS_DIR" ] || return 0
    local f line w cancelled cx cz rx rz shape chunks
    while IFS= read -r -d '' f; do
        w=""; cancelled=""; cx=""; cz=""; rx=""; rz=""; shape=""; chunks=""
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                world=*)     w="${line#world=}" ;;
                cancelled=*) cancelled="${line#cancelled=}" ;;
                center-x=*)  cx="${line#center-x=}" ;;
                center-z=*)  cz="${line#center-z=}" ;;
                radius-x=*)  rx="${line#radius-x=}" ;;
                radius=*)    rx="${line#radius=}" ;;
                radius-z=*)  rz="${line#radius-z=}" ;;
                shape=*)     shape="${line#shape=}" ;;
                chunks=*)    chunks="${line#chunks=}" ;;
            esac
        done <"$f"
        if [ -z "$w" ]; then
            warn "读不懂这个 Chunky 任务文件（格式变了？不参与断点继续）: $f"
            continue
        fi
        if [ "$cancelled" = "true" ]; then continue; fi   # 跑完/已取消的任务不需要继续
        SAVED_WORLDS+=("$w"); SAVED_FILES+=("$f"); SAVED_SHAPE+=("$shape")
        SAVED_CX+=("$cx"); SAVED_CZ+=("$cz"); SAVED_RX+=("$rx"); SAVED_RZ+=("$rz")
        SAVED_CHUNKS+=("$chunks")
        SAVED_DESC+=("${shape:-?} 中心 ${cx:-?},${cz:-?} 半径 ${rx:-?}${rz:+ x $rz}，已处理 ${chunks:-?} 块")
    done < <(find "$TASKS_DIR" -type f -name '*.properties' -print0 2>/dev/null)
}

saved_index() { # 世界名 -> 在 SAVED_* 里的下标
    local w="$1" i
    for i in "${!SAVED_WORLDS[@]}"; do
        if [ "${SAVED_WORLDS[$i]}" = "$w" ]; then printf '%s\n' "$i"; return 0; fi
    done
    return 1
}

canon_world() { # 把命令里的世界写法对齐成存档里的写法
    local t="${1:-}" w
    [ -n "$t" ] || return 1
    for w in ${SAVED_WORLDS[@]+"${SAVED_WORLDS[@]}"}; do
        if [ "$t" = "$w" ] || [ "$t" = "${w#*:}" ]; then printf '%s\n' "$w"; return 0; fi
    done
    case "$t" in
        *:*) printf '%s\n' "$t" ;;
        *)   printf 'minecraft:%s\n' "$t" ;;
    esac
}

is_shape_token() {
    case "$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')" in
        circle|diamond|ellipse|hexagon|oval|pentagon|rectangle|square|star|triangle|csv) return 0 ;;
        *) return 1 ;;
    esac
}

num_eq() { # 数值相等（5000 == 5000.0）；任一边不是纯数字则 return 2（无法比较）
    local a="$1" b="$2"
    [[ "$a" =~ ^[+-]?[0-9]*\.?[0-9]+$ && "$b" =~ ^[+-]?[0-9]*\.?[0-9]+$ ]] || return 2
    awk -v a="$a" -v b="$b" 'BEGIN{ exit !(a-b < 1e-9 && b-a < 1e-9) }'
}

cmp_field() { # 命令值 存档值 字段名 -> 不一致时打印 "字段 命令值≠存档值"
    local cv="$1" sv="$2" name="$3" rc
    [ -n "$cv" ] && [ -n "$sv" ] || return 0
    num_eq "$cv" "$sv"; rc=$?
    [ "$rc" = 0 ] && return 0
    [ "$rc" = 2 ] && return 0
    printf '%s %s≠%s\n' "$name" "$cv" "$sv"
}

resume_rewrite_cmds() { # 直接改全局 CMDS，并把结果世界记到 RESUMED_WORLDS
    local -a out=() tok rest
    local cur_world="" cur_shape="" cur_cx="" cur_cz="" cur_rx="" cur_rz=""
    local c first world idx m mismatch cshape ccx ccz crx crz
    for c in "${CMDS[@]}"; do
        read -r -a tok <<<"$c"
        if [ "${tok[0]:-}" != "chunky" ]; then out+=("$c"); continue; fi
        case "${tok[1]:-}" in
            world)  if [ -n "${tok[2]:-}" ]; then cur_world="$(canon_world "${tok[2]}")" || true; fi
                    out+=("$c"); continue ;;
            shape)  if [ -n "${tok[2]:-}" ]; then cur_shape="${tok[2]}"; fi
                    out+=("$c"); continue ;;
            center) if [ -n "${tok[3]:-}" ]; then cur_cx="${tok[2]}"; cur_cz="${tok[3]}"; fi
                    out+=("$c"); continue ;;
            radius) if [ -n "${tok[2]:-}" ]; then cur_rx="${tok[2]}"; cur_rz="${tok[3]:-}"; fi
                    out+=("$c"); continue ;;
            start)  : ;;
            *)      out+=("$c"); continue ;;
        esac

        # ---- 这条是 chunky start：先确定它作用于哪个世界
        first="${tok[2]:-}"; world=""
        if [ -n "$first" ] && ! is_shape_token "$first"; then
            world="$(canon_world "$first")" || world=""
            rest=("${tok[@]:3}")
        else
            rest=("${tok[@]:2}")
        fi
        [ -n "$world" ] || world="$cur_world"
        # 裸 `chunky start`（世界来自上次的 chunky world，脚本看不到）：只有唯一存档任务在
        # 主世界时才敢假定是它
        if [ -z "$world" ] && [ "${#SAVED_WORLDS[@]}" -eq 1 ] \
           && [ "${SAVED_WORLDS[0]}" = "minecraft:overworld" ]; then
            world="minecraft:overworld"
        fi
        if [ -z "$world" ] || ! idx="$(saved_index "$world")"; then
            out+=("$c")
            if [ "$RESUME" = 1 ]; then
                if [ -n "$world" ]; then
                    info "断点继续: $world 没有未完成的存档任务，按原命令 $c 开新任务"
                else
                    warn "断点继续: 这条命令没带世界名（chunky start 用的是当前选择），脚本无法确定它落在哪个世界，"
                    warn "  只能原样透传；若它落在有存档任务的世界，Chunky 会拒绝 start。稳妥写法是显式写世界名，"
                    warn "  例如 chunky start minecraft:overworld circle 0 0 5000"
                fi
            fi
            continue
        fi

        # ---- 参数核对：只核对能明确对上号的字段，对不上就别继续（免得生成到别的范围）
        cshape="$cur_shape"; crx="$cur_rx"; crz="$cur_rz"; ccx=""; ccz=""
        [ -n "$cur_cx" ] && { ccx="$cur_cx"; ccz="$cur_cz"; }
        if [ "${#rest[@]}" -gt 0 ] && is_shape_token "${rest[0]}"; then
            cshape="${rest[0]}"; ccx="${rest[1]:-}"; ccz="${rest[2]:-}"
            crx="${rest[3]:-}"; crz="${rest[4]:-}"
        fi
        mismatch=""
        if [ -n "$cshape" ] && [ -n "${SAVED_SHAPE[$idx]}" ] &&
           [ "$(printf '%s' "$cshape" | tr '[:upper:]' '[:lower:]')" != \
             "$(printf '%s' "${SAVED_SHAPE[$idx]}" | tr '[:upper:]' '[:lower:]')" ]; then
            mismatch="形状 $cshape≠${SAVED_SHAPE[$idx]}"
        else
            m="$(cmp_field "$ccx" "${SAVED_CX[$idx]}" "中心x")"
            if [ -n "$m" ]; then mismatch="$m"; else
                m="$(cmp_field "$ccz" "${SAVED_CZ[$idx]}" "中心z")"
                if [ -n "$m" ]; then mismatch="$m"; else
                    m="$(cmp_field "$crx" "${SAVED_RX[$idx]}" "半径x")"
                    if [ -n "$m" ]; then mismatch="$m"; else
                        # 半径z 也要核：ellipse/rectangle 这类形状 x/z 半径可以不同，
                        # 只核 x 会让 `circle 0 0 1000 5000` 与存档 1000x2000 被判成一致
                        m="$(cmp_field "$crz" "${SAVED_RZ[$idx]}" "半径z")"
                        [ -n "$m" ] && mismatch="$m"
                    fi
                fi
            fi
        fi
        if [ -n "$mismatch" ]; then
            warn "存档任务与命令参数不一致（$world）: $mismatch"
            warn "  存档参数: ${SAVED_DESC[$idx]}"
            warn "  chunky continue 只会按【存档参数】接着跑，而 chunky start 会被 Chunky 拒绝（要你先确认），"
            warn "  两条路都不是你这条命令想要的范围，所以脚本停在这里。"
            warn "  想按新参数生成: rm '${SAVED_FILES[$idx]}' 后重跑（或 --force 重建世界）"
            warn "  想接着上次跑  : 把命令参数改成与上面存档参数一致"
            die "断点继续参数不一致（避免生成到非预期范围）"
        fi
        out+=("chunky continue $world")
        RESUMED_WORLDS+=("$world")
        info "断点继续: chunky start -> 'chunky continue $world'（${SAVED_DESC[$idx]}）"
    done
    CMDS=("${out[@]}")
}

# =============================================================================
# 主流程
# =============================================================================
info "$SCRIPT_NAME v$SCRIPT_VERSION"
info "Minecraft 版本: $MC_VERSION | 种子: $SEED"
info "透传命令 ${#CMDS[@]} 条，共 $N_GROUPS 组（一个 -c 一组；组内按顺序下发）:"
for i in "${!CMDS[@]}"; do
    if [ "$N_GROUPS" -gt 1 ]; then info "  [第 $(( ${CMD_GROUP[$i]} + 1 ))/$N_GROUPS 组] ${CMDS[$i]}"
    else info "  ${CMDS[$i]}"; fi
done
if [ "$N_GROUPS" -gt 1 ] || [ "$BATCH_FLUSH" = 1 ]; then
    info "分块执行: 每组跑完（日志静默 ${IDLE_TIMEOUT}s）→ $( [ "$BATCH_FLUSH" = 1 ] && printf 'save-all flush → ' )等待 ${BATCH_SLEEP}s → 下一组"
fi

# 工作目录
if [ -z "$WORK_DIR" ]; then
    seed_slug="$(printf '%s' "$SEED" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-40)"
    WORK_DIR="$PWD/work/${MC_VERSION}-${seed_slug}"
fi
mkdir -p "$WORK_DIR"
WORK_DIR="$(readlink -f "$WORK_DIR")"
WORLD_DIR="${WORLD_DIR:-$WORK_DIR/world}"
case "$WORLD_DIR" in /*) ;; *) WORLD_DIR="$PWD/$WORLD_DIR" ;; esac
RUN_LOG="$WORK_DIR/run.log"
SERVER_DIR="$WORK_DIR/server"
CACHE_DIR="$WORK_DIR/cache"
JAR_CACHE="$CACHE_DIR/jars"
MOD_LIST_FILE="$WORK_DIR/mods.tsv"
CONSOLE_LOG="$WORK_DIR/console.log"
FIFO="$WORK_DIR/console.fifo"
SUMMARY_JSON="$WORK_DIR/summary.json"
if [ "$DRY_RUN" != 1 ]; then
    # 上一次的日志别删：崩在哪、退化到什么程度，全靠它复盘
    # （服务端的崩溃报告另在 $SERVER_DIR/crash-reports/，脚本的 run.log 是追加的）
    if [ -s "$CONSOLE_LOG" ]; then
        _ts="$(date '+%Y%m%d-%H%M%S')"; _rot="$WORK_DIR/console.$_ts.log"; _n=0
        while [ -e "$_rot" ]; do _n=$((_n + 1)); _rot="$WORK_DIR/console.$_ts-$_n.log"; done
        if mv -f "$CONSOLE_LOG" "$_rot"; then
            info "上一次的 console.log 已保留: $(basename "$_rot")"
        else
            warn "无法轮转旧日志，本次继续覆盖: $CONSOLE_LOG"
        fi
    fi
    unset _ts _rot _n
    : >"$CONSOLE_LOG"
    mkdir -p "$SERVER_DIR" "$JAR_CACHE" "$WORLD_DIR"
fi
info "工作目录: $WORK_DIR"
info "世界输出: $WORLD_DIR"

FREE_BYTES="$(df -PB1 "$WORK_DIR" | awk 'NR==2{print $4}')"
info "可用磁盘: $(( FREE_BYTES / 1024 / 1024 )) MB"
if [ "$DRY_RUN" != 1 ] && [ "$SKIP_DISK_CHECK" != 1 ] && \
   [ "$FREE_BYTES" -lt $(( 5 * 1024 * 1024 * 1024 )) ]; then
    die "可用空间不足 5GB，清理后再试（真要继续用 --skip-disk-check；--force 不再跳过这个检查）"
fi

# ---- 断点继续：扫描 Chunky 存档任务 ----------------------------------------
if [ "$FORCE" = 1 ] && [ -d "$SERVER_DIR/config/chunky/tasks" ]; then
    if [ "$DRY_RUN" = 1 ]; then
        info "--dry-run：--force 会清掉旧的 Chunky 存档任务（$SERVER_DIR/config/chunky/tasks）"
    else
        info "--force：清掉旧的 Chunky 存档任务（世界都重建了，旧进度没有意义）"
        rm -rf "$SERVER_DIR/config/chunky/tasks"
    fi
fi
scan_saved_tasks
if [ "${#SAVED_WORLDS[@]}" -gt 0 ]; then
    if [ "$RESUME" = 1 ]; then
        info "发现 ${#SAVED_WORLDS[@]} 个未完成的 Chunky 存档任务:"
        for i in "${!SAVED_WORLDS[@]}"; do info "    ${SAVED_WORLDS[$i]}  ${SAVED_DESC[$i]}"; done
    else
        warn "发现 ${#SAVED_WORLDS[@]} 个未完成的 Chunky 存档任务（上次没跑完）:"
        for i in "${!SAVED_WORLDS[@]}"; do warn "    ${SAVED_WORLDS[$i]}  ${SAVED_DESC[$i]}"; done
        warn "不加 --resume 时，这些世界的 'chunky start' 会被 Chunky 拦下来（只打印 A task was already"
        warn "started for this world ...），本次不会真正开始生成。要接着上次的进度跑，请加 --resume。"
        warn "（若这些世界其实已经重新来过，请 rm -rf '$SERVER_DIR/config/chunky/tasks' 后重跑）"
    fi
fi
if [ "$RESUME" = 1 ]; then
    resume_rewrite_cmds
    # resume_rewrite_cmds 只做「一条换一条」的改写，分组信息不会被破坏；这里只是兜底断言
    [ "${#CMDS[@]}" -eq "${#CMD_GROUP[@]}" ] || \
        die "内部错误：断点继续改写了命令条数（${#CMDS[@]} 条 vs 分组表 ${#CMD_GROUP[@]} 条）"
    info "断点继续后实际下发的命令 ${#CMDS[@]} 条（${N_GROUPS} 组）:"
    for i in "${!CMDS[@]}"; do
        if [ "$N_GROUPS" -gt 1 ]; then info "  [第 $(( ${CMD_GROUP[$i]} + 1 ))/$N_GROUPS 组] ${CMDS[$i]}"
        else info "  ${CMDS[$i]}"; fi
    done
fi

# ---- 版本与 loader ---------------------------------------------------------
info "校验游戏版本与 Fabric loader..."
LOADER_JSON="$(http_json "$FABRIC_META/versions/loader/$MC_VERSION")" || die "无法从 Fabric Meta 获取 $MC_VERSION 的 loader 列表"
if [ -z "$LOADER_VER" ]; then
    LOADER_VER="$(jq -r '[.[] | select(.loader.stable == true)][0].loader.version // empty' <<<"$LOADER_JSON")"
    [ -n "$LOADER_VER" ] || LOADER_VER="$(jq -r '.[0].loader.version // empty' <<<"$LOADER_JSON")"
fi
[ -n "$LOADER_VER" ] || die "无法确定 Fabric loader 版本（$MC_VERSION 可能不受 Fabric 支持）"
INSTALLER_VER="$(jq -r '[.[] | select(.stable == true)][0].version // empty' <<<"$(http_json "$FABRIC_META/versions/installer")")"
[ -n "$INSTALLER_VER" ] || die "无法获取 Fabric installer 版本"
FABRIC_SERVER_URL="$FABRIC_META/versions/loader/$MC_VERSION/$LOADER_VER/$INSTALLER_VER/server/jar"
FABRIC_SERVER_JAR="fabric-server-mc.$MC_VERSION-loader.$LOADER_VER-launcher.$INSTALLER_VER.jar"
info "Fabric loader: $LOADER_VER | installer: $INSTALLER_VER"

# ---- 模组解析 --------------------------------------------------------------
info "从 Modrinth 解析 Chunky / C2ME ..."
C2ME_STATUS="未启用"
resolve_mods
MOD_ROWS="$(cat "$MOD_LIST_FILE")"
while IFS=$'\t' read -r proj vnum fname url sha1 sha512 size; do
    [ -n "${proj:-}" ] || continue
    info "  模组: $proj $vnum -> $fname ($(( size / 1024 )) KB)"
done <<<"$MOD_ROWS"

# ---- tick 命令 -------------------------------------------------------------
TICK_SKIP_REASON=""
if [ -n "$TICK_CMD" ] && ! mc_ge "$MC_VERSION" "1.20.3"; then
    TICK_SKIP_REASON="$MC_VERSION 无 /tick 命令"
    TICK_CMD=""
fi
if [ -n "$TICK_CMD" ]; then
    info "tick 命令: $TICK_CMD"
else
    info "tick 命令: 跳过（${TICK_SKIP_REASON:-已用 --no-tick 关闭}）"
fi

# ---- Java ------------------------------------------------------------------
if [ "$DRY_RUN" != 1 ]; then
    JAVA_SEL="$(pick_java)"
else
    JAVA_SEL="$(pick_java 2>/dev/null || echo '（未找到，实际运行时才会报错）')"
fi

# ---- JVM 参数（手动传入；默认一个都不加）-----------------------------------
# 默认什么都不加，用 JVM 自己的默认值（最大堆 = 物理内存的 1/4）：
#   要固定堆      -> --memory 8G（等价于加 -Xmx8G）
#   要别的参数    -> --jvm-args '-Xms8G -XX:+UseZGC'（可重复，原样拼在 java 与 -jar 之间）
# 脚本曾经内置过一整套 Aikar 性能参数，现已去掉：调参交给使用者。
# 顺带记一笔：新工作目录首次启动会慢到几分钟才 Done，那是 Fabric 在 java 进程里
# 下载/解包 vanilla server.jar 与全部依赖库（实测 217s，真正 Done 只用了 2.25s），
# 跟 JVM 参数无关；--start-timeout 默认 900 就是给这种情况留的。
JVM_ARGS=""
if [ -n "$MEMORY" ]; then
    JVM_ARGS="-Xmx$MEMORY"
fi
for _a in ${JVM_ARGS_EXTRA[@]+"${JVM_ARGS_EXTRA[@]}"}; do
    JVM_ARGS="${JVM_ARGS:+$JVM_ARGS }$_a"
done
unset _a
# 默认情况下，命令执行抛异常只会打印一句 "An unexpected error occurred trying to
# execute that command"，真实异常信息藏在悬停提示里、堆栈只在 debug 级别才记录。
if [ "$DEBUG_COMMANDS" = 1 ]; then
    JVM_ARGS="${JVM_ARGS:+$JVM_ARGS }-DMC_DEBUG_ENABLED=true -DMC_DEBUG_VERBOSE_COMMAND_ERRORS=true"
    info "--debug-commands 已开启：命令异常会带真实原因与堆栈写进 console.log"
fi
if [ -n "$JVM_ARGS" ]; then
    info "JVM 参数（手动传入）: $JVM_ARGS"
else
    info "JVM 参数: 未指定，使用 JVM 默认值（堆上限默认 = 物理内存的 1/4）"
fi

# ---- dry-run ---------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
    cat >&2 <<EOF

================= DRY-RUN 计划 =================
工作目录      : $WORK_DIR
世界输出      : $WORLD_DIR
MC 版本       : $MC_VERSION
Fabric loader : $LOADER_VER (installer $INSTALLER_VER)
服务端 jar    : $FABRIC_SERVER_JAR
                $FABRIC_SERVER_URL
Java          : $JAVA_SEL
Java 参数     : ${JVM_ARGS:-（未指定，用 JVM 默认值；堆上限默认 = 物理内存的 1/4）}
C2ME          : $C2ME_STATUS
level-type    : $LEVEL_TYPE (structures=$GEN_STRUCTURES)
断点继续      : $(if [ "$RESUME" != 1 ]; then echo '关闭（未加 --resume）'
                   elif [ "${#RESUMED_WORLDS[@]}" -gt 0 ]; then printf '已继续 %s' "${RESUMED_WORLDS[*]}"
                   elif [ "${#SAVED_WORLDS[@]}" -gt 0 ]; then echo "开启，但没有命令命中这 ${#SAVED_WORLDS[@]} 个存档任务"
                   else echo '开启，但没有未完成的存档任务（等同全新生成）'; fi)
tick 命令     : $( [ -n "$TICK_CMD" ] && echo "$TICK_CMD（就绪后只下发一次，不属于任何一批）" || echo '（不发）' )
命令分组      : 共 $N_GROUPS 组（一个 -c 一组；组内多条命令按书写顺序下发）
$(for g in $(seq 0 $((N_GROUPS - 1))); do
    printf '    [第 %s/%s 组]\n' "$(( g + 1 ))" "$N_GROUPS"
    group_cmds "$g" | sed 's/^/        /'
  done)
每批结束      : $(if [ "$BATCH_FLUSH" = 1 ]; then echo -n 'save-all flush + '; else echo -n '（--no-batch-flush：不 flush）+ '; fi)等待 ${BATCH_SLEEP}s
结束判定      : 每批日志静默 ${IDLE_TIMEOUT}s 判定本批跑完 → 下一批；最后一批之后同样处理，再发送 stop
停服等待      : stop 后最多等 ${STOP_TIMEOUT}s；仍不退出才 SIGTERM，再等 ${STOP_KILL_TIMEOUT}s 才 kill -9
日志轮转      : 启动时把上一次的 console.log 另存为 console.<时间戳>.log（不删旧证据）

下载清单:
$(while IFS=$'\t' read -r proj vnum fname url sha1 sha512 size; do
    [ -n "${proj:-}" ] || continue
    printf '    [mod] %-14s %-28s %8s KB  %s\n' "$proj" "$vnum" "$(( size / 1024 ))" "$fname"
  done <<<"$MOD_ROWS")
    [server] $FABRIC_SERVER_JAR（首次启动自动拉 vanilla server.jar 与依赖库）

模组包（-m）: ${MODS_ZIPS[*]:-（无，只有 chunky+c2me+fabric-api）}
==============================================
EOF
    exit 0
fi

# ---- 下载与组装服务端 ------------------------------------------------------
fetch_file "$FABRIC_SERVER_URL" "$CACHE_DIR/$FABRIC_SERVER_JAR" || die "Fabric 服务端下载失败"
SERVER_JAR="$SERVER_DIR/$FABRIC_SERVER_JAR"
cp -f "$CACHE_DIR/$FABRIC_SERVER_JAR" "$SERVER_JAR" || die "复制服务端 jar 失败"

info "准备 mods/ 目录..."
rm -rf "$SERVER_DIR/mods"; mkdir -p "$SERVER_DIR/mods"
while IFS=$'\t' read -r proj vnum fname url sha1 sha512 size; do
    [ -n "${proj:-}" ] || continue
    fetch_file "$url" "$JAR_CACHE/$fname" "$sha1" "$sha512" || die "模组下载/校验失败: $proj ($fname)"
    cp -f "$JAR_CACHE/$fname" "$SERVER_DIR/mods/" || die "复制模组失败: $fname"
done <<<"$MOD_ROWS"

for z in ${MODS_ZIPS[@]+"${MODS_ZIPS[@]}"}; do
    [ -e "$z" ] || die "模组路径不存在: $z"
    if [ -d "$z" ]; then
        cnt=0
        while IFS= read -r -d '' j; do cp -f "$j" "$SERVER_DIR/mods/" && cnt=$((cnt+1)); done \
            < <(find "$z" -type f -name '*.jar' -print0)
        info "从目录 $z 复制 $cnt 个 jar"
    else
        cnt=0
        if unzip -o -j "$z" '*.jar' -d "$SERVER_DIR/mods/" >/dev/null 2>&1; then
            cnt="$(unzip -Z1 "$z" '*.jar' 2>/dev/null | wc -l)"
        else
            warn "zip 中没有 .jar 条目: $z"
        fi
        info "从 $z 解压 $cnt 个 jar"
    fi
done
info "mods/ 内共 $(find "$SERVER_DIR/mods" -maxdepth 1 -name '*.jar' | wc -l) 个 jar"

# ---- 世界与 server.properties ---------------------------------------------
if [ -e "$WORLD_DIR/level.dat" ]; then
    if [ "$FORCE" = 1 ]; then
        warn "--force：删除已存在的世界 $WORLD_DIR"
        rm -rf "$WORLD_DIR"
    elif [ "$REUSE" = 1 ]; then
        if [ "$RESUME" = 1 ]; then info "--resume：复用已存在的世界（断点继续）"
        else info "--reuse：复用已存在的世界"; fi
    else
        die "世界已存在: $WORLD_DIR（用 --reuse 继续，或 --force 删除重建）"
    fi
fi
mkdir -p "$WORLD_DIR"

cat >"$SERVER_DIR/server.properties" <<EOF
level-name=$WORLD_DIR
level-seed=$SEED
level-type=$LEVEL_TYPE
generator-settings=$GENERATOR_SETTINGS
gamemode=creative
difficulty=peaceful
generate-structures=$([ "$GEN_STRUCTURES" = 1 ] && echo true || echo false)
online-mode=false
enforce-secure-profile=false
white-list=false
max-players=1
motd=chunky-pregeneration
server-port=25565
enable-status=false
enable-query=false
enable-rcon=false
spawn-protection=0
view-distance=3
simulation-distance=3
max-tick-time=-1
sync-chunk-writes=false
pause-when-empty-seconds=0
network-compression-threshold=-1
log-ips=false
EOF
for kv in ${SERVER_PROPS[@]+"${SERVER_PROPS[@]}"}; do
    key="${kv%%=*}"
    [[ "$key" =~ ^[A-Za-z0-9._-]+$ ]] || die "--server-property 键名非法: $kv"
    if [ "$key" = "max-tick-time" ] && [ "${kv#*=}" -gt 0 ] 2>/dev/null; then
        warn "--server-property max-tick-time=${kv#*=} 会打开服务端的 tick 看门狗："
        warn "  生成大图时一次超过该值的慢 tick（比如超大世界写盘）会被服务端自己强制退出，"
        warn "  脚本默认写 max-tick-time=-1 就是为了关掉它；除非你明确要这个行为，否则删掉这条"
    fi
    if grep -qE "^${key}=" "$SERVER_DIR/server.properties"; then
        sed -i -E "s|^${key}=.*|${kv}|" "$SERVER_DIR/server.properties"
    else
        printf '%s\n' "$kv" >>"$SERVER_DIR/server.properties"
    fi
done
printf 'eula=true\n' >"$SERVER_DIR/eula.txt"
info "已写入 server.properties 与 eula.txt=true（脚本按约定自动接受 https://aka.ms/MinecraftEULA）"

# ---- 启动服务端 -------------------------------------------------------------
[ -p "$FIFO" ] || mkfifo "$FIFO"
exec 3<>"$FIFO"
T0="$(date +%s)"
start_server "$JAVA_SEL"

# ---- 等待服务端启动 --------------------------------------------------------
# 为什么要等 "Done (" 才写命令（而不是 java 一启动就写）：
#   服务端控制台线程是「读到那一行」时就把 CommandSourceStack 建好的，而这份
#   source 里的 ServerLevel 要等世界加载完才存在。MC 26.1+ 里所有命令都会去取
#   getLevel().getGameRules()，因此世界加载完成前写入的命令 100% 失败，日志表现为
#       An unexpected error occurred trying to execute that command
#   加 -DMC_DEBUG_ENABLED=true -DMC_DEBUG_VERBOSE_COMMAND_ERRORS=true 才能看到真正原因：
#       NullPointerException: ... because the return value of
#       "net.minecraft.commands.CommandSourceStack.getLevel()" is null
# "Done (" 之后写入，命令会在下一个 tick 被主线程执行，与「启动瞬间写入」相比不快也不慢。
waited=0
started=0
while [ "$waited" -lt "$START_TIMEOUT" ]; do
    if server_alive; then
        if grep -qF 'Done (' <(log_since "$SERVER_LOG_OFFSET"); then
            ok "服务端已就绪（${waited}s）"
            started=1
            break
        fi
        if check_crash "$SERVER_LOG_OFFSET"; then
            tail_log 60; die "服务端启动失败（崩溃）"
        fi
    else
        wait "$SERVER_PID" 2>/dev/null || true
        tail_log 60; die "服务端进程提前退出（启动失败）"
    fi
    sleep 1; waited=$((waited + 1))
done
if [ "$started" != 1 ]; then tail_log 60; die "等待服务端启动超时（${START_TIMEOUT}s）"; fi

# ---- 分块执行：一组命令 -> 等日志静默 -> save-all flush -> 等待 -> 下一组 ------
# * 一个 -c 是一组（组内多条命令按书写顺序下发），多个 -c 从前到后一组一组跑
# * 每组的结束判据与脚本收尾判据完全一致：日志静默 IDLE_TIMEOUT 秒 —— Chunky 生成期间
#   每秒都在刷进度行，静默下来就说明这一组没有任务在跑了
# * 每组收尾动作：save-all flush（把这一批的脏区块全部写盘）+ 等 BATCH_SLEEP 秒；
#   最后一组之后同样做一次，然后才 stop（flush/等待都发生在服务端还活着的时候）
# * 若自动保存/卡顿让日志 >IDLE_TIMEOUT 秒没刷新，本组会被提前判结束、下一组命令提前下发；
#   已经下发的任务不会被取消，只是分块边界变模糊（把 --idle-timeout 调大即可避免）
CHUNKY_OFFSET="$SERVER_LOG_OFFSET"
GROUP_READ_OFF="$SERVER_LOG_OFFSET"   # 增量读日志的游标（只前进，不重复统计）
N_SENT=0
chunky_seen=0
cmd_fail_warned=0
G_CRASH=0; G_CRASH_PAT=""
G_CHUNK=""
G_START_WORLDS=(); G_DONE_WORLDS=()
G_IDLE=0; G_SECS=0; G_START_N=0; G_DONE_N=0; G_MISSING_WORLDS=""
GS_IDLE=(); GS_SECS=(); GS_START_N=(); GS_DONE_N=(); GS_MISSING=()

read_log_chunk() { # 增量读日志：G_CHUNK = [GROUP_READ_OFF, 文件尾)，游标推到文件尾
    local sz avail lastbyte
    sz="$(stat -c%s "$CONSOLE_LOG" 2>/dev/null || echo 0)"
    avail=$(( sz - GROUP_READ_OFF ))
    if [ "$avail" -le 0 ]; then G_CHUNK=""; return 0; fi
    # 只在「文件正好停在行尾」时读取：万一读到一行写到一半（log4j 分多次 write），
    # Task started/finished 这种整行匹配就会漏掉一条（只影响统计，会多一条"未结束"误报）。
    # 读不到就等下一秒（静默判定用的是文件大小，不受这里影响）。
    lastbyte="$(tail -c 1 "$CONSOLE_LOG" 2>/dev/null | od -An -tuC | tr -d ' \n')"
    if [ "$lastbyte" != "10" ]; then G_CHUNK=""; return 0; fi
    G_CHUNK="$(tail -c +"$(( GROUP_READ_OFF + 1 ))" "$CONSOLE_LOG" 2>/dev/null | head -c "$avail" || true)"
    GROUP_READ_OFF=$(( GROUP_READ_OFF + avail ))
    return 0
}

process_new_log() { # 处理新增日志：Chunky 活动、任务开始/结束、命令被拒、崩溃
    local chunk="$1" w p
    [ -n "$chunk" ] || return 0
    if [ "$chunky_seen" = 0 ] && grep -qF '[Chunky]' <<<"$chunk"; then chunky_seen=1; fi
    if [ "$cmd_fail_warned" = 0 ] &&
       grep -qF 'An unexpected error occurred trying to execute that command' <<<"$chunk"; then
        warn "服务端拒绝了至少一条透传命令（日志里的 'An unexpected error occurred...'）；"
        warn "真实原因默认不打印，加 --debug-commands 重跑即可在 console.log 看到异常与堆栈"
        cmd_fail_warned=1
    fi
    if grep -qF 'A task was already started for this world' <<<"$chunk"; then
        warn "Chunky 拒绝了一条 start（那个世界已经有任务在跑，或有未完成的存档任务）：加 --resume 可以接着跑"
    fi
    # [Chunky] Task started in <世界> ...  /  Task finished|stopped for <世界>.
    while IFS= read -r w; do
        if [ -n "$w" ]; then G_START_WORLDS+=("$w"); fi
    done < <(grep -aoE 'Task started in [^ ]+' <<<"$chunk" | awk '{print $4}')
    while IFS= read -r w; do
        if [ -n "$w" ]; then G_DONE_WORLDS+=("$w"); fi
    done < <(grep -aoE 'Task (finished|stopped) for [A-Za-z0-9_.:+-]+' <<<"$chunk" | awk '{print $4}' | sed 's/\.$//')
    for p in "${CRASH_PATTERNS[@]}"; do
        if grep -qF -- "$p" <<<"$chunk"; then
            G_CRASH=1; G_CRASH_PAT="$p"; warn "检测到服务端致命错误: $p"; break
        fi
    done
    return 0
}

wait_group_silence() { # $1=组号(1 起)：等到本组「日志静默 IDLE_TIMEOUT 秒」
    local g="$1" cur_size last_size idle heartbeat t_start w i
    local -a missing=()
    # 注意：G_START_WORLDS / G_DONE_WORLDS 由 run_group 在「下发本组命令之前」清空，
    # 这里不能清 —— 本组命令的 Task started 行可能在下发后、进入这个循环之前就被读走了
    t_start="$(date +%s)"
    last_size="$(stat -c%s "$CONSOLE_LOG" 2>/dev/null || echo 0)"
    idle=0; heartbeat=0
    while server_alive; do
        read_log_chunk; process_new_log "$G_CHUNK"
        if [ "$G_CRASH" = 1 ]; then break; fi
        cur_size="$(stat -c%s "$CONSOLE_LOG" 2>/dev/null || echo 0)"
        if [ "$cur_size" != "$last_size" ]; then
            last_size="$cur_size"; idle=0; heartbeat=$((heartbeat + 1))
        else
            idle=$((idle + 1))
        fi
        if [ "$idle" -ge "$IDLE_TIMEOUT" ]; then break; fi
        # 静默倒计时每 2s 显示一次；生成中每 30s 一个心跳
        if [ "$idle" -gt 0 ] && [ $(( idle % 2 )) -eq 0 ]; then
            printf '\r%s[等待]%s 第 %s/%s 组：日志已静默 %ss / %ss   ' \
                "$C_INFO" "$C_OFF" "$g" "$N_GROUPS" "$idle" "$IDLE_TIMEOUT" >&2
        elif [ "$idle" -eq 0 ] && [ $(( heartbeat % 30 )) -eq 1 ]; then
            printf '\r%s[生成中]%s 第 %s/%s 组：已运行 %ss，日志持续刷新   ' \
                "$C_INFO" "$C_OFF" "$g" "$N_GROUPS" "$(( $(date +%s) - T0 ))" >&2
        fi
        sleep 1
    done
    printf '\n' >&2
    # 收尾再读一次：把「刚好在静默判定那一刻写出」的 Task finished / 崩溃信息也收进来
    if [ "$G_CRASH" = 0 ]; then
        read_log_chunk; process_new_log "$G_CHUNK"
    fi
    G_IDLE="$idle"
    G_SECS=$(( $(date +%s) - t_start ))
    G_START_N="${#G_START_WORLDS[@]}"
    G_DONE_N="${#G_DONE_WORLDS[@]}"
    # 本组「启动了但没看到结束」的任务（同名任务重复时按多重集合扣减）
    missing=(${G_START_WORLDS[@]+"${G_START_WORLDS[@]}"})
    for w in ${G_DONE_WORLDS[@]+"${G_DONE_WORLDS[@]}"}; do
        for i in "${!missing[@]}"; do
            if [ "${missing[$i]}" = "$w" ]; then unset 'missing[i]'; break; fi
        done
    done
    G_MISSING_WORLDS=""
    for i in "${!missing[@]}"; do
        G_MISSING_WORLDS="${G_MISSING_WORLDS:+$G_MISSING_WORLDS }${missing[$i]}"
    done
    return 0
}

run_group() { # $1=组号索引(0 起)：下发本组命令 -> 等静默 -> 收尾统计 -> save-all flush + 等待
    local g="$1" gname="$(( $1 + 1 ))" c i
    local -a gcmds=()
    while IFS= read -r c; do gcmds+=("$c"); done < <(group_cmds "$g")
    # 本组任务的统计从「下发之前」开始算（Task started 行可能在下发后立刻出现）
    G_START_WORLDS=(); G_DONE_WORLDS=()
    info "第 ${gname}/${N_GROUPS} 组：下发 ${#gcmds[@]} 条命令"
    for c in "${gcmds[@]}"; do
        send_cmd "$c" || true
        N_SENT=$((N_SENT + 1))
        info "  → $c"
    done

    # 本组含生成类命令、且还没见过 Chunky 输出时，先确认模组真的在工作
    if [ "$chunky_seen" = 0 ] && group_has_task_cmd "$g"; then
        local deadline=$(( $(date +%s) + START_TIMEOUT ))
        while [ "$(date +%s)" -lt "$deadline" ]; do
            if [ "$chunky_seen" = 1 ]; then break; fi
            read_log_chunk; process_new_log "$G_CHUNK"
            if [ "$G_CRASH" = 1 ]; then
                tail_log 60; die "服务端在等待 Chunky 输出期间崩溃（检测到: $G_CRASH_PAT）"
            fi
            server_alive || { tail_log 60; die "服务端在等待 Chunky 输出期间退出"; }
            sleep 1
        done
        if [ "$chunky_seen" = 0 ]; then
            tail_log 40
            die "未检测到任何 Chunky 输出（命令可能拼错，或模组未加载）"
        fi
    fi

    wait_group_silence "$gname"
    if [ "$G_CRASH" = 1 ]; then
        tail_log 60
        die "服务端在第 ${gname}/${N_GROUPS} 组生成期间崩溃（检测到: $G_CRASH_PAT）；已落盘的区块还在，下次加 --resume 接着跑"
    fi
    if ! server_alive; then
        wait "$SERVER_PID" 2>/dev/null || true
        tail_log 40
        die "服务端在第 ${gname}/${N_GROUPS} 组生成期间退出（未收到 stop）；已落盘的区块还在，下次加 --resume 接着跑"
    fi
    ok "第 ${gname}/${N_GROUPS} 组结束（日志静默 ${G_IDLE}s，本组耗时 ${G_SECS}s）：启动 ${G_START_N} 个任务，结束 ${G_DONE_N} 个"
    if [ -n "$G_MISSING_WORLDS" ]; then
        warn "第 ${gname}/${N_GROUPS} 组静默时仍有任务没看到结束: $G_MISSING_WORLDS"
        warn "  可能是被打断/卡住，也可能只是日志卡顿 >${IDLE_TIMEOUT}s 被提前判定；"
        warn "  进度由 Chunky 在服务端停止时写盘，下次加 --resume 就能从这次的断点接着跑"
    fi
    GS_IDLE+=("$G_IDLE"); GS_SECS+=("$G_SECS")
    GS_START_N+=("$G_START_N"); GS_DONE_N+=("$G_DONE_N"); GS_MISSING+=("$G_MISSING_WORLDS")
    # ---- 每批收尾动作（最后一组之后同样执行，然后才 stop） ----
    if [ "$BATCH_FLUSH" = 1 ]; then
        info "第 ${gname}/${N_GROUPS} 组：save-all flush（把这一批生成的区块全部写盘）"
        send_cmd "save-all flush" || true
    fi
    if [ "$BATCH_SLEEP" -gt 0 ]; then
        info "第 ${gname}/${N_GROUPS} 组：等待 ${BATCH_SLEEP}s 再继续（--batch-sleep 可调）"
        for i in $(seq 1 "$BATCH_SLEEP"); do
            printf '\r%s[批间隔]%s 已等待 %ss / %ss   ' "$C_INFO" "$C_OFF" "$i" "$BATCH_SLEEP" >&2
            sleep 1
            server_alive || break
        done
        printf '\n' >&2
    fi
    if ! server_alive; then
        read_log_chunk; process_new_log "$G_CHUNK"
        wait "$SERVER_PID" 2>/dev/null || true
        tail_log 40
        if [ "$G_CRASH" = 1 ]; then
            die "服务端在第 ${gname}/${N_GROUPS} 组的 save-all flush / 等待期间崩溃（检测到: $G_CRASH_PAT）；已落盘的区块还在，下次加 --resume 接着跑"
        fi
        die "服务端在第 ${gname}/${N_GROUPS} 组的 save-all flush / 等待期间退出；已落盘的区块还在，下次加 --resume 接着跑"
    fi
    return 0
}

# 就绪后：tick 命令只发一次，然后一组一组下发
if [ -n "$TICK_CMD" ]; then
    send_cmd "$TICK_CMD" || true
    N_SENT=$((N_SENT + 1))
    info "已下发 tick 命令: $TICK_CMD"
fi
for g in $(seq 0 $((N_GROUPS - 1))); do
    run_group "$g"
done
ok "全部 ${N_GROUPS} 组执行完毕（共下发 $N_SENT 条命令，总耗时 $(( $(date +%s) - T0 ))s）"

# ---- 收尾 ------------------------------------------------------------------
stop_server
ok "服务端已退出"

ELAPSED=$(( $(date +%s) - T0 ))
WORLD_SIZE="$(du -sh "$WORLD_DIR" 2>/dev/null | cut -f1 || echo '?')"
REGION_STATS="$(python3 - "$WORLD_DIR" <<'PY' 2>/dev/null || echo '{}'
import glob, json, os, sys
w = sys.argv[1]
out = {}
for p in glob.glob(os.path.join(w, '**', 'region', 'r.*.mca'), recursive=True):
    try:
        h = open(p, 'rb').read(4096)
    except OSError:
        continue
    if len(h) < 4096:
        continue
    n = sum(1 for i in range(1024) if h[i*4:i*4+3] != b'\x00\x00\x00')
    rel = os.path.relpath(os.path.dirname(p), w)
    out[rel] = out.get(rel, 0) + n
print(json.dumps(out, ensure_ascii=False))
PY
)"

info "各 region 目录已落盘区块数（仅统计，不做判定）:"
jq -r 'to_entries[] | "    \(.key): \(.value) 区块"' <<<"$REGION_STATS" >&2 2>/dev/null || true

MODS_JSON="$(while IFS=$'\t' read -r proj vnum fname url sha1 sha512 size; do
    [ -n "${proj:-}" ] || continue
    jq -nc --arg p "$proj" --arg v "$vnum" --arg f "$fname" --arg s "$sha1" \
        '{project:$p, version:$v, file:$f, sha1:$s}'
done <<<"$MOD_ROWS" | jq -sc '.')"
CMDS_JSON="$(printf '%s\n' "${CMDS[@]}" | jq -Rsc 'split("\n") | map(select(length>0))')"
GROUPS_JSON="$(for g in $(seq 0 $((N_GROUPS - 1))); do
    group_cmds "$g" | jq -Rsc \
        --argjson idx "$(( g + 1 ))" \
        --argjson idle "${GS_IDLE[$g]:-0}" --argjson secs "${GS_SECS[$g]:-0}" \
        --argjson started "${GS_START_N[$g]:-0}" --argjson done "${GS_DONE_N[$g]:-0}" \
        --arg missing "${GS_MISSING[$g]:-}" \
        '{index:$idx, commands:(split("\n")|map(select(length>0))), idle_seconds:$idle, seconds:$secs,
          tasks_started:$started, tasks_finished:$done,
          unfinished:($missing | if . == "" then [] else split(" ") end)}'
done | jq -sc '.')"
RESUMED_JSON="$(if [ "${#RESUMED_WORLDS[@]}" -eq 0 ]; then printf '[]\n'; else
    printf '%s\n' "${RESUMED_WORLDS[@]}" | jq -Rsc 'split("\n") | map(select(length>0))'; fi)"

jq -n --arg script "$SCRIPT_NAME v$SCRIPT_VERSION" \
    --arg mc "$MC_VERSION" --arg loader "$LOADER_VER" --arg installer "$INSTALLER_VER" \
    --arg java "$JAVA_SEL" --arg jvm_args "$JVM_ARGS" --arg seed "$SEED" \
    --arg world "$WORLD_DIR" --arg workdir "$WORK_DIR" \
    --arg tick_cmd "$TICK_CMD" --argjson cmds "$CMDS_JSON" \
    --arg level_type "$LEVEL_TYPE" --argjson structures "$GEN_STRUCTURES" \
    --arg c2me "$C2ME_STATUS" --argjson mods "$MODS_JSON" \
    --argjson regions "$REGION_STATS" --arg world_size "$WORLD_SIZE" \
    --argjson elapsed "$ELAPSED" \
    --argjson resume "$RESUME" --argjson resumed "$RESUMED_JSON" \
    --argjson groups "$GROUPS_JSON" \
    --argjson batch_sleep "$BATCH_SLEEP" --argjson batch_flush "$BATCH_FLUSH" \
    '{script:$script, minecraft:$mc, fabric_loader:$loader, fabric_installer:$installer,
      java:$java, jvm_args:$jvm_args, requested_seed:$seed, world_dir:$world, work_dir:$workdir,
      level_type:$level_type, generate_structures:($structures==1), c2me:$c2me,
      tick_command:$tick_cmd, passthrough_commands:$cmds, mods:$mods,
      batch:{save_all_flush:($batch_flush==1), sleep_seconds:$batch_sleep}, command_groups:$groups,
      resume:($resume==1), resumed_worlds:$resumed,
      chunks_on_disk:$regions, world_size:$world_size, elapsed_seconds:$elapsed}' \
    >"$SUMMARY_JSON"

cat >&2 <<EOF

==================== 生成完成 ====================
世界目录      : $WORLD_DIR（$WORLD_SIZE）
工作目录      : $WORK_DIR
MC / Fabric   : $MC_VERSION / loader $LOADER_VER
Java          : $JAVA_SEL
Java 参数     : ${JVM_ARGS:-（未指定，用 JVM 默认值）}
种子          : $SEED
C2ME          : $C2ME_STATUS
分块          : $N_GROUPS 组；每批结束 $( [ "$BATCH_FLUSH" = 1 ] && printf 'save-all flush + ' || printf '（未 flush）+ ' )等待 ${BATCH_SLEEP}s
$(for g in $(seq 0 $((N_GROUPS - 1))); do
    printf '  第 %s/%s 组  : 静默 %ss / 本组 %ss / 启动 %s 个任务 / 结束 %s 个%s\n' \
      "$(( g + 1 ))" "$N_GROUPS" "${GS_IDLE[$g]:-0}" "${GS_SECS[$g]:-0}" \
      "${GS_START_N[$g]:-0}" "${GS_DONE_N[$g]:-0}" \
      "$( [ -n "${GS_MISSING[$g]:-}" ] && printf '（未结束: %s）' "${GS_MISSING[$g]}" )"
  done)
下发的命令    : $( [ -n "$TICK_CMD" ] && printf '%s; ' "$TICK_CMD" )$(printf '%s; ' "${CMDS[@]}")
断点继续      : $(if [ "${#RESUMED_WORLDS[@]}" -gt 0 ]; then printf '%s; ' "${RESUMED_WORLDS[@]}"; else echo '无'; fi)
耗时          : ${ELAPSED}s
汇总          : $SUMMARY_JSON
控制台日志    : $CONSOLE_LOG
==================================================
EOF
ok "全部完成：$WORLD_DIR"
