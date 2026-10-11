#include "nbt.hpp"
#include <algorithm>
/*chunk.cpp：区块（世界坐标那套），和原始NBT操作分开
 *	section本身就是个compound，它的方块状态（调色板）读写放在block.cpp
 *	Chunk_Cursor是把"sy→section"记下来的小备忘：连着按格子读一个区块时，
 *	不用每个方块都把sections列表扫一遍
 *	POI（poi/目录里的方块索引）也在这里：它就是个普通compound，按段存记录，
 *	和方块区块一样是"按区块存"的，所以跟着这个文件走
 */

int64_t nbt_bad_poi=0;/*POI区块/记录坏了几处（跳过+计数，见nbt.hpp）*/
int64_t nbt_bad_entity=0;/*实体区块坏了几处*/
static void bad_Poi(const char *what,int32_t a,int32_t b,int32_t c){/*记一笔，并最多打前BAD_SHOW条*/
	if(nbt_bad_poi<=BAD_SHOW){
		printf("poi bad:%s（%d,%d,%d）：这一块/这条跳过（后面同类只计数）\n",what,a,b,c);
	}
	nbt_bad_poi++;
}
static void bad_Entity(const char *what,int32_t x,int32_t z){/*实体区块的坏数据*/
	if(nbt_bad_entity<=BAD_SHOW){
		printf("entity bad:%s（chunk %d,%d）：这一块跳过（后面同类只计数）\n",what,x,z);
	}
	nbt_bad_entity++;
}

/*---- Chunk_Cursor：连着读同一个区块的游标 ----*/
int Chunk_Cursor::follows(const Chunk *c)const{/*地址和serial都对上才算跟的是同一个区块*/
	return chunk==c&&serial==c->serial;
}
void Chunk_Cursor::reset(){/*换区块、或者自己大改过树之后清一下最保险*/
	chunk=NULL;
	serial=0;
	secs=NULL;
	secs_len=0;
	sy=INT32_MIN;
	sec=NULL;
	states_sec=NULL;
	states_members=0;
	states=NULL;
	palette=NULL;
	data=NULL;
	data_len=0;
}
nbt_Compound *Chunk_Cursor::find_Section(const Chunk *c,int32_t sy){
	if(!follows(c)){/*换区块了（哪怕地址看着一样，serial不同就是另一个区块）：重新拿sections列表，别的缓存全作废*/
		chunk=c;
		serial=c->serial;
		nbt_Any *s=c->get("sections");
		secs=s&&s->type==0x09?(nbt_List*)s:NULL;
		secs_len=secs?secs->con.size():0;
		this->sy=INT32_MIN;
		sec=NULL;
		states_sec=NULL;
		states=NULL;
		palette=NULL;
		data=NULL;
		data_len=0;
		bes=NULL;
		bes_ok=0;
	}else if(!secs){
		/*上次查的时候这个区块**还没有sections成员**：它可能后来才被建出来（merge的make_Section就是这样：
		   先补一段空section再回头读），缓存里那句"没有sections"就过时了。
		   不重看一眼的后果很严重：这一整个区块后面每次get_Block都返回"这段不存在"，方块被安安静静漏掉
		   （实测：merge报告"合并1个区块、0次改动、失败0个"、退出码0，新世界那块地一个方块都没搬）。
		   只在 secs==NULL 时多查一次hash表，正常路径（区块有sections）一点不受影响*/
		nbt_Any *s=c->get("sections");
		nbt_List *now=s&&s->type==0x09?(nbt_List*)s:NULL;
		if(now){
			secs=now;
			secs_len=now->con.size();
			this->sy=INT32_MIN;
			sec=NULL;
			states_sec=NULL;
			states=NULL;
			palette=NULL;
			data=NULL;
			data_len=0;
		}
	}
	if(this->sy==sy&&secs_len==(secs?secs->con.size():0)){/*同一段、列表也没变长：用记住的*/
		return sec;
	}
	secs_len=secs?secs->con.size():0;
	this->sy=sy;
	sec=NULL;
	for(size_t i=0;i<secs_len;i++){
		if(secs->con[i]->type!=0x0a){/*sections里的元素本该都是compound；不是就当没这段（别硬转过去读别人的内存）*/
			continue;
		}
		const nbt_Any *y=((const nbt_Compound*)secs->con[i])->get("Y");/*Y是带符号Byte*/
		if(y&&(int8_t)y->as_Int()==sy){
			sec=(nbt_Compound*)secs->con[i];
			break;
		}
	}
	return sec;
}

nbt_List *Chunk_Cursor::block_Entities(const Chunk *c){/*chunk的block_entities列表，游标里记一次就够了*/
	if(!bes_ok||!follows(c)){
		nbt_Any *s=c->get("block_entities");
		bes=s&&s->type==0x09?(nbt_List*)s:NULL;
		bes_ok=bes!=NULL;/*查不到就先不记：万一是"还没这个成员"，记下来会漏掉后来新建的*/
	}
	return bes;
}

/*---- Chunk：一个区块 ----*/
Chunk *Chunk::read(nbt_Raw *raw){/*一个区块就是一个根compound，直接建成Chunk；数据坏了返回NULL（跳过这一块）*/
	static uint64_t serial_cnt=0;
	raw->read_Root_Header();
	if(raw->fail){/*根tag就不对/数据是空的：read_Root_Header已经记过数了*/
		return NULL;
	}
	Chunk *chunk=new Chunk;
	chunk->serial=++serial_cnt;/*新号：游标靠它认区块，别让free掉又new出来的地址蒙混过关*/
	chunk->read_Members(raw);
	if(raw->fail){/*读到一半坏了：整块扔掉（半截区块比没有更危险），外面当"这个区块读不出来"*/
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
nbt_Any *Chunk::get_Section(int32_t sy)const{/*按section的Y找人，Y是带符号的Byte*/
	const nbt_Any *any=get("sections");
	const nbt_List *secs=any&&any->type==0x09?(const nbt_List*)any:NULL;/*不是list就当没有sections*/
	for(size_t i=0;secs&&i<secs->con.size();i++){
		if(secs->con[i]->type!=0x0a){/*元素本该都是compound：不是就跳过，别硬转*/
			continue;
		}
		const nbt_Any *y=((const nbt_Compound*)secs->con[i])->get("Y");
		if(y&&(int8_t)y->as_Int()==sy){
			return secs->con[i];
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
Block Chunk::get_Block(int x,int y,int z,Chunk_Cursor *cur)const{/*x,z相对0~15，y是世界绝对Y*/
	Block ret={NULL,NULL};
	if(x<0||x>15||z<0||z>15){
		return ret;
	}
	int sy=section_Y(y);/*y来自文件/调用者，用64位算，见nbt.hpp的section_Y*/
	int ly=y-sy*16;
	nbt_Compound *sec=cur?cur->find_Section(this,sy):(nbt_Compound*)get_Section(sy);
	if(sec){
		ret.block=cur?get_BlockData(sec,cur,x,ly,z):get_BlockData(sec,x,ly,z);
	}
	nbt_List *bes=cur?cur->block_Entities(this):(nbt_List*)get("block_entities");
	if(!bes||bes->type!=0x09||bes->con.empty()){/*没有方块实体就别查坐标了，绝大多数位置都走这里*/
		return ret;
	}
	const nbt_Any *xPos=get("xPos");
	const nbt_Any *zPos=get("zPos");
	if(xPos&&xPos->type==0x03&&zPos&&zPos->type==0x03){/*as_Int不认类型会返回0，所以先看是不是Int*/
		int32_t ax=(int32_t)((int64_t)xPos->as_Int()*16+x);/*走64位：xPos是坏数据时 int32 乘法是UB（UBSan会中止）*/
		int32_t ay=y;
		int32_t az=(int32_t)((int64_t)zPos->as_Int()*16+z);
		for(size_t i=0;i<bes->con.size();i++){
			if(bes->con[i]->type!=0x0a){
				continue;
			}
			const nbt_Compound *be=(const nbt_Compound*)bes->con[i];
			const nbt_Any *ex=be->get("x");
			const nbt_Any *ey=be->get("y");
			const nbt_Any *ez=be->get("z");
			if(ex&&ex->type==0x03&&ey&&ey->type==0x03&&ez&&ez->type==0x03&&
			   ex->as_Int()==ax&&ey->as_Int()==ay&&ez->as_Int()==az){
				ret.entity=bes->con[i];
				break;
			}
		}
	}
	return ret;
}
int Chunk::set_BlockEntity(nbt_Any *entity,int x,int y,int z){/*entity为NULL就删掉该位置的方块实体*/
	if(x<0||x>15||z<0||z>15){
		return 0;
	}
	const nbt_Any *xPos=get("xPos");
	const nbt_Any *zPos=get("zPos");
	if(!xPos||xPos->type!=0x03||!zPos||zPos->type!=0x03){/*xPos/zPos是Int，没有就没法算世界坐标*/
		return 0;
	}
	int32_t ax=(int32_t)((int64_t)xPos->as_Int()*16+x),ay=y,az=(int32_t)((int64_t)zPos->as_Int()*16+z);/*64位防溢出*/
	nbt_Any *ba=get("block_entities");
	/*必须是"元素为compound"的list（elem==0x0a）：elem是别的类型时（坏存档），往里push compound会写出
	  一个"表头说元素是Byte、内容却是compound"的怪list，本库读得回来，MC按elem解析整块区块会失败。
	  当成"没有这个成员"，下面会新建一个干净的*/
	nbt_List *bes=ba&&ba->type==0x09&&((nbt_List*)ba)->elem==0x0a?(nbt_List*)ba:NULL;
	int32_t find=-1;
	for(size_t i=0;bes&&i<bes->con.size();i++){
		if(bes->con[i]->type!=0x0a){/*元素本该都是compound*/
			continue;
		}
		const nbt_Compound *be=(const nbt_Compound*)bes->con[i];
		const nbt_Any *ex=be->get("x");
		const nbt_Any *ey=be->get("y");
		const nbt_Any *ez=be->get("z");
		if(ex&&ex->type==0x03&&ey&&ey->type==0x03&&ez&&ez->type==0x03&&
		   ex->as_Int()==ax&&ey->as_Int()==ay&&ez->as_Int()==az){
			find=(int32_t)i;
			break;
		}
	}
	if(entity){
		if(find>=0&&bes->con[find]==entity){/*给的就是原来那个：别free了再copy，自己拷自己会读已释放的内存*/
			return 1;
		}
		if(find>=0){/*同一位置已有就换掉*/
			bes->con[find]->free();
			bes->con[find]=entity->copy();
		}else{
			if(!bes){
				bes=new nbt_List(0x0a);
				set("block_entities",bes);
			}
			bes->push_back(entity->copy());
		}
	}else if(find>=0){
		bes->erase((size_t)find);
	}
	return 1;
}
int Chunk::set_Block(Block block,int x,int y,int z,Chunk_Cursor *cur){/*x,z相对0~15，y是世界绝对Y，和get_Block一致*/
	if(!block.block||x<0||x>15||z<0||z>15){
		return 0;
	}
	int sy=section_Y(y);/*y来自文件/调用者，用64位算，见nbt.hpp的section_Y*/
	nbt_Compound *sec=cur?cur->find_Section(this,sy):(nbt_Compound*)get_Section(sy);
	if(!sec){
		return 0;
	}
	int ret=cur?set_BlockData(sec,block.block,x,y-sy*16,z,cur):set_BlockData(sec,block.block,x,y-sy*16,z);
	if(ret){
		set_BlockEntity(block.entity,x,y,z);
		if(cur&&block.entity){/*可能刚建出block_entities，缓存作废*/
			cur->bes_ok=0;
		}
	}
	return ret;
}

/*==================== POI：poi/目录里的方块索引 ====================*/
/*表：方块basename→POI类型+票上限。票上限照wiki Point of Interest：
  村民能认领的（床/工作站）1张、钟32张；不参与认领的（传送门/蜂巢/避雷针/磁石）恒0张。
  26.1.1 的POI类型全在这张表里；以后版本新加的类型这里没有＝认不出＝那条记录会被删掉（见ReadMeDeveloper）*/
struct poi_Def{const char *block;const char *type;int32_t tickets;};
static const poi_Def poi_Defs[]={
	{"bell","minecraft:meeting",32},
	{"composter","minecraft:farmer",1},
	{"barrel","minecraft:fisherman",1},
	{"smoker","minecraft:butcher",1},
	{"blast_furnace","minecraft:armorer",1},
	{"cartography_table","minecraft:cartographer",1},
	{"brewing_stand","minecraft:cleric",1},
	{"lectern","minecraft:librarian",1},
	{"stonecutter","minecraft:mason",1},
	{"loom","minecraft:shepherd",1},
	{"smithing_table","minecraft:toolsmith",1},
	{"grindstone","minecraft:weaponsmith",1},
	{"fletching_table","minecraft:fletcher",1},
	{"cauldron","minecraft:leatherworker",1},
	{"water_cauldron","minecraft:leatherworker",1},
	{"lava_cauldron","minecraft:leatherworker",1},
	{"powder_snow_cauldron","minecraft:leatherworker",1},
	{"bee_nest","minecraft:bee_nest",0},
	{"beehive","minecraft:beehive",0},
	{"lodestone","minecraft:lodestone",0},
	{"lightning_rod","minecraft:lightning_rod",0},
	{"exposed_lightning_rod","minecraft:lightning_rod",0},
	{"weathered_lightning_rod","minecraft:lightning_rod",0},
	{"oxidized_lightning_rod","minecraft:lightning_rod",0},
	{"nether_portal","minecraft:nether_portal",0},
	{"test_instance_block","minecraft:test_instance",0},/*开发用方块，wiki表里有，一起认了免得白删记录*/
};
static int name_Is_Air(const char *name){/*空气：air/cave_air/void_air都没有方块。
	（section_Const里只认minecraft:air，那是"整段能不能跳"的优化，把洞穴空气当实的更保险；
	 这里是"这段里到底有没有东西"，三种空气都得算空）*/
	return name&&(!strcmp(name,"minecraft:air")||!strcmp(name,"minecraft:cave_air")||!strcmp(name,"minecraft:void_air"));
}
int chunk_No_Block(const Chunk *c){/*这个区块里一个方块都没有吗？*/
	const nbt_Any *sc=c?c->get("sections"):NULL;
	if(!sc||sc->type!=0x09){
		return 1;/*连sections都没有：一个方块都没有*/
	}
	const nbt_List *secs=(const nbt_List*)sc;
	for(size_t i=0;i<secs->con.size();i++){
		const nbt_Any *s=secs->con[i];
		if(!s||s->type!=0x0a){
			continue;/*不是compound的段：跳过（当没有）*/
		}
		const nbt_Any *bs=((const nbt_Compound*)s)->get("block_states");
		if(!bs||bs->type!=0x0a){
			continue;/*没有block_states（starlight那种只有光的段）＝空气*/
		}
		const nbt_Any *pany=((const nbt_Compound*)bs)->get("palette");
		if(!pany||pany->type!=0x09){
			continue;/*没有调色板：读不出来，当空气（真出问题时外面还有坏数据计数）*/
		}
		const nbt_List *pal=(const nbt_List*)pany;
		size_t n=pal->con.size();
		if(!n){
			continue;
		}
		if(n>256){/*direct调色板（超过256种状态）：这种区块一定是生成过的，别判它空*/
			return 0;
		}
		bool has_solid=false;
		for(size_t j=0;j<n;j++){
			if(!name_Is_Air(get_Name(pal->con[j]))){
				has_solid=true;break;
			}
		}
		if(!has_solid){
			continue;/*整段只有空气*/
		}
		const nbt_Any *dany=((const nbt_Compound*)bs)->get("data");
		if(!dany||dany->type!=0x0c){/*没有data：整段都是palette[0]*/
			if(!name_Is_Air(get_Name(pal->con[0]))){
				return 0;
			}
			continue;
		}
		const nbt_Long_Array *d=(const nbt_Long_Array*)dany;
		/*先扫data里有没有非0的long：全0＝每一格都是palette[0]（空壳区块/整段同一种方块最常见），
		  这样末地那种几十万个虚空区块不用一格格翻，快得多*/
		{
			int any=0;
			for(size_t j=0;j<d->con.size();j++){
				if(d->con[j]){
					any=1;
					break;
				}
			}
			if(!any){
				if(!name_Is_Air(get_Name(pal->con[0]))){
					return 0;
				}
				continue;
			}
		}
		uint32_t bits=4;/*wiki: Java Edition 区块格式 1.16+：bits=max(4,ceil(log2(调色板长度)))，每long从低位往高位数格*/
		while((1u<<bits)<n){
			bits++;
		}
		uint32_t per=64/bits;
		uint64_t mask=(1ull<<bits)-1;
		for(uint32_t k=0;k<4096;k++){/*位置顺序是 y*256+z*16+x，这里只关心"有没有非空气"，顺序无所谓*/
			size_t w=k/per;
			if(w>=d->con.size()){
				return 0;/*data不够长：形状不对，宁可信其有（当它有方块）*/
			}
			uint32_t idx=(uint32_t)((d->con[w]>>((k%per)*bits))&mask);
			if(idx<n&&!name_Is_Air(get_Name(pal->con[idx]))){
				return 0;
			}
		}
	}
	return 1;
}
int chunk_Full_Status(const Chunk *c){/*这个区块生成完了吗*/
	const nbt_Any *st=c?c->get("Status"):NULL;
	if(!st||st->type!=0x08){
		return -1;/*没有Status字段（老存档/别的工具写的）：不知道，调用方自己定规矩*/
	}
	return !strcmp(st->as_Str(),"minecraft:full");
}
static int ends_With(const char *s,const char *suf){/*s以suf结尾？*/
	size_t a=strlen(s),b=strlen(suf);
	return a>=b&&!strcmp(s+a-b,suf);
}
static int poi_Name_Ok(const nbt_Any *block){/*只认minecraft命名空间（没冒号也算），mod方块不猜*/
	const char *nm=get_Name(block);
	if(!nm||!nm[0]){
		return 0;
	}
	const char *c=strchr(nm,':');
	return !c||((size_t)(c-nm)==9&&!strncmp(nm,"minecraft",9));
}
const char *poi_Type_Of_Block(const nbt_Any *block){
	if(!block||block->type!=0x0a||!poi_Name_Ok(block)){
		return NULL;
	}
	const char *base=get_Basename(block);
	if(!base||!base[0]){
		return NULL;
	}
	if(ends_With(base,"_bed")){/*床只有head那半有记录（wiki Point of Interest注1："The coordinates for the head of the bed are saved"）*/
		return get_Prop_Is(block,"part","head")?"minecraft:home":NULL;
	}
	for(size_t i=0;i<sizeof(poi_Defs)/sizeof(poi_Defs[0]);i++){
		if(!strcmp(base,poi_Defs[i].block)){
			return poi_Defs[i].type;
		}
	}
	return NULL;/*不是POI方块，或者认不出（表里没有）*/
}
int32_t poi_Ticket_Max(const char *type){
	if(!type){
		return 0;
	}
	if(!strcmp(type,"minecraft:home")){/*床不在下面的表里（方块按*_bed后缀认），票上限和村民工作站一样是1*/
		return 1;
	}
	for(size_t i=0;i<sizeof(poi_Defs)/sizeof(poi_Defs[0]);i++){
		if(!strcmp(type,poi_Defs[i].type)){
			return poi_Defs[i].tickets;
		}
	}
	return 0;/*不认识的类型当"不能认领"（正常走不到：写记录的类型都从方块来的）*/
}
static nbt_Compound *poi_Sections(const nbt_Compound *chunk){/*Sections那个compound；没有/类型不对返回NULL*/
	if(!chunk){
		return NULL;
	}
	const nbt_Any *s=chunk->get("Sections");
	return s&&s->type==0x0a?(nbt_Compound*)s:NULL;
}
static int poi_Key_Ok(const std::string &key){/*段键＝段Y的十进制字符串（wiki: "stringified chunk section's Y"）*/
	if(key.empty()||key.size()>11){
		return 0;
	}
	size_t i=(key[0]=='-')?1:0;
	if(i>=key.size()){
		return 0;
	}
	for(;i<key.size();i++){
		if(key[i]<'0'||key[i]>'9'){
			return 0;
		}
	}
	return 1;
}
int poi_Shape_Ok(const nbt_Compound *chunk){/*形状：Sections是compound（有的话）、段键是整数、每段是compound、Records是list（缺Records当空）。
												形状不对＝这一块读不出来，调用者整块跳过（别当"这块没记录"——那会把里面的记录默默丢掉或把对方的记录抹了）*/
	if(!chunk||chunk->type!=0x0a){
		return 0;
	}
	const nbt_Any *sa=chunk->get("Sections");
	if(sa&&sa->type!=0x0a){
		return 0;
	}
	const nbt_Compound *secs=sa?(const nbt_Compound*)sa:NULL;
	for(std::unordered_map<std::string,nbt_Any*>::const_iterator it=secs?secs->con.begin():std::unordered_map<std::string,nbt_Any*>::const_iterator();secs&&it!=secs->con.end();++it){
		if(!poi_Key_Ok(it->first)){
			return 0;
		}
		const nbt_Any *s=it->second;
		if(!s||s->type!=0x0a){
			return 0;
		}
		const nbt_Any *r=((const nbt_Compound*)s)->get("Records");
		if(r&&r->type!=0x09){
			return 0;
		}
	}
	return 1;
}
static int poi_Section_Recs(const nbt_Compound *sec,const nbt_List **out){/*一段里的Records列表；没有/类型不对给NULL*/
	if(!sec||sec->type!=0x0a){/*NULL也当"不是compound"：con里理论上不会有NULL，但不靠这个假设*/
		return 0;
	}
	const nbt_Any *r=sec->get("Records");
	if(!r){
		*out=NULL;
		return 1;/*没写Records＝这段没记录*/
	}
	if(r->type!=0x09){
		return 0;
	}
	*out=(const nbt_List*)r;
	return 1;
}
int poi_Count(const nbt_Compound *chunk){
	const nbt_Compound *secs=poi_Sections(chunk);
	int n=0;
	for(std::unordered_map<std::string,nbt_Any*>::const_iterator it=secs?secs->con.begin():std::unordered_map<std::string,nbt_Any*>::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!poi_Section_Recs((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		n+=(int)(recs?recs->con.size():0);
	}
	return n;
}
int poi_Get_At(const nbt_Compound *chunk,int idx,int32_t *x,int32_t *y,int32_t *z,const char **type,int32_t *free_tickets){
	const nbt_Compound *secs=poi_Sections(chunk);
	int n=0;
	for(std::unordered_map<std::string,nbt_Any*>::const_iterator it=secs?secs->con.begin():std::unordered_map<std::string,nbt_Any*>::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!poi_Section_Recs((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		for(size_t i=0;recs&&i<recs->con.size();i++){
			if(n++!=idx){
				continue;
			}
			const nbt_Any *e=recs->con[i];
			if(!e||e->type!=0x0a){/*形状不对：这一条读不出来（计一笔，调用者跳过它）*/
				bad_Poi("record not compound",idx,0,0);
				return 0;
			}
			const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
			const nbt_Any *t=((const nbt_Compound*)e)->get("type");
			const nbt_Any *f=((const nbt_Compound*)e)->get("free_tickets");
			if(!p||p->type!=0x0b||((const nbt_Int_Array*)p)->con.size()!=3||!t||t->type!=0x08||!f||f->type!=0x03){
				bad_Poi("record fields",idx,0,0);/*pos/type/free_tickets不对*/
				return 0;
			}
			*x=(int32_t)((const nbt_Int_Array*)p)->con[0];
			*y=(int32_t)((const nbt_Int_Array*)p)->con[1];
			*z=(int32_t)((const nbt_Int_Array*)p)->con[2];
			*type=t->as_Str();
			*free_tickets=(int32_t)f->as_Int();
			return 1;
		}
	}
	return 0;
}
const nbt_Any *poi_Get(const nbt_Compound *chunk,int32_t x,int32_t y,int32_t z){
	const nbt_Compound *secs=poi_Sections(chunk);
	for(std::unordered_map<std::string,nbt_Any*>::const_iterator it=secs?secs->con.begin():std::unordered_map<std::string,nbt_Any*>::const_iterator();secs&&it!=secs->con.end();++it){
		const nbt_List *recs=NULL;
		if(!poi_Section_Recs((const nbt_Compound*)it->second,&recs)){
			continue;
		}
		for(size_t i=0;recs&&i<recs->con.size();i++){
			const nbt_Any *e=recs->con[i];
			if(!e||e->type!=0x0a){
				continue;
			}
			const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
			if(!p||p->type!=0x0b||((const nbt_Int_Array*)p)->con.size()!=3){
				continue;
			}
			if((int32_t)((const nbt_Int_Array*)p)->con[0]==x&&(int32_t)((const nbt_Int_Array*)p)->con[1]==y&&(int32_t)((const nbt_Int_Array*)p)->con[2]==z){
				return e;
			}
		}
	}
	return NULL;
}
static nbt_Compound *poi_Section(nbt_Compound *chunk,int32_t sy){/*拿这一段的compound，没有就建（Sections类型不对也重建）*/
	char key[24];
	snprintf(key,sizeof(key),"%d",sy);/*段键是段Y的十进制字符串（可以是负数）*/
	nbt_Any *sa=chunk->get("Sections");
	nbt_Compound *secs=sa&&sa->type==0x0a?(nbt_Compound*)sa:NULL;
	if(!secs){
		secs=new nbt_Compound;
		chunk->set("Sections",secs);
	}
	nbt_Any *s=secs->get(key);
	nbt_Compound *sec=s&&s->type==0x0a?(nbt_Compound*)s:NULL;
	if(!sec){
		sec=new nbt_Compound;
		secs->set(key,sec);
	}
	nbt_Any *ra=sec->get("Records");
	if(!ra||ra->type!=0x09||((nbt_List*)ra)->elem!=0x0a){/*元素必须是compound：elem是别的类型时，
		往里push compound会写出"表头说Byte、内容是compound"的怪list（本库读得回来，MC读不了整块）*/
		sec->set("Records",new nbt_List(0x0a));
	}
	sec->set("Valid",new nbt_Byte(1));/*我们自己算出来的记录＝算过了（Valid=1）；不写0去让游戏重算*/
	return sec;
}
int poi_Set(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z,const char *type,int32_t free_tickets){
	if(!chunk||!type||!type[0]){
		return 0;
	}
	int32_t sy=section_Y(y);
	nbt_Compound *sec=poi_Section(chunk,sy);
	nbt_List *recs=(nbt_List*)sec->get("Records");
	nbt_Compound *rec=new nbt_Compound;
	rec->set_String("type",type);
	nbt_Int_Array *pos=new nbt_Int_Array;
	pos->push_back((uint32_t)x);pos->push_back((uint32_t)y);pos->push_back((uint32_t)z);
	rec->set("pos",pos);
	rec->set_Int("free_tickets",free_tickets);
	for(size_t i=0;i<recs->con.size();i++){
		const nbt_Any *e=recs->con[i];
		if(!e||e->type!=0x0a){
			continue;
		}
		const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
		if(!p||p->type!=0x0b||((const nbt_Int_Array*)p)->con.size()!=3){
			continue;
		}
		if((int32_t)((const nbt_Int_Array*)p)->con[0]==x&&(int32_t)((const nbt_Int_Array*)p)->con[1]==y&&(int32_t)((const nbt_Int_Array*)p)->con[2]==z){
			recs->con[i]->free();/*同一位置已有：换掉*/
			recs->con[i]=rec;
			return 1;
		}
	}
	recs->push_back(rec);
	return 1;
}
int poi_Del(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z){
	nbt_Compound *secs=(nbt_Compound*)poi_Sections(chunk);
	int del=0;
	for(std::unordered_map<std::string,nbt_Any*>::iterator it=secs?secs->con.begin():std::unordered_map<std::string,nbt_Any*>::iterator();secs&&it!=secs->con.end();++it){
		nbt_Any *s=it->second;
		if(!s||s->type!=0x0a){
			continue;
		}
		nbt_Any *ra=((nbt_Compound*)s)->get("Records");
		nbt_List *recs=ra&&ra->type==0x09?(nbt_List*)ra:NULL;
		for(size_t i=0;recs&&i<recs->con.size();){
			const nbt_Any *e=recs->con[i];
			const nbt_Any *p=e&&e->type==0x0a?((const nbt_Compound*)e)->get("pos"):NULL;
			if(p&&p->type==0x0b&&((const nbt_Int_Array*)p)->con.size()==3&&
			   (int32_t)((const nbt_Int_Array*)p)->con[0]==x&&(int32_t)((const nbt_Int_Array*)p)->con[1]==y&&
			   (int32_t)((const nbt_Int_Array*)p)->con[2]==z){
				recs->erase(i);
				del++;
				continue;
			}
			i++;
		}
	}
	return del;
}
int poi_Clean(nbt_Compound *chunk){/*Records空的段直接删掉：记录集合＝段键集合（原版新生成的区块就是这样）*/
	nbt_Compound *secs=(nbt_Compound*)poi_Sections(chunk);
	if(!secs){
		return 0;
	}
	std::vector<std::string> drop;
	for(std::unordered_map<std::string,nbt_Any*>::iterator it=secs->con.begin();it!=secs->con.end();++it){
		nbt_Any *s=it->second;
		const nbt_List *recs=NULL;
		if(!s||s->type!=0x0a||!poi_Section_Recs((const nbt_Compound*)s,&recs)||!recs||recs->con.empty()){
			drop.push_back(it->first);
		}
	}
	for(size_t i=0;i<drop.size();i++){
		secs->erase(drop[i]);
	}
	return (int)drop.size();
}

/*---- 实体区块：形状检查 ----*/
int entity_Check(const nbt_Compound *chunk,int32_t cx,int32_t cz){
	if(!chunk||chunk->type!=0x0a){
		bad_Entity("not a compound",cx,cz);
		return 0;
	}
	const nbt_Any *pos=chunk->get("Position");
	if(!pos||pos->type!=0x0b||((const nbt_Int_Array*)pos)->con.size()!=2){
		bad_Entity("no/bad Position",cx,cz);
		return 0;
	}
	if((int32_t)((const nbt_Int_Array*)pos)->con[0]!=cx||(int32_t)((const nbt_Int_Array*)pos)->con[1]!=cz){
		bad_Entity("Position != slot",cx,cz);/*写错槽位会让实体散到别的区块里，宁可跳过*/
		return 0;
	}
	const nbt_Any *dv=chunk->get("DataVersion");
	if(!dv||dv->type!=0x03){
		bad_Entity("no DataVersion",cx,cz);/*没有版本号游戏会从最老的开始升级，别冒这个险*/
		return 0;
	}
	const nbt_Any *es=chunk->get("Entities");
	if(!es||es->type!=0x09){
		bad_Entity("no/bad Entities",cx,cz);
		return 0;
	}
	return 1;
}
