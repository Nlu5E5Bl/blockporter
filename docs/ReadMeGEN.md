# gen_raw_map.sh —— Fabric + Chunky(+C2ME) 纯净地图生成（命令透传版）

用真实 Fabric 服务端跑 Chunky 预生成地图。**脚本只做四件事**：

1. 备好 Fabric 服务端（自动下载 loader / 服务端 jar / Chunky / C2ME / 你的模组包）；
2. 把你的命令**原样透传**——服务端打印 `Done (` 后立刻写进控制台输入缓冲区，不等回显、不等执行结果；
3. **分块执行**：一个 `-c` 是一批（一批里还能用逗号写多条命令），多个 `-c` 从前到后一批一批跑；
   每批跑完先 `save-all flush` 并等待 `--batch-sleep` 秒（默认 30），再下发下一批；
4. 让 Chunky 一直刷屏当存活信号，**每批日志静默 `--idle-timeout` 秒**（默认 10）就算这批跑完；
   所有批次结束后再发 `stop` 收工。

脚本**不解析地图范围、不校验参数、不检查落盘率**：Chunky 的行为（含它的特性与 bug）由使用者负责。

> 踩过的坑集中在 [已知坑（实测汇总）](#已知坑实测汇总)：命令写入时机、`--debug-commands`、
> 冻结与 `stop` 的关系、地图交接给真服务端、世界时间等。第一次用建议先扫一遍。
> 跑到一半想接着跑（断点继续）见 [断点继续（--resume）](#断点继续--resume)；
> 想分批跑大图见 [分块执行（分批与逗号）](#分块执行分批与逗号)。

## 系统要求

| 项 | 要求 |
|---|---|
| 系统 | Linux，bash ≥ 4.4 |
| 命令 | `curl` `jq` `sha1sum` `unzip`（`-m` 用） |
| Java | 按 MC 版本自动挑选；26.x（如 26.1.1）需 25，1.20.5–1.21.x 需 21+，1.18–1.20.4 需 17，1.17 需 16/17 |
| 网络 | 需访问 `api.modrinth.com`、`meta.fabricmc.net`、Mojang 资源站 |

## 快速开始

```bash
# 主世界：以 (0,0) 为中心、半径 2000 方块生成
./gen_raw_map.sh -s 12345 -v 1.21.1 -c 'chunky start minecraft:overworld circle 0 0 2000'

# 三个维度一次跑（chunky 会按顺序执行）
./gen_raw_map.sh -s 12345 -v 1.21.1 -m ./mods.zip \
    -c 'chunky start minecraft:overworld circle 0 0 5000' \
    -c 'chunky start minecraft:the_nether circle 0 0 5000' \
    -c 'chunky start minecraft:the_end  circle 0 0 5000'

# 三个维度分批跑：一批一个维度，每批跑完 save-all flush + 等 60s 再下一批
./gen_raw_map.sh -s 12345 -v 1.21.1 -m ./mods.zip --idle-timeout 60 --batch-sleep 60 \
    -c 'chunky start minecraft:overworld circle 0 0 5000' \
    -c 'chunky start minecraft:the_nether circle 0 0 5000' \
    -c 'chunky start minecraft:the_end  circle 0 0 5000'

# 一条 -c 里用逗号写多条命令 = 同一批（这两条 start 会同时开始生成）
./gen_raw_map.sh -s 12345 -v 1.21.1 \
    -c 'chunky start minecraft:overworld circle 0 0 1000, chunky start minecraft:the_nether circle 0 0 1000'

# 模组维度 / 用 chunky 自己的分步命令（同样只是透传，顺序即执行顺序；同一批里用逗号）
./gen_raw_map.sh -s 12345 -v 1.21.1 -m ./mods.zip \
    -c 'chunky world mymod:skylands, chunky center 0 0, chunky radius 1000 1000, chunky shape diamond, chunky start'

# 上次跑到一半（Ctrl-C / 静默超时收尾 / 崩溃重启后都算），接着跑：命令不用改，只加 --resume
./gen_raw_map.sh -s 12345 -v 1.21.1 --resume -c 'chunky start minecraft:overworld circle 0 0 5000'

# 只看计划（打印下载清单与将写入缓冲区的命令），不下载不启动
./gen_raw_map.sh -s 12345 -v 1.21.1 -c 'chunky start minecraft:overworld circle 0 0 2000' --dry-run
```

参数（对应四个输入）：

| 输入 | 参数 |
|---|---|
| 1. 种子 | `-s/--seed` |
| 2. 游戏版本 | `-v/--game-version` |
| 3. 模组 zip（可选） | `-m/--mods-zip`，可重复；zip 内 `*.jar` 解压进 `mods/`，也接受 jar 目录 |
| 4. 地图范围 | `-c/--command`，**你直接编辑好的 chunky 命令**。一个 `-c` 是一批，可重复（一批一个维度）；一批里想写多条命令就用半角逗号分隔 |

其它常用项：`--work-dir`、`--world-dir`、`--java`、`--loader`、
`--no-c2me`/`--require-c2me`、`--level-type`、`--generator-settings`、`--no-structures`、
`--tick-cmd`(默认 `tick freeze`)/`--no-tick`、`--idle-timeout`(默认 10)、`--batch-sleep`(默认 30)、
`--no-batch-flush`、`--start-timeout`(默认 900)、`--stop-timeout`(默认 600)、
`--stop-kill-timeout`(默认 120)、`--skip-disk-check`、
`--server-property K=V`、`--debug-commands`（诊断用，见下）、`--resume`（断点继续，见下）、
`--reuse`、`--force`、`--dry-run`。详见 `-h`。

JVM 参数**默认一个都不加**（用 JVM 自己的默认值，最大堆默认是物理内存的 1/4），要调就手动传：

```bash
./gen_raw_map.sh ... --memory 8G                          # 只是加 -Xmx8G
./gen_raw_map.sh ... --jvm-args '-Xms8G -Xmx8G -XX:+UseZGC'   # 原样拼在 java 与 -jar 之间，可重复
```

## 执行时序（实测日志）

```
...
[18:45:15] [Server thread/INFO]: Done (1.545s)! For help, type "help"
[控制台输入] tick freeze                                        ← 看到 Done ( 立刻写进 FIFO（只发一次）
[控制台输入] chunky start minecraft:overworld circle 0 0 256    ← 第 1/3 批，顺序即执行顺序
[控制台输入] chunky start minecraft:the_nether circle 0 0 256   ← 同一批里的第二条（逗号分隔）
[18:45:16] [Server thread/INFO]: [Chunky] Task started in minecraft:overworld for the circle region centered at 0, 0 with radius 256.
[18:45:17] [Server thread/INFO]: [Chunky] Task running ... Processed: 24 chunks (2.20%), Rate: 157.9 cps
...（Chunky 默认每秒刷一行，脚本据此判断存活）
[18:45:20] [Chunky] Task finished for minecraft:overworld ... Processed: 1089 chunks (100.00%), Total time: 0:00:03
[18:45:24] [Chunky] Task finished for minecraft:the_nether ...
[控制台输入] save-all flush                                     ← 这一批跑完（日志静默 10s）后写盘
                                                                ← 再等 30s（--batch-sleep）
[控制台输入] chunky start minecraft:the_end circle 0 0 256      ← 第 2/3 批（下一批）
...
[控制台输入] save-all flush                                     ← 最后一批跑完也 flush + 等待
[控制台输入] stop                                               ← 然后才收工
```

要点：

* 命令**在服务端就绪（`Done (`）后立刻写入**，不等回显、不等执行结果；命令仍由服务端主线程在下一个
  tick 执行，和"启动瞬间写入"一样快（**不能更早**，见下节）；
* **一批 = 一个 `-c`**；一批里的多条命令（逗号分隔）按书写顺序下发，同一批的多个 `chunky start`
  会**同时**开始生成（Chunky 支持多世界任务并行）；
* **不发送 `chunky quiet`**：Chunky 默认每 1 秒刷一行进度（实测），这就是存活信号；
  若你改过 Chunky 配置让刷新变慢，请把 `--idle-timeout` 调大（建议 ≥ 3× 刷新间隔）；
* **每批的结束判据就是日志静默 `--idle-timeout` 秒**（和整体收尾判据同一个）。静默前最后一条
  Chunky 日志仍是 `Task running ...`、或本批有任务没等到 `Task finished/stopped` 时，脚本会打印
  「未结束的任务」警告（仅提示，不中断）；
* 每批结束 → `save-all flush`（把这一批生成的区块全部写盘）→ 等 `--batch-sleep` 秒 →
  下一批；**最后一批之后同样做一次**，然后才 `stop`。想跳过 flush 用 `--no-batch-flush`，
  不想等待用 `--batch-sleep 0`；
* **收尾用 `stop`**，由服务端自身关闭流程保存世界；实测 `/tick freeze` 冻结 tick 后，
  控制台命令仍会被处理，`save-all flush` 与 `stop` 照常生效；
* **不修改任何游戏规则、不改世界时间**，只发一条 `/tick freeze`（1.20.3 以下自动跳过；
  `--no-tick` 可完全不发）。冻结期间世界时钟**不推进**：26.1 存档里只有 `Time`，
  脚本跑完（含 Chunky 生成 + 静默等待）保存下来的 `Time=5`；同样一张图不冻结跑 10 秒
  则 `Time` +205 tick（≈20/s）。1.21.x 存档里对应字段是 `DayTime`，实测为 `1`（第 0 天）。

## 分块执行（分批与逗号）

一个 `-c` 就是一批，多个 `-c` **从前到后**一批一批跑；一批里要用多条命令，用半角逗号分隔：

```bash
# 3 批：主世界 -> 下界 -> 末地；每批之间 save-all flush + 等 60s
./gen_raw_map.sh -s 12345 -v 1.21.1 --idle-timeout 60 --batch-sleep 60 \
    -c 'chunky start minecraft:overworld circle 0 0 2000' \
    -c 'chunky start minecraft:the_nether circle 0 0 2000' \
    -c 'chunky start minecraft:the_end  circle 0 0 2000'

# 2 批：第 1 批两条 start 并行跑，第 2 批一条
./gen_raw_map.sh -s 12345 -v 1.21.1 \
    -c 'chunky start minecraft:overworld circle 0 0 1000, chunky start minecraft:the_nether circle 0 0 1000' \
    -c 'chunky start minecraft:the_end circle 0 0 1000'

# 同一批里先改设置再 start（顺序保留）
./gen_raw_map.sh -s 12345 -v 1.21.1 \
    -c 'chunky world minecraft:the_end, chunky shape square, chunky radius 500, chunky start minecraft:the_end'
```

规则与坑：

* **逗号只按"顶层"拆**：写在 `{}` `[]` 或 `'...'` `"..."` 里的逗号不会拆（NBT/JSON 常带逗号），
  也可以用 `\,` 表示一个字面逗号。裸逗号（不在括号/引号里）一定会被当成命令分隔符；
  反斜杠本身原样保留，所以 `\\,` 是"一个反斜杠 + 一个分隔符"，不是字面逗号；
  引号/括号没配对时脚本会告警（提醒这一行的逗号可能没被拆开），不会静默当成一条命令；
* 一条 `-c` 里也可以直接换行写多条命令，等价于逗号；
* 每批之间的动作是 **`save-all flush` + 等待 `--batch-sleep` 秒**（默认 30），最后一批之后也做；
  `--no-batch-flush` 关掉 flush；
* 每批的结束判据是**日志静默 `--idle-timeout` 秒**。自动保存/卡顿会让日志十几秒不刷新，
  大世界请把 `--idle-timeout` 调到 60~120，否则某一批可能被提前判结束（下一个批次的命令会提前
  下发；已经下发的任务不会被取消，只是分块边界变模糊）；
* 分批只是把"生成 → 写盘"切成小段，**不会**改变单个任务的范围与最终地图内容；同一张图分批跑与
  一次跑完的结果一致；
* 想"接着上一批继续"（脚本被 Ctrl-C、服务端崩溃、机器重启），下次命令一个字都不用改，加 `--resume`
  即可（见下一节）。

## 断点继续（--resume）

跑到一半（Ctrl-C、静默超时收尾、服务端崩溃后重启）想接着跑：**命令一个字都不用改，只加 `--resume`**。

```bash
# 第一次：三个维度，半径 5000
./gen_raw_map.sh -s 12345 -v 1.21.1 \
    -c 'chunky start minecraft:overworld circle 0 0 5000' \
    -c 'chunky start minecraft:the_nether circle 0 0 5000' \
    -c 'chunky start minecraft:the_end   circle 0 0 5000'

# 中途 Ctrl-C 之后：同一条命令加 --resume（隐含 --reuse，不用再写 --reuse）
./gen_raw_map.sh -s 12345 -v 1.21.1 --resume \
    -c 'chunky start minecraft:overworld circle 0 0 5000' \
    -c 'chunky start minecraft:the_nether circle 0 0 5000' \
    -c 'chunky start minecraft:the_end   circle 0 0 5000'
```

### 凭什么能续：Chunky 自己存了断点

Chunky 把每个没跑完的任务写在**工作目录**里（不是世界里）：

```
<work-dir>/server/config/chunky/tasks/minecraft/overworld.properties
world=minecraft:overworld
cancelled=false        ← false = 没跑完，可以继续（跑完或取消会变 true）
center-x=0.0
center-z=0.0
radius=5000.0
shape=circle
pattern=region
chunks=172773          ← 断点：已经迭代到第 172773 个区块
time=4312319           ← 累计耗时（毫秒），继续时累加
```

`chunky continue <世界>` 会拿这份文件建任务（`GenerationTask(selection, count=chunks, time, cancelled)`
→ `ChunkIteratorFactory.getChunkIterator(selection, count)`），迭代器直接**从 `chunks` 处接着走**，
不是把坐标空间从头再扫一遍。另外 Chunky 启动时会读 region 文件头（`RegionCache`），
默认 `forceLoadExistingChunks=false`，已存在的区块直接跳过 —— 双保险。

### 脚本做了什么

1. 启动前扫 `<work-dir>/server/config/chunky/tasks/**/*.properties`，挑出 `cancelled=false` 的任务；
2. 对每条 `chunky start ...`：认出它作用于哪个世界，该世界有未完成任务就改写成 `chunky continue <世界>`，
   并在日志里逐条打印，例如
   `断点继续: chunky start -> 'chunky continue minecraft:overworld'（circle 中心 0.0,0.0 半径 5000.0，已处理 172773 块）`；
3. 参数（形状 / 中心 / 半径x / 半径z）与存档不一致时**直接报错停下**，不会默默按存档参数生成到别的范围；
4. 没有未完成任务的世界照旧 `chunky start`；
5. 不开服务端也能预演：`--resume --dry-run` 会把改写后的命令打出来。

世界名怎么认：`chunky start minecraft:overworld ...` 直接认；`overworld` 这类简写按存档里的
`minecraft:overworld` 对齐；`chunky world X` + `chunky center/radius/shape` + 裸 `chunky start`
的分步写法会跟踪当前选择；裸 `chunky start` 且只有一个未完成任务、且它是主世界时，按主世界处理。

### 什么时候能续、什么时候续不了

| 情况 | 能续吗 | 说明 |
|---|---|---|
| 正常跑完 | 不需要 | 任务被标成 `cancelled=true`，`continue` 会跳过它；再跑同一条命令就是新一轮（已存在区块被跳过） |
| Ctrl-C / 静默超时收尾 / 正常 `stop` | ✅ | 脚本收尾都会 `stop`，Chunky 在服务端停止时写盘（`Chunky.disable()` → `saveTasks()`） |
| 服务端崩溃后重启 | ✅ | 恢复到崩溃前最近一次写盘的进度 |
| `kill -9` 脚本/服务端、断电 | ⚠️ | 进度只在「任务结束」和「服务端停止」两个时机写盘，硬杀会丢上次写盘之后的部分；已落盘的 region 文件不受影响，重跑时那些区块照样被跳过，只是要把坐标空间重新迭代一遍 |
| 手删过 `world/` | ❌ | 世界都没了就没有断点；用 `--force` 或先删 `config/chunky/tasks/` 再跑 |

### 想放弃断点、重新来

```bash
rm -rf <work-dir>/server/config/chunky/tasks       # 只清断点
./gen_raw_map.sh -s 12345 -v 1.21.1 --force ...    # 连世界一起重建（脚本会顺手清掉断点）
```

### 实测（1.21 + Chunky 1.4.23，circle r=1200，总量约 22.8k 区块）

第一次跑到 4458 块时 Ctrl-C（脚本收尾 `stop`，Chunky 在服务端停止时写盘）：

```
<work-dir>/server/config/chunky/tasks/minecraft/overworld.properties
cancelled=false   chunks=4458   time=17291
```

加 `--resume` 重跑**同一条命令**（命令一个字没改）：

```
[信息] 发现 1 个未完成的 Chunky 存档任务:
[信息]     minecraft:overworld  circle 中心 0.0,0.0 半径 1200.0，已处理 4458 块
[信息] 断点继续: chunky start -> 'chunky continue minecraft:overworld'（circle 中心 0.0,0.0 半径 1200.0，已处理 4458 块）
[信息] --resume：复用已存在的世界（断点继续）
[Chunky] Task continuing for minecraft:overworld.
[Chunky] Task running ... Processed: 4470 chunks (19.60%) ...    ← 从 4458 接着数，不是从 0
...
[Chunky] Task running ... Processed: 9853 chunks (43.21%) ...
# 再 Ctrl-C 一次：
cancelled=false   chunks=9881   time=38026                       ← 计数与累计时间都接着涨
```

对照：存档任务在，但**不加** `--resume`（只 `--reuse`）：

```
[警告] 发现 1 个未完成的 Chunky 存档任务（上次没跑完）:
[警告]     minecraft:overworld  circle 中心 0.0,0.0 半径 1200.0，已处理 9881 块
[警告] 不加 --resume 时，这些世界的 'chunky start' 会被 Chunky 拦下来（只打印 A task was already
[警告] started for this world ...），本次不会真正开始生成。要接着上次的进度跑，请加 --resume。
```

服务端那边确实只回了一句、`Processed` 进度行 0 条（一个区块都没生成）：

```
[Chunky] A task was already started for this world. To continue running it, type
         '/chunky continue'. To start a new task, type '/chunky confirm'.
```

同一时刻在控制台敲 `chunky continue minecraft:overworld` 就接着跑了
（`Processed: 10988 chunks (48.19%)`，从 9881 接着数）。

> `--resume` 与 `--force` 互斥（一个要续、一个要重建），脚本直接报错。
> `--resume` 隐含 `--reuse`；要继续就必须用同一个 `--work-dir`（默认 `work/<版本>-<种子>`，
> 所以 `-v`/`-s` 相同就自动是同一个）。

## 已知坑（实测汇总）

### 1. 命令必须等 `Done (` 之后再写（MC 26.1+）

服务端控制台线程是**读到那一行时**就把 `CommandSourceStack` 建好、连同命令一起塞进 `consoleInput` 队列的，
而这份 source 里的 `ServerLevel` 要等世界加载完才存在。MC 26.1+ 里**所有**命令（包括 `list` 这种）都会去取
`getLevel().getGameRules()`，所以世界加载完成前写入的命令 **100% 失败**，日志只留下：

```
[Server thread/INFO]: An unexpected error occurred trying to execute that command
```

`Done (` 之后写入则一切正常（命令仍由主线程在下一个 tick 执行，和"启动瞬间写入"一样快）。
早期版本 `gen_raw_map.sh` ≤ v2.0.0 是"java 一起动就写"，在 MC 26.1 上必踩此坑；v2.1.0 起改为就绪后写。

### 2. 命令被拒绝时，默认看不到原因 → 用 `--debug-commands`

上面那句就是默认的全部信息：真实异常藏在悬停提示里，堆栈只在 debug 级别记录（26.1 的
`Commands.performCommand` 把 `LOGGER.error("Command exception: ...")` 包在 `LOGGER.isDebugEnabled()` 里）。
加 `--debug-commands`（= 加 `-DMC_DEBUG_ENABLED -DMC_DEBUG_VERBOSE_COMMAND_ERRORS`）重跑即可：

```
[Server thread/ERROR]: '/list' threw an exception
java.lang.NullPointerException: Cannot invoke "net.minecraft.server.level.ServerLevel.getGameRules()"
        because the return value of "net.minecraft.commands.CommandSourceStack.getLevel()" is null
```

不带该开关时，脚本若在日志里看到上面那句失败提示，会额外 `[警告]` 一次提醒你加开关重跑（仅提示，不影响流程）。

### 3. `stop` 前不解冻，也不需要解冻

脚本收尾只发 `stop`，**不发 `tick unfreeze`**：

* `stop` 走 `MinecraftServer.stopServer()` 主线程关闭流程，与 tick 循环无关；冻结状态下控制台队列
  照常被处理（`DedicatedServer.tickConnection()` → `handleConsoleInputs()`，否则你也发不出 `/tick unfreeze`）；
* 实测（冻结中 Ctrl-C 收尾）日志顺序：`Stopping server` → `Saving chunks for level ... overworld/the_nether/the_end`
  → `ThreadedAnvilChunkStorage: All dimensions are saved`，每个维度各自完整保存；
* 反过来说，解冻会让世界时钟继续走（见下条），与"生成时不加游戏天数"的要求冲突。

### 4. `/tick freeze` 是内存状态，**不写进存档** —— 地图交给真服务端不会默认冻结

* 代码层：`net.minecraft.world.TickRateManager` / `ServerTickRateManager` 里对 `NbtCompound` 的引用数为 **0**，
  `setFrozen(boolean)` 只是改一个内存布尔值（和 `/tick rate` 一样是运行时调试状态）；
* 存档层：在 `The game is frozen` 之后 `stop` 保存的 `level.dat`，全部键里**没有任何** tick/frozen 相关项；
* 实测层：拿这张世界开一个干净服务端（不发任何命令），`/tick query` 输出
  `The game is running normally` + `Target tick rate: 20.0 per second.`，
  且 `level.dat` 的 `Time` 在 10 秒内 +205 tick（≈20/s），世界自己在正常走 tick。

想自己复核：`/tick query` 看状态，或比对 `level.dat` 里 `Time` 的增长。

### 5. 世界时间 / 天数

冻结期间世界时钟不推进。26.1 的 `level.dat` 里**没有 `DayTime` 键**，只有 `Time`：脚本跑完
（Chunky 生成 + 静默等待）保存下来的 `Time=5`（只有 `tick freeze` 生效前那几个 tick）；
同样一张图在干净服务端上跑 10 秒则 `Time` +205 tick。1.21.x 的字段是 `DayTime`，实测为 `1`（第 0 天）。

### 6. 看门狗与慢 tick

`A single server tick took ...` / `Considering it to be crashed` 这两句只由 `ServerWatchdog` 打印。
脚本写入 `max-tick-time=-1` 时，1.21 的 `MinecraftServer` **根本不会创建看门狗线程**
（javap 验证：`getMaxTickLength() <= 0` 就不 `new ServerWatchdog`），所以这两句不会出现：
慢 tick 既不会被误判成崩溃，也不会被强杀。

反过来，用 `--server-property max-tick-time=<正数>` 会**打开**看门狗：一次超过该值的慢 tick
（比如超大世界 `save-all` 写盘）会让服务端自己 `System.exit(1)`。脚本检测到这种覆盖会告警。

收尾时脚本也不抢跑：`stop` 之后默认最多等 `--stop-timeout`（600s），仍不退出才 SIGTERM，
再等 `--stop-kill-timeout`（120s）才 `kill -9`；等待期间会打印 `[停止] 已等待 x/y s`，
日志里一旦出现 `All dimensions are saved` 会提示「世界写完，等进程退出」。

### 7. 把生成好的地图交给"真正的服务端"

| 事项 | 说明 |
|---|---|
| 版本 | 世界由对应版本的 Fabric 服务端生成，真服务端版本必须 ≥ 该版本（更低版本会拒绝加载更新 data version 的存档） |
| 数据包 | 26.1 存档的 `DataPacks.Enabled` 为 `vanilla` + `fabric-convention-tags-v2`（Fabric API 带的）。纯原版端加载会在日志里提示找不到后者并忽略，**不影响世界数据**；若要完全一致，用 Fabric + Fabric API 端 |
| 拷贝范围 | 只需 `world/` 整个目录（`level.dat`、`dimensions/`、`players/`、`session.lock` 等）；`server/`、`mods/`、`server.properties`、`config/` 属于工作目录，不是世界的一部分 |
| 世界内容 | `level-type=minecraft:normal`、无玩家痕迹、出生点即生成中心；Chunky/C2ME 不添加方块、维度或数据包内容 |
| 中断也是干净的 | 中途 Ctrl-C 会触发脚本收尾（`收到中断信号` → `stop`），世界同样完整保存；重跑同样命令加 `--resume` 可继续。大世界（半径 5000）收尾时 `stop` 后写盘可能好几分钟：脚本先等 `--stop-timeout`（默认 600s）、再 SIGTERM 并多给 `--stop-kill-timeout`（默认 120s）才强制结束（`console.log` 里通常已经 `All dimensions are saved`，只是 JVM 退得慢） |

### 8. 断点继续要靠 `--resume`，否则 `chunky start` 会被 Chunky 拒绝

Chunky 把没跑完的任务存成 `<work-dir>/server/config/chunky/tasks/<命名空间>/<世界>.properties`
（`cancelled=false` + `chunks=<已处理数>`）。这种文件存在时：

* `chunky start ...` **不会开始生成**，只在控制台打印
  `[Chunky] A task was already started for this world. To continue running it, type '/chunky continue'. To start a new task, type '/chunky confirm'.`
  —— 脚本会照常等 `[Chunky]` 输出、静默 `--idle-timeout` 秒后进入下一批，看起来"跑完了"，
  其实那个世界一个区块都没生成（脚本检测到这句拒绝信息会额外警告）；
* `chunky continue <世界>` 才会从 `chunks=` 记录的断点接着跑。

所以：**接着跑要加 `--resume`**（脚本自动把 `chunky start` 换成 `chunky continue`，参数不一致时会直接报错停下）；
**想重新来过就不要 `--resume`**，但要先清掉存档任务：`rm -rf <work-dir>/server/config/chunky/tasks`
（`--force` 现在会顺手清）。详见[断点继续（--resume）](#断点继续--resume) 与[已知坑 7 的中断那行](#7-把生成好的地图交给真正的服务端)。

### 9. JVM 参数：默认一个都不加

启动命令就是 `java -jar fabric-server-<版本>.jar nogui`，**不加任何 JVM 参数**（最大堆 = 物理内存的 1/4）。
之前脚本内置过一整套 Aikar 参数（`-Xms2G -Xmx8G -XX:+AlwaysPreTouch ...`），已全部去掉——调参交给使用者：

```bash
--memory 8G                             # 只加 -Xmx8G
--jvm-args '-Xms8G -Xmx8G -XX:+UseZGC'  # 原样拼在 java 与 -jar 之间，可重复
```

实际命令行会打进 `run.log`（`启动 Fabric 服务端: ...`）与 `summary.json` 的 `jvm_args`。

顺带澄清一个容易误会的现象：**新工作目录第一次启动慢到几分钟才 `Done (`**，那是 Fabric 在 java 进程里
下载/解包 vanilla `server.jar` 与全部依赖库（实测 217s，而真正的 `Done (2.251s)` 只占 2 秒），
跟 JVM 参数没关系；`--start-timeout` 默认 900 就是给这种情况留的，第二次起就快了。


### 10. 分块执行的边界也是「日志静默」；`save-all flush` 是脚本按批下发的

* 每批的结束判据 = 日志静默 `--idle-timeout` 秒（与脚本原来的收尾判据同一个）。Chunky 生成期间
  每秒刷一行进度，所以"静默"就等于"这批没任务在跑了"；但**自动保存/卡顿也会让日志十几秒不刷新**，
  这时本批会被提前判结束、下一批命令提前下发（已下发的任务不会被取消，只是分块边界变模糊）。
  实测一份 45 分钟、4735 行的 `console.log`：>10s 的间隔只有 3 次，最大 83s 出现在 OOM 崩溃那一刻
  —— 大世界建议 `--idle-timeout 60~120`；
* 脚本只把命令写进控制台，不动世界数据；批与批之间（以及最后一组之后）下发的 `save-all flush` 与
  收尾的 `stop` 都由服务端自己执行。1.21 里 `/save-all` 仍有 `flush` 子参数（两者都是完整保存），
  要关掉脚本这个动作就加 `--no-batch-flush`；
* 一条 `-c` 里的**裸逗号一定会被当成"多条命令"的分隔符**。命令本身带逗号（NBT/JSON）时请写在
  `{}` `[]` 或 `'...'` `"..."` 里，或用 `\,` 转义；一条 `-c` 也可以直接写多行（换行同样是分隔符）。
  反斜杠原样保留（`\\,` = 一个反斜杠 + 一个分隔符）；引号/括号没配对时脚本会告警，不会闷着拆错。

## 输出

```
<work-dir>/                 默认 ./work/<版本>-<种子>
├── world/                  世界数据（1.21.x：region/、DIM-1/、DIM1/；
│                           26.1+：dimensions/minecraft/<维度>/region/ 等，全维度在 dimensions/ 下）
├── server/                 Fabric 服务端（server.jar、mods/、server.properties、eula.txt）
│   └── config/chunky/tasks/<命名空间>/<世界>.properties   断点继续用的任务进度（见上）
├── console.log             本次运行的服务端完整控制台输出（含每条 [控制台输入] 标记）
├── console.<时间戳>.log     上一次运行的 console.log（每次启动自动轮转保留，崩溃/退化证据不丢）
├── run.log                 脚本自身日志
├── mods.tsv                本次安装的 Chunky/C2ME/依赖 版本与哈希
└── summary.json            汇总（版本、种子、透传命令、模组哈希、耗时、各 region 落盘区块数统计、
                            断点继续状态 resume/resumed_worlds、JVM 参数 jvm_args、
                            分块状态 batch 与 command_groups：每批的命令、静默秒数、启动/结束任务数、未结束任务）
```

`summary.json` 里的 `chunks_on_disk` 只是"各 region 目录落盘了多少区块"的统计，**不做任何合格判定**。

退出码：`0` 正常；`1` 参数/依赖/下载/启动失败，或服务端崩溃、提前退出。

## 服务端配置（脚本写入的 server.properties）

| 项 | 值 | 原因 |
|---|---|---|
| `level-name` | 绝对路径指向 `--world-dir` | 世界与脚本、服务端文件分离 |
| `level-seed` | 你的种子 | 原样写入 |
| `pause-when-empty-seconds` | `0` | **必须**：1.21.2+ 无人时暂停服务端会让 Chunky 假死 |
| `max-tick-time` | `-1` | 关掉看门狗，避免长 tick 被当崩溃强杀 |
| `sync-chunk-writes` | `false` | 关同步写盘 |
| `online-mode` | `false` | 单机预热，无需认证 |
| `view-distance` / `simulation-distance` | `3` | 降低内存占用 |
| `difficulty` / `gamemode` | `peaceful` / `creative` | 与生成无关的玩家逻辑 |

需要别的值就用 `--server-property key=value` 覆盖（可重复）。`eula.txt` 自动写 `true`。

## 已知 Chunky 行为（供参考，脚本不做干预）

用 `check_chunks.py`（**可选的手工诊断工具，脚本不会调用**）逐区块读 `Status` 实测过：

* Chunky 的 `Processed: N chunks (100%)` 是**派发计数**，可能远早于真实生成完成
  （曾出现 2 秒报 100%、`Rate: 3774 cps`，随后 CPU 立刻 0%，落盘区块停在 `structure_starts`）；
* 默认 `region` 迭代器按 region 对齐 + Hilbert 顺序派发，生成**数量**接近请求值但**位置有偏移**，
  请求范围内会有一小块系统性缺口；
* 重新运行不会补缺口（Chunky 认为"区块已存在"就跳过）；
* 版本差异明显：MC 1.21.1 + Chunky 1.4.23 的落盘情况明显好于 1.21.8 + Chunky 1.4.40。

想查缺口时（`check_chunks.py` 会自动识别 1.21.x 的 `region/`、`DIM-1/`、`DIM1/` 布局与
26.1+ 的 `dimensions/<命名空间>/<维度>/region` 布局）：

```bash
./check_chunks.py --world <work-dir>/world --center 0 0 --radius 2000 --map
```

## 常见问题

| 现象 | 处理 |
|---|---|
| 加了 `--resume` 却在「校验游戏版本与 Fabric loader」就报错退出 | 每次运行都要访问 `meta.fabricmc.net`（选/校验 loader 与 installer）和 `api.modrinth.com`（解析 Chunky/C2ME 版本），这两个站不通时脚本会在**启动服务端之前**就退出：世界和断点进度一点都不受影响，网络恢复后重跑同一条命令即可 |
| 想调 JVM/GC 参数、或固定堆大小 | 脚本**默认一个 JVM 参数都不加**（用 JVM 默认值，最大堆 = 物理内存 1/4）；要加就 `--memory 8G`（只加 `-Xmx8G`）或 `--jvm-args '-Xms8G -Xmx8G -XX:+UseZGC'`（原样拼在 `java` 与 `-jar` 之间，可重复）；实际命令行会打进 `run.log` |
| 第一次启动卡了好几分钟才 `Done (` | 那是 Fabric 在 java 进程里下载/解包 vanilla `server.jar` 与全部依赖库（实测新工作目录 217s，而真正的 `Done (2.251s)` 只占 2 秒），与 JVM 参数无关；`--start-timeout` 默认 900 就是给这种情况留的。第二次起就快了 |
| `An unexpected error occurred trying to execute that command` | 你的命令被服务端拒绝了。默认看不到原因，加 `--debug-commands` 重跑，`console.log` 里会有真实异常与堆栈（脚本也会在等待 Chunky 输出时提示一次）。见[已知坑 1/2](#1-命令必须等-done--之后再写mc-261) |
| `未检测到任何 Chunky 输出` | 命令拼错或模组没加载；先看 `console.log` |
| 静默 10s 就停了，但任务没跑完 | Chunky 刷新间隔比 10s 长 → 调大 `--idle-timeout` |
| 想让每批之间存盘并歇一会儿 | 默认就会：每个 `-c` 跑完 `save-all flush` + 等 `--batch-sleep` 秒（默认 30，最后一批之后也做）。想更快就 `--batch-sleep 0`，想关掉 flush 就 `--no-batch-flush` |
| 想跑很大的图（几十万区块） | 用多个 `-c` 分批 + 把 `--idle-timeout` 调到 60~120 + `--batch-sleep 60`：每批小一点，写盘和停顿都更频繁。注意**内存 ≈ 已生成区块数 × 约 44KB**（实测：24G 堆在约 55 万区块时 OOM），分批只限制"每批的量"，真正把内存降回去要靠**重启服务端**（脚本不做，可中途 Ctrl-C 后用 `--resume` 接着跑）。另外大世界生成后期会明显变慢（自动保存要遍历所有驻留区块，实测从 ~500 cps 掉到 ~30 cps），属于服务端/Chunky 侧行为，不是脚本问题 |
| 一批跑完到下一批之间日志安静了几十秒，是卡死了吗 | 那是脚本在 `save-all flush` + 等 `--batch-sleep`（默认 30s）：终端会显示 `[批间隔] 已等待 x/y s` 的倒计时，`run.log` 里记着 `第 x/y 组：save-all flush ...` 与 `等待 Ns 再继续`。大世界 `save-all flush` 本身也要几秒到几十秒（写盘期间控制台不刷新） |
| 启动报 `UnsupportedClassVersionError` | 用 `--java <路径>` 指定更高版本 JDK |
| 第三方 mod 缺依赖导致启动崩溃 | 脚本不解析第三方 mod 依赖，直接退出并打印日志尾部，按日志补 jar 后重跑 |
| 想超平坦/虚空 | `--level-type minecraft:flat --generator-settings '{"layers":[],"biome":"minecraft:the_void"}'` |
| 磁盘不够 | 可用空间 < 5GB 时脚本拒绝启动（要强行继续用 `--skip-disk-check`；`--force` 不再跳过这个检查） |
| 担心地图带给真服务端后被默认冻结 | 不会：`/tick freeze` 是内存状态、不写进存档。见[已知坑 4](#4-tick-freeze-是内存状态不写进存档--地图交给真服务端不会默认冻结) |
| 生成完发现世界时间/天数动了 | 冻结期间世界时钟不推进（26.1 实测 `Time=5`）。见[已知坑 5](#5-世界时间--天数) |
| 中途 Ctrl-C 了，世界会不会坏 | 不会：脚本收到中断信号会先 `stop` 让服务端完整保存，再退出；想接着跑加 `--resume`（见[断点继续](#断点继续--resume)），只想复用世界则 `--reuse` |
| 想接着上次的断点继续生成 | 加 `--resume`，命令一个字都不用改：脚本会把有未完成任务的世界的 `chunky start` 换成 `chunky continue <世界>`。见[断点继续（--resume）](#断点继续--resume) |
| 加了 `--reuse` 重跑，日志里有 `A task was already started for this world...`，一个区块都没生成 | 那个世界有未完成的存档任务，Chunky 拒绝了 `start`。要续就加 `--resume`；要重来先 `rm -rf <work-dir>/server/config/chunky/tasks`（或 `--force`）。见[已知坑 8](#8-断点继续要靠---resume否则-chunky-start-会被-chunky-拒绝) |
| 报错 `断点继续参数不一致（避免生成到非预期范围）` | 存档任务的形状/中心/半径与这次的命令不一致。要么把命令参数改回与存档一致继续，要么删掉那个 `.properties` 后按新参数重跑（或 `--force`） |
| `--resume` 报 `--force 冲突` | 两者语义相反：`--resume` 要接着跑，`--force` 要删掉世界重建。二选一 |
