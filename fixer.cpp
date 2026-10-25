#include "nbtrw.hpp"
#include "rules.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
/*fixer.cpp：修一个存档的非法方块/非法数据（规则从 rules.so 来）
 *
 *	顺序（用户定的）：
 *	  1. 规则物理判定：按 rules.so 逐格判，撑不住的拆/改；不写 InhabitedTime。
 *	     fixer 只把这一格和六个面的信息交给 rules；写水先删旧刻，再补 fluid_ticks(t=5,p=0)。
 *	  2. 区块自身：按文件位置重写 xPos/zPos；Chunk::format_Chunk() 删 Y 非Byte子区块、
 *	     去重复 Y、压紧废弃调色板；Status != full 的删；全空气、InhabitedTime 明确 Long 0
 *	     的，只有整块都在范围内才删；缺 InhabitedTime 写 0 但不因此删。
 *	     区块级结构坏到 check_Chunk_Base()==false 或 GET_BAD_CHUNK 的整块删并记入 deleted；
 *	     单条计划刻/POI/实体坏记录按最小单位删。
 *	  3. 光照/高度：选中区块删 Heightmaps；自己和周围8区块删 BlockLight/SkyLight 并把 isLightOn 置0；
 *	     范围外邻居只删光照，不改坐标/重复段；坏邻居不写回。
 *	  4. 计划刻：先删缺字段/类型不对的单条，再删指向方块/类型对不上；同一位置还有重复
 *	     合法记录时只留 p 最小、p同t最大的一条。
 *	  5. POI：按 rules.poi_Get 删单条坏记录/错类型/越界票/重复，补缺失，重复保留
 *	     free_tickets 最小；范围外保留，容器坏的 POI 块重建并写 DataVersion。
 *	  6. 实体：单条坏记录只删这一条；UUID 冲突删后出现的那一个；容器/根坏才整块删。
 *	  7. POI/实体与 region 对齐：0字节region先删（否则会被当IO坏文件跳过），再按region
 *	     没有这一块删掉 poi/entities 同格，不新建 region。
 *	  8. 选中范围碰到的 region/poi/entities 文件：0字节文件删，空表头删文件，非空 format。
 *	     前面任一次 save/close 失败就停止后续 POI/实体删除和 format，避免三方不一致。
 *	  终端是 tty 时各阶段在 stderr 画进度条。
 *
 *	范围：chunkunit=1 时 x/z 对齐到区块（整块处理）；0 时严格按给定范围，范围外只读不写；
 *	      删区块/删光照/删高度按整区块；3×3 光照邻居是“只写光照”的明确例外。
 *	      维度用逗号隔开，默认 minecraft:overworld。
 *
 *	区块缓存全部在 Level 里：处理一个区块时用 pin_Chunk 钉住当前 3x3，用完 unpin；
 *	fixer 自己不保留跨区块的区块副本。
 *
 *	只改区块里的 xPos/zPos/block_entities/block_ticks/fluid_ticks/Heightmaps/InhabitedTime/
 *	isLightOn/sections（删Y坏段/重复Y/删光照）/block_states/BlockLight/SkyLight；POI/实体在各自目录里。
 */

static void usage(const char *me){
	printf("用法：%s <path> <x1> <y1> <z1> <x2> <y2> <z2> <chunkunit> [dims]\n",me);
	printf("  path       存档目录（会被改写！）\n");
	printf("  x1 y1 z1 x2 y2 z2  方块范围的两个角（谁大谁小都行）\n");
	printf("  chunkunit  1=x/z对齐区块整块处理；0=严格按范围（范围外只读不写）\n");
	printf("  dims       逗号隔开的维度名，默认 minecraft:overworld\n");
	printf("  规则插件固定找本程序旁边的 rules.so（没有就找当前目录）\n");
}

static bool path_IsDir(const std::string &p){
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static bool chunk_Unreadable(int why){/*读不出来但“不知道对不对”的原因：保留记录、跳过，不按坏块删*/
	return why==Level::GET_BAD_FILE||why==Level::GET_BAD_CHUNK||why==Level::GET_UNSUPPORTED_COMPRESSION;
}
static bool make_Dir(const std::string &p){/*只补最后一级：poi/ 的父目录是维度目录，正常情况下已经在了*/
	if(path_IsDir(p)){
		return true;
	}
	if(!mkdir(p.c_str(),0777)){
		return true;
	}
	return path_IsDir(p);
}
/*终端进度条：只在stderr是终端时画，测试/重定向时不污染日志；重画同一行。*/
struct Progress{
	const char *name;
	int64_t total,done,last;
	bool tty;
	Progress(const char *n,int64_t t):name(n),total(t>0?t:0),done(0),last(0),tty(isatty(STDERR_FILENO)!=0){}
	void tick(int64_t n=1){
		done+=n;
		if(done>total){
			done=total;
		}
		if(!tty||total<=0){
			return;
		}
		if(done!=total&&done-last<64){
			return;
		}
		last=done;
		int pct=(int)(done*100/total);
		int bar=(int)(done*24/total);
		fprintf(stderr,"\r%-8s [",name);
		for(int i=0;i<24;i++){
			fputc(i<bar?'#':'-',stderr);
		}
		fprintf(stderr,"] %lld/%lld %3d%%",(long long)done,(long long)total,pct);
		fflush(stderr);
	}
	void finish(){
		if(tty&&total>0&&done<total){
			done=total;
			tick(0);
		}
		if(tty&&total>0){
			fputc('\n',stderr);
			fflush(stderr);
		}
	}
};
static void split_List(const char *s,std::vector<std::string> &out){
	if(!s){
		return;
	}
	std::string cur;
	for(const char *p=s;;p++){
		if(*p==','||!*p){
			if(!cur.empty()){
				out.push_back(cur);
				cur.clear();
			}
			if(!*p){
				break;
			}
		}else{
			cur+=*p;
		}
	}
}
static int64_t floor_Div(int64_t v,int64_t d){
	return v>=0?v/d:(v-(d-1))/d;
}
static int64_t floor_Mul16(int64_t v){return floor_Div(v,16)*16;}
static int32_t div16(int32_t v){return (int32_t)floor_Div(v,16);}
static int32_t div32(int32_t v){return (int32_t)floor_Div(v,32);}
static uint64_t chunk_Key(int32_t cx,int32_t cz){
	return ((uint64_t)(uint32_t)cx<<32)|(uint32_t)cz;
}
static int32_t clamp_I32(int64_t v){
	return v<INT32_MIN?INT32_MIN:(v>INT32_MAX?INT32_MAX:(int32_t)v);
}
struct Fix_Range{
	int32_t x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;
	int32_t bx0=0,bz0=0,bx1=0,bz1=0;/*实际处理/写入的x/z范围（chunkunit=1对齐区块）*/
	bool chunk_unit=true;
	bool in_Write(int64_t x,int64_t y,int64_t z)const{
		return x>=bx0&&x<=bx1&&y>=y0&&y<=y1&&z>=bz0&&z<=bz1;
	}
	bool chunk_Selected(int32_t cx,int32_t cz)const{
		int64_t wx0=(int64_t)cx*16,wx1=wx0+15,wz0=(int64_t)cz*16,wz1=wz0+15;
		return !(wx1<bx0||wx0>bx1||wz1<bz0||wz0>bz1);
	}
	bool chunk_Covered(int32_t cx,int32_t cz,const Chunk *c)const{
		int64_t wx0=(int64_t)cx*16,wx1=wx0+15,wz0=(int64_t)cz*16,wz1=wz0+15;
		if(wx0<bx0||wx1>bx1||wz0<bz0||wz1>bz1){
			return false;
		}
		const nbt_Any *sa=c?c->get("sections"):NULL;
		if(!sa){
			return true;
		}
		if(sa->type!=NBT_LIST){
			return false;
		}
		const nbt_List *secs=(const nbt_List*)sa;
		for(size_t i=0;i<secs->con.size();i++){
			const nbt_Any *s=secs->con[i];
			if(!s||s->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Any *yv=((const nbt_Compound*)s)->get("Y");
			if(!yv||yv->type!=NBT_BYTE){
				continue;
			}
			int32_t sy=(int32_t)(int8_t)yv->as_Int();
			int64_t lo=(int64_t)sy*16,hi=lo+15;
			if(lo<y0||hi>y1){
				return false;
			}
		}
		return true;
	}
};

static int64_t selected_Chunk_Total(const Fix_Range &rg){
	int64_t n=0;
	for(int32_t cx=div16(rg.bx0);cx<=div16(rg.bx1);cx++){
		for(int32_t cz=div16(rg.bz0);cz<=div16(rg.bz1);cz++){
			if(rg.chunk_Selected(cx,cz)){
				n++;
			}
		}
	}
	return n;
}

/*==================== 规则插件 ====================*/
static std::string rules_Find(){
	char exe[4096];
	ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
	if(n>0){
		exe[n]=0;
		std::string p=exe;
		size_t slash=p.rfind('/');
		if(slash!=std::string::npos){
			std::string same=p.substr(0,slash)+"/rules.so";
			struct stat st;
			if(!stat(same.c_str(),&st)&&S_ISREG(st.st_mode)){
				return same;
			}
		}
	}
	return "./rules.so";
}
static const Rules_Api *rules_Load(const std::string &path){
	void *h=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
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
	nbt_Compound *states=NULL;
	nbt_List *palette=NULL;
	nbt_Long_Array *data=NULL;
	uint32_t bits=0,per=0,words=0;
	bool bad=false;/*block_states在但不是compound：这段读不出来*/
	std::vector<int16_t> rule;
	std::vector<const Poi_Info*> poi;/*和info对齐：这条调色板条目产生什么POI，NULL=不产生*/
	std::deque<Blk_Info> info;
	int cand=0;
	int poi_cand=0;
};
static void sec_Bits(Fix_Sec &s){
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	s.bits=4;
	while(s.bits<32&&((uint64_t)1<<s.bits)<len){
		s.bits++;
	}
	if(s.bits>32){
		s.bits=32;
	}
	s.per=s.bits?64/s.bits:0;
	s.words=s.data?(uint32_t)s.data->con.size():0;
}
static void sec_Add(Fix_Sec &s,const Rules_Api *api,uint32_t mask){
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
		api->blk_Fill(&s.info[i],s.palette->con[i]);
		s.rule[i]=(int16_t)api->blk_Rule(&s.info[i],mask);
		if(s.rule[i]>=0){
			s.cand++;
		}
		s.poi[i]=api->poi_Get?api->poi_Get(&s.info[i]):NULL;
		if(s.poi[i]){
			s.poi_cand++;
		}
	}
}
static void sec_Build(Fix_Sec &s,nbt_Compound *sec,Chunk_Cursor *cur,const Rules_Api *api,uint32_t mask){
	s.bad=false;
	nbt_Any *bs=sec?sec->get("block_states"):NULL;
	if(bs&&bs->type!=NBT_COMPOUND){
		s.states=NULL;
		s.palette=NULL;
		s.data=NULL;
		s.bad=true;
		s.info.clear();
		s.rule.clear();
		s.poi.clear();
		s.cand=0;
		s.poi_cand=0;
		return;
	}
	s.states=sec?cur->get_States(sec):NULL;
	s.palette=s.states?cur->palette:NULL;
	nbt_Any *d=s.states?s.states->get("data"):NULL;
	s.data=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
	s.info.clear();
	s.rule.clear();
	s.cand=0;
	if(!s.states){
		return;
	}
	if(!s.palette||s.palette->con.empty()){
		s.palette=NULL;
		s.data=NULL;
		return;
	}
	sec_Bits(s);
	sec_Add(s,api,mask);
}
static void sec_Refresh(Fix_Sec &s,nbt_Compound *sec,Chunk_Cursor *cur,const Rules_Api *api,uint32_t mask){
	nbt_Any *bs=sec?sec->get("block_states"):NULL;
	if(bs&&bs->type!=NBT_COMPOUND){
		s.states=NULL;
		s.palette=NULL;
		s.data=NULL;
		s.bad=true;
		s.info.clear();
		s.rule.clear();
		s.poi.clear();
		s.cand=0;
		s.poi_cand=0;
		return;
	}
	s.states=cur->get_States(sec);
	s.palette=s.states?cur->palette:NULL;
	nbt_Any *d=s.states?s.states->get("data"):NULL;
	s.data=d&&d->type==NBT_LONG_ARRAY?(nbt_Long_Array*)d:NULL;
	s.bad=false;
	if(!s.states||!s.palette||s.palette->con.empty()){
		s.palette=NULL;
		s.data=NULL;
		s.bits=0;
		s.per=0;
		s.words=0;
		s.poi.clear();
		s.cand=0;
		s.poi_cand=0;
		return;
	}
	sec_Bits(s);
	sec_Add(s,api,mask);
}
#define IDX_BAD 0xFFFFFFFFu
static inline uint32_t sec_Idx(const Fix_Sec &s,uint32_t i){
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	if(!s.data||!s.per){
		return len<=1?0:IDX_BAD;
	}
	uint32_t w=i/s.per;
	if(w>=s.words){
		return IDX_BAD;
	}
	uint32_t idx=(uint32_t)((uint64_t)s.data->con[w]>>((i%s.per)*s.bits))&((uint32_t)(((uint64_t)1<<s.bits)-1));
	if(idx>=len){
		return IDX_BAD;
	}
	return idx;
}
static Blk_Info info_Air_Make(){
	Blk_Info b;
	b.flags=BF_AIR;
	return b;
}
static const Blk_Info info_Air=info_Air_Make();

/*==================== 物理判定 ====================*/
struct Fix_Stat{
	int64_t chunks=0,sec_all=0,sec_hit=0,pos=0,hit=0,air=0,wet=0,set=0,miss=0,blind=0,noop=0;
	int64_t pos_bad=0,sec_bad=0,sec_dup=0,sec_y=0,palette=0,coord_fix=0,tick_add=0,tick_del=0,tick_fail=0,flush_fail=0;
	std::vector<int64_t> rule_hit,rule_change;
	std::unordered_map<std::string,int64_t> base_change;
	void init(int n){
		rule_hit.assign((size_t)(n>0?n:1),0);
		rule_change.assign((size_t)(n>0?n:1),0);
	}
};
static Block state_Block(const nbt_Any *st,const nbt_Compound *be){
	Block b;
	const char *name=get_Name(st);
	if(!name||!name[0]){
		return b;
	}
	b.id=nbt_String(name);
	if(st&&st->type==NBT_COMPOUND){
		const nbt_Any *p=((const nbt_Compound*)st)->get("Properties");
		if(p&&p->type==NBT_COMPOUND){
			b.properties=*(const nbt_Compound*)p;
		}
	}
	if(be){
		b.block_entities=*be;
	}
	return b;
}
/*==================== 区块视图：区块缓存全在 Level 里，这里只钉住当前3x3 ====================*/
/*format_Chunk 的薄包装：Y坏段/重复Y/废弃调色板都在里面处理。
  返回删掉的段数；-1=sections不是List（这段区块不碰）。*/
static int fix_Chunk_Sections(Chunk *c,int *bad_y=NULL,int *palette_cleaned=NULL){
	if(!c){
		if(bad_y){*bad_y=0;}
		if(palette_cleaned){*palette_cleaned=0;}
		return 0;
	}
	return c->format_Chunk(bad_y,palette_cleaned);
}
static bool fix_Chunk_Pos(Chunk *c,int32_t cx,int32_t cz){
	if(!c){
		return false;
	}
	bool changed=false;
	const nbt_Any *xp=c->get("xPos");
	if(!xp||xp->type!=NBT_INT||xp->as_Int()!=cx){
		c->set_Int("xPos",cx);
		changed=true;
	}
	const nbt_Any *zp=c->get("zPos");
	if(!zp||zp->type!=NBT_INT||zp->as_Int()!=cz){
		c->set_Int("zPos",cz);
		changed=true;
	}
	if(changed){/*坐标也是游标缓存（xPos/zPos）的一部分，换版本让旧游标重查*/
		c->format_generation++;
	}
	return changed;
}

struct Fix_Pad{/*一个被钉住的区块+调色板表；不拥有Chunk，也不复制区块*/
	int32_t cx=0,cz=0;
	Chunk *chunk=NULL;
	Chunk_Cursor cur;
	std::unordered_map<nbt_Compound*,Fix_Sec> secs;
	nbt_Compound *last_sec=NULL;
	Fix_Sec *last_info=NULL;
	Fix_Sec *sec_Of(nbt_Compound *sec,const Rules_Api *api,uint32_t mask){
		if(last_sec==sec){
			return last_info;
		}
		auto it=secs.find(sec);
		if(it==secs.end()){
			Fix_Sec si;
			sec_Build(si,sec,&cur,api,mask);
			it=secs.emplace(sec,std::move(si)).first;
		}
		last_sec=sec;
		last_info=&it->second;
		return last_info;
	}
};
struct Fix_Coord{int32_t x,y,z;};
struct Fix_Job;

struct Fix_Scan{
	Fix_Job &job;
	int32_t cx=0,cz=0;
	Fix_Pad pad[3][3];
	Fix_Pad *center=NULL;
	std::deque<Fix_Coord> local;
	bool ok=false,changed=false,finished=false;

	Fix_Scan(Fix_Job &j,int32_t x,int32_t z);
	~Fix_Scan(){finish();}
	void finish();
	int scan_Center();
	int judge_Pos(int32_t x,int32_t y,int32_t z);
	void drain_Local();
	bool read_Info(int dir,int32_t x,int32_t y,int32_t z,const Blk_Info **out);
	void fill_Ctx(Rules_Ctx &c,int32_t x,int32_t y,int32_t z);
	bool write_Block(int32_t x,int32_t y,int32_t z,const Block &b);
	void enqueue_Neighbors(int32_t x,int32_t y,int32_t z);
	void sec_Wrote(Fix_Sec &s,nbt_Compound *sec);
};

struct Fix_Job{
	Level *lv;
	std::string dim;
	const Rules_Api *api;
	uint32_t mask;
	Fix_Range rg;
	Fix_Stat st;
	std::deque<Fix_Coord> cross;/*跨区块的待复判位置；区块本身不在这里缓存*/

	Fix_Job(Level *l,const char *d,const Fix_Range &r,const Rules_Api *a,uint32_t m):
		lv(l),dim(d?d:""),api(a),mask(m),rg(r){
		st.init(a->rule_Count());
	}
	void scan_Chunk(int32_t cx,int32_t cz);
	void drain_Cross();
};

static int fix_At(Fix_Job &j,Fix_Scan &sc,int32_t x,int32_t y,int32_t z,Fix_Sec &s,nbt_Compound *sec,
	int di,uint32_t idx);

Fix_Scan::Fix_Scan(Fix_Job &j,int32_t x,int32_t z):job(j),cx(x),cz(z){
	for(int dx=-1;dx<=1;dx++){
		for(int dz=-1;dz<=1;dz++){
			Fix_Pad &p=pad[dx+1][dz+1];
			p.cx=cx+dx;
			p.cz=cz+dz;
			Chunk *c=job.lv->pin_Chunk(job.dim.c_str(),p.cx,p.cz);
			if(c&&c->is_Full()){
				p.chunk=c;
			}else{
				if(c){/*没生成完：不钉在里面，规则读到它时按读不出来处理*/
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
	ok=true;
	if(fix_Chunk_Pos(center->chunk,cx,cz)){
		changed=true;
		job.st.coord_fix++;
	}
	int bad_y=0,palette=0;
	int d=fix_Chunk_Sections(center->chunk,&bad_y,&palette);
	if(d<0){
		job.st.sec_bad++;
		ok=false;
		finish();
		return;
	}
	if(d>0){
		changed=true;
		job.st.sec_dup+=d-bad_y;/*d把Y坏段也算进去了，重复段只算d-bad_y*/
	}
	if(bad_y){
		changed=true;
		job.st.sec_y+=bad_y;
	}
	if(palette){
		changed=true;
		job.st.palette+=palette;
	}
	center->cur.reset();/*format_Chunk改了sections表，旧游标作废*/
}
void Fix_Scan::finish(){
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
bool Fix_Scan::read_Info(int dir,int32_t x,int32_t y,int32_t z,const Blk_Info **out){
	static const int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	*out=NULL;
	if(dir<0||dir>5){
		return false;
	}
	int32_t nx=x+off[dir][0],ny=y+off[dir][1],nz=z+off[dir][2];
	int32_t pcx=div16(nx),pcz=div16(nz);
	int ix=pcx-cx+1,iz=pcz-cz+1;
	if(ix<0||ix>2||iz<0||iz>2){
		return false;
	}
	Fix_Pad &p=pad[ix][iz];
	if(!p.chunk){
		return false;
	}
	const nbt_Any *sa=p.chunk->get("sections");
	if(sa&&sa->type!=NBT_LIST){
		return false;/*sections成员在但不是表：整块读不出来*/
	}
	int32_t sy=get_SectionY(ny);
	nbt_Compound *sec=p.cur.find_Section(p.chunk,sy);
	if(!sec){/*缺section＝空气（用户定的）*/
		*out=&info_Air;
		return true;
	}
	Fix_Sec *s=p.sec_Of(sec,job.api,job.mask);
	if(s->bad){
		return false;
	}
	if(!s->states){/*纯光段＝空气*/
		*out=&info_Air;
		return true;
	}
	if(!s->palette){
		return false;
	}
	int32_t lx=(int32_t)((int64_t)nx-(int64_t)pcx*16);
	int32_t lz=(int32_t)((int64_t)nz-(int64_t)pcz*16);
	uint32_t i=(uint32_t)(ny-sy*16)*256+(uint32_t)lz*16+(uint32_t)lx;
	uint32_t idx=sec_Idx(*s,i);
	if(idx==IDX_BAD||idx>=s->info.size()){
		return false;
	}
	*out=&s->info[idx];
	return true;
}
void Fix_Scan::fill_Ctx(Rules_Ctx &c,int32_t x,int32_t y,int32_t z){
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
bool Fix_Scan::write_Block(int32_t x,int32_t y,int32_t z,const Block &b){
	int32_t lx=(int32_t)((int64_t)x-(int64_t)cx*16);
	int32_t lz=(int32_t)((int64_t)z-(int64_t)cz*16);
	if(!center->chunk->set_Block(b,lx,y,lz,&center->cur)){
		return false;
	}
	changed=true;
	/*改了方块就把它原来的方块刻/流体刻清掉；新方块是水就补原版水源的 t=5 刻（霜冰化水也走这里）*/
	int d0=del_ChunkTick(center->chunk,0,x,y,z);
	int d1=del_ChunkTick(center->chunk,1,x,y,z);
	if(d0<0||d1<0){
		job.st.tick_fail++;
	}
	if(d0>0)job.st.tick_del+=d0;
	if(d1>0)job.st.tick_del+=d1;
	if(b.name()=="minecraft:water"){
		if(add_ChunkTick(center->chunk,1,"minecraft:water",x,y,z,5,0)){
			job.st.tick_add++;
		}else{
			job.st.tick_fail++;
		}
	}
	return true;
}
void Fix_Scan::enqueue_Neighbors(int32_t x,int32_t y,int32_t z){
	static const int off[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
	for(int d=0;d<6;d++){
		int32_t nx=x+off[d][0],ny=y+off[d][1],nz=z+off[d][2];
		if(!job.rg.in_Write(nx,ny,nz)){
			continue;
		}
		if(div16(nx)==cx&&div16(nz)==cz){
			local.push_back({nx,ny,nz});
		}else{
			job.cross.push_back({nx,ny,nz});
		}
	}
}
void Fix_Scan::sec_Wrote(Fix_Sec &s,nbt_Compound *sec){
	sec_Refresh(s,sec,&center->cur,job.api,job.mask);
}
int Fix_Scan::scan_Center(){
	if(!ok){
		return 0;
	}
	nbt_List *secs=(nbt_List*)center->chunk->get("sections");
	if(!secs||secs->type!=NBT_LIST){
		return 0;
	}
	int64_t wx=(int64_t)cx*16,wz=(int64_t)cz*16;
	int32_t lx0=0,lx1=15,lz0=0,lz1=15;
	if(!job.rg.chunk_unit){
		lx0=(int32_t)std::max<int64_t>(0,(int64_t)job.rg.bx0-wx);
		lx1=(int32_t)std::min<int64_t>(15,(int64_t)job.rg.bx1-wx);
		lz0=(int32_t)std::max<int64_t>(0,(int64_t)job.rg.bz0-wz);
		lz1=(int32_t)std::min<int64_t>(15,(int64_t)job.rg.bz1-wz);
	}
	int32_t sy0=get_SectionY(job.rg.y0),sy1=get_SectionY(job.rg.y1);
	for(size_t k=0;k<secs->con.size();k++){
		nbt_Any *a=secs->con[k];
		if(!a||a->type!=NBT_COMPOUND){
			continue;
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		const nbt_Any *yv=sec->get("Y");
		if(!yv||yv->type!=NBT_BYTE){
			continue;
		}
		int32_t sy=(int32_t)(int8_t)yv->as_Int();
		if(sy<sy0||sy>sy1){
			continue;
		}
		job.st.sec_all++;
		Fix_Sec *s=center->sec_Of(sec,job.api,job.mask);
		if(!s->cand){
			if(s->bad||(s->states&&!s->palette)){
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
					if(!job.rg.in_Write(xx,yy,zz)){
						continue;
					}
					uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
					uint32_t idx=sec_Idx(*s,i);
					if(idx==IDX_BAD){
						job.st.pos_bad++;
						continue;
					}
					if(idx>=s->rule.size()||s->rule[idx]<0){
						continue;
					}
					job.st.pos++;
					if(fix_At(job,*this,(int32_t)xx,(int32_t)yy,(int32_t)zz,*s,sec,s->rule[idx],idx)){
						changed=true;
					}
				}
			}
		}
	}
	return changed;
}
int Fix_Scan::judge_Pos(int32_t x,int32_t y,int32_t z){
	if(!ok||!job.rg.in_Write(x,y,z)){
		return 0;
	}
	if(div16(x)!=cx||div16(z)!=cz){
		job.cross.push_back({x,y,z});
		return 0;
	}
	int32_t sy=get_SectionY(y);
	nbt_Compound *sec=center->cur.find_Section(center->chunk,sy);
	if(!sec){
		return 0;
	}
	Fix_Sec *s=center->sec_Of(sec,job.api,job.mask);
	if(!s->cand){
		return 0;
	}
	int32_t lx=(int32_t)((int64_t)x-(int64_t)cx*16);
	int32_t lz=(int32_t)((int64_t)z-(int64_t)cz*16);
	uint32_t i=(uint32_t)(y-sy*16)*256+(uint32_t)lz*16+(uint32_t)lx;
	uint32_t idx=sec_Idx(*s,i);
	if(idx==IDX_BAD){
		job.st.pos_bad++;
		return 0;
	}
	if(idx>=s->rule.size()||s->rule[idx]<0){
		return 0;
	}
	job.st.pos++;
	if(fix_At(job,*this,x,y,z,*s,sec,s->rule[idx],idx)){
		changed=true;
		return 1;
	}
	return 0;
}
void Fix_Scan::drain_Local(){
	while(!local.empty()){
		Fix_Coord p=local.front();
		local.pop_front();
		judge_Pos(p.x,p.y,p.z);
	}
}
static int fix_At(Fix_Job &j,Fix_Scan &sc,int32_t x,int32_t y,int32_t z,Fix_Sec &s,nbt_Compound *sec,
	int di,uint32_t idx){
	const Blk_Info *self=&s.info[idx];
	std::string base(self->base);/*set_Block会换掉调色板，self随后就悬空；统计用的名字先拷出来*/
	int ru=j.api->def_Rule(di);
	j.st.hit++;
	if(ru>=0&&ru<(int)j.st.rule_hit.size()){
		j.st.rule_hit[ru]++;
	}
	Rules_Ctx c;
	c.self=self;
	c.x=x;c.y=y;c.z=z;
	sc.fill_Ctx(c,x,y,z);
	int act=j.api->judge(self,di,&c);
	if(c.blind){
		j.st.blind++;
		if(c.out_block){
			c.out_block->free();
		}
		return 0;
	}
	if(act==FX_KEEP){
		if(c.out_block){
			c.out_block->free();
		}
		return 0;
	}
	if(act==FX_AIR){
		if(self->flags&BF_AIR){
			j.st.noop++;
			return 0;
		}
		bool wet=(self->flags&BF_WATER)!=0;
		Block b(wet?"minecraft:water":"minecraft:air");
		if(wet){
			b.properties.set_String("level","0");
		}
		if(!sc.write_Block(x,y,z,b)){
			j.st.miss++;
			return 0;
		}
		j.st.air++;
		if(wet){
			j.st.wet++;
		}
		sc.sec_Wrote(s,sec);
		if(ru>=0){
			j.st.rule_change[ru]++;
			j.st.base_change[base]++;
		}
		sc.enqueue_Neighbors(x,y,z);
		return 1;
	}
	if(act!=FX_SET){
		if(c.out_block){
			c.out_block->free();
		}
		j.st.noop++;
		return 0;
	}
	if(!c.out_block){
		return 0;
	}
	if(self->blk&&*c.out_block==*self->blk){
		j.st.noop++;
		c.out_block->free();
		return 0;
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
	Block b=state_Block(c.out_block,has_keep?&keep:NULL);
	c.out_block->free();
	if(!b.valid()){
		j.st.miss++;
		return 0;
	}
	if(!sc.write_Block(x,y,z,b)){
		j.st.miss++;
		return 0;
	}
	j.st.set++;
	sc.sec_Wrote(s,sec);
	if(ru>=0){
		j.st.rule_change[ru]++;
		j.st.base_change[base]++;
	}
	sc.enqueue_Neighbors(x,y,z);
	return 1;
}
void Fix_Job::scan_Chunk(int32_t cx,int32_t cz){
	Fix_Scan sc(*this,cx,cz);
	if(sc.ok){
		st.chunks++;
		sc.scan_Center();
		sc.drain_Local();
	}
	sc.finish();
	drain_Cross();
}
void Fix_Job::drain_Cross(){
	while(!cross.empty()){
		Fix_Coord p=cross.front();
		cross.pop_front();
		Fix_Scan sc(*this,div16(p.x),div16(p.z));
		if(sc.ok){
			sc.judge_Pos(p.x,p.y,p.z);
			sc.drain_Local();
		}
		sc.finish();
	}
}
/*==================== 区块清理 ====================*/
static bool is_ChunkEmpty(const Chunk *c){return !has_Block(c);}
static bool inhabited_Zero(const Chunk *c,bool *known){
	const nbt_Any *v=c?c->get("InhabitedTime"):NULL;
	if(!v){/*没有这个字段不等于0：老存档/工具生成的区块可能没有，不能因此整块删*/
		*known=false;
		return false;
	}
	if(v->type!=NBT_LONG){
		*known=false;
		return false;
	}
	*known=true;
	return v->as_Long()==0;
}
struct Fix_Count{
	int64_t chunks_deleted=0,status_deleted=0,empty_deleted=0,it_deleted=0,it_fixed=0,corrupt_deleted=0;
	int64_t light_chunks=0,height_chunks=0;
	int64_t ticks_deleted=0,ticks_bad=0,ticks_dup=0,poi_deleted=0,poi_added=0,poi_dup=0,poi_bad=0,poi_corrupt=0,poi_chunks=0,poi_files=0;
	int64_t entities_deleted=0,entities_bad=0,entity_chunks=0,entity_files=0;
	int64_t mca_deleted=0,mca_formatted=0;
	int64_t coord_fix=0,sec_dup=0,sec_y_deleted=0,palette_cleaned=0;
	int64_t io_skipped=0,failed=0;
};
static int cleanup_Chunks(Level &lv,const std::string &dim,const Fix_Range &rg,Fix_Count &ct,
	std::set<uint64_t> &deleted){
	Progress pr("清理区块",selected_Chunk_Total(rg));
	for(int32_t cx=div16(rg.bx0);cx<=div16(rg.bx1);cx++){
		for(int32_t cz=div16(rg.bz0);cz<=div16(rg.bz1);cz++){
			if(!rg.chunk_Selected(cx,cz)){
				continue;
			}
			pr.tick();
			Chunk *c=lv.get_Chunk(dim.c_str(),cx,cz);
			if(!c){
				if(lv.miss_why==Level::GET_BAD_CHUNK){/*压缩/NBT解析失败：整块是垃圾，删掉*/
					printf("fixer: 区块(%d,%d) NBT损坏，整块删除\n",cx,cz);
					if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
						ct.failed++;
					}else{
						ct.corrupt_deleted++;
						deleted.insert(chunk_Key(cx,cz));
					}
				}else if(lv.miss_why==Level::GET_UNSUPPORTED_COMPRESSION){
					printf("fixer: 区块(%d,%d) 压缩类型本库不支持，跳过不删（只记日志）\n",cx,cz);
					ct.io_skipped++;
				}else if(lv.miss_why==Level::GET_BAD_FILE){
					printf("fixer: 区域文件读不出来，跳过区块(%d,%d)（只记日志）\n",cx,cz);
					ct.io_skipped++;
				}
				continue;
			}
			int bad_y=0,palette=0;
			int fmt=fix_Chunk_Sections(c,&bad_y,&palette);/*Y非Byte删子区块、重复Y去重、调色板压紧*/
			if(fmt<0||!check_Chunk_Base(c)){/*区块级NBT/结构坏了：fixer修不了，整块删，不能原样留给MC*/
				printf("fixer: 区块(%d,%d) 数据形状损坏，整块删除\n",cx,cz);
				if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
					ct.failed++;
				}else{
					ct.corrupt_deleted++;
					deleted.insert(chunk_Key(cx,cz));
				}
				continue;
			}
			bool coords=fix_Chunk_Pos(c,cx,cz);
			bool changed=coords||fmt>0||palette>0;
			ct.sec_dup+=fmt-bad_y;
			ct.sec_y_deleted+=bad_y;
			ct.palette_cleaned+=palette;
			if(bad_y||fmt-bad_y||palette){
				printf("fixer: 区块(%d,%d) 清理：Y坏段 %d、重复Y %d、调色板 %d\n",
					cx,cz,bad_y,fmt-bad_y,palette);
			}
			bool del=false,status_bad=false;
			Chunk_Full_Status st=get_ChunkStatus(c);
			if(st!=CHUNK_FULL){
				del=true;
				status_bad=true;
			}else{
				bool it_missing=c->get("InhabitedTime")==NULL;
				if(it_missing){/*缺失写0，但不因此删区块；只有明确 Long 0 才删*/
					c->set_Long("InhabitedTime",0);
					ct.it_fixed++;
					changed=true;
				}
				bool known=true;
				bool empty=is_ChunkEmpty(c);
				bool it0=inhabited_Zero(c,&known);
				if(rg.chunk_Covered(cx,cz,c)&&((empty)||(!it_missing&&known&&it0))){
					del=true;
					if(empty){
						ct.empty_deleted++;
					}else{
						ct.it_deleted++;
					}
				}
			}
			if(del){
				if(!lv.erase_Chunk(dim.c_str(),cx,cz)){
					ct.failed++;
				}else{
					ct.chunks_deleted++;
					deleted.insert(chunk_Key(cx,cz));
					if(status_bad){
						ct.status_deleted++;
					}
				}
			}else if(changed){
				if(lv.set_Chunk(dim.c_str(),cx,cz,*c)){
					if(coords){
						ct.coord_fix++;
					}
				}else{
					ct.failed++;
				}
			}
		}
	}
	pr.finish();
	return 0;
}
static int strip_Light(Chunk *c,int64_t *sections){
	nbt_Any *sa=c->get("sections");
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
	c->set("isLightOn",new nbt_Byte(0));
	if(sections){
		*sections+=n;
	}
	return n;
}
static int cleanup_Light(Level &lv,const std::string &dim,const Fix_Range &rg,Fix_Count &ct){
	std::set<std::pair<int32_t,int32_t>> todo;
	for(int32_t cx=div16(rg.bx0);cx<=div16(rg.bx1);cx++){
		for(int32_t cz=div16(rg.bz0);cz<=div16(rg.bz1);cz++){
			if(!rg.chunk_Selected(cx,cz)){
				continue;
			}
			for(int32_t dx=-1;dx<=1;dx++){
				for(int32_t dz=-1;dz<=1;dz++){
					todo.insert({cx+dx,cz+dz});
				}
			}
		}
	}
	Progress pr("光照",(int64_t)todo.size());
	for(auto &p:todo){
		int32_t cx=p.first,cz=p.second;
		pr.tick();
		Chunk *c=lv.get_Chunk(dim.c_str(),cx,cz);
		if(!c){
			if(lv.miss_why==Level::GET_BAD_FILE||lv.miss_why==Level::GET_UNSUPPORTED_COMPRESSION){
				printf("fixer: 光照邻居(%d,%d)读不出来，跳过（只记日志）\n",cx,cz);
				ct.io_skipped++;
			}else if(lv.miss_why==Level::GET_BAD_CHUNK){
				ct.failed++;
			}
			continue;
		}
		bool selected=rg.chunk_Selected(cx,cz);
		int bad_y=0,palette=0;
		int fmt=0;
		if(selected){/*范围外的3x3邻居只删光照；不重写它的坐标/重复段，保持“范围外只读”*/
			fmt=fix_Chunk_Sections(c,&bad_y,&palette);
		}
		if(fmt<0||!check_Chunk_Base(c)){
			if(selected){/*选中的坏块本该在cleanup_Chunks删掉；这里兜底再删一次*/
				printf("fixer: 光照区块(%d,%d)结构损坏，整块删除\n",cx,cz);
				if(lv.erase_Chunk(dim.c_str(),cx,cz)){
					ct.corrupt_deleted++;
				}else{
					ct.failed++;
				}
			}else{
				printf("fixer: 光照邻居(%d,%d)结构损坏，只删光照这一步跳过它\n",cx,cz);
			}
			continue;
		}
		bool coords=false;
		if(selected){
			coords=fix_Chunk_Pos(c,cx,cz);
			ct.sec_dup+=fmt-bad_y;
			ct.sec_y_deleted+=bad_y;
			ct.palette_cleaned+=palette;
			if(fmt-bad_y>0||bad_y||palette){
				printf("fixer: 区块(%d,%d) 光照阶段清理：Y坏段 %d、重复Y %d、调色板 %d\n",
					cx,cz,bad_y,fmt-bad_y,palette);
			}
			c->erase("Heightmaps");
			ct.height_chunks++;
		}
		int64_t n=0;
		strip_Light(c,&n);
		if(!lv.set_Chunk(dim.c_str(),cx,cz,*c)){
			ct.failed++;
		}else{
			ct.light_chunks++;
		}
		if(coords){
			ct.coord_fix++;
		}
	}
	pr.finish();
	return 0;
}
static bool fluid_Match(const Block &b,const std::string &type){
	if(!b.valid()){
		return false;
	}
	if(type=="minecraft:water"){
		const nbt_Any *w=b.properties.get("waterlogged");
		bool wet=w&&w->as_Str()&&!strcmp(w->as_Str(),"true");
		return b.name()=="minecraft:water"||wet||
			b.name()=="minecraft:kelp"||b.name()=="minecraft:kelp_plant"||b.name()=="minecraft:seagrass"||
			b.name()=="minecraft:tall_seagrass"||b.name()=="minecraft:bubble_column";
	}
	return b.name()==type;
}
struct Tick_Pos{
	int32_t x,y,z;
	bool operator<(const Tick_Pos &o)const{
		if(x!=o.x){return x<o.x;}
		if(y!=o.y){return y<o.y;}
		return z<o.z;
	}
};
struct Tick_Rec{
	int32_t x=0,y=0,z=0,t=0,p=0;
	std::string type;
	bool in=false;/*在写范围内？范围外只读*/
	bool known=true;/*方块读得出来？读不出来时不知道记录对不对，保留不参与选优*/
	bool valid=true;/*指向的方块和记录类型对得上？*/
	bool keep=true;
};
static int cleanup_Ticks(Level &lv,const std::string &dim,const Fix_Range &rg,Fix_Count &ct){
	Progress pr("计划刻",selected_Chunk_Total(rg));
	for(int32_t cx=div16(rg.bx0);cx<=div16(rg.bx1);cx++){
		for(int32_t cz=div16(rg.bz0);cz<=div16(rg.bz1);cz++){
			if(!rg.chunk_Selected(cx,cz)){
				continue;
			}
			pr.tick();
			/*钉住当前区块：下面查方块可能把别的区块挤出去，当前这个不能悬空*/
			Chunk *c=lv.pin_Chunk(dim.c_str(),cx,cz);
			if(!c){
				if(lv.miss_why==Level::GET_BAD_FILE||lv.miss_why==Level::GET_UNSUPPORTED_COMPRESSION){
					printf("fixer: 计划刻所在的区块(%d,%d)读不出来，跳过（只记日志）\n",cx,cz);
					ct.io_skipped++;
				}else if(lv.miss_why==Level::GET_BAD_CHUNK){
					ct.failed++;
				}
				continue;
			}
			int bad_y=0,palette=0;
			int fmt=fix_Chunk_Sections(c,&bad_y,&palette);
			if(fmt<0||!check_Chunk_Base(c)){
				ct.failed++;
				lv.unpin_Chunk(dim.c_str(),cx,cz);
				continue;
			}
			ct.sec_dup+=fmt-bad_y;
			ct.sec_y_deleted+=bad_y;
			ct.palette_cleaned+=palette;
			bool ok=true,changed=(fmt>0||palette>0),coords=false;
			if(fix_Chunk_Pos(c,cx,cz)){
				coords=true;
				changed=true;
			}
			for(int fluid=0;ok&&fluid<2;fluid++){
				const char *key=fluid?"fluid_ticks":"block_ticks";
				nbt_Any *ta=c->get(key);
				if(!ta){
					continue;
				}
				if(ta->type!=NBT_LIST){
					ct.failed++;
					ok=false;
					break;
				}
				nbt_List *list=(nbt_List*)ta;
				/*计划刻信息不全：只删这一条记录；表本身不是List才是区块级错误*/
				int bad=0;
				for(size_t i=list->con.size();i>0;i--){
					Chunk_Tick t;
					if(!get_ChunkTick(c,fluid,(int)(i-1),&t)){
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
					if(!get_ChunkTick(c,fluid,(int)i,&t)){
						ct.failed++;
						ok=false;
						break;
					}
					recs[i].x=t.x;recs[i].y=t.y;recs[i].z=t.z;
					recs[i].t=t.delay;recs[i].p=t.priority;
					if(!t.type.empty()){
						recs[i].type.assign(t.type.data(),t.type.size());
					}
					recs[i].in=rg.in_Write(recs[i].x,recs[i].y,recs[i].z);
				}
				if(!ok){
					break;
				}
				for(size_t i=0;i<recs.size();i++){
					if(!recs[i].in){
						continue;/*范围外只读：不判、不去重*/
					}
					Block b=lv.get_Block(dim.c_str(),recs[i].x,recs[i].y,recs[i].z);
					if(!b.valid()){
						int why=lv.miss_why;
						if(chunk_Unreadable(why)){
							recs[i].known=false;/*读不出来：不知道对不对，保留，不参与选优*/
						}else{
							recs[i].valid=false;/*方块不存在（含没有区域文件）：记录指向空气，删*/
						}
						continue;
					}
					recs[i].valid=fluid?fluid_Match(b,recs[i].type):(std::string(b.name())==recs[i].type);
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
			if(ok&&changed){
				if(!lv.set_Chunk(dim.c_str(),cx,cz,*c)){
					ct.failed++;
				}else if(coords){
					ct.coord_fix++;
				}
			}
			lv.unpin_Chunk(dim.c_str(),cx,cz);
		}
	}
	pr.finish();
	return 0;
}

/*==================== POI ====================*/
/*POI 的记录和方块必须一一对应。fixer 的做法：
  · 先把这一块里“应该产生POI”的方块按 rules 的表扫出来（床只认head，工作站/钟/蜂巢/磁石/避雷针/传送门…）；
  · 写范围内每个位置只允许一条记录：类型不对/票数越界/重复的全部删掉，缺失的按 rules 的默认值补；
  · 写范围外的记录原样保留（chunkunit=0 时范围只盖住部分区块）；
  · POI 区块本身形状坏了，修不了，整块丢掉后按方块重建（不把垃圾留给MC）。*/
struct Poi_Rec{int32_t x,y,z;const Poi_Info *info;};
struct Poi_Seen{std::string type;int32_t ft=0;};
struct Poi_Keep{int32_t x,y,z;std::string type;int32_t ft=0;};
static bool name_IsMinecraft(std::string_view n){/*minecraft:xxx或没有命名空间的旧写法*/
	size_t p=n.find(':');
	return p==std::string_view::npos||(p==9&&n.substr(0,9)=="minecraft");
}
static bool scan_POI_Blocks(Chunk *c,int32_t cx,int32_t cz,const Fix_Range &rg,const Rules_Api *api,
	std::vector<Poi_Rec> &out){
	if(!c){
		return true;
	}
	nbt_Any *sa=c->get("sections");
	if(!sa){
		return true;
	}
	if(sa->type!=NBT_LIST){
		return false;
	}
	nbt_List *secs=(nbt_List*)sa;
	Chunk_Cursor cur;
	for(size_t k=0;k<secs->con.size();k++){
		nbt_Any *a=secs->con[k];
		if(!a||a->type!=NBT_COMPOUND){
			return false;
		}
		nbt_Compound *sec=(nbt_Compound*)a;
		nbt_Any *yv=sec->get("Y");
		if(!yv||yv->type!=NBT_BYTE){
			return false;
		}
		Fix_Sec s;
		sec_Build(s,sec,&cur,api,0xFFFFFFFFu);
		if(s.bad){
			return false;
		}
		if(!s.states||!s.palette||s.palette->con.empty()||!s.poi_cand){
			continue;
		}
		int32_t sy=(int32_t)(int8_t)yv->as_Int();
		int64_t wx=(int64_t)cx*16,wz=(int64_t)cz*16;
		for(uint32_t i=0;i<4096;i++){
			uint32_t idx=sec_Idx(s,i);
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
			if(!rg.in_Write(x,y,z)){
				continue;
			}
			out.push_back({(int32_t)x,(int32_t)y,(int32_t)z,s.poi[idx]});
		}
	}
	return true;
}
static bool poi_PosOf(const nbt_Any *e,int32_t *x,int32_t *y,int32_t *z){
	if(!e||e->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Any *p=((const nbt_Compound*)e)->get("pos");
	if(!p||p->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)p)->con.size()!=3){
		return false;
	}
	*x=((const nbt_Int_Array*)p)->con[0];
	*y=((const nbt_Int_Array*)p)->con[1];
	*z=((const nbt_Int_Array*)p)->con[2];
	return true;
}
static bool poi_RecOf(const nbt_Any *e,int32_t *x,int32_t *y,int32_t *z,std::string &type,int32_t *ft){
	if(!poi_PosOf(e,x,y,z)){
		return false;
	}
	const nbt_Compound *c=(const nbt_Compound*)e;
	const nbt_Any *t=c->get("type");
	const nbt_Any *f=c->get("free_tickets");
	if(!t||t->type!=NBT_STRING||!t->as_Str()||!t->as_Str()[0]||!f||f->type!=NBT_INT){
		return false;
	}
	type=t->as_Str();
	*ft=f->as_Int();
	return true;
}
/*扫一遍POI记录：合法且写范围内的进seen；范围外的进keep_out（重建时救回来）。
  remove_bad=true时把单条坏记录从原表里删掉（只删这一条，不整块重建）。
  返回删掉的坏记录数。*/
static int scan_POIRecords(nbt_Compound *pc,const Fix_Range &rg,
	std::map<Tick_Pos,std::vector<Poi_Seen>> &seen,std::vector<Poi_Keep> &keep_out,bool remove_bad){
	if(!pc){
		return 0;
	}
	nbt_Any *sa=pc->get("Sections");
	nbt_Compound *secs=sa&&sa->type==NBT_COMPOUND?(nbt_Compound*)sa:NULL;
	if(!secs){
		return 0;
	}
	int bad=0;
	for(nbt_Compound::iterator it=secs->con.begin();it!=secs->con.end();++it){
		nbt_Any *sp=it->second;
		nbt_Compound *sec=sp&&sp->type==NBT_COMPOUND?(nbt_Compound*)sp:NULL;
		nbt_Any *ra=sec?sec->get("Records"):NULL;
		nbt_List *recs=ra&&ra->type==NBT_LIST?(nbt_List*)ra:NULL;
		if(!recs){
			continue;
		}
		for(size_t i=recs->con.size();i>0;i--){
			nbt_Any *e=recs->con[i-1];
			int32_t x=0,y=0,z=0,ft=0;
			std::string type;
			bool full=poi_RecOf(e,&x,&y,&z,type,&ft);
			int32_t px=0,py=0,pz=0;
			bool has_pos=poi_PosOf(e,&px,&py,&pz);
			if(!has_pos){
				if(remove_bad){
					recs->erase(i-1);
					bad++;
				}
				continue;
			}
			if(!rg.in_Write(px,py,pz)){
				if(full){
					keep_out.push_back({x,y,z,type,ft});
				}
				continue;/*范围外：合法/不合法都只读，不替人作主*/
			}
			if(!full){
				if(remove_bad){
					recs->erase(i-1);
					bad++;
				}
				continue;
			}
			seen[Tick_Pos{x,y,z}].push_back({type,ft});
		}
		if(remove_bad&&recs->con.empty()&&recs->elem!=NBT_END&&recs->elem!=NBT_COMPOUND){
			recs->elem=NBT_COMPOUND;
		}
	}
	return bad;
}
static int cleanup_POI(Level &lv,const std::string &dim,const Fix_Range &rg,const std::set<uint64_t> &deleted,
	const Rules_Api *api,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"poi");
	if(rel.empty()){
		return -1;
	}
	Progress pr("POI",selected_Chunk_Total(rg));
	for(int32_t rx=div32(div16(rg.bx0));rx<=div32(div16(rg.bx1));rx++){
		for(int32_t rz=div32(div16(rg.bz0));rz<=div32(div16(rg.bz1));rz++){
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
			MCA m(file);
			bool zero_file=false;
			if(m.bad){
				struct stat pst;
				bool zero=!stat(file.c_str(),&pst)&&S_ISREG(pst.st_mode)&&pst.st_size==0;
				if(zero){/*0字节的POI文件没有任何可丢的数据：当空区域重建，不跳过缺POI的方块*/
					printf("fixer: POI %s 是空文件，按空区域重建\n",file.c_str());
					m=MCA();
					m.path=file;
					zero_file=true;
				}else{
					printf("fixer: POI %s 读不出来，跳过（只记日志）\n",file.c_str());
					ct.io_skipped++;
					continue;
				}
			}
			bool changed=false;
			int32_t cx0=std::max(div16(rg.bx0),rx*32),cx1=std::min(div16(rg.bx1),rx*32+31);
			int32_t cz0=std::max(div16(rg.bz0),rz*32),cz1=std::min(div16(rg.bz1),rz*32+31);
			for(int32_t cx=cx0;cx<=cx1;cx++){
				for(int32_t cz=cz0;cz<=cz1;cz++){
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					pr.tick();
					if(deleted.count(chunk_Key(cx,cz))){
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
					std::vector<Poi_Rec> want_recs;
					bool scan_ok=scan_POI_Blocks(c,cx,cz,rg,api,want_recs);
					if(c){
						lv.unpin_Chunk(dim.c_str(),cx,cz);
					}
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
					nbt_Compound *pc=m.get_Compound(lx,lz);
					if(!pc&&m.last_error==MCA_READ_UNSUPPORTED_COMPRESSION){
						printf("fixer: POI区块(%d,%d)压缩类型不支持，跳过（只记日志）\n",cx,cz);
						ct.io_skipped++;
						continue;
					}
					bool dirty=false;
					std::map<Tick_Pos,std::vector<Poi_Seen>> seen;
					std::vector<Poi_Keep> keep_out;/*范围外记录：POI块要重建时也要救回来*/
					int badrec=0;
					bool shape_ok=pc?is_POIShapeOk(pc):true;
					if(pc){
						/*容器形状对：坏记录单条删；容器坏：整块重建，但先把能读出来的合法记录收进seen/keep_out。*/
						badrec=scan_POIRecords(pc,rg,seen,keep_out,shape_ok);
						if(badrec){
							ct.poi_bad+=badrec;
							dirty=true;
							printf("fixer: POI区块(%d,%d) 删坏记录 %d 条\n",cx,cz,badrec);
						}
					}
					if(pc&&!shape_ok){
						printf("fixer: POI区块(%d,%d)形状不对，整块重建\n",cx,cz);
						pc->free();
						pc=NULL;/*seen里已经读出来的合法记录还留着：下面按原free_tickets重新落盘，不覆盖成默认值*/
						had=true;
						dirty=true;
						ct.poi_corrupt++;
					}
					auto new_pc=[&](){
						nbt_Compound *p=new nbt_Compound;
						p->set_Int("DataVersion",lv.data_version);
						return p;
					};
					if(!pc&&!keep_out.empty()){
						pc=new_pc();
						for(size_t i=0;i<keep_out.size();i++){
							if(!set_POI(pc,keep_out[i].x,keep_out[i].y,keep_out[i].z,keep_out[i].type.c_str(),keep_out[i].ft)){
								ct.failed++;
							}
						}
					}
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
							if(b.valid()&&!name_IsMinecraft(b.name())){
								keep=true;
							}
							for(size_t i=0;i<recs.size();i++){
								if(!name_IsMinecraft(recs[i].type)){
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
							del+=del_POI(pc,pos.x,pos.y,pos.z);
						}
						if(!missing){
							if(!pc){
								pc=new_pc();
							}
							int32_t ft=has_valid?best:info->free_tickets;
							if(!set_POI(pc,pos.x,pos.y,pos.z,info->type,ft)){
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
						if(!set_POI(pc,pos.x,pos.y,pos.z,kv.second->type,kv.second->free_tickets)){
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
	return 0;
}

/*==================== 实体 UUID ====================*/
static bool entity_InRange(const nbt_Any *e,const Fix_Range &rg){
	const nbt_Any *p=e&&e->type==NBT_COMPOUND?((const nbt_Compound*)e)->get("Pos"):NULL;
	if(!p||p->type!=NBT_LIST){
		return true;
	}
	const nbt_List *l=(const nbt_List*)p;
	if(l->con.size()<3||!l->con[0]||!l->con[1]||!l->con[2]){
		return true;
	}
	double x=l->con[0]->as_Double(),y=l->con[1]->as_Double(),z=l->con[2]->as_Double();
	if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)){
		return true;/*坏坐标：范围外处理，不删*/
	}
	x=std::floor(x);y=std::floor(y);z=std::floor(z);
	if(x<(double)INT32_MIN||x>(double)INT32_MAX||y<(double)INT32_MIN||y>(double)INT32_MAX||
	   z<(double)INT32_MIN||z>(double)INT32_MAX){
		return false;/*超出int32范围：当范围外，保留实体*/
	}
	return rg.in_Write((int64_t)x,(int64_t)y,(int64_t)z);
}
static bool entity_UUID(const nbt_Compound *e,std::string &key){
	const nbt_Any *u=e?e->get("UUID"):NULL;
	if(!u||u->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)u)->con.size()!=4){
		return false;
	}
	const nbt_Int_Array *a=(const nbt_Int_Array*)u;
	key.assign((const char*)a->con.data(),16);
	return true;
}
static int cleanup_Entities(Level &lv,const std::string &dim,const Fix_Range &rg,const std::set<uint64_t> &deleted,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"entities");
	if(rel.empty()){
		return -1;
	}
	Progress pr("实体",selected_Chunk_Total(rg));
	std::set<std::string> seen;
	for(int32_t rx=div32(div16(rg.bx0));rx<=div32(div16(rg.bx1));rx++){
		for(int32_t rz=div32(div16(rg.bz0));rz<=div32(div16(rg.bz1));rz++){
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
			struct stat st;
			if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)){
				continue;
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
			bool changed=false;
			int32_t cx0=std::max(div16(rg.bx0),rx*32),cx1=std::min(div16(rg.bx1),rx*32+31);
			int32_t cz0=std::max(div16(rg.bz0),rz*32),cz1=std::min(div16(rg.bz1),rz*32+31);
			for(int32_t cx=cx0;cx<=cx1;cx++){
				for(int32_t cz=cz0;cz<=cz1;cz++){
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					pr.tick();
					if(deleted.count(chunk_Key(cx,cz))){
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
					Chunk *rc=lv.get_Chunk(dim.c_str(),cx,cz);
					if(!rc){
						int why=lv.miss_why;
						if(why==Level::GET_NO_FILE||why==Level::GET_NO_CHUNK){
							/*region没有这个区块：实体也不该有。整块实体删掉，不新建region。*/
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
						if(why==Level::GET_BAD_FILE||why==Level::GET_UNSUPPORTED_COMPRESSION){
							printf("fixer: 实体对应的区块(%d,%d)读不出来，跳过（只记日志）\n",cx,cz);
							ct.io_skipped++;
							continue;
						}
						ct.failed++;
						continue;
					}
					nbt_Compound *ec=m.get_Compound(lx,lz);
					if(!ec){
						if(m.last_error==MCA_READ_UNSUPPORTED_COMPRESSION){
							printf("fixer: 实体区块(%d,%d)压缩类型不支持，跳过（只记日志）\n",cx,cz);
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
					if(!check_Entity_Container(ec,cx,cz)){
						printf("fixer: 实体区块(%d,%d) 容器损坏（Position/DataVersion/Entities），整块删除\n",cx,cz);
						ec->free();
						if(m.erase_Chunk(lx,lz)){
							ct.entity_chunks++;
							changed=true;
						}else{
							ct.failed++;
						}
						continue;
					}
					nbt_List *es=(nbt_List*)ec->get("Entities");
					int dropped=0,bad=0;
					bool elem_fixed=false;
					/*从前往后：UUID重复时保留先出现的，删后面的。原地erase后下标不前进。*/
					for(size_t i=0;i<es->con.size();){
						nbt_Any *e=es->con[i];
						bool in_range=entity_InRange(e,rg);
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
						if(!entity_UUID((nbt_Compound*)e,key)){
							es->erase(i);
							dropped++;
							bad++;
							continue;
						}
						if(seen.count(key)){
							es->erase(i);
							dropped++;
						}else{
							seen.insert(key);
							i++;
						}
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
	return 0;
}

/*==================== MCA 空文件/format ====================*/
static int format_One(const std::string &file,Fix_Count &ct){
	struct stat st;
	if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)){
		return 0;
	}
	if(st.st_size==0){/*0字节的 region/poi/entities 文件一律删掉，不能留*/
		if(remove(file.c_str())&&errno!=ENOENT){
			ct.failed++;
		}else{
			ct.mca_deleted++;
		}
		return 0;
	}
	MCA m(file);
	if(m.bad){
		printf("fixer: %s 读不出来，不整理（只记日志）\n",file.c_str());
		ct.io_skipped++;
		return 0;
	}
	if(m.is_Empty()){
		if(remove(file.c_str())&&errno!=ENOENT){
			ct.failed++;
		}else{
			ct.mca_deleted++;
		}
		return 0;
	}
	if(!m.format()){
		printf("fixer: %s 表头/数据异常，不整理\n",file.c_str());
		ct.failed++;
		return 0;
	}
	if(!m.save(file.c_str())){
		ct.failed++;
		return 0;
	}
	ct.mca_formatted++;
	return 0;
}
/*0字节region要在别的阶段读它之前删掉。
  否则Level会把0字节文件当GET_BAD_FILE（文件级IO坏），cleanup_Chunks/POI/实体都只记日志跳过；
  等到最后format_Regions删文件时，POI/实体里对应区块已经没机会按“region没有这块”清掉，
  会留下orphan。只动选中区域里的region文件；删失败按处理失败记，后面照旧跳过。*/
static void remove_ZeroRegions(Level &lv,const std::string &dim,const Fix_Range &rg,Fix_Count &ct){
	std::string rel=lv.get_DimensionPath(dim.c_str(),"region");
	if(rel.empty()){
		return;
	}
	for(int32_t rx=div32(div16(rg.bx0));rx<=div32(div16(rg.bx1));rx++){
		for(int32_t rz=div32(div16(rg.bz0));rz<=div32(div16(rg.bz1));rz++){
			std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
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
static int format_Regions(Level &lv,const std::string &dim,const Fix_Range &rg,Fix_Count &ct){
	const char *kinds[3]={"region","poi","entities"};
	for(int k=0;k<3;k++){
		std::string rel=lv.get_DimensionPath(dim.c_str(),kinds[k]);
		if(rel.empty()){
			continue;
		}
		for(int32_t rx=div32(div16(rg.bx0));rx<=div32(div16(rg.bx1));rx++){
			for(int32_t rz=div32(div16(rg.bz0));rz<=div32(div16(rg.bz1));rz++){
				std::string file=get_MCAPath((lv.path+"/"+rel).c_str(),rx,rz);
				format_One(file,ct);
			}
		}
	}
	return 0;
}

/*==================== 一个维度 ====================*/
static int fix_Dim(const std::string &path,const std::string &dim,const Fix_Range &rg,const Rules_Api *api){
	Level lv(path.c_str(),256,16,4);
	std::string rrel=lv.get_DimensionPath(dim.c_str(),"region");
	if(rrel.empty()){
		printf("fixer: [%s] 维度路径无效\n",dim.c_str());
		return -1;
	}
	Fix_Count ct;
	printf("fixer: [%s] 范围 x[%d,%d] y[%d,%d] z[%d,%d] 处理范围 x[%d,%d] z[%d,%d]\n",
		dim.c_str(),rg.x0,rg.x1,rg.y0,rg.y1,rg.z0,rg.z1,rg.bx0,rg.bx1,rg.bz0,rg.bz1);
	/*0字节region先删，后面的“region里没有这个区块”检查才能看见真实情况*/
	remove_ZeroRegions(lv,dim,rg,ct);
	/*1. 物理*/
	{
		Fix_Job j(&lv,dim.c_str(),rg,api,0xFFFFFFFFu);
		Progress pr("物理",selected_Chunk_Total(rg));
		for(int32_t cx=div16(rg.bx0);cx<=div16(rg.bx1);cx++){
			for(int32_t cz=div16(rg.bz0);cz<=div16(rg.bz1);cz++){
				if(!rg.chunk_Selected(cx,cz)){
					continue;
				}
				pr.tick();
				j.scan_Chunk(cx,cz);
			}
		}
		pr.finish();
		if(j.st.flush_fail){
			ct.failed++;
		}
		if(!lv.save()){
			ct.failed++;
		}
		printf("fixer: [%s] 物理：区块 %lld，段 %lld/%lld，判了 %lld 格，拆 %lld（含水 %lld）、改状态 %lld、读不出 %lld、跳过 %lld；"
			"坏下标 %lld、坏段 %lld、Y坏段 %lld、压调色板 %lld、改坐标 %lld、删重复段 %lld；水刻 +%lld/-%lld 失败 %lld；noop %lld\n",
			dim.c_str(),(long long)j.st.chunks,(long long)j.st.sec_hit,(long long)j.st.sec_all,
			(long long)j.st.pos,(long long)j.st.air,(long long)j.st.wet,(long long)j.st.set,
			(long long)j.st.miss,(long long)j.st.blind,(long long)j.st.pos_bad,(long long)j.st.sec_bad,
			(long long)j.st.sec_y,(long long)j.st.palette,
			(long long)j.st.coord_fix,(long long)j.st.sec_dup,
			(long long)j.st.tick_add,(long long)j.st.tick_del,(long long)j.st.tick_fail,(long long)j.st.noop);
		/*sec_bad由后面的区块清理整块删除并计入corrupt_deleted，不算处理失败*/
		if(j.st.pos_bad||j.st.miss||j.st.tick_fail||j.st.flush_fail){
			ct.failed++;
		}
		for(int r=1;r<api->rule_Count();r++){
			if(j.st.rule_change[r]){
				printf("fixer: [%s]   规则 %-28s 改动 %lld\n",dim.c_str(),api->rule_Name(r),(long long)j.st.rule_change[r]);
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
	std::set<uint64_t> deleted;
	bool storage_ok=true;
	cleanup_Chunks(lv,dim,rg,ct,deleted);
	if(!lv.save()){
		ct.failed++;
		storage_ok=false;/*region都没落盘：后面再删POI/实体/format会把三方弄不一致*/
	}
	/*3. 光照/高度*/
	if(storage_ok){
		cleanup_Light(lv,dim,rg,ct);
		if(!lv.save()){
			ct.failed++;
			storage_ok=false;
		}
	}
	/*4. 计划刻*/
	if(storage_ok){
		cleanup_Ticks(lv,dim,rg,ct);
		if(!lv.save()){
			ct.failed++;
			storage_ok=false;
		}
	}
	/*5. POI*/
	if(storage_ok){
		cleanup_POI(lv,dim,rg,deleted,api,ct);
		/*6. 实体*/
		cleanup_Entities(lv,dim,rg,deleted,ct);
	}
	/*7. 落盘+format*/
	if(!lv.close()){
		ct.failed++;
		storage_ok=false;
	}
	if(storage_ok){
		format_Regions(lv,dim,rg,ct);
	}
	printf("fixer: [%s] 清理：删区块 %lld（status %lld、全空气 %lld、InhabitedTime0 %lld、坏NBT %lld）；"
		"补InhabitedTime %lld；光照 %lld 块、高度 %lld 块；计划刻 删 %lld/坏记录 %lld/重复 %lld；"
		"POI 删 %lld/补 %lld/重复 %lld/坏记录 %lld/坏块 %lld/%lld 块/%lld 文件；"
		"实体删 %lld 个/坏记录 %lld、删块 %lld、文件 %lld；改坐标 %lld、删重复段 %lld、Y坏段 %lld、压调色板 %lld；"
		"删空MCA %lld、format %lld；IO跳过 %lld；失败 %lld\n",
		dim.c_str(),(long long)ct.chunks_deleted,(long long)ct.status_deleted,(long long)ct.empty_deleted,
		(long long)ct.it_deleted,(long long)ct.corrupt_deleted,(long long)ct.it_fixed,
		(long long)ct.light_chunks,(long long)ct.height_chunks,
		(long long)ct.ticks_deleted,(long long)ct.ticks_bad,(long long)ct.ticks_dup,
		(long long)ct.poi_deleted,(long long)ct.poi_added,(long long)ct.poi_dup,(long long)ct.poi_bad,
		(long long)ct.poi_corrupt,(long long)ct.poi_chunks,(long long)ct.poi_files,
		(long long)ct.entities_deleted,(long long)ct.entities_bad,(long long)ct.entity_chunks,(long long)ct.entity_files,
		(long long)ct.coord_fix,(long long)ct.sec_dup,(long long)ct.sec_y_deleted,(long long)ct.palette_cleaned,
		(long long)ct.mca_deleted,(long long)ct.mca_formatted,(long long)ct.io_skipped,(long long)ct.failed);
	return ct.failed? -1:0;
}

int main(int argc,char **argv){
	if(argc<9){
		usage(argv[0]);
		return -1;
	}
	std::string path=argv[1];
	if(!path_IsDir(path)){
		printf("fixer: %s 不是目录\n",path.c_str());
		return -1;
	}
	int32_t nums[6];
	static const char *nname[6]={"x1","y1","z1","x2","y2","z2"};
	for(int i=0;i<6;i++){
		if(!parse_Int(argv[2+i],&nums[i])){
			printf("fixer: %s 不是整数：%s\n",nname[i],argv[2+i]);
			return -1;
		}
	}
	int32_t cu=0;
	if(!parse_Int(argv[8],&cu)||(cu!=0&&cu!=1)){
		printf("fixer: chunkunit 只能是0或1：%s\n",argv[8]);
		return -1;
	}
	std::vector<std::string> dims;
	if(argc>9){
		split_List(argv[9],dims);
	}
	if(dims.empty()){
		dims.push_back("minecraft:overworld");
	}
	for(size_t i=0;i<dims.size();i++){
		for(size_t k=0;k<i;k++){
			if(dims[k]==dims[i]){
				dims.erase(dims.begin()+(long)i);
				i--;
				break;
			}
		}
	}
	Level probe(path.c_str(),1,1,1);
	if(!probe.dat_ok){
		printf("fixer: %s 的 level.dat 读不到 DataVersion\n",path.c_str());
		return -1;
	}
	std::string rpath=rules_Find();
	const Rules_Api *api=rules_Load(rpath);
	if(!api){
		printf("fixer: 规则插件没装上，什么都没改\n");
		return -1;
	}
	if(api->minecraft_version!=probe.data_version){
		printf("fixer: rules.so 的 minecraft_version=%d，存档 DataVersion=%d，不匹配\n",
			api->minecraft_version,probe.data_version);
		return -1;
	}
	printf("fixer: 规则 %s（%s，MC DataVersion %d，abi=0x%08x）\n",
		api->name?api->name:"?",api->version?api->version:"?",api->minecraft_version,RULES_ABI);
	Fix_Range rg;
	rg.x0=nums[0];rg.y0=nums[1];rg.z0=nums[2];rg.x1=nums[3];rg.y1=nums[4];rg.z1=nums[5];
	int32_t tx0=std::min(rg.x0,rg.x1),tx1=std::max(rg.x0,rg.x1);
	int32_t ty0=std::min(rg.y0,rg.y1),ty1=std::max(rg.y0,rg.y1);
	int32_t tz0=std::min(rg.z0,rg.z1),tz1=std::max(rg.z0,rg.z1);
	rg.x0=tx0;rg.x1=tx1;rg.y0=ty0;rg.y1=ty1;rg.z0=tz0;rg.z1=tz1;
	rg.chunk_unit=(cu!=0);
	rg.bx0=rg.chunk_unit?clamp_I32(floor_Mul16(tx0)):tx0;
	rg.bx1=rg.chunk_unit?clamp_I32(floor_Mul16(tx1)+15):tx1;
	rg.bz0=rg.chunk_unit?clamp_I32(floor_Mul16(tz0)):tz0;
	rg.bz1=rg.chunk_unit?clamp_I32(floor_Mul16(tz1)+15):tz1;
	int ret=0;
	for(size_t i=0;i<dims.size();i++){
		if(fix_Dim(path,dims[i],rg,api)<0){
			ret=-1;
		}
	}
	return ret;
}
