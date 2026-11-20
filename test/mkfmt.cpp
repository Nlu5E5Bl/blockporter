#include "../nbtrw.hpp"
#include <dirent.h>
#include <stdlib.h>
#include <string>
#include <vector>
/*mkfmt.cpp：同一批场景放进两种存档格式的世界，给 test/format.sh 用（只动 test/work/fmt/）
 *	mkfmt fix    造 old（test/data/lvlworld 原样：26.1.1，DataVersion 4788，Name/Properties、Status）
 *	             和 new（同一份数据转成 26.4 的写法：DataVersion 5119，id/properties/字符串调色板、status、
 *	             每格一个的 biomes），再用同一段代码在两边布置场景（库按各自区块的格式写）。
 *	mkfmt merge  拿修完的两个世界造 merge 用例：
 *	             同版本 m_now/m_new/m_old（new 少 Y=-3 一段：merge 不补段，跳过）；
 *	             跨版本 x_now(4788)→x_new(5119)、反方向 y_now(5119)→y_new(4788)（暴力合并：照搬、各按各的格式写）；
 *	             z_now(4788)→z_new(1631，1.13.2：Level 外壳、Palette/BlockStates、没有 POI、实体在区块的 Level.Entities)。
 *	格式依据：wiki Chunk_format §History（26.3 snap7 调色板、26.4 snap1 Status→status、biomes 每格一个）。*/
static const char *D="minecraft:overworld";
static const char *F="test/work/fmt";

/*---- 26.1.1 → 26.4 的写法 ----*/
static void conv_Palette(nbt_Compound *bs){
	nbt_Any *pa=bs->get("palette");
	if(!pa||pa->type!=NBT_LIST){
		return;
	}
	nbt_List *pal=(nbt_List*)pa;
	std::vector<nbt_Any*> v;
	bool all_str=true;
	for(size_t i=0;i<pal->con.size();i++){
		nbt_Any *n=make_State(make_StateBlock(pal->con[i],STATE_NAME),STATE_ID);
		if(!n){
			n=new nbt_String("minecraft:air");
		}
		all_str=all_str&&n->type==NBT_STRING;
		v.push_back(n);
	}
	nbt_List *np=new nbt_List(all_str?NBT_STRING:NBT_COMPOUND);
	for(nbt_Any *n:v){
		if(!all_str&&n->type==NBT_STRING){
			nbt_Compound *w=new nbt_Compound;
			w->set("",n);/*混进复合列表的默认状态：{"":ID}*/
			n=w;
		}
		np->push_back(n);
	}
	bs->set("palette",np);
}
static uint32_t get_BiomeBits(size_t n){/*biomes 的下标没有最小位宽：刚好放得下最大下标*/
	uint32_t b=0;
	while(((size_t)1<<b)<n){
		b++;
	}
	return b;
}
static void conv_Biomes(nbt_Compound *bio){/*4×4×4 一格（64个下标）→ 每格一个（4096个下标）*/
	nbt_Any *pa=bio->get("palette"),*da=bio->get("data");
	if(!pa||pa->type!=NBT_LIST||!da||da->type!=NBT_LONG_ARRAY){
		return;/*只有一种生物群系：没有 data，两种格式一样*/
	}
	uint32_t bits=get_BiomeBits(((nbt_List*)pa)->con.size());
	if(!bits){
		return;
	}
	uint32_t per=64/bits;
	const std::vector<int64_t> &w=((nbt_Long_Array*)da)->con;
	std::vector<uint32_t> cell(64,0);
	for(uint32_t i=0;i<64&&i/per<w.size();i++){
		cell[i]=(uint32_t)(((uint64_t)w[i/per]>>((i%per)*bits))&(((uint64_t)1<<bits)-1));
	}
	std::vector<int64_t> nw((4096+per-1)/per,0);
	for(uint32_t y=0;y<16;y++){
		for(uint32_t z=0;z<16;z++){
			for(uint32_t x=0;x<16;x++){
				uint32_t i=y*256+z*16+x;
				uint64_t v=cell[(y/4)*16+(z/4)*4+(x/4)];
				nw[i/per]|=(int64_t)(v<<((i%per)*bits));
			}
		}
	}
	bio->set("data",new nbt_Long_Array(std::move(nw)));
}
static void convert_Chunk(Chunk *c){/*一个 4788 的区块原地改写成 5119 的样子：DataVersion、status、调色板条目的写法、每格一个的 biomes*/
	c->con.set_Int("DataVersion",5119);
	nbt_Any *st=c->con.get("Status");
	if(st){
		c->con.set("status",st->copy());
		c->con.erase("Status");
	}
	nbt_Any *sa=c->con.get("sections");
	if(!sa||sa->type!=NBT_LIST){
		return;
	}
	for(nbt_Any *s:((nbt_List*)sa)->con){
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		nbt_Any *bs=((nbt_Compound*)s)->get("block_states");
		if(bs&&bs->type==NBT_COMPOUND){
			conv_Palette((nbt_Compound*)bs);
		}
		nbt_Any *bio=((nbt_Compound*)s)->get("biomes");
		if(bio&&bio->type==NBT_COMPOUND){
			conv_Biomes((nbt_Compound*)bio);
		}
	}
}
static bool convert_World(const std::string &w){/*整个世界转成 5119：level.dat 的版本号，加上主世界 region 里的每个区块（convert_Chunk）*/
	std::string dat=w+"/level.dat";
	nbt_Compound *root=nbt_Compound::read_Dat(dat.c_str());
	nbt_Any *data=root?root->get("Data"):NULL;
	if(!data||data->type!=NBT_COMPOUND){
		return false;
	}
	((nbt_Compound*)data)->set_Int("DataVersion",5119);
	nbt_Any *ver=((nbt_Compound*)data)->get("Version");
	if(ver&&ver->type==NBT_COMPOUND){
		((nbt_Compound*)ver)->set_Int("Id",5119);
		((nbt_Compound*)ver)->set_String("Name","26.4-snapshot-1");
	}
	bool ok=root->save_Dat(dat.c_str(),1);
	root->free();
	if(!ok){
		return false;
	}
	std::string rd=w+"/dimensions/minecraft/overworld/region";
	DIR *dp=opendir(rd.c_str());
	if(!dp){
		return false;
	}
	struct dirent *e;
	while((e=readdir(dp))){
		int rx,rz;
		if(sscanf(e->d_name,"r.%d.%d.mca",&rx,&rz)!=2){
			continue;
		}
		MCA m(get_MCAPath(rd.c_str(),rx,rz));
		if(m.bad){
			continue;
		}
		for(uint32_t i=0;i<32;i++){
			for(uint32_t j=0;j<32;j++){
				if(!m.has_Chunk(i,j)){
					continue;
				}
				std::optional<nbt_Compound> r=m.get_Compound(i,j);
				if(!r){
					continue;
				}
				Chunk c(std::move(*r));
				convert_Chunk(&c);
				ok=m.set_Compound(c.con,i,j)&&ok;
			}
		}
		ok=m.save()&&ok;
	}
	closedir(dp);
	return ok;
}

/*---- 两边一模一样的场景（坐标都在区块(0,0)，y=-31..-29 先清空） ----*/
static bool set_BlockAt(Level &lv,int x,int y,int z,const Block &b){
	bool ok=lv.set_Block(D,x,y,z,b);
	if(!ok){
		printf("set %d %d %d %s 失败\n",x,y,z,std::string(b.name()).c_str());
	}
	return ok;
}
static Block make_Block(const char *n,const char *props=NULL){/*props 写成 "k=v,k=v"*/
	Block b(n);
	std::string p=props?props:"";
	size_t pos=0;
	while(pos<p.size()){
		size_t q=p.find(',',pos);
		if(q==std::string::npos){
			q=p.size();
		}
		std::string kv=p.substr(pos,q-pos);
		size_t eq=kv.find('=');
		if(eq!=std::string::npos){
			b.properties.set_String(kv.substr(0,eq),kv.substr(eq+1));
		}
		pos=q+1;
	}
	return b;
}
static bool add_Tick(Level &lv,bool fluid,const char *type,int x,int y,int z){/*给区块 (0,0) 加一条计划刻（t=7 p=0）*/
	Chunk *c=lv.get_Chunk(D,0,0);
	return c&&add_ChunkTick(&c->con,fluid,type,x,y,z,7,0)&&lv.set_Chunk(D,0,0,*c);
}
static bool build_Scene(Level &lv){/*在区块 (0,0) 里摆同一批场景（悬空的、含水的、要 POI 的、计划刻对得上对不上的…），两种格式的世界各摆一遍*/
	for(int x=0;x<16;x++){
		for(int z=0;z<16;z++){
			for(int y=-31;y<=-29;y++){
				if(!set_BlockAt(lv,x,y,z,Block("minecraft:air"))){
					return false;
				}
			}
		}
	}
	bool ok=true;
	ok=ok&&set_BlockAt(lv,1,-30,1,make_Block("minecraft:torch"));                                    /*A 悬空火把→拆*/
	ok=ok&&set_BlockAt(lv,3,-30,3,make_Block("minecraft:lantern","hanging=false,waterlogged=true")); /*B 悬空含水灯笼→水源+水刻*/
	ok=ok&&set_BlockAt(lv,5,-30,5,make_Block("minecraft:frosted_ice","age=0"));                      /*C 霜冰→水源+水刻*/
	ok=ok&&set_BlockAt(lv,7,-31,7,make_Block("minecraft:stone"));                                     /*D 地衣：下面有石头、北面没东西→关掉北面*/
	ok=ok&&set_BlockAt(lv,7,-30,7,make_Block("minecraft:glow_lichen",
		"down=true,up=false,north=true,south=false,east=false,west=false,waterlogged=false"));
	ok=ok&&set_BlockAt(lv,9,-30,9,make_Block("minecraft:oak_slab","type=top,waterlogged=false"));   /*E 上半砖顶面整面：火把留着*/
	ok=ok&&set_BlockAt(lv,9,-29,9,make_Block("minecraft:torch"));
	ok=ok&&set_BlockAt(lv,11,-30,11,make_Block("minecraft:oak_slab","type=bottom,waterlogged=false"));/*F 下半砖顶面空：火把拆*/
	ok=ok&&set_BlockAt(lv,11,-29,11,make_Block("minecraft:torch"));
	ok=ok&&set_BlockAt(lv,13,-31,13,make_Block("minecraft:stone"));                                   /*G 流体刻*/
	ok=ok&&set_BlockAt(lv,13,-30,13,make_Block("minecraft:water","level=3"));
	ok=ok&&set_BlockAt(lv,13,-30,9,make_Block("minecraft:water","level=0"));
	ok=ok&&set_BlockAt(lv,1,-30,13,make_Block("minecraft:stone"));                                    /*H 方块刻*/
	ok=ok&&set_BlockAt(lv,5,-30,13,make_Block("minecraft:oak_log","axis=x"));                         /*I 带属性、不归规则管的方块原样*/
	ok=ok&&add_Tick(lv,true,"minecraft:flowing_water",13,-30,13);/*流动水里的 flowing_water 刻：对得上，留*/
	ok=ok&&add_Tick(lv,true,"minecraft:water",13,-30,11);        /*空气里的水刻：删*/
	ok=ok&&add_Tick(lv,true,"minecraft:lava",13,-30,9);          /*水里的岩浆刻：删*/
	ok=ok&&add_Tick(lv,false,"minecraft:stone",1,-30,13);        /*石头上的石头刻：留*/
	ok=ok&&add_Tick(lv,false,"minecraft:torch",3,-30,13);        /*空气上的火把刻：删*/
	Chunk *c=lv.get_Chunk(D,0,0);
	if(!c){
		return false;
	}
	c->con.set_Long("InhabitedTime",100);/*别的区块 InhabitedTime=0，两边都会被删*/
	ok=ok&&lv.set_Chunk(D,0,0,*c);
	return ok&&lv.save();
}

/*---- merge 用例 ----*/
static bool drop_Section(const std::string &w,int sy){/*让 new 少一段：merge 不补段，这一段的格子跳过*/
	Level lv(w.c_str(),16,4,4);
	Chunk *c=lv.get_Chunk(D,0,0);
	nbt_Any *sa=c?c->con.get("sections"):NULL;
	if(!sa||sa->type!=NBT_LIST){
		return false;
	}
	nbt_List *l=(nbt_List*)sa;
	for(size_t i=0;i<l->con.size();i++){
		nbt_Any *y=l->con[i]&&l->con[i]->type==NBT_COMPOUND?((nbt_Compound*)l->con[i])->get("Y"):NULL;
		if(y&&y->as_Int()==sy){
			l->erase(i);
			return lv.set_Chunk(D,0,0,*c)&&lv.save();
		}
	}
	return false;
}
static bool paint_Now(const std::string &w){/*merge 用的 now：在几个格子上放和 new/old 不一样的方块*/
	Level lv(w.c_str(),16,4,4);
	bool ok=set_BlockAt(lv,2,-50,2,make_Block("minecraft:gold_block"));
	ok=ok&&set_BlockAt(lv,3,-50,3,make_Block("minecraft:oak_slab","type=top,waterlogged=false"));
	ok=ok&&set_BlockAt(lv,4,-40,4,make_Block("minecraft:diamond_block"));/*Y=-3 段：m_new 里没有这一段*/
	if(!ok||!lv.save()){
		return false;
	}
	/*区块 (0,0) 加一只猪（实体区块）和一条 POI：merge 走库的中间格式搬，按 new 的版本写*/
	std::string ef=get_MCAPath((w+"/"+lv.get_DimensionPath(D,"entities")).c_str(),0,0);
	std::string pf=get_MCAPath((w+"/"+lv.get_DimensionPath(D,"poi")).c_str(),0,0);
	MCA em(ef),pm(pf);
	std::optional<nbt_Compound> e=em.get_Compound(0,0),p=pm.get_Compound(0,0);
	nbt_Compound ec=e?*e:make_EntityChunk(0,0,lv.data_version),pc=p?*p:make_POIChunk(lv.data_version);
	Entity pig("minecraft:pig");
	pig.set_Pos(6.5,-50,6.5);
	return add_ChunkEntity(&ec,pig)&&set_POI(&pc,7,-50,7,POI("minecraft:home"))&&
		em.set_Compound(ec,0,0)&&pm.set_Compound(pc,0,0)&&
		!system(("mkdir -p "+w+"/"+lv.get_DimensionPath(D,"entities")+" "+w+"/"+lv.get_DimensionPath(D,"poi")).c_str())&&
		em.save(ef.c_str())&&pm.save(pf.c_str());
}
/*1.13.2（DataVersion 1631）的空板：level.dat + region/r.0.0.mca 里一个生成完的区块 (0,0)。
  照 1.13 的写法：Level 外壳、Status=postprocessed（1905 以前生成完的名字）、段里直接是 Palette/BlockStates；
  Y=0、1 两段全是石头，别的段不写（＝空气，merge 不补段）*/
static bool make_World113(const std::string &w){
	if(system(("rm -rf "+w+" && mkdir -p "+w+"/region").c_str())){
		return false;
	}
	nbt_Compound dat;
	nbt_Compound *data=new nbt_Compound,*ver=new nbt_Compound;
	data->set_Int("DataVersion",1631);
	ver->set_Int("Id",1631);
	ver->set_String("Name","1.13.2");
	data->set("Version",ver);
	dat.set("Data",data);
	if(!dat.save_Dat((w+"/level.dat").c_str())){
		return false;
	}
	Chunk_Layout L=get_ChunkLayout(1631);
	nbt_Compound c;
	c.set_Int("DataVersion",1631);
	nbt_Compound *body=new nbt_Compound;
	c.set("Level",body);
	body->set_Int("xPos",0);
	body->set_Int("zPos",0);
	body->set_String(L.status,"postprocessed");
	body->set_Long("InhabitedTime",0);
	nbt_List *secs=new nbt_List(NBT_COMPOUND);
	for(int y=0;y<2;y++){
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte((int8_t)y));
		nbt_List *pal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *st=new nbt_Compound;
		st->set_String("Name","minecraft:stone");
		pal->push_back(st);
		sec->set(L.palette,pal);
		sec->set(L.data,new nbt_Long_Array(std::vector<int64_t>(256,0)));/*最少 4 位，4096 格 256 个 long*/
		secs->push_back(sec);
	}
	body->set(L.sections,secs);
	std::string p=get_MCAPath((w+"/region").c_str(),0,0);
	MCA m(p);
	return m.set_Compound(c,0,0)&&m.save(p.c_str());
}
int main(int argc,char **argv){/*参数 fix 或 merge：造 test/work/fmt 下面那几个世界*/
	std::string f=F;
	if(argc>1&&!strcmp(argv[1],"merge")){
		std::string cmd="cd "+f+" && rm -rf m_new m_now m_old x_new x_now x_old y_new y_now y_old z_now z_old"
			" && cp -a new m_new && cp -a new m_now && cp -a new m_old"
			" && cp -a new x_new && cp -a old x_now && cp -a old x_old"
			" && cp -a old y_new && cp -a new y_now && cp -a new y_old"
			" && cp -a old z_now && cp -a old z_old";
		if(system(cmd.c_str())){
			return 1;
		}
		if(!drop_Section(f+"/m_new",-3)||!paint_Now(f+"/m_now")){
			return 1;
		}
		if(!paint_Now(f+"/x_now")||!paint_Now(f+"/y_now")||!paint_Now(f+"/z_now")||!make_World113(f+"/z_new")){
			return 1;
		}
		{/*z_now 再改一格落在 1.13 高度里的（Y=0 段）：搬进 1.13 那块，按 1.13 的写法写*/
			Level lz((f+"/z_now").c_str(),16,4,4);
			if(!set_BlockAt(lz,8,10,8,make_Block("minecraft:emerald_block"))||!lz.save()){
				return 1;
			}
		}
		printf("mkfmt merge: m_*（5119→5119）、x_*（4788→5119）、y_*（5119→4788）、z_*（4788→1631）就绪\n");
		return 0;
	}
	std::string cmd="rm -rf "+f+"/old "+f+"/new && mkdir -p "+f+
		" && cp -a test/data/lvlworld "+f+"/old && cp -a test/data/lvlworld "+f+"/new";
	if(system(cmd.c_str())){
		return 1;
	}
	if(!convert_World(f+"/new")){
		printf("mkfmt: 转 26.4 写法失败\n");
		return 1;
	}
	for(const char *w:{"old","new"}){
		Level lv((f+"/"+w).c_str(),16,4,4);
		if(!lv.dat_ok||!build_Scene(lv)){
			printf("mkfmt: %s 布置场景失败\n",w);
			return 1;
		}
	}
	printf("mkfmt fix: old（4788）、new（5119）就绪\n");
	return 0;
}
