/*	nbt.hpp —— 底层库（libnbt.so）的全部声明
 *
 *	这里只有"底层操作"：NBT的读写、方块/区块、区域文件、存档目录。规则（rules.so）和
 *	merge/fixer两个主程序都不在这个头里，各是各的：
 *	  nbt.hpp/nbt.cpp        nbt_Raw游标、nbt_Any（所有nbt格式的父struct，也当泛型用）及11种tag、
 *	                         nbt_List、nbt_Compound，以及level.dat（dat就是gzip压缩的nbt，也在nbt.cpp）
 *	  mca.cpp                mca_Region（.mca文件的内存镜像）+ zlib压缩解压
 *	  chunk.cpp              Chunk（一个区块，继承nbt_Compound）和Chunk_Cursor（连着读时的小备忘）
 *	  block.cpp              Block（方块id+方块属性+方块实体）、方块状态小工具、section里的调色板读写
 *	  level.cpp              level（一个存档目录：按DataVersion定维度目录、区域按需读、区块缓存、落盘）
 *	  → libnbt.so            （上面5个.cpp编成的动态库）
 *	  rules.hpp/rules.cpp    → rules.so.26.1（规则插件：哪些方块要管、怎么判；fixer用dlopen装它）
 *	  merge.cpp              → merge（搬方块，merge.py的原型）
 *	  fixer.cpp              → fixer（拿rules.so逐格扫、拆浮空物）
 *	  nbtrw.cpp              → nbtrw（这套底层库的demo）
 *
 *	一个虚函数都没加：type在每个结构体里都还在偏移0（没有虚表），(nbt_Compound*)any 这类强转照样成立。
 *	容器换成STL了，所以内存布局跟C版不一样了；强转只依赖tag和"每个结构体第一个成员是type"这一条。
 *	内存全用new/delete，容器用STL，nbt_Raw/mca_Region自己管自己的内存，不用手动free。
 *	注意：编码时compound成员按unordered_map的遍历顺序写，和MC原始文件的顺序不同（NBT无序，MC照读），
 *	      所以“读进来再写回去”字节不再逐位相同，但 byte_size() 一定不变，逐值比较也一定相同。
 *	      要比两个区块一不一样，用 cmp()/== ，别比字节。
 *
 *	需要C++23（用了string_view、default member initializer这些）；用了STL所以要用g++链接。
 *	编译：make（见Makefile）。手工编的话（顺序别乱，rules/merge/fixer都要链libnbt.so）：
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -shared nbt.cpp block.cpp chunk.cpp mca.cpp level.cpp -o libnbt.so -lz
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -shared rules.cpp -o rules.so.26.1 -L. -lnbt -lz -Wl,-rpath,'$ORIGIN'
 *	      ln -sf rules.so.26.1 rules.so
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic merge.cpp -o merge -L. -lnbt -lz -Wl,-rpath,'$ORIGIN'
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic fixer.cpp -o fixer -L. -lnbt -lz -ldl -Wl,-rpath,'$ORIGIN'
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic nbtrw.cpp -o nbtrw -L. -lnbt -lz -Wl,-rpath,'$ORIGIN'
 */
#ifndef NBT_HPP
#define NBT_HPP
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

typedef uint8_t byte;

struct nbt_Raw;struct nbt_Any;struct nbt_Compound;struct Chunk;

struct nbt_Any{
	const uint8_t type;
	nbt_Any(uint8_t type):type(type){}
	void free();/*删掉自己和下面所有节点（没有虚析构，按tag分派）*/
	void print(size_t depth)const;void print()const;/*NBT风格的snbt*/
	size_t byte_size()const;/*序列化字节数，不含tag和名字*/
	void encode(nbt_Raw *raw)const;/*只写payload，tag和名字由上层写*/
	nbt_Any *copy()const;/*深拷贝，数组按值复制*/
	int cmp(const nbt_Any *b)const;/*逐值比较；compound按名字找成员，与顺序无关*/
	bool operator==(const nbt_Any &b)const;bool operator!=(const nbt_Any &b)const;
	/*取值便利，类型不对就返回0/NULL*/
	int32_t as_Int()const;int64_t as_Long()const;const char *as_Str()const;bool is(const char *str)const;
};
static_assert(sizeof(nbt_Any)==1,"nbt_Any不能加虚函数：type必须在偏移0，派生结构的强转才成立");

struct nbt_Byte:nbt_Any{uint8_t con;nbt_Byte(uint8_t con=0):nbt_Any(0x01),con(con){}};
struct nbt_Short:nbt_Any{uint16_t con;nbt_Short(uint16_t con=0):nbt_Any(0x02),con(con){}};
struct nbt_Int:nbt_Any{uint32_t con;nbt_Int(uint32_t con=0):nbt_Any(0x03),con(con){}};
struct nbt_Long:nbt_Any{uint64_t con;nbt_Long(uint64_t con=0):nbt_Any(0x04),con(con){}};
struct nbt_Float:nbt_Any{float con;nbt_Float(float con=0):nbt_Any(0x05),con(con){}};
struct nbt_Double:nbt_Any{double con;nbt_Double(double con=0):nbt_Any(0x06),con(con){}};
struct nbt_String:nbt_Any{std::string con;nbt_String(const std::string &con=""):nbt_Any(0x08),con(con){}};
struct nbt_Byte_Array:nbt_Any{/*仿vector*/
	std::vector<uint8_t> con;
	nbt_Byte_Array():nbt_Any(0x07){}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	uint8_t &operator[](size_t i){return con[i];}const uint8_t &operator[](size_t i)const{return con[i];}
	uint8_t &at(size_t i){return con.at(i);}const uint8_t &at(size_t i)const{return con.at(i);}
	void push_back(uint8_t v){con.push_back(v);}void clear(){con.clear();}
};
struct nbt_Int_Array:nbt_Any{/*仿vector*/
	std::vector<uint32_t> con;
	nbt_Int_Array():nbt_Any(0x0b){}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	uint32_t &operator[](size_t i){return con[i];}const uint32_t &operator[](size_t i)const{return con[i];}
	uint32_t &at(size_t i){return con.at(i);}const uint32_t &at(size_t i)const{return con.at(i);}
	void push_back(uint32_t v){con.push_back(v);}void clear(){con.clear();}
};
struct nbt_Long_Array:nbt_Any{/*仿vector*/
	std::vector<uint64_t> con;
	nbt_Long_Array():nbt_Any(0x0c){}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	uint64_t &operator[](size_t i){return con[i];}const uint64_t &operator[](size_t i)const{return con[i];}
	uint64_t &at(size_t i){return con.at(i);}const uint64_t &at(size_t i)const{return con.at(i);}
	void push_back(uint64_t v){con.push_back(v);}void clear(){con.clear();}
};

struct nbt_AnyRef{/*l[i]的代理：读就是nbt_Any*，赋值会先删掉旧的（不写就别管它）*/
	nbt_Any **slot;
	nbt_AnyRef(nbt_Any **slot):slot(slot){}
	operator nbt_Any*()const{return *slot;}
	template<class T> operator T*()const{return (T*)*slot;}/*要直接 (nbt_Compound*) 强转时用这个*/
	nbt_Any *get()const{return *slot;}
	nbt_Any *operator->()const{return *slot;}
	nbt_Any &operator*()const{return **slot;}
	nbt_AnyRef &operator=(nbt_Any *any);
	bool operator==(const nbt_Any *any)const{return *slot==any;}
	bool operator!=(const nbt_Any *any)const{return *slot!=any;}
};

struct nbt_Compound:nbt_Any{/*仿unordered_map*/
	std::unordered_map<std::string,nbt_Any*> con;
	uint64_t serial=0;/*区块用：Chunk::read每读一个区块给个新号；游标靠它发现"同一个地址换了区块"（free完再new很容易撞地址）*/
	nbt_Compound():nbt_Any(0x0a){}
	~nbt_Compound();/*释放所有成员*/
	typedef std::unordered_map<std::string,nbt_Any*>::iterator iterator;
	typedef std::unordered_map<std::string,nbt_Any*>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	size_t count(const std::string &key)const{return con.count(key);}
	bool contains(const std::string &key)const{return con.count(key)!=0;}
	iterator find(const std::string &key){return con.find(key);}
	const_iterator find(const std::string &key)const{return con.find(key);}
	nbt_Any *at(const std::string &key)const{return con.at(key);}/*没有会抛out_of_range*/
	nbt_Any *get(const std::string &key)const;/*没有返回NULL*/
	nbt_Any *set(const std::string &key,nbt_Any *any);/*接管所有权，同名的旧的删掉；any为NULL等于erase*/
	size_t erase(const std::string &key);/*删掉了返回1*/
	void clear();/*全删掉*/
	nbt_Int *set_Int(const std::string &key,int32_t v);
	nbt_Long *set_Long(const std::string &key,int64_t v);
	nbt_String *set_String(const std::string &key,const std::string &str);
	struct Ref{/*c["key"]的代理：读不存在的key不会插空成员，赋值会先删掉旧的（不写就别管它）*/
		nbt_Compound *self;std::string key;
		Ref(nbt_Compound *self,const std::string &key):self(self),key(key){}
		operator nbt_Any*()const{return self->get(key);}
		template<class T> operator T*()const{return (T*)self->get(key);}
		nbt_Any *get()const{return self->get(key);}
		nbt_Any *operator->()const{return self->get(key);}
		nbt_Any &operator*()const{return *self->get(key);}
		Ref &operator=(nbt_Any *any);
		bool operator==(const nbt_Any *any)const{return self->get(key)==any;}
		bool operator!=(const nbt_Any *any)const{return self->get(key)!=any;}
	};
	Ref operator[](const std::string &key){return Ref(this,key);}
	const nbt_Any *operator[](const std::string &key)const{return get(key);}
	/*指针上的写法：c是nbt_Compound*时 c["k"] 会变成指针下标，用 c->ref("k") 代替(*c)["k"]*/
	Ref ref(const std::string &key){return Ref(this,key);}
	const nbt_Any *ref(const std::string &key)const{return get(key);}
	/*整棵树*/
	void read_Members(nbt_Raw *raw);/*内部：读成员直到End（tag和名字由上层读）*/
	static nbt_Compound *read(const char *path);/*读.nbt文件（根tag+名字自动跳过），失败返回NULL*/
	static nbt_Compound *read_Dat(const char *path);/*level.dat，gzip/zlib/裸数据都认*/
	nbt_Raw to_Raw()const;/*编码成文件里的样子（根tag+空名字），给save/write_Chunk用*/
	int save(const char *path)const;/*编码后落盘*/
};
struct nbt_List:nbt_Any{/*仿vector；元素是nbt_Any*，push_back接管所有权*/
	uint8_t elem;/*元素类型；空list写文件时用它，第一个元素进来时自动定正*/
	std::vector<nbt_Any*> con;
	nbt_List(uint8_t elem=0x00):nbt_Any(0x09),elem(elem){}
	~nbt_List();/*释放所有元素*/
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	nbt_Any *at(size_t i)const;/*越界抛out_of_range*/
	nbt_Any *operator[](size_t i)const{return con[i];}
	nbt_AnyRef operator[](size_t i){return nbt_AnyRef(&con[i]);}
	/*指针上的写法，同nbt_Compound::ref*/
	nbt_Any *ref(size_t i)const{return con[i];}
	nbt_AnyRef ref(size_t i){return nbt_AnyRef(&con[i]);}
	nbt_Any *front()const{return con.front();}nbt_Any *back()const{return con.back();}
	void push_back(nbt_Any *any);/*接管所有权*/
	nbt_Compound *push_Compound();/*末尾加一个空compound并返回*/
	void pop_back();/*删掉最后一个*/
	void erase(size_t i);void insert(size_t i,nbt_Any *any);
	void clear();/*全删掉*/
	typedef std::vector<nbt_Any*>::iterator iterator;
	typedef std::vector<nbt_Any*>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};

struct Block{/*两个指针都指在chunk树里，别释放；name这些每次从NBT现取，不单独存*/
	nbt_Any *block;nbt_Any *entity;
	std::string_view name()const;/*Name原样*/
	std::string_view nspace()const;/*冒号前那段，没冒号算minecraft（namespace是关键字，只能叫nspace）*/
	std::string_view basename()const;/*冒号后那段，没冒号就是整个name*/
	std::string strid()const;/*nspace+':'+basename*/
	nbt_Compound *properties()const;/*block里的Properties，没有就NULL*/
};
/*方块状态的小工具（block.cpp）：Name/Properties都从NBT现取，返回的指针都指在树里，树没了就作废*/
const char *get_Name(const nbt_Any *b);/*block里的Name原样；不是compound/没有就给空串*/
const char *get_Basename(const nbt_Any *b);/*冒号后面那段；没有冒号就是整个name*/
const char *get_Prop(const nbt_Any *b,const char *key);/*Properties.<key>的字符串值，没有给NULL*/
int get_Prop_Is(const nbt_Any *b,const char *key,const char *val);/*属性正好等于val？*/

/*section级的方块状态（section本身就是个compound，和Chunk无关）；带cur的版本用游标里的调色板，少查几次*/
nbt_Any *get_BlockData(nbt_Compound *section,int lx,int ly,int lz);
nbt_Any *get_BlockData(nbt_Compound *section,struct Chunk_Cursor *cur,int lx,int ly,int lz);
int set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz);
int set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz,struct Chunk_Cursor *cur);
int section_Const(nbt_Compound *section,struct Chunk_Cursor *cur,nbt_Any **block);/*整段都是一个方块？空气算NULL*/

struct Chunk_Cursor{/*连着读写同一个区块时的游标：把"sy→section→调色板→data"记住，省得每个方块都把sections列表扫一遍
					   里面的指针都指在树里；换区块、section列表长度变了、section成员数变了，它自己会发现重查。
					   一个游标别跨区块用：level的每个缓存槽有一个，merge自己每个区块reset一次*/
	const Chunk *chunk=NULL;/*现在跟的是哪个区块*/
	uint64_t serial=0;/*那个区块的serial：同地址换了区块时靠它认出来*/
	nbt_List *secs=NULL;/*sections那个list（对象不会换，只会变长）*/
	size_t secs_len=0;/*当时列表多长*/
	int32_t sy=INT32_MIN;/*上次查的段号*/
	nbt_Compound *sec=NULL;/*那一段，NULL=没有*/
	nbt_Compound *states_sec=NULL;/*下面几个属于哪个section*/
	size_t states_members=0;/*当时section有几个成员*/
	nbt_Compound *states=NULL;/*block_states，NULL=没有（纯空气段）*/
	nbt_List *palette=NULL;/*states.palette*/
	nbt_Long_Array *data=NULL;/*states.data，NULL=全都指向palette[0]*/
	size_t data_len=0;/*上面这个data是palette多长的时候查的（加方块会让palette变长，那就重查）*/
	nbt_List *bes=NULL;/*chunk的block_entities列表，NULL=没有；bes_ok=0表示这块缓存失效*/
	int bes_ok=0;
	void reset();/*换区块/自己大改过树就清一下*/
	int follows(const Chunk *c)const;/*这游标跟的是不是这个区块（地址一样但serial不同也算没跟上）*/
	/*内部用（chunk.cpp/block.cpp）*/
	nbt_Compound *find_Section(const Chunk *c,int32_t sy);
	nbt_Compound *states_Of(nbt_Compound *section);
	nbt_List *block_Entities(const Chunk *c);/*chunk的block_entities列表，游标里记一次*/
};

struct Chunk:nbt_Compound{/*一个区块；只加成员函数，不加数据成员（delete要按nbt_Compound走）*/
	static Chunk *read(nbt_Raw *raw);/*从一个NBT流读一个区块*/
	static Chunk *read(const char *path);/*读3.txt这种单区块文件，失败返回NULL*/
	nbt_Any *get_Section(int32_t sy)const;/*线性扫；连着读同一段就给游标*/
	nbt_Any *get_Section(int32_t sy,Chunk_Cursor *cur)const;
	Block get_Block(int x,int y,int z,Chunk_Cursor *cur=NULL)const;/*x,z相对0~15，y是世界绝对Y*/
	int set_Block(Block block,int x,int y,int z,Chunk_Cursor *cur=NULL);/*返回是否成功*/
	int set_BlockEntity(nbt_Any *entity,int x,int y,int z);/*entity为NULL就删掉该位置的方块实体*/
};
/*"这个区块生成完了没有/是不是空壳"两个判断（chunk.cpp）：用来认C2ME/生成中断留在盘上的残块，
  见ReadMeDeveloper里"Chunky跳过已存在区块"那条坑。merge拿它当"整块跳过"的依据，prune拿它当"删哪个"的依据*/
int chunk_No_Block(const Chunk *c);/*1=一个方块都没有；0=有方块（形状认不出来时也给0，宁可信其有）*/
int chunk_Full_Status(const Chunk *c);/*1=Status是minecraft:full；0=还没生成完；-1=没有Status字段（不知道）*/
static_assert(sizeof(Chunk)==sizeof(nbt_Compound),"Chunk不能加数据成员：delete要按nbt_Compound走");

struct mca_Region{/*一个.mca文件的内存镜像，放不下会在末尾追加sector*/
	std::vector<byte> data;
	int bad=0;/*文件**存在但读不出来**（权限/IO/是个目录/不是普通文件）：这时 data 是空的，但绝不能说
	              "这个区域没有区块"——那会让调用者拿空镜像去写盘，把没碰过的区块整片抹掉。
	              调用者看到 bad=1 应当：不写这个文件、不删它、报错并让退出码非0（见merge/level）*/
	static mca_Region read(const char *path,int32_t x,int32_t z);/*path是目录；没有这个文件就是空的*/
	static mca_Region empty();/*新建8KB空区域，用来写还不存在的mca*/
	Chunk *read_Chunk(uint32_t x,uint32_t z)const;/*读出来的区块要自己free*/
	nbt_Compound *read_Compound(uint32_t x,uint32_t z)const;/*读一个"裸"区块：实体/POI那种，不按方块区块解析；读不出来返回NULL*/
	int has_Chunk(uint32_t x,uint32_t z)const;/*表头这一格有区块吗*/
	int erase_Chunk(uint32_t x,uint32_t z);/*把这一格从表头删掉（偏移/扇区数/时间戳清0，数据区留空洞）；删了返回1*/
	int is_Empty()const;/*表头一格都没有：这个文件等于不存在（用来决定"存盘还是删文件"）*/
	int write_ChunkAt(nbt_Compound *chunk,uint32_t x,uint32_t z);/*只改内存镜像，返回是否成功*/
	void write_Chunk(nbt_Compound *chunk);/*不返回，坐标从chunk自己的xPos/zPos取，失败只打印*/
	int write_File(const char *file)const;/*先写.tmp再rename，避免写坏存档*/
	int write(const char *path,uint32_t x,uint32_t z)const;/*path是目录，文件名和read的一样*/
};
void mca_Path(char *out,size_t size,const char *dir,int32_t x,int32_t z);/*拼出 dir/r.x.z.mca（读/写/删都用同一个拼法）*/

/*---- POI（poi/ 目录里的"方块索引"：村民认领床位/工作站/钟，传送门找出口门都用它）----
  一个POI区块的形状（wiki Point of Interest format + 26.1.1实测）：
      { DataVersion:Int, Sections:{ "<段Y>":{ Valid:Byte, Records:[ {type:String,pos:[I;x,y,z],free_tickets:Int} ] } } }
  只有"有记录的段"才写键（原版新生成的区块就是这样，实测板上一条空键都没有）；
  free_tickets＝还剩几张票，0表示这个POI不能被认领（非村民类如传送门/蜂巢/避雷针本来就是0）。
  规矩：记录必须和那一格的方块对得上——对不上、认不出类型、表里没有的，都删掉这条记录（见ReadMeDeveloper）*/
const char *poi_Type_Of_Block(const nbt_Any *block);/*方块状态→POI类型全名（"minecraft:home"这种）；不是POI方块/认不出返回NULL*/
int32_t poi_Ticket_Max(const char *type);/*这个POI类型最多几张票：村民类1、钟32、非村民类（传送门/蜂巢/避雷针/磁石）0*/
int poi_Shape_Ok(const nbt_Compound *chunk);/*容器形状对不对：Sections是compound、段键是十进制整数、每段是compound、Records是list；
												不对返回0（调用者整块跳过——不能当"这块没记录"，那会把记录默默丢了）*/
int poi_Count(const nbt_Compound *chunk);/*这个区块一共几条记录*/
int poi_Get_At(const nbt_Compound *chunk,int idx,int32_t *x,int32_t *y,int32_t *z,const char **type,int32_t *free_tickets);
	/*按下标取一条记录（记录形状不对返回0，调用者当坏数据跳过）；type指在树里，树没了就作废*/
const nbt_Any *poi_Get(const nbt_Compound *chunk,int32_t x,int32_t y,int32_t z);/*找这一格的记录，没有返回NULL*/
int poi_Set(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z,const char *type,int32_t free_tickets);/*加/改一条记录（段和Valid自动补）*/
int poi_Del(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z);/*删这一格的记录，返回删了几条*/
int poi_Clean(nbt_Compound *chunk);/*把Records空的段删掉（记录集合＝段键集合，和原版新生成的区块一致），返回删了几个段*/
int entity_Check(const nbt_Compound *chunk,int32_t cx,int32_t cz);/*实体区块形状对不对：Position==槽位、DataVersion在、Entities是list；不对返回0并计数*/

/*存档结构在26.1快照6（DataVersion 4774）改过一次：维度从 region/、DIM-1/、DIM1/ 挪到
  dimensions/minecraft/<dim>/region/。level按DataVersion严格判，不按目录存在与否猜（见level.cpp）*/
#define DAT_NEW_LAYOUT 4774

struct level{/*一个存档目录（world_api.py的level）：区域按需读、区块按槽缓存、save/close时统一落盘*/
	std::string path;/*存档目录*/
	int32_t data_version=0;/*level.dat里的Data.DataVersion（Int）；读不到是0，看dat_ok*/
	int dat_ok=0;/*level.dat读到了、DataVersion也在？0表示这目录不像存档（程序应当拒绝干活）*/
	int new_layout=0;/*DataVersion>=DAT_NEW_LAYOUT：用dimensions/新布局，否则老布局*/
	std::string version_name;/*level.dat里Version.Name，只用来打印（"26.1.1"这种）*/
	std::string region_dir[3];/*0=主世界 1=下界 2=末地；相对存档目录的路径（不带结尾斜杠）*/
	std::string entities_dir[3];/*同上，entities/ 目录（非方块实体：村民/宠物/展示框/箱车…）*/
	std::string poi_dir[3];/*同上，poi/ 目录（方块索引：床/工作站/钟/传送门…）*/
	int cache_cnt;/*区块缓存槽数（直接映射不是LRU，和python一样，默认1）*/
	int region_cnt;/*区域镜像最多留几个（一个8MB），超出把最久没用的写盘扔掉；<=0表示不限*/
	int err_cnt=0;/*内部出错次数（写区块/写区域失败），外面看这个知道有没有丢东西*/
	struct Cache{/*一个区块缓存槽*/
		int32_t cx=0,cz=0;int dim=-1;/*dim=-1表示空槽*/
		Chunk *chunk=NULL;
		bool changed=false;/*改过就要写回*/
		Chunk_Cursor cur;/*这个槽的读游标，换区块时会reset*/
	};
	struct Region{/*一个区域文件的内存镜像*/
		int32_t rx=0,rz=0;int dim=-1;
		mca_Region mca;
		bool changed=false;
		uint64_t use=0;/*LRU时间戳*/
	};
	/*get_Chunk 上一次为什么没读到区块（每次调用都重新填，成功时是GET_OK）：
	  "文件不在"和"表里没这一格"都是正常的（那片还没生成），坏数据那两种另外还计进 nbt_bad_*。
	  merge 拿它把"跳过"按原因分类报数（用户要的四类：mca不存在/表里没这区块/数据异常/Status≠full，
	  最后一类不是这里判的，见 merge.cpp）*/
	enum{GET_OK=0,     /*读到了*/
	     GET_NO_FILE=1,/*区域文件（或整个区域目录）不在*/
	     GET_NO_CHUNK=2,/*文件在，但表头那一格是空的：这个区块不在*/
	     GET_BAD_FILE=3,/*文件在、读不出来（权限/IO/不足8KB）：坏数据，这个区域不写*/
	     GET_BAD_CHUNK=4,/*表头说这格有区块，但压缩/NBT坏，读不出来*/
	     GET_ARG=5};   /*参数写错（cache_id太大之类）：调用方自己的bug*/
	int miss_why=GET_OK;
	std::vector<Cache> cache;
	std::vector<Region> regions;
	uint64_t use_cnt=0;
	level(const char *path,int cache_cnt=1,int region_cnt=8);
	~level();/*自动close*/
	void close();/*落盘+把缓存的区块全free掉，可以重复调；写盘失败就记err_cnt，然后内存里的改动就没了*/
	int save();/*脏区块→区域镜像，脏镜像→文件；返回是否全成功*/
	static int dim_Of(const char *world);/*维度名→0/1/2，认nether/end关键字，其它算主世界*/
	static const char *dim_Name(int dim);/*0/1/2→"minecraft:overworld"这种全名*/
	/*x,z是世界坐标；chunk/block的指针都指在缓存里，下次取别的区块就可能失效，要留着得copy()*/
	Chunk *get_Chunk(int32_t cx,int32_t cz,const char *world="minecraft:overworld",int cache_id=0);/*没有返回NULL*/
	Block get_Block(int x,int y,int z,const char *world="minecraft:overworld",int cache_id=0);
	int set_Block(int x,int y,int z,const char *world,Block block,int cache_id=0);/*返回是否成功*/
	int set_Block(int x,int y,int z,const char *world,nbt_Any *block,nbt_Any *entity=NULL,int cache_id=0);
	int mark_Changed(int32_t cx,int32_t cz,const char *world="minecraft:overworld",int cache_id=0,int keep_cur=0);/*自己动过区块树就手动标脏；
			keep_cur=1表示这次改动是走Chunk::set_Block写的（游标自己是一致的，别清）*/
	Chunk_Cursor *cursor(int cache_id=0);/*借这个槽的读游标（别存着这个指针；换区块时level自己会reset）*/
	/*下面几个是内部用的，指针下次取就可能作废*/
	Region *get_Region(int dim,int32_t rx,int32_t rz,int create);
	int flush_Chunk(size_t i);int flush_Region(size_t i);
	void drop_Cache(size_t i);
};

std::vector<byte> inflate_data(const byte *in,size_t in_len);/*gzip/zlib都认；出错返回空并计 nbt_bad_chunk*/
std::vector<byte> deflate_data(const byte *in,size_t in_len);/*zlib默认级别，失败返回空（都在mca.cpp）*/
int file_Read_All(const char *path,std::vector<byte> &out);/*整个文件读进out；不是普通文件（目录等）返回0（nbt.cpp）*/

/*世界Y→段号（y/16，向下取整）。看着简单，但 y=INT32_MIN 时 (y-15) 是有符号溢出（UB，UBSan会当场中止）：
  POI记录里的pos[1]是从文件读来的，坏数据给什么都有可能，所以统一用这个函数算，内部走64位*/
inline int32_t section_Y(int32_t y){
	return (int32_t)(((int64_t)y>=0)?((int64_t)y/16):(((int64_t)y-15)/16));
}

/*命令行上的整数参数：整串必须正好是一个 int32 十进制数（"abc"、"12x"、"x1=-1000" 全算错），成功返回1。
  三个工具都用它代替 atol/atoi——那两个碰上非数字会**静默给0**、超出 int32 会静默回绕，
  装在坐标参数上就是"悄悄按另一个范围跑"（fixer/prune 还是就地改，正是 ReadMe 里说要防的那种事）*/
inline int arg_Int(const char *s,int32_t *out){
	if(!s||!*s){
		return 0;
	}
	errno=0;
	char *end=NULL;
	long long v=strtol(s,&end,10);
	if(errno||!end||end==s||*end||v<INT32_MIN||v>INT32_MAX){
		return 0;
	}
	*out=(int32_t)v;
	return 1;
}

/*坏数据计数（全局、单线程）：读存档时碰到坏数据就"跳过这一块"并累加，**不再 exit(-1)**——
  坏区块只影响它自己，不该让整趟merge/fixer停在那儿。merge/fixer跑完会把这些数打进报告里，
  0就说明一个坏区块都没有、全读到了（详见ReadMeDeveloper的"坏数据策略"）。
  nbt_bad_nbt：NBT流本身坏了（长度越界、根tag不对、tag不认识）
  nbt_bad_chunk：区块读不出来（压缩数据坏、压缩类型不认识）
  nbt_bad_poi：POI区块读不出来、或记录形状不对（Sections/Records/pos/type/free_tickets不对）
  nbt_bad_entity：实体区块读不出来、Position对不上槽位、没有DataVersion或Entities不是list*/
extern int64_t nbt_bad_nbt;
extern int64_t nbt_bad_chunk;
extern int64_t nbt_bad_poi;
extern int64_t nbt_bad_entity;
#define BAD_SHOW 10/*坏数据最多打这么多条详情，之后的只计数（mca.cpp/chunk.cpp用）*/

/*NBT嵌套深度上限：真存档的树深不过十几层（区块/实体/POI都是浅的），但"别人给的"文件里
  一层 Compound 只要 0A 00 00 三字节，几万层就能把8MB栈打穿（SIGSEGV，连报告都没有）。
  超了就按坏数据处理：记一笔、这一块跳过（不 exit、不动存档）*/
#define NBT_DEPTH_MAX 256
struct nbt_Raw{/*中间过程：一段NBT字节+游标；内存自己管，外面基本不用碰*/
	std::vector<byte> data;size_t pos=0;
	int fail=0;/*读到坏数据了（越界/坏tag）：这一块作废，读出来的入口会给NULL，别拿半截数据去改存档*/
	size_t depth=0;/*当前递归深度（read_Compound/read_List 进出各±1，见 NBT_DEPTH_MAX）*/
	nbt_Raw(){}
	nbt_Raw(const std::vector<byte> &src):data(src){}
	void read_bytes(void *ret,size_t size,size_t len,bool direction=0);/*direction=0时元素内反序（大端）*/
	int read_Need(size_t need);/*读之前先看剩下的字节够不够：不够返回0并记一笔（坏文件里的长度字段可能是几十亿）*/
	void write_bytes(const void *src,size_t size,size_t len,bool direction=0);/*read_bytes的逆*/
	nbt_Any *read_Any(uint8_t type);/*建树，读出来的节点要自己free；坏tag返回NULL*/
	nbt_Byte *read_Byte();nbt_Short *read_Short();nbt_Int *read_Int();nbt_Long *read_Long();
	nbt_Float *read_Float();nbt_Double *read_Double();nbt_Byte_Array *read_Byte_Array();nbt_String *read_String();
	nbt_List *read_List();nbt_Compound *read_Compound();nbt_Int_Array *read_Int_Array();nbt_Long_Array *read_Long_Array();
	void read_Root_Header();/*跳过根tag+名字，检查确实是compound*/
	nbt_Compound *read_Root();/*读一个根compound；坏数据返回NULL（已经计过数）*/
	static nbt_Raw load(const char *path);/*读整个文件；失败返回空*/
	static nbt_Raw load_Dat(const char *path);/*level.dat，gzip/zlib/裸数据都认*/
	int save(const char *path)const;/*写data.size()个字节*/
};
#endif
