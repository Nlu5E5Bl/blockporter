#include "../nbtrw.hpp"
#include <cmath>
#include <stdlib.h>
/*mkfix2.cpp：造 fixer 的合成世界（悬空方块、未完成/全空气/InhabitedTime=0 区块、POI、重复 UUID）。*/
static const char *D="minecraft:overworld";
static bool set_BlockAt(Level &lv,int x,int y,int z,const char *n){/*主世界里放一个只有名字的方块；放不进去打一行*/
	bool ok=lv.set_Block(D,x,y,z,Block(n));
	if(!ok){
		printf("set %d %d %d %s 失败\n",x,y,z,n);
	}
	return ok;
}
int main(){/*把 test/data/lvlworld 拷成 test/work/fixtest，再往里造各种该修/不该修的情况（每块上面都有说明）*/
	system("rm -rf test/work/fixtest && cp -a test/data/lvlworld test/work/fixtest");
	Level lv("test/work/fixtest",16,4,4);
	/*悬空火把：先把下面换成空气，再放火把（火把放下时下面可以是石头，放下后再抽掉）*/
	if(!set_BlockAt(lv,1,-63,1,"minecraft:stone")||!set_BlockAt(lv,1,-62,1,"minecraft:torch")||
	   !set_BlockAt(lv,1,-63,1,"minecraft:air"))return 1;
	/*悬空沙子*/
	if(!set_BlockAt(lv,2,-61,2,"minecraft:air")||!set_BlockAt(lv,2,-60,2,"minecraft:sand")){
		return 1;
	}
	/*霜冰：fixer 换成水源时要补一条 fluid_ticks(t=5,p=0)，旧方块刻要删掉*/
	if(!set_BlockAt(lv,1,-60,1,"minecraft:frosted_ice")){
		return 1;
	}
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c||!add_ChunkTick(&c->con,false,"minecraft:frosted_ice",1,-60,1,17,0)){
			return 1;
		}
		if(!lv.set_Chunk(D,0,0,*c)){
			return 1;
		}
	}
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c){
			return 1;
		}
		if(!add_ChunkTick(&c->con,false,"minecraft:torch",1,-62,1,20,0)){
			return 1;
		}
		c->con.set_Long("InhabitedTime",100);
		if(!lv.set_Chunk(D,0,0,*c)){
			return 1;
		}
	}
	/*重力方块（FallingBlock.isFree）：下面是岩浆/栅栏/水。岩浆和水是液体，拆；栅栏托得住*/
	{
		if(!set_BlockAt(lv,8,-61,8,"minecraft:lava")||!set_BlockAt(lv,8,-60,8,"minecraft:sand")){
			return 1;
		}
		if(!set_BlockAt(lv,9,-62,9,"minecraft:stone")||!set_BlockAt(lv,9,-61,9,"minecraft:oak_fence")||
		   !set_BlockAt(lv,9,-60,9,"minecraft:sand"))return 1;
		if(!set_BlockAt(lv,10,-61,10,"minecraft:water")||!set_BlockAt(lv,10,-60,10,"minecraft:sand")){
			return 1;
		}
	}
	/*26.1.1 Wiki 的几条补充规则：可疑沙要掉、栅栏上的落叶层要留、龟蛋下的灯笼要留*/
	{
		if(!set_BlockAt(lv,11,-61,11,"minecraft:air")||!set_BlockAt(lv,11,-60,11,"minecraft:suspicious_sand")){
			return 1;
		}
		/*栅栏上的落叶层：要顶面完整（25w09a），fixer 要拆*/
		if(!set_BlockAt(lv,12,-62,12,"minecraft:oak_fence")||!set_BlockAt(lv,12,-61,12,"minecraft:leaf_litter")){
			return 1;
		}
		if(!set_BlockAt(lv,13,-59,13,"minecraft:turtle_egg")){
			return 1;
		}
		Block lan("minecraft:lantern");
		lan.properties.set_String("hanging","true");
		if(!lv.set_Block(D,13,-60,13,lan)){
			return 1;
		}
	}
	/*POI 补录/去重和计划刻去重：一张床（已有重复home记录，free=1和0）、一个没有POI记录的堆肥桶；
	  床上再放同一方块的两条合法计划刻（p相同，t不同）和一条类型对不上的计划刻。*/
	{
		Block bed("minecraft:red_bed");
		bed.properties.set_String("part","head");
		bed.properties.set_String("facing","north");
		if(!lv.set_Block(D,5,-64,5,bed)){
			printf("bed fail\n");
			return 1;
		}
		Block foot("minecraft:red_bed");/*床脚在床头 facing 的反面（南）：只放半张床会被规则当成残床拆掉*/
		foot.properties.set_String("part","foot");
		foot.properties.set_String("facing","north");
		if(!lv.set_Block(D,5,-64,6,foot)){
			printf("bed foot fail\n");
			return 1;
		}
		/*含水门：下面抽空后规则会把它拆成水源，必须补fluid_ticks(t=5,p=0)*/
		Block door("minecraft:oak_door");
		door.properties.set_String("half","lower");
		door.properties.set_String("facing","north");
		door.properties.set_String("hinge","left");
		door.properties.set_String("open","false");
		door.properties.set_String("waterlogged","true");
		if(!set_BlockAt(lv,4,-63,4,"minecraft:air")||!lv.set_Block(D,4,-62,4,door)){
			printf("door fail\n");
			return 1;
		}
		if(!set_BlockAt(lv,7,-64,7,"minecraft:stone")||!set_BlockAt(lv,7,-63,7,"minecraft:composter")){
			printf("comp fail\n");
			return 1;
		}
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c){
			printf("chunk fail\n");
			return 1;
		}
		if(!add_ChunkTick(&c->con,false,"minecraft:red_bed",5,-64,5,20,1)){
			printf("tick fail\n");
			return 1;
		}
		nbt_List *tl=(nbt_List*)c->con.get("block_ticks");
		if(!tl){
			printf("tick list fail\n");
			return 1;
		}
		for(int k=0;k<3;k++){
			nbt_Compound *e=new nbt_Compound;
			e->set_String("i",k==2?"minecraft:stone":"minecraft:red_bed");
			e->set_Int("x",5);e->set_Int("y",-64);e->set_Int("z",5);
			e->set_Int("t",k==0?10:(k==1?12:1));e->set_Int("p",0);
			if(!tl->push_back(e)){
				printf("dup tick fail\n");
				return 1;
			}
		}
		if(!add_ChunkTick(&c->con,false,"minecraft:oak_door",4,-62,4,30,0)||
		   !add_ChunkTick(&c->con,true,"minecraft:water",4,-62,4,30,0)){printf("door tick fail\n");return 1;}
		/*缺p的坏计划刻：只删这一条，不能整块删*/
		{
			nbt_Compound *e=new nbt_Compound;
			e->set_String("i","minecraft:stone");
			e->set_Int("x",6);e->set_Int("y",-64);e->set_Int("z",6);e->set_Int("t",5);
			if(!tl->push_back(e)){
				printf("bad tick fail\n");
				return 1;
			}
		}
		if(!lv.set_Chunk(D,0,0,*c)){
			printf("setchunk fail\n");
			return 1;
		}
	}
	/*sections 类型不对的区块：游戏静默当没有段（不算坏）；整块是空气，fixer 按全空气删*/
	{
		Chunk bad(0,2);
		bad.con.set_Int("DataVersion",4788);
		bad.con.set_String("Status","minecraft:full");
		bad.con.set_Long("InhabitedTime",5);
		bad.con.set("sections",new nbt_Int(1));
		if(!lv.set_Chunk(D,0,2,bad)){
			printf("badchunk fail\n");
			return 1;
		}
	}
	/*一个没生成完的区块*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c){
			return 1;
		}
		Chunk nc(*c);
		nc.con.set_Int("xPos",1);
		nc.con.set_Int("zPos",0);
		nc.con.set("Status",new nbt_String("minecraft:features"));
		if(!lv.set_Chunk(D,1,0,nc)){
			return 1;
		}
	}
	/*区块的 DataVersion（4325＝1.21.5，没升级的老区块）和规则的（4788）不一样：规则不判、不改——
	  悬空沙子留着，和方块对不上的计划刻也留着*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c){
			return 1;
		}
		Chunk nc(*c);
		nc.con.set_Int("xPos",6);
		nc.con.set_Int("zPos",2);
		nc.con.set_Long("InhabitedTime",100);
		nc.con.erase("block_ticks");
		nc.con.erase("fluid_ticks");
		if(!nc.set_Block(Block("minecraft:air"),4,-61,8)||!nc.set_Block(Block("minecraft:sand"),4,-60,8)){
			return 1;
		}
		if(!add_ChunkTick(&nc.con,false,"minecraft:torch",100,-60,40,20,0)){
			return 1;
		}
		nc.con.set_Int("DataVersion",4325);
		if(!lv.set_Chunk(D,6,2,nc)){
			return 1;
		}
	}
	/*一个全空气、生成完、InhabitedTime=0 的区块（整块在范围内）*/
	{
		Chunk nc(0,1);
		nc.con.set_Int("DataVersion",4788);
		nc.con.set_String("Status","minecraft:full");
		nc.con.set_Long("InhabitedTime",0);
		nbt_List *secs=new nbt_List(NBT_COMPOUND);
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte((int8_t)-4));
		nbt_Compound *bs=new nbt_Compound;
		nbt_List *pal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *air=new nbt_Compound;air->set_String("Name","minecraft:air");
		pal->push_back(air);bs->set("palette",pal);sec->set("block_states",bs);
		nbt_Compound *bio=new nbt_Compound;nbt_List *bp=new nbt_List(NBT_STRING);
		bp->push_back(new nbt_String("minecraft:plains"));bio->set("palette",bp);sec->set("biomes",bio);
		secs->push_back(sec);nc.con.set("sections",secs);
		if(!lv.set_Chunk(D,0,1,nc)){
			return 1;
		}
	}
	/*一个有方块、full、InhabitedTime=0 的区块（整块在范围内）*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",1);
		nc.con.set_Int("zPos",1);
		nc.con.set_Long("InhabitedTime",0);
		if(!lv.set_Chunk(D,1,1,nc)){
			return 1;
		}
	}
	/*有方块、full、但没有InhabitedTime字段：游戏当0，fixer 按 InhabitedTime=0 删*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",2);
		nc.con.set_Int("zPos",1);
		nc.con.erase("InhabitedTime");
		if(!lv.set_Chunk(D,2,1,nc)){
			return 1;
		}
	}
	/*InhabitedTime=0，另外多一段范围外（Y=-5，block.sh 的范围是 y -64..319）只有光照的段：照删
	  （删区块是区块级的，x/z 碰到就整块看，y 不管；1.17 及以前每个区块都带这种光照段）*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",3);
		nc.con.set_Int("zPos",1);
		nc.con.set_Long("InhabitedTime",0);
		nbt_Compound *light=new nbt_Compound;
		light->set("Y",new nbt_Byte((int8_t)-5));
		light->set("BlockLight",new nbt_Byte_Array(std::vector<int8_t>(2048,0)));
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		if(!secs||secs->type!=NBT_LIST||!secs->push_back(light)){
			return 1;
		}
		if(!lv.set_Chunk(D,3,1,nc)){
			return 1;
		}
	}
	/*同上，但范围外那一段有方块：也照删（用户 2026-11-14：y 的限制只对方块级的操作有效，删区块不看 y）*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",4);
		nc.con.set_Int("zPos",1);
		nc.con.set_Long("InhabitedTime",0);
		nbt_Compound *low=new nbt_Compound;
		low->set("Y",new nbt_Byte((int8_t)-5));
		nbt_Compound *bs=new nbt_Compound;
		nbt_List *pal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *stone=new nbt_Compound;
		stone->set_String("Name","minecraft:stone");
		pal->push_back(stone);
		bs->set("palette",pal);
		low->set("block_states",bs);
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		if(!secs||secs->type!=NBT_LIST||!secs->push_back(low)){
			return 1;
		}
		if(!lv.set_Chunk(D,4,1,nc)){
			return 1;
		}
	}
	/*一个压缩类型读不了的区块（改成 127 自定义）：fixer只能跳过，不能删*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",1);
		nc.con.set_Int("zPos",2);
		nc.con.set_Long("InhabitedTime",5);
		if(!lv.set_Chunk(D,1,2,nc)){
			return 1;
		}
	}
	/*一个NBT/压缩坏掉但表头还在的区块：fixer要整块删*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",0);
		nc.con.set_Int("zPos",3);
		nc.con.set_Long("InhabitedTime",5);
		if(!nc.set_Block(Block("minecraft:gold_block"),1,-64,1)){
			return 1;
		}
		if(!lv.set_Chunk(D,0,3,nc)){
			return 1;
		}
	}
	/*xPos/zPos 和文件位置对不上：fixer 要按放置位置改回来，不能跳过*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",99);
		nc.con.set_Int("zPos",-99);
		nc.con.set_Long("InhabitedTime",5);
		if(!nc.set_Block(Block("minecraft:gold_block"),2,-64,2)){
			return 1;
		}
		/*一条方块实体，坐标在区块自己说的位置（99,-99）里：整个区块挪到(2,0)以后它跟着走 → (37,-50,5)，不算“指到别的区块”*/
		{
			nbt_Compound *fur=new nbt_Compound;
			fur->set_String("id","minecraft:furnace");
			fur->set_String("marker","with_chunk");
			fur->set_Int("x",99*16+5);fur->set_Int("y",-50);fur->set_Int("z",-99*16+5);
			nbt_Any *ba=nc.con.get("block_entities");
			if(!ba||ba->type!=NBT_LIST||!((nbt_List*)ba)->push_back(fur)){
				return 1;
			}
			nc.reindex();
		}
		if(!lv.set_Chunk(D,2,0,nc)){
			return 1;
		}
		/*(12,1) 也挪过位置（xPos/zPos 写成 77,77），里面一条跟着它挪的熔炉；(10,1) 里还有一条方块实体指向 (12,1)。
		  (10,1) 先修：目标 (12,1) 的位置还不对，那条留在 (10,1)（下一趟再迁）；轮到 (12,1) 时熔炉照样跟着折回 (195,-50,19)。
		  2026-11-21 以前迁移时先在缓存里把 (12,1) 的位置改了，熔炉就被当成“指到别处”、留在 (77,77) 那个坐标上*/
		Chunk w(nc);
		w.con.set_Int("xPos",77);
		w.con.set_Int("zPos",77);
		nbt_List *wb=new nbt_List(NBT_COMPOUND);
		nbt_Compound *wf=new nbt_Compound;
		wf->set_String("id","minecraft:furnace");
		wf->set_String("marker","with_chunk2");
		wf->set_Int("x",77*16+3);wf->set_Int("y",-50);wf->set_Int("z",77*16+3);
		wb->push_back(wf);
		w.con.set("block_entities",wb);
		w.reindex();
		if(!lv.set_Chunk(D,12,1,w)){
			return 1;
		}
		/*(-1,0) 在范围外（block.sh 的范围是 x 0..1023，它在另一个区域文件 r.-1.0 里），挨着范围里的 (0,0)：范围外一律不动，光照、高度图也不删
		  （用户 2026-11-21；以前“3×3 邻居删光照”会把它的光照删掉、isLightOn 改成 0）*/
		Chunk o(nc);
		o.con.set_Int("xPos",-1);
		o.con.set_Int("zPos",0);
		o.con.erase("block_entities");
		o.con.set("isLightOn",new nbt_Byte(1));
		o.con.set("Heightmaps",new nbt_Compound);
		nbt_Any *osa=o.con.get("sections");
		if(!osa||osa->type!=NBT_LIST||((nbt_List*)osa)->con.empty()){
			return 1;
		}
		((nbt_Compound*)((nbt_List*)osa)->con[0])->set("BlockLight",new nbt_Byte_Array(std::vector<int8_t>(2048,0x11)));
		o.reindex();
		if(!lv.set_Chunk(D,-1,0,o)){
			return 1;
		}
	}
	/*同一Y两段：fixer 要删掉前面那段（游戏取最后一段），不能原样写回；set_Block 写的也是最后那段*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",3);
		nc.con.set_Int("zPos",0);
		nc.con.set_Long("InhabitedTime",7);
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		if(!secs||secs->type!=NBT_LIST||secs->con.empty()){
			return 1;
		}
		if(!secs->push_back(secs->con[0]->copy())){
			return 1;
		}
		if(!nc.set_Block(Block("minecraft:gold_block"),2,-64,2)){
			return 1;
		}
		if(!lv.set_Chunk(D,3,0,nc)){
			return 1;
		}
	}
	/*Y不是Byte的子区块：fixer 照游戏的读法改成Byte，不删*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",4);
		nc.con.set_Int("zPos",0);
		nc.con.set_Long("InhabitedTime",5);
		if(!nc.set_Block(Block("minecraft:gold_block"),2,-64,2)){
			return 1;
		}
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		nbt_Compound *badsec=new nbt_Compound;
		badsec->set("Y",new nbt_Int(100));
		if(!secs->push_back(badsec)){
			return 1;
		}
		if(!lv.set_Chunk(D,4,0,nc)){
			return 1;
		}
	}
	/*调色板里塞一条没被data引用的条目：fixer要压紧掉，区块保留*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",6);
		nc.con.set_Int("zPos",0);
		nc.con.set_Long("InhabitedTime",5);
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		nbt_Compound *s0=secs&&!secs->con.empty()?(nbt_Compound*)secs->con[0]:NULL;
		nbt_Compound *bs=s0?(nbt_Compound*)s0->get("block_states"):NULL;
		nbt_List *pal=bs?(nbt_List*)bs->get("palette"):NULL;
		if(!pal){
			return 1;
		}
		nbt_Compound *unused=new nbt_Compound;
		unused->set_String("Name","minecraft:unused_palette_test");
		if(!pal->push_back(unused)){
			return 1;
		}
		if(!lv.set_Chunk(D,6,0,nc)){
			return 1;
		}
	}
	/*中低优先级Wiki规则：压力板下的树叶、栅栏上的地衣、树叶上的紫水晶、漏斗上的海带、
	  栅栏门上的火把、distance=7的脚手架、墙钟只认背面支撑*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",8);
		nc.con.set_Int("zPos",0);
		nc.con.set_Long("InhabitedTime",5);
		if(!lv.set_Chunk(D,8,0,nc)){
			return 1;
		}
	}
	{
		Block plate("minecraft:oak_pressure_plate");
		if(!set_BlockAt(lv,129,100,1,"minecraft:oak_leaves")||!set_BlockAt(lv,129,101,1,"minecraft:air")||
		   !lv.set_Block(D,129,101,1,plate))return 1;
		Block gl("minecraft:glow_lichen");gl.properties.set_String("west","true");
		if(!set_BlockAt(lv,131,100,1,"minecraft:oak_fence")||!set_BlockAt(lv,132,100,1,"minecraft:air")||
		   !lv.set_Block(D,132,100,1,gl))return 1;
		Block am("minecraft:amethyst_cluster");am.properties.set_String("facing","up");
		if(!set_BlockAt(lv,134,100,1,"minecraft:oak_leaves")||!set_BlockAt(lv,134,101,1,"minecraft:air")||
		   !lv.set_Block(D,134,101,1,am))return 1;
		/*紫水晶簇的支撑是另一枚芽（薄片）：支持集里虽列了芽，也必须按完整面拒绝*/
		Block am_bud("minecraft:small_amethyst_bud");am_bud.properties.set_String("facing","up");
		Block am_top("minecraft:amethyst_cluster");am_top.properties.set_String("facing","up");
		if(!set_BlockAt(lv,143,99,1,"minecraft:amethyst_block")||!set_BlockAt(lv,143,100,1,"minecraft:air")||
		   !lv.set_Block(D,143,100,1,am_bud)||!set_BlockAt(lv,143,101,1,"minecraft:air")||
		   !lv.set_Block(D,143,101,1,am_top))return 1;
		/*上半砖的顶面是完整面：紫水晶应保留，不能把“完整面”误做成“整方块”*/
		Block am_slab("minecraft:amethyst_cluster");am_slab.properties.set_String("facing","up");
		Block top_slab("minecraft:oak_slab");top_slab.properties.set_String("type","top");
		if(!set_BlockAt(lv,137,99,1,"minecraft:air")||!lv.set_Block(D,137,99,1,top_slab)||
		   !set_BlockAt(lv,137,100,1,"minecraft:air")||!lv.set_Block(D,137,100,1,am_slab))return 1;
		if(!set_BlockAt(lv,136,100,1,"minecraft:hopper")||!set_BlockAt(lv,136,101,1,"minecraft:air")||
		   !lv.set_Block(D,136,101,1,Block("minecraft:kelp")))return 1;
		/*栅栏门顶上的火把：22w43a 只去掉了栅栏门底下的中心支撑，顶上照样能放（Opacity/Placement）*/
		if(!set_BlockAt(lv,138,100,1,"minecraft:oak_fence_gate")||!set_BlockAt(lv,138,101,1,"minecraft:air")||
		   !lv.set_Block(D,138,101,1,Block("minecraft:torch")))return 1;
		Block sc7("minecraft:scaffolding"),sc6("minecraft:scaffolding");
		sc7.properties.set_String("distance","7");sc7.properties.set_String("bottom","true");
		sc6.properties.set_String("distance","6");sc6.properties.set_String("bottom","false");
		if(!set_BlockAt(lv,140,99,1,"minecraft:air")||!set_BlockAt(lv,141,99,1,"minecraft:air")||
		   !lv.set_Block(D,140,100,1,sc7)||!lv.set_Block(D,141,100,1,sc6))return 1;
		Block bell("minecraft:bell");
		bell.properties.set_String("attachment","single_wall");
		bell.properties.set_String("facing","north");
		if(!set_BlockAt(lv,139,102,0,"minecraft:stone")||!set_BlockAt(lv,139,102,2,"minecraft:stone")||
		   !set_BlockAt(lv,139,102,1,"minecraft:air")||!lv.set_Block(D,139,102,1,bell))return 1;
		if(!set_BlockAt(lv,141,102,0,"minecraft:air")||!set_BlockAt(lv,141,102,2,"minecraft:stone")||
		   !set_BlockAt(lv,141,102,1,"minecraft:air")||!lv.set_Block(D,141,102,1,bell))return 1;
		/*大箱子的左半（facing=north，另一半该在东边）、东边是空气：游戏把它改成单箱（ChestBlock.updateShape），箱子里的东西照留*/
		Block ch("minecraft:chest");
		ch.properties.set_String("facing","north");
		ch.properties.set_String("type","left");
		ch.properties.set_String("waterlogged","false");
		ch.block_entities.set_String("id","minecraft:chest");
		nbt_List *items=new nbt_List(NBT_COMPOUND);
		nbt_Compound *it=new nbt_Compound;
		it->set_String("id","minecraft:diamond");
		it->set_Int("count",3);
		it->set("Slot",new nbt_Byte(0));
		items->push_back(it);
		ch.block_entities.set("Items",items);
		if(!set_BlockAt(lv,131,100,3,"minecraft:air")||!set_BlockAt(lv,130,100,3,"minecraft:air")||!lv.set_Block(D,130,100,3,ch)){
			return 1;
		}
	}
	/*给(10,1)里指过来的方块实体/流体刻准备落脚的格子：(14,-50,14) 一个没有方块实体的箱子，(14,-50,12) 一格水*/
	{
		Block ch("minecraft:chest");
		ch.properties.set_String("facing","north");
		ch.properties.set_String("type","single");
		ch.properties.set_String("waterlogged","false");
		Block water("minecraft:water");
		water.properties.set_String("level","0");
		if(!lv.set_Block(D,14,-50,14,ch)||!lv.set_Block(D,14,-50,12,water)){
			return 1;
		}
		Chunk *c=lv.get_Chunk(D,0,0);
		if(!c||c->get_BlockEntity(14,-50,14)){
			return 1;
		}
	}
	/*区块(10,1)：什么都坏一点（用户 2026-11-07：游戏读档报 ERROR/WARN 的按最小单位删，不报的不动）*/
	{
		Chunk *c=lv.get_Chunk(D,0,0);
		Chunk nc(*c);
		nc.con.set_Int("xPos",10);
		nc.con.set_Int("zPos",1);
		nc.con.set_Long("InhabitedTime",5);
		nc.con.erase("block_entities");
		nc.con.erase("block_ticks");
		nc.con.erase("fluid_ticks");
		nbt_List *secs=(nbt_List*)nc.con.get("sections");
		/*Y=5：两条调色板、data 少一个 long → 游戏整块载入失败；fixer 删这一段的 block_states*/
		{
			nbt_Compound *sec=new nbt_Compound;
			sec->set("Y",new nbt_Byte(5));
			nbt_Compound *bs=new nbt_Compound;
			nbt_List *pal=new nbt_List(NBT_COMPOUND);
			nbt_Compound *a=new nbt_Compound;a->set_String("Name","minecraft:air");pal->push_back(a);
			nbt_Compound *g=new nbt_Compound;g->set_String("Name","minecraft:gold_block");pal->push_back(g);
			bs->set("palette",pal);
			bs->set("data",new nbt_Long_Array(std::vector<int64_t>(255,0x1111111111111111LL)));
			sec->set("block_states",bs);
			secs->push_back(sec);
		}
		/*Y=6：调色板一条认不出名字的 → 游戏换成空气（ERROR）；fixer 换成空气条目*/
		{
			nbt_Compound *sec=new nbt_Compound;
			sec->set("Y",new nbt_Byte(6));
			nbt_Compound *bs=new nbt_Compound;
			nbt_List *pal=new nbt_List(NBT_COMPOUND);
			nbt_Compound *e=new nbt_Compound;e->set_String("Foo","bar");pal->push_back(e);
			bs->set("palette",pal);
			sec->set("block_states",bs);
			secs->push_back(sec);
		}
		nc.con.set("block_ticks",new nbt_String("garbage"));/*不是 List：ERROR → 删成员*/
		nbt_List *ft=new nbt_List(NBT_COMPOUND);
		{
			nbt_Compound *t=new nbt_Compound;/*坐标是 Short：Codec.INT 照读 → 留着*/
			t->set_String("i","minecraft:water");
			t->set("x",new nbt_Short(163));t->set("y",new nbt_Short(-60));t->set("z",new nbt_Short(19));
			t->set_Int("t",5);t->set_Int("p",0);
			ft->push_back(t);
			nbt_Compound *b=new nbt_Compound;/*x 是 String：读不进来 → 删这一条*/
			b->set_String("i","minecraft:water");
			b->set_String("x","163");b->set_Int("y",-60);b->set_Int("z",19);b->set_Int("t",5);b->set_Int("p",0);
			ft->push_back(b);
		}
		{
			nbt_Compound *t=new nbt_Compound;/*坐标指到(0,0)的流体刻（游戏静默丢掉）：迁到(0,0)，那一格是水、对得上，留着*/
			t->set_String("i","minecraft:water");
			t->set_Int("x",14);t->set_Int("y",-50);t->set_Int("z",12);t->set_Int("t",7);t->set_Int("p",0);
			ft->push_back(t);
		}
		nc.con.set("fluid_ticks",ft);
		nbt_List *bes=new nbt_List(NBT_COMPOUND);
		{
			nbt_Compound *noid=new nbt_Compound;/*没有 id：Skipping block entity with invalid type → 删*/
			noid->set_Int("x",161);noid->set_Int("y",-64);noid->set_Int("z",17);
			bes->push_back(noid);
			/*坐标指到别的区块的方块实体（游戏 WARN、折回本区块）：迁到坐标所在的区块（用户 2026-11-14）*/
			struct{const char *marker;int32_t x,y,z;}far[5]={
				{"moved",14,-50,14},   /*目标(0,0)那一格没有方块实体 → 迁过去*/
				{"dup",130,100,3},     /*目标(8,0)那一格已经有箱子的方块实体 → 留那里原来的，这条删*/
				{"lost",641,-50,49},   /*目标(40,3)没有这个区块 → 删*/
				{"out",3,-50,5000},    /*坐标在范围外（方块级，只读不写）→ 原样留着*/
				{"wrongpos",198,-50,22}};/*目标(12,1)自己的 xPos/zPos 也不对、还没轮到修 → 原样留着*/
			for(int k=0;k<5;k++){
				nbt_Compound *e=new nbt_Compound;
				e->set_String("id","minecraft:chest");
				e->set_String("marker",far[k].marker);
				e->set_Int("x",far[k].x);e->set_Int("y",far[k].y);e->set_Int("z",far[k].z);
				bes->push_back(e);
			}
			nbt_Compound *nopos=new nbt_Compound;/*根本没有 x/z（游戏当 0、折回本区块）：没有坐标可信，照游戏折回 → (160,-50,16)*/
			nopos->set_String("id","minecraft:barrel");
			nopos->set_String("marker","nopos");
			nopos->set_Int("y",-50);
			bes->push_back(nopos);
		}
		nc.con.set("block_entities",bes);
		nbt_Compound *st=new nbt_Compound;
		nbt_Compound *refs=new nbt_Compound;
		/*References：指到 8 个区块以外的那个坐标丢掉（WARN）；名字写法不合法的整项丢掉*/
		refs->set("minecraft:village_plains",new nbt_Long_Array(std::vector<int64_t>{
			(int64_t)((uint64_t)(uint32_t)10|((uint64_t)(uint32_t)1<<32)),(int64_t)((uint64_t)(uint32_t)30|((uint64_t)(uint32_t)1<<32))}));
		refs->set("Bad Key",new nbt_Long_Array());
		st->set("References",refs);
		nbt_Compound *starts=new nbt_Compound;
		starts->set("BAD",new nbt_Compound);
		starts->set("minecraft:mineshaft",new nbt_Compound);
		st->set("starts",starts);
		nc.con.set("structures",st);
		nc.con.set("bogus",new nbt_String(std::string("\xFF")));/*readUTF 读不了 → 整块读失败；fixer 删这个 tag*/
		if(!lv.set_Chunk(D,10,1,nc)){
			return 1;
		}
	}
	/*POI：一个指向火把的 home（固定会被删）*/
	{
		system("mkdir -p test/work/fixtest/dimensions/minecraft/overworld/poi");
		MCA m;
		nbt_Compound *p=new nbt_Compound;
		p->set_Int("DataVersion",4788);/*游戏写的 POI 都有；没有的游戏当 1945、先升级，fixer 不动*/
		if(!set_POI(p,1,-62,1,POI("minecraft:home",1))){
			return 1;
		}
		/*床：同一位置两条合法home，修完只留free_tickets最小的一条（0）*/
		if(!set_POI(p,5,-64,5,POI("minecraft:home",1))){
			printf("poi bed fail\n");
			return 1;
		}
		{
			nbt_Any *sa=p->get("Sections");
			nbt_Any *s=sa&&sa->type==NBT_COMPOUND?((nbt_Compound*)sa)->get("-4"):NULL;
			nbt_Any *r=s&&s->type==NBT_COMPOUND?((nbt_Compound*)s)->get("Records"):NULL;
			nbt_List *recs=r&&r->type==NBT_LIST?(nbt_List*)r:NULL;
			if(!recs||recs->con.empty()){
				printf("recs fail\n");
				return 1;
			}
			nbt_Any *dup=recs->con.back()->copy();
			((nbt_Compound*)dup)->set_Int("free_tickets",0);
			if(!recs->push_back(dup)){
				printf("dup push fail\n");
				return 1;
			}
			/*再塞一条pos只有2个int的坏记录：fixer要丢坏记录、重建POI块，但不能把合法home的free_tickets=0冲掉*/
			nbt_Compound *bad=new nbt_Compound;
			bad->set_String("type","minecraft:home");
			nbt_Int_Array *bp=new nbt_Int_Array;bp->push_back(5);bp->push_back(-64);
			bad->set("pos",bp);bad->set_Int("free_tickets",1);
			if(!recs->push_back(bad)){
				printf("bad rec push fail\n");
				return 1;
			}
		}
		/*(6,2) 的 POI DataVersion 4325（和规则的不一样）：指向空气的记录也不动*/
		{
			nbt_Compound po;
			po.set_Int("DataVersion",4325);
			if(!set_POI(&po,6*16+2,-50,2*16+3,POI("minecraft:home",1))){
				return 1;
			}
			if(!m.set_Compound(po,6,2)){
				return 1;
			}
		}
		/*region里没有(5,0)这个区块，但POI里有：fixer要删掉这条POI（不新建region）*/
		if(!set_POI(p,5*16+1,-64,1,POI("minecraft:home",1))){
			return 1;
		}
		m.set_Compound(*p,5,0);
		/*(10,1)：一段没有 Records（游戏 ERROR、整段不要）→ fixer 删这一段*/
		{
			nbt_Compound pb;
			pb.set_Int("DataVersion",4788);
			nbt_Compound *ps=new nbt_Compound;
			nbt_Compound *s5=new nbt_Compound;
			s5->set("Valid",new nbt_Byte(1));
			ps->set("5",s5);
			pb.set("Sections",ps);
			m.set_Compound(pb,10,1);
		}
		/*堆肥桶故意不写POI，fixer要按rules补farmer/1*/
		m.set_Compound(*p,0,0);
		if(!m.save("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca")){
			return 1;
		}
		p->free();
		/*0字节POI文件：跑完fixer不应留下*/
		FILE *zf=fopen("test/work/fixtest/dimensions/minecraft/overworld/poi/r.1.0.mca","wb");
		if(!zf||fclose(zf)){
			return 1;
		}
	}
	/*实体：两条同 UUID*/
	{
		system("mkdir -p test/work/fixtest/dimensions/minecraft/overworld/entities");
		MCA m;
		nbt_Compound *e=new nbt_Compound;
		nbt_Int_Array *pos=new nbt_Int_Array;pos->push_back(0);pos->push_back(0);
		e->set("Position",pos);e->set_Int("DataVersion",4788);
		nbt_List *es=new nbt_List(NBT_COMPOUND);
		for(int k=0;k<2;k++){
			nbt_Compound *one=new nbt_Compound;
			one->set_String("id","minecraft:pig");
			nbt_Int_Array *u=new nbt_Int_Array;u->push_back(1);u->push_back(2);u->push_back(3);u->push_back(4);
			one->set("UUID",u);
			nbt_List *pp=new nbt_List(NBT_DOUBLE);pp->push_back(new nbt_Double(1.5+k));pp->push_back(new nbt_Double(-64.0));pp->push_back(new nbt_Double(1.5));
			one->set("Pos",pp);es->push_back(one);
		}
		/*UUID 用 Long_Array 写的同一个 UUID（游戏 Codec.INT_STREAM 照读）：也是重复，删。以前只认 Int_Array/数值列表，查不出来*/
		{
			nbt_Compound *one=new nbt_Compound;
			one->set_String("id","minecraft:pig");
			one->set("UUID",new nbt_Long_Array(std::vector<int64_t>{1,2,3,4}));
			nbt_List *pp=new nbt_List(NBT_DOUBLE);pp->push_back(new nbt_Double(3.5));pp->push_back(new nbt_Double(-64.0));pp->push_back(new nbt_Double(1.5));
			one->set("Pos",pp);es->push_back(one);
		}
		/*一条信息不全的实体（缺id）：只删这条记录，不能删整块*/
		{
			nbt_Compound *one=new nbt_Compound;
			nbt_Int_Array *u=new nbt_Int_Array;u->push_back(9);u->push_back(8);u->push_back(7);u->push_back(6);
			one->set("UUID",u);
			nbt_List *pp=new nbt_List(NBT_DOUBLE);pp->push_back(new nbt_Double(2.5));pp->push_back(new nbt_Double(-64.0));pp->push_back(new nbt_Double(2.5));
			one->set("Pos",pp);es->push_back(one);
		}
		e->set("Entities",es);
		m.set_Compound(*e,0,0);
		/*(10,1)：Position 对不上（ERROR、实体照载）→ 按文件位置重写；没有 UUID 的留着；Pos 有 NaN 的删；
		  坏乘客只删那个乘客*/
		{
			nbt_Compound ec;
			ec.set("Position",new nbt_Int_Array(std::vector<int32_t>{9,9}));
			ec.set_Int("DataVersion",4788);
			nbt_List *el=new nbt_List(NBT_COMPOUND);
			auto mk_Pos=[](double x,double y,double z){
				nbt_List *pp=new nbt_List(NBT_DOUBLE);
				pp->push_back(new nbt_Double(x));pp->push_back(new nbt_Double(y));pp->push_back(new nbt_Double(z));
				return pp;
			};
			nbt_Compound *nouuid=new nbt_Compound;
			nouuid->set_String("id","minecraft:cow");
			nouuid->set("Pos",mk_Pos(162.5,-63.0,18.5));
			el->push_back(nouuid);
			nbt_Compound *nan=new nbt_Compound;
			nan->set_String("id","minecraft:cow");
			nan->set("Pos",mk_Pos(162.5,std::nan(""),18.5));
			el->push_back(nan);
			nbt_Compound *rider=new nbt_Compound;
			rider->set_String("id","minecraft:horse");
			rider->set("Pos",mk_Pos(164.5,-63.0,20.5));
			nbt_List *ps=new nbt_List(NBT_COMPOUND);
			nbt_Compound *ok=new nbt_Compound;ok->set_String("id","minecraft:zombie");ps->push_back(ok);
			nbt_Compound *bad=new nbt_Compound;bad->set_String("name","no id");ps->push_back(bad);
			rider->set("Passengers",ps);
			el->push_back(rider);
			ec.set("Entities",el);
			m.set_Compound(ec,10,1);
		}
		/*region里没有(5,0)这个区块，但实体文件里有：fixer要删掉这块实体*/
		nbt_Compound *orphan=new nbt_Compound;
		nbt_Int_Array *op=new nbt_Int_Array;op->push_back(5);op->push_back(0);
		orphan->set("Position",op);orphan->set_Int("DataVersion",4788);
		orphan->set("Entities",new nbt_List(NBT_COMPOUND));
		m.set_Compound(*orphan,5,0);
		orphan->free();
		if(!m.save("test/work/fixtest/dimensions/minecraft/overworld/entities/r.0.0.mca")){
			return 1;
		}
		e->free();
		/*0字节实体文件：跑完fixer不应留下*/
		FILE *zf=fopen("test/work/fixtest/dimensions/minecraft/overworld/entities/r.1.0.mca","wb");
		if(!zf||fclose(zf)){
			return 1;
		}
	}
	if(!lv.save()){
		return 1;
	}
	lv.close();
	/*把(0,3)的压缩类型改成不认识的0x7f：表头还有，但区块读不出来*/
	{
		std::string f="test/work/fixtest/dimensions/minecraft/overworld/region/r.0.0.mca";
		std::vector<byte> d;
		if(!read_File(f.c_str(),d)||d.size()<8192){
			return 1;
		}
		size_t loc=4*(0+3*32);
		size_t pos=((size_t)d[loc]<<16|(size_t)d[loc+1]<<8|d[loc+2])*4096;
		if(!pos||pos+5>d.size()){
			return 1;
		}
		d[pos+4]=0x02;/*zlib；再把压缩数据打坏，让解压失败→BAD_NBT/坏压缩，fixer删*/
		d[pos+5]=0xff;d[pos+6]=0xff;
		/*另一个区块只改压缩类型为127(模组的自定义压缩)：本库读不了，fixer必须跳过、保留表头*/
		size_t loc2=4*(1+2*32);
		size_t pos2=((size_t)d[loc2]<<16|(size_t)d[loc2+1]<<8|d[loc2+2])*4096;
		if(!pos2||pos2+5>d.size()){
			return 1;
		}
		d[pos2+4]=0x7F;
		/*表头(20,3)那一格偏移异常（偏移 1 指在表头里；游戏打开文件时 WARN、当这一格没有）：fixer 遍历时跳过它，
		  同格的 POI 按“region 里没有这个区块”删掉，收尾整理时把这一格清成 0*/
		size_t loc3=4*(20+3*32);
		if(d[loc3]|d[loc3+1]|d[loc3+2]|d[loc3+3]){
			return 1;
		}
		d[loc3+2]=1;d[loc3+3]=1;
		FILE *fp=fopen(f.c_str(),"wb");
		if(!fp||fwrite(d.data(),1,d.size(),fp)!=d.size()||fclose(fp)){
			return 1;
		}
	}
	{
		MCA pm("test/work/fixtest/dimensions/minecraft/overworld/poi/r.0.0.mca");
		nbt_Compound pc;
		pc.set_Int("DataVersion",4788);
		if(pm.bad||!set_POI(&pc,20*16+1,-50,3*16+1,POI("minecraft:home",1))||!pm.set_Compound(pc,20,3)||!pm.save()){
			return 1;
		}
	}
	printf("fixer测试世界造好了\n");
	return 0;
}
