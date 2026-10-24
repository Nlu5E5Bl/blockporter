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

static int demo_nbt(const char *path){
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

static int demo_nbtw(const char *path){
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
	printf("roundtrip: %s\n",root->is_Equal(back)?"ok":"FAIL");/*重新解析后逐值比较*/
	if(back){
		back->free();
	}
	nbt_Any *copy=root->copy();
	printf("copy: %s\n",root->is_Equal(copy)?"ok":"FAIL");
	root->set_Long("nbtw_test",123);
	printf("set: %s\n",!root->is_Equal(copy)&&root->get("nbtw_test")&&root->get("nbtw_test")->as_Long()==123?"ok":"FAIL");
	root->erase("nbtw_test");
	nbt_Raw back2=root->to_Raw();
	printf("del: %s (size %zu, 编码字节%s)\n",root->is_Equal(copy)&&back2.data.size()==raw.data.size()?"ok":"FAIL",back2.data.size(),
		back2.data.size()==raw.data.size()&&!memcmp(back2.data.data(),raw.data.data(),raw.data.size())?"和原树逐位相同":"长度相同但顺序变了(unordered_map的rehash)");
	copy->free();
	root->free();

	/*nbt结构体的接口自测：data()深拷贝、从C++类型构造、比较、转换*/
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
	std::vector<nbt_Any*> ld=list.data();/*深拷贝：调用者接管新指针*/
	printf("list: %s（elem=Int，%zu项，data()深拷贝%zu项）\n",list.elem==NBT_INT&&list.size()==2?"ok":"FAIL",
		list.size(),ld.size());
	free_AnyVector(ld);/*data()给的指针自己释放*/
	return 0;
}

static int demo_dat(const char *path){
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

static int demo_mca(int argc,char **argv){
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
	Chunk *chunk=mca.get_Chunk((uint32_t)cx,(uint32_t)cz);
	if(!chunk){
		printf("no chunk\n");
		return -1;
	}
	chunk->print();
	putchar('\n');
	if(argc>5&&argv[5][0]=='w'){/*回写自测：编码→改内存镜像→落盘成.mca_test.mca*/
		mca.set_Chunk(*chunk,(uint32_t)cx,(uint32_t)cz);
		printf("write:%d size:%zu\n",mca.save(".mca_test.mca"),chunk->to_Raw().data.size());
	}
	chunk->free();
	return 0;
}

static int demo_chunk(int argc,char **argv){
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
	Chunk *chunk=Chunk::read(path);
	if(!chunk){
		printf("no chunk\n");
		return -1;
	}
	if(name){/*写回自测：造一个只有id的Block塞进去，再编码-解析回来确认*/
		Block block(name);
		printf("set: %d\n",chunk->set_Block(block,x,y,z));
		nbt_Raw raw=chunk->to_Raw();
		nbt_Raw again=raw;
		Chunk *chunk2=Chunk::read(&again);
		if(!chunk2){
			printf("reparse: 读不回来（编码出来的东西自己解析不了？）\n");
			chunk->free();
			return -1;
		}
		Block b2=chunk2->get_Block(x,y,z);
		printf("reparse %zu bytes: %s\n",raw.data.size(),b2.valid()?b2.strid().c_str():"null");
		chunk2->free();
	}
	Block b=chunk->get_Block(x,y,z);
	printf("block: %s\n",b.valid()?b.strid().c_str():"null");
	if(b.valid()){
		printf("strid: %s (spacename=%.*s, basename=%.*s, properties=%s)\n",
			b.strid().c_str(),(int)b.spacename().size(),b.spacename().data(),
			(int)b.basename().size(),b.basename().data(),b.has_Properties()?"有":"无");
	}
	printf("entity: ");
	if(b.has_BlockEntity())b.block_entities.print();
	else printf("null");
	printf("\n");
	chunk->free();
	return 0;
}

static int demo_level(const char *path){
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
			const nbt_Any *x=c->get("xPos"),*z=c->get("zPos");
			xPos=x&&x->type==NBT_INT?x->as_Int():0;
			zPos=z&&z->type==NBT_INT?z->as_Int():0;
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

int main(int argc,char **argv){
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
		if(argc<3)return -1;
		return demo_nbt(argv[2]);
	}
	if(!strcmp(cmd,"nbtw")){
		if(argc<3)return -1;
		return demo_nbtw(argv[2]);
	}
	if(!strcmp(cmd,"dat")){
		if(argc<3)return -1;
		return demo_dat(argv[2]);
	}
	if(!strcmp(cmd,"mca")){
		if(argc<7)return -1;
		return demo_mca(argc-2,argv+2);
	}
	if(!strcmp(cmd,"chunk")){
		if(argc<6)return -1;
		return demo_chunk(argc-2,argv+2);
	}
	if(!strcmp(cmd,"level")){
		if(argc<3)return -1;
		return demo_level(argv[2]);
	}
	printf("unknown: %s\n",cmd);
	return -1;
}
