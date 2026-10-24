# nbtrw —— 用户手册

项目分两部分：

| 部分 | 内容 | 产物 |
|---|---|---|
| **nbtrw / libnbtrw** | NBT、Block/Chunk、MCA 区域文件、Level 存档 | `libnbtrw.so`，demo `nbtrw` |
| **blockporter** | merge（方块搬运）、fixer（存档修复）、rules（物理规则插件） | `merge`、`fixer`、`rules.so.26.1.1` |

`prune` 已经并进 `fixer`，不再单独提供。`test/` 里依赖旧 API 的 `run.sh`/`conv.sh` 等还没迁移；
当前能直接跑的是库测试 `sh test/lib.sh` 和 `test/lvl.cpp`（见第 6 节）。

## 1. 编译

```sh
make                 # libnbtrw.so + nbtrw（demo）
make blockporter     # rules.so.26.1.1（并链出 rules.so）+ merge + fixer
make clean
```

C++23，`-Wall -Wextra -pedantic`，程序用 `-Wl,-rpath,'$ORIGIN'` 找同目录的 `libnbtrw.so`。
`fixer` 运行时先找自己目录下的 `rules.so`，没有再找当前目录的 `rules.so`；`rules.so` 是指向
`rules.so.26.1.1` 的符号链接。规则接口的兼容性只由 `RULES_ABI` 把关（见 §5）。

## 2. 库：Level 是整个存档，不绑定维度

`Level` 对应 wiki 的“世界/存档（World/Level）”：一个 `level.dat` 下的所有维度。

```cpp
Level lv("path/to/save", 64, 8, 16);   // path, chunk缓存, region缓存, dat缓存

lv.get_Block("overworld", x, y, z);
lv.set_Block("the_nether", x, y, z, block);
lv.get_Chunk("minecraft:the_end", cx, cz);
lv.get_MCA("DIM-1", rx, rz);          // 非原版维度名：当相对存档根的维度目录
lv.get_Dat("level.dat");              // dat 是存档级，不带维度
```

维度参数的规则：

- 只认三个原版维度名：`overworld`、`the_nether`、`the_end`，可以带 `minecraft:` 前缀；
  这三种按 `DataVersion` 自动选新/老布局：
  - 新（`DataVersion >= 4774`）：`dimensions/minecraft/<name>/`
  - 老：`region/`、`DIM-1/`、`DIM1/`
- 其它字符串一律当“相对存档根的维度目录”，例如 `DIM-1`、`dimensions/minecraft/custom`；
  自动拼 `/region`、`/entities`、`/poi`。不许绝对路径、`..`、空路径段。
- 同一个 `Level` 里不同维度同坐标不会串；缓存按“维度目录 + 坐标”分桶。

`Level` 还提供：

- `get_DimensionBase()`：维度参数 → 相对目录（老布局主世界是空串），无效返回 `nullopt`；
- `get_DimensionPath(dim, kind)`：`kind` 是 `region`/`entities`/`poi`，返回相对存档根的路径；
- `pin_Chunk()`/`unpin_Chunk()`：钉住一个区块，`unpin` 前它的指针不会被 LRU 淘汰；缓冲区全被钉住时
  `get_Chunk`/`pin_Chunk` 返回 `GET_CACHE`，不会硬抢；
- `Chunk::format_Chunk()`：整理区块，当前只做一件事——同一 `Y` 有多段时只留列表最前面那段，
  返回删掉的段数；`Chunk` 的读写接口本身永远取第一个 `Y` 匹配的段，不自动删除；
- `erase_Chunk()`：把区块从 MCA 表头删掉（fixer 用，本来没有也算成功）；
- `save()`/`close()` 返回 `bool`，库不打印错误、不 exit、不记全局计数。

库的其它约定见 `nbtrw.hpp` 顶部注释：`nbt_Any` 一字节 type、无虚函数；`Chunk` 有只增不减的
`serial`，判断同一区块只看 serial；缺 section 算空气；不完整区块不读不写。

## 3. merge：把 now 的改动搬到 new

```sh
./merge <new> <now> <old[,old...]> <x1> <y1> <z1> <x2> <y2> <z2> <chunkunit> [dims]
```

| 参数 | 说明 |
|---|---|
| `new` | 新版本空板，**输出**（会被改写） |
| `now` | 要升级的地图，只读 |
| `old` | 旧版本空板，逗号隔开多个；没有就写 `""` |
| 范围 | 两个角，谁大谁小都行，含两头 |
| `chunkunit=1` | x/z 对齐到区块边界，最终整块处理（包含给定范围） |
| `chunkunit=0` | 严格按给定范围；范围外只读不写（方块/计划刻/POI/实体按坐标过滤） |
| `dims` | 逗号隔开的维度名，默认 `minecraft:overworld` |

判断规则：

- 对每个方块，`now` 和任意一个 `old`、或者和 `new` 自己相同 → 保留 `new`；
- 否则算 `now` 改过 → 把 `now` 的方块（含方块实体）写到 `new`；
- 没有 `old` 就只和 `new` 比；
- `now` 不可读、`new` 不可写/没有区块（mca 坏、区块没生成或 `Status != full`）→ 这个区块跳过；
  某个 `old` 读不出来只忽略那一个；
- 比较包含方块实体：箱子内容、告示牌文字等变化也算改动，会跟着方块一起搬。

其它行为：

- `new` 缺 section 时补一段空 section（`block_states` 一条 air，`biomes` 从 `now` 同段/附近抄）；
- 计划刻、POI、实体原样追加到 `new`，允许和 `new` 原有记录重复；对不上/重复由 fixer 清理；
- 每个处理过的 `new` 区块，`InhabitedTime` 覆盖为这一趟从 `now` 搬过去的方块数（写空气也算一次）；
- 区块的读取/落盘走 `Level` 的缓存（merge 自己不维护区块缓存）；
- 不清理任何非法数据（重复 section 等原样保留，交给 fixer 的 `format_Chunk`）；
- 不清理其它垃圾，不动 `Heightmaps`/`BlockLight`/`SkyLight`/`isLightOn`；
- 只写 `new`，`now`/`old` 全程只读。

## 4. fixer：修一个地图的非法方块/非法数据

```sh
./fixer <path> <x1> <y1> <z1> <x2> <y2> <z2> <chunkunit> [dims]
```

顺序：

1. **物理**：用 `rules.so` 逐格判，撑不住的拆掉/改状态；不写 `InhabitedTime`。
   fixer 只把这一格和它周围 **6 个面**的方块信息交给 rules，不提供斜向/更远的读取；修改会就地连锁这 6 个方向。
   写方块时先删这一格的旧 `block_ticks`/`fluid_ticks`；新方块是水就补一条
   `fluid_ticks(i=minecraft:water,t=5,p=0)`（霜冰化水、含水方块拆成水都走这里）。
2. **区块自身修正**：
   - 按区块在文件里的放置位置重写 `xPos`/`zPos`（`yPos` 不管）；
   - 同一 `Y` 有多段 section 时只留列表第一段（其余删掉，不把重复段写回 Minecraft）；
   - `Status != minecraft:full`：删整个区块（即使范围只覆盖半块）；
   - 全空气区块、`InhabitedTime == 0` 区块：只有整块都在范围内才删；
   - 删区块时，它在 `poi/`、`entities/` 里的对应区块也一起删，避免留下指向空洞的记录。
3. **光照/高度**：选中区块删 `Heightmaps`；自己和周围 8 个区块删 `BlockLight`/`SkyLight`，
   并把 `isLightOn` 置 0。写不了的区块不操作。
4. **计划刻**：`block_ticks`/`fluid_ticks` 指向的方块和记录类型对不上（或区块已删）→ 删记录。
5. **POI**：`poi/` 记录指向的方块和 `type`/票数对不上 → 删记录；一条不剩就删 POI 区块/文件。
6. **实体**：扫选中区域的实体，UUID 重复时删后出现的那一个。
7. **MCA**：选中范围碰到的 `region/`、`poi/`、`entities/` 文件，表头全 0 的删文件；
   非空的执行 `format`（消除扇区空洞）。

数据访问：fixer 不实现自己的区块缓存；用 `Level::pin_Chunk` 钉住当前 3×3 区块（当前块 + 8 个邻居）
读完就 `unpin`，其余全部由 libnbtrw 的 LRU 缓存管理。

起始版本检查：

- `rules.so` 的 `minecraft_version` 必须等于存档 `level.dat` 的 `DataVersion`，否则退出；
- 规则插件的 `RULES_ABI`/`sizeof(Rules_Api)` 必须和 fixer 一致，否则退出；
  以前单独的 `blockporter_version` 已经去掉，兼容性只由 ABI 负责（不兼容的规则改动就加 ABI）。

fixer 只改区块里的这些字段：`xPos`/`zPos`、`sections`（只删同 `Y` 重复段和光照数据）、
`block_entities`、`block_ticks`、`fluid_ticks`、`Heightmaps`、`InhabitedTime`（只读来判删除）、
`isLightOn`、`block_states`、`BlockLight`、`SkyLight`；整区块删除、MCA 删除/整理不在此列。
POI/实体在各自目录里。

## 5. rules.so：规则插件

- `rules.hpp` 只有 API（`Blk_Info`、`Rules_Ctx`、`Rules_Api`、入口符号），规则表/名字池/`fill`
  的实现都在 `rules.cpp`；
- 规则按 minecraft.wiki（Java Edition 26.1.1，`DataVersion=4788`）的方块页面/Block tag 页写，
  不采用精简 wiki 或第三方博客；
- `Rules_Api` 里记录 `minecraft_version`（DataVersion）；接口兼容性由 `RULES_ABI` 和
  `sizeof(Rules_Api)` 管，不兼容的规则改动就加 ABI；
- rules 判一格时只能看 fixer 填好的六个面（`Rules_Ctx::nb[6]`），没有读其它位置的接口；
- 规则只能“越判越少”（拆方块/关面/换更小状态），fixer 的收敛依赖这一条；
- 换规则：改 `rules.cpp` 后 `make rules`，或者把 `rules.so` 链到另一个同 ABI 的插件。

## 6. 测试

```sh
sh test/lib.sh                 # 216 个库用例 + 34 个 Level 用例
sh test/block.sh               # merge/fixer 合成世界回归（需要先 make blockporter）
```

`test/block.sh` 用 `test/data/lvlworld` 造 now/new/old1/old2 和 fixer 世界，覆盖：

- merge：多 old、`now==new`、`now==old`、改成空气、缺 section 补段、`InhabitedTime`、
  计划刻/POI/实体追加、方块实体；
- fixer：悬空方块、`Status!=full`、全空气、非空 `InhabitedTime==0`、光照/高度、计划刻/POI、
  实体 UUID、空 MCA/format、霜冰化水时的流体刻、坏 `xPos/zPos` 改写、同 Y 重复 section 清理。

旧测试 `test/run.sh`、`conv.sh`、`safe.sh`、`rules26.sh`、`test/mk*.cpp` 还按旧 `level`/`mca_Region`
API 写，暂时不能直接跑；`test/bad.cpp` 同理。`sh test/safe.sh asan` 也需要跟着新 API 改。

## 7. 注意

- 两个程序都是**就地改存档**，先在副本上跑。
- `fixer` 改过的地不要再跑 `merge`：`merge` 只按“now 和板子不一样”搬，会把 fixer 刚拆的
  水草/藤蔓原样搬回来。正确顺序始终是 `merge → fixer`。
- `merge`/`fixer` 没有头文件；`nbtrw.hpp` 是库的头文件，`rules.hpp` 是 rules API 的头文件。
- 真实地图只读测试请用副本；本仓库根目录的 `now/`、`new/`、`old/` 是测试素材，不要直接改。
