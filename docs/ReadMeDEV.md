# nbtrw / blockporter 开发文档

给改代码的人看。用户怎么跑命令看 [blockporter-README.md](blockporter-README.md)；
库 API 看 [ReadMe.md](ReadMe.md)。本文只讲**现行规范**、**历史踩坑**、**测试**和**已知限制**。

基线：`2c8a34e`（0 字节 region 先删、UUID 保留先出现、merge 不覆盖读不出的目标 POI/实体、MCA 重叠扇区写回不擦邻居、紫水晶完整面）。
本文还包含 merge 的零拷贝比较/游标缓存优化：同一组合成数据（973 个区块、34,639,872 格、
搬 564,466 格）从 22.5s 降到 2.9s 左右，输出逐区块 NBT 与原比较路径完全一致。
文中 `[现行]` 是现在必须保持的行为，`[历史]` 是旧实现/旧教训，**不要照历史改代码**。

## 1. 现行规范

### 1.1 目录、职责、产物

| 文件 | 职责 |
|---|---|
| `nbtrw.hpp` | libnbtrw 全部声明；merge/fixer/rules 的东西不进来 |
| `nbt.cpp` | NBT 树、原始 NBT 读写、普通 dat/gzip/zlib |
| `block.cpp` | `Block`（id + properties + block_entities） |
| `chunk.cpp` | `Chunk`、section/调色板、`Chunk_Cursor`、计划刻、POI 结构、`check_Chunk`、`format_Chunk` |
| `mca.cpp` | `.mca` 内存镜像、压缩/解压、`format` |
| `level.cpp` | `Level` 存档、维度路径、chunk/MCA/dat 缓存、`pin_Chunk` |
| `nbtrw.cpp` | 库 demo；参数全部必填，没有默认路径/坐标 |
| `merge.cpp` | 方块搬运；没有头文件 |
| `fixer.cpp` | 存档修复；没有头文件 |
| `rules.hpp` / `rules.cpp` | 规则插件 API / 26.1.1 规则与 POI 表 |
| `test/lib.sh` / `test/block.sh` | 现行回归入口 |

产物：`libnbtrw.so`、`nbtrw`、`merge`、`fixer`、`rules.so.26.1.1`（`rules.so` 是符号链接）。
`make` 只编库+demo；`make blockporter` 编 rules/merge/fixer。

### 1.2 库：NBT / Block / Chunk / MCA

- `nbt_Any` 一字节 `type`，无虚函数；数组 `vector`、字符串 `string`、compound
  `unordered_map`；`copy()`/`data()` 深拷贝。List 元素类型必须统一，空 List `elem=0`
  由第一个元素定类型。比较只做低精度→高精度，List/Compound 只比相等，不比大小。
- `Block` 的空 `id` 表示“读不到”，`minecraft:air` 才是空气；`block_entities` 是**一个**
  compound，不是列表。
- `Chunk` 继承 `nbt_Compound`，`serial` 全局只增不减；判断同一区块看 serial。
  `Status != minecraft:full` 或没有 `Status` 时不读不写。缺 section 算空气。
- `Chunk_Cursor` 缓存 `Y→section` 表、`block_states`、palette 的 Name/Properties、
  `xPos/zPos`、`Status`、方块实体索引。`Chunk::format_generation` 在
  `format_Chunk`/merge 的 `ensure_Section` 改动 sections/block_states、fixer 改写
  `xPos/zPos` 时 +1；`Chunk::be_generation` 在 `set_BlockEntity`/`erase_BlockEntity` 真改动
  时 +1。游标看到版本变化会自动重查。**绕过库接口直接改 NBT 树后必须 `cur.reset()`**；直接用
  `sections->con.push_back` 这类操作不会改版本号，游标可能还指着旧表。
- `Block_Ref` 是 `get_Block` 的零拷贝只读视图；`block_Ref_Equal` 按 `Block` 的语义比
  Name/Properties/方块实体（空 compound 和“没有”都算空）。merge 逐格用它比较，
  只有真正要写的格子才构造 `Block`。`Block_Ref` 不能跨 `set_Block`/缓存淘汰保存。
- section 的 `block_states` 是 `palette` + 可选 `data`（LongArray，位宽至少 4）；
  同一个 `Y` 有两段时，读写接口永远取列表里**第一段**，不自动删。
- `format_Chunk(bad_y=NULL,palette_cleaned=NULL)` 是 fixer 的整理函数：
  Y 缺失/不是 Byte 的段整段删；同一 `Y` 只留第一段；`block_states.palette` 里没被
  `data` 引用的条目压紧，并重写全部 4096 个下标（位宽可能变小）。返回删掉的段数；
  `sections` 不是 List 返回 `-1`。可选出参报 Y 坏段数/压过调色板的段数。merge 不调用。
- `check_Chunk()` 是严格检查：NBT 成员类型/必需结构、Y 必须是 Byte、每条计划刻六样俱全；
  缺 section、纯光段、缺 `Status`、重复 Y 都不算坏（重复 Y 由 `format_Chunk` 修）。
  `check_Chunk_Base()` 只查区块级结构（sections/block_states/palette/data/方块实体表、
  刻表成员类型、data 下标），不查单条刻和 Y；fixer 用 Base 版，再按最小单位删记录/子区块。
  false 的区块级损坏：merge 整块跳过、fixer 整块删除。
- `MCA` 文件不在=空镜像；文件在但读不出来=`bad=true`。`format()` 按 8K 表头顺序压紧，
  坏表头一个字节不动；`save()` 先写 `.tmp` 再 rename。
- `Level` 是**整个存档**，不绑定维度；每个操作带 `dimension`。原版三维度按 DataVersion
  选 `dimensions/minecraft/<dim>/`（>=4774）或老 `region/`、`DIM-1/`、`DIM1/`；其它字符串
  当相对存档根的维度目录。chunk/MCA/dat 缓存都在 `Level`，merge/fixer 不自己实现缓存。
- `pin_Chunk`/`unpin_Chunk`：pin>0 的区块不被 LRU 淘汰；全被 pin 时返回 NULL 且
  `miss_why=GET_CACHE`。fixer 的 3×3 工作集就靠它。`set_Chunk` 覆盖被 pin 的坐标时
  原位拷进旧对象，不 free，pin 指针保持有效。

### 1.3 `set_Block`：先预检，再原位写

[现行] `Chunk::set_Block` 不克隆整棵 `block_states`。顺序是：

1. 检查完整区块、坐标、section、`block_states`/palette/`data` 类型；
2. 计算绝对坐标；带方块实体但 `xPos/zPos` 不对直接失败；
3. 预检 `block_entities`：非 List、非 compound 元素、空表但 elem 被定死成别的类型，
   在**写方块状态之前**返回 false；
4. 原位调 `set_BlockData`（调色板/data 已是合法形状，失败路径已提前排除）；
5. 写/删这一格的方块实体，失败只可能是 OOM 这类不会返回 false 的情况。

这样修掉了旧实现的半写问题：旧代码先把方块状态写进调色板，再 `set_BlockEntity`，
方块实体失败时调用方看到 false 但区块已经改了一半。`set_Block` 失败时不改区块；
成功后 palette 指针保持稳定，`Chunk_Cursor` 的 data/位宽缓存和方块实体索引按
`format_generation`/`be_generation` 重查。`format_Chunk` 改过结构会换版本号，旧游标
下次读取自动重查，不必手动 reset（但直接改树后仍要 reset）。

`Level::set_Block` 走同一个函数；直接对 pinned chunk 调 `Chunk::set_Block` 也不会让
`Level` 里的游标悬空（游标按 section/block_states/block_entities 的版本和长度重查）。

### 1.4 merge

[现行] 参数：`new now old[,old...] x1 y1 z1 x2 y2 z2 chunkunit [dims]`。

- 对每格：`now` 与任意 `old` 相同，或与 `new` 自己相同 → 保留 `new`；否则把 `now`
  的方块（含方块实体）写进 `new`。
- `now`/`new` 必须都有、`Status=full`、`xPos/zPos` 对得上文件位置、`check_Chunk`
  通过，才处理；否则整块跳过。`old` 读不出来只忽略那一个。
- 只有真的要写方块时才 `ensure_Section` 补空气段；没改动的区块不补。
- 计划刻/POI/实体原样追加，允许重复；不按方块校验、不去重、不补 POI。
- `now` 有 POI/实体而 `new` 没有对应 MCA 文件时，可以为已处理的区块新建 `poi/` 或
  `entities/` 文件；**region 文件/区块不新建**，new 没有的区块仍跳过。
- `get_Compound()` 返回 NULL 时先看 `last_error`：只有 `MCA_READ_NO_CHUNK` 才当“没有
  这块”并新建；`UNSUPPORTED_COMPRESSION`/`BAD`/`BAD_NBT` 一律跳过，不能覆盖目标，源块
  读不出来也同样跳过。
- 每个处理过的 `new` 区块 `InhabitedTime` 覆盖为搬运方块数（写空气也算）。
- 不删光照/高度图，不整理重复 section，不清理任何非法数据；merge 不调用 `format_Chunk`。
- `set_Chunk` 失败时跳过本区块的实体/POI 追加；本区域 `save` 失败后不再保存实体/POI。
- 区块数据走 `Level` 缓存；实体/POI 仍是直接 MCA 文件 I/O，不是区块缓存。
- 逐格比较用 `get_BlockRef` + `block_Ref_Equal`，不构造两个 `Block`；`Block` 只在确定
  要写这一格时才构造。比较结果必须和 `get_Block` 的 `Block::operator==` 完全一致。
- 终端是 tty 时在 stderr 画区块进度条，不污染 stdout 日志。

### 1.5 fixer

[现行] 顺序：

1. **物理**：rules 逐格判，只给这一格 + 六个面；撑不住拆/改状态。每次写方块先用
   `del_ChunkTick` 删这一格两类旧刻；只要结果方块是 `minecraft:water`（霜冰、含水方块
   拆水、`FX_SET` 水）就补 `fluid_ticks(i=minecraft:water,t=5,p=0)`。连锁只排六个方向。
   不写 `InhabitedTime`。
2. **区块清理**：按文件位置重写 `xPos/zPos`；`format_Chunk` 删 Y 非 Byte 子区块、
   去重复 Y、压紧废弃调色板。`Status != full` 删；全空气或 `InhabitedTime` 明确 Long 0
   且整块在范围内删。**缺 `InhabitedTime` 写 Long 0，但不因此删块**。区块级损坏
   （`check_Chunk_Base()==false` 或 `GET_BAD_CHUNK`）整块 `erase_Chunk`，并记入
   `deleted` 供 POI/实体清理。单条计划刻缺字段、Y 非 Byte 等可修数据不在这里整块删。
   文件级 `BAD_FILE`、不支持的压缩类型（`GET_UNSUPPORTED_COMPRESSION`，如 LZ4 type 4）
   只记日志/`IO跳过`，不删。
3. **光照/高度**：选中区块删 `Heightmaps`、删 `BlockLight/SkyLight`、`isLightOn=0`；
   3×3 邻居（含范围外）只删光照，不改邻居坐标/重复段。邻居 `check_Chunk_Base` 坏则跳过，
   不写回；选中的坏块兜底删除。
4. **计划刻**：先删“六样缺一/类型不对”的单条记录（缺 `p`、缺 `i` 等只删这一条），
   再删指向方块/类型不对的；同一位置重复合法记录只留一条：`block_ticks`/`fluid_ticks`
   各自处理，`p` 最小，`p` 相同 `t` 最大。目标方块读不出来（`BAD_FILE/BAD_CHUNK`）时
   记录保留、不参与选优；范围外不动。刻表本身不是 List 才是区块级错误。
5. **POI**：rules 的 `poi_Get` 提供方块→默认记录。写范围内每个位置只允许一条：
   缺 `pos/type/free_tickets` 的单条记录直接删（不整块重建），再处理类型不符/票数越界/
   重复；缺失按 `Poi_Info.free_tickets` 补；重复时保留 `free_tickets` 最小；已有单条
   合法记录保留原票数。床只 `part=head`。范围外记录保留（重建 POI 块时用 `keep_out`
   救回）。只有 POI 容器形状坏（`Sections`/`Records` 类型、段键）才整块重建，`seen` 里
   已读出的合法记录按原票数重新落盘；新建 POI 根写 `DataVersion`。0 字节 POI 文件按
   空区域重建（没有要补的记录就删文件）。mod/未知命名空间 POI 不猜、不误删。
6. **实体**：单条实体缺 `id`/`UUID`/`Pos` 或不是 compound 时只删这一条；`Entities` 容器
   或根 `Position`/`DataVersion` 坏才整块删。范围内 UUID 重复删后出现的。坏记录删光
   就删掉整块实体；文件级读不出来只记日志。0 字节实体文件直接删。
7. **POI/实体与 region 对齐**：region 里没有的区块，`poi/`、`entities/` 同格记录删掉，
   不新建 region。删除区块时 POI/实体同格一起删。
8. **落盘+format**：`lv.close()` 后对 `region/poi/entities` 的 0 字节文件直接删；空表头
   删文件，非空 `format`。前面任一次 `save`/`close` 失败就停止后续 POI/实体删除和 format，
   避免 region 没落盘而 POI/实体已删。

fixer 的区块缓存全部是 `Level`；当前 3×3 工作集用 `pin_Chunk` 钉住，读完 `unpin`。
fixer 各阶段在 stderr（仅 tty）画进度条。

### 1.6 rules / ABI / 26.1.1

- `rules.so.26.1.1`，DataVersion 4788；`minecraft_version` 必须和 `level.dat` 完全相等。
- 兼容性由 `RULES_ABI`（当前 `0x4e425234`，NBR4）**和** `sizeof(Rules_Api)` 共同把关；
  两边必须同一套 g++/libstdc++（`Blk_Info` 有 `string_view`）。旧的
  `blockporter_version` 已删除，不要加回来。
- rules 只能看 `Rules_Ctx.nb[6]`，没有任意坐标读取；只能越判越少（`FX_AIR` / 关面 /
  `FX_SET` 成规则表不再管的更小状态）。fixer 的收敛依赖这条。
- POI 表在 `rules.cpp` 的 `poi_Defs`：13 种工作站（1 票）、4 种炼药锅（leatherworker 1）、
  床只 head（home 1）、钟 meeting 32、bee_nest/beehive 分开（0）、lodestone/lightning_rod
  8 变体/nether_portal/test_instance（0）。新建默认 `free_tickets=max`；已有记录不重算。
- 26.1.1 重力规则：`free_Below` 只认空气/水/岩浆/火；其它可替换方块（草、雪、藤蔓…）
  和 waterlogged 都不算“托不住”。混凝土粉末仍按原版先看六邻水面凝固（这是 `FX_SET`，
  不受“删除”收紧影响，文档要写清楚）。
- 薄片分类：`resin_clump`、`powder_snow`、`chorus_flower`、`chorus_plant`、`copper_lantern`
  等已归薄片/不支撑；`leaf_litter` 用顶面中心支撑；龟蛋可撑下方灯笼；活板门 open 时
  不再当顶面支撑；雪层要求 full top face；附生瓜茎只认 farmland；`suspicious_sand/gravel`
  也是重力方块。修这些时不要扩大支撑面。
- 中低优先级 Wiki 差异：`glow_lichen`/`sculk_vein` 和紫水晶改 lev=3；紫水晶即使支持集里
  列了芽/簇也要按完整面判，背面再排除树叶/蜂蜜（上半砖顶面这种完整面要保留）；压力板
  排除树叶/TNT；栅栏门不再提供 center support；脚手架 `distance=7` 无下方支撑删；墙钟
  只认 `facing` 反方向的背面；海带改 full top face。

## 2. 历史踩坑（只读，不要当规范）

以下都是旧实现或旧文档里出现过的东西，标 `[历史]`：

- `[历史]` fixer 的 `Fix_Store`：深拷贝区块 + 自己的 LRU。已删除；缓存只在 `Level`，
  fixer 用 `pin_Chunk` 钉 3×3。
- `[历史]` rules 读斜下方/10 个方向连锁/`nb_Below_Of`。现行只给六个面、只排六方向；
  紫颂植株因此保留“旁边有紫颂”的横枝（漏拆，不误拆），甘蔗邻水也看不到。
- `[历史]` merge 删 `isLightOn`/`Heightmaps`、跳过区块清 `InhabitedTime`、`old=""` 只统计、
  POI “now ∪ new 去重/逐条核对方块”。现行 merge 全不删、空 old 照常写、POI 只追加。
- `[历史]` `prune` 是独立程序；已并进 fixer。`make` 不再编 merge/fixer。
- `[历史]` NBT 深度 256 上限、`nbt_bad_*`/`err_cnt` 全局计数、解压 256MB 上限、
  y 限制 ±4096、`CACHE_MAX`/`1<<22` 区域数上限。现行库没有这些限制；错误由调用方处理。
- `[历史]` 旧 `chunk.cpp` 的 `get_POIType/get_POITicketMax` 和 POI 表。已删除，权威表在
  `rules.cpp`；外部若还链接这两个符号会断。
- `[历史]` `RULES_ABI=0x4e425233`（NBR3）、单独的 `blockporter_version`。现行 NBR4，
  旧插件必须重编。
- `[历史]` 旧 `rules.so`、旧 `test/run.sh`/`conv.sh`/`safe.sh`/`rules26.sh`、
  `mk.cpp`/`mkfix.cpp`/`mkpoi.cpp`/`mkprune.cpp`/`mkrule.cpp`/`bad.cpp`/`check*.py`。
  它们按旧 API 写，不能当作现行验证；`mkmerge.cpp`/`mkfix2.cpp` 才是现行。
- `[历史]` `set_Block` 先写调色板、再写方块实体，方块实体失败时半写。现在先预检再原位写。
- `[历史]` `format_Chunk` 曾叫 `dedupe_Sections` 并由 merge 调用。现在只在 fixer 调用，
  merge 原样保留重复 Y。
- `[历史]` 旧文档说“没有 `InhabitedTime` 按 0 处理/不写”。现行缺字段写 Long 0，但不因此删块；
  只有明确 Long 0（或本来就缺失且全空气）才按删除规则处理。
- `[历史]` 旧文档说“3×3 邻居只删光照但会改坐标/重复段”。现在邻居只删光照，不改这些。
- `[历史]` 旧文档把 `test/mk*.cpp` 一刀切说成旧测试。现行是 `mkmerge.cpp`/`mkfix2.cpp`。

## 3. 测试与验证

```sh
sh test/lib.sh     # 269 个库用例 + 34 个 Level 用例
sh test/block.sh  # merge/fixer 合成世界回归（先 make blockporter）
```

`lib.sh` 覆盖 NBT/Block/Chunk/MCA/Level、`format_Chunk`（重复 Y/Y 非 Byte/废弃调色板/
下标重映射）、`pin_Chunk`、`check_Chunk`（含 data 下标越界）、`set_Block` 原子失败/旧游标、
坏 MCA、表头扇区重叠时写回不擦邻居。
`block.sh` 覆盖 merge 搬运/新建 POI/实体文件（不新建 region）、目标 POI/实体坏压缩跳过不覆盖、
fixer 最小单位删记录、Y 非 Byte 子区块、POI 补/去重/坏记录单删/重建（含 DataVersion 和票数保留）、
计划刻坏记录单删/去重、实体坏记录单删/UUID（保留先出现的）/容器整删、0 字节 POI/实体文件、
0 字节 region 先删并清对应 POI/实体、region 缺失的 POI/实体、霜冰/含水门水刻、重力规则、
xPos/zPos、重复 Y、光照/高度、format、中低优先级 Wiki 规则（紫水晶含不能长在芽上、上半砖完整面保留）。

已跑：
- `make` / `make blockporter` 无警告；`lib.sh`、`block.sh` 全过。
- Valgrind `test/work/lib` 和 fixer 合成世界：0 errors / 0 definitely lost。
- 真实区域副本：fixer 能跑完，第二遍 0 改动（真实图 `InhabitedTime=0` 的区块会按规则删除，
  所以要先确认范围/接受删块）。

旧测试未迁移：`test/run.sh`、`conv.sh`、`safe.sh`、`rules26.sh`、`mk*.cpp`（除
`mkmerge/mkfix2`）、`bad.cpp`、`check*.py`、`poitab.py`。不要拿它们的结果当现行证据。

## 4. 已知限制

- rules 只看六面：紫颂横枝可能漏拆、甘蔗旁水看不到、滴水石锥下落等特殊机制没有完整模拟。
  脚手架只处理 `distance=7` 这一条，不重建整条链。
- 重力规则是用户指定版，不是原版 `isFree`：只拆下方空气/水/岩浆/火；其它可替换方块保留。
- 混凝土粉末侧水凝固仍是原版行为；如果用户要“只在下方式水/空气时才凝固”，需要改
  `touches_Water`/`rule_Falling`。
- 3×3 光照邻居仍会写范围外邻居的 `BlockLight/SkyLight/isLightOn`（用户明确的例外），
  但不改它们的坐标/重复段。
- POI 只补 vanilla 表；mod/未知类型保留不猜。POI 记录坏到读不出时只能丢坏记录，
  无法保证范围外同块记录都救回。
- 实体只删单条坏记录和 UUID 重复，不重建实体；POI 不重算已有票数。
- `check_Chunk`/`check_Chunk_Base` 只做结构/下标校验，不判断方块物理合法性；物理交给 rules。
- 文件级坏 MCA（权限/IO/截断到头部）只日志，不覆盖；不支持的压缩类型只跳过，不删。
  0 字节 region 在清理前先删掉，再按“region 没有区块”清对应 POI/实体；0 字节 poi/entities
  文件也删除，0 字节 POI 文件若仍有方块需要补记录，会先按空区域重建。
- fixer 的区块级 `check_Chunk_Base` 仍是白名单：`sections`/`block_states`/palette/data/
  方块实体表容器坏就整块删；单条计划刻/POI/实体坏只删单条，Y 非 Byte 只删子区块。
- merge 用严格 `check_Chunk`/`check_Entity`，任一条刻/实体记录坏就整块安全跳过，
  不代 fixer 清理。
- merge 不升级方块 id，不搬玩家数据/进度/计分板；不清理重复 section/POI/实体。
- fixer 后再跑 merge 会把拆掉的东西搬回来；顺序只能 merge → fixer 一趟。

## 5. 统计字段

fixer 物理行：区块/段/判格/拆/含水/改状态/读不出/跳过/坏下标/坏段/Y坏段/压调色板/
改坐标/删重复段/水刻 +- /noop。清理行：删区块（status/全空气/InhabitedTime0/坏NBT）、
补InhabitedTime、光照/高度、计划刻 删/坏记录/重复、POI 删/补/重复/坏记录/坏块/块/文件、
实体删/坏记录/块/文件、改坐标/重复段/Y坏段/压调色板、删空 MCA/format、IO 跳过、失败。

merge 行：区块/方块/搬了/计划刻/实体/POI；跳过 now 不可读、new 不可写、坏方块、
写不进、坏区域、旧图坏、存盘失败。两程序在 tty 下另有 stderr 进度条。
