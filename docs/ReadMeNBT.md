# nbtrw（libnbtrw）库手册

`nbtrw` 是 blockporter 的底层库：NBT 树、方块/区块、MCA 区域文件、整个存档（`Level`）。
面向用户的 merge/fixer 操作说明在 [blockporter-README.md](blockporter-README.md)；
开发约定、现行规范和历史踩坑在 [ReadMeDeveloper.md](ReadMeDeveloper.md)。

| 部分 | 内容 | 产物 |
|---|---|---|
| **nbtrw / libnbtrw** | NBT、Block/Chunk、MCA、Level | `libnbtrw.so`，demo `nbtrw` |
| **blockporter** | merge / fixer / rules | `merge`、`fixer`、`rules.so.26.1.1` |

## 编译

```sh
make                 # libnbtrw.so + nbtrw（demo）
make blockporter     # rules.so.26.1.1（并链出 rules.so）+ merge + fixer
make rules           # 只重编规则插件
make clean
```

C++23，`-Wall -Wextra -pedantic`。程序用 `-Wl,-rpath,'$ORIGIN'` 找同目录的
`libnbtrw.so`；自己的程序包含 `nbtrw.hpp`，链接 `-lnbtrw -lz`。

## 基本对象

- **`nbt_Any`**：所有 NBT 值的基类，一字节 `type`，无虚函数。数组/字符串/compound
  分别用 `vector`/`string`/`unordered_map` 存；`data()` 返回深拷贝，`copy()` 深拷贝整棵树。
- **`Block`**：`id`（`nbt_String`）+ `properties`（`nbt_Compound`）+ `block_entities`
  （一个 `nbt_Compound`）。空 `id` 表示“读不到”，`minecraft:air` 才是空气。
- **`Chunk`**：继承 `nbt_Compound`；有只增不减的 `serial`（判断同一区块只看 serial）。
  不完整区块（`Status != minecraft:full`，或没有 `Status`）不读也不写；缺 section 算空气。
- **`MCA`**：一个 `.mca` 文件的内存镜像。文件不在是空镜像；文件存在但读不出来是
  `bad=true`。`format()` 按 8K 表头顺序压紧数据区。

## Chunk 读写

```cpp
Chunk *c = lv.get_Chunk("overworld", cx, cz);   // 没有返回 NULL
Block b = c->get_Block(lx, y, lz);              // lx/lz 0..15，y 是世界绝对 Y
bool ok = c->set_Block(b, lx, y, lz);           // 失败不改区块
```

`set_Block` 先在原位预检调色板、`data`、方块实体表，全部能写才动方块状态；不会
“调色板写了一半、方块实体失败返回 false”。没有对应 section、`xPos/zPos` 不是 Int
且要写方块实体、`block_entities` 形状坏时直接失败，不改区块。成功后 `block_states`
指针保持稳定，`Chunk_Cursor` 里的调色板/data 缓存会按需重查。

`Chunk_Cursor` 是把“段号→section→palette→data→方块实体”记住的小缓存；同一区块连续
读写时用它少扫几次列表、少做哈希。`format_Chunk()` 是 fixer 用的整理函数：Y 缺失/不是
Byte 的段整段删、同一 `Y` 的多段只留最前面一段、`block_states.palette` 里没被 `data`
引用的条目压紧并重写全部 4096 个下标。返回删掉的段数（`sections` 不是 List 返回 `-1`）；
后两个可选出参分别报“Y坏删掉的段数”和“压过调色板的段数”。merge 不调用它。读写接口
本身永远取第一个匹配 `Y` 的段，不自动删段。

用库接口改区块（`set_Block`/`format_Chunk`/`set_BlockEntity` 等）会让旧游标自动重查；
如果绕过接口直接改 NBT 树（尤其是整个替换 `sections`/`block_states`），改完要
`cur.reset()`，否则游标里的指针和缓存可能过期。需要逐格比较时不要先造两个 `Block`：

```cpp
Block_Ref a, b;
if(c1->get_BlockRef(x, y, z, &a, &cur1) &&
   c2->get_BlockRef(x, y, z, &b, &cur2)) {
    bool same = block_Ref_Equal(a, b);   // Name/Properties/方块实体，语义同 get_Block
}
```

`Block_Ref` 只指在树里，不能跨 `set_Block` 或缓存淘汰保存；`block_Ref_Equal(ref, Block)`
可以和已有的 `Block` 比。`get_BlockRef` 返回 false 表示这一格读不出来（不完整区块/坏
调色板/坏下标），空气是成功且 `state==NULL`（或指向 `minecraft:air` 调色板条目）。

`check_Chunk(c)` 是严格检查：NBT 成员类型/必需结构、Y 必须是 Byte、每条计划刻六样俱全；
`check_Chunk_Base(c)` 只查区块级结构（sections/block_states/palette/data/方块实体表、
刻表成员类型），不查单条刻和 Y。merge 用严格版，遇到坏就整块跳过；fixer 用 Base 版，
再用 `format_Chunk`/`cleanup_Ticks` 按最小单位删。它不判断“这格方块合不合理”，缺 section/
纯光段/缺 `Status` 仍是正常数据。

## 计划刻与 POI 辅助

- `count_ChunkTicks(chunk, fluid)`、`get_ChunkTick()`、`del_ChunkTick()`、
  `add_ChunkTick()`、`filter_ChunkTick()`：`fluid=0` 是 `block_ticks`，`1` 是
  `fluid_ticks`；表形状不对返回 `-1`/`false`，调用者不要改。
- `is_POIShapeOk()`、`count_POI()`、`get_POIAt()`、`get_POI()`、`set_POI()`、
  `del_POI()`、`clean_POI()`：POI 区块是普通 compound，按 `Sections`/`Records` 存。
  这些函数只管结构，不决定“哪个方块该有什么 POI”；方块→默认 POI 表在
  `rules.cpp` 的 `Poi_Info`。

## Level：一个存档，不绑定维度

`Level` 对应 level.dat 下的所有维度；构造只给存档目录，每个操作带维度参数。

```cpp
Level lv("path/to/save", 64, 8, 16);   // path, chunk缓存, region缓存, dat缓存
lv.get_Block("overworld", x, y, z);
lv.set_Block("the_nether", x, y, z, block);
lv.get_Chunk("minecraft:the_end", cx, cz);
lv.get_MCA("DIM-1", rx, rz);          // 非原版维度名：当相对存档根的维度目录
lv.get_Dat("level.dat");              // dat 是存档级，不带维度
```

维度规则：

- 只认 `overworld`、`the_nether`、`the_end`，可带 `minecraft:` 前缀；按 `DataVersion`
  自动选新/老布局：`>= 4774` 用 `dimensions/minecraft/<name>/`，否则用
  `region/`、`DIM-1/`、`DIM1/`。
- 其它字符串当“相对存档根的维度目录”，例如 `DIM-1`、`dimensions/minecraft/custom`；
  自动拼 `/region`、`/entities`、`/poi`。不许绝对路径、`..`、空路径段。

`Level` 还提供：

- `get_DimensionBase()`：维度参数→相对目录（老布局主世界是空串），无效 `nullopt`；
- `get_DimensionPath(dim, kind)`：`kind` 是 `region`/`entities`/`poi`；
- `pin_Chunk()`/`unpin_Chunk()`：钉住区块，`unpin` 前不会被 LRU 淘汰；缓冲区全被
  钉住时 `get_Chunk`/`pin_Chunk` 返回 `NULL` 且 `miss_why=GET_CACHE`，不硬抢；
- `erase_Chunk()`：把区块从 MCA 表头删掉（fixer 用，本来没有也算成功）；
- `save()`/`close()` 返回 `bool`，库不打印错误、不 exit、不记全局计数。

当前区块缓存/区域缓存/dat 缓存都由 `Level` 管，merge/fixer 不自己实现缓存。
fixer 只额外用 `pin_Chunk` 钉住当前 3×3 工作集，读完 `unpin`。

## demo

```sh
./nbtrw nbt   <file.nbt>                       # 打印成 SNBT
./nbtrw nbtw  <file.nbt>                       # 读、编码、再解析，核对编码/深拷贝/比较（不回写）
./nbtrw dat   <level.dat>                      # 读 level.dat
./nbtrw mca   <目录> <rx> <rz> <cx> <cz> [w]   # 读区域里的一个区块；w 会另存 .mca_test.mca
./nbtrw chunk <file.nbt> <x> <y> <z> [方块名]  # 读单区块文件里的一格
./nbtrw level <存档目录>                       # 看 DataVersion、三个维度的区域目录、(0,0) 在不在
```

`level` 适合先确认目录认没认错。参数都要写全，没有默认路径/坐标；缺参数直接非 0 退出。

## 测试

```sh
sh test/lib.sh     # 269 个库用例 + 34 个 Level 用例
sh test/block.sh   # merge/fixer 合成世界回归（需要先 make blockporter）
```

`test/lib.cpp` 覆盖 NBT/Block/Chunk/MCA/Level、`format_Chunk`（重复 Y、Y 非 Byte、
废弃调色板）、`pin_Chunk`、`set_Block` 原子失败和旧游标、`Block_Ref` 零拷贝比较
（空气/空Properties/方块实体索引/结构版本失效）；`test/lvl.cpp` 覆盖 Level
维度/缓存。`test/block.sh` 用 `test/data/lvlworld` 造 now/new/old 和 fixer 世界，
覆盖 merge 搬运/新建 POI/实体文件、fixer 物理/最小单位删记录/Y 非 Byte 子区块/
POI/计划刻/水刻/实体/0 字节文件/format/规则中低优先级差异。

旧测试 `test/run.sh`、`conv.sh`、`safe.sh`、`rules26.sh`、`test/mk.cpp`、
`mkfix.cpp`、`mkpoi.cpp`、`mkprune.cpp`、`mkrule.cpp`、`bad.cpp`、`check*.py`
还在旧 API 上，暂时不能直接跑；`test/mkmerge.cpp`、`test/mkfix2.cpp` 和
`checkmerge.cpp`/`checkfix2.cpp` 是现行回归。

## 注意

- 程序都是**就地改存档**，先在副本上跑；顺序只能是 merge → fixer，且只走一趟。
- `nbtrw.hpp` 是库头文件，`rules.hpp` 是 rules 插件 API 头文件；merge/fixer 没有头文件。
- 真实地图只读测试请用副本；仓库根目录的 `now/`、`new/`、`old/` 是测试素材。
