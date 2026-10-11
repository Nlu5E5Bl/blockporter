#include "nbt.hpp"
/*nbtrw.cpp：底层库（libnbt.so）的 demo，用子命令跑：
 *	./nbtrw nbt   [file]							读NBT文件，打印整棵树
 *	./nbtrw nbtw  [file]							编码/深拷贝/增删改自测
 *	./nbtrw dat   [file]							level.dat → 树 → 打印
 *	./nbtrw mca   <dir> <rx> <rz> [cx] [cz] [w]		读区块（cx/cz 不给就是0），w=回写（生成.mca_test.mca）
 *	./nbtrw chunk <file> <x> <y> <z> [name]			读方块，给了name就写进去再编码-解析回来验证
 *	./nbtrw level <dir>								看level怎么认这个存档（DataVersion/布局/维度目录）
 *	（merge/fixer是各自独立的程序：merge.cpp和fixer.cpp，用法见各文件的文件头）
 *	默认路径：区块用 test/work/3.txt（跑一次 sh test/run.sh 会用到它；那是测试素材，仓库里不提交），
 *	          level.dat 用 test/data/level.dat（这是个样例存档，在仓库里）。
 *	这个demo只演示底层库：读NBT、编码、level.dat、区域文件、区块里取方块、存档目录
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
	printf("roundtrip: %s\n",root->cmp(back)?"ok":"FAIL");/*重新解析后逐值比较（成员顺序会变，所以要按值比）*/
	back->free();
	nbt_Any *copy=root->copy();
	printf("copy: %s\n",root->cmp(copy)?"ok":"FAIL");
	root->set_Long("nbtw_test",123);
	printf("set: %s\n",!root->cmp(copy)&&root->get("nbtw_test")&&root->get("nbtw_test")->as_Long()==123?"ok":"FAIL");
	root->erase("nbtw_test");
	nbt_Raw back2=root->to_Raw();
	printf("del: %s (size %zu, 编码字节%s)\n",root->cmp(copy)&&back2.data.size()==raw.data.size()?"ok":"FAIL",back2.data.size(),
		back2.data.size()==raw.data.size()&&!memcmp(back2.data.data(),raw.data.data(),raw.data.size())?"和原树逐位相同":"长度相同但顺序变了(unordered_map的rehash)");
	copy->free();
	root->free();
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
	const char *path=argc>0?argv[0]:".";
	int32_t rx=argc>1?atoi(argv[1]):0;
	int32_t rz=argc>2?atoi(argv[2]):0;
	uint32_t cx=argc>3?(uint32_t)atoi(argv[3]):0;
	uint32_t cz=argc>4?(uint32_t)atoi(argv[4]):0;
	mca_Region mca=mca_Region::read(path,rx,rz);
	if(mca.data.empty()){
		printf("no mca\n");
		return -1;
	}
	Chunk *chunk=mca.read_Chunk(cx,cz);
	if(!chunk){
		printf("no chunk\n");
		return -1;
	}
	chunk->print();
	putchar('\n');
	if(argc>5&&argv[5][0]=='w'){/*回写自测：编码→改进内存镜像→落盘成.mca_test.mca*/
		mca.write_Chunk(chunk);
		printf("write:%d size:%zu\n",mca.write_File(".mca_test.mca"),chunk->to_Raw().data.size());
	}
	chunk->free();
	return 0;
}

static int demo_chunk(int argc,char **argv){
	const char *path=argc>0?argv[0]:"test/work/3.txt";
	int x=argc>1?atoi(argv[1]):0;
	int y=argc>2?atoi(argv[2]):0;
	int z=argc>3?atoi(argv[3]):0;
	const char *name=argc>4?argv[4]:NULL;
	Chunk *chunk=Chunk::read(path);
	if(!chunk){
		printf("no chunk\n");
		return -1;
	}
	if(name){/*写回自测：造一个只有Name的方块塞进去，再编码-解析回来确认*/
		nbt_Compound *block=new nbt_Compound;
		block->set_String("Name",name);
		printf("set: %d\n",chunk->set_Block(Block{block,NULL},x,y,z));
		block->free();/*set_Block里是深拷贝，临时树可以扔了*/
		nbt_Raw raw=chunk->to_Raw();
		nbt_Raw again=raw;
		Chunk *chunk2=Chunk::read(&again);
		if(!chunk2){
			printf("reparse: 读不回来（编码出来的东西自己解析不了？）\n");
			chunk->free();
			return -1;
		}
		Block b2=chunk2->get_Block(x,y,z);
		printf("reparse %zu bytes: ",raw.data.size());
		if(b2.block)b2.block->print();
		else printf("null");
		printf("\n");
		chunk2->free();
	}
	Block b=chunk->get_Block(x,y,z);
	printf("block: ");
	if(b.block)b.block->print();
	else printf("null");
	printf("\n");
	if(b.block){
		printf("strid: %s (nspace=%.*s, basename=%.*s, properties=%s)\n",
			b.strid().c_str(),(int)b.nspace().size(),b.nspace().data(),
			(int)b.basename().size(),b.basename().data(),b.properties()?"有":"无");
	}
	printf("entity: ");
	if(b.entity)b.entity->print();
	else printf("null");
	printf("\n");
	chunk->free();
	return 0;
}

static int demo_level(const char *path){
	level lv(path,1,1);
	printf("path: %s\n",lv.path.c_str());
	printf("level.dat: %s DataVersion=%d Version.Name=%s\n",lv.dat_ok?"读到":"读不到",lv.data_version,lv.version_name.c_str());
	printf("布局: %s（DataVersion%s%d 就用 dimensions/minecraft/<dim>/region/，否则用老的 region/、DIM-1/、DIM1/）\n",
		lv.new_layout?"新":"老",lv.data_version>=DAT_NEW_LAYOUT?">=":"<",DAT_NEW_LAYOUT);
	for(int i=0;i<3;i++){
		const char *w=level::dim_Name(i);
		Chunk *c=lv.get_Chunk(0,0,w);/*顺手证明路径对不对：能读到(0,0)这个区块就说明目录没认错*/
		int32_t xPos=0,zPos=0;
		if(c){
			const nbt_Any *x=c->get("xPos"),*z=c->get("zPos");
			xPos=x?x->as_Int():0;
			zPos=z?z->as_Int():0;
		}
		printf("  %-20s %s  (0,0)的区块：%s",w,lv.region_dir[i].c_str(),c?"有":"没有");
		if(c){
			printf("，里面的xPos=%d zPos=%d%s",xPos,zPos,(xPos||zPos)?" ← 对不上说明认错文件了":"");
		}
		printf("\n");
	}
	lv.close();/*没改过东西，close只是把缓存放掉*/
	return lv.dat_ok?0:-1;
}

int main(int argc,char **argv){
	if(argc<2){
		printf("usage: %s {nbt|nbtw|dat|mca|chunk|level} ...\n",argv[0]);
		printf("  %s nbt   [file]                          读NBT文件打印整棵树\n",argv[0]);
		printf("  %s nbtw  [file]                          编码/深拷贝/增删改自测\n",argv[0]);
		printf("  %s dat   [file]                          level.dat → 树 → 打印\n",argv[0]);
		printf("  %s mca   <dir> <rx> <rz> [cx] [cz] [w]    读区块（cx/cz不给就是0），w=回写.mca_test.mca\n",argv[0]);
		printf("  %s chunk <file> <x> <y> <z> [name]       读方块，给了name就写进去\n",argv[0]);
		printf("  %s level <dir>                           看level怎么认这个存档\n",argv[0]);
		printf("  （区块默认用 test/work/3.txt，level.dat默认用 test/data/level.dat）\n");
		return -1;
	}
	const char *cmd=argv[1];
	if(!strcmp(cmd,"nbt")) return demo_nbt(argc>2?argv[2]:"test/work/3.txt");
	if(!strcmp(cmd,"nbtw")) return demo_nbtw(argc>2?argv[2]:"test/work/3.txt");
	if(!strcmp(cmd,"dat")) return demo_dat(argc>2?argv[2]:"test/data/level.dat");
	if(!strcmp(cmd,"mca")) return demo_mca(argc-2,argv+2);
	if(!strcmp(cmd,"chunk")) return demo_chunk(argc-2,argv+2);
	if(!strcmp(cmd,"level")) return demo_level(argc>2?argv[2]:".");
	printf("unknown: %s\n",cmd);
	return -1;
}
