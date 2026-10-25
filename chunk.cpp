#include "nbtrw.hpp"
#include <algorithm>
/*chunk.cpp：区块（世界坐标那套），和原始NBT操作分开
 *	section本身就是个compound，它的方块状态（调色板/子区块）读写也在这里：调色板和子区块属于区块级。
 *	Chunk_Cursor是把“sy→section→palette/data→方块实体”记下来的小备忘：连着按格子读
 *	一个区块时，不用每个方块都把sections列表扫一遍、也不重复哈希block_states/Name。
 *	库接口改树会换format_generation/be_generation，游标据此重查；直接改树后要reset。
 *	不完整（Status不是minecraft:full，或者根本没有Status）的区块：get_Block返回空id的Block、
 *	set_Block返回false——残块不是玩家的东西，不读也不往里写。
 *	缺某一段section＝空气（返回id=minecraft:air的有效Block）。
 *	POI（poi/目录里的方块索引）也在这里：它就是个普通compound，按段存记录。
 */

/*---- Chunk_Cursor：连着读同一个区块的游标 ----*/
bool Chunk_Cursor::follows(const Chunk *c)const{/*serial相同才算跟的是同一个区块，不看地址*/
	return c&&serial&&serial==c->serial;
}
void Chunk_Cursor::reset(){/*换区块、或者自己大改过树之后清一下最保险*/
	serial=0;
	secs_any=NULL;
	secs=NULL;
	secs_len=0;
	sy=INT32_MIN;
	sec=NULL;
	sec_map_ready=false;
	full_known=false;
	full=false;
	seen_generation=0;
	states_sec=NULL;
	states_members=0;
	states=NULL;
	states_known=false;
	states_bad=false;
	palette=NULL;
	palette_bits=0;
	palette_per=0;
	data=NULL;
	data_len=0;
	palette_name.clear();
	palette_props.clear();
	bes=NULL;
	bes_ok=false;
	bes_generation=0;
	be_index_ready=false;
	be_index_list=NULL;
	be_index_len=0;
	be_index_generation=0;
	be_index.clear();
	origin_ready=false;
	origin_valid=false;
	origin_x=0;
	origin_z=0;
}
bool Chunk_Cursor::is_Full(const Chunk *c){
	if(!c){
		return false;
	}
	if(!follows(c)){/*换区块：旧缓存全部作废，但Status可以马上重查*/
		reset();
		serial=c->serial;
	}
	if(!full_known){
		full_known=true;
		full=(get_ChunkStatus(c)==CHUNK_FULL);
	}
	return full;
}
bool Chunk_Cursor::get_Origin(const Chunk *c,int32_t *cx,int32_t *cz){
	if(!c){
		return false;
	}
	if(!follows(c)){
		reset();
		serial=c->serial;
	}
	if(!origin_ready){
		const nbt_Any *xp=c->get("xPos");
		const nbt_Any *zp=c->get("zPos");
		origin_valid=xp&&xp->type==NBT_INT&&zp&&zp->type==NBT_INT;
		if(origin_valid){
			origin_x=xp->as_Int();
			origin_z=zp->as_Int();
		}
		origin_ready=true;
	}
	if(origin_valid){
		*cx=origin_x;
		*cz=origin_z;
	}
	return origin_valid;
}
nbt_Any *Chunk_Cursor::get_Sections(const Chunk *c){
	if(!c){
		return NULL;
	}
	if(!follows(c)||seen_generation!=c->format_generation){/*换区块，或format_Chunk刚改过树*/
		reset();
		serial=c->serial;
		seen_generation=c->format_generation;
	}
	nbt_Any *now=c->get("sections");
	if(now!=secs_any){/*sections成员被整体换掉：旧指针可能已释放，先不碰它*/
		secs_any=now;
		secs=now&&now->type==NBT_LIST?(nbt_List*)now:NULL;
		secs_len=secs?secs->con.size():0;
		sy=INT32_MIN;
		sec=NULL;
		sec_map_ready=false;
		states_sec=NULL;
		states=NULL;
		states_known=false;
		states_bad=false;
		palette=NULL;
		palette_bits=0;
		palette_per=0;
		data=NULL;
		data_len=0;
		palette_name.clear();
		palette_props.clear();
		bes=NULL;
		bes_ok=false;
	}
	return secs_any;
}
nbt_Compound *Chunk_Cursor::find_Section(const Chunk *c,int32_t sy){
	if(!c){
		return NULL;
	}
	/*同一区块、结构版本没变、表已建好：直接查表。逐格扫时连续16格都在同一段，
	  这样不必每个方块都去哈希"sections"；format_Chunk/ensure_Section改过树会换版本号。*/
	if(follows(c)&&seen_generation==c->format_generation&&sec_map_ready&&
	   secs_len==(secs?secs->con.size():0)){
		this->sy=sy;
		sec=(sy>=-128&&sy<=127)?sec_by_y[sy+128]:NULL;
		return sec;
	}
	get_Sections(c);
	size_t len=secs?secs->con.size():0;
	if(!sec_map_ready||secs_len!=len){
		/*一次把Y→section建成表：merge/ fixer按Y递增扫时不再每段都线性找一遍。
		  同一Y有多段时保留列表里最前面的，和get_Section()的语义一致。*/
		memset(sec_by_y,0,sizeof(sec_by_y));
		for(size_t i=0;i<len;i++){
			nbt_Any *s=secs->con[i];
			if(!s||s->type!=NBT_COMPOUND){/*sections里的元素本该都是compound：不是就跳过*/
				continue;
			}
			const nbt_Any *y=((const nbt_Compound*)s)->get("Y");/*Y必须是带符号Byte；非Byte是坏数据，不猜*/
			if(!y||y->type!=NBT_BYTE){
				continue;
			}
			int32_t v=(int32_t)(int8_t)y->as_Int();
			if(v>=-128&&v<=127&&!sec_by_y[v+128]){
				sec_by_y[v+128]=(nbt_Compound*)s;
			}
		}
		secs_len=len;
		sec_map_ready=true;
	}
	this->sy=sy;
	sec=(sy>=-128&&sy<=127)?sec_by_y[sy+128]:NULL;
	return sec;
}
nbt_List *Chunk_Cursor::get_BlockEntities(const Chunk *c){/*chunk的block_entities列表，游标里记一次就够了*/
	if(!c){
		return NULL;
	}
	if(!follows(c)||seen_generation!=c->format_generation){
		get_Sections(c);/*换区块/format过：把游标整体重来*/
	}
	if(!bes_ok||bes_generation!=c->be_generation){/*set_Block/set_BlockEntity改过表才重查*/
		nbt_Any *s=c->get("block_entities");
		bes=s&&s->type==NBT_LIST?(nbt_List*)s:NULL;
		bes_ok=true;
		bes_generation=c->be_generation;
		be_index_ready=false;
	}
	return bes;
}

/*---- section里的方块状态：调色板+data ----*/
static uint32_t get_PaletteBits(size_t n){/*MC的位宽：至少4，往上取到能装下n个下标；n<=2^32*/
	uint32_t bits=4;
	while(bits<32&&((uint64_t)1<<bits)<n){
		bits++;
	}
	return bits;
}
nbt_Compound *Chunk_Cursor::get_States(nbt_Compound *section){/*section的block_states（连palette/data一起记住）
															   成员数变了说明树被外面动过，那就重查*/
	if(!section){
		return NULL;
	}
	if(states_sec==section&&states_known&&states_members==section->con.size()){
		return states;/*同一段、成员数没变：不再查block_states这个key*/
	}
	bool same_sec=states_sec==section;
	states_sec=section;
	states_members=section->con.size();
	nbt_Any *s=section->get("block_states");
	states_bad=s&&s->type!=NBT_COMPOUND;
	states=s&&s->type==NBT_COMPOUND?(nbt_Compound*)s:NULL;
	nbt_Any *p=states?states->get("palette"):NULL;
	palette=p&&p->type==NBT_LIST?(nbt_List*)p:NULL;
	palette_bits=palette?get_PaletteBits(palette->con.size()):0;
	palette_per=palette_bits?64/palette_bits:0;
	if(!same_sec){
		palette_name.clear();
		palette_props.clear();
	}
	data=NULL;/*换了section，data重新查*/
	data_len=0;
	states_known=true;
	return states;
}
static nbt_Any *blockDataAtCursor(Chunk_Cursor *cur,int lx,int ly,int lz,uint32_t *out_idx){
	nbt_List *palette=cur?cur->palette:NULL;
	if(!palette||palette->con.empty()||palette->con.size()>0xFFFFFFFFu){
		return NULL;
	}
	if(cur->data_len!=palette->con.size()){/*调色板长度变了就把data和位宽重查一遍*/
		nbt_Any *d=cur->states?cur->states->get("data"):NULL;
		cur->data=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
		cur->data_len=palette->con.size();
		cur->palette_bits=get_PaletteBits(palette->con.size());
		cur->palette_per=cur->palette_bits?64/cur->palette_bits:0;
	}
	nbt_Long_Array *data=cur->data;
	uint32_t bits=cur->palette_bits,per=cur->palette_per;
	uint32_t idx=0;
	if(data){
		uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
		if(!per||i/per>=data->con.size()){
			return NULL;
		}
		idx=(uint32_t)((uint64_t)data->con[i/per]>>((i%per)*bits))&((uint32_t)(((uint64_t)1<<bits)-1));
	}
	if(idx>=palette->con.size()){
		return NULL;
	}
	if(out_idx){
		*out_idx=idx;
	}
	return palette->con[idx];
}
static nbt_Any *get_BlockDataIdx(nbt_Compound *section,Chunk_Cursor *cur,int lx,int ly,int lz,uint32_t *out_idx){
	if(!section||!cur){
		return NULL;
	}
	if(lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15*/
		return NULL;
	}
	cur->get_States(section);
	return blockDataAtCursor(cur,lx,ly,lz,out_idx);
}
nbt_Any *get_BlockData(nbt_Compound *section,Chunk_Cursor *cur,int lx,int ly,int lz){/*带游标：调色板/data都在游标里*/
	return get_BlockDataIdx(section,cur,lx,ly,lz,NULL);
}
nbt_Any *get_BlockData(nbt_Compound *section,int lx,int ly,int lz){/*不带游标：临时游标走一遍，等于现查*/
	Chunk_Cursor cur;
	return get_BlockData(section,&cur,lx,ly,lz);
}
bool is_SectionConst(nbt_Compound *section,Chunk_Cursor *cur,nbt_Any **block){/*这一段是不是整段同一个方块？*/
	Chunk_Cursor tmp;
	if(!cur){
		cur=&tmp;
	}
	nbt_Compound *states=section?cur->get_States(section):NULL;
	if(!section||!states){/*没有这段section，或者这段没有block_states（纯光段）＝全空气*/
		*block=NULL;
		return true;
	}
	nbt_List *palette=cur->palette;
	if(!palette||palette->con.size()!=1){
		return false;/*读不出来（direct调色板），或者不止一种方块*/
	}
	if(states->get("data")){
		return false;/*调色板只有一条却还带data：下标可能是乱的，不当常量看*/
	}
	nbt_Any *b=palette->con[0];
	const char *name=get_Name(b);
	*block=(name&&!strcmp(name,"minecraft:air"))?NULL:b;/*空气一律算NULL*/
	return true;
}
static bool is_StateEqual(const nbt_Any *a,const nbt_Any *b){/*方块状态比较：先比Name（不一样就不用深比了），一样再比*/
	if(a==b){
		return true;
	}
	if(!a||!b||a->type!=NBT_COMPOUND||b->type!=NBT_COMPOUND){
		return a&&b&&(*a==*b);
	}
	const nbt_Any *na=((const nbt_Compound*)a)->get("Name");
	const nbt_Any *nb=((const nbt_Compound*)b)->get("Name");
	if(na&&nb&&na->type==NBT_STRING&&nb->type==NBT_STRING&&((const nbt_String*)na)->con!=((const nbt_String*)nb)->con){
		return false;
	}
	return *a==*b;
}
static bool set_BlockData_States(nbt_Compound *states,nbt_List *palette,nbt_Any *block,
	int lx,int ly,int lz,Chunk_Cursor *cur){/*把方块状态写进已经找好的block_states（set_Block事务用）*/
	if(!states||!block||block->type!=NBT_COMPOUND){
		return false;
	}
	if(lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15*/
		return false;
	}
	if(!palette||palette->con.empty()||palette->con.size()>0xFFFFFFFFu){
		return false;
	}
	nbt_Any *d=states?states->get("data"):NULL;
	nbt_Long_Array *data=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
	size_t old_len=palette->con.size();
	uint32_t bits_old=0;/*MC保存时位宽总按palette长度取最小*/
	if(data){
		bits_old=get_PaletteBits(old_len);
	}
	size_t idx=old_len;
	for(size_t i=0;i<old_len;i++){
		if(is_StateEqual(palette->con[i],block)){/*已有同样状态就复用，不重复加*/
			idx=i;
			break;
		}
	}
	if(idx==old_len){
		if(old_len>=0xFFFFFFFFu){/*NBT list长度4字节，放不下了*/
			return false;
		}
		if(!palette->push_back(block->copy())){
			return false;
		}
	}
	uint32_t bits=get_PaletteBits(palette->con.size());
	uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
	if(!data){
		if(idx==0){/*整段都指向palette[0]，不用建data*/
			return true;
		}
		data=new nbt_Long_Array;
		data->con.resize((4096+64/bits-1)/(64/bits),0);/*下标不跨long，长度向上取整*/
		states->set("data",data);
	}else if(bits!=bits_old){/*位宽变了要把整段下标重排*/
		uint32_t per_old=64/bits_old,per_new=64/bits;
		uint32_t need_old=(4096+per_old-1)/per_old;
		if(data->con.size()<need_old){/*文件里的data比该有的短：缺的当palette[0]*/
			data->con.resize(need_old,0);
		}
		std::vector<int64_t> buf((4096+per_new-1)/per_new,0);
		for(uint32_t k=0;k<4096;k++){
			uint64_t v=(uint64_t)(data->con[k/per_old]>>((k%per_old)*bits_old))&((uint64_t)(((uint64_t)1<<bits_old)-1));
			buf[k/per_new]|=(int64_t)(v<<((k%per_new)*bits));
		}
		data->con=std::move(buf);
	}
	uint32_t per=64/bits,shift=(i%per)*bits;
	uint32_t need=(4096+per-1)/per;
	if(data->con.size()<need){/*短了就补长再写*/
		data->con.resize(need,0);
	}
	uint64_t mask=((uint64_t)1<<bits)-1;
	data->con[i/per]=(int64_t)(((uint64_t)data->con[i/per]&~(mask<<shift))|((uint64_t)idx<<shift));
	if(cur){/*游标里记住的“这一段有没有data”可能刚被这次写入改掉：直接作废，下次重查*/
		cur->data=NULL;
		cur->data_len=0;
	}
	return true;
}
bool set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz,Chunk_Cursor *cur){/*把方块状态写进section*/
	if(!section||!block||block->type!=NBT_COMPOUND){
		return false;
	}
	nbt_Compound *states;
	nbt_List *palette;
	if(cur){
		states=cur->get_States(section);
		palette=cur->palette;
	}else{
		nbt_Any *s=section->get("block_states");
		states=s&&s->type==NBT_COMPOUND?(nbt_Compound*)s:NULL;
		nbt_Any *p=states?states->get("palette"):NULL;
		palette=p&&p->type==NBT_LIST?(nbt_List*)p:NULL;
	}
	return set_BlockData_States(states,palette,block,lx,ly,lz,cur);
}
bool set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz){
	return set_BlockData(section,block,lx,ly,lz,NULL);
}

/*---- Chunk：一个区块 ----*/
static uint64_t chunk_Serial_Next=0;/*全局只增不减；0留给“没有区块”，第一个Chunk是1*/
static uint64_t new_Serial(){
	return ++chunk_Serial_Next;
}
Chunk::Chunk():nbt_Compound(),serial(new_Serial()){}
Chunk::Chunk(int32_t cx,int32_t cz):Chunk(){
	set_Int("xPos",cx);
	set_Int("zPos",cz);
}
Chunk::Chunk(const nbt_Compound &c):nbt_Compound(c),serial(new_Serial()){}
Chunk::Chunk(nbt_Compound &&c):nbt_Compound(std::move(c)),serial(new_Serial()){}
Chunk::Chunk(const std::unordered_map<std::string,nbt_Any*> &m):nbt_Compound(m),serial(new_Serial()){}
Chunk::Chunk(std::unordered_map<std::string,nbt_Any*> &&m):nbt_Compound(std::move(m)),serial(new_Serial()){}
Chunk::Chunk(const Chunk &o):nbt_Compound(o),serial(new_Serial()){}
Chunk::Chunk(Chunk &&o)noexcept:nbt_Compound(std::move(o)),serial(new_Serial()){
	o.serial=0;
}
Chunk &Chunk::operator=(const Chunk &o){
	if(this!=&o){
		nbt_Compound::operator=(o);
		serial=new_Serial();
	}
	return *this;
}
Chunk &Chunk::operator=(Chunk &&o)noexcept{
	if(this!=&o){
		nbt_Compound::operator=(std::move(o));
		serial=new_Serial();
		o.serial=0;
	}
	return *this;
}
Chunk *Chunk::read(nbt_Raw *raw){/*一个区块就是一个根compound，直接建成Chunk；数据坏了返回NULL*/
	if(!raw||!raw->read_RootHeader()||raw->fail){/*根tag就不对/数据是空的*/
		return NULL;
	}
	Chunk *chunk=new Chunk;/*构造时已经拿好serial*/
	chunk->read_Members(raw);
	if(raw->fail){/*读到一半坏了：整块扔掉（半截区块比没有更危险）*/
		chunk->free();
		return NULL;
	}
	return chunk;
}
Chunk *Chunk::read(const char *path){
	nbt_Raw raw=nbt_Raw::load(path);
	if(raw.data.empty()){
		return NULL;
	}
	return read(&raw);
}
bool Chunk::is_Full()const{/*只有Status=="minecraft:full"才算完整；没有Status字段也算不完整（宁可不动）*/
	return get_ChunkStatus(this)==CHUNK_FULL;
}
nbt_Any *Chunk::get_Section(int32_t sy)const{/*按section的Y找人，Y是带符号的Byte*/
	const nbt_Any *any=get("sections");
	const nbt_List *secs=any&&any->type==NBT_LIST?(const nbt_List*)any:NULL;
	for(size_t i=0;secs&&i<secs->con.size();i++){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){/*元素本该都是compound：不是就跳过*/
			continue;
		}
		const nbt_Any *y=((const nbt_Compound*)s)->get("Y");
		if(y&&y->type==NBT_BYTE&&(int8_t)y->as_Int()==sy){
			return s;
		}
	}
	return NULL;
}
nbt_Any *Chunk::get_Section(int32_t sy,Chunk_Cursor *cur)const{
	if(!cur){
		return get_Section(sy);
	}
	return cur->find_Section(this,sy);
}
/*调色板压紧：只留 data 真正引用到的条目，重写全部4096格的下标。
  形状/长度/下标不对时返回false且一个字都不改（交给check_Chunk_Base整块判坏）。*/
static bool compact_Palette(nbt_Compound *sec){
	nbt_Any *bs=sec?sec->get("block_states"):NULL;
	if(!bs||bs->type!=NBT_COMPOUND){
		return false;
	}
	nbt_Compound *states=(nbt_Compound*)bs;
	nbt_Any *pa=states->get("palette");
	nbt_Any *da=states->get("data");
	if(!pa||pa->type!=NBT_LIST){
		return false;
	}
	nbt_List *pal=(nbt_List*)pa;
	if(pal->con.empty()||pal->elem!=NBT_COMPOUND){
		return false;
	}
	for(size_t i=0;i<pal->con.size();i++){
		if(!pal->con[i]||pal->con[i]->type!=NBT_COMPOUND){
			return false;/*调色板条目本身坏了：不压，交给check_Chunk_Base整块判坏*/
		}
	}
	size_t n=pal->con.size();
	std::vector<uint8_t> used(n,0);
	uint32_t old_bits=0,old_per=0,old_need=0;
	if(!da){
		if(n!=1){
			return false;/*多调色板却没有data：所有格子都读不出来，这不是能压紧的数据*/
		}
		used[0]=1;
	}else{
		if(da->type!=NBT_LONG_ARRAY){
			return false;
		}
		const std::vector<int64_t> &w=((const nbt_Long_Array*)da)->con;
		old_bits=4;
		while(old_bits<32&&((uint64_t)1<<old_bits)<n){
			old_bits++;
		}
		old_per=64/old_bits;
		old_need=(4096+old_per-1)/old_per;
		if(w.size()<old_need){
			return false;
		}
		uint64_t mask=((uint64_t)1<<old_bits)-1;
		for(uint32_t i=0;i<4096;i++){
			uint32_t idx=(uint32_t)(((uint64_t)w[i/old_per]>>((i%old_per)*old_bits))&mask);
			if(idx>=n){
				return false;/*越界：调色板/data本来就不一致，别猜*/
			}
			used[idx]=1;
		}
	}
	size_t used_n=0;
	for(size_t i=0;i<n;i++){
		used_n+=used[i]?1:0;
	}
	if(used_n==n){
		return false;/*没有废弃条目*/
	}
	std::vector<int32_t> remap(n,-1);
	nbt_List *np=new nbt_List(NBT_COMPOUND);
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
	if(used_n==1){
		states->erase("data");/*单值调色板不需要data*/
	}else{
		uint32_t bits=4;
		while(bits<32&&((uint64_t)1<<bits)<used_n){
			bits++;
		}
		uint32_t per=64/bits;
		uint32_t need=(4096+per-1)/per;
		std::vector<int64_t> w(need,0);
		/*先拷一份旧data：states->set("data")会free旧数组，不能拿它的引用继续读*/
		std::vector<int64_t> old=((const nbt_Long_Array*)da)->con;
		uint64_t mask=((uint64_t)1<<old_bits)-1;
		for(uint32_t i=0;i<4096;i++){
			uint32_t idx=(uint32_t)(((uint64_t)old[i/old_per]>>((i%old_per)*old_bits))&mask);
			uint64_t nidx=(uint64_t)remap[idx];
			w[i/per]|=(int64_t)(nidx<<((i%per)*bits));
		}
		states->set("data",new nbt_Long_Array(std::move(w)));
	}
	states->set("palette",np);
	return true;
}
int Chunk::format_Chunk(int *bad_y,int *palette_cleaned){
	if(bad_y){
		*bad_y=0;
	}
	if(palette_cleaned){
		*palette_cleaned=0;
	}
	nbt_Any *any=get("sections");
	if(!any){
		return 0;
	}
	if(any->type!=NBT_LIST){
		return -1;
	}
	nbt_List *secs=(nbt_List*)any;
	int removed=0;
	/*1. Y缺失/不是Byte：整段删掉（最小单位是子区块，不是整个区块）*/
	for(size_t i=0;i<secs->con.size();){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			i++;
			continue;
		}
		const nbt_Any *y=((nbt_Compound*)s)->get("Y");
		if(!y||y->type!=NBT_BYTE){
			secs->erase(i);
			removed++;
			if(bad_y){
				(*bad_y)++;
			}
			continue;
		}
		i++;
	}
	/*2. 同一Y有多段：留列表里第一段*/
	uint8_t seen[256]={0};
	for(size_t i=0;i<secs->con.size();){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			i++;
			continue;
		}
		const nbt_Any *y=((nbt_Compound*)s)->get("Y");
		if(!y||y->type!=NBT_BYTE){
			i++;
			continue;
		}
		uint8_t sy=(uint8_t)(int8_t)y->as_Int();
		if(seen[sy]){
			secs->erase(i);
			removed++;
			continue;
		}
		seen[sy]=1;
		i++;
	}
	/*3. 每个子区块的调色板压紧*/
	int pal_changed=0;
	for(size_t i=0;i<secs->con.size();i++){
		nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		if(compact_Palette((nbt_Compound*)s)){
			pal_changed++;
		}
	}
	if(palette_cleaned){
		*palette_cleaned=pal_changed;
	}
	if(removed||pal_changed){/*sections/调色板动过：拿着游标的调用者要重查*/
		format_generation++;
	}
	return removed;
}
/*---- 方块实体的坐标工具：xPos/zPos（Int）×16+x/z 走64位，溢出int32就当读不到 ----*/
static bool get_ChunkPos(const Chunk *c,int32_t *x,int32_t *z){
	const nbt_Any *xp=c?c->get("xPos"):NULL;
	const nbt_Any *zp=c?c->get("zPos"):NULL;
	if(!xp||xp->type!=NBT_INT||!zp||zp->type!=NBT_INT){
		return false;
	}
	*x=xp->as_Int();
	*z=zp->as_Int();
	return true;
}
static bool get_ChunkAbs(const Chunk *c,int x,int z,int32_t *ax,int32_t *az){
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
static nbt_Compound *find_BlockEntity(nbt_List *bes,int32_t ax,int32_t ay,int32_t az){
	if(!bes){
		return NULL;
	}
	for(size_t i=0;i<bes->con.size();i++){
		nbt_Any *e=bes->con[i];
		if(!e||e->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Compound *be=(const nbt_Compound*)e;
		const nbt_Any *ex=be->get("x");
		const nbt_Any *ey=be->get("y");
		const nbt_Any *ez=be->get("z");
		if(!ex||ex->type!=NBT_INT||!ey||ey->type!=NBT_INT||!ez||ez->type!=NBT_INT){
			continue;
		}
		if(ex->as_Int()==ax&&ey->as_Int()==ay&&ez->as_Int()==az){
			return (nbt_Compound*)e;
		}
	}
	return NULL;
}
static inline uint64_t be_LocalKey(int32_t y,int32_t lx,int32_t lz){
	return ((uint64_t)(uint32_t)y<<8)|((uint32_t)lz<<4)|(uint32_t)lx;
}
nbt_Compound *Chunk_Cursor::find_BlockEntityAt(const Chunk *c,int x,int y,int z){
	if(!c||x<0||x>15||z<0||z>15){
		return NULL;
	}
	int32_t cx=0,cz=0;
	if(!get_Origin(c,&cx,&cz)){
		return NULL;
	}
	int64_t bx=(int64_t)cx*16,bz=(int64_t)cz*16;
	int64_t ax=bx+x,az=bz+z;
	if(ax<INT32_MIN||ax>INT32_MAX||az<INT32_MIN||az>INT32_MAX){
		return NULL;
	}
	nbt_List *list=get_BlockEntities(c);
	if(!list||list->con.empty()){
		return NULL;
	}
	if(!be_index_ready||be_index_list!=list||be_index_len!=list->con.size()||
	   be_index_generation!=c->be_generation){
		be_index.clear();
		be_index.reserve(list->con.size());
		for(size_t i=0;i<list->con.size();i++){
			nbt_Any *e=list->con[i];
			if(!e||e->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Compound *be=(const nbt_Compound*)e;
			const nbt_Any *ex=be->get("x");
			const nbt_Any *ey=be->get("y");
			const nbt_Any *ez=be->get("z");
			if(!ex||ex->type!=NBT_INT||!ey||ey->type!=NBT_INT||!ez||ez->type!=NBT_INT){
				continue;
			}
			int64_t vx=ex->as_Int(),vy=ey->as_Int(),vz=ez->as_Int();
			if(vx<bx||vx>bx+15||vz<bz||vz>bz+15){/*不在本区块范围：永远配不上本区块的格子*/
				continue;
			}
			be_index.emplace(be_LocalKey((int32_t)vy,(int32_t)(vx&15),(int32_t)(vz&15)),(nbt_Compound*)e);
		}
		be_index_ready=true;
		be_index_list=list;
		be_index_len=list->con.size();
		be_index_generation=c->be_generation;
	}
	auto it=be_index.find(be_LocalKey(y,(int32_t)(ax&15),(int32_t)(az&15)));
	return it==be_index.end()?NULL:it->second;
}
Block Chunk::get_Block(int x,int y,int z)const{
	return get_Block(x,y,z,NULL);
}
Block Chunk::get_Block(int x,int y,int z,Chunk_Cursor *cur)const{
	bool full=cur?cur->is_Full(this):is_Full();
	if(!full||x<0||x>15||z<0||z>15){/*不完整区块不读；坐标越界也不读*/
		return Block();
	}
	int sy=get_SectionY(y);
	int ly=y-sy*16;
	nbt_Compound *sec=cur?cur->find_Section(this,sy):(nbt_Compound*)get_Section(sy);
	const nbt_Any *sc=cur?cur->secs_any:get("sections");
	if(sc&&sc->type!=NBT_LIST){/*sections成员在但不是list：坏数据，不能当空气（会把对方方块清掉）*/
		return Block();
	}
	if(!sec){/*这一段压根没有＝空气（用户定的：缺section算空气）*/
		return Block("minecraft:air");
	}
	if(cur){
		nbt_Compound *st=cur->get_States(sec);
		if(cur->states_bad){/*block_states不是compound：形状不对，读不到*/
			return Block();
		}
		if(!st){/*纯光段＝空气*/
			return Block("minecraft:air");
		}
	}else{
		const nbt_Any *bs=sec->get("block_states");
		if(!bs){/*纯光段＝空气*/
			return Block("minecraft:air");
		}
		if(bs->type!=NBT_COMPOUND){/*block_states不是compound：形状不对，读不到*/
			return Block();
		}
	}
	nbt_Any *state=cur?blockDataAtCursor(cur,x,ly,z,NULL):get_BlockData(sec,x,ly,z);
	if(!state||state->type!=NBT_COMPOUND){/*没有调色板/direct调色板/下标越界：读不到*/
		return Block();
	}
	const nbt_Any *name=((const nbt_Compound*)state)->get("Name");
	const char *nm=name?name->as_Str():NULL;
	if(!nm){/*没有Name：读不到*/
		return Block();
	}
	Block b;
	b.id=nbt_String(nm);
	const nbt_Any *props=((const nbt_Compound*)state)->get("Properties");
	if(props&&props->type==NBT_COMPOUND){
		b.properties=*(const nbt_Compound*)props;/*深拷贝*/
	}
	if(cur){
		if(!cur->bes_ok||cur->bes_generation!=be_generation){
			cur->get_BlockEntities(this);
		}
		if(cur->bes&&!cur->bes->con.empty()){
			nbt_Compound *be=cur->find_BlockEntityAt(this,x,y,z);
			if(be){
				b.block_entities=*be;/*深拷贝*/
			}
		}
	}else{
		nbt_List *bes=(nbt_List*)get("block_entities");
		if(bes&&bes->type==NBT_LIST&&!bes->con.empty()){/*没有方块实体就不查xPos/zPos，省两次哈希*/
			int32_t ax=0,az=0;
			if(get_ChunkAbs(this,x,z,&ax,&az)){
				nbt_Compound *be=find_BlockEntity(bes,ax,y,az);
				if(be){
					b.block_entities=*be;/*深拷贝*/
				}
			}
		}
	}
	return b;
}
static bool palette_Meta(Chunk_Cursor *cur,uint32_t idx,const nbt_Any **name,const nbt_Any **props){
	nbt_List *palette=cur?cur->palette:NULL;
	if(!palette||idx>=palette->con.size()){
		return false;
	}
	if(cur->palette_name.size()!=palette->con.size()||cur->palette_props.size()!=palette->con.size()){
		cur->palette_name.resize(palette->con.size(),NULL);
		cur->palette_props.resize(palette->con.size(),NULL);
		for(size_t i=0;i<palette->con.size();i++){
			const nbt_Any *e=palette->con[i];
			if(!e||e->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Compound *st=(const nbt_Compound*)e;
			cur->palette_name[i]=st->get("Name");
			const nbt_Any *p=st->get("Properties");
			cur->palette_props[i]=p&&p->type==NBT_COMPOUND?p:NULL;
		}
	}
	*name=cur->palette_name[idx];
	*props=cur->palette_props[idx];
	return true;
}
bool Chunk::get_BlockRef(int x,int y,int z,Block_Ref *out,Chunk_Cursor *cur)const{
	if(out){
		out->state=NULL;
		out->name=NULL;
		out->props=NULL;
		out->be=NULL;
	}
	bool full=cur?cur->is_Full(this):is_Full();
	if(!out||!full||x<0||x>15||z<0||z>15){
		return false;
	}
	int sy=get_SectionY(y);
	int ly=y-sy*16;
	nbt_Compound *sec=cur?cur->find_Section(this,sy):(nbt_Compound*)get_Section(sy);
	const nbt_Any *sc=cur?cur->secs_any:get("sections");
	if(sc&&sc->type!=NBT_LIST){
		return false;
	}
	if(!sec){/*缺段：空气，按get_Block的规矩不看方块实体*/
		out->name="minecraft:air";
		return true;
	}
	if(cur){
		nbt_Compound *st=cur->get_States(sec);
		if(cur->states_bad){
			return false;
		}
		if(!st){/*缺段/纯光段：空气，且按get_Block的规矩不看方块实体*/
			out->name="minecraft:air";
			return true;
		}
	}else{
		const nbt_Any *bs=sec?sec->get("block_states"):NULL;
		if(!bs){/*缺段/纯光段：空气，且按get_Block的规矩不看方块实体*/
			out->name="minecraft:air";
			return true;
		}
		if(bs->type!=NBT_COMPOUND){
			return false;
		}
	}
	uint32_t idx=0;
	nbt_Any *state=cur?blockDataAtCursor(cur,x,ly,z,&idx):get_BlockData(sec,x,ly,z);
	if(!state||state->type!=NBT_COMPOUND){
		return false;
	}
	const char *nm=NULL;
	const nbt_Any *props=NULL;
	if(cur){
		const nbt_Any *name_any=NULL,*props_any=NULL;
		if(!palette_Meta(cur,idx,&name_any,&props_any)){
			return false;
		}
		nm=name_any?name_any->as_Str():NULL;
		props=props_any;
	}else{
		const nbt_Any *name_any=((const nbt_Compound*)state)->get("Name");
		nm=name_any?name_any->as_Str():NULL;
		const nbt_Any *p=((const nbt_Compound*)state)->get("Properties");
		props=p&&p->type==NBT_COMPOUND?p:NULL;
	}
	if(!nm||!nm[0]){
		return false;
	}
	out->state=state;
	out->name=nm;
	out->props=props;
	if(cur){
		if(!cur->bes_ok||cur->bes_generation!=be_generation){
			cur->get_BlockEntities(this);
		}
		if(cur->bes&&!cur->bes->con.empty()){
			out->be=cur->find_BlockEntityAt(this,x,y,z);
		}
	}else{
		nbt_List *bes=(nbt_List*)get("block_entities");
		if(bes&&bes->type==NBT_LIST&&!bes->con.empty()){
			int32_t ax=0,az=0;
			if(get_ChunkAbs(this,x,z,&ax,&az)){
				out->be=find_BlockEntity(bes,ax,y,az);
			}
		}
	}
	return true;
}
bool block_Ref_Equal(const Block_Ref &a,const Block_Ref &b){
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
bool block_Ref_Equal(const Block_Ref &a,const Block &b){
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
static bool blockEntities_CanAppend(const Chunk *c,bool has_ent){/*写方块实体前的预检；没有实体要写时坏表也不能挡住方块写入*/
	const nbt_Any *ba=c?c->get("block_entities"):NULL;
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
bool Chunk::set_Block(const Block &block,int x,int y,int z){
	return set_Block(block,x,y,z,NULL);
}
bool Chunk::set_Block(const Block &block,int x,int y,int z,Chunk_Cursor *cur){
	if(!is_Full()||!block.valid()||x<0||x>15||z<0||z>15){/*不完整区块不写；空id也写不了*/
		return false;
	}
	bool has_ent=!block.block_entities.empty();
	int32_t ax=0,az=0;
	bool have_abs=get_ChunkAbs(this,x,z,&ax,&az);
	if(has_ent&&!have_abs){
		return false;/*带方块实体但xPos/zPos不对：先别动*/
	}
	int sy=get_SectionY(y);
	int ly=y-sy*16;
	nbt_Compound *sec=cur?cur->find_Section(this,sy):(nbt_Compound*)get_Section(sy);
	if(!sec){/*没有这一段：不自动建section（biome/光照不是这里能补的）*/
		return false;
	}
	nbt_Any *bs=sec->get("block_states");
	if(!bs||bs->type!=NBT_COMPOUND){/*没有调色板：不自动造*/
		return false;
	}
	nbt_Any *pal=((nbt_Compound*)bs)->get("palette");
	nbt_List *palette=pal&&pal->type==NBT_LIST?(nbt_List*)pal:NULL;
	if(!palette||palette->con.empty()||palette->elem!=NBT_COMPOUND){
		return false;
	}
	nbt_Any *dat=((nbt_Compound*)bs)->get("data");
	if(dat&&dat->type!=NBT_LONG_ARRAY){
		return false;
	}
	if(!blockEntities_CanAppend(this,has_ent)){
		return false;/*方块实体表形状坏：在写方块状态之前就拒绝，不能留下半写状态*/
	}
	nbt_Compound *st=new nbt_Compound;
	st->set_String("Name",block.id.con);
	if(!block.properties.empty()){
		st->set("Properties",block.properties.copy());/*copy()返回堆上新树，set接管*/
	}
	/*预检已经覆盖调色板/data/方块实体表的失败路径；这里原位写，成功后再动方块实体。
	  不克隆block_states，避免fixer持有的Blk_Info指针和游标缓存突然悬空。*/
	bool ret=cur?set_BlockData(sec,st,x,ly,z,cur):set_BlockData(sec,st,x,ly,z);
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
	/*方块实体表真变了时set_BlockEntity/erase_BlockEntity会+be_generation；游标按它重查，
	  没变就别把bes_ok打掉，省掉搬运方块时的重复哈希。*/
	return true;
}
bool Chunk::set_BlockEntity(const nbt_Compound &entity,int x,int y,int z){
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return false;
	}
	int32_t ax=0,az=0;
	if(!get_ChunkAbs(this,x,z,&ax,&az)){/*xPos/zPos不是Int：没法算绝对坐标*/
		return false;
	}
	nbt_Any *ba=get("block_entities");
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
	if(bes){
		size_t old=bes->con.size();
		for(size_t i=0;i<bes->con.size();i++){
			nbt_Any *e=bes->con[i];
			if(!e||e->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Compound *be=(const nbt_Compound*)e;
			const nbt_Any *ex=be->get("x");
			const nbt_Any *ey=be->get("y");
			const nbt_Any *ez=be->get("z");
			if(ex&&ex->type==NBT_INT&&ey&&ey->type==NBT_INT&&ez&&ez->type==NBT_INT&&
			   ex->as_Int()==ax&&ey->as_Int()==y&&ez->as_Int()==az){
				old=i;
				break;
			}
		}
		if(old<bes->con.size()){/*同一位置已有就换掉*/
			bes->con[old]->free();
			bes->con[old]=copy;
			be_generation++;
			return true;
		}
		if(bes->push_back(copy)){
			be_generation++;
			return true;
		}
		return false;/*push_back失败时已经释放了copy*/
	}
	bes=new nbt_List(NBT_COMPOUND);
	set("block_entities",bes);
	if(!bes->push_back(copy)){
		return false;/*push_back失败时已经释放了copy*/
	}
	be_generation++;
	return true;
}
int Chunk::erase_BlockEntity(int x,int y,int z){
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return 0;
	}
	int32_t ax=0,az=0;
	if(!get_ChunkAbs(this,x,z,&ax,&az)){
		return 0;
	}
	nbt_Any *ba=get("block_entities");
	if(!ba||ba->type!=NBT_LIST){
		return 0;
	}
	nbt_List *bes=(nbt_List*)ba;
	int del=0;
	for(size_t i=bes->con.size();i>0;i--){/*从后往前：erase不会挪动更小的下标*/
		nbt_Any *e=bes->con[i-1];
		if(!e||e->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Compound *be=(const nbt_Compound*)e;
		const nbt_Any *ex=be->get("x");
		const nbt_Any *ey=be->get("y");
		const nbt_Any *ez=be->get("z");
		if(ex&&ex->type==NBT_INT&&ey&&ey->type==NBT_INT&&ez&&ez->type==NBT_INT&&
		   ex->as_Int()==ax&&ey->as_Int()==y&&ez->as_Int()==az){
			bes->erase(i-1);
			del++;
		}
	}
	if(del){
		be_generation++;
	}
	return del;
}
nbt_Compound *Chunk::get_BlockEntity(int x,int y,int z)const{
	if(!is_Full()||x<0||x>15||z<0||z>15){
		return NULL;
	}
	int32_t ax=0,az=0;
	if(!get_ChunkAbs(this,x,z,&ax,&az)){
		return NULL;
	}
	const nbt_Any *ba=get("block_entities");
	nbt_List *bes=ba&&ba->type==NBT_LIST?(nbt_List*)ba:NULL;
	return find_BlockEntity(bes,ax,y,az);
}

/*==================== 计划刻（block_ticks / fluid_ticks） ====================
  表里的元素形状和坐标含义见 nbtrw.hpp 的 Chunk_Tick 注释。这里只认“六样俱全且类型对”的条目：
  缺一样就当整张表形状不对（返回-1），调用者一个字都不改。*/
static const char *get_TickName(int fluid){return fluid?"fluid_ticks":"block_ticks";}
static bool read_Tick(const nbt_Any *e,Chunk_Tick *t){/*一条刻：形状对不对（i是String，x/y/z/t/p都是Int）*/
	if(!e||e->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *c=(const nbt_Compound*)e;
	const nbt_Any *i=c->get("i"),*x=c->get("x"),*y=c->get("y"),*z=c->get("z"),*d=c->get("t"),*p=c->get("p");
	if(!i||i->type!=NBT_STRING||!x||x->type!=NBT_INT||!y||y->type!=NBT_INT||!z||z->type!=NBT_INT||
	   !d||d->type!=NBT_INT||!p||p->type!=NBT_INT){
		return false;
	}
	t->type=std::string_view(i->as_Str());
	t->x=x->as_Int();
	t->y=y->as_Int();
	t->z=z->as_Int();
	t->delay=d->as_Int();
	t->priority=p->as_Int();
	return true;
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
int count_ChunkTicks(const nbt_Compound *chunk,int fluid){
	int n=0;
	if(!chunk){
		return 0;
	}
	return check_TickShape(chunk->get(get_TickName(fluid)),&n)?n:-1;
}
bool get_ChunkTick(const nbt_Compound *chunk,int fluid,int i,Chunk_Tick *out){
	if(!chunk||!out){
		return false;
	}
	const nbt_Any *f=chunk->get(get_TickName(fluid));
	if(!f||f->type!=NBT_LIST){
		return false;
	}
	const nbt_List *l=(const nbt_List*)f;
	if(i<0||(size_t)i>=l->con.size()){
		return false;
	}
	return read_Tick(l->con[i],out);
}
int del_ChunkTick(nbt_Compound *chunk,int fluid,int32_t x,int32_t y,int32_t z){
	if(!chunk){
		return 0;
	}
	nbt_Any *f=chunk->get(get_TickName(fluid));
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
bool add_ChunkTick(nbt_Compound *chunk,int fluid,const char *type,int32_t x,int32_t y,int32_t z,
	int32_t delay,int32_t priority){
	if(!chunk||!type||!type[0]){
		return false;
	}
	nbt_Any *f=chunk->get(get_TickName(fluid));
	if(f&&f->type!=NBT_LIST){
		return false;/*表本身不是List：区块级坏数据，不往里塞*/
	}
	if(!f){
		f=new nbt_List(NBT_COMPOUND);
		chunk->set(get_TickName(fluid),f);
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
		l->elem=NBT_END;/*坏记录删光了：让下一條push_back重新定类型*/
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
int filter_ChunkTick(nbt_Compound *chunk,int fluid,Chunk_Tick_Keep_Fn keep,void *ud){
	if(!chunk){
		return 0;
	}
	nbt_Any *f=chunk->get(get_TickName(fluid));
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

/*check_Chunk：NBT成员类型和必需结构是不是完整。merge看到false就跳过，fixer用check_Chunk_Base。
  只认形状，不认“这格方块合不合理”：缺section/纯光段/缺Status都算正常数据。
  strict=true：Y必须是Byte、每条计划刻六样俱全（merge安全跳过的门槛）；
  strict=false：区块级结构（sections/block_states/palette/data/block_entities/刻表类型），
                单条刻/Y的问题交给fixer按最小单位删。*/
static bool check_Chunk_Internal(const Chunk *c,bool strict){
	const nbt_Any *sa=c?c->get("sections"):NULL;
	if(sa){
		if(sa->type!=NBT_LIST){
			return false;
		}
		const nbt_List *secs=(const nbt_List*)sa;
		for(size_t i=0;i<secs->con.size();i++){
			nbt_Any *a=secs->con[i];
			if(!a||a->type!=NBT_COMPOUND){
				return false;
			}
			nbt_Compound *sec=(nbt_Compound*)a;
			const nbt_Any *yv=sec->get("Y");
			if(strict&&(!yv||yv->type!=NBT_BYTE)){
				return false;
			}
			const nbt_Any *bs=sec->get("block_states");
			if(!bs){
				continue;/*纯光段：合法*/
			}
			if(bs->type!=NBT_COMPOUND){
				return false;
			}
			const nbt_Compound *states=(const nbt_Compound*)bs;
			const nbt_Any *pa=states->get("palette");
			if(!pa||pa->type!=NBT_LIST){
				return false;
			}
			const nbt_List *palette=(const nbt_List*)pa;
			if(palette->con.empty()||palette->elem!=NBT_COMPOUND){
				return false;
			}
			for(size_t k=0;k<palette->con.size();k++){
				const nbt_Any *e=palette->con[k];
				if(!e||e->type!=NBT_COMPOUND||!get_Name(e)||!get_Name(e)[0]){
					return false;/*调色板条目没有Name：就是坏数据*/
				}
			}
			const nbt_Any *d=states->get("data");
			if(d&&d->type!=NBT_LONG_ARRAY){
				return false;
			}
			if(palette->con.size()>1&&!d){
				return false;/*多调色板却没有data：所有格子都读不出来*/
			}
			if(d){
				uint32_t bits=4;
				while(bits<32&&((uint64_t)1<<bits)<palette->con.size()){
					bits++;
				}
				uint32_t per=64/bits;
				uint32_t need=(4096+per-1)/per;
				const std::vector<int64_t> &w=((const nbt_Long_Array*)d)->con;
				if(w.size()<need){
					return false;/*data比该有的短：剩下的是垃圾*/
				}
				uint64_t mask=((uint64_t)1<<bits)-1;
				for(uint32_t i=0;i<4096;i++){
					uint32_t idx=(uint32_t)(((uint64_t)w[i/per]>>((i%per)*bits))&mask);
					if(idx>=palette->con.size()){
						return false;/*下标指到调色板外：所有格子都可能读错*/
					}
				}
			}
		}
	}
	const nbt_Any *ba=c?c->get("block_entities"):NULL;
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
		for(size_t i=0;i<bes->con.size();i++){
			if(!bes->con[i]||bes->con[i]->type!=NBT_COMPOUND){
				return false;
			}
		}
	}
	for(int fluid=0;fluid<2;fluid++){
		const nbt_Any *f=c?c->get(get_TickName(fluid)):NULL;
		if(!f){
			continue;
		}
		if(f->type!=NBT_LIST){
			return false;/*刻表本身不是List：这是区块级NBT坏，最小单位也只能整块删*/
		}
		if(strict){
			int n=0;
			if(!check_TickShape(f,&n)){
				return false;
			}
		}
	}
	return true;
}
bool check_Chunk(const Chunk *c){
	return check_Chunk_Internal(c,true);
}
bool check_Chunk_Base(const Chunk *c){
	return check_Chunk_Internal(c,false);
}

/*==================== POI：poi/目录里的方块索引 ====================*/
static bool is_AirName(const char *name){/*空气：air/cave_air/void_air都没有方块*/
	return name&&(!strcmp(name,"minecraft:air")||!strcmp(name,"minecraft:cave_air")||!strcmp(name,"minecraft:void_air"));
}
static nbt_Compound *get_POISections(const nbt_Compound *chunk){
	if(!chunk){
		return NULL;
	}
	const nbt_Any *s=chunk->get("Sections");
	return s&&s->type==NBT_COMPOUND?(nbt_Compound*)s:NULL;
}
static bool is_POIKeyOk(const std::string &key){/*段键＝段Y的十进制字符串*/
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
bool is_POIShapeOk(const nbt_Compound *chunk){/*形状不对＝这一块读不出来，调用者整块跳过*/
	if(!chunk||chunk->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Any *sa=chunk->get("Sections");
	if(sa&&sa->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *secs=sa?(const nbt_Compound*)sa:NULL;
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		if(!is_POIKeyOk(it->first)){
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
static bool get_POISectionRecords(const nbt_Compound *sec,const nbt_List **out){/*一段里的Records列表；没有/类型不对给NULL*/
	if(!sec||sec->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Any *r=sec->get("Records");
	if(!r){
		*out=NULL;
		return true;/*没写Records＝这段没记录*/
	}
	if(r->type!=NBT_LIST){
		return false;
	}
	*out=(const nbt_List*)r;
	return true;
}
int count_POI(const nbt_Compound *chunk){
	const nbt_Compound *secs=get_POISections(chunk);
	int n=0;
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!get_POISectionRecords((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		n+=(int)(recs?recs->con.size():0);
	}
	return n;
}
bool get_POIAt(const nbt_Compound *chunk,int idx,int32_t *x,int32_t *y,int32_t *z,const char **type,int32_t *free_tickets){
	const nbt_Compound *secs=get_POISections(chunk);
	int n=0;
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!get_POISectionRecords((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		for(size_t i=0;recs&&i<recs->con.size();i++){
			if(n++!=idx){
				continue;
			}
			const nbt_Any *e=recs->con[i];
			if(!e||e->type!=NBT_COMPOUND){/*形状不对：这一条读不出来*/
				return false;
			}
			const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
			const nbt_Any *t=((const nbt_Compound*)e)->get("type");
			const nbt_Any *f=((const nbt_Compound*)e)->get("free_tickets");
			if(!p||p->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)p)->con.size()!=3||!t||t->type!=NBT_STRING||!f||f->type!=NBT_INT){
				return false;
			}
			*x=((const nbt_Int_Array*)p)->con[0];
			*y=((const nbt_Int_Array*)p)->con[1];
			*z=((const nbt_Int_Array*)p)->con[2];
			*type=t->as_Str();
			*free_tickets=f->as_Int();
			return true;
		}
	}
	return false;
}
const nbt_Any *get_POI(const nbt_Compound *chunk,int32_t x,int32_t y,int32_t z){
	const nbt_Compound *secs=get_POISections(chunk);
	for(nbt_Compound::const_iterator it=secs?secs->con.begin():nbt_Compound::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!get_POISectionRecords((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		for(size_t i=0;recs&&i<recs->con.size();i++){
			const nbt_Any *e=recs->con[i];
			if(!e||e->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
			if(!p||p->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)p)->con.size()!=3){
				continue;
			}
			if(((const nbt_Int_Array*)p)->con[0]==x&&((const nbt_Int_Array*)p)->con[1]==y&&((const nbt_Int_Array*)p)->con[2]==z){
				return e;
			}
		}
	}
	return NULL;
}
static nbt_Compound *make_POISection(nbt_Compound *chunk,int32_t sy){/*拿这一段的compound，没有就建*/
	char key[24];
	snprintf(key,sizeof(key),"%d",sy);
	nbt_Any *sa=chunk->get("Sections");
	nbt_Compound *secs=sa&&sa->type==NBT_COMPOUND?(nbt_Compound*)sa:NULL;
	if(!secs){
		secs=new nbt_Compound;
		chunk->set("Sections",secs);
	}
	nbt_Any *s=secs->get(key);
	nbt_Compound *sec=s&&s->type==NBT_COMPOUND?(nbt_Compound*)s:NULL;
	if(!sec){
		sec=new nbt_Compound;
		secs->set(key,sec);
	}
	nbt_Any *ra=sec->get("Records");
	if(!ra||ra->type!=NBT_LIST||((nbt_List*)ra)->elem!=NBT_COMPOUND){
		sec->set("Records",new nbt_List(NBT_COMPOUND));
	}
	sec->set("Valid",new nbt_Byte(1));
	return sec;
}
bool set_POI(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z,const char *type,int32_t free_tickets){
	if(!chunk||!type||!type[0]){
		return false;
	}
	int32_t sy=get_SectionY(y);
	nbt_Compound *sec=make_POISection(chunk,sy);
	nbt_List *recs=(nbt_List*)sec->get("Records");
	nbt_Compound *rec=new nbt_Compound;
	rec->set_String("type",type);
	nbt_Int_Array *pos=new nbt_Int_Array;
	pos->push_back(x);pos->push_back(y);pos->push_back(z);
	rec->set("pos",pos);
	rec->set_Int("free_tickets",free_tickets);
	for(size_t i=0;i<recs->con.size();i++){
		const nbt_Any *e=recs->con[i];
		if(!e||e->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
		if(!p||p->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)p)->con.size()!=3){
			continue;
		}
		if(((const nbt_Int_Array*)p)->con[0]==x&&((const nbt_Int_Array*)p)->con[1]==y&&((const nbt_Int_Array*)p)->con[2]==z){
			recs->con[i]->free();/*同一位置已有：换掉*/
			recs->con[i]=rec;
			return true;
		}
	}
	if(!recs->push_back(rec)){
		return false;/*push_back失败时已经释放了rec*/
	}
	return true;
}
int del_POI(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z){
	nbt_Compound *secs=get_POISections(chunk);
	int del=0;
	for(nbt_Compound::iterator it=secs?secs->con.begin():nbt_Compound::iterator();secs&&it!=secs->con.end();++it){
		nbt_Any *s=it->second;
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		nbt_Any *ra=((nbt_Compound*)s)->get("Records");
		nbt_List *recs=ra&&ra->type==NBT_LIST?(nbt_List*)ra:NULL;
		for(size_t i=0;recs&&i<recs->con.size();){
			const nbt_Any *e=recs->con[i];
			const nbt_Any *p=e&&e->type==NBT_COMPOUND?((const nbt_Compound*)e)->get("pos"):NULL;
			if(p&&p->type==NBT_INT_ARRAY&&((const nbt_Int_Array*)p)->con.size()==3&&
			   ((const nbt_Int_Array*)p)->con[0]==x&&((const nbt_Int_Array*)p)->con[1]==y&&
			   ((const nbt_Int_Array*)p)->con[2]==z){
				recs->erase(i);
				del++;
				continue;
			}
			i++;
		}
	}
	return del;
}
int clean_POI(nbt_Compound *chunk){/*Records空的段直接删掉*/
	nbt_Compound *secs=get_POISections(chunk);
	if(!secs){
		return 0;
	}
	std::vector<std::string> drop;
	for(nbt_Compound::iterator it=secs->con.begin();it!=secs->con.end();++it){
		nbt_Any *s=it->second;
		const nbt_List *recs=NULL;
		if(!s||s->type!=NBT_COMPOUND||!get_POISectionRecords((const nbt_Compound*)s,&recs)||!recs||recs->con.empty()){
			drop.push_back(it->first);
		}
	}
	for(size_t i=0;i<drop.size();i++){
		secs->erase(drop[i]);
	}
	return (int)drop.size();
}

bool has_Block(const Chunk *c){/*这个区块里有没有任意一个非空气方块？*/
	const nbt_Any *sc=c?c->get("sections"):NULL;
	if(!sc||sc->type!=NBT_LIST){
		return false;/*连sections都没有：一个方块都没有*/
	}
	const nbt_List *secs=(const nbt_List*)sc;
	for(size_t i=0;i<secs->con.size();i++){
		const nbt_Any *s=secs->con[i];
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *bs=((const nbt_Compound*)s)->get("block_states");
		if(!bs||bs->type!=NBT_COMPOUND){
			continue;/*没有block_states（只有光的段）＝空气*/
		}
		const nbt_Any *pany=((const nbt_Compound*)bs)->get("palette");
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
			if(!is_AirName(get_Name(pal->con[j]))){
				has_solid=true;break;
			}
		}
		if(!has_solid){
			continue;/*整段只有空气*/
		}
		const nbt_Any *dany=((const nbt_Compound*)bs)->get("data");
		if(!dany||dany->type!=NBT_LONG_ARRAY){/*没有data：整段都是palette[0]*/
			if(!is_AirName(get_Name(pal->con[0]))){
				return true;
			}
			continue;
		}
		const nbt_Long_Array *d=(const nbt_Long_Array*)dany;
		/*先扫data里有没有非0的long：全0＝每一格都是palette[0]，快得多*/
		{
			bool any=false;
			for(size_t j=0;j<d->con.size();j++){
				if(d->con[j]){
					any=true;
					break;
				}
			}
			if(!any){
				if(!is_AirName(get_Name(pal->con[0]))){
					return true;
				}
				continue;
			}
		}
		uint32_t bits=get_PaletteBits(n);/*bits=max(4,ceil(log2(调色板长度)))，每long从低位往高位数格*/
		uint32_t per=64/bits;
		uint64_t mask=((uint64_t)1<<bits)-1;
		for(uint32_t k=0;k<4096;k++){/*位置顺序是 y*256+z*16+x，这里只关心有没有非空气*/
			size_t w=k/per;
			if(w>=d->con.size()){
				return true;/*data不够长：形状不对，宁可信其有*/
			}
			uint32_t idx=(uint32_t)(((uint64_t)d->con[w]>>((k%per)*bits))&mask);
			if(idx<n&&!is_AirName(get_Name(pal->con[idx]))){
				return true;
			}
		}
	}
	return false;
}
Chunk_Full_Status get_ChunkStatus(const Chunk *c){/*这个区块生成完了吗*/
	const nbt_Any *st=c?c->get("Status"):NULL;
	if(!st||st->type!=NBT_STRING){
		return CHUNK_NO_STATUS;/*没有Status字段（老存档/别的工具写的）：不知道*/
	}
	return !strcmp(st->as_Str(),"minecraft:full")?CHUNK_FULL:CHUNK_NOT_FULL;
}

/*---- 实体区块：容器和单条记录分开查 ----
  fixer：容器坏→整块实体删；单条记录坏→只删那一条。
  merge：check_Entity 严格，任何一条坏就整块跳过（merge不清理）。*/
static bool entity_IsNumber(const nbt_Any *a){
	if(!a){
		return false;
	}
	switch(a->type){
		case NBT_BYTE:case NBT_SHORT:case NBT_INT:case NBT_LONG:
		case NBT_FLOAT:case NBT_DOUBLE:
			return true;
		default:
			return false;
	}
}
bool check_EntityRecord(const nbt_Any *entity){
	if(!entity||entity->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *e=(const nbt_Compound*)entity;
	const nbt_Any *id=e->get("id");
	if(!id||id->type!=NBT_STRING||!id->as_Str()||!id->as_Str()[0]){
		return false;
	}
	const nbt_Any *uuid=e->get("UUID");
	if(!uuid||uuid->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)uuid)->con.size()!=4){
		return false;
	}
	const nbt_Any *pos=e->get("Pos");
	if(!pos||pos->type!=NBT_LIST){
		return false;
	}
	const nbt_List *l=(const nbt_List*)pos;
	if(l->con.size()<3){
		return false;
	}
	for(int i=0;i<3;i++){
		if(!entity_IsNumber(l->con[(size_t)i])){
			return false;
		}
	}
	return true;
}
bool check_Entity_Container(const nbt_Compound *chunk,int32_t cx,int32_t cz){
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
	if(!dv||dv->type!=NBT_INT){
		return false;
	}
	const nbt_Any *es=chunk->get("Entities");
	if(!es||es->type!=NBT_LIST){
		return false;
	}
	return true;
}
bool check_Entity(const nbt_Compound *chunk,int32_t cx,int32_t cz){
	if(!check_Entity_Container(chunk,cx,cz)){
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
