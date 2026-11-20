#include "nbtrw.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
/*chunk.cpp：区块（世界坐标那套），和原始NBT操作分开
 *	section本身就是个compound，它的方块状态（调色板/子区块）读写也在这里：调色板和子区块属于区块级，
 *	所以 section 级的函数都要传 Chunk*（段表/调色板/data 走区块的索引，编码按区块的 DataVersion）。
 *	Chunk = 区块的 nbt_Compound（con，按值持有）+ 索引：Y→section、每段的 palette/data、方块实体坐标→下标。
 *	连着按格子读一个区块时，不用每个方块都把sections列表扫一遍、也不重复哈希block_states/palette/data。
 *	能 O(1) 从树里读的（生成状态、DataVersion、xPos/zPos、block_states 成员本身）每次现读，不缓存。
 *	库接口改树时自己更新/重建索引；绕过接口直接改树后要 reindex()（见 nbtrw.hpp 的 Chunk 注释）。
 *	格式按区块根上的DataVersion定（不看level.dat：老区块没升级前还是老格式）：
 *	  字段在哪一层、叫什么（2842 以前在 Level 里、老键名；Chunk_Layout/get_ChunkBody），
 *	  段的方块状态（2832 以前是段里的 Palette/BlockStates，2527 以前下标跨 long；照游戏升级再读的样子判，get_StatesView）、
 *	  调色板条目的编码（26.3快照7起 id/properties/字符串，见 nbtrw.hpp 的 State_Format）、
 *	  生成状态的字段名（26.4快照1起 status）。
 *	不完整（生成状态不是minecraft:full，或者根本没有这个字段）的区块：get_Block返回空id的Block、
 *	set_Block返回false——残块不是玩家的东西，不读也不往里写。
 *	缺某一段section＝空气（返回id=minecraft:air的有效Block）。
 *	POI 区块（poi/ 下的方块索引）和实体表（区块里的、entities/ 下实体区块里的）的读写也在这里：
 *	它们没有结构体，直接对 nbt_Compound 操作；一条 POI/一个实体（POI/Entity）在 atom.cpp。
 */

/*---- 段的Y：照游戏读（SerializableChunkData.parse 的 getByteOr("Y",0)） ----*/
static int8_t get_FloorByte(double v){/*Java 的 (byte)(Mth.floor(v)&0xFF)：先向下取整成int（越界饱和、NaN当0），再取低8位*/
	double f=std::floor(v);
	int32_t i=0;
	if(std::isnan(f)){
		i=0;
	}else if(f>=2147483647.0){
		i=INT32_MAX;
	}else if(f<=-2147483648.0){
		i=INT32_MIN;
	}else{
		i=(int32_t)f;
	}
	return (int8_t)(uint8_t)((uint32_t)i&0xFFu);
}
int8_t read_SectionY(const nbt_Compound *sec){/*段的 Y 照游戏读成 byte（声明处有说明）；sec 为 NULL 也给 0*/
	const nbt_Any *y=sec?sec->get("Y"):NULL;
	if(!y){
		return 0;
	}
	switch(y->type){
	case NBT_BYTE:
		return ((const nbt_Byte*)y)->con;
	case NBT_SHORT:
		return (int8_t)(uint8_t)((uint16_t)((const nbt_Short*)y)->con&0xFFu);
	case NBT_INT:
		return (int8_t)(uint8_t)((uint32_t)((const nbt_Int*)y)->con&0xFFu);
	case NBT_LONG:
		return (int8_t)(uint8_t)((uint64_t)((const nbt_Long*)y)->con&0xFFu);
	case NBT_FLOAT:
		return get_FloorByte((double)((const nbt_Float*)y)->con);
	case NBT_DOUBLE:
		return get_FloorByte(((const nbt_Double*)y)->con);
	default:
		return 0;/*不是数值：游戏当0*/
	}
}

/*---- 区块格式按 DataVersion（分界见 nbtrw.hpp 的 Chunk_Layout 注释） ----*/
Chunk_Layout get_ChunkLayout(int32_t dv){
	Chunk_Layout L;
	L.dv=dv;
	if(dv<=0){
		return L;/*没有 DataVersion：当最新的格式读（工具自己造的区块常常不写；游戏会当它是 -1、从头升级，fixer 不动它）*/
	}
	L.level=dv<CHUNK_FLAT;
	L.states=dv>=CHUNK_STATES;
	L.span=dv<CHUNK_ALIGN;
	L.fmt=get_StateFormat(dv);
	if(L.level){
		L.entities="Entities";
		L.sections="Sections";
		L.block_entities="TileEntities";
		L.block_ticks="TileTicks";
		L.structures="Structures";
		L.starts="Starts";
	}
	if(dv<CHUNK_FLUID_TICKS){
		L.fluid_ticks="LiquidTicks";
	}
	L.status=get_StatusKey(dv);
	if(!L.states){
		L.palette="Palette";
		L.data="BlockStates";
	}
	return L;
}
const nbt_Compound *get_ChunkBody(const nbt_Compound *c){/*区块字段所在的那一层：2842 以前是根的 Level（没有/不是 compound 给 NULL），之后就是根*/
	if(!c||!get_ChunkLayout(c).level){
		return c;
	}
	const nbt_Any *l=c->get("Level");
	return l&&l->type==NBT_COMPOUND?(const nbt_Compound*)l:NULL;
}
nbt_Compound *get_ChunkBody(nbt_Compound *c){/*同上，可写的版本*/
	return (nbt_Compound*)get_ChunkBody((const nbt_Compound*)c);
}
const nbt_Compound *get_SectionStates(const nbt_Compound *sec,const Chunk_Layout &L){/*段的方块状态容器：2832 起是段里的 block_states（没有/不是 compound 给 NULL），之前就是段本身*/
	if(!sec||!L.states){
		return sec;
	}
	const nbt_Any *bs=sec->get("block_states");
	return bs&&bs->type==NBT_COMPOUND?(const nbt_Compound*)bs:NULL;
}
nbt_Compound *get_SectionStates(nbt_Compound *sec,const Chunk_Layout &L){/*同上，可写的版本*/
	return (nbt_Compound*)get_SectionStates((const nbt_Compound*)sec,L);
}
/*区块的某个字段（在 body 那一层）*/
static const nbt_Any *get_BodyMember(const nbt_Compound *c,const char *Chunk_Layout::*key){
	Chunk_Layout L=get_ChunkLayout(c);
	const nbt_Compound *b=get_ChunkBody(c);
	return b?b->get(L.*key):NULL;
}

/*---- Chunk 的索引 ----*/
static inline bool is_SectionY(int32_t sy){return sy>=-128&&sy<=127;}/*段号Y是Byte，超出范围的段不存在*/
static inline uint8_t get_SlotY(int32_t sy){return (uint8_t)sy;}/*按uint8存：负的从后往前（-1→255）*/
static bool get_ChunkPos(const nbt_Compound *c,int32_t *x,int32_t *z){/*xPos/zPos（在 body 那一层）：游戏是 getIntOr，任何数值都认；不是数值就读不到*/
	const nbt_Compound *b=get_ChunkBody(c);
	std::optional<int32_t> xp=get_TagInt(b?b->get("xPos"):NULL),zp=get_TagInt(b?b->get("zPos"):NULL);
	if(!xp||!zp){
		return false;
	}
	*x=*xp;
	*z=*zp;
	return true;
}
/*方块实体在游戏里落在本区块哪一格：BlockEntity.getPosFromTag 按 getIntOr(x/y/z,0) 读，不在本区块的折回本区块
  （WARN），所以区块内的 x/z 总是坐标的低4位*/
static uint64_t get_BlockEntityKeyOf(const nbt_Compound *be){
	int32_t x=get_TagInt(be->get("x")).value_or(0),y=get_TagInt(be->get("y")).value_or(0),z=get_TagInt(be->get("z")).value_or(0);
	return get_BlockEntityKey((int)((uint32_t)x&15u),y,(int)((uint32_t)z&15u));
}
void Chunk::clear_Index()const noexcept{/*索引全部回到“还没建”的样子（树被拿走/移动走以后用）*/
	memset(sec_by_y,0,sizeof(sec_by_y));
	memset(palette_by_y,0,sizeof(palette_by_y));
	memset(data_by_y,0,sizeof(data_by_y));
	memset(states_by_y,0,sizeof(states_by_y));
	be_by_pos.clear();
	idx_dv=0;
	idx_states=true;
	idx_span=false;
	idx_secs_any=NULL;
	idx_secs=NULL;
	idx_secs_len=0;
	idx_bes=NULL;
	idx_bes_len=0;
}
void Chunk::take_Index(Chunk &o)noexcept{/*树的节点是移过来的（地址没变），索引照样指得对*/
	memcpy(sec_by_y,o.sec_by_y,sizeof(sec_by_y));
	memcpy(palette_by_y,o.palette_by_y,sizeof(palette_by_y));
	memcpy(data_by_y,o.data_by_y,sizeof(data_by_y));
	memcpy(states_by_y,o.states_by_y,sizeof(states_by_y));
	be_by_pos=std::move(o.be_by_pos);
	/*建索引时的版本/格式也要接过来：以前漏了这三个，接过来的索引带着“DataVersion=0”，
	  下一次读的时候 check_Sections 看到版本对不上，把段表白白重建一遍（每个从 MCA 读出来的区块都多建一次）*/
	idx_dv=o.idx_dv;
	idx_states=o.idx_states;
	idx_span=o.idx_span;
	idx_secs_any=o.idx_secs_any;
	idx_secs=o.idx_secs;
	idx_secs_len=o.idx_secs_len;
	idx_bes=o.idx_bes;
	idx_bes_len=o.idx_bes_len;
	o.clear_Index();
}
Chunk &Chunk::operator=(const Chunk &o){/*拷贝赋值：先整个拷一份（会抛异常的都在这一步），再挪进来；拷贝失败原对象不动*/
	if(this!=&o){
		Chunk t(o);
		*this=std::move(t);
	}
	return *this;
}
Chunk &Chunk::operator=(Chunk &&o)noexcept{/*移动赋值：树的节点不动，索引直接接过来，o 清空*/
	if(this!=&o){
		con=std::move(o.con);
		take_Index(o);
	}
	return *this;
}
void Chunk::reindex()const{/*段表和方块实体索引都按现在的树重建*/
	index_Sections();
	index_BlockEntities();
}
void Chunk::index_Slot(uint8_t i)const{/*重读 sec_by_y[i] 这一段的 palette/data 指针和 states_by_y（写过这一段、可能刚建了 data 以后调）*/
	palette_by_y[i]=NULL;
	data_by_y[i]=NULL;
	states_by_y[i]=0;
	nbt_Compound *sec=sec_by_y[i];
	if(sec){
		Chunk_Layout L=get_ChunkLayout(idx_dv);
		States_View v=get_StatesView(sec,L,false);
		states_by_y[i]=!v.canonical||v.kind==STATES_BAD?-1:(v.kind==STATES_AIR?0:(int32_t)v.n_game);
	}
	nbt_Any *bs=sec;
	if(sec&&idx_states){
		bs=sec->get("block_states");
	}
	if(!bs||bs->type!=NBT_COMPOUND){/*没有block_states（纯光段）或者形状坏：读的时候现查是哪一种*/
		return;
	}
	nbt_Any *p=((nbt_Compound*)bs)->get(idx_states?"palette":"Palette");
	palette_by_y[i]=p&&p->type==NBT_LIST?(nbt_List*)p:NULL;
	nbt_Any *d=((nbt_Compound*)bs)->get(idx_states?"data":"BlockStates");
	data_by_y[i]=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
}
void Chunk::index_Sections()const{/*重建段表：记下 DataVersion/格式和 sections 成员，按 Y 把每段放进 256 格，再逐段 index_Slot*/
	memset(sec_by_y,0,sizeof(sec_by_y));
	memset(palette_by_y,0,sizeof(palette_by_y));
	memset(data_by_y,0,sizeof(data_by_y));
	memset(states_by_y,0,sizeof(states_by_y));
	idx_secs_any=NULL;
	idx_secs=NULL;
	idx_secs_len=0;
	idx_dv=get_ChunkDataVersion(&con);
	Chunk_Layout L=get_ChunkLayout(idx_dv);
	idx_states=L.states;
	idx_span=L.span;
	const nbt_Compound *body=get_ChunkBody(&con);
	const nbt_Any *any=body?body->get(L.sections):NULL;
	idx_secs_any=any;
	if(!any||any->type!=NBT_LIST){
		return;
	}
	const nbt_List *list=(const nbt_List*)any;
	idx_secs=list;
	idx_secs_len=list->con.size();
	for(size_t k=0;k<list->con.size();k++){
		nbt_Any *s=list->con[k];
		if(!s||s->type!=NBT_COMPOUND){/*不是compound的元素游戏也跳过*/
			continue;
		}
		/*Y照游戏读（截断/缺了当0）；同一Y有几段时后面的覆盖前面的，和游戏一样*/
		sec_by_y[(uint8_t)read_SectionY((const nbt_Compound*)s)]=(nbt_Compound*)s;
	}
	for(int i=0;i<256;i++){
		if(sec_by_y[i]){
			index_Slot((uint8_t)i);
		}
	}
}
void Chunk::check_Sections(const nbt_Any *secs,int32_t dv)const{/*调用者已经现读了 sections 成员和 DataVersion：和建索引时记的对不上就重建*/
	const nbt_List *list=secs&&secs->type==NBT_LIST?(const nbt_List*)secs:NULL;
	if(dv!=idx_dv||secs!=idx_secs_any||(list&&list->con.size()!=idx_secs_len)){/*格式变了/成员换了/长度变了：外面直接改过树*/
		index_Sections();
	}
}
/*读格子都要走这里：DataVersion 只读一次（get_BodyMember 会读三遍，逐格调用时很费）*/
static const nbt_Any *get_SectionsMember(const nbt_Compound *c,int32_t dv){
	Chunk_Layout L=get_ChunkLayout(dv);
	const nbt_Compound *b=c;
	if(L.level){
		const nbt_Any *l=c->get("Level");
		b=l&&l->type==NBT_COMPOUND?(const nbt_Compound*)l:NULL;
	}
	return b?b->get(L.sections):NULL;
}
void Chunk::check_Sections()const{/*自己现读一遍再核对（读一格以外的入口用）*/
	int32_t dv=get_ChunkDataVersion(&con);
	check_Sections(get_SectionsMember(&con,dv),dv);
}
const nbt_Any *Chunk::get_Sections()const{/*核对索引，顺便给出 sections 成员（任何类型；没有给 NULL）*/
	check_Sections();
	return idx_secs_any;
}
void Chunk::index_BlockEntities()const{/*重建方块实体索引：区块内位置 → 在 block_entities 里的下标（同一格几条记最后那条）*/
	be_by_pos.clear();
	idx_bes=NULL;
	idx_bes_len=0;
	const nbt_Any *ba=get_BodyMember(&con,&Chunk_Layout::block_entities);
	if(!ba||ba->type!=NBT_LIST){
		return;
	}
	const nbt_List *list=(const nbt_List*)ba;
	idx_bes=list;
	idx_bes_len=list->con.size();
	be_by_pos.reserve(list->con.size());
	for(size_t i=0;i<list->con.size();i++){
		const nbt_Any *e=list->con[i];
		if(!e||e->type!=NBT_COMPOUND){/*不是compound的游戏也跳过*/
			continue;
		}
		/*同一格有几条时后面的覆盖前面的（游戏 setBlockEntity 也是后放的顶掉前面的）*/
		be_by_pos[get_BlockEntityKeyOf((const nbt_Compound*)e)]=(uint32_t)i;
	}
}
bool Chunk::check_BlockEntities(const nbt_Any *ba)const{/*ba 是现读的 block_entities 成员：和建索引时的对不上就重建；true＝是 List 而且非空（有东西可查）*/
	const nbt_List *list=ba&&ba->type==NBT_LIST?(const nbt_List*)ba:NULL;
	if(list!=idx_bes||(list&&list->con.size()!=idx_bes_len)){
		index_BlockEntities();
	}
	return list&&!list->con.empty();
}
nbt_Compound *Chunk::find_BlockEntityAt(int x,int32_t y,int z)const{/*只走索引找这一格的方块实体（调用者先 check_BlockEntities 过）；没有给 NULL*/
	if(x<0||x>15||z<0||z>15||!idx_bes){
		return NULL;
	}
	auto it=be_by_pos.find(get_BlockEntityKey(x,y,z));
	if(it==be_by_pos.end()||it->second>=idx_bes->con.size()){
		return NULL;
	}
	nbt_Any *e=idx_bes->con[it->second];
	return e&&e->type==NBT_COMPOUND?(nbt_Compound*)e:NULL;
}
/*现读 block_entities、核对索引，再按坐标找（get_Block/get_BlockEntity 用）*/
static nbt_Compound *find_BlockEntityRead(const Chunk *c,int x,int32_t y,int z){
	const nbt_Any *ba=get_BodyMember(&c->con,&Chunk_Layout::block_entities);
	if(!c->check_BlockEntities(ba)){
		return NULL;
	}
	return c->find_BlockEntityAt(x,y,z);
}

/*---- section里的方块状态：调色板+data ----*/
/*下标所在的格子：没有data、或者新格式调色板只有一条（游戏不看 data）就全是 palette[0]。
  states/span 是区块的格式（Chunk_Layout）。下标指到调色板外：新格式读不出来（游戏抛异常）；老格式游戏当空气，给 *air=true*/
/*n_game：游戏读的时候调色板有几条（states_by_y；老格式升级时可能在后面补了空气）*/
static const nbt_Any *get_PaletteAt(const nbt_List *palette,const nbt_Long_Array *data,int lx,int ly,int lz,uint32_t *out_idx,
	bool states,bool span,uint32_t n_game,bool *air){
	if(air){
		*air=false;
	}
	if(!palette||palette->con.empty()||palette->con.size()>0xFFFFFFFFu){
		return NULL;
	}
	uint32_t idx=0;
	if(palette->con.size()>1){/*单条：游戏不看 data（老格式升级时扔掉 data）*/
		uint32_t bits=get_PaletteBitsFor(palette->con.size(),states);
		if(!data||data->con.size()!=get_PackedWords(4096,bits,span)){
			return NULL;/*长度不对：游戏要么读不了、要么按别的位宽读（库不猜）*/
		}
		idx=get_PackedIndex(data->con,(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx,bits,span);
	}
	if(idx>=palette->con.size()){
		if(idx<n_game&&air){
			*air=true;/*老格式：升级时补在调色板后面的空气*/
		}
		return NULL;
	}
	if(out_idx){
		*out_idx=idx;
	}
	return palette->con[idx];
}
/*读一段里的一格（调用者先 check_Sections 过）：1=空气（缺段/纯光段，*state=NULL）、0=读到（*state是调色板条目）、
  -1=读不出来（block_states/palette 形状坏、下标越界）。
  老格式（2832 以前）：Palette 和 BlockStates 两样都在才读（游戏两样都有才建这一段），少一样＝这一段空气*/
static int read_SectionState(const Chunk *c,int32_t sy,int lx,int ly,int lz,const nbt_Any **state,uint32_t *idx){
	*state=NULL;
	if(!is_SectionY(sy)){
		return 1;
	}
	uint8_t i=get_SlotY(sy);
	const nbt_Compound *sec=c->sec_by_y[i];
	if(!sec){/*这一段压根没有＝空气（用户定的：缺section算空气）*/
		return 1;
	}
	int32_t g=c->states_by_y[i];
	if(g==0){/*没有方块状态（纯光段、老格式少一样）＝空气*/
		return 1;
	}
	if(g<0){/*游戏读不了，或者不是库能原样读的样子*/
		return -1;
	}
	bool air=false;
	*state=get_PaletteAt(c->palette_by_y[i],c->data_by_y[i],lx,ly,lz,idx,c->idx_states,c->idx_span,(uint32_t)g,&air);
	if(air){
		return 1;
	}
	return *state?0:-1;
}
const nbt_Any *get_BlockData(const Chunk *c,int32_t sy,int lx,int ly,int lz){/*这一格的调色板条目；空气（缺段/纯光段）和读不出来都给 NULL，要分清用 Chunk::get_BlockRef*/
	if(!c||lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15*/
		return NULL;
	}
	c->check_Sections();
	const nbt_Any *state=NULL;
	return read_SectionState(c,sy,lx,ly,lz,&state,NULL)==0?state:NULL;
}
bool is_SectionConst(const Chunk *c,int32_t sy,const nbt_Any **block){/*这一段是不是整段同一个方块？*/
	*block=NULL;
	if(!c){
		return false;
	}
	const nbt_Compound *sec=c->get_Section(sy);
	if(!sec){/*没有这段section＝全空气*/
		return true;
	}
	int32_t g=c->states_by_y[get_SlotY(sy)];
	if(g==0){/*没有方块状态＝全空气*/
		return true;
	}
	const nbt_List *palette=c->palette_by_y[get_SlotY(sy)];
	if(g<0||!palette||palette->con.size()!=1){
		return false;/*读不出来，或者不止一种方块*/
	}
	/*调色板只有一条：游戏不看 data（单值调色板；老格式升级时扔掉 data），整段就是这一种*/
	const nbt_Any *b=palette->con[0];
	*block=!strcmp(get_StateName(b,get_ChunkStateFormat(&c->con)),"minecraft:air")?NULL:b;/*空气一律算NULL*/
	return true;
}
/*往调色板末尾加一条：新格式里字符串和复合可能混在一起，NBT 列表只能装一种类型——
  字符串进复合列表时包成{"":ID}；复合进字符串列表时先把已有的字符串条目都原地包成{"":ID}。
  包的时候把原来的String节点挪进去（不复制），外面拿着的名字指针仍然有效。接管e；失败时e已释放。*/
static bool push_PaletteState(nbt_List *palette,nbt_Any *e){
	if(!palette||!e){
		if(e){
			e->free();
		}
		return false;
	}
	if(palette->con.empty()||palette->elem==e->type){
		return palette->push_back(e);
	}
	if(e->type==NBT_STRING&&palette->elem==NBT_COMPOUND){
		nbt_Compound *w=new nbt_Compound;
		w->set("",e);
		return palette->push_back(w);
	}
	if(e->type==NBT_COMPOUND&&palette->elem==NBT_STRING){
		for(size_t i=0;i<palette->con.size();i++){
			nbt_Compound *w=new nbt_Compound;
			w->set("",palette->con[i]);
			palette->con[i]=w;
		}
		palette->elem=NBT_COMPOUND;
		return palette->push_back(e);
	}
	e->free();
	return false;
}
static bool set_BlockDataStates(nbt_Compound *states,nbt_List *palette,const nbt_Any *block,State_Format f,
	int lx,int ly,int lz,const Chunk_Layout &L){/*把方块状态写进已经找好的方块状态容器（新格式 block_states，老格式段本身）*/
	if(!states||!get_StateNameTag(block,f)){
		return false;
	}
	if(lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15*/
		return false;
	}
	if(!palette||palette->con.empty()||palette->con.size()>0xFFFFFFFFu){
		return false;
	}
	nbt_Any *d=states->get(L.data);
	nbt_Long_Array *data=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
	size_t old_len=palette->con.size();
	uint32_t bits_old=get_PaletteBitsFor(old_len,L.states);/*MC保存时位宽总按palette长度取最小*/
	if(!bits_old){
		data=NULL;/*新格式单条调色板游戏不看 data：里面的下标当它不存在，要建 data 时整个换掉*/
	}
	if(!L.states&&!data){
		return false;/*老格式少了 BlockStates：游戏当这一段空气，不往里写（写了会把整段变成 palette[0]）*/
	}
	if(!L.states&&old_len==1){
		data->con.assign(get_PackedWords(4096,bits_old,L.span),0);/*老格式单条：游戏升级时扔掉 data，整段都是 palette[0]*/
	}else if(!L.states&&data->con.size()==get_PackedWords(4096,bits_old,L.span)){
		for(uint32_t k=0;k<4096;k++){
			if(get_PackedIndex(data->con,k,bits_old,L.span)>=old_len){
				return false;/*有格子指到调色板外（游戏补的空气）：往调色板里加条目会让它们变成别的方块，不写*/
			}
		}
	}
	/*已有同样状态就复用，不重复加：先找一模一样的条目；没有再找名字和属性相同的
	  （新格式里同一个默认状态可能写成字符串，也可能包成{"":ID}）*/
	size_t idx=old_len,same=old_len;
	for(size_t i=0;i<old_len;i++){
		const nbt_Any *e=palette->con[i];
		if(e==block||(e&&*e==*block)){
			idx=i;
			break;
		}
		if(same==old_len&&is_StateSame(e,block,f)){
			same=i;
		}
	}
	if(idx==old_len){
		idx=same;
	}
	if(idx==old_len){
		if(old_len>=0xFFFFFFFFu){/*NBT list长度4字节，放不下了*/
			return false;
		}
		if(!push_PaletteState(palette,block->copy())){
			return false;
		}
	}
	uint32_t bits=get_PaletteBitsFor(palette->con.size(),L.states);
	uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
	if(!data){
		if(idx==0&&!bits){/*新格式单条调色板：整段都是palette[0]，不用建data*/
			return true;
		}
		/*没有 data（多条调色板没 data 游戏读不进来）：建一份全0的（全是 palette[0]），写完这一段就合法了*/
		data=new nbt_Long_Array;
		data->con.resize(get_PackedWords(4096,bits,L.span),0);
		states->set(L.data,data);
	}else if(bits!=bits_old){/*位宽变了要把整段下标重排*/
		uint32_t need_old=get_PackedWords(4096,bits_old,L.span);
		if(data->con.size()<need_old){/*文件里的data比该有的短：缺的当palette[0]*/
			data->con.resize(need_old,0);
		}
		std::vector<int64_t> buf(get_PackedWords(4096,bits,L.span),0);
		for(uint32_t k=0;k<4096;k++){
			set_PackedIndex(buf,k,bits,L.span,get_PackedIndex(data->con,k,bits_old,L.span));
		}
		data->con=std::move(buf);
	}
	uint32_t need=get_PackedWords(4096,bits,L.span);
	if(data->con.size()<need){/*短了就补长再写*/
		data->con.resize(need,0);
	}
	set_PackedIndex(data->con,i,bits,L.span,(uint32_t)idx);
	return true;
}
/*这一段库能不能往里写：游戏读得了的只写库能原样读写的；游戏读不了的照旧写（data 缺了/短了补0，写完就合法）*/
static bool can_WriteStates(const nbt_Compound *sec,const Chunk_Layout &L){
	States_View v=get_StatesView(sec,L,false);
	return v.kind==STATES_BAD||(v.kind==STATES_OK&&v.canonical);
}
bool set_BlockData(Chunk *c,int32_t sy,const nbt_Any *block,int lx,int ly,int lz){/*把方块状态写进第sy段*/
	if(!c){
		return false;
	}
	nbt_Compound *sec=c->get_Section(sy);
	Chunk_Layout L=get_ChunkLayout(&c->con);
	if(!sec||!get_StateNameTag(block,L.fmt)||!can_WriteStates(sec,L)){
		return false;
	}
	nbt_Compound *states=get_SectionStates(sec,L);
	nbt_Any *p=states?states->get(L.palette):NULL;
	nbt_List *palette=p&&p->type==NBT_LIST?(nbt_List*)p:NULL;
	bool ok=set_BlockDataStates(states,palette,block,L.fmt,lx,ly,lz,L);
	c->index_Slot(get_SlotY(sy));/*这次写可能刚建了data：这一段的索引重读*/
	return ok;
}

/*---- Chunk：一个区块 ----*/
Chunk::Chunk(int32_t cx,int32_t cz){
	con.set_Int("xPos",cx);
	con.set_Int("zPos",cz);
}
bool Chunk::is_Full()const{/*生成完了才算完整（各版本的写法见 get_ChunkStatus）；没有生成状态也算不完整（宁可不动）*/
	return get_ChunkStatus(&con)==CHUNK_FULL;
}
nbt_Compound *Chunk::get_Section(int32_t sy)const{/*按section的Y找人（Y是带符号的Byte），走索引的Y表*/
	check_Sections();
	return is_SectionY(sy)?sec_by_y[get_SlotY(sy)]:NULL;
}
/*一段的 4096 个下标全解出来（不跨 long 的按字走，不做除法）*/
static void unpack_Indices(const std::vector<int64_t> &w,uint32_t bits,bool span,uint32_t *out){
	if(span){
		for(uint32_t i=0;i<4096;i++){
			out[i]=get_PackedIndex(w,i,bits,true);
		}
		return;
	}
	uint32_t per=64/bits,i=0;
	uint64_t mask=((uint64_t)1<<bits)-1;
	for(size_t k=0;i<4096&&k<w.size();k++){
		uint64_t v=(uint64_t)w[k];
		for(uint32_t j=0;j<per&&i<4096;j++,i++){
			out[i]=(uint32_t)(v&mask);
			v>>=bits;
		}
	}
	for(;i<4096;i++){
		out[i]=0;
	}
}
/*调色板压紧：只留 data 真正引用到的条目，重写全部4096格的下标。true=压过了；
  没有废弃条目、单条调色板、游戏读不了或者不是库能原样读写的样子，都返回false且一个字都不改。*/
static bool compact_Palette(nbt_Compound *sec,const Chunk_Layout &L){
	nbt_Compound *states=get_SectionStates(sec,L);
	if(!states){
		return false;
	}
	States_View v=get_StatesView(sec,L,false);
	if(v.kind!=STATES_OK||!v.canonical){
		return false;/*游戏读不了，或者不是库能原样读写的样子：不压*/
	}
	nbt_Any *pa=states->get(L.palette);
	nbt_Any *da=states->get(L.data);
	if(!pa||pa->type!=NBT_LIST){
		return false;
	}
	nbt_List *pal=(nbt_List*)pa;
	for(size_t i=0;i<pal->con.size();i++){
		if(!get_StateNameTag(pal->con[i],L.fmt)){
			return false;/*调色板条目本身坏了（这种格式下认不出名字）：不压*/
		}
	}
	size_t n=pal->con.size();
	if(n<=1||!da||da->type!=NBT_LONG_ARRAY){
		return false;/*单条：没什么可压的；多条没有data：不是能压紧的数据*/
	}
	uint32_t old_bits=get_PaletteBitsFor(n,L.states);
	const std::vector<int64_t> &old=((const nbt_Long_Array*)da)->con;
	if(old.size()!=get_PackedWords(4096,old_bits,L.span)){
		return false;
	}
	uint32_t idx_of[4096];
	unpack_Indices(old,old_bits,L.span,idx_of);
	std::vector<uint8_t> used(n,0);
	for(uint32_t i=0;i<4096;i++){
		if(idx_of[i]>=n){
			return false;/*越界：调色板/data本来就不一致，别猜*/
		}
		used[idx_of[i]]=1;
	}
	size_t used_n=0;
	for(size_t i=0;i<n;i++){
		used_n+=used[i]?1:0;
	}
	if(used_n==n){
		return false;/*没有废弃条目*/
	}
	std::vector<int32_t> remap(n,-1);
	nbt_List *np=new nbt_List(pal->elem);/*元素类型照旧（新格式可能是字符串列表）*/
	for(size_t i=0;i<n;i++){
		if(!used[i]){
			continue;
		}
		remap[i]=(int32_t)np->con.size();
		if(!np->push_back(pal->con[i]->copy())){
			np->free();
			return false;
		}
	}
	if(np->con.empty()){
		np->free();
		return false;
	}
	uint32_t bits=get_PaletteBitsFor(used_n,L.states);
	if(!bits){
		states->erase(L.data);/*新格式单值调色板不需要data*/
	}else{
		std::vector<int64_t> w(get_PackedWords(4096,bits,L.span),0);/*老格式单条也要4位的 BlockStates（全0）*/
		for(uint32_t i=0;i<4096;i++){
			set_PackedIndex(w,i,bits,L.span,(uint32_t)remap[idx_of[i]]);
		}
		states->set(L.data,new nbt_Long_Array(std::move(w)));/*旧数组在这里才释放（上面一直按 idx_of 读）*/
	}
	states->set(L.palette,np);
	return true;
}
int Chunk::format_Chunk(int *bad_y,int *palette_cleaned){/*整理区块（做什么见 nbtrw.hpp 的声明）；只有 fixer 调*/
	Chunk_Layout L=get_ChunkLayout(&con);
	nbt_Compound *body=get_ChunkBody(&con);
	if(bad_y){
		*bad_y=0;
	}
	if(palette_cleaned){
		*palette_cleaned=0;
	}
	nbt_Any *any=body?body->get(L.sections):NULL;
	if(!any){
		return 0;
	}
	if(any->type!=NBT_LIST){
		return -1;
	}
	nbt_List *secs=(nbt_List*)any;
	int removed=0;
	/*1. Y照游戏的读法改成合法的Byte：别的数值截成byte，缺了/不是数值补0（游戏就是这么读的，改完读出来的段不变）*/
	int fixed=0;
	for(size_t i=0;i<secs->con.size();i++){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		nbt_Compound *sec=(nbt_Compound*)s;
		const nbt_Any *y=sec->get("Y");
		if(!y||y->type!=NBT_BYTE){
			sec->set("Y",new nbt_Byte(read_SectionY(sec)));
			fixed++;
		}
	}
	if(bad_y){
		*bad_y=fixed;
	}
	/*2. 同一Y有几段（改Y以后撞上的也算）：游戏取最后一段，前面的删掉*/
	bool seen[256]={};
	for(size_t i=secs->con.size();i>0;i--){/*从后往前：erase不会挪动更小的下标*/
		nbt_Any *s=secs->con[i-1];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		uint8_t sy=(uint8_t)read_SectionY((const nbt_Compound*)s);
		if(seen[sy]){
			secs->erase(i-1);
			removed++;
			continue;
		}
		seen[sy]=true;
	}
	/*3. 每个子区块的调色板压紧*/
	int pal_changed=0;
	for(size_t i=0;i<secs->con.size();i++){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		if(compact_Palette((nbt_Compound*)s,L)){
			pal_changed++;
		}
	}
	if(palette_cleaned){
		*palette_cleaned=pal_changed;
	}
	if(fixed||removed||pal_changed){/*改了Y/删了段/换了调色板：旧指针可能已经释放，段表重建*/
		index_Sections();
	}
	return removed;
}
/*---- 方块实体的坐标工具：xPos/zPos（Int）×16+x/z 走64位，溢出int32就当读不到 ----*/
static bool get_ChunkAbs(const nbt_Compound *c,int x,int z,int32_t *ax,int32_t *az){
	int32_t cx=0,cz=0;
	if(!get_ChunkPos(c,&cx,&cz)){
		return false;
	}
	int64_t vx=(int64_t)cx*16+x,vy=(int64_t)cz*16+z;
	if(vx<INT32_MIN||vx>INT32_MAX||vy<INT32_MIN||vy>INT32_MAX){
		return false;
	}
	*ax=(int32_t)vx;
	*az=(int32_t)vy;
	return true;
}
/*sections 成员在但不是“compound 的 List”（不是List，或非空List的元素不是compound）：
  整个区块读不出来。不能当“缺段=空气”——那会让邻格规则/merge 拿空气去覆盖真实方块。*/
static bool is_SectionsBad(const nbt_Any *sc){
	if(!sc){
		return false;
	}
	if(sc->type!=NBT_LIST){
		return true;
	}
	const nbt_List *l=(const nbt_List*)sc;
	return !l->con.empty()&&l->elem!=NBT_COMPOUND;
}
/*生成完了没有（各版本的写法见 nbtrw.hpp 的 get_ChunkStatus）；body 是字段所在的那一层（get_ChunkBody）*/
static Chunk_Full_Status read_ChunkStatus(const nbt_Compound *body,const Chunk_Layout &L){
	if(!body){
		return CHUNK_NO_STATUS;
	}
	if(L.dv>0&&L.dv<CHUNK_PROTO){/*18w06a 以前没有 Status：TerrainPopulated 为真才算生成完*/
		std::optional<int64_t> tp=get_TagLong(body->get("TerrainPopulated"));
		return !tp?CHUNK_NO_STATUS:(*tp?CHUNK_FULL:CHUNK_NOT_FULL);
	}
	const nbt_Any *st=body->get(L.status);
	if(!st||st->type!=NBT_STRING){
		return CHUNK_NO_STATUS;/*没有这个字段（老存档/别的工具写的/字段名和DataVersion对不上）：不知道*/
	}
	std::string_view v=((const nbt_String*)st)->con;
	bool full;
	if(L.dv>0&&L.dv<CHUNK_FULLCHUNK){
		full=v=="postprocessed"||v=="fullchunk";
	}else if(L.dv>0&&L.dv<CHUNK_STATUS_FULL){
		full=v=="fullchunk";
	}else{/*按名字比（Identifier.tryParse）：full、:full、minecraft:full 是同一个*/
		full=v=="full"||v==":full"||v=="minecraft:full";
	}
	return full?CHUNK_FULL:CHUNK_NOT_FULL;
}
/*读一格之前要现读的几样：DataVersion（定格式）、body（老格式的 Level）、生成状态、sections 成员。
  不完整区块/sections 形状坏 给 false*/
static bool begin_ChunkRead(const nbt_Compound &con,State_Format *fmt,const nbt_Any **secs,int32_t *dv){
	*dv=get_ChunkDataVersion(&con);
	Chunk_Layout L=get_ChunkLayout(*dv);
	const nbt_Compound *body=get_ChunkBody(&con);
	if(read_ChunkStatus(body,L)!=CHUNK_FULL){
		return false;
	}
	*fmt=L.fmt;
	*secs=body->get(L.sections);
	return !is_SectionsBad(*secs);/*sections成员形状坏：坏数据，不能当空气（会把对方方块清掉）*/
}
Block Chunk::get_Block(int x,int y,int z)const{/*读一格：空 id＝读不到（区块不完整/坏段/坏下标/坐标越界），缺段和纯光段给 minecraft:air；带上这一格方块实体的深拷贝*/
	State_Format fmt=STATE_NAME;
	const nbt_Any *secs=NULL;
	int32_t dv=0;
	if(x<0||x>15||z<0||z>15||!begin_ChunkRead(con,&fmt,&secs,&dv)){/*不完整区块不读；坐标越界也不读*/
		return Block();
	}
	check_Sections(secs,dv);
	int32_t sy=get_SectionY(y);
	const nbt_Any *state=NULL;
	int r=read_SectionState(this,sy,x,y-sy*16,z,&state,NULL);
	if(r>0){
		return Block("minecraft:air");
	}
	if(r<0){
		return Block();
	}
	Block b=make_StateBlock(state,fmt);/*这种格式下认不出名字：空id＝读不到*/
	if(!b.valid()){
		return b;
	}
	const nbt_Compound *be=find_BlockEntityRead(this,x,y,z);
	if(be){
		b.block_entities=*be;/*深拷贝*/
	}
	return b;
}
Section_View Chunk::view_Section(int32_t sy)const{/*开一个段视图：区块级要现读的在这里读一次、核对一遍索引；区块不完整/sections 坏时 ok=false*/
	Section_View v;
	v.chunk=this;
	v.sy=sy;
	const nbt_Any *secs=NULL;
	int32_t dv=0;
	if(!begin_ChunkRead(con,&v.fmt,&secs,&dv)){
		return v;
	}
	check_Sections(secs,dv);
	v.ok=true;
	v.bes=check_BlockEntities(get_BodyMember(&con,&Chunk_Layout::block_entities));
	return v;
}
bool Section_View::get_BlockRef(int x,int ly,int z,Block_Ref *out)const{/*视图里读一格（零拷贝）：true=读到（空气时 out->state 是 NULL）；false=读不出来*/
	if(out){
		out->state=NULL;
		out->name=NULL;
		out->props=NULL;
		out->be=NULL;
	}
	if(!out||!ok||!chunk||x<0||x>15||z<0||z>15||ly<0||ly>15){
		return false;
	}
	const nbt_Any *state=NULL;
	uint32_t idx=0;
	int r=read_SectionState(chunk,sy,x,ly,z,&state,&idx);
	if(r>0){/*缺段/纯光段：空气，按get_Block的规矩不看方块实体*/
		out->name="minecraft:air";
		return true;
	}
	if(r<0){
		return false;
	}
	const nbt_List *pal=chunk->palette_by_y[get_SlotY(sy)];/*读到了：sy 在范围内，这一段有 palette*/
	if(meta_pal!=pal||meta_name.size()!=pal->con.size()){
		meta_pal=pal;
		meta_name.resize(pal->con.size());
		meta_props.resize(pal->con.size());
		for(size_t i=0;i<pal->con.size();i++){
			meta_name[i]=get_StateNameTag(pal->con[i],fmt);
			meta_props[i]=get_StateProps(pal->con[i],fmt);
		}
	}
	const nbt_Any *name_any=meta_name[idx];
	const char *nm=name_any?((const nbt_String*)name_any)->con.c_str():NULL;
	if(!nm||!nm[0]){
		return false;
	}
	out->state=state;
	out->name=nm;
	out->props=meta_props[idx];
	if(bes){
		out->be=chunk->find_BlockEntityAt(x,sy*16+ly,z);
	}
	return true;
}
bool Chunk::get_BlockRef(int x,int y,int z,Block_Ref *out)const{/*就是开一个段视图读一格（两边语义一定一样）*/
	if(out){
		out->state=NULL;
		out->name=NULL;
		out->props=NULL;
		out->be=NULL;
	}
	if(x<0||x>15||z<0||z>15){
		return false;
	}
	int sy=get_SectionY(y);
	return view_Section(sy).get_BlockRef(x,y-sy*16,z,out);
}
bool equal_BlockRef(const Block_Ref &a,const Block_Ref &b){/*两个视图是不是同一个方块：名字、属性、方块实体都逐值相等（“没有”和“空 compound”算一样）*/
	if(!a.name||!b.name||strcmp(a.name,b.name)){
		return false;
	}
	const nbt_Any *pa=a.props,*pb=b.props;
	if(pa&&pb){
		if(!(*pa==*pb)){
			return false;
		}
	}else if(pa||pb){/*一边没有Properties：另一边只有空compound才等价（Block不区分“没有”和“空”）*/
		const nbt_Any *p=pa?pa:pb;
		if(!((const nbt_Compound*)p)->con.empty()){
			return false;
		}
	}
	if(a.be&&b.be){
		if(!(*a.be==*b.be)){
			return false;
		}
	}else if(a.be||b.be){/*同上：空方块实体compound和没有方块实体在Block里都是空*/
		const nbt_Compound *e=a.be?a.be:b.be;
		if(!e->con.empty()){
			return false;
		}
	}
	return true;
}
bool equal_BlockRef(const Block_Ref &a,const Block &b){/*视图和 Block 比，语义同上（和 Block::operator== 的结果一致）*/
	if(!a.name||b.id.con!=a.name){
		return false;
	}
	const nbt_Any *pa=a.props;
	if(pa){
		if(!(*pa==b.properties)){
			return false;
		}
	}else if(!b.properties.empty()){
		return false;
	}
	if(a.be){
		if(!(*a.be==b.block_entities)){
			return false;
		}
	}else if(!b.block_entities.empty()){
		return false;
	}
	return true;
}
static bool can_AppendBlockEntity(const nbt_Compound *c,bool has_ent){/*写方块实体前的预检；没有实体要写时坏表也不能挡住方块写入*/
	if(has_ent&&!get_ChunkBody(c)){
		return false;/*老格式没有 Level：没地方放方块实体表*/
	}
	const nbt_Any *ba=get_BodyMember(c,&Chunk_Layout::block_entities);
	if(!ba){
		return true;
	}
	if(ba->type!=NBT_LIST){
		return !has_ent;
	}
	const nbt_List *bes=(const nbt_List*)ba;
	if(!has_ent){
		return true;
	}
	if(!bes->con.empty()&&bes->elem!=NBT_COMPOUND){
		return false;
	}
	if(bes->con.empty()&&bes->elem!=NBT_END&&bes->elem!=NBT_COMPOUND){
		return false;/*空表但元素类型被定死成别的：push_back会失败*/
	}
	return true;
}
bool Chunk::set_Block(const Block &block,int x,int y,int z){/*写一格：先把所有会失败的条件查完，再原位写方块状态和方块实体；失败不改区块*/
	if(!is_Full()||!block.valid()||x<0||x>15||z<0||z>15){/*不完整区块不写；空id也写不了*/
		return false;
	}
	bool has_ent=!block.block_entities.empty();
	int32_t ax=0,az=0;
	bool have_abs=get_ChunkAbs(&con,x,z,&ax,&az);
	if(has_ent&&!have_abs){
		return false;/*带方块实体但xPos/zPos不对：先别动*/
	}
	int sy=get_SectionY(y);
	int ly=y-sy*16;
	nbt_Compound *sec=get_Section(sy);
	if(!sec){/*没有这一段：不自动建section（biome/光照不是这里能补的）*/
		return false;
	}
	Chunk_Layout L=get_ChunkLayout(&con);
	nbt_Compound *bs=get_SectionStates(sec,L);
	if(!bs){/*没有调色板：不自动造*/
		return false;
	}
	nbt_Any *pal=bs->get(L.palette);
	nbt_List *palette=pal&&pal->type==NBT_LIST?(nbt_List*)pal:NULL;
	State_Format fmt=L.fmt;
	if(!palette||palette->con.empty()||
	   (palette->elem!=NBT_COMPOUND&&!(fmt==STATE_ID&&palette->elem==NBT_STRING))){
		return false;/*老格式只有复合列表；新格式还可以是字符串列表*/
	}
	nbt_Any *dat=bs->get(L.data);
	if((dat&&dat->type!=NBT_LONG_ARRAY)||(!L.states&&!dat)){
		return false;/*2832 以前少了 BlockStates：游戏当这一段空气，不往里写*/
	}
	if(!can_WriteStates(sec,L)){
		return false;/*游戏读得了、但不是库能原样读写的样子（按别的位宽读、补了空气…）：不动*/
	}
	if(!can_AppendBlockEntity(&con,has_ent)){
		return false;/*方块实体表形状坏：在写方块状态之前就拒绝，不能留下半写状态*/
	}
	nbt_Any *st=make_State(block,fmt);/*按这个区块的格式编码*/
	/*预检已经覆盖调色板/data/方块实体表的失败路径；这里原位写，成功后再动方块实体。
	  不克隆block_states，避免fixer持有的Blk_Info指针和索引突然悬空
	  （新格式要把字符串列表改成复合列表时，原来的String节点是挪进去的，指针照样有效）。*/
	bool ret=set_BlockData(this,sy,st,x,ly,z);
	st->free();
	if(!ret){
		return false;
	}
	if(has_ent){
		if(!set_BlockEntity(block.block_entities,x,y,z)){
			return false;/*预检后只剩OOM这种不会返回false的路径；真出现时至少不谎报成功*/
		}
	}else if(have_abs){
		erase_BlockEntity(x,y,z);/*没有方块实体：删这一格旧的；坏表删不掉也不影响方块状态*/
	}
	return true;
}
bool Chunk::set_BlockEntity(const nbt_Compound &entity,int x,int y,int z){/*放/换这一格的方块实体（深拷贝进去，x/y/z 按放置位置重写）；表形状不对、坐标算不出来给 false*/
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return false;
	}
	int32_t ax=0,az=0;
	if(!get_ChunkAbs(&con,x,z,&ax,&az)){/*xPos/zPos不是数值：没法算绝对坐标*/
		return false;
	}
	Chunk_Layout L=get_ChunkLayout(&con);
	nbt_Compound *body=get_ChunkBody(&con);
	if(!body){
		return false;
	}
	nbt_Any *ba=body->get(L.block_entities);
	nbt_List *bes=NULL;
	if(ba){
		if(ba->type!=NBT_LIST){/*形状不对：不破坏它，失败*/
			return false;
		}
		bes=(nbt_List*)ba;
		if(!bes->con.empty()&&bes->elem!=NBT_COMPOUND){/*非空表里的元素不是compound：不往里塞*/
			return false;
		}
	}
	nbt_Compound *copy=new nbt_Compound(entity);
	copy->set_Int("x",ax);/*传入entity里的x/y/z一律丢掉，按放置坐标和区块xPos/zPos重算*/
	copy->set_Int("y",y);
	copy->set_Int("z",az);
	uint64_t key=get_BlockEntityKey(x,y,z);
	if(bes){
		check_BlockEntities(bes);
		auto it=be_by_pos.find(key);
		if(it!=be_by_pos.end()&&it->second<bes->con.size()){/*同一位置已有就换掉游戏用的那条（最后那条）：下标不变，索引不用动*/
			bes->con[it->second]->free();
			bes->con[it->second]=copy;
			return true;
		}
		if(!bes->push_back(copy)){
			return false;/*push_back失败时已经释放了copy*/
		}
		be_by_pos[key]=(uint32_t)(bes->con.size()-1);/*新加在末尾：等于重建，只是不用全扫*/
		idx_bes_len=bes->con.size();
		return true;
	}
	bes=new nbt_List(NBT_COMPOUND);
	body->set(L.block_entities,bes);
	bool ok=bes->push_back(copy);/*push_back失败时已经释放了copy*/
	index_BlockEntities();/*新表：重建*/
	return ok;
}
int Chunk::erase_BlockEntity(int x,int y,int z){/*删掉游戏眼里落在这一格的所有方块实体；给删了几条*/
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return 0;
	}
	nbt_Any *ba=(nbt_Any*)get_BodyMember(&con,&Chunk_Layout::block_entities);
	if(!check_BlockEntities(ba)){
		return 0;
	}
	nbt_List *bes=(nbt_List*)ba;
	uint64_t key=get_BlockEntityKey(x,y,z);
	if(!be_by_pos.count(key)){/*索引里没有＝游戏眼里这一格一条都没有*/
		return 0;
	}
	int del=0;
	for(size_t i=bes->con.size();i>0;i--){/*从后往前：erase不会挪动更小的下标；游戏落到这一格的全删（含坐标在别的区块、会被折回来的）*/
		nbt_Any *e=bes->con[i-1];
		if(e&&e->type==NBT_COMPOUND&&get_BlockEntityKeyOf((const nbt_Compound*)e)==key){
			bes->erase(i-1);
			del++;
		}
	}
	if(del){
		index_BlockEntities();/*删了：后面的下标都挪了，重建*/
	}
	return del;
}
nbt_Compound *Chunk::get_BlockEntity(int x,int y,int z)const{/*这一格的方块实体（指在树里，同一格几条给最后那条）；没有、区块不完整给 NULL*/
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return NULL;
	}
	return find_BlockEntityRead(this,x,y,z);
}

/*==================== 计划刻（block_ticks / fluid_ticks） ====================
  表里的元素形状和坐标含义见 nbtrw.hpp 的 Chunk_Tick 注释。这里只认游戏读得进来的条目（check_ChunkTickRecord）：
  有一条读不进来就当整张表形状不对（返回-1），调用者一个字都不改（fixer 先按最小单位删掉坏条目）。*/
static const char *get_TickName(const Chunk_Layout &L,bool fluid){return fluid?L.fluid_ticks:L.block_ticks;}/*TileTicks/LiquidTicks（老）、block_ticks/fluid_ticks*/
static const nbt_Any *get_TickList(const nbt_Compound *chunk,bool fluid){/*这个区块的方块刻/流体刻表成员（任何类型；没有、老格式缺 Level 给 NULL）*/
	const nbt_Compound *body=get_ChunkBody(chunk);
	return body?body->get(get_TickName(get_ChunkLayout(chunk),fluid)):NULL;
}
static nbt_Any *get_TickList(nbt_Compound *chunk,bool fluid){/*同上，可写的版本*/
	return (nbt_Any*)get_TickList((const nbt_Compound*)chunk,fluid);
}
static bool read_Tick(const nbt_Any *e,Chunk_Tick *t){/*一条刻照游戏的 SavedTick.codec：i 是写法合法的名字，x/y/z/t/p 是数值*/
	if(!e||e->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *c=(const nbt_Compound*)e;
	const nbt_Any *i=c->get("i");
	std::optional<int32_t> x=get_NumberInt(c->get("x")),y=get_NumberInt(c->get("y")),z=get_NumberInt(c->get("z"));
	std::optional<int32_t> d=get_NumberInt(c->get("t")),p=get_NumberInt(c->get("p"));
	if(!i||i->type!=NBT_STRING||!check_Identifier(((const nbt_String*)i)->con)||!x||!y||!z||!d||!p){
		return false;
	}
	t->type=((const nbt_String*)i)->con;
	t->x=*x;
	t->y=*y;
	t->z=*z;
	t->delay=*d;
	t->priority=*p;
	return true;
}
bool check_ChunkTickRecord(const nbt_Any *e){/*这一条刻游戏读得进来吗（read_Tick 读得出来就是）*/
	Chunk_Tick t;
	return read_Tick(e,&t);
}
static bool check_TickShape(const nbt_Any *f,int *count){/*true=表形状对（不存在也算对，count=0）；false=形状不对*/
	if(!f){
		*count=0;
		return true;
	}
	if(f->type!=NBT_LIST){
		return false;
	}
	const nbt_List *l=(const nbt_List*)f;
	if(!l->con.empty()&&l->elem!=NBT_COMPOUND){
		return false;/*表里是compound记录，元素类型被定死成别的就写不进去*/
	}
	if(l->con.empty()&&l->elem!=NBT_END&&l->elem!=NBT_COMPOUND){
		return false;/*空表但类型固定成别的：add_ChunkTick会失败*/
	}
	for(size_t i=0;i<l->con.size();i++){
		Chunk_Tick t;
		if(!read_Tick(l->con[i],&t)){
			return false;
		}
	}
	*count=(int)l->con.size();
	return true;
}
int count_ChunkTicks(const nbt_Compound *chunk,bool fluid){/*表里有几条；表不在给 0，表的形状不对或者有一条游戏读不进来给 -1*/
	int n=0;
	if(!chunk){
		return 0;
	}
	return check_TickShape(get_TickList(chunk,fluid),&n)?n:-1;
}
bool get_ChunkTick(const nbt_Compound *chunk,bool fluid,int i,Chunk_Tick *out){/*取第 i 条；下标越界、这一条读不进来给 false*/
	if(!chunk||!out){
		return false;
	}
	const nbt_Any *f=get_TickList(chunk,fluid);
	if(!f||f->type!=NBT_LIST){
		return false;
	}
	const nbt_List *l=(const nbt_List*)f;
	if(i<0||(size_t)i>=l->con.size()){
		return false;
	}
	return read_Tick(l->con[i],out);
}
int del_ChunkTick(nbt_Compound *chunk,bool fluid,int32_t x,int32_t y,int32_t z){/*删掉指着这一格的刻（读不进来的条目不动）；给删了几条，表不是 List 给 -1*/
	if(!chunk){
		return 0;
	}
	nbt_Any *f=get_TickList(chunk,fluid);
	if(!f){
		return 0;
	}
	if(f->type!=NBT_LIST){
		return -1;/*表本身不是List：区块级坏数据，不碰*/
	}
	nbt_List *l=(nbt_List*)f;
	int del=0;
	for(int i=(int)l->con.size()-1;i>=0;i--){/*从后往前：erase(i)不会挪动更小的下标*/
		Chunk_Tick t;
		if(read_Tick(l->con[i],&t)&&t.x==x&&t.y==y&&t.z==z){
			l->erase((size_t)i);
			del++;
		}
	}
	return del;
}
bool add_ChunkTick(nbt_Compound *chunk,bool fluid,const char *type,int32_t x,int32_t y,int32_t z,
	int32_t delay,int32_t priority){
	if(!chunk||!type||!type[0]){
		return false;
	}
	nbt_Any *f=get_TickList(chunk,fluid);
	if(f&&f->type!=NBT_LIST){
		return false;/*表本身不是List：区块级坏数据，不往里塞*/
	}
	if(!f){
		nbt_Compound *body=get_ChunkBody(chunk);
		if(!body){
			return false;/*老格式没有 Level：没地方放刻表*/
		}
		f=new nbt_List(NBT_COMPOUND);
		body->set(get_TickName(get_ChunkLayout(chunk),fluid),f);
	}
	nbt_List *l=(nbt_List*)f;
	/*表里可能有单条缺字段的坏记录：先删掉这些条目，再正常去重/追加。
	  这是“最小单位删”的一部分；cleanup_Ticks 后面也会做同样的清理。*/
	for(size_t i=l->con.size();i>0;i--){
		Chunk_Tick t;
		if(!read_Tick(l->con[i-1],&t)){
			l->erase(i-1);
		}
	}
	if(l->con.empty()){
		l->elem=NBT_END;/*坏记录删光了：让下一条push_back重新定类型*/
	}else if(l->elem!=NBT_COMPOUND){
		l->elem=NBT_COMPOUND;/*剩下的都是compound；元素类型字段坏了就改回来*/
	}
	for(size_t i=0;i<l->con.size();i++){/*同位置同类型已经有了就不重复加（原版也按(type,pos)去重）*/
		Chunk_Tick t;
		if(read_Tick(l->con[i],&t)&&t.x==x&&t.y==y&&t.z==z&&t.type==std::string_view(type)){
			return false;
		}
	}
	nbt_Compound *e=new nbt_Compound;
	e->set_String("i",type);
	e->set_Int("x",x);
	e->set_Int("y",y);
	e->set_Int("z",z);
	e->set_Int("t",delay);
	e->set_Int("p",priority);
	if(!((nbt_List*)f)->push_back(e)){/*类型不对（正常不会）*/
		return false;
	}
	return true;
}
int filter_ChunkTick(nbt_Compound *chunk,bool fluid,Chunk_Tick_Keep_Fn keep,void *ud){/*逐条问 keep，返回 false 的删掉；表形状不对给 -1 且一条不动*/
	if(!chunk){
		return 0;
	}
	nbt_Any *f=get_TickList(chunk,fluid);
	int n=0;
	if(!check_TickShape(f,&n)){
		return -1;
	}
	if(!f){
		return 0;
	}
	nbt_List *l=(nbt_List*)f;
	std::vector<nbt_Any*> out;
	out.reserve(l->con.size());
	int del=0;
	for(size_t i=0;i<l->con.size();i++){
		Chunk_Tick t;
		if(!keep||!read_Tick(l->con[i],&t)||keep(ud,&t)){
			out.push_back(l->con[i]);/*留着（形状不对的也留着：没读懂就不替人作主）*/
		}else{
			l->con[i]->free();
			del++;
		}
	}
	l->con.swap(out);
	return del;
}

/*---- 游戏怎么读一段的调色板容器（26.1.1；2832 以前先过 DataFixers）。test/mcdump.sh upgrade 拿游戏逐格对拍过 ----*/
static uint32_t get_CeilLog2(uint64_t n){return n<=1?0:(uint32_t)std::bit_width(n-1);}/*ceil(log2(n))，0 和 1 给 0（Mth.ceillog2）*/
/*ChunkHeightAndBiomeFix.ceillog2：Math.ceil(Math.log(n)/Math.log(2))，0 给 0。浮点误差只在 2^29 上算成 30
  （2^0~2^30 逐个问过游戏，test/work/mc/<版本>/upgrade/ceillog2.txt）*/
static uint32_t get_FixCeilLog2(uint32_t n){
	return n==((uint32_t)1<<29)?30:get_CeilLog2(n);
}
/*BitStorageAlignFix.addPadding（4096 格）：紧排跨 long 的下标转成不跨 long，长度按位宽正好（短了补0、长了扔掉；空的原样）*/
static std::vector<int64_t> pad_SpanData(const std::vector<int64_t> &in,uint32_t bits){
	if(in.empty()){
		return in;
	}
	std::vector<int64_t> out(get_PackedWords(4096,bits,false),0);
	uint64_t mask=((uint64_t)1<<bits)-1;
	for(uint32_t i=0;i<4096;i++){
		uint64_t bit=(uint64_t)i*bits;
		size_t k=(size_t)(bit>>6);
		uint32_t off=(uint32_t)(bit&63);
		uint64_t v=k<in.size()?(uint64_t)in[k]>>off:0;
		if(off+bits>64&&k+1<in.size()){
			v|=(uint64_t)in[k+1]<<(64-off);
		}
		set_PackedIndex(out,i,bits,false,(uint32_t)(v&mask));
	}
	return out;
}
/*26.1.1 的 PalettedContainer 编解码（codecRW/codecRO + unpack）：调色板 n 条（是集合就行，元素认不出来游戏换默认值）；
  data 是 getLongStream 读出来的不跨 long 的下标（NULL＝没有/读不了：lenientOptionalFieldOf 当没有）。
  读不了给 -1，不然给位宽（0＝单值，不看 data）。scan 时查下标越界（建段数方块时 MissingPaletteEntryException）*/
static int read_GameContainer(size_t n,const std::vector<int64_t> *data,uint32_t entries,bool blocks,bool scan){
	if(!n||n>(size_t)INT32_MAX){
		return -1;/*空调色板：单值调色板没有值，一数方块就抛*/
	}
	uint32_t bits=get_CeilLog2(n);
	if(blocks&&bits&&bits<4){
		bits=4;/*方块：1~4位都按4位线性调色板存*/
	}
	if(!bits){
		return 0;
	}
	if(!data||data->size()!=get_PackedWords(entries,bits,false)){
		return -1;/*Missing values for non-zero storage / Invalid length given for storage*/
	}
	if(scan){
		uint32_t per=64/bits,i=0;
		uint64_t mask=((uint64_t)1<<bits)-1;
		for(size_t k=0;i<entries&&k<data->size();k++){
			uint64_t v=(uint64_t)(*data)[k];
			for(uint32_t j=0;j<per&&i<entries;j++,i++){
				if((v&mask)>=n){
					return -1;
				}
				v>>=bits;
			}
		}
	}
	return (int)bits;
}
/*data 成员按 getLongStream 读：Long_Array 直接用（不拷），别的集合转到 tmp；读不了给 NULL*/
static const std::vector<int64_t> *get_DataLongs(const nbt_Any *d,std::vector<int64_t> &tmp){
	if(d&&d->type==NBT_LONG_ARRAY){
		return &((const nbt_Long_Array*)d)->con;
	}
	return get_LongStream(d,&tmp)?&tmp:NULL;
}
bool check_PaletteContainer(const nbt_Compound *c,uint32_t entries,bool blocks){/*一个调色板容器（block_states 或 biomes）游戏读得进来吗；下标越界也查*/
	std::optional<size_t> n=get_CollectionSize(c?c->get("palette"):NULL);
	if(!n){
		return false;/*palette 是必需字段，listOf 只认集合：没有/不是集合就没有结果，getOrThrow 整块失败*/
	}
	std::vector<int64_t> tmp;
	return read_GameContainer(*n,get_DataLongs(c->get("data"),tmp),entries,blocks,true)>=0;
}
static States_View get_NewStatesView(const nbt_Compound *sec,bool scan){/*2832 起的段：block_states{palette,data} 直接按 26.1.1 的读法判*/
	States_View v;
	const nbt_Any *bs=sec->get("block_states");
	if(!bs||bs->type!=NBT_COMPOUND){
		v.canonical=!bs;/*getCompound 读不到：游戏当全空气；不是 compound 的库当读不出来*/
		return v;
	}
	const nbt_Compound *c=(const nbt_Compound*)bs;
	const nbt_Any *p=c->get("palette"),*d=c->get("data");
	std::optional<size_t> n=get_CollectionSize(p);
	if(!n||*n>(size_t)INT32_MAX){
		v.kind=STATES_BAD;
		return v;
	}
	std::vector<int64_t> tmp;
	int bits=read_GameContainer(*n,get_DataLongs(d,tmp),4096,true,scan);
	if(bits<0){
		v.kind=STATES_BAD;
		return v;
	}
	v.kind=STATES_OK;
	v.n=v.n_game=(uint32_t)*n;
	v.bits=(uint32_t)bits;
	v.canonical=p->type==NBT_LIST&&(!bits||d->type==NBT_LONG_ARRAY);
	return v;
}
static States_View get_OldStatesView(const nbt_Compound *sec,const Chunk_Layout &L,bool scan){/*2832 以前的段：照游戏先过两道升级（BitStorageAlignFix、ChunkHeightAndBiomeFix）再按 26.1.1 读的样子判*/
	States_View v;
	const nbt_Any *p=sec->get("Palette"),*d=sec->get("BlockStates");
	if(!p||!d){/*ChunkHeightAndBiomeFix：两样都在才要，少一样换成空气容器*/
		v.canonical=(!p||p->type==NBT_LIST)&&(!d||d->type==NBT_LONG_ARRAY);
		return v;
	}
	std::optional<size_t> pn=get_CollectionSize(p);
	size_t n=pn.value_or(0);/*不是集合：asStream 读不出来，当0条*/
	if(n>(size_t)INT32_MAX){
		v.kind=STATES_BAD;
		return v;
	}
	v.n=(uint32_t)n;
	bool list=p->type==NBT_LIST,longs=d->type==NBT_LONG_ARRAY;
	if(n==1){/*makeOptimizedPalettedContainer：单条只留 palette，data 扔掉*/
		v.kind=STATES_OK;
		v.n_game=1;
		v.canonical=list&&longs;
		return v;
	}
	std::vector<int64_t> tmp,padded;
	const std::vector<int64_t> *data=get_DataLongs(d,tmp);
	uint32_t b0=std::max<uint32_t>(4,get_CeilLog2(n));/*BitStorageAlignFix：Math.max(4, ceillog2(palette.size()))*/
	bool native_span=false;/*紧排、长度正好：转出来的长度不用真算，下标也直接按紧排读*/
	size_t len=data?data->size():0;
	if(L.span&&(b0&(b0-1))){/*2527 以前、位宽不是2的幂才转（2的幂两种排法一样）；读不了的 data 转成空 Long_Array*/
		if(longs&&data->size()==get_PackedWords(4096,b0,true)){
			native_span=true;
			len=get_PackedWords(4096,b0,false);
		}else{
			padded=pad_SpanData(data?*data:std::vector<int64_t>(),b0);
			data=&padded;
			len=padded.size();
		}
	}
	/*ChunkHeightAndBiomeFix.padPaletteEntries：按 data 长度估位宽，比调色板要的大就补空气到 2^(位宽-1)+1 条（Java int）*/
	uint64_t est=(uint64_t)len*64/4096;
	uint32_t n2=(uint32_t)n;
	if(est>get_FixCeilLog2((uint32_t)n)){
		int32_t m=(int32_t)(((uint32_t)1<<((uint32_t)(est-1)&31))+1u);
		if(m>(int32_t)n){
			n2=(uint32_t)m;
		}
	}
	if(!pn&&n2==n){
		v.kind=STATES_BAD;/*Palette 不是集合、也没补成列表：26.1.1 读 palette 读不了*/
		return v;
	}
	int bits;
	if(native_span&&std::max<uint32_t>(4,get_CeilLog2(n2))==b0){
		bits=(int)b0;/*转出来的正好是 b0 位不跨 long 的长度，游戏也按 b0 位读*/
	}else{
		if(native_span){/*补了空气、位宽也变了（比如调色板 1025~2048 条）：游戏按新位宽读转出来的数据*/
			padded=pad_SpanData(*data,b0);
			data=&padded;
			native_span=false;
		}
		bits=read_GameContainer(n2,data,4096,true,false);
	}
	if(bits<0){
		v.kind=STATES_BAD;
		return v;
	}
	if(scan&&bits){
		for(uint32_t i=0;i<4096;i++){
			if(get_PackedIndex(*data,i,(uint32_t)bits,native_span)>=n2){
				v.kind=STATES_BAD;/*建段数方块时 MissingPaletteEntryException*/
				return v;
			}
		}
	}
	v.kind=STATES_OK;
	v.n_game=n2;
	v.bits=(uint32_t)bits;
	v.canonical=list&&longs&&n>=1&&(uint32_t)bits==b0&&
		((const nbt_Long_Array*)d)->con.size()==get_PackedWords(4096,b0,L.span);
	return v;
}
States_View get_StatesView(const nbt_Compound *sec,const Chunk_Layout &L,bool scan){/*按区块的格式分到上面两个；sec 为 NULL 当没有方块状态（空气）*/
	if(!sec){
		return States_View();
	}
	return L.states?get_NewStatesView(sec,scan):get_OldStatesView(sec,L,scan);
}
bool check_SectionStates(const nbt_Compound *sec,const Chunk_Layout &L){/*这一段的方块状态游戏读得进来吗（下标越界也查）*/
	return get_StatesView(sec,L,true).kind!=STATES_BAD;
}
/*check_Chunk：NBT成员类型和必需结构是不是完整。merge看到false就跳过，fixer用check_ChunkBase。
  只认形状，不认“这格方块合不合理”：缺section/纯光段/缺生成状态都算正常数据。
  调色板条目按区块DataVersion的格式认（新格式可以是字符串列表、条目可以没有属性）。
  Y 不查：游戏照读（截断/缺了当0），format_Chunk 才把它改成合法的。
  strict=true（merge 安全跳过的门槛）：sections/block_states/block_entities/刻表形状都对、每条计划刻游戏读得进来、
              调色板条目都认得出来；
  strict=false（fixer）：只查游戏会让整块载入失败、fixer 又没按最小单位修掉的——段的 block_states/biomes 坏到
              PalettedContainer 解不出来。sections/block_states/block_entities 类型不对游戏静默当没有，不算。*/
static bool check_ChunkInternal(const nbt_Compound *c,bool strict){
	Chunk_Layout L=get_ChunkLayout(c);
	State_Format fmt=L.fmt;
	bool biome64=!is_BiomePerBlock(L.dv);
	const nbt_Compound *body=get_ChunkBody(c);
	if(!body){
		return !strict;/*老格式没有 Level：一个字段都读不到（游戏也读不出东西），merge 跳过*/
	}
	const nbt_Any *sa=body->get(L.sections);
	if(sa&&sa->type!=NBT_LIST){
		if(strict){
			return false;
		}
		sa=NULL;/*游戏 getListOrEmpty：当没有段*/
	}
	const nbt_List *secs=sa?(const nbt_List*)sa:NULL;
	for(size_t i=0;secs&&i<secs->con.size();i++){
		nbt_Any *a=secs->con[i];
		if(!a||a->type!=NBT_COMPOUND){
			if(strict){
				return false;
			}
			continue;/*游戏跳过不是compound的元素*/
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		States_View sv=get_StatesView(sec,L,true);
		if(sv.kind==STATES_BAD){
			return false;
		}
		const nbt_Compound *bs=get_SectionStates(sec,L);
		if(strict){
			if(!sv.canonical){
				return false;/*游戏读得了、但不是库能原样读写的样子：merge 跳过*/
			}
			if(L.states){
				const nbt_Any *b=sec->get("block_states");
				if(b&&b->type!=NBT_COMPOUND){
					return false;
				}
			}else{
				const nbt_Any *p=sec->get("Palette"),*d=sec->get("BlockStates");
				if((p&&p->type!=NBT_LIST)||(d&&d->type!=NBT_LONG_ARRAY)){
					return false;
				}
			}
			const nbt_Any *pa=bs?bs->get(L.palette):NULL;
			if(pa&&pa->type==NBT_LIST){
				const nbt_List *palette=(const nbt_List*)pa;
				for(size_t k=0;k<palette->con.size();k++){
					if(!get_StateName(palette->con[k],fmt)[0]){
						return false;/*调色板条目在这种格式下认不出名字（游戏换成空气）：merge/rules 不拿它当方块*/
					}
				}
			}
		}
		const nbt_Any *bio=L.states?sec->get("biomes"):NULL;
		if(biome64&&bio&&bio->type==NBT_COMPOUND&&!check_PaletteContainer((const nbt_Compound*)bio,64,false)){
			return false;/*biomes 坏到游戏整块载入失败（26.4 起每格一个，源码还没有，先不查；2832 以前生物群系在 Level.Biomes）*/
		}
	}
	if(!strict){
		return true;
	}
	const nbt_Any *ba=body->get(L.block_entities);
	if(ba){
		if(ba->type!=NBT_LIST){
			return false;
		}
		const nbt_List *bes=(const nbt_List*)ba;
		if(!bes->con.empty()&&bes->elem!=NBT_COMPOUND){
			return false;
		}
		if(bes->con.empty()&&bes->elem!=NBT_END&&bes->elem!=NBT_COMPOUND){
			return false;/*空表但类型被定死成别的：追加方块实体会失败*/
		}
	}
	for(bool fluid:{false,true}){
		int n=0;
		if(!check_TickShape(c?get_TickList(c,fluid):NULL,&n)){
			return false;
		}
	}
	return true;
}
bool check_Chunk(const nbt_Compound *c){/*严格检查：merge 用（见上面 strict=true）*/
	return check_ChunkInternal(c,true);
}
bool check_ChunkBase(const nbt_Compound *c){/*只查整块载入失败的：fixer 用（见上面 strict=false）*/
	return check_ChunkInternal(c,false);
}

static bool is_AirName(const char *name){/*空气：air/cave_air/void_air都没有方块*/
	return name&&(!strcmp(name,"minecraft:air")||!strcmp(name,"minecraft:cave_air")||!strcmp(name,"minecraft:void_air"));
}

/*==================== POI 区块：poi/ 下的方块索引（写法和读法见 nbtrw.hpp） ====================*/
int32_t get_POIDataVersion(const nbt_Compound *poi){/*getIntOr 的读法（任何数值都认）；没有当最新（游戏是 1945，见 nbtrw.hpp）*/
	return get_TagInt(poi?poi->get("DataVersion"):NULL).value_or(0);
}
static bool is_POIOld(const nbt_Compound *poi){/*1946 以前的写法：根上 "0".."15"*/
	int32_t dv=get_POIDataVersion(poi);
	return dv>0&&dv<POI_SECTIONS;
}
nbt_Compound make_POIChunk(int32_t dv){/*空 POI 区块：dv 定写法（老写法根上什么都不放）*/
	nbt_Compound c;
	if(dv>0){
		c.set_Int("DataVersion",dv);
	}
	if(dv<=0||dv>=POI_SECTIONS){
		c.set("Sections",new nbt_Compound);
	}
	return c;
}
static nbt_Compound *get_POISections(const nbt_Compound *poi){/*新写法根上的 Sections（段号字符串 → 段）；没有/不是 compound 给 NULL*/
	const nbt_Any *s=poi?poi->get("Sections"):NULL;
	return s&&s->type==NBT_COMPOUND?(nbt_Compound*)s:NULL;
}
static bool check_POIKey(const std::string &key){/*段键＝段Y的十进制字符串*/
	if(key.empty()||key.size()>11){/*int32的十进制最长11个字符*/
		return false;
	}
	size_t i=(key[0]=='-')?1:0;
	if(i>=key.size()){
		return false;
	}
	for(;i<key.size();i++){
		if(key[i]<'0'||key[i]>'9'){
			return false;
		}
	}
	return true;
}
bool check_POI(const nbt_Compound *poi){/*形状不对＝库当这一块读不出来，调用者整块跳过*/
	if(!poi||poi->type!=NBT_COMPOUND){
		return false;
	}
	if(is_POIOld(poi)){
		for(int i=0;i<16;i++){
			const nbt_Any *r=poi->get(std::to_string(i));
			if(r&&r->type!=NBT_LIST){
				return false;
			}
		}
		return true;
	}
	const nbt_Any *sa=poi->get("Sections");
	if(sa&&sa->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *secs=sa?(const nbt_Compound*)sa:NULL;
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		if(!check_POIKey(it->first)){
			return false;
		}
		const nbt_Any *s=it->second;
		if(!s||s->type!=NBT_COMPOUND){
			return false;
		}
		const nbt_Any *r=((const nbt_Compound*)s)->get("Records");
		if(r&&r->type!=NBT_LIST){
			return false;
		}
	}
	return true;
}
static nbt_List *find_POIList(const nbt_Compound *poi,int32_t sy){/*游戏读得出来的这一段的记录表；没有/读不出来给 NULL*/
	const nbt_Any *r=NULL;
	if(is_POIOld(poi)){
		r=sy>=0&&sy<=15?poi->get(std::to_string(sy)):NULL;
	}else{
		const nbt_Compound *secs=get_POISections(poi);
		const nbt_Any *sec=secs?secs->get(std::to_string(sy)):NULL;
		r=sec&&sec->type==NBT_COMPOUND?((const nbt_Compound*)sec)->get("Records"):NULL;
	}
	return r&&r->type==NBT_LIST?(nbt_List*)r:NULL;
}
static std::vector<nbt_List*> list_POILists(const nbt_Compound *poi){/*所有读得出来的记录表（老写法按 "0".."15"，新写法按 Sections 的顺序）*/
	std::vector<nbt_List*> out;
	if(!poi||poi->type!=NBT_COMPOUND){
		return out;
	}
	if(is_POIOld(poi)){
		for(int i=0;i<16;i++){
			if(nbt_List *l=find_POIList(poi,i)){
				out.push_back(l);
			}
		}
		return out;
	}
	const nbt_Compound *secs=get_POISections(poi);
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_Any *sec=it->second;
		const nbt_Any *r=sec&&sec->type==NBT_COMPOUND?((const nbt_Compound*)sec)->get("Records"):NULL;
		if(r&&r->type==NBT_LIST){
			out.push_back((nbt_List*)r);
		}
	}
	return out;
}
POI get_POI(const nbt_Compound *poi,int32_t x,int32_t y,int32_t z){/*只看 y 所在那段的记录表，照 PoiSection.add 取一条*/
	POI got;
	const nbt_List *l=poi&&poi->type==NBT_COMPOUND?find_POIList(poi,get_SectionY(y)):NULL;
	for(size_t i=0;l&&i<l->con.size();i++){
		POI p;
		if(get_POIRecord(l->con[i],&p)&&p.x==x&&p.y==y&&p.z==z&&(!got.valid()||p.type!=got.type)){
			got=std::move(p);/*PoiSection.add：同类型留先来的，不同类型后来的顶掉*/
		}
	}
	return got;
}
int erase_POI(nbt_Compound *poi,int32_t x,int32_t y,int32_t z){/*所有读得出来的记录表里按位置删（位置读不出来的留着）*/
	int del=0;
	std::vector<nbt_List*> lists=list_POILists(poi);
	for(nbt_List *l:lists){
		for(size_t i=0;i<l->con.size();){
			int32_t px=0,py=0,pz=0;
			if(get_POIRecordPos(l->con[i],&px,&py,&pz)&&px==x&&py==y&&pz==z){
				l->erase(i);
				del++;
				continue;
			}
			i++;
		}
	}
	return del;
}
static nbt_List *make_POIList(nbt_Compound *parent,const std::string &key){/*parent 里的记录表：没有、不是 List、装的不是 compound 的
                                                                          （游戏一条也读不出来）都换成新的空表*/
	nbt_Any *r=parent->get(key);
	if(r&&r->type==NBT_LIST&&(((nbt_List*)r)->con.empty()||((nbt_List*)r)->elem==NBT_COMPOUND)){
		((nbt_List*)r)->elem=NBT_COMPOUND;
		return (nbt_List*)r;
	}
	nbt_List *l=new nbt_List(NBT_COMPOUND);
	parent->set(key,l);
	return l;
}
bool set_POI(nbt_Compound *poi,int32_t x,int32_t y,int32_t z,const POI &p){/*先查完会失败的条件、再删旧的写新的；写法跟着 POI 区块的 DataVersion*/
	if(!poi||poi->type!=NBT_COMPOUND||!p.valid()||!check_Identifier(p.type.con)){
		return false;
	}
	int32_t sy=get_SectionY(y);
	bool old=is_POIOld(poi);
	if(old&&(sy<0||sy>15)){
		return false;
	}
	erase_POI(poi,x,y,z);
	std::string key=std::to_string(sy);
	nbt_List *l=NULL;
	if(old){
		l=make_POIList(poi,key);
	}else{
		nbt_Compound *secs=get_POISections(poi);
		if(!secs){/*没有/不是 compound：游戏当没有*/
			secs=new nbt_Compound;
			poi->set("Sections",secs);
		}
		nbt_Any *s=secs->get(key);
		if(!s||s->type!=NBT_COMPOUND){/*段解不出来：游戏整段不要*/
			s=new nbt_Compound;
			secs->set(key,s);
		}
		((nbt_Compound*)s)->set("Valid",new nbt_Byte(1));
		l=make_POIList((nbt_Compound*)s,"Records");
	}
	return l->push_back(make_POIRecord(p,x,y,z));/*只会在内存不够时失败*/
}
int list_POI(const nbt_Compound *poi,std::vector<POI> *out){/*按表的顺序列出来；读不进来的那条只计数*/
	if(!out||!check_POI(poi)){
		return -1;
	}
	int bad=0;
	std::vector<nbt_List*> lists=list_POILists(poi);
	for(const nbt_List *l:lists){
		for(size_t i=0;i<l->con.size();i++){
			POI p;
			if(!get_POIRecord(l->con[i],&p)){
				bad++;
				continue;
			}
			out->push_back(std::move(p));
		}
	}
	return bad;
}
int count_POI(const nbt_Compound *poi){/*含读不进来的记录*/
	int n=0;
	std::vector<nbt_List*> lists=list_POILists(poi);
	for(const nbt_List *l:lists){
		n+=(int)l->con.size();
	}
	return n;
}
int clean_POI(nbt_Compound *poi){/*两种写法：空表和读不出来的段都删*/
	if(!poi||poi->type!=NBT_COMPOUND){
		return 0;
	}
	std::vector<std::string> drop;
	nbt_Compound *parent=NULL;
	if(is_POIOld(poi)){
		parent=poi;
		for(int i=0;i<16;i++){
			std::string k=std::to_string(i);
			const nbt_Any *r=poi->get(k);
			if(r&&(r->type!=NBT_LIST||((const nbt_List*)r)->con.empty())){
				drop.push_back(k);
			}
		}
	}else if((parent=get_POISections(poi))){
		for(nbt_Compound::iterator it=parent->con.begin();it!=parent->con.end();++it){
			const nbt_Any *s=it->second;
			const nbt_Any *r=s&&s->type==NBT_COMPOUND?((const nbt_Compound*)s)->get("Records"):NULL;
			if(!r||r->type!=NBT_LIST||((const nbt_List*)r)->con.empty()){
				drop.push_back(it->first);
			}
		}
	}
	for(size_t i=0;i<drop.size();i++){
		parent->erase(drop[i]);
	}
	return (int)drop.size();
}

std::vector<nbt_List*> get_POILists(nbt_Compound *poi){/*给调用者按条改用（指在树里）*/
	return list_POILists(poi);
}
int repair_POI(nbt_Compound *poi){/*只删游戏整段不要的，别的不动*/
	if(!poi||poi->type!=NBT_COMPOUND){
		return 0;
	}
	std::vector<std::string> drop;
	nbt_Compound *parent=poi;
	if(is_POIOld(poi)){
		for(int i=0;i<16;i++){
			std::string k=std::to_string(i);
			const nbt_Any *r=poi->get(k);
			if(r&&r->type!=NBT_LIST){
				drop.push_back(k);
			}
		}
	}else{
		parent=get_POISections(poi);
		for(nbt_Compound::iterator it=parent?parent->con.begin():nbt_Compound::iterator();parent&&it!=parent->con.end();++it){
			const nbt_Any *s=it->second;
			const nbt_Any *r=s&&s->type==NBT_COMPOUND?((const nbt_Compound*)s)->get("Records"):NULL;
			if(!r||r->type!=NBT_LIST){
				drop.push_back(it->first);
			}
		}
	}
	for(size_t i=0;i<drop.size();i++){
		parent->erase(drop[i]);
	}
	return (int)drop.size();
}

bool has_Block(const nbt_Compound *c){/*这个区块里有没有任意一个非空气方块？*/
	Chunk_Layout L=get_ChunkLayout(c);
	State_Format fmt=L.fmt;
	const nbt_Compound *body=get_ChunkBody(c);
	const nbt_Any *sc=body?body->get(L.sections):NULL;
	if(!sc||sc->type!=NBT_LIST){
		return false;/*连sections都没有：一个方块都没有*/
	}
	const nbt_List *secs=(const nbt_List*)sc;
	for(size_t i=0;i<secs->con.size();i++){
		const nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Compound *bs=get_SectionStates((const nbt_Compound*)s,L);
		if(!bs){
			continue;/*没有block_states（只有光的段）＝空气*/
		}
		const nbt_Any *pany=bs->get(L.palette);
		if(!pany||pany->type!=NBT_LIST){
			continue;/*没有调色板：读不出来，当空气*/
		}
		const nbt_List *pal=(const nbt_List*)pany;
		size_t n=pal->con.size();
		if(!n){
			continue;
		}
		if(n>0xFFFFFFFFu){/*NBT list长度4字节：这种形状一定是生成过的，别判它空*/
			return true;
		}
		bool has_solid=false;
		for(size_t j=0;j<n;j++){
			if(!is_AirName(get_StateName(pal->con[j],fmt))){
				has_solid=true;break;
			}
		}
		if(!has_solid){
			continue;/*整段只有空气*/
		}
		const nbt_Any *dany=bs->get(L.data);
		const nbt_Long_Array *d=dany&&dany->type==NBT_LONG_ARRAY?(const nbt_Long_Array*)dany:NULL;
		uint32_t bits=get_PaletteBitsFor(n,L.states);
		if(!L.states&&!d){
			continue;/*2832 以前少了 BlockStates：游戏当这一段空气*/
		}
		if(!bits||!d){/*新格式没有data（或者单条调色板，游戏不看data）：整段都是palette[0]*/
			if(!is_AirName(get_StateName(pal->con[0],fmt))){
				return true;
			}
			continue;
		}
		if(d->con.size()!=get_PackedWords(4096,bits,L.span)){
			return true;/*data长度不对：形状不对，宁可信其有*/
		}
		for(uint32_t k=0;k<4096;k++){/*位置顺序是 y*256+z*16+x，这里只关心有没有非空气*/
			uint32_t idx=get_PackedIndex(d->con,k,bits,L.span);
			if(idx<n&&!is_AirName(get_StateName(pal->con[idx],fmt))){
				return true;
			}
		}
	}
	return false;
}
int32_t get_ChunkDataVersion(const nbt_Compound *chunk){/*游戏是 getIntOr：任何数值都认*/
	return get_TagInt(chunk?chunk->get("DataVersion"):NULL).value_or(0);
}
Chunk_Full_Status get_ChunkStatus(const nbt_Compound *c){/*这个区块生成完了吗*/
	return c?read_ChunkStatus(get_ChunkBody(c),get_ChunkLayout(c)):CHUNK_NO_STATUS;
}

/*---- 实体：实体区块的容器检查，和区块/实体区块两种实体表的读写 ----
  fixer：容器坏→整块实体删；单条记录坏（check_EntityRecord，atom.cpp）→只删那一条。
  merge：check_Entity 严格，任何一条坏就整块跳过（merge不清理）。*/
bool check_EntityContainer(const nbt_Compound *chunk,int32_t cx,int32_t cz){/*实体区块的容器对不对：Position 是 [cx,cz]、Entities 是 List；DataVersion 可以没有，有就得是 Int*/
	if(!chunk||chunk->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Any *pos=chunk->get("Position");
	if(!pos||pos->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)pos)->con.size()!=2){
		return false;
	}
	if(((const nbt_Int_Array*)pos)->con[0]!=cx||((const nbt_Int_Array*)pos)->con[1]!=cz){
		return false;
	}
	const nbt_Any *dv=chunk->get("DataVersion");
	if(dv&&dv->type!=NBT_INT){
		return false;/*没有 DataVersion 不算坏：游戏当它是 -1（中文 实体格式），照样读、照样升级*/
	}
	const nbt_Any *es=chunk->get("Entities");
	if(!es||es->type!=NBT_LIST){
		return false;
	}
	return true;
}
bool check_Entity(const nbt_Compound *chunk,int32_t cx,int32_t cz){/*容器对、每条实体也都读得进来（merge 用：有一条坏的整块跳过）*/
	if(!check_EntityContainer(chunk,cx,cz)){
		return false;
	}
	const nbt_List *list=(const nbt_List*)chunk->get("Entities");
	if(!list->con.empty()&&list->elem!=NBT_COMPOUND){
		return false;
	}
	for(size_t i=0;i<list->con.size();i++){
		if(!check_EntityRecord(list->con[i])){
			return false;
		}
	}
	return true;
}
nbt_Compound make_EntityChunk(int32_t cx,int32_t cz,int32_t dv){/*游戏 EntityStorage 写的样子*/
	nbt_Compound c;
	if(dv>0){
		c.set_Int("DataVersion",dv);
	}
	nbt_Int_Array *pos=new nbt_Int_Array;
	pos->push_back(cx);
	pos->push_back(cz);
	c.set("Position",pos);
	c.set("Entities",new nbt_List(NBT_COMPOUND));
	return c;
}
/*实体表在哪一层、叫什么：根上有 Position 的是实体区块（Entities，必须在），否则是区块（body 那层、按版本的键，可以没有）。
  认不出来给 NULL*/
static nbt_Compound *find_EntityParent(const nbt_Compound *chunk,const char **key,bool *need){
	if(!chunk||chunk->type!=NBT_COMPOUND||chunk->con.empty()){
		return NULL;
	}
	if(const nbt_Any *pos=chunk->get("Position")){
		if(pos->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)pos)->con.size()!=2){
			return NULL;
		}
		*key="Entities";
		*need=true;
		return (nbt_Compound*)chunk;
	}
	*key=get_ChunkLayout(chunk).entities;
	*need=false;
	return (nbt_Compound*)get_ChunkBody(chunk);
}
static bool check_EntityList(const nbt_Any *ea){/*实体表：List，非空时元素是 compound*/
	return ea&&ea->type==NBT_LIST&&(((const nbt_List*)ea)->con.empty()||((const nbt_List*)ea)->elem==NBT_COMPOUND);
}
int get_ChunkEntities(const nbt_Compound *chunk,std::vector<Entity> *out){/*深拷贝每条顶层实体；读不出来一条不给*/
	const char *key=NULL;
	bool need=false;
	const nbt_Compound *parent=find_EntityParent(chunk,&key,&need);
	if(!parent||!out){
		return -1;
	}
	const nbt_Any *ea=parent->get(key);
	if(!ea){
		return need?-1:0;
	}
	if(!check_EntityList(ea)){
		return -1;
	}
	const nbt_List *l=(const nbt_List*)ea;
	out->reserve(out->size()+l->con.size());
	for(size_t i=0;i<l->con.size();i++){
		out->emplace_back(*(const nbt_Compound*)l->con[i]);
	}
	return (int)l->con.size();
}
bool add_ChunkEntity(nbt_Compound *chunk,const Entity &e){/*先查完会失败的条件再写，失败一点不改*/
	const char *key=NULL;
	bool need=false;
	nbt_Compound *parent=find_EntityParent(chunk,&key,&need);
	if(!parent||!e.valid()){
		return false;
	}
	nbt_Any *ea=parent->get(key);
	if(ea&&!check_EntityList(ea)){
		return false;
	}
	if(!ea){
		ea=new nbt_List(NBT_COMPOUND);
		parent->set(key,ea);
	}
	((nbt_List*)ea)->elem=NBT_COMPOUND;/*空表的元素类型可能是别的：先定成 compound*/
	return ((nbt_List*)ea)->push_back(e.con.copy());/*只会在内存不够时失败*/
}
int erase_ChunkEntity(nbt_Compound *chunk,std::string_view uuid){/*按 get_EntityUUID 的字符串比，只看顶层*/
	const char *key=NULL;
	bool need=false;
	nbt_Compound *parent=find_EntityParent(chunk,&key,&need);
	nbt_Any *ea=parent?parent->get(key):NULL;
	if(!check_EntityList(ea)){
		return 0;
	}
	nbt_List *l=(nbt_List*)ea;
	int del=0;
	for(size_t i=0;i<l->con.size();){
		if(get_EntityUUID((const nbt_Compound*)l->con[i])==uuid){
			l->erase(i);
			del++;
			continue;
		}
		i++;
	}
	return del;
}
