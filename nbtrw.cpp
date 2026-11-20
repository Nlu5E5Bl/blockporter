#include "nbtrw.hpp"
/*nbtrw.cpp：libnbtrw 的 demo（不是库的一部分），用子命令跑：
 *	./nbtrw nbt   <file>							读NBT文件，打印整棵树
 *	./nbtrw nbtw  <file>							编码/深拷贝/比较/转换/增删改自测
 *	./nbtrw dat   <file>							level.dat → 树 → 打印
 *	./nbtrw mca   <dir> <rx> <rz> <cx> <cz> [w]		读区块，w=回写（生成.mca_test.mca）
 *	./nbtrw chunk <file> <x> <y> <z> [name]			读方块，给了name就写进去再编码-解析回来验证
 *	./nbtrw level <dir>								看Level怎么认这个存档（一个Level，三个维度）
 *	所有参数都要给，没有默认路径/默认坐标；缺参数直接退出。
 *	这个demo只演示底层库：读NBT、编码、比较/转换、level.dat、区域文件、区块里取方块、存档目录。
 */

static int demo_NBT(const char *path){/*nbt：读一个裸 NBT 文件，整棵树打出来*/
	nbt_Compound *root=nbt_Compound::read(path);
	if(!root){
		printf("no file\n");
		return -1;
	}
	root->print();
	putchar('\n');
	root->free();
	return 0;
}

static int demo_NBTW(const char *path){/*nbtw：读进来以后把编码/重新解析/深拷贝/增删成员/转换/比较各试一遍，每项打 ok 或 FAIL*/
	nbt_Compound *root=nbt_Compound::read(path);
	if(!root){
		printf("no file\n");
		return -1;
	}
	nbt_Raw raw=root->to_Raw();
	nbt_Raw file=nbt_Raw::load(path);
	printf("size: %zu -> %zu %s\n",file.data.size(),raw.data.size(),file.data.size()==raw.data.size()?"ok":"SIZE CHANGED");
	nbt_Raw again=raw;
	nbt_Compound *back=again.read_Root();
	printf("roundtrip: %s\n",back&&*root==*back?"ok":"FAIL");/*重新解析后逐值比较*/
	if(back){
		back->free();
	}
	nbt_Any *copy=root->copy();
	printf("copy: %s\n",copy&&*root==*copy?"ok":"FAIL");
	root->set_Long("nbtw_test",123);
	printf("set: %s\n",*root!=*copy&&root->get("nbtw_test")&&root->get("nbtw_test")->as_Long()==123?"ok":"FAIL");
	root->erase("nbtw_test");
	nbt_Raw back2=root->to_Raw();
	printf("del: %s (size %zu, 编码字节%s)\n",*root==*copy&&back2.data.size()==raw.data.size()?"ok":"FAIL",back2.data.size(),
		back2.data.size()==raw.data.size()&&!memcmp(back2.data.data(),raw.data.data(),raw.data.size())?"和原树逐位相同":"长度相同但顺序变了(unordered_map的rehash)");
	copy->free();
	root->free();

	/*nbt结构体的接口自测：data()给con的指针、隐式转换、as_*（转不了给nullopt）、比较、转换*/
	nbt_Int i(7);
	nbt_Long *l=(nbt_Long*)i.convert(NBT_LONG);/*byte/short/int→long*/
	nbt_Any *bad=l?l->convert(NBT_INT):NULL;/*高精度→低精度：不允许*/
	printf("convert: %s（int→long=%s，long→int=%s）\n",l&&l->con==7&&!bad?"ok":"FAIL",
		l&&l->con==7?"ok":"FAIL",!bad?"拒绝":"FAIL");
	if(bad){
		bad->free();
	}
	if(l){
		l->free();
	}
	nbt_Byte_Array ba(std::vector<int8_t>{1,2,3});
	nbt_Int_Array *ia=(nbt_Int_Array*)ba.convert(NBT_INT_ARRAY);
	printf("array convert: %s（%zu项）\n",ia&&ia->con.size()==3?"ok":"FAIL",ia?ia->con.size():0);
	if(ia){
		ia->free();
	}
	nbt_Int a(5);
	nbt_Long b(5);
	nbt_Short c2(6);
	printf("compare: %s（Int5==Long5，Int5<Short6，Int5!=Long6）\n",
		a==b&&a<c2&&a!=nbt_Long(6)?"ok":"FAIL");
	std::vector<nbt_Any*> v;
	v.push_back(new nbt_Int(1));
	v.push_back(new nbt_Int(2));
	nbt_List list(std::move(v));/*接管指针；类型统一*/
	printf("list: %s（elem=Int，%zu项，data()就是con：%s）\n",list.elem==NBT_INT&&list.size()==2?"ok":"FAIL",
		list.size(),list.data()==&list.con?"ok":"FAIL");
	int64_t wide=a;/*nbt_Int隐式转int32_t，再由C++扩到int64_t*/
	std::vector<int64_t> wv=ba;/*byte_array隐式转成更高精度的vector*/
	nbt_String str("minecraft:air");
	const nbt_Any &any=str;
	printf("as/隐式转换: %s（Int→int64=%lld，byte_array→long vector %zu项，String as_Int=%s，as_String==air %s）\n",
		wide==5&&wv.size()==3&&!any.as_Int()&&any.as_String()=="minecraft:air"&&str=="minecraft:air"?"ok":"FAIL",
		(long long)wide,wv.size(),any.as_Int()?"有值":"空",any.as_String()=="minecraft:air"?"ok":"FAIL");
	return 0;
}

static int demo_Dat(const char *path){/*dat：读 level.dat 这类（gzip/zlib/裸都认），整棵树打出来*/
	nbt_Compound *root=nbt_Compound::read_Dat(path);
	if(!root){
		printf("no dat\n");
		return -1;
	}
	root->print();
	putchar('\n');
	root->free();
	return 0;
}

static int demo_MCA(int argc,char **argv){/*mca：从 <dir>/r.<rx>.<rz>.mca 里读区块 (cx,cz) 打出来；带 w 再写回内存镜像、另存成 .mca_test.mca*/
	if(argc<5){
		return -1;
	}
	const char *path=argv[0];
	int32_t rx=0,rz=0,cx=0,cz=0;
	if(!parse_Int(argv[1],&rx)||!parse_Int(argv[2],&rz)||
	   !parse_Int(argv[3],&cx)||!parse_Int(argv[4],&cz)||cx<0||cx>31||cz<0||cz>31){
		printf("mca: rx/rz/cx/cz 必须是整数，cx/cz 在0~31\n");
		return -1;
	}
	std::string file=get_MCAPath(path,rx,rz);
	MCA mca(file);
	if(mca.bad){
		printf("bad mca\n");
		return -1;
	}
	if(mca.data.empty()){
		printf("no mca\n");
		return -1;
	}
	std::optional<nbt_Compound> raw=mca.get_Compound((uint32_t)cx,(uint32_t)cz);/*MCA读出来的是nbt_Compound*/
	if(!raw){
		printf("no chunk\n");
		return -1;
	}
	Chunk chunk(std::move(*raw));/*要当区块用就转成Chunk（接管，不拷贝）*/
	chunk.con.print();
	putchar('\n');
	if(argc>5&&argv[5][0]=='w'){/*回写自测：Chunk先转回nbt_Compound再写进内存镜像→落盘成.mca_test.mca*/
		bool set=mca.set_Compound((const nbt_Compound&)chunk,(uint32_t)cx,(uint32_t)cz);
		bool saved=set&&mca.save(".mca_test.mca");
		printf("write: %s size:%zu\n",saved?"ok":"失败",chunk.con.to_Raw().data.size());
	}
	return 0;
}

static int demo_Chunk(int argc,char **argv){/*chunk：把一个裸 NBT 文件当区块读，取 (x,y,z) 的方块；给了 name 就先写进去，再编码、解析回来确认*/
	if(argc<4){
		return -1;
	}
	const char *path=argv[0];
	int32_t x=0,y=0,z=0;
	if(!parse_Int(argv[1],&x)||!parse_Int(argv[2],&y)||!parse_Int(argv[3],&z)){
		printf("chunk: x/y/z 必须是整数\n");
		return -1;
	}
	const char *name=argc>4?argv[4]:NULL;
	nbt_Compound *root=nbt_Compound::read(path);
	if(!root){
		printf("no chunk\n");
		return -1;
	}
	Chunk chunk(std::move(*root));
	root->free();
	if(name){/*写回自测：造一个只有id的Block塞进去，再编码-解析回来确认*/
		Block block(name);
		printf("set: %s\n",chunk.set_Block(block,x,y,z)?"ok":"失败");
		nbt_Raw raw=chunk.con.to_Raw();
		nbt_Raw again=raw;
		nbt_Compound *back=again.read_Root();
		if(!back){
			printf("reparse: 读不回来（编码出来的东西自己解析不了？）\n");
			return -1;
		}
		Chunk chunk2(std::move(*back));
		back->free();
		Block b2=chunk2.get_Block(x,y,z);
		printf("reparse %zu bytes: %s\n",raw.data.size(),b2.valid()?b2.strid().c_str():"null");
	}
	Block b=chunk.get_Block(x,y,z);
	printf("block: %s\n",b.valid()?b.strid().c_str():"null");
	if(b.valid()){
		printf("strid: %s (spacename=%.*s, basename=%.*s, properties=%s)\n",
			b.strid().c_str(),(int)b.spacename().size(),b.spacename().data(),
			(int)b.basename().size(),b.basename().data(),b.has_Properties()?"有":"无");
	}
	printf("entity: ");
	if(b.has_BlockEntity()){
		b.block_entities.print();
	}else{
		printf("null");
	}
	printf("\n");
	return 0;
}

static int demo_Level(const char *path){/*level：看 Level 怎么认这个存档——DataVersion、目录布局、三个维度各自的区域目录和 (0,0) 区块*/
	static const char *dims[3]={"overworld","the_nether","the_end"};
	Level lv(path,8,4,8);/*Level是整个存档，不绑定维度*/
	printf("path: %s\n",lv.path.c_str());
	printf("level.dat: %s DataVersion=%d Version.Name=%s\n",lv.dat_ok?"读到":"读不到",lv.data_version,lv.version_name.c_str());
	printf("布局: %s（DataVersion%s%d）\n",
		lv.new_layout?"新":"老",lv.data_version>=DAT_NEW_LAYOUT?">=":"<",DAT_NEW_LAYOUT);
	for(int i=0;i<3;i++){
		std::string rdir=lv.get_DimensionPath(dims[i],"region");
		Chunk *c=lv.get_Chunk(dims[i],0,0);/*顺手证明路径对不对：能读到(0,0)就说明目录没认错*/
		int32_t xPos=0,zPos=0;
		if(c){
			const nbt_Compound *body=get_ChunkBody(&c->con);/*老格式（2842 以前）的 xPos/zPos 在 Level 里*/
			xPos=get_TagInt(body?body->get("xPos"):NULL).value_or(0);
			zPos=get_TagInt(body?body->get("zPos"):NULL).value_or(0);
		}
		printf("[%s] 区域目录=%s\n",dims[i],rdir.c_str());
		printf("  (0,0)的区块：%s",c?"有":"没有");
		if(c){
			printf("，里面的xPos=%d zPos=%d%s",xPos,zPos,(xPos||zPos)?" ← 对不上说明认错文件了":"");
		}
		printf("\n");
	}
	int ok=lv.dat_ok?0:-1;
	lv.close();
	return ok;
}

int main(int argc,char **argv){/*按第一个参数分到上面各个 demo；参数不够直接返回 -1*/
	if(argc<2){
		printf("usage: %s {nbt|nbtw|dat|mca|chunk|level} ...\n",argv[0]);
		printf("  %s nbt   <file>                          读NBT文件打印整棵树\n",argv[0]);
		printf("  %s nbtw  <file>                          编码/深拷贝/比较/转换自测\n",argv[0]);
		printf("  %s dat   <file>                          level.dat → 树 → 打印\n",argv[0]);
		printf("  %s mca   <dir> <rx> <rz> <cx> <cz> [w]    读区块，w=回写.mca_test.mca\n",argv[0]);
		printf("  %s chunk <file> <x> <y> <z> [name]       读方块，给了name就写进去\n",argv[0]);
		printf("  %s level <dir>                           一个Level看三个维度的目录/区块\n",argv[0]);
		printf("  所有路径/坐标都要显式给；缺参数直接退出。\n");
		return -1;
	}
	const char *cmd=argv[1];
	if(!strcmp(cmd,"nbt")){
		if(argc<3){
			return -1;
		}
		return demo_NBT(argv[2]);
	}
	if(!strcmp(cmd,"nbtw")){
		if(argc<3){
			return -1;
		}
		return demo_NBTW(argv[2]);
	}
	if(!strcmp(cmd,"dat")){
		if(argc<3){
			return -1;
		}
		return demo_Dat(argv[2]);
	}
	if(!strcmp(cmd,"mca")){
		if(argc<7){
			return -1;
		}
		return demo_MCA(argc-2,argv+2);
	}
	if(!strcmp(cmd,"chunk")){
		if(argc<6){
			return -1;
		}
		return demo_Chunk(argc-2,argv+2);
	}
	if(!strcmp(cmd,"level")){
		if(argc<3){
			return -1;
		}
		return demo_Level(argv[2]);
	}
	printf("unknown: %s\n",cmd);
	return -1;
}
