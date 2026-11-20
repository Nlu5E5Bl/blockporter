#include "nbtrw.hpp"
#include "rules.hpp"
#include "blockporter.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
/*fixer.cpp：修一个存档的非法方块/非法数据（规则从规则插件来）
 *
 *	总的规矩（用户 2026-11-07）：以游戏为准——游戏读档会报 ERROR/WARN 的按最小单位删，不报的不删不改
 *	（判据逐条见 ReadMeDeveloper 1.10）。顺序：
 *	  先删 0 字节 region（游戏当它是空区域），后面“region 里没有这个区块”的检查才看得见真实情况。
 *	  0. 修 NBT（repair_Chunks，在物理之前，规则看到的是修好的区块）：游戏读不进来的（GET_BAD_CHUNK：解压/NBT 坏）
 *	     整块删并记入 deleted；xPos/zPos 按文件位置重写；读不了的 tag、解不出来的一段 block_states/biomes、
 *	     认不出名字的调色板条目、坏刻表/坏单条刻、坏方块实体、坏结构引用按最小单位删/换空气；
 *	     方块实体、计划刻的坐标指到别的区块的，迁到那个区块里去（migrate_Strays）。
 *	  1. 物理：按规则插件逐格判，撑不住的拆/改；fixer 只把这一格和六个面交给插件；写方块先删这一格的旧刻，
 *	     再补插件给的那条（26.1.1：含水方块/霜冰写水源、补 fluid_ticks t=5 p=0）。不写 InhabitedTime。
 *	  2. 区块清理：生成状态不是 full 的删；全空气、或 InhabitedTime 是 0（getLongOr：没有/不是数值都当0）的删；
 *	     修 NBT 没修掉的整块载入失败兜底删。
 *	  3. 光照/高度：范围碰到的区块删 Heightmaps、BlockLight/SkyLight，isLightOn 置0；范围外的不动。
 *	  4. 计划刻：先删游戏读不进来的单条，再删指向方块/类型对不上的；同一位置还有重复
 *	     合法记录时只留 p 最小、p同t最大的一条。
 *	  5. POI：解不出来的段删那一段；按 rules.get_POIInfo 删单条坏记录/错类型/越界票/重复，补缺失，重复保留
 *	     free_tickets 最小；范围外保留；新建的 POI 根写 DataVersion。region 里没有的区块同格 POI 删掉。
 *	  6. 实体：Position 坏/对不上按文件位置重写；单条坏记录（乘客也一个个判）只删这一条；UUID 冲突删后出现的那一个；
 *	     没有 UUID/Pos 的照留（游戏随机给 UUID、放原点）。region 里没有的区块同格实体删掉。不新建 region。
 *	  7. 落盘后整理文件（format_Regions），范围碰到的 region/poi/entities 文件各一遍：0字节文件删；表头上偏移异常的格子清成 0；
 *	     一个区块都没有的删文件；region 文件里选中的区块在这里做 format_Chunk（Y 改成 Byte、重复 Y 只留最后一段、压紧废弃调色板，
 *	     整趟只做这一遍）；最后 MCA::format 压紧。前面任一次 save/close 失败就不做这一步，避免三方不一致。
 *	版本（用户 2026-11-07：fixer 只要求“皮”支持多版本）：区块 DataVersion 不认识（1451 以前、没写）的修NBT/清理/光照/
 *	  计划刻/POI 都不碰；
 *	  DataVersion 和规则不一样的（is_RulesChunk）物理不判、计划刻不拿方块对、POI 不按方块补删，修NBT/清理/光照照做；
 *	  POI 文件的 DataVersion（没有当 1945）和规则的不一样整块不动。
 *	终端是 tty 时各阶段在 stderr 画进度条。
 *
 *	范围（用户 2026-11-14：去掉 chunkunit，每种操作都按自己的最小单位、向外取整到那个单位，范围外只读不写）：
 *	  · 方块级（物理、计划刻/POI/实体的单条记录、迁移）：严格按给定的 x/y/z；
 *	  · 区块级（修NBT、删区块、删光照/高度、format_Chunk）：x/z 碰到的区块整块做，y 不管；
 *	  · 区域文件级（清偏移异常的格子、删空文件、压紧）：x/z 碰到的区域文件整个做。
 *	  范围外一律只读不写，没有例外（以前 3×3 光照邻居是例外，那是给 chunkunit 定的，2026-11-21 去掉）。维度用逗号隔开，默认 minecraft:overworld。
 *	遍历（用户 2026-11-14）：列出目录里实际存在的区域文件再和范围求交（不逐个区域坐标去试），region 文件读不出来的整个区域跳过；每个区域里走表头的 1024 格，
 *	  不存在、偏移异常的区块跳过（偏移异常的格子游戏当没有，收尾整理时清成 0）。
 *
 *	区块缓存全部在 Level 里：处理一个区块时用 pin_Chunk 钉住当前 3x3，用完 unpin；
 *	fixer 自己不保留跨区块的区块副本。
 *
 *	只改区块里的 xPos/zPos/block_entities/block_ticks/fluid_ticks/Heightmaps/isLightOn/structures（坏引用/起点）/
 *	sections（改Y/删重复Y/删光照/删坏 block_states、biomes）/block_states/BlockLight/SkyLight，外加读不了的 tag；
 *	不写 InhabitedTime。POI/实体在各自目录里。
 */

/*区块的字段在 body 那一层（2842 以前在根的 Level 里），键名按区块的 DataVersion（Chunk_Layout）*/
static const nbt_Any *get_ChunkField(const Chunk *c,const char *key){
	const nbt_Compound *b=c?get_ChunkBody(&c->con):NULL;
	return b?b->get(key):NULL;
}
static const nbt_Any *get_ChunkSections(const Chunk *c){/*区块的 sections 成员（任何类型；没有给 NULL），键名按区块的 DataVersion*/
	return c?get_ChunkField(c,get_ChunkLayout(&c->con).sections):NULL;
}
static nbt_Any *get_ChunkSections(Chunk *c){/*同上，可写的版本*/
	return (nbt_Any*)get_ChunkSections((const Chunk*)c);
}
/*MCA 读出来的是值（optional）；下面 POI/实体那几段按“堆上的树 + 用完 free()”写，先放到堆上*/
static nbt_Compound *read_MCACompound(const MCA &m,uint32_t x,uint32_t z){
	std::optional<nbt_Compound> r=m.get_Compound(x,z);
	return r?new nbt_Compound(std::move(*r)):NULL;
}
static void print_Usage(const char *me){/*参数不够时打的用法*/
	printf("用法：%s <path> <x1> <y1> <z1> <x2> <y2> <z2> [dims] [rules]\n",me);
	printf("  path       存档目录（会被改写！）\n");
	printf("  x1 y1 z1 x2 y2 z2  方块范围的两个角（谁大谁小都行）。方块级的操作严格按它；\n");
	printf("             区块级的（删区块、删光照…）按 x/z 碰到的区块，文件级的（整理）按碰到的区域文件\n");
	printf("  dims       逗号隔开的维度名，默认 minecraft:overworld\n");
	printf("  rules      规则插件的路径（rules.so.<版本>），默认 ./rules.so；要给它就得先给 dims\n");
}

static bool is_ChunkUnreadable(int why){/*读不出来但“不知道对不对”的原因：保留记录、跳过，不按坏块删*/
	return why==Level::GET_BAD_FILE||why==Level::GET_BAD_CHUNK||why==Level::GET_UNSUPPORTED_COMPRESSION;
}
static uint64_t get_ChunkKey(int32_t cx,int32_t cz){/*区块坐标拼成一个键（deleted 集合用）*/
	return ((uint64_t)(uint32_t)cx<<32)|(uint32_t)cz;
}
struct Fix_Region{int32_t rx,rz;};/*一个 region 文件在的区域*/

/*==================== 规则插件 ====================*/
static const Rules_Api *load_Rules(const std::string &path){/*dlopen 规则插件、找入口、核对 ABI；装不上打一行原因给 NULL（句柄不关：插件要用到进程结束）*/
	std::string file=path.find('/')==std::string::npos?"./"+path:path;/*不带 '/' 的名字 dlopen 会去系统库目录里找，不找当前目录*/
	void *h=dlopen(file.c_str(),RTLD_NOW|RTLD_LOCAL);
	if(!h){
		printf("fixer: 装不上规则插件 %s：%s\n",path.c_str(),dlerror());
		return NULL;
	}
	void *sym=dlsym(h,RULES_ENTRY);
	if(!sym){
		printf("fixer: 插件里没有 %s\n",RULES_ENTRY);
		return NULL;
	}
	Rules_Entry_Fn fn=(Rules_Entry_Fn)sym;
	const Rules_Api *api=fn(RULES_ABI,(uint32_t)sizeof(Rules_Api));
	if(!api){
		printf("fixer: 规则插件接口对不上（fixer abi=0x%08x size=%u）\n",RULES_ABI,(unsigned)sizeof(Rules_Api));
		return NULL;
	}
	if(api->abi!=RULES_ABI||api->size!=sizeof(Rules_Api)){
		printf("fixer: 规则插件头文件版本对不上\n");
		return NULL;
	}
	return api;
}

/*==================== 一段section：调色板 + 每个条目的规则 ====================*/
struct Fix_Sec{
	nbt_Compound *box=NULL;/*方块状态容器：新格式是 block_states，老格式是段本身；NULL＝这一段空气（缺段/纯光段/老格式少一样）*/
	nbt_List *palette=NULL;
	nbt_Long_Array *data=NULL;
	uint32_t bits=0,per=0,words=0,need=0;
	bool new_states=true,span=false;/*区块格式（Chunk_Layout）：2832 以前调色板在段里、2527 以前下标跨 long*/
	bool bad=false;/*库不读这一段（Chunk::states_by_y<0）：游戏读不了，或者读得了但不是库能原样读写的样子*/
	States_Kind kind=STATES_AIR;/*游戏眼里这一段（get_StatesView）：bad 时分得清“游戏当空气”和“不知道是什么”*/
	uint32_t n_game=0;/*游戏读的时候调色板有几条（老格式升级时可能补了空气）*/
	State_Format fmt=STATE_NAME;/*所在区块的方块状态编码（按区块DataVersion）*/
	std::vector<int16_t> rule;
	std::vector<const Poi_Info*> poi;/*和info对齐：这条调色板条目产生什么POI，NULL=不产生*/
	std::deque<Blk_Info> info;
	int cand=0;
	int poi_cand=0;
};
static void calc_SecBits(Fix_Sec &s){/*按调色板条数算这一段下标的位宽、每个 long 放几个、data 该有几个 long*/
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	s.bits=get_PaletteBitsFor(len,s.new_states);
	s.per=s.bits?64/s.bits:0;
	s.words=s.data?(uint32_t)s.data->con.size():0;
	s.need=get_PackedWords(4096,s.bits,s.span);
}
static void add_SecInfo(Fix_Sec &s,const Rules_Api *api,uint32_t mask){/*让 info/rule/poi 三张表和调色板一样长：调色板长了（写方块时加了条目）只给新加的条目问插件，旧的不重算*/
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	if(s.info.size()>(size_t)len){
		s.info.resize(len);
		s.rule.resize(len);
		s.poi.resize(len);
	}
	size_t old=s.info.size();
	if(old>=(size_t)len){
		return;
	}
	s.info.resize(len);
	s.rule.resize(len);
	s.poi.resize(len);
	for(size_t i=old;i<(size_t)len;i++){
		Blk_Info &o=s.info[i];
		const nbt_Any *e=s.palette->con[i];
		o.blk=e;
		o.name=get_StateName(e,s.fmt);/*认不出来是空串：插件当它不认识，不管*/
		o.props=get_StateProps(e,s.fmt);
		api->fill_BlkInfo(&o);
		s.rule[i]=(int16_t)api->get_BlkRule(&s.info[i],mask);
		if(s.rule[i]>=0){
			s.cand++;
		}
		s.poi[i]=api->get_POIInfo?api->get_POIInfo(&s.info[i]):NULL;
		if(s.poi[i]){
			s.poi_cand++;
		}
	}
}
/*第sy段的方块状态（palette/data 走区块的索引，格式跟着区块的 DataVersion）：
  新格式 block_states 不是 compound＝坏；2832 以前 Palette/BlockStates 类型不对＝坏、少一样＝这一段空气（游戏不建这一段）*/
static void read_SecStates(Fix_Sec &s,const Chunk *c,int32_t sy){/*照库的段索引（states_by_y）：和 get_Block 读到的一样*/
	Chunk_Layout L=get_ChunkLayout(&c->con);
	s.new_states=L.states;
	s.span=L.span;
	s.bad=false;
	s.kind=STATES_AIR;
	s.n_game=0;
	s.palette=NULL;
	s.data=NULL;
	s.box=NULL;
	nbt_Compound *sec=c->get_Section(sy);
	if(!sec){
		return;
	}
	int32_t g=c->states_by_y[(uint8_t)sy];
	if(g<0){
		s.bad=true;
		s.kind=get_StatesView(sec,L,false).kind;
		return;
	}
	if(!g){
		return;/*没有方块状态：空气*/
	}
	s.kind=STATES_OK;
	s.n_game=(uint32_t)g;
	s.box=get_SectionStates(sec,L);
	s.palette=c->palette_by_y[(uint8_t)sy];
	s.data=c->data_by_y[(uint8_t)sy];
}
static void build_Sec(Fix_Sec &s,const Chunk *c,int32_t sy,const Rules_Api *api,uint32_t mask){/*从头建第 sy 段的规则表：读方块状态、每条调色板条目问一遍插件*/
	s.fmt=get_ChunkStateFormat(&c->con);
	read_SecStates(s,c,sy);
	s.info.clear();
	s.rule.clear();
	s.poi.clear();
	s.cand=0;
	s.poi_cand=0;
	if(s.bad||!s.box){
		s.palette=NULL;
		s.data=NULL;
		return;
	}
	if(!s.palette||s.palette->con.empty()){
		s.palette=NULL;
		s.data=NULL;
		return;
	}
	calc_SecBits(s);
	add_SecInfo(s,api,mask);
}
static void refresh_Sec(Fix_Sec &s,const Chunk *c,int32_t sy,const Rules_Api *api,uint32_t mask){/*s.fmt沿用build_Sec时的*/
	read_SecStates(s,c,sy);
	if(s.bad||!s.box||!s.palette||s.palette->con.empty()){
		s.palette=NULL;
		s.data=NULL;
		s.bits=0;
		s.per=0;
		s.words=0;
		s.need=0;
		if(s.bad){
			s.info.clear();
			s.rule.clear();
		}
		s.poi.clear();
		s.cand=0;
		s.poi_cand=0;
		return;
	}
	calc_SecBits(s);
	add_SecInfo(s,api,mask);
}
#define IDX_BAD 0xFFFFFFFFu
#define IDX_AIR 0xFFFFFFFEu/*老格式下标落在游戏升级时补的空气上*/
static inline uint32_t get_SecIndex(const Fix_Sec &s,uint32_t i){
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	if(len==1){
		return 0;/*单条调色板：游戏不看 data（老格式升级时扔掉 data）*/
	}
	if(!s.bits||!s.data||!s.per||s.words!=s.need){
		return IDX_BAD;/*库的段索引保证长度正好；这里只是兜底*/
	}
	uint32_t idx;
	if(s.span){
		idx=get_PackedIndex(s.data->con,i,s.bits,true);
	}else{
		idx=(uint32_t)((uint64_t)s.data->con[i/s.per]>>((i%s.per)*s.bits))&((uint32_t)(((uint64_t)1<<s.bits)-1));
	}
	if(idx>=len){
		return idx<s.n_game?IDX_AIR:IDX_BAD;
	}
	return idx;
}
static Blk_Info info_Air;/*空气占位（缺段/纯光段/范围外当空气的邻居）：装好插件后让插件填一次，别的时候只读*/
static void init_AirInfo(const Rules_Api *api){
	info_Air=Blk_Info();
	info_Air.name="minecraft:air";
	api->fill_BlkInfo(&info_Air);
	info_Air.flags|=BF_AIR;/*主机自己的约定：占位一定当空气*/
}

/*==================== 物理判定 ====================*/
struct Fix_Stat{/*物理阶段的统计（最后打“物理：”那一行，各字段见 ReadMeDeveloper 5.）*/
	int64_t chunks=0,sec_all=0,sec_hit=0,pos=0,hit=0,air=0,wet=0,set=0,miss=0,blind=0,noop=0;
	int64_t pos_bad=0,sec_bad=0,tick_add=0,tick_del=0,tick_fail=0,flush_fail=0;
	int64_t ver_skip=0;/*区块的 DataVersion 和规则的不一样：规则不判、不改*/
	std::vector<int64_t> rule_hit,rule_change;
	std::unordered_map<std::string,int64_t> base_change;
	void init(int n){
		rule_hit.assign((size_t)(n>0?n:1),0);
		rule_change.assign((size_t)(n>0?n:1),0);
	}
};
/*规则（方块名字、状态、POI/计划刻对不对得上）是按某一个版本做的：区块的 DataVersion 和规则的一样才拿规则判
  （存档里没升级的老区块名字可能不一样，比如 1.21.9 把 chain 改成 iron_chain）；不一样的规则当它读不出来，不改*/
static bool is_RulesChunk(const Chunk *c,const Rules_Api *api){
	return c&&api&&get_ChunkDataVersion(&c->con)==api->minecraft_version;
}
/*==================== 区块视图：区块缓存全在 Level 里，这里只钉住当前3x3 ====================*/
static bool fix_ChunkPos(Chunk *c,int32_t cx,int32_t cz){/*xPos/zPos 和文件位置对不上就按文件位置重写；true=改了*/
	if(!c){
		return false;
	}
	bool changed=false;/*游戏是 getIntOr：别的数值类型只要值对就不报错，不改（用户：游戏不报错的不动）*/
	nbt_Compound *b=get_ChunkBody(&c->con);/*xPos/zPos 在 body 那一层（老格式是 Level）*/
	if(!b){
		return false;
	}
	if(get_TagInt(b->get("xPos"))!=std::optional<int32_t>(cx)){
		b->set_Int("xPos",cx);
		changed=true;
	}
	if(get_TagInt(b->get("zPos"))!=std::optional<int32_t>(cz)){
		b->set_Int("zPos",cz);
		changed=true;
	}
	return changed;/*索引不缓存xPos/zPos，不用重建*/
}

struct Fix_Pad{/*一个被钉住的区块+每段的规则表（按段号）；不拥有Chunk，也不复制区块*/
	int32_t cx=0,cz=0;
	Chunk *chunk=NULL;/*段表/调色板的索引在 Chunk 自己身上*/
	std::unordered_map<int32_t,Fix_Sec> secs;
	int32_t last_sy=0;
	Fix_Sec *last_info=NULL;
	Fix_Sec *get_Sec(int32_t sy,const Rules_Api *api,uint32_t mask){/*调用者先确认这一段在（get_Section非空）*/
		if(last_info&&last_sy==sy){
			return last_info;
		}
		auto it=secs.find(sy);
		if(it==secs.end()){
			Fix_Sec si;
			build_Sec(si,chunk,sy,api,mask);
			it=secs.emplace(sy,std::move(si)).first;
		}
		last_sy=sy;
		last_info=&it->second;
		return last_info;
	}
};
struct Fix_Coord{int32_t x,y,z;};/*排队等重判的一格（世界坐标）*/
struct Fix_Job;

struct Fix_Scan{/*以一个区块为中心的一次处理：钉住它和周围八个，只改中心那块；析构/finish() 时交回、松开*/
	Fix_Job &job;
	int32_t cx=0,cz=0;
	Fix_Pad pad[3][3];
	Fix_Pad *center=NULL;
	std::deque<Fix_Coord> local;
	bool ok=false,changed=false,finished=false;

	Fix_Scan(Fix_Job &j,int32_t x,int32_t z);
	~Fix_Scan(){finish();}
	void finish();
	bool scan_Center();
	bool judge_Pos(int32_t x,int32_t y,int32_t z);
	void drain_Local();
	bool read_Info(int dir,int32_t x,int32_t y,int32_t z,const Blk_Info **out);
	void fill_Ctx(Rules_Ctx &c,int32_t x,int32_t y,int32_t z);
	bool write_Block(int32_t x,int32_t y,int32_t z,const Block &b,const Rules_Tick &t);
	void enqueue_Neighbors(int32_t x,int32_t y,int32_t z);
	void refresh_WrittenSec(Fix_Sec &s,int32_t sy);
};

struct Fix_Job{/*一个维度的物理阶段：范围、插件、统计，和跨区块的连锁队列*/
	Level *lv;
	std::string dim;
	const Rules_Api *api;
	uint32_t mask;
	Block_Range rg;
	Fix_Stat st;
	std::deque<Fix_Coord> cross;/*跨区块的待复判位置；区块本身不在这里缓存*/

	Fix_Job(Level *l,const char *d,const Block_Range &r,const Rules_Api *a,uint32_t m):
		lv(l),dim(d?d:""),api(a),mask(m),rg(r){
		st.init(a->count_Rule());
	}
	void scan_Chunk(int32_t cx,int32_t cz);
	void drain_Cross();
};

static bool fix_At(Fix_Job &j,Fix_Scan &sc,int32_t x,int32_t y,int32_t z,Fix_Sec &s,int32_t sy,
	int di,uint32_t idx);

Fix_Scan::Fix_Scan(Fix_Job &j,int32_t x,int32_t z):job(j),cx(x),cz(z){/*钉住以 (x,z) 为中心的 3×3：没生成完/版本和规则不同的不钉；中心钉不上 ok=false*/
	for(int dx=-1;dx<=1;dx++){
		for(int dz=-1;dz<=1;dz++){
			Fix_Pad &p=pad[dx+1][dz+1];
			p.cx=cx+dx;
			p.cz=cz+dz;
			Chunk *c=job.lv->pin_Chunk(job.dim.c_str(),p.cx,p.cz);
			if(c&&c->is_Full()&&is_RulesChunk(c,job.api)){
				p.chunk=c;
			}else{
				if(c&&!dx&&!dz&&c->is_Full()){
					job.st.ver_skip++;
				}
				if(c){/*没生成完/版本和规则的不一样：不钉在里面，规则读到它时按读不出来处理*/
					job.lv->unpin_Chunk(job.dim.c_str(),p.cx,p.cz);
				}
				p.chunk=NULL;
			}
		}
	}
	center=&pad[1][1];
	if(!center->chunk){
		finish();
		return;
	}
	ok=true;/*xPos/zPos 修NBT 阶段已经按文件位置改好；重复Y/Y不是Byte 的段库照游戏取（索引里那段），整理留到收尾*/
}
void Fix_Scan::finish(){/*收尾：中心区块改过就交回 Level 标脏，九个区块全部松开；可以重复调*/
	if(finished){
		return;
	}
	finished=true;
	if(changed&&center&&center->chunk){
		if(!job.lv->set_Chunk(job.dim.c_str(),cx,cz,*center->chunk)){
			job.st.flush_fail++;
		}
	}
	for(int dx=0;dx<3;dx++){
		for(int dz=0;dz<3;dz++){
			Fix_Pad &p=pad[dx][dz];
			if(p.chunk){
				job.lv->unpin_Chunk(job.dim.c_str(),p.cx,p.cz);
				p.chunk=NULL;
			}
		}
	}
	ok=false;
}
bool Fix_Scan::read_Info(int dir,int32_t x,int32_t y,int32_t z,const Blk_Info **out){/*读 (x,y,z) 朝 dir 方向那一格的 Blk_Info（可能在旁边的区块里）；false=读不出来（区块没钉住、坏段、坏下标），规则会按 blind 处理*/
	static const int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	*out=NULL;
	if(dir<0||dir>5){
		return false;
	}
	int32_t nx=x+off[dir][0],ny=y+off[dir][1],nz=z+off[dir][2];
	int32_t pcx=floor_Div16(nx),pcz=floor_Div16(nz);
	int ix=pcx-cx+1,iz=pcz-cz+1;
	if(ix<0||ix>2||iz<0||iz>2){
		return false;
	}
	Fix_Pad &p=pad[ix][iz];
	if(!p.chunk){
		return false;
	}
	const nbt_Any *sa=p.chunk->get_Sections();/*逐格调用：走区块索引，DataVersion 只读一次*/
	if(sa&&(sa->type!=NBT_LIST||(!((const nbt_List*)sa)->con.empty()&&((const nbt_List*)sa)->elem!=NBT_COMPOUND))){
		return false;/*sections成员在但不是compound的表：整块读不出来，不能当空气邻居*/
	}
	int32_t sy=get_SectionY(ny);
	if(sy<-128||sy>127||!p.chunk->sec_by_y[(uint8_t)sy]){/*缺section＝空气（用户定的）；上面刚核对过索引*/
		*out=&info_Air;
		return true;
	}
	Fix_Sec *s=p.get_Sec(sy,job.api,job.mask);
	if(s->bad){
		return false;
	}
	if(!s->box){/*纯光段（老格式少了 Palette/BlockStates）＝空气*/
		*out=&info_Air;
		return true;
	}
	if(!s->palette){
		return false;
	}
	int32_t lx=(int32_t)((int64_t)nx-(int64_t)pcx*16);
	int32_t lz=(int32_t)((int64_t)nz-(int64_t)pcz*16);
	uint32_t i=(uint32_t)(ny-sy*16)*256+(uint32_t)lz*16+(uint32_t)lx;
	uint32_t idx=get_SecIndex(*s,i);
	if(idx==IDX_AIR){
		*out=&info_Air;
		return true;
	}
	if(idx==IDX_BAD||idx>=s->info.size()){
		return false;
	}
	*out=&s->info[idx];
	return true;
}
void Fix_Scan::fill_Ctx(Rules_Ctx &c,int32_t x,int32_t y,int32_t z){/*把六个面填进 c.nb；读不出来的方向记在 c.bad 里*/
	c.got=0;
	c.bad=0;
	for(int d=0;d<6;d++){
		c.got|=(uint8_t)(1<<d);
		const Blk_Info *b=NULL;
		if(!read_Info(d,x,y,z,&b)){
			c.bad|=(uint8_t)(1<<d);
			c.nb[d]=NULL;
		}else{
			c.nb[d]=b;
		}
	}
}
bool Fix_Scan::write_Block(int32_t x,int32_t y,int32_t z,const Block &b,const Rules_Tick &t){/*往中心区块写一格：写方块、删这一格原来的两类刻、补插件要的那条；方块没写进去给 false*/
	int32_t lx=(int32_t)((int64_t)x-(int64_t)cx*16);
	int32_t lz=(int32_t)((int64_t)z-(int64_t)cz*16);
	if(!center->chunk->set_Block(b,lx,y,lz)){
		return false;
	}
	changed=true;
	/*改了方块就把它原来的方块刻/流体刻清掉；插件说要补的（比如拆完留下的水要流）再补上*/
	int d0=del_ChunkTick(&center->chunk->con,false,x,y,z);
	int d1=del_ChunkTick(&center->chunk->con,true,x,y,z);
	if(d0<0||d1<0){
		job.st.tick_fail++;
	}
	if(d0>0){
		job.st.tick_del+=d0;
	}
	if(d1>0){
		job.st.tick_del+=d1;
	}
	if(!t.type.empty()){
		if(add_ChunkTick(&center->chunk->con,t.fluid,std::string(t.type).c_str(),x,y,z,t.delay,t.priority)){
			job.st.tick_add++;
		}else{
			job.st.tick_fail++;
		}
	}
	return true;
}
void Fix_Scan::enqueue_Neighbors(int32_t x,int32_t y,int32_t z){/*这一格改了：六个邻居排队重判（本区块的进 local，别的区块的进 job.cross；写范围外的不排）*/
	static const int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	for(int d=0;d<6;d++){
		int32_t nx=x+off[d][0],ny=y+off[d][1],nz=z+off[d][2];
		if(!job.rg.is_Inside(nx,ny,nz)){
			continue;
		}
		if(floor_Div16(nx)==cx&&floor_Div16(nz)==cz){
			local.push_back({nx,ny,nz});
		}else{
			job.cross.push_back({nx,ny,nz});
		}
	}
}
void Fix_Scan::refresh_WrittenSec(Fix_Sec &s,int32_t sy){/*刚写过第 sy 段：重读它的 palette/data（可能刚建了 data、调色板多了一条）*/
	refresh_Sec(s,center->chunk,sy,job.api,job.mask);
}
bool Fix_Scan::scan_Center(){/*把中心区块在范围里的格子扫一遍：调色板条目有规则的才交给 fix_At；true=改过*/
	if(!ok){
		return false;
	}
	nbt_List *secs=(nbt_List*)get_ChunkSections(center->chunk);
	if(!secs||secs->type!=NBT_LIST){
		return false;
	}
	int64_t wx=(int64_t)cx*16,wz=(int64_t)cz*16;
	int32_t lx0=(int32_t)std::max<int64_t>(0,(int64_t)job.rg.x0-wx);/*方块级：只扫范围里的格子*/
	int32_t lx1=(int32_t)std::min<int64_t>(15,(int64_t)job.rg.x1-wx);
	int32_t lz0=(int32_t)std::max<int64_t>(0,(int64_t)job.rg.z0-wz);
	int32_t lz1=(int32_t)std::min<int64_t>(15,(int64_t)job.rg.z1-wz);
	int32_t sy0=get_SectionY(job.rg.y0),sy1=get_SectionY(job.rg.y1);
	for(size_t k=0;k<secs->con.size();k++){
		nbt_Any *a=secs->con[k];
		if(!a||a->type!=NBT_COMPOUND){
			continue;
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		int32_t sy=read_SectionY(sec);
		if(sy<sy0||sy>sy1||center->chunk->get_Section(sy)!=sec){/*同一个Y有几段：只认索引里那段（最后那段，游戏取的就是它）*/
			continue;
		}
		job.st.sec_all++;
		Fix_Sec *s=center->get_Sec(sy,job.api,job.mask);
		if(!s->cand){
			if(s->bad||(s->box&&!s->palette)){
				job.st.sec_bad++;
			}
			continue;
		}
		job.st.sec_hit++;
		int32_t ly0=std::max<int32_t>(0,job.rg.y0-sy*16);
		int32_t ly1=std::min<int32_t>(15,job.rg.y1-sy*16);
		for(int32_t lx=lx0;lx<=lx1;lx++){
			for(int32_t lz=lz0;lz<=lz1;lz++){
				for(int32_t ly=ly0;ly<=ly1;ly++){
					int64_t xx=wx+lx,yy=(int64_t)sy*16+ly,zz=wz+lz;
					if(xx<INT32_MIN||xx>INT32_MAX||yy<INT32_MIN||yy>INT32_MAX||zz<INT32_MIN||zz>INT32_MAX){
						continue;
					}
					if(!job.rg.is_Inside(xx,yy,zz)){
						continue;
					}
					uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
					uint32_t idx=get_SecIndex(*s,i);
					if(idx==IDX_BAD){
						job.st.pos_bad++;
						continue;
					}
					if(idx>=s->rule.size()||s->rule[idx]<0){
						continue;
					}
					job.st.pos++;
					if(fix_At(job,*this,(int32_t)xx,(int32_t)yy,(int32_t)zz,*s,sy,s->rule[idx],idx)){
						changed=true;
					}
				}
			}
		}
	}
	return changed;
}
bool Fix_Scan::judge_Pos(int32_t x,int32_t y,int32_t z){/*重判一格（连锁排进来的）；不在本区块的转给 job.cross；true=改了*/
	if(!ok||!job.rg.is_Inside(x,y,z)){
		return false;
	}
	if(floor_Div16(x)!=cx||floor_Div16(z)!=cz){
		job.cross.push_back({x,y,z});
		return false;
	}
	int32_t sy=get_SectionY(y);
	if(!center->chunk->get_Section(sy)){
		return false;
	}
	Fix_Sec *s=center->get_Sec(sy,job.api,job.mask);
	if(!s->cand){
		return false;
	}
	int32_t lx=(int32_t)((int64_t)x-(int64_t)cx*16);
	int32_t lz=(int32_t)((int64_t)z-(int64_t)cz*16);
	uint32_t i=(uint32_t)(y-sy*16)*256+(uint32_t)lz*16+(uint32_t)lx;
	uint32_t idx=get_SecIndex(*s,i);
	if(idx==IDX_BAD){
		job.st.pos_bad++;
		return false;
	}
	if(idx>=s->rule.size()||s->rule[idx]<0){
		return false;
	}
	job.st.pos++;
	if(fix_At(job,*this,x,y,z,*s,sy,s->rule[idx],idx)){
		changed=true;
		return true;
	}
	return false;
}
void Fix_Scan::drain_Local(){/*把本区块排着的格子判完（判的时候还会接着往里排）*/
	while(!local.empty()){
		Fix_Coord p=local.front();
		local.pop_front();
		judge_Pos(p.x,p.y,p.z);
	}
}
static bool fix_At(Fix_Job &j,Fix_Scan &sc,int32_t x,int32_t y,int32_t z,Fix_Sec &s,int32_t sy,
	int di,uint32_t idx){
	const Blk_Info *self=&s.info[idx];
	std::string base(self->base);/*set_Block会换掉调色板，self随后就悬空；统计用的名字先拷出来*/
	int ru=j.api->get_DefRule(di);
	j.st.hit++;
	if(ru>=0&&ru<(int)j.st.rule_hit.size()){
		j.st.rule_hit[ru]++;
	}
	Rules_Ctx c;
	c.self=self;
	c.x=x;c.y=y;c.z=z;
	sc.fill_Ctx(c,x,y,z);
	int act=j.api->judge_Blk(self,di,&c);
	if(c.blind){
		j.st.blind++;
		return false;
	}
	if(act==FX_KEEP){
		return false;
	}
	if(act==FX_AIR){
		if(self->flags&BF_AIR){
			j.st.noop++;
			return false;
		}
		bool wet=(self->flags&BF_WATER)!=0;/*只用来计数：拆了留水的有几格（留什么由插件给）*/
		Block b=c.out_block.valid()?std::move(c.out_block):Block("minecraft:air");
		if(!sc.write_Block(x,y,z,b,c.out_tick)){
			j.st.miss++;
			return false;
		}
		j.st.air++;
		if(wet){
			j.st.wet++;
		}
		sc.refresh_WrittenSec(s,sy);
		if(ru>=0){
			j.st.rule_change[ru]++;
			j.st.base_change[base]++;
		}
		sc.enqueue_Neighbors(x,y,z);
		return true;
	}
	if(act!=FX_SET){
		j.st.noop++;
		return false;
	}
	if(!c.out_block.valid()){
		j.st.miss++;
		return false;
	}
	if(self->blk&&c.out_block.name()==self->name&&c.out_block.properties==(self->props?*self->props:nbt_Compound())){
		j.st.noop++;/*改出来和原来一样（名字和写出来的属性都相同）：白干*/
		return false;
	}
	nbt_Compound keep;
	bool has_keep=false;
	if(c.out_keep_be){
		int32_t lx=(int32_t)((int64_t)x-(int64_t)sc.cx*16);
		int32_t lz=(int32_t)((int64_t)z-(int64_t)sc.cz*16);
		nbt_Compound *be=sc.center->chunk->get_BlockEntity(lx,y,lz);
		if(be){
			keep=*be;
			has_keep=true;
		}
	}
	Block b=std::move(c.out_block);
	if(has_keep){
		b.block_entities=std::move(keep);
	}
	if(!sc.write_Block(x,y,z,b,c.out_tick)){
		j.st.miss++;
		return false;
	}
	j.st.set++;
	sc.refresh_WrittenSec(s,sy);
	if(ru>=0){
		j.st.rule_change[ru]++;
		j.st.base_change[base]++;
	}
	sc.enqueue_Neighbors(x,y,z);
	return true;
}
void Fix_Job::scan_Chunk(int32_t cx,int32_t cz){/*处理一个选中的区块：钉住 3×3 → 扫 → 判完本区块的连锁 → 松开 → 再处理连锁到别的区块的*/
	Fix_Scan sc(*this,cx,cz);
	if(sc.ok){
		st.chunks++;
		sc.scan_Center();
		sc.drain_Local();
	}
	sc.finish();
	drain_Cross();
}
void Fix_Job::drain_Cross(){/*跨区块的连锁：每格以它所在的区块为中心另开一次 3×3（上一个已经松开），判完为止*/
	while(!cross.empty()){
		Fix_Coord p=cross.front();
		cross.pop_front();
		Fix_Scan sc(*this,floor_Div16(p.x),floor_Div16(p.z));
		if(sc.ok){
			sc.judge_Pos(p.x,p.y,p.z);
			sc.drain_Local();
		}
		sc.finish();
	}
}
/*==================== 区块清理 ====================*/
static bool is_ChunkEmpty(const Chunk *c){return !has_Block(&c->con);}/*一个非空气的方块都没有*/
static bool is_InhabitedZero(const Chunk *c){/*游戏是 getLongOr(InhabitedTime,0)：没有/不是数值当0（用户 2026-11-07：读不到当0删）*/
	return get_TagLong(get_ChunkField(c,"InhabitedTime")).value_or(0)==0;
}
struct Fix_Count{/*修NBT和各清理阶段的统计（最后打“修NBT：”“清理：”两行，各字段见 ReadMeDeveloper 5.）*/
	int64_t chunks_deleted=0,status_deleted=0,empty_deleted=0,it_deleted=0,corrupt_deleted=0;
	int64_t nbt_bad=0,states_bad=0,biomes_bad=0,palette_air=0,tick_lists=0,be_bad=0,refs_bad=0,starts_bad=0;
	int64_t be_moved=0,tick_moved=0,stray_lost=0,stray_dup=0,stray_left=0,stray_folded=0;/*坐标指到别的区块的方块实体/计划刻：
		迁过去的、目标区块没有所以删掉的、目标那一格已经有了所以删掉的、迁不走原样留着的；没有坐标/跟着挪了位置的区块改到本区块里的*/
	int64_t light_chunks=0,height_chunks=0;
	int64_t ticks_deleted=0,ticks_bad=0,ticks_dup=0,poi_deleted=0,poi_added=0,poi_dup=0,poi_bad=0,poi_corrupt=0,poi_chunks=0,poi_files=0;
	int64_t entities_deleted=0,entities_bad=0,entity_chunks=0,entity_files=0;
	int64_t mca_deleted=0,mca_formatted=0,off_cleaned=0;/*off_cleaned：表头上偏移异常、清成 0 的格子*/
	int64_t coord_fix=0,sec_dup=0,sec_y_fixed=0,palette_cleaned=0;
	int64_t io_skipped=0,ver_skipped=0,failed=0;
};
/*==================== 遍历 ====================*/
/*kind（region/poi/entities）目录里实际存在、范围碰到的区域文件（按坐标排序）。不逐个区域坐标去 stat：
  范围给到整个世界（±3000 万格）时区域坐标有上百亿个，转不完；列目录再和范围求交*/
static std::vector<Fix_Region> list_Files(Level &lv,const std::string &dim,const char *kind,const Block_Range &rg){
	std::vector<Fix_Region> out;
	std::string rel=lv.get_DimensionPath(dim.c_str(),kind);
	if(rel.empty()){
		return out;
	}
	for(const std::pair<int32_t,int32_t> &p:list_MCAFiles((lv.path+"/"+rel).c_str())){
		if(rg.is_RegionSelected(p.first,p.second)){
			out.push_back({p.first,p.second});
		}
	}
	return out;
}
/*范围碰到、region 文件在的区域（后面各阶段都只走这份清单），不在的整个区域跳过*/
static void list_Regions(Level &lv,const std::string &dim,const Block_Range &rg,std::vector<Fix_Region> &out){
	out=list_Files(lv,dim,"region",rg);
}
/*走一遍清单里的区域：区域镜像读不出来的整个跳过（say=true 时打一行、计一次 IO 跳过）；每个区域走表头的 1024 格，
  范围没碰到的、不存在的、偏移异常的区块跳过，剩下的一个个交给 fn(cx,cz)。
  先把这个区域要走的区块列出来再调 fn：fn 里取别的区块时，区域镜像可能被 Level 换出去*/
template<class Fn> static void each_Chunk(Level &lv,const std::string &dim,const Block_Range &rg,
	const std::vector<Fix_Region> &regions,const char *stage,bool say,Fix_Count &ct,Fn fn){
	Progress pr(stage,(int64_t)regions.size()*1024);
	std::vector<std::pair<int32_t,int32_t>> todo;
	for(const Fix_Region &r:regions){
		MCA *m=lv.get_MCA(dim.c_str(),r.rx,r.rz);
		if(!m||m->bad){
			if(say&&(m||lv.miss_why!=Level::GET_NO_FILE)){
				printf("fixer: 区域文件 r.%d.%d.mca 读不出来，整个区域跳过（只记日志）\n",r.rx,r.rz);
				ct.io_skipped++;
			}
			pr.tick(1024);
			continue;
		}
		todo.clear();
		for(int32_t lx=0;lx<32;lx++){
			for(int32_t lz=0;lz<32;lz++){
				int32_t cx=r.rx*32+lx,cz=r.rz*32+lz;
				if(rg.is_ChunkSelected(cx,cz)&&m->has_Chunk((uint32_t)lx,(uint32_t)lz)){
					todo.push_back({cx,cz});
				}
			}
		}
		int64_t left=1024;
		for(size_t i=0;i<todo.size();i++){
			fn(todo[i].first,todo[i].second);
			int64_t step=1024/(int64_t)todo.size();
			pr.tick(step);
			left-=step;
		}
		pr.tick(left);
	}
	pr.finish();
}
/*region 文件 (rx,rz) 的表头上哪些格子有区块（下标 lx+lz*32；偏移异常的不算）。文件不在＝一格都没有，给 true；
  文件在但读不出来给 false（不知道有没有，调用者整个区域跳过）*/
static bool read_RegionSlots(Level &lv,const std::string &dim,int32_t rx,int32_t rz,std::bitset<1024> &has){
	has.reset();
	MCA *m=lv.get_MCA(dim.c_str(),rx,rz);
	if(!m){
		return lv.miss_why==Level::GET_NO_FILE;
	}
	if(m->bad){
		return false;
	}
	for(uint32_t i=0;i<1024;i++){
		has[i]=m->has_Chunk(i%32,i/32);
	}
	return true;
}
/*==================== NBT 修复（用户 2026-11-07：以游戏为准，游戏读档报 ERROR/WARN 的按最小单位删，不报的不动） ====================
  照 26.1.1 的 SerializableChunkData.parse / PalettedContainer / BlockEntity.getPosFromTag / loadStatic /
  SavedTick.codec / unpackStructureReferences 逐条对的（ReadMeDeveloper 1.10 有表）：
  · NbtIo 读不了的 tag（嵌套超过512层、字符串/键名不是 modified UTF-8）：游戏整块读失败 → 删那个 tag；
  · 段的 block_states/biomes 解不出来（palette 缺/空、多条没 data、data 长度不对、下标越界）：游戏整块载入失败
    → 删这一段的 block_states/biomes（这一段在游戏里就是全空气/默认生物群系）；
  · 调色板条目认不出名字：游戏换成空气、打 ERROR → 换成空气条目（生物群系换成 plains）；
  · block_ticks/fluid_ticks 不是 List：ERROR → 删这个成员；单条读不进来（i 不是合法名字、x/y/z/t/p 不是数值）：
    ERROR、游戏丢掉这一条 → 删这一条；
  · 方块实体没有 id/id 不是合法名字：ERROR、丢掉 → 删这一条；坐标不在本区块（游戏 WARN、折回本区块）、
    计划刻坐标不在本区块（游戏静默丢掉）：迁到坐标所在的区块（migrate_Strays，用户 2026-11-14）；
  · structures.References/starts 的名字写法不合法：WARN/ERROR → 删这一项；References 指到 8 个区块以外：WARN、丢掉 → 删这个坐标。
  名字认不认识（要注册表）、方块实体和方块对不对得上：先不做（用户：先不改）。*/
static int32_t java_Abs(int32_t v){return v==INT32_MIN?INT32_MIN:(v<0?-v:v);}/*Math.abs(int)：INT_MIN 还是 INT_MIN*/
static int32_t java_Sub(int32_t a,int32_t b){return (int32_t)((uint32_t)a-(uint32_t)b);}/*Java 的 int 减法：溢出回绕*/
static nbt_Any *make_AirEntry(uint8_t elem,State_Format fmt){/*调色板里替换坏条目用的空气，和列表元素类型一致*/
	if(elem==NBT_STRING){
		return new nbt_String("minecraft:air");
	}
	nbt_Compound *e=new nbt_Compound;
	e->set_String(fmt==STATE_ID?"id":"Name","minecraft:air");
	return e;
}
/*调色板条目游戏认不出来的（BlockState.CODEC 解不了：游戏换空气、打 ERROR Recoverable errors）换成空气条目。
  调色板不是 List（Byte/Int/Long_Array 也算集合，元素是数值）、或者 List 的元素类型这种格式下一条都认不出来：整条换成 n 条空气。
  返回换了几条*/
static int64_t repair_PaletteEntries(nbt_Compound *box,const char *key,State_Format fmt){
	nbt_Any *pa=box->get(key);
	std::optional<size_t> n=get_CollectionSize(pa);
	if(!n){
		return 0;
	}
	uint8_t ok_elem=fmt==STATE_ID?NBT_STRING:NBT_COMPOUND;
	bool whole=pa->type!=NBT_LIST;
	if(!whole){
		uint8_t e=((const nbt_List*)pa)->elem;
		whole=*n&&e!=NBT_COMPOUND&&e!=ok_elem;
	}
	if(whole){
		nbt_List *np=new nbt_List(ok_elem);
		for(size_t k=0;k<*n;k++){
			np->push_back(make_AirEntry(ok_elem,fmt));
		}
		box->set(key,np);
		return (int64_t)*n;
	}
	nbt_List *pal=(nbt_List*)pa;
	int64_t m=0;
	for(size_t k=0;k<pal->con.size();k++){
		if(!get_StateName(pal->con[k],fmt)[0]){
			pal->con[k]->free();
			pal->con[k]=make_AirEntry(pal->elem==NBT_STRING?NBT_STRING:NBT_COMPOUND,fmt);
			m++;
		}
	}
	return m;
}
static bool repair_ChunkNBT(Chunk *c,int32_t cx,int32_t cz,Fix_Count &ct){/*一个区块按上面的清单修一遍；true=改了（调用者负责交回 Level）。改过就 reindex()*/
	bool changed=false;
	int n=prune_NBTUnreadable(&c->con,512);
	if(n){
		ct.nbt_bad+=n;
		changed=true;
	}
	Chunk_Layout L=get_ChunkLayout(&c->con);
	State_Format fmt=L.fmt;
	bool biome64=L.states&&!is_BiomePerBlock(L.dv);/*2832 以前生物群系在 Level.Biomes（整块一个数组），不在段里*/
	nbt_Compound *body=get_ChunkBody(&c->con);
	if(!body){
		if(changed){
			c->reindex();
		}
		return changed;/*老格式没有 Level：没有字段可修*/
	}
	nbt_Any *sa=body->get(L.sections);
	nbt_List *secs=sa&&sa->type==NBT_LIST?(nbt_List*)sa:NULL;
	for(size_t i=0;secs&&i<secs->con.size();i++){
		nbt_Any *a=secs->con[i];
		if(!a||a->type!=NBT_COMPOUND){
			continue;/*游戏静默跳过：不动*/
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		States_View sv=get_StatesView(sec,L,true);/*游戏怎么读这一段（老格式先过 DataFixers）*/
		if(sv.kind==STATES_BAD){
			if(L.states){
				sec->erase("block_states");
			}else{
				sec->erase("Palette");/*老格式：两样一起删，这一段在游戏里就是空气*/
				sec->erase("BlockStates");
			}
			ct.states_bad++;
			changed=true;
		}else if(sv.kind==STATES_OK){
			int64_t m=repair_PaletteEntries(get_SectionStates(sec,L),L.palette,fmt);
			if(m){
				ct.palette_air+=m;
				changed=true;
			}
		}
		nbt_Any *bio=sec->get("biomes");
		if(biome64&&bio&&bio->type==NBT_COMPOUND){
			if(!check_PaletteContainer((nbt_Compound*)bio,64,false)){
				sec->erase("biomes");
				ct.biomes_bad++;
				changed=true;
			}else{
				nbt_Any *pa=((nbt_Compound*)bio)->get("palette");
				nbt_List *pal=pa->type==NBT_LIST?(nbt_List*)pa:NULL;
				if(!pal||pal->elem!=NBT_STRING){/*生物群系是名字列表：不是字符串的条目（数组的元素也是）游戏换成 plains*/
					size_t n=get_CollectionSize(pa).value_or(0);
					nbt_List *np=new nbt_List(NBT_STRING);
					for(size_t k=0;k<n;k++){
						np->push_back(new nbt_String("minecraft:plains"));
					}
					((nbt_Compound*)bio)->set("palette",np);
					ct.palette_air+=(int64_t)n;
					changed=true;
				}else{
					for(size_t k=0;k<pal->con.size();k++){
						if(!check_Identifier(((nbt_String*)pal->con[k])->con)){
							((nbt_String*)pal->con[k])->con="minecraft:plains";
							ct.palette_air++;
							changed=true;
						}
					}
				}
			}
		}
	}
	for(bool fluid:{false,true}){
		const char *key=fluid?L.fluid_ticks:L.block_ticks;
		nbt_Any *ta=body->get(key);
		if(!ta){
			continue;
		}
		if(ta->type!=NBT_LIST){
			body->erase(key);
			ct.tick_lists++;
			changed=true;
			continue;
		}
		nbt_List *l=(nbt_List*)ta;
		for(size_t i=l->con.size();i>0;i--){
			if(!check_ChunkTickRecord(l->con[i-1])){
				l->erase(i-1);
				ct.ticks_bad++;
				ct.ticks_deleted++;
				changed=true;
			}
		}
	}
	nbt_Any *ba=body->get(L.block_entities);
	if(ba&&ba->type==NBT_LIST){
		nbt_List *bes=(nbt_List*)ba;
		for(size_t i=bes->con.size();i>0;i--){
			nbt_Any *e=bes->con[i-1];
			if(!e||e->type!=NBT_COMPOUND){
				continue;/*游戏静默跳过：不动*/
			}
			nbt_Compound *be=(nbt_Compound*)e;
			const nbt_Any *id=be->get("id");
			if(!id||id->type!=NBT_STRING||!check_Identifier(((const nbt_String*)id)->con)){
				bes->erase(i-1);
				ct.be_bad++;
				changed=true;
				continue;
			}
		}
	}
	nbt_Any *sta=body->get(L.structures);
	if(sta&&sta->type==NBT_COMPOUND){
		nbt_Compound *st=(nbt_Compound*)sta;
		nbt_Any *ra=st->get("References");
		if(ra&&ra->type==NBT_COMPOUND){
			nbt_Compound *refs=(nbt_Compound*)ra;
			std::vector<std::string> drop;
			for(auto &kv:refs->con){
				if(!check_Identifier(kv.first)){
					drop.push_back(kv.first);
					continue;
				}
				if(!kv.second||kv.second->type!=NBT_LONG_ARRAY){
					continue;/*不是 Long_Array：游戏静默跳过*/
				}
				std::vector<int64_t> &v=((nbt_Long_Array*)kv.second)->con;
				size_t before=v.size();
				v.erase(std::remove_if(v.begin(),v.end(),[cx,cz](int64_t p){
					int32_t rx=(int32_t)(uint32_t)(uint64_t)p,rz=(int32_t)(uint32_t)((uint64_t)p>>32);
					return std::max(java_Abs(java_Sub(rx,cx)),java_Abs(java_Sub(rz,cz)))>8;
				}),v.end());
				if(v.size()!=before){
					ct.refs_bad+=(int64_t)(before-v.size());
					changed=true;
				}
			}
			for(const std::string &k:drop){
				refs->erase(k);
				ct.refs_bad++;
				changed=true;
			}
		}
		nbt_Any *sa2=st->get(L.starts);
		if(sa2&&sa2->type==NBT_COMPOUND){
			nbt_Compound *starts=(nbt_Compound*)sa2;
			std::vector<std::string> drop;
			for(auto &kv:starts->con){
				if(!check_Identifier(kv.first)){
					drop.push_back(kv.first);
				}
			}
			for(const std::string &k:drop){
				starts->erase(k);
				ct.starts_bad++;
				changed=true;
			}
		}
	}
	if(changed){
		c->reindex();
	}
	return changed;
}
/*---- 坐标指到别的区块的方块实体/计划刻：迁到坐标所在的区块（用户 2026-11-14） ----
  游戏读档时方块实体折回本区块（WARN，x/z 取低4位：落在一个不相干的格子上）、计划刻静默丢掉；这里按坐标把它们放回该在的区块。
  两种情况不算“指到别的区块”，照原来的办法改到本区块里（x/z 取低4位，区块级，不看范围）：
    · 方块实体根本没有 x/z（游戏当 0）：没有坐标可信；
    · 整个区块挪过位置（xPos/zPos 和文件位置对不上、刚按文件位置改过），记录指的是区块原来的 xPos/zPos：跟着区块走。
  真指到别处的是方块级的操作：坐标在范围外的不动。目标区块：
    · 不存在、游戏读不进来、没生成完（后面区块清理会删）：这条记录没地方去，删；
    · 那一格已经有方块实体/同样的计划刻：留那里原来的，这条删；
    · 读不出来（区域文件坏、自定义压缩、缓存腾不出位置）、DataVersion 和原来的区块不一样、表的形状不对：迁不走，原样留着；
    · 目标区块自己的 xPos/zPos 也不对（还没轮到修）：也留着，不替它先改——2026-11-21 以前在这里先把目标的 xPos/zPos 改了却不标脏，
      轮到目标时看不出它挪过位置，跟着它挪的记录被当成“指到别处”，xPos 的修正还可能没落盘。下一趟目标修好了再迁。*/
enum{STRAY_MOVED=0,STRAY_LOST=1,STRAY_DUP=2,STRAY_LEFT=3};
static int deliver_Stray(Level &lv,const std::string &dim,int32_t dv,int kind,const nbt_Compound &rec,
	int32_t x,int32_t y,int32_t z,Fix_Count &ct){/*kind：0=方块实体 1=方块刻 2=流体刻；返回 STRAY_*，放进去了还要把目标区块交回 Level*/
	int32_t tx=floor_Div16(x),tz=floor_Div16(z);
	Chunk *t=lv.get_Chunk(dim.c_str(),tx,tz);
	if(!t){
		int why=lv.miss_why;
		return why==Level::GET_NO_FILE||why==Level::GET_NO_CHUNK||why==Level::GET_BAD_CHUNK?STRAY_LOST:STRAY_LEFT;
	}
	if(get_ChunkDataVersion(&t->con)!=dv){
		return STRAY_LEFT;/*两个区块版本不一样：记录的写法可能不同，不搬*/
	}
	if(!t->is_Full()){
		return STRAY_LOST;
	}
	Chunk_Layout L=get_ChunkLayout(&t->con);
	nbt_Compound *body=get_ChunkBody(&t->con);
	if(!body||get_TagInt(body->get("xPos"))!=std::optional<int32_t>(tx)||get_TagInt(body->get("zPos"))!=std::optional<int32_t>(tz)){
		return STRAY_LEFT;/*目标自己的位置也不对：先不搬（见上）*/
	}
	int r=STRAY_MOVED;
	if(kind==0){
		const nbt_Any *ba=body->get(L.block_entities);
		if(ba&&(ba->type!=NBT_LIST||(!((const nbt_List*)ba)->con.empty()&&((const nbt_List*)ba)->elem!=NBT_COMPOUND))){
			return STRAY_LEFT;
		}
		if(t->get_BlockEntity(x&15,y,z&15)){
			r=STRAY_DUP;
		}else if(!t->set_BlockEntity(rec,x&15,y,z&15)){
			return STRAY_LEFT;
		}
	}else{
		bool fluid=kind==2;
		const nbt_Any *ta=body->get(fluid?L.fluid_ticks:L.block_ticks);
		if(ta&&ta->type!=NBT_LIST){
			return STRAY_LEFT;
		}
		const nbt_Any *ia=rec.get("i");
		std::string type=ia&&ia->type==NBT_STRING?((const nbt_String*)ia)->con:std::string();
		int32_t d=get_NumberInt(rec.get("t")).value_or(0),pr=get_NumberInt(rec.get("p")).value_or(0);
		if(!add_ChunkTick(&t->con,fluid,type.c_str(),x,y,z,d,pr)){
			r=STRAY_DUP;/*那一格已经有同样的刻*/
		}
	}
	if(r==STRAY_MOVED&&!lv.set_Chunk(dim.c_str(),tx,tz,*t)){
		ct.failed++;
	}
	return r;
}
/*把区块 c（钉住的）里坐标指到别处的方块实体/计划刻迁走；true=c 改了。坏记录在这之前已经删掉。
  moved=这个区块的 xPos/zPos 刚从 (ox,oz) 改成文件位置 (cx,cz)*/
static bool migrate_Strays(Level &lv,const std::string &dim,const Block_Range &rg,Chunk *c,int32_t cx,int32_t cz,
	bool moved,int32_t ox,int32_t oz,Fix_Count &ct){
	Chunk_Layout L=get_ChunkLayout(&c->con);
	nbt_Compound *body=get_ChunkBody(&c->con);
	if(!body){
		return false;
	}
	int32_t dv=get_ChunkDataVersion(&c->con);
	bool changed=false;
	const char *keys[3]={L.block_entities,L.block_ticks,L.fluid_ticks};
	for(int kind=0;kind<3;kind++){
		nbt_Any *la=body->get(keys[kind]);
		if(!la||la->type!=NBT_LIST){
			continue;
		}
		nbt_List *l=(nbt_List*)la;
		for(size_t i=l->con.size();i>0;i--){
			nbt_Any *e=l->con[i-1];
			if(!e||e->type!=NBT_COMPOUND){
				continue;
			}
			nbt_Compound *rec=(nbt_Compound*)e;
			int32_t x,y,z;
			bool has_xz=true;
			if(kind==0){/*方块实体的坐标游戏是 getIntOr(…,0)，计划刻是 Codec.INT*/
				has_xz=get_TagInt(rec->get("x")).has_value()&&get_TagInt(rec->get("z")).has_value();
				x=get_TagInt(rec->get("x")).value_or(0);
				y=get_TagInt(rec->get("y")).value_or(0);
				z=get_TagInt(rec->get("z")).value_or(0);
			}else{
				x=get_NumberInt(rec->get("x")).value_or(0);
				y=get_NumberInt(rec->get("y")).value_or(0);
				z=get_NumberInt(rec->get("z")).value_or(0);
			}
			if(floor_Div16(x)==cx&&floor_Div16(z)==cz){
				continue;
			}
			if(!has_xz||(moved&&floor_Div16(x)==ox&&floor_Div16(z)==oz)){/*没有坐标/跟着区块挪：改到本区块里*/
				rec->set_Int("x",(int32_t)((int64_t)cx*16+(x&15)));
				rec->set_Int("z",(int32_t)((int64_t)cz*16+(z&15)));
				ct.stray_folded++;
				changed=true;
				continue;
			}
			if(!rg.is_Inside(x,y,z)){
				continue;
			}
			int r=deliver_Stray(lv,dim,dv,kind,*rec,x,y,z,ct);
			if(r==STRAY_LEFT){
				ct.stray_left++;
				continue;
			}
			if(r==STRAY_MOVED){
				(kind==0?ct.be_moved:ct.tick_moved)++;
			}else if(r==STRAY_LOST){
				ct.stray_lost++;
			}else{
				ct.stray_dup++;
			}
			l->erase(i-1);
			changed=true;
		}
	}
	if(changed){
		c->reindex();
	}
	return changed;
}
/*修复阶段：选中范围里每个区块先按游戏修成读得进来的样子，再交给物理判定和后面的清理*/
static bool repair_Chunks(Level &lv,const std::string &dim,const Block_Range &rg,const std::vector<Fix_Region> &regions,
	Fix_Count &ct,std::set<uint64_t> &deleted){
	each_Chunk(lv,dim,rg,regions,"修NBT",true,ct,[&](int32_t cx,int32_t cz){
		Chunk *c=lv.pin_Chunk(dim.c_str(),cx,cz);/*钉住：迁移时要取别的区块，这个不能被换出去*/
		if(!c){
			if(lv.miss_why==Level::GET_BAD_CHUNK){/*解压/NBT解析失败：游戏读不进来（重新生成），删掉*/
				printf("fixer: 区块(%d,%d) NBT损坏，整块删除\n",cx,cz);
				if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
					ct.failed++;
				}else{
					ct.corrupt_deleted++;
					deleted.insert(get_ChunkKey(cx,cz));
				}
			}else if(lv.miss_why==Level::GET_UNSUPPORTED_COMPRESSION){
				printf("fixer: 区块(%d,%d) 是模组的自定义压缩，跳过不删（只记日志）\n",cx,cz);
				ct.io_skipped++;
			}else if(lv.miss_why!=Level::GET_NO_CHUNK&&lv.miss_why!=Level::GET_NO_FILE){
				printf("fixer: 区块(%d,%d) 取不到（原因 %d），跳过\n",cx,cz,lv.miss_why);
				ct.failed++;
			}
			return;
		}
		if(!is_ChunkVersionKnown(get_ChunkDataVersion(&c->con))){
			printf("fixer: 区块(%d,%d) DataVersion=%d 库不认识（1.13 扁平化以前，或者没写），跳过不动\n",cx,cz,get_ChunkDataVersion(&c->con));
			ct.ver_skipped++;
			lv.unpin_Chunk(dim.c_str(),cx,cz);
			return;
		}
		int32_t ox=cx,oz=cz;/*改之前区块自己说的位置：里面的记录指着它的话是跟着区块挪的，不是指到别处*/
		if(const nbt_Compound *b=get_ChunkBody(&c->con)){
			ox=get_TagInt(b->get("xPos")).value_or(cx);
			oz=get_TagInt(b->get("zPos")).value_or(cz);
		}
		bool coords=fix_ChunkPos(c,cx,cz);
		bool changed=repair_ChunkNBT(c,cx,cz,ct)||coords;
		if(migrate_Strays(lv,dim,rg,c,cx,cz,coords,ox,oz,ct)){
			changed=true;
		}
		if(changed){
			if(lv.set_Chunk(dim.c_str(),cx,cz,*c)){
				if(coords){
					ct.coord_fix++;
				}
			}else{
				ct.failed++;
			}
		}
		lv.unpin_Chunk(dim.c_str(),cx,cz);
	});
	return true;
}
/*区块清理（区块级：范围碰到的区块整块看，y 不管）：没生成完的删；全空气、InhabitedTime 是 0 的删*/
static bool cleanup_Chunks(Level &lv,const std::string &dim,const Block_Range &rg,const std::vector<Fix_Region> &regions,
	Fix_Count &ct,std::set<uint64_t> &deleted){
	each_Chunk(lv,dim,rg,regions,"清理区块",false,ct,[&](int32_t cx,int32_t cz){
		Chunk *c=lv.get_Chunk(dim.c_str(),cx,cz);
		if(!c){
			if(lv.miss_why==Level::GET_BAD_CHUNK){/*压缩/NBT解析失败：整块是垃圾，删掉（修NBT 阶段没删成的兜底）*/
				printf("fixer: 区块(%d,%d) NBT损坏，整块删除\n",cx,cz);
				if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
					ct.failed++;
				}else{
					ct.corrupt_deleted++;
					deleted.insert(get_ChunkKey(cx,cz));
				}
			}
			return;/*自定义压缩等：修NBT 阶段已经记过*/
		}
		if(!is_ChunkVersionKnown(get_ChunkDataVersion(&c->con))){
			return;/*格式不认识：不删（修NBT 阶段已经记过）*/
		}
		if(!check_ChunkBase(&c->con)){/*修复阶段没修掉的整块载入失败（正常不会有）：整块删，不能原样留给MC*/
			printf("fixer: 区块(%d,%d) 数据形状损坏，整块删除\n",cx,cz);
			if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
				ct.failed++;
			}else{
				ct.corrupt_deleted++;
				deleted.insert(get_ChunkKey(cx,cz));
			}
			return;
		}
		bool del=false,status_bad=false,empty=false;
		if(get_ChunkStatus(&c->con)!=CHUNK_FULL){
			del=true;
			status_bad=true;
		}else{
			empty=is_ChunkEmpty(c);
			del=empty||is_InhabitedZero(c);
		}
		if(!del){
			return;
		}
		if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
			ct.failed++;
			return;
		}
		ct.chunks_deleted++;
		deleted.insert(get_ChunkKey(cx,cz));
		if(status_bad){
			ct.status_deleted++;
		}else if(empty){
			ct.empty_deleted++;
		}else{
			ct.it_deleted++;
		}
	});
	return true;
}
static int strip_Light(Chunk *c,int64_t *sections){/*删掉每段的 BlockLight/SkyLight，isLightOn 置 0（游戏读到 0 就整块重算光照）；给删了几个数组*/
	nbt_Any *sa=get_ChunkSections(c);
	int n=0;
	if(sa&&sa->type==NBT_LIST){
		const nbt_List *secs=(const nbt_List*)sa;
		for(size_t i=0;i<secs->con.size();i++){
			nbt_Any *s=secs->con[i];
			if(!s||s->type!=NBT_COMPOUND){
				continue;
			}
			if(((nbt_Compound*)s)->erase("BlockLight")){
				n++;
			}
			if(((nbt_Compound*)s)->erase("SkyLight")){
				n++;
			}
		}
	}
	if(nbt_Compound *b=get_ChunkBody(&c->con)){/*isLightOn 在 body 那一层（老格式是 Level）*/
		b->set("isLightOn",new nbt_Byte(0));
	}
	if(sections){
		*sections+=n;
	}
	return n;
}
/*光照/高度（区块级）：范围碰到的区块删 Heightmaps 和光照，范围外的一律不动（用户 2026-11-21：以前“3×3 邻居也删光照”是给 chunkunit 定的，已废弃）*/
static bool cleanup_Light(Level &lv,const std::string &dim,const Block_Range &rg,const std::vector<Fix_Region> &regions,Fix_Count &ct){
	each_Chunk(lv,dim,rg,regions,"光照",false,ct,[&](int32_t cx,int32_t cz){
		Chunk *c=lv.get_Chunk(dim.c_str(),cx,cz);
		if(!c){
			if(lv.miss_why==Level::GET_BAD_CHUNK){
				ct.failed++;/*坏块前面就该删掉了*/
			}
			return;/*自定义压缩等：修NBT 阶段已经记过*/
		}
		if(!is_ChunkVersionKnown(get_ChunkDataVersion(&c->con))){
			return;/*格式不认识：不动*/
		}
		if(!check_ChunkBase(&c->con)){/*本该在 cleanup_Chunks 删掉；这里兜底再删一次*/
			printf("fixer: 光照区块(%d,%d)结构损坏，整块删除\n",cx,cz);
			if(lv.erase_Chunk(dim.c_str(),cx,cz)){
				ct.corrupt_deleted++;
			}else{
				ct.failed++;
			}
			return;
		}
		if(nbt_Compound *b=get_ChunkBody(&c->con)){
			b->erase("Heightmaps");
		}
		ct.height_chunks++;
		int64_t n=0;
		strip_Light(c,&n);
		if(!lv.set_Chunk(dim.c_str(),cx,cz,*c)){
			ct.failed++;
		}else{
			ct.light_chunks++;
		}
	});
	return true;
}
/*这条计划刻和这格方块对得上吗：方块/流体知识在插件里（check_Tick），主机只把方块交过去*/
static bool check_TickBlock(const Rules_Api *api,const Block &b,bool fluid,const std::string &type){
	if(!api->check_Tick){
		return true;
	}
	Blk_Info o;
	o.name=b.name();
	o.props=b.properties.empty()?NULL:&b.properties;
	api->fill_BlkInfo(&o);
	return api->check_Tick(&o,fluid,type);
}
struct Tick_Pos{/*一格的坐标，当 map 的键用（计划刻去重、POI 按位置对账都用它）*/
	int32_t x,y,z;
	bool operator<(const Tick_Pos &o)const{
		if(x!=o.x){
			return x<o.x;
		}
		if(y!=o.y){
			return y<o.y;
		}
		return z<o.z;
	}
};
struct Tick_Rec{/*一条计划刻读出来的样子，加上判出来的几个结论*/
	int32_t x=0,y=0,z=0,t=0,p=0;
	std::string type;
	bool in=false;/*在写范围内？范围外只读*/
	bool known=true;/*方块读得出来？读不出来时不知道记录对不对，保留不参与选优*/
	bool valid=true;/*指向的方块和记录类型对得上？*/
	bool keep=true;
};
/*计划刻（单条记录是方块级的，严格按范围）：坏的单条删掉；范围里指着的方块对不上的删掉；同一格重复的只留一条（p 最小，p 相同 t 最大）*/
static bool cleanup_Ticks(Level &lv,const std::string &dim,const Block_Range &rg,const std::vector<Fix_Region> &regions,
	const Rules_Api *api,Fix_Count &ct){
	each_Chunk(lv,dim,rg,regions,"计划刻",false,ct,[&](int32_t cx,int32_t cz){
		/*钉住当前区块：下面查方块可能把别的区块挤出去，当前这个不能悬空*/
		Chunk *c=lv.pin_Chunk(dim.c_str(),cx,cz);
		if(!c){
			if(lv.miss_why==Level::GET_BAD_CHUNK){
				ct.failed++;/*坏块前面就该删掉了*/
			}
			return;/*自定义压缩等：修NBT 阶段已经记过*/
		}
		if(!is_ChunkVersionKnown(get_ChunkDataVersion(&c->con))){
			lv.unpin_Chunk(dim.c_str(),cx,cz);
			return;/*格式不认识：不动*/
		}
		bool judge=is_RulesChunk(c,api);
		if(!check_ChunkBase(&c->con)){
			ct.failed++;
			lv.unpin_Chunk(dim.c_str(),cx,cz);
			return;
		}
		bool ok=true,changed=false;
		Chunk_Layout tl=get_ChunkLayout(&c->con);
		nbt_Compound *tb=get_ChunkBody(&c->con);
		for(int k=0;tb&&ok&&k<2;k++){
			bool fluid=(k==1);
			const char *key=fluid?tl.fluid_ticks:tl.block_ticks;/*TileTicks/LiquidTicks（老格式）*/
			nbt_Any *ta=tb->get(key);
			if(!ta){
				continue;
			}
			if(ta->type!=NBT_LIST){/*游戏 ERROR、读不进来：删这个成员（修复阶段已经删过，范围外邻居才会走到这）*/
				tb->erase(key);
				ct.tick_lists++;
				changed=true;
				continue;
			}
			nbt_List *list=(nbt_List*)ta;
			/*计划刻信息不全：只删这一条记录；表本身不是List才是区块级错误*/
			int bad=0;
			for(size_t i=list->con.size();i>0;i--){
				Chunk_Tick t;
				if(!get_ChunkTick(&c->con,fluid,(int)(i-1),&t)){
					list->erase(i-1);
					bad++;
				}
			}
			if(bad){
				ct.ticks_bad+=bad;
				ct.ticks_deleted+=bad;
				changed=true;
				printf("fixer: 区块(%d,%d) %s 删坏记录 %d 条\n",cx,cz,key,bad);
			}
			if(!list->con.empty()&&list->elem!=NBT_COMPOUND){
				list->elem=NBT_COMPOUND;/*记录都还在，只是表的元素类型字段坏了*/
				changed=true;
			}else if(list->con.empty()&&list->elem!=NBT_END&&list->elem!=NBT_COMPOUND){
				list->elem=NBT_COMPOUND;
				changed=true;
			}
			std::vector<Tick_Rec> recs(list->con.size());
			for(size_t i=0;i<list->con.size();i++){
				Chunk_Tick t;
				if(!get_ChunkTick(&c->con,fluid,(int)i,&t)){
					ct.failed++;
					ok=false;
					break;
				}
				recs[i].x=t.x;recs[i].y=t.y;recs[i].z=t.z;
				recs[i].t=t.delay;recs[i].p=t.priority;
				if(!t.type.empty()){
					recs[i].type.assign(t.type.data(),t.type.size());
				}
				recs[i].in=rg.is_Inside(recs[i].x,recs[i].y,recs[i].z);
			}
			if(!ok){
				break;
			}
			for(size_t i=0;i<recs.size();i++){
				if(!recs[i].in){
					continue;/*范围外只读：不判、不去重*/
				}
				if(!judge){
					recs[i].known=false;/*区块版本和规则的不一样：不知道方块和刻对不对得上，保留，不参与选优*/
					continue;
				}
				Block b=lv.get_Block(dim.c_str(),recs[i].x,recs[i].y,recs[i].z);
				if(!b.valid()){
					int why=lv.miss_why;
					if(is_ChunkUnreadable(why)){
						recs[i].known=false;/*读不出来：不知道对不对，保留，不参与选优*/
					}else{
						recs[i].valid=false;/*方块不存在（含没有区域文件）：记录指向空气，删*/
					}
					continue;
				}
				recs[i].valid=check_TickBlock(api,b,fluid,recs[i].type);
			}
			/*同一位置删掉对不上的记录后还重复：计划刻选p最小、p相同选t最大的一条*/
			std::map<Tick_Pos,int> pick;
			for(size_t i=0;i<recs.size();i++){
				if(!recs[i].in||!recs[i].known||!recs[i].valid){
					continue;
				}
				Tick_Pos pos{recs[i].x,recs[i].y,recs[i].z};
				auto it=pick.find(pos);
				if(it==pick.end()){
					pick.emplace(pos,(int)i);
					continue;
				}
				const Tick_Rec &old=recs[(size_t)it->second];
				if(recs[i].p<old.p||(recs[i].p==old.p&&recs[i].t>old.t)){
					it->second=(int)i;
				}
			}
			int mism=0,dups=0;
			for(size_t i=0;i<recs.size();i++){
				if(!recs[i].in){
					continue;
				}
				if(!recs[i].known){
					continue;/*未知记录不删，也不能让未知记录把合法记录挤掉*/
				}
				if(!recs[i].valid){
					recs[i].keep=false;
					mism++;
					continue;
				}
				Tick_Pos pos{recs[i].x,recs[i].y,recs[i].z};
				auto it=pick.find(pos);
				if(it==pick.end()||it->second!=(int)i){
					recs[i].keep=false;
					dups++;
				}
			}
			int del=0;
			for(size_t i=recs.size();i>0;i--){
				if(!recs[i-1].keep){
					list->erase(i-1);
					del++;
				}
			}
			if(del){
				ct.ticks_deleted+=mism;
				ct.ticks_dup+=dups;
				changed=true;
			}
		}
		if(ok&&changed&&!lv.set_Chunk(dim.c_str(),cx,cz,*c)){
			ct.failed++;
		}
		lv.unpin_Chunk(dim.c_str(),cx,cz);
	});
	return true;
}

/*==================== POI ====================*/
/*POI 的记录和方块必须一一对应。fixer 的做法：
  · 先把这一块里“应该产生POI”的方块按 rules 的表扫出来（床只认head，工作站/钟/蜂巢/磁石/避雷针/传送门…）；
  · 写范围内每个位置只允许一条记录：类型不对/票数越界/重复的全部删掉，缺失的按 rules 的默认值补；
  · 范围外的记录原样保留（单条记录是方块级的，严格按范围）；
  · POI 区块里解不出来的段按段删（库的 repair_POI），读不进来的单条记录按条删；不整块重建。*/
struct Poi_Rec{int32_t x,y,z;const Poi_Info *info;};/*方块扫出来的：这一格应该有什么 POI*/
struct Poi_Seen{std::string type;int32_t ft=0;};/*POI 文件里读到的一条记录（类型、剩余票数）*/
static bool is_MinecraftName(std::string_view n){/*minecraft:xxx或没有命名空间的旧写法*/
	size_t p=n.find(':');
	return p==std::string_view::npos||(p==9&&n.substr(0,9)=="minecraft");
}
static bool scan_POIBlocks(Chunk *c,int32_t cx,int32_t cz,const Block_Range &rg,const Rules_Api *api,
	std::vector<Poi_Rec> &out){
	if(!c){
		return true;
	}
	nbt_Any *sa=get_ChunkSections(c);
	if(!sa||sa->type!=NBT_LIST){
		return true;/*没有段（不是 List 游戏也当没有）：没有要 POI 的方块*/
	}
	nbt_List *secs=(nbt_List*)sa;
	for(size_t k=0;k<secs->con.size();k++){
		nbt_Any *a=secs->con[k];
		if(!a||a->type!=NBT_COMPOUND){
			continue;/*游戏跳过不是compound的段*/
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		int32_t sy=read_SectionY(sec);
		if(c->get_Section(sy)!=sec){/*同一Y有几段：和读方块一样只认索引里那段（最后那段，游戏取的就是它）*/
			continue;
		}
		Fix_Sec s;
		build_Sec(s,c,sy,api,0xFFFFFFFFu);
		if(s.bad){
			if(s.kind==STATES_AIR){
				continue;/*block_states 不是 compound 之类：游戏当整段空气*/
			}
			return false;/*游戏读得了、库不读（或者游戏读不了）：不知道有没有要 POI 的方块，这块的 POI 不动*/
		}
		if(!s.box||!s.palette||s.palette->con.empty()||!s.poi_cand){
			continue;
		}
		int64_t wx=(int64_t)cx*16,wz=(int64_t)cz*16;
		for(uint32_t i=0;i<4096;i++){
			uint32_t idx=get_SecIndex(s,i);
			if(idx==IDX_BAD||idx>=s.poi.size()||!s.poi[idx]){
				continue;
			}
			int32_t lx=(int32_t)(i&15);
			int32_t lz=(int32_t)((i>>4)&15);
			int32_t ly=(int32_t)(i>>8);
			int64_t x=wx+lx,y=(int64_t)sy*16+ly,z=wz+lz;
			if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||z<INT32_MIN||z>INT32_MAX){
				continue;
			}
			if(!rg.is_Inside(x,y,z)){
				continue;
			}
			out.push_back({(int32_t)x,(int32_t)y,(int32_t)z,s.poi[idx]});
		}
	}
	return true;
}
/*扫一遍POI记录（新老写法都走库的 get_POILists）：合法且写范围内的进seen；写范围内游戏读不进来的单条记录从原表里删掉（只删这一条）；
  范围外的一律不动。返回删掉的坏记录数。解不出来的段先由库的 repair_POI 按段删（游戏 ERROR、整段不要）。*/
static int scan_POIRecords(nbt_Compound *pc,const Block_Range &rg,std::map<Tick_Pos,std::vector<Poi_Seen>> &seen){
	int bad=0;
	std::vector<nbt_List*> lists=get_POILists(pc);
	for(nbt_List *recs:lists){
		for(size_t i=recs->con.size();i>0;i--){
			nbt_Any *e=recs->con[i-1];
			POI p;
			bool full=get_POIRecord(e,&p);
			int32_t px=0,py=0,pz=0;
			bool has_pos=get_POIRecordPos(e,&px,&py,&pz);
			if(!has_pos){/*连位置都读不出来：不知道在不在范围里，游戏反正不要这一条*/
				recs->erase(i-1);
				bad++;
				continue;
			}
			if(!rg.is_Inside(px,py,pz)){
				continue;/*范围外：合法/不合法都只读，不替人作主*/
			}
			if(!full){
				recs->erase(i-1);
				bad++;
				continue;
			}
			seen[Tick_Pos{p.x,p.y,p.z}].push_back({p.type.con,p.free_tickets});
		}
		if(recs->con.empty()&&recs->elem!=NBT_END&&recs->elem!=NBT_COMPOUND){
			recs->elem=NBT_COMPOUND;
		}
	}
	return bad;
}
static bool cleanup_POI(Level &lv,const std::string &dim,const Block_Range &rg,const std::set<uint64_t> &deleted,
	const Rules_Api *api,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"poi");
	if(rel.empty()){
		return false;
	}
	if(!has_POIFile(lv.data_version)){
		return true;/*19w09a（1935）以前的版本（1.13 在内）没有 POI：游戏不读 poi/，不补也不删*/
	}
	/*要看的区域：region 文件在的（可能要补 POI）并上 POI 文件在的（可能要删孤儿）*/
	std::set<std::pair<int32_t,int32_t>> todo;
	for(const Fix_Region &r:list_Files(lv,dim,"region",rg)){
		todo.insert({r.rx,r.rz});
	}
	for(const Fix_Region &r:list_Files(lv,dim,"poi",rg)){
		todo.insert({r.rx,r.rz});
	}
	Progress pr("POI",(int64_t)todo.size());
	for(const std::pair<int32_t,int32_t> &rp:todo){
		int32_t rx=rp.first,rz=rp.second;
		{
			pr.tick();
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
			/*先看这个区域的 region 文件：它和 POI 文件都不在就整个跳过；region 读不出来也整个跳过（不知道哪些区块在）*/
			std::bitset<1024> has_region;
			if(!read_RegionSlots(lv,dim,rx,rz,has_region)){
				continue;/*修NBT 阶段已经记过*/
			}
			struct stat fst;
			if(has_region.none()&&stat(file.c_str(),&fst)){
				continue;
			}
			MCA m(file);
			if(m.bad){
				printf("fixer: POI %s 读不出来，跳过（只记日志）\n",file.c_str());
				ct.io_skipped++;
				continue;
			}
			/*0字节的POI文件：库照游戏当空区域读（不算坏）。有要补的记录就在下面重建出来，没有的话这个空文件要删掉。
			  （以前这里是靠 m.bad 认0字节文件的；库改成照游戏读以后那条路走不到了，空文件要等到最后 format 那一步才删）*/
			struct stat pst;
			bool zero_file=!stat(file.c_str(),&pst)&&S_ISREG(pst.st_mode)&&pst.st_size==0;
			if(zero_file){
				printf("fixer: POI %s 是空文件，按空区域重建\n",file.c_str());
			}
			bool changed=false;
			int32_t cx0=std::max(rg.get_CX0(),rx*32),cx1=std::min(rg.get_CX1(),rx*32+31);
			int32_t cz0=std::max(rg.get_CZ0(),rz*32),cz1=std::min(rg.get_CZ1(),rz*32+31);
			for(int32_t cx=cx0;cx<=cx1;cx++){
				for(int32_t cz=cz0;cz<=cz1;cz++){
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					if(!has_region[lx+lz*32]&&!m.has_Chunk(lx,lz)){
						continue;/*region 和 POI 都没有这一格*/
					}
					if(deleted.count(get_ChunkKey(cx,cz))){
						if(m.has_Chunk(lx,lz)){
							if(m.erase_Chunk(lx,lz)){
								ct.poi_chunks++;
								changed=true;
							}else{
								ct.failed++;
							}
						}
						continue;
					}
					Chunk *c=lv.pin_Chunk(dim.c_str(),cx,cz);
					int why=lv.miss_why;
					if(!c&&(why==Level::GET_NO_FILE||why==Level::GET_NO_CHUNK)){
						/*region里没有这个区块：POI也不该有。整块POI删掉，不新建region。*/
						if(m.has_Chunk(lx,lz)){
							if(m.erase_Chunk(lx,lz)){
								ct.poi_chunks++;
								changed=true;
							}else{
								ct.failed++;
							}
						}
						continue;
					}
					if(!c&&(why==Level::GET_BAD_FILE||why==Level::GET_UNSUPPORTED_COMPRESSION)){
						printf("fixer: POI对应的区块(%d,%d)读不出来，跳过（只记日志）\n",cx,cz);
						ct.io_skipped++;
						continue;
					}
					if(!c&&why==Level::GET_BAD_CHUNK){
						printf("fixer: POI对应的区块(%d,%d) NBT损坏，等待区块清理删除\n",cx,cz);
						ct.failed++;
						continue;
					}
					if(!c){
						/*别的原因取不到（缓存腾不出位置、落盘失败…）：不知道这块里有哪些方块要 POI，不动。
						  以前漏了这一条，会走到下面当成“一个要 POI 的方块都没有”，把范围里的 POI 记录全删掉*/
						printf("fixer: POI对应的区块(%d,%d)取不到（原因 %d），跳过\n",cx,cz,why);
						ct.failed++;
						continue;
					}
					if(!is_RulesChunk(c,api)){
						lv.unpin_Chunk(dim.c_str(),cx,cz);
						continue;/*区块版本和规则的不一样（或者格式不认识）：不知道哪些方块要 POI，POI 不动*/
					}
					std::vector<Poi_Rec> want_recs;
					bool scan_ok=scan_POIBlocks(c,cx,cz,rg,api,want_recs);
					lv.unpin_Chunk(dim.c_str(),cx,cz);
					if(!scan_ok){
						printf("fixer: 区块(%d,%d) 的方块数据坏了，POI不动\n",cx,cz);
						ct.failed++;
						continue;
					}
					std::map<Tick_Pos,const Poi_Info*> want;
					for(size_t i=0;i<want_recs.size();i++){
						want[Tick_Pos{want_recs[i].x,want_recs[i].y,want_recs[i].z}]=want_recs[i].info;
					}
					bool had=m.has_Chunk(lx,lz);
					nbt_Compound *pc=read_MCACompound(m,lx,lz);
					if(!pc&&m.last_error==MCA_READ_UNSUPPORTED_COMPRESSION){
						printf("fixer: POI区块(%d,%d) 是模组的自定义压缩，跳过（只记日志）\n",cx,cz);
						ct.io_skipped++;
						continue;
					}
					if(pc){
						/*POI 的格式/类型也按版本（游戏读的时候先升级：SectionStorage 缺 DataVersion 当 1945；1946 ReorganizePoi、
						  2209 bee_hive 改名、2218 ForcePoiRebuild、3097 删 unemployed/nitwit…）。和规则的版本不一样就不动：
						  写回去会把 DataVersion 改成新的，游戏就不再升级它了*/
						int32_t pdv=get_TagInt(pc->get("DataVersion")).value_or(1945);
						if(pdv!=lv.data_version){
							printf("fixer: POI区块(%d,%d) DataVersion=%d 和规则的 %d 不一样，跳过不动\n",cx,cz,pdv,lv.data_version);
							ct.ver_skipped++;
							pc->free();
							continue;
						}
					}
					bool dirty=false;
					std::map<Tick_Pos,std::vector<Poi_Seen>> seen;
					int badrec=0;
					if(pc){
						int unreadable=prune_NBTUnreadable(pc,512);
						int badsec=repair_POI(pc);
						if(unreadable||badsec){
							ct.nbt_bad+=unreadable;
							ct.poi_corrupt+=badsec;
							dirty=true;
							printf("fixer: POI区块(%d,%d) 删读不了的tag %d 个、坏段 %d 段\n",cx,cz,unreadable,badsec);
						}
					}
					if(pc){
						/*单条读不进来的记录按条删；合法的收进 seen，下面和方块对账*/
						badrec=scan_POIRecords(pc,rg,seen);
						if(badrec){
							ct.poi_bad+=badrec;
							dirty=true;
							printf("fixer: POI区块(%d,%d) 删坏记录 %d 条\n",cx,cz,badrec);
						}
					}
					auto new_pc=[&](){/*这一块还没有 POI 区块、又有记录要补：按规则的版本建一个空的（带 DataVersion，写法跟版本走）*/
						return new nbt_Compound(make_POIChunk(lv.data_version));
					};
					int del=0,add=0,dups=0;
					for(auto &kv:seen){
						const Tick_Pos &pos=kv.first;
						const std::vector<Poi_Seen> &recs=kv.second;
						auto wi=want.find(pos);
						bool missing=(wi==want.end());
						const Poi_Info *info=missing?NULL:wi->second;
						if(missing){/*范围里某个位置没有rules默认值：先看它是不是mod方块/未知POI，别误删*/
							Block b=lv.get_Block(dim.c_str(),pos.x,pos.y,pos.z);
							bool keep=!b.valid()&&(lv.miss_why==Level::GET_BAD_FILE||lv.miss_why==Level::GET_BAD_CHUNK);
							if(b.valid()&&!is_MinecraftName(b.name())){
								keep=true;
							}
							for(size_t i=0;i<recs.size();i++){
								if(!is_MinecraftName(recs[i].type)){
									keep=true;
								}
							}
							if(keep){
								continue;
							}
						}
						bool need=missing||recs.size()>1;
						bool has_valid=false;
						int32_t best=INT32_MAX;
						for(size_t i=0;i<recs.size();i++){
							bool valid=info&&recs[i].type==info->type&&recs[i].ft>=0&&recs[i].ft<=info->max_tickets;
							if(valid){
								has_valid=true;
								if(recs[i].ft<best){
									best=recs[i].ft;
								}
							}else{
								need=true;
							}
						}
						if(!need){
							continue;
						}
						if(pc){
							del+=erase_POI(pc,pos.x,pos.y,pos.z);
						}
						if(!missing){
							if(!pc){
								pc=new_pc();
							}
							int32_t ft=has_valid?best:info->free_tickets;
							if(!set_POI(pc,pos.x,pos.y,pos.z,POI(info->type,ft))){
								ct.failed++;
							}else{
								add++;
							}
						}
						if(recs.size()>1){
							dups+=(int)recs.size()-1;
						}
						dirty=true;
					}
					for(auto &kv:want){
						const Tick_Pos &pos=kv.first;
						if(seen.count(pos)){
							continue;/*上面已经按“缺/重复/错类型”处理过*/
						}
						if(!pc){
							pc=new_pc();
						}
						if(!set_POI(pc,pos.x,pos.y,pos.z,POI(kv.second->type,kv.second->free_tickets))){
							ct.failed++;
						}else{
							add++;
						}
						dirty=true;
					}
					if(pc){
						int n=count_POI(pc);
						if(n==0){
							if(had){
								if(m.erase_Chunk(lx,lz)){
									ct.poi_chunks++;
									changed=true;
								}else{
									ct.failed++;
								}
							}
						}else if(dirty){
							pc->set_Int("DataVersion",lv.data_version);/*POI根必须有DataVersion*/
							if(!m.set_Compound(*pc,lx,lz)){
								ct.failed++;
							}else{
								changed=true;
							}
						}
						pc->free();
					}else if(had){
						if(m.erase_Chunk(lx,lz)){
							ct.poi_chunks++;
							changed=true;
						}else{
							ct.failed++;
						}
					}
					ct.poi_deleted+=del;
					ct.poi_added+=add;
					ct.poi_dup+=dups;
				}
			}
			if(!changed){
				if(zero_file){/*0字节POI文件即使没有要补的记录也删掉，不能留下空文件*/
					if(remove(file.c_str())&&errno!=ENOENT){
						ct.failed++;
					}else{
						ct.mca_deleted++;
					}
				}
				continue;
			}
			std::string dir=lv.path+"/"+rel;
			if(m.is_Empty()){
				if(remove(file.c_str())&&errno!=ENOENT){
					ct.failed++;
				}else{
					ct.poi_files++;
				}
			}else{
				if(!make_Dir(dir)||!m.save(file.c_str())){
					ct.failed++;
				}
			}
		}
	}
	pr.finish();
	return true;
}

/*==================== 实体 UUID ====================*/
/*查重用的 UUID 键：照游戏 UUIDUtil.CODEC（Codec.INT_STREAM，Int_Array 或者 Byte/Long_Array、数值列表都认）正好 4 个数。
  没有 UUID 给 false（游戏随机给一个，不参与查重）。entities/ 的实体区块都是 2681 以后的，老的 UUIDMost/UUIDLeast 游戏不看，这里也不认
  （2026-11-21 以前自己写了一份读法，只认 Int_Array 和数值列表：Long_Array 写的重复 UUID 查不出来）*/
static bool get_UUIDKey(const nbt_Compound *e,std::string &key){
	std::vector<int32_t> v;
	if(!e||!get_IntStream(e->get("UUID"),&v)||v.size()!=4){
		return false;
	}
	key.assign((const char*)v.data(),16);
	return true;
}
/*乘客：游戏 loadEntityRecursive 一个个读，读不进来的（WARN）只丢那一个；UUID 重复的后加的加不进去（WARN）*/
static int prune_Passengers(nbt_Compound *e,std::set<std::string> &seen,int *bad){
	nbt_Any *pa=e->get("Passengers");
	if(!pa){
		return 0;
	}
	if(pa->type!=NBT_LIST){
		e->erase("Passengers");
		(*bad)++;
		return 1;
	}
	nbt_List *l=(nbt_List*)pa;
	int n=0;
	for(size_t i=0;i<l->con.size();){
		nbt_Any *p=l->con[i];
		if(!check_EntityRecord(p)){
			l->erase(i);
			n++;
			(*bad)++;
			continue;
		}
		std::string key;
		if(get_UUIDKey((nbt_Compound*)p,key)&&!seen.insert(key).second){
			l->erase(i);
			n++;
			continue;
		}
		n+=prune_Passengers((nbt_Compound*)p,seen,bad);
		i++;
	}
	return n;
}
/*实体：一个区域文件一个区域文件过（没有实体文件的区域跳过）；region 里没有的区块同格实体删掉，坏的单条删掉，UUID 重复的删后出现的。
  区块在不在只看 region 表头（不用把区块读出来）；单条实体是方块级的，按 Pos 严格卡范围*/
static bool cleanup_Entities(Level &lv,const std::string &dim,const Block_Range &rg,const std::set<uint64_t> &deleted,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"entities");
	if(rel.empty()){
		return false;
	}
	std::vector<Fix_Region> files=list_Files(lv,dim,"entities",rg);/*只走实体文件在的区域*/
	Progress pr("实体",(int64_t)files.size());
	std::set<std::string> seen;
	for(const Fix_Region &fr:files){
		int32_t rx=fr.rx,rz=fr.rz;
		{
			pr.tick();
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
			struct stat st;
			if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)){
				continue;/*没有实体文件：这个区域跳过*/
			}
			if(st.st_size==0){/*0字节实体文件：直接删，不能留*/
				if(remove(file.c_str())&&errno!=ENOENT){
					ct.failed++;
				}else{
					ct.mca_deleted++;
				}
				continue;
			}
			MCA m(file);
			if(m.bad){
				printf("fixer: 实体 %s 读不出来，跳过（只记日志）\n",file.c_str());
				ct.io_skipped++;
				continue;
			}
			std::bitset<1024> has_region;
			if(!read_RegionSlots(lv,dim,rx,rz,has_region)){
				continue;/*region 文件读不出来：不知道哪些区块在，整个区域的实体不动（修NBT 阶段已经记过）*/
			}
			bool changed=false;
			int32_t cx0=std::max(rg.get_CX0(),rx*32),cx1=std::min(rg.get_CX1(),rx*32+31);
			int32_t cz0=std::max(rg.get_CZ0(),rz*32),cz1=std::min(rg.get_CZ1(),rz*32+31);
			for(int32_t cx=cx0;cx<=cx1;cx++){
				for(int32_t cz=cz0;cz<=cz1;cz++){
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					if(!m.has_Chunk(lx,lz)){
						continue;/*这一格没有实体区块（偏移异常的也算没有，收尾整理时清掉）*/
					}
					if(!has_region[lx+lz*32]||deleted.count(get_ChunkKey(cx,cz))){/*region没有这个区块：实体也不该有。整块实体删掉，不新建region*/
						if(m.erase_Chunk(lx,lz)){
							ct.entity_chunks++;
							changed=true;
						}else{
							ct.failed++;
						}
						continue;
					}
					nbt_Compound *ec=read_MCACompound(m,lx,lz);
					if(!ec){
						if(m.last_error==MCA_READ_UNSUPPORTED_COMPRESSION){
							printf("fixer: 实体区块(%d,%d) 是模组的自定义压缩，跳过（只记日志）\n",cx,cz);
							ct.io_skipped++;
							continue;
						}
						if(m.has_Chunk(lx,lz)){/*表头有、NBT/压缩坏了：整块实体删掉，不能留给MC*/
							printf("fixer: 实体区块(%d,%d) NBT损坏，整块删除\n",cx,cz);
							if(m.erase_Chunk(lx,lz)){
								ct.entity_chunks++;
								changed=true;
							}else{
								ct.failed++;
							}
						}
						continue;
					}
					/*容器照游戏（EntityStorage.loadEntities）：Position 坏了 WARN、对不上 ERROR，实体都照载 → 按文件位置重写；
					  Entities 不是 List 是 WARN（当没有）→ 删掉这个成员；NbtIo 读不了的 tag 先按最小单位删*/
					bool elem_fixed=false;
					int unreadable=prune_NBTUnreadable(ec,512);
					if(unreadable){
						ct.nbt_bad+=unreadable;
						elem_fixed=true;
					}
					const nbt_Any *pos=ec->get("Position");
					if(!pos||pos->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)pos)->con.size()!=2||
					   ((const nbt_Int_Array*)pos)->con[0]!=cx||((const nbt_Int_Array*)pos)->con[1]!=cz){
						ec->set("Position",new nbt_Int_Array(std::vector<int32_t>{cx,cz}));
						ct.coord_fix++;
						elem_fixed=true;
					}
					nbt_Any *ea=ec->get("Entities");
					if(ea&&ea->type!=NBT_LIST){
						ec->erase("Entities");
						ea=NULL;
						ct.entities_bad++;
						elem_fixed=true;
					}
					if(!ea){
						ec->set("Entities",new nbt_List(NBT_COMPOUND));
					}
					nbt_List *es=(nbt_List*)ec->get("Entities");
					int dropped=0,bad=0;
					/*从前往后：UUID重复时保留先出现的，删后面的。原地erase后下标不前进。*/
					for(size_t i=0;i<es->con.size();){
						nbt_Any *e=es->con[i];
						bool in_range=is_EntityInRange(e,rg);
						if(!check_EntityRecord(e)){
							if(!in_range){
								i++;/*范围外只读，不替人作主*/
								continue;
							}
							es->erase(i);
							dropped++;
							bad++;
							continue;
						}
						if(!in_range){
							i++;
							continue;
						}
						std::string key;
						if(get_UUIDKey((nbt_Compound*)e,key)&&!seen.insert(key).second){/*没有 UUID 的游戏随机给一个：不删*/
							es->erase(i);
							dropped++;
							continue;
						}
						dropped+=prune_Passengers((nbt_Compound*)e,seen,&bad);
						i++;
					}
					if(!es->con.empty()&&es->elem!=NBT_COMPOUND){
						es->elem=NBT_COMPOUND;/*记录还在，只是元素类型字段坏了*/
						elem_fixed=true;
						changed=true;
					}
					if(dropped){
						ct.entities_deleted+=dropped;
						ct.entities_bad+=bad;
					}
					if(es->con.empty()){
						ec->free();
						if(m.has_Chunk(lx,lz)){
							if(m.erase_Chunk(lx,lz)){
								ct.entity_chunks++;
								changed=true;
							}else{
								ct.failed++;
							}
						}
						continue;
					}
					if(dropped||elem_fixed){
						if(!m.set_Compound(*ec,lx,lz)){
							ct.failed++;
						}else{
							changed=true;
						}
					}
					ec->free();
				}
			}
			if(changed){
				if(m.is_Empty()){
					if(remove(file.c_str())&&errno!=ENOENT){
						ct.failed++;
					}else{
						ct.entity_files++;
					}
				}else if(!m.save(file.c_str())){
					ct.failed++;
				}
			}
		}
	}
	pr.finish();
	return true;
}

/*==================== 收尾：整理文件（区域文件级） ====================*/
/*整理一个 region/poi/entities 文件：0字节的删；表头上偏移异常的格子清成 0（游戏打开文件时也当它们没有）；一个区块都没有的删文件；
  chunks=true（region 文件）时范围碰到的区块在这里做 format_Chunk——整趟只做这一遍，放在删完废弃的格子、空文件之后，不白做；
  最后 MCA::format 压紧、落盘*/
static void format_RegionFile(const std::string &file,int32_t rx,int32_t rz,bool chunks,const Block_Range &rg,Fix_Count &ct){
	struct stat st;
	if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)){
		return;
	}
	if(st.st_size==0){/*0字节的 region/poi/entities 文件一律删掉，不能留*/
		if(remove(file.c_str())&&errno!=ENOENT){
			ct.failed++;
		}else{
			ct.mca_deleted++;
		}
		return;
	}
	MCA m(file);
	if(m.bad){
		printf("fixer: %s 读不出来，不整理（只记日志）\n",file.c_str());
		ct.io_skipped++;
		return;
	}
	int cleaned=m.clean_Offsets();
	if(cleaned){
		printf("fixer: %s 表头上有 %d 格偏移异常，清成 0\n",file.c_str(),cleaned);
		ct.off_cleaned+=cleaned;
	}
	if(m.is_Empty()){
		if(remove(file.c_str())&&errno!=ENOENT){
			ct.failed++;
		}else{
			ct.mca_deleted++;
		}
		return;
	}
	if(chunks){
		int32_t cx0=std::max(rg.get_CX0(),rx*32),cx1=std::min(rg.get_CX1(),rx*32+31);
		int32_t cz0=std::max(rg.get_CZ0(),rz*32),cz1=std::min(rg.get_CZ1(),rz*32+31);
		for(int32_t cx=cx0;cx<=cx1;cx++){
			for(int32_t cz=cz0;cz<=cz1;cz++){
				uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
				if(!m.has_Chunk(lx,lz)){
					continue;
				}
				std::optional<nbt_Compound> raw=m.get_Compound(lx,lz);
				if(!raw){
					continue;/*读不出来的（自定义压缩等）前面已经记过*/
				}
				Chunk c(std::move(*raw));
				if(!is_ChunkVersionKnown(get_ChunkDataVersion(&c.con))){
					continue;
				}
				int bad_y=0,palette=0;
				int dup=c.format_Chunk(&bad_y,&palette);/*Y改成Byte、重复Y留最后一段、调色板压紧；sections 不是 List 给-1（游戏当没有段，不动）*/
				if(dup<=0&&!bad_y&&!palette){
					continue;
				}
				printf("fixer: 区块(%d,%d) 整理：改Y %d段、重复Y %d段、调色板 %d段\n",cx,cz,bad_y,dup,palette);
				if(!m.set_Compound(c.con,lx,lz)){
					ct.failed++;
					continue;
				}
				ct.sec_dup+=dup;
				ct.sec_y_fixed+=bad_y;
				ct.palette_cleaned+=palette;
			}
		}
	}
	if(!m.format()){
		printf("fixer: %s 整理失败，不动它\n",file.c_str());
		ct.failed++;
		return;
	}
	if(!m.save(file.c_str())){
		ct.failed++;
		return;
	}
	ct.mca_formatted++;
}
/*0字节region先删掉：游戏把它当空区域（一块区块都没有），删了也一样，后面“region里没有这块”的
  POI/实体清理照常进行。只动范围碰到的region文件；删失败按处理失败记，后面照旧跳过。*/
static void remove_ZeroRegions(Level &lv,const std::string &dim,const Block_Range &rg,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"region");
	if(rel.empty()){
		return;
	}
	for(const Fix_Region &r:list_Files(lv,dim,"region",rg)){
		{
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),r.rx,r.rz);
			struct stat st;
			if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)||st.st_size!=0){
				continue;
			}
			if(remove(file.c_str())&&errno!=ENOENT){
				ct.failed++;
			}else{
				ct.mca_deleted++;
			}
		}
	}
}
static bool format_Regions(Level &lv,const std::string &dim,const Block_Range &rg,Fix_Count &ct){/*范围碰到的 region/poi/entities 文件各整理一遍（文件不在的区域跳过）*/
	const char *kinds[3]={"region","poi","entities"};
	std::vector<Fix_Region> files[3];
	int64_t total=0;
	for(int k=0;k<3;k++){
		files[k]=list_Files(lv,dim,kinds[k],rg);
		total+=(int64_t)files[k].size();
	}
	Progress pr("整理",total);
	for(int k=0;k<3;k++){
		std::string rel=lv.get_DimensionPath(dim.c_str(),kinds[k]);
		for(const Fix_Region &r:files[k]){
			pr.tick();
			format_RegionFile(get_MCAPath((lv.path+"/"+rel).c_str(),r.rx,r.rz),r.rx,r.rz,k==0,rg,ct);
		}
	}
	pr.finish();
	return true;
}

/*==================== 一个维度 ====================*/
static bool fix_Dim(const std::string &path,const std::string &dim,const Block_Range &rg,const Rules_Api *api){
	Level lv(path.c_str(),256,16,4);
	std::string rrel=lv.get_DimensionPath(dim.c_str(),"region");
	if(rrel.empty()){
		printf("fixer: [%s] 维度路径无效\n",dim.c_str());
		return false;
	}
	Fix_Count ct;
	printf("fixer: [%s] 方块 x[%d,%d] y[%d,%d] z[%d,%d]，区块 x[%d,%d] z[%d,%d]，区域 x[%d,%d] z[%d,%d]\n",
		dim.c_str(),rg.x0,rg.x1,rg.y0,rg.y1,rg.z0,rg.z1,rg.get_CX0(),rg.get_CX1(),rg.get_CZ0(),rg.get_CZ1(),
		rg.get_RX0(),rg.get_RX1(),rg.get_RZ0(),rg.get_RZ1());
	/*0字节region先删，后面的“region里没有这个区块”检查才能看见真实情况*/
	remove_ZeroRegions(lv,dim,rg,ct);
	/*范围碰到的区域里 region 文件在的那些：后面各阶段都只走它们*/
	std::vector<Fix_Region> regions;
	list_Regions(lv,dim,rg,regions);
	/*0. NBT 修复：游戏读档会报错的按最小单位删，规则看到的是修好的区块*/
	std::set<uint64_t> deleted;
	if(!repair_Chunks(lv,dim,rg,regions,ct,deleted)){
		ct.failed++;
	}
	if(!lv.save()){
		ct.failed++;
	}
	/*1. 物理*/
	{
		Fix_Job j(&lv,dim.c_str(),rg,api,0xFFFFFFFFu);
		each_Chunk(lv,dim,rg,regions,"物理",false,ct,[&](int32_t cx,int32_t cz){
			j.scan_Chunk(cx,cz);
		});
		if(j.st.flush_fail){
			ct.failed++;
		}
		if(!lv.save()){
			ct.failed++;
		}
		printf("fixer: [%s] 物理：区块 %lld，段 %lld/%lld，判了 %lld 格，拆 %lld（含水 %lld）、改状态 %lld、读不出 %lld、跳过 %lld；"
			"坏下标 %lld、坏段 %lld；水刻 +%lld/-%lld 失败 %lld；noop %lld；"
			"版本和规则不同跳过 %lld\n",
			dim.c_str(),(long long)j.st.chunks,(long long)j.st.sec_hit,(long long)j.st.sec_all,
			(long long)j.st.pos,(long long)j.st.air,(long long)j.st.wet,(long long)j.st.set,
			(long long)j.st.miss,(long long)j.st.blind,(long long)j.st.pos_bad,(long long)j.st.sec_bad,
			(long long)j.st.tick_add,(long long)j.st.tick_del,(long long)j.st.tick_fail,(long long)j.st.noop,
			(long long)j.st.ver_skip);
		/*sec_bad由后面的区块清理整块删除并计入corrupt_deleted，不算处理失败*/
		if(j.st.pos_bad||j.st.miss||j.st.tick_fail||j.st.flush_fail){
			ct.failed++;
		}
		for(int r=1;r<api->count_Rule();r++){
			if(j.st.rule_change[r]){
				printf("fixer: [%s]   规则 %-28s 改动 %lld\n",dim.c_str(),api->get_RuleName(r),(long long)j.st.rule_change[r]);
			}
		}
		if(!j.st.base_change.empty()){
			std::vector<std::pair<std::string,int64_t>> top(j.st.base_change.begin(),j.st.base_change.end());
			std::sort(top.begin(),top.end(),[](const std::pair<std::string,int64_t>&a,const std::pair<std::string,int64_t>&b){
				return a.second>b.second;
			});
			printf("fixer: [%s]   改得最多的方块：",dim.c_str());
			for(size_t i=0;i<top.size()&&i<12;i++){
				printf("%s%s×%lld",i?", ":"",top[i].first.c_str(),(long long)top[i].second);
			}
			printf("\n");
		}
	}
	/*2. 区块清理*/
	bool storage_ok=true;
	if(!cleanup_Chunks(lv,dim,rg,regions,ct,deleted)){
		ct.failed++;
	}
	if(!lv.save()){
		ct.failed++;
		storage_ok=false;/*region都没落盘：后面再删POI/实体/format会把三方弄不一致*/
	}
	/*3. 光照/高度*/
	if(storage_ok){
		if(!cleanup_Light(lv,dim,rg,regions,ct)){
			ct.failed++;
		}
		if(!lv.save()){
			ct.failed++;
			storage_ok=false;
		}
	}
	/*4. 计划刻*/
	if(storage_ok){
		if(!cleanup_Ticks(lv,dim,rg,regions,api,ct)){
			ct.failed++;
		}
		if(!lv.save()){
			ct.failed++;
			storage_ok=false;
		}
	}
	/*5. POI*/
	if(storage_ok){
		if(!cleanup_POI(lv,dim,rg,deleted,api,ct)){
			ct.failed++;
		}
		/*6. 实体*/
		if(!cleanup_Entities(lv,dim,rg,deleted,ct)){
			ct.failed++;
		}
	}
	/*7. 落盘，然后整理文件（清偏移异常的格子、删空文件、format_Chunk、压紧）*/
	if(!lv.close()){
		ct.failed++;
		storage_ok=false;
	}
	if(storage_ok){
		if(!format_Regions(lv,dim,rg,ct)){
			ct.failed++;
		}
	}
	printf("fixer: [%s] 修NBT：读不了的tag %lld、坏block_states %lld段、坏biomes %lld段、认不出的调色板条目 %lld、"
		"坏刻表 %lld、坏方块实体 %lld、坏结构引用 %lld、坏结构起点 %lld；"
		"坐标在别的区块的：迁走方块实体 %lld、计划刻 %lld，目标区块没有删 %lld、目标已有删 %lld、迁不走留着 %lld、改到本区块 %lld\n",
		dim.c_str(),(long long)ct.nbt_bad,(long long)ct.states_bad,(long long)ct.biomes_bad,(long long)ct.palette_air,
		(long long)ct.tick_lists,(long long)ct.be_bad,(long long)ct.refs_bad,(long long)ct.starts_bad,
		(long long)ct.be_moved,(long long)ct.tick_moved,(long long)ct.stray_lost,(long long)ct.stray_dup,(long long)ct.stray_left,
		(long long)ct.stray_folded);
	printf("fixer: [%s] 清理：删区块 %lld（status %lld、全空气 %lld、InhabitedTime0 %lld、坏NBT %lld）；"
		"光照 %lld 块、高度 %lld 块；计划刻 删 %lld/坏记录 %lld/重复 %lld；"
		"POI 删 %lld/补 %lld/重复 %lld/坏记录 %lld/坏块 %lld/%lld 块/%lld 文件；"
		"实体删 %lld 个/坏记录 %lld、删块 %lld、文件 %lld；改坐标 %lld、删重复段 %lld、改Y段 %lld、压调色板 %lld；"
		"删空MCA %lld、format %lld、清偏移异常 %lld 格；IO跳过 %lld、版本不认识跳过 %lld；失败 %lld\n",
		dim.c_str(),(long long)ct.chunks_deleted,(long long)ct.status_deleted,(long long)ct.empty_deleted,
		(long long)ct.it_deleted,(long long)ct.corrupt_deleted,
		(long long)ct.light_chunks,(long long)ct.height_chunks,
		(long long)ct.ticks_deleted,(long long)ct.ticks_bad,(long long)ct.ticks_dup,
		(long long)ct.poi_deleted,(long long)ct.poi_added,(long long)ct.poi_dup,(long long)ct.poi_bad,
		(long long)ct.poi_corrupt,(long long)ct.poi_chunks,(long long)ct.poi_files,
		(long long)ct.entities_deleted,(long long)ct.entities_bad,(long long)ct.entity_chunks,(long long)ct.entity_files,
		(long long)ct.coord_fix,(long long)ct.sec_dup,(long long)ct.sec_y_fixed,(long long)ct.palette_cleaned,
		(long long)ct.mca_deleted,(long long)ct.mca_formatted,(long long)ct.off_cleaned,(long long)ct.io_skipped,(long long)ct.ver_skipped,(long long)ct.failed);
	return ct.failed==0;
}

int main(int argc,char **argv){/*解析参数、装规则插件、核对版本、规整范围，然后一个维度一个维度修；有维度失败返回 -1*/
	if(argc<8||argc>10){
		print_Usage(argv[0]);
		return -1;
	}
	std::string path=argv[1];
	if(!is_Dir(path)){
		printf("fixer: %s 不是目录\n",path.c_str());
		return -1;
	}
	Block_Range rg;
	if(!parse_Range(argv+2,"fixer",&rg)){
		return -1;
	}
	std::vector<std::string> dims;
	if(argc>8){
		split_List(argv[8],dims);
	}
	if(dims.empty()){
		dims.push_back("minecraft:overworld");
	}
	std::string rpath=argc>9?argv[9]:"./rules.so";/*最后一个参数：规则插件的路径，不给就用当前目录的 rules.so*/
	Level probe(path.c_str(),1,1,1);
	if(!probe.dat_ok){
		printf("fixer: %s 的 level.dat 读不到 DataVersion\n",path.c_str());
		return -1;
	}
	dedupe_Dims({&probe},dims,"fixer");
	const Rules_Api *api=load_Rules(rpath);
	if(!api){
		printf("fixer: 规则插件没装上，什么都没改\n");
		return -1;
	}
	if(api->minecraft_version!=probe.data_version){
		printf("fixer: 规则插件 %s 的 minecraft_version=%d，存档 DataVersion=%d，不匹配\n",
			rpath.c_str(),api->minecraft_version,probe.data_version);
		return -1;
	}
	printf("fixer: 规则 %s（%s，MC DataVersion %d，abi=0x%08x）来自 %s\n",
		api->name?api->name:"?",api->version?api->version:"?",api->minecraft_version,RULES_ABI,rpath.c_str());
	init_AirInfo(api);
	int ret=0;
	for(size_t i=0;i<dims.size();i++){
		if(!fix_Dim(path,dims[i],rg,api)){
			ret=-1;
		}
	}
	return ret;
}
