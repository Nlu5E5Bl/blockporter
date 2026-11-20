#include "../nbtrw.hpp"
#include <sys/stat.h>
/*checkfix2.cpp：核对 mkfix2 + fixer（拆悬空、清区块、光/高、POI/计划刻、实体 UUID、坐标/重复段、format）。*/
/*---- 新接口的测试小工具：Chunk::read/MCA::get_Chunk 没了，读成 compound 再转 ----*/
static Chunk *get_MCAChunk(const MCA &m,uint32_t x,uint32_t z){
	std::optional<nbt_Compound> r=m.get_Compound(x,z);
	return r?new Chunk(std::move(*r)):NULL;
}
static nbt_Compound *get_MCACompound(const MCA &m,uint32_t x,uint32_t z){/*从区域镜像里读一格，放到堆上（调用者 free）；读不到给 NULL*/
	std::optional<nbt_Compound> r=m.get_Compound(x,z);
	return r?new nbt_Compound(std::move(*r)):NULL;
}
static const char *D="minecraft:overworld";
static bool has_PaletteName(const Chunk *c,const char *name){/*这个区块有没有哪一段的调色板里带着 name 这种方块（查调色板压没压紧用）*/
	const nbt_Any *sa=c?c->con.get("sections"):NULL;
	if(!sa||sa->type!=NBT_LIST){
		return false;
	}
	for(auto a:((const nbt_List*)sa)->con){
		if(!a||a->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *bs=((const nbt_Compound*)a)->get("block_states");
		if(!bs||bs->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *pa=((const nbt_Compound*)bs)->get("palette");
		if(!pa||pa->type!=NBT_LIST){
			continue;
		}
		for(auto e:((const nbt_List*)pa)->con){
			const char *n=e&&e->type==NBT_COMPOUND?get_PropValue((const nbt_Compound*)e,"Name"):NULL;
			if(n&&!strcmp(n,name)){
				return true;
			}
		}
	}
	return false;
}
static int count_Sections(const Chunk *c,int sy){/*这个区块里 Y==sy 的段有几个（查重复段删没删用）；没有 sections 给 -1*/
	const nbt_Any *sa=c?c->con.get("sections"):NULL;
	if(!sa||sa->type!=NBT_LIST){
		return -1;
	}
	int n=0;
	for(auto a:((const nbt_List*)sa)->con){
		if(!a||a->type!=NBT_COMPOUND){
			continue;
		}
		const nbt_Any *y=((const nbt_Compound*)a)->get("Y");
		if(y&&(y->type==NBT_BYTE||y->type==NBT_INT)&&y->as_Int()==sy){
			n++;
		}
	}
	return n;
}
int main(){/*mkfix2 造的世界让 fixer 跑过以后逐项核对；每项打一行，有不对的返回 1*/
	Level lv("test/work/fixtest",8,4,2);
	int bad=0;
	printf("torch=%s sand=%s\n",std::string(lv.get_Block(D,1,-62,1).name()).c_str(),std::string(lv.get_Block(D,2,-60,2).name()).c_str());
	if(lv.get_Block(D,1,-62,1).valid()&&lv.get_Block(D,1,-62,1).name()!="minecraft:air"){
		bad++;
	}
	if(lv.get_Block(D,2,-60,2).valid()&&lv.get_Block(D,2,-60,2).name()!="minecraft:air"){
		bad++;
	}
	/*霜冰→水源，并且只有一条 t=5/p=0 的水刻，原来的霜冰方块刻没了*/
	{
		Block b=lv.get_Block(D,1,-60,1);
		const nbt_Any *lvp=b.properties.get("level");
		printf("frost=%s level=%s\n",std::string(b.name()).c_str(),std::string(lvp?lvp->as_String().value_or("(缺)"):"(缺)").c_str());
		if(b.name()!="minecraft:water"||!lvp||lvp->as_String()!="0"){
			bad++;
		}
		Chunk *fc=lv.get_Chunk(D,0,0);
		int n=fc?count_ChunkTicks(&fc->con,true):-1;
		int got=0;
		for(int i=0;i<n;i++){
			Chunk_Tick t;
			if(get_ChunkTick(&fc->con,true,i,&t)&&t.x==1&&t.y==-60&&t.z==1){
				got++;
				printf("frost fluid_tick i=%s t=%d p=%d\n",std::string(t.type).c_str(),t.delay,t.priority);
				if(t.type!="minecraft:water"||t.delay!=5||t.priority!=0){
					bad++;
				}
			}
		}
		if(got!=1){
			bad++;
		}
		for(int i=0;i<(fc?count_ChunkTicks(&fc->con,false):0);i++){
			Chunk_Tick t;
			if(get_ChunkTick(&fc->con,false,i,&t)&&t.x==1&&t.y==-60&&t.z==1){
				printf("霜冰旧方块刻没删\n");
				bad++;
			}
		}
	}
	/*重力方块：下面是液体（岩浆/水）拆，栅栏上留*/
	printf("gravity lava=%s fence=%s water=%s\n",std::string(lv.get_Block(D,8,-60,8).name()).c_str(),
		std::string(lv.get_Block(D,9,-60,9).name()).c_str(),std::string(lv.get_Block(D,10,-60,10).name()).c_str());
	if(lv.get_Block(D,8,-60,8).name()!="minecraft:air"){
		bad++;/*下面岩浆：按新规则要拆*/
	}
	if(lv.get_Block(D,9,-60,9).name()!="minecraft:sand"){
		bad++;
	}
	if(lv.get_Block(D,10,-60,10).name()!="minecraft:air"){
		bad++;/*下面是水（liquid）：要拆*/
	}
	/*26.1.1补充规则*/
	printf("susp=%s leaf=%s lantern=%s\n",std::string(lv.get_Block(D,11,-60,11).name()).c_str(),
		std::string(lv.get_Block(D,12,-61,12).name()).c_str(),std::string(lv.get_Block(D,13,-60,13).name()).c_str());
	if(lv.get_Block(D,11,-60,11).name()!="minecraft:air"){
		bad++;/*可疑沙下方空气，按重力规则要拆*/
	}
	if(lv.get_Block(D,12,-61,12).name()!="minecraft:air"){
		bad++;/*落叶层要顶面完整（Wiki Leaf Litter 25w09a），栅栏不行*/
	}
	if(lv.get_Block(D,13,-60,13).name()!="minecraft:lantern"){
		bad++;/*龟蛋能挂灯笼*/
	}
	/*含水门拆成水：同样有且只有一条 t=5/p=0 的水刻，旧方块刻/流体刻清掉*/
	{
		Block b=lv.get_Block(D,4,-62,4);
		printf("waterlogged door=%s level=%s\n",std::string(b.name()).c_str(),
			std::string(b.properties.get("level")?b.properties.get("level")->as_String().value_or("(缺)"):"(缺)").c_str());
		if(b.name()!="minecraft:water"||!b.properties.get("level")||b.properties.get("level")->as_String()!="0"){
			bad++;
		}
		Chunk *dc=lv.get_Chunk(D,0,0);
		int nf=dc?count_ChunkTicks(&dc->con,true):-1,got=0;
		for(int i=0;i<nf;i++){
			Chunk_Tick t;
			if(get_ChunkTick(&dc->con,true,i,&t)&&t.x==4&&t.y==-62&&t.z==4){
				got++;
				if(t.type!="minecraft:water"||t.delay!=5||t.priority!=0){
					bad++;
				}
			}
		}
		if(got!=1){
			bad++;
		}
		for(int i=0;i<(dc?count_ChunkTicks(&dc->con,false):0);i++){
			Chunk_Tick t;
			if(get_ChunkTick(&dc->con,false,i,&t)&&t.x==4&&t.y==-62&&t.z==4){
				bad++;
			}
		}
	}
	Chunk *c=lv.get_Chunk(D,0,0);
	printf("chunk00=%p IT=%lld Heightmaps=%p isLightOn=%p ticks=%d/%d\n",(void*)c,
		(long long)(c&&c->con.get("InhabitedTime")?c->con.get("InhabitedTime")->as_Long().value_or(-1):-1),
		(void*)(c?c->con.get("Heightmaps"):NULL),(void*)(c?c->con.get("isLightOn"):NULL),
		c?count_ChunkTicks(&c->con,false):-1,c?count_ChunkTicks(&c->con,true):-1);
	if(!c){
		bad++;
	}
	if(c&&c->con.get("Heightmaps")){
		bad++;
	}
	if(c&&c->con.get("isLightOn")&&c->con.get("isLightOn")->as_Int()!=0){
		bad++;
	}
	if(c){
		const nbt_Any *sa=c->con.get("sections");
		if(sa&&sa->type==NBT_LIST){
			for(auto a:((nbt_List*)sa)->con){
				if(a&&a->type==NBT_COMPOUND){
					if(((nbt_Compound*)a)->get("BlockLight")||((nbt_Compound*)a)->get("SkyLight")){
						bad++;
					}
				}
			}
		}
	}
	printf("chunk10=%p chunk01=%p chunk11=%p\n",(void*)lv.get_Chunk(D,1,0),(void*)lv.get_Chunk(D,0,1),(void*)lv.get_Chunk(D,1,1));
	if(lv.get_Chunk(D,1,0)||lv.get_Chunk(D,0,1)||lv.get_Chunk(D,1,1)){
		bad++;
	}
	/*xPos/zPos按放置位置改写*/
	{
		Chunk *c2=lv.get_Chunk(D,2,0);
		const nbt_Any *xp=c2?c2->con.get("xPos"):NULL;
		const nbt_Any *zp=c2?c2->con.get("zPos"):NULL;
		printf("chunk20=%p xPos=%d zPos=%d block=%s\n",(void*)c2,
			xp?xp->as_Int().value_or(-9999):-9999,zp?zp->as_Int().value_or(-9999):-9999,
			std::string(lv.get_Block(D,34,-64,2).name()).c_str());
		if(!c2||!xp||!zp||xp->as_Int()!=2||zp->as_Int()!=0){
			bad++;
		}
		if(lv.get_Block(D,34,-64,2).name()!="minecraft:gold_block"){
			bad++;
		}
	}
	/*同Y重复段被删到只剩一段（留最后那段，游戏取的就是它；mkfix2 的金块写在最后那段里）*/
	{
		Chunk *c3=lv.get_Chunk(D,3,0);
		const nbt_Any *xp=c3?c3->con.get("xPos"):NULL;
		const nbt_Any *zp=c3?c3->con.get("zPos"):NULL;
		int n=count_Sections(c3,-4);
		printf("chunk30=%p xPos=%d zPos=%d 同Y=-4段数=%d\n",(void*)c3,
			xp?xp->as_Int().value_or(-9999):-9999,zp?zp->as_Int().value_or(-9999):-9999,n);
		if(!c3||!xp||!zp||xp->as_Int()!=3||zp->as_Int()!=0){
			bad++;
		}
		if(n!=1){
			bad++;
		}
		if(lv.get_Block(D,50,-64,2).name()!="minecraft:gold_block"){
			bad++;
		}
	}
	/*床/堆肥桶的POI：床去重保留free_tickets=0，堆肥桶缺失要补farmer/1；火把那条要删*/
	{
		MCA m("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		nbt_Compound *p=get_MCACompound(m,0,0);
		int n=p?count_POI(p):0;
		int bed=0,comp=0;
		const nbt_Any *dv=p?p->get("DataVersion"):NULL;
		printf("POI=%d DataVersion=%d\n",n,dv?dv->as_Int().value_or(-1):-1);
		if(!dv||dv->as_Int()!=4788){
			bad++;
		}
		std::vector<POI> all;
		if(p&&list_POI(p,&all)!=0){
			bad++;
		}
		for(int i=0;i<(int)all.size();i++){
			int32_t x=all[i].x,y=all[i].y,z=all[i].z,ft=all[i].free_tickets;
			std::string ts(all[i].name());
			printf("POI[%d]=%s %d %d %d ft=%d\n",i,ts.c_str(),x,y,z,ft);
			if(x==5&&y==-64&&z==5){
				bed++;
				if(ts!="minecraft:home"||ft!=0){
					bad++;
				}
			}
			if(x==7&&y==-63&&z==7){
				comp++;
				if(ts!="minecraft:farmer"||ft!=1){
					bad++;
				}
			}
			if(x==1&&y==-62&&z==1){
				bad++;
			}
		}
		if(bed!=1||comp!=1){
			bad++;
		}
		if(p){
			p->free();
		}
	}
	/*同一位置两条合法方块刻：保留p最小、p相同t最大的那条；类型对不上的删掉*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		int n=c?count_ChunkTicks(&c->con,false):-1;
		int bed=0;
		for(int i=0;i<n;i++){
			Chunk_Tick t;
			if(get_ChunkTick(&c->con,false,i,&t)&&t.x==5&&t.y==-64&&t.z==5){
				bed++;
				printf("bed tick=%s t=%d p=%d\n",std::string(t.type).c_str(),t.delay,t.priority);
				if(t.type!="minecraft:red_bed"||t.delay!=12||t.priority!=0){
					bad++;
				}
			}
		}
		if(bed!=1){
			bad++;
		}
	}
	/*中低优先级Wiki规则*/
	printf("wiki plate=%s glow=%s amethyst=%s kelp=%s torch=%s scaf7=%s bell_keep=%s bell_drop=%s am_bud=%s am_on_bud=%s am_on_slab=%s\n",
		std::string(lv.get_Block(D,129,101,1).name()).c_str(),
		std::string(lv.get_Block(D,132,100,1).name()).c_str(),
		std::string(lv.get_Block(D,134,101,1).name()).c_str(),
		std::string(lv.get_Block(D,136,101,1).name()).c_str(),
		std::string(lv.get_Block(D,138,101,1).name()).c_str(),
		std::string(lv.get_Block(D,140,100,1).name()).c_str(),
		std::string(lv.get_Block(D,139,102,1).name()).c_str(),
		std::string(lv.get_Block(D,141,102,1).name()).c_str(),
		std::string(lv.get_Block(D,143,100,1).name()).c_str(),
		std::string(lv.get_Block(D,143,101,1).name()).c_str(),
		std::string(lv.get_Block(D,137,100,1).name()).c_str());
	if(lv.get_Block(D,129,101,1).name()!="minecraft:air"){
		bad++;/*压力板不能放树叶上*/
	}
	if(lv.get_Block(D,132,100,1).name()!="minecraft:air"){
		bad++;/*地衣要solid面，栅栏不算*/
	}
	if(lv.get_Block(D,134,101,1).name()!="minecraft:air"){
		bad++;/*紫水晶不能长树叶上*/
	}
	if(lv.get_Block(D,143,100,1).name()!="minecraft:small_amethyst_bud"){
		bad++;/*芽本身在紫水晶块上，保留*/
	}
	if(lv.get_Block(D,143,101,1).name()!="minecraft:air"){
		bad++;/*簇不能长在薄薄的芽上*/
	}
	if(lv.get_Block(D,137,100,1).name()!="minecraft:amethyst_cluster"){
		bad++;/*上半砖顶面是完整面，保留*/
	}
	if(lv.get_Block(D,136,101,1).name()!="minecraft:water"){
		bad++;/*海带要full top face，漏斗不算；删了留水*/
	}
	if(lv.get_Block(D,138,101,1).name()!="minecraft:torch"){
		bad++;/*栅栏门顶上照样给中心支撑（22w43a 去掉的是底下的）*/
	}
	if(lv.get_Block(D,140,100,1).name()!="minecraft:air"){
		bad++;/*scaffolding distance=7无支撑要掉*/
	}
	if(lv.get_Block(D,139,102,1).name()!="minecraft:bell"){
		bad++;/*墙钟 facing 那一侧（北）有支撑，保留*/
	}
	if(lv.get_Block(D,141,102,1).name()!="minecraft:air"){
		bad++;/*只在 facing 反面（南）有块，不算支撑（BellBlock.canSurvive）*/
	}
	{/*大箱子的左半另一半没了：变单箱，箱子里的东西（方块实体）照留*/
		Block ch=lv.get_Block(D,130,100,3);
		const char *t=get_PropValue(&ch.properties,"type");
		const nbt_Any *items=ch.block_entities.get("Items");
		printf("chest=%s type=%s items=%s\n",std::string(ch.name()).c_str(),t?t:"?",items?"有":"没有");
		if(ch.name()!="minecraft:chest"||!t||strcmp(t,"single")||!items){
			bad++;
		}
	}
	/*缺p的坏计划刻只删单条，区块还在*/
	{
		Chunk *tc=lv.get_Chunk(D,0,0);
		int n=tc?count_ChunkTicks(&tc->con,false):-1;
		for(int i=0;i<n;i++){
			Chunk_Tick t;
			if(get_ChunkTick(&tc->con,false,i,&t)&&t.x==6&&t.y==-64&&t.z==6){
				printf("坏计划刻没删\n");
				bad++;
			}
		}
	}
	/*Y不是Byte：照游戏的读法改成Byte（Int 100→Byte 100），子区块和方块都还在*/
	{
		Chunk *yc=lv.get_Chunk(D,4,0);
		const nbt_Compound *ys=yc?yc->get_Section(100):NULL;
		const nbt_Any *yt=ys?ys->get("Y"):NULL;
		printf("chunk40=%p block=%s Y=100段数=%d Y类型=%d\n",(void*)yc,std::string(lv.get_Block(D,66,-64,2).name()).c_str(),
			count_Sections(yc,100),yt?yt->type:-1);
		if(!yc){
			bad++;
		}
		if(lv.get_Block(D,66,-64,2).name()!="minecraft:gold_block"){
			bad++;
		}
		if(count_Sections(yc,100)!=1||!yt||yt->type!=NBT_BYTE){
			bad++;
		}
	}
	/*废弃调色板条目被压紧，区块还在*/
	{
		Chunk *pc=lv.get_Chunk(D,6,0);
		printf("chunk60=%p unused_palette=%d\n",(void*)pc,has_PaletteName(pc,"minecraft:unused_palette_test")?1:0);
		if(!pc||has_PaletteName(pc,"minecraft:unused_palette_test")){
			bad++;
		}
	}
	/*sections 不是 List 的区块：游戏静默当没有段（不算坏），整块就是空气，按“全空气”删掉*/
	if(lv.get_Chunk(D,0,2)){
		bad++;
	}
	/*没有 InhabitedTime 的区块：游戏 getLongOr 当0（用户 2026-11-07：读不到当0删）*/
	{
		Chunk *ic=lv.get_Chunk(D,2,1);
		printf("chunk21=%p（期望已删：缺 InhabitedTime 当0）\n",(void*)ic);
		if(ic){
			bad++;
		}
	}
	/*InhabitedTime=0 + y 范围外多一段（只有光照的、有方块的各一块）：删区块是区块级的，不看 y，两块都删*/
	{
		Chunk *lc=lv.get_Chunk(D,3,1);
		Chunk *sc=lv.get_Chunk(D,4,1);
		printf("chunk31=%s chunk41=%s（期望都删了：删区块不看 y）\n",lc?"在":"没了",sc?"在":"没了");
		if(lc||sc){
			bad++;
		}
	}
	/*坐标指到别的区块的方块实体/计划刻（都放在(10,1)里）：迁到坐标所在的区块*/
	{
		auto get_Marker=[](const nbt_Compound *be){/*方块实体上做的记号（没有给空串）*/
			const nbt_Any *m=be?be->get("marker"):NULL;
			return m&&m->type==NBT_STRING?((const nbt_String*)m)->con:std::string();
		};
		Chunk *c0=lv.get_Chunk(D,0,0);
		const nbt_Compound *moved=c0?c0->get_BlockEntity(14,-50,14):NULL;
		bool tick=false;
		for(int i=0;c0&&i<count_ChunkTicks(&c0->con,true);i++){
			Chunk_Tick t;
			if(get_ChunkTick(&c0->con,true,i,&t)&&t.x==14&&t.y==-50&&t.z==12&&t.type=="minecraft:water"&&t.delay==7){
				tick=true;
			}
		}
		printf("迁移：(14,-50,14) 的方块实体=%s（期望 moved） 流体刻=%s（期望迁到(0,0)）\n",get_Marker(moved).c_str(),tick?"在":"没有");
		if(get_Marker(moved)!="moved"||!tick){
			bad++;
		}
		Block ch=lv.get_Block(D,130,100,3);
		printf("迁移：(130,100,3) 原来的箱子 marker=%s Items=%s（期望没被顶掉）\n",get_Marker(&ch.block_entities).c_str(),ch.block_entities.get("Items")?"有":"没有");
		if(!get_Marker(&ch.block_entities).empty()||!ch.block_entities.get("Items")){
			bad++;
		}
		Chunk *c2=lv.get_Chunk(D,2,0);
		const nbt_Compound *fur=c2?c2->get_BlockEntity(5,-50,5):NULL;
		printf("迁移：挪过位置的区块(2,0) 里的熔炉=%s x=%d z=%d（期望 with_chunk 37 5）\n",get_Marker(fur).c_str(),
			fur?get_TagInt(fur->get("x")).value_or(0):0,fur?get_TagInt(fur->get("z")).value_or(0):0);
		if(get_Marker(fur)!="with_chunk"||get_TagInt(fur->get("x"))!=37||get_TagInt(fur->get("z"))!=5){
			bad++;
		}
		Chunk *c12=lv.get_Chunk(D,12,1);
		const nbt_Compound *fur2=c12?c12->get_BlockEntity(3,-50,3):NULL;
		const nbt_Any *x12=c12?c12->con.get("xPos"):NULL;
		printf("迁移：挪过位置的区块(12,1) xPos=%d 熔炉=%s x=%d z=%d（期望 12、with_chunk2 195 19：有别的区块的记录指向它也照样认出它挪过）\n",
			get_TagInt(x12).value_or(-1),get_Marker(fur2).c_str(),
			fur2?get_TagInt(fur2->get("x")).value_or(0):0,fur2?get_TagInt(fur2->get("z")).value_or(0):0);
		if(get_TagInt(x12)!=12||get_Marker(fur2)!="with_chunk2"||get_TagInt(fur2->get("x"))!=195||get_TagInt(fur2->get("z"))!=19){
			bad++;
		}
	}
	/*区块(10,1)：游戏读档报错的按最小单位删，不报的不动*/
	{
		Chunk *c=lv.get_Chunk(D,10,1);
		printf("chunk(10,1)=%p\n",(void*)c);
		if(!c){
			bad++;
		}else{
			const nbt_Compound *s5=c->get_Section(5),*s6=c->get_Section(6);
			const nbt_Any *pal6=s6&&s6->get("block_states")?((const nbt_Compound*)s6->get("block_states"))->get("palette"):NULL;
			printf("  Y=5 block_states=%s Y=6 palette[0]=%s bogus=%s\n",s5&&s5->get("block_states")?"在":"删了",
				pal6&&pal6->type==NBT_LIST?get_StateName(((const nbt_List*)pal6)->con[0],STATE_NAME):"?",c->con.get("bogus")?"在":"删了");
			if(!s5||s5->get("block_states")||!s6||!pal6||strcmp(get_StateName(((const nbt_List*)pal6)->con[0],STATE_NAME),"minecraft:air")){
				bad++;
			}
			if(c->con.get("bogus")){
				bad++;
			}
			if(c->con.get("block_ticks")){
				bad++;
			}
			int nf=count_ChunkTicks(&c->con,true);
			Chunk_Tick t;
			printf("  block_ticks=%s fluid_ticks=%d\n",c->con.get("block_ticks")?"在":"删了",nf);
			/*x 是 String 的那条删了：表里每一条游戏都读得进来（count 不是 -1）；Short 坐标那条格子不是水，
			  计划刻阶段按“刻和方块对不上”删掉是正常的（Short 照读在 lib.cpp 里单测）*/
			(void)t;
			if(nf<0){
				bad++;
			}
			const nbt_Any *ba=c->con.get("block_entities");
			const nbt_List *bl=ba&&ba->type==NBT_LIST?(const nbt_List*)ba:NULL;
			/*留下三条：坐标在范围外的原样留着，目标区块位置也不对的原样留着，没有坐标的折回本区块；没 id 的、迁走的、目标没有的、目标已有的都不在了*/
			bool out=false,nopos=false,wrongpos=false;
			for(size_t k=0;bl&&k<bl->con.size();k++){
				const nbt_Compound *be=(const nbt_Compound*)bl->con[k];
				const nbt_Any *m=be->get("marker");
				std::string mk=m&&m->type==NBT_STRING?((const nbt_String*)m)->con:std::string();
				int32_t bx=get_TagInt(be->get("x")).value_or(-1),bz=get_TagInt(be->get("z")).value_or(-1);
				if(mk=="out"&&bx==3&&bz==5000){
					out=true;
				}
				if(mk=="nopos"&&bx==160&&bz==16){
					nopos=true;
				}
				if(mk=="wrongpos"&&bx==198&&bz==22){
					wrongpos=true;
				}
			}
			printf("  block_entities=%zu 条（期望 3：范围外的原样=%d、目标位置不对的原样=%d、没坐标的折回(160,16)=%d）\n",
				bl?bl->con.size():(size_t)0,out,wrongpos,nopos);
			if(!bl||bl->con.size()!=3||!out||!wrongpos||!nopos){
				bad++;
			}
			const nbt_Any *sa=c->con.get("structures");
			const nbt_Compound *refs=sa&&sa->type==NBT_COMPOUND?(const nbt_Compound*)((const nbt_Compound*)sa)->get("References"):NULL;
			const nbt_Compound *starts=sa&&sa->type==NBT_COMPOUND?(const nbt_Compound*)((const nbt_Compound*)sa)->get("starts"):NULL;
			const nbt_Any *vp=refs?refs->get("minecraft:village_plains"):NULL;
			printf("  References=%zu 项（村庄 %zu 个坐标） starts=%zu 项\n",refs?refs->size():(size_t)0,
				vp&&vp->type==NBT_LONG_ARRAY?((const nbt_Long_Array*)vp)->size():(size_t)0,starts?starts->size():(size_t)0);
			if(!refs||refs->size()!=1||!vp||((const nbt_Long_Array*)vp)->size()!=1||!starts||starts->size()!=1||!starts->get("minecraft:mineshaft")){
				bad++;
			}
		}
		MCA pm("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		std::optional<nbt_Compound> pc=pm.get_Compound(10,1);
		const nbt_Any *ps=pc?pc->get("Sections"):NULL;
		printf("  POI(10,1)=%s 段5=%s\n",pc?"在":"没了",ps&&ps->type==NBT_COMPOUND&&((const nbt_Compound*)ps)->get("5")?"在":"删了");
		if(ps&&ps->type==NBT_COMPOUND&&((const nbt_Compound*)ps)->get("5")){
			bad++;
		}
		MCA em("test/work/fixtest/dimensions/minecraft/overworld/entities/r.0.0.mca");
		std::optional<nbt_Compound> ec=em.get_Compound(10,1);
		const nbt_Any *pos=ec?ec->get("Position"):NULL;
		const nbt_Any *ea=ec?ec->get("Entities"):NULL;
		const nbt_List *el=ea&&ea->type==NBT_LIST?(const nbt_List*)ea:NULL;
		const nbt_Any *pas=el&&el->con.size()==2?((const nbt_Compound*)el->con[1])->get("Passengers"):NULL;
		printf("  实体(10,1) Position=%s 实体 %zu 个，马的乘客 %zu 个\n",
			pos&&pos->type==NBT_INT_ARRAY&&((const nbt_Int_Array*)pos)->con==std::vector<int32_t>{10,1}?"改回(10,1)":"不对",
			el?el->con.size():(size_t)0,pas&&pas->type==NBT_LIST?((const nbt_List*)pas)->con.size():(size_t)0);
		if(!pos||pos->type!=NBT_INT_ARRAY||((const nbt_Int_Array*)pos)->con!=std::vector<int32_t>{10,1}){
			bad++;
		}
		if(!el||el->con.size()!=2||!pas||pas->type!=NBT_LIST||((const nbt_List*)pas)->con.size()!=1){
			bad++;
		}
	}
	/*压缩/NBT坏掉的区块整块删除，表头不能还留着；不支持的压缩类型必须保留*/
	{
		MCA m("test/work/fixtest/dimensions/minecraft/overworld/region/r.0.0.mca");
		if(m.has_Chunk(0,3)){
			bad++;
		}
		if(!m.has_Chunk(1,2)){
			bad++;
		}
		Chunk *uc=get_MCAChunk(m,1,2);
		if(uc||m.last_error!=MCA_READ_UNSUPPORTED_COMPRESSION){
			bad++;
		}
	}
	{
		MCA m("test/work/fixtest/dimensions/minecraft/overworld/entities/r.0.0.mca");
		nbt_Compound *e=get_MCACompound(m,0,0);
		const nbt_Any *es=e?e->get("Entities"):NULL;
		int n=es&&es->type==NBT_LIST?(int)((nbt_List*)es)->con.size():-1;
		printf("entities=%d\n",n);
		if(n!=1){
			bad++;
		}
		if(n==1){/*UUID重复：保留先出现的那条（Pos.x=1.5），不是末尾的2.5*/
			const nbt_Any*e0=((const nbt_List*)es)->con[0];
			const nbt_Any*p0=e0&&e0->type==NBT_COMPOUND?((const nbt_Compound*)e0)->get("Pos"):NULL;
			double x=p0&&p0->type==NBT_LIST&&((const nbt_List*)p0)->con.size()>=1?
				((const nbt_List*)p0)->con[0]->as_Double().value_or(-1):-1;
			printf("kept entity Pos.x=%.1f（期望1.5）\n",x);
			if(x!=1.5){
				bad++;
			}
		}
		if(e){
			e->free();
		}
	}
	/*region没有的区块：POI/实体都不应该有*/
	{
		MCA pm("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		MCA em("test/work/fixtest/dimensions/minecraft/overworld/entities/r.0.0.mca");
		printf("orphan poi=%d entity=%d\n",pm.has_Chunk(5,0)?1:0,em.has_Chunk(5,0)?1:0);
		if(pm.has_Chunk(5,0)||em.has_Chunk(5,0)){
			bad++;
		}
	}
	/*0字节poi/entities文件都被删掉*/
	{
		struct stat st;
		if(!stat("test/work/fixtest/dimensions/minecraft/overworld/poi/r.1.0.mca",&st)){
			bad++;
		}
		if(!stat("test/work/fixtest/dimensions/minecraft/overworld/entities/r.1.0.mca",&st)){
			bad++;
		}
	}
	/*DataVersion 和规则的不一样的区块：规则不判、不改*/
	{
		if(lv.get_Block(D,100,-60,40).name()!="minecraft:sand"){
			printf("FAIL: 版本和规则不同的区块里，悬空沙子被拆了\n");
			bad++;
		}
		Chunk *c=lv.get_Chunk(D,6,2);
		if(!c||count_ChunkTicks(&c->con,false)!=1||get_ChunkDataVersion(&c->con)!=4325){
			printf("FAIL: 版本和规则不同的区块里，计划刻/DataVersion 被改了\n");
			bad++;
		}
		MCA pm("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		std::optional<nbt_Compound> po=pm.get_Compound(6,2);
		if(!po||count_POI(&*po)!=1||get_TagInt(po->get("DataVersion")).value_or(0)!=4325){
			printf("FAIL: DataVersion 4325 的 POI 区块被改了\n");
			bad++;
		}
	}
	lv.close();
	{/*表头(20,3)那一格偏移异常：收尾整理时清成 0；同格的 POI 按“region 里没有这个区块”删掉*/
		MCA rm("test/work/fixtest/dimensions/minecraft/overworld/region/r.0.0.mca");
		MCA pm("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		size_t loc=4*(20+3*32);
		bool zero=rm.data.size()>=8192&&!(rm.data[loc]|rm.data[loc+1]|rm.data[loc+2]|rm.data[loc+3]);
		printf("偏移异常的格子(20,3)：表头=%s POI=%s（期望清成0、POI 删了）\n",zero?"清成0":"还在",pm.has_Chunk(20,3)?"在":"删了");
		if(!zero||pm.has_Chunk(20,3)){
			bad++;
		}
	}
	{/*范围外一律不动（用户 2026-11-21）：(-1,0) 挨着范围里的 (0,0)，光照、isLightOn、高度图都要原样留着
	   （以前“3×3 邻居删光照”会改它）*/
		MCA m("test/work/fixtest/dimensions/minecraft/overworld/region/r.-1.0.mca");
		std::optional<nbt_Compound> o=m.get_Compound(31,0);
		const nbt_Any *on=o?o->get("isLightOn"):NULL,*sa=o?o->get("sections"):NULL;
		const nbt_Any *bl=sa&&sa->type==NBT_LIST&&!((const nbt_List*)sa)->con.empty()?
			((const nbt_Compound*)((const nbt_List*)sa)->con[0])->get("BlockLight"):NULL;
		bool keep=o&&on&&on->as_Int()==1&&bl&&o->get("Heightmaps");
		printf("范围外的邻居(-1,0)：%s（期望 isLightOn=1、BlockLight 和 Heightmaps 都在）\n",keep?"原样":"被改了");
		if(!keep){
			bad++;
		}
	}
	return bad?1:0;
}
