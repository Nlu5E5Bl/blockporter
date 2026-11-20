#include "../rules.hpp"
#include <dlfcn.h>
#include <string>
#include <vector>
/*shapecmp.cpp：手动工具——拿 ShapeDump 导出的游戏形状表逐个状态对拍 rules.so 的形状位图（priv[P_SHAPE]）
 *	shapecmp [rules.so] < shapes.tsv      默认 ./rules.so
 *	输出一行一处不一致：状态 \t 项 \t 游戏 \t 规则（1=有 0=没有）
 *	  游戏1规则0：规则比游戏严，可能误拆；游戏0规则1：规则比游戏松，只会漏拆。
 *	比的项：FULL_<面>、CENTER_U/CENTER_D（原版只在上下两面用 CENTER：火把/灯笼/蜡烛/钟/孢子花/压力板）、
 *	  ATTACH_<面>（FULL 或碰撞面满：MultifaceBlock.canAttachTo）、RIGID_U、COLL_U（碰撞箱顶面满：雪层）、SOLID、RENDER、TOPCOLL；
 *	  还有三种材质性质（在 priv[3] 的 PF_* 位，(priv[3]>>8)&位）：REPLACE（canBeReplaced）=4、LIQUID=8、BURN（FireBlock.canBurn）=16；
 *	  UNDER_P/N/E/S/W（放在墙上面时碰撞箱底面盖住墙的柱子那块/朝那边的臂，WallBlock.isCovered）。
 *	  竹子/滴水石锥的碰撞箱随坐标偏移，rules.so 不在位图里记、墙规则按坐标现算：这两种的 UNDER 不比位图，
 *	  改比 @off 行——在那个坐标上把它放在一段墙上面（南北两边接石头、再换成东西两边），让 rules.so 判墙，
 *	  和照游戏的 under 推出来的墙一样不一样（项名 WALL_NS/WALL_EW）。
 *	位图布局照 rules.cpp“按面的支撑位图”一节：FULL(d)=bit d，CENTER(d)=6+d，RIGID_UP=12，COLL(d)=13+d，
 *	  SOLID=19，RENDER=20，TOPCOLL=21，UNDER(d)=22+d（d=NB_U 那位是柱子）；d 按 NB_*（东西上下南北）。rules.cpp 改了布局这里要跟着改。
 *	编译：g++ -std=c++23 -O2 -I. test/shapecmp.cpp -o test/work/shapecmp -L. -lnbtrw -lz -ldl -Wl,-rpath,$PWD
 */
static const int col_Dir[6]={NB_D,NB_U,NB_N,NB_S,NB_W,NB_E};/*tsv 的六列：下 上 北 南 西 东*/
static const char *col_Name[6]={"D","U","N","S","W","E"};
static void split_Tab(const std::string &l,std::vector<std::string> &f){/*一行按制表符拆成列*/
	f.clear();
	size_t pos=0;
	for(;;){
		size_t q=l.find('\t',pos);
		f.push_back(l.substr(pos,q==std::string::npos?std::string::npos:q-pos));
		if(q==std::string::npos){
			break;
		}
		pos=q+1;
	}
}
static void print_Diff(const std::string &st,const char *item,const char *face,bool game,bool ours){/*游戏和 rules.so 在某一项上不一样就打一行*/
	if(game!=ours){
		printf("%s\t%s%s\t%d\t%d\n",st.c_str(),item,face,game?1:0,ours?1:0);
	}
}
static void parse_State(const std::string &st,std::string &name,nbt_Compound &props){/*minecraft:名字[k=v,...]*/
	name=st;
	std::string p;
	size_t b=name.find('[');
	if(b!=std::string::npos){
		p=name.substr(b+1,name.size()-b-2);
		name=name.substr(0,b);
	}
	size_t pos=0;
	while(pos<p.size()){
		size_t q=p.find(',',pos);
		if(q==std::string::npos){
			q=p.size();
		}
		std::string kv=p.substr(pos,q-pos);
		size_t eq=kv.find('=');
		if(eq!=std::string::npos){
			props.set_String(kv.substr(0,eq),kv.substr(eq+1));
		}
		pos=q+1;
	}
}
static const int under_Bit[5]={NB_U,NB_N,NB_E,NB_S,NB_W};/*under 列的字母 P N E S W*/
static const char *under_Ch="PNESW";
struct Cmp_Blk{/*shapecmp 自己造的一个方块（墙核对用）*/
	std::string name;
	nbt_Compound props;
	Blk_Info bi;
	void make(const Rules_Api *api,const std::string &st){
		props=nbt_Compound();
		parse_State(st,name,props);
		bi=Blk_Info();
		bi.name=name;
		bi.props=props.empty()?NULL:&props;
		api->fill_BlkInfo(&bi);
	}
};
/*@off 行：把 st 放在 (x,65,z)，下面 (x,64,z) 是一段墙、两边（ns=南北 / 东西）接石头，让 rules.so 判这段墙；
  照游戏的 under 推墙应该是什么样（WallBlock：连着的边上面盖住那条臂就是 tall；两边都 tall 不立柱子，
  不然看柱子那块盖没盖住），两边不一样就打一行*/
static void check_Offset(const Rules_Api *api,const std::string &st,int32_t x,int32_t z,const std::string &under,bool ns){
	static Cmp_Blk wall,stone,air;
	static bool init=false;
	if(!init){
		wall.make(api,"minecraft:cobblestone_wall[east=none,north=none,south=none,up=true,waterlogged=false,west=none]");
		stone.make(api,"minecraft:stone");
		air.make(api,"minecraft:air");
		init=true;
	}
	Cmp_Blk above;
	above.make(api,st);
	Rules_Ctx c;
	c.self=&wall.bi;
	c.x=x;c.y=64;c.z=z;
	for(int i=0;i<6;i++){
		c.nb[i]=&air.bi;
	}
	int a=ns?NB_N:NB_E,b=ns?NB_S:NB_W;
	c.nb[a]=&stone.bi;
	c.nb[b]=&stone.bi;
	c.nb[NB_U]=&above.bi;
	c.got=0x3F;
	int def=api->get_BlkRule(&wall.bi,0xFFFFFFFFu);
	int r=def<0?FX_KEEP:api->judge_Blk(&wall.bi,def,&c);
	const nbt_Compound &got=r==FX_SET?c.out_block.properties:wall.props;
	bool ta=under.find(ns?'N':'E')!=std::string::npos,tb=under.find(ns?'S':'W')!=std::string::npos;
	bool post=!(ta&&tb)&&under.find('P')!=std::string::npos;
	const char *ka=ns?"north":"east",*kb=ns?"south":"west";
	std::string want=std::string(ta?"tall":"low")+"/"+(tb?"tall":"low")+"/"+(post?"true":"false");
	std::string have=std::string(get_PropValue(&got,ka)?get_PropValue(&got,ka):"?")+"/"+
		(get_PropValue(&got,kb)?get_PropValue(&got,kb):"?")+"/"+(get_PropValue(&got,"up")?get_PropValue(&got,"up"):"?");
	if(want!=have){
		printf("%s@%d,%d\t%s\t%s\t%s\n",st.c_str(),x,z,ns?"WALL_NS":"WALL_EW",want.c_str(),have.c_str());
	}
}
int main(int argc,char **argv){/*从 stdin 读游戏导出的形状表（每行一个方块状态），逐状态让 rules.so 算形状位图，一位一位比；不一样的打出来*/
	const char *path=argc>1?argv[1]:"./rules.so";
	void *h=dlopen(path,RTLD_NOW|RTLD_LOCAL);
	Rules_Entry_Fn fn=h?(Rules_Entry_Fn)dlsym(h,RULES_ENTRY):NULL;
	const Rules_Api *api=fn?fn(RULES_ABI,(uint32_t)sizeof(Rules_Api)):NULL;
	if(!api){
		fprintf(stderr,"shapecmp: 装不上 %s\n",path);
		return 1;
	}
	char line[8192];
	std::vector<std::string> f;
	while(fgets(line,sizeof(line),stdin)){
		std::string l(line);
		while(!l.empty()&&(l.back()=='\n'||l.back()=='\r')){
			l.pop_back();
		}
		if(l.empty()||l[0]=='#'){
			continue;
		}
		split_Tab(l,f);
		if(f[0]=="@off"){
			if(f.size()<5){
				fprintf(stderr,"shapecmp: 列数不对：%s\n",l.c_str());
				return 1;
			}
			check_Offset(api,f[1],(int32_t)atol(f[2].c_str()),(int32_t)atol(f[3].c_str()),f[4],true);
			check_Offset(api,f[1],(int32_t)atol(f[2].c_str()),(int32_t)atol(f[3].c_str()),f[4],false);
			continue;
		}
		if(f.size()<11){
			fprintf(stderr,"shapecmp: 列数不对：%s\n",l.c_str());
			return 1;
		}
		std::string st=f[0],name=st,p;
		size_t b=name.find('[');
		if(b!=std::string::npos){
			p=name.substr(b+1,name.size()-b-2);
			name=name.substr(0,b);
		}
		nbt_Compound props;
		size_t pos=0;
		while(pos<p.size()){
			size_t q=p.find(',',pos);
			if(q==std::string::npos){
				q=p.size();
			}
			std::string kv=p.substr(pos,q-pos);
			size_t eq=kv.find('=');
			if(eq!=std::string::npos){
				props.set_String(kv.substr(0,eq),kv.substr(eq+1));
			}
			pos=q+1;
		}
		Blk_Info bi;
		bi.name=name;
		bi.props=props.empty()?NULL:&props;
		api->fill_BlkInfo(&bi);
		uint32_t s=bi.priv[0];
		for(int i=0;i<6;i++){
			int d=col_Dir[i];
			const std::string &g=f[1+i];
			bool g_full=g.find('F')!=std::string::npos;
			bool g_center=g.find('C')!=std::string::npos;
			bool g_rigid=g.find('R')!=std::string::npos;
			bool g_coll=g.find('K')!=std::string::npos;
			bool o_full=s&(1u<<d);
			bool o_center=s&(1u<<(6+d));
			bool o_coll=s&(1u<<(13+d));
			print_Diff(st,"FULL_",col_Name[i],g_full,o_full);
			if(d==NB_U||d==NB_D){
				print_Diff(st,"CENTER_",col_Name[i],g_center,o_center);
			}
			print_Diff(st,"ATTACH_",col_Name[i],g_full||g_coll,o_full||o_coll);
			if(d==NB_U){
				print_Diff(st,"RIGID_",col_Name[i],g_rigid,s&(1u<<12));
				print_Diff(st,"COLL_",col_Name[i],g_coll,o_coll);/*雪层看碰撞箱顶面（SnowLayerBlock.canSurvive）*/
			}
		}
		print_Diff(st,"SOLID","",f[7]=="solid",s&(1u<<19));
		print_Diff(st,"RENDER","",f[8]=="render",s&(1u<<20));
		print_Diff(st,"TOPCOLL","",f[10]=="topcoll",s&(1u<<21));
		if(f.size()>=14){
			uint32_t k=bi.priv[3]>>8;
			print_Diff(st,"REPLACE","",f[11]=="replace",k&4u);
			print_Diff(st,"LIQUID","",f[12]=="liquid",k&8u);
			print_Diff(st,"BURN","",f[13]=="burn",k&16u);
		}
		if(f.size()>=15&&name!="minecraft:bamboo"&&name!="minecraft:pointed_dripstone"&&name!="minecraft:sulfur_spike"){
			for(int i=0;i<5;i++){
				char item[16];
				snprintf(item,sizeof(item),"UNDER_%c",under_Ch[i]);
				print_Diff(st,item,"",f[14].find(under_Ch[i])!=std::string::npos,s&(1u<<(22+under_Bit[i])));
			}
		}
		if(f.size()>=17){/*音符盒的音色（priv[3] 的 24..28 位，NoteBlockInstrument 的序号）、树叶当 distance 0 的（第 29 位）*/
			int gi=atoi(f[15].c_str()),oi=(int)((bi.priv[3]>>24)&31u);
			if(gi!=oi){
				printf("%s\tINSTR\t游戏%d\t规则%d\n",st.c_str(),gi,oi);
			}
			print_Diff(st,"LEAFKEEP","",f[16]=="leafkeep",(bi.priv[3]>>29)&1u);
		}
	}
	return 0;
}
