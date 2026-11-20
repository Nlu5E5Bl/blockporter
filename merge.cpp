#include "nbtrw.hpp"
#include "blockporter.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
/*merge.cpp：把 now（游玩的地图）里相对 old/new 改过的方块搬到 new（新版本空板，输出）
 *
 *	规矩（用户定的）：
 *	  · 对每个方块，now 和任意一个 old、或者和 new 自己相同，就保留 new 的；否则算 now 改过，把 now 的搬到 new。
 *	  · 没有 old 就只和 new 比；old 可以给多个，某个 old 读不出来就忽略那一个。
 *	  · now 不可读 / new 不可写（mca坏、区块没生成或不完整、check_Chunk()==false）→ 这个区块跳过。
 *	  · 跨版本只做暴力合并（用户 2026-11-14）：now/new/old 的 DataVersion（level.dat 的、区块自己的）不同照样合，
 *	    不限制方向（高版本合进低版本、有 mod 合进没 mod 都不拦），只管搬运。方块按各自区块的格式读、按 new 那块的格式写，
 *	    比较按方块语义（名字/属性/方块实体），不做版本间的改名/属性升级：1.13 以后正式版之间改过名或属性的方块只有十来种
 *	    （列在 ReadMeDeveloper 1.4），搬过去就是原样的名字。方块实体、计划刻、实体、POI 的内容也原样搬。
 *	  · 只搬方块状态；方块实体跟着方块一起搬（Block 比较包含方块实体，箱子内容/告示牌文字的变化也算改动）。
 *	  · 计划刻、实体无脑追加到 new，允许和 new 原有记录重复；对不上/重复是 fixer 的事。
 *	    POI 一格只留一条（游戏 PoiSection 按位置存）：now 的顶掉 new 同一格的。
 *	    POI/实体先读成库的 POI/Entity：按 now 那边的版本读（实体两处都读：区块里的实体表、entities/ 的实体区块），
 *	    按 new 那块的版本写（实体：2681 以前写进区块的 Level.Entities，之后写实体区块；
 *	    POI：1935 以前（1.13 等）没有 POI、不搬，1946 以前根上 "0".."15"，之后 Sections）。
 *	  · now 有 POI/实体而 new 没有对应 .mca 时可以为已处理区块新建 poi/entities 文件；
 *	    region 不新建（new 没这个 region/区块就跳过）。
 *	  · 每个处理过的 new 区块，InhabitedTime 覆盖为这一趟从 now 搬过去的方块数（原来多少都丢掉）。
 *	  · 不补段：new 里这一段不在（或者没有 block_states）的格子跳过，计入“缺段跳过”。
 *	  · 不清理任何东西，不动光照图和高度图（fixer 负责）；也不调用 Chunk::format_Chunk，
 *	    重复 Y / Y 不是 Byte 的 section 原样保留，读的时候照游戏的规矩取段（用户约定：修成合法是 fixer 的事）。
 *	  · 终端是 tty 时在 stderr 画区块进度条。
 *	  · 区块读写走 Level 的缓存；merge 自己不再持有/缓存区块镜像。
 *	  · 逐格比较用 Section_View::get_BlockRef + equal_BlockRef 的零拷贝视图（每段开一次视图），和 get_Block 的
 *	    Block 相等同义；只有确定要写的格子才构造 Block。
 *	  · set_Chunk 失败时跳过本区块的实体/POI 追加；本区域 save 失败后不再保存实体/POI。
 *
 *	范围（用户 2026-11-14：去掉 chunkunit）：搬运是方块级的，严格按给定范围，范围外的方块/计划刻/POI/实体只读不写。
 *	  区块级的副作用只有 InhabitedTime（范围碰到的区块都会被覆盖成搬运数）。
 *	  x1..z2 不要求有序，内部取 min/max。维度用逗号隔开，默认 minecraft:overworld。
 *	遍历（用户 2026-11-14）：列出 now 的 region 目录里实际存在的区域文件（list_MCAFiles）再和范围求交，
 *	  不逐个区域坐标去试；new 的 region 文件不在/读不出来的整个区域跳过；
 *	  每个区域里走表头的 1024 格，now 里不存在（含偏移异常）的区块跳过。
 *
 *	用法：
 *	  ./merge <new> <now> <old[,old...]> <x1> <y1> <z1> <x2> <y2> <z2> [dims]
 *	  old 一个都不给就写空串 ""。
 */

static void print_Usage(const char *me){/*参数不够时打的用法*/
	printf("用法：%s <new> <now> <old[,old...]> <x1> <y1> <z1> <x2> <y2> <z2> [dims]\n",me);
	printf("  new   新版本空板，输出（会被改写）\n");
	printf("  now   要升级的地图，只读\n");
	printf("  old   旧版本空板，逗号隔开多个；没有就写 \"\"\n");
	printf("  范围  两个角，谁大谁小都行；严格按它搬（范围外只读不写）\n");
	printf("  dims  逗号隔开的维度名，默认 minecraft:overworld；例：overworld,the_nether\n");
}

static std::string get_PathParent(const std::string &p){/*最后一个 '/' 前面那段；没有就是空*/
	size_t i=p.rfind('/');
	return i==std::string::npos?std::string():p.substr(0,i);
}
struct Merge_Stat{/*一个维度的统计，最后打两行（各字段见 ReadMeDeveloper 5.）*/
	int64_t chunks=0,blocks=0,changed=0,ticks=0,pos=0,ents=0;
	int64_t skip_now=0,skip_new=0,bad_block=0,unwritable=0,no_section=0,old_bad=0,bad_poi=0,bad_entity=0;
	int64_t bad_regions=0,save_fail=0;
};
static bool check_ChunkPos(const Chunk *c,int32_t cx,int32_t cz){/*xPos/zPos 在 body 那一层（老格式是 Level），游戏是 getIntOr*/
	const nbt_Compound *b=c?get_ChunkBody(&c->con):NULL;
	return b&&get_TagInt(b->get("xPos"))==std::optional<int32_t>(cx)&&get_TagInt(b->get("zPos"))==std::optional<int32_t>(cz);
}
static bool has_SectionBlockEntity(const Chunk *c,int32_t sy){/*这一段Y范围内有没有方块实体：有就不能整段跳过*/
	const nbt_Compound *b=c?get_ChunkBody(&c->con):NULL;
	const nbt_Any *ba=b?b->get(get_ChunkLayout(&c->con).block_entities):NULL;
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
		int32_t y=get_TagInt(((const nbt_Compound*)e)->get("y")).value_or(0);/*游戏是 getIntOr(y,0)*/
		if(y>=lo&&y<=hi){
			return true;
		}
	}
	return false;
}

/*new 的这一段能不能写：段在、block_states 是 compound。用户定的：merge 不补段，缺段（和纯光段）的格子跳过
  （游戏写出来的区块在世界高度内每段都有，全空气的也有）。*/
static bool has_SectionStates(const Chunk *dst,int32_t sy){
	const nbt_Compound *sec=dst->get_Section(sy);
	Chunk_Layout L=get_ChunkLayout(&dst->con);
	const nbt_Compound *bs=get_SectionStates(sec,L);
	if(!bs){
		return false;
	}
	if(L.states){
		return true;
	}
	const nbt_Any *p=bs->get(L.palette),*d=bs->get(L.data);/*2832 以前：Palette 和 BlockStates 两样都在才算这一段有方块*/
	return p&&p->type==NBT_LIST&&d&&d->type==NBT_LONG_ARRAY;
}

/*计划刻：now 里落在写范围内的条目，深拷贝追加到 new 的同名表；
  表形状读不出来就不动，返回-1；返回追加了几条。*/
static int append_Ticks(const Chunk *now_c,Chunk *new_c,const Block_Range &rg,bool fluid){
	int n=count_ChunkTicks(&now_c->con,fluid);
	if(n<0){
		return -1;
	}
	if(n==0){
		return 0;
	}
	Chunk_Layout L=get_ChunkLayout(&new_c->con),SL=get_ChunkLayout(&now_c->con);/*两边版本可以不同：表名各按各的布局*/
	const char *name=fluid?L.fluid_ticks:L.block_ticks;
	const char *src_name=fluid?SL.fluid_ticks:SL.block_ticks;
	nbt_Compound *body=get_ChunkBody(&new_c->con);
	const nbt_Compound *src=get_ChunkBody(&now_c->con);
	if(!body||!src){
		return -1;
	}
	nbt_Any *ta=body->get(name);
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
		body->set(name,tl);
	}
	const nbt_Any *raw=src->get(src_name);
	if(!raw||raw->type!=NBT_LIST){
		return -1;
	}
	const nbt_List *rl=(const nbt_List*)raw;
	int added=0;
	for(int i=0;i<n;i++){
		Chunk_Tick t;
		if(!get_ChunkTick(&now_c->con,fluid,i,&t)){
			return -1;
		}
		if(!rg.is_Inside(t.x,t.y,t.z)){
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

static bool check_EntityList(const std::vector<Entity> &list){/*每条实体游戏都读得进来（有一条坏的，这一处来源整个不搬）*/
	for(size_t i=0;i<list.size();i++){
		if(!list[i].valid()){
			return false;
		}
	}
	return true;
}
static void keep_EntityInRange(std::vector<Entity> &got,const Block_Range &rg,std::vector<Entity> &out){/*落在写范围里的挪到 out*/
	for(size_t i=0;i<got.size();i++){
		if(is_EntityInRange(&got[i].con,rg)){
			out.push_back(std::move(got[i]));
		}
	}
	got.clear();
}
/*实体：now 这个区块的实体（游戏两处都读：区块里的实体表、entities/ 的实体区块），只留写范围里的。
  一处来源读不出来或者有一条坏实体，这一处整个不搬，计一次坏实体*/
static void get_NowEntities(const Chunk *now_c,MCA &now_ent,uint32_t lx,uint32_t lz,int32_t cx,int32_t cz,
	const Block_Range &rg,std::vector<Entity> &out,Merge_Stat &st){
	std::vector<Entity> got;
	if(get_ChunkEntities(now_c->data(),&got)<0||!check_EntityList(got)){
		st.bad_entity++;
		got.clear();
	}
	keep_EntityInRange(got,rg,out);
	if(now_ent.bad){
		st.bad_entity++;
		return;
	}
	std::optional<nbt_Compound> src=now_ent.get_Compound(lx,lz);
	if(!src){
		if(now_ent.last_error!=MCA_READ_NO_CHUNK){
			st.bad_entity++;/*自定义压缩/坏压缩/NBT坏：不能当“没有这块”*/
		}
		return;
	}
	if(!check_EntityContainer(&*src,cx,cz)||get_ChunkEntities(&*src,&got)<0||!check_EntityList(got)){
		st.bad_entity++;
		return;
	}
	keep_EntityInRange(got,rg,out);
}
static bool add_Entities(nbt_Compound *dst,const std::vector<Entity> &list){/*逐条追加；表的形状在第一条就查出来，失败时一条没写*/
	for(size_t i=0;i<list.size();i++){
		if(!add_ChunkEntity(dst,list[i])){
			return false;
		}
	}
	return true;
}
/*实体写进 new 的实体区块（2681 起的版本）：目标块读不出来/形状坏就跳过，不覆盖成新容器、不往垃圾里追加*/
static bool put_EntityFile(MCA &new_ent,uint32_t lx,uint32_t lz,int32_t cx,int32_t cz,int32_t dv,
	const std::vector<Entity> &list,Merge_Stat &st){
	if(new_ent.bad){
		st.bad_entity++;
		return false;
	}
	std::optional<nbt_Compound> dst=new_ent.get_Compound(lx,lz);
	if(!dst){
		if(new_ent.last_error!=MCA_READ_NO_CHUNK){
			st.bad_entity++;
			return false;
		}
		dst=make_EntityChunk(cx,cz,dv);
	}else if(!check_Entity(&*dst,cx,cz)){
		st.bad_entity++;
		return false;
	}
	if(!dst->get("DataVersion")&&dv>0){
		dst->set_Int("DataVersion",dv);/*没写游戏当 -1，会把整块从头升级一遍*/
	}
	if(!add_Entities(&*dst,list)){
		st.bad_entity++;
		return false;
	}
	if(!new_ent.set_Compound(*dst,lx,lz)){
		st.save_fail++;
		return false;
	}
	st.ents+=(int64_t)list.size();
	return true;
}
/*POI：now 的记录（按 now 那块 POI 自己的版本读），写范围里的放进 new（按 new 那块的版本写；一格只留一条，now 的顶掉 new 的）。
  返回放了几条，-1＝读不出来/写不进去（new 那块一点不改）*/
static int merge_POIChunk(MCA &now_poi,MCA &new_poi,uint32_t lx,uint32_t lz,int32_t dv,const Block_Range &rg,Merge_Stat &st){
	if(now_poi.bad||new_poi.bad){
		st.bad_poi++;
		return -1;
	}
	std::optional<nbt_Compound> src=now_poi.get_Compound(lx,lz);
	if(!src){
		if(now_poi.last_error!=MCA_READ_NO_CHUNK){
			st.bad_poi++;/*自定义压缩/坏压缩/NBT坏：不能当“没有这块”*/
			return -1;
		}
		return 0;
	}
	std::vector<POI> all,recs;
	int bad=list_POI(&*src,&all);
	if(bad<0){
		st.bad_poi++;
		return -1;
	}
	st.bad_poi+=bad;
	std::map<std::array<int32_t,3>,size_t> at;/*一格只留一条，照游戏 PoiSection.add：同类型留先来的，不同类型后来的顶掉*/
	for(size_t i=0;i<all.size();i++){
		if(!rg.is_Inside(all[i].x,all[i].y,all[i].z)){
			continue;
		}
		std::array<int32_t,3> k{all[i].x,all[i].y,all[i].z};
		std::map<std::array<int32_t,3>,size_t>::iterator it=at.find(k);
		if(it==at.end()){
			at.emplace(k,recs.size());
			recs.push_back(std::move(all[i]));
		}else if(recs[it->second].type!=all[i].type){
			recs[it->second]=std::move(all[i]);
		}
	}
	if(recs.empty()){
		return 0;
	}
	std::optional<nbt_Compound> dst=new_poi.get_Compound(lx,lz);
	if(!dst){
		if(new_poi.last_error!=MCA_READ_NO_CHUNK){
			st.bad_poi++;/*目标块读不出来：跳过，不能覆盖成新容器*/
			return -1;
		}
		dst=make_POIChunk(dv);
	}else if(!check_POI(&*dst)){
		st.bad_poi++;/*形状坏：别往里写，留给 fixer*/
		return -1;
	}
	if(!dst->get("DataVersion")&&dv>=POI_SECTIONS){
		dst->set_Int("DataVersion",dv);/*没写游戏当 1945，Sections 会被盖掉*/
	}
	for(size_t i=0;i<recs.size();i++){/*在拷贝上写：老写法装不下的那条失败时 new 那块一点不改*/
		if(!set_POI(&*dst,recs[i].x,recs[i].y,recs[i].z,recs[i])){
			st.bad_poi++;
			return -1;
		}
	}
	if(!new_poi.set_Compound(*dst,lx,lz)){
		st.save_fail++;
		return -1;
	}
	st.pos+=(int64_t)recs.size();
	return (int)recs.size();
}

struct Merge_Map{/*一个存档：路径 + 它的 Level（now/new/每个 old 各一个）*/
	std::string path;
	Level *lv=NULL;
};
static bool save_MCA(MCA &mca,const std::string &file,bool &dirty){/*dirty 时把 poi/entities 的区域镜像落盘（目录不在就建）；成功后 dirty 置回 false*/
	if(!dirty){
		return true;
	}
	if(mca.bad){
		return false;
	}
	std::string parent=get_PathParent(file);
	if(!parent.empty()&&!make_Dir(parent)){
		return false;
	}
	if(!mca.save(file.c_str())){
		return false;
	}
	dirty=false;
	return true;
}
/*合一个维度：按区域→区块→段→格子走；一个区域的区块处理完就落盘一次，再存这个区域的实体/POI。
  返回 false＝有存盘失败或者读不出来的区域*/
static bool merge_Dim(const std::string &dim,const Merge_Map &now,const Merge_Map &dst,
	const std::vector<Merge_Map> &olds,const Block_Range &rg){
	std::string now_rel=now.lv->get_DimensionPath(dim.c_str(),"region");
	std::string dst_rel=dst.lv->get_DimensionPath(dim.c_str(),"region");
	std::string now_ent_rel=now.lv->get_DimensionPath(dim.c_str(),"entities");
	std::string dst_ent_rel=dst.lv->get_DimensionPath(dim.c_str(),"entities");
	std::string now_poi_rel=now.lv->get_DimensionPath(dim.c_str(),"poi");
	std::string dst_poi_rel=dst.lv->get_DimensionPath(dim.c_str(),"poi");
	if(now_rel.empty()||dst_rel.empty()||now_ent_rel.empty()||dst_ent_rel.empty()||
		now_poi_rel.empty()||dst_poi_rel.empty()){
		printf("merge: [%s] 维度路径无效\n",dim.c_str());
		return false;
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
	/*方块坐标→区块（/16）→区域（再/32），都向下取整。以前区域这一步写成了“方块坐标/32”，
	  区域范围每个方向宽了 16 倍：结果不会错（下面逐区块还会按 cx0..cx1 卡一遍），但范围外凡是有文件的区域都会被白读一遍*/
	int32_t cx0=rg.get_CX0(),cx1=rg.get_CX1(),cz0=rg.get_CZ0(),cz1=rg.get_CZ1();
	int32_t rx0=rg.get_RX0(),rx1=rg.get_RX1(),rz0=rg.get_RZ0(),rz1=rg.get_RZ1();
	/*段号是 Byte（-128..127），范围给得再大也只有这些段可能有东西；不夹的话 y 给到 ±20 亿时每个区块要空转上亿次*/
	int32_t sy0=std::max<int32_t>(get_SectionY(rg.y0),-128);
	int32_t sy1=std::min<int32_t>(get_SectionY(rg.y1),127);
	Merge_Stat st;
	/*只走 now 的 region 文件在的区域（列目录再和范围求交；范围给到整个世界时区域坐标有上百亿个，不能一个个试）*/
	std::vector<std::pair<int32_t,int32_t>> regions;
	for(const std::pair<int32_t,int32_t> &p:list_MCAFiles((now.path+"/"+now_rel).c_str())){
		if(p.first>=rx0&&p.first<=rx1&&p.second>=rz0&&p.second<=rz1){
			regions.push_back(p);
		}
	}
	Progress pr("merge",(int64_t)regions.size());
	printf("merge: [%s] 方块 x[%d,%d] y[%d,%d] z[%d,%d]，区块 x[%d,%d] z[%d,%d]，区域 x[%d,%d] z[%d,%d]\n",
		dim.c_str(),rg.x0,rg.x1,rg.y0,rg.y1,rg.z0,rg.z1,cx0,cx1,cz0,cz1,rx0,rx1,rz0,rz1);
	for(const std::pair<int32_t,int32_t> &rp:regions){
		int32_t rx=rp.first,rz=rp.second;
		{
			pr.tick();
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
			/*now 的表头上哪些格子有区块（不存在、偏移异常的不走）。先记下来：后面取区块时区域镜像可能被换出去*/
			std::bitset<1024> now_has;
			now_region=now.lv->get_MCA(dim.c_str(),rx,rz);
			for(uint32_t i=0;now_region&&!now_region->bad&&i<1024;i++){
				now_has[i]=now_region->has_Chunk(i%32,i/32);
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
					int64_t wx0=(int64_t)cx*16,wz0=(int64_t)cz*16;
					if(!now_has[(size_t)(cx&31)+(size_t)(cz&31)*32]){
						continue;/*now 没有这个区块：没东西可搬*/
					}
					Chunk *now_c=now.lv->get_Chunk(dim.c_str(),cx,cz);
					if(!now_c||!now_c->is_Full()||!check_Chunk(&now_c->con)||!check_ChunkPos(now_c,cx,cz)){
						st.skip_now++;
						continue;
					}
					Chunk *dst_c=dst.lv->get_Chunk(dim.c_str(),cx,cz);
					if(!dst_c||!dst_c->is_Full()||!check_Chunk(&dst_c->con)||!check_ChunkPos(dst_c,cx,cz)){
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
						if(c&&(!c->is_Full()||!check_Chunk(&c->con)||!check_ChunkPos(c,cx,cz))){
							c=NULL;
						}
						old_c.push_back(c);
					}
					int32_t lx0=(int32_t)std::max<int64_t>(0,(int64_t)rg.x0-wx0);
					int32_t lx1=(int32_t)std::min<int64_t>(15,(int64_t)rg.x1-wx0);
					int32_t lz0=(int32_t)std::max<int64_t>(0,(int64_t)rg.z0-wz0);
					int32_t lz1=(int32_t)std::min<int64_t>(15,(int64_t)rg.z1-wz0);
					int64_t wrote=0;/*每个区块带着自己的索引，段表/调色板只建一次*/
					for(int32_t sy=sy0;sy<=sy1;sy++){
						const nbt_Any *nc=NULL,*dc=NULL;
						State_Format nf=get_ChunkStateFormat(&now_c->con),df=get_ChunkStateFormat(&dst_c->con);
						bool nconst=is_SectionConst(now_c,sy,&nc);
						bool dconst=is_SectionConst(dst_c,sy,&dc);
						if(nconst&&dconst&&!has_SectionBlockEntity(now_c,sy)&&!has_SectionBlockEntity(dst_c,sy)){
							/*两边格式不同就不在这里比（编码不一样，深比较一定不等），下面逐格按方块语义比*/
							bool same=(nc==NULL&&dc==NULL)||(nc&&dc&&nf==df&&is_StateSame(nc,dc,nf));
							if(same){
								continue;/*整段一样（多数是空气），这一整段跳过*/
							}
						}
						int32_t ly0=std::max<int32_t>(0,rg.y0-sy*16);
						int32_t ly1=std::min<int32_t>(15,rg.y1-sy*16);
						/*按段打开视图：区块级的字段每段现读一次，逐格只走索引（get_BlockRef 每格都要现读一遍）*/
						Section_View nv=now_c->view_Section(sy),dv=dst_c->view_Section(sy);
						std::vector<Section_View> ov(old_c.size());
						for(size_t oi=0;oi<old_c.size();oi++){
							if(old_c[oi]){
								ov[oi]=old_c[oi]->view_Section(sy);
							}
						}
						for(int32_t lx_i=lx0;lx_i<=lx1;lx_i++){
							for(int32_t lz_i=lz0;lz_i<=lz1;lz_i++){
								for(int32_t ly=ly0;ly<=ly1;ly++){
									int64_t y=(int64_t)sy*16+ly;
									int64_t x=wx0+lx_i,z=wz0+lz_i;
									if(!rg.is_Inside(x,y,z)){
										continue;
									}
									st.blocks++;
									/*先零拷贝比状态：绝大多数格子相同，省掉Block的深拷贝和hash查找*/
									Block_Ref nbr,dbr;
									if(!nv.get_BlockRef(lx_i,ly,lz_i,&nbr)){
										st.bad_block++;
										continue;
									}
									if(!dv.get_BlockRef(lx_i,ly,lz_i,&dbr)){
										st.bad_block++;
										continue;
									}
									if(equal_BlockRef(nbr,dbr)){
										continue;
									}
									Block nb=now_c->get_Block(lx_i,(int32_t)y,lz_i);
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
										if(ov[oi].get_BlockRef(lx_i,ly,lz_i,&obr)&&
										   equal_BlockRef(obr,nb)){
											keep=true;
											break;
										}
									}
									if(keep){
										continue;
									}
									if(!has_SectionStates(dst_c,sy)){
										st.no_section++;
										continue;
									}
									bool ok=dst_c->set_Block(nb,lx_i,(int32_t)y,lz_i);
									dv=dst_c->view_Section(sy);/*改过树：视图重开*/
									if(!ok){
										st.unwritable++;
										continue;
									}
									wrote++;
								}
							}
						}
					}
					/*计划刻：now 的整张表按写范围过滤后追加；即使没搬方块也追加（用户要求无脑拷贝）*/
					int t0=append_Ticks(now_c,dst_c,rg,false);
					int t1=append_Ticks(now_c,dst_c,rg,true);
					if(t0<0||t1<0){
						st.bad_block++;
					}else{
						st.ticks+=t0+t1;
					}
					if(nbt_Compound *db=get_ChunkBody(&dst_c->con)){/*InhabitedTime 在 body 那一层（老格式是 Level）*/
						db->set_Long("InhabitedTime",wrote);
					}
					st.changed+=wrote;
					/*实体：now 两处来源读成 Entity；new 那块是 2681 以前的版本就写进区块的实体表（跟区块一起落盘），否则写实体区块*/
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					std::vector<Entity> ents;
					get_NowEntities(now_c,now_ent,lx,lz,cx,cz,rg,ents,st);
					bool ent_file=has_EntityFile(get_ChunkDataVersion(dst_c->data()));
					bool ent_in_chunk=false;
					if(!ent_file&&!ents.empty()){
						if(add_Entities(dst_c->data(),ents)){
							ent_in_chunk=true;
						}else{
							st.bad_entity++;
						}
					}
					if(!dst.lv->set_Chunk(dim.c_str(),cx,cz,*dst_c)){
						st.save_fail++;
						continue;/*区块没写进去：别再把实体/POI追加成不一致的状态*/
					}
					chunk_changed=true;
					if(ent_in_chunk){
						st.ents+=(int64_t)ents.size();
					}
					/*实体文件/POI：区块已经处理过，追加 now 的记录*/
					if(ent_file&&!ents.empty()&&put_EntityFile(dst_ent,lx,lz,cx,cz,dst.lv->data_version,ents,st)){
						ent_changed=true;
					}
					if(has_POIFile(dst.lv->data_version)&&merge_POIChunk(now_poi,dst_poi,lx,lz,dst.lv->data_version,rg,st)>0){/*new 没有 POI 的版本（1.13 等）：不搬、不建 poi/*/
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
	printf("merge: [%s] 跳过 now不可读 %lld、new不可写/没区块 %lld；坏方块 %lld、写不进 %lld、缺段跳过 %lld；坏区域 %lld、旧图坏 %lld、坏实体 %lld、坏POI %lld、存盘失败 %lld\n",
		dim.c_str(),(long long)st.skip_now,(long long)st.skip_new,(long long)st.bad_block,(long long)st.unwritable,(long long)st.no_section,
		(long long)st.bad_regions,(long long)st.old_bad,(long long)st.bad_entity,(long long)st.bad_poi,(long long)st.save_fail);
	return !st.save_fail&&!st.bad_regions;
}

int main(int argc,char **argv){/*解析参数、打开各个存档、规整范围，然后一个维度一个维度合；有维度失败返回 -1*/
	if(argc<10||argc>11){
		print_Usage(argv[0]);
		return -1;
	}
	std::string path_new=argv[1],path_now=argv[2];
	std::vector<std::string> old_paths;
	split_List(argv[3],old_paths);
	Block_Range rg;
	if(!parse_Range(argv+4,"merge",&rg)){
		return -1;
	}
	if(!is_Dir(path_new)||!is_Dir(path_now)){
		printf("merge: new/now 必须是目录\n");
		return -1;
	}
	if(is_SamePath(path_new,path_now)){
		printf("merge: new 和 now 是同一个目录（new是输出，会毁掉now）\n");
		return -1;
	}
	for(size_t i=0;i<old_paths.size();i++){
		if(!is_Dir(old_paths[i])){
			printf("merge: old 不是目录：%s\n",old_paths[i].c_str());
			return -1;
		}
		if(is_SamePath(path_new,old_paths[i])||is_SamePath(path_now,old_paths[i])){
			printf("merge: old 和 new/now 是同一个目录：%s\n",old_paths[i].c_str());
			return -1;
		}
	}
	std::vector<std::string> dims;
	if(argc>10){
		split_List(argv[10],dims);
	}
	if(dims.empty()){
		dims.push_back("minecraft:overworld");
	}
	Level now_lv(path_now.c_str(),64,8,2),dst_lv(path_new.c_str(),64,8,2);
	if(!now_lv.dat_ok||!dst_lv.dat_ok){
		printf("merge: now/new 的 level.dat 读不到 DataVersion，不干活\n");
		return -1;
	}
	dedupe_Dims({&now_lv,&dst_lv},dims,"merge");
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
	int ret=0;
	for(size_t i=0;i<dims.size();i++){
		if(!merge_Dim(dims[i],now,dst,olds,rg)){
			ret=-1;
		}
	}
	for(size_t i=0;i<olds.size();i++){
		delete olds[i].lv;
	}
	return ret;
}
