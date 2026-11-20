#include "../nbtrw.hpp"
#include <stdlib.h>
/*lvl.cpp：直接测Level的API（库级测试的一部分；更全的在 test/lib.cpp）
 *	用到的小世界在 test/data/（lvlworld/lvlold/lvlnodat），不依赖 test/work 里造的数据。
 *	覆盖：DataVersion决定新老布局、Level不绑定维度、维度名/路径解析、
 *	      不同维度同坐标不串、区块缓存满了自动落盘、close后还能接着用、
 *	      没有level.dat的目录、旧布局目录。
 *	lvlworld=新布局(DataVersion 4788)、lvlold=老布局(DataVersion 3465)、lvlnodat=只有region没有level.dat
 */
static int fails=0,cases=0;
static void check_Case(int cond,const char *msg){/*一项核对：打 ok/FAIL，记用例数和失败数*/
	cases++;
	printf("%s %s\n",cond?"ok  ":"FAIL",msg);
	if(!cond){
		fails++;
	}
}
int main(){/*拿 test/data 里几个小存档测 Level 的维度目录、缓存和落盘*/
	static const char *OW="minecraft:overworld";
	Level lvl("test/data/lvlworld",2,1,2);
	check_Case(lvl.dat_ok&&lvl.data_version==4788,"新布局世界：level.dat读到DataVersion=4788");
	check_Case(lvl.new_layout,"DataVersion>=4774 → 用dimensions/新布局");
	check_Case(lvl.get_DimensionPath("overworld","region")=="dimensions/minecraft/overworld/region",
		"overworld（无命名空间）→ 主世界区域目录");
	check_Case(lvl.get_DimensionPath("minecraft:the_nether","region")=="dimensions/minecraft/the_nether/region",
		"minecraft:the_nether → 下界区域目录");
	check_Case(lvl.get_DimensionPath("the_end","poi")=="dimensions/minecraft/the_end/poi",
		"the_end + poi → 末地POI目录");
	check_Case(lvl.get_DimensionPath("DIM-1","region")=="DIM-1/region",
		"非原版维度名当路径：DIM-1");
	check_Case(lvl.get_DimensionPath("dimensions/custom/dim","entities")=="dimensions/custom/dim/entities",
		"非原版维度名当路径：任意相对目录");
	check_Case(lvl.get_DimensionPath("","region").empty()&&lvl.get_DimensionPath("/abs","region").empty()&&
		lvl.get_DimensionPath("../x","region").empty()&&lvl.get_DimensionPath("a//b","region").empty()&&
		lvl.get_DimensionPath("a/","region").empty(),"空/绝对/../空段/结尾斜杠的维度路径都无效");
	check_Case(Level::is_DimensionName("overworld")&&Level::is_DimensionName("minecraft:the_end")&&
		!Level::is_DimensionName("nether")&&!Level::is_DimensionName("minecraft:custom")&&
		!Level::is_DimensionName(NULL),"is_DimensionName只认三个原版维度名");
	check_Case(lvl.get_Block(OW,1,-64,1).valid(),"get_Block读到(1,-64,1)");
	lvl.close();

	/*缓存满了自动落盘：cache_cnt=1，取第二个区块会把第一个写回MCA*/
	system("rm -rf test/work/lvlcopy && cp -a test/data/lvlworld test/work/lvlcopy");
	{
		Level lv("test/work/lvlcopy",1,1,2);
		check_Case(lv.set_Block(OW,1,-64,1,Block("minecraft:emerald_block")),"改区块(0,0)");
		Chunk *c=lv.get_Chunk(OW,-2,-2);/*不同区域：把(0,0)挤出缓存并要求落盘*/
		check_Case(c!=NULL,"cache_cnt=1取到区块(-2,-2)");
		check_Case(lv.set_Block(OW,-32,-64,-32,Block("minecraft:diamond_block")),"改区块(-2,-2)");
		check_Case(lv.save(),"save");
		lv.close();
	}
	{
		Level lv("test/work/lvlcopy",2,1,2);
		check_Case(lv.get_Block(OW,1,-64,1).name()=="minecraft:emerald_block","被挤出缓存的改动也存住了（(0,0)）");
		check_Case(lv.get_Block(OW,-32,-64,-32).name()=="minecraft:diamond_block","第二个区块的改动也存住了（(-2,-2)）");
		lv.close();
		/*close之后还能接着用（缓存自己补回来），改动已经落盘*/
		check_Case(lv.get_Block(OW,1,-64,1).name()=="minecraft:emerald_block","close后重读还是落盘的emerald_block");
	}
	/*Level不绑定维度：同一个Level里两个维度同坐标互不串*/
	{
		Level lv("test/work/lvlcopy",4,2,2);
		Chunk *src=lv.get_Chunk(OW,0,0);
		check_Case(src!=NULL,"取主世界(0,0)当模板");
		if(src){
			Chunk nc(*src);
			check_Case(nc.set_Block(Block("minecraft:emerald_block"),1,-64,1),"模板里改一格");
			check_Case(lv.set_Chunk("the_nether",0,0,nc),"写入下界(0,0)");
			check_Case(lv.set_Block(OW,1,-64,1,Block("minecraft:diamond_block")),"主世界同坐标改成钻石块");
			check_Case(lv.save(),"两个维度一起落盘");
			check_Case(lv.get_Block("the_nether",1,-64,1).name()=="minecraft:emerald_block","下界读到自己那份");
			check_Case(lv.get_Block(OW,1,-64,1).name()=="minecraft:diamond_block","主世界同坐标没被下界覆盖");
		}
		lv.close();
	}
	/*老布局：DataVersion 3465(1.20.1) → region/、DIM-1/、DIM1/ */
	{
		Level old("test/data/lvlold",1,1,2);
		check_Case(old.dat_ok&&old.data_version==3465,"老布局世界：DataVersion=3465");
		check_Case(!old.new_layout,"DataVersion<4774 → 用老的region/布局");
		check_Case(old.get_DimensionPath("overworld","region")=="region","老布局主世界区域目录=region");
		check_Case(old.get_DimensionPath("the_nether","region")=="DIM-1/region","老布局下界区域目录=DIM-1/region");
		check_Case(old.get_DimensionPath("the_end","region")=="DIM1/region","老布局末地区域目录=DIM1/region");
		check_Case(old.get_Chunk(OW,0,0)!=NULL,"老布局照样能读到区块");
		old.close();
	}
	/*没有level.dat的目录：dat_ok=0，程序应当拒绝干活（DataVersion严格判，不按目录猜）*/
	{
		Level nodat("test/data/lvlnodat",1,1,2);
		check_Case(!nodat.dat_ok,"没有level.dat → dat_ok=0");
		check_Case(nodat.data_version==0,"没有level.dat → DataVersion=0");
		check_Case(!nodat.new_layout,"没有level.dat → 按老布局算");
		check_Case(nodat.get_DimensionPath("overworld","region")=="region","没有level.dat → 主世界region/");
		nodat.close();
	}
	printf("\n%s：%d个用例，%d个失败\n",fails?"FAIL":"OK",cases,fails);
	return fails?1:0;
}
