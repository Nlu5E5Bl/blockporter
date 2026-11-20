#include "../nbtrw.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
/*lib.cpp：libnbtrw 的库级测试（不依赖merge/fixer）
 *	覆盖：nbt结构体的深拷贝/data()/比较/转换、List类型统一、Block值语义、
 *	      Chunk读写/缺section=空气/不完整区块拒绝、MCA取写/整理/坏文件、Level自动缓存/落盘/dat。
 *	用法：./test/work/lib（工作目录是仓库根；test/lib.sh 会编好并跑）
 */
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
static int fails=0,cases=0;
static void check_Case(bool cond,const char *msg){/*一项核对：打 ok/FAIL，记用例数和失败数*/
	cases++;
	if(cond){
		printf("ok   %s\n",msg);
	}else{
		fails++;
		printf("FAIL %s\n",msg);
	}
}
static void check_Case(bool cond,const std::string &msg){/*同上，说明是拼出来的字符串*/
	check_Case(cond,msg.c_str());
}
static int write_File(const std::string &p,const void *d,size_t n){/*把一段字节原样写成文件；1=写成了*/
	FILE *fp=fopen(p.c_str(),"wb");
	if(!fp){
		return 0;
	}
	size_t w=fwrite(d,1,n,fp);
	int ok=fclose(fp)==0;
	return ok&&w==n;
}
static size_t get_MCAOffset(const MCA &m,int32_t x,int32_t z){/*表头里这一格的偏移（第几个扇区）*/
	size_t loc=4*(size_t)((x&31)+(z&31)*32);
	return (size_t)m.data[loc]<<16|(size_t)m.data[loc+1]<<8|m.data[loc+2];
}
static uint32_t get_MCATimestamp(const MCA &m,int32_t x,int32_t z){/*表头里这一格的时间戳*/
	size_t loc=4096+4*(size_t)((x&31)+(z&31)*32);
	return (uint32_t)m.data[loc]<<24|(uint32_t)m.data[loc+1]<<16|(uint32_t)m.data[loc+2]<<8|m.data[loc+3];
}
static Chunk *make_Chunk(int32_t cx=0,int32_t cz=0){/*造一块 4788 格式的完整区块：只有 Y=4 一段，整段石头（单条调色板、没有 data）；调用者 delete*/
	Chunk *c=new Chunk;
	c->con.set_Int("DataVersion",4788);
	c->con.set_Int("xPos",cx);
	c->con.set_Int("zPos",cz);
	c->con.set_String("Status","minecraft:full");
	nbt_List *secs=new nbt_List(NBT_COMPOUND);
	nbt_Compound *s=new nbt_Compound;
	s->set("Y",new nbt_Byte(4));/*y=64~79这一段*/
	nbt_Compound *bs=new nbt_Compound;
	nbt_List *pal=new nbt_List(NBT_COMPOUND);
	nbt_Compound *stone=new nbt_Compound;
	stone->set_String("Name","minecraft:stone");
	pal->push_back(stone);
	bs->set("palette",pal);/*没有data：整段都是palette[0]（石头）*/
	s->set("block_states",bs);
	secs->push_back(s);
	c->con.set("sections",secs);
	return c;
}
/*老格式测试用：自己按位打包下标（不借用库的函数）。span=2527 以前紧排跨 long；否则每个 long 放 64/bits 个*/
static std::vector<int64_t> pack_TestIndices(const std::vector<uint32_t> &v,uint32_t bits,bool span){
	std::vector<int64_t> w;
	if(span){
		w.assign((v.size()*bits+63)/64,0);
		for(size_t i=0;i<v.size();i++){
			for(uint32_t b=0;b<bits;b++){
				if((v[i]>>b)&1){
					size_t bit=i*bits+b;
					w[bit/64]|=(int64_t)((uint64_t)1<<(bit%64));
				}
			}
		}
	}else{
		uint32_t per=64/bits;
		w.assign((v.size()+per-1)/per,0);
		for(size_t i=0;i<v.size();i++){
			w[i/per]|=(int64_t)((uint64_t)v[i]<<((i%per)*bits));
		}
	}
	return w;
}
/*造一块 dv 这个版本格式的区块：Y=4 一段，调色板 n 条（0 是空气，其余 minecraft:b<i>），格子 i 的下标是 get_TestPattern(i,n)*/
static uint32_t get_TestPattern(uint32_t i,uint32_t n){return (i*7+i/16)%n;}/*第 i 格放调色板的第几条*/
static std::string make_OldTag(const char *s){return std::string("1.16：")+s;}/*老格式用例的说明统一加个前缀*/
static Chunk *make_OldChunk(int32_t dv,int32_t cx,int32_t cz,uint32_t n){/*见上面那行说明（调用者 delete）*/
	Chunk_Layout L=get_ChunkLayout(dv);
	Chunk *c=new Chunk;
	c->con.set_Int("DataVersion",dv);
	nbt_Compound *body=&c->con;
	if(L.level){
		body=new nbt_Compound;
		c->con.set("Level",body);
	}
	body->set_Int("xPos",cx);
	body->set_Int("zPos",cz);
	if(dv<CHUNK_PROTO){
		body->set("TerrainPopulated",new nbt_Byte(1));
	}else{
		body->set_String(L.status,dv<CHUNK_FULLCHUNK?"postprocessed":(dv<CHUNK_STATUS_FULL?"fullchunk":"full"));
	}
	body->set_Long("InhabitedTime",5);
	nbt_List *pal=new nbt_List(NBT_COMPOUND);
	for(uint32_t i=0;i<n;i++){
		nbt_Compound *e=new nbt_Compound;
		e->set_String("Name",i?("minecraft:b"+std::to_string(i)):std::string("minecraft:air"));
		pal->push_back(e);
	}
	std::vector<uint32_t> v(4096);
	for(uint32_t i=0;i<4096;i++){
		v[i]=get_TestPattern(i,n);
	}
	uint32_t bits=4;
	while(((uint32_t)1<<bits)<n){
		bits++;
	}
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte(4));
	nbt_Compound *box=sec;
	if(L.states){
		box=new nbt_Compound;
		sec->set("block_states",box);
	}
	box->set(L.palette,pal);
	box->set(L.data,new nbt_Long_Array(pack_TestIndices(v,bits,L.span)));
	nbt_List *secs=new nbt_List(NBT_COMPOUND);
	secs->push_back(sec);
	body->set(L.sections,secs);
	c->reindex();
	return c;
}
static void drop_Section(Chunk *c,int32_t sy){/*直接从树上删掉 Y==sy 的那一段（故意绕过库接口，测索引会不会自己发现）*/
	nbt_List *secs=(nbt_List*)c->con.get("sections");
	for(size_t i=0;secs&&i<secs->con.size();i++){
		nbt_Compound *s=(nbt_Compound*)secs->con[i];
		const nbt_Any *y=s?s->get("Y"):NULL;
		if(y&&y->as_Byte()==sy){
			secs->erase(i);
			return;
		}
	}
}
static int make_World(const std::string &dir,int32_t dv,int new_layout){/*造一个最小的存档：level.dat（DataVersion=dv）+ 主世界一个区域文件，里面 (0,0)、(1,0) 两个区块*/
	std::filesystem::create_directories(dir);
	nbt_Compound root;
	nbt_Compound *data=new nbt_Compound;
	data->set_Int("DataVersion",dv);
	root.set("Data",data);
	if(!root.save_Dat((dir+"/level.dat").c_str(),1)){
		return 0;
	}
	std::string rdir=dir+(new_layout?"/dimensions/minecraft/overworld/region":"/region");
	std::filesystem::create_directories(rdir);
	MCA m;
	Chunk *c0=make_Chunk(0,0);
	Chunk *c1=make_Chunk(1,0);
	int ok=m.set_Compound(c0->con,0,0)&&m.set_Compound(c1->con,1,0);
	delete c0;delete c1;
	if(!ok){
		return 0;
	}
	return m.save((rdir+"/r.0.0.mca").c_str());
}

int main(){/*所有用例顺着跑一遍，最后打总数；有失败返回 1*/
	std::filesystem::remove_all("test/work/libtest");
	std::filesystem::create_directories("test/work/libtest");

	/*---- nbt标量/String/数组 ----*/
	{
		nbt_Byte b(5);
		nbt_Short s(6);
		nbt_Int i(7);
		nbt_Long l(8);
		nbt_Float f(1.5f);
		nbt_Double d(2.5);
		check_Case(*b.data()==5&&*s.data()==6&&*i.data()==7&&*l.data()==8&&b.data()==&b.con,"标量 data() 给 con 的指针");
		{/*NaN：两边都是 NaN 算相等（树和自己的拷贝要相等），一边是一边不是不相等，也没有大小*/
			nbt_Float fn(NAN),fn2(NAN),f1(1.0f);
			nbt_Double dn(NAN),d1(1.0);
			check_Case(fn==fn2&&dn==nbt_Double(NAN)&&fn==dn,"NaN==NaN（float/double、跨精度）");
			check_Case(!(fn==f1)&&!(dn==d1)&&!(fn<f1)&&!(fn>f1),"NaN 和别的数不相等、没有大小");
			nbt_Compound cn;
			nbt_List *pos=new nbt_List(NBT_DOUBLE);
			pos->push_back(new nbt_Double(NAN));
			pos->push_back(new nbt_Double(-0.0));
			cn.set("Pos",pos);
			cn.set("f",new nbt_Float(NAN));
			nbt_Compound cc(cn);
			check_Case(cn==cc,"带 NaN 的 compound 和自己的深拷贝相等");
			((nbt_Double*)pos->con[1])->con=0.0;
			check_Case(cn==cc,"-0.0 和 0.0 相等（值相等）");
			((nbt_Double*)pos->con[0])->con=2.0;
			check_Case(!(cn==cc),"NaN 换成别的数就不相等");
		}
		check_Case(*f.data()==1.5f&&*d.data()==2.5,"浮点 data()");
		*i.data()=70;
		check_Case(i.con==70,"改 data() 指着的就是改 con");
		int32_t iv=i;
		int64_t lv=i;
		check_Case(iv==70&&lv==70&&i+1==71&&i==70,"nbt_Int 隐式转成 int32_t（再往宽转交给C++）");
		const nbt_Any &ai=i;
		check_Case(ai.as_Long()==70&&ai.as_Int()==70&&!ai.as_Short()&&!ai.as_Byte()&&!ai.as_Float()&&!ai.as_String(),"as_* 只往宽转，转不了给 nullopt");
		const nbt_Any &af=f;
		check_Case(af.as_Double()==1.5&&af.as_Float()==1.5f&&!af.as_Long(),"float 只能转 double");
		nbt_String str("abc");
		str+="d";
		str.append("ef");
		check_Case(str.size()==6&&str[0]=='a'&&str.find("cde")==2&&str.substr(1,2)=="bc","String仿string接口");
		check_Case(*str.data()=="abcdef"&&str.data()==&str.con,"String data() 给 con 的指针");
		const std::string &sr=str;
		check_Case(&sr==&str.con&&str=="abcdef"&&str!="abc"&&((const nbt_Any&)str).as_String()=="abcdef","String 隐式转成 const std::string&、能直接和字符串比");
		nbt_Int_Array ia;
		ia.push_back(1);ia.push_back(2);ia.push_back(3);
		check_Case(ia.size()==3&&ia[1]==2&&ia.data()->size()==3,"Int_Array仿vector接口");
		(*ia.data())[0]=99;
		check_Case(ia[0]==99,"data() 指着 con，改它就是改数组");
		std::vector<int64_t> wide=ia;
		const std::vector<int32_t> &same_v=ia;
		check_Case(wide.size()==3&&wide[0]==99&&&same_v==&ia.con,"Int_Array 隐式转成自己的 vector（引用）和 long vector（新拷贝）");
		nbt_Byte_Array ba(std::vector<int8_t>{-1,2});
		const nbt_Any &aba=ba,&aia=ia;
		check_Case(aba.as_Int_Array()==std::vector<int32_t>{-1,2}&&aba.as_Long_Array()==std::vector<int64_t>{-1,2}&&aba.as_Byte_Array(),"Byte_Array 能 as 成 int/long 数组");
		check_Case(!aia.as_Byte_Array()&&aia.as_Long_Array()&&aia.as_Long_Array()->size()==3,"Int_Array 不能 as 成 byte 数组（不截断）");
	}
	/*---- List：类型统一 + data() ----*/
	{
		nbt_List l;
		check_Case(l.elem==NBT_END&&l.empty(),"空List的elem=0");
		check_Case(l.push_back(new nbt_Int(1)),"空List加第一个元素：类型自动定");
		check_Case(l.elem==NBT_INT&&l.size()==1,"elem=Int");
		check_Case(!l.push_back(new nbt_String("x")),"类型不符的push_back被拒绝（元素已释放）");
		check_Case(l.size()==1,"被拒绝的元素没有进List");
		std::vector<nbt_Any*> mixed;
		mixed.push_back(new nbt_Int(2));
		mixed.push_back(new nbt_String("y"));
		nbt_List lm(mixed);/*const&：深拷贝；类型不统一→空*/
		check_Case(lm.empty()&&lm.elem==NBT_END,"从类型不统一的vector构造→空List");
		free_AnyVector(mixed);/*原指针仍归调用者*/
		std::vector<nbt_Any*> uni;
		uni.push_back(new nbt_Int(3));
		uni.push_back(new nbt_Int(4));
		nbt_List lu(uni);/*深拷贝*/
		check_Case(lu.size()==2&&lu[0].get()!=uni[0]&&lu[0]->as_Int()==3,"从vector深拷贝构造List");
		free_AnyVector(uni);
		check_Case(lu.data()==&lu.con&&lu.data()->size()==2,"List data() 给 con 的指针");
		nbt_List lcopy(lu);
		check_Case(lcopy==lu&&lcopy[0].get()!=lu[0].get(),"List拷贝构造是深拷贝");
		nbt_List lfromdata(*lu.data());/*const vector&：逐个深拷贝*/
		check_Case(lfromdata==lu&&lfromdata.size()==lu.size()&&lfromdata[0].get()!=lu[0].get(),"List(vector)深拷贝构造");
		check_Case(lu.set(0,new nbt_Int(9))&&lu[0]->as_Int()==9,"List::set 改元素");
		check_Case(!lu.set(0,new nbt_String("bad"))&&lu[0]->as_Int()==9,"List::set 类型不符被拒绝");
		lu.erase(0);
		check_Case(lu.size()==1,"List::erase");
		lu.clear();
		check_Case(lu.empty()&&lu.elem==NBT_END,"List::clear 把elem归0");
	}
	/*---- Compound：顺序无关 + 深拷贝 + data() ----*/
	{
		nbt_Compound a;
		a.set_String("x","hello");
		a.set_Int("y",42);
		nbt_Compound b;
		b.set_Int("y",42);/*插入顺序不同*/
		b.set_String("x","hello");
		check_Case(a==b,"Compound相等与成员顺序无关");
		b.set_Int("y",43);
		check_Case(a!=b,"Compound不相等");
		nbt_Compound c(a);
		c.set_Int("y",7);
		check_Case(a.get("y")->as_Int()==42&&c.get("y")->as_Int()==7,"Compound拷贝构造是深拷贝");
		check_Case(a.data()==&a.con&&a.data()->size()==2,"Compound data() 给 con 的指针");
		nbt_Compound cfromdata(*a.data());/*const map&：逐个深拷贝*/
		check_Case(cfromdata==a&&cfromdata.get("y")->as_Int()==42&&cfromdata.get("y")!=a.get("y"),"Compound(map)深拷贝构造");
		check_Case(a.erase("x")==1&&!a.contains("x"),"Compound::erase");
		a.set("end",new nbt_Any(NBT_END));/*END不是值：等于erase*/
		check_Case(!a.contains("end"),"Compound拒绝NBT_END成员");
		nbt_Int *same=new nbt_Int(1);
		a.set("same",same);
		a.set("same",same);/*同一个指针：不能free了再存回去*/
		check_Case(a.get("same")==same&&same->as_Int()==1,"Compound同一个指针重复set不double free");
		nbt_List dup;
		nbt_Int *q=new nbt_Int(1);
		check_Case(dup.push_back(q)&&!dup.push_back(q),"List拒绝同一个指针进两次");
		check_Case(!dup.push_back(new nbt_Any(NBT_END)),"List拒绝NBT_END元素");
	}
	/*---- 比较：跨精度/字典序/List与Compound只能比相等 ----*/
	{
		nbt_Byte b(5);
		nbt_Long l(5);
		nbt_Short s(6);
		check_Case(b==l&&b<=l,"Byte5 == Long5（先转高精度）");
		check_Case(b<s&&s>b,"短整型和长整型比大小");
		check_Case(((b<=>l)==std::partial_ordering::equivalent),"<=>返回equivalent");
		check_Case(nbt_String("abc")<nbt_String("abd"),"String按字典序");
		check_Case(nbt_Int_Array(std::vector<int32_t>{1,2})<nbt_Int_Array(std::vector<int32_t>{1,3}),"数组按字典序");
		check_Case(nbt_Byte_Array(std::vector<int8_t>{1,2})==nbt_Int_Array(std::vector<int32_t>{1,2}),"Byte_Array按值扩宽后和Int_Array相等");
		check_Case(nbt_Byte_Array(std::vector<int8_t>{1,2})<nbt_Int_Array(std::vector<int32_t>{1,2,0}),"数组前缀更短的小");
		check_Case(nbt_Int(1)!=nbt_Float(1.0f),"整数和浮点没有扩宽关系，不相等");
		std::vector<nbt_Any*> v1,v2;
		v1.push_back(new nbt_Int(1));v1.push_back(new nbt_Int(2));
		v2.push_back(new nbt_Int(1));v2.push_back(new nbt_Int(2));
		nbt_List l1(std::move(v1)),l2(std::move(v2));
		check_Case(l1==l2,"List遍历比较相等");
		l2.set(1,new nbt_Int(3));
		check_Case(l1!=l2,"List不相等");
		check_Case((l1<=>l2)==std::partial_ordering::unordered,"List没有大小：<=>返回unordered");
		nbt_Compound c1,c2;
		c1.set_Int("a",1);
		c2.set_Int("a",1);
		check_Case(c1==c2&&(c1<=>c2)==std::partial_ordering::equivalent,"Compound相等");
		c2.set_Int("b",2);
		check_Case(c1!=c2&&(c1<=>c2)==std::partial_ordering::unordered,"Compound没有大小");
	}
	/*---- 转换：只低→高 ----*/
	{
		nbt_Short s(300);
		check_Case(s.can_Convert(NBT_INT)&&s.can_Convert(NBT_LONG),"Short可以转Int/Long");
		check_Case(!s.can_Convert(NBT_BYTE),"Short不能转Byte（不截断）");
		nbt_Any *i=(nbt_Int*)s.convert(NBT_INT);
		check_Case(i&&i->type==NBT_INT&&i->as_Int()==300,"Short→Int值不变");
		if(i){
			i->free();
		}
		nbt_Long l(5);
		check_Case(!l.can_Convert(NBT_INT)&&l.convert(NBT_INT)==NULL,"Long→Int被拒绝");
		check_Case(!l.as_Int()&&l.as_Long()==5,"as_Int不截断Long（转不了给nullopt）");
		nbt_Float f(1.25f);
		nbt_Any *d=(nbt_Double*)f.convert(NBT_DOUBLE);
		check_Case(d&&d->as_Double()==1.25,"Float→Double");
		if(d){
			d->free();
		}
		check_Case(f.as_Double()==1.25&&!nbt_Double(1.25).as_Float(),"as_Double可以扩宽，as_Float不截断（给nullopt）");
		nbt_Byte_Array ba(std::vector<int8_t>{-1,2});
		nbt_Any *ia=(nbt_Int_Array*)ba.convert(NBT_INT_ARRAY);
		check_Case(ia&&((nbt_Int_Array*)ia)->con[0]==-1&&((nbt_Int_Array*)ia)->con[1]==2,"Byte_Array→Int_Array带符号扩宽");
		if(ia){
			nbt_Any *la=ia->convert(NBT_LONG_ARRAY);
			check_Case(la&&((nbt_Long_Array*)la)->con[0]==-1,"Int_Array→Long_Array");
			if(la){
				la->free();
			}
			ia->free();
		}
		nbt_Int same(7);
		nbt_Any *copy=same.convert(NBT_INT);
		check_Case(copy&&copy!=&same&&*copy==same,"同类型convert=深拷贝");
		if(copy){
			copy->free();
		}
	}
	/*---- Block：值语义 ----*/
	{
		Block stone("minecraft:stone");
		check_Case(stone.valid()&&stone.strid()=="minecraft:stone"&&stone.spacename()=="minecraft"&&stone.basename()=="stone","Block名字");
		Block bare("stone");
		check_Case(bare.spacename()=="minecraft"&&bare.basename()=="stone","没冒号的spacename算minecraft");
		Block air("minecraft:air");
		check_Case(air.valid()&&air.name()=="minecraft:air","空气也是有效Block");
		Block bad;
		check_Case(!bad.valid(),"空id=读不到");
		Block copy=stone;
		copy.properties.set_String("k","v");
		check_Case(stone.properties.empty()&&copy.has_Properties(),"Block拷贝是深拷贝");
		stone.properties.set_String("k","v");
		check_Case(stone==copy,"Block相等比较id+属性+方块实体");
		copy.block_entities.set_String("id","minecraft:chest");
		check_Case(stone!=copy,"方块实体不同→Block不相等");
		nbt_Compound bs;
		bs.set_String("Name","minecraft:oak_stairs");
		nbt_Compound props;
		props.set_String("facing","east");
		bs.set("Properties",props.copy());
		const nbt_Compound *bp=get_StateProps(&bs,STATE_NAME);
		check_Case(bp&&get_PropValue(bp,"facing")&&!strcmp(get_PropValue(bp,"facing"),"east")&&!get_PropValue(bp,"half"),"get_PropValue");
		check_Case(!get_StateProps(&bs,STATE_ID)&&!get_StateName(&bs,STATE_ID)[0],"老格式条目按新格式认不出来（格式按DataVersion，不猜）");
	}
	/*---- Block_Ref：零拷贝比较要和 get_Block 同义 ----*/
	{
		Chunk *ca=make_Chunk(0,0);
		Chunk *cb=make_Chunk(0,0);
		Block_Ref a,b;
		check_Case(ca->get_BlockRef(3,64,3,&a),"get_BlockRef读到石头");
		check_Case(cb->get_BlockRef(3,64,3,&b),"第二区块的get_BlockRef");
		check_Case(a.state&&a.name&&!strcmp(a.name,"minecraft:stone")&&!a.props,"Block_Ref带Name、无Properties");
		check_Case(equal_BlockRef(a,b),"两个石头的Block_Ref相等");
		Block st=ca->get_Block(3,64,3);
		check_Case(equal_BlockRef(a,st)&&equal_BlockRef(b,st),"Block_Ref和get_Block的Block相等");
		Block_Ref air_missing,air_real;
		check_Case(ca->get_BlockRef(0,112,0,&air_missing),"缺section的格子读成空气");
		check_Case(!air_missing.state&&air_missing.name&&!strcmp(air_missing.name,"minecraft:air"),"缺section：state为空、Name=air");
		/*同一区块加一段真正的空气，和“缺section”在Block语义上必须相等*/
		nbt_List *secs=(nbt_List*)ca->con.get("sections");
		nbt_Compound *airsec=new nbt_Compound;
		airsec->set("Y",new nbt_Byte(6));
		nbt_Compound *abs=new nbt_Compound;
		nbt_List *apal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *air=new nbt_Compound;
		air->set_String("Name","minecraft:air");
		apal->push_back(air);
		abs->set("palette",apal);
		airsec->set("block_states",abs);
		secs->push_back(airsec);
		check_Case(ca->get_BlockRef(0,96,0,&air_real),"真实空气段读到palette条目");
		check_Case(air_real.state&&equal_BlockRef(air_missing,air_real),"缺section空气==真实空气");
		check_Case(equal_BlockRef(air_real,ca->get_Block(0,96,0)),"真实空气的ref和Block相等");
		/*空的Properties compound和“没有Properties”在Block里都是空，必须相等*/
		nbt_Compound *bs0=NULL;
		for(size_t i=0;i<secs->con.size();i++){
			nbt_Compound *sec=(nbt_Compound*)secs->con[i];
			const nbt_Any *y=sec?sec->get("Y"):NULL;
			if(y&&y->as_Byte()==4){
				bs0=(nbt_Compound*)sec->get("block_states");
			}
		}
		nbt_List *pal0=bs0?(nbt_List*)bs0->get("palette"):NULL;
		nbt_Compound *p0=pal0?(nbt_Compound*)pal0->con[0]:NULL;
		if(p0){
			nbt_Compound empty_props;
			p0->set("Properties",empty_props.copy());
		}
		ca->reindex();/*上面直接改了树（换了调色板条目里的成员，长度没变，索引看不出来）*/
		Block_Ref with_empty,without;
		check_Case(ca->get_BlockRef(3,64,3,&with_empty)&&cb->get_BlockRef(3,64,3,&without),
			"读带空Properties和不带Properties的两个石头");
		check_Case(with_empty.props&&!without.props&&equal_BlockRef(with_empty,without),
			"空Properties和没有Properties在Block语义下相等");
		/*坏block_states之后再看缺段：不能把上一段的states_bad带到缺段上*/
		Chunk *cbad=make_Chunk(0,0);
		nbt_List *bsecs=(nbt_List*)cbad->con.get("sections");
		nbt_Compound *bsec=new nbt_Compound;
		bsec->set("Y",new nbt_Byte(7));
		bsec->set("block_states",new nbt_Int(1));
		bsecs->push_back(bsec);
		Block_Ref badref,missref;
		check_Case(!cbad->get_BlockRef(0,112,0,&badref),"block_states是Int：get_BlockRef返回false");
		check_Case(cbad->get_BlockRef(0,128,0,&missref)&&!missref.state&&
		   missref.name&&!strcmp(missref.name,"minecraft:air"),
			"上一个坏段之后的缺段仍按空气读");
		delete cbad;
		delete ca;
		delete cb;
	}
	/*---- Block_Ref：方块实体索引 + 缓存失效 ----*/
	{
		Chunk *c=make_Chunk(10,-3);
		Block chest("minecraft:chest");
		chest.block_entities.set_String("id","minecraft:chest");
		check_Case(c->set_Block(chest,5,64,5),"放一个带实体的箱子");
		/*往表里塞一条坐标在别的区块的实体：游戏（BlockEntity.getPosFromTag）把它折回本区块的(5,64,5)，
		  排在后面就顶掉前面的箱子——索引照游戏*/
		nbt_List *bes=(nbt_List*)c->con.get("block_entities");
		nbt_Compound *bogus=new nbt_Compound;
		bogus->set_String("id","minecraft:barrel");
		bogus->set_Int("x",5);
		bogus->set_Int("y",64);
		bogus->set_Int("z",5);
		bes->push_back(bogus);
		Block_Ref r;
		check_Case(c->get_BlockRef(5,64,5,&r),"索引查到正确坐标的方块实体");
		check_Case(r.be&&r.be->get("id")&&r.be->get("id")->as_String()=="minecraft:barrel","坐标在别的区块的折回本区块，同一格取后面那条（和游戏一样）");
		check_Case(equal_BlockRef(r,c->get_Block(5,64,5)),"带实体的ref和Block相等");
		check_Case(c->set_Block(Block("minecraft:gold_block"),5,64,5),"用无实体方块覆盖箱子");
		Block_Ref r2;
		check_Case(c->get_BlockRef(5,64,5,&r2),"方块实体删除后索引跟着重建");
		check_Case(!r2.be&&r2.name&&!strcmp(r2.name,"minecraft:gold_block"),"方块实体不残留、方块已更新");
		delete c;
		/*format_Chunk删段后，索引的Y→section表必须重建*/
		Chunk *cf=make_Chunk(0,0);
		check_Case(cf->get_Block(3,64,3).name()=="minecraft:stone","format前先读一次（建好索引）");
		nbt_List *fs=(nbt_List*)cf->con.get("sections");
		nbt_Compound *badsec=new nbt_Compound;
		badsec->set("Y",new nbt_Int(5));/*Y不是Byte：format_Chunk照游戏的读法改成Byte，不删*/
		fs->push_back(badsec);
		int fy=0;
		check_Case(cf->format_Chunk(&fy)==0&&fy==1&&fs->con.size()==2,"format_Chunk把Int的Y改成Byte、不删段");
		const nbt_Any *ny=badsec->get("Y");
		check_Case(ny&&ny->type==NBT_BYTE&&ny->as_Int()==5&&cf->get_Section(5)==badsec,"改完是Byte 5，索引也对");
		Block_Ref fr;
		check_Case(cf->get_BlockRef(3,64,3,&fr)&&fr.name&&!strcmp(fr.name,"minecraft:stone"),
			"format_Chunk后索引仍能读对");
		delete cf;
	}
	/*---- 1.13 扁平化以后各版本的区块格式（分界照 DataFixers）：读/写/压调色板/判坏/方块实体/计划刻 ----*/
	{
		check_Case(!is_ChunkVersionKnown(1343)&&!is_ChunkVersionKnown(0)&&is_ChunkVersionKnown(1451),"1451 以前、没写 DataVersion 的不认识");
		Chunk_Layout l13=get_ChunkLayout(1631),l16=get_ChunkLayout(2586),l18s=get_ChunkLayout(2834),l18=get_ChunkLayout(2860);
		check_Case(l13.level&&l13.span&&!l13.states&&!strcmp(l13.sections,"Sections")&&!strcmp(l13.data,"BlockStates")&&!strcmp(l13.fluid_ticks,"LiquidTicks"),
			"1.13：Level 外壳、跨 long、Palette/BlockStates、LiquidTicks");
		check_Case(l16.level&&!l16.span&&!l16.states,"1.16：不跨 long 了，还是 Level/Palette");
		check_Case(l18s.level&&l18s.states&&!strcmp(l18s.block_ticks,"TileTicks")&&!strcmp(l18s.fluid_ticks,"LiquidTicks")&&
		   !strcmp(get_ChunkLayout(2841).fluid_ticks,"fluid_ticks"),"21w37a~：Level 里已经是 block_states；2841 起 fluid_ticks");
		check_Case(!l18.level&&!strcmp(l18.block_entities,"block_entities")&&!strcmp(l18.starts,"starts"),"2842 起去掉 Level、改名");
		for(int32_t dv:{1460,1631,1976,2586,2730,2834,2841,2860}){
			std::string tag="DataVersion "+std::to_string(dv)+"：";
			for(uint32_t n:{17u,3u}){/*17条=5位（跨 long 时会跨）；3条=4位*/
				Chunk *c=make_OldChunk(dv,3,-2,n);
				check_Case(c->is_Full()&&check_Chunk(&c->con)&&check_ChunkBase(&c->con)&&has_Block(&c->con),tag+"生成完、形状对、有方块");
				bool all=true;
				for(uint32_t i=0;i<4096;i+=37){
					int x=(int)(i&15),z=(int)((i>>4)&15),y=64+(int)(i>>8);
					uint32_t want=get_TestPattern(i,n);
					std::string nm=want?"minecraft:b"+std::to_string(want):std::string("minecraft:air");
					Block_Ref r;
					all=all&&c->get_Block(x,y,z).name()==nm&&c->get_BlockRef(x,y,z,&r)&&r.name&&nm==r.name;
				}
				check_Case(all,tag+"逐格读对（"+std::to_string(n)+"条调色板）");
				Block gold("minecraft:gold_block");
				check_Case(c->set_Block(gold,5,70,6)&&c->get_Block(5,70,6).name()=="minecraft:gold_block"&&
				   c->get_Block(4,70,6).name()==(get_TestPattern(6*256+6*16+4,n)?"minecraft:b"+std::to_string(get_TestPattern(6*256+6*16+4,n)):std::string("minecraft:air")),
					tag+"写一格，旁边不变");
				for(uint32_t k=0;k<20;k++){/*再加20种：位宽变大，整段要按这个版本的打包方式重排*/
					c->set_Block(Block(("minecraft:x"+std::to_string(k)).c_str()),(int)k%16,71,(int)k/16);
				}
				bool still=c->get_Block(5,70,6).name()=="minecraft:gold_block"&&c->get_Block(19%16,71,19/16).name()=="minecraft:x19";
				for(uint32_t i=0;i<4096;i+=53){
					int x=(int)(i&15),z=(int)((i>>4)&15),y=64+(int)(i>>8);
					if(y==71||(y==70&&x==5&&z==6)){
						continue;
					}
					uint32_t want=get_TestPattern(i,n);
					still=still&&c->get_Block(x,y,z).name()==(want?"minecraft:b"+std::to_string(want):std::string("minecraft:air"));
				}
				check_Case(still&&check_Chunk(&c->con),tag+"位宽变大后重排，旧格子不变");
				check_Case(c->set_Block(Block("minecraft:stone"),5,70,6),tag+"把金块那一格改成石头（金块那条调色板没人用了）");
				int pc=0;
				check_Case(c->format_Chunk(NULL,&pc)==0&&pc==1&&c->get_Block(5,70,6).name()=="minecraft:stone"&&
				   c->get_Block(19%16,71,19/16).name()=="minecraft:x19"&&check_Chunk(&c->con),
					tag+"format_Chunk 压掉没人用的调色板条目（按这个版本的打包方式），格子不变");
				Block chest("minecraft:chest");
				chest.block_entities.set_String("id","minecraft:chest");
				check_Case(c->set_Block(chest,1,65,2)&&c->get_BlockEntity(1,65,2)&&get_ChunkLayout(dv).block_entities&&
				   get_ChunkBody(&c->con)->get(get_ChunkLayout(dv).block_entities),tag+"方块实体写进这个版本的表（TileEntities/block_entities）");
				check_Case(add_ChunkTick(&c->con,true,"minecraft:water",48,64,-32,5,0)&&count_ChunkTicks(&c->con,true)==1&&
				   get_ChunkBody(&c->con)->get(get_ChunkLayout(dv).fluid_ticks),tag+"流体刻写进这个版本的表（LiquidTicks/fluid_ticks）");
				delete c;
			}
		}
		/*BlockStates 长度不对（游戏的升级+读法见 get_StatesView，test/mcdump.sh upgrade 逐格对拍过）：
		  2527 以前位宽不是2的幂，BitStorageAlignFix 按位宽补齐/截断，游戏读得了（库不原样读写）；2527 起整块载入失败*/
		Chunk *c=make_OldChunk(1631,0,0,17);
		nbt_Compound *sec=(nbt_Compound*)((nbt_List*)get_ChunkBody(&c->con)->get("Sections"))->con[0];
		((nbt_Long_Array*)sec->get("BlockStates"))->con.pop_back();
		c->reindex();
		States_View sv=get_StatesView(sec,get_ChunkLayout(1631),true);
		check_Case(check_ChunkBase(&c->con)&&sv.kind==STATES_OK&&!sv.canonical&&!check_Chunk(&c->con)&&!c->get_Block(0,64,0).valid()&&
		   !c->set_Block(Block("minecraft:stone"),0,64,0),"1.13：BlockStates 短一个 long＝游戏补齐照读；库当读不出来、不写、merge 跳过");
		sec->erase("BlockStates");
		c->reindex();
		check_Case(check_ChunkBase(&c->con)&&c->get_Block(0,64,0).name()=="minecraft:air"&&!c->set_Block(Block("minecraft:stone"),0,64,0),
			"1.13：少了 BlockStates＝这一段空气，不往里写");
		delete c;
		c=make_OldChunk(2586,0,0,17);
		sec=(nbt_Compound*)((nbt_List*)get_ChunkBody(&c->con)->get("Sections"))->con[0];
		((nbt_Long_Array*)sec->get("BlockStates"))->con.pop_back();
		c->reindex();
		check_Case(!check_ChunkBase(&c->con)&&!check_SectionStates(sec,get_ChunkLayout(2586)),"1.16：BlockStates 短一个 long＝整块载入失败");
		delete c;
		/*老格式单条调色板：游戏升级时扔掉 data（长度不对也照读，整段是这一种）；往里写时库按整段 palette[0] 重建*/
		c=make_OldChunk(2586,0,0,1);
		sec=(nbt_Compound*)((nbt_List*)get_ChunkBody(&c->con)->get("Sections"))->con[0];
		sec->set("BlockStates",new nbt_Long_Array(std::vector<int64_t>(7,-1)));
		c->reindex();
		check_Case(check_Chunk(&c->con)&&c->get_Block(3,70,9).name()=="minecraft:air",make_OldTag("单条调色板 data 长度不对：照读，整段 palette[0]"));
		check_Case(c->set_Block(Block("minecraft:stone"),3,70,9)&&c->get_Block(3,70,9).name()=="minecraft:stone"&&
		   c->get_Block(4,70,9).name()=="minecraft:air"&&check_Chunk(&c->con),make_OldTag("单条调色板写一格：别的格子还是 palette[0]"));
		delete c;
		/*老格式 2~8 条调色板：升级时补空气到 9 条，下标 3~8 是空气；有这种格子时库不往里写（加条目会让它们变成别的方块）*/
		c=make_OldChunk(2586,0,0,3);
		sec=(nbt_Compound*)((nbt_List*)get_ChunkBody(&c->con)->get("Sections"))->con[0];
		set_PackedIndex(((nbt_Long_Array*)sec->get("BlockStates"))->con,0,4,false,7);
		c->reindex();
		check_Case(check_ChunkBase(&c->con)&&c->get_Block(0,64,0).name()=="minecraft:air"&&!c->set_Block(Block("minecraft:stone"),1,64,0),
			make_OldTag("3 条调色板、下标 7：游戏补的空气；不往里写"));
		set_PackedIndex(((nbt_Long_Array*)sec->get("BlockStates"))->con,0,4,false,9);
		c->reindex();
		check_Case(!check_ChunkBase(&c->con),make_OldTag("3 条调色板、下标 9：补到 9 条也越界＝整块载入失败"));
		delete c;
		/*新格式 data 是 Int_Array/元素全是数值的 List：游戏 getLongStream 照读（库不原样读写）；不是数值＝没有 data＝整块载入失败*/
		Chunk *nc=make_OldChunk(4788,0,0,3);
		nbt_Compound *bs=(nbt_Compound*)((nbt_Compound*)((nbt_List*)nc->con.get("sections"))->con[0])->get("block_states");
		std::vector<int64_t> w=((nbt_Long_Array*)bs->get("data"))->con;
		nbt_List *dl=new nbt_List(NBT_LONG);
		for(int64_t x:w){
			dl->push_back(new nbt_Long(x));
		}
		bs->set("data",dl);
		nc->reindex();
		check_Case(check_ChunkBase(&nc->con)&&!check_Chunk(&nc->con)&&!nc->get_Block(0,64,0).valid(),"26.1.1：data 是 Long 的 List：游戏照读，库当读不出来");
		nbt_List *sl=new nbt_List(NBT_STRING);
		sl->push_back(new nbt_String("x"));
		bs->set("data",sl);
		nc->reindex();
		check_Case(!check_ChunkBase(&nc->con),"26.1.1：data 的 List 里有不是数值的：当没有 data＝整块载入失败");
		delete nc;
		Chunk *st=make_OldChunk(1976,0,0,3);
		get_ChunkBody(&st->con)->set_String("Status","minecraft:full");
		check_Case(st->is_Full(),"1.14 起状态按名字比：minecraft:full 也算");
		get_ChunkBody(&st->con)->set_String("Status","postprocessed");
		check_Case(!st->is_Full(),"1.14 起 postprocessed 不算完整");
		delete st;
		Chunk *nl=make_OldChunk(1631,0,0,3);
		nl->con.erase("Level");
		nl->reindex();
		check_Case(!nl->is_Full()&&!check_Chunk(&nl->con)&&!nl->get_Block(0,64,0).valid(),"老格式没有 Level：读不到");
		delete nl;
	}
	/*---- 照游戏读数值/名字/字符串：getIntOr/getLongOr/Codec.INT、Identifier.tryParse、readUTF ----*/
	{
		nbt_Long lg((int64_t)0x100000005LL);
		nbt_Float fm(-1.5f),fbig(1e20f);
		nbt_Double dn(std::nan("")),dm(-1.5);
		nbt_String str("1");
		check_Case(get_TagInt(&lg)==5&&get_TagLong(&lg)==0x100000005LL,"long→int 取低32位");
		check_Case(get_TagInt(&fm)==-2&&get_NumberInt(&fm)==-1,"浮点：getIntOr 向下取整，Codec.INT 向零取整");
		check_Case(get_TagLong(&fm)==-1&&get_TagLong(&dm)==-2,"getLongOr：float 向零、double 向下取整");
		check_Case(get_TagInt(&fbig)==INT32_MAX&&get_TagInt(&dn)==0,"越界饱和、NaN 当0");
		check_Case(!get_TagInt(&str)&&!get_TagInt(NULL),"不是数值给 nullopt");
		check_Case(check_Identifier("minecraft:stone")&&check_Identifier("stone")&&check_Identifier(":stone")&&check_Identifier("minecraft:")&&
		   check_Identifier("my_mod:a/b.c-d"),"合法名字");
		{
			nbt_Compound tk;
			tk.set_String("i","minecraft:water");
			tk.set("x",new nbt_Short(1));tk.set("y",new nbt_Long(2));tk.set("z",new nbt_Double(3.9));
			tk.set("t",new nbt_Byte(5));tk.set("p",new nbt_Int(0));
			Chunk_Tick ct;
			nbt_Compound holder;
			nbt_List *tl=new nbt_List(NBT_COMPOUND);
			tl->push_back(tk.copy());
			holder.set("fluid_ticks",tl);
			check_Case(check_ChunkTickRecord(&tk)&&get_ChunkTick(&holder,true,0,&ct)&&ct.x==1&&ct.y==2&&ct.z==3,
				"计划刻：坐标是 Short/Long/Double 也照读（Codec.INT，浮点向零取整）");
			tk.set_String("i","Water");
			check_Case(!check_ChunkTickRecord(&tk),"计划刻：i 写法不合法读不进来");
		}
		check_Case(!check_Identifier("Minecraft:stone")&&!check_Identifier("a:b:c")&&!check_Identifier("..:x")&&!check_Identifier("a b")&&
		   !check_Identifier("ns/x:y"),"不合法名字：大写、路径里有冒号、命名空间是..、空格、命名空间有/");
		check_Case(check_ModifiedUTF8("abc")&&check_ModifiedUTF8("\xC3\xA9")&&check_ModifiedUTF8("\xE4\xB8\xAD")&&check_ModifiedUTF8("\xC0\x80")&&
		   check_ModifiedUTF8(std::string_view("a\0b",3)),"readUTF 认的：ASCII、2/3字节、C0 80、裸0");
		check_Case(!check_ModifiedUTF8("\xF0\x9F\x98\x80")&&!check_ModifiedUTF8("\x80")&&!check_ModifiedUTF8("\xC3")&&!check_ModifiedUTF8("\xE4\xB8")&&
		   !check_ModifiedUTF8("\xC3\x41"),"readUTF 不认的：4字节、孤立的10xxxxxx、半个字符、后续字节不对");
		/*嵌套：根算第1层，最多512层*/
		nbt_Compound root;
		nbt_Compound *cur=&root;
		for(int d=2;d<=513;d++){
			nbt_Compound *n=new nbt_Compound;
			cur->set("n",n);
			cur=n;
		}
		check_Case(!check_NBTReadable(&root,512),"513层：游戏读不了");
		cur->set("bad",new nbt_String(std::string("\xFF")));
		check_Case(prune_NBTUnreadable(&root,512)==1&&check_NBTReadable(&root,512),"删掉第513层那个容器（连带它下面的坏字符串），剩下512层能读");
		nbt_Compound flat;
		flat.set_String("ok","x");
		flat.set("bad",new nbt_String(std::string("\xF0\x9F\x98\x80")));
		flat.set(std::string("\xFF"),new nbt_Int(1));
		nbt_List *sl=new nbt_List(NBT_STRING);
		sl->push_back(new nbt_String("a"));
		sl->push_back(new nbt_String(std::string("\x80")));
		flat.set("list",sl);
		check_Case(!check_NBTReadable(&flat,512)&&prune_NBTUnreadable(&flat,512)==3&&flat.get("ok")&&!flat.get("bad")&&
		   flat.size()==2&&sl->size()==1,"坏字符串/坏键名/列表里的坏字符串：只删那一个");
	}
	/*---- PalettedContainer：游戏整块载入失败的条件 ----*/
	{
		auto mk=[](int n,int64_t *data,size_t len){
			nbt_Compound *c=new nbt_Compound;
			nbt_List *pal=new nbt_List(NBT_COMPOUND);
			for(int i=0;i<n;i++){
				nbt_Compound *e=new nbt_Compound;
				e->set_String("Name",i?"minecraft:stone":"minecraft:air");
				pal->push_back(e);
			}
			c->set("palette",pal);
			if(data){
				c->set("data",new nbt_Long_Array(std::vector<int64_t>(data,data+len)));
			}
			return c;
		};
		std::vector<int64_t> z256(256,0),z255(255,0),z257(257,0);
		nbt_Compound *a=mk(1,NULL,0),*b=mk(2,z256.data(),256),*c=mk(2,NULL,0),*d=mk(2,z255.data(),255),*e=mk(2,z257.data(),257),*f=mk(0,NULL,0);
		check_Case(check_PaletteContainer(a,4096,true)&&check_PaletteContainer(b,4096,true),"单条无data、两条+正好256个long：读得进来");
		check_Case(!check_PaletteContainer(c,4096,true)&&!check_PaletteContainer(d,4096,true)&&!check_PaletteContainer(e,4096,true)&&
		   !check_PaletteContainer(f,4096,true),"两条没data、data短一个/长一个、空调色板：整块载入失败");
		z256[0]=2;
		nbt_Compound *g=mk(2,z256.data(),256);
		check_Case(!check_PaletteContainer(g,4096,true),"下标越界：整块载入失败");
		std::vector<int64_t> bio1(1,0),bio2(2,0);
		nbt_Compound *h=mk(2,bio1.data(),1),*k=mk(2,bio2.data(),2);
		check_Case(check_PaletteContainer(h,64,false)&&!check_PaletteContainer(k,64,false),"生物群系2条是1位：64格正好1个long，2个就不对");
		for(nbt_Compound *x:{a,b,c,d,e,f,g,h,k}){
			x->free();
		}
	}
	/*---- 单条 POI/实体照游戏的 codec ----*/
	{
		nbt_Compound r;
		r.set_String("type","minecraft:home");
		nbt_List *pl=new nbt_List(NBT_INT);
		pl->push_back(new nbt_Int(1));pl->push_back(new nbt_Int(2));pl->push_back(new nbt_Int(3));
		r.set("pos",pl);
		POI pr;
		pr.free_tickets=-1;
		check_Case(get_POIRecord(&r,&pr)&&pr.x==1&&pr.z==3&&pr.name()=="minecraft:home"&&pr.free_tickets==0,"POI：pos 是数值列表也认、没有 free_tickets 当0");
		r.set_String("type","Home");
		POI pb;
		check_Case(!get_POIRecord(&r,&pb)&&!pb.valid(),"POI：type 写法不合法读不进来、out 不动");
		nbt_Compound en;
		en.set_String("id","minecraft:pig");
		check_Case(check_EntityRecord(&en),"实体：只有 id 也读得进来（UUID 随机、Pos 原点）");
		nbt_List *pos=new nbt_List(NBT_DOUBLE);
		pos->push_back(new nbt_Double(1));pos->push_back(new nbt_Double(std::nan("")));pos->push_back(new nbt_Double(3));
		en.set("Pos",pos);
		check_Case(!check_EntityRecord(&en),"实体：Pos 有 NaN（Entity has invalid position）");
		nbt_List *pos2=new nbt_List(NBT_DOUBLE);
		pos2->push_back(new nbt_Double(1));pos2->push_back(new nbt_Double(1e300*1e300));pos2->push_back(new nbt_Double(3));
		en.set("Pos",pos2);
		nbt_List *rot=new nbt_List(NBT_FLOAT);
		rot->push_back(new nbt_Float(0));rot->push_back(new nbt_Float(0));
		en.set("Rotation",rot);
		check_Case(check_EntityRecord(&en),"实体：Pos 无穷大游戏会夹到边界，照读");
		((nbt_List*)en.get("Rotation"))->set(0,new nbt_Float(std::numeric_limits<float>::infinity()));
		check_Case(!check_EntityRecord(&en),"实体：Rotation 不是有限数（Invalid entity rotation）");
	}
	/*---- POI 区块：get/set/erase 按 POI 区块自己的版本，坐标是绝对坐标 ----*/
	{
		auto mk_Rec=[](int32_t x,int32_t y,int32_t z,const char *type){
			nbt_Compound *r=new nbt_Compound;
			nbt_Int_Array *p=new nbt_Int_Array;
			p->push_back(x);p->push_back(y);p->push_back(z);
			r->set("pos",p);
			r->set_String("type",type);
			r->set_Int("free_tickets",1);
			return r;
		};
		POI h("minecraft:home",1);
		check_Case(h.valid()&&h.spacename()=="minecraft"&&h.basename()=="home"&&!POI().valid(),"POI：名字两段、空 type 读不到");
		h.set_Pos(1,2,3);h.set_Type("minecraft:bed");h.set_FreeTickets(5);
		check_Case(h.x==1&&h.y==2&&h.z==3&&h.name()=="minecraft:bed"&&h.free_tickets==5,"POI：修改函数");
		/*新写法：Sections{"-4":{Records}}；坏的那条跳过并计数*/
		nbt_Compound np;
		np.set_Int("DataVersion",3700);
		nbt_Compound *secs=new nbt_Compound,*sec=new nbt_Compound;
		nbt_List *recs=new nbt_List(NBT_COMPOUND);
		recs->push_back(mk_Rec(1,-60,2,"minecraft:home"));
		recs->push_back(mk_Rec(1,-59,2,"Bad Type"));
		sec->set("Records",recs);
		secs->set("-4",sec);
		np.set("Sections",secs);
		std::vector<POI> got;
		check_Case(list_POI(&np,&got)==1&&got.size()==1&&got[0].y==-60&&got[0].name()=="minecraft:home"&&got[0].free_tickets==1&&count_POI(&np)==2,
			"POI：新写法列出记录，坏的那条跳过并计数");
		check_Case(get_POI(&np,1,-60,2).name()=="minecraft:home"&&!get_POI(&np,1,-59,2).valid()&&!get_POI(&np,1,-60,3).valid(),"POI：get_POI 按绝对坐标找");
		/*1946 以前：只认根上 "0".."15"，Sections 不看（游戏的 ReorganizePoi 会把它盖掉）*/
		nbt_Compound op(np);
		op.set_Int("DataVersion",1945);
		nbt_List *r0=new nbt_List(NBT_COMPOUND);
		r0->push_back(mk_Rec(3,5,4,"minecraft:bed"));
		op.set("0",r0);
		got.clear();
		check_Case(list_POI(&op,&got)==0&&got.size()==1&&got[0].name()=="minecraft:bed"&&get_POI(&op,3,5,4).valid()&&!get_POI(&op,1,-60,2).valid(),
			"POI：1945 只认根上的 \"0\"..\"15\"");
		nbt_Compound mp(np);
		mp.erase("DataVersion");
		got.clear();
		check_Case(list_POI(&mp,&got)==1&&got.size()==1&&got[0].y==-60,"POI：没写 DataVersion 当最新写法");
		nbt_Compound bp;
		bp.set_Int("DataVersion",3700);
		bp.set("Sections",new nbt_List(NBT_COMPOUND));
		got.clear();
		check_Case(list_POI(&bp,&got)==-1&&got.empty()&&!check_POI(&bp),"POI：Sections 不是 compound 库当整块读不出来");
		check_Case(set_POI(&bp,1,2,3,POI("minecraft:home"))&&check_POI(&bp)&&get_POI(&bp,1,2,3).valid(),"POI：游戏当没有的 Sections，set 直接换掉");
		/*写：pos 按写入位置重写；一格只留一条；老写法段Y只能 0..15，装不下就一点不改*/
		POI w("minecraft:bell",32);
		w.set_Pos(100,100,100);
		nbt_Compound a=make_POIChunk(1945);
		check_Case(set_POI(&a,7,40,8,w)&&a.get("2")&&!a.get("Sections")&&a.get("DataVersion")->as_Int()==1945,"POI：按 1945 写进根上的 \"2\"");
		got.clear();
		check_Case(list_POI(&a,&got)==0&&got.size()==1&&got[0].x==7&&got[0].y==40&&got[0].free_tickets==32,"POI：pos 按写入位置重写，老写法读得回来");
		nbt_Compound b=make_POIChunk(3700);
		check_Case(set_POI(&b,7,40,8,w)&&b.get("Sections")&&!b.get("2"),"POI：按 3700 写进 Sections");
		const nbt_Any *bs=((const nbt_Compound*)b.get("Sections"))->get("2");
		check_Case(bs&&bs->type==NBT_COMPOUND&&((const nbt_Compound*)bs)->get("Valid")&&check_POI(&b),"POI：新写法的段 Valid=1、形状对");
		POI w2("minecraft:home",1);
		check_Case(set_POI(&b,7,40,8,w2)&&count_POI(&b)==1&&get_POI(&b,7,40,8).name()=="minecraft:home","POI：同一格再 set 是换掉");
		nbt_Compound c(a);
		check_Case(!set_POI(&c,1,-20,1,w)&&c==a,"POI：老写法装不下负的段，给 false、一点不改");
		check_Case(!set_POI(&c,1,2,1,POI())&&!set_POI(&c,1,2,1,POI("Bad Type"))&&c==a,"POI：type 空/写法不合法不写");
		/*同一格几条（PoiSection.add）：同类型留先来的，不同类型后来的顶掉；erase 全删*/
		nbt_Compound dp=make_POIChunk(3700);
		nbt_Compound *ds=new nbt_Compound,*dsec=new nbt_Compound;
		nbt_List *dr=new nbt_List(NBT_COMPOUND);
		nbt_Compound *r1=mk_Rec(1,2,3,"minecraft:home"),*r2=mk_Rec(1,2,3,"minecraft:home"),*r3=mk_Rec(1,2,3,"minecraft:bed");
		r2->set_Int("free_tickets",9);
		dr->push_back(r1);dr->push_back(r2);
		dsec->set("Records",dr);ds->set("0",dsec);dp.set("Sections",ds);
		check_Case(get_POI(&dp,1,2,3).free_tickets==1,"POI：同类型重复留先来的");
		dr->push_back(r3);
		check_Case(get_POI(&dp,1,2,3).name()=="minecraft:bed","POI：不同类型后来的顶掉");
		check_Case(erase_POI(&dp,1,2,3)==3&&count_POI(&dp)==0&&clean_POI(&dp)==1,"POI：erase 删掉这一格所有记录、clean 删空段");
		/*版本：1935（19w09a）以前没有 POI；repair_POI/get_POILists 新老写法都认*/
		check_Case(!has_POIFile(1631)&&!has_POIFile(1934)&&has_POIFile(1935)&&has_POIFile(0)&&has_POIFile(5119),"POI：19w09a（1935）起才有 poi/");
		nbt_Compound ro=make_POIChunk(1940);
		check_Case(set_POI(&ro,1,20,1,POI("minecraft:home"))&&set_POI(&ro,1,40,1,POI("minecraft:bed")),"POI：1940 的老写法写两段");
		ro.set("5",new nbt_Int(3));
		ro.set("Sections",new nbt_Int(1));/*老写法里的 Sections 游戏升级时会被盖掉：不算段*/
		check_Case(get_POILists(&ro).size()==2&&repair_POI(&ro)==1&&!ro.get("5")&&ro.get("Sections")&&count_POI(&ro)==2,
			"POI：老写法根上 \"5\" 不是 List，repair 删这一段、别的不动");
		nbt_Compound rn=make_POIChunk(3700);
		set_POI(&rn,1,20,1,POI("minecraft:home"));
		nbt_Compound *rs=(nbt_Compound*)rn.get("Sections");
		rs->set("7",new nbt_Compound);
		rs->set("8",new nbt_Int(0));
		rs->set("x",new nbt_Compound);
		check_Case(get_POILists(&rn).size()==1&&repair_POI(&rn)==3&&rs->get("1")&&get_POI(&rn,1,20,1).valid(),
			"POI：新写法没 Records/不是 compound 的段 repair 删掉，好段不动");
	}
	/*---- 实体：get/add/erase 两种写法都收（区块里的实体表、entities/ 的实体区块） ----*/
	{
		check_Case(!has_EntityFile(2586)&&has_EntityFile(2681)&&has_EntityFile(0),"实体：2681 起才写 entities/");
		Entity pig("minecraft:pig");
		check_Case(pig.valid()&&pig.name()=="minecraft:pig"&&pig.basename()=="pig"&&pig.uuid()==""&&!Entity().valid(),"实体：只有 id、没有 UUID 是空串");
		check_Case(pig.set_UUID("0123abcd-0000-0001-8000-00000000ffff")&&pig.uuid()=="0123abcd-0000-0001-8000-00000000ffff","实体：UUID 写进去读得回来");
		const nbt_Any *ua=pig.con.get("UUID");
		check_Case(ua&&ua->type==NBT_INT_ARRAY&&((const nbt_Int_Array*)ua)->con[2]==(int32_t)0x80000000u,"实体：UUID 写成 IntArray[4]");
		check_Case(!pig.set_UUID("xyz")&&!pig.set_UUID("0123abcd00000001800000000000ffff0000")&&pig.uuid()=="0123abcd-0000-0001-8000-00000000ffff","实体：UUID 格式不对不改");
		Entity old("minecraft:cow");
		old.con.set_Long("UUIDMost",0x0123456789abcdefLL);
		old.con.set_Long("UUIDLeast",-1);
		check_Case(old.uuid()=="01234567-89ab-cdef-ffff-ffffffffffff","实体：老写法 UUIDMost/UUIDLeast 也认");
		check_Case(old.set_UUID("00000000-0000-0002-0000-000000000003")&&!old.con.get("UUID")&&old.con.get("UUIDMost")->as_Long()==2,"实体：老写法照老写法改");
		double px=0,py=0,pz=0;
		pig.set_Pos(1.5,-60,2.5);
		check_Case(pig.get_Pos(&px,&py,&pz)&&px==1.5&&py==-60&&pz==2.5&&pig.valid(),"实体：Pos 读写");
		std::vector<Entity> ents;
		Chunk *oc=make_OldChunk(2586,0,0,2);
		check_Case(get_ChunkEntities(oc->data(),&ents)==0&&add_ChunkEntity(oc->data(),pig)&&get_ChunkBody(oc->data())->get("Entities")&&
			!oc->con.get("entities"),"实体：2586 的区块写进 Level.Entities");
		check_Case(get_ChunkEntities(oc->data(),&ents)==1&&ents.size()==1&&ents[0]==pig,"实体：Level.Entities 读得回来");
		delete oc;
		Chunk *nc=make_Chunk();
		ents.clear();
		check_Case(add_ChunkEntity(nc->data(),pig)&&nc->con.get("entities")&&get_ChunkEntities(nc->data(),&ents)==1,"实体：4788 的区块写进 entities");
		check_Case(!add_ChunkEntity(nc->data(),Entity("Bad Id"))&&get_ChunkEntities(nc->data(),&ents)==1,"实体：游戏读不进来的不写");
		nc->con.set("entities",new nbt_Int(1));
		check_Case(get_ChunkEntities(nc->data(),&ents)==-1&&!add_ChunkEntity(nc->data(),pig)&&erase_ChunkEntity(nc->data(),"")==0,"实体：区块的实体表不是 List 读写删都不干");
		delete nc;
		nbt_Compound ec=make_EntityChunk(3,-2,4788);
		check_Case(add_ChunkEntity(&ec,pig)&&add_ChunkEntity(&ec,pig)&&add_ChunkEntity(&ec,Entity("minecraft:sheep"))&&check_Entity(&ec,3,-2)&&
			ec.get("DataVersion")->as_Int()==4788&&!ec.get("entities"),"实体：实体区块写进 Entities");
		ents.clear();
		check_Case(get_ChunkEntities(&ec,&ents)==3&&ents.size()==3&&!check_EntityContainer(&ec,3,-1),"实体：实体区块读得回来、位置对不上容器检查不过");
		check_Case(erase_ChunkEntity(&ec,pig.uuid())==2&&erase_ChunkEntity(&ec,"")==1&&(ents.clear(),get_ChunkEntities(&ec,&ents))==0,
			"实体：按 uuid 删，重复的全删，没有 UUID 的算空串");
		nbt_Compound empty;
		check_Case(get_ChunkEntities(&empty,&ents)==-1&&!add_ChunkEntity(&empty,pig),"实体：空 compound 认不出是哪种");
		nbt_Compound ne;
		ne.set("Position",new nbt_Int_Array(std::vector<int32_t>{0,0}));
		check_Case(get_ChunkEntities(&ne,&ents)==-1,"实体：实体区块没有 Entities 读不出来");
	}
	/*---- 段的Y照游戏读：截断成byte、浮点向下取整、缺了/不是数值当0；改Y以后撞上的也算重复，留最后那段 ----*/
	{
		nbt_Compound ys;
		check_Case(read_SectionY(&ys)==0,"没有Y当0");
		ys.set("Y",new nbt_Int(260));
		check_Case(read_SectionY(&ys)==4,"Int 260截成4");
		ys.set("Y",new nbt_Long(-1));
		check_Case(read_SectionY(&ys)==-1,"Long -1截成-1");
		ys.set("Y",new nbt_Short(0x1FF));
		check_Case(read_SectionY(&ys)==-1,"Short 0x1FF截成-1");
		ys.set("Y",new nbt_Float(-0.5f));
		check_Case(read_SectionY(&ys)==-1,"Float -0.5向下取整成-1");
		ys.set("Y",new nbt_Double(3.9));
		check_Case(read_SectionY(&ys)==3,"Double 3.9向下取整成3");
		ys.set("Y",new nbt_Double(std::nan("")));
		check_Case(read_SectionY(&ys)==0,"NaN当0（Java的(int)NaN=0）");
		ys.set("Y",new nbt_Double(1e20));
		check_Case(read_SectionY(&ys)==-1,"太大饱和成INT32_MAX，低8位是0xFF");
		ys.set("Y",new nbt_Double(-1e20));
		check_Case(read_SectionY(&ys)==0,"太小饱和成INT32_MIN，低8位是0");
		ys.set_String("Y","4");
		check_Case(read_SectionY(&ys)==0,"不是数值当0");
		Chunk *c=make_Chunk(0,0);/*Y=4一段石头*/
		nbt_List *secs=(nbt_List*)c->con.get("sections");
		nbt_Compound *g=new nbt_Compound;
		g->set("Y",new nbt_Int(260));/*截断后也是4，排在后面*/
		nbt_Compound *gbs=new nbt_Compound;
		nbt_List *gpal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *gold=new nbt_Compound;
		gold->set_String("Name","minecraft:gold_block");
		gpal->push_back(gold);
		gbs->set("palette",gpal);
		g->set("block_states",gbs);
		secs->push_back(g);
		nbt_Compound *noy=new nbt_Compound;/*没有Y：当Y=0*/
		secs->push_back(noy);
		check_Case(c->get_Block(0,64,0).name()=="minecraft:gold_block","Int 260的段当Y=4读，排在后面就盖住原来的Y=4");
		check_Case(c->get_Section(0)==noy&&c->get_Block(0,0,0).name()=="minecraft:air","没有Y的段当Y=0（纯光段：空气）");
		int fy=0;
		check_Case(c->format_Chunk(&fy)==1&&fy==2&&secs->con.size()==2,"format_Chunk：改Y 2段、改完撞上的前一段删掉");
		const nbt_Any *gy=g->get("Y");
		check_Case(gy&&gy->type==NBT_BYTE&&gy->as_Int()==4&&c->get_Section(4)==g&&c->get_Block(0,64,0).name()=="minecraft:gold_block",
			"留下的是后面那段，Y改成Byte 4，读出来不变");
		check_Case(c->format_Chunk(&fy)==0&&fy==0,"format_Chunk第二遍0改动");
		delete c;
	}
	/*---- Chunk 的索引：负Y按uint8存、拷贝/移动后照样能读、方块实体按下标 ----*/
	{
		Chunk *c=make_Chunk(2,3);
		nbt_List *secs=(nbt_List*)c->con.get("sections");
		nbt_Compound *neg=new nbt_Compound;
		neg->set("Y",new nbt_Byte(-4));
		nbt_Compound *nbs=new nbt_Compound;
		nbt_List *npal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *deep=new nbt_Compound;
		deep->set_String("Name","minecraft:deepslate");
		npal->push_back(deep);
		nbs->set("palette",npal);
		neg->set("block_states",nbs);
		secs->push_back(neg);
		check_Case(c->get_Block(0,-64,0).name()=="minecraft:deepslate","负Y段（-4）读得到（直接加段，长度变了索引自己重建）");
		check_Case(c->sec_by_y[252]==neg&&c->palette_by_y[252]==npal&&!c->data_by_y[252],"Y=-4存在第252格（uint8，负的从后往前）");
		check_Case(c->sec_by_y[4]&&c->get_Section(4)==c->sec_by_y[4]&&!c->sec_by_y[128+4],"Y=4存在第4格");
		Block chest("minecraft:chest");
		chest.block_entities.set_String("id","minecraft:chest");
		check_Case(c->set_Block(chest,5,-60,6),"负Y放箱子");
		check_Case(c->data_by_y[252]!=NULL,"写进第二种方块后data建出来，索引跟着更新");
		auto it=c->be_by_pos.find(get_BlockEntityKey(5,-60,6));
		check_Case(it!=c->be_by_pos.end()&&it->second==0,"方块实体索引：相对x/z+绝对y→下标0");
		Chunk cp(*c);
		check_Case(cp.sec_by_y[252]&&cp.sec_by_y[252]!=neg,"拷贝：索引指向新树");
		check_Case(cp.get_Block(5,-60,6).name()=="minecraft:chest"&&cp.get_BlockEntity(5,-60,6)&&
		   cp.get_BlockEntity(5,-60,6)!=c->get_BlockEntity(5,-60,6),"拷贝：读得到、方块实体是新的一份");
		nbt_Compound *keep=cp.sec_by_y[252];
		Chunk mv(std::move(cp));
		check_Case(mv.sec_by_y[252]==keep&&mv.get_Block(5,-60,6).has_BlockEntity(),"移动：树没动，索引跟着走");
		check_Case(!cp.sec_by_y[252]&&cp.be_by_pos.empty()&&cp.con.empty(),"移动：原对象清空");
		Chunk as;
		as=mv;
		check_Case(as.get_Block(0,-64,0).name()=="minecraft:deepslate"&&mv.get_Block(0,-64,0).name()=="minecraft:deepslate","拷贝赋值两边都能读");
		nbt_Compound back=(nbt_Compound)std::move(as);
		check_Case(back.get("sections")&&!as.sec_by_y[252],"右值转成nbt_Compound：拿走树，索引清空");
		check_Case(c->set_Block(Block("minecraft:stone"),5,-60,6)&&c->be_by_pos.empty()&&!c->get_BlockEntity(5,-60,6),
			"删掉方块实体后索引重建");
		delete c;
	}
	/*---- 移动 Chunk：建索引时的版本/格式跟着走（漏了的话下一次读要把段表白建一遍） ----*/
	{
		Chunk *o=make_OldChunk(1631,0,0,17);/*2527 以前：调色板在段里、下标跨 long*/
		check_Case(o->idx_dv==1631&&!o->idx_states&&o->idx_span,"老区块建索引：记下 DataVersion 和格式");
		Chunk mv(std::move(*o));
		check_Case(mv.idx_dv==1631&&!mv.idx_states&&mv.idx_span,"移动构造：索引的版本/格式跟着走");
		check_Case(o->idx_dv==0&&o->idx_states&&!o->idx_span&&!o->idx_secs,"移动构造：原对象的索引回到初始值");
		Chunk as(7,8);
		as=std::move(mv);
		check_Case(as.idx_dv==1631&&!as.idx_states&&as.idx_span,"移动赋值：索引的版本/格式跟着走");
		check_Case(as.get_Block(1,64,0).valid(),"移动后的老区块照样能读");
		delete o;
	}
	/*---- Chunk：读写/缺section/不完整 ----*/
	{
		Chunk *c=make_Chunk();
		check_Case(c->is_Full(),"Status=full：区块完整");
		Block st=c->get_Block(3,64,3);
		check_Case(st.valid()&&st.name()=="minecraft:stone"&&!st.has_Properties(),"get_Block读到石头");
		check_Case(c->set_Block(Block("minecraft:gold_block"),3,64,3),"set_Block写gold_block");
		Block g=c->get_Block(3,64,3);
		check_Case(g.valid()&&g.name()=="minecraft:gold_block","写进去再读出来是gold_block");
		Block stairs("minecraft:oak_stairs");
		stairs.properties.set_String("facing","east");
		check_Case(c->set_Block(stairs,4,64,4),"set_Block带Properties");
		Block s2=c->get_Block(4,64,4);
		check_Case(s2.has_Properties()&&s2.properties.get("facing")&&s2.properties.get("facing")->as_String()=="east","Properties原样读回");
		nbt_Compound be;
		be.set_String("id","minecraft:chest");
		check_Case(c->set_BlockEntity(be,5,64,5),"set_BlockEntity");
		Block ch=c->get_Block(5,64,5);
		check_Case(ch.has_BlockEntity()&&ch.block_entities.get("id")->as_String()=="minecraft:chest","Block带方块实体读回");
		check_Case(ch.block_entities.get("x")&&ch.block_entities.get("x")->as_Int()==5,"方块实体x被改成绝对坐标");
		check_Case(c->erase_BlockEntity(5,64,5)==1,"erase_BlockEntity");
		check_Case(!c->get_Block(5,64,5).has_BlockEntity(),"删掉后不再有方块实体");
		/*编码→解析回来，读写结果一致*/
		nbt_Raw raw=c->con.to_Raw();
		nbt_Raw again=raw;
		Chunk *back=read_TestChunk(&again);
		check_Case(back&&back->get_Block(3,64,3).name()=="minecraft:gold_block","Chunk编码/解析后gold_block还在");
		check_Case(back&&back->get_Block(4,64,4).properties.get("facing")->as_String()=="east","编码/解析后Properties还在");
		if(back){
			delete back;
		}
		/*缺section=空气*/
		Chunk *c2=new Chunk(*c);
		drop_Section(c2,4);
		Block air=c2->get_Block(3,64,3);
		check_Case(air.valid()&&air.name()=="minecraft:air","缺section读出来是空气");
		delete c2;
		/*不完整区块不读不写*/
		Chunk *c3=new Chunk(*c);
		c3->con.set_String("Status","minecraft:structure_starts");
		check_Case(!c3->is_Full(),"Status不是full：区块不完整");
		check_Case(!c3->get_Block(3,64,3).valid(),"不完整区块get_Block返回空Block");
		check_Case(!c3->set_Block(Block("minecraft:stone"),3,64,3),"不完整区块set_Block失败");
		c3->con.erase("Status");
		check_Case(!c3->is_Full()&&!c3->get_Block(3,64,3).valid(),"没有Status也当不完整");
		delete c3;
		/*一个方块都没有的区块*/
		Chunk *empty=new Chunk;
		check_Case(!has_Block(&empty->con)&&get_ChunkStatus(&empty->con)==CHUNK_NO_STATUS,"空区块/没有Status");
		delete empty;
		/*方块实体：传进来的x/y/z一律丢弃，按区块xPos/zPos+放置坐标重算*/
		{
			Chunk *ce=make_Chunk(10,-3);
			Block chest("minecraft:chest");
			chest.block_entities.set_String("id","minecraft:chest");
			chest.block_entities.set_Int("x",9999);
			chest.block_entities.set_Int("y",-9999);
			chest.block_entities.set_Int("z",9999);
			check_Case(ce->set_Block(chest,5,64,5),"set_Block带方块实体");
			Block out=ce->get_Block(5,64,5);
			const nbt_Any *ex=out.block_entities.get("x");
			const nbt_Any *ey=out.block_entities.get("y");
			const nbt_Any *ez=out.block_entities.get("z");
			check_Case(ex&&ey&&ez&&ex->as_Int()==165&&ey->as_Int()==64&&ez->as_Int()==-43,
				"方块实体坐标按xPos/zPos+放置坐标重算（丢弃传入值）");
			delete ce;
		}
		/*set_Block原子性：方块实体表坏时，不能先把方块状态写进调色板再返回失败*/
		{
			Chunk *ca=make_Chunk(4,4);
			Block chest("minecraft:chest");
			chest.block_entities.set_String("id","minecraft:chest");
			check_Case(ca->set_Block(Block("minecraft:gold_block"),2,64,2),"原子性测试先放gold_block");
			ca->con.set("block_entities",new nbt_Int(1));
			check_Case(!ca->set_Block(chest,2,64,2),"block_entities不是List：set_Block失败");
			check_Case(ca->get_Block(2,64,2).name()=="minecraft:gold_block","失败后原方块没被改成chest");
			ca->con.erase("block_entities");
			nbt_List *badlist=new nbt_List(NBT_STRING);/*空表但元素类型定死成String：塞compound必失败*/
			ca->con.set("block_entities",badlist);
			check_Case(!ca->set_Block(chest,2,64,2),"空block_entities元素类型不匹配：set_Block失败");
			check_Case(ca->get_Block(2,64,2).name()=="minecraft:gold_block","失败后原方块还是gold_block");
			ca->con.erase("block_entities");
			ca->con.set("block_entities",new nbt_List(NBT_END));
			check_Case(ca->set_Block(chest,2,64,2),"空block_entities(elem=0)可以正常写入并定类型");
			Block chk=ca->get_Block(2,64,2);
			check_Case(chk.name()=="minecraft:chest"&&chk.block_entities.get("id"),"写入后方块和方块实体都在");
			delete ca;
			/*没有方块实体的set_Block也要按正确绝对坐标删旧实体，不能拿(0,0)去误删*/
			Chunk *cb=make_Chunk(5,5);
			Block chest2("minecraft:chest");
			chest2.block_entities.set_String("id","minecraft:chest");
			check_Case(cb->set_Block(chest2,3,64,3),"放一个带方块实体的箱子");
			check_Case(cb->get_BlockEntity(3,64,3)!=NULL,"箱子实体存在");
			check_Case(cb->set_Block(Block("minecraft:stone"),3,64,3),"用不带实体的石头替换箱子");
			check_Case(cb->get_BlockEntity(3,64,3)==NULL,"替换后该格旧方块实体被删掉，没有残留");
			delete cb;
			/*set_Block可能新建data：索引要跟着更新，不能悬空*/
			Chunk *cc=make_Chunk(6,6);
			cc->get_Block(1,64,1);
			check_Case(cc->set_Block(Block("minecraft:gold_block"),1,64,1),"换方块前先读过（建好索引）");
			check_Case(cc->get_Block(1,64,1).name()=="minecraft:gold_block","set_Block后索引跟着更新，不悬空");
			delete cc;
		}
		delete c;
	}
	/*---- 手动新建Chunk/MCA、调色板>256、无嵌套深度限制 ----*/
	{
		Chunk *mc=new Chunk(0,0);
		check_Case(mc->con.get("xPos")&&mc->con.get("xPos")->as_Int()==0&&mc->con.size()==2,"new Chunk(cx,cz)只写xPos/zPos");
		Chunk cs1(3,4);
		Chunk cs2(cs1);
		check_Case(cs2.con==cs1.con&&cs2.con.get("xPos")!=cs1.con.get("xPos"),"Chunk拷贝是深拷贝");
		Chunk cs3(std::move(cs2));
		check_Case(cs3.con==cs1.con&&cs2.con.empty(),"Chunk移动接管con，源变空");
		nbt_Compound raw_c=(nbt_Compound)cs3;/*强转成nbt_Compound就是con（这里是拷贝）*/
		nbt_Compound moved_c=(nbt_Compound)std::move(cs3);/*右值强转：把con挪走*/
		check_Case(raw_c==cs1.con&&moved_c==cs1.con&&cs3.con.empty(),"Chunk强转成nbt_Compound");
		Chunk from_c(std::move(moved_c));
		check_Case(from_c.con==cs1.con&&moved_c.empty()&&from_c.data()==&from_c.con,"从nbt_Compound接管构造、data()给con的指针");
		check_Case(check_Chunk(&mc->con),"手动Chunk形状检查通过");
		{
			Chunk badsec(*mc);
			badsec.con.set("sections",new nbt_Int(1));
			check_Case(!check_Chunk(&badsec.con),"sections不是List：check_Chunk判坏");
			Chunk badbe(*mc);
			badbe.con.set("block_entities",new nbt_Int(1));
			check_Case(!check_Chunk(&badbe.con),"block_entities不是List：check_Chunk判坏");
			Chunk badbe2(*mc);
			badbe2.con.set("block_entities",new nbt_List(NBT_INT));
			check_Case(!check_Chunk(&badbe2.con),"空block_entities元素类型不对：check_Chunk判坏");
			Chunk *tmpidx=make_Chunk(0,0);
			Chunk badidx(*tmpidx);
			delete tmpidx;
			{
				nbt_List *ss=(nbt_List*)badidx.con.get("sections");
				nbt_Compound *s0=ss&&!ss->con.empty()?(nbt_Compound*)ss->con[0]:NULL;
				nbt_Compound *bs0=s0?(nbt_Compound*)s0->get("block_states"):NULL;
				nbt_Long_Array *da=new nbt_Long_Array;
				da->con.assign(256,0);
				da->con[0]=1;/*palette只有1条：游戏是单值调色板，data 根本不看*/
				if(bs0){
					bs0->set("data",da);
				}
			}
			check_Case(check_Chunk(&badidx.con)&&badidx.get_Block(0,64,0).name()=="minecraft:stone"&&badidx.get_Block(1,64,0).name()=="minecraft:stone",
				"单条调色板带着乱 data：游戏不看 data，整段都是那一种");
			const nbt_Any *k1=NULL;
			check_Case(is_SectionConst(&badidx,4,&k1)&&k1,"单条调色板带 data 也算整段同一种");
			check_Case(badidx.set_Block(Block("minecraft:gold_block"),1,64,0)&&badidx.get_Block(0,64,0).name()=="minecraft:stone"&&
			   badidx.get_Block(1,64,0).name()=="minecraft:gold_block","往这种段写第二种方块：旧 data 当不存在，重建");
			Chunk *tmp2=make_Chunk(0,0);
			Chunk badidx2(*tmp2);
			delete tmp2;
			{
				nbt_List *ss=(nbt_List*)badidx2.con.get("sections");
				nbt_Compound *bs0=(nbt_Compound*)((nbt_Compound*)ss->con[0])->get("block_states");
				nbt_List *pal=(nbt_List*)bs0->get("palette");
				nbt_Compound *g=new nbt_Compound;
				g->set_String("Name","minecraft:gold_block");
				pal->push_back(g);
				nbt_Long_Array *da=new nbt_Long_Array;
				da->con.assign(256,0);
				da->con[0]=2;/*palette 2条，下标2越界*/
				bs0->set("data",da);
			}
			check_Case(!check_Chunk(&badidx2.con)&&!check_ChunkBase(&badidx2.con),"data下标越界：游戏整块载入失败，check_Chunk/check_ChunkBase 都判坏");
		}
		check_Case(!mc->set_Block(Block("minecraft:stone"),0,64,0),"没Status：未生成区块不能set_Block");
		mc->con.set_String("Status","minecraft:full");
		check_Case(!mc->set_Block(Block("minecraft:stone"),0,64,0),"没section：set_Block失败");
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte(4));
		nbt_Compound *bs=new nbt_Compound;
		nbt_List *pal=new nbt_List(NBT_COMPOUND);
		for(int i=0;i<300;i++){/*300条：旧代码的256上限要能过去*/
			nbt_Compound *st=new nbt_Compound;
			st->set_String("Name","minecraft:stone");
			st->set_String("n",std::to_string(i));
			pal->push_back(st);
		}
		bs->set("palette",pal);
		sec->set("block_states",bs);
		nbt_List *secs=new nbt_List(NBT_COMPOUND);
		secs->push_back(sec);
		mc->con.set("sections",secs);
		check_Case(mc->set_Block(Block("minecraft:stone"),1,64,1),"手动建的section可以set_Block");
		check_Case(mc->get_Block(1,64,1).valid(),"手动建的section读回");
		/*set_Block会换掉block_states副本，旧palette指针作废：重新取一遍再测*/
		bs=(nbt_Compound*)sec->get("block_states");
		pal=(nbt_List*)bs->get("palette");
		nbt_Any *target=pal->con[299];
		check_Case(set_BlockData(mc,4,target,2,3,4),"调色板300条仍能写");
		check_Case(get_BlockData(mc,4,2,3,4)==target,"调色板300条读回同一状态");
		/*同一Y出现两段：format_Chunk留最后一段（游戏取的就是它）、删前面的*/
		{
			nbt_Compound *dup=(nbt_Compound*)sec->copy();
			secs->push_back(dup);
			check_Case(mc->get_Section(4)==dup,"同一Y两段：读最后那段（和游戏一样）");
			check_Case(mc->format_Chunk()==1,"format_Chunk删掉同Y的前一段");
			check_Case(secs->con.size()==1&&mc->get_Section(4)==dup,"format_Chunk保留最后那段");
			sec=dup;/*下面接着用留下来的那段*/
			check_Case(mc->format_Chunk()==0,"format_Chunk第二遍0改动");
			/*调色板最前面插一条没人引用的：format_Chunk要压紧，data下标跟着重排。
			  先给旧data所有下标+1，保证它们仍指向原来的方块；压紧后必须还能读回原来的n=299。*/
			bs=(nbt_Compound*)sec->get("block_states");
			pal=(nbt_List*)bs->get("palette");
			size_t before=pal->con.size();
			nbt_Long_Array *da=(nbt_Long_Array*)bs->get("data");
			check_Case(da!=NULL,"调色板>1要有data");
			if(da){
				uint32_t bits=4;
				while(bits<32&&((uint32_t)1<<bits)<pal->con.size()){
					bits++;
				}
				uint32_t per=64/bits;
				uint64_t mask=((uint64_t)1<<bits)-1;
				for(uint32_t i=0;i<4096;i++){
					size_t wi=i/per;
					uint32_t sh=(i%per)*bits;
					uint64_t v=((uint64_t)da->con[wi]>>sh)&mask;
					v++;
					da->con[wi]=(da->con[wi]&~(int64_t)(mask<<sh))|(int64_t)(v<<sh);
				}
			}
			nbt_Compound *unused=new nbt_Compound;
			unused->set_String("Name","minecraft:unused_palette_test");
			pal->con.insert(pal->con.begin(),unused);
			int pc=0;
			check_Case(mc->format_Chunk(NULL,&pc)==0,"format_Chunk压调色板不删段");
			check_Case(pc==1,"format_Chunk报了1段压调色板");
			bs=(nbt_Compound*)sec->get("block_states");
			pal=(nbt_List*)bs->get("palette");
			check_Case(pal->con.size()==before,"废弃调色板条目被删掉");
			const nbt_Any *back=get_BlockData(mc,4,2,3,4);
			check_Case(back&&back->type==NBT_COMPOUND&&!strcmp(get_StateName(back,STATE_NAME),"minecraft:stone"),
				"压调色板后原有data下标仍指向stone");
			const nbt_Any *backn=back&&back->type==NBT_COMPOUND?((const nbt_Compound*)back)->get("n"):NULL;
			check_Case(backn&&backn->type==NBT_STRING&&backn->as_String()=="299",
				"压调色板后data下标重映射到原来的n=299");
			pc=0;
			check_Case(mc->format_Chunk(NULL,&pc)==0&&pc==0,"format_Chunk第二遍不再压调色板");
		}
		/*同Y两段内容不同：读写都只认最后那段；format_Chunk之后只留最后那段*/
		{
			Chunk *rc=make_Chunk(9,9);
			nbt_List *rs=(nbt_List*)rc->con.get("sections");
			nbt_Compound *gs=new nbt_Compound;
			gs->set("Y",new nbt_Byte(4));
			nbt_Compound *gbs=new nbt_Compound;
			nbt_List *gpal=new nbt_List(NBT_COMPOUND);
			nbt_Compound *gold=new nbt_Compound;
			gold->set_String("Name","minecraft:gold_block");
			gpal->push_back(gold);
			gbs->set("palette",gpal);
			gs->set("block_states",gbs);
			rs->push_back(gs);
			check_Case(rc->get_Block(0,64,0).name()=="minecraft:gold_block","同Y两段：读最后那段");
			check_Case(rc->set_Block(Block("minecraft:air"),0,64,0),"同Y两段：写最后那段");
			check_Case(rc->get_Block(0,64,0).name()=="minecraft:air"&&rc->get_Block(1,64,1).name()=="minecraft:gold_block",
				"同Y两段：写完还读最后那段");
			check_Case(rc->format_Chunk()==1&&rs->con.size()==1&&rs->con[0]==gs,"同Y两段：format_Chunk后只剩最后那段");
			delete rc;
		}
		/*256→257：位宽从8变9，整段下标要重排*/
		{
			Chunk c2(0,0);
			nbt_Compound *s2=new nbt_Compound;
			s2->set("Y",new nbt_Byte(1));
			nbt_Compound *bs2=new nbt_Compound;
			nbt_List *pal2=new nbt_List(NBT_COMPOUND);
			for(int i=0;i<256;i++){
				nbt_Compound *st=new nbt_Compound;
				st->set_String("Name","minecraft:stone");
				st->set_String("n",std::to_string(i));
				pal2->push_back(st);
			}
			bs2->set("palette",pal2);
			s2->set("block_states",bs2);
			nbt_List *l2=new nbt_List(NBT_COMPOUND);
			l2->push_back(s2);
			c2.con.set("sections",l2);
			check_Case(set_BlockData(&c2,1,pal2->con[255],1,1,1),"palette=256时写第255项");
			nbt_Compound extra;
			extra.set_String("Name","minecraft:gold_block");
			check_Case(set_BlockData(&c2,1,&extra,2,2,2),"palette加到257项（8位→9位）");
			check_Case(get_BlockData(&c2,1,1,1,1)==pal2->con[255],"位宽变化后旧下标仍对");
			const nbt_Any *g=get_BlockData(&c2,1,2,2,2);
			check_Case(g&&g->type==NBT_COMPOUND&&!strcmp(get_StateName(g,STATE_NAME),"minecraft:gold_block"),"位宽变化后新方块读回");
		}
		MCA mm;
		check_Case(mm.set_Compound(mc->con,0,0),"手动新建MCA并写Chunk");
		Chunk *mb=get_MCAChunk(mm,0,0);
		check_Case(mb&&mb->is_Full(),"手动新建MCA读回Chunk");
		if(mb){
			delete mb;
		}
		delete mc;
		/*嵌套300层：旧代码的256层上限要能过去*/
		nbt_Compound *root=new nbt_Compound;
		nbt_Compound *cur=root;
		for(int i=0;i<300;i++){
			nbt_Compound *next=new nbt_Compound;
			cur->set("n",next);
			cur=next;
		}
		nbt_Raw deep=root->to_Raw();
		check_Case(!deep.fail,"300层嵌套编码成功");
		nbt_Raw deep2=deep;
		nbt_Compound *back=deep2.read_Root();
		check_Case(back!=NULL,"300层嵌套解析成功");
		if(back){
			back->free();
		}
		root->free();
	}
	/*---- 压缩：lz4（lz4-java 的 LZ4Block 流，游戏 region 压缩类型 4） ----*/
	{
		std::vector<byte> pat(200000),rnd(70000);
		for(size_t i=0;i<pat.size();i++){
			pat[i]=(byte)((i/7)%13);
		}
		uint64_t sd=99;
		for(size_t i=0;i<rnd.size();i++){
			sd=sd*6364136223846793005ull+1442695040888963407ull;
			rnd[i]=(byte)(sd>>56);
		}
		bool comp=false;
		std::vector<byte> zp=lz4_Data(pat.data(),pat.size());
		check_Case(zp.size()<pat.size()/4&&unlz4_Data(zp.data(),zp.size(),&comp)==pat&&comp,"lz4：能压的数据来回一样（4个64KB块+结束块）");
		std::vector<byte> zr=lz4_Data(rnd.data(),rnd.size());
		check_Case(zr.size()==rnd.size()+3*21&&zr[8]==(0x10|6)&&unlz4_Data(zr.data(),zr.size(),&comp)==rnd&&comp,
			"lz4：压不小的块原样存（方法0x10），来回一样");
		std::vector<byte> ze=lz4_Data(NULL,0);
		check_Case(ze.size()==21&&unlz4_Data(ze.data(),ze.size(),&comp).empty()&&comp,"lz4：空数据只有结束块");
		/*第2块数据打坏：校验对不上，只交出第1块（游戏的 LZ4BlockInputStream 也是一块一块验了才交）*/
		std::vector<byte> zb=zp;
		uint32_t c1=(uint32_t)zb[9]|(uint32_t)zb[10]<<8|(uint32_t)zb[11]<<16|(uint32_t)zb[12]<<24;
		zb[21+c1+21+5]^=0x55;
		std::vector<byte> part=unlz4_Data(zb.data(),zb.size(),&comp);
		check_Case(!comp&&part.size()==65536&&std::equal(part.begin(),part.end(),pat.begin()),"lz4：坏块之前的照交，坏块和后面的不交");
		std::vector<byte> zt=zp;
		zt.resize(zt.size()-21);
		check_Case(unlz4_Data(zt.data(),zt.size(),&comp)==pat&&!comp,"lz4：没有结束块，数据照样全给，只是不算完整结束");
		/*游戏（26.1.1 的 RegionFile + lz4-java）写的 lz4 区域：(1,2)是普通区块，(3,4)外置但样本里没带 .mcc*/
		MCA gm("test/data/lz4/r.0.0.mca");
		std::optional<nbt_Compound> ga=gm.get_Compound(1,2);
		const nbt_Any *gd=ga?ga->get("data"):NULL;
		check_Case(ga&&ga->get("xPos")&&ga->get("xPos")->as_Int()==1&&gd&&gd->type==NBT_LONG_ARRAY&&((const nbt_Long_Array*)gd)->con.size()==256&&
		   ((const nbt_Long_Array*)gd)->con[3]==(int64_t)(3*0x9E3779B97F4A7C15ull),"游戏写的 lz4 区块读得出来、内容对");
		check_Case(!gm.get_Compound(3,4)&&gm.last_error==MCA_READ_BAD,"外置区块的 .mcc 不在：坏块（游戏 ERROR）");
	}
	/*---- 外置 .mcc：压完要占 256 个扇区以上的放外置，save 写出去；改回内嵌/删掉后 save 删掉；另存别处跟着抄 ----*/
	{
		nbt_Compound big,small;
		std::vector<int8_t> noise(1536*1024);
		uint64_t sd=7;
		for(size_t i=0;i<noise.size();i++){
			sd=sd*6364136223846793005ull+1442695040888963407ull;
			noise[i]=(int8_t)(sd>>56);
		}
		big.set("noise",new nbt_Byte_Array(std::move(noise)));
		small.set_Int("v",1);
		std::string d="test/work/libtest/ext",d2="test/work/libtest/ext2";
		std::filesystem::remove_all(d);
		std::filesystem::remove_all(d2);
		std::filesystem::create_directories(d);
		std::filesystem::create_directories(d2);
		for(uint8_t t:{(uint8_t)MCA_ZLIB,(uint8_t)MCA_LZ4}){
			MCA m;
			m.compression=t;
			check_Case(m.set_Compound(big,3,4)&&m.ext.count(3+4*32)==1,"大区块放外置（先放在内存里）");
			std::optional<nbt_Compound> pb=m.get_Compound(3,4);
			check_Case(pb&&*pb==big,"还没落盘：从内存里的外置数据读回来一样");
			check_Case(m.save((d+"/r.-1.2.mca").c_str()),"存盘（连外置）");
			std::string mcc=d+"/c.-29.68.mcc";/*区块绝对坐标：-1*32+3、2*32+4*/
			check_Case(std::filesystem::exists(mcc),"外置文件名按区块绝对坐标：c.-29.68.mcc");
			MCA r(d+"/r.-1.2.mca");
			std::optional<nbt_Compound> rb=r.get_Compound(3,4);
			check_Case(rb&&*rb==big,"重新打开：从 .mcc 读回来一样");
			check_Case(r.save((d2+"/r.-1.2.mca").c_str())&&std::filesystem::exists(d2+"/c.-29.68.mcc"),"另存别处：.mcc 跟着抄过去");
			check_Case(r.set_Compound(small,3,4)&&r.ext.empty()&&r.save()&&!std::filesystem::exists(mcc),"改回普通大小：save 删掉 .mcc");
			MCA r2(d+"/r.-1.2.mca");
			std::optional<nbt_Compound> rs=r2.get_Compound(3,4);
			check_Case(rs&&*rs==small,"改回内嵌后读到新内容");
			MCA e(d2+"/r.-1.2.mca");
			check_Case(e.erase_Chunk(3,4)&&e.save()&&!std::filesystem::exists(d2+"/c.-29.68.mcc"),"删掉这一格：save 删掉 .mcc");
			MCA bad_name;
			bad_name.set_Compound(big,0,0);
			check_Case(!bad_name.save((d+"/x.mca").c_str()),"有外置数据、文件名又不是 r.<x>.<z>.mca：不知道 .mcc 叫什么，拒绝存");
		}
	}
	/*---- 读一格照游戏：类型 1 只认 gzip、2 只认 zlib；流尾巴坏了不要紧，根 compound 读得全就行 ----*/
	{
		nbt_Compound c;
		c.set_String("k","v");
		MCA m;
		check_Case(m.set_Compound(c,0,0),"写一格（zlib）");
		size_t pos=((size_t)m.data[0]<<16|(size_t)m.data[1]<<8|m.data[2])*4096;
		MCA g=m;
		g.data[pos+4]=MCA_GZIP;
		check_Case(!g.get_Compound(0,0)&&g.last_error==MCA_READ_BAD,"类型写gzip、数据是zlib：游戏的 GZIPInputStream 读不了，坏块");
		MCA t=m;
		uint32_t len=(uint32_t)t.data[pos]<<24|(uint32_t)t.data[pos+1]<<16|(uint32_t)t.data[pos+2]<<8|t.data[pos+3];
		len-=4;/*砍掉 zlib 尾巴的 adler32*/
		t.data[pos]=(byte)(len>>24);t.data[pos+1]=(byte)(len>>16);t.data[pos+2]=(byte)(len>>8);t.data[pos+3]=(byte)len;
		std::optional<nbt_Compound> tc=t.get_Compound(0,0);
		check_Case(tc&&*tc==c,"zlib 少了校验尾巴：根 compound 读全了就算读到（游戏读完根就不往后读）");
	}
	/*---- MCA：取写/整理/坏文件 ----*/
	{
		MCA m;
		check_Case(m.data.size()==8192&&m.is_Empty(),"空MCA=8KB表头");
		Chunk *c0=make_Chunk(0,0);
		Chunk *c1=make_Chunk(1,0);
		check_Case(m.set_Compound(c1->con,1,0)&&m.set_Compound(c0->con,0,0),"MCA写两个区块（索引1先写）");
		check_Case(get_MCAOffset(m,1,0)<get_MCAOffset(m,0,0),"写的时候索引1在前");
		uint32_t ts0=get_MCATimestamp(m,0,0),ts1=get_MCATimestamp(m,1,0);
		check_Case(m.format()==1,"MCA整理成功");
		check_Case(ts0==get_MCATimestamp(m,0,0)&&ts1==get_MCATimestamp(m,1,0),"整理保留时间戳");
		check_Case(get_MCAOffset(m,0,0)<get_MCAOffset(m,1,0),"整理后按索引顺序：(0,0)在前");
		check_Case(m.data.size()%4096==0,"整理后还是整扇区");
		size_t sz=m.data.size();
		check_Case(m.format()==1&&m.data.size()==sz&&get_MCAOffset(m,0,0)==2,"整理是幂等的，第一个区块从sector2开始");
		Chunk *r0=get_MCAChunk(m,0,0),*r1=get_MCAChunk(m,1,0);
		check_Case(r0&&r1&&r0->con.get("xPos")->as_Int()==0&&r1->con.get("xPos")->as_Int()==1,"整理后两个区块都能读回来");
		if(r0){
			delete r0;
		}
		if(r1){
			delete r1;
		}
		check_Case(m.save("test/work/libtest/a.mca"),"MCA落盘");
		MCA m2("test/work/libtest/a.mca");
		check_Case(!m2.bad&&m2.has_Chunk(0,0)&&m2.has_Chunk(1,0),"重新读MCA");
		Chunk *rr=get_MCAChunk(m2,1,0);
		check_Case(rr&&rr->get_Block(3,64,3).name()=="minecraft:stone","MCA里区块内容正确");
		if(rr){
			delete rr;
		}
		check_Case(m2.erase_Chunk(0,0)==1,"erase_Chunk");
		int f=m2.format();
		check_Case(f==1&&!m2.has_Chunk(0,0)&&m2.has_Chunk(1,0),"删掉一格后整理，另一格还在");
		check_Case(m2.data.size()<sz,"整理把空段消掉、文件变短");
		{/*list_MCAFiles：只认文件名正好是 r.<x>.<z>.mca 的普通文件（含 0 字节的），按坐标排序*/
			std::filesystem::create_directories("test/work/libtest/ls/r.9.9.mca");
			write_File("test/work/libtest/ls/r.1.-2.mca","",0);
			write_File("test/work/libtest/ls/r.-3.0.mca","x",1);
			write_File("test/work/libtest/ls/r.1.2.mca.tmp","x",1);
			write_File("test/work/libtest/ls/c.1.2.mcc","x",1);
			write_File("test/work/libtest/ls/r.a.2.mca","x",1);
			/*游戏不读的写法：多余的 0、正号、-0。以前也认，r.01.-2.mca 和 r.1.-2.mca 算成同一个区域，merge/fixer 会把它走两遍*/
			write_File("test/work/libtest/ls/r.01.-2.mca","x",1);
			write_File("test/work/libtest/ls/r.+1.-2.mca","x",1);
			write_File("test/work/libtest/ls/r.-0.0.mca","x",1);
			std::vector<std::pair<int32_t,int32_t>> ls=list_MCAFiles("test/work/libtest/ls");
			check_Case(ls==std::vector<std::pair<int32_t,int32_t>>{{-3,0},{1,-2}}&&list_MCAFiles("test/work/libtest/nodir").empty(),
				"list_MCAFiles 只列游戏拼得出来的 r.<x>.<z>.mca 普通文件（r.01/r.+1/r.-0 不算）、按坐标排序");
			check_Case(get_ExternalPath("test/work/libtest/ls/r.01.-2.mca",0,0).empty()&&
				get_ExternalPath("test/work/libtest/ls/r.1.-2.mca",0,0)=="test/work/libtest/ls/c.32.-64.mcc",
				"外置 .mcc 的路径也只按游戏的文件名算");
		}
		MCA nope("test/work/libtest/nope.mca");
		check_Case(!nope.bad&&nope.data.empty(),"文件不在：空镜像、bad=false");
		std::vector<byte> junk(100,0x5a);
		check_Case(write_File("test/work/libtest/bad.mca",junk.data(),junk.size()),"造一个100字节的乱mca");
		MCA bad("test/work/libtest/bad.mca");
		check_Case(!bad.bad&&bad.truncated&&bad.data.size()==8192&&get_MCAChunk(bad,0,0)==NULL&&bad.last_error==MCA_READ_NO_CHUNK&&
			!bad.has_Chunk(0,0)&&bad.has_BadOffset(0,0)&&bad.is_Empty(),
			"表头不满8KB：照游戏补0照读（truncated），偏移指到文件外的格子照游戏当没有");
		{
			MCA fm("test/work/libtest/bad.mca");
			check_Case(fm.format()&&!fm.has_BadOffset(0,0)&&fm.data==std::vector<byte>(8192,0),"format 把偏移异常的格子清成 0");
		}
		check_Case(bad.save("test/work/libtest/bad2.mca"),"补全的表头能落盘");
		std::filesystem::create_directories("test/work/libtest/dir.mca");
		MCA dir_mca("test/work/libtest/dir.mca");
		check_Case(dir_mca.bad&&get_MCAChunk(dir_mca,0,0)==NULL&&!dir_mca.save("test/work/libtest/dir2.mca"),"读不出来的（目录冒充文件）：bad=true、不许落盘");
		check_Case(write_File("test/work/libtest/empty.mca","",0),"造一个0字节的mca");
		MCA empty_file("test/work/libtest/empty.mca");
		check_Case(!empty_file.bad&&!empty_file.truncated&&empty_file.is_Empty(),"0字节的mca：游戏当空区域（没有 WARN）");
		/*目标表头的扇区数被写大了：原地memset会覆盖下一块，必须改成追加*/
		{
			MCA ov;
			Chunk *oa=make_Chunk(0,0),*ob=make_Chunk(1,0);
			bool base=ov.set_Compound(oa->con,0,0)&&ov.set_Compound(ob->con,1,0);
			size_t loc=4*(0+0*32);
			size_t old=ov.data[loc+3];
			if(base&&old<255){
				ov.data[loc+3]=(byte)(old+1);/*A的旧范围现在盖到B*/
				Chunk repl(*oa);
				bool edited=repl.set_Block(Block("minecraft:gold_block"),3,64,3);
				bool wrote=ov.set_Compound(repl.con,0,0);
				Chunk *rb=get_MCAChunk(ov,1,0);
				check_Case(base&&edited&&wrote&&rb&&rb->get_Block(3,64,3).name()=="minecraft:stone",
					"旧扇区与下一块重叠时写A不能擦掉B");
				if(rb){
					delete rb;
				}
			}else{
				check_Case(false,"旧扇区与下一块重叠测试准备失败");
			}
			delete oa;delete ob;
		}
		delete c0;delete c1;
	}
	/*---- Level：自动缓存/落盘/dat ----*/
	{
		static const char *DIM="minecraft:overworld";
		check_Case(make_World("test/work/libtest/world",4788,1),"造新布局测试世界");
		Level lv("test/work/libtest/world",1,1,2);/*cache_cnt=1：取第二个区块会把第一个挤出去写回*/
		check_Case(lv.dat_ok&&lv.data_version==4788&&lv.new_layout,"Level读到DataVersion=4788、新布局");
		check_Case(lv.get_DimensionPath(DIM,"region")=="dimensions/minecraft/overworld/region","区域目录");
		Chunk *a=lv.get_Chunk(DIM,1,0);
		check_Case(a&&a->is_Full(),"cache_cnt=1：取到区块(1,0)");
		check_Case(lv.set_Block(DIM,17,64,1,Block("minecraft:redstone_block")),"改区块(1,0)");
		Chunk *b=lv.get_Chunk(DIM,0,0);/*把(1,0)挤出缓存，应写回MCA*/
		check_Case(b!=NULL,"再取区块(0,0)（挤出(1,0)）");
		check_Case(lv.set_Block(DIM,1,64,1,Block("minecraft:glass")),"改区块(0,0)");
		check_Case(lv.save(),"Level落盘");
		lv.close();
		Level lv2("test/work/libtest/world",8,4,2);
		check_Case(lv2.get_Block(DIM,17,64,1).name()=="minecraft:redstone_block","被挤出去的改动也存住了（(1,0)）");
		check_Case(lv2.get_Block(DIM,1,64,1).name()=="minecraft:glass","(0,0)的改动也存住了");
		/*set_Chunk(维度,区块)：坐标从区块自己取；老区块（2842 以前）的 xPos/zPos 在 Level 里，也要取得到*/
		{
			Level ls("test/work/libtest/world",8,4,2);
			Chunk *nc=make_Chunk(5,6);
			check_Case(ls.set_Chunk(DIM,*nc)&&ls.get_Chunk(DIM,5,6)&&!ls.get_Chunk(DIM,6,5),"set_Chunk 不给坐标：按 xPos/zPos 放到 (5,6)");
			Chunk *oc=make_OldChunk(1631,7,-3,17);
			check_Case(ls.set_Chunk(DIM,*oc)&&ls.get_Chunk(DIM,7,-3),"set_Chunk 不给坐标：老区块的坐标在 Level 里（放到 (7,-3)）");
			Chunk none;
			check_Case(!ls.set_Chunk(DIM,none)&&ls.miss_why==Level::GET_ARG,"set_Chunk 不给坐标：区块里没有 xPos/zPos 给 GET_ARG");
			delete nc;
			delete oc;
			check_Case(ls.close(),"set_Chunk 不给坐标：落盘");
			Level lr("test/work/libtest/world",8,4,2);
			Chunk *rb=lr.get_Chunk(DIM,7,-3);
			check_Case(rb&&get_ChunkDataVersion(&rb->con)==1631&&rb->get_Block(1,64,0).valid(),"老区块落盘后读得回来");
			check_Case(lr.erase_Chunk(DIM,7,-3)&&lr.erase_Chunk(DIM,5,6)&&lr.close(),"清掉刚加的两块（后面的用例按原来的世界算）");
		}
		/*pin：cache_cnt=1时钉住一个区块，别的区块不能把它挤掉*/
		{
			Level lp("test/work/libtest/world",1,1,2);
			Chunk *pin=lp.pin_Chunk(DIM,1,0);
			check_Case(pin&&pin->is_Full(),"pin_Chunk取到(1,0)");
			Chunk *blocked=lp.get_Chunk(DIM,0,0);
			check_Case(blocked==NULL&&lp.miss_why==Level::GET_CACHE,"缓存全被钉住时get_Chunk返回GET_CACHE");
			check_Case(pin&&pin->get_Block(1,64,1).name()=="minecraft:redstone_block","被钉住的区块指针仍然有效");
			/*set_Chunk用另一个对象覆盖同一个坐标：被pin的Chunk要原位拷进去，地址不变*/
			Chunk other(*pin);
			check_Case(other.set_Block(Block("minecraft:emerald_block"),2,64,2),"准备覆盖用的区块");
			check_Case(lp.set_Chunk(DIM,1,0,other),"set_Chunk覆盖被pin的区块");
			check_Case(pin->get_Block(2,64,2).name()=="minecraft:emerald_block","覆盖后pin指针仍有效且看到新内容");
			check_Case(lp.unpin_Chunk(DIM,1,0),"unpin_Chunk");
			check_Case(lp.get_Chunk(DIM,0,0)!=NULL,"unpin后缓存能继续换入区块");
			lp.close();
		}
		MCA *pm=lv2.get_MCA(DIM,0,0);
		check_Case(pm&&pm->has_Chunk(0,0)&&pm->has_Chunk(1,0),"Level::get_MCA");
		check_Case(lv2.format_MCA(DIM,0,0)==1,"Level::format_MCA");
		check_Case(lv2.save(),"整理后落盘");
		lv2.close();
		Level lv3("test/work/libtest/world",8,4,2);
		check_Case(lv3.get_Block(DIM,17,64,1).name()=="minecraft:redstone_block"&&lv3.get_Block(DIM,1,64,1).name()=="minecraft:glass",
			"整理不影响区块内容");
		/*format_mca之前先改一格（还在chunk缓存里、没落盘）：format_mca要先把脏区块刷进镜像*/
		check_Case(lv3.set_Block(DIM,2,64,2,Block("minecraft:lapis_block")),"format_mca前改一格");
		check_Case(lv3.format_MCA(DIM,0,0)==1,"format_mca会先刷脏区块");
		check_Case(lv3.save(),"format_mca后落盘");
		lv3.close();
		Level lv3b("test/work/libtest/world",8,4,2);
		check_Case(lv3b.get_Block(DIM,2,64,2).name()=="minecraft:lapis_block","format_mca前改的格子也进了文件");
		lv3b.close();
		/*dat：读、改、原压缩方式写回*/
		nbt_Compound *d=lv3.get_Dat("level.dat");
		check_Case(d!=NULL,"get_Dat(level.dat)");
		const nbt_Any *data=d?d->get("Data"):NULL;
		const nbt_Any *dv=data&&data->type==NBT_COMPOUND?((nbt_Compound*)data)->get("DataVersion"):NULL;
		check_Case(dv&&dv->as_Int()==4788,"dat里的DataVersion");
		check_Case(lv3.get_Dat("../escape")==NULL,"dat相对路径不许..");
		nbt_Compound nd(*d);
		nd.set_Int("TestKey",42);
		check_Case(lv3.set_Dat("level.dat",nd),"set_Dat(level.dat)");
		nbt_Compound nd2;
		nd2.set_String("hello","world");
		check_Case(lv3.set_Dat("data/new.dat",nd2),"set_dat新文件（自动建目录）");
		check_Case(lv3.save(),"dat落盘");
		lv3.close();
		Level lv4("test/work/libtest/world",8,4,2);
		nbt_Compound *d2=lv4.get_Dat("level.dat");
		check_Case(d2&&d2->get("TestKey")&&d2->get("TestKey")->as_Int()==42,"level.dat改的内容读回来了");
		nbt_Compound *d3=lv4.get_Dat("data/new.dat");
		check_Case(d3&&d3->get("hello")&&d3->get("hello")->as_String()=="world","新dat读回来了");
		check_Case(get_DatCompression("test/work/libtest/world/data/new.dat")==1,"新dat是gzip");
		/*set_Chunk：新建一个区块*/
		Chunk *nc=make_Chunk(2,0);
		nc->set_Block(Block("minecraft:diamond_block"),1,64,1);
		check_Case(lv4.set_Chunk(DIM,2,0,*nc),"set_chunk新建区块(2,0)");
		delete nc;
		/*set_MCA：直接放一个区域镜像*/
		MCA sm;
		Chunk *sc=make_Chunk(0,0);
		sm.set_Compound(sc->con,0,0);
		delete sc;
		check_Case(lv4.set_MCA(DIM,3,3,sm),"set_MCA(3,3)");
		/*坐标从chunk自己的xPos/zPos取的重载*/
		Chunk *oc=make_Chunk(4,0);
		oc->set_Block(Block("minecraft:gold_block"),1,64,1);
		check_Case(lv4.set_Chunk(DIM,*oc),"set_Chunk(dim,chunk)从xPos/zPos取坐标");
		delete oc;
		check_Case(lv4.save(),"set_Chunk/set_mca落盘");
		lv4.close();
		/*set_Chunk传缓存里同一个Chunk：只标脏，不能先清掉再拷（否则源已经没了）*/
		{
			Level ls("test/work/libtest/world",8,4,2);
			Chunk *same=ls.get_Chunk(DIM,0,0);
			check_Case(same!=NULL,"同对象测试取到(0,0)");
			if(same){
				same->set_Block(Block("minecraft:emerald_block"),3,64,3);
				check_Case(ls.set_Chunk(DIM,0,0,*same),"set_Chunk传缓存里同一个Chunk");
				check_Case(ls.save(),"同对象 set_Chunk落盘");
			}
			ls.close();
			Level ls2("test/work/libtest/world",8,4,2);
			check_Case(ls2.get_Block(DIM,3,64,3).name()=="minecraft:emerald_block","同对象改动生效");
			ls2.close();
		}
		Level lv5("test/work/libtest/world",8,4,2);
		check_Case(lv5.get_Block(DIM,33,64,1).name()=="minecraft:diamond_block","set_chunk的新区块读回来了");
		check_Case(lv5.get_Block(DIM,65,64,1).name()=="minecraft:gold_block","set_Chunk(dim,chunk)按xPos/zPos落到(4,0)");
		MCA *pm2=lv5.get_MCA(DIM,3,3);
		check_Case(pm2&&pm2->has_Chunk(0,0),"set_mca的区域文件读回来了");
		check_Case(!lv5.set_Block(DIM,1000,64,1000,Block("minecraft:stone")),"没有的区块set_Block不能凭空建");
		check_Case(!lv5.get_Block(DIM,INT32_MIN,0,INT32_MIN).valid(),"INT32_MIN坐标get_block不崩");
		check_Case(!lv5.set_Block(DIM,INT32_MAX,0,INT32_MAX,Block("minecraft:stone")),"INT32_MAX坐标set_Block不崩");
		check_Case(lv5.erase_Chunk(DIM,2,0),"erase_Chunk删(2,0)");
		check_Case(lv5.get_Chunk(DIM,2,0)==NULL,"删掉后缓存里也读不到");
		check_Case(lv5.save(),"删区块落盘");
		lv5.close();
		{
			Level lv6("test/work/libtest/world",8,4,2);
			check_Case(lv6.get_Chunk(DIM,2,0)==NULL,"erase_Chunk落盘后重读还是没有");
			lv6.close();
		}
		/*region_cnt=1 + 新区域里缺区块：腾缓存时可能把刚拿到的 MCA 指针挤掉*/
		{
			MCA r1;
			Chunk *only=make_Chunk(33,0);
			r1.set_Compound(only->con,1,0);
			delete only;
			check_Case(r1.save("test/work/libtest/world/dimensions/minecraft/overworld/region/r.1.0.mca"),"造r.1.0.mca");
			Level le("test/work/libtest/world",1,1,1);
			check_Case(le.get_Chunk(DIM,0,0)!=NULL,"取(0,0)占住region/chunk缓存");
			check_Case(le.get_Chunk(DIM,32,0)==NULL&&le.miss_why==Level::GET_NO_CHUNK,"取r.1.0里不存在的区块：不崩");
			le.close();
		}
		/*pin_MCA/pin_Dat：钉住的不被挤掉；全被钉住时取新的给GET_CACHE；close()不放钉住的*/
		{
			Level lq("test/work/libtest/world",8,1,1);
			MCA *pm=lq.pin_MCA(DIM,0,0);
			check_Case(pm&&pm->has_Chunk(0,0),"pin_MCA取到r.0.0");
			check_Case(lq.get_MCA(DIM,1,0)==NULL&&lq.miss_why==Level::GET_CACHE,"区域缓存全被钉住时get_MCA返回GET_CACHE");
			check_Case(pm&&pm->has_Chunk(0,0),"被钉住的MCA指针仍然有效");
			nbt_Compound *pd=lq.pin_Dat("level.dat");
			check_Case(pd&&pd->get("Data"),"pin_Dat取到level.dat");
			check_Case(lq.get_Dat("data/new.dat")==NULL&&lq.miss_why==Level::GET_CACHE,"dat缓存全被钉住时get_Dat返回GET_CACHE");
			check_Case(lq.close(),"有钉住的也能close（落盘）");
			check_Case(pm&&pm->has_Chunk(0,0)&&pd&&pd->get("Data"),"close()不放钉住的：指针还有效");
			check_Case(lq.unpin_MCA(DIM,0,0)&&!lq.unpin_MCA(DIM,0,0),"unpin_MCA；重复unpin失败");
			check_Case(lq.unpin_Dat("level.dat")&&!lq.unpin_Dat("level.dat"),"unpin_Dat；重复unpin失败");
			check_Case(lq.get_MCA(DIM,1,0)!=NULL,"unpin后区域缓存能继续换入");
			check_Case(lq.get_Dat("data/new.dat")!=NULL,"unpin后dat缓存能继续换入");
			lq.close();
		}
		/*dat缓存淘汰：dat_cnt=1，取第二个dat时把脏的第一个先落盘*/
		{
			Level lvd("test/work/libtest/world",8,4,1);
			nbt_Compound *dd=lvd.get_Dat("level.dat");
			check_Case(dd!=NULL,"dat缓存：先取level.dat");
			nbt_Compound ndd(*dd);
			ndd.set_Int("EvictTest",7);
			check_Case(lvd.set_Dat("level.dat",ndd),"dat缓存：改level.dat");
			check_Case(lvd.get_Dat("data/new.dat")!=NULL,"dat缓存：取第二个dat，把第一个挤出去");
			check_Case(lvd.save(),"dat缓存淘汰后落盘");
			lvd.close();
			Level lvd2("test/work/libtest/world",8,4,2);
			nbt_Compound *dd2=lvd2.get_Dat("level.dat");
			check_Case(dd2&&dd2->get("EvictTest")&&dd2->get("EvictTest")->as_Int()==7,"被挤出去的脏dat也存住了");
			lvd2.close();
		}
		/*raw dat：读进来是什么压缩方式，写回去还是什么*/
		{
			nbt_Compound rawdat;
			rawdat.set_Int("k",1);
			check_Case(rawdat.save("test/work/libtest/world/data/raw.dat"),"写一个裸NBT dat");
			Level lvr("test/work/libtest/world",8,4,2);
			nbt_Compound *rd=lvr.get_Dat("data/raw.dat");
			check_Case(rd&&rd->get("k")&&rd->get("k")->as_Int()==1,"读裸NBT dat");
			nbt_Compound nd(*rd);
			nd.set_Int("k",2);
			check_Case(lvr.set_Dat("data/raw.dat",nd),"改裸NBT dat");
			check_Case(lvr.save(),"裸NBT dat落盘");
			lvr.close();
			check_Case(get_DatCompression("test/work/libtest/world/data/raw.dat")==0,"裸NBT dat按裸数据写回");
			Level lvr2("test/work/libtest/world",8,4,2);
			nbt_Compound *rd2=lvr2.get_Dat("data/raw.dat");
			check_Case(rd2&&rd2->get("k")&&rd2->get("k")->as_Int()==2,"裸NBT dat改的内容读回来了");
			lvr2.close();
		}
		/*zlib dat：读进来是zlib，写回去还是zlib*/
		{
			nbt_Compound zd;
			zd.set_Int("z",1);
			check_Case(zd.save_Dat("test/work/libtest/world/data/z.dat",2),"写一个zlib dat");
			Level lvz("test/work/libtest/world",8,4,2);
			nbt_Compound *zdp=lvz.get_Dat("data/z.dat");
			check_Case(zdp&&zdp->get("z")&&zdp->get("z")->as_Int()==1,"读zlib dat");
			if(zdp){
				nbt_Compound nzd(*zdp);
				nzd.set_Int("z",2);
				check_Case(lvz.set_Dat("data/z.dat",nzd),"改zlib dat");
				check_Case(lvz.save(),"zlib dat落盘");
			}
			lvz.close();
			check_Case(get_DatCompression("test/work/libtest/world/data/z.dat")==2,"zlib dat按zlib写回");
			Level lvz2("test/work/libtest/world",8,4,2);
			nbt_Compound *zd2=lvz2.get_Dat("data/z.dat");
			check_Case(zd2&&zd2->get("z")&&zd2->get("z")->as_Int()==2,"zlib dat改的内容读回来了");
			lvz2.close();
		}
		/*老布局*/
		check_Case(make_World("test/work/libtest/old",3465,0),"造老布局测试世界");
		Level old("test/work/libtest/old",4,2,2);
		check_Case(old.dat_ok&&old.data_version==3465&&!old.new_layout,"DataVersion=3465→老布局");
		check_Case(old.get_DimensionPath(DIM,"region")=="region","老布局区域目录=region");
		check_Case(old.get_Block(DIM,1,64,1).name()=="minecraft:stone","老布局读方块");
		old.close();
		/*没有level.dat*/
		std::filesystem::create_directories("test/work/libtest/nodat");
		Level nodat("test/work/libtest/nodat",4,2,2);
		check_Case(!nodat.dat_ok&&!nodat.new_layout,"没有level.dat→dat_ok=0、老布局");
		check_Case(nodat.get_Chunk(DIM,0,0)==NULL&&nodat.miss_why==Level::GET_NO_FILE,"没有区域文件→GET_NO_FILE");
		nodat.close();
		/*Level遇到坏mca：缓存bad=1，不解析、不写*/
		{
			std::filesystem::create_directories("test/work/libtest/badworld/dimensions/minecraft/overworld/region");
			std::filesystem::copy_file("test/work/libtest/world/level.dat","test/work/libtest/badworld/level.dat",
				std::filesystem::copy_options::overwrite_existing);
			std::vector<byte> junk(100,0x33);
			check_Case(write_File("test/work/libtest/badworld/dimensions/minecraft/overworld/region/r.0.0.mca",junk.data(),junk.size()),
				"造表头不满的mca");
			{
				Level lt("test/work/libtest/badworld",4,2,2);
				MCA *tm=lt.get_MCA(DIM,0,0);
				check_Case(tm&&!tm->bad&&tm->truncated,"表头不满的照游戏读，不算坏文件");
				check_Case(lt.get_Chunk(DIM,0,0)==NULL&&lt.miss_why==Level::GET_NO_CHUNK,"偏移指到文件外的格子：照游戏当没有（GET_NO_CHUNK）");
				lt.close();
			}
			std::filesystem::remove("test/work/libtest/badworld/dimensions/minecraft/overworld/region/r.0.0.mca");
			std::filesystem::create_directories("test/work/libtest/badworld/dimensions/minecraft/overworld/region/r.0.0.mca");
			Level lb("test/work/libtest/badworld",4,2,2);
			MCA *bm=lb.get_MCA(DIM,0,0);
			check_Case(bm&&bm->bad,"Level::get_MCA把坏文件缓存成bad=true");
			check_Case(lb.get_Chunk(DIM,0,0)==NULL&&lb.miss_why==Level::GET_BAD_FILE,"坏mca里的区块读不出来：GET_BAD_FILE");
			check_Case(!lb.set_Block(DIM,0,64,0,Block("minecraft:stone")),"坏mca不写");
			check_Case(lb.format_MCA(DIM,0,0)==0,"坏mca不整理");
			lb.close();
		}
		/*Level不绑定维度：同一个Level里维度路径/缓存按维度分开*/
		{
			Level dim("test/work/libtest/world",4,2,2);
			check_Case(Level::is_DimensionName("overworld")&&Level::is_DimensionName("minecraft:the_nether")&&
				!Level::is_DimensionName("nether")&&!Level::is_DimensionName("minecraft:custom"),
				"is_DimensionName只认三个原版维度名");
			check_Case(dim.get_DimensionPath("the_nether","region")=="dimensions/minecraft/the_nether/region",
				"同一个Level下界目录");
			check_Case(dim.get_DimensionPath("DIM-1","region")=="DIM-1/region","非原版维度名当路径");
			check_Case(dim.get_DimensionPath("../x","region").empty(),"维度路径不许..");
			check_Case(dim.get_Chunk("the_nether",0,0)==NULL,"下界没有区块");
			check_Case(dim.get_Chunk(DIM,0,0)!=NULL,"同一个Level主世界还有区块");
			dim.close();
		}
	}
	/*---- 随机NBT：解析器不能崩（配合 ASan/valgrind 跑） ----*/
	{
		uint32_t seed=12345;
		for(int i=0;i<500;i++){
			seed=seed*1103515245u+12345u;
			size_t n=(seed>>8)%160;
			std::vector<byte> d(n);
			for(size_t j=0;j<n;j++){
				seed=seed*1103515245u+12345u;
				d[j]=(byte)(seed>>16);
			}
			nbt_Raw raw(d);
			nbt_Compound *r=raw.read_Root();
			if(r){
				r->free();
			}
		}
		check_Case(1,"500个随机NBT流不崩");
	}
	/*---- 坏NBT：截断的流不崩、返回NULL ----*/
	{
		std::vector<byte> d;
		d.push_back(NBT_COMPOUND);
		d.push_back(0);d.push_back(0);/*根tag+空名字*/
		nbt_Raw raw(d);
		nbt_Compound *c=raw.read_Root();
		check_Case(c==NULL,"截断的NBT返回NULL（不崩）");
			}
	/*---- 按区块 DataVersion 选格式：生成状态字段名（26.4快照1=5119 起 status）----*/
	{
		Chunk a(0,0),b(0,0),c(0,0),d(0,0);
		a.con.set_Int("DataVersion",5118);a.con.set_String("Status","minecraft:full");
		b.con.set_Int("DataVersion",5118);b.con.set_String("status","minecraft:full");
		c.con.set_Int("DataVersion",5119);c.con.set_String("status","minecraft:full");
		d.con.set_Int("DataVersion",5119);d.con.set_String("Status","minecraft:full");
		check_Case(get_ChunkStatus(&a.con)==CHUNK_FULL&&get_ChunkStatus(&c.con)==CHUNK_FULL,"Status/status 按区块 DataVersion 取");
		check_Case(get_ChunkStatus(&b.con)==CHUNK_NO_STATUS&&get_ChunkStatus(&d.con)==CHUNK_NO_STATUS,"字段名和 DataVersion 对不上＝没有（不猜）");
		Chunk e(0,0);
		e.con.set_String("Status","minecraft:full");
		check_Case(get_ChunkDataVersion(&e.con)==0&&e.is_Full(),"没有 DataVersion 的区块按最老的格式");
	}
	/*---- 26.3快照7（5009）起的方块状态编码：字符串调色板、{id,properties}、{"":ID} ----*/
	{
		Chunk *c=new Chunk(0,0);
		c->con.set_Int("DataVersion",5009);
		c->con.set_String("Status","minecraft:full");
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte(0));
		nbt_Compound *bs=new nbt_Compound;
		nbt_List *pal=new nbt_List(NBT_STRING);
		pal->push_back(new nbt_String("minecraft:air"));
		pal->push_back(new nbt_String("minecraft:stone"));
		pal->push_back(new nbt_String("minecraft:dirt"));/*没人引用：压紧时要删*/
		bs->set("palette",pal);
		std::vector<int64_t> w(256,0);
		w[0]=1;/*(0,0,0)=stone，其余空气*/
		bs->set("data",new nbt_Long_Array(std::move(w)));
		sec->set("block_states",bs);
		nbt_List *secs=new nbt_List(NBT_COMPOUND);
		secs->push_back(sec);
		c->con.set("sections",secs);
		check_Case(check_Chunk(&c->con)&&check_ChunkBase(&c->con),"新格式：字符串调色板是合法形状");
		check_Case(c->get_Block(0,0,0).name()=="minecraft:stone"&&c->get_Block(1,0,0).name()=="minecraft:air","新格式：字符串条目读出方块");
		check_Case(has_Block(&c->con),"新格式：字符串调色板里有石头＝不是空壳");
		Block_Ref r;
		check_Case(c->get_BlockRef(0,0,0,&r)&&!strcmp(r.name,"minecraft:stone")&&!r.props,"新格式：Block_Ref 取名字，没有属性");
		const char *stone_name=r.name;
		int bad_y=0,cleaned=0;
		c->format_Chunk(&bad_y,&cleaned);
		pal=(nbt_List*)((nbt_Compound*)sec->get("block_states"))->get("palette");
		check_Case(cleaned==1&&pal->con.size()==2&&pal->elem==NBT_STRING,"新格式：压紧调色板后还是字符串列表");
		check_Case(c->get_Block(0,0,0).name()=="minecraft:stone","新格式：压紧后下标仍对");
		/*写一个带属性的方块：字符串列表要原地改成复合列表，原来的字符串包成{"":ID}*/
		Block slab("minecraft:oak_slab");
		slab.properties.set_String("type","top");
		check_Case(c->get_BlockRef(0,0,0,&r),"重新取一次名字指针");
		stone_name=r.name;
		check_Case(c->set_Block(slab,2,0,0),"新格式：往字符串调色板写带属性的方块");
		pal=(nbt_List*)((nbt_Compound*)sec->get("block_states"))->get("palette");
		check_Case(pal->elem==NBT_COMPOUND&&pal->con.size()==3,"字符串列表改成复合列表");
		const nbt_Any *wrapped=pal->con[1];
		check_Case(wrapped->type==NBT_COMPOUND&&((const nbt_Compound*)wrapped)->get("")&&!strcmp(get_StateName(wrapped,STATE_ID),"minecraft:stone"),
			"原来的字符串条目包成{\"\":ID}");
		check_Case(!strcmp(stone_name,"minecraft:stone")&&((const nbt_String*)get_StateNameTag(wrapped,STATE_ID))->con.c_str()==stone_name,
			"包的时候String节点是挪进去的：之前拿到的名字指针仍有效");
		const nbt_Any *ns=pal->con[2];
		check_Case(ns->type==NBT_COMPOUND&&((const nbt_Compound*)ns)->get("id")&&((const nbt_Compound*)ns)->get("properties")&&
			!((const nbt_Compound*)ns)->get("Name"),"新条目写成{id,properties}");
		Block back=c->get_Block(2,0,0);
		check_Case(back.name()=="minecraft:oak_slab"&&back.properties.get("type")&&back.properties.get("type")->as_String()=="top","带属性的方块读回");
		check_Case(c->get_Block(0,0,0).name()=="minecraft:stone"&&c->get_Block(5,0,5).name()=="minecraft:air","改列表类型后旧方块不变");
		/*复合列表里再写石头：复用{"":stone}，不重复加；写泥土：加成{"":dirt}*/
		check_Case(c->set_Block(Block("minecraft:stone"),3,0,0)&&pal->con.size()==3,"新格式：同一默认状态复用{\"\":ID}条目");
		check_Case(c->set_Block(Block("minecraft:dirt"),4,0,0)&&pal->con.size()==4,"新格式：复合列表里加默认状态");
		const nbt_Any *dirt=pal->con[3];
		check_Case(dirt->type==NBT_COMPOUND&&((const nbt_Compound*)dirt)->get("")&&!strcmp(get_StateName(dirt,STATE_ID),"minecraft:dirt"),
			"默认状态进复合列表包成{\"\":ID}");
		check_Case(c->get_Block(4,0,0).name()=="minecraft:dirt"&&check_Chunk(&c->con),"混合调色板读回、形状合法");
		/*{id} 只写名字也合法；新版本区块里出现老写法 {Name} 就是坏数据*/
		nbt_Compound *idonly=new nbt_Compound;
		idonly->set_String("id","minecraft:gold_block");
		pal->push_back(idonly);
		check_Case(check_Chunk(&c->con)&&!strcmp(get_StateName(idonly,STATE_ID),"minecraft:gold_block")&&!get_StateProps(idonly,STATE_ID),"{id}条目合法、没有属性");
		nbt_Compound *old=new nbt_Compound;
		old->set_String("Name","minecraft:stone");
		pal->push_back(old);
		check_Case(!check_Chunk(&c->con)&&check_ChunkBase(&c->con),"DataVersion>=5009 的区块里出现 {Name} 条目：认不出来（不按两种格式猜）；"
			"游戏换成空气、能载入，fixer 修，不算整块坏");
		pal->pop_back();
		/*老版本区块里出现字符串条目也是坏数据*/
		c->con.set_Int("DataVersion",5008);
		check_Case(!check_Chunk(&c->con),"DataVersion<5009 的区块里出现新写法＝坏数据");
		delete c;
	}
	/*---- 新格式的全空气段：字符串 "minecraft:air" 一条 ----*/
	{
		Chunk *c=new Chunk(0,0);
		c->con.set_Int("DataVersion",5119);
		c->con.set_String("status","minecraft:full");
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte(0));
		nbt_Compound *bs=new nbt_Compound;
		nbt_List *pal=new nbt_List(NBT_STRING);
		pal->push_back(new nbt_String("minecraft:air"));
		bs->set("palette",pal);
		sec->set("block_states",bs);
		nbt_List *secs=new nbt_List(NBT_COMPOUND);
		secs->push_back(sec);
		c->con.set("sections",secs);
		check_Case(c->is_Full()&&!has_Block(&c->con),"26.4：status 小写、全空气段＝空壳");
		const nbt_Any *k=NULL;
		check_Case(is_SectionConst(c,0,&k)&&k==NULL,"新格式：整段空气认得出");
		check_Case(c->set_Block(Block("minecraft:stone"),0,0,0)&&c->get_Block(0,0,0).name()=="minecraft:stone"&&pal->elem==NBT_STRING,
			"新格式：字符串列表里写默认状态还是字符串");
		delete c;
	}
	printf("\n%s：%d个用例，%d个失败\n",fails?"FAIL":"OK",cases,fails);
	return fails?1:0;
}
