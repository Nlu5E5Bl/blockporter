# nbtrw —— 技术细节 / 设计取舍 / 踩过的坑

这份是给改代码的人看的。用户手册（怎么编译、怎么用命令行）在 `ReadMe.md`。

> **当前状态（blockporter 规范这一轮）**：`Level` 改成“整个存档、不绑定维度”，所有读写操作带
> `dimension` 参数；`merge` 按“多 old + now + new”重写；`fixer` 增加区块清理、光照/高度删除、
> 计划刻/POI/实体校验和 MCA format；`rules.so` 只保留 `minecraft_version`，兼容性统一由 `RULES_ABI` 管；
> `prune` 已并进 fixer。**第二轮**（见 §6.15）又改了：fixer 不再有 `Fix_Store`，改用 `Level` 的
> `pin_Chunk`；`Chunk::format_Chunk` 处理重复 Y；fixer 按文件位置重写 `xPos`/`zPos`；写水补
> `fluid_ticks`；rules 只拿六个面；`rules.so` 改名为 `rules.so.26.1.1`。库的新分工见 §2。
> 下面 §3 之后的 fixer/merge 细节是**旧实现的历史记录**，当前行为以 `ReadMe.md`、
> `fixer.cpp`/`merge.cpp` 文件头和代码为准。

---

## 1. 目录与职责

| 文件 | 职责 | 产物 |
|---|---|---|
| `nbtrw.hpp` | **底层库的全部声明**（只有库，没有 merge/fixer/规则的东西） | —— |
| `nbt.cpp` | `nbt_Raw` 游标、`nbt_Any`（所有 tag 的父结构，也当泛型用）及 11 种 tag、`nbt_List`、`nbt_Compound`；深拷贝、比较、转换、dat（gzip/zlib/裸数据）读写 | `libnbtrw.so` |
| `block.cpp` | `Block`（`nbt_String id` + `nbt_Compound properties` + `nbt_Compound block_entities`，按值持有）、方块状态属性小工具 | 同上 |
| `chunk.cpp` | `Chunk`（一个区块，继承 `nbt_Compound`；不完整不读不写）、`Chunk_Cursor`、**section 调色板/子区块**、计划刻、**POI/兴趣点那一整套**（`get_POIType`/`is_POIShapeOk`/`get_POI`/`set_POI`/`del_POI`/`clean_POI`）和实体区块的形状检查（`check_Entity`） | 同上 |
| `mca.cpp` | `MCA`（一个 `.mca` 文件的内存镜像，方块/实体/POI 三种文件共用；取/写区块、`save`、`format` 稠密整理）+ zlib/gzip | 同上 |
| `level.cpp` | `Level`（**整个存档，不绑定维度**；按维度参数和 `DataVersion` 定目录、dat/mca/chunk 三级自动缓存、落盘、删区块） | 同上 |
| `rules.hpp` | 规则插件的接口（写插件的人看这个）；含版本字段，规则实现不写进头文件 | —— |
| `rules.cpp` | 规则实现：规则表、方块分类、26.1.1 的支持集、判定函数 | `rules.so.26.1.1` |
| `merge.cpp` | 搬方块 + 自己的 `main`（没有 `merge.hpp`，配置结构就在文件里） | `merge` |
| `fixer.cpp` | 修图主程序 + 自己的 `main`（没有 `fixer.hpp`） | `fixer` |
| `prune.cpp` | 已删除；删残块/空 MCA/format 并进 `fixer` | —— |
| `nbtrw.cpp` | 底层库的 demo | `nbtrw` |
| `Makefile` | 上面全部（默认只编库 + demo；`make blockporter` 编 rules/merge/fixer） | —— |

历史：`world_api.cpp` / `main.cpp` / `merge.hpp` / `fixer.hpp` 都已经拆掉或删掉了；
`merge.py` / `world_api.py` / `fixer.py` / `rule.py` 是最早的 Python 原型，留着当参照（**不是**当前实现）。

## 1.1 测试目录（`test/`）

不属于库、也不属于两个主程序的东西全在 `test/` 下，和库源码分开：

| 路径 | 是什么 | 进 git |
|---|---|---|
| `test/*.sh` | 入口：`lib.sh`（库级）、`block.sh`（merge/fixer 合成世界）；`run.sh`/`conv.sh`/`safe.sh`/`rules26.sh` 还按旧 API 写，待迁移 | 是 |
| `test/*.py` | 独立核对脚本（`check*.py`、`check_poi*.py`）+ 自己写的 NBT 解析器 `nbt.py` + `cmp_mca.py` + 兴趣点方块表 `poitab.py` | 是 |
| `test/lib.sh` + `test/lib.cpp`/`lvl.cpp` | **当前能跑的库级测试（216 + 34 个用例）**：nbt 接口/深拷贝/比较/转换、Block/Chunk、`format_Chunk`、Level 缓存/维度/删区块/`pin_Chunk` | 是 |
| `test/*.cpp` | `mkmerge`/`checkmerge`、`mkfix2`/`checkfix2` 是当前 blockporter 回归；`mk` `mkfix` `mkpoi` `mkrule` `bad` `pal` `props` 还按旧 API 写，待迁移 | 是 |
| `test/c/` | 移植时用的 C 参照（`nbt.c` `mca.c` `chunk.c` `dat.c` `nbtw.c`），**不改**；各自能单独编 | 是 |
| `test/data/` | 小样例数据（`level.dat` + `lvlworld`/`lvlold`/`lvlnodat` 小世界，demo 和测试用） | 是 |
| `test/work/` | 跑测试造出来的一切（测试世界、日志、编出来的辅助程序）+ 大块素材（`3.txt`、`r.0.0.mca`、下载的 wiki 原文与调研笔记） | **否**（`.gitignore` 整个挡掉） |

约定：测试脚本里 `S=test`、`T=test/work`，**造出来的东西只往 `$T` 里写**（所以 `rm -rf test/work` 就等于全清）。
`test/work/` 里那两块素材（一块真实区块、一个真实区域文件）是"有就跑、没有就跳过并说明"，
其它测试不依赖它们——理由见第 7 节。

---

## 2. 底层库的思路

### 2.1 一个虚函数都没有

`nbt_Any` 只有一个成员：`uint8_t type`，放在偏移 0；11 种 tag 都是它的派生结构，`type` 是唯一的多态标识。
（`type` 不再是 `const`：值语义要求 nbt 结构体可以拷贝赋值；约定只读、不改。）

```cpp
struct nbt_Any{uint8_t type;nbt_Any(uint8_t type):type(type){} bool free();void print(size_t depth)const;...};
struct nbt_Byte:nbt_Any{int8_t con;...};
struct nbt_Compound:nbt_Any{std::unordered_map<std::string,nbt_Any*> con;...};
struct Chunk:nbt_Compound{uint64_t serial=0;...};   /* serial 是 Chunk 的数据成员 */
static_assert(sizeof(nbt_Any)==1,"...");            /* 加了虚函数这个断言就炸 */
```

- 没有虚表 ⇒ `(nbt_Compound*)any`、`(nbt_Long_Array*)p` 这种强转永远成立，分派靠 `switch(type)`（`free/print/encode/is_Equal/get_ByteSize` 各一个）。
- `Chunk` 在 `nbt_Compound` 之外只加了一个 `uint64_t serial`（trivial）。`free()` 仍按 `nbt_Compound*` 删，
  `nbt_Compound` 给了忽略 size 的 `operator delete`；`Chunk` 以后也只能加 trivial 字段，别加需要析构的成员。
- 容器换成 STL（`unordered_map`/`vector`/`string`），所以内存布局和 C 版不同；强转只依赖"tag + `type` 在偏移 0"这一条。
- 代价：**写回去的字节顺序会变**（`unordered_map` 遍历序，NBT 无序、MC 照读）。比两个区块一不一样要用
  `is_Equal()`/`==`，别比字节。`get_ByteSize()` 一定不变。

### 2.2 游标（`Chunk_Cursor`）与 serial

连着按格子读同一个区块时，`sy → section → palette → data` 每次重查太贵。游标把这些记住，但它必须能发现
"我记的是不是同一个区块"：`free` 掉一个区块再 `new`，很容易拿到同一个地址。所以 `Chunk` 里放一个
全局递增的 `uint64_t serial`（初始 0，每新建/复制/读一个 Chunk 就 `++`，不回收），游标只比较
`serial==c->serial`，不比较地址；`sections` 列表长度、section 成员数变了也会自己重查。

### 2.3 调色板（方块状态怎么存）

一段 section 里方块状态是"调色板 + data（Long 数组）"：

```
bits = max(4, ⌈log2(palette_len)⌉);  per = 64/bits;  i = ly*256 + lz*16 + lx;  idx = (data[i/per] >> ((i%per)*bits)) & ((1<<bits)-1)
```

- 位宽能由调色板长度反推（MC 保存时总取最小位宽）。调色板可以超过 256 条；原版写出时最多 4096 条
  （一段 16³ 个方块），读的时候更大的也认。**没有 256 条上限**，`bits` 最大支持到 32。
- 没有 `data` 时：调色板只有一条 ⇒ 整段都是 `palette[0]`；长度 >1 却没有 data ⇒ 存档不对，按"读不出来"处理。
- 形状不对（比如 `palette` 不是 list、`data` 长度不够）按"读不出来"处理，**不能当空气**（当空气会连累邻居被误拆）。

### 2.4 nbt 结构体的值语义（本轮新规范）

- **存储**：数组用 `std::vector`、String 用 `std::string`、Compound 用 `std::unordered_map<string,nbt_Any*>` 实际存储；
  接口照着 STL 仿（`size/empty/begin/end/operator[]/at/push_back/insert/erase/clear/...`）。
  成员 `con` 是公开的：直接改它就是原位改，不额外拷贝。
- **`data()`**：返回对应 C++ 类型的**深拷贝**（`byte`→`int8_t`、`int_array`→`vector<int32_t>`、
  `list`→`vector<nbt_Any*>`、`compound`→`unordered_map<string,nbt_Any*>`），可以直接拿去构造新结构体。
  List/Compound 的 `data()` 返回的指针是新 copy，归调用者（`free_AnyVector`/`free_AnyMap` 可以整批释放）。
- **拷贝**：拷贝构造/赋值都是深拷贝（List/Compound 递归往下拷）；多态拷贝用 `nbt_Any::copy()`，
  返回堆上的新树，调用者 `free()`。从 C++ 类型构造 nbt 结构体也是全量深拷贝；右值容器则接管指针（不额外复制）。
- **比较**：同类型按 C++ 比较；整数跨精度先转高精度（byte→short→int→long）、float→double；
  数组、String 按字典序；List/Compound 顺序无关、只能比相等，`compare()` 返回
  `std::partial_ordering::unordered` 表示"没有大小"。`==`/`<` 这些直接用 C++ 运算符，
  内部是 `operator<=>` + `compare()`。
- **转换**：只提供低→高，不截断：byte/short/int→long、byte/short→int、byte→short、float→double、
  byte_array/int_array→long_array、byte_array→int_array。`can_Convert`/`convert` 是显式转换；
  `as_Byte/as_Short/as_Int/as_Long/as_Float/as_Double` 只做扩宽读取（例如对 Long 调 `as_Int()` 返回 0，不截断）。
- **List 类型统一**：空 List 的 `elem=0`，第一个元素进来时自动定；`push_back/insert/set` 遇到类型不符会
  拒绝并释放传进来的元素。从 `vector<nbt_Any*>` 构造时，类型不统一 → 空 List（`const&` 版本不接管传入指针；
  `&&` 版本接管，失败时全部释放）。
- **Block**：`nbt_String id + nbt_Compound properties + nbt_Compound block_entities` 按值持有；
  空 id＝读不到，`minecraft:air` 才是空气。`get_Block` 返回独立 Block，`set_Block` 深拷贝进区块，
  离开区块/换缓存都不会悬空。`Block==Block` 就是三个 nbt 结构逐个 `==`。

### 2.5 Chunk：不完整不读不写，缺 section=空气

- `Chunk::is_Full()` 只认 `Status=="minecraft:full"`；没有 `Status` 也算不完整（和 merge 的规矩一致）。
  `get_Block` 对不完整区块返回空 id 的 Block，`set_Block` 返回 false——残块不是玩家的东西，不读也不往里写。
- 某一段 section 根本不存在（或者存在但没有 `block_states`）读出来是**空气**（`id=minecraft:air` 的有效 Block）。
  这和旧版"读不到"不同：旧版 merge 会因此跳过、不删对方多出来的方块；新版按语义把空气当真空气。
- `set_Block` 不自动建 section：没有这一段就失败（biome/光照不是这里能补的）。`set_Block` 传进来的
  `Block::block_entities` 里原有的 `x/y/z` **一律丢弃**，按区块 `xPos/zPos` 和放置坐标重算；为空表示
  "这一格没有方块实体"（会删掉这一格旧的方块实体）。
- 允许手动新建 `MCA()`、`Chunk()`/`Chunk(cx,cz)`，也允许把这种 Chunk 写进 MCA；但 `set_Block` 只接受
  已生成（`Status=="minecraft:full"`）且已经有对应 section 的区块，没生成的区块不能直接写。
- `Chunk_Cursor` 还是内部加速用的：把 `sy→section→palette→data` 记一次；换区块/列表长度变了会自己重查。
  它和 Level 的缓存是两回事——游标只是"连着读同一个区块"的备忘。

### 2.6 MCA 与 Level

- `MCA` = 一个 `.mca` 文件的内存镜像：`MCA(path)` 读文件，`MCA()` 空区域（8KB 表头）。
  `get_Chunk(x,z)`/`set_Chunk(chunk,x,z)` 取写区块，`get_Compound/set_Compound` 给实体/POI 用，
  `save(path)` 落盘（先写 `.tmp` 再 rename），`format()` 整理。
- **`format()` 整理**：按 8K 头索引顺序（索引 = x + z*32，也就是 0,0 < 1,0 < … < 31,0 < 0,1 < … < 31,31）
  把每个区块的原始压缩数据依次放到 sector 2 开始的位置，消除空洞、丢掉多余的尾部 padding，
  并重写表头偏移/扇区数/时间戳。只改内存；坏表头/坏长度返回 0 且一个字节不动；幂等。
- `Level` 是**整个存档（Level=世界，所有维度）**，构造只给存档目录：`Level(path, cache_cnt, region_cnt, dat_cnt)`。
  每个读写操作都带 `dimension`：`get_Block("the_nether", x,y,z)`、`get_Chunk("minecraft:overworld", cx,cz)`。
  布局严格按 `level.dat` 的 `Data.DataVersion` 判（≥4774 用 `dimensions/minecraft/<dim>/region/`，
  否则 `region/`、`DIM-1/region/`、`DIM1/region/`）；读不到 `level.dat` 就 `dat_ok=0`，按老布局算。
- 维度参数：只认 `overworld`/`the_nether`/`the_end`（可带 `minecraft:`），其它字符串当相对存档根的
  维度目录，自动拼 `/region`、`/entities`、`/poi`；绝对路径、`..`、空段无效。`get_DimensionBase()` /
  `get_DimensionPath(dim,kind)` 是给 blockporter 用的解析入口；`region_dir` 这类“绑死维度”的字段已经删了。
- 三级缓存全在 `Level` 里自动管理：dat/mca/chunk，各自一个容量（构造参数），满了按 LRU 落盘再扔；
  chunk/MCA 缓存按“解析后的维度目录 + 坐标”分桶，不同维度同坐标不会串。
  `save()` 把脏 chunk→MCA、脏 MCA→文件、脏 dat→文件，`close()` 刷完再放掉全部缓存。
  调用者不用再传 `cache_id`、不用自己实现缓存。
- `erase_Chunk(dim,cx,cz)` 把区块从 MCA 表头删掉（缓存里的那份也扔掉），给 fixer 清残块用。
- 文件在但读不出来的 MCA 也会缓存（`bad=true`），`get_Chunk` 报 `GET_BAD_FILE`，`set_Block`/`format_MCA`
  都不碰它；坏文件绝不会被当成"空区域"写回去。读不到的 chunk 也会缓存一个负条目（`why=GET_NO_CHUNK`/
  `GET_BAD_CHUNK`），下次不用再翻盘。
- `Level::set_Chunk` 先看 `serial`：传的是缓存里同一个 Chunk 就只标脏，不会先 free 再拷；不同 serial
  才深拷贝并换新 serial。缓存容量不设上限（传多大就是多大）。
- 坐标：`y` 是世界绝对 Y；段号 `sy = y>=0 ? y/16 : (y-15)/16`（C++ 整除向 0 取整，负数必须这么写）；
  段自己的 `Y` 成员是带符号 Byte。区块/区域坐标用**向下取整**的除法（不依赖负数右移的实现定义行为）。
- `Level` 不把 `level.dat` 映射成结构体：只读 `Data.DataVersion` 和 `Version.Name`；要别的字段用
  `get_Dat("level.dat")`。`get_Dat/set_Dat` 的路径相对**存档根**（例如 `level.dat`、
  `dimensions/minecraft/overworld/data/minecraft/raids.dat`），不许绝对路径/`..`；读进来是什么压缩方式，
  写回去还是什么（新文件默认 gzip）。`set_Dat("level.dat", ...)` 改了 `DataVersion` **不会**重新算
  `new_layout`；要换布局就重新构造一个 `Level`。
- 返回的 `Chunk*`/`MCA*`/`nbt_Compound*` 都指在缓存里：下一次取别的对象、`save()`、`close()` 后可能失效，
  要留着就 `copy()`。

### 2.7 本轮规范（9 条）对照

1. **分工**：调色板/子区块属于区块级，放 `chunk.cpp`；`block.cpp` 只留 `Block` 和属性小工具。
2. **spacename**：`Block::nspace()` 改名 `spacename()`——正式的 `namespace` 是关键字，不能当函数名。
3. **区块身份**：`serial` 放进 `Chunk`（`uint64_t`，全局只增、不回收）；判断是不是同一个区块只比 serial，不比地址。
4. **不记账**：库里没有 `nbt_bad_*`、没有 `err_cnt`、不打印错误日志、不 `exit`；成功/失败都用返回值。
5. **方块实体坐标**：`set_Block`/`set_BlockEntity` 丢掉传入的 `x/y/z`，按区块 `xPos/zPos`+放置坐标重算。
6. **不用 int 当 bool**：判断/成功失败用 `bool`，三态用 enum，计数/错误码才留 `int`。
7. **手动新建**：`MCA()`、`Chunk()`/`Chunk(cx,cz)` 都能直接建；未生成（没有 `Status=full` 或没有 section）不能 `set_Block`。
8. **命名**：`<操作>_<操作对象>`，操作全小写，对象是缩写就全大写（`POI`/`MCA`/`NBT`）、是单词就首字母大写、
   是结构体就用结构体名；例如 `set_Block`、`clean_POI`、`format_MCA`、`set_BlockEntity`。
9. **不设上限**：NBT 递归深度、palette 条数、缓存槽数都不设人为上限；只保留文件格式本身的硬限制
   （2/4 字节长度、MCA 255 扇区、3 字节偏移等）。

---

## 3. fixer 的思路

> **历史说明**：§3~§6 描述的是 blockporter 旧实现和当时的踩坑记录，保留作参考。其中
> “NBT 256 层深度上限、`nbt_bad_*` 计数器、64MB/256MB 解压上限、`CACHE_MAX`、`err_cnt`”
> 等做法已被 §2.7 的新规范明确取代：库不再设这些人为上限、不再记账/打日志。

### 3.1 规则和宿主分开

```
fixer（宿主）                          rules.so（插件）
读存档、认区块、调色板建表             哪种方块要管（defs 表）
逐格问插件"这一格判成什么"             它靠什么才能活（rule_* 函数）
按结论拆掉 / 关面                      支撑三档、支持集、规则家族
改动就地连锁补判、落盘、统计           （只通过 Rules_Api 里的函数指针说话）
```

宿主 **不链接** 插件，是 `dlopen` 装的（先找 fixer 自己那个目录下的 `rules.so`，再找当前目录）。
所以换规则只要重编一个 `.so`。接口在 `rules.hpp`：`abi`/`size` 双向核对，对不上直接报错退出，不带病跑。
判定结果只有三种：留着（`FX_KEEP`）、拆掉（`FX_AIR`）、改状态（`FX_SET`，比如把藤蔓撑不住的面改成 `false`）。

### 3.2 规则只读 6 个面（＋紫颂的斜下方）⇒ 改一格补判 10 格

`rules.cpp` 里只走 6 个方向，规则要哪个方向才读哪个；唯一例外是**紫颂植株**：它还要读"水平邻居的下面"
（`nb_Below_Of`，`(x±1,y-1,z)` 和 `(x,y-1,z±1)`）。
所以"谁可能被这次改动影响"是两类格：6 个严格相邻 + 4 个"斜上方"（`(x±1,y+1,z)`、`(x,y+1,z±1)`——
这些格的"水平邻居的下面"正好是刚改的那一格）。`fixer.cpp` 的 `work_Push` 一次排这 10 格，就地补判（DFS），
不整段重扫。

> **加规则时最容易踩这条**：哪天加一条读别处的规则（更远的斜角、两格远、读整列…），`work_Push` 必须跟着改，
> 否则"扫完+队列判空=不动点"不成立（改动传不到那些格子上，得再扫一遍才发现）。
> **这条真踩过**：紫颂规则加了斜下方读法却忘了改 `work_Push`，实测有"中间那株靠别的方向还站得住、
> 改动传不过去"的反例（见 6.13 第 1 条）；`test/mkfix.cpp` 里那组"斜上方连锁"用例就是专门盯它的。

### 3.3 为什么必然收敛

规则只会做这几件事：① 拆方块（含水物换成水，其余换空气；重力方块也走这条）；② 把"撑不住的那几个 `true` 面"
改成 `false`；③ 霜冰换成水；④ 混凝土粉末碰到水换成混凝土（粉末在规则表里、混凝土不在，M 照样减 1）。
这是 `rules.hpp` 里写明的插件合约。

取 M = 范围内"要管的方块"个数 + 这些方块身上"属性为 true 的方向面"总数（非负整数）：

- 每拆掉一个方块，方块数必减 1（拆之前先看过它不是空气，所以绝不会"把空气拆成空气"）；
- 每关掉一个面，true 面数至少减 1（只收当前确实是 true 的面；一条都没有时返回"不动"）；
- 没有任何规则会放方块、也不会把 false 改成 true ⇒ M 单调不增，每次改动至少减 1 ⇒ 改动次数有限 ⇒ 队列一定会空。

顺序不影响结果：只会拆、只会把面关小，支撑单调变少，先拆哪个最后都是同一个不动点。

**"扫一遍就够"**：全量扫的时候每格都判过一次；之后每次改动，它可能影响到的格子（6 个严格相邻）都会被重判
（队列在扫的过程中持续清空，每扫完一个区块清一次）。所以"最后一次可能影响某格的改动"之后那一格一定被判过了
⇒ 扫完一遍 + 队列判空 = 不动点。**没有"复扫 N 遍"这种参数**：不需要，也不给。
测试里另跑一遍确认 0 改动（`test/conv.sh`、`run.sh` 的幂等检查）是**验证**，不是算法的一部分。

### 3.4 快在哪

- 方块按**名字**分类一次就记住（支撑档/支持集/规则号/是不是水……），扫的时候只比一个字节，不再现查 NBT；
- 一段 section 的"调色板下标 → 规则表下标"表建一次就留在区块缓存槽里，改完只补新加的那几条；
- 区块缓存槽直接存 `Chunk*`，并按坐标记住"这个坐标读不出来"（负缓存，别一遍遍重试——那正是最费时间的地方）；
- 正在扫的区块把自己那个槽**钉住**（`pin`），读邻居不会把它挤掉，手里的 `Fix_Sec*`/`Blk_Info*` 就一直有效；
- 规则要哪个方向的邻居才读哪个，读不出来就放弃这一格（`blind`，宁可漏拆）；
- 改动就地连锁，一趟到不动点。

参考数字（`now` 全量 23726 个区块，同一台机器）：早期"限遍数 + 脏段重扫"版本 10.2s / 连锁判 13.5 万次；
现在的"6 面队列 + 一趟"版本 4.6~5.1s / 连锁判 2.9 万次 / 内存约 250MB。
单个 1024 区块的区域：1.45s → 0.68s。插件化（函数指针调用）之后这些数字没变。

### 3.5 拆掉还是换成水

拆"含水物"（`waterlogged=true`，或者海草/海带/珊瑚这种泡在水里的）时写的是 **`minecraft:water` + `level=0`**，
不是空气：地图里的水是一整片，写空气会在水底留洞，还会让旁边靠这格水活着的东西白丢水源；
MC 自己拆含水方块也是留水。（`BF_WATER` 决定写水还是空气，`BF_WATER_SRC` 是"水源"，给青蛙卵那类规则用。）

**霜冰化成水**走的是另一条路：不是"拆"，而是 `FX_SET` 换一个方块种类（霜冰 → `minecraft:water[level=0]`，
见 `rules.cpp` 的 `RULE_FROST` 和 6.11）。这里有个坑：新状态得**现造**（`Name` + `Properties.level=0`），
不能拿 `self->copy()` 改属性——霜冰带着 `age` 属性，改出来会变成 `water[age=0,level=0]` 这种非法状态。

---

## 4. merge 的思路

- 只在"now 有区块"的地方干活；now/new 任一读不到就跳过。
- 每个区块开头顶算一遍"哪些 section 两边都是整段同一个方块"（大片空气、整段石头），这些整段跳（`section_Const`）。
- 同一个"方块状态指针对儿"比过一次就记住（一列里几十个石头，比一次就够）；每个区块开始把备忘作废一代。
- `old` 可以给多个：`now` 里某格跟任一 `old` 一样，就说明早更新过，不用搬。
- 搬完一个区块：删 `isLightOn`、删 `Heightmaps`（光照和高度图算不了，让 MC 运行时自己算），
  `InhabitedTime` 写成这次搬了几格。
- `new` 缺 section 就当空气并补一段空 section（biome 抄 `now` 的）；只有光照没有 `block_states` 的段也算空气。
- 一个区域存一次盘。
- **顺序是 merge → fixer，修过的地别再 merge**：merge 只会"`now` 和板子不一样就搬过来"，它不知道哪边对。
  fixer 拆掉/换成水的方块在 `now` 里还在，再跑 merge 就原样搬回来（实测：拿修过的板子重 merge 一个区域，
  搬回 28243 处，全是 fixer 刚拆掉的水草——不是 bug，是这两个程序的职责边界，ReadMe 第 4 节也写了）。

### 4.1 实体（`entities/`）：为什么整块搬

1.17（20w45a）起实体从 `region/` 的区块里拆出来，一个区块一个实体区块（`entities/r.x.z.mca` 的一格），
结构是 `{DataVersion:Int, Position:[I;cx,cz], Entities:[…]}`。它和方块区块**不是一回事**：方块可以从
世界生成重新长出来，实体不行（村民的职业/交易/认领的床和工作站、宠物的忠诚、展示框里挂的东西……全在里面）。

所以按"范围内 `now` 有实体区块 且 `new` 有对应的方块区块"就**整块照抄**（`DataVersion` 也照抄，让游戏自己
DFU 升级），和"这个区块里的方块搬没搬"无关。**但 `old` 一个都没给时不搬**：那条路是"只统计、什么都不改"
（不给 old ⇒ `use.empty()` ⇒ 连实体/POI/空文件清理都跳过，见 4.4），早期版本实体那段写在 `use.empty()`
判断之前，于是"试跑"照样改 `new` 的实体、还删空的 `entities/` 文件——dry run 把输出板改了是最坑的：新版本的世界生成和旧版本不一样（实测两块板的 poi 记录
**重叠 0 条**），村民住的那片地皮在新图里是**另一套地形**，方块故意不搬，可村民还得跟着人走。
`Entities` 是**空表**的区块直接删掉（不写空表）；`now` 没有的区块保留 `new` 自己的。
抄之前 `entity_Check()` 核形状（`Position` 必须等于槽位、得有 `DataVersion`、`Entities` 得是 list）——
`Position` 和槽位对不上的话，实体在游戏里会散到别的区块去，宁可跳过并计数。

### 4.2 兴趣点（`poi/`）：为什么按位置求并、为什么不能标脏

`poi/` 是"方块索引"：村民认领床位/工作站、钟算不算村中心、传送门找出口，用的都是它。格式（wiki
`Point_of_Interest_format` + `Java_Edition_level_format` 的 poi format 一节，26.1.1 实测一致）：

```
{ DataVersion: Int,                       // 根下就这两个成员（另有一页wiki写成 {Data:{…}}，实测是根下）
  Sections: { "<段Y>": {                  // 键＝段Y的十进制字符串（可以是负数，"0" 就是 y 0~15）
      Valid: Byte,                        // 1；0＝"下次加载时把 Records 清空重建"
      Records: [ { type: String,          // 21 种之一，见下
                   pos: [I;x,y,z],        // 世界坐标；床只记 head 那半
                   free_tickets: Int } ] } } }
```

只有"有记录的段"才写键（实测原版新生成的区块一条空键都没有）；`free_tickets` 是"还剩几张票"，
0 表示这个 POI 不能被认领。**类型和票上限照 wiki `Point_of_Interest` 的表**（`chunk.cpp` 的 `poi_Defs[]`，
每行都注了依据）：13 个村民工作站 + 床(`home`) 是 1，钟(`meeting`) 是 32，
其余（蜂巢/蜂箱、避雷针、传送门、磁石、`test_instance`）恒 0。`*_bed` 只认 `part=head` 那半
（wiki 脚注："The coordinates for the head of the bed are saved as the Point of Interest"）。
**26.1.1 一共就这 21 种类型**，铜傀儡/铜箱子那些新东西不走 POI（wiki 三页都 0 命中，铜傀儡是"在
65×17×65 范围里搜箱子"），所以表里没有。

合并规则（用户定的，见 `ReadMe.md`）：候选＝`now` 的记录 ∪ `new` 的记录（同位置 `now` 赢），
逐条拿**合并完那一格的最终方块**去核：对得上就留（票照抄来源），对不上／认不出／表里没有就删这条。
不合成（不给"有方块没记录"的位置补记录），不写 `Valid=0`，段里记录空了就把段键删掉，区块里一条不剩就
把区块从 mca 里删掉，文件空了删文件。

**为什么不用 `Valid=0`"标脏让游戏重算"**（这是最容易想到的做法，但会是事故）：

- `Valid=0` ＝"下次加载这个区块时把 Records 全清掉重建"。重建出来的记录**票全是满的**：
  原来 143 条被认领的记录（`server` 那张图实测）会全部变成"可认领"，
  而 wiki 明说"**A job site block can be claimed only if it is unclaimed**" ⇒ 没交易过的失业村民
  48 格球内一搜，就会把别人认领的工作站**抢走**（村民那边 brain 里的记忆还指着老位置，两边对不上）。
- 而且"重建"依赖区块被加载：不加载就一直是空的，村民没工作、没床睡。
- 我们只删"确实对不上"的那几条，票原样保留，认领关系就不会动。实测合并后同位置票不一致率 **0**。

**为什么"最终方块"要以合并后的 `new` 为准**：因为 `now` 的方块**不一定被搬**（`now` 和 `new` 一样、
或者 `now` 和某个 `old` 一样就跳过）。POI 记录跟的是"这块地皮最后长什么样"，不是"原来长什么样"：
比如 `now` 里有一张床、新版本世界生成在同位置也是同一张床，方块不用搬，记录留着就对；
反过来 `now` 的床搬到新图别的位置去了，那这条记录得跟着新方块走（在新区块里被 `now` 那份记录覆盖）。

### 4.3 落盘顺序与空文件

- 一个区域里：**先 `dst.save()` 落方块，再写 `entities/`，最后写 `poi/`**。中途失败/断电时宁可
  "方块在、索引指着旧的"（下一趟 merge 会照方块把索引修对），也别"索引说有、方块没有"。
- `erase_Chunk()` 只清表头（偏移/扇区数/时间戳清 0），数据区留成空洞：不重排文件，省得动别的区块。
- 表头一格都没有了 ＝ 这个文件等于不存在 ⇒ `remove()`。
- 游戏自己会留"0 字节"和"正好 8KB 头、一格区块都没有"的区域文件（生成时建了文件没写内容），
  merge 顺手清掉——**只清输出（`new`）的 `entities/`、`poi/` 目录**，`now`/`old` 一个字不碰。
  条件卡得很死：名字必须正好是 `r.<十进制>.<十进制>.mca`，0 字节的删，正好 8192 字节且表头全 0 的删，
  **比 8192 大但表头全空的只打一行提醒不删**（数据扇区可能还在，属于"表头被清零"的可救文件）。

### 4.4 "宁可不动"的几条（都会被报出来，退出码非 0）

- **`xPos`/`zPos` 和文件位置对不上**：`merge` 仍然整块跳过（方块/实体/POI 全不动），因为搬运时
  方块是按文件位置读写的（自洽），但方块实体的查找用的是区块自己的 `xPos`（`chunk.cpp` 的
  `get_Block`/`set_BlockEntity`）：两套坐标一打架，就会在**错的位置**搬/删实体。
  **fixer 的处理已经改了**：它按文件里的放置位置直接重写 `xPos`/`zPos`（`yPos` 不管），不再跳过；
  见 §6.15。
- **区域文件存在但读不出来**（权限/IO/截断/名字撞上目录）→ `MCA::bad=1`：不写、不删、报错、退出码非 0。
  根因是 `file_Read_All` 对"打不开"和"文件不在"返回同一个 0，`MCA` 构造函数以前也不看返回值——
  于是"读失败"和"没有这个文件"完全同形，`data.size()<8192` 那一句就把镜像换成空的、`write_File` 一 rename
  就把**没读到的区块**整片抹掉（实测：`new/entities/r.0.0.mca` 里一个 `now` 没有的区块就这么消失了，
  报告"失败 0 个、读不出来 0 个"、退出码 0）。现在 `MCA` 构造函数里多一次 `stat`：路径存在 ⇒ 坏文件。
  0 字节不算坏（游戏自己会留这种，要能往里写）；`0 < size < 8192` 算坏（表头被截断）。
- **区域文件长度不是 4096 的整数倍**（拷贝中断/被截短/别的工具写的）→ 追加写之前先把起点对齐到 sector。
  表头存的是"第几个 4096 字节"，直接按 `data.size()` 追加会让它记成向下取整的位置（指到新数据前面），
  刚写的区块立刻读不回来、`write_ChunkAt` 还返回 1（实测：截到 15500 字节的文件，写成功后重开是空的，
  `err_cnt`、`nbt_bad_*` 全是 0）。补几个 0 对齐，最多浪费 4095 字节。
- **表头说"这里有区块"但偏移指到文件外 / 长度字段不合理** → 计 `nbt_bad_chunk`。以前这两种是静默 `return 0`
  （看着就像"这格本来没区块"），区块凭空消失、报告 0 坏块、退出码 0。
- **多维度跑时失败要"粘住"**：`merge_World`/`fix_World` 以前是 `ret=-1` 之后还 `ret+=n`，
  坏维度在前、好维度在后（好维度只要有 1 个区块/1 次改动）就把失败加成非负数，退出码变 0。
  现在用一个独立的 `fail` 标记。

### 4.5 "跳过"是一个概念：五种原因 + `InhabitedTime` + 不新建

用户跑完真图给的规矩：`now` 的区块读不到就**整块跳过**，而且"跳过"要当成**一类事**对待——分类报数、
`new` 里没有的不新建、`InhabitedTime` 清 0。落到代码上：

- `Level::get_chunk` 新增一个成员 `miss_why`（`GET_NO_FILE`/`GET_NO_CHUNK`/`GET_BAD_FILE`/`GET_BAD_CHUNK`）：
  `get_Region` 拿到 NULL ＝ 区域文件不在；镜像 `bad` ＝ 文件在但读不出来；`read_Chunk` 返回 NULL 时再看
  `has_Chunk` 分辨"表头那格本来就是空的"和"数据坏了"。**不加参数、不新增函数**，就一个成员变量，
  注释里写明"只有紧接着的那次调用有效"（merge 读完立刻取）。
- 第四类 `Status != minecraft:full` 在 merge 里判（`chunk_Full_Status`）。没有 `Status` 字段（`-1`）
  也算"不是 full"，**单独计一类**报出来：真存档实测（`server/` 主世界+下界各一个区域，4810 个区块）
  "没有 Status"的一个都没有，而 `structure_starts`/`biomes`/`carvers`/`initialize_light` 这些半成品
  成百上千——第四类才是真正常见的那种残块。
- **"空壳"**（`Status=full` 但一个方块都没有，`chunk_No_Block`）继续留着：它不是"没生成完"，
  但搬过去就是把 `new` 的地形写成空气。
- **`new` 里没有这个区块 ⇒ 不新建**。以前 `dst.get_Chunk` 只在 `now` 有时才调（顺带做到了）；现在每个区块
  都要读 `new`（见下一条），所以这条得**明写**：读不到就跳过，`mark_Changed`/`flush_Chunk` 只认
  真从盘上读出来的区块，`new` 的空洞不会被"顺手补上"。
- **`InhabitedTime` 严格按"这一趟搬过去的方块数"写**：合并的区块 `set_Long(opcnt)`（老行为，真图反推验证过，
  见 6.11）；**跳过的区块清 0**（新行为）。两个限制：① 只有**本来不是 0** 才写盘（第二趟跑不会平白重写
  一遍区块）；② `dry`（没给 old）那趟一个字都不改，连这个 0 也不清——"只统计"的承诺优先。
- **代价**（ReadMe 里写明了）：为了知道 `new` 哪些区块的 `InhabitedTime` 不是 0，merge 必须把范围里 `new` 的
  区块**都读出来**（哪怕 `now` 里压根没这块地）。板子越大、"now 覆盖得越少"，这一趟就越慢；
  换来的是"板子上不留下进游戏看过的脏 tick"。
- 顺带把跳过的计数从 2 个（残块/空壳）扩到 10 个（`merge_Dim` 里的 `sk_*`），报告一行打完；
  老的 `skip_raw`/`skip_raw_part`/`skip_raw_empty` 合并掉了。

### 4.6 测试上的两条（这轮回归才暴露）

- `run.sh` 从来没编译 `test/work/mkprune`，一直靠 `safe.sh`（或手工）编出来的那个二进制：
  干净的 `test/work/` 上跑到 prune 那轮会直接"文件不存在"。现在编译段里补上了。
- 合成世界的区块**必须带 `Status=minecraft:full`**（真实区块都有）。`mkpoi.cpp` 以前没写，
  于是"跳过 Status != full"一上线，POI/实体那一轮 23 条断言全挂；`mkfix.cpp` 也补了
  （它同时喂 merge 的多维度那几轮）。判据不是"测试绿不绿"，而是真数据抽样：查了 4810 个真区块，
  `Status` 全在、四分之一是半成品——所以这条规则既不会误伤真图，又是真图里最常见的情形。

---

## 5. 26.1 的规则覆盖

规则依据全部取 `minecraft.wiki`（Java 版），每条在 `rules.cpp` 里都标了依据；抓下来的原文留在
`test/work/wiki/research/*.md`（`g1`~`g7` 是分组的调研笔记；`test/work/wiki/` 整个是本地草稿、不进 git，
里面是几百 MB 抓下来的 wiki 页面，看完可以整个删掉）。

- **只判 26.1**：26.1 新增方块只有 `golden_dandelion` 一个；26.2 的 cinnabar/sulfur 系列、
  26.3 的 poplar 木set / red_shrub / shelf_mushroom / straw_bed / concrete_stairs 一律不收。
- **只读 6 个面**，外加紫颂植株那一处"斜下方"（`nb_Below_Of`，见 3.2）。
- **支持集直接按 wiki 的标签展开**：`#supports_vegetation`、`#supports_dry_vegetation`、`#supports_sugar_cane`、
  `#supports_bamboo`、`#supports_cactus`、`#supports_lily_pad`、`#supports_cocoa`、`#supports_chorus_flower/plant`、
  `#supports_wither_rose`、`#supports_azalea`、`#supports_mangrove_propagule`、`#supports_crimson/warped_fungus/roots`、
  `#supports_nether_wart`、`#support_override_cactus_flower`、`#support_override_snow_layer`、
  `#cannot_support_snow_layer`、`#cannot_support_kelp/seagrass` 等等。
  一共 **28 个支持集**（`rules.cpp` 的 `set_List`，wiki 的 `#supports_*` 系列 + 几个 `#support_override_*`/`#cannot_support_*`）。
  这些集合**照原版 26.1.1 的标签 JSON 逐个对过**（`data/minecraft/tags/block/*.json`，方法见 6.13 第 5 条）：
  没有"标签里有、我们漏了"的成员；多出来的几项都在注释里写明了是"宁可放过"（比如 `pale_moss_block`）。
- **23 条规则**（`rules.cpp` 的 `RULE_*`，`rule_Name` 里是中文名）：
  不管 / 下面要有支持 / 两格高 / 藤蔓 / 六面附着 / 贴一面 / 上面要有支持 / 下面要顶面带边 / 吊挂 / 悬挂告示牌 /
  门 / 可可豆 / 瓜茎 / 滴水石锥 / 钟 / 脚手架 / 小垂滴叶 / 胎生苗 / 紫颂花 / 紫颂植株 / 雪层 / 霜冰化成水 / 重力方块。
  5 个家族（`group_Name_Tab`）：`below` `2x` `vine` `attach` `misc`（`RULE_FROST`/`RULE_FALLING` 落在 `misc` 里）。
- **支撑分三档、2/3 档按"面"判**（`sup_OK(d,b,dir)`，dir＝支撑块朝自己那一面）：整面（`SupportType.FULL`）、
  面中心（`CENTER`）、薄片。台阶/楼梯/活板门按半格分方向（上半只有顶面、下半只有底面，见 6.14）。
- **故意不处理的**（都写在 `fixer.cpp` 文件头）：珊瑚缺水变死珊瑚、树叶腐烂、
  普通冰融化（**霜冰是例外**：`RULE_FROST` 直接化成水，依据见 6.11）、甘蔗旁边的水、下界传送门（要整个门框）、
  蘑菇方块的六个面（那是贴图不是合法性）、
  `observer/piston/moving_piston`、以及 wiki **明确说"不需要支撑、能悬空"**（活板门/栅栏门/花盆/铁链/
  末地烛/床）或者**全页根本没提支撑要求**的那些（避雷针/压力板/梯子/蜡烛……——"没写"就按"不用支撑"
  放过，宁可漏拆；`rules.cpp` 里每条 RULE_NONE 都注了依据）。
- **兴趣点（POI）方块单独登记成"不管"**：13 类村民工作站 + 4 种炼药锅 + 蜂巢/蜂箱/磁石/测试方块/传送门，
  依据是 wiki 各页 Breaking/Usage 里**没有任何"支撑没了会掉"的措辞**（`test/work/wiki/research/g6_audit.md`
  的表 2/A.2 那批"无"）；砂轮有直接原文 "it doesn't need a supporting block."，床有
  "the bed remains in place if its supporting blocks are later removed"。整个 POI 家族里**只有钟**是
  wiki 明说会掉的（`rules.cpp` 的 RULE_BELL）。登记成 `RULE_NONE` 的行不会被判（`api_blk_Rule` 返回 -1），
  所以是**纯文档 + 反例**：`mkrule.cpp` 里 21 条"悬空的工作站/炼药锅必须留着"的用例盯着别误拆
  （误拆一个工作站 = 顺手删掉它那条 POI 记录 = 村民的职业/认领没了）。
- 26.1 时代 id 变化：`chain` → `iron_chain`；`copper_torch`、`copper_lantern`、`copper_chain`、
  `copper_door/trapdoor`、`*_lightning_rod` 这些都是 26.1（1.21.9）就有的。
- 规则用例（`test/mkrule.cpp`，239 条，每条标了 wiki 依据）跑一遍**全绿**，而且第二遍 0 改动。
  这套用例是"不缺不漏"的主要证据；`test/work/wiki/research/g6_audit.md` 里还有一份 157 个候选 id 的审计清单。

---

## 6. 踩过的坑

### 6.1 NBT / 区域文件格式

1. **List 的元素没有 tag 字节**；Compound 的成员是 `tag + 名字长度(2) + 名字 + payload`。按"每个元素都带 tag"读，整棵树会错位。
2. **`length` 字段包含压缩类型那 1 字节**（1=gzip / 2=zlib / 3=裸），少减一个字节就解压失败。
3. **编码顺序会变**（见 2.1）：所有对拍只能逐值比（`cmp_mca.py` 还得容忍文件长度不同——sector 摆放不同）。
   谁写"md5 一样才算过"的测试，会一直红。
4. **负 y 的整除**：段号必须是 `y>=0 ? y/16 : (y-15)/16`；段自己的 `Y` 是带符号 Byte。
5. **`xPos`/`zPos` 和文件位置对不上的区块**：读按 `xPos` 算世界坐标、写按 `x>>4` 找区块，两套坐标会打架
   （同一个区块在缓存里出现两份、落盘还互相覆盖）。`merge` 对这种区块整块跳过并计数；`fixer` 则按文件位置
   重写 `xPos`/`zPos`（§6.15），不再跳过。
6. **`Chunk` 不能加数据成员**（`static_assert`）。
7. **游标失效**：`free` 完再 `new` 很容易拿到同一个地址 ⇒ 用每区块唯一的 `serial`。
8. **`*/` 别写在 `//` 注释里**（比如 `Chunk*/Fix_Sec*`），会把注释提前结束，编译报一堆莫名其妙的错。踩过两次。
9. **区域表头的偏移是文件里存着的数**：文件被截短过、或者表头被人改坏时，它可能指到文件外。
   "原 sector 放得下就原地写"必须**连 `pos` 一起查**（`pos + count*4096 <= data.size()`），
   否则就是按坏表头往堆外 `memset`——这是实打实的越界写。现在不合规就改成"追加到文件末尾"。
10. **`fopen` 对目录也能成功**：存档里出现一个叫 `level.dat` 或者 `r.0.0.mca` 的**目录**时，
    `ftell` 会给出个天文数字，照着它 `resize` 直接 `std::bad_alloc` → `terminate`。
    现在统一走 `file_Read_All()`：`fstat` 认"是不是普通文件"，读回来多少算多少。
11. **坏文件里的长度字段是"攻击面"**：`Byte_Array`/`Int_Array`/`Long_Array`/`List` 的长度都来自文件，
    照着它 `resize`/`reserve` 会几个 G 地要内存。现在读之前先 `read_Need(需要的字节数)`，
    不够就记一笔并**放弃这一块**（返回 0，调用者不再 resize；合法文件里这些字节一定都在，对正常数据没影响）。
12. **列表/成员的类型也要查**：`sections` 的元素、`block_entities` 的元素、`xPos`/`block_states`/`palette`
    这些成员，坏文件里可以是别的 tag。硬转过去读就把别人的内存当自己的结构解释。
    现在一律"类型不对就当这个成员/元素不存在"（`as_Int` 之前先看 `type==0x03` 之类）。
13. **NBT 读取有递归深度上限**（`NBT_DEPTH_MAX=256`，`nbtrw.hpp` / `nbt.cpp` 的 `read_Compound`/`read_List`）：
    正常存档只有十几层，但"别人给的/坏掉的"文件里一层 Compound 只要 `0A 00 00` 三字节，
    几万层就能把 8MB 栈打穿（SIGSEGV，崩在写盘途中连报告都没有）。超限按坏数据处理：记一笔、
    这一块跳过（不 exit、不动存档），256 层对合法数据没有任何影响。
    这条是审查时补上的——原来写的是"故意不加"，理由是"只处理自己家的存档"，
    但 `nbtrw nbt/chunk/mca` 和 `merge <别人的.mca>` 都算外部输入，代价又只有十几行。

### 6.2 坏数据策略：跳过 + 计数（不是 fail-fast）

坏数据（区域文件/level.dat 被截短、压缩数据坏、tag 不认识、长度字段超界）碰到时的动作是：
**只跳过这一块**——那一个区块按"读不出来"处理、原样不动，整趟 merge/fixer 继续跑完，
最后在报告里写清楚跳过了几个（`有 N 个区块的数据坏了（NBT坏 x、压缩坏 y），按"读不出来"跳过了`），
退出码给 255 让人知道"这次有区块没处理到"。

实现方式：

- 库里两个计数器，声明在 `nbtrw.hpp`：`nbt_bad_nbt`（NBT 流本身坏了：长度越界、根 tag 不对、tag 不认识，
  在 `nbt.cpp` 里加）和 `nbt_bad_chunk`（区块压不开、压缩类型不认识，在 `mca.cpp` 里加）。
  详情最多打 10 条（`BAD_SHOW`），剩下的只计数——一个坏文件可能一大片，刷屏没有意义。
- 读的入口全部改成"给 NULL/空"：`nbt_Raw::fail` 一旦置上，`read_Root`/`read_Compound`/`read_List`
  就把半截树扔掉返回 NULL；`Chunk::read`、`MCA::get_Chunk`、`nbt_Compound::read/read_Dat`
  同理。调用者（`Level::get_chunk` → merge/fixer）本来就有一条"这个区块读不出来"的路，直接复用。
- `read_bytes` 读不够时会把目标**清零**再返回：上层还会继续跑几步（读到 0 长度自然收手），
  不清零就是拿着未初始化的长度/数值往下算。
- merge/fixer 每个维度开始时给计数器拍个快照，跑完看差值，把"跳过"和别的失败分开报
  （坏数据不等于"程序没跑完"，但确实有区块没处理，所以算失败）。
- **还留着 `exit(-1)` 的地方**只有"写出去的数据违反了本来就不该出现的不变量"：
  字符串/成员名超过 0xFFFF、`write_bytes` 越界、`free`/`print` 碰到不认识的 tag。
  这些不是外部数据能造出来的（文件里的名字长度是 2 字节），真出现就是程序自己的 bug，当场停更好查。

`test/bad.cpp` 里那 6 个新用例专门盯这条策略：坏数据必须给 NULL/0 **并且**计数，
不能退出；还有一个端到端用例（一个区域里放一个坏区块 + 一个好区块）：坏的跳过、好的照读、进程不退。

### 6.3 算法与收敛

14. **"最多循环 8 遍"是错的**：挂 16 个悬空藤蔓，每遍只拆得掉一层，8 遍停手之后同一个存档再跑还能改出东西。
    后来改成"跑到 0 改动"，再改成"改一格只补判 6 个严格相邻（DFS 栈）"——现在一趟就是不动点
    （老算法要跑 3 趟才到 0 改动）。
15. **斜向机制已经删掉**：rules 只拿六个面，fixer 的 `read_Info` 也只会填这六个方向；`rule_ChorusPlant`
    因此看不到"旁边那株的下面"，改成"旁边有紫颂植株就留"（漏拆，不误拆）。见 §6.15。
16. **队列里不能存指针**：现在队列只存坐标；处理某个区块时用 `Level::pin_Chunk` 把当前 3×3 钉住，
    区块树/调色板表在 `unpin` 之前不会被 LRU 释放。跨区块的坐标进入 `cross` 队列，换区块时重新 pin。
17. **缓存槽要够 3×3**：判一格最多碰"当前区块 + 8 个邻居 = 9 个区块"；fixer 用 `Level` 的 256 个槽，
    并且用 pin 保证这 9 个不被挤掉。旧 `Fix_Store` 的 256 个深拷贝槽已经删掉。
18. **脏段掩码不如按坐标补判**：早期用"标记脏 section，下一轮整段重扫"，一趟要判 13.5 万次；
    改成 6 面队列后是 2.9 万次。
19. **"空气拆空气"会转不完**：规则表要是把一个本来就是空气的位置也收进来，写下去等于没改，下一遍还会再来。
    `fix_At` 里专门有条兜底：本来就是空气就只记一笔 `noop` 不写。
20. **`blind` 路径的泄漏**：规则可能已经造好了新状态（藤蔓关面那种），这时邻居读不出来、结论作废，
    那个新状态必须自己释放——valgrind 抓出来的漏点（现在 ASan 的坏数据用例也盯着这条）。
21. **收敛证明要守住**：规则只能"把东西变少"。以后加一条"会放方块"或者"把 false 改成 true"的规则，
    这套证明就崩了（这条写进了 `rules.hpp` 的插件合约）。

### 6.4 参数与范围（防手滑）

22. **命令行只收位置参数**，范围必须 6 个数写全。以前那种 `key=value`（`passes=`、`dry_run=`、`report=`、
    `rules=`、`world=`……）全删了：能悄悄改变行为的东西越少越好，配置项都在程序里定死。
23. **`old_path` 给空串 = 什么都不合并**（不是"用默认值"）：判断条件是 `(now and new and old)`，
    一个 old 都没有就只能是"只统计"。测试里专门断言"new 一个字节都没写"。
24. **范围要卡上限**：坐标多打一个 0 就会去翻上百万个区域文件，看着像卡死。
    merge/fixer 都按"要碰多少个区域文件"卡一道（`1<<22`），超了直接不干。
25. **（旧接口，已删）`cache_id` 也要卡**：旧的 `level` 会按 `cache_id` 把缓存槽数组 `resize` 到 `cache_id+1`，
    给个 `INT_MAX` 就是几个 G 的空槽。新的 `Level` 用自动 LRU，不再有 `cache_id`，这类参数错误没有了；
    容量上限（`CACHE_MAX`/`REGION_CACHE_MAX`）仍然在。

### 6.5 规则（wiki 与现实）

26. **藤蔓**：wiki 说贴在"填满整格且挡住移动"的方块侧面。原型 `lists.py` 的 `vine_attachable` 把
    `farmland`/`dirt_path`/`barrel` 也算"能贴"，而我们把它们归成"有形状的"（侧面撑不住）⇒ 会**误关合法藤蔓的面**。
    单开一个判断放行（宁可放过）。
27. **蘑菇**：wiki 写的是"下面要整块不透明的方块"，不是"泥土"。一开始只认泥土类 ⇒ 石头上的合法蘑菇被误拆。
28. **蜡烛是 "small square"**（台阶/栅栏/墙/铁砧上都行），**铁轨/中继器/比较器/红石线是 "hollow square"**
    （堆肥桶/炼药锅/漏斗也算）。一开始按"整方块"处理，会误拆合法红石线路。
29. **仙人掌要"先查旁边不能是整方块"再看"能不能叠同类"**：顺序反了，靠叠同类先返回"合法"，旁边贴着实心方块也不管了。
30. **含水物拆掉要换水**（见 3.5）。睡莲下面的"水"要把含水方块/水草/海带都算上，但**排除珊瑚块**（它是实心、自己不含水）。
31. **紫颂花/紫颂植株**（末地那两种，规则里唯一要读**斜下方**那格的）：
    - 花：下面是紫颂植株或末地石；或者下面是空气 + 水平方向**正好一个**紫颂植株、**其余水平方向全是空气**
      （0 个没靠山，2 个以上原版也不长，旁边杵着别的方块原版照样碎）。
    - 植株：`CanSurvive` 是两段，**第二段后半句最要紧**——"下面不是紫颂植株/末地石时，如果水平挨着的某一株
      紫颂植株**它自己下面**是紫颂植株/末地石，那这格活着"（要读 `(x±1,y-1,z)`/`(x,y-1,z±1)` 这种斜下方，
      不在"六面"里）。紫颂树是"横着伸一格再往上长"的，横枝底下就是空气，全靠这半句撑着；
      漏了它，末地的树会被 fixer 拆掉一大半（见 6.12）。
    - 还有一段"水平挨着紫颂、上下又都不是空气 → 碎"：末地自然生成的地形里就有极少数这种格子
      （旁边另一株"种子"长出来的矮桩），原版要等下一次方块更新才拆；我们**默认不拆**（`CHORUS_PLANT_STRICT=0`，
      见 6.12），所以末地地形跑完是 0 改动。
32. **支撑三档不是拍脑袋**：原版"下面要有支撑"用的是"面中心被盖住"（所以栅栏、花盆、上半砖都算，第 2 档 `S_PART`），
    而"贴侧面"要求整面（第 3 档 `S_FULL`）。比原版宽松一点是故意的。
33. **雪层**：先查 `#cannot_support_snow_layer`（barrier/ice/packed_ice）和 `#support_override_snow_layer`
    （honey_block/mud/soul_soil），其余要"顶面完整"，而且**下面不能是冰**（`F_NO_ICE`，曾经漏判）。
34. **海泡菜（sea_pickle）**：要求"下面不是空气"就行（水也算），不是"下面要有支持"——按 `lev=1` 判会把它在水里全删掉。
35. **26.1 的 id 变化**：`chain` 现在是 `iron_chain`；`*_wall_torch` 这种模式匹配不到 `wall_torch`（没有下划线前缀），
    得单独列一条——这是用例跑出来的真实漏项。

### 6.6 内存、工程与拆分

36. **Python 校验脚本吃内存**：`check_fix.py`（已删）把区块全解析成对象，一个区块约 150MB，扫 1024 个区块能把机器吃到 80% 内存。
    现在的核对脚本只用小窗口（≤32 区块）。
37. **插件拆分**：宿主是 `dlopen` 装插件、**不链接**它，所以宿主不能直接调 `rules.so` 里的函数
    （链接期就找不到符号）。所有跨界调用只能走 `Rules_Api` 里的函数指针，或者在头文件里 `inline`。
38. **`Blk_Info` 里有 `std::string_view`** ⇒ 两边必须同一套编译器/标准库（g++ + libstdc++）；
    改结构体就要 `RULES_ABI + 1`，插件入口 `rules_Entry(abi,size)` 两边对不上就返回 NULL。
39. **`Blk_Info.base` 指在插件的名字池里** ⇒ 插件的 `reset()` 之后，之前发出去的 `Blk_Info` 全部作废，
    必须在地图里任何 `Blk_Info` 出现之前调（宿主就是这么调的）；名字池用 `std::deque<std::string>`，
    加元素时旧地址不变。
40. **`FX_SET` 造出来的新状态由宿主 `free`**，规则里不要自己省。
41. **`dlsym` 的返回值转函数指针**：`-pedantic` 下要写成 `*(void**)(&fn)=dlsym(...)`，直接 `(Fn)dlsym(...)` 会被警告。
42. **脚本正在跑的时候别改脚本**：`sh` 是边读边执行的，改一下行号就错位，会报"未预期的记号"。重跑即可。
43. **`/tmp` 在两次命令之间不保留**（这个开发环境）：测试的临时文件一律写在 `test/work/` 里。
44. **仓库根在父目录**：`git` 仓库是 `/home/zyh/mc/world_update`，`nbtrw/` 只是其中一块；
    地图数据（`now/ new/ old/` 等）写在 `.gitignore` 里，但历史上已经提交过一批，
    **`.gitignore` 对已跟踪文件无效**，要真去掉得 `git rm --cached`。
    （2026-10 已经这么做了：地图数据、`nbt` 那个老二进制、`.venv/` 全部 `git rm --cached`，
    磁盘上的文件一个没动。历史里的老 blob 还在，`.git` 有 791MB；要缩得重写历史，暂时不管。）
45. **"测试数据"也分两种**：小到可以进 git 的（`test/data/level.dat`、测试脚本和 C 参照代码）
    和"跑起来才用得上、体积又不小"的素材（`test/work/3.txt`、`test/work/r.0.0.mca`、
    下载的 wiki 原文）。后一种放 `test/work/`（整个目录不进 git），脚本按"有就跑、没有就跳过并说明"
    处理——所以从零 clone 下来也能跑，只是少几轮真数据测试。不要把大素材硬塞进 git：
    上一轮就是因为把 654MB 的地图提交进去了才要清理。

### 6.7 通读复查补的几条

后来把整套代码从头到尾又读了一遍，下面这些是那一轮查出来并补上的（编号接着上面排）：

45. **游标里"这一段有没有 data"会过期**：`block.cpp` 的 `get_BlockData` 拿 `data_len != palette长度`
    当"要不要重查 data"的判据。要是这一段原来没有 `data`（整段都是 `palette[0]`），写一个
    已经在 palette 里的状态时 palette 不会变长、`data` 却刚被建出来——判据不成立，之后接着读
    还是按"没有 data"算，**整段都返回 `palette[0]`**（读出来的方块是错的，虽然不会越界）。
    现在 `set_BlockData` 写完就把游标里的 `data`/`data_len` 作废，下次重查一遍。
46. **压缩炸弹**：区域文件里那个 `length` 只管"压缩后"多大，"解出来"多大是没谱的——1MB 的坏数据
    能解出好几个 G，`inflate_data` 里那个"不够就翻倍"会一路 `resize` 到被 OOM 杀掉。
    现在解压上限卡在 256MB（合法区块差着两个数量级），超了按坏数据报错退出。
47. **`write_Chunk` 是从 chunk 自己取坐标的**：`xPos`/`zPos` 不是 Int 时 `as_Int()` 返回 0，
    区块就被写进 `(0,0)` 那个槽，**覆盖掉别人的区块**。现在先认 `type==0x03`，不是就当写不了。
48. **写坏的 `.tmp` 要删掉**：`write_File` 失败（写不全 / `rename` 失败）时原来会把半截的
    `r.x.z.mca.tmp` 留在存档目录里。现在失败就 `remove()`。
49. **`file_Read_All` 失败时不能留着旧内容**：以前只在成功路径上 `out.clear()`，打不开 / 不是普通文件时
    调用者手里的 `out` 还是上一个文件的数据——复用同一个缓冲区的调用者会照着旧图干活。
    现在一进来就清空。
50. **Compound 成员名字也有 2 字节长度上限**：超了以前是静默截断，写出去的文件里名字是断的，
    而且 `byte_size()` 算的长度和真写下去的字节数对不上（尾部留一串 0）。现在和 String 一样报错退出。
51. **插件给一个"和现在一模一样"的新状态会转不完**：收敛证明靠"每次改动都让 M 至少减 1"，
    要是 `FX_SET` 给的状态和现在相同，写下去等于没改、队列还会继续长。现在宿主写之前 `cmp()` 一下，
    一样就当"不动"（记 `noop`）。插件返回不认识的动作值也当"不动"，并把它造出来的 `out_block` 释放掉。
52. **`Rules_Ctx` 的邻居槽要由 fixer 填满**：第二轮起 `Rules_Ctx` 删掉了 `read` 回调，只保留
    `nb[6]`/`got`/`bad`；`Rules_Ctx c;` 的指针/标志都有默认初值。fixer 必须把六个方向都填上
    （空气用 `info_Air`，读不出来置 `bad` 位），rules 不允许再去读第七格。
    （加默认成员初值不改布局，ABI 不用动）。
53. **y 范围也要卡**：x/z 有"最多碰多少个区域文件"那道闸，y 没有；y 多打几个 0 会让 merge 对着
    每个 (x,z) 白循环上亿层，`(sy+1)*16`、`(y0-15)/16` 这些算法在极端值上还会整数溢出。
    现在 merge/fixer 都要求 y 在 ±4096 以内（自定义维度最高也就是 y=2031）。
54. **`new_path` 就是 `now_path`**：merge 是一边读 now 一边写 new 的，同一个目录会把要升级的地图
    当场改坏。现在用 `stat` 比 `st_dev/st_ino`（软链接指到同一处也算），一样就直接不干；
    `old` 和 `new` 给成同一个目录也拒（old 必须只读）。
55. **测试用例表的尾部字段没初值**：`mkrule.cpp` 的 `Case` 尾部三个字段只有特殊用例才写，
    没初值时 `-Wextra` 会刷一屏 `-Wmissing-field-initializers`。给它们 `=NULL` 就干净了。
56. **坏数据原来是 fail-fast**（一个坏区块会中断整趟 merge/fixer）。现在改成"跳过这一块 + 计数 +
    报告里写清楚 + 退出码非 0"，实现和取舍见 6.2；`test/bad.cpp` 里加了 6 个用例盯这条
    （必须给 NULL/0 且计数，不能退出；还有一个"一个区域里坏的好的一起放"的端到端用例）。

---

### 6.8 实体/兴趣点那一轮复查补的几条

补实体和 POI 的时候又通读了一遍，下面这些是那一轮查出来并补上的（编号接着 56 排）：

57. **fixer 删 POI 记录时把世界坐标当成区域槽位用了**：`poi_Del_Apply` 里写的是 `dels[j].x & 31`，
    可表头要的是"区块在区域里的坐标"（0~31），世界坐标得先 `>>4`。结果**一条记录都删不到**
    （查的是别的槽），而且报告还照样打"删了 0 条"。是 `test/check_poifix.py` 第一次跑就喊出来的。
58. **删记录只改了内存里的树，没写回区域镜像**：`read_Compound()` 读出来的是一棵**新树**，
    在它上面 `poi_Del` 不影响 `mca.data`，`write_File()` 落盘还是"删之前"那份字节。
    更阴的是它**看起来像成功的**：文件真被删/被改了（那是 `erase_Chunk` 动表头的效果），
    只有"区块还在、只删了几条记录"这种情况才露馅。修法：删完 `write_ChunkAt(pc,lx,lz)` 写回。
59. **`erase_Chunk` 之后没置 `changed`**：内存里擦了表头、`chunk_del` 也加了 1，但 `if(changed)` 不成立，
    这个擦除就丢了盘、报告却已经报了数。这种"报了数但没落盘"的错误方向最危险：**数据没坏，但人以为好了**。
60. **`poi_Shape_Ok` 把"Sections 不是 compound"当成形状正确**（循环条件 `secs && …` 直接跳过、返回 1）：
    于是一个坏区块被当成"这块没有记录"，候选集合空了 ⇒ 把**对方（`new`）那些本来对得上的记录全删了**。
    现在形状检查要求：`Sections` 存在就必须是 compound、段键必须是十进制整数、每段必须是 compound、
    `Records` 存在就必须是 list；任一不满足 ⇒ 整块跳过 + 计数 + 退出码非 0（宁可留着旧数据让人看）。
    实测 6 张图 8426 个 POI 区块、12214 条记录，全部满足这些条件（所以这条不会误伤真数据）。
61. **`poi_Ticket_Max("minecraft:home")` 返回 0**：床是按 `*_bed` 后缀认的，**不在 `poi_Defs[]` 表里**，
    查表自然查不到 ⇒ "票数超上限"的自检把每张床都报了一遍（合成世界里的床用例抓的）。
    这也顺带把一个死代码问题解决了：`poi_Ticket_Max` 原来没人调用，现在 merge 用它做"票数超上限只提醒、
    不改数据"的自检——它同时是"我们表里类型对错了"的探针。
62. **`now` 里同一个位置有两条记录时不会收敛**：候选集合不去重 ⇒ `keep` 比 `new` 那边多一条 ⇒
    每次都判"有改动"、每次都重写这个区块（内容其实一样）。现在按位置去重（留一条、其余的当"删掉"计数）。
    合成世界里专门摆了一条重复记录盯这个（第二趟必须 0 改动、文件 md5 不变）。
63. **空文件清理的删除面太宽**：原来是"名字里含 `.mca` 就认"，而且"`>=8192` 且表头全 0"一律删——
    这会在**输出目录里做不可逆删除**，包括 `r.0.0.mca.bak` 这种、以及"表头被清零但数据扇区还能救"的文件。
    现在：精确匹配 `r.<十进制>.<十进制>.mca`；0 字节的删；正好 8192 且表头全 0 的删；更大的只提醒。
64. **这张表必须全，漏一个方块就是"删人家记录"**：第一版漏了 `fletching_table`（制箭台），
    结果是 4 条合法的 `minecraft:fletcher` 记录被判成"认不出"删掉。抓它的办法是**反向对拍**：
    走一遍区块里所有"按 wiki 该有记录"的方块，看是不是都有记录——这就是 `test/check_poi_scan.py`
    存在的理由（拿真数据检验硬编码表）。同理还有几个"名字像但不是一个东西"的地方要盯：
    床只认 `part=head`、蜂巢(`bee_nest`)和蜂箱(`beehive`)是两个类型、
    测试方块的 POI 类型叫 `minecraft:test_instance`（不是 `test_instance_block`）。
65. **段键和 `pos.y` 不一致时不会被归一化**（已知、无害，写在这里省得以后当成 bug 查）：
    判断"这个区块要不要重写"是按**位置+类型+票**比的，不含"记录是不是挂在正确的段键下"。
    所以如果一个区块里啥都没变、而它原来的段键就是错的（记录挂在 `"5"` 下但 pos.y 属于第 7 段），
    我们会认为"内容一样"、一个字不改。参与重建的区块则会按 `pos.y` 正确归档。
    影响仅限于"本来就已经坏了的键"没被顺手修好；真要修得加一条"段键 vs pos.y"的比较，代价是可能
    因为别人写的怪数据每次都重写——不值得。
66. **`poi_Section_Recs` 不判 NULL、`mca_Path` 会静默截断**：前者现在有了（虽然 `con` 里理论上不会有
    NULL）；后者路径超过 4096 字节时改成"给个空串 + 报一行"，让后面的 `fopen`/`remove` 干净地失败，
    总比拿截断出来的名字去读写（甚至删）别的文件强——多了 `remove()` 语义之后这条才真正要命。

### 6.9 "要跑真实任务"之前那一轮复查（两个只读子代理 + 我自己的真图对拍）

67. **大垂滴叶的叶子长在自己茎的顶上，规则里漏了这条 ⇒ 真图一张区域误删 1455 片叶子**（这轮最严重的一条）。
    `{"big_dripleaf",RULE_BELOW,set_big_dripleaf}` 只认"泥土/黏土那 11 种地面"，可是《Big Dripleaf》Usage
    原文是 "Big dripleaves consist of any number of non-solid stem blocks atop of each other and a single leaf
    as the uppermost block"，Growth 那段又说骨粉是"把叶子顶高一格、在下面补一根茎"——**叶子下面是自己那根
    `big_dripleaf_stem` 才是常态**。26.1.1 真图实测：一张区域 2123 片（不含含水的）+837 片（含水的）里，
    1469 片是"叶子坐在茎上"；跑一遍 fixer 会拆掉 1455 片叶子 + 37 根茎（连锁：底下的茎因为地面不在表里被拆，
    上面的茎和叶子跟着塌），占那次全部改动（1624）的 90%。抓它的办法不是"再看一遍代码"，而是：
    ① 报告里按规则打出**改掉的方块名**（`└ 改的是：…`，这轮加的）；② 拿真图副本跑一遍，用
    `fixdiff` 那种逐格对比看"改之前是什么、下面是什么"。修法：叶子单独一张集合
    （`set_big_dripleaf_leaf` ＝ 11 种地面 + `big_dripleaf_stem`），茎那条不变（茎叠茎靠 `F_SELF`、
    最下面那根靠地面集）。修完同一张区域只剩 185 次改动，且"地面∈集合"的正常成株**一株没动**。
    **教训**：多块结构的植物，"哪种方块能当谁的支撑"要按 wiki 的结构描述逐条对，不能只看"放置"那一句；
    漏一条就是成片误删，而且删完还"已收敛、退出码 0"。
68. **游标里"这个区块没有 sections"会一直陈旧**（`Chunk_Cursor::find_Section`）：merge 的 `make_Section`
    是"先补一段空 section、再回头读这一格"，而游标缓存的是 `secs==NULL`（查的时候确实还没有这个成员）+
    `secs_len==0`，两者都没变 ⇒ 命中缓存、返回 NULL ⇒ **这一整个区块后面每次 `get_Block` 都返回"这段不存在"**。
    实测：`new` 里那个区块没有 `sections` 成员时，merge 报"合并 1 个区块、0 次改动、失败 0 个"、退出码 0，
    新世界那块地**一个方块都没搬**（第二趟因为 `sections` 已经建出来了才正常搬——所以特别难发现）。
    修法：`secs==NULL` 时多查一次 hash 表（只在没有 sections 的区块上多这一次，正常路径不受影响）。
69. **`merge` 的"只统计"会写盘**：实体那段写在 `use.empty()` 之前 + 空文件清理无条件跑 ⇒ `old_path=""`
    那趟照样改 `new` 的实体、删空的 `entities/`、`poi/` 文件。文档和提示语都说"只统计、不改任何东西"，
    实测一个 `now` 里没有的实体区块整个文件被删掉了。现在 `dry` 时实体/POI/清理全跳过（run.sh 里有一轮
    专门对 region/entities/poi 三个目录做 md5 对比）。
70. **多维度跑时退出码会被掩盖**：`ret=-1` 之后继续 `ret+=n`，坏的维度在前、好的维度在后 ⇒ 退出码变 0。
    fixer/merge 都有这个问题（实测：`the_nether,overworld` 跑出来 255 是"运气"——nether 那次改动数正好是 0；
    nether 有改动时就是 0）。现在用独立的 `fail` 标记，run.sh 里两个程序各有一轮回归。
71. **（旧接口，已删）`level::get_Block`/`set_Block` 没夹负数 `cache_id`**（`get_Chunk`/`cursor`/`mark_Changed`
    都夹了）：旧实现里 `get_Chunk` 把 -1 夹成 0 并**成功返回区块**，然后 `&cache[-1].cur` 就是拿堆外内存当游标、
    `find_Section` 还往里写（子代理 ASan 实测 SEGV）。新的 `Level` 没有 `cache_id`，缓存条目由 `unordered_map`
    管，坐标先做向下取整的除法，不会再有负槽号。
72. **`y-15` 的有符号溢出**（`y>=0?y/16:(y-15)/16`，y 从 POI 记录的 `pos[1]` 来，文件里读的）：
    `y=INT32_MIN` 时 UB（UBSan 当场中止；`safe.sh` 用的就是 `-fno-sanitize-recover=undefined`），
    正常编译下会写出 `"Sections":{"134217727":…}` 这种怪段键。现在统一走 `section_Y()`（内部 64 位），
    `bad.cpp` 里有用例。顺带把 x/z 范围也卡了（±3 亿）：`cx*16+16`、`((x1>>4)+1)<<4` 在 `x1=INT32_MAX`
    时会溢出，而且"区域文件个数"那道检查拦不住单个区域里的天价坐标。
73. **（旧实现，已改）fixer 的连锁路径没做 `xPos`/`zPos` 检查**：旧版 `scan_Chunk` 有、`fix_Pos` 没有，
    会出现"跳过"的区块被邻居连锁改动。第二轮起 fixer 统一按文件位置重写 `xPos`/`zPos`，
    不再有"坐标不对就跳过"的分支；`Fix_Scan` 进入时就把当前块坐标修正。
    现在两处都查（`chunk_Warp()`）。
74. **同一区块里两段 Y 相同的 section ⇒ "判 A 段、写 B 段"**：初扫是按每一段建表逐段判的，写回走
    `Chunk::set_Block` → `find_Section`（只认第一个 Y 匹配的段），于是删掉一个**根本没判过**的方块
    （可能是石头/箱子），真正悬空的那个还留着，报告还记成一次成功改动。现在这种区块整块跳过 + 计数。
75. **fixer 读不出来的东西以前是"静默少判"**：direct 调色板段（>256 条）`cand=0` 直接 continue，
    data 缺/短的格子 `IDX_BAD` 也 continue，两者都不计数，报告照样打"已收敛"、退出码 0
    （同一个段当"邻居"读时反而会算进 `blind`——口径不一致）。现在分开计数：调色板读不出的段只报数
    （有意不读），data 缺/短的算坏数据、进退出码。
76. **fixer 的 FX_AIR 兜底只防"空气拆空气"**，没防"拆完写回的水和现在一样"：插件只要把 `water`（level=0）
    收进规则表返 FX_AIR，就会 water→water 反复入队，破坏"每次改动 M 至少减 1"的收敛证明。现在和 FX_SET
    那条一样先 `cmp` 一下（现有 rules.so 跑不到：`water` 不在 `defs` 里，但这是插件合约级的护栏）。
77. **fixer 删 POI 记录时"文件读不出来"被当"这块没有 POI 数据"**：方块拆了、记录留着指向空气，
    一个字都不报、退出码 0。现在 `mca.bad` 一票否决：报错 + 返回 -1（`err_cnt++` → 退出码非 0）。
    同一条路径上 `remove()` 失败以前也是静默的（空文件留在那儿没人知道），现在报错 + 非 0。
78. **`Chunk::set_BlockEntity` 会往"元素是 Byte"的坏 `block_entities` 里 push compound**：写出来的 list
    表头说元素是 Byte、内容却是 compound——本库读得回来，MC 按 `elem` 解析整块区块会失败。现在只认
    `elem==0x0a` 的列表（不然当成没这个成员、新建一个干净的）。
79. **`push_back` 只在列表为空时改 `elem`**（`nbt_List`）：这是 NBT 的表达能力问题（列表元素必须同类型），
    库不打算"修"它，而是靠调用方别往异类列表里塞——上面第 78 条就是一处，`sections`/`palette`/`POI Records`
    都是本程序自己新建的 list，不受影响。
80. **规则表的"支持集没登记"会整类清空**：`Fix_Def.set` 指向的数组如果没进 `set_List`，`set_bit` 就是 0，
    `sup_OK` 永远判不成立 ⇒ 这类方块全被当悬空拆掉（`def_Init` 只打印一行）。现在 `api_blk_Rule` 直接
    拒绝管这类方块（宁可不判）。
81. **方向属性缺失时以前会"猜着拆"**：`RULE_SIDE`/`RULE_COCOA`/`RULE_STEM` 拿不到 `facing`/`face` 时
    `sup` 是 NULL ⇒ `sup_OK` 假 ⇒ 拆掉。现在判断不了就 `FX_KEEP`（宁可漏拆；正常存档这些属性一定有，
    实测真图这一轮"贴一面"判了 410 格、改动 0）。
82. **`nbtrw` demo 的 `chunk2` 没判 NULL**（编码后解析失败会空指针）；`demo_mca` 里写的是当前目录的
    `.mca_test.mca`（测试用，无所谓）。demo 也不该崩。

---

### 6.10 残块（proto-chunk）："已存在就跳过" + `Status=full` 的空壳 = 永久空洞

真实事故：一张半径 5000 的板子，中间一圈方块进了游戏是空洞，而且**怎么加载都不补**。

* **现象与定位手法**（可复现）：
  * MCA Selector 的 `world_settings.json` 里 `tileMapBackground: BLACK` ⇒ 图上"**黑＝这一格渲染不出任何方块**"
    （空块，或者根本没有区域数据），灰＝有方块，深灰＝那个区域文件都不存在；
  * 按"1 区域 = 512 块"反算像素比例（事故那张图 37px/区域），把候选世界"一个方块都没有的区块"集合
    渲染成同比例的掩膜，和图上黑区算 IoU：
    `server.260930` 下界 **0.879**（覆盖 95% 的黑区）、`nbtrw/26.1.1` 下界 0.648、`nbtrw/1.20.4` 下界 0.000
    —— 一眼就知道是哪个世界的哪个维度，不用猜；
  * 逐块点名：`server.260930` 下界 (−40,0) 是 `Status=minecraft:structure_starts`、379 字节、
    **0 个方块**、生物群系还是默认的 `minecraft:plains`、`InhabitedTime=0`；(30,30) 停在 `carvers`；
    (−142,81) 是 `full` 但只有 4 段有方块。下界里这种 0 方块区块 3670 个（主世界 3567 个），
    形状就是图上那个空心方环＋左下那个小方块。
* **机制**：C2ME 会把"生成到一半"的 chunk（proto chunk）先落盘，服务器的生成被打断就留下这批半成品。
  之后在**那个世界上接着跑 Chunky**（`--reuse`／生成到已有世界的流程）时，Chunky 的判断是
  "盘上有这个区块＝已经生成过"⇒跳过；而板子里这些块又被写成 `Status=minecraft:full`
  （生成/存档升级那一步），游戏侧就再也不会补。两处"跳过"叠在一起 = 永久空洞。
* **修法**（两条一起，缺一条都会再犯）：
  1. `prune`：把"没生成完 / 一个方块都没有"的区块**删掉**（表头清 0）⇒ 这块地重新变成"没生成"，
     游戏或 Chunky 下次会按种子生成。判断"空壳"必须**翻 `data` 数方块**（`chunk_No_Block`），
     不能看调色板——空壳的调色板里还留着石头/netherrack，看着像有地形；
  2. `merge` 护栏：`now` 里这种残块**整块跳过**，绝不拿它去覆盖 `new` 的地形。不跳过的后果很具体：
     空区块的每一格在 `now` 里都读成空气，相对 `new`/`old` 又都"不一样"，merge 会照规矩把
     `new` 的方块**写成空气**（把新板戳出一圈洞），而这些块 `Status=full`，游戏永远不会补。
* **教训**：判断"这块地生成过没有"不能只看"区域文件里有没有这个区块"——
  要看 `Status`，而且要看**实际有没有方块**；板子要么在**空世界**里生成再把玩家区块 merge 进来，
  要么生成前先 `prune` 一遍。

### 6.11 真图跑测回来的四条（跳过语义、`InhabitedTime`、不新建、霜冰）

用户在真图（`nbtrw/server` → 板子 `26.1.1`，old 给 `1.20.4,1.21,1.21.3,1.21.5` 四个旧板）跑完之后点了四条，
逐条记下来（前三条是一套的，落在 4.5 里）：

1. **"跳过"要是一个概念**：原来 merge 的跳过是散的——`now` 没有区块是静默 `continue`（连数都不记）、
   坏数据混在"有坏数据"那行里、`Status != full` 单独一行、`new` 里没有是"顺带"跳过的。
   用户要的是"这四种都算'区块读不到'，一起报"。**教训**：用户嘴里的"跳过"是一个业务概念，
   不是代码里的某条 `continue`；报数要按用户的心智模型分组（现在是 `sk_*` 十个计数器一行打完）。
2. **`InhabitedTime` 是算出来的，不是抄来的**：合并＝这一趟搬过去的方块数、跳过＝0。
   真图反推验证：板子上 (0,0) 的 `InhabitedTime=840`，而"`now` 和所有 old 都不一样"的格子数是 841
   （板子上本来已经一样的有 1 格）——840 就是那一趟的 `opcnt`，(1,0)/(5,5)/(10,10) 分别是 445/375/237
   对 451/379/237，全都对得上。**老代码的 `set_Long(opcnt)` 一直是对的**，用户要补的是"跳过的那一侧"：
   板子被进游戏看过的地方会留下几百 tick 的脏值（(0,0) 那次是 840，看着就像"访问时长"，
   但其实是被 merge 重写过的 opcnt——两边数值巧合，别被带偏）。
   **教训**：想验证"某个值是不是我们写的"，最快是拿真数据反推（这里是数一遍"要搬的格子数"），
   而不是盯着代码猜；另外 `nbtrw/26.1.1` 的 dir mtime 比 `nbtrw/server` 早三天，
   说明板子是**更早那次的产物**，不是这次拷贝——时间戳能省掉一堆猜测。
3. **不许凭空造区块**：用户明确"new 中没有这个区块这不新建"。测试里专门留了一格
   （`mkprune` 变体3 里没有 (5,0)）：跑完它还得是"不在"，同时报告里要出现 `new里没这个区块 1（不新建）`。
4. **霜冰（`frosted_ice`）化成水**：用户的服务端 bug 会往存档里写一大片霜冰（本该早就化了）。
   wiki（原文留在 `test/work/wiki/Frosted_Ice.html`）：*"Frosted ice naturally melts fairly quickly …
   When the age reaches 4, the block turns into water"*，而且*"in places with low light levels …
   it never melts unless broken"*。所以规则是"见到就换成水源 `level=0`"，**不判光照**
   （我们读不到光照图，而且低光照下它本来就赖着不化）。实现上是 `FX_SET` 造一个新状态——
   坑在**不能拿 `self->copy()` 改属性**：霜冰带着 `age`，改出来会变成 `water[age=0,level=0]` 这种非法状态，
   必须现造 `{Name:water,Properties:{level:0}}`。收敛证明不受影响：霜冰在规则表里（S_FULL），
   水谁都不管，M 减 1——这一轮把"`FX_SET` 换种类"的条件写进了 `rules.hpp` 的合约。
   代价（ReadMe 里写了）：玩家穿冰霜行者靴子刚踩出来、还没化的霜冰也会被一起化掉。

**两个测试上的教训**（这轮跑回归才发现）：

- `run.sh` 从来没编译 `test/work/mkprune`，一直靠 `safe.sh`（或手工）编出来的那个二进制；
  干净的 `test/work/` 上跑到 prune 那轮会直接"文件不存在"。现在编译段里补上了。
- 合成世界的区块**必须带 `Status=minecraft:full`**（真实区块都有）。`mkpoi.cpp` 以前没写，
  于是"跳过 Status != full"一上线，POI/实体那一轮 23 条断言全挂（方块、实体、POI 全被跳过）；
  `mkfix.cpp` 也补了（它同时喂 merge 的多维度那几轮）。**判据不是"测试绿不绿"，而是真数据抽样**：
  这轮查了 4810 个真区块，`Status` 全在、四分之一是半成品——所以这条规则既不会误伤真图，
  又正好是真图里最常见的情形。

### 6.12 紫颂规则把末地的树拆掉一大半（2026-10-03）

用户在真图上跑完 fixer，贴回来两行：

```
fixer: [minecraft:the_end]   紫颂花                18253 / 18222
fixer: [minecraft:the_end]   紫颂植株             158791 / 119232
```

末地的紫颂花 99.8%、紫颂植株 75% 都被拆了。**这不是"服务端 bug 留下的脏方块"，是我们自己的规则写错了。**

**定位过程（顺序很重要，每一步都能单独定案）**：

1. **先把一块真区域跑一遍**（`test/work/cx/`，只读地复制 `26.1.1` 的 `r.2.3`）：11595 个紫颂方块里
   9597 个被拆。样例行一眼看出病根——被拆的第一批是"下=空气、上=chorus_plant"的格子，比如
   `chorus_plant[down=false,east=false,north=true,south=false,up=true,west=false]`：
   **它自己下面确实是空气，北边却挨着一株紫颂植株**。
2. **看树长什么样**：把那一块末地按 y 层打成字符画，紫颂树是"主干往上长、横着伸一格、再从那格往上长"的
   阶梯形状——**横枝（和它上面那截）底下天生就是空气**，靠的就是"旁边那株有根"撑着。
3. **拿原版预生成的世界当裁判**：`get_raw_map/work/26.1.1--2687470505943798884/world/`（`gen_raw_map.sh`
   用 Chunky 生成的裸图，同种子、同版本）里的同一块区域，紫颂方块**逐格逐状态完全一样**（11595 行 diff 为空）。
   也就是说这些"浮空"的紫颂是**原版自己生成的合法地形**，fixer 一个都不该动。
   这一步把"是不是用户存档被谁改坏了"这个问题一次性排除掉。
4. **找原版源码对答案**：`~/.gradle/caches/neoformruntime/intermediate_results/` 里有 NeoForm 反编译出来的
   **26.1.1** 源码（`mergeWithSources_*.jar`，`version.json` 里写着 `world_version: 4788`），
   抽 `ChorusPlantBlock.java` / `ChorusFlowerBlock.java` 出来看，`canSurvive` 是：

   ```java
   boolean blockAboveOrBelow = !level.getBlockState(pos.above()).isAir() && !belowState.isAir();
   for (Direction d : Direction.Plane.HORIZONTAL) {
       BlockState n = level.getBlockState(pos.relative(d));
       if (n.is(this)) {                                  // 水平挨着紫颂植株
           if (blockAboveOrBelow) return false;           // 上下都不空 → 碎
           BlockState nb = level.getBlockState(nPos.below());
           if (nb.is(this) || nb.is(SUPPORTS_CHORUS_PLANT)) return true;   // ← 我们漏的就是这句
       }
   }
   return belowState.is(this) || belowState.is(SUPPORTS_CHORUS_PLANT);
   ```

   我们原来的实现只写了最后一行（"下面要是末地石/紫颂植株"），另外把"水平挨着紫颂时上下要有一格空气"
   写成了一条独立的"碎"条件——**两句都提到了，却把"旁边那株有根 → 活着"整个漏掉**，于是所有横枝都被当"断根"
   拆掉；拆掉之后连锁（`work_Push` 补判六邻）把整棵树、包括树顶的花一起带走：这就是 119232 / 18222 的来源。
5. **改完再对一遍真数据**：`r.2.3` 从 9597 改动 → **0 改动**；整个末地（34315 个区块、165010 个要判的位置，
   原版裸图和有改动的板子各跑一遍）都是 **0 改动**，`r.2.3.mca` 的 md5 一个字节没变。

**改法**：`rule_ChorusPlant` 照 `canSurvive` 逐句搬，包括那个**斜下方**的读法（`nb_Below_Of`：读
`(x±1,y-1,z)` 或 `(x,y-1,z±1)`——它不在"六面懒读缓存"里，读不出来就置 `blind`，这一格索性不判）；
`rule_Chorus`（花）也补齐了原版"下面不是紫颂植株/末地石、也不是空气就碎"和"其余水平方向必须是空气"两句。

**一处故意偏离原版**：`canSurvive` 第一句（水平挨着紫颂 + 上下都不是空气 → 碎）在末地**自然生成的地形里
就有**——旁边另一株"种子"长出来的矮桩贴着主干，主干那一层上下都不空。原版要等下一次方块更新
（谁在旁边放/拆一格）才把它连着一截树干拆掉；我们跑 fixer 时若照做，等于替原版把树提前拆了，
用户看到的是"修完末地少了几棵树"。**实测**：照原版执行，这一块区域拆 171 格（含连锁；1024 个区块里
约 4 处触发点）；不执行，0 格。规则里留了开关 `CHORUS_PLANT_STRICT`（默认 0＝不拆，1＝严格照原版），
两种都编得过、都不影响收敛证明（拆的都是 `M` 减 1）。

**教训（可以推广的）**：

- **规则必须拿"原版自己生成的地形"过一遍**：fixer 的验收标准不只是"用例绿"，而是"裸图跑完 0 改动"。
  这一轮之后 `test/run.sh` 多了一轮：拿一块真末地（`test/work/end/r.2.3.mca`）跑 fixer，
  必须"共 0 次改动"且文件 md5 不变。**任何规则改动都该先在这块地上跑一遍。**
- **真图就是最好的规则文档**：wiki 那两条（Chorus Plant → Support）其实写清了"或者水平挨着的某一株
  紫颂植株在紫颂植株/末地石**上面**"，我们当初照 wiki 抄的时候漏了后半句；是先看真数据、
  再回去读 wiki，才读出那句的分量。
- **能拿到原版源码就别猜**：`neoformruntime` 的 `mergeWithSources_*.jar` 是现成的 26.1.1 反编译源码
  （各个方块类都在），比在 wiki 的散文里抠措辞靠谱得多；抽出来的
  `ChorusPlantBlock.java`/`ChorusFlowerBlock.java`/`ChorusPlantFeature.java` 留在 `test/work/mcsrc/`（不进 git）备查。
- **"改得多"本身就是报警**：fixer 报告里"按规则分"那行就是为这个准备的（`big_dripleaf` 那次也是靠它
  抓出来的）。一条规则在一张图上改动几万格，先当成 bug 查，别当成"这图真脏"。

### 6.13 紫颂修完之后那一轮审查（5 个子代理 + 我自己的真图复核，2026-10-03）

紫颂修完当天又开 5 个只读子代理分头审（内存/越界、规则正确性、工具层逻辑、测试有效性、约束与文档），
下面是**它们查出来、并且已经改掉**的东西（每条都写了"怎么保证以后不再犯"）：

1. **我自己刚犯的错：紫颂读了斜下方，`work_Push` 却还只排 6 个面。**
   这是 6.3 那条"加规则必须同时改 work_Push"的现成反例：改动发生在 q，而另一个紫颂植株 p 的合法性
   取决于"p 的水平邻居的下面是不是 q"——中间那株（q 正上方）如果靠别的方向还站得住，它自己不用改，
   改动就传不到 p 上，p 会留着；**再跑一遍 fixer 才拆得掉**（"扫完+队列判空=不动点"当场失效）。
   改法：`work_Push` 排 10 格（6 面 + 4 个"斜上方"）。`test/mkfix.cpp` 里加了一组专门的用例
   （q(16,5,20) 拆、h/m 留着、p(15,6,20) 该跟着拆，p 故意放在**先扫到的那个区块**里），
   `conv.sh` 逐格断言；拿旧版（只排 6 格）跑同一组用例：第一趟 16 改动、第二趟还有 1 改动，
   新版第一趟 17 改动、第二趟 0——用例确实抓得住这个漏法。
2. **`soul_sand` 被当成"有形状的"（S_PART），原版是整方块。** `SoulSandBlock` 特意把
   `getBlockSupportShape` 覆盖成 `Shapes.block()`（只有走路碰撞箱是 14/16），`getShape` 也没覆盖、
   默认就是整方块 ⇒ `isSolidRender()`=true、`isFaceSturdy(UP)`=true。我们判成 S_PART 的代价：
   下界 **15298 个蘑菇里 144 个长在灵魂沙上被误拆**（`MushroomBlock.mayPlaceOn = isSolidRender()`），
   顺带还有灵魂沙上的红石线/铁轨/墙火把/梯子/门/海草（`SeagrassBlock.mayPlaceOn` 要 `isFaceSturdy(UP)`）。
   改法：把它移出 `part_Is`（`mud` 是同一类方块，本来就没在里面——两个方块不一致本身就是线索），
   `mkrule` 补 5 条用例（蘑菇/火把/红石/铁轨/藤蔓贴灵魂沙）。
3. **`thin_Is` 的 `"*_lantern"` 把 `jack_o_lantern`/`sea_lantern` 一起吞了**（两个都是整方块）：
   南瓜灯/海晶灯上的火把会被误拆。改成 `"*_copper_lantern"`（真灯笼 `lantern`/`soul_lantern` 在 exact 里）。
   同一轮还补进了 `noCollision` 的植物（`bush`/`firefly_bush`/`cactus_flower`/`mangrove_propagule`/
   `pitcher_crop`/`torchflower_crop` + `#small_flowers` 那 17 种花）——它们以前被当成"整方块"，
   会让旁边的仙人掌被误拆（原版 `CactusBlock.canSurvive` 用 `neighbor.isSolid()`）。
   对 `forceSolidOff` 的 `chorus_plant`/`chorus_flower`/`azalea`/`flowering_azalea`/`snow` 则单独加了
   `solid_No()` 名单——它们**能当支撑**（support shape 是满的）但**不算实心**，一档 lev 表达不了两件事。
4. **8 层雪是整方块**（`SnowLayerBlock.getBlockSupportShape = SHAPES[layers]`，`layers==8` 就是满格），
   我们一律当薄片 ⇒ 8 层雪上的火把/铁轨/红石/另一层雪会被误拆。改法照双台阶那条：`Blk_Info::fill` 里
   对 `snow[layers=8]` 特判 `lev=S_FULL`。
5. **规则表和原版标签做了一次机器对拍**（这次审查最值钱的一招，以后改支持集都该跑）：
   从 26.1.1 的 jar 里抽 `data/minecraft/tags/block/*.json`（245 个，`test/work/tags261/`，不进 git），
   递归展开 `#` 引用，再拿 `Blocks.java` 的 1054 个方块名把 `rules.cpp` 里 28 个集合的 `*` 模式展开，
   逐个集合双向 diff。结论：**没有"标签里有、我们漏了"的成员**（唯一的"缺"是 `set_kelp` 对
   `cannot_support_kelp` 的 `magma_block`，那条由 `F_NO_MAGMA` 单独管）；多出来的是注释里写明的故意放宽
   （`pale_moss_block`、`farmland`/`dirt_path`/`barrel` 给藤蔓）。
6. **气泡柱少了"下面还是气泡柱"这一条**（原版 `canSurvive` 第一句）：整列的第二格起会被拆掉
   （而且它是含水方块，拆完留水）。改法：def 加 `F_SELF`，`mkrule` 补用例。
7. **几处"不够原版"但方向安全的小偏差**，这轮顺手改掉的：小垂滴叶泡水的泥土类该走
   `#supports_vegetation`（含耕地，不是 `#substrate_overworld`）；挂着的红树胎生苗上面必须是红树叶
   （原来写"是树叶就行"，`set_mangrove_leaves` 因此一直是死表）；大垂滴叶叶叠叶也算合法
   （原版 `canSurvive` 第一句 `below.is(this)`）；`set_dry_veg` 的 `"*terracotta"` 会把上釉陶瓦
   一起收进来（改成逐个列 17 种普通陶瓦）；海带/海草原来要求"周围有水"，原版 `canSurvive` 没这条
   （改成看"自己是不是水里的方块"）。
8. **测试体系本身查出来的洞（比代码 bug 更值得记）**：
   - `safe.sh` 里 8 处 ASan 用例是 `... | tail -N`，POSIX sh 没有 `pipefail`，**退出码被 tail 吃掉**。
     一改成检查退出码就发现：ASan 版 fixer **一直在报"装不上规则插件"**（`-shared rules.so` 里的
     `nbt_*` 符号没法从可执行文件里解析，缺 `-rdynamic`），也就是整套 ASan 那层一直是装饰。
     现在：`san_run` 落日志+判退出码（故意失败的用例用 `SAN_EXPECT_RC=255`），fixer 加 `-rdynamic`。
   - valgrind 那轮的 `vg_run` 只把 `rc==99`（valgrind 报错）当失败，**程序自己退出 255、或者崩了 139，
     都会被打印成 ok**；fd 统计那行缺失也当"没有泄露"。现在退出码必须等于 `VG_EXPECT_RC`（默认0）、
     缺 fd 统计直接失败。一收紧就抓出 `test/bad.cpp` 自己漏了三棵树（`ent_Make` 里没用上的
     `Int_Array`、`poi_RecordBad` 里 `which==0` 时白拼的 `r1`、`mca_Misaligned` 里两次"读一下看看"的
     返回值）——都是**测试代码**的泄露，库本身是干净的。
   - `check_rules.py` 把"读不出来"（`block_at` 返回 None）当成"不是空气"，约 100 条"该留"的断言
     在数据被写坏时会静默通过；现在 None 直接算不符，并且用例少于 200 条也报错（以前 0 条也打印 RULES OK）。
   - 真末地那轮（0 改动 + md5 不变）**没有正控**：维度名写错会静默退回主世界、目录不对会扫 0 个区块，
     两种情况都会打印"共 0 次改动"而全绿。现在断言"扫了 ≥100 个区块、判了 ≥1000 个位置"。
   - `run.sh`/`rules26.sh` 里三处"再跑一遍（应该0改动）"只 `grep` 打印、不断言，不收敛会被放过；现在都断言。
9. **其它加固**（都是"防手滑/防坏数据"级别）：命令行参数改 `arg_Int` 严格解析（非数字/超 int32 直接
   报错，以前静默变 0 会按另一个范围跑）；`merge` 的 `dims` 去重（写两遍会让第二趟把 `InhabitedTime`
   又清 0）；>`64MB 的"压缩块"直接判坏（`inflate_data` 的首次分配原来可能一次要几百 MB，
   `bad_alloc` 没人兜就是 terminate）；NBT 加 256 层深度上限；`sec_Idx` 下标越界算坏数据
   （以前初扫当"这格不用管"静默跳过，不计 pos_bad、退出码还是 0）；`xPos*16` 走 int64（坏数据下是 UB）；
   POI 的 `Records` 元素类型也要是 compound（不然会写出 MC 读不了的怪表）；fixer 收到 `FX_KEEP` 时
   也要释放插件造出来的状态；`merge` 里"old 里一个都没有"的区块按手册"方块/实体/兴趣点一个都不碰"
   （实体段原来排在这个判断前面，会把实体搬进一个没搬地形的区块）。
10. **真图复核（0 改动的判据之外，再加一层"逐条对原版"）**：把原版裸图跑 fixer 前后的全量方块快照
    `sort`+归并比较（`test/work/van_before.txt` / `van_after.txt`，只含11种要判的方块），
    分出"真拆掉 9941 个"和"只改状态 84 个"；再给每个被拆的位置读 6 邻（临时工具
    `test/work/probe_at.cpp`，跨区也能读），用原版的 `canSurvive`/标签逐条复核：
    藤蔓 5095 个里 3067 个"一个面都贴不住"、2019 个"顺着一串往上走、链顶也没得贴"（连锁塌），
    剩下 9 个邻居跨区读不到；发光地衣 1160 个同理（1114 + 46）；矮草/蕨/大垂滴叶/铁轨/落叶
    全部是"下面不在 `#supports_vegetation`/`#supports_big_dripleaf` 里、或者顶面不是整方块"。
    结论：**这 9941 个都是原版下一次方块更新也会拆的"亚稳态"世界生成产物**（最典型的是下界蘑菇长在
    灵魂沙上、大垂滴叶从水里长在深板岩上——原版自己的 `dripleaf` 配置特征只要求"目标格是空气或水"，
    根本不看底下是什么），不是我们的规则错。要不要连这些也留着，是个策略问题（见 6.13 末条）。

**这轮之后仍然**保持的"故意不原版"的地方（都写在代码注释里）：`CHORUS_PLANT_STRICT=0`（6.12）、
大垂滴叶茎不查"上面还得是茎或叶子"（只漏拆）、瓜茎没了果实我们是拆掉而原版是变回普通茎（更保守）、
仙人掌旁边是台阶/栅栏这类"原版认为实心"的方块时我们放过（漏拆）、藤蔓 up 面/六面附着用 S_PART
比原版的"整面"宽（漏拆）、灯笼/告示牌/钟/脚手架这些只用"上/下面不是空气"近似（漏拆）。

### 6.14 计划刻 / 重力方块 / 按面支撑（用户测出来的 4 个 bug，2026-10-04）

用户报了四条：①流到一半的水、循环红石电路"搬运后冻结"，`block_ticks`/`fluid_ticks` 搬得不对或根本没搬；
②fixer 不清理非法浮空的重力方块；③上半砖不能支持上方方块、下半砖不能支持下方方块；
④霜冰化成水没写 `fluid_ticks`，水不会流。四条都修了，依据是 26.1.1 反编译源码 + wiki 原文 + 原版裸图实测。

**1. 计划刻的格式和为什么以前会"冻住"**
`block_ticks`/`fluid_ticks` 是区块级的两张表，元素是 `{i:String 方块/流体id, x/y/z:Int 绝对世界坐标,
t:Int 还有几tick, p:Int 优先级}`（wiki Chunk format 的 Tile Tick Format；1.20.4、1.21、1.21.3、1.21.5、
26.1.1 真档都实测过，格式一样）。原版载入时 `SavedTick.filterTickListForChunk` 只留"坐标落在本区块里"的，
保存时 `LevelChunkTicks.pack` 把 `t` 写成"距现在还有几tick"（相对量），`LevelChunkTicks.ticksPerPosition`
按 `(type,pos)` 去重。以前 libnbtrw 里压根没碰这两张表：merge 只搬方块状态，玩家造的红石时钟/正在流的水
搬过去没有刻，就冻住了。现在库里有 `Chunk_Tick` + `chunk_Ticks/chunk_Tick_Get/chunk_Tick_Del/
chunk_Tick_Add/chunk_Tick_Filter`（`nbtrw.hpp`/`chunk.cpp`），四个工具都通过它说话。
merge 的规则是"刻跟着状态来源走"：这次**写过**的格子（状态来自 now）→ 删掉 new 里同格的刻、把 now 里
这一格的刻原样搬过来；没写过的格子保留 new 自己的刻。只搬写过的格子是有意的取舍（逐格比 old 太贵），
代价是 `now==new` 的格子（玩家改过但恰好和板子一样）刻的时间表可能还是板子的——同一电路只要搬过来
一个元件，下一次更新就会把整条电路叫醒。表形状看不懂（缺字段/类型不对）时一条都不动、报数、退出码非 0。
fixer 那边：改一格就删这一格的旧刻（红石元件/水流到点会按旧状态再判一次，留着至少是白跑），
新写的是水就补一条 `fluid_ticks(i=minecraft:water,t=5,p=0)`——和原版 `LiquidBlock.onPlace` 对水源
`scheduleTick(water, WaterFluid.getTickDelay=5)` 一致；`IceBlock.melt` 把霜冰换成水以后也是走这条路
（`setBlockAndUpdate` → onPlace → 补流体刻），这就是 bug ④。

**2. 重力方块（`RULE_FALLING`）**
原版 `FallingBlock.tick` 的条件是 `isFree(下面那格) && y>=minY`，`isFree = state.isAir() ||
state.is(BlockTags.FIRE) || state.liquid() || state.canBeReplaced()`。`canBeReplaced()` 是无参的那个
（`BlockStateBase.replaceable`，静态属性），不是带 context 的重载——所以**雪不管几层都算"托不住"**
（`SnowLayerBlock` 只重载了带 context 的 `canBeReplaced`），而 `firefly_bush`/`cactus_flower` 虽然也是
noCollision 植物却**不在** replaceable 名单里（原版就这样，别按直觉加）。名单是拿 `Blocks.java` 里
`.replaceable()` 的 29 个 register 抽出来的（air/water/lava/short_grass/fern/dead_bush/bush/short_dry_grass/
tall_dry_grass/seagrass/tall_seagrass/fire/soul_fire/snow/vine/glow_lichen/resin_clump/light/tall_grass/
large_fern/structure_void/void_air/cave_air/bubble_column/warped_roots/nether_sprouts/crimson_roots/
leaf_litter/hanging_roots）。重力方块一共 23 种：沙/红沙/沙砾/铁砧三态/龙蛋（26.1.1 的 `DragonEggBlock`
也继承 `FallingBlock`，`getDelayAfterPlace=5`）/16 种混凝土粉末。混凝土粉末特殊：`ConcretePowderBlock.updateShape`
先 `touchesLiquid`，碰到水就凝固成混凝土、根本不会掉；这里照原版用"六邻里含水、且它朝我们这一面不是满的"
近似 `touchesLiquid`（`face_Full` 已经有方向了，正好用上）。fixer 的动作是**拆掉**（不生成
`FallingBlockEntity`/掉落物，也不把方块搬到底下），和拆别的浮空方块一个语义。

**3. 按面支撑（bug ③）**
原版 `SupportType.FULL = Block.isFaceFull(getBlockSupportShape(...).getFaceShape(dir))`，
`CENTER = getFaceShape(dir)` + 中心 2x2x10 的柱子（`VoxelShape.getFaceShape` 会先把"没碰到这一边"的
box 全部去掉，再把碰到的 box 沿轴拉满——所以面在格子边界哪一侧很关键）。因此：
- 台阶：上半砖只有**顶面**是满的、下半砖只有**底面**是满的（wiki Slab 原句："Generally, the top face of
  top slabs, the bottom face of bottom slabs, and all faces of double slabs are handled as solid blocks.
  Due to this, blocks that require a solid surface for placement can be placed on these faces."）；
- 楼梯：`half=top` 只有顶面、`half=bottom` 只有底面（wiki Stairs；朝 facing 的侧面其实也是满的，但内外角
  台阶会变形，先不认）；
- 活板门：关着的 `half=top` 只有顶面、`half=bottom` 只有底面（wiki Trapdoor 原句："Lanterns ... can be
  placed on top of a closed trapdoor that occupies the top half of a block, and below a closed trapdoor that
  occupies the bottom half of a block."）；开着是竖板，顶/底都不算。
以前只有一个不带方向的 `lev`（"有形状的"都当上下能撑），于是上半砖上的门/海草、下半砖下的钟乳石
（原版都是 `isFaceSturdy(UP/DOWN)=真`）被误拆；反过来下半砖上的火把、上半砖下的吊灯（原版 CENTER 是假）
又留着。现在 `sup_OK` 带 `dir`（支撑块朝自己那一面），`face_Full`/`face_Center` 按上面的规则分方向；
**蘑菇是例外**（`MushroomBlock.mayPlaceOn = isSolidRender()`，是"整块实心"不是"这一面满"），
所以加了 `F_SOLID`：`lev=3` 但只看 `b->lev>=S_FULL`，上半砖顶面再满也不算。差点在这里犯错：
如果让蘑菇也走 `face_Full`，上半砖上的蘑菇就会被留下（原版 `isSolidRender(top slab)=false`，会掉）。
其余"有形状的"方块（栅栏/墙/箱子/玻璃板…）仍按名字粗判（上下都当能中心支撑）——它们的形状随连接变，
逐面算容易算错；台阶/楼梯/活板门的**水平面**也还是宽松的（宁可漏拆）。

**4. 真图复核（这次用了"旧规则 vs 新规则"对拍）**
从 git HEAD 编一份旧 `rules.cpp` 当 `rules.so`，和当前 fixer 一起跑一份原版裸图副本；当前 fixer 跑另一份。
再写个逐格比较工具（`test/work/wcmp.cpp`，对比两个 region 目录，整段 NBT 相同就跳过，只逐格比有变化的段），
得到"新规则比旧规则多拆/少拆了哪些格"。主世界 82,411 区块：新规则 1,073,053 次改动（旧规则 10,975），
其中重力方块 1,049,357（gravel 884,699 / sand 163,317 / red_sand 1,341），逐条核对**直接"下面就是空气/水/岩浆/火/
可替换方块"486,648 个、下面那格自己也被拆了的连锁 562,709 个、可疑 0 个**；新旧差值 1,062,191 格里，
除了重力方块还有 1,670 个非重力方块的连锁（干草/海草/铁轨/藤蔓…；旧规则本来也会拆的两遍都有，不在差值里）。
下界 25,826 区块：63,068 次改动（其中重力方块 63,044：直接 30,511、连锁 32,533、可疑 0）；末地 34,315 区块 **0 改动**
（正控：龙蛋在基岩上、紫颂没被按面支撑误伤）。旧规则那边同一份图是主世界 10,975、下界 10、末地 0。
"可疑 0"的判法：被拆的重力方块，若 `下` 不在 free 集合里，则要求下面那格也在"被拆集合"里（连锁）；
否则就是规则判错了。这个数字很大（预生成图里有很多被洞穴/含水层掏空的沙砾脉，原版下一次方块更新都会塌），
但每一条都能对上原版判据；不想要这条规则就换一个不带 `RULE_FALLING` 的插件。

**5. 测试这轮加了什么**
- `mkrule.cpp` +32 条用例（207→239）：台阶/双台阶/楼梯/活板门的上下面（门/海草/钟乳石/火把/吊灯/蘑菇），
  重力方块的 8 种典型情形（悬空/有支撑/下面是水/矮草/火把/沙砾/铁砧/龙蛋），混凝土粉末碰水凝固（`del=3`）。
  加了重力方块以后，用例里那些"支撑块"（沙子/沙砾/仙人掌）自己也得站得住，所以 fixture 在 y=-60 统一垫石头
  （`below2` 指定时覆盖它、并在 y=-61 再垫一层），不然整个用例变成在测连锁。
- `mk.cpp`/`check.py`：now 里放 4 条刻（2 方块、2 流体，分别落在"要搬/不搬"的格子上），new 里放 3 条旧刻
  （2 条在被写的格子上、1 条在没写的格子上），merge 后逐条核对"搬来的/删掉的/没动的"。
- `mkfix.cpp`/`conv.sh`：特殊规则那组加了霜冰、含水藤蔓、悬空沙、沙砾、矮草上的沙、水上的混凝土粉末、
  火把上的沙，并给它们预置旧刻；fixer 后核对水源 `level=0`、`fluid_ticks` 恰好一条且 `t=5`、
  旧方块刻/旧水刻都没了、该留的沙砾/草/火把/沙还在。
- `check_del.py` 的 COVERED 补上新的 8 个名字和后缀，并允许 `*_concrete_powder → *_concrete` 的换名。

**仍然保留的近似/取舍**（都在代码注释里）：merge 只同步"写过"格子的刻；刻的 `t` 是相对量，直接搬到另一个
存档会保留"还剩几tick"而不是"绝对时间"（原版存的就是相对量，只能这样）；fixer 只给水补流体刻
（别的流体不归它管）；重力方块只拆、不生成实体/掉落物；混凝土粉末的 `touchesLiquid` 用"面不满"近似；
楼梯/活板门/其他"有形状的"方块的水平面仍宽松；`F_SOLID` 用我们粗分的 S_FULL（杜鹃/紫颂植株这种
`isSolidRender` 其实为假的方块会被当"实心"放过，方向是漏拆）。

### 6.15 第二轮八条（缓存、format_Chunk、坐标、六邻居、版本，本轮）

用户这一轮给了八条，逐条落到的位置：

1. **霜冰要写流体刻**：`fixer.cpp` 的 `Fix_Scan::write_Block` 现在对所有写成水的方块统一处理：
   先删这一格的旧 `block_ticks`/`fluid_ticks`，再补 `fluid_ticks(i=minecraft:water,t=5,p=0)`。
   霜冰化水、含水方块拆成水都走这里；不再只对霜冰特判。
2. **merge/fixer 不实现缓存**：`Fix_Store`（深拷贝 256 个区块 + 自己 LRU）整个删掉。fixer 用
   `Level::pin_Chunk` 钉住当前 3×3，所有区块树/调色板都在 `Level` 的缓存里；`unpin` 后由 LRU 管。
   merge 也不再自己构造 `MCA` 读区块，改成 `Level::get_Chunk`/`set_Chunk` + `Level::save`；
   实体/POI 仍按文件读写（它们不是区块缓存）。
3. **重复 Y 的 section**：`Chunk::format_Chunk()` 是库接口，当前只做“同一 Y 只留列表最前面的那段”。
   `Chunk::get_Section`/`Chunk_Cursor::find_Section`/`set_Block` 仍取**第一个** Y 匹配的段，稳定、不删除。
   fixer 在物理扫描、清区块、光/高、计划刻里调用 `format_Chunk`；merge 按约定不清理非法数据，
   merge 的输出如果有重复段，原样留给 fixer。
4. **fixer 改写 xPos/zPos**：按区块在 region 文件里的实际放置位置写 `xPos=cx`、`zPos=cz`，
   不再因为坐标对不上跳过；`yPos` 不碰。这样方块实体用 `xPos/zPos` 重算坐标时也是对的。
5. **版本检查合并**：删掉 `Rules_Api::blockporter_version` 和 fixer/merge 里的 `BLOCKPORTER_VERSION`；
   兼容性只看 `RULES_ABI`（改成 `0x4e425233`，`NBR3`）和 `sizeof(Rules_Api)`。以后语义不兼容就加 ABI。
6. **规则文件名**：Makefile 的 `RULES_VER=26.1.1`，产物是 `rules.so.26.1.1`，再链出 `rules.so`；
   fixer 仍按 `rules.so` 找插件。
7. **demo 不提供默认值**：`nbtrw nbt/nbtw/dat/mca/chunk/level` 的路径/坐标都必须显式给；
   `mca` 的 cx/cz 也变成必填（0~31），缺参数直接返回。
8. **rules 只看六个面**：`Rules_Ctx` 删掉 `read` 回调和 `ud`；fixer 预先填好 `nb[6]`/`got`/`bad`，
   rules 的 `nb_Get` 只读这六格。因此 `rule_ChorusPlant` 删掉了斜下方读取，改成“旁边有紫颂植株就留”
   （拿不到那株的根，只能漏拆，不能误拆）。fixer 的连锁队列也从 10 个方向缩成 6 个严格相邻。

测试：`test/lib.sh` 现在 216 + 34 个用例（新增 `format_Chunk`、`pin_Chunk`）；`test/block.sh` 的
fixer 范围扩到 0..63，新增霜冰水刻、坏 `xPos/zPos`、重复 Y 段三个世界，全部通过。

---

## 7. 怎么保证没改错

- **独立校验器**：`test/check*.py` 是另写的 Python 解析器（`test/nbt.py`）+ 独立的规则实现，
  逐格核对"该拆没拆 0 个、误拆 0 个"。它和 C++ 代码不共享任何逻辑，改错了它会喊。
- **兴趣点那张表也有两份**：C++ 是 `chunk.cpp` 的 `poi_Defs[]`（干活用），测试是 `test/poitab.py`
  （照 wiki 手工抄的，用来算期望值）。两边是**故意**分开的：表里漏一个方块、或者类型对错了，
  `check_poi.py` 的"前向/期望记录"立刻不一样。改一边必须同时改另一边。
- **规则用例**：`test/mkrule.cpp` 一个规则一个例子（239 条，每条标 wiki 依据），
  `check_rules.py` 逐条核对，而且第二遍必须 0 改动。
- **合成世界**：`test/mkfix.cpp` 造"该留的 / 该拆的 / 要靠连锁才拆得掉的链子 / 含水物 / 紫颂花"，
  改动数和关键坐标都断言死（`conv.sh`）；`test/mkpoi.cpp` 造"POI/实体"那三套
  （merge / fixer / 坏数据），期望值写在 `check_poi*.py` 里（`EXPECT`/`GONE`/`NO_SYNTH`），
  `run.sh` 还会断言报告里的数字（留了几条、删了几条、删了几个空区块/空文件）和第二趟的幂等
  （`poi/`、`entities/` 一个字节都不能变）。
- **POI 的核对是"双向"的**：前向（每条记录的 type 必须等于那一格方块算出来的类型）、
  反向（区块里该有记录的方块不能缺记录）、形状（根下成员、`Valid`、段键＝`pos.y>>4`、pos 在本区块内、
  没有重复位置）、票（跟 `now` 一样）、实体（整块覆盖/空表删掉/保留原样）。
  "有方块没记录"在真数据上**不算失败**（原版没加载过的区块本来就可能没有），
  但在合成世界里是硬要求（那边没有这种天然空洞，缺了就是 bug）。
- **真存档**：仓库里的 `now/ new/ old/` 是真实升级流程的三份地图，`run.sh` 会自动多跑几轮真数据：
  merge 逐位置核对 4 个区块；fixer 在一个区域里扫到 32 个区块；POI/实体那一轮把 `r.0.0.mca` 的
  region/entities/poi 三份都拿进来，**独立重算**"最终方块 → 期望记录集合"再逐条比（1023 个区块），
  还要第二趟幂等 + `now` 一个字节没动。
- **拿真图副本先试跑**：改完规则/逻辑，**复制一个真区域文件**到 `test/work/` 下面跑一遍 fixer，
  然后看报告里那条 `└ 改的是：方块名×次数`。大垂滴叶那次（误删 1455 片叶子）就是这样抓出来的：
  报告里"下面要有支持"改动 1572 次、`└ 改的是：big_dripleaf×1455,…` 一眼就不对劲。
  要更细就逐格对拍（`test/work/fixdiff.cpp` 那种：把"改之前是什么、它下面是什么"摊开看）——
  真图**只读**，永远在副本上跑。
- **手动扫真图**：`python3 test/check_poi_scan.py <世界目录>`（只读）把一张图的 poi/ 全扫一遍，
  前向/票/形状/反向都给出来。它不进 `run.sh`（纯 python 慢），是"拿真数据检验表"的工具——
  当初就是靠它发现表里漏了 `fletching_table`。实测 `server`（26.1.1，46 个 poi 文件、602 个
  POI 区块、873 条记录）：前向只有 1 条对不上（`(-156,91,410)` 那条 `home`，那张图本来就有的一张
  被挖掉的床留下的悬空记录）、票越界 0、形状错 0、反向缺记录 0。
- **"不动盘"的三轮回归**（`run.sh`，这轮加的，都是真踩过的坑）：
  ① `old` 给空串那趟，`new` 的 `region/`、`entities/`、`poi/` 三个目录 md5 一个字节都不能变；
  ② 一个维度有坏数据时，多维度一起跑必须非 0 退出（`fixer`、`merge` 各一轮，坏的维度故意放前面）；
  ③ 区域文件 `chmod 000`（存在但读不出来）：必须非 0 退出、而且文件 md5 不变；顺带试一次
  `entities/` 文件（merge 不能把它覆盖成"只有这次搬的那几个区块"）。造坏区块用
  `test/corrupt_chunk.py <file.mca> <cx> <cz>`。
- **坏数据用例**：`test/bad.cpp` 66 个用例（截短的 NBT、几十亿的长度字段、坏 tag、类型不对的成员、
  坏区域表头、坏压缩数据、半个 gzip、目录冒充文件、越界坐标、fd 反复开关、POI 的坏形状/坏记录/
  负 Y 段键、实体区块的坏形状、0 字节和只有 8KB 头的区域文件、`INT32_MIN` 的 y、
  "sections 是后补的"游标回归、非 4096 对齐的区域文件写入、`block_entities` 是坏 list……），每个都在子进程里跑：
  **崩了/被信号杀/ASan 报错/用例自己喊 FAIL = FAIL，正常返回或者自己 `exit(-1)` = OK**。
  其中"坏数据必须只跳过、不能退出"是专项（坏 NBT 给 NULL+计数、坏压缩给 NULL+计数、
  一个区域里坏区块跳过而好区块照读、坏 POI 区块整块跳过而好区块照做）。
- **ASan + UBSan**：`sh test/safe.sh asan` 用 clang 或 g++（gcc 要有 libasan）编整套，
  跑坏数据用例 + merge/fixer 真数据 + 规则用例世界 + level 自测，越界/未初始化/UB/泄露都会报。
- **valgrind**：`sh test/safe.sh vg` 跑 14 个场景（含插件 `dlopen`、真实数据读写、fd 跟踪），
  要求 `ERROR SUMMARY: 0`、`definitely/indirectly lost: 0`、退出时只剩 3 个 fd（stdin/stdout/stderr）。

```sh
sh test/run.sh         # 一键回归（造世界 → merge → fixer → python 独立核对 → 规则用例）
sh test/conv.sh        # 连锁/收敛、含水换水、甘蔗不处理、紫颂花
sh test/safe.sh        # ASan+UBSan+valgrind（越界/野指针/泄露/fd泄露）
```

`test/pal.cpp`（世界上出现过哪些方块）和 `test/props.cpp`（每个方块有哪些属性名=取值）是**写规则用的工具**，
不在回归流程里，只在 `run.sh` 里做语法检查。

---

## 8. 加规则 / 换规则（检查清单）

1. 在 `rules.cpp` 的 `defs[]` 表里加一行：`{名字, 规则, 支持集, 支撑档, 标志, 属性名}`。
   名字支持 `*` 前缀/后缀（表里**靠前的先匹配**，所以更具体的要写在前面，比如 `potted_*` 必须排在 `*_sapling` 前面）。
2. 新集合要登记进 `set_List`（忘了会在启动时报"集合没登记"，支持集判定会失效）。
3. 挑一条现成的 `rule_*`，或者写一条新的（23 条见 §5 的清单）。
4. **只读 6 个严格相邻**。fixer 只填这六格；要是规则想读斜向或两格远，改不了插件接口，只能放弃这条规则。
5. **只能把东西变少**（拆掉 / 把 true 面改 false），否则收敛证明不成立（见 3.3）。
6. 拿不准就放过：这个项目的底线是"宁可漏拆，不乱拆玩家搭的东西"。
   要是这条规则**可能拆到兴趣点方块**（村民工作站/床/钟/传送门…），先去 `chunk.cpp` 的 `poi_Defs[]`
   对一下类型，再在 `test/mkpoi.cpp` 的 fixer 世界里加一个例子——拆掉 POI 方块必须连带把它那条
   `poi/` 记录删掉（`test/check_poifix.py` 会核对），不然村民会去找一个不存在的工作站。
7. **多块结构的植物要按 wiki 的结构描述逐条对**（不是只看"放置"那一句）：叶子/茎、上截/下截、
   果/藤……"哪种方块能当谁的支撑"漏一条就是成片误删（第 67 条：大垂滴叶的叶子长在自己茎上）。
8. `make rules`，然后 `sh test/run.sh` + `sh test/conv.sh` + `sh test/safe.sh`；
   再**复制一个真区域文件**到 `test/work/` 下跑一遍 fixer（真图只读，永远跑副本），
   看两处：开头那几行"样例"、以及报告里 `按规则分` 下面的 `└ 改的是：方块名×次数`——
   某条规则拆了一大片你没想到的方块，就是这条规则写错了。
9. 规则版本变了（比如 26.2）：改 `rules.cpp` 里的 `version` 字符串、`Makefile` 的 `RULES_VER`，
   编出 `rules.so.26.2` 后再 `ln -sf rules.so.26.2 rules.so`——fixer 找的符号链接始终是 `rules.so`。
