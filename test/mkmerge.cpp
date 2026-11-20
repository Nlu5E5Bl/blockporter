#include "../nbtrw.hpp"
#include <stdlib.h>
#include <stdio.h>
/*mkmerge.cpp：造 merge 的合成世界（多 old、now==new、now==old、空气、方块实体、高段、计划刻、POI/实体）。*/
static const char *D="minecraft:overworld";
/*把某格区块的压缩类型改成type：用来造“目标POI/实体块读不出来但文件表头还在”的用例。
  只改已存在的sector类型字节，不动数据；merge 必须跳过而不是覆盖。*/
static bool corrupt_Type(const char *path,int lx,int lz,unsigned char type){
	std::vector<byte> d;
	if(!read_File(path,d)||d.size()<8192){
		return false;
	}
	size_t loc=4*((size_t)lx+(size_t)lz*32);
	size_t pos=((size_t)d[loc]<<16|(size_t)d[loc+1]<<8|d[loc+2])*4096;
	if(!pos||pos+5>d.size()){
		return false;
	}
	d[pos+4]=type;
	FILE *fp=fopen(path,"wb");
	if(!fp){
		return false;
	}
	bool ok=fwrite(d.data(),1,d.size(),fp)==d.size()&&fclose(fp)==0;
	return ok;
}
static void set_BlockAt(Level &lv,int x,int y,int z,const char *name){/*主世界里放一个只有名字的方块；放不进去直接退出*/
	if(!lv.set_Block(D,x,y,z,Block(name))){
		printf("set %d %d %d %s 失败\n",x,y,z,name);
		exit(1);
	}
}
int main(){/*把 test/data/lvlworld 拷成 new/now/old1/old2 四份，再各自改出要测的差别*/
	system("rm -rf test/work/merge && mkdir -p test/work/merge");
	system("cp -a test/data/lvlworld test/work/merge/new");
	system("cp -a test/data/lvlworld test/work/merge/now");
	system("cp -a test/data/lvlworld test/work/merge/old1");
	system("cp -a test/data/lvlworld test/work/merge/old2");
	Level now("test/work/merge/now",4,2,2),nw("test/work/merge/new",4,2,2),o1("test/work/merge/old1",4,2,2),o2("test/work/merge/old2",4,2,2);
	set_BlockAt(now,1,-64,1,"minecraft:diamond_block");set_BlockAt(nw,1,-64,1,"minecraft:stone");set_BlockAt(o1,1,-64,1,"minecraft:stone");set_BlockAt(o2,1,-64,1,"minecraft:stone");
	set_BlockAt(now,2,-64,2,"minecraft:gold_block");set_BlockAt(nw,2,-64,2,"minecraft:gold_block");set_BlockAt(o1,2,-64,2,"minecraft:stone");set_BlockAt(o2,2,-64,2,"minecraft:stone");
	set_BlockAt(now,3,-64,3,"minecraft:iron_block");set_BlockAt(nw,3,-64,3,"minecraft:emerald_block");set_BlockAt(o1,3,-64,3,"minecraft:iron_block");set_BlockAt(o2,3,-64,3,"minecraft:iron_block");
	set_BlockAt(now,4,-64,4,"minecraft:air");set_BlockAt(nw,4,-64,4,"minecraft:stone");set_BlockAt(o1,4,-64,4,"minecraft:stone");set_BlockAt(o2,4,-64,4,"minecraft:stone");
	set_BlockAt(now,5,-64,5,"minecraft:diamond_block");set_BlockAt(nw,5,-64,5,"minecraft:stone");set_BlockAt(o1,5,-64,5,"minecraft:stone");set_BlockAt(o2,5,-64,5,"minecraft:diamond_block");
	{
		Block b("minecraft:chest");
		b.block_entities.set_String("id","minecraft:chest");
		b.block_entities.set("Items",new nbt_List(NBT_COMPOUND));
		if(!now.set_Block(D,6,-64,6,b)){
			printf("放箱子失败\n");
			return 1;
		}
		set_BlockAt(nw,6,-64,6,"minecraft:stone");set_BlockAt(o1,6,-64,6,"minecraft:stone");set_BlockAt(o2,6,-64,6,"minecraft:stone");
	}
	{
		Chunk *c=now.get_Chunk(D,0,0);
		if(!c||!add_ChunkTick(&c->con,false,"minecraft:redstone_wire",1,-64,1,10,0)){
			printf("加刻失败\n");return 1;
		}
		now.set_Chunk(D,0,0,*c);
	}
	{
		Chunk *c=nw.get_Chunk(D,0,0);
		c->con.set_Long("InhabitedTime",777);
		/*merge 按约定不清理非法数据：new 里故意留一段重复Y，merge 后应该原样还在*/
		nbt_List *secs=(nbt_List*)c->con.get("sections");
		if(!secs||secs->type!=NBT_LIST||secs->con.empty()){
			printf("new sections坏\n");
			return 1;
		}
		if(!secs->push_back(secs->con[0]->copy())){
			printf("加重复段失败\n");
			return 1;
		}
		/*new 去掉 Y=12 这一段（底图里有）：now 在这一段放的方块 merge 不补段、要跳过*/
		for(size_t i=secs->con.size();i>0;i--){
			if(secs->con[i-1]->type==NBT_COMPOUND&&read_SectionY((nbt_Compound*)secs->con[i-1])==12){
				secs->erase(i-1);
			}
		}
		nw.set_Chunk(D,0,0,*c);
	}

	/*now 多一个高段：new 里没有 section(Y=12)，merge 不补段，这一段的格子跳过*/
	{
		Chunk *c=now.get_Chunk(D,0,0);
		nbt_List *secs=(nbt_List*)c->con.get("sections");
		if(!secs||secs->type!=NBT_LIST){
			printf("now sections坏\n");
			return 1;
		}
		nbt_Compound *sec=new nbt_Compound;
		sec->set("Y",new nbt_Byte((int8_t)12));
		nbt_Compound *bs=new nbt_Compound;nbt_List *pal=new nbt_List(NBT_COMPOUND);
		nbt_Compound *air=new nbt_Compound;air->set_String("Name","minecraft:air");pal->push_back(air);bs->set("palette",pal);
		sec->set("block_states",bs);
		nbt_Compound *bio=new nbt_Compound;nbt_List *bp=new nbt_List(NBT_STRING);bp->push_back(new nbt_String("minecraft:plains"));bio->set("palette",bp);
		sec->set("biomes",bio);
		secs->push_back(sec);
		if(!c->set_Block(Block("minecraft:diamond_block"),8,200,8)){
			printf("写高段失败\n");
			return 1;
		}
		now.set_Chunk(D,0,0,*c);
	}
	/*再准备区块(1,0)：now 和 new 都完整，用来测“目标实体/POI块读不出来时不能覆盖”。*/
	{
		Chunk *s=now.get_Chunk(D,0,0);
		Chunk nc(*s);
		nc.con.set_Int("xPos",1);nc.con.set_Int("zPos",0);nc.con.set_Long("InhabitedTime",5);
		if(!nc.set_Block(Block("minecraft:diamond_block"),1,-64,1)){
			printf("写now 1,0失败\n");
			return 1;
		}
		if(!now.set_Chunk(D,1,0,nc)){
			printf("存now 1,0失败\n");
			return 1;
		}
	}
	{
		Chunk *s=nw.get_Chunk(D,0,0);
		Chunk nc(*s);
		nc.con.set_Int("xPos",1);nc.con.set_Int("zPos",0);nc.con.set_Long("InhabitedTime",5);
		if(!nc.set_Block(Block("minecraft:stone"),1,-64,1)){
			printf("写new 1,0失败\n");
			return 1;
		}
		if(!nw.set_Chunk(D,1,0,nc)){
			printf("存new 1,0失败\n");
			return 1;
		}
	}
	/*实体/POI：now 在(0,0)(1,0)各放记录；new没有(0,0)文件、只有(1,0)坏压缩块。
	  merge要新建(0,0)的poi/entities文件，同时跳过读不出来的(1,0)目标，不能覆盖。*/
	system("mkdir -p test/work/merge/now/dimensions/minecraft/overworld/entities test/work/merge/new/dimensions/minecraft/overworld/entities");
	system("mkdir -p test/work/merge/now/dimensions/minecraft/overworld/poi test/work/merge/new/dimensions/minecraft/overworld/poi");
	{
		nbt_Compound *e=new nbt_Compound;
		nbt_Int_Array *p=new nbt_Int_Array;p->push_back(0);p->push_back(0);
		e->set("Position",p);
		e->set_Int("DataVersion",4788);
		nbt_List *es=new nbt_List(NBT_COMPOUND);
		nbt_Compound *one=new nbt_Compound;one->set_String("id","minecraft:pig");one->set_Int("Test",1);
		nbt_Int_Array *u=new nbt_Int_Array;u->push_back(1);u->push_back(2);u->push_back(3);u->push_back(4);
		one->set("UUID",u);
		nbt_List *pos=new nbt_List(NBT_DOUBLE);pos->push_back(new nbt_Double(1.5));pos->push_back(new nbt_Double(-64.0));pos->push_back(new nbt_Double(1.5));
		one->set("Pos",pos);
		es->push_back(one);
		/*Pos 用 Int 写（游戏 Vec3.CODEC 照读）、y=400 在范围外：不搬。以前只认浮点，当成坏坐标照搬*/
		nbt_Compound *hi=new nbt_Compound;hi->set_String("id","minecraft:bat");
		nbt_List *ipos=new nbt_List(NBT_INT);ipos->push_back(new nbt_Int(5));ipos->push_back(new nbt_Int(400));ipos->push_back(new nbt_Int(5));
		hi->set("Pos",ipos);
		es->push_back(hi);
		e->set("Entities",es);
		MCA m; m.set_Compound(*e,0,0);
		/*同一文件里再放一个(1,0)的now实体：目标是坏压缩块时merge要跳过，不能覆盖*/
		nbt_Compound *e2=new nbt_Compound;
		nbt_Int_Array *p2=new nbt_Int_Array;p2->push_back(1);p2->push_back(0);
		e2->set("Position",p2);
		e2->set_Int("DataVersion",4788);
		nbt_List *es2=new nbt_List(NBT_COMPOUND);
		nbt_Compound *two=new nbt_Compound;two->set_String("id","minecraft:cow");two->set_Int("Test",3);
		nbt_Int_Array *u2=new nbt_Int_Array;u2->push_back(5);u2->push_back(6);u2->push_back(7);u2->push_back(8);
		two->set("UUID",u2);
		nbt_List *pos2=new nbt_List(NBT_DOUBLE);pos2->push_back(new nbt_Double(17.5));pos2->push_back(new nbt_Double(-64.0));pos2->push_back(new nbt_Double(1.5));
		two->set("Pos",pos2);
		es2->push_back(two);e2->set("Entities",es2);
		m.set_Compound(*e2,1,0);
		e2->free();
		if(!m.save("test/work/merge/now/dimensions/minecraft/overworld/entities/r.0.0.mca")){
			printf("存now实体失败\n");
			return 1;
		}
		e->free();
	}
	{
		nbt_Compound *e=new nbt_Compound;
		nbt_Int_Array *p=new nbt_Int_Array;p->push_back(1);p->push_back(0);
		e->set("Position",p);
		e->set_Int("DataVersion",4788);
		nbt_List *es=new nbt_List(NBT_COMPOUND);
		nbt_Compound *one=new nbt_Compound;one->set_String("id","minecraft:cow");one->set_Int("Test",2);
		es->push_back(one);e->set("Entities",es);
		MCA m; m.set_Compound(*e,1,0);
		if(!m.save("test/work/merge/new/dimensions/minecraft/overworld/entities/r.0.0.mca")){
			printf("存new实体失败\n");
			return 1;
		}
		if(!corrupt_Type("test/work/merge/new/dimensions/minecraft/overworld/entities/r.0.0.mca",1,0,0x7F)){
			printf("造坏new实体块失败\n");return 1;
		}
		e->free();
	}
	{
		nbt_Compound *p=new nbt_Compound;
		if(!set_POI(p,1,-64,1,POI("minecraft:home",1))||!set_POI(p,2,-64,2,POI("minecraft:home",1))){
			printf("设now POI失败\n");
			return 1;
		}
		/*同一格再塞一条（set_POI 会先删，只能直接往表里加）：游戏 PoiSection.add 同类型留先来的、不同类型后来的顶掉。
		  merge 搬过去也要这样：(1,-64,1) 留 home 1 票，(2,-64,2) 变成 meeting 5 票*/
		{
			std::vector<nbt_List*> ls=get_POILists(p);
			if(ls.size()!=1||!ls[0]->push_back(make_POIRecord(POI("minecraft:home",0),1,-64,1))||
			   !ls[0]->push_back(make_POIRecord(POI("minecraft:meeting",5),2,-64,2))){
				printf("造重复 POI 失败\n");
				return 1;
			}
		}
		MCA m; m.set_Compound(*p,0,0);
		nbt_Compound *p2=new nbt_Compound;
		if(!set_POI(p2,17,-64,1,POI("minecraft:home",1))){
			printf("设now 1,0 POI失败\n");
			return 1;
		}
		m.set_Compound(*p2,1,0);
		p2->free();
		if(!m.save("test/work/merge/now/dimensions/minecraft/overworld/poi/r.0.0.mca")){
			printf("存now POI失败\n");
			return 1;
		}
		p->free();
	}
	{
		nbt_Compound *p=new nbt_Compound;
		if(!set_POI(p,17,-64,1,POI("minecraft:home",1))){
			printf("设new POI失败\n");
			return 1;
		}
		MCA m; m.set_Compound(*p,1,0);
		if(!m.save("test/work/merge/new/dimensions/minecraft/overworld/poi/r.0.0.mca")){
			printf("存new POI失败\n");
			return 1;
		}
		if(!corrupt_Type("test/work/merge/new/dimensions/minecraft/overworld/poi/r.0.0.mca",1,0,0x7F)){
			printf("造坏new POI块失败\n");return 1;
		}
		p->free();
	}
	/*区块(32,0)在区域 r.1.0：now 有区块+POI+实体文件，new 只有区块、整个 poi/entities 文件都没有。
	  merge 要为它新建 new 的 poi/r.1.0.mca、entities/r.1.0.mca（文件不在≠读不出来）。
	  区块(64,0)在区域 r.2.0：两边都没有 poi/entities 文件，不能算成坏POI/坏实体。*/
	for(int cx:{32,64}){
		Chunk *s=now.get_Chunk(D,0,0);
		Chunk a(*s);
		a.con.set_Int("xPos",cx);a.con.set_Int("zPos",0);
		if(!a.set_Block(Block("minecraft:lapis_block"),1,-64,1)){
			printf("写now %d,0失败\n",cx);
			return 1;
		}
		if(!now.set_Chunk(D,cx,0,a)){
			printf("存now %d,0失败\n",cx);
			return 1;
		}
		Chunk *t=nw.get_Chunk(D,0,0);
		Chunk b(*t);
		b.con.set_Int("xPos",cx);b.con.set_Int("zPos",0);
		if(!nw.set_Chunk(D,cx,0,b)){
			printf("存new %d,0失败\n",cx);
			return 1;
		}
	}
	{
		nbt_Compound p;
		if(!set_POI(&p,513,-64,1,POI("minecraft:home",1))){
			printf("设now 32,0 POI失败\n");
			return 1;
		}
		MCA m;
		if(!m.set_Compound(p,0,0)||!m.save("test/work/merge/now/dimensions/minecraft/overworld/poi/r.1.0.mca")){
			printf("存now r.1.0 POI失败\n");return 1;
		}
		nbt_Compound e;
		e.set("Position",new nbt_Int_Array(std::vector<int32_t>{32,0}));
		e.set_Int("DataVersion",4788);
		nbt_List *es=new nbt_List(NBT_COMPOUND);
		nbt_Compound *one=new nbt_Compound;one->set_String("id","minecraft:sheep");
		one->set("UUID",new nbt_Int_Array(std::vector<int32_t>{9,10,11,12}));
		nbt_List *pos=new nbt_List(NBT_DOUBLE);pos->push_back(new nbt_Double(513.5));pos->push_back(new nbt_Double(-64.0));pos->push_back(new nbt_Double(1.5));
		one->set("Pos",pos);
		es->push_back(one);e.set("Entities",es);
		MCA me;
		if(!me.set_Compound(e,0,0)||!me.save("test/work/merge/now/dimensions/minecraft/overworld/entities/r.1.0.mca")){
			printf("存now r.1.0 实体失败\n");return 1;
		}
	}
	/*new 的实体/POI文件里只放(1,0)坏压缩块：0,0仍没有文件，merge要新建0,0且跳过1,0*/
	now.save();nw.save();o1.save();o2.save();
	now.close();nw.close();o1.close();o2.close();
	/*范围外的区域 r.5.0（方块 x 2560 起；block.sh 给的范围只到 1039）放一个读不出来的“文件”（其实是个目录）：
	  merge 不许去碰它。以前区域范围算宽了 16 倍，会去读它、报“now r.5.0 读不出来”、计一个坏区域、退出码非0*/
	if(system("mkdir -p test/work/merge/now/dimensions/minecraft/overworld/region/r.5.0.mca")){
		printf("造范围外的坏区域失败\n");
		return 1;
	}
	printf("造好了\n");
	return 0;
}
