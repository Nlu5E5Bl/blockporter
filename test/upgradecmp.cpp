#include "../nbtrw.hpp"
#include <functional>
#include <string>
#include <vector>
/*upgradecmp.cpp：段的方块状态（2832 以前段里的 Palette/BlockStates，之后的 block_states）库的读法和游戏对拍（test/mcdump.sh upgrade 调它，
 *Java 那边是 test/mc/UpgradeProbe.java：游戏先用 DataFixers 升级、再用 26.1.1 的编解码读）。
 *	upgradecmp prep  <目录>  造一批老格式的区块，每个只有 Y=2 一段，写成 in_<名字>.nbt
 *	upgradecmp check <目录>  读游戏写的 out_<名字>.txt：游戏读不了（FATAL/UPGRADE_FAIL）↔ 库 check_SectionStates 判坏；
 *	                         游戏读得了 → 库逐格读出来的方块和游戏一样（库读不出来的格子要游戏是空气；
 *	                         库认不出名字的调色板条目 fixer 换空气，也当空气比）*/
static int bad=0,known=0;
static void check_Case(bool c,const std::string &msg){/*一项核对：打 ok/FAIL，记失败数*/
	printf("%s %s\n",c?"ok  ":"FAIL",msg.c_str());
	if(!c){
		bad++;
	}
}
static std::vector<std::string> names;
static void init_Names(){/*攒一批真实存在的方块名（16 色 × 6 种）给调色板用：游戏不认识的名字会被换成空气*/
	static const char *colors[16]={"white","orange","magenta","light_blue","yellow","lime","pink","gray",
		"light_gray","cyan","purple","blue","brown","green","red","black"};
	static const char *kinds[6]={"wool","concrete","terracotta","stained_glass","concrete_powder","carpet"};
	for(const char *k:kinds){
		for(const char *c:colors){
			names.push_back(std::string("minecraft:")+c+"_"+k);
		}
	}
}
static uint32_t get_Pattern(uint32_t i){return i*7+i/16;}/*第 i 格的测试图样（再对调色板条数取模）*/
static std::vector<int64_t> pack_Indices(const std::vector<uint32_t> &v,uint32_t bits,bool span){/*4096 个下标按位宽打包成 long 数组（span＝紧排、跨 long）*/
	std::vector<int64_t> w(get_PackedWords(4096,bits,span),0);
	for(uint32_t i=0;i<4096;i++){
		set_PackedIndex(w,i,bits,span,v[i]);
	}
	return w;
}
static std::vector<uint32_t> make_Values(uint32_t mod){/*4096 个下标的测试图样，都在 [0,mod) 里*/
	std::vector<uint32_t> v(4096);
	for(uint32_t i=0;i<4096;i++){
		v[i]=mod?get_Pattern(i)%mod:0;
	}
	return v;
}
/*有几格换成 bad（越界下标）*/
static std::vector<uint32_t> poke_Index(std::vector<uint32_t> v,uint32_t bad_index){
	for(uint32_t i=5;i<4096;i+=97){
		v[i]=bad_index;
	}
	return v;
}
static nbt_List *make_Palette(uint32_t n){/*n 条的调色板：第 0 条空气，后面依次取 names 里的方块*/
	nbt_List *p=new nbt_List(NBT_COMPOUND);
	for(uint32_t i=0;i<n;i++){
		nbt_Compound *e=new nbt_Compound;
		e->set_String("Name",names[i]);
		p->push_back(e);
	}
	return p;
}
struct Case{
	std::string name;
	std::string note;/*非空：已知和游戏不一样（写原因），不算失败*/
	std::function<void(nbt_Compound *box,const Chunk_Layout &L)> build;/*往方块状态容器里写（新格式 block_states，老格式段本身）*/
};
static std::vector<Case> make_Cases(){/*要测的那 32 种段：正常的、长了短了、位宽不对、下标越界、类型不对…*/
	std::vector<Case> cs;
	auto pal_data=[](uint32_t n,std::function<std::vector<int64_t>(bool)> data){
		return [n,data](nbt_Compound *box,const Chunk_Layout &L){
			box->set(L.palette,make_Palette(n));
			if(data){
				box->set(L.data,new nbt_Long_Array(data(L.span)));
			}
		};
	};
	auto exact=[](uint32_t n,uint32_t bits){
		return [n,bits](bool span){return pack_Indices(make_Values(n),bits,span);};
	};
	auto words=[](size_t k,uint32_t seed){
		return [k,seed](bool){
			std::vector<int64_t> w(k);
			for(size_t i=0;i<k;i++){
				w[i]=seed?(int64_t)((i+1)*0x9E3779B97F4A7C15ull*seed):0;
			}
			return w;
		};
	};
	cs.push_back({"exact17","",pal_data(17,exact(17,5))});
	cs.push_back({"exact3","",pal_data(3,exact(3,4))});
	cs.push_back({"exact40","",pal_data(40,exact(40,6))});
	cs.push_back({"exact16","",pal_data(16,exact(16,4))});
	cs.push_back({"one_wrong","",pal_data(1,words(7,3))});
	cs.push_back({"one_none","",pal_data(1,NULL)});
	cs.push_back({"one_exact","",pal_data(1,words(256,0))});
	cs.push_back({"nopal","",[](nbt_Compound *box,const Chunk_Layout &L){box->set(L.data,new nbt_Long_Array(std::vector<int64_t>(256,0)));}});
	cs.push_back({"empty256","",pal_data(0,words(256,0))});
	cs.push_back({"empty256x","",pal_data(0,words(256,5))});
	cs.push_back({"empty0","",pal_data(0,words(0,0))});
	cs.push_back({"empty64","",pal_data(0,words(64,0))});
	cs.push_back({"short17","",pal_data(17,[](bool span){std::vector<int64_t> w=pack_Indices(make_Values(17),5,span);w.resize(w.size()-20);return w;})});
	cs.push_back({"long17","",pal_data(17,[](bool span){std::vector<int64_t> w=pack_Indices(make_Values(17),5,span);w.resize(w.size()+30,0);return w;})});
	cs.push_back({"wide17","",pal_data(17,[](bool span){return pack_Indices(make_Values(17),6,span);})});
	cs.push_back({"wide3_8","",pal_data(3,[](bool span){return pack_Indices(make_Values(3),8,span);})});
	cs.push_back({"wide3_5","",pal_data(3,[](bool span){return pack_Indices(make_Values(3),5,span);})});
	cs.push_back({"oor3_8","",pal_data(3,[](bool span){return pack_Indices(poke_Index(make_Values(3),8),4,span);})});
	cs.push_back({"oor3_12","",pal_data(3,[](bool span){return pack_Indices(poke_Index(make_Values(3),12),4,span);})});
	cs.push_back({"oor8_8","",pal_data(8,[](bool span){return pack_Indices(poke_Index(make_Values(8),8),4,span);})});
	cs.push_back({"oor4_6","",pal_data(4,[](bool span){return pack_Indices(poke_Index(make_Values(4),6),4,span);})});
	cs.push_back({"oor2_3","",pal_data(2,[](bool span){return pack_Indices(poke_Index(make_Values(2),3),4,span);})});
	cs.push_back({"oor12_13","",pal_data(12,[](bool span){return pack_Indices(poke_Index(make_Values(12),13),4,span);})});
	cs.push_back({"oor17_20","",pal_data(17,[](bool span){return pack_Indices(poke_Index(make_Values(17),20),5,span);})});
	cs.push_back({"short16","",pal_data(16,words(200,0))});
	cs.push_back({"empty_data17","",pal_data(17,words(0,0))});
	cs.push_back({"pal_int","",[](nbt_Compound *box,const Chunk_Layout &L){
		box->set_Int(L.palette,5);
		box->set(L.data,new nbt_Long_Array(std::vector<int64_t>(256,0)));
	}});
	cs.push_back({"bs_int","",[](nbt_Compound *box,const Chunk_Layout &L){bool span=L.span;
		box->set(L.palette,make_Palette(3));
		std::vector<int64_t> w=pack_Indices(make_Values(3),4,span);
		std::vector<int32_t> iv(w.size());
		for(size_t i=0;i<w.size();i++){
			iv[i]=(int32_t)w[i];
		}
		box->set(L.data,new nbt_Int_Array(std::move(iv)));
	}});
	cs.push_back({"bs_list","",[](nbt_Compound *box,const Chunk_Layout &L){bool span=L.span;
		box->set(L.palette,make_Palette(3));
		nbt_List *l=new nbt_List(NBT_LONG);
		for(int64_t x:pack_Indices(make_Values(3),4,span)){
			l->push_back(new nbt_Long(x));
		}
		box->set(L.data,l);
	}});
	cs.push_back({"pal_str","",[](nbt_Compound *box,const Chunk_Layout &L){bool span=L.span;
		nbt_List *p=new nbt_List(NBT_STRING);
		for(int i=0;i<3;i++){
			p->push_back(new nbt_String(names[i]));
		}
		box->set(L.palette,p);
		box->set(L.data,new nbt_Long_Array(pack_Indices(make_Values(3),4,span)));
	}});
	cs.push_back({"pal_noname","",[](nbt_Compound *box,const Chunk_Layout &L){bool span=L.span;
		nbt_List *p=make_Palette(3);
		((nbt_Compound*)p->con[1])->erase("Name");
		box->set(L.palette,p);
		box->set(L.data,new nbt_Long_Array(pack_Indices(make_Values(3),4,span)));
	}});
	cs.push_back({"pal_unknown","库没有方块注册表：写法合法、游戏不认识的名字库照留（游戏换空气）",[](nbt_Compound *box,const Chunk_Layout &L){bool span=L.span;
		nbt_List *p=make_Palette(3);
		((nbt_Compound*)p->con[1])->set_String("Name","minecraft:no_such_block");
		box->set(L.palette,p);
		box->set(L.data,new nbt_Long_Array(pack_Indices(make_Values(3),4,span)));
	}});
	return cs;
}
static const int32_t dvs[]={1631,2230,2586,2730,2860,4788};/*1.13.2/1.15.2 紧排；1.16.5/1.17.1 不跨 long；1.18.1 新格式要升级；26.1.1 直接读*/
static std::string get_Path(const std::string &dir,const std::string &kind,int32_t dv,const std::string &name,const char *ext){
	return dir+"/"+kind+"_"+std::to_string(dv)+"_"+name+ext;
}
static nbt_Compound make_Chunk(int32_t dv,const Case &k){/*按 dv 这个版本的格式造一个只有一段的区块，段里是 k 这种情况*/
	Chunk_Layout L=get_ChunkLayout(dv);
	nbt_Compound c;
	c.set_Int("DataVersion",dv);
	nbt_Compound *lv=L.level?new nbt_Compound:&c;
	lv->set_Int("xPos",0);
	lv->set_Int("zPos",0);
	lv->set_String(L.status,dv<CHUNK_STATUS_FULL?"postprocessed":(dv<3450?"full":"minecraft:full"));
	lv->set_Long("InhabitedTime",5);
	lv->set_Long("LastUpdate",5);
	nbt_List *secs=new nbt_List(NBT_COMPOUND);
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte(2));
	nbt_Compound *box=sec;
	if(L.states){
		box=new nbt_Compound;
		sec->set("block_states",box);
	}
	k.build(box,L);
	secs->push_back(sec);
	lv->set(L.sections,secs);
	if(L.level){
		c.set("Level",lv);
	}
	return c;
}
int main(int argc,char **argv){/*prep / check 两个子命令，做什么见文件头*/
	if(argc<3){
		printf("用法：upgradecmp prep|check <目录>\n");
		return 2;
	}
	init_Names();
	std::string cmd=argv[1],dir=argv[2];
	std::vector<Case> cs=make_Cases();
	if(cmd=="prep"){
		for(int32_t dv:dvs){
			for(const Case &k:cs){
				nbt_Compound c=make_Chunk(dv,k);
				check_Case(c.save(get_Path(dir,"in",dv,k.name,".nbt").c_str()),"写 "+std::to_string(dv)+" "+k.name);
			}
		}
		return bad?1:0;
	}
	if(cmd!="check"){
		printf("不认识的命令：%s\n",cmd.c_str());
		return 2;
	}
	for(int32_t dv:dvs){
		for(const Case &k:cs){
			std::string tag=std::to_string(dv)+" "+k.name+"：";
			std::string game;
			FILE *fp=fopen(get_Path(dir,"out",dv,k.name,".txt").c_str(),"r");
			if(fp){
				std::string line;
				int ch;
				while((ch=fgetc(fp))!=EOF){
					if(ch=='\n'){
						if(line.rfind("Y 2 ",0)==0||line.rfind("UPGRADE_FAIL",0)==0){
							game=line;
						}
						line.clear();
					}else{
						line+=(char)ch;
					}
				}
				fclose(fp);
			}
			if(game.empty()){
				check_Case(false,tag+"游戏没写 Y=2 这一段的结果");
				continue;
			}
			nbt_Compound in=make_Chunk(dv,k);
			Chunk c(in);
			Chunk_Layout L=get_ChunkLayout(dv);
			const nbt_Compound *sec=(const nbt_Compound*)((const nbt_List*)get_ChunkBody(&c.con)->get(L.sections))->con[0];
			bool lib_bad=!check_SectionStates(sec,L);
			bool game_bad=game.rfind("UPGRADE_FAIL",0)==0||game.rfind("Y 2 FATAL",0)==0||game.rfind("Y 2 GETTHROW",0)==0;/*取格子才抛：游戏 LevelChunkSection 构造时就数方块（recalcBlockCounts），一样整块载入失败*/
			std::string gshort=game.substr(0,std::min<size_t>(game.size(),game_bad?160:12));
			if(lib_bad||game_bad){
				bool same=lib_bad==game_bad;
				if(!same&&!k.note.empty()){
					known++;
					printf("已知 %s游戏「%s」库%s——%s\n",tag.c_str(),gshort.c_str(),lib_bad?"判坏":"读得了",k.note.c_str());
					continue;
				}
				check_Case(same,tag+"游戏「"+gshort+"」，库"+(lib_bad?"判坏":"读得了"));
				continue;
			}
			/*Y 2 OK <部分错误数> <4096 个名字>*/
			std::vector<std::string> gn;
			size_t pos=game.find(' ',game.find(' ',game.find(' ',game.find(' ')+1)+1)+1);
			while(pos!=std::string::npos&&gn.size()<4096){
				size_t e=game.find(' ',pos+1);
				gn.push_back(game.substr(pos+1,e==std::string::npos?std::string::npos:e-pos-1));
				pos=e;
			}
			if(gn.size()!=4096){
				check_Case(false,tag+"游戏的结果不是 4096 格");
				continue;
			}
			States_View sv=get_StatesView(sec,L,true);
			if(sv.kind==STATES_OK&&!sv.canonical){/*游戏读得了、库不原样读写：每格都要读不出来（merge 跳过、规则 blind）*/
				int readable=0;
				for(uint32_t i=0;i<4096;i++){
					readable+=c.get_Block((int)(i&15),32+(int)(i>>8),(int)((i>>4)&15)).valid()?1:0;
				}
				check_Case(!readable,tag+"游戏读得了，库不原样读写（当读不出来）"+(readable?"，却读出了 "+std::to_string(readable)+" 格":std::string()));
				continue;
			}
			int diff=0;
			std::string first;
			for(uint32_t i=0;i<4096;i++){
				int x=(int)(i&15),z=(int)((i>>4)&15),y=32+(int)(i>>8);
				Block b=c.get_Block(x,y,z);
				std::string ln=b.valid()?std::string(b.name()):std::string("minecraft:air");/*读不出来＝游戏的空气段*/
				if(ln.empty()){
					ln="minecraft:air";/*认不出名字的条目 fixer 换成空气*/
				}
				if(ln!=gn[i]){
					if(!diff){
						first="第 "+std::to_string(i)+" 格 游戏 "+gn[i]+" 库 "+ln;
					}
					diff++;
				}
			}
			if(diff&&!k.note.empty()){
				known++;
				printf("已知 %s%d 格不一样（%s）——%s\n",tag.c_str(),diff,first.c_str(),k.note.c_str());
				continue;
			}
			check_Case(!diff,tag+"游戏读得了，逐格"+(diff?"差 "+std::to_string(diff)+" 格（"+first+"）":std::string("一样")));
		}
	}
	printf("%s：%d 处不对，已知 %d 处\n",bad?"FAIL":"OK",bad,known);
	return bad?1:0;
}
