#include "nbt.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <algorithm>
#include <chrono>
/*merge.cpp：merge.py 的原型（单独编成 merge，链 libnbt.so）
 *	把 now（游玩的地图）里"相对旧版本改过"的方块，搬到 new（新版本空板）里，new 同时也是输出；
 *	old 是旧版本空板：now 里某个方块跟任意一个 old 一样，就说明它早就更新过了，不用再搬。
 *	合并的条件（和 merge.py 一样，只是写得直白）：
 *	  一个区块要 (now 有) and (new 有) and (至少一个 old 有) 才动；
 *	  一个方块要 (now 读得出来) and (now != new) and (now 和所有 old 都不一样) 才搬过去。
 *	  所以 old 一个都不给（给空串）＝什么也不改，只会打印统计。
 *	快的几处（结果和 python 一样，只是少干活）：
 *	  1. 每个区块顶算一遍"哪些 section 两边都是整段同一个方块"（大片空气/整段石头），这些整段跳；
 *	  2. 同一个方块状态指针对儿比过一次就记住（一列里连着几十个石头，比一次就够）；
 *	  3. 区块拿到手就直接在 Chunk 上取方块（带游标），不走 level 的缓存和维度名解析。
 *	和 python 的五点差别：
 *	  1. dst 缺某段 section 时，当成空气并补一段空 section（biome 抄 now 的），跟 amulet 自动建 section 一个意思；
 *	     没有 block_states 的段（starlight 那种只有光的段）也算空气；
 *	  2. 搬不动的（比如 palette 满 256 条）、以及 direct 调色板读不出的位置，会记数，最后一起报出来，不闷着；
 *	  3. "跳过"是一个概念，now 那边的几种原因（mca 不存在 / 表里没这个区块 / 数据读不出来 /
 *	     Status 不是 minecraft:full / 没有 Status 字段 / full 但一个方块都没有 / xPos/zPos 对不上），
 *	     外加 new 里没这个区块（**不新建**）、new 那边读不出来、old 里一个都没有——
 *	     这些格子**方块/实体/兴趣点一个都不碰**，分类计数报出来（见 merge_Dim 里的 sk_*，一共 10 类）。
 *	  4. now 里"没生成完/空壳"的区块整块跳过，保留 new 的地形：C2ME/生成中断留下的残块不是玩家的东西，
 *	     搬过去只会把新板戳出游戏永远不补的空洞（真实事故：服务器下界中间一圈；配套工具 ./prune 删这种残块，见ReadMe）。
 *	  5. InhabitedTime 严格按"这一趟搬过去的方块数"写：合并的区块＝opcnt，跳过（含 old 里没有）的位置＝0；
 *	     为此 new 那份**每个区块都要读出来看一眼**（哪怕 now 里压根没这块地），只有本来不是 0 的才写盘。
 *	     跳过的位置 0 是"没搬玩家的东西＝没住过"；new 里没有的区块不新建（见第3条）。
 *	用法：./merge <new_path> <now_path> <old_path[,old_path...]> <x1> <y1> <z1> <x2> <y2> <z2> <chunk_unit> [dims]
 *	      new_path  新版本空板，合并的输出（会被改写！）
 *	      now_path  要升级的地图，只读
 *	      old_path  旧版本空板，逗号隔开可以给多个，只读；一个都不给就写空串 ""
 *	      x1 y1 z1 x2 y2 z2  方块范围的两个角，谁大谁小都行（内部取 min/max），含两头的临界值
 *	      chunk_unit  1=以区块为单位（x/z 不裁，整块整块地看）；0=裁到上面的范围里
 *	      dims  可选，逗号隔开的维度名，默认 minecraft:overworld；例：minecraft:overworld,minecraft:the_nether
 *	其它参数（区块缓存槽数、区域镜像数、进度条）都在程序里定死，见下面的 merge_Config。
 */

/*命令行的范围参数合理性上限：一次最多去碰多少个区域文件（见下面 merge_World 里的说明，防手滑多打几个0）*/
#define MERGE_REGION_MAX (1<<22)
/*y方向的合理范围：MC自定义维度的 min_y 最低-2032、height 最高4064（wiki: Java Edition level format /
  dimension type），也就是世界最高到 y=2031。给到±4096足够，再往外就是坐标写错了：
  不卡这道的话，y范围多打几个0会让这个程序对着每个(x,z)白循环上亿层，还会整数溢出*/
#define MERGE_Y_MAX 4096
/*x/z方向的合理范围：世界边界是±29999984，这里放到±3亿（比边界宽10倍）。卡这道是为了防坐标溢出（UB）
  和"手滑多打几个0"：cx*16+16 这类算式只有|x|超过2^27才可能溢出*/
#define MERGE_XZ_MAX 300000000

struct merge_Config{/*merge.py 上面那堆配置；命令行只给路径/范围/维度，别的都是这里的默认值*/
	std::string path_new;/*新版本空板（同时也是输出）*/
	std::string path_now;/*游玩的地图（只读）*/
	std::vector<std::string> path_olds;/*旧版本空板（只读，可以多个；一个都没有就什么都不合并）*/
	int32_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;/*方块范围，含头尾临界值，内部会取 min/max*/
	std::vector<std::string> dims;/*要处理的维度（默认主世界）*/
	int chunk_unit=1;/*整区块处理，不按方块范围裁 x/z*/
	int cache_cnt=1;/*区块缓存槽数（merge 只要两个槽：now 一个、new 一个，old 是直接拿指针的）*/
	int region_cnt=8;/*区域镜像最多留几个*/
	int progress=1;/*终端上画进度条，非终端每处理完一个区域打一行*/
	int verbose=0;/*每个区块打一行*/
};

static double secs_Now(std::chrono::steady_clock::time_point t){/*从t到现在多少秒*/
	return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
}
static int dir_OK(const std::string &path){/*存档目录在不在*/
	struct stat st;
	return !stat(path.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static int dir_Same(const std::string &a,const std::string &b){/*两个路径是不是同一个目录（软链接指到同一处也算）*/
	struct stat sa,sb;
	return !stat(a.c_str(),&sa)&&!stat(b.c_str(),&sb)&&sa.st_dev==sb.st_dev&&sa.st_ino==sb.st_ino;
}
/*把实体/POI的区域镜像落盘：一个区块都没有就删文件（空文件等于没有，留着只是垃圾）。
  delete_cnt记删了几个文件；只有输出（new）这么干，now/old是只读的*/
static int mca_Save(const mca_Region &mca,const std::string &dir,int32_t rx,int32_t rz,int64_t *delete_cnt){
	char file[4096];
	mca_Path(file,sizeof(file),dir.c_str(),rx,rz);
	if(mca.bad){/*这个文件我们根本没读出来：写或删都是拿"没读到的东西"去覆盖，宁可不干（外面已经计过坏数据）*/
		printf("merge: 区域文件 %s 读不出来，不写也不删（免得把没读到的区块抹掉）\n",file);
		return 0;
	}
	if(mca.is_Empty()){
		struct stat st;
		if(!stat(file,&st)&&S_ISREG(st.st_mode)){/*存在才删（0字节、只有8KB头的也算空）*/
			if(remove(file)&&errno!=ENOENT){
				printf("merge: 删不掉空文件 %s\n",file);
				return 0;
			}
			(*delete_cnt)++;
		}
		return 1;
	}
	return mca.write_File(file);
}
static int file_Has_Chunk(const char *file){/*文件里至少有一个区块吗（只看8KB表头）*/
	FILE *fp=fopen(file,"rb");
	if(!fp){
		return 1;/*读不出来就当有东西，别乱删*/
	}
	byte head[8192];
	size_t n=fread(head,1,sizeof(head),fp);
	fclose(fp);
	if(n<8192){
		return 1;
	}
	for(size_t i=0;i<8192;i+=4){
		if(head[i]|head[i+1]|head[i+2]){
			return 1;
		}
	}
	return 0;
}
static int clean_Empty_Files(const std::string &dir,int64_t *zero,int64_t *hdr){
	/*清"空的"区域文件：0字节的、正好只有8KB头（1024格全空）的——游戏自己会留这种（生成时建了文件没写内容）。
	  只认 r.<数字>.<数字>.mca 这种名字（别的名字里含.mca的一律不碰），也只扫输出（new）的entities/poi目录；
	  比8KB大、但表头全空的**不删**（数据扇区可能还在，属于"表头被清零"的可救文件），只打一行提醒*/
	DIR *dp=opendir(dir.c_str());
	if(!dp){
		return 1;/*目录不在＝没什么可清的*/
	}
	struct dirent *de;
	int ok=1;
	while((de=readdir(dp))){
		int rx=0,rz=0;
		char tail=0;
		if(sscanf(de->d_name,"r.%d.%d.mca%c",&rx,&rz,&tail)!=2){/*名字必须正好是 r.x.z.mca*/
			continue;
		}
		std::string file=dir+"/"+de->d_name;
		struct stat st;
		if(stat(file.c_str(),&st)||!S_ISREG(st.st_mode)){
			continue;
		}
		if(st.st_size==0){
			if(remove(file.c_str())){
				printf("merge: 删不掉空文件 %s\n",file.c_str());
				ok=0;
			}else{
				(*zero)++;
			}
			continue;
		}
		if(st.st_size==8192&&!file_Has_Chunk(file.c_str())){/*正好一个头、没有数据区：确定是空的*/
			if(remove(file.c_str())){
				printf("merge: 删不掉空文件 %s\n",file.c_str());
				ok=0;
			}else{
				(*hdr)++;
			}
		}else if(st.st_size>8192&&!file_Has_Chunk(file.c_str())){/*表头全空但后面还有数据：不敢删，报一声*/
			printf("merge: 注意 %s：表头一格区块都没有，但文件里还有 %lld 字节数据，没敢删（要看就自己看）\n",
				file.c_str(),(long long)(st.st_size-8192));
		}
	}
	closedir(dp);
	return ok;
}
/*一条候选POI记录（从树里抄出来的：原树马上要free）*/
struct Merge_Poi{int32_t x,y,z,ft;std::string type;int from_now;};
static void poi_Dedup(std::vector<Merge_Poi> &v,int64_t *drop){
	/*同一个位置出现多条记录（坏数据：正常存档一个位置只有一条）：留一条，其余的当"删掉"。
	  不去重的话 keep 会比 new 那边多一条，每次跑都判成"有改动"→永远重写这个区块，不收敛*/
	if(v.size()<2){
		return;
	}
	std::sort(v.begin(),v.end(),[](const Merge_Poi &a,const Merge_Poi &b){
		if(a.x!=b.x){
			return a.x<b.x;
		}
		if(a.y!=b.y){
			return a.y<b.y;
		}
		return a.z<b.z;
	});
	size_t w=0;
	for(size_t i=0;i<v.size();i++){
		if(w&&v[w-1].x==v[i].x&&v[w-1].y==v[i].y&&v[w-1].z==v[i].z){
			(*drop)++;
			continue;
		}
		v[w++]=v[i];
	}
	v.resize(w);
}
static void poi_Collect(const nbt_Compound *chunk,int from_now,std::vector<Merge_Poi> &out,int64_t *bad){
	int n=poi_Count(chunk);
	for(int i=0;i<n;i++){
		int32_t x=0,y=0,z=0,ft=0;
		const char *t=NULL;
		if(!poi_Get_At(chunk,i,&x,&y,&z,&t,&ft)){/*这条形状不对：当坏数据丢掉，不往新世界带*/
			(*bad)++;
			continue;
		}
		Merge_Poi r;
		r.x=x;r.y=y;r.z=z;r.ft=ft;r.type=t?t:"";r.from_now=from_now;
		out.push_back(r);
	}
}
static int Block_Equal_Slow(const Block &a,const Block &b){/*python里 now_block==new_block 那种比较（方块+方块实体都算）*/
	if(a.block==b.block&&a.entity==b.entity){
		return 1;
	}
	return a.block&&b.block&&a.block->cmp(b.block)&&
		(a.entity&&b.entity?a.entity->cmp(b.entity):a.entity==b.entity);
}
/*一列里连着几十个同样的方块，同一个"指针对儿"会反复出现：比过一次就记下来。
  只记两个实体都为NULL的情况（有实体的话，方块实体会被换掉/删掉，地址可能被复用，别乱记）；
  每个区块开头顶掉一代，跨区块的旧记录就不算数了*/
struct Eq_Memo{const nbt_Any *a=NULL,*b=NULL;uint32_t gen=0;int eq=0;};
static Eq_Memo eq_memo[256];/*（单线程用的；以后要多线程合并，把这俩改成thread_local）*/
static uint32_t eq_gen=0;
static int Block_Equal(const Block &a,const Block &b){
	if(a.entity||b.entity){
		return Block_Equal_Slow(a,b);
	}
	Eq_Memo &m=eq_memo[(((uintptr_t)a.block>>4)^((uintptr_t)b.block>>4))&255];
	if(m.gen==eq_gen&&m.a==a.block&&m.b==b.block){
		return m.eq;
	}
	int eq=Block_Equal_Slow(a,b);
	m.a=a.block;
	m.b=b.block;
	m.gen=eq_gen;
	m.eq=eq;
	return eq;
}
static nbt_Compound *new_Air_States(){/*block_states:{palette:[air]}*/
	nbt_Compound *states=new nbt_Compound;
	nbt_List *palette=new nbt_List(0x0a);
	nbt_Compound *air=new nbt_Compound;
	air->set_String("Name","minecraft:air");
	palette->push_back(air);
	states->set("palette",palette);
	return states;
}
static int make_Section(Chunk *dst,Chunk *src,int32_t sy){/*dst没这段section就补一段空的（全空气），biome抄src的，返回是否补了*/
	if(dst->get_Section(sy)){
		return 0;
	}
	nbt_Any *s=src->get_Section(sy);
	if(!s){/*src也没有这段就不管了*/
		return 0;
	}
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte((uint8_t)(int8_t)sy));/*Y是带符号Byte*/
	sec->set("block_states",new_Air_States());
	const nbt_Any *biomes=((nbt_Compound*)s)->get("biomes");
	if(biomes){
		sec->set("biomes",biomes->copy());/*biome跟着src走，别给个空的*/
	}
	nbt_Any *secs=dst->get("sections");
	if(!secs||secs->type!=0x09){
		secs=new nbt_List(0x0a);
		dst->set("sections",secs);
	}
	((nbt_List*)secs)->push_back(sec);/*MC只认每段自己的Y，顺序无所谓*/
	return 1;
}
static int fill_Palette(nbt_Compound *sec){/*段在但没有block_states（比如starlight那种只有光的段）＝全空气，补个空palette好往里写*/
	if(sec->get("block_states")){
		return 0;
	}
	sec->set("block_states",new_Air_States());
	return 1;
}
static int band_Const(Chunk *a,Chunk *b,int32_t sy,Chunk_Cursor *ca,Chunk_Cursor *cb){
	/*两边这一段（一个section＝16个y）是不是整段同一个方块且一样？一样就能整段跳过（大片空气最常见）*/
	nbt_Any *va=NULL,*vb=NULL;
	if(!section_Const((nbt_Compound*)a->get_Section(sy,ca),ca,&va)||
	   !section_Const((nbt_Compound*)b->get_Section(sy,cb),cb,&vb)){
		return 0;/*有一边读不出来或者不止一种方块*/
	}
	if(va==vb){
		return 1;
	}
	return va&&vb&&va->cmp(vb);
}
static void mark_Bands(Chunk *c,Chunk_Cursor *cur,std::vector<uint8_t> &has,int32_t sy0,int32_t sy1){
	/*这个区块里有方块实体的那些section标一下：方块虽然整段一样，但实体可能不一样，不能整段跳*/
	nbt_List *bes=cur->block_Entities(c);
	for(size_t i=0;bes&&i<bes->con.size();i++){
		if(bes->con[i]->type!=0x0a){/*方块实体本该都是compound：不是就跳过（别硬转去读别人的内存）*/
			continue;
		}
		const nbt_Any *ey=((const nbt_Compound*)bes->con[i])->get("y");
		if(!ey||ey->type!=0x03){/*y是Int*/
			continue;
		}
		int32_t y=ey->as_Int();
		int32_t sy=section_Y(y);
		if(sy>=sy0&&sy<=sy1){
			has[sy-sy0]=1;
		}
	}
}
static void print_Bar(int64_t done,int64_t total,double secs){/*只在终端上画，重定向到文件就不画了（有区域那几行）*/
	if(!isatty(1)){
		return;
	}
	int w=30,fill=total?(int)(done*w/total):w;
	printf("\r%6.2f%% [",total?done*100.0/total:100.0);
	for(int i=0;i<w;i++){
		putchar(i<fill?'#':'-');
	}
	printf("] %lld/%lld chunk(s), %.1fs   ",(long long)done,(long long)total,secs);
	fflush(stdout);
}

/*跳过这一格时，把 new 那份的 InhabitedTime 清成 0。
  规矩（用户定）：合并的区块那边 InhabitedTime **严格等于这一趟搬过去的方块数**（见下面的 set_Long(opcnt)），
  跳过（一格方块都没搬）的位置就是 0——这些地方没搬玩家的东西，"被住过多久"不该留着。
  new 里没有这个区块时调用者根本不会调到这里（不新建，见 merge_Dim）。
  只写本来不是 0 的（第二趟跑要"0改动"，别为了一样的值把区块重写一遍）；没有这个字段就当它是 0。
  返回 1=写了（清了 0），0=本来就是 0/没有这个字段，不用动*/
static int it_Clear(Chunk *c,level &dst,int32_t cx,int32_t cz,const char *world){
	const nbt_Any *it=c->get("InhabitedTime");
	if(!it){
		return 0;
	}
	if(it->type==0x04&&it->as_Long()==0){/*标准的Long 0：什么都不用做*/
		return 0;
	}
	/*不是0（含"类型不对"这种坏数据：Int/Short 之类的值我们不猜，直接写成标准的 Long 0）*/
	c->set_Long("InhabitedTime",0);
	dst.mark_Changed(cx,cz,world);
	return 1;
}

/*==================== 一个维度跑一遍 ====================*/
static int merge_Dim(const merge_Config &cfg,int dim,int32_t x0,int32_t x1,int32_t y0,int32_t y1,int32_t z0,int32_t z1,
		level &now,level &dst,std::vector<level*> &olds){
	const char *world=level::dim_Name(dim);
	int64_t total=(int64_t)((x1>>4)-(x0>>4)+1)*((z1>>4)-(z0>>4)+1);/*python的tqdm总数：范围内的区块数*/
	int64_t done=0,ops=0,chunks=0,new_sec=0,air_sec=0,miss=0,reg_fail=0,blind_now=0,blind_dst=0,skip_pos=0;
	int64_t now_hit=0,dst_hit=0;/*真读到区块的个数，用来发现"路径不对→一个都没读到"*/
	int64_t bad_nbt0=nbt_bad_nbt,bad_chunk0=nbt_bad_chunk;/*这一维度开始时坏数据的计数（跑完看差值）*/
	int64_t bad_poi0=nbt_bad_poi,bad_ent0=nbt_bad_entity;
	/*实体：now有的整块搬过来覆盖new；POI：now+new的记录按"合并完那一格是哪个方块"定去留*/
	int64_t ent_chunks=0,ent_cnt=0,ent_empty=0,ent_bad=0,ent_fail=0,ent_file_del=0,ent_same=0;
	int64_t poi_keep=0,poi_drop=0,poi_add=0,poi_bad=0,poi_chunk_del=0,poi_file_del=0,poi_ft_odd=0;
	int64_t clean_zero=0,clean_hdr=0,warp=0;
	/*跳过的区块（一个方块都没搬）按原因记数——用户要的就是"跳过"这一个概念，下面这些只是分类：
	  sk_no_mca   now 那边连 mca 文件都没有（这片没生成过，正常）
	  sk_no_chunk now 的 mca 表里没这一格（这格没生成过，正常）
	  sk_bad_now  now 的区块/区域读不出来（坏数据，另外还计在 nbt_bad_* 里，退出码非0）
	  sk_not_full now 的区块 Status 是别的值（C2ME/中断留下的半成品）
	  sk_no_status now 的区块压根没有 Status 字段（老存档/别的工具写的）：也不是 full，一样跳过
	  sk_shell    now 的区块是 full 但一个方块都没有（空壳，游戏永远不会再补地形）
	  sk_warp     now/new 的 xPos/zPos 和文件位置对不上（存档有毛病，宁可不动）
	  sk_new      new 里没这个区块：不新建，直接跳过（板子上没有的地方不凭空造区块）
	  sk_new_bad  new 里的区块/区域读不出来（坏数据）
	  sk_no_old   这个位置 old 里一个都没有：判不出是不是玩家改的，整块不合并*/
	int64_t sk_no_mca=0,sk_no_chunk=0,sk_bad_now=0,sk_not_full=0,sk_no_status=0,sk_shell=0,sk_new=0,sk_new_bad=0,sk_no_old=0;
	int64_t it_zero=0;/*跳过的位置里，顺手把 new 的 InhabitedTime 清成 0 的格数*/
	int path_bad=0;
	/*没给old（old_path是空串）＝只统计、什么都不改：连实体/POI/空文件清理也一起跳过。
	  以前实体那段在 use.empty() 判断**之前**，所以"只统计"这趟照样会改new的实体、还会删空的entities/poi文件——
	  和文件头/提示语承诺的"什么也不改"不符（dry run把输出板改了是最坑的）*/
	int dry=olds.empty();
	int ent_dir_bad=0,poi_dir_bad=0;/*这一维度里"文件在但读不出来"的区域数（只有bad的那几个区域不写）*/
	std::string ent_dst_dir_base=cfg.path_new+"/"+dst.entities_dir[dim];
	std::string poi_dst_dir_base=cfg.path_new+"/"+dst.poi_dir[dim];
	int32_t sy0=section_Y(y0),sy1=section_Y(y1);/*y范围盖住的section*/
	std::vector<uint8_t> band_ok,band_be;/*每个section：能不能整段跳（和x,z无关，一个区块算一次）*/
	std::vector<int> use;/*这个区块在哪些old世界里存在（一个都没有就整块不合并）*/
	std::vector<Chunk*> old_c(olds.size(),NULL);/*那些old区块，取一次存着，别每个方块都走level的缓存*/
	std::chrono::steady_clock::time_point t_all=std::chrono::steady_clock::now();
	printf("merge: [%s] x[%d,%d] y[%d,%d] z[%d,%d] 区域目录 now=%s\n",
		world,x0,x1,y0,y1,z0,z1,(cfg.path_now+"/"+now.region_dir[dim]).c_str());
	printf("merge: [%s] 区域目录 new=%s\n",world,(cfg.path_new+"/"+dst.region_dir[dim]).c_str());
	for(size_t i=0;i<olds.size();i++){
		printf("merge: [%s] 区域目录 old=%s\n",world,(cfg.path_olds[i]+"/"+olds[i]->region_dir[dim]).c_str());
	}

	for(int32_t rx=x0>>9;rx<=x1>>9;rx++){/*>>9就是除512*/
		for(int32_t rz=z0>>9;rz<=z1>>9;rz++){
			int64_t reg_chunks=0,reg_ops=0,reg_sec=0;
			std::chrono::steady_clock::time_point t_reg=std::chrono::steady_clock::now();
			std::string ent_now_dir=cfg.path_now+"/"+now.entities_dir[dim];
			std::string ent_dst_dir=cfg.path_new+"/"+dst.entities_dir[dim];
			std::string poi_now_dir=cfg.path_now+"/"+now.poi_dir[dim];
			std::string poi_dst_dir=cfg.path_new+"/"+dst.poi_dir[dim];
			/*实体/POI和方块一样按区域来：一个区域四个镜像（实体/POI文件小，整片读进来最省事）*/
			mca_Region ent_now=mca_Region::read(ent_now_dir.c_str(),rx,rz);
			mca_Region ent_dst=mca_Region::read(ent_dst_dir.c_str(),rx,rz);
			mca_Region poi_now=mca_Region::read(poi_now_dir.c_str(),rx,rz);
			mca_Region poi_dst=mca_Region::read(poi_dst_dir.c_str(),rx,rz);
			int ent_changed=0,poi_changed=0;
			int32_t cx_begin=std::max(x0>>4,rx*32),cx_end=std::min((x1>>4)+1,rx*32+32);
			int32_t cz_begin=std::max(z0>>4,rz*32),cz_end=std::min((z1>>4)+1,rz*32+32);
			for(int32_t cx=cx_begin;cx<cx_end;cx++){
				for(int32_t cz=cz_begin;cz<cz_end;cz++){
					done++;
					int32_t sx=cx*16,sz=cz*16,ex=cx*16+16,ez=cz*16+16;
					if(!cfg.chunk_unit){/*不整区块处理就把x/z裁到方块范围里*/
						sx=std::max(sx,x0);
						sz=std::max(sz,z0);
						ex=std::min(ex,x1+1);
						ez=std::min(ez,z1+1);
					}
					/*两边都先读出来。new 那份**跳过时也要读**：InhabitedTime 要清 0（见 it_Clear）。
					  读不出来也没关系：下面按原因分类跳过，不会新建任何区块（写盘只在"要合并"和"清0"两条路上）*/
					Chunk *now_c=now.get_Chunk(cx,cz,world);
					int now_why=now.miss_why;/*get_Chunk 刚写进去的失败原因（成功是 GET_OK）*/
					if(now_c){
						now_hit++;
					}
					Chunk *dst_c=dst.get_Chunk(cx,cz,world);
					int dst_why=dst.miss_why;
					if(dst_c){
						dst_hit++;
					}
					/*---- 这一格要不要跳过：跳过＝一个方块都不搬（实体/POI也不碰），只把 new 的 InhabitedTime 清 0 ----*/
					int skip=0;
					if(!now_c){
						/*now 那边读不出来：mca不存在 / 表里没这区块 / 数据异常（用户说的前三种跳过）*/
						skip=1;
						if(now_why==level::GET_NO_FILE){
							sk_no_mca++;
						}else if(now_why==level::GET_NO_CHUNK){
							sk_no_chunk++;
						}else{/*坏数据（GET_BAD_FILE/GET_BAD_CHUNK，nbt_bad_* 里也计了数）：照样跳过，退出码会非0*/
							sk_bad_now++;
						}
					}else if(!dst_c){
						/*new 里没有这个区块：不新建，直接跳过（这格 new 本来就没东西，也没什么可清0的）*/
						skip=1;
						if(dst_why==level::GET_NO_FILE||dst_why==level::GET_NO_CHUNK){
							sk_new++;
						}else{
							sk_new_bad++;/*new 那份是坏数据：也跳过（nbt_bad_* 里计了，退出码非0）*/
						}
					}else{
						/*xPos/zPos和文件位置对不上（存档有毛病）：整块跳过。方块是按文件位置读写的，自洽；
						  但方块实体是按区块自己的xPos/zPos找的（chunk.cpp），两套坐标一打架就会在错的位置
						   搬/删实体——宁可不动（fixer里是同一条规矩）*/
						const nbt_Any *nx=now_c->get("xPos"),*nz=now_c->get("zPos");
						const nbt_Any *dx=dst_c->get("xPos"),*dz=dst_c->get("zPos");
						if((nx&&nx->type==0x03&&nx->as_Int()!=cx)||(nz&&nz->type==0x03&&nz->as_Int()!=cz)||
						   (dx&&dx->type==0x03&&dx->as_Int()!=cx)||(dz&&dz->type==0x03&&dz->as_Int()!=cz)){
							skip=1;
							warp++;
						}else{
							/*now里这一块是"没生成完/空壳"：Status 不是 minecraft:full 的一律当读不到（用户说的第四种跳过），
							  另外 Status=full 但一个方块都没有的"空壳"也算（游戏认为生成完了，永远不会再补地形）。
							  理由（真实事故，见ReadMeDeveloper）：C2ME/生成被中断会把这两种残块留在盘上，
							  它们不是玩家的东西，搬过去只会把新板戳出"游戏永远不补"的空洞；
							  now 里没有内容的格子，new 那边自己生成的地形才是我们要的。
							  （末地虚空区块也是0方块，跳过它没有坏处：两边本来就是空的，new保留自己那份）*/
							int st=chunk_Full_Status(now_c);/*1=Status是minecraft:full，0=不是full，-1=压根没有Status字段*/
							if(st==1){
								if(chunk_No_Block(now_c)){/*full但一个方块都没有的空壳：同样不能搬（搬过去就是空气）*/
									skip=1;
									sk_shell++;
								}
							}else{/*"Status!=full一律当读不到"（用户定的；没有Status字段也算不full，宁可不动）*/
								skip=1;
								if(st==0){
									sk_not_full++;
								}else{
									sk_no_status++;
								}
							}
						}
					}
					if(skip){
						/*跳过＝没搬方块：new 的 InhabitedTime 清 0。dry（没给old）那趟承诺"一个字都不改"，就不清了*/
						if(!dry&&dst_c&&it_Clear(dst_c,dst,cx,cz,world)){
							it_zero++;
						}
						if((done&15)==0){
							print_Bar(done,total,secs_Now(t_all));
						}
						continue;
					}
					use.clear();
					for(size_t i=0;i<olds.size();i++){
						Chunk *oc=olds[i]->get_Chunk(cx,cz,world);
						if(oc){
							use.push_back((int)i);
							old_c[i]=oc;
						}
					}
					if(use.empty()){/*(now and new and (old1 or old2 ...))才合并：一个old都没有就不动这一块。
						  方块/实体/兴趣点一个都不碰（手册承诺），所以这段要排在实体段**前面**——
						  以前实体段在前面，"old里一个都没有"的区块照样把 now 的实体覆盖进 new 了*/

						sk_no_old++;
						if(!dry&&it_Clear(dst_c,dst,cx,cz,world)){/*dry那趟什么都不改*/
							it_zero++;
						}
						if((done&15)==0){
							print_Bar(done,total,secs_Now(t_all));
						}
						continue;
					}
					/*---- 实体：now有就整块覆盖new，now没有就保留new的。
					  只对"这一次真参与合并"的区块做（被跳过的区块在上面就continue了，见use.empty()那段）----*/
					if(!dry&&ent_now.has_Chunk((uint32_t)(cx&31),(uint32_t)(cz&31))){
						nbt_Compound *ec=ent_now.read_Compound((uint32_t)(cx&31),(uint32_t)(cz&31));
						if(!ec||!entity_Check(ec,cx,cz)){
							ent_bad++;/*读不出来/形状不对：这一块跳过（entity_Check里已经计过数）*/
							if(ec){
								ec->free();
							}
						}else{
							const nbt_Any *es=ec->get("Entities");
							if(((const nbt_List*)es)->con.empty()){/*空表＝这里没实体：把new那一块删掉，不写空表*/
								if(!ent_dst.bad&&ent_dst.erase_Chunk((uint32_t)(cx&31),(uint32_t)(cz&31))){
									ent_changed=1;
								}
								ent_empty++;
							}else{
								/*内容一样就别写了：第二趟跑要"0改动"（时间戳也是改动，能不碰就不碰）*/
								int same=0;
								if(ent_dst.has_Chunk((uint32_t)(cx&31),(uint32_t)(cz&31))){
									nbt_Compound *old_ec=ent_dst.read_Compound((uint32_t)(cx&31),(uint32_t)(cz&31));
									if(old_ec){
										same=old_ec->cmp(ec);
										old_ec->free();
									}
								}
								if(same){
									ent_same++;
								}else if(ent_dst.bad){/*这个实体文件读不出来：不写它（写下去就把没读到的区块抹了）*/
									ent_fail++;
									ent_dir_bad=1;
								}else{
									if(ent_dst.data.size()<8192){
										ent_dst=mca_Region::empty();
									}
									if(ent_dst.write_ChunkAt(ec,(uint32_t)(cx&31),(uint32_t)(cz&31))){
										ent_changed=1;
										ent_chunks++;
										ent_cnt+=(int64_t)((const nbt_List*)es)->con.size();
									}else{
										ent_fail++;
									}
								}
							}
							ec->free();
						}
					}
					eq_gen++;/*上一个区块的备忘作废*/
					Chunk_Cursor *now_cur=now.cursor(),*dst_cur=dst.cursor();/*借level槽里的游标：换区块时level已经reset过了*/
					band_ok.assign((size_t)(sy1-sy0+1),0);/*整段跳的section先算出来，省得每个(x,z)都判一次*/
					band_be.assign((size_t)(sy1-sy0+1),0);
					mark_Bands(now_c,now_cur,band_be,sy0,sy1);
					mark_Bands(dst_c,dst_cur,band_be,sy0,sy1);
					for(int32_t s=sy0;s<=sy1;s++){
						band_ok[s-sy0]=(uint8_t)(!band_be[s-sy0]&&band_Const(now_c,dst_c,s,now_cur,dst_cur));
					}
					int32_t opcnt=0;
					for(int32_t x=sx;x<ex;x++){
						for(int32_t z=sz;z<ez;z++){
							int32_t y=y0;
							while(y<=y1){/*一个section一段地过：整段两边都一样的（大片空气/整段石头）直接跳*/
								int32_t sy=section_Y(y);
								int32_t yend=(sy+1)*16-1;
								if(yend>y1){
									yend=y1;
								}
								if(band_ok[sy-sy0]){
									skip_pos+=yend-y+1;
									y=yend+1;
									continue;
								}
								for(;y<=yend;y++){
									Block nb=now_c->get_Block(x&15,y,z&15,now_cur);
									if(!nb.block){/*now这边读不出方块*/
										nbt_Any *ns=now_c->get_Section(sy,now_cur);
										if(ns&&((nbt_Compound*)ns)->get("block_states")){
											blind_now++;/*段在、也有block_states，但palette读不出来（direct调色板）*/
										}
										continue;/*连block_states都没有的段就是全空气（starlight那种只有光的段），不用管*/
									}
									Block db=dst_c->get_Block(x&15,y,z&15,dst_cur);
									if(!db.block){
										nbt_Any *ds=dst_c->get_Section(sy,dst_cur);
										if(!ds){/*dst原来这段section都没有，当空气处理*/
											if(make_Section(dst_c,now_c,sy)){
												new_sec++;
												reg_sec++;
												db=dst_c->get_Block(x&15,y,z&15,dst_cur);
											}
										}else if(((nbt_Compound*)ds)->get("block_states")){/*段在但palette读不出来*/
											blind_dst++;
											continue;
										}else if(fill_Palette((nbt_Compound*)ds)){/*纯光段：补个空palette当空气*/
											air_sec++;
											db=dst_c->get_Block(x&15,y,z&15,dst_cur);
										}
									}
									if(!db.block||Block_Equal(nb,db)){/*一样，或者dst那边还是读不出来，跳过*/
										continue;
									}
									int same=0;
									for(size_t i=0;i<use.size();i++){
										Block ob=old_c[use[i]]->get_Block(x&15,y,z&15,olds[use[i]]->cursor());
										if(ob.block&&Block_Equal(nb,ob)){/*old里已经跟now一样了，说明早更新过*/
											same=1;
											break;
										}
									}
									if(same){
										continue;
									}
									if(dst_c->set_Block(nb,x&15,y,z&15,dst_cur)){
										opcnt++;
									}else{/*多半是palette满256条了*/
										miss++;
									}
								}
								y=yend+1;
							}
						}
					}
					/*禁用光照图，删了高度图；我算不了光照和高度，就让minecraft运行时算*/
					dst_c->erase("isLightOn");
					dst_c->erase("Heightmaps");
					dst_c->set_Long("InhabitedTime",opcnt);
					dst.mark_Changed(cx,cz,world);
					ops+=opcnt;
					reg_ops+=opcnt;
					chunks++;
					reg_chunks++;
					if(cfg.verbose){
						printf("chunk(%d,%d): %d op(s)\n",cx,cz,(int)opcnt);
					}
					/*---- POI：候选＝now的记录 + new的记录（同位置now覆盖new）；
					      逐条看"合并完那一格是哪个方块"，对不上/认不出/表里没有就删这条 ----
					      只有两边都没有记录的区块才什么都不做（保住板子自己世界生成的那份）；
					      dry（没给old）时整段跳过：那趟承诺"什么都不改"*/
					if(!dry){
						uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
						int has_n=poi_now.has_Chunk(lx,lz),has_d=poi_dst.has_Chunk(lx,lz);
						nbt_Compound *pn=has_n?poi_now.read_Compound(lx,lz):NULL;
						nbt_Compound *pd=has_d?poi_dst.read_Compound(lx,lz):NULL;
						if((has_n&&!pn)||(has_d&&!pd)||(pn&&!poi_Shape_Ok(pn))||(pd&&!poi_Shape_Ok(pd))){
							poi_bad++;/*读不出来/形状不对：这一块跳过，两边的记录都不动*/
						}else if(pn||pd){
							std::vector<Merge_Poi> cand,tmp,keep;
							int64_t bad=0;
							if(pn){
								poi_Collect(pn,1,cand,&bad);
								poi_Dedup(cand,&bad);/*now自己同一位置有多条（坏数据）：留一条，别的当删掉*/
							}
							if(pd){
								poi_Collect(pd,0,tmp,&bad);
								poi_Dedup(tmp,&bad);
								for(size_t i=0;i<tmp.size();i++){/*同位置now优先：new里这条不要了*/
									int dup=0;
									for(size_t j=0;j<cand.size();j++){
										if(cand[j].x==tmp[i].x&&cand[j].y==tmp[i].y&&cand[j].z==tmp[i].z){
											dup=1;
											break;
										}
									}
									if(!dup){
										cand.push_back(tmp[i]);
									}
								}
							}
							for(size_t i=0;i<cand.size();i++){
								if((cand[i].x>>4)!=cx||(cand[i].z>>4)!=cz){/*记录的坐标不在这个区块里：坏数据，删掉（别去查别人的方块）*/
									poi_drop++;
									continue;
								}
								const nbt_Any *blk=dst_c->get_Block(cand[i].x&15,cand[i].y,cand[i].z&15,dst_cur).block;
								const char *t=poi_Type_Of_Block(blk);
								if(t&&!cand[i].type.empty()&&!strcmp(t,cand[i].type.c_str())){
									keep.push_back(cand[i]);
									/*票照抄来源，不夹到上限：票数是"实体那边还剩几张"的镜像，改了就成假数据了。
									  超过上限只说明这条记录本来就怪（或者我们的表把类型对错了），报个数让人看见*/
									if(cand[i].ft<0||cand[i].ft>poi_Ticket_Max(t)){
										poi_ft_odd++;
									}
								}else{
									poi_drop++;/*方块对不上（含那张床没了的home）/认不出/表里没有：删这条*/
								}
							}
							int pd_n=pd?poi_Count(pd):0;
							int changed=0;
							if((int)keep.size()!=pd_n){
								changed=1;
							}else{
								for(size_t i=0;i<keep.size();i++){/*逐条比：位置+类型+票都一样才算没改（保证第二趟0改动）*/
									const nbt_Any *e=pd?poi_Get(pd,keep[i].x,keep[i].y,keep[i].z):NULL;
									const nbt_Any *t=e&&e->type==0x0a?((const nbt_Compound*)e)->get("type"):NULL;
									const nbt_Any *f=e&&e->type==0x0a?((const nbt_Compound*)e)->get("free_tickets"):NULL;
									if(!t||t->type!=0x08||!f||f->type!=0x03||
									   strcmp(t->as_Str(),keep[i].type.c_str())||(int32_t)f->as_Int()!=keep[i].ft){
										changed=1;
										break;
									}
								}
							}
							if(pd&&poi_Clean(pd)){/*new里Records空的段：记录集合＝段键集合，我们一起清掉*/
								changed=1;
							}
							for(size_t i=0;i<keep.size();i++){/*补了几条：now有、new里没有（或不一样）的*/
								const nbt_Any *e=pd?poi_Get(pd,keep[i].x,keep[i].y,keep[i].z):NULL;
								const nbt_Any *t=e&&e->type==0x0a?((const nbt_Compound*)e)->get("type"):NULL;
								const nbt_Any *f=e&&e->type==0x0a?((const nbt_Compound*)e)->get("free_tickets"):NULL;
								if(keep[i].from_now&&(!t||t->type!=0x08||!f||f->type!=0x03||
								   strcmp(t->as_Str(),keep[i].type.c_str())||(int32_t)f->as_Int()!=keep[i].ft)){
									poi_add++;
								}
							}
							poi_keep+=(int64_t)keep.size();
							if(bad){/*形状不对的记录（库里的bad_Poi已经计过数）：这些条也等于被删掉了*/
								poi_drop+=bad;
							}
							if(changed&&poi_dst.bad){/*这个POI文件读不出来：一个字都不写（写下去等于把没读到的记录抹掉）*/
								poi_bad++;
								poi_dir_bad=1;
							}else if(changed){
								if(keep.empty()){/*一条都不剩：这个区块的POI记录删掉*/
									if(!poi_dst.bad&&poi_dst.erase_Chunk(lx,lz)){
										poi_chunk_del++;
										poi_changed=1;
									}
								}else{
									nbt_Compound *out=new nbt_Compound;
									const nbt_Any *dv=dst_c->get("DataVersion");
									out->set_Int("DataVersion",dv&&dv->type==0x03?(int32_t)dv->as_Int():dst.data_version);
									for(size_t i=0;i<keep.size();i++){
										poi_Set(out,keep[i].x,keep[i].y,keep[i].z,keep[i].type.c_str(),keep[i].ft);
									}
									if(poi_dst.data.size()<8192){
										poi_dst=mca_Region::empty();
									}
									if(poi_dst.write_ChunkAt(out,lx,lz)){
										poi_changed=1;
									}else{
										printf("merge: POI写不进去 chunk(%d,%d)\n",cx,cz);
										poi_bad++;
									}
									out->free();
								}
							}
						}
						if(pn){
							pn->free();
						}
						if(pd){
							pd->free();
						}
					}
					if((done&15)==0){
						print_Bar(done,total,secs_Now(t_all));
					}
				}
			}
			/*实体/POI和方块一起落地：没有区块就删文件。
			  顺序是先方块region、再实体、最后POI：万一中途挂了/写失败，宁可"方块在、索引还指着旧东西"，
			  也别反过来变成"索引说有床/村民、方块却没写进去"（前者下一趟merge会照方块把索引修对）*/
			if(!dst.save()){/*每个区域存一次，和merge.py一样*/
				reg_fail++;
			}
			if(ent_changed&&!mca_Save(ent_dst,ent_dst_dir,rx,rz,&ent_file_del)){
				reg_fail++;
			}
			if(poi_changed&&!mca_Save(poi_dst,poi_dst_dir,rx,rz,&poi_file_del)){
				reg_fail++;
			}
			if(cfg.progress){
				printf("r.%d.%d: %lld chunk(s), %lld op(s), %lld new section(s), %.1fs\n",
					(int)rx,(int)rz,(long long)reg_chunks,(long long)reg_ops,(long long)reg_sec,secs_Now(t_reg));
			}
		}
	}
	if(isatty(1)&&cfg.progress){
		putchar('\n');
	}
	/*把new的entities/poi目录里"空的"区域文件清掉（游戏自己会留0字节/只有头的文件）；dry（没给old）时不动*/
	if(!dry&&(!clean_Empty_Files(ent_dst_dir_base, &clean_zero, &clean_hdr)||
	   !clean_Empty_Files(poi_dst_dir_base, &clean_zero, &clean_hdr))){
		reg_fail++;
	}
	if(!now_hit&&done&&!dir_OK(cfg.path_now+"/"+now.region_dir[dim])){/*一个区块都没读到，而且目录压根不存在：多半是布局认错了*/
		path_bad=1;
	}
	printf("merge: [%s] 合并 %lld 个区块，%lld 次改动，新补 %lld 段section，失败 %lld 个，区域存盘失败 %lld 次，用时 %.1fs\n",
		world,(long long)chunks,(long long)ops,(long long)new_sec,(long long)miss,(long long)reg_fail,secs_Now(t_all));
	if(!now_hit&&done){
		printf("merge: [%s] 警告：%s 在这%d个区块里一个都没读到，now的区域目录是 %s，看看路径/存档结构对不对\n",
			world,cfg.path_now.c_str(),(int)done,(cfg.path_now+"/"+now.region_dir[dim]).c_str());
	}
	if(!dst_hit&&now_hit){
		printf("merge: [%s] 警告：%s 在now有区块的地方一个都没有，new的区域目录是 %s（空世界？还没生成？）\n",
			world,cfg.path_new.c_str(),(cfg.path_new+"/"+dst.region_dir[dim]).c_str());
	}
	if(skip_pos){
		printf("merge: [%s] 其中 %lld 个位置是整段一样的（大片空气/整段同一种方块），直接跳过的\n",world,(long long)skip_pos);
	}
	if(air_sec){
		printf("merge: [%s] 顺带给 %lld 段只有光的空section补了空palette（当空气用）\n",world,(long long)air_sec);
	}
	if(dry){
		printf("merge: [%s] 没给old：这一趟只统计（方块/实体/POI/空文件清理/InhabitedTime 全都跳过），new 一个字节都没改\n",world);
	}else{
		printf("merge: [%s] 实体：now有 %lld 个区块，搬了 %lld 个（%lld 个实体），和new一样没动 %lld 个，now空表删掉 %lld 个，读不出来/形状不对 %lld 个，写不进去 %lld 个\n",
			world,(long long)(ent_chunks+ent_empty+ent_bad+ent_same+ent_fail),(long long)ent_chunks,(long long)ent_cnt,
			(long long)ent_same,(long long)ent_empty,(long long)ent_bad,(long long)ent_fail);
		printf("merge: [%s] POI：留 %lld 条、删 %lld 条（方块对不上/认不出/重复/坏记录）、补 %lld 条；删空区块 %lld 个、删空文件 %lld 个（实体 %lld + POI %lld）\n",
			world,(long long)poi_keep,(long long)poi_drop,(long long)poi_add,
			(long long)poi_chunk_del,(long long)(ent_file_del+poi_file_del),(long long)ent_file_del,(long long)poi_file_del);
	}
	if(ent_dir_bad||poi_dir_bad){/*文件在但读不出来：那个区域的实体/POI一个字没写（见mca_Region::bad）*/
		printf("merge: [%s] 有读不出来的实体/POI区域文件（实体 %d 个区域、POI %d 个区域）：这些区域的表一个字都没改\n",
			world,ent_dir_bad,poi_dir_bad);
	}
	if(warp){/*xPos/zPos和文件位置对不上：整块跳过，方块/实体/POI都不动*/
		printf("merge: [%s] 有 %lld 个区块的xPos/zPos和文件位置对不上，整块跳过了（存档有毛病，宁可不动）\n",world,(long long)warp);
	}
	if(sk_no_mca||sk_no_chunk||sk_bad_now||sk_not_full||sk_no_status||sk_shell||sk_new||sk_new_bad||sk_no_old||warp){
		/*"跳过"＝这一格一个方块都没搬。用户点名的四类（now 的 mca 不存在 / 表里没这区块 / 数据读不出来 /
		  Status 不是 full）＋没有Status字段＋空壳＋new里没有（不新建）＋old里没有（判不出），
		  另有 xPos/zPos 对不上的（那类算失败，下面还会单独报一次）。*/
		printf("merge: [%s] 跳过 %lld 个区块（一个方块都没搬）：now那边 mca不存在 %lld、表里没这个区块 %lld、"
			"数据读不出来 %lld、Status不是full %lld、没有Status字段 %lld、full但一个方块都没有 %lld、xPos/zPos对不上 %lld；"
			"new里没这个区块 %lld（不新建）、new那边读不出来 %lld；old里一个都没有 %lld\n",
			world,(long long)(sk_no_mca+sk_no_chunk+sk_bad_now+sk_not_full+sk_no_status+sk_shell+sk_new+sk_new_bad+sk_no_old+warp),
			(long long)sk_no_mca,(long long)sk_no_chunk,(long long)sk_bad_now,(long long)sk_not_full,(long long)sk_no_status,
			(long long)sk_shell,(long long)warp,(long long)sk_new,(long long)sk_new_bad,(long long)sk_no_old);
		if(dry){
			printf("merge: [%s]   没给old：这一趟什么都不改，跳过的位置也没去清 InhabitedTime（要清就带上old跑）\n",world);
		}else{
			printf("merge: [%s] 跳过的位置把 new 那份的 InhabitedTime 清成 0（这趟清了 %lld 格；合并的区块那边严格等于搬过去的方块数）\n",
				world,(long long)it_zero);
		}
		if(sk_shell||sk_not_full){
			printf("merge: [%s]   这些位置要是new那边也空着，用 ./prune 把now/新板里的残块删掉，让游戏按种子重新生成\n",world);
		}
	}
	if(clean_zero||clean_hdr){
		printf("merge: [%s] 顺手清掉 %lld 个0字节的区域文件、%lld 个只有8KB头没有区块的区域文件（entities/poi目录，只清new）\n",
			world,(long long)clean_zero,(long long)clean_hdr);
	}
	if(poi_ft_odd){/*照抄但明显不对劲的票数：报一声，不改数据*/
		printf("merge: [%s] 有 %lld 条记录的票数超过该类型的上限（数据本来就不对，照抄没改；也可能是我们表里把类型对错了）\n",
			world,(long long)poi_ft_odd);
	}
	if(blind_now||blind_dst){
		printf("merge: [%s] 有 %lld 个位置now读不出、%lld 个位置new读不出（direct调色板，超过256种方块状态），这些位置没动\n",
			world,(long long)blind_now,(long long)blind_dst);
	}
	/*坏区块只跳过它自己，不中断整趟；这些区块一个字没改，所以照样算"有失败"，退出码非0*/
	{
		int64_t bad=(nbt_bad_nbt-bad_nbt0)+(nbt_bad_chunk-bad_chunk0);
		int64_t bad_poi=nbt_bad_poi-bad_poi0,bad_ent=nbt_bad_entity-bad_ent0;
		if(bad||bad_poi||bad_ent||poi_bad){
			printf("merge: [%s] 有坏数据（NBT坏 %lld、压缩坏 %lld、实体区块 %lld、POI区块/记录坏 %lld 处），"
				"另有 %lld 个POI区块形状不对（Sections/Records不是该有的类型）整块跳过；这些位置一个字没动\n",
				world,(long long)(nbt_bad_nbt-bad_nbt0),(long long)(nbt_bad_chunk-bad_chunk0),
				(long long)bad_ent,(long long)bad_poi,(long long)poi_bad);
			if(miss||reg_fail||path_bad||now.err_cnt||dst.err_cnt||warp||ent_dir_bad||poi_dir_bad){
				printf("merge: [%s] 还有别的失败（改不动%lld、存盘失败%lld、路径不对%d、xPos对不上%lld、读写出错now=%d new=%d），上面的输出里找原因\n",
					world,(long long)miss,(long long)reg_fail,path_bad,(long long)warp,now.err_cnt,dst.err_cnt);
			}
			return -1;
		}
	}
	if(miss||reg_fail||path_bad||now.err_cnt||dst.err_cnt||warp||ent_dir_bad||poi_dir_bad){
		printf("merge: [%s] 有失败（改不动%lld、存盘失败%lld、路径不对%d、xPos对不上%lld、读写出错now=%d new=%d），上面的输出里找原因\n",
			world,(long long)miss,(long long)reg_fail,path_bad,(long long)warp,now.err_cnt,dst.err_cnt);
		return -1;
	}
	return (int)chunks;
}

/*==================== 跑完所有维度 ====================*/
int merge_World(const merge_Config &cfg){
	if(!dir_OK(cfg.path_now)||!dir_OK(cfg.path_new)){/*old可以为空，now/new必须是真的目录*/
		printf("merge: 目录不对 new=%s now=%s\n",cfg.path_new.c_str(),cfg.path_now.c_str());
		return -1;
	}
	int32_t x0=cfg.x0,x1=cfg.x1,y0=cfg.y0,y1=cfg.y1,z0=cfg.z0,z1=cfg.z1;
	if(x0>x1){
		std::swap(x0,x1);
	}
	if(y0>y1){
		std::swap(y0,y1);
	}
	if(z0>z1){
		std::swap(z0,z1);
	}
	/*范围合理性：坐标打错一个0（比如把 1000 打成 1000000000）就会去翻上百万个区域文件，
	  看着像卡死。这里按"要碰多少个区域文件"卡一道，超了直接不干，别让手滑变成事故*/
	int64_t regions=(int64_t)((x1>>9)-(x0>>9)+1)*((z1>>9)-(z0>>9)+1);
	if(regions>MERGE_REGION_MAX){
		printf("merge: 范围太大（x[%d,%d] z[%d,%d] 要碰 %lld 个区域文件，上限 %d），坐标是不是写错了？什么都没干\n",
			x0,x1,z0,z1,(long long)regions,MERGE_REGION_MAX);
		return -1;
	}
	if(y0<-MERGE_Y_MAX||y1>MERGE_Y_MAX){/*y范围也有个谱：见MERGE_Y_MAX的说明*/
		printf("merge: y范围不对（[%d,%d]，只在 ±%d 以内），坐标是不是写错了？什么都没干\n",y0,y1,MERGE_Y_MAX);
		return -1;
	}
	if(x0<-MERGE_XZ_MAX||x1>MERGE_XZ_MAX||z0<-MERGE_XZ_MAX||z1>MERGE_XZ_MAX){
		/*x/z也要有个谱：世界边界是±29999984，这里给到±3亿。
		  不卡的话 cx*16+16 这种算式在 x1=INT32_MAX 时会溢出（UB），区块被"一格不搬"却照样收尾落盘*/
		printf("merge: x/z范围不对（x[%d,%d] z[%d,%d]，只在 ±%d 以内），坐标是不是写错了？什么都没干\n",
			x0,x1,z0,z1,MERGE_XZ_MAX);
		return -1;
	}
	if(dir_Same(cfg.path_new,cfg.path_now)){/*new就是now：这个程序是一边读now一边写new的，
	                                          同目录会把要升级的地图当场改坏，宁可不干*/
		printf("merge: new_path 和 now_path 是同一个目录（%s）：new 会被改写、now 只读，必须分开给\n",cfg.path_new.c_str());
		return -1;
	}
	level now(cfg.path_now.c_str(),cfg.cache_cnt,cfg.region_cnt);
	level dst(cfg.path_new.c_str(),cfg.cache_cnt,cfg.region_cnt);
	std::vector<level*> olds;
	int ret=0,fail=0;
	if(!now.dat_ok||!dst.dat_ok){/*level.dat读不到就没法按DataVersion定维度目录，宁可不干*/
		printf("merge: %s 读不到 level.dat（不是存档目录？），什么都没干\n",
			!now.dat_ok?cfg.path_now.c_str():cfg.path_new.c_str());
		return -1;
	}
	for(size_t i=0;i<cfg.path_olds.size();i++){
		if(!dir_OK(cfg.path_olds[i])){/*old给错路径不能装看不见：那样会漏判"已经更新过"，白搬一堆*/
			printf("merge: old目录不对 %s\n",cfg.path_olds[i].c_str());
			for(size_t k=0;k<olds.size();k++){
				delete olds[k];
			}
			return -1;
		}
		if(dir_Same(cfg.path_new,cfg.path_olds[i])){/*old也是new：old必须只读，同目录会被写坏*/
			printf("merge: old_path %s 和 new_path 是同一个目录，old 只读、new 会被改写，必须分开给\n",
				cfg.path_olds[i].c_str());
			for(size_t k=0;k<olds.size();k++){
				delete olds[k];
			}
			return -1;
		}
		level *o=new level(cfg.path_olds[i].c_str(),cfg.cache_cnt,cfg.region_cnt);
		if(!o->dat_ok){
			printf("merge: old目录 %s 读不到 level.dat（不是存档目录？）\n",cfg.path_olds[i].c_str());
			delete o;
			for(size_t k=0;k<olds.size();k++){
				delete olds[k];
			}
			return -1;
		}
		olds.push_back(o);
	}
	if(olds.empty()){
		printf("merge: 一个 old 都没给（old_path 是空串）：按规则 (now and new and old) 才合并，这次只会统计、不会改任何方块\n");
	}
	printf("merge: new=%s (DataVersion %d %s)\n",cfg.path_new.c_str(),dst.data_version,dst.version_name.c_str());
	printf("merge: now=%s (DataVersion %d %s)\n",cfg.path_now.c_str(),now.data_version,now.version_name.c_str());
	for(int d=0;d<(int)cfg.dims.size();d++){
		int dim=level::dim_Of(cfg.dims[d].c_str());
		int n=merge_Dim(cfg,dim,x0,x1,y0,y1,z0,z1,now,dst,olds);
		if(n<0){
			fail=1;/*失败要"粘住"：以前是 ret=-1 然后继续 ret+=n，后面的维度一旦搬了>=1个区块，
			         就把前面的失败加成正数、退出码变成0——多维度跑的时候坏数据会被当成成功*/
		}else if(!fail){
			ret+=n;
		}
	}
	for(size_t i=0;i<olds.size();i++){
		delete olds[i];/*析构里会close*/
	}
	olds.clear();
	return fail?-1:ret;
}

/*==================== 命令行 ====================*/
static void split_List(const char *s,std::vector<std::string> &out){/*逗号隔开的一串，空段跳过*/
	out.clear();
	if(!s){
		return;
	}
	std::string v=s;
	size_t p=0;
	while(p<v.size()){
		size_t q=v.find(',',p);
		if(q==std::string::npos){
			q=v.size();
		}
		std::string one=v.substr(p,q-p);
		if(!one.empty()){
			out.push_back(one);
		}
		p=q+1;
	}
}
static void usage(const char *me){
	printf("用法：%s <new_path> <now_path> <old_path[,old_path...]> <x1> <y1> <z1> <x2> <y2> <z2> <chunk_unit> [dims]\n",me);
	printf("  new_path   新版本空板，合并的输出（会被改写！）\n");
	printf("  now_path   要升级的地图，只读\n");
	printf("  old_path   旧版本空板，逗号隔开可以给多个，只读；一个都不给就写空串 \"\"\n");
	printf("  x1 y1 z1 x2 y2 z2  方块范围的两个角（谁大谁小都行，内部取min/max），含两头临界值\n");
	printf("  chunk_unit 1=以区块为单位（x/z 不裁）；0=裁到上面的范围里\n");
	printf("  dims       可选，逗号隔开的维度名，默认 minecraft:overworld\n");
	printf("             例：minecraft:overworld,minecraft:the_nether,minecraft:the_end\n");
	printf("例：%s ./new ./now ./old -1000 -63 -1000 1000 319 1000 1\n",me);
}
int main(int argc,char **argv){
	if(argc<11){/*3个路径+6个范围+chunk_unit=10个参数，维度可选*/
		usage(argv[0]);
		return -1;
	}
	merge_Config cfg;
	cfg.path_new=argv[1];
	cfg.path_now=argv[2];
	split_List(argv[3],cfg.path_olds);
	int32_t *nums[6]={&cfg.x0,&cfg.y0,&cfg.z0,&cfg.x1,&cfg.y1,&cfg.z1};
	static const char *nname[6]={"x1","y1","z1","x2","y2","z2"};
	for(int i=0;i<6;i++){
		if(!arg_Int(argv[4+i],nums[i])){
			printf("merge: %s 不是整数：%s\n",nname[i],argv[4+i]);
			return -1;
		}
	}
	int32_t cu=0;
	if(!arg_Int(argv[10],&cu)||(cu!=0&&cu!=1)){/*只认0/1：以前"abc"/2/-1 全当1，等于按另一个范围扫*/
		printf("merge: chunk_unit 只能是 0 或 1：%s\n",argv[10]);
		return -1;
	}
	cfg.chunk_unit=(int)cu;
	if(argc>11){
		split_List(argv[11],cfg.dims);
	}
	if(cfg.dims.empty()){
		cfg.dims.push_back("minecraft:overworld");
	}
	for(size_t i=0;i<cfg.dims.size();i++){/*去重：同一条命令里把同一维度写两遍，第二趟会把第一趟刚写好的
	                                          InhabitedTime 又清成 0（那种"重复跑就留不住"的坑），统计也会打两遍*/
		const char *w=cfg.dims[i].c_str();
		if(level::dim_Of(w)==0&&strcmp(w,"minecraft:overworld")&&strcmp(w,"overworld")){
			printf("merge: 维度名 %s 认不出来（只认 overworld/the_nether/the_end 这三种），改用主世界\n",w);
			cfg.dims[i]="minecraft:overworld";
		}
		for(size_t k=0;k<i;k++){
			if(cfg.dims[k]==cfg.dims[i]){
				printf("merge: 维度 %s 写了不止一次，只跑一趟\n",cfg.dims[i].c_str());
				cfg.dims.erase(cfg.dims.begin()+(long)i);
				i--;
				break;
			}
		}
	}
	int ret=merge_World(cfg);
	return ret<0?-1:0;
}
