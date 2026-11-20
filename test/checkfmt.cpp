#include "../nbtrw.hpp"
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>
/*checkfmt.cpp：查 test/format.sh 的结果（只读）
 *	checkfmt fix    old(4788) 和 new(5119) 修完以后：区块一样多、每一格方块一样、计划刻一样；
 *	                new 里不许混进老写法（Name/Properties/Status），场景的预期结果逐条对。
 *	checkfmt merge  m_new（同版本）：抄过来的方块在、写法对，new 缺的段不补；
 *	                x_new（4788 合进 5119）、y_new（5119 合进 4788）、z_new（4788 合进 1.13.2）：暴力合并，改过的格子照搬、按 new 的格式写，
 *	                没改过的不动（InhabitedTime＝3）。*/
static const char *D="minecraft:overworld";
static const char *F="test/work/fmt";
static int bad=0;
static void check_Case(bool c,const char *msg){/*一项核对：打 ok/FAIL，记失败数*/
	printf("%s %s\n",c?"ok  ":"FAIL",msg);
	if(!c){
		bad++;
	}
}
typedef std::tuple<int,std::string,int,int,int,int,int> Tick_Key;
static std::set<std::pair<int,int>> list_Chunks(const std::string &w){/*这个世界主世界里现有的区块坐标（扫区域文件的表头）*/
	std::set<std::pair<int,int>> out;
	std::string rd=w+"/dimensions/minecraft/overworld/region";
	DIR *dp=opendir(rd.c_str());
	if(!dp){
		return out;
	}
	struct dirent *e;
	while((e=readdir(dp))){
		int rx,rz;
		if(sscanf(e->d_name,"r.%d.%d.mca",&rx,&rz)!=2){
			continue;
		}
		MCA m(get_MCAPath(rd.c_str(),rx,rz));
		for(uint32_t i=0;i<32;i++){
			for(uint32_t j=0;j<32;j++){
				if(!m.bad&&m.has_Chunk(i,j)){
					out.insert({rx*32+(int)i,rz*32+(int)j});
				}
			}
		}
	}
	closedir(dp);
	return out;
}
static std::vector<Tick_Key> list_Ticks(const Chunk *c){/*这个区块的方块刻和流体刻，排好序（两种格式的结果拿来直接比）*/
	std::vector<Tick_Key> v;
	for(int k=0;k<2;k++){
		int n=count_ChunkTicks(&c->con,k==1);
		for(int i=0;i<n;i++){
			Chunk_Tick t;
			if(get_ChunkTick(&c->con,k==1,i,&t)){
				v.emplace_back(k,std::string(t.type),t.x,t.y,t.z,t.delay,t.priority);
			}
		}
	}
	std::sort(v.begin(),v.end());
	return v;
}
static bool has_Tick(const Chunk *c,bool fluid,const char *type,int x,int y,int z){/*有没有这么一条刻*/
	for(const Tick_Key &t:list_Ticks(c)){
		if(std::get<0>(t)==(fluid?1:0)&&std::get<1>(t)==type&&std::get<2>(t)==x&&std::get<3>(t)==y&&std::get<4>(t)==z){
			return true;
		}
	}
	return false;
}
static bool is_NewStyle(const Chunk *c){/*26.4 写法：DataVersion 5119、status、调色板里没有 Name/Properties*/
	const nbt_Any *dv=c->con.get("DataVersion");
	if(!dv||dv->as_Int()!=5119||c->con.get("Status")||!c->con.get("status")){
		return false;
	}
	const nbt_Any *sa=c->con.get("sections");
	if(!sa||sa->type!=NBT_LIST){
		return false;
	}
	for(const nbt_Any *s:((const nbt_List*)sa)->con){
		const nbt_Any *bs=s&&s->type==NBT_COMPOUND?((const nbt_Compound*)s)->get("block_states"):NULL;
		const nbt_Any *pa=bs&&bs->type==NBT_COMPOUND?((const nbt_Compound*)bs)->get("palette"):NULL;
		if(!pa||pa->type!=NBT_LIST){
			continue;
		}
		for(const nbt_Any *e:((const nbt_List*)pa)->con){
			if(e->type==NBT_COMPOUND&&(((const nbt_Compound*)e)->get("Name")||((const nbt_Compound*)e)->get("Properties"))){
				return false;
			}
		}
	}
	return true;
}
static bool is_Prop(const Block &b,const char *k,const char *v){/*方块的属性 k 是不是 v*/
	const nbt_Any *p=b.properties.get(k);
	return p&&p->as_String()==v;
}
static int check_Fix(){/*fixer：4788 和 5119 两个世界修完以后，区块、每格方块、计划刻都一样，新格式的写法也对*/
	std::string fo=std::string(F)+"/old",fn=std::string(F)+"/new";
	std::set<std::pair<int,int>> co=list_Chunks(fo),cn=list_Chunks(fn);
	check_Case(!co.empty()&&co==cn,"两种格式修完剩下的区块一样");
	Level lo(fo.c_str(),16,4,4),ln(fn.c_str(),16,4,4);
	check_Case(lo.data_version==4788&&ln.data_version==5119,"level.dat 的 DataVersion 没被改");
	long diff=0,cells=0,style_bad=0,tick_diff=0;
	for(const auto &p:co){
		Chunk *a=lo.get_Chunk(D,p.first,p.second);
		Chunk *b=a?ln.pin_Chunk(D,p.first,p.second):NULL;
		if(!a||!b){
			diff++;
			continue;
		}
		if(!is_NewStyle(b)||check_Chunk(&a->con)!=check_Chunk(&b->con)){/*测试存档里有故意弄坏的区块：两边结论一样就行*/
			style_bad++;
		}
		if(list_Ticks(a)!=list_Ticks(b)){
			tick_diff++;
		}
		for(int y=-64;y<320;y++){
			for(int x=0;x<16;x++){
				for(int z=0;z<16;z++){
					cells++;
					if(a->get_Block(x,y,z)!=b->get_Block(x,y,z)){
						if(diff<5){
							printf("  不一样：区块(%d,%d) %d,%d,%d %s / %s\n",p.first,p.second,x,y,z,
								std::string(a->get_Block(x,y,z).name()).c_str(),std::string(b->get_Block(x,y,z).name()).c_str());
						}
						diff++;
					}
				}
			}
		}
		ln.unpin_Chunk(D,p.first,p.second);
	}
	printf("  比了 %ld 格，不一样 %ld 格；计划刻不一样的区块 %ld；new 里写法不对的区块 %ld\n",cells,diff,tick_diff,style_bad);
	check_Case(cells>0&&diff==0,"每一格方块两边一样");
	check_Case(tick_diff==0,"计划刻两边一样");
	check_Case(style_bad==0,"new 里全是 26.4 写法（status、id/properties/字符串），形状检查和 old 结论一样");
	/*场景的预期（两边一样，看 new 就够）*/
	check_Case(ln.get_Block(D,1,-30,1).name()=="minecraft:air","A 悬空火把拆了");
	Block w=ln.get_Block(D,3,-30,3);
	Chunk *c=ln.get_Chunk(D,0,0);
	check_Case(w.name()=="minecraft:water"&&is_Prop(w,"level","0")&&c&&has_Tick(c,true,"minecraft:water",3,-30,3),"B 含水灯笼拆完留水源，补了水刻");
	w=ln.get_Block(D,5,-30,5);
	check_Case(w.name()=="minecraft:water"&&is_Prop(w,"level","0")&&c&&has_Tick(c,true,"minecraft:water",5,-30,5),"C 霜冰化成水源，补了水刻");
	Block g=ln.get_Block(D,7,-30,7);
	check_Case(g.name()=="minecraft:glow_lichen"&&is_Prop(g,"north","false")&&is_Prop(g,"down","true"),"D 地衣关掉没支撑的北面");
	check_Case(ln.get_Block(D,9,-29,9).name()=="minecraft:torch","E 上半砖上的火把留着（新格式也认得出 type=top）");
	check_Case(ln.get_Block(D,11,-29,11).name()=="minecraft:air","F 下半砖上的火把拆了");
	check_Case(c&&has_Tick(c,true,"minecraft:flowing_water",13,-30,13),"G 流动水里的 flowing_water 刻留着");
	check_Case(c&&!has_Tick(c,true,"minecraft:water",13,-30,11)&&!has_Tick(c,true,"minecraft:lava",13,-30,9),"G 空气里的水刻、水里的岩浆刻删了");
	check_Case(c&&has_Tick(c,false,"minecraft:stone",1,-30,13)&&!has_Tick(c,false,"minecraft:torch",3,-30,13),"H 方块刻：对得上的留，对不上的删");
	Block l=ln.get_Block(D,5,-30,13);
	check_Case(l.name()=="minecraft:oak_log"&&is_Prop(l,"axis","x"),"I 不归规则管的带属性方块原样");
	return bad;
}
static int check_Merge(){/*merge：5119→5119 搬过去的方块和写法对，缺段的格子跳过*/
	/*同版本 m：new 有 Y=-4 段，方块照 new 的格式搬过去；new 没有 Y=-3 段，merge 不补段、那一格跳过*/
	{
		std::string dn=std::string(F)+"/m_new";
		Level ln(dn.c_str(),16,4,4);
		Chunk *c=ln.get_Chunk(D,0,0);
		Block slab=ln.get_Block(D,3,-50,3);
		check_Case(ln.get_Block(D,2,-50,2).name()=="minecraft:gold_block"&&slab.name()=="minecraft:oak_slab"&&is_Prop(slab,"type","top"),
			"同版本：now 的方块抄进 new");
		check_Case(c&&is_NewStyle(c)&&check_Chunk(&c->con),"同版本：写的方块是 26.4 写法");
		check_Case(c&&!c->get_Section(-3)&&ln.get_Block(D,4,-40,4).name()=="minecraft:air","同版本：new 缺的段不补，那一格跳过");
	}
	/*跨版本 x（4788 合进 5119）、y（5119 合进 4788）：照搬三格，写成 new 那边的格式；old 和 now 同版本，别的格子不动*/
	for(const char *w:{"x_new","y_new"}){
		std::string dn=std::string(F)+"/"+w;
		bool up=w[0]=='x';
		Level ln(dn.c_str(),16,4,4);
		Chunk *c=ln.get_Chunk(D,0,0);
		Block slab=ln.get_Block(D,3,-50,3);
		const nbt_Compound *b=c?get_ChunkBody(&c->con):NULL;
		std::string tag=std::string(up?"4788→5119":"5119→4788")+"：";
		check_Case(ln.get_Block(D,2,-50,2).name()=="minecraft:gold_block"&&slab.name()=="minecraft:oak_slab"&&is_Prop(slab,"type","top")&&
			ln.get_Block(D,4,-40,4).name()=="minecraft:diamond_block",(tag+"now 改过的三格抄进 new").c_str());
		check_Case(c&&(up?is_NewStyle(c):get_ChunkDataVersion(&c->con)==4788)&&check_Chunk(&c->con),(tag+"区块还是 new 的版本和写法").c_str());
		check_Case(b&&b->get("InhabitedTime")&&b->get("InhabitedTime")->as_Long()==std::optional<int64_t>(3),(tag+"只搬了这三格（InhabitedTime=3）").c_str());
		MCA em(get_MCAPath((dn+"/"+ln.get_DimensionPath(D,"entities")).c_str(),0,0));
		MCA pm(get_MCAPath((dn+"/"+ln.get_DimensionPath(D,"poi")).c_str(),0,0));
		std::optional<nbt_Compound> e=em.get_Compound(0,0),p=pm.get_Compound(0,0);
		std::vector<Entity> ents;
		std::vector<POI> pois;
		check_Case(e&&get_ChunkEntities(&*e,&ents)>=0&&ents.size()==1&&check_Entity(&*e,0,0)&&
			e->get("DataVersion")->as_Int()==ln.data_version,(tag+"猪搬进 new 的实体区块，版本号是 new 的").c_str());
		check_Case(p&&list_POI(&*p,&pois)==0&&pois.size()==1&&pois[0].name()=="minecraft:home"&&pois[0].y==-50&&check_POI(&*p),
			(tag+"POI 搬进 new").c_str());
	}
	/*z（4788 合进 1.13.2）：区块还是 1631 的写法；实体进区块的 Level.Entities；没有 POI、不建 poi/、不建 entities/*/
	{
		std::string dn=std::string(F)+"/z_new";
		Level ln(dn.c_str(),16,4,4);
		Chunk *c=ln.get_Chunk(D,0,0);
		const nbt_Compound *b=c?get_ChunkBody(&c->con):NULL;
		check_Case(c&&get_ChunkDataVersion(&c->con)==1631&&c->con.get("Level")&&check_Chunk(&c->con),"4788→1631：区块还是 1.13 的版本和写法");
		check_Case(ln.get_Block(D,8,10,8).name()=="minecraft:emerald_block"&&ln.get_Block(D,7,10,8).name()=="minecraft:stone",
			"4788→1631：1.13 高度里改过的那格搬进去，旁边不动");
		check_Case(b&&b->get("InhabitedTime")&&b->get("InhabitedTime")->as_Long()==std::optional<int64_t>(1),"4788→1631：InhabitedTime=1");
		std::vector<Entity> ents;
		check_Case(c&&get_ChunkEntities(c->data(),&ents)==1&&ents[0].name()=="minecraft:pig"&&b->get("Entities")&&!b->get("entities"),
			"4788→1631：猪写进区块的 Level.Entities");
		check_Case(c&&count_ChunkTicks(&c->con,false)+count_ChunkTicks(&c->con,true)==4&&b->get("TileTicks"),"4788→1631：计划刻写进 Level.TileTicks/LiquidTicks");
		std::string pp=get_MCAPath((dn+"/"+ln.get_DimensionPath(D,"poi")).c_str(),0,0);
		std::string ep=get_MCAPath((dn+"/"+ln.get_DimensionPath(D,"entities")).c_str(),0,0);
		check_Case(access(pp.c_str(),F_OK)!=0&&access(ep.c_str(),F_OK)!=0,"4788→1631：不建 poi/、entities/ 的文件");
	}
	return bad;
}
int main(int argc,char **argv){/*参数 fix 或 merge，分到上面两个*/
	if(argc>1&&!strcmp(argv[1],"merge")){
		check_Merge();
	}else{
		check_Fix();
	}
	printf("%s：%d 处不对\n",bad?"FAIL":"OK",bad);
	return bad?1:0;
}
