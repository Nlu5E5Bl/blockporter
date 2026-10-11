#include "nbt.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
/*prune.cpp：把一个存档里"没生成完/空壳"的区块删掉，让游戏（或者Chunky）按种子重新生成它们。
 *	为什么要有这个工具（真实事故，详见ReadMeDeveloper"Chunky跳过已存在区块"那条）：
 *	  C2ME/生成被中断，会在盘上留两种残块——
 *	    1) Status 停在 structure_starts/biomes/carvers/... 的半成品：地形根本没写；
 *	    2) Status=minecraft:full 但一个方块都没有的"空壳"：更坑，游戏认为这一块早就生成完了，
 *	       永远不会再补地形，于是地图中间就是一个空洞（人走进去看得见、但怎么加载都不长）。
 *	  Chunky（以及"在已有世界上接着跑"的生成流程）判断"盘上有这个区块＝已经生成过"，
 *	  于是这一圈永远不被生成、残块永远留着。把残块从区域文件里删掉（表头那一格置0）＝
 *	  这块地重新变成"没生成"，下次加载/下次 chunky start 就会老老实实生成。
 *	删的一定是残块，所以不会丢东西：一个方块都没有的区块删了本来也没内容；
 *	Status != full 的半成品不是玩家的成果（玩家玩过的区块一定是 full）。
 *	用法：./prune <path> <x1> <z1> <x2> <z2> <mode> [dims]
 *	      path  存档目录（会被改写！先拿副本跑）
 *	      x1 z1 x2 z2  方块范围的两个角，谁大谁小都行（内部取 min/max，按区块整块处理）
 *	      mode  0=只统计不删（dry）
 *	            1=只删"没生成完"的（Status != minecraft:full）
 *	            2=只删"空壳"的（一个方块都没有）
 *	            3=两种都删（一般用这个）
 *	      dims  可选，逗号隔开的维度名，默认 minecraft:overworld；
 *	            例：./prune /副本 0 0 511 511 3 minecraft:overworld,minecraft:the_nether
 *	删区块时连 entities/poi 里对应的那一格一起删（不然会留下指着空洞的索引）；
 *	区域文件被删空了就把文件也删掉（空文件等于没有，留着只是垃圾）。
 *	坏的/读不出来的区域文件一律不碰、记数、退出码非0（和merge/fixer一个规矩）。
 */

/*命令行的范围参数合理性上限：一次最多去碰多少个区域文件（防手滑多打几个0）*/
#define PRUNE_REGION_MAX (1<<22)
/*x/z方向的合理范围：世界边界±29999984，这里放到±3亿（和merge/fixer一样，防坐标溢出和手滑）*/
#define PRUNE_XZ_MAX 300000000

struct prune_Config{
	std::string path;/*存档目录（要改的那个）*/
	int32_t x0=0,x1=0,z0=0,z1=0;/*方块范围，含头尾临界值，内部取min/max*/
	int mode=3;/*0=只统计 1=只删没生成完的 2=只删空壳 3=都删*/
	std::vector<std::string> dims;/*要处理的维度（默认主世界）*/
};

static void usage(const char *me){
	printf("用法：%s <path> <x1> <z1> <x2> <z2> <mode> [dims]\n",me);
	printf("  path   存档目录（会被改写！先在副本上跑）\n");
	printf("  x1 z1 x2 z2  方块范围的两个角，谁大谁小都行（整区块处理）\n");
	printf("  mode   0=只统计不删；1=只删没生成完的(Status != minecraft:full)；\n");
	printf("         2=只删空壳的(一个方块都没有)；3=两种都删（一般用这个）\n");
	printf("  dims   可选，逗号隔开的维度名，默认 minecraft:overworld\n");
	printf("例：%s /path/to/copy 0 0 511 511 0           只统计主世界，一个字节都不改\n",me);
	printf("    %s /path/to/copy 0 0 511 511 3 minecraft:the_nether\n",me);
}
static void split_List(const char *s,std::vector<std::string> &out){/*逗号隔开的一串，空段跳过*/
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
/*删区块时顺手把 entities/poi 的对应格也删掉；区域文件空了就把文件删掉。
  mca读不出来（bad）的文件一个字都不动：调用方已经计过坏数据*/
static int save_Or_Del(mca_Region &mca,const std::string &dir,int32_t rx,int32_t rz,int64_t *file_del){
	char file[4096];
	mca_Path(file,sizeof(file),dir.c_str(),rx,rz);
	if(mca.bad){
		printf("prune: 区域文件 %s 读不出来，不写也不删（免得把没读到的区块抹掉）\n",file);
		return 0;
	}
	if(mca.is_Empty()){
		struct stat st;
		if(!stat(file,&st)&&S_ISREG(st.st_mode)){
			if(remove(file)&&errno!=ENOENT){
				printf("prune: 删不掉空区域文件 %s（%s）\n",file,strerror(errno));
				return 0;
			}
			(*file_del)++;
		}
		return 1;
	}
	if(!mca.write_File(file)){
		printf("prune: 存盘失败 %s\n",file);
		return 0;
	}
	return 1;
}
static int prune_Dim(const prune_Config &cfg,int dim,int32_t x0,int32_t x1,int32_t z0,int32_t z1){
	const char *world=level::dim_Name(dim);
	level lv(cfg.path.c_str());/*只为了拿三个目录名（新/老布局由level按DataVersion决定）*/
	if(!lv.dat_ok){
		printf("prune: [%s] %s 不像个存档（level.dat读不到DataVersion），不干活\n",world,cfg.path.c_str());
		return -1;
	}
	std::string rdir=cfg.path+"/"+lv.region_dir[dim];
	std::string edir=cfg.path+"/"+lv.entities_dir[dim];
	std::string pdir=cfg.path+"/"+lv.poi_dir[dim];
	int64_t scanned=0,part=0,empty=0,del_chunks=0,del_part=0,del_empty=0,del_ent=0,del_poi=0,file_del=0,bad=0,fail=0;
	int64_t bad_nbt0=nbt_bad_nbt,bad_chunk0=nbt_bad_chunk;
	int32_t rx0=x0>>9,rx1=x1>>9,rz0=z0>>9,rz1=z1>>9;
	printf("prune: [%s] x[%d,%d] z[%d,%d] 区域目录 %s\n",world,x0,x1,z0,z1,rdir.c_str());
	for(int32_t rx=rx0;rx<=rx1;rx++){
		for(int32_t rz=rz0;rz<=rz1;rz++){
			mca_Region m=mca_Region::read(rdir.c_str(),rx,rz);
			if(m.is_Empty()&&!m.bad){
				continue;/*没有这个区域文件（或者里面一格都没有）：什么都不用干*/
			}
			mca_Region ent=mca_Region::read(edir.c_str(),rx,rz);
			mca_Region poi=mca_Region::read(pdir.c_str(),rx,rz);
			if(m.bad||ent.bad||poi.bad){/*有一个读不出来：这个区域整个不碰（宁可不动）*/
				bad++;
				printf("prune: [%s] r.%d.%d 有读不出来的文件（region坏%d entities坏%d poi坏%d），这个区域一个字节没动\n",
					world,(int)rx,(int)rz,m.bad?1:0,ent.bad?1:0,poi.bad?1:0);
				continue;
			}
			int m_changed=0,e_changed=0,p_changed=0;
			int32_t cx_begin=std::max(x0>>4,rx*32),cx_end=std::min((x1>>4)+1,rx*32+32);
			int32_t cz_begin=std::max(z0>>4,rz*32),cz_end=std::min((z1>>4)+1,rz*32+32);
			for(int32_t cx=cx_begin;cx<cx_end;cx++){
				for(int32_t cz=cz_begin;cz<cz_end;cz++){
					uint32_t lx=(uint32_t)(cx&31),lz=(uint32_t)(cz&31);
					if(!m.has_Chunk(lx,lz)){
						continue;/*这一格本来就没有区块*/
					}
					scanned++;
					Chunk *c=m.read_Chunk(lx,lz);/*读不出来（坏数据）就当"认不出来"：不删它，外面已经计过数*/
					if(!c){
						continue;
					}
					int is_empty=chunk_No_Block(c);
					int st=chunk_Full_Status(c);
					int is_part=(st==0);/*status没有(-1)＝不知道，不当残块（宁可少删）*/
					c->free();
					if(is_empty){
						empty++;
					}
					if(is_part){
						part++;
					}
					int want=(cfg.mode==1&&is_part)||(cfg.mode==2&&is_empty)||(cfg.mode==3&&(is_part||is_empty));
					if(!want||cfg.mode==0){
						continue;
					}
					if(!m.erase_Chunk(lx,lz)){/*删区块：表头那一格清0，数据区留空洞（游戏看它是"没生成"）*/
						fail++;
						continue;
					}
					m_changed=1;
					del_chunks++;
					if(is_part){
						del_part++;
					}
					if(is_empty){
						del_empty++;
					}
					if(ent.has_Chunk(lx,lz)&&ent.erase_Chunk(lx,lz)){/*索引跟着删：不然会留下指着空洞的POI/实体*/
						e_changed=1;
						del_ent++;
					}
					if(poi.has_Chunk(lx,lz)&&poi.erase_Chunk(lx,lz)){
						p_changed=1;
						del_poi++;
					}
				}
			}
			if(m_changed&&!save_Or_Del(m,rdir,rx,rz,&file_del)){
				fail++;
			}
			if(e_changed&&!save_Or_Del(ent,edir,rx,rz,&file_del)){
				fail++;
			}
			if(p_changed&&!save_Or_Del(poi,pdir,rx,rz,&file_del)){
				fail++;
			}
		}
	}
	int64_t bad_nbt=nbt_bad_nbt-bad_nbt0,bad_chunk=nbt_bad_chunk-bad_chunk0;
	printf("prune: [%s] 看了 %lld 个区块：没生成完 %lld 个、空壳（一个方块都没有）%lld 个\n",
		world,(long long)scanned,(long long)part,(long long)empty);
	if(cfg.mode==0){
		printf("prune: [%s] mode=0：只统计，一个字节都没改\n",world);
	}else{
		printf("prune: [%s] 删了 %lld 个区块（其中没生成完的 %lld 个、一个方块都没有的 %lld 个，两样都占的会重复计），"
			"顺带删 entities 索引 %lld 个、poi 索引 %lld 个，删空文件 %lld 个\n",
			world,(long long)del_chunks,(long long)del_part,(long long)del_empty,
			(long long)del_ent,(long long)del_poi,(long long)file_del);
		printf("prune: [%s] 这些格子现在等于\"没生成\"：进游戏/跑一次chunky start 就会按种子重新生成\n",world);
	}
	if(bad||bad_nbt||bad_chunk||fail||lv.err_cnt){
		printf("prune: [%s] 有失败（坏区域文件 %lld 个、NBT坏 %lld、区块坏 %lld、删/存盘失败 %lld、读写出错 %lld），上面的输出里找原因；"
			"坏的那些位置一个字没动\n",world,(long long)bad,(long long)bad_nbt,(long long)bad_chunk,(long long)fail,(long long)lv.err_cnt);
		return -1;
	}
	return 0;
}
int main(int argc,char **argv){
	if(argc<7){/*path+4个范围+mode=6个参数，维度可选*/
		usage(argv[0]);
		return -1;
	}
	prune_Config cfg;
	cfg.path=argv[1];
	int32_t *nums[4]={&cfg.x0,&cfg.z0,&cfg.x1,&cfg.z1};
	static const char *nname[4]={"x1","z1","x2","z2"};
	for(int i=0;i<4;i++){
		if(!arg_Int(argv[2+i],nums[i])){/*非数字/超int32：直接报错，别静默按0跑（prune是就地删）*/
			printf("prune: %s 不是整数：%s\n",nname[i],argv[2+i]);
			return -1;
		}
	}
	int32_t md=0;
	if(!arg_Int(argv[6],&md)){
		printf("prune: mode 不是整数：%s\n",argv[6]);
		return -1;
	}
	cfg.mode=(int)md;
	if(argc>7){
		split_List(argv[7],cfg.dims);
	}
	if(cfg.dims.empty()){
		cfg.dims.push_back("minecraft:overworld");
	}
	if(cfg.mode<0||cfg.mode>3){
		printf("prune: mode 只能是0/1/2/3，给的是 %d\n",cfg.mode);
		usage(argv[0]);
		return -1;
	}
	int32_t x0=std::min(cfg.x0,cfg.x1),x1=std::max(cfg.x0,cfg.x1);
	int32_t z0=std::min(cfg.z0,cfg.z1),z1=std::max(cfg.z0,cfg.z1);
	if(x0<-PRUNE_XZ_MAX||x1>PRUNE_XZ_MAX||z0<-PRUNE_XZ_MAX||z1>PRUNE_XZ_MAX){
		printf("prune: x/z范围超出±%d（世界边界是±29999984，超出就是坐标写错了）\n",PRUNE_XZ_MAX);
		return -1;
	}
	for(size_t i=0;i<cfg.dims.size();i++){
		const char *w=cfg.dims[i].c_str();
		if(level::dim_Of(w)==0&&strcmp(w,"minecraft:overworld")&&strcmp(w,"overworld")){
			printf("prune: 维度名 %s 认不出来（只认 overworld/the_nether/the_end 这三种），改用主世界\n",w);
			cfg.dims[i]="minecraft:overworld";
		}
	}
	int64_t regions=(int64_t)((x1>>9)-(x0>>9)+1)*((z1>>9)-(z0>>9)+1);
	if(regions>(int64_t)PRUNE_REGION_MAX){
		printf("prune: 这次要碰 %lld 个区域文件（上限 %d），范围写小点\n",(long long)regions,PRUNE_REGION_MAX);
		return -1;
	}
	struct stat st;
	if(stat(cfg.path.c_str(),&st)||!S_ISDIR(st.st_mode)){
		printf("prune: %s 不是个目录\n",cfg.path.c_str());
		return -1;
	}
	if(cfg.mode==0){
		printf("prune: mode=0（只统计）：%s 一个字节都不会改\n",cfg.path.c_str());
	}else{
		printf("prune: 要改 %s：先把副本跑一遍（mode=0）看看要删多少，再在副本上真删\n",cfg.path.c_str());
	}
	int ret=0;
	for(size_t i=0;i<cfg.dims.size();i++){
		int r=prune_Dim(cfg,level::dim_Of(cfg.dims[i].c_str()),x0,x1,z0,z1);
		if(r<0){
			ret=-1;/*某一个维度有失败：记住，别让后面的维度把退出码盖回去（sticky，和merge/fixer一样）*/
		}
	}
	return ret;
}
