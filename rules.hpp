/*	rules.hpp —— 规则插件（rules.so）的接口：fixer 只认这张表，规则想换就换
 *
 *	分工：fixer管"怎么走"（读存档、认区块、调色板建表、连锁补判、落盘），rules管"怎么判"
 *	      （哪些方块要管、靠什么才能活、撑不住时拆掉还是关掉某个面）。
 *	两边只通过Rules_Api里的函数指针说话：fixer是dlopen装rules.so的，不链接它，
 *	所以换一个rules.so（改规则、加规则）不用动fixer，也不用重编fixer。
 *
 *	怎么换：
 *	  1. 改rules.cpp（规则表defs、集合set_*、支撑档、rule_*函数），make rules（编出 rules.so.26.1.1 并把 rules.so 链过去）；
 *	  2. 想完全另写一个插件也行：导出下面的入口和Rules_Api，行为对得上就能用；
 *	  3. 插件位置是固定的：先找"fixer自己那个目录下的 rules.so"，没有再找当前目录下的 rules.so（见fixer.cpp的rules_Find）。
 *	接口对不上（结构体改了、abi没跟着加）插件会返回NULL，fixer会直接报错不干，不会带病跑。
 *
 *	版本：Rules_Api.minecraft_version 是规则对应的 DataVersion，fixer 必须和存档完全相等。
 *	ABI：RULES_ABI 是唯一的兼容性闸门（以前另有一个blockporter_version，作用重复，已经并进来）：
 *	     改接口/语义到不兼容时就把 RULES_ABI 加1，旧插件自然装不上。
 *	     两边必须用同一套编译器/标准库编（这里就是g++ -std=c++23 + libstdc++），
 *	     因为Blk_Info里有std::string_view。改下面任何一个结构体/枚举，RULES_ABI就要加1。
 *
 *	写规则必须守住的一条（fixer靠它保证转得完，证明见fixer.cpp文件头）：
 *	  只能"越判越少"——拆方块（FX_AIR）或者把撑不住的面改成false（FX_SET），
 *	  不许放方块、不许把false改成true、不许换成一个"更大"的状态。破坏了这条，fixer可能转不完。
 *	  FX_SET 也可以把方块整个换成另一种（比如霜冰换水、混凝土粉末换混凝土，见rules.cpp的RULE_FROST/RULE_FALLING），
 *	  条件是换过去的状态"不比原来大"：最好压根不在规则表里（＝主机不会再管它），也不能多出能撑东西的面——
 *	  证明里的 M＝"要管的方块数＋true面数"每次改动至少减1，靠的就是这个。
 */
#ifndef RULES_HPP
#define RULES_HPP
#include "nbtrw.hpp"
#include <string_view>

#define RULES_ABI 0x4e425233u     /*接口版本："NBR3"；改了本文件的结构体/语义就+1*/
#define RULES_ENTRY "rules_Entry" /*插件必须导出的符号（extern "C"），fixer用dlsym找它*/

/*支撑三档：一条规则要求"下面/上面/侧面"达到哪一档才算撑得住。
  注意 2/3 档是"按面"算的：同一个方块，不同方向的面可能给出不同答案（上半砖只有顶面算整面、
  下半砖只有底面算；楼梯/活板门同理），规则插件判的时候必须把"支撑块朝自己那一面"一起传进去
  （rules.cpp 的 sup_OK 带 dir 参数）。整方块（S_FULL）六个面都算。*/
enum{S_NONE=0, /*压根不是方块（空气/水/火/传送门）*/
     S_THIN=1, /*薄片：撑不住任何东西（草、花、火把、铁轨、告示牌…）*/
     S_PART=2, /*"中心支撑"那一档：栅栏/墙/箱子（上下）、上半砖顶面/下半砖底面…*/
     S_FULL=3};/*整面：原版 isFaceSturdy(FULL) 够得着的那一面*/
/*Blk_Info.flags：这一条方块状态是什么（和名字/属性有关的那部分）*/
enum{BF_AIR=1,      /*就是空气（水不算！）*/
     BF_WATER=2,    /*水，或者含水方块（waterlogged=true 或名字本身就是水草海带）*/
     BF_WATER_SRC=4,/*水源（level=0）*/
     BF_ICE=8,BF_MAGMA=16,BF_LEAVES=32};/*冰/岩浆块/树叶：几条规则专门要排除它们*/
/*6个面的编号（东、西、上、下、南、北；fixer按这个顺序把六格填进Rules_Ctx::nb）*/
enum{NB_E=0,NB_W=1,NB_U=2,NB_D=3,NB_S=4,NB_N=5};
/*judge的返回值：这一格怎么处理*/
enum{FX_KEEP=0,/*留着，合法*/
     FX_AIR=1, /*拆掉（主机按self->flags决定写空气还是水：含水物拆完留水）*/
     FX_SET=2};/*改成另一个状态（rules造好放Rules_Ctx::out_block里，主机写完负责free）*/

struct Blk_Info{           /*调色板里一条方块状态算好的东西：规则只看这里，扫的时候不碰NBT树*/
	nbt_Any *blk=NULL;     /*状态本身（指在区块树里）；NULL=空气那种占位*/
	std::string_view base; /*basename（指在插件自己的名字池里，插件活着就一直有效）*/
	uint8_t lev=S_NONE;    /*支撑档S_*（双台阶这种名字看不出来的，看属性补成S_FULL）*/
	uint8_t flags=0;       /*BF_*：空气/水/水源/冰/岩浆块/树叶*/
	int16_t def=-1;        /*规则表下标（judge的参数），-1=这个方块不用管*/
	uint32_t set_mask=0;   /*命中哪几个支持集（插件内部用，主机不管）*/
	void fill(nbt_Any *b); /*按名字/属性把上面这些填好（实现和名字池都在rules.cpp里；主机通过blk_Fill调）*/
};
inline int blk_Air(const Blk_Info *b){return !b||(b->flags&BF_AIR);}/*真的空气（水不算）*/
inline int blk_Water(const Blk_Info *b){return b&&(b->flags&BF_WATER);}/*水或含水方块*/

struct Rules_Ctx{          /*判一格时的上下文：自己 + fixer填好的6个面（只给这6格，不能读别处）*/
	const Blk_Info *self=NULL;/*这一格是什么（主机从段表里拿的）*/
	int32_t x=0,y=0,z=0;      /*世界坐标（绝对Y）*/
	const Blk_Info *nb[6]={NULL,NULL,NULL,NULL,NULL,NULL};/*6个面（按NB_*下标）；空气填info_Air，不是NULL*/
	uint8_t got=0;            /*这6个方向哪个有内容（fixer正常会把6位全置上）*/
	uint8_t bad=0;            /*这6个方向哪个读不出来（规则真的用到时nb_Get置blind）*/
	int blind=0;              /*要用的邻居读不出来：这一格的结论作废，主机原样留着（宁可漏拆）*/
	nbt_Any *out_block=NULL;  /*FX_SET时：rules造的新状态（所有权交给主机，主机free）*/
	int out_keep_be=0;        /*FX_SET时：要不要把方块实体也搬过去（藤蔓关面要保留）*/
};

struct Rules_Api{
	uint32_t abi;         /*RULES_ABI：fixer核对，唯一的兼容性检查*/
	uint32_t size;        /*sizeof(Rules_Api)：两边头文件不一致时能发现*/
	const char *name;     /*插件名字（fixer启动时打一行）*/
	const char *version;  /*插件/规则版本串（人看的，如"26.1.1"）*/
	int32_t minecraft_version;/*这份规则对应的 DataVersion（如26.1.1=4788）；fixer必须和存档完全相等*/
	int (*rule_Count)(void);/*规则条数（含0号"不管"）；report按1..count-1打*/
	const char *(*rule_Name)(int r);/*第r条规则的名字（中文，fixer直接打）*/
	int (*group_Count)(void);/*规则家族个数（留给将来的rules=掩码用；现在fixer恒传0xFFFFFFFF，全收）*/
	const char *(*group_Name)(int g);/*第g个家族的名字（以后恢复rules=开关时命令行可以写名字）*/
	void (*reset)(void);/*清分类缓存（换世界前调；调完之前发出去的Blk_Info全作废）*/
	void (*blk_Fill)(Blk_Info *o,nbt_Any *blk);/*调色板里一条状态→Blk_Info（建段表时每条调一次）*/
	int (*blk_Rule)(const Blk_Info *b,uint32_t mask);/*这条要管吗？给规则表下标，-1=不管*/
	int (*def_Rule)(int def);/*规则表下标→规则号（1..rule_Count-1；fixer的report按规则分类计数用）*/
	int (*judge)(const Blk_Info *self,int def,Rules_Ctx *c);/*判一格：FX_KEEP/FX_AIR/FX_SET*/
};
typedef const Rules_Api *(*Rules_Entry_Fn)(uint32_t abi,uint32_t size);/*入口函数的样子*/
#endif
