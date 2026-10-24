#include "nbtrw.hpp"
#include <sys/stat.h>
#include <sys/types.h>
/*level.cpp：一个存档（Level=世界：主世界/下界/末地等所有维度）
 *	Level 不绑定维度；每个读写操作都带 dimension 参数：
 *	  · overworld / the_nether / the_end（可带 minecraft:）走原版维度名，
 *	    按 DataVersion 严格选布局，不按目录存在与否猜：
 *	      新（DataVersion >= 4774）：dimensions/minecraft/overworld|the_nether|the_end/
 *	      老（DataVersion <  4774）：region/、DIM-1/、DIM1/
 *	  · 其它字符串当“相对存档根的维度目录”，后面自动拼 region/、entities/、poi/；
 *	    不许绝对路径、不许 ..、不许空路径段。
 *	dat/mca/chunk三级缓存都在这里自动管理：
 *	  - 构造时给缓存大小（chunk/区域/dat各一个），满了按LRU落盘再扔；不设上限；
 *	  - 脏chunk→MCA（内存镜像），脏MCA→文件，脏dat→文件；
 *	  - save()把三级都刷一遍，close()刷完把缓存全放掉（之后还能接着用）。
 *	返回的指针都指在缓存里：下一次取别的对象、save()、close()后可能失效，要留着就copy()。
 *	level.dat 只读 Data.DataVersion 和 Version.Name，别的字段一个都不碰；要别的字段自己 get_Dat("level.dat")。
 */

/*---- 目录小工具 ----*/
static bool is_Dir(const std::string &p){/*目录在不在*/
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static bool make_Dir(const std::string &p){/*mkdir -p：写文件前把目录补出来*/
	if(p.empty()||is_Dir(p)){
		return true;
	}
	size_t i=p.find('/',1);/*从第2个字符开始找：开头的'/'和“相对路径没有/”都要照顾到*/
	for(;;){
		if(i==std::string::npos){
			i=p.size();
		}
		std::string sub=p.substr(0,i);
		if(!sub.empty()&&!is_Dir(sub)&&mkdir(sub.c_str(),0777)&&!is_Dir(sub)){
			return false;
		}
		if(i==p.size()){
			return true;
		}
		i=p.find('/',i+1);
	}
}
static std::string get_PathParent(const std::string &p){/*最后一个'/'前面那段；没有就是空*/
	size_t i=p.rfind('/');
	return i==std::string::npos?std::string():p.substr(0,i);
}
static bool is_RelPath(const char *rel){/*相对路径：不许绝对、不许..、不许空段、不许结尾/ */
	if(!rel||!rel[0]||rel[0]=='/'){
		return false;
	}
	const char *p=rel;
	while(*p){
		const char *e=strchr(p,'/');
		size_t n=e?(size_t)(e-p):strlen(p);
		if(n==0||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')){
			return false;
		}
		if(!e){
			break;
		}
		if(!e[1]){/*结尾的'/'也是空段*/
			return false;
		}
		p=e+1;
	}
	return true;
}

/*---- 维度目录 ----*/
static const char *dim_Base_New[3]={"dimensions/minecraft/overworld","dimensions/minecraft/the_nether","dimensions/minecraft/the_end"};
static const char *dim_Base_Old[3]={"","DIM-1","DIM1"};
static std::string get_DimSub(const std::string &base,const char *kind){/*空base=老布局主世界，直接拼region/...*/
	return base.empty()?std::string(kind):base+"/"+kind;
}
bool Level::is_DimensionName(const char *dimension){/*只认三个原版维度名，可带 minecraft: 前缀*/
	if(!dimension){
		return false;
	}
	std::string_view v(dimension);
	if(v.starts_with("minecraft:")){
		v.remove_prefix(sizeof("minecraft:")-1);
	}
	return v=="overworld"||v=="the_nether"||v=="the_end";
}
std::optional<std::string> Level::get_DimensionBase(const char *dimension) const{
	if(!dimension||!*dimension){
		return std::nullopt;
	}
	std::string_view v(dimension);
	std::string_view name=v;
	if(name.starts_with("minecraft:")){
		name.remove_prefix(sizeof("minecraft:")-1);
	}
	int idx=-1;
	if(name=="overworld"){
		idx=0;
	}else if(name=="the_nether"){
		idx=1;
	}else if(name=="the_end"){
		idx=2;
	}
	if(idx>=0&&(v.size()==name.size()||v.starts_with("minecraft:"))){
		return std::string(new_layout?dim_Base_New[idx]:dim_Base_Old[idx]);
	}
	if(!is_RelPath(dimension)){/*不是原版维度名，只能当相对路径*/
		return std::nullopt;
	}
	return std::string(dimension);
}
std::string Level::get_DimensionPath(const char *dimension,const char *kind) const{
	if(!kind){
		return std::string();
	}
	std::optional<std::string> base=get_DimensionBase(dimension);
	return base?get_DimSub(*base,kind):std::string();
}

/*---- 坐标换算：负数也要向下取整（右移在负数上是实现定义行为，这里用除法） ----*/
static int32_t floor_Div16(int32_t v){/*负数向下取整；用64位算，INT32_MIN-15不在int32里*/
	return (int32_t)(((int64_t)v>=0)?((int64_t)v/16):(((int64_t)v-15)/16));
}
static int32_t floor_Div32(int32_t v){
	return (int32_t)(((int64_t)v>=0)?((int64_t)v/32):(((int64_t)v-31)/32));
}
static uint64_t get_CoordKey(int32_t x,int32_t z){
	return ((uint64_t)(uint32_t)x<<32)|(uint32_t)z;
}
static void split_Coord(int32_t x,int32_t z,int32_t *cx,int32_t *cz,int32_t *lx,int32_t *lz){
	*cx=floor_Div16(x);
	*cz=floor_Div16(z);
	*lx=x-*cx*16;
	*lz=z-*cz*16;
}

Level::Level(const char *p,size_t cc,size_t rc,size_t dc):
	cache_cnt(cc?cc:64),region_cnt(rc?rc:8),dat_cnt(dc?dc:16){
	path=p?p:"";
	while(path.size()>1&&path[path.size()-1]=='/'){/*结尾的斜杠去掉，拼路径好看点*/
		path.resize(path.size()-1);
	}
	/*level.dat：gzip压缩的NBT，根下Data.DataVersion是Int*/
	nbt_Compound *root=nbt_Compound::read_Dat((path+"/level.dat").c_str());
	if(root){
		const nbt_Any *data=root->get("Data");
		const nbt_Compound *dc=data&&data->type==NBT_COMPOUND?(const nbt_Compound*)data:NULL;
		const nbt_Any *dv=dc?dc->get("DataVersion"):NULL;
		if(dv&&dv->type==NBT_INT){
			data_version=dv->as_Int();
			dat_ok=true;
		}
		const nbt_Any *ver=dc?dc->get("Version"):NULL;
		const nbt_Compound *vc=ver&&ver->type==NBT_COMPOUND?(const nbt_Compound*)ver:NULL;
		const nbt_Any *vn=vc?vc->get("Name"):NULL;
		if(vn&&vn->as_Str()){
			version_name=vn->as_Str();
		}
		root->free();
	}
	new_layout=dat_ok&&data_version>=DAT_NEW_LAYOUT;
}

/*---- 区域镜像：自动缓存，满了先把最久没用的写盘再扔 ----*/
Level::MCA_Entry *Level::get_MCAEntry(const char *dimension,int32_t rx,int32_t rz,int create){
	std::optional<std::string> base=get_DimensionBase(dimension);
	if(!base){
		miss_why=GET_ARG;
		return NULL;
	}
	return get_MCAEntryBase(*base,rx,rz,create);
}
Level::MCA_Entry *Level::get_MCAEntryBase(const std::string &base,int32_t rx,int32_t rz,int create){
	uint64_t key=get_CoordKey(rx,rz);
	auto oit=mcas.find(base);
	if(oit!=mcas.end()){
		auto it=oit->second.find(key);
		if(it!=oit->second.end()){
			it->second.use=++use_cnt;
			return &it->second;
		}
	}
	std::string dir=path+"/"+get_DimSub(base,"region");
	std::string file=get_MCAPath(dir.c_str(),rx,rz);
	MCA mca(file);/*先读：读不到（没有这个文件）就不该为它腾缓存*/
	if(mca.data.empty()&&!mca.bad){/*没有这个文件*/
		if(create<1){
			miss_why=GET_NO_FILE;
			return NULL;
		}
		mca=MCA::empty();/*要往里写：建一个空区域*/
	}
	bool bad=mca.bad;/*文件在、读不出来：也缓存起来（bad=true），别每次取区块都重读一遍坏文件*/
	size_t total=0;
	for(auto &o:mcas){
		total+=o.second.size();
	}
	if(total>=region_cnt){/*腾位置：最久没用过的先写盘*/
		MCA_Entry *victim=NULL;
		for(auto &o:mcas){
			for(auto &kv:o.second){
				if(!victim||kv.second.use<victim->use){
					victim=&kv.second;
				}
			}
		}
		if(victim){
			std::string vb=victim->dimension;
			int32_t vrx=victim->rx,vrz=victim->rz;
			if(!flush_MCA(*victim)){/*写不进去：先留着不扔（改了的东西还在内存里）*/
				miss_why=GET_CACHE;
				return NULL;
			}
			auto o=mcas.find(vb);
			if(o!=mcas.end()){
				o->second.erase(get_CoordKey(vrx,vrz));
				if(o->second.empty()){
					mcas.erase(o);
				}
			}
		}
	}
	MCA_Entry e;
	e.dimension=base;
	e.rx=rx;
	e.rz=rz;
	e.mca=std::move(mca);
	e.changed=false;
	e.use=++use_cnt;
	auto &inner=mcas[base];
	auto ins=inner.emplace(key,std::move(e));
	if(bad){
		miss_why=GET_BAD_FILE;
	}
	return &ins.first->second;
}
bool Level::flush_MCA(MCA_Entry &e){/*区域镜像落盘（先写.tmp再rename，见mca.cpp）*/
	if(!e.changed){
		return true;
	}
	std::string dir=path+"/"+get_DimSub(e.dimension,"region");
	if(!make_Dir(dir)){/*目录不在就建出来*/
		return false;
	}
	std::string file=get_MCAPath(dir.c_str(),e.rx,e.rz);
	if(!e.mca.save(file.c_str())){
		return false;
	}
	e.changed=false;
	return true;
}

/*---- 区块：自动缓存，满了先写回区域镜像 ----*/
Level::Chunk_Entry *Level::get_ChunkEntry(const char *dimension,int32_t cx,int32_t cz,int create){
	std::optional<std::string> base=get_DimensionBase(dimension);
	if(!base){
		miss_why=GET_ARG;
		return NULL;
	}
	return get_ChunkEntryBase(*base,cx,cz,create);
}
Level::Chunk_Entry *Level::get_ChunkEntryBase(const std::string &base,int32_t cx,int32_t cz,int create){
	uint64_t key=get_CoordKey(cx,cz);
	auto oit=chunks.find(base);
	if(oit!=chunks.end()){
		auto it=oit->second.find(key);
		if(it!=oit->second.end()){
			it->second.use=++use_cnt;
			return &it->second;
		}
	}
	int32_t rx=floor_Div32(cx),rz=floor_Div32(cz);
	MCA_Entry *m=get_MCAEntryBase(base,rx,rz,create>=2?1:0);/*create=2允许为新chunk建空区域*/
	if(!m){/*get_MCAEntryBase已经把miss_why填好（没有文件/坏文件/缓存满）*/
		return NULL;
	}
	if(m->mca.bad){/*区域文件读不出来：这个区块算坏数据，别去解析半截镜像*/
		miss_why=GET_BAD_FILE;
		return NULL;
	}
	int32_t lx=cx-rx*32,lz=cz-rz*32;
	bool has=m->mca.has_Chunk((uint32_t)lx,(uint32_t)lz);/*先记下来：下面腾缓存时 m 可能被挤掉*/
	Chunk *c=m->mca.get_Chunk((uint32_t)lx,(uint32_t)lz);
	if(!c&&!create){/*只查不缓存：读到就返回，读不到就不占缓存*/
		miss_why=has?GET_BAD_CHUNK:GET_NO_CHUNK;
		return NULL;
	}
	size_t total=0;
	for(auto &o:chunks){
		total+=o.second.size();
	}
	if(total>=cache_cnt){/*腾位置：最久没用过的先写回区域镜像（钉住的一个都不动）*/
		Chunk_Entry *victim=NULL;
		for(auto &o:chunks){
			for(auto &kv:o.second){
				if(kv.second.pin>0){
					continue;
				}
				if(!victim||kv.second.use<victim->use){
					victim=&kv.second;
				}
			}
		}
		if(!victim){/*全被调用者钉住了：宁可失败，也不把别人手里的指针弄悬空*/
			miss_why=GET_CACHE;
			if(c){
				c->free();
			}
			return NULL;
		}
		if(victim){
			std::string vb=victim->dimension;
			int32_t vcx=victim->cx,vcz=victim->cz;
			if(!flush_Chunk(*victim)){/*写不回去：先留着不扔*/
				miss_why=GET_CACHE;
				if(c){
					c->free();/*这个还没进缓存，读出来的得自己放掉*/
				}
				return NULL;
			}
			auto o=chunks.find(vb);
			if(o!=chunks.end()){
				auto it=o->second.find(get_CoordKey(vcx,vcz));
				if(it!=o->second.end()){
					if(it->second.chunk){/*写回去了，缓存槽要扔：区块树自己释放*/
						it->second.chunk->free();
						it->second.chunk=NULL;
					}
					o->second.erase(it);
					if(o->second.empty()){
						chunks.erase(o);
					}
				}
			}
		}
	}
	Chunk_Entry e;
	e.dimension=base;
	e.cx=cx;
	e.cz=cz;
	e.chunk=c;
	e.changed=false;
	e.use=++use_cnt;
	if(!c){/*读不出来：把原因记在条目里，下次不用再翻盘*/
		e.why=has?GET_BAD_CHUNK:GET_NO_CHUNK;
		miss_why=e.why;
	}
	auto &inner=chunks[base];
	auto ins=inner.emplace(key,std::move(e));
	return &ins.first->second;
}
bool Level::flush_Chunk(Chunk_Entry &e){/*脏区块→区域镜像（还没落盘）*/
	if(!e.changed||!e.chunk){
		return true;
	}
	int32_t rx=floor_Div32(e.cx),rz=floor_Div32(e.cz);
	MCA_Entry *m=get_MCAEntryBase(e.dimension,rx,rz,1);
	if(!m||m->mca.bad){
		return false;
	}
	int32_t lx=e.cx-rx*32,lz=e.cz-rz*32;
	if(!m->mca.set_Chunk(*e.chunk,(uint32_t)lx,(uint32_t)lz)){
		return false;
	}
	m->changed=true;
	e.changed=false;
	return true;
}

/*---- dat：自动缓存，满了先落盘 ----*/
Level::Dat_Entry *Level::get_DatEntry(const char *rel,int create){
	if(!is_RelPath(rel)){/*相对路径：不许绝对/..，防止跑出存档目录*/
		miss_why=GET_ARG;
		return NULL;
	}
	auto it=dats.find(rel);
	if(it!=dats.end()){
		it->second.use=++use_cnt;
		return &it->second;
	}
	std::string full=path+"/"+rel;
	nbt_Compound *dat=nbt_Compound::read_Dat(full.c_str());
	if(!dat){
		/*读不出来：文件在但坏了（绝不覆盖）→报错；文件不在且是set_Dat才建空条目*/
		struct stat st;
		if(!stat(full.c_str(),&st)){
			miss_why=GET_BAD_FILE;
			return NULL;
		}
		if(create<2){
			miss_why=GET_NO_FILE;
			return NULL;
		}
	}
	if(dats.size()>=dat_cnt){/*腾位置：最久没用过的先写盘*/
		auto victim=dats.end();
		for(auto i=dats.begin();i!=dats.end();++i){
			if(victim==dats.end()||i->second.use<victim->second.use){
				victim=i;
			}
		}
		if(victim!=dats.end()){
			if(!flush_Dat(victim->second)){
				miss_why=GET_CACHE;
				if(dat){
					dat->free();/*这个还没进缓存，读出来的得自己放掉*/
				}
				return NULL;
			}
			if(victim->second.dat){/*写回去了，缓存槽要扔：dat树自己释放*/
				victim->second.dat->free();
				victim->second.dat=NULL;
			}
			dats.erase(victim);
		}
	}
	int comp=dat?get_DatCompression(full.c_str()):-1;
	if(comp<0){
		comp=1;/*新文件默认gzip（level.dat就是gzip）*/
	}
	Dat_Entry e;
	e.rel=rel;
	e.dat=dat;
	e.changed=false;
	e.compression=comp;
	e.use=++use_cnt;
	auto ins=dats.emplace(rel,std::move(e));
	return &ins.first->second;
}
bool Level::flush_Dat(Dat_Entry &e){/*脏dat→文件（按读进来时的压缩方式写回去）*/
	if(!e.changed){
		return true;
	}
	if(!e.dat){
		e.changed=false;
		return true;
	}
	std::string full=path+"/"+e.rel;
	std::string parent=get_PathParent(full);
	if(!parent.empty()&&!make_Dir(parent)){
		return false;
	}
	if(!e.dat->save_Dat(full.c_str(),e.compression)){
		return false;
	}
	e.changed=false;
	return true;
}

/*---- 对外接口：都是转发 ----*/
MCA *Level::get_MCA(const char *dimension,int32_t rx,int32_t rz){
	miss_why=GET_OK;
	MCA_Entry *e=get_MCAEntry(dimension,rx,rz,0);
	if(!e){
		if(miss_why==GET_OK){
			miss_why=GET_NO_FILE;
		}
		return NULL;
	}
	if(e->mca.bad){
		miss_why=GET_BAD_FILE;
	}
	return &e->mca;
}
bool Level::set_MCA(const char *dimension,int32_t rx,int32_t rz,const MCA &mca){
	if(mca.bad){/*坏的区域镜像不能往缓存里放（放进去一save就把原文件抹了）*/
		return false;
	}
	MCA_Entry *e=get_MCAEntry(dimension,rx,rz,1);
	if(!e){
		return false;
	}
	e->mca=mca;
	e->changed=true;
	return true;
}
bool Level::format_MCA(const char *dimension,int32_t rx,int32_t rz){
	MCA_Entry *e=get_MCAEntry(dimension,rx,rz,0);
	if(!e||e->mca.bad){/*坏文件不整理*/
		return false;
	}
	/*先把属于这个维度、这个区域、还在chunk缓存里的脏区块写进镜像，否则整理出来的文件不含这些改动*/
	for(auto &o:chunks){
		for(auto &kv:o.second){
			Chunk_Entry &ce=kv.second;
			if(ce.changed&&ce.dimension==e->dimension&&
				floor_Div32(ce.cx)==rx&&floor_Div32(ce.cz)==rz){
				if(!flush_Chunk(ce)){
					return false;
				}
			}
		}
	}
	if(!e->mca.format()){/*坏表头/坏数据：一个字节不动*/
		return false;
	}
	e->changed=true;
	return true;
}
Chunk *Level::get_Chunk(const char *dimension,int32_t cx,int32_t cz){
	miss_why=GET_OK;
	Chunk_Entry *e=get_ChunkEntry(dimension,cx,cz,1);
	if(!e){
		if(miss_why==GET_OK){
			miss_why=GET_NO_FILE;
		}
		return NULL;
	}
	if(!e->chunk){
		miss_why=e->why?e->why:GET_NO_CHUNK;
		return NULL;
	}
	return e->chunk;
}
Chunk *Level::pin_Chunk(const char *dimension,int32_t cx,int32_t cz){
	miss_why=GET_OK;
	Chunk_Entry *e=get_ChunkEntry(dimension,cx,cz,1);
	if(!e){
		if(miss_why==GET_OK){
			miss_why=GET_NO_FILE;
		}
		return NULL;
	}
	if(!e->chunk){
		miss_why=e->why?e->why:GET_NO_CHUNK;
		return NULL;
	}
	e->pin++;
	return e->chunk;
}
bool Level::unpin_Chunk(const char *dimension,int32_t cx,int32_t cz){
	std::optional<std::string> base=get_DimensionBase(dimension);
	if(!base){
		miss_why=GET_ARG;
		return false;
	}
	auto o=chunks.find(*base);
	if(o==chunks.end()){
		return false;
	}
	auto it=o->second.find(get_CoordKey(cx,cz));
	if(it==o->second.end()||it->second.pin<=0){
		return false;
	}
	it->second.pin--;
	return true;
}
bool Level::erase_Chunk(const char *dimension,int32_t cx,int32_t cz){
	std::optional<std::string> base=get_DimensionBase(dimension);
	if(!base){
		miss_why=GET_ARG;
		return false;
	}
	int32_t rx=floor_Div32(cx),rz=floor_Div32(cz);
	int32_t lx=cx-rx*32,lz=cz-rz*32;
	/*先把缓存里的那份扔掉（它是要删的区块，不能再落盘）*/
	auto o=chunks.find(*base);
	if(o!=chunks.end()){
		auto it=o->second.find(get_CoordKey(cx,cz));
		if(it!=o->second.end()){
			if(it->second.pin>0){/*有人还拿着指针：不能在这里释放*/
				miss_why=GET_CACHE;
				return false;
			}
			if(it->second.chunk){
				it->second.chunk->free();
				it->second.chunk=NULL;
			}
			o->second.erase(it);
			if(o->second.empty()){
				chunks.erase(o);
			}
		}
	}
	MCA_Entry *m=get_MCAEntryBase(*base,rx,rz,0);
	if(!m){/*没有这个区域文件＝本来就没有区块可删*/
		return miss_why==GET_NO_FILE;
	}
	if(m->mca.bad){
		miss_why=GET_BAD_FILE;
		return false;
	}
	if(!m->mca.has_Chunk((uint32_t)lx,(uint32_t)lz)){
		return true;
	}
	if(!m->mca.erase_Chunk((uint32_t)lx,(uint32_t)lz)){
		return false;
	}
	m->changed=true;
	return true;
}
bool Level::set_Chunk(const char *dimension,int32_t cx,int32_t cz,const Chunk &chunk){
	Chunk_Entry *e=get_ChunkEntry(dimension,cx,cz,2);/*允许为新chunk建空区域*/
	if(!e){
		return false;
	}
	if(e->chunk&&e->chunk->serial==chunk.serial){
		/*同一个区块（serial相同，不比较地址）：调用者拿到的就是缓存里这个，已经改在树上了*/
		e->why=GET_OK;
		e->changed=true;
		e->cur.reset();
		return true;
	}
	Chunk *copy=new Chunk(chunk);/*先拷再删旧的：拷贝抛异常也不会把缓存里的弄丢*/
	if(e->chunk){
		e->chunk->free();
	}
	e->chunk=copy;
	e->why=GET_OK;
	e->changed=true;
	e->cur.reset();
	return true;
}
bool Level::set_Chunk(const char *dimension,const Chunk &chunk){
	const nbt_Any *x=chunk.get("xPos");
	const nbt_Any *z=chunk.get("zPos");
	if(!x||!z||x->type!=NBT_INT||z->type!=NBT_INT){
		miss_why=GET_ARG;
		return false;
	}
	return set_Chunk(dimension,x->as_Int(),z->as_Int(),chunk);
}
Block Level::get_Block(const char *dimension,int x,int y,int z){
	int32_t cx=0,cz=0,lx=0,lz=0;
	split_Coord(x,z,&cx,&cz,&lx,&lz);
	miss_why=GET_OK;
	Chunk_Entry *e=get_ChunkEntry(dimension,cx,cz,1);
	if(!e){
		if(miss_why==GET_OK){
			miss_why=GET_NO_FILE;
		}
		return Block();
	}
	if(!e->chunk){
		miss_why=e->why?e->why:GET_NO_CHUNK;
		return Block();
	}
	return e->chunk->get_Block(lx,y,lz,&e->cur);
}
bool Level::set_Block(const char *dimension,int x,int y,int z,const Block &block){
	int32_t cx=0,cz=0,lx=0,lz=0;
	split_Coord(x,z,&cx,&cz,&lx,&lz);
	miss_why=GET_OK;
	Chunk_Entry *e=get_ChunkEntry(dimension,cx,cz,1);/*写方块不建区域/不建区块：区块本来就没有就没地方写*/
	if(!e||!e->chunk){
		return false;
	}
	if(!e->chunk->is_Full()){
		return false;/*没生成的区块不许直接set_Block*/
	}
	if(!e->chunk->set_Block(block,lx,y,lz,&e->cur)){
		return false;
	}
	e->changed=true;
	return true;
}
nbt_Compound *Level::get_Dat(const char *rel){
	Dat_Entry *e=get_DatEntry(rel,1);
	return e?e->dat:NULL;
}
bool Level::set_Dat(const char *rel,const nbt_Compound &dat){
	Dat_Entry *e=get_DatEntry(rel,2);/*没有这个文件就建新条目；文件在但读不出来就失败*/
	if(!e){
		return false;
	}
	nbt_Compound *copy=new nbt_Compound(dat);
	if(e->dat){
		e->dat->free();
	}
	e->dat=copy;
	e->changed=true;
	return true;
}

bool Level::save(){/*脏chunk→区域镜像，脏镜像→文件，脏dat→文件*/
	bool ok=true;
	for(auto &o:chunks){
		for(auto &kv:o.second){
			if(!flush_Chunk(kv.second)){
				ok=false;
			}
		}
	}
	for(auto &o:mcas){
		for(auto &kv:o.second){
			if(!flush_MCA(kv.second)){
				ok=false;
			}
		}
	}
	for(auto &kv:dats){
		if(!flush_Dat(kv.second)){
			ok=false;
		}
	}
	return ok;
}
bool Level::close(){/*落盘+把缓存的区块/区域/dat全放掉，可以重复调*/
	bool ok=save();
	for(auto &o:chunks){
		for(auto &kv:o.second){
			if(kv.second.chunk){
				kv.second.chunk->free();
				kv.second.chunk=NULL;
			}
		}
	}
	chunks.clear();
	mcas.clear();/*MCA里的vector自己释放*/
	for(auto &kv:dats){
		if(kv.second.dat){
			kv.second.dat->free();
			kv.second.dat=NULL;
		}
	}
	dats.clear();
	return ok;
}
Level::~Level(){
	close();/*没close过就这里补上，不丢改动*/
}
