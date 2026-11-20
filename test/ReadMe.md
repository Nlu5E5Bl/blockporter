# test —— nbtrw / blockporter 的测试

用户手册在 `../blockporter-README.md`，库 API 在 `../ReadMe.md`，开发约定在 `../ReadMeDeveloper.md`。

## 怎么跑

```sh
cd ..                  # 回到 nbtrw/
sh test/lib.sh         # 库：lib.cpp 612 个 + lvl.cpp 34 个 + bad.cpp 75 个坏数据用例 + demo 的 nbtw 自测（只编 make nbtrw）
sh test/block.sh       # merge/fixer 合成世界回归
sh test/rules.sh       # 26.1.1 规则（rules.so.26.1.1）：473 条用例、第二遍 0 改动、POI 表对拍
sh test/format.sh      # 同一批场景放进 4788/5119 两种存档格式，fixer/merge 结果必须一样
sh test/safe.sh        # ASan(+UBSan)+LSan 另编一套，把上面四个原样再跑一遍
```

五个脚本都从 `nbtrw/` 跑，只往 `test/work/` 写，不碰真实地图。`test/work` 是指到 `/tmp/nbtrw-work` 的符号链接（`test/work.sh`
建它，每个脚本开头都 `.` 进来；`NBTRW_WORK=<目录>` 可以换）：测试造的世界、副本、日志都能重新生成，一律放 `/tmp`，重启就没了。要编的先 `make`（`lib.sh` 只要 `make nbtrw`）。
`lib.sh`/`block.sh`/`rules.sh`/`format.sh` 认环境变量 `B=<目录>`：设了就不编译，直接用那个目录里编好的程序
（`safe.sh` 就是这样把 ASan 版塞进去的）。脚本里不用 `程序 | tee` 这种管道：`set -e` 只看管道最后
一个命令，程序失败会被吞掉，所以都是先重定向到 `test/work/*.log` 再检查退出码。

拿原版服务端 jar 让游戏自己判的对拍（手动跑，要 jar 和 Java 25，见下面“和游戏对拍”）不在这五个脚本里。

## 目录

```
test/
  lib.sh  block.sh  rules.sh  format.sh  safe.sh   五个入口
  lib.cpp                 库级用例（NBT/Block/Chunk/MCA/Level/format_Chunk/pin/Block_Ref/lz4/外置 .mcc/1451 以后各版本的区块格式）
  lvl.cpp                 Level 维度/布局/缓存
  gen_tables.py           从 mcdump.sh shape 导出的 shapes.tsv 生成规则插件里“只跟名字有关”的数据表，原地写进 rules.cpp.<版本> 的 /*<gen:…>*/ 之间
  bad.cpp                 坏数据：每个用例 fork 一个子进程（300 秒转不出来按崩了算）；库只许用返回值报错，不崩、不 exit、不漏 fd
  mkmerge.cpp checkmerge.cpp   造/核对 merge 合成世界（多 old、new 缺段跳过、POI/实体追加与新建、坏目标块）
  mkfix2.cpp  checkfix2.cpp    造/核对 fixer 合成世界（物理、最小单位删、POI/计划刻/实体、0 字节文件、规则版本不同的区块…）
  checkzero.cpp           核对 MCA 里某一格还在不在（0 字节 region 用例）
  mkfmt.cpp  checkfmt.cpp      把 lvlworld 转成 26.4 写法（DataVersion 5119），两种格式布置同一批场景；
                               核对 fixer 修完逐格一样、新格式没混进老写法；merge 同版本写法对、缺段跳过、跨版本拒绝
  mkrule.cpp              “一条规则一个例子”的用例表（每条写了依据）+ 造世界
  check_rules.py          用 nbt.py 独立核对 rules.sh 的结果
  poidump.cpp             把 rules.so 的 方块→默认POI 表打出来
  check_poitab.py         poidump 的输出和 poitab.py 对拍
  poitab.py               照 wiki 手抄的 方块→POI 类型/票上限 表（故意不看 rules.cpp）
  nbt.py                  独立写的 NBT/区域解析器（核对脚本用，故意不复用 C++ 代码；区块只认 gzip/zlib，不读 lz4/外置 .mcc）

  mcdump.sh               和游戏对拍的入口（见下面）
  shapecmp.cpp            mcdump.sh shape 用：游戏形状表逐状态比 rules.so 的 priv[P_SHAPE]（游戏1规则0＝可能误拆）；
                          碰撞箱随坐标偏移的竹子/滴水石锥（@off 行）在那个坐标上让 rules.so 判一段墙来核对
  regioncmp.cpp           mcdump.sh region 用：和游戏的 RegionFile 互相读对方写的区域文件（四种压缩 + 外置 .mcc）；
                          再拿游戏的 BitStorageAlignFix 核对 2527 以前紧排下标的读法
  upgradecmp.cpp          mcdump.sh upgrade 用：6 个版本 × 32 种段（Palette/BlockStates、block_states 的各种好坏写法），
                          游戏（mc/UpgradeProbe.java：DataFixers 升级 + 26.1.1 编解码读）怎么判/怎么读，库的判坏和逐格读法要一样
  mc/McWorld.java         初始化注册表+绑原版标签，造“自己+六个邻居”的假世界让游戏判（只认 26.x）
  mc/ShapeDump.java       shape：逐状态导出六个面的 FULL/CENTER/RIGID/碰撞面满、isSolid、isSolidRender、可替换/液体/可燃、
                          放在墙上面时碰撞箱底面盖住墙的哪几块（WallBlock.isCovered）
  mc/RulesCmp.java        cmp：每个要支撑的方块状态（加上栅栏/墙这些规则表管的），六个邻居轮流换成全部状态，
                          游戏判定 vs rules.so（故意不一样的在 known 表）
  mc/rules_shim.cpp       cmp 用：把 rules.so 的 C++ 接口包成 extern "C"，Java 用 FFM 调
  mc/RegionProbe.java     region：游戏的 RegionFile 读写区域文件；align 模式跑 BitStorageAlignFix.addPadding
  mc/UpgradeProbe.java    upgrade：照 ChunkMap.upgradeChunkTag 用 DataFixers 升级老区块，照 SerializableChunkData.parse 读每一段

  手动工具（不进自动测试）：
  judge.cpp check_placement.py  直接装 rules.so 判一格 + 拿 wiki Opacity/Placement 三张放置表（2269 格）逐格对拍；有了游戏对拍以后
                          只是旁证，wiki 和游戏不一样的格子记在 KNOWN 里（注明“游戏：…”）。judge 装不上插件就报错（以前静默当 0 格不一致）
  mc/probe/               规则从游戏生成（ReadMeDeveloper 1.9）的原型：SurviveProbe（每种方块靠哪些邻居活）、FitProbe（拟合支撑条件），mcdump.sh probe|fit 跑
  cmp_mca.py              两个 .mca 逐区块逐值比（改代码前后各跑一遍 merge/fixer 对输出用；只认 gzip/zlib）
  check_poi_scan.py       只读扫一个真实存档的 poi/，看记录和方块对不对得上（慢）
  palstat.cpp             统计 region 目录里的方块种类/属性取值（写规则时查真实属性名）
  wiki2md.py              抓 minecraft.wiki 页面的 HTML 转成 Markdown（只用标准库，不用 action=raw）
  wiki_pages.txt          wiki2md.py -f 用的页面清单：规则/格式依据涉及的页面（加 --lang zh 抓同一批的中文页）
  wiki_pages_zh.txt       只能用中文名抓的中文页（格式、版本、判定箱……）

  data/level.dat          样例 level.dat（DataVersion 4788）
  data/lvlworld/          小世界：新布局（只有 dimensions/，没有老的 region/）
  data/lvlold/            小世界：老布局（DataVersion 3465）
  data/lvlnodat/          小世界：只有 region 没有 level.dat
  data/lz4/r.0.0.mca      游戏（26.1.1 的 RegionFile + lz4-java）写的 lz4 区域样本，lib.cpp 读它
  work.sh                 各脚本开头 . 进来：建 test/work → /tmp/nbtrw-work 的链接
  work/                   跑测试造出来的一切（符号链接，不进 git）
```

## 约定

- 核对脚本用独立的 `nbt.py`/`poitab.py`，不链 `libnbtrw.so`（自己验自己没意义）；Python 用 `python3 -I` 跑；
- 测试脚本怎么写由开发者自己定（用户 2026-11-21）：期望值写死并注明依据、改代码前后 A/B 对比，都是建议不是限制；
- 长期用的测试/工具代码放 `test/` 正式目录，不放临时目录；临时副本、日志一律在 `test/work`（也就是 `/tmp`）；
- 真实地图只在副本上跑；`test/work/wiki/` 是规则调研用的 wiki HTML/笔记，不是测试输入。

## 和游戏对拍（第一依据）

官方服务端 jar（启动器包），要 Java 25；只读 jar，解出来的东西和结果都在 `test/work/mc/<版本>/`。

```sh
# 插件不给就用 ./rules.so.<jar 的版本>（26.1.1 的 jar 配 rules.so.26.1.1，26.3 的配 rules.so.26.3）
sh test/mcdump.sh <server.jar> shape          # 形状、材质、音色、树叶标签：shapes.tsv vs 插件 → shapes-diff.tsv（应为空）
python3 -I test/gen_tables.py test/work/mc/<版本>/shapes.tsv rules.cpp.<版本>   # 用 shapes.tsv 重新生成插件里的数据表（音色/树叶标签/材质/默认属性）
sh test/mcdump.sh <server.jar> cmp            # 存活规则和栅栏/墙连接穷举对拍 → rules-cmp.txt（“#差异合计”应为没有）；20 核约 20 分钟
sh test/mcdump.sh <server.jar> cmp ./rules.so.26.1.1 vine,oak_door   # 只测几种方块（改一条规则时用）
sh test/mcdump.sh <server.jar> all            # shape + cmp
sh test/mcdump.sh <server.jar> region         # 区域文件：游戏和 libnbtrw 互读 gzip/deflate/none/lz4 + 外置 .mcc → region/check.txt
sh test/mcdump.sh <server.jar> upgrade        # 段的方块状态：各版本交给游戏升级+读，和库比 → upgrade/check.txt（“0 处不对”）
sh test/mcdump.sh <server.jar> probe|fit      # 规则从游戏生成的两个原型（test/mc/probe/）
# wiki 放置表对拍（旁证）：
#   python3 -I test/wiki2md.py -o test/work/wiki Opacity/Placement
#   g++ -std=c++23 -O2 -I. test/judge.cpp -o test/work/judge -L. -lnbtrw -lz -ldl -Wl,-rpath,$PWD
#   python3 -I test/check_placement.py [rules.so.26.1.1]    # 2026-11-21：2269 格，不一致 52 格都在 KNOWN 里
```

## wiki 资料（第三依据）

```sh
python3 -I test/wiki2md.py -f test/wiki_pages.txt -o test/work/wiki            # 清单里的页面（有缓存用缓存）
python3 -I test/wiki2md.py -f test/wiki_pages.txt -o test/work/wiki --refresh  # 全部重新抓
python3 -I test/wiki2md.py -f test/wiki_pages.txt -o test/work/wiki --lang zh  # 同一批的中文页（存成 zh_<英文名>.md）
python3 -I test/wiki2md.py -f test/wiki_pages_zh.txt -o test/work/wiki --lang zh
python3 -I test/wiki2md.py --section "Block states" Slab                       # 只看一节
python3 -I test/wiki2md.py --toc Chunk_format                                  # 只看目录
python3 -I test/wiki2md.py --oldid 3860864 Chunk_format                        # 指定修订
```

只抓 `https://minecraft.wiki/w/<页面>`（或语言子站、`?oldid=`）的渲染后 HTML，拒绝 `action=raw` 这类接口和
站外网址。`-o` 目录里存 `<名>.html`（原文，下次直接转换不联网）和 `<名>.md`；每个 `.md` 开头有网址、
修订号、最后修改时间。默认去掉站内链接网址、图片、导航框和给页面脚本用的 JSON 数据块，
`--links`/`--images`/`--navbox` 可以留下。表格的 rowspan 会往下复制（单看一行也完整），
“左 th 右 td”的属性表（信息框）写成列表。
