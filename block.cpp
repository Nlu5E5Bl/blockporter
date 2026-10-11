#include "nbt.hpp"
/*block.cpp：方块状态（Block取名字那几个成员 + 方块状态属性小工具 + section里调色板那套读写）
 *	Block里只有两个指针，name/nspace/basename/strid/properties都从NBT现取，不单独存
 *	get_Name/get_Basename/get_Prop/get_Prop_Is是给外面（merge/fixer/rules.so）用的同一套取法
 *	section里的方块状态是"调色板+data"：位宽按调色板长度取最小（MC就是这么存的），
 *	所以位宽能从调色板长度反推；带Chunk_Cursor的版本把这些查一次记住，连着读快很多
 */

/*---- 方块状态的小工具：从nbt里现取 ----*/
const char *get_Name(const nbt_Any *b){/*block里的Name，不是compound或者没有就给空串*/
	const nbt_Any *n=b&&b->type==0x0a?((const nbt_Compound*)b)->get("Name"):NULL;
	const char *s=n?n->as_Str():NULL;
	return s?s:"";
}
const char *get_Basename(const nbt_Any *b){/*冒号后那段；没有冒号就是整个name*/
	const char *n=get_Name(b);
	const char *p=strchr(n,':');
	return p?p+1:n;
}
const char *get_Prop(const nbt_Any *b,const char *key){/*Properties.<key>，不是字符串或者没有就给NULL*/
	if(!b||b->type!=0x0a){
		return NULL;
	}
	nbt_Any *p=((nbt_Compound*)b)->get("Properties");
	if(!p||p->type!=0x0a){
		return NULL;
	}
	nbt_Any *v=((nbt_Compound*)p)->get(key);
	return v?v->as_Str():NULL;
}
int get_Prop_Is(const nbt_Any *b,const char *key,const char *val){/*属性正好等于val？*/
	const char *s=get_Prop(b,key);
	return s&&!strcmp(s,val);
}

/*---- Block：从nbt里现取 ----*/
static std::string_view name_Of(const Block &b){/*block里的Name，没有就是空串*/
	const char *s=get_Name(b.block);
	return std::string_view(s,strlen(s));
}
std::string_view Block::name()const{
	return name_Of(*this);
}
std::string_view Block::nspace()const{/*冒号前那段；没有冒号算minecraft（namespace是关键字，只能叫nspace）*/
	std::string_view n=name();
	size_t p=n.find(':');
	return p==std::string_view::npos?"minecraft":n.substr(0,p);
}
std::string_view Block::basename()const{/*冒号后那段；没有冒号就是整个name*/
	const char *s=get_Basename(block);
	return std::string_view(s,strlen(s));
}
std::string Block::strid()const{/*namespace+':'+basename；本来没冒号的会被补上minecraft:*/
	return std::string(nspace())+":"+std::string(basename());
}
nbt_Compound *Block::properties()const{/*block里的Properties，不是compound或没有就NULL*/
	const nbt_Any *p=block&&block->type==0x0a?((const nbt_Compound*)block)->get("Properties"):NULL;
	return p&&p->type==0x0a?(nbt_Compound*)p:NULL;
}

/*---- section里的方块状态：调色板那套 ----*/
nbt_Compound *Chunk_Cursor::states_Of(nbt_Compound *section){/*section的block_states（连palette/data一起记住）
															   成员数变了说明树被外面动过（比如给纯空气段补了block_states），那就重查*/
	if(states_sec==section&&states_members==section->con.size()&&palette){
		return states;
	}
	states_sec=section;
	states_members=section->con.size();
	nbt_Any *s=section->get("block_states");
	states=s&&s->type==0x0a?(nbt_Compound*)s:NULL;
	nbt_Any *p=states?states->get("palette"):NULL;
	palette=p&&p->type==0x09?(nbt_List*)p:NULL;
	data=NULL;/*换了section，data重新查*/
	data_len=0;
	return states;
}
nbt_Any *get_BlockData(nbt_Compound *section,Chunk_Cursor *cur,int lx,int ly,int lz){/*带游标：调色板/data都在游标里*/
	if(!section||!cur){
		return NULL;
	}
	if(lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15；越界了直接说读不出来*/
		return NULL;
	}
	cur->states_Of(section);
	nbt_List *palette=cur->palette;
	if(!palette||palette->con.empty()){
		return NULL;
	}
	if(cur->data_len!=palette->con.size()){/*调色板长度变了就把data重查一遍（往里加方块时它一定同时变长）*/
		nbt_Any *d=cur->states?cur->states->get("data"):NULL;
		cur->data=d&&d->type==0x0c?(nbt_Long_Array*)d:NULL;
		cur->data_len=palette->con.size();
	}
	nbt_Long_Array *data=cur->data;
	uint32_t idx=0;
	if(data){
		uint32_t bits=4;
		while(bits<32&&(1u<<bits)<palette->con.size()){/*加bits<32：调色板长度是文件里来的，别让1u<<32变成未定义*/
			bits++;
		}
		if(bits>31){
			return NULL;/*这种长度不可能是真存档*/
		}
		uint32_t per=64/bits;
		uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
		if(i/per>=data->con.size()){
			return NULL;
		}
		idx=(uint32_t)(data->con[i/per]>>((i%per)*bits))&((1u<<bits)-1);
	}
	if(idx>=palette->con.size()){
		return NULL;
	}
	return palette->con[idx];
}
nbt_Any *get_BlockData(nbt_Compound *section,int lx,int ly,int lz){/*不带游标：临时游标走一遍，等于现查*/
	Chunk_Cursor cur;
	return get_BlockData(section,&cur,lx,ly,lz);
}
int section_Const(nbt_Compound *section,Chunk_Cursor *cur,nbt_Any **block){/*这一段是不是整段同一个方块？是就填*block（空气填NULL），不是返回0*/
	Chunk_Cursor tmp;
	if(!cur){
		cur=&tmp;
	}
	nbt_Compound *states=section?cur->states_Of(section):NULL;
	if(!section||!states){/*没有这段section，或者这段没有block_states（纯光段）＝全空气*/
		*block=NULL;
		return 1;
	}
	nbt_List *palette=cur->palette;
	if(!palette||palette->con.size()!=1){
		return 0;/*读不出来（direct调色板），或者不止一种方块*/
	}
	if(states->get("data")){
		return 0;/*调色板只有一条却还带data：下标可能是乱的，不当常量看*/
	}
	nbt_Any *b=palette->con[0];
	const nbt_Any *n=b&&b->type==0x0a?((const nbt_Compound*)b)->get("Name"):NULL;
	const char *name=n?n->as_Str():NULL;
	*block=(name&&!strcmp(name,"minecraft:air"))?NULL:b;/*空气一律算NULL，"没有这段"和"这段全是空气"就是同一个意思*/
	return 1;
}
static int state_Equal(const nbt_Any *a,const nbt_Any *b){/*方块状态比较：先比Name（不一样就不用深比了），一样再cmp*/
	if(a==b){
		return 1;
	}
	if(!a||!b||a->type!=0x0a||b->type!=0x0a){
		return a&&b?a->cmp(b):0;
	}
	const nbt_Any *na=((const nbt_Compound*)a)->get("Name");
	const nbt_Any *nb=((const nbt_Compound*)b)->get("Name");
	if(na&&nb&&na->type==0x08&&nb->type==0x08&&((nbt_String*)na)->con!=((nbt_String*)nb)->con){
		return 0;
	}
	return a->cmp(b);
}
int set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz,Chunk_Cursor *cur){/*把方块状态写进section，返回是否成功*/
	if(!section||!block||block->type!=0x0a){
		return 0;
	}
	if(lx<0||lx>15||ly<0||ly>15||lz<0||lz>15){/*section里的局部坐标只有0~15*/
		return 0;
	}
	nbt_Compound *states;
	nbt_List *palette;
	if(cur){
		states=cur->states_Of(section);
		palette=cur->palette;
	}else{
		nbt_Any *s=section->get("block_states");
		states=s&&s->type==0x0a?(nbt_Compound*)s:NULL;
		nbt_Any *p=states?states->get("palette"):NULL;
		palette=p&&p->type==0x09?(nbt_List*)p:NULL;
	}
	if(!palette||palette->con.empty()){
		return 0;
	}
	nbt_Any *d=states?states->get("data"):NULL;
	nbt_Long_Array *data=d&&d->type==0x0c?(nbt_Long_Array*)d:NULL;
	uint32_t old_len=(uint32_t)palette->con.size(),bits_old=0;/*MC保存时位宽总按palette长度取最小，所以位宽能从长度反推*/
	if(data){
		bits_old=4;
		while(bits_old<32&&(1u<<bits_old)<old_len){/*加bits_old<32：长度是文件里来的，别让1u<<32变成未定义*/
			bits_old++;
		}
		if(bits_old>31||old_len>256){/*>256条是direct调色板，本函数不支持（下面还有一道同样的检查）*/
			return 0;
		}
	}
	uint32_t idx=old_len;
	for(uint32_t i=0;i<old_len;i++){
		if(state_Equal(palette->con[i],block)){/*已有同样状态就复用，不重复加*/
			idx=i;
			break;
		}
	}
	if(idx==old_len){
		if(old_len>=256){/*超过256条MC要改用direct调色板（全局id），这里不支持*/
			return 0;
		}
		palette->push_back(block->copy());
	}
	uint32_t bits=4;
	while((1u<<bits)<palette->con.size()){
		bits++;
	}
	uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
	if(!data){
		if(idx==0){/*整段都指向palette[0]，不用建data*/
			return 1;
		}
		data=new nbt_Long_Array;
		data->con.resize((4096+64/bits-1)/(64/bits),0);/*下标不跨long，长度向上取整*/
		states->set("data",data);
	}else if(bits!=bits_old){/*位宽变了要把整段下标重排*/
		uint32_t per_old=64/bits_old,per_new=64/bits;
		uint32_t need_old=(4096+per_old-1)/per_old;
		if(data->con.size()<need_old){/*文件里的data比该有的短（不该发生，但别越界读）：缺的当palette[0]*/
			data->con.resize(need_old,0);
		}
		std::vector<uint64_t> buf((4096+per_new-1)/per_new,0);
		for(uint32_t k=0;k<4096;k++){
			uint64_t v=(data->con[k/per_old]>>((k%per_old)*bits_old))&((1ull<<bits_old)-1);
			buf[k/per_new]|=v<<((k%per_new)*bits);
		}
		data->con=std::move(buf);
	}
	uint32_t per=64/bits,shift=(i%per)*bits;
	uint32_t need=(4096+per-1)/per;
	if(data->con.size()<need){/*同上：短了就补长再写*/
		data->con.resize(need,0);
	}
	uint64_t mask=((uint64_t)1<<bits)-1;
	data->con[i/per]=(data->con[i/per]&~(mask<<shift))|((uint64_t)idx<<shift);
	if(cur){/*游标里记住的"这一段有没有data"可能刚被这次写入改掉（原来palette里已经有这个状态、
	          但这一段没有data，这次建出了data）：data_len还等于palette长度，get_BlockData
	          就会照着旧结论返回palette[0]，读出来的方块是错的。这里直接作废，让它下次重查一遍*/
		cur->data=NULL;
		cur->data_len=0;
	}
	return 1;
}
int set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz){
	return set_BlockData(section,block,lx,ly,lz,NULL);
}
