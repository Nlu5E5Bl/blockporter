# nbtrw（libnbtrw）库手册

`nbtrw` 是 blockporter 的底层库：NBT 树、方块/区块、MCA 区域文件、整个存档（`Level`）。
面向用户的 merge/fixer 操作说明在 [blockporter-README.md](blockporter-README.md)；
开发约定、现行规范和历史踩坑在 [ReadMeDeveloper.md](ReadMeDeveloper.md)。

| 部分 | 内容 | 产物 |
|---|---|---|
| **nbtrw / libnbtrw** | NBT、Block/Chunk、MCA、Level | `libnbtrw.so`，demo `nbtrw` |
| **blockporter** | merge / fixer / rules | `merge`、`fixer`、`rules.so.26.1.1`、`rules.so.26.3` |

## 编译

```sh
make                 # 全编：下面两部分
make nbtrw           # 只编库 libnbtrw.so + demo nbtrw
make blockporter     # 只编规则插件 + merge + fixer（库没编会先编库）
make rules           # 只重编规则插件：每个 rules.cpp.<版本> 编出 rules.so.<版本>（不链 rules.so）
make clear           # 清掉编译产物（不碰 test/work）
```

C++23，`-Wall -Wextra -pedantic`。库要 zlib、lz4、xxhash 的开发包（Fedora：`zlib-devel lz4-devel xxhash-devel`）。
程序用 `-Wl,-rpath,'$ORIGIN'` 找同目录的 `libnbtrw.so`；自己的程序包含 `nbtrw.hpp`，链接 `-lnbtrw -lz`。

## 基本对象

- **`nbt_Any`**：所有 NBT 值的基类，一字节 `type`，无虚函数。数组/字符串/compound
  分别用 `vector`/`string`/`unordered_map` 存在公开成员 `con` 里。
  - `data()` 给 `con` 的指针（不拷贝），改它就是改树；`copy()` 深拷贝整棵树（堆上新树，用完 `free()`）；
    派生类型的拷贝构造/赋值也是深拷贝，移动是接管。`nbt_Any` 本身不能按值拷（会切片）。
  - 比较只有 `==` 和 `<=>`（`!=` 由 C++20 推出来）：整数/浮点/数组先转高精度再比，List/Compound 只比相等；
    浮点两边都是 NaN 算相等（带 NaN 的树和自己的拷贝要相等）。
    `nbt_String` 还能直接和字符串比：`s=="minecraft:air"`。
  - 取值：`nbt_Any` 上的 `as_Byte/as_Short/as_Int/as_Long/as_Float/as_Double/as_String/as_Byte_Array/
    as_Int_Array/as_Long_Array` 是函数，只往高精度转，转不了给 `std::nullopt`：`any->as_Int()==5`、
    `any->as_String()=="x"` 直接能写，空值时比较为假；要数值用 `*`/`value_or()`。
  - 具体类型能隐式转成自己的 C++ 类型：`int32_t v=nbt_int;`、`const std::string &s=nbt_str;`、
    `std::vector<int64_t> w=nbt_byte_array;`（数组还能隐式转成更高精度的 vector，新拷贝）。
    标量只有“转成自己那个类型”一个隐式转换，往宽转交给 C++ 本身（多给几个会让 `a+b` 二义）。
- **`Block`**：`id`（`nbt_String`）+ `properties`（`nbt_Compound`）+ `block_entities`
  （一个 `nbt_Compound`）。空 `id` 表示“读不到”，`minecraft:air` 才是空气。
- **`Chunk`**：`con`（区块的 `nbt_Compound`，按值持有）+ 索引（见下面“Chunk 读写”）。不继承 `nbt_Compound`：
  `Chunk(std::move(compound))` 接管构造、`Chunk(compound)` 深拷贝构造；拷贝是深拷贝（索引按新树重建），移动接管（索引跟着走）；
  `data()` 给 `con` 的指针，`(nbt_Compound&)c`/`(nbt_Compound)std::move(c)` 强转就是 `con`。
  生成状态、DataVersion、`xPos/zPos` 这些能 O(1) 从树里读的不缓存，每次现读。
  不完整区块（生成状态不是 `minecraft:full`，或没有这个字段）不读也不写；缺 section 算空气。
  格式按**区块自己根上的 `DataVersion`** 选（见下面“按 DataVersion 选格式”），不看 level.dat。
- **`MCA`**：一个 `.mca` 文件的内存镜像，读写的都是 `nbt_Compound`（region 区块/实体/POI 一样）：
  `get_Compound(x,z)` 给 `std::optional<nbt_Compound>`，`set_Compound(c,x,z)`；要当区块用就
  `Chunk c(std::move(*r))`，写回去传 `c.con`。文件不在是空镜像（取区块给 `MCA_READ_NO_CHUNK`，
  和“表头这一格是空的”一样）；文件存在但读不出来（是个目录、权限/IO 错…）是 `bad=true`；
  0 字节、不到 8KB 表头的照游戏补 0 照读（后一种 `truncated=true`）。表头一格照游戏的 `RegionFile`：
  偏移指在表头里（<2）、扇区数是 0、偏移在文件外是“偏移异常”，游戏打开文件时 WARN 一声当没有，库也当没有
  （`has_Chunk` 假、取出来 `MCA_READ_NO_CHUNK`、`has_BadOffset` 真）；`clean_Offsets()` 把它们清成 0。
  `format()` 先清偏移异常的格子，再按 8K 表头顺序压紧数据区；数据坏的格子（长度字段对不上）原样搬它占的扇区。
  `list_MCAFiles(dir)` 列出目录里实际存在的 `r.<x>.<z>.mca`（按坐标排序；文件名只认游戏拼得出来的写法，`r.01.0.mca` 这种不算），范围很大时用它代替逐个区域坐标去试。
  压缩类型照游戏：1 gzip、2 zlib、3 不压、4 lz4（lz4-java 的 `LZ4Block` 流）、127 自定义（模组的，读不了，
  给 `MCA_READ_UNSUPPORTED_COMPRESSION`，不算坏块）；类型最高位是外置：数据在同目录的 `c.<区块x>.<区块z>.mcc`。
  写的时候按 `compression`（默认 zlib）压，压完要占 256 个扇区及以上就像游戏一样放外置，`save()` 时连 `.mcc` 一起写，
  改回内嵌/删掉的格子把旧 `.mcc` 删掉。读一格的检查照游戏的 `RegionFile`：游戏边解压边读 NBT、读完根 compound 就停，
  所以压缩流后面坏了/lz4 没有结束块不算错，根 compound 能完整读出来就行。

## 照游戏读：数值、名字、NBT 能不能读

库读存档的规矩尽量和游戏（26.1.1 源码）一样，fixer 判“坏不坏”也按这个：

```cpp
get_TagInt(v);      // CompoundTag.getIntOr 的读法：任何数值 tag，long 取低32位，浮点向下取整；不是数值给 nullopt
get_TagLong(v);     // getLongOr：float 向零取整、double 向下取整
get_NumberInt(v);   // Codec.INT 的读法（计划刻 x/y/z/t/p…）：浮点向零取整
get_NumberDouble(v);// Codec.DOUBLE/FLOAT（实体 Pos/Rotation…）
check_Identifier("minecraft:stone");     // 名字写法（Identifier.tryParse），不看游戏认不认识
check_NBTReadable(root, 512);            // 游戏的 NbtIo 读得了吗：嵌套 ≤512 层、字符串/键名是 modified UTF-8
prune_NBTUnreadable(root, 512);          // 读不了的 tag 按最小单位删
check_PaletteContainer(bs, 4096, true);  // 段的 block_states/biomes 游戏解得出来吗（解不出来＝整块载入失败）
check_ChunkTickRecord(tick);             // 一条计划刻游戏读得进来吗
get_POIRecord(rec, &poi);                // 一条 POI 照 PoiRecord 的 codec 读成 POI（free_tickets 没有当0）
check_EntityRecord(entity);              // 一条实体游戏读得进来吗（没有 UUID/Pos 不算坏）
```

`xPos/zPos`、`DataVersion`、方块实体坐标都按 `getIntOr` 读（任何数值类型都认）。方块实体的位置也照游戏：
区块内的格子取 x/z 的低 4 位（坐标在别的区块的游戏会折回本区块；fixer 会把它们迁到坐标所在的区块），同一格有几条时取最后那条。
调色板只有一条时游戏不看 `data`，库也不看。

## Chunk 读写

```cpp
Chunk *c = lv.get_Chunk("overworld", cx, cz);   // 没有返回 NULL
Block b = c->get_Block(lx, y, lz);              // lx/lz 0..15，y 是世界绝对 Y
bool ok = c->set_Block(b, lx, y, lz);           // 失败不改区块
```

`set_Block` 先在原位预检调色板、`data`、方块实体表，全部能写才动方块状态；不会
“调色板写了一半、方块实体失败返回 false”。没有对应 section、`xPos/zPos` 不是 Int
且要写方块实体、`block_entities` 形状坏时直接失败，不改区块。成功后 `block_states`
指针保持稳定，这一段的索引跟着更新。

Chunk 自己带着索引，只记要扫列表才能得到的东西，都指在 `con` 树里：

| 成员 | 内容 |
|---|---|
| `sec_by_y[256]` | 段的 `Y` 按 uint8 存：0~127 在前，负的从后往前（-1→255）。`Y` 照游戏读（`read_SectionY`）；同一 `Y` 有几段时记最后那段 |
| `palette_by_y[256]` | 那一段的方块调色板：`block_states.palette`，2832 以前是段里的 `Palette`（没有/不是 List 是 NULL） |
| `data_by_y[256]` | 那一段的下标：`block_states.data`，2832 以前是段里的 `BlockStates`（没有/不是 Long_Array 是 NULL） |
| `states_by_y[256]` | 那一段游戏读的时候调色板有几条（见下面“段的方块状态：照游戏读”的 `n_game`）：库能原样读写时 >0；0＝没有方块状态（空气）；-1＝库不读 |
| `be_by_pos` | 方块实体：`get_BlockEntityKey(x,y,z)`（x/z 是 0~15 的相对坐标，y 是绝对坐标）→ `block_entities` 里的下标；照游戏同一格取最后那条 |

`reindex()` 强制重建。读接口是 const 的，索引是 mutable，所以同一个 Chunk 别在多个线程里同时读。
段的 `Y` 照游戏读（`SerializableChunkData.parse` 的 `getByteOr("Y",0)`）：任何数值都截成 byte（浮点先向下取整），
缺了/不是数值当 0；同一 `Y` 有几段时后面的覆盖前面的（改 Y 以后撞上的也算）。库的读写都这样取段，merge 也是。
`format_Chunk()` 是 fixer 用的整理函数，改完游戏读出来的东西不变：Y 不是 Byte 的照上面的读法改成 Byte、
同一 `Y` 的多段只留最后一段、`block_states.palette` 里没被 `data`
引用的条目压紧并重写全部 4096 个下标。返回删掉的段数（`sections` 不是 List 返回 `-1`）；
后两个可选出参分别报“改了 Y 的段数”和“压过调色板的段数”。merge 不调用它。读写接口
本身不自动删段。

用库接口改区块（`set_Block`/`format_Chunk`/`set_BlockEntity`/`set_BlockData` 等）会自己更新索引：
增删 section、删方块实体时整个重建，新加方块实体只往索引里补一条。
如果绕过接口直接改 NBT 树（`c->con.…` 或拿着 `data()` 的指针改），改完要 `c->reindex()`。
`sections`/`block_entities` 列表被换掉或长度变了、`xPos/zPos` 变了，下次读的时候能自己看出来并重建；
换掉某段的 `block_states`/`palette`/`data`、改方块实体的 x/y/z 看不出来，不 `reindex()` 就会读到旧指针。
需要逐格比较时不要先造两个 `Block`：

```cpp
Block_Ref a, b;
if(c1->get_BlockRef(x, y, z, &a) && c2->get_BlockRef(x, y, z, &b)) {
    bool same = equal_BlockRef(a, b);    // Name/Properties/方块实体，语义同 get_Block
}
Section_View v = c1->view_Section(sy);   // 一段一段扫：区块级字段打开时读一次，逐格只走索引
v.get_BlockRef(x, ly, z, &a);            // ly 是段内 0..15；改过树（set_Block 等）要重新打开
```

`Block_Ref`/`Section_View` 只指在树里，不能跨 `set_Block` 或缓存淘汰保存；`equal_BlockRef(ref, Block)`
可以和已有的 `Block` 比。`get_BlockRef` 返回 false 表示这一格读不出来（不完整区块/坏
调色板/坏下标/`sections` 不是 compound 的 List），空气是成功且 `state==NULL`（或指向
`minecraft:air` 调色板条目）。`get_Block` 读不出来时返回空 id 的 `Block`，规则相同：
`sections` 形状坏的区块整块读不出来，不会被当成“缺段=空气”。

`check_Chunk(&c->con)` 是严格检查（这几个检查函数的参数都是区块根 `nbt_Compound`）：NBT 成员类型/必需结构、每条计划刻六样俱全（Y 不查，游戏照读）；
`check_ChunkBase(c)` 只查区块级结构（sections/block_states/palette/data/方块实体表、
刻表成员类型），不查单条刻和 Y。merge 用严格版，遇到坏就整块跳过；fixer 用 Base 版，
再用 `format_Chunk`/`cleanup_Ticks` 按最小单位删。它不判断“这格方块合不合理”，缺 section/
纯光段/缺生成状态字段仍是正常数据。调色板条目按区块的格式认：DataVersion≥5009 的区块里出现
`{Name:…}` 老写法、或 <5009 的区块里出现字符串条目，都算坏数据（不按两种格式猜）。

## 按 DataVersion 选格式

库是跨版本的：同一份代码读写不同版本的存档，格式差异全按 DataVersion 判断，不看目录或字段在不在来猜。
level.dat 的 DataVersion 定目录布局；区块的格式看区块根上的 `DataVersion`（一个存档里可以混着没升级的
老区块），`get_ChunkDataVersion(c)` 没有这个字段给 0。认得的是 1.13 扁平化（1451）以后的区块
（`is_ChunkVersionKnown`）；没写 DataVersion 的当最新格式读（工具自己造的区块常常不写），但不算认得——游戏当它是 -1、
从最老的格式升级，fixer 不碰它。

| DataVersion | 版本 | 变化 | 库里 |
|---|---|---|---|
| 1451 | 1.13 快照 17w47a | 扁平化：段里 `Palette`/`BlockStates`，字段都在根的 `Level` 里 | `CHUNK_FLATTEN`、`get_ChunkLayout`/`get_ChunkBody` |
| 1466 | 1.13 快照 18w06a | `Level.Status` 代替 `TerrainPopulated`（ChunkToProtochunkFix） | `CHUNK_PROTO`、`get_ChunkStatus` |
| 1905 / 1911 | 1.14 快照 | 生成完的状态 `postprocessed`→`fullchunk`→`full`（ChunkStatusFix/ChunkStatusFix2） | `CHUNK_FULLCHUNK`、`CHUNK_STATUS_FULL` |
| 2527 | 1.16 快照 20w17a | `BlockStates` 的下标不再跨 long（BitStorageAlignFix） | `CHUNK_ALIGN`、`Chunk_Layout::span` |
| 2832 | 1.18 快照 21w37a | 段里改成 `block_states{palette,data}` 和 `biomes`（ChunkHeightAndBiomeFix） | `CHUNK_STATES`、`Chunk_Layout::states` |
| 2841 | 1.18 快照 | `LiquidTicks` 改名 `fluid_ticks`（ChunkProtoTickListFix） | `CHUNK_FLUID_TICKS` |
| 2842 | 1.18 快照 21w43a | 去掉 `Level` 外壳，`Sections`/`TileEntities`/`TileTicks`/`Structures`/`Starts` 改小写新名（ChunkRenamesFix） | `CHUNK_FLAT` |
| 4774 | 26.1 快照 6 | 维度目录挪进 `dimensions/minecraft/<维度>/` | `DAT_NEW_LAYOUT`、`Level::new_layout` |
| 5009 | 26.3 快照 7 | 调色板条目 `Name`/`Properties` 改名 `id`/`properties`；默认状态可以只写方块 ID（字符串，混在复合列表里时包成 `{"":ID}`）；没写的属性取默认值 | `STATE_ID_FORMAT`、`State_Format` |
| 5119 | 26.4 快照 1 | 区块根的 `Status` 改名 `status`；段的 `biomes` 改成每格一个（`data` 4096 个下标） | `CHUNK_STATUS_LOWER`/`get_StatusKey`、`BIOME_PER_BLOCK`/`is_BiomePerBlock` |

调色板条目的编解码（都在 atom.cpp，格式由调用者按区块给，`get_ChunkStateFormat(c)`）：

```cpp
State_Format f = get_ChunkStateFormat(c);          // STATE_NAME（<5009）或 STATE_ID（>=5009）
const char *name = get_StateName(e, f);            // 名字；认不出来给 ""
const nbt_Compound *p = get_StateProps(e, f);      // 写出来的属性；没写给 NULL
const char *v = get_PropValue(p, "facing");        // 属性值；没有给 NULL
nbt_Any *e2 = make_State(block, f);                // 按格式造条目（新格式没属性就是字符串）
Block b = make_StateBlock(e, f);                   // 条目 → Block（不带方块实体）
bool same = is_StateSame(e, e2, f);                // 名字和写出来的属性都相同
```

库只管编码，不知道每种方块的默认属性：新格式里没写的属性就是“没有”，要补默认值是规则插件的事。
`set_Block` 往新格式的调色板写：没有属性的方块写成字符串；字符串列表里要写带属性的方块时，先把已有条目
原地包成 `{"":ID}`（原来的 String 节点挪进去，之前拿到的名字指针仍有效），再加 `{id,properties}`。
section 级的 `get_BlockData`/`set_BlockData`/`is_SectionConst` 都要传 `Chunk*` 和段号：段表/调色板/data 走区块的索引，
格式按区块的 DataVersion。

区块字段的位置和键名按 `Chunk_Layout`（`get_ChunkLayout(dv)` 或 `get_ChunkLayout(chunk)`）：`get_ChunkBody(c)` 给字段所在的那层
（2842 以前是 `Level`，没有就 NULL），`L.sections`/`L.block_entities`/`L.block_ticks`/`L.fluid_ticks`/`L.structures`/`L.starts`/
`L.status` 是这个版本的键，`get_SectionStates(sec,L)` 给段的方块状态容器（新格式 `block_states`，老格式就是段本身），
`L.palette`/`L.data` 是里面的键。

### 段的方块状态：照游戏读

`get_StatesView(sec,L,scan)` 照游戏读档的样子看一段（26.1.1；2832 以前的区块游戏先用 DataFixers 升级再读，库照着算：
2527 以前位宽不是 2 的幂时 `BitStorageAlignFix` 把下标转成不跨 long、长度补齐/截断；2832 时 `Palette`/`BlockStates` 少一样
就是空气段、调色板只有一条就扔掉 data、data 比调色板要的长就在调色板后面补空气）。`test/mcdump.sh upgrade` 拿游戏逐格对拍过：

- `kind`：`STATES_AIR`（没有方块状态，游戏当全空气）/`STATES_OK`/`STATES_BAD`（游戏整块载入失败，`check_SectionStates` 给 false）；
- `canonical`：库能原样读写——palette 是 List、data 是 Long_Array、长度按这个版本的排法正好、位宽和游戏读的一样。
  游戏读得了但不是这样的（data 是 `Int_Array` 或数值 List、老格式按别的位宽读、补了空气后位宽变了…），
  库当读不出来：`get_Block` 给无效、`set_Block` 不写、`check_Chunk` 不过（merge 跳过），fixer 不动它；
- `n_game`：游戏读的时候调色板的条数，老格式下标落在 `[n,n_game)` 的格子是空气（库读出来就是空气）。

data 照 `NbtOps.getLongStream` 读（`get_LongStream`：Long_Array，或元素全是数值的 List/Int_Array/Byte_Array），
palette 照 `getStream`（`get_CollectionSize`：List 和三种数组都算集合）。

## 计划刻与 POI 辅助

- `count_ChunkTicks(chunk, fluid)`、`get_ChunkTick()`、`del_ChunkTick()`、
  `add_ChunkTick()`、`filter_ChunkTick()`：`fluid` 是 `bool`，`false` 是 `block_ticks`，
  `true` 是 `fluid_ticks`；表形状不对返回 `-1`/`false`，调用者不要改。`add_ChunkTick` 顺带删掉表里游戏读不进来的条目，
  同一格已经有同名的刻就不加（给 `false`）。`filter_ChunkTick`
  的回调返回 `bool`，`false` 的那条删掉。
- **POI 和 Entity**（atom.cpp，和 `Block` 一样按值持有、和容器无关）：
  - `POI{x,y,z,type,free_tickets}`：一条兴趣点记录（游戏的 `PoiRecord.Packed`）。空 `type` 是读不到；
    `name()/spacename()/basename()/strid()`，修改用 `set_Pos()`/`set_Type()`/`set_FreeTickets()`（成员也能直接改）。
    `get_POIRecord(rec,&poi)`/`make_POIRecord(poi,x,y,z)` 是单条记录的读写。
  - `Entity{con}`：一个实体（实体表里那条 compound 原样，含乘客）。`name()`（id）、`uuid()`（Java `UUID.toString` 的
    8-4-4-4-12 小写十六进制；`UUID` IntArray 和老的 `UUIDMost`/`UUIDLeast` 都认；没有是 `""`）、`set_UUID()`（照它原来的写法写）、
    `get_Pos()`/`set_Pos()`、`valid()`（`check_EntityRecord`：单条 `id`/`UUID`/`Pos`/`Motion`/`Rotation`）。
- **POI 区块**（poi/ 下；没有结构体，直接传 `nbt_Compound*`，坐标是绝对坐标）：
  `get_POI(poi,x,y,z)` 给游戏眼里这一格的那条（没有给空 `type`）；`set_POI(poi,x,y,z,p)` 先删掉这一格原有的，
  再按 `(x,y,z)` 重写 pos 写一条；`erase_POI(poi,x,y,z)` 删掉这一格所有记录、给条数；`list_POI(poi,&out)` 列全部
  （给跳过的坏记录数，`check_POI` 不过给 -1）；`count_POI`、`clean_POI`、`make_POIChunk(dv)`（空容器）。
  写法按 POI 区块自己的 DataVersion：`POI_FILE`=1935（19w09a）以前没有 POI（`has_POIFile(dv)`，1.13 在内），
  `POI_SECTIONS`=1946 以前根上 `"0".."15"` 直接是记录表，之后
  `Sections{"<段Y>":{Records,Valid}}`；没写 DataVersion 当最新（游戏当 1945，落盘前要写上）。老写法段 Y 只能 0..15，装不下给 false、一点不改。
  读照游戏的宽松读法：游戏读不出来的部分当没有，`set_POI` 直接换掉；同一格几条时同类型留先来的、不同类型后来的顶掉。
  `repair_POI` 删游戏解不出来的段，`get_POILists` 给所有读得出来的记录表（要按条处理时用），两种写法都认。
  `check_POI` 是库的严格判法（容器形状，单条记录坏不算）。这些函数只管结构，不决定“哪个方块该有什么 POI”；
  方块→默认 POI 表在 `rules.cpp` 的 `Poi_Info`。
- **实体表**：`get_ChunkEntities(c,&out)`、`add_ChunkEntity(c,e)`、`erase_ChunkEntity(c,uuid)` 两种写法都收：
  老写法传区块（`Chunk.data()`；键按区块版本，2842 以前 `Level.Entities`，之后 `entities`，即 `Chunk_Layout::entities`），
  新写法传 `entities/` 里解出来的实体区块（根上有 `Position` 的就当实体区块）。`erase` 按 uuid 删顶层实体，没有 UUID 的算 `""`，
  重复的全删。`make_EntityChunk(cx,cz,dv)` 建空实体区块；`has_EntityFile(dv)`：`ENTITY_FILE`=2681 起实体在 `entities/`。
  `check_Entity()`（实体区块的容器+每条记录都完整）、`check_EntityContainer()`（只查 `Position`/`Entities`；
  `DataVersion` 可以没有——游戏当它是 -1，在就得是 Int）。读不出来给 -1/false，`out` 和目标都不动。

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
- 缓存（dat/MCA/区块）全按值放在 `Level` 里。指针的有效期分两种：
  - 默认没钉住：`get_Dat`/`get_MCA`/`get_Chunk` 给的指针只能用到下一次调用 `Level` 的接口为止
    （任何取/写/`close` 都可能把它淘汰掉），每次要用都重新 `get_*`；
  - `pin_Dat`/`pin_MCA`/`pin_Chunk` 钉住的在对应的 `unpin_*` 之前一直有效：LRU 不淘汰、`close()` 也不放掉
    （只有析构会）。可以钉好几次，unpin 同样次数才松开。缓存全被钉住时取新对象返回 `NULL` 且
    `miss_why=GET_CACHE`，不硬抢；
- `set_Chunk(dim, cx, cz, chunk)`/`set_MCA`/`set_Dat`：深拷贝进缓存里原来那个对象（地址不变，钉着的指针照样有效）并标脏；
  传的就是 `get_Chunk` 拿到的那个对象时只标脏。`set_Chunk(dim, chunk)` 不给坐标，从区块自己的 `xPos/zPos` 取
  （必须是 Int；2842 以前的老区块在 `Level` 里）。`Level` 不能拷贝（两份缓存会把同一个文件各写一遍）；
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
sh test/lib.sh     # 库：612 个用例 + 34 个 Level 用例 + 75 个坏数据用例
sh test/block.sh   # merge/fixer 合成世界回归
sh test/rules.sh   # 473 条规则用例 + 第二遍0改动 + POI 表对拍
sh test/format.sh  # 同一批场景放进 4788/5119 两种存档格式，fixer/merge 结果必须一样
sh test/safe.sh    # ASan(+UBSan，如果装了)+LSan 下把上面四个再跑一遍
sh test/mcdump.sh <server.jar> region|upgrade   # 手动：拿原版服务端 jar 和游戏对拍区域文件、老区块的读法
```

`test/lib.cpp` 覆盖 NBT/Block/Chunk/MCA/Level、段的 Y 照游戏读（截断/浮点/NaN/缺了当0）、`format_Chunk`（重复 Y 留最后一段、改 Y、
废弃调色板）、Chunk 的索引（负 Y、拷贝/移动、方块实体下标）、`pin_Chunk`/`pin_MCA`/`pin_Dat`、
`set_Block` 原子失败、`Block_Ref` 零拷贝比较、lz4/外置 `.mcc`、1451 以后各版本的区块格式；
`test/lvl.cpp` 覆盖 Level 维度/缓存；`test/bad.cpp` 每个用例 fork 一个子进程，喂截短/
超长长度/每层谎报长度的嵌套 List/坏 tag/坏表头/坏压缩/坏形状/越界坐标，库只许用返回值报错，不许崩、不许 exit、
不许漏 fd，转不出来（300 秒）也算失败。测试目录说明见 [test/ReadMe.md](test/ReadMe.md)。

## 注意

- 程序都是**就地改存档**，先在副本上跑；顺序只能是 merge → fixer，且只走一趟。
- `nbtrw.hpp` 是库头文件，`rules.hpp` 是 rules 插件 API 头文件；`blockporter.hpp` 是 merge/fixer 共用的上层小工具，不属于库。
- 函数一律 `<操作>_<操作对象>`：操作全小写，对象是缩写就全大写（`POI`/`MCA`/`NBT`），
  是单词就首字母大写，是结构体就写结构体名（`get_BlockRef`、`check_ChunkBase`）。
  真假一律用 `bool`，不拿 `int` 当 `bool`。
- 真实地图只读测试请用副本；仓库根目录的 `now/`、`new/`、`old/` 是测试素材。
