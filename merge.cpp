#include "nbtrw.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
/*merge.cpp：把 now（游玩的地图）里相对 old/new 改过的方块搬到 new（新版本空板，输出）
 *
 *	规矩（用户定的）：
 *	  · 对每个方块，now 和任意一个 old、或者和 new 自己相同，就保留 new 的；否则算 now 改过，把 now 的搬到 new。
 *	  · 没有 old 就只和 new 比；old 可以给多个，某个 old 读不出来就忽略那一个。
 *	  · now 不可读 / new 不可写（mca坏、区块没生成或不完整、check_Chunk()==false）→ 这个区块跳过。
 *	  · 只搬方块状态；方块实体跟着方块一起搬（Block 比较包含方块实体，箱子内容/告示牌文字的变化也算改动）。
 *	  · 计划刻、POI、实体无脑追加到 new，允许和 new 原有记录重复；对不上/重复是 fixer 的事。
 *	  · now 有 POI/实体而 new 没有对应 .mca 时可以为已处理区块新建 poi/entities 文件；
 *	    region 不新建（new 没这个 region/区块就跳过）。
 *	  · 每个处理过的 new 区块，InhabitedTime 覆盖为这一趟从 now 搬过去的方块数（原来多少都丢掉）。
 *	  · 不清理任何东西，不动光照图和高度图（fixer 负责）；也不调用 Chunk::format_Chunk，
 *	    重复 Y 的 section 原样保留（用户约定：清理非法数据是 fixer 的事）。
 *	  · 终端是 tty 时在 stderr 画区块进度条。
 *	  · 区块读写走 Level 的缓存；merge 自己不再持有/缓存区块镜像。
 *	  · 逐格比较用 Chunk::get_BlockRef + block_Ref_Equal 的零拷贝视图，和 get_Block 的
 *	    Block 相等同义；只有确定要写的格子才构造 Block。
 *	  · set_Chunk 失败时跳过本区块的实体/POI 追加；本区域 save 失败后不再保存实体/POI。
 *
 *	范围：
 *	  chunk_unit=1：x/z 上取整到区块边界，最终整块整块地处理（包含给定范围）；
 *	  chunk_unit=0：严格按给定范围，范围外的方块/计划刻只读不写。
 *	  x1..z2 不要求有序，内部取 min/max。维度用逗号隔开，默认 minecraft:overworld。
 *
 *	用法：
 *	  ./merge <new> <now> <old[,old...]> <x1> <y1> <z1> <x2> <y2> <z2> <chunkunit> [dims]
 *	  old 一个都不给就写空串 ""。
 */

static void usage(const char *me){
	printf("用法：%s <new> <now> <old[,old...]> <x1> <y1> <z1> <x2> <y2> <z2> <chunkunit> [dims]\n",me);
	printf("  new   新版本空板，输出（会被改写）\n");
	printf("  now   要升级的地图，只读\n");
	printf("  old   旧版本空板，逗号隔开多个；没有就写 \"\"\n");
	printf("  范围  两个角，谁大谁小都行；chunkunit=1 按区块对齐整块处理，0 严格按范围\n");
	printf("  dims  逗号隔开的维度名，默认 minecraft:overworld；例：overworld,the_nether\n");
}

static bool path_IsDir(const std::string &p){
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static bool path_Same(const std::string &a,const std::string &b){
	struct stat sa,sb;
	return !stat(a.c_str(),&sa)&&!stat(b.c_str(),&sb)&&sa.st_dev==sb.st_dev&&sa.st_ino==sb.st_ino;
}
static bool make_Dir(const std::string &p){
	if(p.empty()||path_IsDir(p)){
		return true;
	}
	size_t i=p.find('/',1);
	for(;;){
		if(i==std::string::npos){
			i=p.size();
		}
		std::string sub=p.substr(0,i);
		if(!sub.empty()&&!path_IsDir(sub)&&mkdir(sub.c_str(),0777)&&!path_IsDir(sub)){
			return false;
		}
		if(i==p.size()){
			return true;
		}
		i=p.find('/',i+1);
	}
}
static std::string path_Parent(const std::string &p){
	size_t i=p.rfind('/');
	return i==std::string::npos?std::string():p.substr(0,i);
}
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
static int64_t floor_Mul32(int64_t v){return floor_Div(v,32)*32;}
static int32_t clamp_I32(int64_t v){
	return v<INT32_MIN?INT32_MIN:(v>INT32_MAX?INT32_MAX:(int32_t)v);
}

struct Merge_Range{
	int32_t x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;/*用户范围，含头尾，已经规整过*/
	int32_t bx0=0,bz0=0,bx1=0,bz1=0;/*实际写范围（chunkunit=1时对齐到区块）*/
	bool inside(int64_t x,int64_t y,int64_t z)const{
		return x>=bx0&&x<=bx1&&y>=y0&&y<=y1&&z>=bz0&&z<=bz1;
	}
};
struct Merge_Stat{
	int64_t chunks=0,blocks=0,changed=0,ticks=0,pos=0,ents=0;
	int64_t skip_now=0,skip_new=0,bad_block=0,unwritable=0,old_bad=0,bad_poi=0,bad_entity=0;
	int64_t bad_regions=0,save_fail=0;
};
/*终端进度条：只在stderr是终端时画。*/
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
static bool chunk_Coords(const Chunk *c,int32_t cx,int32_t cz){
	const nbt_Any *xp=c?c->get("xPos"):NULL;
	const nbt_Any *zp=c?c->get("zPos"):NULL;
	return xp&&xp->type==NBT_INT&&zp&&zp->type==NBT_INT&&xp->as_Int()==cx&&zp->as_Int()==cz;
}
static bool section_Const(nbt_Any *any,Chunk_Cursor *cur,nbt_Any **state){
	if(!any){
		*state=NULL;
		return true;
	}
	if(any->type!=NBT_COMPOUND){
		return false;
	}
	return is_SectionConst((nbt_Compound*)any,cur,state);
}
static bool section_HasEntity(const Chunk *c,int32_t sy){/*这一段Y范围内有没有方块实体：有就不能整段跳过*/
	const nbt_Any *ba=c?c->get("block_entities"):NULL;
	if(!ba||ba->type!=NBT_LIST){
		return false;
	}
	const nbt_List *bes=(const nbt_List*)ba;
	int32_t lo=sy*16,hi=lo+15;
	for(size_t i=0;i<bes->con.size();i++){
		const nbt_Any *e=bes->con[i];
		if(!e||e->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *y=((const nbt_Compound*)e)->get("y");
		if(y&&y->type==NBT_INT&&y->as_Int()>=lo&&y->as_Int()<=hi){
			return true;
		}
	}
	return false;
}

/*缺section时补一段空section：block_states里一条air；biomes尽量从now同段/附近抄，抄不到用plains。
  这是用户确认过的“补一段空section再写”；只会发生在new区块已经存在且full的时候。*/
static bool ensure_Section(Chunk *dst,const Chunk *src,int32_t sy){
	if(!dst||!dst->is_Full()){
		return false;
	}
	nbt_Any *old=dst->get_Section(sy);
	if(old){
		if(old->type!=NBT_COMPOUND){
			return false;
		}
		nbt_Compound *sec=(nbt_Compound*)old;
		nbt_Any *bs=sec->get("block_states");
		if(bs&&bs->type!=NBT_COMPOUND){
			return false;
		}
		if(!bs){
			nbt_Compound *st=new nbt_Compound;
			nbt_List *pal=new nbt_List(NBT_COMPOUND);
			nbt_Compound *air=new nbt_Compound;
			air->set_String("Name","minecraft:air");
			if(!pal->push_back(air)){
				pal->free();
				st->free();
				return false;
			}
			st->set("palette",pal);
			sec->set("block_states",st);
			dst->format_generation++;
		}
		return true;
	}
	nbt_List *secs=NULL;
	nbt_Any *sa=dst->get("sections");
	if(sa){
		if(sa->type!=NBT_LIST){
			return false;
		}
		secs=(nbt_List*)sa;
		if(!secs->con.empty()&&secs->elem!=NBT_COMPOUND){
			return false;
		}
	}else{
		secs=new nbt_List(NBT_COMPOUND);
		dst->set("sections",secs);
		dst->format_generation++;
	}
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte((int8_t)sy));
	nbt_Compound *st=new nbt_Compound;
	nbt_List *pal=new nbt_List(NBT_COMPOUND);
	nbt_Compound *air=new nbt_Compound;
	air->set_String("Name","minecraft:air");
	if(!pal->push_back(air)){
		pal->free();
		st->free();
		sec->free();
		return false;
	}
	st->set("palette",pal);
	sec->set("block_states",st);
	/*biomes：优先抄src同段；否则抄src任意一段；再否则抄dst任意一段；都没有就plains*/
	const nbt_Any *bio=NULL;
	if(src){
		const nbt_Any *ss=src->get_Section(sy);
		if(ss&&ss->type==NBT_COMPOUND){
			bio=((const nbt_Compound*)ss)->get("biomes");
		}
		if(!bio){
			const nbt_Any *sl=src->get("sections");
			if(sl&&sl->type==NBT_LIST){
				const nbt_List *ls=(const nbt_List*)sl;
				for(size_t i=0;i<ls->con.size()&&!bio;i++){
					const nbt_Any *s=ls->con[i];
					if(s&&s->type==NBT_COMPOUND){
						bio=((const nbt_Compound*)s)->get("biomes");
					}
				}
			}
		}
	}
	if(!bio){
		const nbt_Any *sl=dst->get("sections");
		if(sl&&sl->type==NBT_LIST){
			const nbt_List *ls=(const nbt_List*)sl;
			for(size_t i=0;i<ls->con.size()&&!bio;i++){
				const nbt_Any *s=ls->con[i];
				if(s&&s->type==NBT_COMPOUND){
					bio=((const nbt_Compound*)s)->get("biomes");
				}
			}
		}
	}
	if(bio){
		sec->set("biomes",bio->copy());
	}else{
		nbt_Compound *bio_new=new nbt_Compound;
		nbt_List *bp=new nbt_List(NBT_STRING);
		if(bp->push_back(new nbt_String("minecraft:plains"))){
			bio_new->set("palette",bp);
		}else{
			bp->free();
			bio_new->free();
			bio_new=NULL;
		}
		if(bio_new){
			sec->set("biomes",bio_new);
		}
	}
	if(!secs->push_back(sec)){/*失败时push_back已经释放sec*/
		return false;
	}
	dst->format_generation++;
	return true;
}

/*计划刻：now 里落在写范围内的条目，深拷贝追加到 new 的同名表；
  表形状读不出来就不动，返回-1；返回追加了几条。*/
static int append_Ticks(const Chunk *now_c,Chunk *new_c,const Merge_Range &rg,int fluid){
	int n=count_ChunkTicks(now_c,fluid);
	if(n<0){
		return -1;
	}
	if(n==0){
		return 0;
	}
	const char *name=fluid?"fluid_ticks":"block_ticks";
	nbt_Any *ta=new_c->get(name);
	nbt_List *tl=NULL;
	if(ta){
		if(ta->type!=NBT_LIST){
			return -1;
		}
		tl=(nbt_List*)ta;
		if(!tl->con.empty()&&tl->elem!=NBT_COMPOUND){
			return -1;
		}
	}else{
		tl=new nbt_List(NBT_COMPOUND);
		new_c->set(name,tl);
	}
	const nbt_Any *raw=now_c->get(name);
	if(!raw||raw->type!=NBT_LIST){
		return -1;
	}
	const nbt_List *rl=(const nbt_List*)raw;
	int added=0;
	for(int i=0;i<n;i++){
		Chunk_Tick t;
		if(!get_ChunkTick(now_c,fluid,i,&t)){
			return -1;
		}
		if(!rg.inside(t.x,t.y,t.z)){
			continue;
		}
		if((size_t)i>=rl->con.size()||!rl->con[i]||rl->con[i]->type!=NBT_COMPOUND){
			return -1;
		}
		if(!tl->push_back(rl->con[i]->copy())){
			return -1;
		}
		added++;
	}
	return added;
}

/*POI：把now的一条记录原样追加到dst（不按坐标去重；重复留给fixer）*/
static bool poi_Append(nbt_Compound *dst,const nbt_Any *rec,int32_t y){
	if(!dst||!rec||rec->type!=NBT_COMPOUND){
		return false;
	}
	nbt_Any *sa=dst->get("Sections");
	nbt_Compound *secs=NULL;
	if(sa){
		if(sa->type!=NBT_COMPOUND){
			return false;
		}
		secs=(nbt_Compound*)sa;
	}else{
		secs=new nbt_Compound;
		dst->set("Sections",secs);
	}
	std::string key=std::to_string(get_SectionY(y));
	nbt_Any *sp=secs->get(key);
	nbt_Compound *sec=NULL;
	if(sp){
		if(sp->type!=NBT_COMPOUND){
			return false;
		}
		sec=(nbt_Compound*)sp;
	}else{
		sec=new nbt_Compound;
		secs->set(key,sec);
	}
	nbt_Any *ra=sec->get("Records");
	nbt_List *recs=NULL;
	if(ra){
		if(ra->type!=NBT_LIST){
			return false;
		}
		recs=(nbt_List*)ra;
		if(!recs->con.empty()&&recs->elem!=NBT_COMPOUND){
			return false;
		}
	}else{
		recs=new nbt_List(NBT_COMPOUND);
		sec->set("Records",recs);
	}
	if(!recs->push_back(rec->copy())){
		return false;
	}
	sec->set("Valid",new nbt_Byte(1));
	return true;
}

static bool entity_InRange(const nbt_Any *e,const Merge_Range &rg){
	const nbt_Any *p=e&&e->type==NBT_COMPOUND?((const nbt_Compound*)e)->get("Pos"):NULL;
	if(!p||p->type!=NBT_LIST){
		return true;/*没有Pos：形状本来就不对，交给fixer，别在merge里丢*/
	}
	const nbt_List *l=(const nbt_List*)p;
	if(l->con.size()<3||!l->con[0]||!l->con[1]||!l->con[2]){
		return true;
	}
	double x=l->con[0]->as_Double(),y=l->con[1]->as_Double(),z=l->con[2]->as_Double();
	if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)){
		return true;/*坏坐标：别在merge里丢，留给fixer*/
	}
	x=std::floor(x);y=std::floor(y);z=std::floor(z);
	if(x<(double)INT32_MIN||x>(double)INT32_MAX||y<(double)INT32_MIN||y>(double)INT32_MAX||
	   z<(double)INT32_MIN||z>(double)INT32_MAX){
		return false;/*范围是int32方块坐标：超出范围的有限double不能转int64（UB），直接当范围外*/
	}
	return rg.inside((int64_t)x,(int64_t)y,(int64_t)z);
}
/*实体：把now这个区块的Entities列表原样追加到new（允许重复；UUID冲突留给fixer）*/
static int merge_EntityChunk(MCA &now_ent,MCA &new_ent,int32_t lx,int32_t lz,
	int32_t cx,int32_t cz,int32_t dv,const Merge_Range &rg,Merge_Stat &st){
	if(now_ent.bad||new_ent.bad){
		st.bad_entity++;
		return -1;
	}
	nbt_Compound *src=now_ent.get_Compound((uint32_t)lx,(uint32_t)lz);
	if(!src){
		if(now_ent.last_error!=MCA_READ_NO_CHUNK){
			st.bad_entity++;/*压缩不支持/NBT坏：不能当“没有这块”*/
			return -1;
		}
		return 0;
	}
	if(!check_Entity(src,cx,cz)){
		src->free();
		st.bad_entity++;
		return -1;
	}
	nbt_Compound *dst=new_ent.get_Compound((uint32_t)lx,(uint32_t)lz);
	bool created=false;
	if(!dst){
		if(new_ent.last_error!=MCA_READ_NO_CHUNK){
			src->free();
			st.bad_entity++;/*目标块读不出来：跳过，不能覆盖成新容器*/
			return -1;
		}
		dst=new nbt_Compound;
		nbt_Int_Array *pos=new nbt_Int_Array;
		pos->push_back(cx);
		pos->push_back(cz);
		dst->set("Position",pos);
		dst->set_Int("DataVersion",dv);
		dst->set("Entities",new nbt_List(NBT_COMPOUND));
		created=true;
	}else if(!dst->get("DataVersion")){
		dst->set_Int("DataVersion",dv);
	}
	if(!check_Entity(dst,cx,cz)){/*目标实体区块形状坏：整块跳过，不往垃圾里追加*/
		src->free();
		dst->free();
		st.bad_entity++;
		return -1;
	}
	const nbt_Any *ss=src->get("Entities");
	nbt_Any *dd=dst->get("Entities");
	if(!ss||ss->type!=NBT_LIST||!dd||dd->type!=NBT_LIST){
		src->free();
		dst->free();
		st.bad_entity++;
		return -1;
	}
	nbt_List *sl=(nbt_List*)ss,*dl=(nbt_List*)dd;
	if(!dl->con.empty()&&dl->elem!=NBT_COMPOUND){
		src->free();
		dst->free();
		st.bad_entity++;
		return -1;
	}
	int n=0;
	for(size_t i=0;i<sl->con.size();i++){
		nbt_Any *e=sl->con[i];
		if(!e||e->type!=NBT_COMPOUND){
			st.bad_entity++;
			continue;
		}
		if(!entity_InRange(e,rg)){
			continue;
		}
		if(!dl->push_back(e->copy())){
			st.bad_entity++;
			break;
		}
		n++;
	}
	if(created&&n==0){
		dst->free();
		src->free();
		return 0;
	}
	if(!new_ent.set_Compound(*dst,(uint32_t)lx,(uint32_t)lz)){
		st.save_fail++;
		dst->free();
		src->free();
		return -1;
	}
	dst->free();
	src->free();
	st.ents+=n;
	return n;
}

/*POI：直接遍历now的Sections/Records，原样追加（不按坐标去重，重复位置也保留）*/
static int merge_POIChunk(MCA &now_poi,MCA &new_poi,int32_t lx,int32_t lz,int32_t dv,const Merge_Range &rg,Merge_Stat &st){
	if(now_poi.bad||new_poi.bad){
		st.bad_poi++;
		return -1;
	}
	nbt_Compound *src=now_poi.get_Compound((uint32_t)lx,(uint32_t)lz);
	if(!src){
		if(now_poi.last_error!=MCA_READ_NO_CHUNK){
			st.bad_poi++;/*压缩不支持/NBT坏：不能当“没有这块”*/
			return -1;
		}
		return 0;
	}
	if(!is_POIShapeOk(src)){
		src->free();
		st.bad_poi++;
		return -1;
	}
	nbt_Compound *dst=new_poi.get_Compound((uint32_t)lx,(uint32_t)lz);
	bool created=false;
	if(!dst){
		if(new_poi.last_error!=MCA_READ_NO_CHUNK){
			src->free();
			st.bad_poi++;/*目标块读不出来：跳过，不能覆盖成新容器*/
			return -1;
		}
		dst=new nbt_Compound;
		dst->set("Sections",new nbt_Compound);
		dst->set_Int("DataVersion",dv);
		created=true;
	}else if(!dst->get("DataVersion")){
		dst->set_Int("DataVersion",dv);
	}
	if(!is_POIShapeOk(dst)){
		src->free();
		dst->free();
		st.bad_poi++;
		return -1;
	}
	const nbt_Any *sa=src->get("Sections");
	int added=0;
	if(sa&&sa->type==NBT_COMPOUND){
		const nbt_Compound *secs=(const nbt_Compound*)sa;
		for(nbt_Compound::const_iterator it=secs->con.begin();it!=secs->con.end();++it){
			const nbt_Any *s=it->second;
			if(!s||s->type!=NBT_COMPOUND){
				continue;
			}
			const nbt_Any *ra=((const nbt_Compound*)s)->get("Records");
			if(!ra||ra->type!=NBT_LIST){
				continue;
			}
			const nbt_List *recs=(const nbt_List*)ra;
			for(size_t i=0;i<recs->con.size();i++){
				const nbt_Any *rec=recs->con[i];
				if(!rec||rec->type!=NBT_COMPOUND){
					st.bad_poi++;
					continue;
				}
				const nbt_Any *pos=((const nbt_Compound*)rec)->get("pos");
				if(!pos||pos->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)pos)->con.size()!=3){
					st.bad_poi++;
					continue;
				}
				int32_t x=((const nbt_Int_Array*)pos)->con[0];
				int32_t y=((const nbt_Int_Array*)pos)->con[1];
				int32_t z=((const nbt_Int_Array*)pos)->con[2];
				if(!rg.inside(x,y,z)){
					continue;
				}
				if(!poi_Append(dst,rec,y)){
					st.bad_poi++;
					continue;
				}
				added++;
			}
		}
	}
	if(created&&added==0){
		dst->free();
		src->free();
		return 0;
	}
	if(!new_poi.set_Compound(*dst,(uint32_t)lx,(uint32_t)lz)){
		st.save_fail++;
		dst->free();
		src->free();
		return -1;
	}
	dst->free();
	src->free();
	st.pos+=added;
	return added;
}

struct Merge_Map{
	std::string path;
	Level *lv=NULL;
};
static bool save_MCA(MCA &mca,const std::string &file,bool &dirty){
	if(!dirty){
		return true;
	}
	if(mca.bad){
		return false;
	}
	std::string parent=path_Parent(file);
	if(!parent.empty()&&!make_Dir(parent)){
		return false;
	}
	if(!mca.save(file.c_str())){
		return false;
	}
	dirty=false;
	return true;
}
static int merge_Dim(const std::string &dim,const Merge_Map &now,const Merge_Map &dst,
	const std::vector<Merge_Map> &olds,const Merge_Range &rg){
	std::string now_rel=now.lv->get_DimensionPath(dim.c_str(),"region");
	std::string dst_rel=dst.lv->get_DimensionPath(dim.c_str(),"region");
	std::string now_ent_rel=now.lv->get_DimensionPath(dim.c_str(),"entities");
	std::string dst_ent_rel=dst.lv->get_DimensionPath(dim.c_str(),"entities");
	std::string now_poi_rel=now.lv->get_DimensionPath(dim.c_str(),"poi");
	std::string dst_poi_rel=dst.lv->get_DimensionPath(dim.c_str(),"poi");
	if(now_rel.empty()||dst_rel.empty()||now_ent_rel.empty()||dst_ent_rel.empty()||
		now_poi_rel.empty()||dst_poi_rel.empty()){
		printf("merge: [%s] 维度路径无效\n",dim.c_str());
		return -1;
	}
	std::vector<bool> old_ok;
	for(size_t i=0;i<olds.size();i++){
		std::string r=olds[i].lv->get_DimensionPath(dim.c_str(),"region");
		bool ok=olds[i].lv->dat_ok&&!r.empty();
		old_ok.push_back(ok);
		if(!ok){
			printf("merge: [%s] 忽略 old #%zu（读不到level.dat/维度路径）\n",dim.c_str(),i);
		}
	}
	int32_t rx0=(int32_t)(floor_Mul32(floor_Mul16(rg.bx0))/32);
	int32_t rz0=(int32_t)(floor_Mul32(floor_Mul16(rg.bz0))/32);
	int32_t rx1=(int32_t)(floor_Mul32(floor_Mul16(rg.bx1))/32);
	int32_t rz1=(int32_t)(floor_Mul32(floor_Mul16(rg.bz1))/32);
	int32_t cx0=(int32_t)(floor_Mul16(rg.bx0)/16);
	int32_t cx1=(int32_t)(floor_Mul16(rg.bx1)/16);
	int32_t cz0=(int32_t)(floor_Mul16(rg.bz0)/16);
	int32_t cz1=(int32_t)(floor_Mul16(rg.bz1)/16);
	Merge_Stat st;
	Progress pr("merge",(int64_t)(cx1-cx0+1)*(int64_t)(cz1-cz0+1));
	printf("merge: [%s] 方块x[%d,%d] y[%d,%d] z[%d,%d] chunkunit写范围 x[%d,%d] z[%d,%d]\n",
		dim.c_str(),rg.x0,rg.x1,rg.y0,rg.y1,rg.z0,rg.z1,rg.bx0,rg.bx1,rg.bz0,rg.bz1);
	for(int32_t rx=rx0;rx<=rx1;rx++){
		for(int32_t rz=rz0;rz<=rz1;rz++){
			MCA *now_region=now.lv->get_MCA(dim.c_str(),rx,rz);
			if(!now_region){/*now没这个区域*/
				if(now.lv->miss_why!=Level::GET_NO_FILE){
					st.bad_regions++;
				}
				continue;
			}
			if(now_region->bad){
				st.bad_regions++;
				printf("merge: [%s] now r.%d.%d 读不出来，跳过\n",dim.c_str(),rx,rz);
				continue;
			}
			MCA *dst_region=dst.lv->get_MCA(dim.c_str(),rx,rz);
			if(!dst_region){/*new没这个区域＝这块地new没生成，不能凭空建区块*/
				if(dst.lv->miss_why!=Level::GET_NO_FILE){
					st.bad_regions++;
				}
				continue;
			}
			if(dst_region->bad){
				st.bad_regions++;
				printf("merge: [%s] new r.%d.%d 读不出来，跳过\n",dim.c_str(),rx,rz);
				continue;
			}
			for(size_t i=0;i<olds.size();i++){
				if(!old_ok[i]){
					continue;
				}
				MCA *om=olds[i].lv->get_MCA(dim.c_str(),rx,rz);
				if(om&&om->bad){
					st.old_bad++;
				}
			}
			/*实体/POI文件：只在真正处理到区块时才写*/
			std::string now_ent_file=get_MCAPath((now.path+"/"+now_ent_rel).c_str(),rx,rz);
			std::string dst_ent_file=get_MCAPath((dst.path+"/"+dst_ent_rel).c_str(),rx,rz);
			std::string now_poi_file=get_MCAPath((now.path+"/"+now_poi_rel).c_str(),rx,rz);
			std::string dst_poi_file=get_MCAPath((dst.path+"/"+dst_poi_rel).c_str(),rx,rz);
			MCA now_ent(now_ent_file),dst_ent(dst_ent_file);
			MCA now_poi(now_poi_file),dst_poi(dst_poi_file);
			bool ent_changed=false,poi_changed=false,chunk_changed=false;
			int32_t cx_begin=std::max(cx0,rx*32);
			int32_t cx_end=std::min(cx1,rx*32+31);
			int32_t cz_begin=std::max(cz0,rz*32);
			int32_t cz_end=std::min(cz1,rz*32+31);
			for(int32_t cx=cx_begin;cx<=cx_end;cx++){
				for(int32_t cz=cz_begin;cz<=cz_end;cz++){
					int64_t wx0=(int64_t)cx*16,wx1=wx0+15,wz0=(int64_t)cz*16,wz1=wz0+15;
					if(wx1<rg.bx0||wx0>rg.bx1||wz1<rg.bz0||wz0>rg.bz1){
						continue;
					}
					pr.tick();
					Chunk *now_c=now.lv->get_Chunk(dim.c_str(),cx,cz);
					if(!now_c||!now_c->is_Full()||!check_Chunk(now_c)||!chunk_Coords(now_c,cx,cz)){
						st.skip_now++;
						continue;
					}
					Chunk *dst_c=dst.lv->get_Chunk(dim.c_str(),cx,cz);
					if(!dst_c||!dst_c->is_Full()||!check_Chunk(dst_c)||!chunk_Coords(dst_c,cx,cz)){
						st.skip_new++;
						continue;
					}
					std::vector<Chunk*> old_c;
					for(size_t i=0;i<olds.size();i++){
						if(!old_ok[i]){
							old_c.push_back(NULL);
							continue;
						}
						Chunk *c=olds[i].lv->get_Chunk(dim.c_str(),cx,cz);
						if(c&&(!c->is_Full()||!check_Chunk(c)||!chunk_Coords(c,cx,cz))){
							c=NULL;
						}
						old_c.push_back(c);
					}
					int32_t lx0=(int32_t)std::max<int64_t>(0,(int64_t)rg.bx0-wx0);
					int32_t lx1=(int32_t)std::min<int64_t>(15,(int64_t)rg.bx1-wx0);
					int32_t lz0=(int32_t)std::max<int64_t>(0,(int64_t)rg.bz0-wz0);
					int32_t lz1=(int32_t)std::min<int64_t>(15,(int64_t)rg.bz1-wz0);
					Chunk_Cursor now_cur,dst_cur;
					std::vector<Chunk_Cursor> old_cur(old_c.size());/*old也按区块记段表，别每格重扫sections*/
					int64_t wrote=0;
					for(int32_t sy=get_SectionY(rg.y0);sy<=get_SectionY(rg.y1);sy++){
						nbt_Any *now_sec=now_c->get_Section(sy,&now_cur);
						nbt_Any *dst_sec=dst_c->get_Section(sy,&dst_cur);
						nbt_Any *nc=NULL,*dc=NULL;
						bool nconst=section_Const(now_sec,&now_cur,&nc);
						bool dconst=section_Const(dst_sec,&dst_cur,&dc);
						if(nconst&&dconst&&!section_HasEntity(now_c,sy)&&!section_HasEntity(dst_c,sy)){
							bool same=(nc==NULL&&dc==NULL)||(nc&&dc&&*nc==*dc);
							if(same){
								continue;/*整段一样（多数是空气），这一整段跳过*/
							}
						}
						int32_t ly0=std::max<int32_t>(0,rg.y0-sy*16);
						int32_t ly1=std::min<int32_t>(15,rg.y1-sy*16);
						for(int32_t lx_i=lx0;lx_i<=lx1;lx_i++){
							for(int32_t lz_i=lz0;lz_i<=lz1;lz_i++){
								for(int32_t ly=ly0;ly<=ly1;ly++){
									int64_t y=(int64_t)sy*16+ly;
									int64_t x=wx0+lx_i,z=wz0+lz_i;
									if(!rg.inside(x,y,z)){
										continue;
									}
									st.blocks++;
									/*先零拷贝比状态：绝大多数格子相同，省掉Block的深拷贝和hash查找*/
									Block_Ref nbr,dbr;
									if(!now_c->get_BlockRef(lx_i,(int32_t)y,lz_i,&nbr,&now_cur)){
										st.bad_block++;
										continue;
									}
									if(!dst_c->get_BlockRef(lx_i,(int32_t)y,lz_i,&dbr,&dst_cur)){
										st.bad_block++;
										continue;
									}
									if(block_Ref_Equal(nbr,dbr)){
										continue;
									}
									Block nb=now_c->get_Block(lx_i,(int32_t)y,lz_i,&now_cur);
									if(!nb.valid()){
										st.bad_block++;
										continue;
									}
									bool keep=false;
									for(size_t oi=0;oi<old_c.size();oi++){
										if(!old_c[oi]){
											continue;
										}
										Block_Ref obr;
										if(old_c[oi]->get_BlockRef(lx_i,(int32_t)y,lz_i,&obr,&old_cur[oi])&&
										   block_Ref_Equal(obr,nb)){
											keep=true;
											break;
										}
									}
									if(keep){
										continue;
									}
									if(!ensure_Section(dst_c,now_c,sy)){
										st.unwritable++;
										continue;
									}
									if(!dst_c->set_Block(nb,lx_i,(int32_t)y,lz_i)){
										st.unwritable++;
										continue;
									}
									wrote++;
								}
							}
						}
					}
					/*计划刻：now 的整张表按写范围过滤后追加；即使没搬方块也追加（用户要求无脑拷贝）*/
					int t0=append_Ticks(now_c,dst_c,rg,0);
					int t1=append_Ticks(now_c,dst_c,rg,1);
					if(t0<0||t1<0){
						st.bad_block++;
					}else{
						st.ticks+=t0+t1;
					}
					dst_c->set_Long("InhabitedTime",wrote);
					st.changed+=wrote;
					if(!dst.lv->set_Chunk(dim.c_str(),cx,cz,*dst_c)){
						st.save_fail++;
						continue;/*区块没写进去：别再把实体/POI追加成不一致的状态*/
					}
					chunk_changed=true;
					/*实体/POI：区块已经处理过，追加 now 的记录*/
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					if(merge_EntityChunk(now_ent,dst_ent,lx,lz,cx,cz,dst.lv->data_version,rg,st)>0){
						ent_changed=true;
					}
					if(merge_POIChunk(now_poi,dst_poi,lx,lz,dst.lv->data_version,rg,st)>0){
						poi_changed=true;
					}
					st.chunks++;
				}
			}
			bool region_ok=true;
			if(chunk_changed&&!dst.lv->save()){
				st.save_fail++;
				region_ok=false;
			}
			if(region_ok&&ent_changed&&!save_MCA(dst_ent,dst_ent_file,ent_changed)){
				st.save_fail++;
			}
			if(region_ok&&poi_changed&&!save_MCA(dst_poi,dst_poi_file,poi_changed)){
				st.save_fail++;
			}
		}
	}
	pr.finish();
	printf("merge: [%s] 区块 %lld，方块 %lld，搬了 %lld；计划刻 %lld，实体 %lld，POI %lld\n",
		dim.c_str(),(long long)st.chunks,(long long)st.blocks,(long long)st.changed,
		(long long)st.ticks,(long long)st.ents,(long long)st.pos);
	printf("merge: [%s] 跳过 now不可读 %lld、new不可写/没区块 %lld；坏方块 %lld、写不进 %lld；坏区域 %lld、旧图坏 %lld、坏实体 %lld、坏POI %lld、存盘失败 %lld\n",
		dim.c_str(),(long long)st.skip_now,(long long)st.skip_new,(long long)st.bad_block,(long long)st.unwritable,
		(long long)st.bad_regions,(long long)st.old_bad,(long long)st.bad_entity,(long long)st.bad_poi,(long long)st.save_fail);
	return (st.save_fail||st.bad_regions)?-1:0;
}

int main(int argc,char **argv){
	if(argc<11){
		usage(argv[0]);
		return -1;
	}
	std::string path_new=argv[1],path_now=argv[2];
	std::vector<std::string> old_paths;
	split_List(argv[3],old_paths);
	int32_t nums[6];
	static const char *nname[6]={"x1","y1","z1","x2","y2","z2"};
	for(int i=0;i<6;i++){
		if(!parse_Int(argv[4+i],&nums[i])){
			printf("merge: %s 不是整数：%s\n",nname[i],argv[4+i]);
			return -1;
		}
	}
	int32_t cu=0;
	if(!parse_Int(argv[10],&cu)||(cu!=0&&cu!=1)){
		printf("merge: chunkunit 只能是0或1：%s\n",argv[10]);
		return -1;
	}
	if(!path_IsDir(path_new)||!path_IsDir(path_now)){
		printf("merge: new/now 必须是目录\n");
		return -1;
	}
	if(path_Same(path_new,path_now)){
		printf("merge: new 和 now 是同一个目录（new是输出，会毁掉now）\n");
		return -1;
	}
	for(size_t i=0;i<old_paths.size();i++){
		if(!path_IsDir(old_paths[i])){
			printf("merge: old 不是目录：%s\n",old_paths[i].c_str());
			return -1;
		}
		if(path_Same(path_new,old_paths[i])||path_Same(path_now,old_paths[i])){
			printf("merge: old 和 new/now 是同一个目录：%s\n",old_paths[i].c_str());
			return -1;
		}
	}
	std::vector<std::string> dims;
	if(argc>11){
		split_List(argv[11],dims);
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
	Level now_lv(path_now.c_str(),64,8,2),dst_lv(path_new.c_str(),64,8,2);
	if(!now_lv.dat_ok||!dst_lv.dat_ok){
		printf("merge: now/new 的 level.dat 读不到 DataVersion，不干活\n");
		return -1;
	}
	Merge_Map now{path_now,&now_lv},dst{path_new,&dst_lv};
	std::vector<Merge_Map> olds;
	for(size_t i=0;i<old_paths.size();i++){
		Level *lv=new Level(old_paths[i].c_str(),64,8,2);
		if(!lv->dat_ok){
			printf("merge: 忽略 old %s（level.dat 读不到 DataVersion）\n",old_paths[i].c_str());
			delete lv;
			continue;
		}
		olds.push_back(Merge_Map{old_paths[i],lv});
	}
	Merge_Range rg;
	rg.x0=nums[0];rg.y0=nums[1];rg.z0=nums[2];rg.x1=nums[3];rg.y1=nums[4];rg.z1=nums[5];
	int32_t tx0=std::min(rg.x0,rg.x1),tx1=std::max(rg.x0,rg.x1);
	int32_t ty0=std::min(rg.y0,rg.y1),ty1=std::max(rg.y0,rg.y1);
	int32_t tz0=std::min(rg.z0,rg.z1),tz1=std::max(rg.z0,rg.z1);
	rg.x0=tx0;rg.x1=tx1;rg.y0=ty0;rg.y1=ty1;rg.z0=tz0;rg.z1=tz1;
	rg.bx0=cu?clamp_I32(floor_Mul16(tx0)):tx0;
	rg.bx1=cu?clamp_I32(floor_Mul16(tx1)+15):tx1;
	rg.bz0=cu?clamp_I32(floor_Mul16(tz0)):tz0;
	rg.bz1=cu?clamp_I32(floor_Mul16(tz1)+15):tz1;
	int ret=0;
	for(size_t i=0;i<dims.size();i++){
		if(merge_Dim(dims[i],now,dst,olds,rg)<0){
			ret=-1;
		}
	}
	for(size_t i=0;i<olds.size();i++){
		delete olds[i].lv;
	}
	return ret;
}
