# nbtrw / blockporter 开发文档

给改代码的人看。用户怎么跑命令看 [blockporter-README.md](blockporter-README.md)；
库 API 看 [ReadMe.md](ReadMe.md)，测试目录看 [test/ReadMe.md](test/ReadMe.md)。本文只讲**现行规范**、**历史踩坑**、**测试**和**已知限制**。

文中 `[现行]` 是现在必须保持的行为，`[历史]` 是旧实现/旧教训，**不要照历史改代码**。

现状（2026-11-21）：

- 依据按“游戏 jar ＞ 反编译源码 ＞ wiki”（1.8）。形状、存活规则、区域文件、老区块的读法都拿 26.1.1 的 jar 对拍过（`test/mcdump.sh`）。
- 库是跨版本的：区块按自己根上的 DataVersion 读写（1.13 扁平化 1451 以后，1.2.1）；区域文件 gzip/zlib/不压/lz4，外加外置 `.mcc`。
- merge 不补段；跨版本只做暴力合并（1.4：版本不同照样合、不限方向，方块不改名不升级）；fixer 按游戏读档报不报错判坏、
  按最小单位删（1.5、1.10），规则只判 DataVersion 和规则一样的区块；规则插件一个版本一份：`rules.cpp.26.1.1`、`rules.cpp.26.3`（1.6）。
- 范围（用户 2026-11-14 下半）：没有 chunkunit，每种操作按自己的最小单位向外取整，范围外只读不写；遍历先走区域文件（1.4、1.5）。
- 规则除了存活，还照游戏重算几种外观：门两半、藤头藤身、火的面、栅栏/墙/玻璃板/铁栏杆/紫颂植株的连接、苍白苔藓地毯的侧面、
  栅栏门贴墙、箱子配对、雪地草、蘑菇方块的面、音符盒音色、石锥粗细、树叶 distance、脚手架 distance/bottom（1.6）。
- 2026-11-14 做过一轮全量检查：统一排版和命名、定义处补全注释、修了 9 处暗藏的 bug（见 2.1）；2026-11-21 又复查了一遍这以后加的代码（修 5 处，见 2.1）；
  还没解决的缺陷和下一步见 4.、4.1。
- 每一轮改了什么、在真实存档上和上一轮比差多少，见 2.1。

## 1. 现行规范

### 1.1 目录、职责、产物

| 文件 | 职责 |
|---|---|
| `nbtrw.hpp` | libnbtrw 全部声明；merge/fixer/rules 的东西不进来 |
| `nbt.cpp` | NBT 树、原始 NBT 读写、普通 dat/gzip/zlib |
| `atom.cpp` | 游戏的最小单位：`Block`（id + properties + block_entities）、`POI`（一条兴趣点记录）、`Entity`（一个实体） |
| `chunk.cpp` | `Chunk` 和它的索引、section/调色板、计划刻、POI 区块和实体表的读写、`check_Chunk`、`format_Chunk` |
| `mca.cpp` | `.mca` 内存镜像、压缩/解压、`format` |
| `level.cpp` | `Level` 存档、维度路径、chunk/MCA/dat 缓存（按值持有）、`pin_Chunk`/`pin_MCA`/`pin_Dat` |
| `nbtrw.cpp` | 库 demo；参数全部必填，没有默认路径/坐标 |
| `merge.cpp` | 方块搬运 |
| `fixer.cpp` | 存档修复 |
| `blockporter.hpp` | merge/fixer 共用的上层小工具（范围 `Block_Range`、解析坐标、维度去重、目录、进度条、实体在不在范围里）；不是库的一部分 |
| `rules.hpp` / `rules.cpp.<版本>` | 规则插件 API / 每个版本一份规则与 POI 表（`rules.cpp.26.1.1`、`rules.cpp.26.3`），`/*<gen:…>*/` 之间是 `test/gen_tables.py` 从 jar 导出生成的 |
| `test/lib.sh` / `block.sh` / `rules.sh` / `format.sh` / `safe.sh` | 现行回归入口（目录说明见 `test/ReadMe.md`） |
| `test/mcdump.sh` | 拿原版服务端 jar 让游戏自己判，和库/规则对拍（手动跑，见 1.8） |
| `test/work` | 指到 `/tmp/nbtrw-work` 的符号链接（`test/work.sh` 建）：测试造的世界、副本、日志、编出来的辅助程序 |

产物：`libnbtrw.so`、`nbtrw`、`merge`、`fixer`、`rules.so.<版本>`（Makefile 按 `rules.cpp.*` 通配，不链 `rules.so`；
fixer 的最后一个参数给插件路径，默认 `./rules.so`，用户 2026-11-14 定的）。
`make` 全编；`make nbtrw` 只编库+demo，`make blockporter` 只编 rules/merge/fixer（库没编会先编库），`make rules` 只重编插件、`make rules.so.26.3` 只编一个，
`make clear` 清掉这些产物（不碰 `test/work`）。

### 1.2 库：NBT / Block / Chunk / MCA

- `nbt_Any` 一字节 `type`，无虚函数；数组 `vector`、字符串 `string`、compound
  `unordered_map`；`copy()`/拷贝构造深拷贝，`data()` 给 `con` 的指针。List 元素类型必须统一，空 List `elem=0`
  由第一个元素定类型。比较只有 `==`/`<=>`，只做低精度→高精度，List/Compound 只比相等，不比大小。
  `nbt_Any::as_*` 转不了给 `std::nullopt`；具体类型各有一个隐式转换（标量只转成自己的类型：多给几个 `a+b`
  会二义；`nbt_String` 从字符串构造是 explicit，不然 `s=="x"` 二义；`nbt_String` 自己定义同类型 `==`，
  不然 C++20 的反向候选二义）。`nbt_Any` 的构造是 explicit、拷贝构造 protected（防 `5→nbt_Any`、防切片）。
  浮点两边都是 NaN 算相等（2026-11-14：比的是“存的是不是同一个值”，带 NaN 的树和自己的拷贝要相等，不然 merge 会把
  没改过的方块当成改过的；游戏的 FloatTag/DoubleTag 是 record，也是这样）。
- 读 NBT（`nbt_Raw::read_*`）读坏就停：List 读到坏数据立刻跳出，也**不按文件里的长度字段预留内存**（2026-11-14；
  以前每层都谎报长度的嵌套 List 能让读取按“层数×长度”空转、预留几十 GB）；长度字段先按“每种元素最少占几个字节”
  和剩下的字节数核对。嵌套深度仍然不设上限（深到爆栈会崩，见 4.）。
- `Block` 的空 `id` 表示“读不到”，`minecraft:air` 才是空气；`block_entities` 是**一个**
  compound，不是列表。
- `Chunk` = `con`（`nbt_Compound`，按值持有）+ 索引（用户 2026-11-07 定的，游标拆进 Chunk）：
  `sec_by_y[256]`/`palette_by_y[256]`/`data_by_y[256]`/`states_by_y[256]` 按段 `Y` 的 uint8 存（不 +128，负的从后往前；
  `states_by_y` 见 1.2.1），
  `be_by_pos` 是 `get_BlockEntityKey(x,y,z)`（x/z 相对 0~15、y 绝对）→ `block_entities` 下标。
  生成状态不是 `minecraft:full`（或没有这个字段）时不读不写。缺 section 算空气。MCA 只认 `nbt_Compound`，
  `Level` 读出来以后 `Chunk(std::move(compound))`，写回时传 `chunk->con`。
- 索引只记要扫列表才得到的；生成状态、DataVersion、`xPos/zPos`、`block_states` 成员本身每次现读
  （段的方块状态建索引时就判好放进 `states_by_y`：纯光段/老格式少一样是 0＝空气，游戏读不了或库不能原样读写是 -1＝读不出来）。
  构造/拷贝时建好；移动时树的节点不动，索引直接接过来。`reindex()` 强制重建；库接口改树时自己维护：
  增删 section（`format_Chunk`）重建段表，`set_BlockData` 重读这一段，
  删方块实体重建方块实体索引，新加的只往索引补一条（等于重建，只是不全扫）。
  `sections`/`block_entities` 列表被换掉或长度变了、`xPos/zPos` 变了，读的时候能自己看出来并重建；
  **别的直接改树（换某段的 block_states/palette/data、改方块实体 x/y/z）之后必须 `c->reindex()`**。
- section 级函数（`get_BlockData`/`set_BlockData`/`is_SectionConst`）都要传 `Chunk*` 和段号：走区块的索引，编码按区块的 DataVersion。
- 逐格扫一段用 `Section_View`（`view_Section(sy)` 打开时现读一次区块级字段、核对一遍索引）：`get_BlockRef` 每格都现读
  DataVersion/生成状态/sections/block_entities，扫大图会慢一倍。视图里还按调色板下标记着名字/属性指针
  （第一次读时建，palette 换了/长度变了重建），不然每格多两次哈希（实测 merge 慢 25%）。
- `Block_Ref` 是 `get_Block` 的零拷贝只读视图；`equal_BlockRef` 按 `Block` 的语义比
  Name/Properties/方块实体（空 compound 和“没有”都算空）。merge 逐格用它比较，
  只有真正要写的格子才构造 `Block`。`Block_Ref` 不能跨 `set_Block`/缓存淘汰保存。
- section 的 `block_states` 是 `palette` + 可选 `data`（LongArray，位宽至少 4）；
  段的 `Y` 照游戏读（`read_SectionY`＝`getByteOr("Y",0)`：数值截成 byte、浮点先向下取整、缺了/不是数值当 0），
  同一个 `Y` 有几段时取**最后一段**（游戏 `sections[idx]=…` 后面覆盖前面；改 Y 以后撞上的也算），不自动删。
  用户 2026-11-07 定的：读的时候都按游戏取段，只有 fixer 才修成合法。
- `sections` 在但不是“compound 的 List”（不是 List，或非空 List 的 `elem` 不是 compound）：
  `get_Block`/`get_BlockRef` 整块读不出来（空 id / false），**不当缺段=空气**。fixer 的
  邻居读取（`read_Info`）同样按读不出来处理，规则因此 blind、宁可漏拆。
- `format_Chunk(bad_y=NULL,palette_cleaned=NULL)` 是 fixer 的整理函数：
  Y 不是 Byte 的照游戏的读法改成 Byte；同一 `Y` 只留最后一段；`block_states.palette` 里没被
  `data` 引用的条目压紧，并重写全部 4096 个下标（位宽可能变小）。返回删掉的段数；
  `sections` 不是 List 返回 `-1`。可选出参报改了 Y 的段数/压过调色板的段数。merge 不调用。
- `check_Chunk()` 是严格检查：NBT 成员类型/必需结构、每条计划刻六样俱全；
  缺 section、纯光段、缺生成状态、重复 Y、Y 不是 Byte 都不算坏（游戏照读，由 `format_Chunk` 修成合法）。
  `check_ChunkBase()` 只查区块级结构（sections/block_states/palette/data/方块实体表、
  刻表成员类型、data 下标），不查单条刻和 Y；fixer 用 Base 版，再按最小单位删记录/子区块。
  false 的区块级损坏：merge 整块跳过、fixer 整块删除。
- `MCA` 文件不在=空镜像，取区块给 `MCA_READ_NO_CHUNK`（和“表头这一格空”同义）；
  文件在但读不出来（是目录、权限/IO 错）=`bad=true`，取区块给 `MCA_READ_BAD`；0 字节、不到 8K 表头的照游戏补 0 照读
  （后一种 `truncated=true`），不算坏。表头一格照游戏 `RegionFile` 的构造函数（用户 2026-11-14 下半：偏移异常的跳过、format 时置 0）：
  4 字节全是 0＝空；偏移指在表头里（<2）、扇区数是 0、偏移在文件外＝**偏移异常**，游戏 WARN 一声把这一格当 0，库也当没有
  （`has_Chunk` 假、`get_Compound` 给 `MCA_READ_NO_CHUNK`、`is_Empty` 不算它、`has_BadOffset` 真）；`clean_Offsets()` 把它们清成 0，
  `erase_Chunk` 也清得掉。写一格时旧扇区放得下才原地写，偏移指在表头里或者和别的格子重叠的一律追加。
  `format()` 先 `clean_Offsets()`，再按 8K 表头顺序压紧；数据坏的格子（不够 5 字节、长度 0/负数/超出扇区）原样搬它占的扇区，
  不让整理失败（删不删由调用者定）；只有 `bad` 和 3 字节偏移装不下才失败、一个字节不动。`save()` 先写 `.tmp` 再 rename。
- 读一格照游戏的 `RegionFile.getChunkDataInputStream`（用户 2026-11-07：做 lz4 和 mcc，压缩优先用库）：
  表头偏移异常（游戏 WARN 后当没有）是 `MCA_READ_NO_CHUNK`；不够 5 字节、长度 0（WARN）、长度负数/超出扇区（ERROR）、
  外置文件不在（ERROR）、类型号不认识（ERROR）都是 `MCA_READ_BAD`；127 自定义是 `UNSUPPORTED_COMPRESSION`（模组的，不算坏块）。
  类型 1 只认 gzip、2 只认 zlib（游戏分别是 `GZIPInputStream`/`InflaterInputStream`，不互认）。
  游戏边解压边读 NBT、读完根 compound 就停：解压给“能解出来的前缀”（`inflate_Prefix`/`unlz4_Data`），根 compound
  读全就算读到；读不全时流本身坏在半路给 `BAD`，流完整却读不出给 `BAD_NBT`。
- lz4 用系统的 liblz4 + libxxhash（lz4-devel/xxhash-devel）。流格式是 lz4-java 的 `LZ4Block`：每块 21 字节头
  （魔数、token=方法|级别、压缩长度/原长度/校验，小端），方法 0x10 原样、0x20 lz4，校验 = XXHash32(种子 0x9747b28c)&0x0FFFFFFF，
  最后一个全 0 的结束块；读法照 jar 里带的 lz4-java 1.10.1（javap 看的 `LZ4BlockInputStream.refill`）：一块验过校验才交出。
  写法和 `LZ4BlockOutputStream` 一样 64KB 一块、压不小原样存。
- 外置 `.mcc`：类型最高位 128，数据在 `.mca` 同目录的 `c.<区块绝对x>.<区块绝对z>.mcc`（文件名里的区域坐标从 `r.<x>.<z>.mca` 取），
  表里是 5 字节的桩（长度 1、类型|128）。写的时候压完要 ≥256 个扇区就放外置（游戏 `RegionFile.write` 的门槛），先放在
  `MCA::ext`，`save()` 先写 `.mcc` 再写 `.mca`，最后把 `touched` 里不再是外置的格子的旧 `.mcc` 删掉；另存到别处时没改过的 `.mcc`
  也跟着抄。和游戏互相读写对拍过（`sh test/mcdump.sh <jar> region`，四种压缩 + 外置都过）。
- `Level` 是**整个存档**，不绑定维度；每个操作带 `dimension`。原版三维度按 DataVersion
  选 `dimensions/minecraft/<dim>/`（>=4774）或老 `region/`、`DIM-1/`、`DIM1/`；其它字符串
  当相对存档根的维度目录。chunk/MCA/dat 缓存都在 `Level`，merge/fixer 不自己实现缓存。
- `Level` 的缓存全按值持有（`Chunk_Entry::chunk` 是 `Chunk`、`Dat_Entry::dat` 是 `nbt_Compound`，`loaded` 分出“没这块/坏块”），
  放在 `unordered_map` 的节点里，节点不搬家。指针有效期：没钉住的只能用到下一次调 `Level` 为止，每次都 `get_*` 现取；
  `pin_Dat`/`pin_MCA`/`pin_Chunk` 钉住的（pin>0）不被 LRU 淘汰、`close()` 也不放，直到 unpin；全被 pin 时返回 NULL 且
  `miss_why=GET_CACHE`。fixer 的 3×3 工作集就靠 `pin_Chunk`。`set_Chunk`/`set_MCA`/`set_Dat` 都是先拷一份再挪进原对象，
  地址不变。

### 1.2.1 存档格式按 DataVersion（[现行]）

- 库是跨版本的：格式差异只按 DataVersion 判，**不按字段/目录在不在猜**。level.dat 的 DataVersion
  定目录布局（`DAT_NEW_LAYOUT`=4774）；区块按**自己根上的** `DataVersion`（`get_ChunkDataVersion`，
  没有给 0：当最新格式读，但 `is_ChunkVersionKnown` 为假），同一个存档里可以混着没升级的老区块。
- 1451～2842（[现行]）：`Chunk_Layout`（`get_ChunkLayout`）给这个版本的字段键名、在不在 `Level` 里（`get_ChunkBody`）、
  段里是 `block_states` 还是 `Palette/BlockStates`（`get_SectionStates`）、下标跨不跨 long（`span`）。分界照 26.1.1 的
  `DataFixers`（表在 ReadMe「按 DataVersion 选格式」）。库里所有读写区块字段的地方（chunk.cpp/merge/fixer）都走它，
  **不许再手写 `"sections"`/`"block_entities"`/`"Status"` 这些键**。
- 段的方块状态照游戏读档（`get_StatesView`，[现行]）：老区块游戏先用 DataFixers 升级再读，库照着算——
  2527 以前位宽不是 2 的幂时 `BitStorageAlignFix.addPadding`（长度按位宽补齐/截断，2 的幂不转：两种排法一样）；
  2832 `ChunkHeightAndBiomeFix`：两样都在才要、单条扔 data、`padPaletteEntries` 按 data 长度估位宽，比 `ceillog2(n)` 大就补空气到
  `2^(位宽-1)+1` 条（Java int 运算，`ceillog2` 用 `Math.log` 算，只在 2^29 上多 1）；然后 26.1.1 的编解码（`listOf` 认任何集合、
  data 是 `lenientOptionalFieldOf` + `getLongStream`、单条不看 data、长度正好、下标越界在建段数方块时抛）。
  `canonical` 才原样读写（`Chunk::states_by_y`，fixer 的 `Fix_Sec` 也照它）；不 canonical 但游戏读得了的：读不出来、不写、
  `check_Chunk` 不过、fixer 不动。依据是游戏本身：`test/mcdump.sh upgrade`（`UpgradeProbe.java` 照 `ChunkMap.upgradeChunkTag`
  升级、照 `SerializableChunkData.parse` 读）。
- 5009（26.3 快照 7）`STATE_ID_FORMAT`：调色板条目 `id`/`properties`；默认状态可以只写方块 ID（字符串）；
  混在复合列表里的字符串包成 `{"":ID}`；没写的属性取默认值。依据：wiki Chunk_format §History 26.3 snap7、
  zh `模板:Nbt inherit/block state/source`、`Java版26.3-snapshot-7`。编解码只在 atom.cpp
  （`get_StateNameTag`/`get_StateName`/`get_StateProps`/`get_PropValue`/`make_State`/`make_StateBlock`/
  `is_StateSame`），chunk.cpp/merge/fixer 一律调它们，**不许再手写 `"Name"`/`"Properties"`**。
- 5119（26.4 快照 1）`CHUNK_STATUS_LOWER`：生成状态字段 `Status`→`status`（`get_StatusKey`）；
  `BIOME_PER_BLOCK`：段的 biomes 从 4×4×4 一格变成每格一个（`data` 64→4096 个下标）。依据：wiki Chunk_format §History 26.4 snap1、zh 区块格式。
- 源码核对过（`src-26.3`）：`BlockState.CODEC`/`StateHolder.codec` 是 `id`/`properties`，默认状态写成方块 ID 字符串；
  `StateDefinition.appendPropertyCodec` 对没写**和写错**的属性都取默认值；`ListTag` 读任何复合列表都会把 `{"":X}` 解开
  （不只调色板）；26.3 的区块还是 `Status`（改小写是 26.4），`carving_mask` 不再写。版本分界见 1.8。
- 严格：DataVersion≥5009 的区块里出现 `{Name}` 条目、<5009 的区块里出现字符串条目，`check_Chunk`/
  `check_ChunkBase` 都判坏（游戏也是按 DataVersion 选解码器）。字段名和 DataVersion 对不上的生成状态＝没有。
- 新格式 NBT 列表只能装一种类型：`push_PaletteState` 往复合列表加字符串时包成 `{"":ID}`；往字符串列表加
  复合时先把已有字符串条目**原地**包起来——String 节点是挪进去的（不复制），fixer 手里 `Blk_Info.name`
  和 `Block_Ref::name` 仍然有效。`compact_Palette` 保持原来的元素类型。
- 调色板查重：先找一模一样的条目，没有再按 `is_StateSame`（名字+写出来的属性）复用。
- 库不知道默认属性：新格式里没写的属性就是“没有”。`Block`/`Block_Ref` 原样给出写出来的属性；补默认值是
  规则插件的事（26.1.1 的插件只会遇到 4788 的存档，属性总是写全的）。

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
成功后 palette 指针保持稳定，这一段的 palette/data 索引重读（可能刚建了 data），方块实体索引跟着增删。
`format_Chunk` 改过结构会自己重建段表，不必手动 `reindex()`（但直接改树后仍要）。

`Level::set_Block` 走同一个函数；索引在 Chunk 自己身上，直接对 pinned chunk 调 `Chunk::set_Block` 也一样。

### 1.4 merge

[现行] 参数：`new now old[,old...] x1 y1 z1 x2 y2 z2 [dims]`（用户 2026-11-14 下半去掉了 chunkunit）。

- 范围：搬运是方块级的，严格按给定的 x/y/z，范围外的方块/计划刻/POI/实体只读不写；区块级的副作用只有 `InhabitedTime`
  （范围碰到的区块都覆盖成搬运数）。y 范围只对方块级的操作有效。
- 遍历（用户 2026-11-14 下半）：先走区域——列出 now 的 region 目录里实际存在的 `r.<x>.<z>.mca`（`list_MCAFiles`）再和范围求交，
  不逐个区域坐标去试（±3000 万格有 1.4×10¹⁰ 个区域坐标，试不完）；文件名只认游戏自己拼的写法（`r.01.0.mca`、`r.+1.0.mca` 游戏不读，
  2026-11-21 以前会和 `r.1.0.mca` 算成同一个区域、走两遍）；new 的不在/读不出来的整个区域跳过；
  区域里只走 now 表头上有的格子（不存在、偏移异常的跳过）。

- 对每格：`now` 与任意 `old` 相同，或与 `new` 自己相同 → 保留 `new`；否则把 `now`
  的方块（含方块实体）写进 `new`。
- `now`/`new` 必须都有、生成状态=full、`xPos/zPos` 对得上文件位置、`check_Chunk`
  通过，才处理；否则整块跳过。`old` 读不出来只忽略那一个。
- **不补段**（用户 2026-11-07）：new 里这一段不在或者没有 `block_states` 的格子跳过，计入“缺段跳过”
  （游戏写出来的区块在世界高度内每段都有，全空气的也有）。
- **跨版本只做暴力合并**（用户 2026-11-14）：now/new/old 的 DataVersion（level.dat 的、区块自己的）不同照样合，
  不限制方向（高版本合进低版本、有 mod 合进没 mod 都不拦）。方块按各自区块的格式读、按 new 那块的格式写，
  比较按方块语义（名字/属性/方块实体）；不做版本间的改名/属性升级，方块实体、计划刻、单条实体/POI 记录的内容也原样搬。
  计划刻表按 now 自己的布局取（2842 以前是 `Level.TileTicks`/`LiquidTicks`），写进 new 布局的表。
  - **POI/实体的容器在库里转**（用户 2026-11-14）：先按来源的版本读成 `POI`/`Entity`，再按目标的版本写
    （`list_POI`/`set_POI`、`get_ChunkEntities`/`add_ChunkEntity`，ReadMe“计划刻与 POI 辅助”；2026-11-20 改成和 `Block` 一样的结构体）。
    POI 一格只留一条：now 的顶掉 new 同一格的（游戏 `PoiSection` 按位置存）；now 自己同一格有几条时照 `PoiSection.add` 取一条
    （同类型留先来的、不同类型后来的顶掉）；new 那块形状坏（`check_POI` 不过）跳过。
    实体在不在范围里按 `Pos` 向下取整到方块（照 `Vec3.CODEC`，任何数值 tag 都认；没有 `Pos` 的跟着区块搬）。
    实体：now 两处都读（区块里的实体表——2842 以前 `Level.Entities`、之后 `entities`——和 `entities/` 的实体区块；游戏两处都读），
    new 那块是 2681 以前的版本就写进区块的 `Level.Entities`（跟区块一起落盘），否则写实体区块。
    POI：按 POI 区块自己的 DataVersion 读写（1935 以前没有 POI，1946 以前根上 `"0".."15"`，之后 `Sections`；没写当最新）。
    一处实体来源里有一条游戏读不进来，这一处整个不搬（计一次坏实体）；POI 坏的那条跳过，按条计坏 POI。
  - **已知特性（用户 2026-11-14 定的，不修）**：默认属性不写导致的误搬（见下）不管；老名字原样搬过去不管，
    以后可以在 rules 里用方块替换做。
  - 为什么可以不管改名（26.3 的 `DataFixers.java` 逐条核过）：1.13 以后**正式版之间**改过名或属性的方块只有这些——
    1802（1.14）`stone_slab`→`smooth_stone_slab`（`stone_slab` 这个名字后来给了新的石台阶）、`sign`/`wall_sign`→`oak_sign`/`oak_wall_sign`；
    2503（1.16）14 种墙的四个侧面 `true/false`→`low/none`；2531（1.16）红石线没有横向连接的侧面 `none`→`side`；
    2679（1.17）`cauldron` 有水位的→`water_cauldron`；2680（1.17）`grass_path`→`dirt_path`；3692（1.20.3）`grass`→`short_grass`；
    4294（1.21.5）`creaking_heart` 的 `active`→`creaking_heart_state`；4541（1.21.9）`chain`→`iron_chain`。
    另外几条只在快照之间（2209 蜂巢、2508 菌、2528 灵魂火把、2690/2691 铜、2696 grimstone、2700 洞穴藤蔓、2717 杜鹃叶、4305 测试方块）。
    5006/5009（26.3）换了调色板的编码，库按 DataVersion 处理。
  - 后果：old 和 now 同版本时，没改过的格子 now 和 old 一样，不会因为改名被误搬；玩家改过的格子按 now 的名字原样搬，
    碰到上面这几种方块，新版本不认识这个名字（交给 fixer/游戏）。
    26.3 起默认状态写成一个字符串、不带属性，和老版本带全属性的写法按语义比不相等：不给 old、now 和 new 跨了 5009 时，
    没改过的带属性方块也会被搬一遍（写进去的状态是一样的，只是搬运数/`InhabitedTime` 偏大）。
- 计划刻/实体原样追加，允许重复；POI 一格一条（见上）；不按方块校验、不补 POI。
- `now` 有 POI/实体而 `new` 没有对应 MCA 文件时，可以为已处理的区块新建 `poi/` 或
  `entities/` 文件；**region 文件/区块不新建**，new 没有的区块仍跳过。
- `get_Compound()` 返回 NULL 时先看 `last_error`：只有 `MCA_READ_NO_CHUNK` 才当“没有
  这块”并新建；`UNSUPPORTED_COMPRESSION`/`BAD`/`BAD_NBT` 一律跳过，不能覆盖目标，源块
  读不出来也同样跳过。
- 每个处理过的 `new` 区块 `InhabitedTime` 覆盖为搬运方块数（写空气也算）。
- 不删光照/高度图，不整理重复 section，不清理任何非法数据；merge 不调用 `format_Chunk`。
- `set_Chunk` 失败时跳过本区块的实体/POI 追加；本区域 `save` 失败后不再保存实体/POI。
- 区块数据走 `Level` 缓存；实体/POI 仍是直接 MCA 文件 I/O，不是区块缓存。
- 逐格比较用 `Section_View::get_BlockRef` + `equal_BlockRef`（每段给 now/new/各个 old 开一个视图，
  写过方块就重开 new 的视图），不构造两个 `Block`；`Block` 只在确定要写这一格时才构造。
  比较结果必须和 `get_Block` 的 `Block::operator==` 完全一致（`Chunk::get_BlockRef` 就是开一个视图读一格）。
- 终端是 tty 时在 stderr 画区块进度条，不污染 stdout 日志。
- 只碰范围里的区域文件（区块坐标 /32 向下取整；2026-11-14 以前算成了方块坐标 /32）；段只走 -128..127（段号是 Byte）。
- 维度列表按 now/new 两边**解析后的目录**去重（`dedupe_Dims`），不按字符串：`overworld`
  和 `minecraft:overworld` 只跑一遍。跑第二遍时 now 和 new 已经相同，`InhabitedTime`
  会被覆盖成 0，紧接着 fixer 就整块删；计划刻/POI/实体也会再追加一遍。
- `now` 没有某个 `poi/`/`entities/` 文件＝这片没有记录（`NO_CHUNK`），不计“坏POI/坏实体”；
  `new` 没有＝可以新建。只有 `BAD`/`BAD_NBT`/`UNSUPPORTED_COMPRESSION` 才跳过、计坏。

### 1.5 fixer

[现行] 顺序（用户 2026-11-07：以游戏为准，merge 安全跳过、fixer 处理垃圾；游戏读档报 ERROR/WARN 的按最小单位删，
不报的不删不改；判据见 1.10 的表）：

范围（用户 2026-11-14 下半：去掉 chunkunit，都以最小操作单位为上取整目标，范围外只读不写，副作用忽略不计）：
方块级（物理、单条计划刻/POI/实体记录、错位记录迁移）严格按 x/y/z；区块级（修 NBT、删区块、光照/高度、`format_Chunk`）按 x/z
碰到的区块整块做、**不看 y**；区域文件级（清偏移异常、删空文件、压紧）按碰到的区域文件。范围外一律不动，没有例外
（用户 2026-11-21：以前“3×3 光照邻居范围外也删光照”是给 chunkunit 定的，已废弃）。
y 的限制只在方块级有效：以前“整块在范围内（含 y）才删全空气/InhabitedTime 0 的区块”（`is_ChunkCovered`）已经去掉。
遍历：先走区域——列出目录里实际存在的区域文件再和范围求交（`list_Files`/库的 `list_MCAFiles`；不逐个区域坐标去试），
region 文件读不出来的整个区域跳过（`list_Regions`，各阶段共用；POI 走 region∪poi 的文件，实体/整理走各自目录的文件）；区域里只走表头上有的格子（`each_Chunk`），
不存在、偏移异常的跳过。规则插件是最后一个参数（默认 `./rules.so`），不再找程序旁边的。

0. **修 NBT**（`repair_Chunks`，在物理之前，规则看到的是修好的区块）：解压/NBT 坏（`GET_BAD_CHUNK`）
   整块 `erase_Chunk` 并记入 `deleted`（偏移异常的格子游戏当没有，遍历时就跳过了，最后整理时清成 0）；`xPos/zPos` 按文件位置重写（数值类型不同但值对的不动）；然后 `repair_ChunkNBT`：
   NbtIo 读不了的 tag（>512 层、坏 modified UTF-8）删那个 tag；段的 block_states/biomes 解不出来删那一段的
   block_states/biomes；认不出名字的调色板条目换成空气（生物群系换成 plains）；刻表不是 List 删成员、单条读不进来删那条；
   方块实体没有 id/id 写法不合法删那条；结构引用/起点名字写法不合法删那项、引用指到 8 区块以外删那个坐标。
   然后 `migrate_Strays`（用户 2026-11-14 下半：pos 指到其它区块的迁移到正确区块，接受缓存频繁失效）：方块实体、计划刻的坐标在别的区块、
   又在范围里的，放进坐标所在的区块——目标区块不存在/读不进来/没生成完就删这条，目标那一格已经有方块实体/同样的刻就留原来的、删这条，
   读不出来（文件坏、自定义压缩、缓存腾不出来）、DataVersion 不同、表形状不对、目标区块自己的 xPos/zPos 也不对（还没轮到修，下一趟再迁）就原样留着。方块实体没有 x/z 的、整个区块挪过位置
   （xPos/zPos 刚改过、记录指着原来的区块）的不算错位，照游戏改到本区块里（x/z 取低 4 位）。**这里不再 `format_Chunk`**（见 8.）。`sections` 不是 List 游戏当没有段，不动。
   自定义压缩（127，模组的）、文件级 `BAD_FILE` 只记日志/`IO跳过`。
1. **物理**：rules 逐格判，只给这一格 + 六个面；撑不住拆/改状态。每次写方块先用
   `del_ChunkTick` 删这一格两类旧刻，再补插件在 `Rules_Ctx.out_tick` 里给的那条（26.1.1 插件：含水方块拆完
   留水源、霜冰化水时补 `fluid_ticks(i=minecraft:water,t=5,p=0)`）。连锁只排六个方向。
   不写 `InhabitedTime`。
2. **区块清理**（只剩删除规矩，区块级）：生成状态不是 full 删；全空气、或 `InhabitedTime` 是0
   （照游戏 `getLongOr`：没有/不是数值都当0，用户 2026-11-07：不补了，读不到当0删）删。x/z 碰到就整块看，不看 y。修 NBT 阶段没修掉的整块载入失败
   （`check_ChunkBase()==false`，正常不会有）兜底删。删掉的都记入 `deleted` 供 POI/实体清理。
3. **光照/高度**：范围碰到的区块删 `Heightmaps`、删 `BlockLight/SkyLight`、`isLightOn=0`；范围外的邻居不动。
   这一步和计划刻那步都不再改坐标、不再 `format_Chunk`（修 NBT 做过/最后整理做）。
   不写回；选中的坏块兜底删除。
4. **计划刻**：先删游戏读不进来的单条（`check_ChunkTickRecord`：i 不是写法合法的名字、x/y/z/t/p 不是数值），
   刻表不是 List 删成员；再删指向方块/类型不对的；同一位置重复合法记录只留一条：`block_ticks`/`fluid_ticks`
   各自处理，`p` 最小，`p` 相同 `t` 最大。目标方块读不出来（`BAD_FILE/BAD_CHUNK`）时
   记录保留、不参与选优；范围外不动。
5. **POI**：rules 的 `get_POIInfo` 提供方块→默认记录。先删 NbtIo 读不了的 tag，再删解不出来的段
   （不是 compound、没有 `Records` 或不是 List；游戏 ERROR 整段不要），不再整块重建。单条记录照
   `PoiRecord.Packed.CODEC`（库的 `get_POIRecord`）：pos 不是 3 个数、type 写法不合法的删；`free_tickets` 没有/不是数值
   游戏当0，照留。写范围内每个位置只允许一条：类型不符/票数越界/重复处理同前；缺失按 `Poi_Info.free_tickets` 补；
   重复时保留 `free_tickets` 最小；已有单条合法记录保留原票数。床只 `part=head`。范围外记录保留。新建 POI 根写 `DataVersion`。
   0 字节 POI 文件按空区域重建（没有要补的记录就删文件）。mod/未知命名空间 POI 不猜、不误删。
   对应的区块取不到（缓存腾不出位置、落盘失败这类）时这一块的 POI 不动、计失败（2026-11-14 以前会当成“没有要 POI 的方块”删记录）。
6. **实体**：先删 NbtIo 读不了的 tag；`Position` 坏/缺/对不上（游戏 WARN/ERROR，实体照载）按文件位置重写，不再整块删；
   `Entities` 不是 List（WARN）删成员。单条实体照 `check_EntityRecord`：不是 compound、id 写法不合法、UUID/Pos/Motion/Rotation
   形状不对、Pos 是 NaN、Rotation 不是有限数的删这一条；**没有 UUID/Pos 的照留**（游戏随机给 UUID、放原点）。
   乘客一个个判，坏的只删那个乘客。范围内 UUID 重复删后出现的（乘客也算；UUID 照 `Codec.INT_STREAM`，Byte/Long_Array、数值列表写的也认）。坏记录删光就删掉整块实体；
   文件级读不出来只记日志。0 字节实体文件直接删。
7. **POI/实体与 region 对齐**：region 里没有的区块，`poi/`、`entities/` 同格记录删掉，
   不新建 region。删除区块时 POI/实体同格一起删。
8. **落盘+整理文件**（`format_Regions`，用户 2026-11-14 下半：format_Chunk 在 format_MCA 处做，清理废弃项和空文件之后只跑一遍）：
   `lv.close()` 后对范围碰到的 `region/poi/entities`：0 字节文件直接删；`clean_Offsets()` 把偏移异常的格子清成 0；一个区块都没有的删文件；
   region 文件里范围碰到的区块在这里 `format_Chunk`（Y 改成 Byte、重复 Y 留最后一段、压调色板）——整趟只这一遍；最后 `MCA::format` 压紧落盘。前面任一次 `save`/`close` 失败就停止后续 POI/实体删除和 format，
   避免 region 没落盘而 POI/实体已删。

版本（[现行]，用户 2026-11-07：fixer 只要求“皮”支持多版本）：
- 区块 DataVersion 不认识（1451 以前、没写）：修NBT/清理/光照/计划刻/POI 都跳过不动，计入“版本不认识跳过”。
- 规则只判 DataVersion 和规则一样（`is_RulesChunk`：等于 `Rules_Api::minecraft_version`）的区块：不一样的物理阶段不钉
  （邻居读到当读不出来、中心不判，计入“版本和规则不同跳过”）；计划刻不拿方块对（保留、不去重）；POI 不按方块补删。
  修NBT/清理/光照照做（库按区块自己的版本做）。
- POI 文件的 DataVersion（没有当 1945，同 `SectionStorage`）和规则的不一样：整块不动（写回会把 DataVersion 改成新的，
  游戏就不再升级它）。实体文件写回保留原来的 DataVersion。

fixer 的区块缓存全部是 `Level`；当前 3×3 工作集用 `pin_Chunk` 钉住，读完 `unpin`。
fixer 各阶段在 stderr（仅 tty）画进度条。维度列表按解析后的目录去重（同 merge）。

### 1.6 rules / ABI / 各版本

- 一个版本一份源码（用户 2026-11-14 下半）：`rules.cpp.26.1.1`（DataVersion 4788）、`rules.cpp.26.3`（5023），Makefile 通配出
  `rules.so.<版本>`。两份的代码一样，不一样的只是数据（方块名单、形状特判、规则表里新方块的行、生成的表）；改判法两份一起改。
  `minecraft_version` 必须和 `level.dat` 完全相等。
- 从 jar 生成的表（`test/gen_tables.py` 读 `mcdump.sh shape` 的 shapes.tsv，写进 `/*<gen:…>*/` 之间，别手改）：音符盒音色 `instr`、
  `#prevents_nearby_leaf_decay`、材质 `set_replace/set_liquid/set_burn`、每种方块默认状态的属性 `defaults`。生成完照样要 shape/cmp 对拍。
- 默认属性（26.3 快照 7 起存档里默认值可以不写）：插件 `fill_BlkInfo` 先把没写的属性按 `defaults` 补上，`Blk_Info.props` 换成指向
  插件自己池子里补全的那份（`full_Props`，按“名字|写出来的属性”去重，`reset_Rules` 之前有效）。规则看到、交回去写的都是写全的状态。
  写错的属性值游戏也取默认值，这里不管（只补没写的）。主机不依赖 `props` 指在树里，所以 ABI 不变。
- 26.3 相对 26.1.1：新方块（白杨木一套、硫磺/朱砂一套、羊毛/混凝土台阶楼梯、`shelf_mushroom`、`red_shrub`、`straw_bed`、`sulfur_spike`、
  `potent_sulfur`）；`SpeleothemBlock`（`#speleothems`＝滴水石锥+硫磺尖刺）：根那一头要同一种石锥，两个尖对上也要同一种；树脂砖墙空状态也强制实心；
  `VegetationBlock` 改看 `#supports_vegetation`。两个 jar 上 shape/cmp 都对过（3.）。
- 兼容性由 `RULES_ABI`（当前 `0x4e425237`，NBR7）**和** `sizeof(Rules_Api)` 共同把关；ABI 只跟
  `rules.hpp` 走（fixer 和插件之间的调用格式），不跟 rules.cpp 走。两边必须同一套 g++/libstdc++
  （`Blk_Info` 有 `string_view`、`Rules_Ctx` 有 `Block`）。旧的 `blockporter_version` 已删除，不要加回来。
  插件入口符号 `get_RulesApi`；`Rules_Api` 成员：`count_Rule`/`get_RuleName`/`count_Group`/`get_GroupName`/
  `reset_Rules`/`fill_BlkInfo`/`get_BlkRule`/`get_DefRule`/`get_POIInfo`/`judge_Blk`/`check_Tick`。
- **分工**：一个 fixer 配不同的 rules.so 修不同版本、不同模组的存档。fixer/libnbtrw 只管存档格式（按
  DataVersion），**fixer 里不许有方块知识**：拆完留什么（`FX_AIR` 写 `out_block`，空 id 才写空气）、写完补
  什么计划刻（`out_tick`）、计划刻对不对得上（`check_Tick`，含 `flowing_water`/`flowing_lava` 这类流体 id）、
  空气占位怎么分类（`init_AirInfo` 让插件填）都问插件。
- `Blk_Info`：主机填 `blk`（调色板条目原样，插件别拆）/`name`/`props`（按区块格式取好，props 为 NULL＝没写）；
  插件填 `base`（统计用短名）/`flags`（主机只看 `BF_AIR`/`BF_WATER`）/`priv[4]`（插件私有）。rules.cpp 的
  `priv` 布局：`P_SHAPE` 形状位图、`P_SETS` 支持集位、`P_DEF` 规则表下标+1（0＝不管，主机造的占位全 0
  正好是“不管”）、`P_MISC` 支撑档|PF_*（岩浆块/树叶/水源…）|CK_*（栅栏/墙连接要认的类别）。支撑档 `S_*`、`PF_*`、`CK_*` 只在 rules.cpp。
- `Rules_Ctx.out_block` 是 `Block`（插件交名字+属性，主机按区块格式写回）；`out_keep_be` 搬方块实体；
  `blind`/`out_keep_be` 是 `bool`。`is_Air`/`is_Water`/`get_Prop`/`is_PropValue` 是 rules.hpp 里给插件用的
  `Blk_Info` 小判断（属性从 `props` 取）。
- `RULES_DATA_VERSION`（默认 4788）只给 `test/format.sh` 用：同一套规则声明成 5119，测新格式的存档。
- rules 只能看 `Rules_Ctx.nb[6]`，没有任意坐标读取（用户 2026-11-10：硬约束）；不放方块，改动只能是 rules.hpp 文件头那三种：
  越判越少（`FX_AIR`/关面/换成更小的状态）、沿单向的链换（结果的瓜茎→普通瓜茎 age=7…）、跟着邻居走（门、藤头藤身、火的面、
  栅栏/墙的连接、苍白苔藓地毯的侧面）。“只关不开”是防死循环的建议，不是要求（用户 2026-11-10）：能证明每格只变有限次就照游戏开面/连上，
  证明写在 rules.hpp 文件头（看的东西不成环）。fixer 不依赖单调，只依赖能停下来。
- POI 表在 `rules.cpp` 的 `poi_Defs`：13 种工作站（1 票）、4 种炼药锅（leatherworker 1）、
  床只 head（home 1）、钟 meeting 32、bee_nest/beehive 分开（0）、lodestone/lightning_rod
  8 变体/nether_portal/test_instance（0）。新建默认 `free_tickets=max`；已有记录不重算。
- 重力规则：`is_FreeBelow`＝原版 `FallingBlock.isFree`（空气、#fire、`liquid()`、`canBeReplaced()`）；waterlogged 不算液体、
  也不可替换。可替换（29 种方块，和属性无关）、液体（水/岩浆/气泡柱）、可燃（`FireBlock.canBurn`：207 种，其中 60 种含水时不算）
  三张表从 jar 导出，存在 `priv[P_MISC]` 的 `PF_REPLACE/PF_LIQUID/PF_BURN`，`mcdump.sh shape` 逐状态核对。原版是变成下落方块落下去，
  fixer 只能拆（不往别处放）。混凝土粉末先看上面和四个侧面有没有水碰得到、碰到就凝固；**正下方的水不算**
  （`ConcretePowderBlock.touchesLiquid` 的循环里向下那次只在粉末自己含水时才看），所以浮在水面上的粉末是掉，不是凝固。
- **支撑按面判**：rules.cpp 的 `fill_Info` 给每个方块状态算形状位图（存在 `priv[P_SHAPE]`）——六个面各自 FULL（isFaceSturdy
  FULL）/ CENTER（只记上下两面，原版只在上下用）/ COLL（碰撞面满），外加 RIGID_UP（顶面带边）、SOLID（legacy isSolid）、
  RENDER（isSolidRender）、TOPCOLL（碰撞箱顶面非空）、UNDER（放在墙上面时碰撞箱底面盖住墙的柱子那块/哪几条臂，`WallBlock.isCovered`；
  碰撞箱底面整面的五块全盖住，`fill_Info` 统一补）。名字能定的在 `calc_NameShape`，要看属性的（台阶、楼梯、活板门、门、
  栅栏门、雪层、紫颂植株、末地烛/避雷针、锁链、漏斗、砂轮、末地传送门框架、钟、梯子、紫水晶、滴水石锥、灯笼、蛋糕、瓶子草作物、
  活塞/活塞头、展示架、墙、吊牌、苍白苔藓地毯、栅栏/玻璃板/铁栏杆、箱子）在 `calc_StateShape`。
  竹子/滴水石锥的碰撞箱随坐标偏移（`offsetType(XZ)`），UNDER 不记在位图里，墙规则按坐标现算（`get_OffsetUnder`，照 `Mth.getSeed`）。规则只查位：`check_Support` 按 lev（3=整面、2=中心、
  F_RIGID 时带边也算）、F_ISSOLID（告示牌/旗帜/蛋糕）、F_SOLID（蘑菇：整面+RENDER）；`can_FaceAttach`＝整面或碰撞面满
  （藤蔓/地衣/幽匿脉络/树脂团/苍白垂须/苍白苔藓地毯侧面，MultifaceBlock.canAttachTo）；雪层看碰撞箱顶面；海泡菜 F_TOPCOLL。
- **形状的依据是游戏自己算的**（[现行]，见 1.8）：`sh test/mcdump.sh <server.jar> shape` 在原版 jar 上逐个方块状态问
  isFaceSturdy/碰撞箱/isSolid/isSolidRender/墙的 isCovered，再逐状态比 `priv[P_SHAPE]`。26.1.1 的 29873 个状态**全部一致**
  （`shapes-diff.tsv` 是空的）。竹子/滴水石锥另导 272 个坐标（-8..7 方格 + 几处 `x*3129871` 溢出 int 的大坐标）上的 under，
  shapecmp 在同样的坐标上让 rules.so 判一段墙核对（8704 行，0 处不对）。改了形状就重跑，不一致的会逐条列出来。
  唯一故意不一样的是开着的潜影盒（形状靠方块实体）按关着算。
- **规则的依据也是游戏**：`sh test/mcdump.sh <server.jar> cmp` 对每个要支撑的方块状态、加上规则表管的（栅栏/墙这些不会掉的也算；
  455 种方块、17520 个状态）先找一个游戏判“能活”的六邻配置，再把六个邻居轮流换成全部 29873 个状态，同时问游戏和 rules.so
  （共 31.4 亿个配置，20 核约 20 分钟；墙有 26 种 × 324 个状态，游戏判墙要做碰撞箱运算，占了大头）。
  现在“差异合计”是没有；故意不一样的写在 `test/mc/RulesCmp.java` 的 `known` 表里，报告里单独列成“#已知”：
  霜冰直接化水（用户要求）；流体规则不管（流动水靠流体刻慢慢算）。比较方式：门/火/藤头藤身/竹子/垂滴叶/栅栏/墙/苍白苔藓地毯
  （`is_Exact`）比完整状态，藤头的 age 游戏随机，不比；藤蔓/地衣这种按面判的只比原来开着的面；别的方块游戏只改了外观属性
  （楼梯形状、玻璃板连接…）不比。栅栏/墙这些不会掉的方块因为规则表管它们，也在测试对象里。
- **连接这一类外观照游戏重算**（[现行]，用户 2026-11-07 同意做栅栏/墙/苍白苔藓地毯、11-10 定“只关不开”只是建议、11-11 “做掉同类的”）：
  都只改样子、不拆，规则族 `link`（苍白苔藓地毯和紫颂植株还在各自的存活规则里）。
  - 栅栏（`RULE_FENCE`，`FenceBlock.connectsTo`）：四个方向各看邻居——同一类栅栏（`#fences` 里木的只连木的、下界砖的只连下界砖的）、
    门板横在这个方向的垂直方向上的栅栏门（`FenceGateBlock.connectsToDirection`）、朝这边整面的方块（`isExceptionForConnection`
    的树叶/屏障/南瓜/雕刻南瓜/南瓜灯/西瓜/潜影盒除外）。不看上下。
  - 墙（`RULE_WALL`，`WallBlock.updateShape`）：连不连＝别的墙、铁栏杆/玻璃板（IronBarsBlock）、同样的栅栏门、朝这边整面的方块（同上的例外）；
    连着的那边，上面那格碰撞箱底面盖住那条臂就是 tall、不然 low；柱子：上面是立着柱子的墙 → 立；四边都不连或者有一边对面不连（拐角/端头）→ 立；
    一条直线两边都 tall → 不立；剩下的看上面是 `#wall_post_override`（火把/绊线/告示牌/旗帜/压力板/仙人掌花）或者盖住了柱子那块。
  - 苍白苔藓地毯（`RULE_PALE_CARPET`，`MossyCarpetBlock.updateShape`）：活不下去拆；活得下去照 `getUpdatedState` 重算四个侧面——
    铺地的那块能贴就长出 low（上面那截同一边有面就 tall），上面那截能贴就留原值、原来是 low 的看下面那块同一边是不是 none。
  - 玻璃板/铁栏杆（`RULE_BARS`，`IronBarsBlock.attachsTo`）：别的玻璃板/铁栏杆、墙、朝这边整面的方块（同上的例外）；不连栅栏门、不连栅栏。
  - 栅栏门（`RULE_GATE`，以前是“不管”）：门板两头（facing 顺时针/逆时针那两边）有一边是 `#walls` 就 `in_wall=true`。支撑形状/碰撞箱和它无关。
  - 箱子（`RULE_CHEST`：箱子/陷阱箱/8 种铜箱子；末影箱没有 type）：照原版顺序（下 上 北 南 西 东）一个方向一个方向走 `ChestBlock.updateShape`——
    那边是能连的箱子（普通/陷阱只认同一种，铜箱子认任何铜箱子）：自己是单箱、它是大箱子一半、朝向一样、另一半指着自己 → 补成另一半；
    那边不是能连的箱子、自己指着那边 → 单箱；指着的是同种箱子但对不上，原版不动。铜箱子是大箱子、指着那边的铜箱子 → 换成那边的方块
    （`CopperChestBlock.updateShape`，氧化/打蜡对齐，谁先判谁照对方改）。方块实体（箱子里的东西）照留，铜箱子都是 `minecraft:chest`。
  - 雪地草（`RULE_SNOWY`：草方块/灰化土/菌丝，`SnowyBlock`）：上面是 `#snow`（雪层/雪块/细雪）就 `snowy=true`。
  - 紫颂植株（`RULE_CHORUS_PLANT` 活得下去以后，`set_ChorusLinks`）：六个方向是紫颂植株/紫颂花就连，向下还认 `#supports_chorus_plant`。
  - 类别（`#fences`/`#wooden_fences`/`#walls`/`#wall_post_override`、IronBarsBlock、FenceGateBlock、例外名单）按名字记在 `priv[P_MISC]` 的 `CK_*`；
    挨着模组方块（标签/形状不知道）、要看的邻居读不出来、栅栏门/箱子没写朝向，整格不动。属性没写的照原版默认值。
    重算出来不一样才写；含水的补一条水的流体刻（原版 updateShape 里含水就排一条，主机写这格时会删掉原来的刻）。
  - 收敛：栅栏/墙/玻璃板的侧面从来不是整面，互相只认名字；墙看上面的墙只往上看；上面那截地毯的面只会关；栅栏门/雪地草/紫颂植株只认名字；
    单箱补成大箱子只一次（指着它的那半不会再变）、大箱子拆开只在另一半不是箱子了时；也没有别的规则靠它们连没连活
    （游戏算出来的状态顶面/底面中心一直在；紫颂花看的是下面是不是紫颂植株，不看形状）。证明见 rules.hpp 文件头。
  - 蘑菇方块/蘑菇柄（`RULE_MUSHROOM`，`HugeMushroomBlock.updateShape`）：挨着同一种方块（全名相同）的面改 false，只关不开。
  - 音符盒（`RULE_NOTE`，`NoteBlock.setInstrument`）：上面那格的音色是头颅（`worksAboveNoteBlock`）就用它，不然用下面的（下面是头颅当 harp）。
    方块→音色的表从 jar 生成（`instr`，priv[P_MISC] 的 24..28 位）；上下是模组方块不动。
  - 滴水石锥/硫磺尖刺（`RULE_DRIP` 活得下去以后）：照 `calculateDripstoneThickness`（26.3 `calculateSpeleothemThickness`）重算 `thickness`，
    只看尖那头（front）和根那头（behind）。
  - 树叶（`RULE_LEAVES`，`LeavesBlock.updateDistance`）：`distance`＝min(7, 六邻 distance+1)，原木这类（`#prevents_nearby_leaf_decay`，
    priv[P_MISC] 第 29 位）算 0，只认树叶自己的 distance（脚手架那个是另一个属性对象，`hasProperty` 按对象认，对拍核过）；模组邻居不动。
    `distance=7` 又不是 `persistent` 的不拆，游戏会随机刻慢慢腐烂。
  - 脚手架（`RULE_SCAFF` 活得下去以后）：`distance`/`bottom` 改成 `ScaffoldingBlock.tick` 算出来的（bottom＝distance>0 且下面不是脚手架）。
  - 收敛：树叶/脚手架是封顶 7 的最短路（每格只依赖邻居存的 distance，游戏自己也是这么一格格传的），石锥粗细只沿朝尖那头往回看，
    蘑菇只关不开，音符盒只看上下是什么方块。
  - 对拍：`McWorld` 的计划刻里照游戏的式子模拟树叶的 `updateDistance`、脚手架的 `tick`（这两个的刻要 ServerLevel，假世界调不了）。
  - 还没做的：红石线连接（`RedstoneWireBlock.getConnectingSide` 要看斜上/斜下那格，超出“只看六邻”的硬约束，用户 2026-11-10 定的），
    红石信号（power/powered）；楼梯形状（`StairBlock.getStairsShape`，前后那格是楼梯时看朝向/半边，都在六邻里）还没做，见 4.1。
- 这一轮照游戏补上的（以前是“用户规矩”或“规则不管”）：
  - 紫颂植株第 1 条（水平连着紫颂、上下都不空 → 碎）照判；真实末地预生成图上会拆极少数自然生成的格子（以前测过 1024 个区块 171 格）。
  - 门（`judge_Door`）：两半互查之外，另一半是门、half 相反就把自己改成“另一半的状态、half 换成自己的”（`DoorBlock.updateShape`，
    别种门、朝向/铰链/开关不同都照改）。fixer 改一格当场写回再排六邻，所以谁先判谁照对方改，改完就一致。
  - 外观转换（`judge_Convert`，规则判“能活”以后再看）：藤头长的方向那格是同族变藤身、藤身那格不是同族变藤头（海带/缠怨藤往上、
    垂泪藤/洞穴藤蔓往下；藤头 age 原版随机 0..24，这里 `get_PosRandom` 按坐标散列；洞穴藤蔓照留 berries）；大型垂滴叶上面是叶子变茎
    （facing/waterlogged 照留）；竹笋上面是竹子变竹子默认状态；竹子上面那节 age 更大就 cycle。
  - 火（`RULE_FIRE`）：`FireBlock.canSurvive`（下面顶面整面或六邻可燃）不成立变空气；成立就照 `BaseFireBlock.getState` 重算：
    下面是 #soul_fire_base_blocks 变灵魂火，下面可燃或顶面整面五个面全关，否则各面看那边可不可燃；age 照留；改了样子补一条
    `minecraft:fire` 方块刻（t=30，原版 onPlace 排 30~39 刻），不然火不会再烧/灭。灵魂火下面不是灵魂沙/灵魂土就拆。
  - 下界传送门（`RULE_PORTAL`）：门框平面里的上下/两侧邻居既不是传送门也不是黑曜石 → 门一定不完整 → 拆（`PortalShape.findAnyShape`
    的门是矩形：平面邻居要么在门里要么在框上）；是黑曜石要看整个门框，留着。
  - 收敛约定改成三种改动（越判越少 / 单向链 / 跟着邻居走），见 rules.hpp 文件头；ABI 不变（fixer 不依赖单调，只依赖能停下来）。
- 这一轮照游戏改掉的（以前照 wiki，有的是误拆、有的是漏拆）：
  - **藤蔓挂在下面看的是上方方块的顶面**（`VineBlock` 调 `isAcceptableNeighbour(上方, DOWN)`，`canAttachTo` 再取反面）：上半砖/
    倒楼梯/上半活板门下面能挂，营火/耕地/讲台/下半砖下面不能挂，脚手架下面能挂。以前写成“上面是完整方块”，会误拆。
    侧面只认“背后那面能贴”或“上面那格是同一面的藤蔓”；以前多认的“旁边那格是同面藤蔓”原版没有（真实图上 220 根这样的藤蔓现在会拆）。
  - **贴墙的钟支撑在 facing 那一侧**（以前反了）；两面贴墙的有一边就行。
  - 睡莲/青蛙卵：下面的流体是**水源**（流动水不行，含水方块可以）；青蛙卵上面放什么都行（原版看的是它自己那格没有流体）。
  - 门：两半互查（下半上面没有上半也拆）；另一半是别种门也留着。床：另一半要是同一种床、part 相反（新加 `RULE_BED`）。
    活塞头：背后要同朝向、伸出来、类型对得上的活塞或 moving_piston（新加 `RULE_PISTON_HEAD`）。
  - 小型垂滴叶下半要有上半；大型垂滴叶茎上面要是茎或叶子；大型垂滴叶不能种在苍白苔藓块上（以前故意留着）。
  - 脚手架照 `ScaffoldingBlock.getDistance` 现算距离（下面的脚手架取它的 distance、下面别的方块顶面整面是 0、旁边的 +1 取小）。
  - 上坡铁轨：坡那一侧的邻居顶面也要带边（`BaseRailBlock.shouldBeRemoved`）。
  - 结果的瓜茎没了果实：变回普通瓜茎 age=7（`FX_SET`），不拆。
  - 苍白苔藓地毯（新加 `RULE_PALE_CARPET`）：挂在上面那截要坐在铺地的那块上，侧面一个个判（能贴、low 面还要下面同一面有），只关不开
    （2026-11-10 起整个照 `getUpdatedState` 重算，会开面，见上一条）。
  - 海带茎也不能长在岩浆块上；高海草下半不能叠在高海草上；仙人掌上面是气泡柱也碎；8 层雪上能放雪（碰撞箱顶面不满，原版单独放行）；
    混凝土粉末不看正下方的水。
  - 形状：中继器/比较器/地毯底面整面；灯笼（放地上的）/蜡烛（含无色 `candle`）/钟/紫水晶/滴水石锥/海泡菜/睡莲/紫颂植株/
    瓶子草作物底面或顶面的中心支撑；梯子/展示架背面整面；门/开着的活板门/内角楼梯只有一个确定的面（不再两个都给）；
    沉重核心只有底面中心、不实心；脚手架/潜影盒/铜格栅不是整块不透明；灵魂沙/泥巴碰撞箱顶面不满；空状态的墙没有中心；
    正着挂的天花板吊牌顶面没有中心；缠怨藤/垂泪藤、铜傀儡雕像、干枯恶魂、活塞（伸出来的）、展示架不再当整方块。
- 还保留的、和 wiki 结论一致而游戏也确认的：栅栏门只是底下不给中心支撑；落叶层要顶面完整；压力板“带边或中心”；
  红石墙上火把要整面；紫颂花不给支撑（只有底面中心，碰撞箱整格所以藤蔓/地衣能贴）；蜡烛不判（`updateShape` 不管支撑，悬着）；
  发光地衣/幽匿脉络/树脂团能贴树叶（碰撞面满）；红石线“下面整面或漏斗”；缠怨藤/垂泪藤藤头藤身互相接。
- 规则表里去掉过的多余特判（[历史]，别加回来）：`F_NO_LEAVES`、`F_AMETHYST`+`set_amethyst`、`F_WIRE` 和 judge_Rim 里写死的
  `"hopper"`、睡莲的 `_coral_block` 排除、`B_ICE`/`B_LEAVES`；这一轮又去掉了 `F_AIR_U`/`F_SRC`（青蛙卵）、`set_water`、
  `is_FullBlock`（藤蔓）。名字模式 `match_Pattern` 支持中间一个 `*`（`dead_*_coral`）。

### 1.7 命名与类型

- 函数一律 `<操作>_<操作对象>`：操作全小写；对象是缩写就全大写（`POI`/`MCA`/`NBT`），
  是单词就首字母大写，是结构体就写结构体名（`set_BlockEntity`、`check_ChunkBase`、
  `equal_BlockRef`）。merge/fixer/rules 文件内的 static 函数、结构体方法也按这个写
  （`is_Dir`、`build_Sec`、`check_Support`、`judge_Below`、`is_InWrite`）。不许大小写两种写法共存。
  demo 和测试程序里的辅助函数也一样（`demo_NBT`、`print_Usage`、`check_Case`、`set_BlockAt`）；仿 STL 接口的方法
  （`push_back`、`erase`、`at`…）和给 Java 调的 `shim_*` 除外。
- 真假一律 `bool`（返回值、参数、局部变量、函数指针），不拿 `int` 当 `bool`；`0/-1`
  表示成败的也改成 `bool`。计数、枚举码（`FX_*`、`GET_*`、`MCA_READ_*`）、三态压缩方式照用 `int`。
- NBT tag 用 `NBT_*` 名字，不写 `0x0a` 这种数字。
- 排版（2026-11-14 统一过一遍）：tab 缩进；`if/for/while/else` 后面一律带花括号、语句各占一行（不写 `if(x)y;`、`if(x){y;}`）；
  `}else{`、`}else if(...){` 接在同一行；`switch` 里的 `case` 和 `switch` 同一级缩进，`case X:` 后面不留空格；死循环写 `for(;;)`；
  运算符、逗号两边不加空格；指针声明 `T *p`；`)const`；注释用 `/* */`。函数定义处都要有一句注释（定义行末尾，或者上一行），
  头文件声明处有详细说明的，定义处写一句要点就行。
- 复用（用户 2026-11-21）：能复用、能抽象就做。库是库、上层是上层：merge/fixer 共用的放 `blockporter.hpp`，插件自己的放 rules.cpp，
  不为了上层大改库（库里加的只能是对谁都有用的通用操作）。
- 目录（用户 2026-11-21）：长期用的测试、demo、工具代码放正式目录，不放临时目录；临时副本、日志这种能任意多次重新生成的一律放 `/tmp`
  （`test/work` 就是指到 `/tmp/nbtrw-work` 的链接；真实地图的副本也放这里）。

### 1.8 依据（[现行]）

依据按这个顺序认，前面的说了算：

1. **游戏本身**：原版服务端 jar（26.x 不混淆）。`test/mcdump.sh` 在 jar 上初始化注册表、绑好原版数据包的标签，
   造一个“自己＋六个邻居”的假世界（动态代理实现 `LevelReader`/`ScheduledTickAccess`），直接调游戏的
   `updateShape`/`canSurvive`/计划刻/`neighborChanged` 判。形状、存活规则、标签成员都以它为准。
   本机有 26.1.1 的 jar（`/home/zyh/mc/26.1.1/server.jar`，用户在跑的服务器目录，只读它）；别的版本要先拿到官方 jar。
2. **反编译源码**：`src-26.1.1/`、`src-26.3/`（用户提供的第三方反编译，只读、不进 git）。用来看懂游戏为什么这么判
   （写模板、写注释）和查版本分界：比如方块状态字段改名在 `DataFixers` 里注册在 5006（`BlockStateFieldNamesFix`），
   目录换布局是 4772（`DimensionStorageFileFix`），库里用的 5009/4774 是 wiki 上的快照号，中间没有发布过的版本，
   对游戏写出来的存档等价；5013 把地形阶段的区块状态合并了（noise/surface→biomes、carvers→terrain，只碰没生成完的区块）。
3. **minecraft.wiki**：只认正常渲染出来的页面 HTML，不用 `?action=raw`/`render`、第三方镜像或博客。用在源码/jar 里
   没有的东西上（26.4 的 `status`/每格 biomes、版本历史、玩家可见的说法）。`test/wiki2md.py` 把页面转成 Markdown，
   原 HTML 存在 `test/work/wiki/`，清单在 `test/wiki_pages.txt`（中文：`--lang zh` 和 `test/wiki_pages_zh.txt`）。
   wiki 和游戏冲突时按游戏；放置表里冲突的格子记在 `test/check_placement.py` 的 `KNOWN` 里（注明“游戏：…”）。

- 代码注释/文档里引用时写出处：游戏类名和方法（`VineBlock.getUpdatedState`）、wiki 页面和节（`Wiki Bell §Block states`）。
- **对拍工具**（改了 rules.cpp 就跑）：
  - `sh test/mcdump.sh <server.jar> shape`：形状位图逐状态对拍，应为 0 处不一致；
  - `sh test/mcdump.sh <server.jar> cmp [rules.so] [方块id,...]`：存活规则穷举对拍，“#差异合计”应为没有；
  - `test/judge.cpp`（直接装 rules.so 判一格）+ `test/check_placement.py`（wiki 放置页三张表 2269 格）：有了游戏对拍以后只当旁证
    （2026-11-09 挪进过 `test/tmp/` 不维护；11-21 挪回来，顺带修了插件装不上时静默报“0 格不一致”；不一致的 52 格都在 `KNOWN` 里）。
- wiki 之间自相矛盾时以前的取舍规则（按历史表、按多数、定不下来取宽松、按表不按通用说法）[历史]：现在有游戏判定，
  不再靠这些推；以前因为“定不下来”取的宽松，这一轮都按游戏定了（见 1.6）。

### 1.9 规则从游戏生成的方案（[计划]，用户 2026-11-14 下半：再设计一下，先不做，最好支持模组方块）

现在 rules.cpp.<版本> 是照游戏判定**手写**的判法 + 一部分**生成**的数据（1.6：音色、树叶标签、材质、默认属性），靠 1.8 的两个对拍工具保证和游戏一致。
目标：每个版本、每套模组只换数据不换代码；手写的只剩“判法模板”。

- **导出器跑在真服务端里**（不是只读原版 jar）：做成一个服务端模组（Fabric），在装好目标模组的服务端里启动时遍历 `BuiltInRegistries.BLOCK`
  的全部方块状态，导出 shapes.tsv 同样的列（六个面的 isFaceSturdy/碰撞面、isSolid/isSolidRender、replace/liquid/burn、under、instr、标签成员、
  默认状态）。模组方块和原版方块走同一条路，名字带自己的命名空间。原版 jar 上的 `ShapeDump` 就是这个导出器的“没装模组”的特例。
- **数据文件不再编进插件**：导出的是一个独立的数据文件（`rules.<版本>[+模组集].tsv` 或二进制），插件启动时读（`Rules_Api` 不变，
  在 `reset_Rules`/第一次 `fill_BlkInfo` 时按插件旁边的同名数据文件加载）。这样一个 `rules.so` 配不同的数据文件修装了不同模组的存档；
  名字只按完整的 `命名空间:名字` 查，不再用 `base`（模组方块和原版同名不会串）。
- **判法怎么选**：每种方块要用哪个判法模板（下面要支撑、贴一面、两格高、藤蔓、连接类…）从方块的 Java 类推：导出器同时导出每个方块的类名和
  父类链（`FenceBlock`、`WallBlock`、`VegetationBlock`、`FaceAttachedHorizontalDirectionalBlock`…），模组方块继承原版类的就自动用同一个模板；
  模板的参数（看哪个标签、要整面还是中心、`mayPlaceOn` 的集合）从 `SurviveProbe`/`FitProbe` 那套“只换一个邻居看游戏判不判死”的探测里拟合
  （`test/mc/probe/` 的原型）。拟合不出来的（自己写了 canSurvive 的模组方块）标成“不管”，宁可漏拆。
- **核对**：`RulesCmp` 也在装了模组的服务端里跑，模组方块同样穷举六邻；差异按方块列出，人工决定写进 known 还是改模板。
- 分三步：① 导出器做成模组、数据文件和插件分开（原版先跑通，生成的表和现在手写的逐项相同）；② 规则表里能从类名 + 探测推出来的行改成生成的，
  手写的规则表只留特判；③ 装几个常见模组（比如有自定义栅栏/树叶/植物的）跑一遍，看模板覆盖多少。
- 原则不变：rules 只看六个邻居；改动只能是 rules.hpp 文件头那三种；六邻看不到的取宽松。

### 1.10 游戏读档报错表（26.1.1 源码，[现行]）

用户 2026-11-07 定的判据：**游戏读档会报 ERROR/WARN 的按最小单位删，不报的暂时不删不改**（以后能替游戏补全了再补）；
`xPos/zPos`、方块实体坐标、实体 `Position` 按真实位置重写；名字认不认识（要注册表）、方块实体和方块对不对得上先不做。
依据都是 `src-26.1.1` 里的类（只读）。“库”一列是库读的时候怎么取（merge/rules 看到的就是这个）。

| 位置 | 情况 | 游戏 | fixer |
|---|---|---|---|
| region 文件 | 0 字节 | 当空区域，不报 | 删文件（等价） |
| region 文件 | 不满 8KB | WARN truncated header，照读读到的部分 | 库补 0 读（`truncated`），format 时写全 |
| 表头一格 | 偏移<2/扇区数0/偏移在文件外 | WARN，当没有 | 当没有（`MCA_READ_NO_CHUNK`），遍历跳过，整理文件时清成 0 |
| 区块数据 | 不够 5 字节/长度 0/长度负数/超出扇区/外置文件不在/类型号不认识 | ERROR/WARN，当没有（重新生成） | 删这一格 |
| 区块数据 | 类型 127 自定义压缩 | ERROR，当没有 | 模组的：只记日志，不删 |
| 区块数据 | 压缩流后面坏了 | 读完根 compound 就停，不报 | 库读前缀，不算坏 |
| NBT | 嵌套 >512 层、字符串/键名不是 modified UTF-8 | 整块读失败 | 删那个 tag（`prune_NBTUnreadable`） |
| 区块根 | 没有 `Status` | ERROR missing level data，重新生成 | 不是 full：删（老规矩） |
| 区块根 | `xPos/zPos` 不对/没有 | ERROR wrong location，照载 | 按文件位置重写 |
| 区块根 | `InhabitedTime` 没有/不是数值 | `getLongOr` 当0，不报 | 当0：整块在范围内就删 |
| 区块根 | `DataVersion` 没有 | 当 -1，从头升级 | 库当最新格式读；fixer 不认识、跳过不动 |
| `sections` | 不是 List / 元素不是 compound | 当没有段/跳过，不报 | 不动（库读的时候当读不出来，merge 跳过、规则 blind） |
| 段 | `Y` 不是 Byte / 没有 / 重复 | `getByteOr(0)` 截断、后面覆盖前面，不报 | 库照游戏取段；`format_Chunk` 改成 Byte、留最后一段 |
| 段 | `Y` 超出维度高度 | 方块不读（光照照读），不报 | 不动 |
| `block_states` | 不是 compound | 当全空气，不报 | 不动（库当读不出来） |
| `block_states` | palette 缺/不是集合/空；多条没 data（data 不是数值集合也算没有）；data 长度不是正好；下标越界 | 整块载入失败 | 删这一段的 block_states |
| `block_states` | data 是 Int_Array/Byte_Array/数值 List | `getLongStream` 照读，不报 | 不动（库不原样读写：当读不出来） |
| `block_states` | 调色板只有一条还带 data | 单值调色板，data 不看 | 库也不看 data |
| `block_states` | 调色板条目认不出名字；palette 是数组/元素类型不对 | ERROR Recoverable，换空气 | 换成空气条目（整条不对就换成 n 条空气） |
| 老格式段（<2832） | `Palette`/`BlockStates` 少一样 | 升级成空气段，不报 | 不动 |
| 老格式段 | 调色板一条 | 升级时扔掉 data（长度不对也行） | 库也不看 data，写的时候按整段 palette[0] 重建 |
| 老格式段 | 2527 以前、位宽不是 2 的幂、长度不对 | 升级时补齐/截断，照读 | 不动（库当读不出来） |
| 老格式段 | data 比调色板要的长 | 升级时调色板补空气；补完长度对得上就照读（下标落在补的那段是空气），对不上整块载入失败 | 对得上不动（位宽变了库当读不出来）；对不上删 Palette+BlockStates |
| 老格式段 | 长度不对（上面两种以外）、下标越界（补完还越界）、调色板空 | 整块载入失败 | 删 Palette+BlockStates |
| `biomes`（<5119） | 同 block_states 的致命条件（64 格、位宽 ceil(log2)） | 整块载入失败 | 删这一段的 biomes |
| `BlockLight/SkyLight` | 长度不是 2048 | 整块载入失败 | 选中区块和邻居本来就删光照 |
| `Heightmaps` | 长度不对 | WARN，重算 | 选中区块本来就删 |
| `block_ticks/fluid_ticks` | 不是 List | ERROR Failed to read field，当空 | 删成员 |
| 刻 | i 写法不合法 / x,y,z,t,p 不是数值 | ERROR，这一条不要 | 删这一条 |
| 刻 | 坐标是 Short/Long/Double | Codec.INT 照读 | 照留 |
| 刻 | 不在本区块 | 静默丢掉 | 在范围里的迁到坐标所在的区块（目标没有就删，目标已有同样的就删这条，读不出来/目标自己位置也不对留着） |
| `block_entities` | 不是 List / 元素不是 compound | 当没有/跳过，不报 | 不动 |
| 方块实体 | 没有 id / id 写法不合法 | ERROR，丢掉 | 删这一条 |
| 方块实体 | 坐标在别的区块 | WARN，折回本区块（x/z 取低 4 位） | 在范围里的迁到坐标所在的区块（同上）；没有 x/z、跟着挪了位置的区块走的照游戏折回 |
| 方块实体 | 同一格几条 | 后放的顶掉前面的，不报 | 不动（库取最后那条） |
| 方块实体 | id 不认识 / 方块不支持这种方块实体 | ERROR/WARN，丢掉 | 先不做（要注册表） |
| `structures` | References/starts 名字写法不合法 | WARN/ERROR，丢掉 | 删这一项 |
| `structures` | References 指到 8 区块以外 | WARN，丢掉 | 删这个坐标 |
| 实体区块 | `Position` 坏/没有/对不上 | WARN/ERROR，实体照载 | 按文件位置重写 |
| 实体区块 | `Entities` 不是 List / 元素不是 compound | WARN，当没有/跳过 | 删成员/删元素 |
| 实体 | id 没有/写法不合法 | WARN Skipping Entity | 删这一条 |
| 实体 | UUID/Pos/Motion/Rotation 形状不对；Pos 有 NaN；Rotation 不是有限数 | WARN/ERROR | 删这一条 |
| 实体 | 没有 UUID / 没有 Pos | 随机 UUID / 原点，不报 | 照留 |
| 实体 | UUID 重复 | WARN，后加的加不进去 | 删后出现的 |
| 实体 | 乘客坏 | 只丢那个乘客 | 只删那个乘客 |
| POI 区块 | 段不是 compound / 没有 Records / Records 不是 List | ERROR，整段不要 | 删这一段 |
| POI 区块 | 段键不是数字、`Sections` 不是 compound | 不按它找/当没有，不报 | 不动 |
| POI 记录 | pos 不是 3 个数 / type 写法不合法 | ERROR，这一条不要 | 删这一条 |
| POI 记录 | 没有 free_tickets | `orElse(0)` | 照留（当0） |

数值字段游戏大多不管具体是哪种数值 tag：`getIntOr`/`getLongOr` 走 `NumericTag.intValue()/longValue()`（库的
`get_TagInt`/`get_TagLong`：long 取低 32 位、浮点转 int 向下取整），codec 读的字段走 `Number.intValue()`（`get_NumberInt`：浮点向零取整）。
名字写法照 `Identifier.tryParse`（`check_Identifier`）。

## 2. 历史踩坑（只读，不要当规范）

### 2.1 各轮记录（新的在前）

真实存档上的前后对比都在副本上跑（`now/new/old` 是 ±1536 全高的三份，`/home/zyh/mc/world-26.1.1` 的副本是三个维度全范围）。

- **`2026-11-21` 复查**（用户：“重新阅读此项目，复习项目规范与文件作用，删去失效文件，修复暗藏 bug 与不符合规范的地方”）：
  把 11-14 全量检查以后加的代码（遍历/范围、迁移、跨版本 merge、POI/Entity 结构体）按 1.7 的规矩和游戏源码再过一遍。
  - 修的 bug（每条都加了用例，在上一次提交 `63df408` 的代码上跑过，确认会失败）：
    1. fixer 迁错位记录时先在缓存里把**目标区块**的 xPos/zPos 改了，却不标脏：轮到目标区块修 NBT 时看不出它挪过位置，
       跟着它挪的方块实体被当成“指到别处”留在原坐标上（游戏会折回到不相干的格子），xPos 的修正也可能根本没落盘。
       现在目标区块自己位置不对时这条记录先留着（下一趟目标修好了再迁），不替它改。
    2. fixer 实体查重自己写了一份 UUID 读法，只认 Int_Array 和数值列表；游戏 `UUIDUtil.CODEC` 走 `Codec.INT_STREAM`，Long/Byte_Array 也认，
       这样写的重复 UUID 查不出来。改用库的 `get_IntStream`。
    3. merge/fixer 判实体在不在范围里只认浮点的 `Pos`；`Vec3.CODEC` 认任何数值，Int 写的 `Pos` 被当成坏坐标：merge 不管在哪都搬，
       fixer 范围外的也拿来查重。改成 `get_NumberDouble`（merge 用 `Entity::get_Pos`）。
    4. merge 搬 now 的 POI 时，now 自己同一格有几条，留下的是最后一条；游戏 `PoiSection.add` 同类型留先来的。现在照游戏取。
    5. `list_MCAFiles`（和外置 `.mcc` 的路径）认 `r.01.0.mca`、`r.+1.0.mca` 这种游戏不读的文件名，还和 `r.1.0.mca` 算成同一个区域：
       merge/fixer 会把这个区域走两遍（merge 第二遍把计划刻/实体再追加一遍、`InhabitedTime` 清成 0）。现在只认游戏拼得出来的写法。
  - 不合规范的：`chunk.cpp` 里新加的 POI/实体函数、`atom.cpp` 的 POI/Entity 方法、几个构造函数定义处没有注释，补上；fixer 里只转一手的
    `get_POIPos`/`get_POIRecord` 去掉、直接调库；fixer 自己的 `get_EntityUUID` 和库的同名函数重载，改名 `get_UUIDKey`；
    `add_ChunkTick` 会顺带删表里读不进来的条目、同一格同名不重复加，头文件里补上；merge/fixer 文件头“先走区域坐标”的说法过时，改掉。
  - 失效文件：`test/data/lvlworld/region`（指向 `dimensions/minecraft/overworld/region` 的符号链接，老布局时代留下的；4788 的存档
    库只认 `dimensions/`，没人读它）删掉；`lib.sh` 里 `nbtrw nbtw test/work/3.txt` 那一步，`3.txt` 早就没了、一直没跑，
    改成拿 `test/data/level.dat` 解压出来跑；根上 `.gitignore` 里早就不编的 `prune`/`libnbt.so`/`chunk`/`dat`/`mca`/`nbtw` 去掉。
  - 同一天用户纠正了五条规矩，照着改了：
    1. 楼梯不是约束，以游戏为准 → 楼梯形状进了 4.1 的待做；
    2. 光照也严格按范围（“3×3 邻居删光照”是给 chunkunit 定的，已废弃）→ `cleanup_Light` 只走范围碰到的区块（`block.sh` 加了范围外邻居 (-1,0) 原样的用例，旧代码会删它的光照）；
    3. 测试脚本由开发者自己定，A/B、写死期望和依据都只是建议（`test/ReadMe.md` 约定改了）；
    4. 长期用的代码不放 tmp、能重新生成的一律放 `/tmp` → `test/tmp/` 里的 `judge.cpp`/`check_placement.py`/`SurviveProbe`/`FitProbe` 挪回 `test/`、`test/mc/probe/`，
       `test/work` 改成指到 `/tmp/nbtrw-work` 的链接（`test/work.sh`），原来 2G 的内容搬过去；挪的时候发现 `check_placement.py`
       在 `./rules.so` 不在时 judge 退出、脚本不看退出码，报“比了 2269 格、不一致 0 格”——改成默认用 `rules.so.26.1.1`、judge 失败就报错；
    5. 能复用就复用，库是库、上层是上层 → merge/fixer 里重复的进度条、目录、列表/坐标解析、维度去重、范围结构体、判实体在不在范围里
       抽进 `blockporter.hpp`（`Merge_Range`/`Fix_Range` 合成 `Block_Range`，`is_InWrite` 统一叫 `is_Inside`），库没动。
  - 验证：见 3.。
- **`2026-11-14` 下半：遍历和范围重做、外观属性、26.3 规则**（用户：merge/fixer 先遍历 region、区块不存在或偏移异常跳过；format 时异常偏移置 0；
  外观属性要判；重力方块先直接删；规则生成再设计、要支持模组；装了 libubsan；format_Chunk 挪到 format_MCA、只跑一遍；pos 指到别的区块的迁移；
  去掉 chunkunit、都按最小单位上取整；做 26.3 的规则，rules.cpp 按版本分文件、Makefile 通配、不自动链接；fixer 最后一个参数是规则路径；跨版本 merge 不做）：
  - 范围/遍历（1.4、1.5）：去掉 chunkunit；方块级严格按 x/y/z，区块级按碰到的区块（不看 y），区域文件级按碰到的文件。先列目录里实际存在的区域文件
    （库新加 `list_MCAFiles`）再和范围求交——第一版照“遍历区域坐标”写的，范围给到 ±3000 万时有 1.4×10¹⁰ 个区域坐标，真实地图上一跑就转不完，改掉了。
  - 表头偏移异常照游戏当“没有”（`has_Chunk` 假、`MCA_READ_NO_CHUNK`、`has_BadOffset`/`clean_Offsets`），`format()` 先清成 0；数据坏的格子不再让 `format()` 失败（1.2）。
  - fixer：`migrate_Strays` 迁移错位的方块实体/计划刻；`format_Chunk` 只在收尾整理文件时做一遍（光照、计划刻阶段不再重复整理、改坐标）；
    `is_ChunkCovered` 去掉（删区块不看 y）；规则插件是最后一个参数。
  - 规则：`rules.cpp` → `rules.cpp.26.1.1`，新加 `rules.cpp.26.3`（DataVersion 5023）；外观新加蘑菇方块、音符盒、树叶三条规则，石锥粗细、脚手架 distance/bottom
    在原来的规则里补上（1.6）；音色/树叶标签/材质表/默认属性改成从 jar 生成（`test/gen_tables.py`）；插件补默认属性（26.3 存档默认值不写）；
    红石线连接要看斜对角，不做。对拍工具：`ShapeDump` 加 instr/leafkeep/default 三列、硫磺尖刺的偏移行；`McWorld` 模拟树叶/脚手架的刻；
    Java 工具在 26.1.1 和 26.3 两个 jar 上都能编（26.3 改了红石线类名、原版数据包接口）。
  - 规则生成方案按“导出器做成服务端模组、数据文件和插件分开、按类名选判法模板”重新设计（1.9），这一轮没做。
  - 验证：`lib.sh`（571+34+75）、`block.sh`、`rules.sh`（473 条）、`format.sh`、`safe.sh`（这次带 UBSan）全过；全部源文件 0 警告。
    `mcdump.sh shape`：26.1.1 的 29873 个状态、26.3 的 35723 个状态都 0 处不对；`cmp`：26.1.1 全量 470 种方块 1.94 万个状态 31 亿配置 0 差异，
    26.3 全量 492 种方块 2.17 万个状态 46.5 亿配置 0 差异（20 核 34 分钟；岩浆有 2 个配置游戏要查 environmentAttributes，假世界没实现，流体本来就不管）。
    真实服务端预生成的图（`test26.1.1`/`test26.3`/`test1.18.2`，都在副本上跑，范围 ±3000 万格、三个维度）：预生成的图 `InhabitedTime` 全是 0，
    fixer 照规矩整张删掉，所以副本先把 `InhabitedTime` 改成 1 再跑两遍：26.3 图第一遍 348 秒、峰值 330MB，主世界拆 77.8 万格（沙砾 64 万、沙子 12 万）、
    改状态 313 万格（树叶 distance 311 万：世界生成留下的 distance 和六邻对不上，存档里同时排着游戏会处理的树叶刻；滴水石锥粗细 929）；
    第二遍三个维度都 0 拆 0 改、0 删区块、0 失败（168 秒）。26.1.1 图同样两遍，第二遍 0 改动（拆的格数和上一次提交一样，多出来的是树叶/石锥/蘑菇）。
    1.18.2 图（老目录布局、DataVersion 2975）now/new 是同一张图的两份，整个世界 merge 185 秒、搬了 0 格；fixer 配 26.3 插件按版本不对拒绝。
- **`2026-11-14` 全量检查**（用户：“全量阅读并检查代码，统一代码格式并补全注释，发现并修复已做部分暗藏的 bug，整理缺陷与下一步”）：
  - 排版/命名统一（规矩记在 1.7）：一个脚本把无花括号的 `if`（179 处，多数在测试里）、单行花括号（47 处）、`switch` 里 `case` 的两种缩进
    （各占一半）、`case X: `/`) const`/`while(1)` 都改成一种写法；`demo_nbt`→`demo_NBT`、`usage`→`print_Usage`、测试里的 `ck`/`setb`/`mk`…
    改成 `<操作>_<对象>`。改完每个源文件编出来的代码段和只读数据段和改之前逐字节一样（只有 `nbt.cpp` 把 `comp_Copy::make`
    换成普通函数 `copy_AnyMap`，内联方式变了）。定义处没有注释的函数约 350 个（库/程序约 240、测试约 110）都补了一句；
    几段放错位置、说法过时的注释挪正/改掉（`check_Chunk` 的说明、“调色板压紧”、重力方块的旧规矩、规则家族的归属…）。
  - 修的 bug（除第 9 条外都加了用例；第 2、3、7、8 条的用例在旧代码上实际跑过，确认会失败）：
    1. `nbt_Raw::read_List` 读坏以后不停手，还按文件里的长度预留内存：每层都谎报长度的嵌套 List，20KB 要 0.9 秒、平方增长，
       50MB 的直接 `bad_alloc` 崩掉。现在读坏就停、不预留，同样的 50MB 0.002 秒返回（`bad.cpp` 加了用例，每例加了 300 秒超时）。
    2. `MCA` 原地写没排除表头扇区：表头偏移是 1 的坏格子，写入时把时间戳表当数据区覆盖，写完游戏照样不认。
    3. `MCA::has_Chunk`/`is_Empty` 只看 3 字节偏移：偏移 0、扇区数非 0 的坏格子“没有”却清不掉，fixer 每次 format 都失败。
    4. `Chunk::take_Index` 漏了建索引时的 DataVersion/格式：每个从 MCA 读出来的区块段表白建第二遍。
    5. `Level::set_Chunk(dim,chunk)` 从根上取 xPos/zPos，2842 以前带 `Level` 外壳的老区块用不了。
    6. `nbt ==` 对 NaN 不相等：带 NaN 的树和自己的深拷贝都不相等，merge 会把这种方块实体所在的格子当成改过。
    7. merge 的区域范围算宽了 16 倍（方块坐标/32 当成了区域坐标）：范围外有文件的区域全被读一遍，读不出来的还让整趟报失败；
       y 范围很大时段循环空转（现在只走 -128..127）。
    8. fixer 判“整块在范围内”把只有光照的段也算上：1.17 及以前的区块永远删不掉全空气/InhabitedTime 0 的。
    9. fixer 的 POI 阶段，区块因为缓存/落盘失败取不到时被当成“没有要 POI 的方块”，会删记录。
    另外清掉了 fixer 里两段走不到的代码：POI 整块重建（`shape_ok` 恒真）、靠 `m.bad` 认 0 字节 POI 文件（库照游戏读以后不再是 bad，
    改成直接看文件大小）。
  - 没修、要用户定的：NBT 嵌套太深爆栈（4.、4.1）。
  - 验证：`lib.sh`（569+34+74）、`block.sh`、`rules.sh`、`format.sh`、`safe.sh` 全过；`mcdump.sh region/upgrade/shape` 0 处不对；
    `rules.cpp` 只动了注释和排版，编出来的代码段和改之前一样，所以没有重跑 31 亿配置的 `cmp`。
    真实存档副本上新旧程序各跑一遍（`test/work/ab5/`）：26.1.1 图三个维度全范围跑 fixer，162 个区域/POI/实体文件逐值相同；
    `now`/`new`/`old`（±1536）跑 merge 再跑 fixer，34 个文件逐值相同；两边的日志逐行一样，用时没变（fixer 68s→69s，merge 41s→41s）。
- **`2026-11-11` 同类的外观都照游戏重算**（用户：“做掉同类的”）：新加 `RULE_BARS`（玻璃板/铁栏杆）、`RULE_GATE`（栅栏门 `in_wall`，以前“不管”）、
  `RULE_CHEST`（箱子配对、铜箱子两半对齐，方块实体照留）、`RULE_SNOWY`（草方块/灰化土/菌丝），紫颂植株活得下去时重算六个方向的连接。
  对拍：455 种方块 31.4 亿配置 0 差异（拿两处故意写错的规则试过，对拍能抓出来）。`block.sh` 加了“大箱子另一半没了变单箱、东西还在”。
  真实 26.1.1 图上和上一次提交比：主世界 218 格草方块 `snowy=false→true`（上面都是细雪：雪坡生成时没更新），末地 5 格紫颂植株
  （下面那截被紫颂规则拆了，跟着断开）；下界没变；玻璃板/栅栏门/箱子在这张图上没有要改的；第二遍 0 改动。
- **`2026-11-10` 栅栏/墙/苍白苔藓地毯的连接照游戏重算**（用户 2026-11-07 同意做这三类、楼梯不做；11-10：每格只看 6 邻是硬约束，
  “只关不开”只是防死循环的建议，能保证不死循环就照游戏开面/连上）：新加 `RULE_FENCE`/`RULE_WALL`，`RULE_PALE_CARPET` 改成整个照
  `getUpdatedState` 重算（以前只关不开）；形状位图加 UNDER 五位（墙看上面那格碰撞箱底面盖住哪几块），竹子/滴水石锥按坐标现算偏移；
  名字类别记在 `CK_*`。收敛的证明改写成“看的东西不成环”（rules.hpp 文件头）。`RulesCmp` 对这三种比完整状态，ShapeDump 加 under 列和 @off 行。
  真实 26.1.1 图上和上一次提交比：主世界差 202 格，全是栅栏和墙（规则统计：栅栏改 151 次、墙 56 次）——143 格是游戏生成时就没更新过的
  （废弃矿井里挨着石头/原木的栅栏存的是都不连、沙漠建筑里上面压着砂岩的砂岩墙存的是 low…，游戏里旁边一更新也会这样改），
  59 格是旁边悬空的沙砾被重力规则拆了跟着断开；下界多判了 3605 格栅栏，一格都不用改；末地没有这类方块；第二遍 0 改动。
- **`2026-11-09` 整理**：`make` 全编（`make nbtrw`/`make blockporter` 各编一半，`make clean` 改名 `make clear`）；
  过时的、临时的测试工具挪进 `test/tmp/`（wiki 放置表对拍 `judge.cpp`+`check_placement.py`、规则生成原型 `SurviveProbe`/`FitProbe`）；
  去掉没用到的函数，库/程序/测试编译都没有警告。
- **`879117b` 多版本**（用户 2026-11-07/08：libnbtrw 做多版本，merge 暂时只合同一个版本，fixer 只要求“皮”支持多版本，规则要哪个版本现做）：
  区块的字段位置/键名按 `Chunk_Layout`（1451～2842 各个分界照 26.1.1 的 DataFixers），段的方块状态照游戏读档的样子判
  （`get_StatesView`：老区块游戏先升级再读，库照着算 BitStorageAlignFix/ChunkHeightAndBiomeFix 的补齐和补空气），
  `test/mcdump.sh upgrade` 把 6 个版本 × 32 种段（正常的、长了短了、位宽不对、下标越界、类型不对…）交给游戏升级+读，库判坏和逐格读法全对上
  （只差“游戏不认识的名字换空气”，库没有注册表）。顺带：新格式 data 是 Int_Array/数值 List 游戏照读（以前库判坏、fixer 会删）；
  fixer 跳过 1451 以前/没写 DataVersion 的区块，规则只判 DataVersion 和规则一样的区块（真实存档里有 2 块 4325 的老区块），
  POI 文件版本不一样也不动。逐格读时 DataVersion 读了三遍、压调色板逐格做除法，fixer 慢了 22%，改掉了。
- **`302b1ac` fixer 按游戏读档的报错判坏**（用户 2026-11-07）：报 ERROR/WARN 的按最小单位删，不报的不动（判据是 1.10 的表）；
  以前按库自己的类型检查判（见 2.2）。新加“修 NBT”阶段（`repair_Chunks`，在物理之前），规则看到的是修好的区块。
  真实存档副本上新旧 merge+fixer 输出逐值相同。
- **`8baffec` MCA 支持 lz4 和外置 `.mcc`**（用户 2026-11-07：压缩优先用库）：读一格照游戏的 `RegionFile`（读完根 compound 就停），
  `sh test/mcdump.sh <jar> region` 和游戏互相读对方写的四种压缩 + 外置；类型 1/2 不再互认。以前 lz4/外置当“压缩类型不支持”跳过。
  真实存档副本上新旧 merge+fixer 输出逐值相同。
- **`ae1f744` 段的 Y 照游戏读**（用户 2026-11-07）：`getByteOr("Y",0)` 截断、缺了当 0，重复段取最后一段，只有 fixer 才修成合法
  （以前 Y 不是 Byte 删段、重复段留最前面那段）；merge 不补段（以前写方块时缺段补一段空气）、暂时只合同一个版本。
  真实存档副本上新旧 merge+fixer 输出逐值相同。
- **`59aba73` 游标拆进 Chunk**（用户 2026-11-07）：`Chunk_Cursor` 拆进 `Chunk`——段表/palette/data 各 256 格（段 Y 按 uint8 存）+
  方块实体“压缩坐标→下标”索引，`reset()` 改名 `reindex()`（强制重建）；section 级函数都收 `Chunk*`；
  `Level` 的缓存全按值持有，dat/MCA/chunk 都能 pin（钉住的指针一直有效，没钉住的每次 `get_*` 现取）。
  调色板名字/属性指针的缓存挪进 `Section_View`（拆掉它 merge 29s→36s，放回来 28s）。同一套副本上新旧输出逐值相同，fixer 不变。
- **`71fa1fd` API**（用户 2026-11-05）：`nbt_*` 的 `data()` 给 `con` 的指针、`as_*` 转不了给 nullopt（`as_Str`→`as_String`，
  加数组的 `as_Byte_Array/as_Int_Array/as_Long_Array`）、具体类型隐式转成自己的 C++ 类型；删掉重复的 `is`/`is_Equal`/`compare`/`!=`；
  `Chunk` 不再继承 `nbt_Compound`，改成 `con`（按值持有）+ 读写缓存，能 O(1) 读的值不缓存，去掉 serial/版本号；
  `MCA` 只读写 `nbt_Compound`（`get_Chunk`/`set_Chunk`/`MCA::read/empty` 删掉）。为了不变慢加了 `Section_View`（按段逐格读）。
  真实存档副本（now/new/old，±1536 全高）上新旧 merge+fixer 输出逐值相同；merge 38s→29s，fixer 不变。
- **`d799af7` 自定规矩改回照游戏**（用户 2026-11-05：以前自定的规矩除霜冰外都按源码与服务端）：重力方块用 `FallingBlock.isFree`
  （下面可替换方块/液体都拆）、紫颂植株第 1 条照判；新加门两半照对方改、藤头/藤身/垂滴叶/竹子的外观转换、火（`RULE_FIRE`）、
  下界传送门（`RULE_PORTAL`）。可替换/液体/可燃三种性质从 jar 导出（`ShapeDump` 的 replace/liquid/burn 列），shapecmp 逐状态核对。
  `RulesCmp` 对门/火/藤头藤身/竹子/垂滴叶按完整状态比（别的方块只比面属性）。对拍：365 种方块 12.9 亿配置，已知差异只剩霜冰和流体。
  真实 26.1.1 图和上一版比：主世界差 851 格（742 格沙砾 + 5 格沙子压在可替换方块上被拆，连带 70 多格发光地衣/藤蔓；33 根洞穴藤蔓藤身变藤头），
  下界 28 格（25 根垂泪藤藤身变藤头、3 格沙砾），末地 6 格紫颂植株；第二遍 0 改动。
- **`276d470` 以游戏为准**：拿原版 26.1.1 服务端 jar 让游戏自己判（`test/mcdump.sh`，见 1.8），rules.cpp 的形状表
  29873 个状态逐个和游戏一致，存活规则穷举 12.9 亿个“自己＋六邻”配置和游戏一致（故意不一样的单列）；顺带修了藤蔓挂顶
  的方向、墙钟方向、睡莲/青蛙卵只认水源、门/床/活塞头/苍白苔藓地毯两半互查等（见 1.6）。规则生成方案见 1.9。
  真实 26.1.1 图（`/home/zyh/mc/world-26.1.1` 的副本，三个维度）上和上一版比：主世界差 231 格——多拆 220 根只挂在别的藤蔓旁边的
  藤蔓和 8 格长在岩浆块上的海带，少拆 1 个贴墙的钟、1 块地毯和它下面的幽匿脉络；下界/末地一样。
- **`821bdf5`/`0827039` 跨版本通用化**：库/merge/fixer 的存档格式全按 DataVersion 判断
  （26.3 快照 7 的新调色板写法、26.4 快照 1 的 `status` 和每格一个的 biomes）；插件接口去掉插件内部的东西和
  fixer 里的方块知识（ABI NBR7）。`test/format.sh` 把同一批场景放进 4788/5119 两种格式，fixer 修完逐格一样。
  同一轮后半段按中英文 wiki 对照把规则又核了一遍（取舍规则见 1.8），加了 `judge.cpp` + `check_placement.py`（现在在 `test/`）
  把英文放置页三张表 2269 格逐格对拍，并去掉了几处多余的特判（见 1.6）。真实 26.1.1 图上和 `821bdf5` 比：主世界只差 4 格
  （3 条藤蔓关掉贴台阶/活板门底面的那一面、古城内角楼梯背面 1 格幽匿脉络留下），下界/末地一样，第二遍 0 改动。
- **`7dd6515` 按 wiki 全量复核**：rules 的支撑判定改成按面的位图（`Blk_Info.shape`），
  对照 wiki Opacity/Placement 三张表和各方块页面逐条改，ABI 升到 NBR6；文件格式/默认值/标签/POI 表核对无误。
  真实 26.1.1 存档上和上一版比：主世界多拆 768 格（全是海底沙砾被重力规则拆掉后跟着塌的珊瑚/海泡菜），
  古城楼梯背面的幽匿脉络不再误拆；下界/末地的物理结果完全一样；第二遍 0 改动。
- **`443373a` 清理旧 API + 规范对齐**：旧测试/旧 Python 原型/C 参照全部删除或迁移，命名统一成“操作_对象”、int 当 bool 全改成 bool，
  rules ABI 升到 NBR5；顺带修了三个 bug（merge 维度重复跑两遍、文件不在的 MCA 被当成读不出来、
  sections 元素类型坏的邻居被当空气）。改名/改类型前后在真实 26.1.1 存档（10925 个区块）上
  merge+fixer 输出逐区块逐值相同（`test/cmp_mca.py`），只有 merge 的“坏实体”计数从 6844 变成 0。

### 2.2 旧实现与教训

以下都是旧实现或旧文档里出现过的东西，标 `[历史]`：

- `[历史]` `Chunk_Cursor`（2026-11-05～11-07）：`Chunk` = `con` + `mutable Chunk_Cursor cur`，section 级函数收
  section 指针 + 游标（或临时游标）。11-07 按用户的意思拆进 Chunk（段表/palette/data 各 256 格 + 方块实体下标索引），
  section 级函数改收 `Chunk*`；`reset()` 改名 `reindex()`（强制重建）；`Level` 的 `unique_ptr<Chunk>`/`nbt_Compound*`
  改成按值，dat/MCA 也能 pin。
- `[历史]` `Chunk` 继承 `nbt_Compound`、带全局只增的 `serial` 和 `format_generation`/`be_generation`，
  游标在 Chunk 外面（`Level::Chunk_Entry::cur`、merge/fixer 自己的 `Chunk_Cursor`），靠 serial 认区块、靠版本号认改动；
  游标还缓存生成状态/格式/xPos/zPos。`MCA::get_Chunk/set_Chunk`、`Chunk::read`、`nbt_Any::is/is_Equal/compare/!=`、
  `as_Str`、深拷贝的 `data()`、转不了给 0 的 `as_*`：2026-11-05 全部改掉。
- `[历史]` fixer 的 `Fix_Store`：深拷贝区块 + 自己的 LRU。已删除；缓存只在 `Level`，
  fixer 用 `pin_Chunk` 钉 3×3。
- `[历史]` rules 读斜下方/10 个方向连锁/`nb_Below_Of`。现行只给六个面、只排六方向；
  紫颂植株因此保留“旁边有紫颂”的横枝（漏拆，不误拆），甘蔗邻水也看不到。
- `[历史]` merge 删 `isLightOn`/`Heightmaps`、跳过区块清 `InhabitedTime`、`old=""` 只统计、
  POI “now ∪ new 去重/逐条核对方块”，之后（～2026-11-20）POI 只追加。现行 merge 全不删、空 old 照常写、POI 一格一条。
- `[历史]` `prune` 是独立程序；已并进 fixer。2026-10-25～11-08 `make` 只编库+demo、merge/fixer 要 `make blockporter`，
  清理是 `make clean`；11-09 起 `make` 全编、清理是 `make clear`。
- `[历史]` NBT 深度 256 上限、`nbt_bad_*`/`err_cnt` 全局计数、解压 256MB 上限、
  y 限制 ±4096、`CACHE_MAX`/`1<<22` 区域数上限。现行库没有这些限制；错误由调用方处理。
- `[历史]` 旧 `chunk.cpp` 的 `get_POIType/get_POITicketMax` 和 POI 表。已删除，权威表在
  `rules.cpp`；外部若还链接这两个符号会断。
- `[历史]` `RULES_ABI=0x4e425233`（NBR3）、单独的 `blockporter_version`；之后的 NBR4
  （`0x4e425234`）用 `rules_Entry`/`poi_Get`/`blk_Fill`/`judge`… 这套名字、`int blind`。
  之后 NBR5（bool、改名）、NBR6（`Blk_Info` 加 `shape`），现行 NBR7，旧插件必须重编。
- `[历史]` NBR6 及以前 `rules.hpp` 里放着插件内部的东西（`S_*` 支撑档、`BF_ICE/BF_MAGMA/BF_LEAVES/BF_WATER_SRC`、
  `Blk_Info.lev/def/set_mask/shape` 和 `fill` 方法），rules.cpp 一换建模就得动 ABI。插件拆调色板条目
  （`get_Name`/`get_Prop` 直接读 `Name`/`Properties`），造新状态手写 NBT。fixer 里写死了“含水方块拆了写
  `minecraft:water` level=0 + t=5 水刻”和一份“算水的方块”名单，流体刻只认 `minecraft:water`，
  `minecraft:flowing_water` 的刻会被当成对不上删掉（26.1.1 真实存档里碰巧没有）。
- `[历史]` 库只认 `Name`/`Properties` 和 `Status`：26.3 快照 7 以后的存档，`check_ChunkBase` 判不了字符串
  调色板、`get_ChunkStatus` 找不到 `status`，fixer 会把所有区块当“没生成完”整块删掉。
- `[历史]` 旧测试 `test/run.sh`/`conv.sh`/`rules26.sh`、`mk.cpp`/`mkfix.cpp`/`mkpoi.cpp`/
  `mkprune.cpp`、`check.py`/`check_big.py`/`check_del.py`/`check_nl.py`/`check_poi*.py`
  （除 `check_poi_scan.py`）/`corrupt_chunk.py`、`test/c/` 的 C 参照、`pal.cpp`/`props.cpp`，
  以及 nbtrw/ 下 Amulet 版的 `fixer.py`/`merge.py`/`rule.py`/`world_api.py`（仓库根上还留着原型）：
  已删除。`bad.cpp`、`safe.sh`、`mkrule.cpp`+`check_rules.py`（→`rules.sh`）按现行 API 重写；
  `pal.cpp`+`props.cpp` 合并成 `palstat.cpp`。旧 bad.cpp 的“坏数据计数 `nbt_bad_*`、允许 exit(-1)”
  是旧库语义，现行库只用返回值。
- `[历史]` 旧 mkrule 期望“沙子落在矮草上要拆”“无根紫颂横枝要拆”；26.1.1 用户规则下前者不拆
  （可替换方块不算托不住），后者看不到斜下方、一律留着。2026-11-05 起重力又照原版（矮草上的沙子拆），横枝仍看不到、留着。
- `[历史]` 用户自定的重力规则（只认空气/水/岩浆/火）、`CHORUS_PLANT_STRICT=0`（紫颂植株第 1 条不判）、门两半不同种留着不改、
  藤头/藤身等外观转换不做、火和传送门不管：2026-11-05 用户要求除霜冰外都照源码/服务端，已全部改掉。
- `[历史]` merge/fixer 维度只按字符串去重；`overworld,minecraft:overworld` 会跑两遍
  （merge 第二遍把 `InhabitedTime` 清成 0）。现行按解析后的目录去重。
- `[历史]` 文件不在的 `MCA` 空镜像取区块返回 `MCA_READ_BAD`：merge 在 new 没有 poi/entities
  文件时当“目标读不出来”跳过（文档说的新建实际不发生），now 没有文件时计成坏实体/坏POI。
  合成测试里 new 的文件总在（只缺那一块），所以一直没测出来。现行返回 `NO_CHUNK`。
- `[历史]` `sections` 是 Int 的 List 时 `get_Block` 给空气、fixer 邻居读成空气。现行读不出来。
- `[历史]` `Level::get_*Entry` 的 `create` 是 0/1/2 混用的 int（区块的 0“只查不缓存”分支没人用）。
  现行 `bool create`。
- `[历史]` 公开函数 `block_Ref_Equal`/`check_Chunk_Base`/`check_Entity_Container`/`is_POIShapeOk`，
  计划刻接口 `int fluid`、`Chunk_Tick_Keep_Fn` 返回 `int`。现行 `equal_BlockRef`/`check_ChunkBase`/
  `check_EntityContainer`/`check_POI`、`bool fluid`、回调返回 `bool`。
- `[历史]` `set_Block` 先写调色板、再写方块实体，方块实体失败时半写。现在先预检再原位写。
- `[历史]` `format_Chunk` 曾叫 `dedupe_Sections` 并由 merge 调用。现在只在 fixer 调用，
  merge 原样保留重复 Y。
- `[历史]` `InhabitedTime` 缺字段：更早“按 0 处理/不写”，后来“写 Long 0、不因此删块”。2026-11-07 起照游戏
  `getLongOr`：没有/不是数值都当0，不补字段，整块在范围内就删。
- `[历史]` 2026-11-07 以前 fixer 的“坏数据”按库自己的类型检查判（Y 不是 Byte 删段、坐标不是 Int 算坏、
  POI 缺 free_tickets 删、实体缺 UUID 删、`sections`/`block_states` 类型不对或 POI/实体容器坏整块删）。现在按 1.10 的表：
  游戏不报错的不动，报错的按最小单位删。
- `[历史]` 旧文档说“3×3 邻居只删光照但会改坐标/重复段”。现在邻居只删光照，不改这些。
- `[历史]` NBR5 及以前的支撑模型：三档 lev（薄片/有形状/整方块）+ 台阶/楼梯/活板门特判。“有形状的”一律上下
  中心支撑、侧面不整，导致：栅栏门顶上的火把被拆（把 22w43a 读成了“栅栏门不提供中心支撑”）、木桶等完整方块
  侧面的东西被拆、紫颂花/脚手架/末地烛顶上的东西被拆、堆肥桶/营火等底下挂的东西被拆、炼药锅/仙人掌/树叶/
  蜂蜜块顶上的火把留着；落叶层用了 25w06a 的中心支撑；压力板排除 TNT；红石墙上火把放宽成中心；告示牌/旗帜/
  蛋糕不看 isSolid；海泡菜/地毯同一条“不是空气就行”；睡莲接受流动水；仙人掌只看邻居是不是整方块。
- `[历史]` 测试脚本里 `程序 | tee`/`| tail`：`set -e` 只看管道最后一个命令，程序失败被吞。
  现行先重定向到日志、检查退出码，再 `cat`/`tail`。

## 3. 测试与验证

```sh
make               # 全编（测试脚本自己也会调）
sh test/lib.sh     # 库：612 个用例 + 34 个 Level 用例 + 75 个坏数据用例（bad.cpp，每例 fork、300 秒超时）+ demo 的 nbtw 自测
sh test/block.sh   # merge/fixer 合成世界回归
sh test/rules.sh   # 473 条规则用例（rules.so.26.1.1）+ 第二遍 0 改动 + POI 表和 poitab.py 对拍
sh test/format.sh  # 同一批场景放进 4788/5119 两种存档格式，fixer 修完逐格一样；merge 同版本写法对、缺段跳过、跨版本两个方向照搬、合进 1.13.2（实体进 Level.Entities、不搬 POI）
sh test/safe.sh    # 用 ASan+UBSan+LSan 另编一套，把上面四个原样再跑一遍
sh test/mcdump.sh <server.jar> all   # 手动：形状逐状态 + 存活规则穷举，和游戏对拍（见 1.8；要 jar 和 Java 25）
sh test/mcdump.sh <server.jar> region   # 手动：区域文件四种压缩/外置 .mcc 和游戏互读；2527 以前紧排下标对游戏的 BitStorageAlignFix
sh test/mcdump.sh <server.jar> upgrade  # 手动：6 个版本 × 32 种段交给游戏升级+读，和库的判坏/逐格读法比（见 1.2.1）
```

`lib.sh` 覆盖 NBT/Block/Chunk/MCA/Level、段的 Y 照游戏读、`format_Chunk`（重复 Y 留最后一段/改 Y/废弃调色板/
下标重映射）、Chunk 索引（负 Y、拷贝/移动、方块实体下标）、`pin_Chunk`/`pin_MCA`/`pin_Dat`、
`check_Chunk`（含 data 下标越界）、`set_Block` 原子失败、
坏 MCA、表头扇区重叠时写回不擦邻居、NaN 的比较、移动 Chunk 后索引的版本/格式、`set_Chunk` 不给坐标（含老区块）；
`bad.cpp` 再喂截短/超长长度/每层谎报长度的嵌套 List/坏 tag/坏表头（含偏移指在表头里、偏移 0 扇区数非 0）/坏压缩/
LZ4 乱数据/外置 mcc 不在/自定义压缩/坏形状区块/坏 POI/坏实体/越界坐标/坏维度参数/缓存钉满/fd 泄漏，库只许用返回值报错。
`lib.sh` 还覆盖 lz4 来回压解/坏块只交前面的/缺结束块、游戏写的 lz4 区域样本（`test/data/lz4/r.0.0.mca`，jar 跑出来的）、
外置 `.mcc` 的写/读/改回内嵌后删/另存跟着抄、类型 1/2 不互认、zlib 尾巴坏了照读。
`block.sh` 覆盖 merge 搬运/新建 POI/实体文件（含 new 整个文件不在、不新建 region）、now 同一格几条 POI 照游戏取一条、
Pos 用 Int 写的实体按坐标卡范围、目标
POI/实体坏压缩跳过不覆盖、重复维度只跑一遍、坏实体/坏POI 计数、范围外读不出来的区域不碰；fixer 最小单位删记录、
Y 非 Byte 子区块、POI 补/去重/坏记录单删/重建（含 DataVersion 和票数保留）、计划刻坏记录单删/
去重、实体坏记录单删/UUID（保留先出现的，Long_Array 写的 UUID 也算）/容器整删、0 字节 POI/实体文件、0 字节 region 先删并清
对应 POI/实体、region 缺失的 POI/实体、霜冰/含水门水刻、重力规则、xPos/zPos、重复 Y、光照/高度、
format、中低优先级规则、InhabitedTime 0 的区块 y 范围外多一段（删区块不看 y，两块都删）、坐标指到别的区块的方块实体/计划刻
（迁过去、目标已有、目标没有、范围外、目标自己位置也不对、没坐标、跟着挪过位置的区块——含有别的区块的记录指向它的）、表头偏移异常的格子（清成 0、同格 POI 删掉）。`rules.sh` 的用例表在 `mkrule.cpp`，每条都写了依据（期望值都按游戏判定核过）。

核对脚本 `check_rules.py`/`check_poitab.py`/`check_poi_scan.py` 用独立的 `nbt.py`/`poitab.py`，
不链 libnbtrw（`nbt.py` 只认 gzip/zlib 的区块，lz4/外置 `.mcc` 的存档用不了这些脚本）。`safe.sh` 先探测有没有 UBSan 运行库（2026-11-14 下半起这台开发机装了 libubsan，UBSan 一起开）。

已跑：
- 2026-11-21 复查那一轮：`lib.sh`（612+34+75，加 demo 的 nbtw）、`block.sh`、`rules.sh`（473 条）、`format.sh`、`safe.sh`（ASan+UBSan）全过，
  库/程序/测试编译 0 警告；新加的 6 条用例换上一次提交的代码跑都失败。真实地图没有重跑（改到的是 Int 写的实体 Pos、now 同一格几条 POI、
  不规范的区域文件名、目标区块位置也不对的错位记录、Long_Array 写的 UUID 这几种少见数据）。
  同一天按用户纠正的五条又改了一遍（光照严格按范围、`blockporter.hpp`、`test/work`→`/tmp`、`test/tmp` 挪回）以后，五个脚本再跑一遍全过；
  光照的新用例在改之前的代码上失败。
- 2026-11-14 下半那一轮：五个脚本全过（safe.sh 带 UBSan）；`mcdump.sh shape` 两个版本 0 处不对；`cmp` 两个版本全量 0 差异；真实服务端预生成图上的结果见 2.1。
- 2026-11-14 全量检查那一轮：五个脚本全过；`mcdump.sh <26.1.1 jar> region/upgrade/shape` 0 处不对；真实存档副本上新旧程序的
  merge/fixer 输出逐值相同（见 2.1）。下面几条是之前各轮的记录。
- `make` 无警告（测试程序和 `test/` 下的手动工具也是）；`lib.sh`、`block.sh`、`rules.sh`、`format.sh`、`safe.sh` 全过。
- `mcdump.sh <26.1.1 jar> region`：0 处不对；`upgrade`：0 处不对、已知 6 处（每个版本一处“游戏不认识的名字换空气”，库没有注册表）。
- `lib.sh` 的多版本用例：1460/1631/1976/2586/2730/2834/2841/2860 各造一块（17 条和 3 条调色板），逐格读、写一格、位宽变大重排、
  压调色板、方块实体/流体刻写进这个版本的表；长度不对/单条调色板/补空气的下标/新格式数值 List 的 data。
  `block.sh` 有一块 DataVersion 4325 的区块：规则不判（悬空沙子、对不上的刻都留着）。
- `mcdump.sh <26.1.1 jar> all`：形状 29873 个状态 0 处不一致（另有竹子/滴水石锥 8704 行按坐标偏移的，也 0 处不对）；
  存活规则 + 连接这一类外观 31.4 亿个配置“差异合计 没有”。
- 真实 26.1.1 图（`/home/zyh/mc/world-26.1.1` 拷两份到 `test/work/ab4/`，三个维度全范围）：上一次提交和这次的 rules.so 各跑一遍 fixer，
  逐格比只差主世界 218 格、末地 5 格（见 2.1 的 2026-11-11），三个维度第二遍都 0 改动、0 失败。
- 真实 26.1.1 图（`/home/zyh/mc/world-26.1.1` 拷两份到 `test/work/ab/`，三个维度全范围）：新旧 rules.so 各跑一遍 fixer，
  逐格比只差 231 格（见 2.1 的 `276d470`），两边都 0 失败。
- 真实 26.1.1 存档副本（`now`/`new`/`old`，x/z ±1536 全高，10925 个区块）：改名/改类型前后
  merge+fixer 输出用 `cmp_mca.py` 逐区块逐值相同；fixer 第二遍物理 0 改动、删区块 0。
  （真实图 `InhabitedTime=0` 的区块会按规则删除，所以要先确认范围/接受删块。）

## 4. 已知限制

- rules 只看六面：紫颂横枝可能漏拆、甘蔗旁水看不到、传送门框只认得出“平面邻居不是黑曜石”、滴水石锥下落等特殊机制没有完整模拟。
  脚手架按邻居存着的 distance 现算（和原版一样），不重算邻居自己的 distance。
- 混凝土粉末侧水凝固仍是原版行为；如果用户要“只在下方式水/空气时才凝固”，需要改
  `is_TouchingWater`/`judge_Falling`。
- POI 只补 vanilla 表；mod/未知类型保留不猜。POI 记录坏到读不出时只能丢坏记录，
  无法保证范围外同块记录都救回。
- 实体只删单条坏记录和 UUID 重复，不重建实体；POI 不重算已有票数。
- `check_Chunk`/`check_ChunkBase` 只做结构/下标校验，不判断方块物理合法性；物理交给 rules。
- 文件级坏 MCA（权限/IO/截断到头部）只日志，不覆盖；自定义压缩（127）只跳过，不删。
  0 字节 region 在清理前先删掉，再按“region 没有区块”清对应 POI/实体；0 字节 poi/entities
  文件也删除，0 字节 POI 文件若仍有方块需要补记录，会先按空区域重建。
- fixer 整块删的只有：游戏读不进来的（解压/NBT 坏、表头这一格坏）、生成状态不是 full 的、整块在范围内又全空气或
  `InhabitedTime` 是 0 的；别的都按最小单位修（1.5、1.10）。`check_ChunkBase` 判坏只是兜底，正常走不到。
- merge 用严格 `check_Chunk`/`check_Entity`，任一条刻/实体记录坏就整块安全跳过，
  不代 fixer 清理。
- merge 不升级方块 id，不搬玩家数据/进度/计分板；不清理重复 section/POI/实体。
- fixer 后再跑 merge 会把拆掉的东西搬回来；顺序只能 merge → fixer 一趟。
- wiki 的 Chunk_format 没有 `isLightOn`（现行字段表和历史都没提）；fixer 写 `isLightOn=0` 是用户要求的
  “删光照并禁用”，依据是原版读区块时 isLightOn 为假就丢弃存档里的光照重算，不是 wiki。
- wiki Entity_format 说 `id` “Does not exist for entities in the world”；26.1.1 真实存档里区块实体都有 `id`
  （抽查 57 个全有），指的是玩家这类不存在区块里的实体。`check_EntityRecord` 继续要求 `id`。
- 规则不看光照（蘑菇、作物：按亮暗里能活的那种算；霜冰见到就化）、不看六邻以外的水（甘蔗）、不管珊瑚死活。
- 规则不重算的“只是样子”的属性：楼梯形状（还没做，见 4.1）、红石线连接（要看斜对角，超出六邻）、红石信号（power/powered）。
  别的（门/藤头藤身/垂滴叶/竹子/火/栅栏/墙/玻璃板/铁栏杆/栅栏门/箱子/雪地草/紫颂植株/苍白苔藓地毯/蘑菇方块/音符盒/石锥粗细/
  树叶 distance/脚手架 distance、bottom）照游戏改（1.6）。流体不管。
- 重力方块（沙子、沙砾、混凝土粉末、悬空的滴水石锥/硫磺尖刺…）原版是变成下落方块落下去，fixer 直接拆掉（用户 2026-11-14 下半：
  暂时不模拟掉落；能做——一格格往下挪——但工作量大）。
- 新格式（≥5009）里没写的属性取默认值，库不补；修 26.3 以后存档的插件得自己带默认属性表。
- 只有 lvlworld 转出来的合成 5119 世界，没有真正 26.3/26.4 游戏写的存档样本；格式按 wiki 实现。
- 老区块的“游戏”是 26.1.1 载入它（先升级再读），`mcdump.sh upgrade` 对拍的就是这个；老版本服务端自己怎么读没核对（没有老 jar）。
  合并两个老版本的存档时，判坏照的仍是 26.1.1 的读法。
- 2832 以前的生物群系在 `Level.Biomes`（整块一个数组），库不查；1451 以前的区块不认，fixer 跳过不动。
  2681 以前实体在区块的 `Level.Entities` 里，fixer 的实体阶段只管 `entities/` 文件。
- POI 从 1.13 到 26.3 都认（用户 2026-11-21）：1935（19w09a，wiki）以前没有 POI（`has_POIFile`，merge 不搬、不建 poi/，fixer 的 POI 阶段整个跳过）；
  1935..1945 根上 `"0".."15"`、1946 起 `Sections`（26.3 `ReorganizePoi`），库的 get/set/erase/list/repair/`get_POILists` 都按 POI 区块自己的
  DataVersion 分。2218 `ForcePoiRebuild` 只删 `Valid`（游戏照方块重建），2209 `bee_hive`→`beehive`、3097 删 `unemployed`/`nitwit`
  是记录内容的变化，和方块改名一样不管。fixer 的 POI 只在 POI 文件和规则同版本时动，现有规则都是 26.x，所以老写法这条路只有库的单测。
- 规则只有 26.1.1、26.3 两个版本：别的版本（比如 1.18.2 的存档，DataVersion 2975）fixer 直接拒绝；存档里 DataVersion 和规则不一样的
  区块（真实存档里有 2 块 4325）fixer 不做物理/方块相关的计划刻和 POI。merge 跨版本只照搬，不升级方块名（1.4）。
- **NBT 嵌套太深会爆栈**（2026-11-14 实测）：读、释放、拷贝、比较、编码、删读不了的 tag 都是递归写的，库又不设深度上限。
  默认 8MB 的栈下嵌套 5～10 万层（原始数据约 300～500KB，压缩后只有几 KB）在读的时候就段错误。游戏自己最多读 512 层，
  这种数据只可能是故意造的或者坏得很巧。用户 2026-11-14 下半：先不管（见 4.1）。
- 迁移错位的方块实体/计划刻每条都要取一次目标区块，错位的多了缓存会频繁换进换出（用户知道、接受）。
- 解压不设大小上限（约定如此）：一格区块的压缩流能解出多大就占多大内存。
- `nbt_List::push_back`/`insert`/`set` 每次都把整张表扫一遍防“同一个指针塞两次”，一条条往大表里追加是平方的
  （几千条以内没感觉；库自己读文件不走它）。
- fixer 的 POI 阶段“区块取不到就跳过”那条路（缓存腾不出位置、落盘失败）没有用例。
- 26.3 插件补默认属性只补“没写的”；写了但值不合法的属性游戏也当默认值，插件照原样看（不知道合法值的范围）。

### 4.1 下一步（按先后）

1. **楼梯形状照游戏重算**（用户 2026-11-21：楼梯不是约束，以游戏为准）：`StairBlock.updateShape`→`getStairsShape`，看前后那格是不是楼梯、
   朝向和 half，算 straight/inner_*/outer_*；邻居都在六邻里。照别的连接类规则的做法加一条，McWorld/RulesCmp 里按完整状态比。
2. **规则从游戏生成**：照 1.9 的新方案，导出器做成服务端模组、数据文件和插件分开、按类名选判法模板，支持模组方块。
3. **深嵌套爆栈**：用户 2026-11-14 下半说先不管。要做的话是设上限还是改成不递归，再请用户定。
4. **重力方块落下去**（现在直接拆）：一格格往下挪，落到第一个不是 `FallingBlock.isFree` 的格子上面；要处理落进别的区块、砸碎的（火把这类）、
   混凝土粉末落进水里凝固。用户说工作量大、暂时不做。
5. 26.4 以后的规则插件：拿到 jar 以后照 26.3 的做法复制一份、`gen_tables.py` 重新生成、shape/cmp 对拍、补新方块的规则行。
6. 小项：`nbt_List` 追加时的查重改成只在调试时开或者换成集合；给 POI 阶段的失败路径补用例；红石线连接要是放宽“只看六邻”可以做。

## 5. 统计字段

fixer 物理行：区块/段/判格/拆/含水/改状态/读不出/跳过/坏下标/坏段/改Y段/压调色板/
改坐标/删重复段/水刻 +- /noop/版本和规则不同跳过，后面跟每条规则的改动数和改得最多的方块。修NBT行：读不了的 tag、坏 block_states 段、坏 biomes 段、认不出的调色板条目、
坏刻表、坏方块实体、折回的方块实体、坏结构引用、坏结构起点。清理行：删区块（status/全空气/InhabitedTime0/坏NBT）、
光照/高度、计划刻 删/坏记录/重复、POI 删/补/重复/坏记录/坏块（删掉的坏段）/块/文件、
实体删/坏记录/块/文件、改坐标（含实体 Position）/重复段/改Y段/压调色板、删空 MCA/format、IO 跳过、版本不认识跳过、失败。

merge 行：区块/方块/搬了/计划刻/实体/POI；跳过 now 不可读、new 不可写/没区块、坏方块、
写不进、缺段跳过、坏区域、旧图坏、坏实体、坏POI、存盘失败。“坏实体/坏POI”只算读不出来的块和坏记录，
文件不在不算。两程序在 tty 下另有 stderr 进度条。
