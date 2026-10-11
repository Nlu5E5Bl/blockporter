#include "nbt.hpp"
#include <sys/stat.h>
#include <sys/types.h>
/*level.cpp：把一个存档目录当"世界"用（world_api.py 的 C++ 版）
 *	level 就干三件事：
 *	  1. 按 DataVersion 定维度目录，区域文件按需读：
 *	     存档目录结构在 **26.1 快照6（DataVersion 4774）** 改过一次（wiki: Java Edition level format
 *	     的 History 表："All default dimensions are now stored in the dimensions subfolder"）：
 *	       新（DataVersion >= 4774）：dimensions/minecraft/overworld|the_nether|the_end/region/
 *	       老（DataVersion <  4774）：region/、DIM-1/region/、DIM1/region/
 *	     这里**严格按 DataVersion 判**，不按"哪个目录存在"猜（手工搬过目录、或者 level.dat 和实际
 *	     布局不一致的情况，直接按 level.dat 说的来，目录不对就读不到区块，会在统计里暴露出来）。
 *	  2. 区块按槽缓存，和 python 一样是"直接映射"：cache_id 那个槽只放最后一次取的那个区块，
 *	     换区块就把旧的写回区域镜像再 free 掉（不是 LRU；cache_id 就是让调用者自己挑槽）。
 *	  3. 改动分两级落盘：区块→区域镜像（flush_Chunk），镜像→文件（flush_Region）。
 *	save() 把两级都刷一遍，close() 还会把缓存的区块和镜像全放掉。
 *	区域镜像超过 region_cnt 个时，把最久没用过的那个写盘扔掉（一个镜像 8MB，省内存）。
 *
 *	level **不把 level.dat 映射成结构体**：只读 Data.DataVersion（还有 Version.Name 用来打印），
 *	别的字段一个都不碰。要别的字段就自己 nbt_Compound::read_Dat() 去读。
 *
 *	注意：从缓存里拿到的 Block/Chunk 指针，下次取别的区块就可能失效（cache_cnt=1 时必然失效）。
 */

/*---- 目录小工具 ----*/
/*区块缓存槽数的上限：只用来挡"参数写错一位"（几百万个槽就是几百万个空Cache，纯浪费内存）*/
#define CACHE_MAX (1<<16)
static int dir_Exists(const std::string &p){/*目录在不在*/
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static int dir_Make(const std::string &p){/*mkdir -p：写区域文件前把目录补出来（path 少一层就不行）*/
	if(p.empty()||dir_Exists(p)){
		return 1;
	}
	size_t i=p.find('/',1);/*从第2个字符开始找：开头的'/'和"相对路径没有/"都要照顾到*/
	for(;;){
		if(i==std::string::npos){
			i=p.size();
		}
		std::string sub=p.substr(0,i);
		if(!sub.empty()&&!dir_Exists(sub)&&mkdir(sub.c_str(),0777)&&!dir_Exists(sub)){
			return 0;
		}
		if(i==p.size()){
			return 1;
		}
		i=p.find('/',i+1);
	}
}

/*---- 维度目录 ----
  一个维度有三个同级目录，名字只差最后一段：region（方块）/entities（非方块实体）/poi（方块索引）。
  新布局（DataVersion>=4774）：dimensions/minecraft/<维度>/<kind>；
  老布局：主世界是裸的 <kind>/，下界 DIM-1/<kind>/，末地 DIM1/<kind>/——主世界没有维度前缀，
  所以表里存的是"维度根"，kind是拼上去的（空的根就直接用kind）*/
static const char *dim_Base_New[3]={"dimensions/minecraft/overworld","dimensions/minecraft/the_nether","dimensions/minecraft/the_end"};
static const char *dim_Base_Old[3]={"","DIM-1","DIM1"};
static std::string dim_Sub(const char *base,const char *kind){
	return base[0]?std::string(base)+"/"+kind:std::string(kind);
}

int level::dim_Of(const char *world){/*维度名→0/1/2；NULL/空/认不出来都算主世界*/
	if(!world||!world[0]){
		return 0;
	}
	if(strstr(world,"nether")){
		return 1;
	}
	if(strstr(world,"end")){
		return 2;
	}
	return 0;
}
const char *level::dim_Name(int dim){/*0/1/2 → 全名（打印和命令行都用这个）*/
	static const char *names[3]={"minecraft:overworld","minecraft:the_nether","minecraft:the_end"};
	return names[dim<0||dim>2?0:dim];
}

level::level(const char *p,int cc,int rc):cache_cnt(cc>0?cc:1),region_cnt(rc>0?rc:0){
	if(cache_cnt>CACHE_MAX){/*只防"参数写错一位"这种：真按几十亿个槽resize会当场bad_alloc*/
		printf("level: cache_cnt=%d太大，按%d算\n",cache_cnt,CACHE_MAX);
		cache_cnt=CACHE_MAX;
	}
	path=p?p:"";
	while(path.size()>1&&path[path.size()-1]=='/'){/*结尾的斜杠去掉，拼路径好看点*/
		path.resize(path.size()-1);
	}
	/*level.dat：gzip 压缩的 NBT（wiki: NBT format "level.dat is stored in compressed NBT format"），
	  根下 Data.DataVersion 是 Int。读不到就当没有——调用者看 dat_ok 决定要不要接着干*/
	nbt_Compound *root=nbt_Compound::read_Dat((path+"/level.dat").c_str());
	if(root){
		const nbt_Any *data=root->get("Data");
		const nbt_Compound *dc=data&&data->type==0x0a?(const nbt_Compound*)data:NULL;
		const nbt_Any *dv=dc?dc->get("DataVersion"):NULL;
		if(dv&&dv->type==0x03){/*Int*/
			data_version=(int32_t)dv->as_Int();
			dat_ok=1;
		}
		const nbt_Any *ver=dc?dc->get("Version"):NULL;
		const nbt_Compound *vc=ver&&ver->type==0x0a?(const nbt_Compound*)ver:NULL;
		const nbt_Any *vn=vc?vc->get("Name"):NULL;
		if(vn&&vn->as_Str()){
			version_name=vn->as_Str();
		}
		root->free();
	}
	int32_t use=dat_ok?data_version:0;/*读不到 level.dat 就当最老的布局（调用者应当先看 dat_ok 再干活）*/
	new_layout=use>=DAT_NEW_LAYOUT;
	for(int i=0;i<3;i++){
		const char *base=new_layout?dim_Base_New[i]:dim_Base_Old[i];
		region_dir[i]=dim_Sub(base,"region");
		entities_dir[i]=dim_Sub(base,"entities");
		poi_dir[i]=dim_Sub(base,"poi");
	}
	cache.resize(cache_cnt);
}
level::~level(){
	close();/*没 close 过就这里补上，不丢改动*/
}

int level::flush_Chunk(size_t i){/*把缓存里第 i 个脏区块编码进区域镜像（还没落盘）*/
	if(i>=cache.size()){
		return 1;
	}
	Cache &c=cache[i];
	if(!c.chunk||!c.changed){
		return 1;
	}
	Region *r=get_Region(c.dim,c.cx>>5,c.cz>>5,1);
	if(!r||!r->mca.write_ChunkAt(c.chunk,(uint32_t)(c.cx&31),(uint32_t)(c.cz&31))){
		err_cnt++;
		return 0;
	}
	r->changed=1;
	c.changed=0;
	return 1;
}
void level::drop_Cache(size_t i){/*写回并扔掉第 i 个槽里的区块*/
	if(i>=cache.size()){
		return;
	}
	flush_Chunk(i);
	if(cache[i].chunk){
		cache[i].chunk->free();
		cache[i].chunk=NULL;
	}
	cache[i].dim=-1;
	cache[i].changed=false;
	cache[i].cur.reset();/*区块没了，游标里的指针不能留*/
}
int level::flush_Region(size_t i){/*区域镜像落盘（先写 .tmp 再 rename，见 mca.cpp）*/
	if(i>=regions.size()){
		return 1;
	}
	Region &r=regions[i];
	if(!r.changed){
		return 1;
	}
	if(r.mca.bad){/*这个文件我们根本没读出来：写下去就是把"没读到的那部分"抹掉。拒绝写，记一笔出错*/
		printf("level: 区域文件 r.%d.%d（维度%d）读不出来，拒绝写它（免得把没读到的区块抹掉）\n",r.rx,r.rz,r.dim);
		r.changed=0;/*标记清掉：再试也是同一个结果，别每轮都刷屏*/
		err_cnt++;
		return 0;
	}
	r.changed=0;
	std::string dir=path+"/"+region_dir[r.dim];
	if(!dir_Make(dir)){/*目录不在就建出来（新世界还没生成过 nether/end 的时候会遇到）*/
		printf("level: 建不了目录 %s\n",dir.c_str());
		r.changed=1;
		err_cnt++;
		return 0;
	}
	if(!r.mca.write(dir.c_str(),(uint32_t)r.rx,(uint32_t)r.rz)){
		r.changed=1;
		err_cnt++;
		return 0;
	}
	return 1;
}
level::Region *level::get_Region(int dim,int32_t rx,int32_t rz,int create){
	if(dim<0||dim>2){
		dim=0;
	}
	for(size_t i=0;i<regions.size();i++){
		if(regions[i].dim==dim&&regions[i].rx==rx&&regions[i].rz==rz){
			regions[i].use=++use_cnt;
			return &regions[i];
		}
	}
	if(region_cnt>0&&(int)regions.size()>=region_cnt){/*腾位置：最久没用过的先写盘扔了*/
		size_t old=0;
		for(size_t i=1;i<regions.size();i++){
			if(regions[i].use<regions[old].use){
				old=i;
			}
		}
		if(flush_Region(old)){
			regions.erase(regions.begin()+old);
		}else{/*写不进去（磁盘满/只读/rename失败）：**这次不扔**——扔了那些还没落盘的改动就永远丢了。
		         代价是镜像数暂时超一点（err_cnt 已经记了一笔，调用方最后会非0退出），下次腾位置还会再试*/
			static int warned=0;
			if(!warned){
				printf("level: 区域镜像腾位置时写盘失败，先留着不扔（改动还在内存里；查磁盘/权限）\n");
				warned=1;
			}
		}
	}
	Region r;
	r.dim=dim;
	r.rx=rx;
	r.rz=rz;
	r.use=++use_cnt;
	r.mca=mca_Region::read((path+"/"+region_dir[dim]).c_str(),rx,rz);
	if(r.mca.bad){/*文件在、但读不出来（权限/IO/截断）：**绝不当成"这个区域没有区块"**——
	                 那会让后面把空镜像写回去，把没碰过的区块整片抹掉。留着 bad 标记，flush_Region会拒绝写*/
		regions.push_back(std::move(r));
		return &regions.back();
	}
	if(r.mca.data.empty()){/*没有这个文件*/
		if(!create){
			return NULL;
		}
		r.mca=mca_Region::empty();
	}
	regions.push_back(std::move(r));
	return &regions.back();
}
int level::save(){
	int ret=1;
	for(size_t i=0;i<cache.size();i++){
		ret&=flush_Chunk(i);/*脏区块进镜像*/
	}
	for(size_t i=0;i<regions.size();i++){
		ret&=flush_Region(i);/*脏镜像进文件*/
	}
	return ret;
}
void level::close(){
	save();/*先把改动写完*/
	for(size_t i=0;i<cache.size();i++){
		if(cache[i].chunk){
			cache[i].chunk->free();
			cache[i].chunk=NULL;
		}
		cache[i].dim=-1;
		cache[i].changed=false;
		cache[i].cur.reset();
	}
	cache.clear();/*再调 get_Chunk 会自己补回来*/
	regions.clear();/*mca_Region 里的 vector 自己释放*/
}

Chunk *level::get_Chunk(int32_t cx,int32_t cz,const char *world,int cache_id){
	int dim=dim_Of(world);
	miss_why=GET_OK;/*这一次调用的结论；下面每条 return NULL 的路都会写明原因*/
	if(cache_id<0){
		cache_id=0;
	}
	if(cache_id>CACHE_MAX){/*只防"参数写错一位"这种：真按几十亿个槽resize会当场bad_alloc*/
		printf("level: cache_id=%d太大（上限%d），当读不出来\n",cache_id,CACHE_MAX);
		miss_why=GET_ARG;
		return NULL;
	}
	if((size_t)cache_id>=cache.size()){/*cache_id 给大了就当 cache_cnt 跟着涨*/
		cache.resize((size_t)cache_id+1);
	}
	Cache &c=cache[cache_id];
	if(c.chunk&&c.cx==cx&&c.cz==cz&&c.dim==dim){
		return c.chunk;
	}
	drop_Cache((size_t)cache_id);/*换区块：脏的先写回区域镜像*/
	Region *r=get_Region(dim,cx>>5,cz>>5,0);/*>>5 就是除 32，负数也是向下取整*/
	if(!r){
		c.dim=-1;
		miss_why=GET_NO_FILE;/*区域文件不在（或者整个区域目录都不在）：这片没生成过，正常*/
		return NULL;
	}
	if(r->mca.bad){/*文件在、但读不出来（mca_Region::read 已经计过 nbt_bad_chunk）：坏数据*/
		c.dim=-1;
		miss_why=GET_BAD_FILE;
		return NULL;
	}
	c.cur.reset();/*换区块前把游标清了（新读出来的 Chunk* 有可能是刚 free 掉的那个地址）*/
	c.chunk=r->mca.read_Chunk((uint32_t)(cx&31),(uint32_t)(cz&31));
	if(!c.chunk){/*读不出来：要么表头那一格本来就是空的，要么数据坏了（坏的那次已经计进 nbt_bad_nbt/nbt_bad_chunk）*/
		c.dim=-1;
		miss_why=r->mca.has_Chunk((uint32_t)(cx&31),(uint32_t)(cz&31))?GET_BAD_CHUNK:GET_NO_CHUNK;
		return NULL;
	}
	c.cx=cx;
	c.cz=cz;
	c.dim=dim;
	c.changed=false;
	return c.chunk;
}
Block level::get_Block(int x,int y,int z,const char *world,int cache_id){
	if(cache_id<0){/*get_Chunk里会把负槽号夹成0并**成功**返回区块，这里要是不夹，
	                 &cache[cache_id] 就是拿堆外内存当游标用（还会往里写）*/
		cache_id=0;
	}
	if(cache_id>CACHE_MAX){
		return Block{NULL,NULL};
	}
	Chunk *c=get_Chunk(x>>4,z>>4,world,cache_id);
	if(!c){
		return Block{NULL,NULL};
	}
	return c->get_Block(x&15,y,z&15,&cache[cache_id].cur);/*游标记住 sy→section，连着读快很多*/
}
int level::set_Block(int x,int y,int z,const char *world,Block block,int cache_id){
	if(cache_id<0){
		cache_id=0;
	}
	if(cache_id>CACHE_MAX){
		return 0;
	}
	Chunk *c=get_Chunk(x>>4,z>>4,world,cache_id);
	if(!c){
		return 0;
	}
	if(!c->set_Block(block,x&15,y,z&15,&cache[cache_id].cur)){
		err_cnt++;
		return 0;
	}
	if((size_t)cache_id<cache.size()){/*get_Chunk 刚把这块填进这个槽*/
		cache[cache_id].changed=1;
	}
	return 1;
}
int level::set_Block(int x,int y,int z,const char *world,nbt_Any *block,nbt_Any *entity,int cache_id){
	Block b{block,entity};
	return set_Block(x,y,z,world,b,cache_id);
}
Chunk_Cursor *level::cursor(int cache_id){/*借槽里的游标用（merge 就是拿它配着 Chunk* 直接读方块）*/
	if(cache_id<0){
		cache_id=0;
	}
	if(cache_id>CACHE_MAX){/*同 get_Chunk：槽号大得离谱就当参数写错了，别去resize*/
		printf("level: cache_id=%d太大（上限%d）\n",cache_id,CACHE_MAX);
		cache_id=0;
	}
	if((size_t)cache_id>=cache.size()){
		cache.resize((size_t)cache_id+1);
	}
	return &cache[cache_id].cur;
}
int level::mark_Changed(int32_t cx,int32_t cz,const char *world,int cache_id,int keep_cur){/*直接改区块树（比如删 isLightOn）就得自己标脏*/
	int dim=dim_Of(world);
	if(cache_id<0||(size_t)cache_id>=cache.size()){
		return 0;
	}
	Cache &c=cache[cache_id];
	if(c.chunk&&c.cx==cx&&c.cz==cz&&c.dim==dim){
		c.changed=1;
		if(!keep_cur){/*外面自己动过树了，游标里的指针不保证还对；走 Chunk::set_Block 写的就不用清*/
			c.cur.reset();
		}
		return 1;
	}
	return 0;
}
