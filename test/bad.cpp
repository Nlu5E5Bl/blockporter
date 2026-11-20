/*bad.cpp：坏数据/边界数据的“不出事”测试（截短、坏长度、坏tag、坏表头、坏形状、越界坐标、fd泄漏）
 *	存档是外部数据：可能被截短、被别的工具写坏、或者干脆不是NBT。libnbtrw 对这些只允许一种反应：
 *	用返回值说“读不出来/写不进去”（NULL、false、MCA::bad、MCA::last_error、Level::miss_why），
 *	不打日志、不计数、不 exit。段错误、堆越界、无限循环、进程被库自己退出，一律算失败。
 *	每个用例 fork 一个子进程跑，子进程的退出码：
 *	  0  = 通过；
 *	  97 = 用例自己发现返回值不对（CHECK 失败）；
 *	  86 = ASan/UBSan/LSan 报错（safe.sh 用 *SAN_OPTIONS=exitcode=86 编进来）；
 *	  信号/别的退出码 = 崩了，或者库自己 exit 了（库不许 exit）。
 *	用法：test/work/bad <工作目录>（每次清空重建；lib.sh 传 test/work/badwork）
 */
#include "../nbtrw.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

/*---- 新接口的测试小工具：Chunk::read/MCA::get_Chunk 没了，读成 compound 再转 ----*/
static Chunk *read_TestChunk(nbt_Raw *raw){
	nbt_Compound *c=raw?raw->read_Root():NULL;
	if(!c){
		return NULL;
	}
	Chunk *r=new Chunk(std::move(*c));
	c->free();
	return r;
}
static Chunk *get_MCAChunk(const MCA &m,uint32_t x,uint32_t z){/*从区域镜像里读一格、转成堆上的 Chunk（调用者 delete）；读不到给 NULL*/
	std::optional<nbt_Compound> r=m.get_Compound(x,z);
	return r?new Chunk(std::move(*r)):NULL;
}
static nbt_Compound *get_MCACompound(const MCA &m,uint32_t x,uint32_t z){/*同上，但留成堆上的 nbt_Compound（调用者 free）*/
	std::optional<nbt_Compound> r=m.get_Compound(x,z);
	return r?new nbt_Compound(std::move(*r)):NULL;
}
static int fail_cnt=0,case_cnt=0;
static std::string work;

#define CHECK(cond,msg) do{if(!(cond)){printf("     FAIL %s（%s:%d）\n",msg,__FILE__,__LINE__);fflush(stdout);exit(97);}}while(0)

/*---- 小工具 ----*/
struct Buf{/*手拼NBT字节：输入长什么样由测试说了算，不用被测代码去造*/
	std::vector<byte> d;
	void u8(uint8_t v){d.push_back(v);}
	void u16(uint16_t v){u8((byte)(v>>8));u8((byte)v);}
	void u32(uint32_t v){u8((byte)(v>>24));u8((byte)(v>>16));u8((byte)(v>>8));u8((byte)v);}
	void i32(int32_t v){u32((uint32_t)v);}
	void raw(const char *s){while(*s){u8((byte)*s++);}}
	void str(const char *s){u16((uint16_t)strlen(s));raw(s);}
	void name(const char *s,uint8_t tag){u8(tag);str(s);}
	void end(){u8(0);}
	void root(){u8(NBT_COMPOUND);u16(0);}/*根compound + 空名字*/
	void full(){name("Status",NBT_STRING);str("minecraft:full");}/*完整区块才会走到读写路径*/
};
static void save_File(const std::string &p,const std::vector<byte> &d){/*把一段字节原样写成文件；写不了直接算用例失败（exit 97）*/
	FILE *fp=fopen(p.c_str(),"wb");
	if(!fp){
		printf("bad: 写不了 %s\n",p.c_str());
		exit(97);
	}
	if(!d.empty()){
		fwrite(d.data(),1,d.size(),fp);
	}
	fclose(fp);
}
static void make_Dir(const std::string &p){/*mkdir -p*/
	for(size_t i=1;i<=p.size();i++){
		if(i==p.size()||p[i]=='/'){
			mkdir(p.substr(0,i).c_str(),0777);
		}
	}
}
static void remove_Tree(const std::string &p){/*rm -rf（每次跑之前清工作目录）*/
	DIR *dp=opendir(p.c_str());
	if(!dp){
		unlink(p.c_str());
		return;
	}
	struct dirent *e;
	while((e=readdir(dp))){
		if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")){
			continue;
		}
		remove_Tree(p+"/"+e->d_name);
	}
	closedir(dp);
	rmdir(p.c_str());
}
static int count_Fd(){/*当前进程打开的fd数（/proc/self/fd 里的条目，减掉opendir自己那个）*/
	DIR *dp=opendir("/proc/self/fd");
	if(!dp){
		return -1;
	}
	int n=0;
	struct dirent *e;
	while((e=readdir(dp))){
		if(e->d_name[0]!='.'){
			n++;
		}
	}
	closedir(dp);
	return n-1;
}
static std::vector<byte> make_LevelDat(int32_t dv,const char *ver){/*正常的level.dat（gzip的nbt）*/
	Buf b;
	b.root();
	b.name("Data",NBT_COMPOUND);
	b.name("DataVersion",NBT_INT);b.i32(dv);
	b.name("Version",NBT_COMPOUND);b.name("Name",NBT_STRING);b.str(ver);b.end();
	b.end();
	b.end();
	return gzip_Data(b.d.data(),b.d.size());
}

/*---- fork 一个子进程跑用例 ----*/
template<class F> static void run_Case(const char *what,F fn){
	case_cnt++;
	fflush(stdout);
	pid_t p=fork();
	if(p==0){
		alarm(300);/*转不出来的（死循环、照着坏长度空转）到点被 SIGALRM 杀掉，按“崩了”算；正常每例不到一秒*/
		fn();
		fflush(stdout);
		exit(0);/*用exit而不是_exit：ASan/LSan 的退出检查要在每个子进程里跑*/
	}
	int st=0;
	waitpid(p,&st,0);
	const char *verdict=NULL;
	char msg[128];
	if(WIFSIGNALED(st)){
		snprintf(msg,sizeof(msg),"崩了（信号 %d）",WTERMSIG(st));
		verdict=msg;
	}else if(WIFEXITED(st)&&WEXITSTATUS(st)==86){
		verdict="ASan/UBSan/LSan 报错";
	}else if(WIFEXITED(st)&&WEXITSTATUS(st)==97){
		verdict="返回值不对（见上一行）";
	}else if(WIFEXITED(st)&&WEXITSTATUS(st)!=0){
		snprintf(msg,sizeof(msg),"退出码 %d（库不许自己exit）",WEXITSTATUS(st));
		verdict=msg;
	}
	if(verdict){
		printf("  FAIL %-46s %s\n",what,verdict);
		fail_cnt++;
	}else{
		printf("  ok   %s\n",what);
	}
}

/*==================== 1. NBT字节流 ====================*/
static void read_Fail(const std::vector<byte> &d,uint8_t type){/*把一段坏字节当某个tag读：必须标fail，读出来的东西能free*/
	nbt_Raw raw(d);
	nbt_Any *a=raw.read_Any(type);
	CHECK(raw.fail,"坏字节流没标fail");
	if(a){
		a->free();
	}
}
static void test_NBTEmpty(){read_Fail(std::vector<byte>(),NBT_COMPOUND);}/*0 字节*/
static void test_NBTOneByte(){read_Fail(std::vector<byte>(1,NBT_COMPOUND),NBT_COMPOUND);}/*只有一个字节*/
static void test_NBTStringShort(){Buf b;b.u16(0xFFFF);read_Fail(b.d,NBT_STRING);}/*String 说自己 65535 字节，后面什么都没有*/
static void test_NBTByteArrayHuge(){Buf b;b.u32(0xFFFFFFFFu);read_Fail(b.d,NBT_BYTE_ARRAY);}/*Byte_Array 的长度比数据长得多：不能照着长度分配*/
static void test_NBTIntArrayHuge(){Buf b;b.u32(0x7FFFFFFFu);read_Fail(b.d,NBT_INT_ARRAY);}/*Int_Array 同上*/
static void test_NBTLongArrayHuge(){Buf b;b.u32(0x0FFFFFFFu);read_Fail(b.d,NBT_LONG_ARRAY);}/*Long_Array 同上*/
static void test_NBTListHuge(){Buf b;b.u8(NBT_COMPOUND);b.u32(0x7FFFFFFFu);read_Fail(b.d,NBT_LIST);}/*List 说自己有 20 多亿个 compound*/
static void test_NBTListEndHuge(){Buf b;b.u8(NBT_END);b.u32(0xFFFFFFF0u);read_Fail(b.d,NBT_LIST);}/*元素是End：0字节/个，最容易骗过长度检查*/
static void test_NBTListGreedy(){/*嵌套的 List，每层都说自己还有很多元素，其实读到第一层最里面数据就完了：
	必须读坏就停。以前读坏了还照着 len 一个个建空节点（每层各转 len 次），还按 len 预留内存，
	300KB 的坏数据要转几分钟、吃掉几十 GB。两种谎报法：骗不过长度检查的（剩下的字节的一半）和正好骗得过的（剩下的字节/5）*/
	for(int sly=0;sly<2;sly++){
		const size_t total=300000;
		Buf b;
		b.root();
		b.name("a",NBT_LIST);
		while(b.d.size()+5<=total){
			size_t left=total-b.d.size()-5;
			b.u8(NBT_LIST);
			b.u32((uint32_t)(sly?left/5:left/2));
		}
		nbt_Raw raw(b.d);
		nbt_Compound *c=raw.read_Root();
		CHECK(!c,"每层谎报长度的嵌套List读出了东西");
		CHECK(raw.fail,"每层谎报长度的嵌套List没标fail");
	}
}
static void test_NBTTagUnknown(){/*不存在的 tag 号：给 NULL、标 fail*/
	nbt_Raw raw(std::vector<byte>(8,0x41));
	CHECK(raw.read_Any(0x0d)==NULL,"不存在的tag读出了东西");
	CHECK(raw.fail,"不存在的tag没标fail");
}
static void test_NBTListElemUnknown(){Buf b;b.u8(0x77);b.u32(4);read_Fail(b.d,NBT_LIST);}/*List 的元素类型不认识*/
static void test_NBTNameShort(){Buf b;b.u8(NBT_STRING);b.u16(3);b.raw("ab");read_Fail(b.d,NBT_COMPOUND);}/*名字说3字节只给了2*/
static void test_NBTDeep(){/*深嵌套：库不设深度上限，200层要能读能编码能free*/
	Buf b;
	b.root();
	for(int i=0;i<200;i++){
		b.name("a",NBT_COMPOUND);
	}
	for(int i=0;i<=200;i++){
		b.end();
	}
	nbt_Raw raw(b.d);
	nbt_Compound *c=raw.read_Root();
	CHECK(c&&!raw.fail,"200层嵌套读不出来");
	nbt_Raw out=c->to_Raw();
	CHECK(!out.fail&&out.data==b.d,"200层嵌套编码回去不一样");
	c->free();
}
static void test_NBTRootEmpty(){/*空数据：根头和根 compound 都读不出来*/
	nbt_Raw raw;
	CHECK(!raw.read_RootHeader(),"空数据读出了根tag");
	nbt_Raw raw2;
	CHECK(raw2.read_Root()==NULL,"空数据读出了根compound");
}
static void test_NBTRootNotCompound(){/*根 tag 不是 compound：不认*/
	Buf b;b.u8(NBT_BYTE);b.u16(0);b.u8(1);
	nbt_Raw raw(b.d);
	CHECK(raw.read_Root()==NULL,"根tag是Byte也读出了compound");
}
static void test_NBTTruncatedRoot(){/*读到一半断了：整棵树作废*/
	Buf b;
	b.root();
	b.name("a",NBT_STRING);b.u16(100);b.raw("short");
	nbt_Raw raw(b.d);
	CHECK(raw.read_Root()==NULL,"截短的数据也读出了树");
	CHECK(raw.fail,"截短没标fail");
}
static void test_NBTMemberTagUnknown(){/*compound 里某个成员的 tag 不认识：整棵作废*/
	Buf b;
	b.root();
	b.name("a",0x77);
	b.end();
	nbt_Raw raw(b.d);
	CHECK(raw.read_Root()==NULL,"成员tag=0x77也读出了树");
}
static void test_NBTHugeLength(){/*Int_Array说20多亿个：不能照着长度resize*/
	Buf b;
	b.root();
	b.name("arr",NBT_INT_ARRAY);b.u32(0x7FFFFFFFu);
	b.end();
	nbt_Raw raw(b.d);
	CHECK(raw.read_Root()==NULL,"几十亿的长度也读出来了");
}
static void test_NBTListEndWithItems(){/*elem=End 却说有3个元素：MC 只会写空的 End list*/
	Buf b;
	b.root();
	b.name("L",NBT_LIST);b.u8(NBT_END);b.u32(3);
	b.end();
	nbt_Raw raw(b.d);
	CHECK(raw.read_Root()==NULL,"elem=End但有元素的list也读出来了");
}

/*==================== 2. 坏区块：成员类型不对 ====================
  都带 Status=full：不完整区块在库里直接短路，坏形状要在完整区块上才走得到读写路径。*/
static std::vector<byte> make_BadChunk(int which){
	Buf b;
	b.root();
	b.full();
	switch(which){
	case 1:/*sections 是 Int*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("sections",NBT_INT);b.i32(7);
		break;
	case 2:/*sections 是 Int 的 list*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("sections",NBT_LIST);b.u8(NBT_INT);b.u32(2);b.i32(1);b.i32(2);
		break;
	case 3:/*Y 是 String、block_states 是 Int*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("sections",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(1);
		b.name("Y",NBT_STRING);b.str("不是数字");
		b.name("block_states",NBT_INT);b.i32(3);
		b.end();
		break;
	case 4:/*palette 元素是 Int，data 比该有的短得多*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("sections",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(1);
		b.name("Y",NBT_BYTE);b.u8(0);
		b.name("block_states",NBT_COMPOUND);
		b.name("palette",NBT_LIST);b.u8(NBT_INT);b.u32(3);b.i32(11);b.i32(22);b.i32(33);
		b.name("data",NBT_LONG_ARRAY);b.u32(1);b.u32(0x12345678u);b.u32(0x9ABCDEF0u);
		b.end();
		b.end();
		break;
	case 5:/*block_entities 是 Int，xPos/zPos 是 String*/
		b.name("xPos",NBT_STRING);b.str("0");b.name("zPos",NBT_STRING);b.str("0");
		b.name("block_entities",NBT_INT);b.i32(1);
		b.name("sections",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(0);
		break;
	case 6:/*方块实体的 x 是 String、还有一条没坐标；section 正常*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("block_entities",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(2);
		b.name("id",NBT_STRING);b.str("minecraft:chest");b.name("x",NBT_STRING);b.str("3");
		b.name("y",NBT_INT);b.i32(0);b.name("z",NBT_INT);b.i32(3);
		b.end();
		b.name("id",NBT_STRING);b.str("minecraft:sign");
		b.end();
		b.name("sections",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(1);
		b.name("Y",NBT_BYTE);b.u8(0);
		b.name("block_states",NBT_COMPOUND);
		b.name("palette",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(1);
		b.name("Name",NBT_STRING);b.str("minecraft:stone");
		b.end();
		b.end();
		b.end();
		break;
	case 7:/*300 条调色板 + 下标乱填的 data（下标可能越出调色板）*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("sections",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(1);
		b.name("Y",NBT_BYTE);b.u8(0);
		b.name("block_states",NBT_COMPOUND);
		b.name("palette",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(300);
		for(int i=0;i<300;i++){
			b.name("Name",NBT_STRING);b.str(i%2?"minecraft:stone":"minecraft:air");
			b.end();
		}
		b.name("data",NBT_LONG_ARRAY);b.u32(600);
		for(int i=0;i<600;i++){
			b.u32(0xFFFFFFFFu);b.u32(0xFFFFFFFFu);/*每个9位下标都是511，越出300*/
		}
		b.end();
		b.end();
		break;
	case 8:/*list 套 list；刻表是 String；sections 缺失*/
		b.name("xPos",NBT_INT);b.i32(0);b.name("zPos",NBT_INT);b.i32(0);
		b.name("L",NBT_LIST);b.u8(NBT_LIST);b.u32(1);
		b.u8(NBT_LIST);b.u32(0);
		b.name("block_ticks",NBT_STRING);b.str("不是表");
		break;
	}
	b.end();
	return b.d;
}
static void exercise_Chunk(Chunk *c){/*把区块级读写接口全走一遍：只许返回失败，不许崩*/
	for(int y=-64;y<320;y+=17){
		for(int x=0;x<16;x+=5){
			Block b=c->get_Block(x,y,(x+1)&15);
			Block b2=c->get_Block(x,y,(x+1)&15);
			Block_Ref r;
			c->get_BlockRef(x,y,(x+1)&15,&r);
			c->get_BlockRef(x,y,(x+1)&15,&r);
			equal_BlockRef(r,b);
			(void)b2;
		}
	}
	for(int sy=-5;sy<21;sy++){
		c->get_Section(sy);
		c->get_Section(sy);
		const nbt_Any *state=NULL;
		is_SectionConst(c,sy,&state);
		for(int i=0;i<16;i+=5){
			get_BlockData(c,sy,i,i,15-i);
		}
	}
	Block air("minecraft:air");
	Block chest("minecraft:chest");
	chest.block_entities.set_String("id","minecraft:chest");
	c->set_Block(air,3,0,3);
	c->set_Block(chest,3,0,3);
	c->set_Block(air,3,99999,3);
	c->set_Block(air,99,0,3);
	c->set_Block(Block(),3,0,3);
	c->set_BlockEntity(chest.block_entities,3,0,3);
	c->get_BlockEntity(3,0,3);
	c->erase_BlockEntity(3,0,3);
	check_Chunk(&c->con);
	check_ChunkBase(&c->con);
	has_Block(&c->con);
	for(bool fluid:{false,true}){
		int n=count_ChunkTicks(&c->con,fluid);
		Chunk_Tick t;
		for(int i=-1;i<=n+1;i++){
			get_ChunkTick(&c->con,fluid,i,&t);
		}
		del_ChunkTick(&c->con,fluid,0,0,0);
		add_ChunkTick(&c->con,fluid,"minecraft:water",0,0,0,5,0);
		filter_ChunkTick(&c->con,fluid,[](void*,const Chunk_Tick*){return false;},NULL);
	}
	int bad_y=0,pal=0;
	c->format_Chunk(&bad_y,&pal);
	c->get_Block(0,0,0);/*format改过结构：旧游标要自己重查*/
	nbt_Raw out=c->con.to_Raw();
	CHECK(!out.fail,"坏区块编码失败");
	nbt_Raw again=out;
	Chunk *back=read_TestChunk(&again);
	CHECK(back,"自己编码出来的区块读不回来");
	CHECK(back->con==c->con,"编码再解码后不一样");
	delete back;
	nbt_Any *copy=c->con.copy();
	CHECK(copy&&*copy==c->con,"深拷贝不一样");
	copy->free();
}
static void test_ChunkBad(int which){/*NBT 合法但形状不对的区块（which 选哪一种，见 make_BadChunk）：读写接口都只许用返回值说不行*/
	std::vector<byte> d=make_BadChunk(which);
	nbt_Raw raw(d);
	Chunk *c=read_TestChunk(&raw);
	CHECK(c,"坏形状但NBT合法的区块应当能读出来");
	CHECK(c->is_Full(),"Status=full没认出来");
	/*游戏整块载入失败的只有调色板/data 解不出来（4、7）；sections/block_states/block_entities 类型不对游戏静默当没有，
	  刻表不是 List 游戏 ERROR 但区块照载（fixer 删这个成员）——这些 check_ChunkBase 放行，严格检查（merge）照样拦*/
	bool base_bad=which==4||which==7;
	bool strict_bad=(which>=1&&which<=5)||which==7||which==8;
	CHECK(base_bad==!check_ChunkBase(&c->con),"check_ChunkBase 和游戏“整块载入失败”对不上");
	CHECK(!(strict_bad&&check_Chunk(&c->con)),"严格检查放行了坏区块");
	if(which==1||which==2){
		CHECK(!c->get_Block(0,0,0).valid(),"sections坏了还读出了方块（会被当空气覆盖）");
		Block_Ref r;
		CHECK(!c->get_BlockRef(0,0,0,&r),"sections坏了get_BlockRef还说读到了");
	}
	if(which==6){
		Block b=c->get_Block(3,0,3);
		CHECK(b.valid()&&b.name()=="minecraft:stone"&&!b.has_BlockEntity(),"x是String的方块实体不该配上这一格");
	}
	exercise_Chunk(c);
	delete c;
}

/*==================== 3. 区域文件 ====================*/
static std::vector<byte> make_Header(){return std::vector<byte>(8192,0);}/*全 0 的 8KB 表头（空区域）*/
static void set_Header(std::vector<byte> &d,uint32_t x,uint32_t z,uint32_t off,uint32_t cnt){/*改表头 (x,z) 这一格：3 字节偏移（第几个扇区）+ 1 字节扇区数*/
	byte *p=d.data()+4*(x+z*32);
	p[0]=(byte)(off>>16);p[1]=(byte)(off>>8);p[2]=(byte)off;p[3]=(byte)cnt;
}
static std::vector<byte> make_Slot(uint32_t len,byte type,byte fill){/*槽(0,0)：偏移1、1个扇区，长度/类型/内容自定*/
	std::vector<byte> d=make_Header();
	set_Header(d,0,0,2,1);
	d.resize(8192+4096,fill);
	d[8192]=(byte)(len>>24);d[8193]=(byte)(len>>16);d[8194]=(byte)(len>>8);d[8195]=(byte)len;d[8196]=type;
	return d;
}
static void read_Slot(const std::vector<byte> &d,int want){/*存成文件、读槽(0,0)：读不出来，原因要对，镜像不能被标坏*/
	std::string f=work+"/slot.mca";
	save_File(f,d);
	MCA m(f);
	CHECK(!m.bad,"文件能读，不该整个标bad");
	CHECK(m.has_Chunk(0,0),"表头有这一格");
	Chunk *c=get_MCAChunk(m,0,0);
	CHECK(!c,"坏槽读出了区块");
	CHECK(m.last_error==want,"last_error 不对");
	nbt_Compound *e=get_MCACompound(m,0,0);
	CHECK(!e,"坏槽读出了compound");
	CHECK(m.last_error==want,"get_Compound 的 last_error 不对");
	CHECK(!get_MCAChunk(m,31,31)&&m.last_error==MCA_READ_NO_CHUNK,"空槽要是NO_CHUNK");
	std::vector<byte> before=m.data;
	bool ok=m.format();/*坏数据：format要么整理成功要么一字不改*/
	CHECK(ok||m.data==before,"format失败却改了镜像");
	CHECK(!get_MCAChunk(m,0,0),"format以后坏槽变成能读了");
}
static void test_MCATiny(){/*不到 8KB 表头的文件：照游戏补 0 照读，偏移指到文件外的格子照游戏当没有，format 清成 0*/
	save_File(work+"/r.0.0.mca",std::vector<byte>(100,0x41));
	MCA m(work+"/r.0.0.mca");
	CHECK(!m.bad&&m.truncated&&m.data.size()==8192,"100字节的文件：照游戏补0照读（truncated）");
	CHECK(!get_MCAChunk(m,0,0)&&m.last_error==MCA_READ_NO_CHUNK,"偏移指到文件外的格子要当没有");
	CHECK(!m.has_Chunk(0,0)&&m.has_BadOffset(0,0)&&m.is_Empty(),"偏移异常的格子：has_Chunk 假、has_BadOffset 真、整个文件算空");
	{
		MCA f2(work+"/r.0.0.mca");
		CHECK(f2.format()&&f2.data==std::vector<byte>(8192,0),"format 要把偏移异常的格子清成 0");
	}
	CHECK(m.save((work+"/r.0.0.mca").c_str()),"补全的表头能落盘");
	Chunk c(0,0);
	CHECK(m.set_Compound(c.con,0,0),"补全后能往里写区块（覆盖那一格坏的）");
	Chunk *back=get_MCAChunk(m,0,0);
	CHECK(back,"写进去的区块读得回来");
	delete back;
}
static void read_BadOffset(const std::vector<byte> &d){/*存成文件、读槽(0,0)：偏移异常的格子照游戏当没有，format 清成 0*/
	std::string f=work+"/slot.mca";
	save_File(f,d);
	MCA m(f);
	CHECK(!m.bad,"文件能读，不该整个标bad");
	CHECK(!m.has_Chunk(0,0)&&m.has_BadOffset(0,0),"偏移异常的格子：has_Chunk 假、has_BadOffset 真");
	CHECK(!m.get_Compound(0,0)&&m.last_error==MCA_READ_NO_CHUNK,"偏移异常的格子读出来要是 NO_CHUNK");
	CHECK(m.format()&&!m.has_BadOffset(0,0)&&m.is_Empty(),"format 要把它清成 0");
}
static void test_MCAOffsetOut(){/*表头偏移指到文件外*/
	std::vector<byte> d=make_Header();
	set_Header(d,0,0,0xFFFFFF,0xFF);
	read_BadOffset(d);
}
static void test_MCALengthOver(){read_Slot(make_Slot(0x7FFFFFFFu,0x02,0),MCA_READ_BAD);}/*长度字段比扇区大得多*/
static void test_MCALengthZero(){read_Slot(make_Slot(0,0x02,0),MCA_READ_BAD);}/*长度字段是 0（游戏 WARN）*/
static void test_MCALengthOne(){read_Slot(make_Slot(1,0x02,0),MCA_READ_BAD);}/*只有类型字节，压缩数据0字节*/
static void test_MCALengthSector(){/*长度比表头给的扇区数还长：不许跨读下一个区块*/
	std::vector<byte> d=make_Slot(5000,0x02,0x5A);
	d.resize(8192+4096*3,0x5A);
	read_Slot(d,MCA_READ_BAD);
}
static void test_MCAZlibGarbage(){read_Slot(make_Slot(0x20,0x02,0x5A),MCA_READ_BAD);}/*说是 zlib，内容是乱字节*/
static void test_MCAGzipGarbage(){read_Slot(make_Slot(0x20,0x01,0x5A),MCA_READ_BAD);}/*说是 gzip，内容是乱字节*/
static void test_MCARawGarbage(){read_Slot(make_Slot(0x20,0x03,0x41),MCA_READ_BAD_NBT);}/*说是不压缩，内容不是 NBT：压缩没坏，是 NBT 坏*/
static void test_MCALz4(){read_Slot(make_Slot(0x20,0x04,0x41),MCA_READ_BAD);}/*LZ4 能读了：乱数据解不出来＝坏块*/
static void test_MCAExternal(){read_Slot(make_Slot(1,0x82,0),MCA_READ_BAD);}/*外置 .mcc 不在（游戏 ERROR）：坏块*/
static void test_MCATypeUnknown(){read_Slot(make_Slot(0x20,0x77,0x41),MCA_READ_BAD);}/*不认识的类型号（游戏 ERROR）：坏块*/
static void test_MCACustom(){read_Slot(make_Slot(0x20,0x7F,0x41),MCA_READ_UNSUPPORTED_COMPRESSION);}/*127 自定义（模组的）：读不了，但不算坏块*/
static void test_MCASectorZero(){/*偏移在、扇区数是0*/
	std::vector<byte> d=make_Slot(0x20,0x02,0x5A);
	d[3]=0;
	read_BadOffset(d);
}
static void test_MCADirInstead(){/*名字撞上目录：文件“在但读不出来”，不能当空区域*/
	std::string f=work+"/r.1.1.mca";
	make_Dir(f);
	MCA m(f);
	CHECK(m.bad,"目录冒充区域文件没标bad");
	CHECK(!m.save(),"坏镜像不许落盘");
	rmdir(f.c_str());
}
static void test_MCAFileStates(){/*不存在＝空镜像；0字节＝空区域；半个表头＝补0照读（truncated）；只有8KB表头＝空区域*/
	MCA none(work+"/r.30.30.mca");
	CHECK(!none.bad&&none.data.empty(),"不存在的文件要是空镜像");
	CHECK(none.is_Empty()&&!none.has_Chunk(0,0),"空镜像is_Empty/has_Chunk不对");
	CHECK(!get_MCAChunk(none,0,0)&&none.last_error==MCA_READ_NO_CHUNK,"文件不在＝这片没有区块，要给NO_CHUNK（不是读不出来）");
	CHECK(!none.get_Compound(0,0)&&none.last_error==MCA_READ_NO_CHUNK,"文件不在时get_Compound也要给NO_CHUNK");
	save_File(work+"/r.9.9.mca",std::vector<byte>());
	MCA zero(work+"/r.9.9.mca");
	CHECK(!zero.bad&&!zero.truncated&&zero.is_Empty(),"0字节的文件：游戏当空区域（没有 WARN）");
	save_File(work+"/r.9.8.mca",std::vector<byte>(4096,0));
	MCA half(work+"/r.9.8.mca");
	CHECK(!half.bad&&half.truncated&&half.is_Empty(),"半个表头：游戏 WARN 照读，补0");
	save_File(work+"/r.9.7.mca",make_Header());
	MCA empty(work+"/r.9.7.mca");
	CHECK(!empty.bad&&empty.is_Empty(),"只有8KB表头的要算空区域");
	CHECK(!empty.erase_Chunk(5,5),"空区域删区块要说没删");
	CHECK(!get_MCAChunk(empty,5,5)&&empty.last_error==MCA_READ_NO_CHUNK,"空槽要给NO_CHUNK");
	CHECK(!get_MCAChunk(empty,32,0)&&!empty.has_Chunk(32,0)&&!empty.erase_Chunk(32,0),"槽号越界");
	nbt_Compound poi;
	poi.set_Int("DataVersion",4788);
	CHECK(set_POI(&poi,7,2,8,POI("minecraft:meeting",32)),"set_POI失败");
	CHECK(empty.set_Compound(poi,5,5),"往空区域写POI区块失败");
	CHECK(!empty.set_Compound(poi,32,5),"槽号越界也写进去了");
	nbt_Compound *back=get_MCACompound(empty,5,5);
	CHECK(back&&count_POI(back)==1&&get_POI(back,7,2,8),"POI区块往返不对");
	back->free();
}
static void test_MCAWriteBadHeader(){/*表头指向文件外：写区块要追加，不许按表头原地memset*/
	std::vector<byte> d=make_Header();
	set_Header(d,0,0,0xFFFFFF,0xFF);
	save_File(work+"/r.2.2.mca",d);
	MCA m(work+"/r.2.2.mca");
	CHECK(!m.bad,"表头坏但文件能读");
	Chunk c(0,0);
	c.con.set_String("Status","minecraft:full");
	CHECK(m.set_Compound(c.con,0,0),"写不进去");
	size_t off=((size_t)m.data[0]<<16|(size_t)m.data[1]<<8|m.data[2])*4096;
	size_t cnt=m.data[3];
	CHECK(cnt&&off+cnt*4096<=m.data.size(),"写完表头还指向文件外");
	Chunk *back=get_MCAChunk(m,0,0);
	CHECK(back&&back->con==c.con,"刚写的区块读不回来");
	delete back;
}
static void test_MCAWriteHeaderSector(){/*表头偏移指在表头自己里面（第1个扇区＝时间戳表）、一个扇区正好放得下：
	写区块不许“原地写”——那会把时间戳表当数据区覆盖，写完偏移还是 1，游戏照样不认这一格*/
	std::vector<byte> d=make_Header();
	set_Header(d,0,0,1,1);
	set_Header(d,5,5,2,1);/*旁边一格正常的，带时间戳*/
	d[4096+4*(5+5*32)+3]=0x77;
	d.resize(8192+3*4096,0);
	save_File(work+"/r.6.6.mca",d);
	MCA m(work+"/r.6.6.mca");
	CHECK(!m.bad,"表头坏但文件能读");
	CHECK(!m.get_Compound(0,0)&&m.last_error==MCA_READ_NO_CHUNK&&m.has_BadOffset(0,0),"偏移在表头里的格子照游戏当没有");
	nbt_Compound c;
	c.set_String("Status","minecraft:full");
	CHECK(m.set_Compound(c,0,0),"写不进去");
	size_t off=(size_t)m.data[0]<<16|(size_t)m.data[1]<<8|m.data[2];
	CHECK(off>=2&&(off+m.data[3])*4096<=m.data.size(),"写完偏移还指在表头里");
	CHECK(m.data[4096+4*(5+5*32)+3]==0x77,"别的格子的时间戳被当成数据区覆盖了");
	std::optional<nbt_Compound> back=m.get_Compound(0,0);
	CHECK(back&&*back==c,"刚写的区块读不回来");
}
static void test_MCAMisaligned(){/*文件长度不是4096的倍数：追加写要对齐扇区*/
	MCA base;
	nbt_Compound c0;
	c0.set_String("Name","minecraft:overworld");
	CHECK(base.set_Compound(c0,0,0),"铺底数据写不进去");
	base.data.resize(base.data.size()+100,0x5A);
	CHECK(base.save((work+"/r.3.3.mca").c_str()),"落盘失败");
	MCA m(work+"/r.3.3.mca");
	CHECK(!m.bad&&m.data.size()%4096!=0,"铺底文件不对");
	CHECK(m.set_Compound(c0,5,5),"非对齐文件里写区块失败");
	size_t loc=4*(5+5*32);
	size_t off=((size_t)m.data[loc]<<16|(size_t)m.data[loc+1]<<8|m.data[loc+2])*4096;
	size_t cnt=m.data[loc+3];
	CHECK(cnt&&off+cnt*4096<=m.data.size(),"表头越界");
	nbt_Compound *a=get_MCACompound(m,5,5),*b=get_MCACompound(m,0,0);
	CHECK(a&&b&&*a==c0&&*b==c0,"写完以后新旧区块读不回来");
	a->free();
	b->free();
	CHECK(m.format(),"format失败");
	CHECK(m.data.size()%4096==0,"format以后没对齐");
	a=get_MCACompound(m,5,5);
	CHECK(a&&*a==c0,"format以后读不回来");
	a->free();
}
static void test_MCAOverlap(){/*两格表头指同一段扇区：写其中一格不许把另一格擦掉*/
	MCA m;
	nbt_Compound c0;
	c0.set_String("Name","x");
	CHECK(m.set_Compound(c0,0,0),"写不进去");
	m.data[4]=m.data[0];m.data[5]=m.data[1];m.data[6]=m.data[2];m.data[7]=m.data[3];/*槽(1,0)也指过去*/
	nbt_Compound c1;
	c1.set_String("Name","y");
	CHECK(m.set_Compound(c1,1,0),"写不进去");
	nbt_Compound *a=get_MCACompound(m,0,0),*b=get_MCACompound(m,1,0);
	CHECK(a&&b&&*a==c0&&*b==c1,"重叠扇区写回把邻居擦了");
	a->free();
	b->free();
	m.data[4]=m.data[0];m.data[5]=m.data[1];m.data[6]=m.data[2];m.data[7]=m.data[3];/*再指成重叠*/
	std::vector<byte> before=m.data;
	bool ok=m.format();/*重叠表头：整理成功就得两格都能读，失败就一字不改*/
	CHECK(ok||m.data==before,"format失败却改了镜像");
	nbt_Compound *x=get_MCACompound(m,0,0),*y=get_MCACompound(m,1,0);
	CHECK(x&&y&&*x==c0&&*y==c0,"format后重叠的两格读不对");
	x->free();
	y->free();
}

/*==================== 4. 存档目录 ====================*/
static void make_World(const std::string &w,const std::vector<byte> &dat,bool bad_region){
	make_Dir(w+"/dimensions/minecraft/overworld/region");
	if(!dat.empty()){
		save_File(w+"/level.dat",dat);
	}
	if(bad_region){
		std::vector<byte> d=make_Header();
		set_Header(d,0,0,0xFFFFFF,0xFF);
		save_File(w+"/dimensions/minecraft/overworld/region/r.0.0.mca",d);
	}
}
static void exercise_Level(const std::string &w){/*坏存档上把Level的接口都调一遍，最后重复close*/
	Level lv(w.c_str(),4,2,2);
	static const char *dims[]={"overworld","minecraft:the_nether","the_end","DIM-1","dimensions/minecraft/custom",
		"","/abs","../up","a//b","a/","./x","minecraft:","Overworld",NULL};
	for(int i=0;i<14;i++){
		lv.get_Chunk(dims[i],0,0);
		lv.get_Chunk(dims[i],INT32_MIN,INT32_MAX);
		lv.get_MCA(dims[i],-1,-1);
		lv.get_Block(dims[i],INT32_MIN,INT32_MIN,INT32_MAX);
		lv.set_Block(dims[i],0,0,0,Block("minecraft:stone"));
		lv.set_Block(dims[i],INT32_MAX,INT32_MAX,INT32_MAX,Block());
		lv.erase_Chunk(dims[i],0,0);
		lv.format_MCA(dims[i],0,0);
		lv.get_DimensionPath(dims[i],"region");
		lv.get_DimensionPath(dims[i],NULL);
		lv.unpin_Chunk(dims[i],0,0);
	}
	CHECK(!lv.get_Dat("../level.dat")&&lv.miss_why==Level::GET_ARG,"dat路径跑出存档目录没拦住");
	CHECK(!lv.get_Dat("/etc/passwd")&&lv.miss_why==Level::GET_ARG,"dat绝对路径没拦住");
	lv.get_Dat("level.dat");
	lv.save();
	lv.close();
	lv.close();
}
static void test_LevelNoDat(){/*没有 level.dat：dat_ok=false，按老布局算，别的接口照样不崩*/
	make_World(work+"/w1",std::vector<byte>(),false);
	Level lv((work+"/w1").c_str());
	CHECK(!lv.dat_ok&&lv.data_version==0&&!lv.new_layout,"没有level.dat要报dat_ok=false");
	CHECK(!lv.set_Block("overworld",0,0,0,Block("minecraft:stone")),"没有区块也写进去了");
	exercise_Level(work+"/w1");
}
static void test_LevelDatGarbage(){/*level.dat 是一串乱字节*/
	make_World(work+"/w2",std::vector<byte>(64,0x41),true);
	Level lv((work+"/w2").c_str());
	CHECK(!lv.dat_ok,"垃圾level.dat也读出DataVersion");
	exercise_Level(work+"/w2");
}
static void test_LevelDatHalfGzip(){/*level.dat 是截掉一半的 gzip*/
	std::vector<byte> g=make_LevelDat(4788,"26.1.1");
	g.resize(g.size()/2);
	make_World(work+"/w3",g,true);
	Level lv((work+"/w3").c_str());
	CHECK(!lv.dat_ok,"半个gzip也读出DataVersion");
	exercise_Level(work+"/w3");
}
static void test_LevelDatNotNbt(){/*level.dat 是 gzip，但里面不是 NBT*/
	std::vector<byte> junk(128,0x37);
	std::vector<byte> g=gzip_Data(junk.data(),junk.size());
	make_World(work+"/w4",g,true);
	Level lv((work+"/w4").c_str());
	CHECK(!lv.dat_ok,"gzip里不是nbt也读出DataVersion");
	exercise_Level(work+"/w4");
}
static void test_MCAHeaderCountOnly(){/*表头一格偏移是0、扇区数不是0：游戏打开文件时 WARN、把这一格当 0（没有这个区块）。
	库照游戏：has_Chunk 假、读出来是 NO_CHUNK、整个文件算空；clean_Offsets()/format()/erase_Chunk 都清得掉*/
	std::vector<byte> d=make_Header();
	set_Header(d,3,4,0,5);
	set_Header(d,5,6,9,0);/*扇区数是 0*/
	set_Header(d,7,8,1,1);/*偏移指在表头里*/
	save_File(work+"/r.7.7.mca",d);
	MCA m(work+"/r.7.7.mca");
	CHECK(!m.bad,"表头坏但文件能读");
	CHECK(!m.has_Chunk(3,4)&&m.has_BadOffset(3,4)&&m.has_BadOffset(5,6)&&m.has_BadOffset(7,8)&&m.is_Empty(),"三种偏移异常的格子都要当没有");
	CHECK(!m.get_Compound(3,4)&&m.last_error==MCA_READ_NO_CHUNK,"这种格子读出来要是 NO_CHUNK");
	CHECK(m.erase_Chunk(3,4)&&!m.has_BadOffset(3,4),"erase_Chunk 要清得掉偏移异常的格子");
	CHECK(m.clean_Offsets()==2&&!m.has_BadOffset(5,6)&&!m.has_BadOffset(7,8),"clean_Offsets 要清掉剩下两格");
	CHECK(m.format()&&m.data==std::vector<byte>(8192,0),"清完是一张空表头");
	MCA f(work+"/r.7.7.mca");
	nbt_Compound c;
	c.set_String("Status","minecraft:full");
	CHECK(f.set_Compound(c,1,1)&&f.format(),"带着偏移异常的格子 format 要成功");
	CHECK(!f.has_BadOffset(3,4)&&!f.has_BadOffset(5,6)&&!f.has_BadOffset(7,8)&&f.has_Chunk(1,1)&&f.get_Compound(1,1),"format 清掉异常格子、好的那格还在");
	/*走 Level：照游戏当没有（fixer 遍历时跳过，收尾整理时清掉）*/
	make_World(work+"/w7",make_LevelDat(4788,"26.1.1"),false);
	save_File(work+"/w7/dimensions/minecraft/overworld/region/r.0.0.mca",d);
	Level lv((work+"/w7").c_str());
	CHECK(!lv.get_Chunk("overworld",3,4)&&lv.miss_why==Level::GET_NO_CHUNK,"Level 要报GET_NO_CHUNK");
	CHECK(lv.format_MCA("overworld",0,0)&&lv.close(),"Level::format_MCA 要成功");
	MCA after(work+"/w7/dimensions/minecraft/overworld/region/r.0.0.mca");
	CHECK(!after.bad&&after.data==std::vector<byte>(8192,0),"落盘以后表头没清干净");
}
static void test_MCAFormatBadData(){/*表头正常、数据坏的格子（长度字段是 0）：format 不失败，原样搬它占的扇区，别的格子照常压紧*/
	std::vector<byte> d=make_Header();
	set_Header(d,0,0,5,2);/*第 5、6 两个扇区，里面全是 0（长度 0：游戏 WARN、当没有）*/
	d.resize(8192+6*4096,0);
	d[5*4096+4096+7]=0x5a;/*第二个扇区里做个记号*/
	save_File(work+"/r.8.8.mca",d);
	MCA m(work+"/r.8.8.mca");
	nbt_Compound c;
	c.set_String("Status","minecraft:full");
	CHECK(m.set_Compound(c,1,0),"写不进去");
	CHECK(m.has_Chunk(0,0)&&!m.get_Compound(0,0)&&m.last_error==MCA_READ_BAD,"数据坏的格子：有、读出来是坏块");
	CHECK(m.format(),"数据坏的格子不该让 format 失败");
	CHECK(m.has_Chunk(0,0)&&!m.get_Compound(0,0)&&m.last_error==MCA_READ_BAD,"format 以后还是坏块（删不删由调用者定）");
	CHECK(m.data[0]==0&&m.data[1]==0&&m.data[2]==2&&m.data[3]==2&&m.data[2*4096+4096+7]==0x5a,"坏格子占的两个扇区原样搬到最前面");
	std::optional<nbt_Compound> r=m.get_Compound(1,0);
	CHECK(r&&*r==c&&m.data[6]==4,"好的那格压紧在后面、读得回来");
}
static void test_LevelBadRegion(){/*level.dat正常、区域表头那一格偏移指到文件外：照游戏当没有这个区块（GET_NO_CHUNK）*/
	make_World(work+"/w5",make_LevelDat(4788,"26.1.1"),true);
	Level lv((work+"/w5").c_str());
	CHECK(lv.dat_ok&&lv.new_layout,"level.dat没读对");
	CHECK(!lv.get_Chunk("overworld",0,0)&&lv.miss_why==Level::GET_NO_CHUNK,"偏移异常的格子要报GET_NO_CHUNK");
	CHECK(!lv.get_Block("overworld",0,0,0).valid(),"坏区块读出了方块");
	CHECK(!lv.set_Block("overworld",0,0,0,Block("minecraft:stone")),"读不出来的区块也写进去了");
	exercise_Level(work+"/w5");
}
static void test_LevelOldLayout(){/*老版本的 DataVersion：三个维度走 region/、DIM-1/、DIM1/*/
	make_World(work+"/w6",make_LevelDat(3465,"1.20.1"),false);
	Level lv((work+"/w6").c_str());
	CHECK(lv.dat_ok&&!lv.new_layout,"老DataVersion要走老布局");
	CHECK(lv.get_DimensionPath("overworld","region")=="region","老布局主世界是region/");
	CHECK(lv.get_DimensionPath("the_nether","poi")=="DIM-1/poi","老布局下界是DIM-1/");
	exercise_Level(work+"/w6");
}
static void test_LevelBadAndGood(){/*一个区域里坏区块(0,0)和好区块(1,1)：坏的读不出、好的照读*/
	std::string w=work+"/w7";
	make_World(w,make_LevelDat(4788,"26.1.1"),false);
	std::string f=w+"/dimensions/minecraft/overworld/region/r.0.0.mca";
	std::vector<byte> d=make_Slot(0x20,0x03,0x41);
	save_File(f,d);
	MCA m(f);
	Chunk good(1,1);
	good.con.set_String("Status","minecraft:full");
	CHECK(m.set_Compound(good.con,1,1)&&m.save(),"好区块写不进去");
	Level lv(w.c_str(),4,2,2);
	CHECK(!lv.get_Chunk("overworld",0,0)&&lv.miss_why==Level::GET_BAD_CHUNK,"坏区块没报GET_BAD_CHUNK");
	CHECK(lv.get_Chunk("overworld",1,1),"好区块被坏区块连累");
	CHECK(!lv.get_Chunk("overworld",2,2)&&lv.miss_why==Level::GET_NO_CHUNK,"没有的区块要报GET_NO_CHUNK");
	CHECK(!lv.get_Chunk("overworld",100,100)&&lv.miss_why==Level::GET_NO_FILE,"没有的区域要报GET_NO_FILE");
	CHECK(lv.close(),"只读过的存档close失败");
}
static void test_LevelCustomChunk(){/*127 自定义压缩：报GET_UNSUPPORTED_COMPRESSION，erase以外不动它*/
	std::string w=work+"/w8";
	make_World(w,make_LevelDat(4788,"26.1.1"),false);
	save_File(w+"/dimensions/minecraft/overworld/region/r.0.0.mca",make_Slot(0x20,0x7F,0x41));
	Level lv(w.c_str());
	CHECK(!lv.get_Chunk("overworld",0,0)&&lv.miss_why==Level::GET_UNSUPPORTED_COMPRESSION,"自定义压缩要报不支持的压缩");
	CHECK(!lv.set_Block("overworld",0,0,0,Block("minecraft:stone")),"读不出来的区块也写进去了");
	CHECK(lv.close(),"close失败");
	MCA after(w+"/dimensions/minecraft/overworld/region/r.0.0.mca");
	CHECK(after.data==make_Slot(0x20,0x7F,0x41),"只读一遍就把不认识的区块改了");
}
static void test_LevelPinAll(){/*缓存全被钉住：取新区块要失败（GET_CACHE），不能把别人手里的指针挤掉*/
	std::string w=work+"/w9";
	make_World(w,make_LevelDat(4788,"26.1.1"),false);
	MCA m;
	for(int i=0;i<3;i++){
		Chunk c(i,0);
		c.con.set_String("Status","minecraft:full");
		CHECK(m.set_Compound(c.con,(uint32_t)i,0),"写不进去");
	}
	CHECK(m.save((w+"/dimensions/minecraft/overworld/region/r.0.0.mca").c_str()),"落盘失败");
	Level lv(w.c_str(),2,2,2);
	Chunk *a=lv.pin_Chunk("overworld",0,0),*b=lv.pin_Chunk("overworld",1,0);
	CHECK(a&&b,"钉不住");
	CHECK(!lv.get_Chunk("overworld",2,0)&&lv.miss_why==Level::GET_CACHE,"缓存全钉住时要报GET_CACHE");
	CHECK(!lv.erase_Chunk("overworld",0,0)&&lv.miss_why==Level::GET_CACHE,"钉住的区块不许erase");
	CHECK(a->is_Full()&&b->is_Full(),"钉住的指针被动过");
	CHECK(lv.unpin_Chunk("overworld",0,0)&&lv.unpin_Chunk("overworld",1,0),"unpin失败");
	CHECK(!lv.unpin_Chunk("overworld",0,0),"重复unpin要失败");
	CHECK(lv.get_Chunk("overworld",2,0),"unpin以后还取不到");
}

/*==================== 5. POI / 实体 / 方块实体 ====================*/
static void write_POIHead(Buf &b){b.root();b.name("DataVersion",NBT_INT);b.i32(4788);}/*POI 区块的开头：根 + DataVersion*/
static nbt_Compound *read_RootBytes(const std::vector<byte> &d){/*一段合法的 NBT 字节读成树；读不出来算用例失败*/
	nbt_Raw raw(d);
	nbt_Compound *c=raw.read_Root();
	CHECK(c,"合法NBT读不出来");
	return c;
}
static void exercise_POI(nbt_Compound *c){/*坏 POI 区块上把库的 POI 接口都调一遍：只要求不崩、不越界*/
	std::vector<POI> all;
	count_POI(c);
	list_POI(c,&all);
	get_POI(c,1,2,1);
	erase_POI(c,1,2,1);
	clean_POI(c);
}
static void test_POISectionsInt(){/*Sections是Int：形状不对，绝不能当“这块没记录”*/
	Buf b;write_POIHead(b);
	b.name("Sections",NBT_INT);b.i32(7);
	b.end();
	nbt_Compound *c=read_RootBytes(b.d);
	CHECK(!check_POI(c),"Sections是Int也判成形状对");
	CHECK(count_POI(c)==0,"坏形状数出了记录");
	exercise_POI(c);
	CHECK(set_POI(c,1,2,1,POI("minecraft:librarian",1)),"set_POI写不进去");
	CHECK(check_POI(c)&&count_POI(c)==1,"set_POI没把形状修好");
	c->free();
}
static void test_POIKeyBad(){/*POI 的段键不是数字：check_POI 判坏，别的接口不崩*/
	const char *keys[]={"abc","","1.5","--1","123456789012"};
	for(const char *k:keys){
		Buf b;write_POIHead(b);
		b.name("Sections",NBT_COMPOUND);
		b.name(k,NBT_COMPOUND);b.name("Valid",NBT_BYTE);b.u8(1);b.name("Records",NBT_LIST);b.u8(NBT_COMPOUND);b.u32(0);b.end();
		b.end();b.end();
		nbt_Compound *c=read_RootBytes(b.d);
		CHECK(!check_POI(c),"段键不是整数也判成形状对");
		exercise_POI(c);
		c->free();
	}
}
static void test_POISectionNotCompound(){/*POI 的段不是 compound*/
	Buf b;write_POIHead(b);
	b.name("Sections",NBT_COMPOUND);
	b.name("0",NBT_INT);b.i32(1);
	b.end();b.end();
	nbt_Compound *c=read_RootBytes(b.d);
	CHECK(!check_POI(c)&&count_POI(c)==0,"段是Int：形状/计数不对");
	exercise_POI(c);
	c->free();
}
static void test_POIRecordsNotList(){/*POI 段里的 Records 不是 List*/
	Buf b;write_POIHead(b);
	b.name("Sections",NBT_COMPOUND);
	b.name("0",NBT_COMPOUND);b.name("Valid",NBT_BYTE);b.u8(1);b.name("Records",NBT_INT);b.i32(3);b.end();
	b.end();b.end();
	nbt_Compound *c=read_RootBytes(b.d);
	CHECK(!check_POI(c)&&count_POI(c)==0,"Records是Int：形状/计数不对");
	exercise_POI(c);
	c->free();
}
static void test_POIEmpty(){/*没有Sections / 空Sections：合法的“没记录”*/
	Buf a;write_POIHead(a);a.end();
	nbt_Compound *c=read_RootBytes(a.d);
	CHECK(check_POI(c)&&count_POI(c)==0,"没有Sections应当算空");
	CHECK(set_POI(c,1,2,1,POI("minecraft:meeting",32))&&count_POI(c)==1,"往空POI区块里写失败");
	c->free();
	Buf b;write_POIHead(b);b.name("Sections",NBT_COMPOUND);b.end();b.end();
	c=read_RootBytes(b.d);
	CHECK(check_POI(c)&&count_POI(c)==0,"空Sections应当算空");
	c->free();
}
static void test_POIRecordBad(int which){/*单条记录坏：容器形状仍然对；坏的那条取不出来，好的照取*/
	nbt_Compound c;
	c.set_Int("DataVersion",4788);
	nbt_Compound *secs=new nbt_Compound;
	c.set("Sections",secs);
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Valid",new nbt_Byte(1));
	nbt_List *recs=new nbt_List(NBT_COMPOUND);
	sec->set("Records",recs);
	secs->set("0",sec);
	nbt_Compound *r1=new nbt_Compound;
	r1->set_String("type","minecraft:librarian");
	r1->set("pos",new nbt_Int_Array(std::vector<int32_t>{1,2,3}));
	r1->set_Int("free_tickets",1);
	if(which==1){
		r1->set("free_tickets",new nbt_String("1"));
	}else if(which==2){
		r1->set("type",new nbt_Int(5));
	}else if(which==3){
		r1->set("pos",new nbt_String("1,2,3"));
	}else if(which==4){
		r1->set("pos",new nbt_Int_Array(std::vector<int32_t>{1,2}));
	}
	CHECK(recs->push_back(r1),"拼记录失败");
	nbt_Compound *r2=new nbt_Compound;
	r2->set_String("type","minecraft:farmer");
	r2->set("pos",new nbt_Int_Array(std::vector<int32_t>{4,5,6}));
	r2->set_Int("free_tickets",1);
	CHECK(recs->push_back(r2),"拼记录失败");
	nbt_Raw raw=c.to_Raw();
	nbt_Compound *back=read_RootBytes(raw.data);
	CHECK(check_POI(back),"单条记录坏不该算容器坏");
	CHECK(count_POI(back)==2,"记录条数不对");
	std::vector<POI> all;
	int nbad=list_POI(back,&all);
	if(which==1){/*free_tickets 是 Codec.INT...orElse(0)：不是数值游戏当0，这一条照读*/
		CHECK(nbad==0&&all.size()==2&&all[0].x==1&&all[0].free_tickets==0,"free_tickets 不是数值：游戏当0照读");
	}else{
		CHECK(nbad==1&&all.size()==1,"坏记录也取出来了");
	}
	CHECK(!all.empty()&&all.back().x==4&&all.back().name()=="minecraft:farmer","好记录被坏记录连累");
	CHECK(get_POI(back,4,5,6),"按坐标找不到好记录");
	if(which==3||which==4){
		CHECK(!get_POI(back,1,2,3)&&erase_POI(back,1,2,3)==0,"坐标坏的记录不该被按坐标找到/删掉");
	}
	exercise_POI(back);
	back->free();
}
static void test_POIRecordNotCompound(){/*Records 里的元素不是 compound*/
	nbt_Compound c;
	nbt_Compound *secs=new nbt_Compound;
	c.set("Sections",secs);
	nbt_Compound *sec=new nbt_Compound;
	nbt_List *recs=new nbt_List(NBT_INT);
	recs->push_back(new nbt_Int(7));
	sec->set("Records",recs);
	secs->set("0",sec);
	CHECK(check_POI(&c),"Records是Int的list：容器形状算对，记录交给fixer单条删");
	std::vector<POI> all;
	CHECK(list_POI(&c,&all)==1&&all.empty(),"Int记录也取出来了");
	CHECK(set_POI(&c,1,2,3,POI("minecraft:home",1))&&get_POI(&c,1,2,3),"Records元素类型坏时set_POI要能写进去");
	exercise_POI(&c);
}
static void test_POINegativeY(){/*负的 y：段键按向下取整算（-1 和 -16 在 "-1" 段，-17 在 "-2" 段）*/
	nbt_Compound c;
	CHECK(set_POI(&c,3,-1,5,POI("minecraft:librarian",1))&&set_POI(&c,4,-16,6,POI("minecraft:home",1))&&
	      set_POI(&c,5,-17,7,POI("minecraft:farmer",1)),"set_POI失败");
	const nbt_Any *secs=c.get("Sections");
	CHECK(secs&&secs->type==NBT_COMPOUND&&((nbt_Compound*)secs)->contains("-1")&&((nbt_Compound*)secs)->contains("-2")&&
	      ((nbt_Compound*)secs)->size()==2,"负Y的段键不对（该有-1和-2）");
	CHECK(get_POI(&c,3,-1,5)&&get_POI(&c,4,-16,6)&&get_POI(&c,5,-17,7),"负Y记录找不回来");
	CHECK(erase_POI(&c,4,-16,6)==1&&count_POI(&c)==2,"删负Y记录失败");
	CHECK(clean_POI(&c)==0,"还有记录的段被清掉了");
	erase_POI(&c,3,-1,5);
	erase_POI(&c,5,-17,7);
	CHECK(count_POI(&c)==0&&clean_POI(&c)==2,"空段没清掉");
}
static void test_POIYMin(){/*y=INT32_MIN：段号不能溢出*/
	nbt_Compound c;
	CHECK(set_POI(&c,0,INT32_MIN,0,POI("minecraft:meeting",32)),"写不进去");
	CHECK(get_POI(&c,0,INT32_MIN,0)&&count_POI(&c)==1,"读不回来");
	char key[32];
	snprintf(key,sizeof(key),"%d",get_SectionY(INT32_MIN));
	const nbt_Any *secs=c.get("Sections");
	CHECK(secs&&((const nbt_Compound*)secs)->get(key),"段键算错");
	CHECK(check_POI(&c),"写出来的段键自己不认");
	CHECK(erase_POI(&c,0,INT32_MIN,0)==1,"删不掉");
}
static nbt_Compound *make_EntityChunk(int32_t cx,int32_t cz,bool dv,int pos,bool list){/*造一个实体区块的根：dv=写不写 DataVersion，pos=0 写成长度不对的数组、1 写对的、2 写成字符串，list=Entities 是不是 List*/
	nbt_Compound *c=new nbt_Compound;
	if(dv){
		c->set_Int("DataVersion",4788);
	}
	if(pos==2){
		c->set_String("Position","0,0");
	}else if(pos==1){
		c->set("Position",new nbt_Int_Array(std::vector<int32_t>{cx,cz}));
	}else{
		c->set("Position",new nbt_Int_Array(std::vector<int32_t>{cx}));
	}
	if(list){
		nbt_List *es=new nbt_List(NBT_COMPOUND);
		nbt_Compound *e=new nbt_Compound;
		e->set_String("id","minecraft:villager");
		e->set("UUID",new nbt_Int_Array(std::vector<int32_t>{1,2,3,4}));
		nbt_List *p=new nbt_List(NBT_DOUBLE);
		p->push_back(new nbt_Double(1.5));p->push_back(new nbt_Double(64));p->push_back(new nbt_Double(-60.5));
		e->set("Pos",p);
		es->push_back(e);
		c->set("Entities",es);
	}else{
		c->set_Int("Entities",1);
	}
	return c;
}
static void test_EntityCheck(){/*实体区块：容器（Position/Entities）和单条记录各种坏法，check_Entity* 要分得清*/
	nbt_Compound *ok=make_EntityChunk(3,-4,true,1,true);
	CHECK(check_Entity(ok,3,-4)&&check_EntityContainer(ok,3,-4),"好实体区块没放行");
	CHECK(!check_Entity(ok,4,-4),"Position和槽位对不上也放行了");
	nbt_List *es=(nbt_List*)ok->get("Entities");
	nbt_Compound *pig=new nbt_Compound;
	pig->set_String("id","minecraft:pig");/*没有UUID/Pos：游戏随机给 UUID、放在原点，不报错*/
	es->push_back(pig);
	CHECK(check_EntityRecord(pig)&&check_Entity(ok,3,-4),"没有 UUID/Pos 的实体游戏照读，不算坏");
	nbt_Compound *e=new nbt_Compound;
	e->set_String("id","Minecraft:Pig");/*名字写法不合法：Skipping Entity（WARN）*/
	es->push_back(e);
	CHECK(!check_Entity(ok,3,-4)&&check_EntityContainer(ok,3,-4),"单条坏：严格检查要拦、容器检查要放");
	CHECK(!check_EntityRecord(e)&&check_EntityRecord(es->con[0]),"单条实体检查不对");
	ok->free();
	struct C{bool dv;int pos;bool list;const char *why;};
	{
		nbt_Compound *c=make_EntityChunk(3,-4,false,1,true);
		CHECK(check_Entity(c,3,-4)&&check_EntityContainer(c,3,-4),"没有DataVersion也该放行（游戏当它是-1，中文 实体格式）");
		c->set_String("DataVersion","4788");
		CHECK(!check_EntityContainer(c,3,-4),"DataVersion不是Int也放行了");
		c->free();
	}
	const C cs[]={{true,1,false,"Entities不是list"},
		{true,0,true,"Position只有1个int"},{true,2,true,"Position类型不对"}};
	for(const C &k:cs){
		nbt_Compound *c=make_EntityChunk(3,-4,k.dv,k.pos,k.list);
		CHECK(!check_Entity(c,3,-4)&&!check_EntityContainer(c,3,-4),k.why);
		c->free();
	}
	CHECK(!check_Entity(NULL,0,0)&&!check_EntityContainer(NULL,0,0)&&!check_EntityRecord(NULL),"NULL也放行了");
	nbt_String s("x");
	CHECK(!check_EntityRecord(&s),"String当实体也放行了");
}
static Chunk *make_FullChunk(){/*一段石头的完整区块*/
	Chunk *c=new Chunk(0,0);
	c->con.set_String("Status","minecraft:full");
	nbt_List *secs=new nbt_List(NBT_COMPOUND);
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte(0));
	nbt_Compound *states=new nbt_Compound;
	nbt_List *pal=new nbt_List(NBT_COMPOUND);
	nbt_Compound *stone=new nbt_Compound;
	stone->set_String("Name","minecraft:stone");
	pal->push_back(stone);
	states->set("palette",pal);
	sec->set("block_states",states);
	secs->push_back(sec);
	c->con.set("sections",secs);
	return c;
}
static void test_IndexSectionsLater(){/*索引查过“没有sections”以后，sections才补进来（merge的ensure_Section就这样）。
	约定：直接改树后要 reindex()；sections 成员本身每次现读，整个换掉时不 reindex 也能看出来*/
	for(int how=0;how<2;how++){
		Chunk *c=new Chunk(0,0);
		c->con.set_String("Status","minecraft:full");
		CHECK(!c->get_Section(0),"还没sections就查到了段");
		CHECK(c->get_Block(0,0,0).name()=="minecraft:air","缺段要算空气");
		Chunk *full=make_FullChunk();
		c->con.set("sections",full->con.get("sections")->copy());
		delete full;
		if(how==0){
			c->reindex();
		}
		CHECK(c->get_Section(0),"补出sections以后索引还说没有");
		CHECK(c->get_Block(0,0,0).name()=="minecraft:stone","补出sections以后读不到方块");
		delete c;
	}
}
static void test_BlockEntityListBad(){/*block_entities 元素类型是Byte：写方块实体失败，方块状态也不能写一半*/
	Chunk *c=make_FullChunk();
	nbt_List *bad=new nbt_List(NBT_BYTE);
	bad->push_back(new nbt_Byte(1));
	c->con.set("block_entities",bad);
	Block chest("minecraft:chest");
	chest.block_entities.set_String("id","minecraft:chest");
	CHECK(!c->set_BlockEntity(chest.block_entities,3,4,5),"坏表也写进了方块实体");
	CHECK(!c->set_Block(chest,3,4,5),"坏表上带方块实体的set_Block要失败");
	CHECK(c->get_Block(3,4,5).name()=="minecraft:stone","失败的set_Block把方块状态写了一半");
	CHECK(c->set_Block(Block("minecraft:dirt"),3,4,5),"不带方块实体的写入不该被坏表挡住");
	nbt_List *now=(nbt_List*)c->con.get("block_entities");
	CHECK(now==bad&&now->size()==1&&now->elem==NBT_BYTE,"库自己改了坏表（该留给fixer）");
	delete c;
}
static void test_BlockEntityCoords(){/*方块实体里原来的x/y/z一律丢掉，按放置坐标+xPos/zPos重算*/
	Chunk *c=make_FullChunk();
	c->con.set_Int("xPos",-3);
	c->con.set_Int("zPos",7);
	Block chest("minecraft:chest");
	chest.block_entities.set_String("id","minecraft:chest");
	chest.block_entities.set_Int("x",999);
	chest.block_entities.set("y",new nbt_String("坏"));
	CHECK(c->set_Block(chest,15,4,0),"写不进去");
	nbt_Compound *be=c->get_BlockEntity(15,4,0);
	CHECK(be&&be->get("x")->as_Int()==-33&&be->get("y")->as_Int()==4&&be->get("z")->as_Int()==112,"方块实体坐标没按区块重算");
	c->con.set_String("xPos","坏");
	CHECK(!c->set_Block(chest,1,4,1),"xPos坏了还写进了方块实体");
	CHECK(c->get_Block(1,4,1).name()=="minecraft:stone","xPos坏时方块状态写了一半");
	delete c;
}

/*==================== 6. fd泄漏 ====================*/
static void test_FdChurn(){
	std::string w5=work+"/w5",w2=work+"/w2";
	int before=count_Fd();
	for(int i=0;i<200;i++){
		nbt_Raw::load((work+"/没有这个文件.nbt").c_str());
		nbt_Raw::load((w5+"/level.dat").c_str());
		MCA a(work+"/r.0.0.mca"),b(work+"/r.99.99.mca"),c(work+"/r.1.1.mca");
		nbt_Compound *root=nbt_Compound::read_Dat((w5+"/level.dat").c_str());
		if(root){
			root->free();
		}
		root=nbt_Compound::read_Dat((w2+"/level.dat").c_str());
		if(root){
			root->free();
		}
		get_DatCompression((w5+"/level.dat").c_str());
		Level lv(w5.c_str(),1,1,1);
		lv.get_Chunk("overworld",0,0);
		lv.get_Dat("level.dat");
	}
	CHECK(count_Fd()==before,"反复开文件以后fd变多了");
}

int main(int argc,char **argv){/*清工作目录，逐个用例 fork 着跑，最后打总数；有失败返回 1*/
	if(argc<2){
		printf("用法：%s <工作目录>（会被清空重建）\n",argv[0]);
		return 2;
	}
	work=argv[1];
	remove_Tree(work);
	make_Dir(work);
	printf("bad: 工作目录 %s\n",work.c_str());

	printf("== 1. NBT字节流 ==\n");
	run_Case("空字节流当compound读",test_NBTEmpty);
	run_Case("只有1个tag字节",test_NBTOneByte);
	run_Case("String长度0xFFFF但没数据",test_NBTStringShort);
	run_Case("Byte_Array长度0xFFFFFFFF",test_NBTByteArrayHuge);
	run_Case("Int_Array长度0x7FFFFFFF",test_NBTIntArrayHuge);
	run_Case("Long_Array长度0x0FFFFFFF",test_NBTLongArrayHuge);
	run_Case("List元素数0x7FFFFFFF",test_NBTListHuge);
	run_Case("嵌套List每层谎报长度（读坏就停）",test_NBTListGreedy);
	run_Case("List元素是End、数量0xFFFFFFF0",test_NBTListEndHuge);
	run_Case("tag=0x0d（不存在）",test_NBTTagUnknown);
	run_Case("List元素tag=0x77",test_NBTListElemUnknown);
	run_Case("compound成员名字长度超界",test_NBTNameShort);
	run_Case("嵌套200层compound（读/编码/free）",test_NBTDeep);
	run_Case("空数据读根tag",test_NBTRootEmpty);
	run_Case("根tag不是compound",test_NBTRootNotCompound);
	run_Case("截短的根compound给NULL",test_NBTTruncatedRoot);
	run_Case("成员tag=0x77给NULL",test_NBTMemberTagUnknown);
	run_Case("长度字段几十亿给NULL",test_NBTHugeLength);
	run_Case("elem=End却有元素的list",test_NBTListEndWithItems);

	printf("== 2. 坏区块：成员类型不对 ==\n");
	static const char *what[9]={"","sections是Int","sections是Int的list","Y是String、block_states是Int",
		"palette元素是Int、data太短","block_entities是Int、xPos是String","方块实体x是String",
		"下标越出300条调色板","刻表是String、list套list"};
	for(int i=1;i<=8;i++){
		run_Case(what[i],[i](){test_ChunkBad(i);});
	}

	printf("== 3. 区域文件 ==\n");
	run_Case("100字节的文件：照游戏补0照读，偏移异常的格子当没有",test_MCATiny);
	run_Case("表头偏移指到文件外",test_MCAOffsetOut);
	run_Case("长度字段比文件长",test_MCALengthOver);
	run_Case("长度字段=0",test_MCALengthZero);
	run_Case("长度字段=1（压缩数据0字节）",test_MCALengthOne);
	run_Case("长度超过表头给的扇区数",test_MCALengthSector);
	run_Case("zlib数据是垃圾",test_MCAZlibGarbage);
	run_Case("gzip数据是垃圾",test_MCAGzipGarbage);
	run_Case("type=3不压缩但内容不是NBT",test_MCARawGarbage);
	run_Case("LZ4（type 4）乱数据：坏块",test_MCALz4);
	run_Case("外置.mcc（type 0x82）文件不在：坏块",test_MCAExternal);
	run_Case("自定义压缩（type 127）：不支持≠坏块",test_MCACustom);
	run_Case("压缩类型0x77",test_MCATypeUnknown);
	run_Case("表头扇区数=0",test_MCASectorZero);
	run_Case("区域文件名是个目录",test_MCADirInstead);
	run_Case("不存在/0字节/半个表头/空表头",test_MCAFileStates);
	run_Case("坏表头上写区块要追加（回归）",test_MCAWriteBadHeader);
	run_Case("非4096对齐的文件追加写/format",test_MCAMisaligned);
	run_Case("重叠扇区写回不擦邻居",test_MCAOverlap);
	run_Case("表头偏移指在时间戳表里：写要追加",test_MCAWriteHeaderSector);
	run_Case("表头偏移异常的三种格子：当没有、清得掉",test_MCAHeaderCountOnly);
	run_Case("区域文件：数据坏的格子不让 format 失败",test_MCAFormatBadData);

	printf("== 4. 存档目录 ==\n");
	run_Case("没有level.dat",test_LevelNoDat);
	run_Case("level.dat是垃圾",test_LevelDatGarbage);
	run_Case("level.dat是半个gzip",test_LevelDatHalfGzip);
	run_Case("level.dat解出来不是nbt",test_LevelDatNotNbt);
	run_Case("level.dat正常、区域表头坏",test_LevelBadRegion);
	run_Case("老DataVersion走老布局",test_LevelOldLayout);
	run_Case("坏区块不连累同区域的好区块",test_LevelBadAndGood);
	run_Case("自定义压缩区块：报不支持、只读不改文件",test_LevelCustomChunk);
	run_Case("缓存全被钉住：GET_CACHE、不挤掉指针",test_LevelPinAll);

	printf("== 5. POI / 实体 / 方块实体 ==\n");
	run_Case("POI：Sections是Int",test_POISectionsInt);
	run_Case("POI：段键不是整数",test_POIKeyBad);
	run_Case("POI：段不是compound",test_POISectionNotCompound);
	run_Case("POI：Records不是list",test_POIRecordsNotList);
	run_Case("POI：没有Sections/空Sections",test_POIEmpty);
	run_Case("POI：free_tickets是String（游戏当0）",[](){test_POIRecordBad(1);});
	run_Case("POI：type是Int",[](){test_POIRecordBad(2);});
	run_Case("POI：pos是String",[](){test_POIRecordBad(3);});
	run_Case("POI：pos只有2个int",[](){test_POIRecordBad(4);});
	run_Case("POI：Records元素不是compound",test_POIRecordNotCompound);
	run_Case("POI：负Y段键（写/查/删/清）",test_POINegativeY);
	run_Case("POI：y=INT32_MIN（溢出回归）",test_POIYMin);
	run_Case("实体区块：容器/单条记录检查",test_EntityCheck);
	run_Case("索引：sections是后补的（回归）",test_IndexSectionsLater);
	run_Case("方块实体：坏表不改、set_Block不半写",test_BlockEntityListBad);
	run_Case("方块实体：坐标按放置位置重算",test_BlockEntityCoords);

	printf("== 6. fd泄漏 ==\n");
	run_Case("反复打开/失败打开200轮",test_FdChurn);

	printf("bad: %d 个用例，失败 %d 个\n",case_cnt,fail_cnt);
	return fail_cnt?1:0;
}
