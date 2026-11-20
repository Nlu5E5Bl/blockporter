/*	rules.hpp —— 规则插件（rules.so）的接口：fixer 只认这张表，规则想换就换
 *
 *	分工：fixer管"怎么走"（读存档、认区块、调色板建表、连锁补判、落盘），rules管"怎么判"
 *	      （哪些方块要管、靠什么才能活、撑不住时拆掉还是关掉某个面）。
 *	两边只通过Rules_Api里的函数指针说话：fixer是dlopen装rules.so的，不链接它，
 *	所以换一个rules.so（改规则、加规则）不用动fixer，也不用重编fixer。
 *
 *	怎么换：
 *	  1. 改rules.cpp.<版本>（规则表defs、集合set_*、形状表、judge_*函数），make rules（每个版本编出一个 rules.so.<版本>）；
 *	  2. 想完全另写一个插件也行：导出下面的入口和Rules_Api，行为对得上就能用；
 *	  3. 插件的路径是 fixer 的最后一个参数，不给就用当前目录的 ./rules.so（make 不建它）。
 *	接口对不上（结构体改了、abi没跟着加）插件会返回NULL，fixer会直接报错不干，不会带病跑。
 *
 *	一个 fixer 配不同的 rules.so，修不同版本、不同模组的存档：
 *	  fixer/libnbtrw 只管存档格式（按 DataVersion 选：调色板编码、生成状态字段名、目录布局…），
 *	  方块知识（支撑、流体、默认属性、POI）全在插件里。插件内部怎么建模（rules.cpp 的支撑档/形状位图…）
 *	  放在 Blk_Info::priv，不出现在这个头文件里，所以改 rules.cpp 不用动 ABI。
 *	版本：Rules_Api.minecraft_version 是规则对应的 DataVersion，fixer 要求和 level.dat 的完全相等；
 *	      存档里 DataVersion 和它不一样的区块（没升级的老区块）fixer 不拿规则判（fixer.cpp 的 is_RulesChunk）。
 *	ABI：RULES_ABI + sizeof(Rules_Api) 是兼容性闸门（以前另有一个blockporter_version，
 *	     作用重复，已经并进来）；改接口/语义到不兼容时就把 RULES_ABI 加1，旧插件自然装不上。
 *	     两边必须用同一套编译器/标准库编（这里就是g++ -std=c++23 + libstdc++），
 *	     因为Blk_Info里有std::string_view、Rules_Ctx里有Block。改下面任何一个结构体/枚举，RULES_ABI就要加1。
 *	POI：Poi_Info + get_POIInfo 是方块→默认POI记录的表；fixer用它补缺失/校验/去重，
 *	     规则插件仍只通过Blk_Info看方块状态，不额外读世界。
 *
 *	写规则必须守住的一条（fixer靠它保证转得完）：不许放方块（空气/流体格不能变成方块），每次改动只能是下面三种之一，
 *	破坏了这条，fixer 可能转不完：
 *	  1. 越判越少：拆方块（FX_AIR）、把撑不住的面改成 false，或者换成一个“不比原来大”的状态（霜冰换水、混凝土粉末换混凝土，
 *	     见 rules.cpp 的 RULE_FROST/RULE_FALLING：换过去的最好不在规则表里，也不能多出能撑东西的面）；
 *	  2. 沿一条单向的链换下去，链上的不会再变回来（结果的瓜茎→普通瓜茎、竹笋→竹子、大型垂滴叶→茎、竹子 age 0→1、火→灵魂火）；
 *	  3. 跟着邻居走的外观状态：新状态只由自己和六邻决定，邻居不变再判一次结果不变（门照另一半改、藤头/藤身互换、火的面、
 *	     栅栏/墙/玻璃板/铁栏杆/紫颂植株的连接、苍白苔藓地毯的侧面、栅栏门贴墙、雪地草、箱子配对、铜箱子两半对齐、
 *	     蘑菇方块的面、音符盒音色、石锥粗细、树叶/脚手架的 distance）。可以开面/连上（用户 2026-11-10：“只关不开”是为了防死循环的建议，能保证不死循环
 *	     就照游戏开），条件是它看的东西每样只变有限次：名字类别和规则不改的属性（栅栏门朝向、地毯的 bottom）不变；
 *	     只会单向变的条件（“长的方向那格是不是同族”只会从是变成不是、上面那截地毯的面只会关；
 *	     指着自己的那半个大箱子不会再变——它的另一半就是自己，所以单箱补成大箱子只发生一次）；
 *	     别的“跟着邻居走”的格子，但这种看法不能成环（墙看上面那格的碰撞箱，下面的墙跟着上面的走、上面的不看下面；
 *	     栅栏/墙/玻璃板的侧面从来不是整面，它们互相只认名字、不认对方连没连；也没有规则靠栅栏/墙连没连活）。
 *	  树叶/脚手架的 distance 是封顶 7 的最短路：每格只由六邻存着的 distance 决定，异步地一格格重算也会停在唯一的不动点（游戏自己就是这么传的）；
 *	  石锥粗细只沿“朝尖那一头”往回看前一格的粗细，尖那一格不看别人的，不成环。
 *	  证明（每格改的次数有上限，总次数就有限）：方块只会变少，拆的次数有限；只关不开的面每格最多关几次；单向链每格最多走链长那么多步；
 *	  跟着邻居走的，新状态是它看的那些东西的函数，那些东西各自只变有限次（按“谁看谁”往上推，没有环），它就只变有限次。
 *	  fixer 改一格就把六邻排进队列重判（改动当场写回，后判的看到的是改过的邻居），所以门两半谁先判谁照对方改，改完就一致了。
 */
#ifndef RULES_HPP
#define RULES_HPP
#include "nbtrw.hpp"
#include <string_view>

#define RULES_ABI 0x4e425237u      /*接口版本："NBR7"；改了本文件的结构体/语义就+1（只跟本文件走，不跟rules.cpp走）*/
#define RULES_ENTRY "get_RulesApi" /*插件必须导出的符号（extern "C"），fixer用dlsym找它*/

/*Blk_Info.flags：主机要看的两位（别的分类是插件自己的事，放 priv）*/
enum{BF_AIR=1,   /*就是空气（水不算！）；主机判“拆空气”为白干*/
     BF_WATER=2};/*水，或者装着水的方块（含水、海带这类）；主机只拿它计数（拆了留水的有几格）*/
/*6个面的编号（东、西、上、下、南、北；fixer按这个顺序把六格填进Rules_Ctx::nb）*/
enum{NB_E=0,NB_W=1,NB_U=2,NB_D=3,NB_S=4,NB_N=5};
/*judge的返回值：这一格怎么处理*/
enum{FX_KEEP=0,/*留着，合法*/
     FX_AIR=1, /*拆掉：主机写 Rules_Ctx::out_block（插件给的“拆完剩下的”，比如含水方块拆完留水）；
                 out_block 是空id就写 minecraft:air*/
     FX_SET=2};/*改成 Rules_Ctx::out_block 这个状态*/

struct Blk_Info{           /*调色板里一条方块状态算好的东西：规则只看这里，扫的时候不碰NBT树*/
	/*---- 主机填（fill_BlkInfo 之前）：都指在区块树里，按区块 DataVersion 的格式取好 ----*/
	const nbt_Any *blk=NULL;       /*调色板条目原样；NULL＝不是从调色板来的（空气占位、计划刻查方块）。
	                                 编码随区块版本变（见 nbtrw.hpp State_Format），插件别拆它，看下面两个*/
	std::string_view name;         /*方块ID全名（如 minecraft:oak_slab）*/
	const nbt_Compound *props=NULL;/*写出来的方块属性；NULL＝没写。26.3快照7起没写的属性取默认值，补默认值是插件的事：
	                                 插件可以在 fill_BlkInfo 里把它换成指向自己池子里补全的那份（reset_Rules 之前要一直有效），主机不依赖它指在树里*/
	/*---- 插件填（fill_BlkInfo）----*/
	std::string_view base;         /*短名（统计用；指在插件自己的名字池里，插件活着就一直有效）*/
	uint8_t flags=0;               /*BF_*：主机要看的两位*/
	uint32_t priv[4]={0,0,0,0};    /*插件自己的缓存（支撑档、形状、支持集…），主机不读不写、不解释*/
};
inline bool is_Air(const Blk_Info *b){return !b||(b->flags&BF_AIR);}/*真的空气（水不算）；NULL也算空气，读不出来要先看blind*/
inline bool is_Water(const Blk_Info *b){return b&&(b->flags&BF_WATER);}/*水或含水方块*/
inline const char *get_Prop(const Blk_Info *b,const char *key){return b?get_PropValue(b->props,key):NULL;}/*写出来的属性值*/
inline bool is_PropValue(const Blk_Info *b,const char *key,const char *val){/*属性正好等于val？*/
	const char *v=get_Prop(b,key);
	return v&&val&&!strcmp(v,val);
}

struct Rules_Tick{         /*写完一格后要在这格补的计划刻（比如拆完留下的水要流）*/
	std::string_view type; /*方块/流体ID；空＝不补（插件给静态字符串）*/
	bool fluid=false;      /*true进fluid_ticks，false进block_ticks*/
	int32_t delay=0,priority=0;
};

struct Poi_Info{           /*一种POI方块的默认记录：fixer发现缺失时按这张表补*/
	const char *type;      /*POI类型全名（如minecraft:home）*/
	int32_t free_tickets;  /*新记录默认的free_tickets（=该类型的票数上限，插件按Wiki填）*/
	int32_t max_tickets;   /*该类型允许的最大票数；fixer用它删越界记录*/
};

struct Rules_Ctx{          /*判一格时的上下文：自己 + fixer填好的6个面（只给这6格，不能读别处）*/
	const Blk_Info *self=NULL;/*这一格是什么（主机从段表里拿的）*/
	int32_t x=0,y=0,z=0;      /*世界坐标（绝对Y）*/
	const Blk_Info *nb[6]={NULL,NULL,NULL,NULL,NULL,NULL};/*6个面（按NB_*下标）；空气填info_Air，不是NULL*/
	uint8_t got=0;            /*这6个方向哪个有内容（fixer正常会把6位全置上）*/
	uint8_t bad=0;            /*这6个方向哪个读不出来（规则真的用到时get_Neighbor置blind）*/
	bool blind=false;         /*要用的邻居读不出来：这一格的结论作废，主机原样留着（宁可漏拆）*/
	Block out_block;          /*FX_SET：新状态；FX_AIR：拆完剩下的（空id＝空气）。主机按区块的格式写回去*/
	bool out_keep_be=false;   /*FX_SET时：要不要把方块实体也搬过去（藤蔓关面要保留）*/
	Rules_Tick out_tick;      /*FX_AIR/FX_SET：写完在这格补的计划刻（type空＝不补）；这格原来的计划刻主机一律删掉*/
};

struct Rules_Api{
	uint32_t abi;         /*RULES_ABI：fixer核对，唯一的兼容性检查*/
	uint32_t size;        /*sizeof(Rules_Api)：两边头文件不一致时能发现*/
	const char *name;     /*插件名字（fixer启动时打一行）*/
	const char *version;  /*插件/规则版本串（人看的，如"26.1.1"）*/
	int32_t minecraft_version;/*这份规则对应的 DataVersion（如26.1.1=4788）；fixer必须和存档完全相等*/
	int (*count_Rule)(void);/*规则条数（含0号"不管"）；report按1..count-1打*/
	const char *(*get_RuleName)(int r);/*第r条规则的名字（中文，fixer直接打）*/
	int (*count_Group)(void);/*规则家族个数（留给将来的rules=掩码用；现在fixer恒传0xFFFFFFFF，全收）*/
	const char *(*get_GroupName)(int g);/*第g个家族的名字（以后恢复rules=开关时命令行可以写名字）*/
	void (*reset_Rules)(void);/*清分类缓存（换世界前调；调完之前发出去的Blk_Info全作废）*/
	void (*fill_BlkInfo)(Blk_Info *o);/*主机填好 blk/name/props 后调，插件填其余的（建段表时每条调一次）*/
	int (*get_BlkRule)(const Blk_Info *b,uint32_t mask);/*这条要管吗？给规则表下标，-1=不管*/
	int (*get_DefRule)(int def);/*规则表下标→规则号（1..count_Rule-1；fixer的report按规则分类计数用）*/
	/*这个方块状态如果产生POI，返回默认记录（静态数据，插件活着一直有效）；不产生返回NULL。
	  床只认part=head；门/双箱这种没有POI。fixer用同一张表判“记录和方块对不对得上”和“缺记录要补什么”。*/
	const Poi_Info *(*get_POIInfo)(const Blk_Info *b);
	int (*judge_Blk)(const Blk_Info *self,int def,Rules_Ctx *c);/*判一格：FX_KEEP/FX_AIR/FX_SET*/
	/*这条计划刻和这格方块对得上吗（fixer清计划刻用）：方块刻看方块，流体刻看这格装的是什么流体。
	  对不上的主机删掉；拿不准就给true（宁可留着）。*/
	bool (*check_Tick)(const Blk_Info *b,bool fluid,std::string_view type);
};
typedef const Rules_Api *(*Rules_Entry_Fn)(uint32_t abi,uint32_t size);/*入口函数的样子*/
#endif
