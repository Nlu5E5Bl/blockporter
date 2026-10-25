/*	rules.cpp —— 规则插件（编成rules.so；fixer用dlopen装它，不链接）
 *
 *	这里只管"怎么判"：哪种方块要管（defs表）、它靠什么才能活（rule_*函数）。
 *	fixer 负责把这一格自己和六个面的 Blk_Info 填好；规则只能看这六格，不能再去读别的位置。
 *	读存档、认区块、连锁补判、落盘都在fixer.cpp里。规则都照着 minecraft.wiki 的 Java 版说明写，
 *	每条下面标了依据。
 *
 *	判的结果三种：留着（FX_KEEP）、拆掉（FX_AIR，含水物由主机换成水而不是空气）、
 *	改状态（FX_SET，藤蔓/发光地衣/幽匿脉络把撑不住的那几个面关掉；霜冰换成水、混凝土粉末换成混凝土
 *	也是这一种，见 RULE_FROST/RULE_FALLING）。
 *	只能"越判越少"：不新增方块、不把false改成true——fixer的收敛证明就靠这一条
 *	（见fixer.cpp文件头：每次改动都让 M=方块数+true面数 至少减1）。
 *
 *	支撑判定分三档（Blk_Info.lev），2/3 档是**按面**判的（sup_OK 的 dir 参数＝支撑块朝自己那一面）：
 *	  3=整面（原版 isFaceSturdy(FULL)：门/海草/钟乳石/墙火把要这档）
 *	  2=面中心够（原版 SupportType.CENTER：火把/灯笼/告示牌这一档；栅栏、墙、箱子上下都算）
 *	  1=薄片（草、花、火把、铁轨…撑不住；垂泪藤这种"只要有东西"的用这档）
 *	台阶/楼梯/活板门要按半格分方向：上半只有顶面算、下半只有底面算（见 face_Full/face_Center 的注释）。
 *	其余"有形状的"方块只按名字粗判（上下都算），比原版宽松一点：宁可漏拆，不乱拆玩家搭的东西。
 *
 *	规则家族（rules= 掩码的位，一个家族可以一次关掉）：
 *	  below  下面要有支持（花/草/树苗/火把/铁轨/海带/雪…）
 *	  2x     两格高、茎、苗这些要上下互查的
 *	  vine   藤蔓（每个面单独判）
 *	  attach 贴面/贴天花板/六面附着/滴水石锥/紫颂花/悬挂告示牌
 *	  misc   其余（门/钟/脚手架/可可豆…）
 *
 *	编译：make rules	（编出 rules.so.26.1.1，并把 rules.so 链过去；换版本见Makefile）
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -shared rules.cpp -o rules.so.26.1.1 -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
 *	      ln -sf rules.so.26.1.1 rules.so
 */
#include "rules.hpp"
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/*==================== 1. 取名字/属性/小判断 ====================*/
static std::string_view base_Of(std::string_view n){/*冒号后面那段（自带命名空间的mod方块会拿到原样）*/
	size_t p=n.find(':');
	return p==std::string_view::npos?n:n.substr(p+1);
}
static int end_With(std::string_view n,const char *suf){/*名字是不是以suf结尾*/
	size_t l=strlen(suf);
	return n.size()>=l&&!memcmp(n.data()+n.size()-l,suf,l);
}
static int pat_Match(const char *pat,std::string_view n){/*支持集/规则表里的名字：*在尾巴=前缀，*在头=后缀，没有=全等*/
	size_t pl=strlen(pat);
	if(!pl){
		return 0;
	}
	if(pat[pl-1]=='*'){
		return n.size()>=pl-1&&!memcmp(n.data(),pat,pl-1);
	}
	if(pat[0]=='*'){
		return n.size()>=pl-1&&!memcmp(n.data()+n.size()-(pl-1),pat+1,pl-1);
	}
	return n.size()==pl&&!memcmp(n.data(),pat,pl);
}

/*==================== 2. 支持集（下面/上面必须是什么） ====================*/
/*支持集的来源：minecraft.wiki 的 Block tag (Java Edition) 页里 26.1 新增的那套 #supports_*
  （26.1 快照3 "Added 29 tags"；Java Edition 26.1 页 Additions→General→Block tags 原文：
   "Added new placement support tags used to determine which blocks specific vegetation can survive on."）。
  这里每个数组对应一个标签，成员按标签值原样抄；被引用的底层标签也展开成最终方块名：
	#dirt = {dirt, coarse_dirt, rooted_dirt}（26.1 起只剩这3个："#dirt now only contains dirt, coarse dirt and rooted dirt"）
	#grass_blocks = {grass_block, mycelium, podzol}；#moss_blocks = {moss_block, pale_moss_block}；#mud = {mud, muddy_mangrove_roots}
	#substrate_overworld = #dirt + #grass_blocks + #moss_blocks + #mud
	#sand = {sand, red_sand, suspicious_sand}；#terracotta = 17 种染色陶瓦（"*terracotta"是后缀匹配，见pat_Match）*/
/*#substrate_overworld*/
static const char *set_substrate[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots",NULL};
/*#supports_vegetation＝草木/花/树苗/粉瓣/野花/甜浆果/萤火虫灌木…（= substrate + 耕地）*/
static const char *set_veg[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland",NULL};
/*#supports_dry_vegetation（枯灌木/矮枯草/高枯草）：标签是 #dirt+#sand+#terracotta+耕地，
  但各页散文还列了草方块/菌丝/灰化土那些，按"宁可放过"取并集。
  陶瓦那 17 个**必须逐个列**：以前写 "*terracotta" 会把 17 种上釉陶瓦（*_glazed_terracotta）
  一起收进来，而标签里没有它们（枯草长在上釉陶瓦上原版会拆，我们却留着）*/
static const char *set_dry_veg[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","sand","red_sand","suspicious_sand","terracotta","white_terracotta","orange_terracotta",
	"magenta_terracotta","light_blue_terracotta","yellow_terracotta","lime_terracotta","pink_terracotta",
	"gray_terracotta","light_gray_terracotta","cyan_terracotta","purple_terracotta","blue_terracotta",
	"brown_terracotta","green_terracotta","red_terracotta","black_terracotta",NULL};
/*#supports_crops＝#supports_stem_crops＝#supports_pumpkin_stem＝#supports_melon_stem*/
static const char *set_crops[]={"farmland",NULL};
/*#sand，也就是 #supports_cactus*/
static const char *set_sand[]={"sand","red_sand","suspicious_sand",NULL};
/*#supports_sugar_cane（不含耕地）*/
static const char *set_sugar[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","sand","red_sand","suspicious_sand",NULL};
/*#supports_bamboo（含竹子自己，所以能叠）*/
static const char *set_bamboo[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","sand","red_sand","suspicious_sand","gravel","suspicious_gravel","bamboo","bamboo_sapling",NULL};
/*#supports_azalea＝#supports_mangrove_propagule（比植被多一个黏土）*/
static const char *set_azalea[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","clay",NULL};
/*#supports_crimson_fungus＝#supports_warped_fungus＝#supports_crimson_roots＝#supports_warped_roots
  （标签里的 #lium 页面没定义，按 #nylium 那两个菌岩补）*/
static const char *set_nylium[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","crimson_nylium","warped_nylium","soul_soil",NULL};
/*#supports_nether_sprouts（和上面同一套14个）*/
static const char *set_sprouts[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","crimson_nylium","warped_nylium","soul_soil",NULL};
/*#supports_wither_rose（不含黏土/菌岩）*/
static const char *set_wither[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","netherrack","soul_sand","soul_soil",NULL};
/*#supports_nether_wart（灵魂土不行）*/
static const char *set_soul_sand[]={"soul_sand",NULL};
static const char *set_water[]={"water",NULL};
/*#supports_lily_pad；水/含水方块由 F_WET_D 单独认（睡莲是放在流体上的，不在标签里）*/
static const char *set_lily[]={"ice","frosted_ice",NULL};
/*#support_override_cactus_flower：这两个方块没有完整顶面也能放仙人掌花*/
static const char *set_cactus_flower[]={"cactus","farmland",NULL};
/*#supports_cocoa*/
static const char *set_jungle_log[]={"jungle_log","jungle_wood","stripped_jungle_log","stripped_jungle_wood",NULL};
/*#supports_chorus_plant＝#supports_chorus_flower*/
static const char *set_chorus[]={"end_stone",NULL};
/*#supports_small_dripleaf（别的泥土类要泡在水里才行，见 rule_SmallDripleaf）；
  26.1 的标签改名记录："small_dripleaf_placeable to supports_small_dripleaf"*/
static const char *set_small_dripleaf[]={"clay","moss_block",NULL};
/*#supports_big_dripleaf＝#supports_small_dripleaf + dirt/coarse_dirt/rooted_dirt/grass_block/mycelium/
  podzol/moss_block/mud/muddy_mangrove_roots/farmland，《Big Dripleaf》Placement那段列的正是这11个。
  pale_moss_block：25w02a 起不在标签里了（改动记录"Big dripleaves can no longer be placed on top of pale
  moss blocks"），我们**故意留着**——老版本存档里可能有长在它上面的，宁可漏拆。
  注意这个集合只给"最下面那根茎/直接放地上的叶子"用；叶子坐在自己茎上的情况见下面的 set_big_dripleaf_leaf*/
static const char *set_big_dripleaf[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","clay",NULL};
/*叶子专用：大垂滴叶是"若干根茎互相叠着、最上面顶一片叶子"的结构——《Big Dripleaf》Usage原文
  "Big dripleaves consist of any number of non-solid stem blocks atop of each other and a single leaf as the
   uppermost block"；Growth那段又说骨粉是"把叶子顶高一格、在它下面补一根茎"。
  所以**叶子下面是 big_dripleaf_stem 是正常成株形态，不是悬空**：26.1.1真图实测一张区域里2123片叶子里
  有1469片就是这种（漏了这一条的代价：fixer会把它们全当悬空拆掉，连茎一起连锁拆）；
  原版 canSurvive 第一句就是 below.is(this)，所以叶叠叶(需要setblock/异常状态)也算数，一起列上*/
static const char *set_big_dripleaf_leaf[]={"dirt","coarse_dirt","rooted_dirt","grass_block","mycelium","podzol","moss_block","pale_moss_block","mud","muddy_mangrove_roots","farmland","clay","big_dripleaf","big_dripleaf_stem",NULL};
static const char *set_cave_vine[]={"cave_vines","cave_vines_plant",NULL};
static const char *set_dripstone[]={"pointed_dripstone",NULL};
static const char *set_kelp[]={"kelp","kelp_plant",NULL};
static const char *set_amethyst[]={"budding_amethyst","amethyst_block","small_amethyst_bud","medium_amethyst_bud","large_amethyst_bud","amethyst_cluster",NULL};
/*#supports_hanging_mangrove_propagule（挂着的红树胎生苗只在红树叶下面）*/
static const char *set_mangrove_leaves[]={"mangrove_leaves",NULL};
/*气泡柱下面：往下吸是岩浆块（#enables_bubble_column_drag_down），往上推是灵魂沙（#enables_bubble_column_push_up）；
  BubbleColumnBlock.canSurvive 还有第一条"下面就是气泡柱"（一整列的第二格起都靠它），所以 def 上有 F_SELF*/
static const char *set_magma_soul[]={"magma_block","soul_sand",NULL};
/*#support_override_snow_layer：这几个顶面不完整也能放雪*/
static const char *set_snow_over[]={"honey_block","mud","soul_sand",NULL};
/*#cannot_support_snow_layer：这些上面一律不能有雪层（比上面那个优先级高）*/
static const char *set_snow_no[]={"barrier","ice","packed_ice",NULL};
/*上面这些集合全列一遍：每个集合给一个位，方块命中哪个集合在分类时就记成位图，
  判"下面是不是支持集里的"就变成一次按位与，不用再逐条比名字*/
static const char *const *set_List[]={set_substrate,set_veg,set_dry_veg,set_crops,set_sand,set_sugar,
	set_bamboo,set_azalea,set_nylium,set_sprouts,set_wither,set_soul_sand,set_water,set_lily,
	set_cactus_flower,set_jungle_log,set_chorus,set_small_dripleaf,set_big_dripleaf,set_big_dripleaf_leaf,
	set_cave_vine,set_dripstone,set_kelp,set_amethyst,set_mangrove_leaves,set_magma_soul,set_snow_over,set_snow_no};
#define SET_CNT ((int)(sizeof(set_List)/sizeof(set_List[0])))

/*==================== 3. 规则表（哪种方块用哪条规则判） ====================*/
enum{
	RULE_NONE=0,  /*明确不管*/
	RULE_BELOW,   /*下面要支持集里的方块*/
	RULE_2X,      /*两格高：上/下半截互查 + 下半截看下面*/
	RULE_VINE,    /*藤蔓：每个面要么背后是整方块，要么上面是同面藤蔓*/
	RULE_FACE6,   /*六个面各查一遍（发光地衣/幽匿脉络），撑不住的面关掉，全关了就走*/
	RULE_SIDE,    /*贴在一面：facing的反方向（face=floor/ceiling时看下/上）*/
	RULE_ABOVE,   /*上面要支持*/
	RULE_RIM,     /*下面要"顶面带边"（整方块、漏斗、上半台阶、倒楼梯、上半活板门）*/
	RULE_HANG,    /*看<key>属性：true→上面要支持，false→下面要支持（吊灯）*/
	RULE_HANG2,   /*上面或下面有一个不是空气就行（悬挂告示牌）*/
	RULE_DOOR,    /*门：上半截看下半截，下半截看下面*/
	RULE_COCOA,   /*可可豆：facing那一面是丛林原木*/
	RULE_STEM,    /*结果的瓜茎：facing那一面是西瓜/南瓜，下面耕地*/
	RULE_DRIP,    /*滴水石锥：尖端朝下看上、朝上看下*/
	RULE_BELL,    /*钟：按attachment看下/上/侧面*/
	RULE_SCAFF,   /*脚手架：下面能顶住，或者水平旁边也是脚手架*/
	RULE_SMALL_DRIPLEAF,/*小垂滴叶：黏土/苔藓直接放，泥土类要自己这格有水*/
	RULE_PROPAGULE,/*红树胎生苗：hanging=true挂树叶上，false种泥土里*/
	RULE_CHORUS,  /*紫颂花：下面末地石/紫颂植株，或者下面空气+水平正好挨着一个紫颂植株（其余水平方向必须全是空气）*/
	RULE_CHORUS_PLANT,/*紫颂植株：wiki上唯一真正按"六个面"判的方块（水平挨着紫颂时上下要有一格空气；"
	                    下面不是末地石/紫颂时，可以靠"旁边那株有根"撑着——末地的树全靠这一条）*/
	RULE_SNOW,    /*雪层：先看 #cannot_support_snow_layer / #support_override_snow_layer 两个标签，其余要"顶面完整"*/
	RULE_FROST,   /*霜冰：不看支撑，一律换成水（服务端bug/中断留下的一整片霜冰，本该早就化了）*/
	RULE_FALLING, /*重力方块：正下方是空气/水/岩浆/火才拆；混凝土粉末碰到水先凝固*/
	RULE_COUNT
};
static const char *rule_Name[RULE_COUNT]={"不管","下面要有支持","两格高","藤蔓","六面附着","贴一面","上面要有支持","下面要顶面带边","吊挂","悬挂告示牌","门","可可豆","瓜茎","滴水石锥","钟","脚手架","小垂滴叶","胎生苗","紫颂花","紫颂植株","雪层","霜冰化成水","重力方块"};

enum{F_SELF=1,     /*下面/上面可以是同类方块（竹子、海带、滴水石）*/
     F_NOSIDE=2,   /*4个水平邻居不能是整方块（仙人掌）*/
     F_WET_D=4,    /*下面得是"水"：水/含水方块(waterlogged)/海草海带这种泡在水里的（睡莲）*/
     F_WATER=8,    /*自己得是"水里的方块"/含水的（海草、海带、气泡柱）——原版 canSurvive 只管下面撑不撑得住，
                     不要求周围有水（水被抽干的角落原版也留着它们，见ReadMeDeveloper）*/
     F_NO_MAGMA=16,/*下面不能是岩浆块（海带、海草）*/
     F_NO_ICE=32,  /*下面不能是冰（雪层）*/
     F_AIR_U=64,   /*自己上面必须是空气（青蛙卵）*/
     F_NONAIR=128, /*只要下面不是空气就行（水也算，地毯/苔藓毯这样）*/
     F_NO_LEAVES=256,/*支撑不能是树叶（孢子花上面、压力板下面）*/
     F_SRC=512,    /*下面必须是水源（level=0，青蛙卵）*/
     F_HOLLOW=1024,/*下面还可以是"中空方块"：漏斗/堆肥桶/炼药锅（铁轨、中继器、比较器、红石线）*/
     F_SOLID=2048,/*lev=3 要看"整块实心"（原版 isSolidRender，蘑菇），不是"这一面是满的"：
                     台阶顶面/楼梯顶面那种"面满但整块不满"的不算。别的 lev=3 规则都是按面判的*/
     F_NO_TNT=4096,/*压力板不能放在TNT上（Wiki Pressure Plate §Placement）*/
     F_AMETHYST=8192};/*紫水晶簇/芽：要求完整面，且树叶/蜂蜜块除外*/

struct Fix_Def{
	const char *name;      /*basename，*结尾=前缀，*开头=后缀*/
	uint8_t rule;          /*RULE_* */
	const char *const *set;/*支持集（NULL结尾；NULL=不用集合）*/
	uint8_t lev;           /*要求的支撑档（0=只看set）*/
	uint16_t flags;        /*F_* */
	const char *key;       /*RULE_HANG的属性名 / RULE_STEM的果实名*/
	uint32_t set_bit=0;    /*set在set_List里的位（def_Init填，不手写）*/
};
static Fix_Def defs[]={
	/*---- 花盆/盆栽放在最前面：规则表按顺序匹配，potted_xxx_sapling 不能被下面的 *_sapling 抢走 ----*/
	{"flower_pot",RULE_NONE,NULL,0,0,NULL},/*wiki:花盆放哪都行（"can be placed on any block or over air"）*/
	{"potted_*",RULE_NONE,NULL,0,0,NULL},
	/*---- 1格植被：#supports_vegetation = #substrate_overworld + 耕地（26.1 的放置支撑标签）----*/
	{"bush",RULE_BELOW,set_veg,0,0,NULL},
	{"firefly_bush",RULE_BELOW,set_veg,0,0,NULL},
	{"short_grass",RULE_BELOW,set_veg,0,0,NULL},
	{"fern",RULE_BELOW,set_veg,0,0,NULL},
	{"pink_petals",RULE_BELOW,set_veg,0,0,NULL},
	{"wildflowers",RULE_BELOW,set_veg,0,0,NULL},
	{"dandelion",RULE_BELOW,set_veg,0,0,NULL},
	{"golden_dandelion",RULE_BELOW,set_veg,0,0,NULL},/*26.1新增的花（快照5）*/
	{"poppy",RULE_BELOW,set_veg,0,0,NULL},
	{"blue_orchid",RULE_BELOW,set_veg,0,0,NULL},
	{"allium",RULE_BELOW,set_veg,0,0,NULL},
	{"azure_bluet",RULE_BELOW,set_veg,0,0,NULL},
	{"red_tulip",RULE_BELOW,set_veg,0,0,NULL},
	{"orange_tulip",RULE_BELOW,set_veg,0,0,NULL},
	{"white_tulip",RULE_BELOW,set_veg,0,0,NULL},
	{"pink_tulip",RULE_BELOW,set_veg,0,0,NULL},
	{"oxeye_daisy",RULE_BELOW,set_veg,0,0,NULL},
	{"cornflower",RULE_BELOW,set_veg,0,0,NULL},
	{"lily_of_the_valley",RULE_BELOW,set_veg,0,0,NULL},
	{"torchflower",RULE_BELOW,set_veg,0,0,NULL},
	{"closed_eyeblossom",RULE_BELOW,set_veg,0,0,NULL},/*眼眸花（开/闭是两个状态）*/
	{"open_eyeblossom",RULE_BELOW,set_veg,0,0,NULL},
	{"sweet_berry_bush",RULE_BELOW,set_veg,0,0,NULL},
	{"*_sapling",RULE_BELOW,set_veg,0,0,NULL},/*各种树苗（必须排在 potted_* 后面）*/
	{"azalea",RULE_BELOW,set_azalea,0,0,NULL},/*#supports_azalea：比植被多一个黏土*/
	{"flowering_azalea",RULE_BELOW,set_azalea,0,0,NULL},
	{"dead_bush",RULE_BELOW,set_dry_veg,0,0,NULL},
	{"short_dry_grass",RULE_BELOW,set_dry_veg,0,0,NULL},
	{"tall_dry_grass",RULE_BELOW,set_dry_veg,0,0,NULL},/*26.1起#dirt只剩 dirt/coarse_dirt/rooted_dirt，别再用老的"泥土系"*/
	{"leaf_litter",RULE_BELOW,NULL,2,0,NULL},/*25w06a起：顶面中心支撑即可（栅栏/墙/锁链也认）*/
	{"wither_rose",RULE_BELOW,set_wither,0,0,NULL},/*#supports_wither_rose：还认下界岩/灵魂沙/灵魂土*/
	{"brown_mushroom",RULE_BELOW,NULL,3,F_NO_ICE|F_SOLID,NULL},/*wiki:下面要是"整块不透明的"，不挑泥土（石头上长蘑菇是合法的）；光照那半条不判。
	                                                              原版 mayPlaceOn=isSolidRender()，是"整块实心"，所以上半砖顶面算满也不行（F_SOLID）*/
	{"red_mushroom",RULE_BELOW,NULL,3,F_NO_ICE|F_SOLID,NULL},
	/*---- 下界那几种（#supports_*_fungus / *_roots / sprouts 都是同一套14个）----*/
	{"crimson_fungus",RULE_BELOW,set_nylium,0,0,NULL},
	{"warped_fungus",RULE_BELOW,set_nylium,0,0,NULL},
	{"crimson_roots",RULE_BELOW,set_nylium,0,0,NULL},
	{"warped_roots",RULE_BELOW,set_nylium,0,0,NULL},
	{"nether_sprouts",RULE_BELOW,set_sprouts,0,0,NULL},
	{"nether_wart",RULE_BELOW,set_soul_sand,0,0,NULL},
	/*---- 庄稼/茎：下面要耕地（#supports_crops）----*/
	{"wheat",RULE_BELOW,set_crops,0,0,NULL},
	{"carrots",RULE_BELOW,set_crops,0,0,NULL},
	{"potatoes",RULE_BELOW,set_crops,0,0,NULL},
	{"beetroots",RULE_BELOW,set_crops,0,0,NULL},
	{"torchflower_crop",RULE_BELOW,set_crops,0,0,NULL},/*成熟期和 torchflower 共用一个ID，不用管：长成了就是花*/
	{"pitcher_crop",RULE_2X,set_crops,0,0,NULL},/*瓶子草作物是两格高（half=lower/upper）*/
	{"melon_stem",RULE_BELOW,set_crops,0,0,NULL},
	{"pumpkin_stem",RULE_BELOW,set_crops,0,0,NULL},
	{"attached_melon_stem",RULE_STEM,set_crops,0,0,"melon"},/*茎只种在farmland上（Wiki Melon/Pumpkin Seeds）*/
	{"attached_pumpkin_stem",RULE_STEM,set_crops,0,0,"pumpkin"},
	/*---- 沙子/陶瓦上长的 ----*/
	{"cactus",RULE_BELOW,set_sand,0,F_SELF|F_NOSIDE,NULL},/*能叠在仙人掌上；4个水平邻居有整方块就破*/
	{"cactus_flower",RULE_BELOW,set_cactus_flower,2,0,NULL},/*#support_override_cactus_flower：仙人掌/耕地上不要求完整顶面*/
	{"sugar_cane",RULE_BELOW,set_sugar,0,F_SELF,NULL},/*能叠；"旁边要有水"锚在下面那格的4个水平邻居上（斜方向），按规矩不判，交给MC*/
	{"bamboo",RULE_BELOW,set_bamboo,0,F_SELF,NULL},
	{"bamboo_sapling",RULE_BELOW,set_bamboo,0,F_SELF,NULL},/*方块id是bamboo_sapling，显示名是Bamboo Shoot*/
	/*---- 要水/长在水里的 ----*/
	{"lily_pad",RULE_BELOW,set_lily,0,F_WET_D,NULL},/*冰/浮冰在set里，水/含水方块由F_WET_D认*/
	{"frogspawn",RULE_BELOW,set_water,0,F_AIR_U|F_SRC,NULL},/*水源+上面空气（#supports_frogspawn 是空标签）*/
	{"seagrass",RULE_BELOW,NULL,3,F_NO_MAGMA,NULL},
	{"kelp",RULE_BELOW,set_kelp,3,F_NO_MAGMA,NULL},
	{"kelp_plant",RULE_BELOW,set_kelp,3,0,NULL},/*海里柱体靠下面那节/海床撑着就行，不用再查水*/
	{"sea_pickle",RULE_BELOW,NULL,0,F_NONAIR,NULL},/*wiki:下面要有支撑（固体、非固体都行，水也算——只要不是空气）*/
	{"bubble_column",RULE_BELOW,set_magma_soul,0,F_SELF|F_WATER,NULL},/*下面要岩浆块/灵魂沙，或者下面还是气泡柱（整列）*/
	/*---- 两格高（上下半截互查 + 下半截看下面）----*/
	{"tall_grass",RULE_2X,set_veg,0,0,NULL},
	{"large_fern",RULE_2X,set_veg,0,0,NULL},
	{"sunflower",RULE_2X,set_veg,0,0,NULL},
	{"lilac",RULE_2X,set_veg,0,0,NULL},
	{"rose_bush",RULE_2X,set_veg,0,0,NULL},
	{"peony",RULE_2X,set_veg,0,0,NULL},
	{"pitcher_plant",RULE_2X,set_veg,0,0,NULL},
	{"tall_seagrass",RULE_2X,NULL,3,F_NO_MAGMA|F_SELF,NULL},/*上下两条叠着也算数；原版不要求周围有水*/
	{"small_dripleaf",RULE_SMALL_DRIPLEAF,NULL,0,0,NULL},
	/*---- 垂滴叶 ----*/
	{"big_dripleaf",RULE_BELOW,set_big_dripleaf_leaf,0,0,NULL},/*叶子：地上/黏土/…，或者坐在**自己的茎**顶上（见set_big_dripleaf_leaf）*/
	{"big_dripleaf_stem",RULE_BELOW,set_big_dripleaf,0,F_SELF,NULL},/*茎能接在茎上；最下面那根坐在support集上。
	                                                                    原版还要求"上面是茎或叶子"（BigDripleafStemBlock.canSurvive 的后半句），
	                                                                    我们没判：顶叶被挖掉的光杆茎会留着——方向是"漏拆"，安全*/
	/*---- 贴面/贴天花板/藤蔓 ----*/
	{"vine",RULE_VINE,NULL,0,0,NULL},
	{"glow_lichen",RULE_FACE6,NULL,3,0,NULL},
	{"sculk_vein",RULE_FACE6,NULL,3,0,NULL},
	{"spore_blossom",RULE_ABOVE,NULL,1,F_NO_LEAVES,NULL},
	{"hanging_roots",RULE_ABOVE,NULL,1,0,NULL},
	{"cave_vines",RULE_ABOVE,set_cave_vine,2,0,NULL},
	{"cave_vines_plant",RULE_ABOVE,set_cave_vine,2,0,NULL},
	{"twisting_vines",RULE_BELOW,NULL,1,F_SELF,NULL},/*缠怨藤往上长：下面只要是方块（含它自己）就行*/
	{"twisting_vines_plant",RULE_BELOW,NULL,1,F_SELF,NULL},
	{"weeping_vines",RULE_ABOVE,NULL,1,F_SELF,NULL},/*垂泪藤往下长：上面只要是方块就行*/
	{"weeping_vines_plant",RULE_ABOVE,NULL,1,F_SELF,NULL},
	{"pointed_dripstone",RULE_DRIP,set_dripstone,3,0,NULL},
	{"amethyst_cluster",RULE_SIDE,set_amethyst,3,F_AMETHYST,NULL},
	{"small_amethyst_bud",RULE_SIDE,set_amethyst,3,F_AMETHYST,NULL},
	{"medium_amethyst_bud",RULE_SIDE,set_amethyst,3,F_AMETHYST,NULL},
	{"large_amethyst_bud",RULE_SIDE,set_amethyst,3,F_AMETHYST,NULL},
	{"cocoa",RULE_COCOA,set_jungle_log,0,0,NULL},
	{"mangrove_propagule",RULE_PROPAGULE,set_azalea,0,0,"hanging"},/*挂着上面必须是红树叶（#supports_hanging_mangrove_propagule），种下看下面*/
	{"moss_carpet",RULE_BELOW,NULL,0,F_NONAIR,NULL},/*wiki没写它的放置条件（只说和地毯差不多），放宽：下面不是空气就行*/
	{"pale_moss_carpet",RULE_BELOW,NULL,0,F_NONAIR,NULL},
	{"*_carpet",RULE_BELOW,NULL,0,F_NONAIR,NULL},
	/*---- 火把/灯笼/梯子/按钮/压力板/牌子/旗帜 ----*/
	{"torch",RULE_BELOW,NULL,2,0,NULL},/*放地上：下面要"顶面坚固"（S_PART以上：栅栏/台阶/玻璃板都算）*/
	{"soul_torch",RULE_BELOW,NULL,2,0,NULL},
	{"copper_torch",RULE_BELOW,NULL,2,0,NULL},/*铜火把（1.21.9 那批铜制品一起加的）*/
	{"redstone_torch",RULE_BELOW,NULL,2,0,NULL},
	{"redstone_wall_torch",RULE_SIDE,NULL,2,0,NULL},/*红石火把比普通火把宽：玻璃/楼梯/开着的活板门侧面也认*/
	{"wall_torch",RULE_SIDE,NULL,3,0,NULL},/*墙火把本体：注意"*_wall_torch"这个后缀匹配不到"wall_torch"它自己*/
	{"*_wall_torch",RULE_SIDE,NULL,3,0,NULL},/*墙上的火把：贴着的那个方块要"完整方形面"*/
	{"lantern",RULE_HANG,NULL,2,0,"hanging"},
	{"soul_lantern",RULE_HANG,NULL,2,0,"hanging"},
	{"copper_lantern",RULE_HANG,NULL,2,0,"hanging"},
	{"*_copper_lantern",RULE_HANG,NULL,2,0,"hanging"},/*氧化/打蜡的那些铜灯笼*/
	{"ladder",RULE_SIDE,NULL,3,0,NULL},
	{"lever",RULE_SIDE,NULL,3,0,NULL},
	{"*_button",RULE_SIDE,NULL,3,0,NULL},
	{"*_pressure_plate",RULE_BELOW,NULL,2,F_NO_LEAVES|F_NO_TNT,NULL},
	{"*_wall_hanging_sign",RULE_NONE,NULL,0,0,NULL},/*wiki:墙上挂的吊牌自带托架，附着块没了也不破（唯一一个这样的）*/
	{"*_hanging_sign",RULE_HANG2,NULL,1,0,NULL},/*吊牌：上面或下面有一个搭着就行（attached属性只是链子交不交于一点）*/
	{"*_wall_sign",RULE_SIDE,NULL,1,0,NULL},
	{"*_sign",RULE_BELOW,NULL,1,0,NULL},/*立牌：下面有方块就行（wiki:栅栏/告示牌上都能放）*/
	{"*_wall_banner",RULE_SIDE,NULL,1,0,NULL},
	{"*_banner",RULE_BELOW,NULL,1,0,NULL},
	{"tripwire_hook",RULE_SIDE,NULL,1,0,NULL},/*wiki只说"贴着一个方块"，放宽点别拆红石装置*/
	/*---- wiki明确说不需要支撑的，列出来省得以后有人问 ----*/
	{"iron_chain",RULE_NONE,NULL,0,0,NULL},/*锁链（1.21.9 起 id 从 chain 改成 iron_chain）能悬空*/
	{"copper_chain",RULE_NONE,NULL,0,0,NULL},
	{"*_copper_chain",RULE_NONE,NULL,0,0,NULL},/*氧化/打蜡的铜链*/
	{"end_rod",RULE_NONE,NULL,0,0,NULL},/*wiki:六个面都能贴，支撑块没了也不破*/
	{"lightning_rod",RULE_NONE,NULL,0,0,NULL},
	{"*_lightning_rod",RULE_NONE,NULL,0,0,NULL},/*避雷针的铜氧化变体*/
	{"tripwire",RULE_NONE,NULL,0,0,NULL},/*wiki:钩子没了线也不破*/
	{"turtle_egg",RULE_NONE,NULL,0,0,NULL},/*wiki:不需要下方支撑（还能往它下面挂灯笼）*/
	{"*_trapdoor",RULE_NONE,NULL,0,0,NULL},/*wiki:活板门附着块没了也不破（开着也一样）*/
	{"*_fence_gate",RULE_NONE,NULL,0,0,NULL},/*wiki:栅栏门可以悬空*/
	{"*_bed",RULE_NONE,NULL,0,0,NULL},/*wiki原文(Java):"beds do not require supporting blocks and can be placed anywhere"；BE那句是"the bed remains in place if its supporting blocks are later removed"——两个版本都说明"下面拆了床不破"*/
	/*---- 兴趣点（POI）方块：wiki 上没有"支撑没了会掉"的依据，登记成"明确不管" ----
	  这 16 个是村民认领的工作站/炼药锅（poi_Defs[] 里那 13 类 + 4 种炼药锅）。
	  它们都是"看着像要底座、其实放下就不用管"的家具方块：wiki 各页的 Breaking/Usage 里
	  **一句支撑/存活的话都没有**（调研见 test/work/wiki/research/g6_audit.md 表2/A.2 的"无"行），
	  而唯一 wiki 明说会掉的是钟（上面那条 RULE_BELL）。这里明确登记成"不管"有两个用处：
	   ①以后有人想给讲台/堆肥桶加规则时能看见这条依据；②mkrule.cpp 里有反例（悬空的工作站必须留）。
	  真正让 fixer 多拆 POI 方块不是这里的事：拆掉床/工作站要按各自的原版机制来，不能拍脑袋。*/
	{"lectern",RULE_NONE,NULL,0,0,NULL},/*wiki无支撑措辞（POI:librarian）*/
	{"composter",RULE_NONE,NULL,0,0,NULL},/*同上（farmer）*/
	{"barrel",RULE_NONE,NULL,0,0,NULL},/*fisherman*/
	{"smoker",RULE_NONE,NULL,0,0,NULL},/*butcher*/
	{"blast_furnace",RULE_NONE,NULL,0,0,NULL},/*armorer*/
	{"cartography_table",RULE_NONE,NULL,0,0,NULL},/*cartographer*/
	{"brewing_stand",RULE_NONE,NULL,0,0,NULL},/*cleric*/
	{"stonecutter",RULE_NONE,NULL,0,0,NULL},/*mason*/
	{"loom",RULE_NONE,NULL,0,0,NULL},/*shepherd*/
	{"smithing_table",RULE_NONE,NULL,0,0,NULL},/*toolsmith*/
	{"grindstone",RULE_NONE,NULL,0,0,NULL},/*wiki原文:"it doesn't need a supporting block."（虽然有face属性，但支撑没了不破）*/
	{"fletching_table",RULE_NONE,NULL,0,0,NULL},/*fletcher*/
	{"cauldron",RULE_NONE,NULL,0,0,NULL},/*leatherworker；4种炼药锅都算*/
	{"water_cauldron",RULE_NONE,NULL,0,0,NULL},
	{"lava_cauldron",RULE_NONE,NULL,0,0,NULL},
	{"powder_snow_cauldron",RULE_NONE,NULL,0,0,NULL},
	{"bee_nest",RULE_NONE,NULL,0,0,NULL},/*蜂巢/蜂箱是完整方块（wiki无支撑措辞），蜂靠 hive_pos 找家，POI 只是顺带*/
	{"beehive",RULE_NONE,NULL,0,0,NULL},
	{"lodestone",RULE_NONE,NULL,0,0,NULL},/*完整方块*/
	{"test_instance_block",RULE_NONE,NULL,0,0,NULL},/*开发用方块：wiki只说"Cannot be placed in Survival"，与支撑无关*/
	{"nether_portal",RULE_NONE,NULL,0,0,NULL},/*wiki原文(Nether Portal (block)页):"Nether portal blocks can stably exist only within a Nether portal structure … When the block is updated and detects that it is not part of such a structure, it is destroyed."——那是"整个门框"的合法性（最多21x21），不是6个面；我们不判，见ReadMe"它不做什么"*/
	/*---- 红石/铁轨：下面要"顶面带边" ----*/
	{"redstone_wire",RULE_RIM,NULL,3,F_HOLLOW,NULL},/*wiki:"full square or hopper"：漏斗/堆肥桶/炼药锅都算*/
	{"repeater",RULE_RIM,NULL,0,F_HOLLOW,NULL},/*wiki:"hollow square"：堆肥桶/炼药锅/漏斗上也能放*/
	{"comparator",RULE_RIM,NULL,0,F_HOLLOW,NULL},
	{"rail",RULE_RIM,NULL,0,F_HOLLOW,NULL},/*顶面带边：整方块/漏斗/上半台阶/倒楼梯/上半活板门*/
	{"*_rail",RULE_RIM,NULL,0,F_HOLLOW,NULL},
	/*---- 蜡烛/蛋糕/珊瑚/雪/门/钟/脚手架/紫颂 ----*/
	{"white_candle",RULE_BELOW,NULL,2,0,NULL},/*wiki:"small square"，和火把(顶放)一档：台阶/栅栏/墙/铁砧上都行*/
	{"*_candle",RULE_BELOW,NULL,2,0,NULL},
	{"cake",RULE_BELOW,NULL,2,0,NULL},/*wiki:只能放地上，下面要 solid（台阶也算）*/
	{"*_coral_wall_fan",RULE_SIDE,NULL,2,0,NULL},/*墙扇：贴侧面（facing的反方向）*/
	{"*_coral_fan",RULE_BELOW,NULL,2,0,NULL},/*地面扇：贴下面。缺水的活珊瑚会变死珊瑚，那是MC自己的事，这里不判*/
	{"snow",RULE_SNOW,NULL,0,0,NULL},
	{"*_door",RULE_DOOR,NULL,3,0,NULL},/*木门/铁门/铜门：下半看下面，上半看下半*/
	{"bell",RULE_BELL,NULL,2,0,NULL},
	{"scaffolding",RULE_SCAFF,NULL,2,0,NULL},
	{"chorus_flower",RULE_CHORUS,NULL,0,0,NULL},/*下面末地石/紫颂植株；下面空气时"水平正好一个紫颂植株"且其余水平方向都是空气*/
	{"chorus_plant",RULE_CHORUS_PLANT,NULL,0,0,NULL},/*末地唯一要按六个面判的：水平连着紫颂时上下要有一格空气；没根时靠旁边那株撑*/
	/*---- 重力方块：正下方是空气/水/岩浆/火才拆（fixer替原版把这次更新做完，不生成掉落物实体） ----
	  用户明确收紧了规则：只有这四种，别的可替换方块（草/雪/藤蔓…）都不删；含水方块（waterlogged）也不算“水”。
	  混凝土粉末（ConcretePowderBlock.updateShape/getStateForPlacement）仍然先看六邻有没有水能碰到，
	  碰到就凝固成混凝土；凝固优先于掉不掉，这是原版行为，不受上面的收紧影响。*/
	{"sand",RULE_FALLING,NULL,0,0,NULL},
	{"red_sand",RULE_FALLING,NULL,0,0,NULL},
	{"gravel",RULE_FALLING,NULL,0,0,NULL},
	{"suspicious_sand",RULE_FALLING,NULL,0,0,NULL},/*26.1.1 的 suspicious sand 也是 FallingBlock，下落即碎*/
	{"suspicious_gravel",RULE_FALLING,NULL,0,0,NULL},
	{"dragon_egg",RULE_FALLING,NULL,0,0,NULL},/*26.1.1 的 DragonEggBlock 继承 FallingBlock（getDelayAfterPlace=5），它真的会掉*/
	{"anvil",RULE_FALLING,NULL,0,0,NULL},
	{"chipped_anvil",RULE_FALLING,NULL,0,0,NULL},
	{"damaged_anvil",RULE_FALLING,NULL,0,0,NULL},
	{"*_concrete_powder",RULE_FALLING,NULL,0,0,NULL},
	/*---- 霜冰（Frost Walker 踩出来的那种）：化成水 ----*/
	{"frosted_ice",RULE_FROST,NULL,0,0,NULL},/*故意的"不管它撑不撑得住"，就是把霜冰换成水（理由见 RULE_FROST 的注释）*/
};
static const int defs_len=(int)(sizeof(defs)/sizeof(defs[0]));

/*名字→规则：全等的放hash表，带*的面少，线性扫（表里靠前的先匹配，所以更具体的写前面）*/
static std::unordered_map<std::string_view,int> def_Map;
static std::vector<int> def_Pat;static void def_Init(){
	if(!def_Map.empty()||!def_Pat.empty()){
		return;
	}
	for(int i=0;i<defs_len;i++){
		const char *n=defs[i].name;
		if(strchr(n,'*')){
			def_Pat.push_back(i);
		}else{
			def_Map[std::string_view(n)]=i;
		}
		if(defs[i].set){/*集合指针→位图里的位：判支持集就不用逐条比名字了*/
			for(int j=0;j<SET_CNT;j++){
				if(set_List[j]==defs[i].set){
					defs[i].set_bit=1u<<j;
					break;
				}
			}
			if(!defs[i].set_bit){
				printf("rules: 规则表里的集合没登记（defs[%d]=%s），支持集判定会失效\n",i,defs[i].name);
			}
		}
	}
}
static int def_Find(std::string_view n){/*方块basename→defs下标，-1=不管*/
	if(n.empty()){
		return -1;
	}
	def_Init();
	std::unordered_map<std::string_view,int>::iterator it=def_Map.find(n);
	if(it!=def_Map.end()){
		return it->second;
	}
	for(size_t i=0;i<def_Pat.size();i++){
		if(pat_Match(defs[def_Pat[i]].name,n)){
			return def_Pat[i];
		}
	}
	return -1;
}
/*规则家族：规则的粗分类，fixer的 rules= 就是按这些位开关（一个家族一次关掉）*/
enum{G_BELOW=1u<<0,G_2X=1u<<1,G_VINE=1u<<2,G_ATTACH=1u<<3,G_MISC=1u<<4};
static const char *group_Name_Tab[]={"below","2x","vine","attach","misc"};
#define GROUP_CNT ((int)(sizeof(group_Name_Tab)/sizeof(group_Name_Tab[0])))
static uint32_t def_Group(const Fix_Def &d){/*这条规则属于哪个家族*/
	switch(d.rule){
		case RULE_BELOW:
		case RULE_RIM:
		case RULE_CHORUS:
			return G_BELOW;
		case RULE_2X:
		case RULE_SMALL_DRIPLEAF:
		case RULE_PROPAGULE:
		case RULE_STEM:
			return G_2X;
		case RULE_VINE:
			return G_VINE;
		case RULE_FACE6:
		case RULE_SIDE:
		case RULE_ABOVE:
		case RULE_DRIP:
			return G_ATTACH;
		default:
			return G_MISC;
	}
}

/*==================== 4. 方块分类（按名字记一次；三档支撑 + 集合位图 + 规则号） ====================*/
enum{B_AIR=1,       /*空气（含"没有这一段"，那时basename是空串）*/
     B_SLAB=2,      /*名字以_slab结尾：还要看type=double才能定档*/
     B_WATER_NAME=4,/*名字本身就是水/水草/珊瑚/霜冰（判"旁边有没有水"时不用再看属性）*/
     B_ICE=8,       /*冰（雪层不能放冰上）*/
     B_MAGMA=16,    /*岩浆块（海带/海草下面不能是它）*/
     B_LEAVES=32,   /*树叶（孢子花不能贴它下面）*/
     B_MC=64};      /*是minecraft命名空间（mod方块不碰）*/
static int thin_Is(std::string_view n){/*薄片：撑不住任何东西（草、花、火把、铁轨、告示牌…）*/
	static const char *exact[]={
		"air","cave_air","void_air","water","lava","bubble_column","light","structure_void",
		"fire","soul_fire","nether_portal","end_portal","end_gateway","cobweb",
		"short_grass","fern","dead_bush","bush","firefly_bush","cactus_flower","mangrove_propagule",
		"pitcher_crop","torchflower_crop","tall_grass","large_fern","seagrass","tall_seagrass","kelp","kelp_plant",
		"sugar_cane","bamboo","bamboo_sapling","wheat","carrots","potatoes","beetroots",
		"melon_stem","pumpkin_stem","attached_melon_stem","attached_pumpkin_stem","nether_wart",
		"vine","glow_lichen","sculk_vein","cave_vines","cave_vines_plant","hanging_roots","spore_blossom",
		"lily_pad","frogspawn","sea_pickle","pointed_dripstone","amethyst_cluster",
		"small_amethyst_bud","medium_amethyst_bud","large_amethyst_bud","moss_carpet","pale_moss_carpet",
		"resin_clump","powder_snow","chorus_flower","chorus_plant",
		"snow","leaf_litter","pink_petals","wildflowers","short_dry_grass","tall_dry_grass","torchflower",
		"pitcher_plant","sunflower","lilac","rose_bush","peony","sweet_berry_bush","brown_mushroom","red_mushroom",
		"crimson_fungus","warped_fungus","crimson_roots","warped_roots","nether_sprouts","cocoa",
		"big_dripleaf","big_dripleaf_stem","small_dripleaf","redstone_wire","tripwire","tripwire_hook",
		"lever","repeater","comparator","rail","powered_rail","detector_rail","activator_rail",
		"flower_pot","turtle_egg","sniffer_egg","end_rod","lightning_rod","ladder",
		"redstone_torch","torch","soul_torch","lantern","soul_lantern","copper_lantern",
		"pale_hanging_moss","hanging_moss",
		/*单格花（#small_flowers 那17种）：原版全是 noCollision，撑不住东西，也不该算"整方块"*/
		"dandelion","open_eyeblossom","poppy","blue_orchid","allium","azure_bluet","red_tulip","orange_tulip",
		"white_tulip","pink_tulip","oxeye_daisy","cornflower","lily_of_the_valley","wither_rose","torchflower",
		"closed_eyeblossom","golden_dandelion"};/*悬垂的两种苔藓也当薄片*/
	for(size_t i=0;i<sizeof(exact)/sizeof(exact[0]);i++){
		if(n==exact[i]){
			return 1;
		}
	}
	static const char *pats[]={"*_sapling","*_torch","*_button","*_pressure_plate","*_sign","*_banner",
		"*_door","*_carpet","*_candle","*_bed","*_coral_fan","*_wall_fan","*_bud","potted_*","*_hanging_sign",
		"*_copper_lantern","*_lightning_rod"};/*铜灯笼(1.21.9起有铜的)、铜避雷针这些变体都算薄片。
		这里**不能**写"*_lantern"：jack_o_lantern（南瓜灯）和 sea_lantern（海晶灯）都是整方块
		（原版没覆盖 getShape），被那条模式一起吞进来会让"放在南瓜灯/海晶灯上的东西"被误拆
		（南瓜灯上放火把是很正常的摆法）；lantern/soul_lantern 两个真灯笼在上面 exact 里列着。*/
	for(size_t i=0;i<sizeof(pats)/sizeof(pats[0]);i++){
		if(pat_Match(pats[i],n)){
			return 1;
		}
	}
	return 0;
}
static int part_Is(std::string_view n){/*有形状的：上下能顶住，侧面不能贴
	注意灵魂沙**不在**这里：它虽然只有14/16的走路碰撞箱，但原版 SoulSandBlock 特意把
	getBlockSupportShape 覆盖成整方块，而 getShape（决定遮挡/是否"实心可放东西"）也没覆盖、
	默认就是整方块——所以对"支撑"这件事它就是整方块：蘑菇能长在上面（26.1.1 的 MushroomBlock.mayPlaceOn
	= isSolidRender()，灵魂沙为真）、火把/梯子/红石/海草都能放，仙人掌挨着它照样破。
	以前把它归成"有形状的"，下界 15298 个蘑菇里就有 144 个长在灵魂沙上被误拆（见ReadMeDeveloper）。
	泥土/土径这种原版确实覆盖了 getShape 的才留在这里。*/
	static const char *pats[]={"*_slab","*_stairs","*_fence","*_fence_gate","*_pane","*_trapdoor","*_wall",
		"*_bars","*_chain"};/*铁栏杆/铜栏杆、锁链(iron_chain/铜链)都算"有形状的"*/
	for(size_t i=0;i<sizeof(pats)/sizeof(pats[0]);i++){
		if(pat_Match(pats[i],n)){
			return 1;
		}
	}
	static const char *exact[]={"chest","trapped_chest","ender_chest","barrel","hopper","cauldron","water_cauldron",
		"lava_cauldron","powder_snow_cauldron","composter","brewing_stand","enchanting_table","end_portal_frame",
		"farmland","dirt_path","scaffolding","decorated_pot","lectern","grindstone","stonecutter",
		"cartography_table","fletching_table","smithing_table","loom","bell","campfire","soul_campfire","conduit",
		"anvil","chipped_anvil","damaged_anvil","dragon_egg","iron_bars","cake","candle_cake","iron_chain","chain"};
	for(size_t i=0;i<sizeof(exact)/sizeof(exact[0]);i++){
		if(n==exact[i]){
			return 1;
		}
	}
	return 0;
}
static int is_Empty(std::string_view n){/*空气/流体/光/传送门这种"压根不是方块"的（水也算撑不住东西）*/
	return n.empty()||n=="air"||n=="cave_air"||n=="void_air"||n=="water"||n=="lava"||n=="bubble_column"||
		n=="light"||n=="structure_void"||n=="fire"||n=="soul_fire"||n=="nether_portal"||
		n=="end_portal"||n=="end_gateway";
}
static int is_Air_Name(std::string_view n){/*真的空气：水不算！（"上面必须是空气""下面不是空气就行"用这个，别跟is_Empty混了）*/
	return n.empty()||n=="air"||n=="cave_air"||n=="void_air";
}
static int free_Below(const Blk_Info *b){/*用户26.1.1规则：重力方块下方是空气/水/岩浆/火才拆。
	空气含cave_air/void_air；水只认真正的minecraft:water（含水方块、水草不算——用户明确说“可替换方块不拆”）。
	岩浆/火同理：只有方块本身就是岩浆/火才算，不把“可替换标签”一股脑算进来。*/
	if(!b){
		return 0;/*读不出来：让调用方当blind处理，绝不能当空气*/
	}
	return (b->flags&BF_AIR)||b->base=="water"||b->base=="lava"||
		b->base=="fire"||b->base=="soul_fire";
}
static int lev_Of_Name(std::string_view n){/*光看名字能定的支撑档（台阶的双台阶要再看属性，见Blk_Info::fill）*/
	if(is_Empty(n)){
		return S_NONE;
	}
	if(thin_Is(n)){
		return S_THIN;
	}
	if(end_With(n,"_slab")){
		return S_PART;
	}
	if(part_Is(n)){
		return S_PART;
	}
	return S_FULL;/*屏障也算整方块：能往上放东西，宁可漏拆*/
}

struct Name_Info{          /*只跟名字有关的那部分：同一个名字（各种属性、各个区块）共用一份*/
	std::string_view base; /*冒号后面那段*/
	uint8_t lev;           /*支撑档 S_* */
	uint8_t nflags;        /*B_*（只有跟名字有关的那些位）*/
	int16_t def;           /*defs下标，-1=不管（mod方块也是-1）*/
	uint32_t set_mask;     /*命中哪几个支持集（位对应set_List下标）*/
};
/*名字池：hash表的key指在池子里那份字符串上（NBT里的字符串活不过区块缓存，不能拿来当key）*/
static std::deque<std::string> name_Pool;
static std::unordered_map<std::string_view,Name_Info> name_Map;
static const Name_Info &name_Info_Of(std::string_view nm){
	std::unordered_map<std::string_view,Name_Info>::iterator it=name_Map.find(nm);
	if(it!=name_Map.end()){
		return it->second;
	}
	name_Pool.push_back(std::string(nm));
	std::string_view own(name_Pool.back());
	std::string_view n=base_Of(own);
	Name_Info ni;
	ni.base=n;
	unsigned f=0;
	size_t p=own.find(':');
	if(p==std::string_view::npos||own.substr(0,p)=="minecraft"){
		f|=B_MC;
	}
	if(is_Air_Name(n)){
		f|=B_AIR;
	}
	if(n=="ice"||n=="packed_ice"||n=="blue_ice"||n=="frosted_ice"){
		f|=B_ICE;
	}
	if(n=="magma_block"){
		f|=B_MAGMA;
	}
	if(end_With(n,"_leaves")){
		f|=B_LEAVES;
	}
	if(n=="water"||n=="bubble_column"||n=="kelp"||n=="kelp_plant"||n=="seagrass"||
	   n=="tall_seagrass"){
		f|=B_WATER_NAME;/*只有名字本身就是水/水草的才无条件算水；珊瑚/海泡菜靠waterlogged属性*/
	}
	if(end_With(n,"_slab")){
		f|=B_SLAB;
	}
	ni.lev=(uint8_t)lev_Of_Name(n);
	ni.nflags=(uint8_t)f;
	ni.def=(f&B_MC)?(int16_t)def_Find(n):-1;
	uint32_t m=0;
	for(int j=0;j<SET_CNT;j++){
		for(int k=0;set_List[j][k];k++){
			if(pat_Match(set_List[j][k],n)){
				m|=1u<<j;
				break;
			}
		}
	}
	ni.set_mask=m;
	return name_Map.emplace(std::string_view(name_Pool.back()),ni).first->second;
}
void Blk_Info::fill(nbt_Any *b){/*一个调色板条目只算一次*/
	const Name_Info &ni=name_Info_Of(std::string_view(get_Name(b)));
	blk=b;
	base=ni.base;
	lev=ni.lev;
	def=ni.def;
	set_mask=ni.set_mask;
	unsigned f=0;
	if(ni.nflags&B_AIR){
		f|=BF_AIR;
	}
	if(ni.nflags&B_ICE){
		f|=BF_ICE;
	}
	if(ni.nflags&B_MAGMA){
		f|=BF_MAGMA;
	}
	if(ni.nflags&B_LEAVES){
		f|=BF_LEAVES;
	}
	if((ni.nflags&B_SLAB)&&is_PropValue(b,"type","double")){
		lev=S_FULL;/*双台阶就是整方块*/
	}
	if(ni.base=="snow"&&is_PropValue(b,"layers","8")){
		lev=S_FULL;/*8层雪就是整方块：SnowLayerBlock.getBlockSupportShape=SHAPES[layers]，
		             8层那格顶面是满的，原版上面能放火把/铁轨/红石/门，雪也能叠上去（layers<8才只算薄片）*/
	}
	if(ni.nflags&B_WATER_NAME){
		f|=BF_WATER;/*名字就是水：不用看属性*/
	}else if(is_PropValue(b,"waterlogged","true")){
		f|=BF_WATER;/*含水的方块也算水（甘蔗、海带旁边认这个）*/
	}
	if(ni.base=="water"){
		const char *lv=get_Prop(b,"level");
		if(!lv||!strcmp(lv,"0")){
			f|=BF_WATER_SRC;/*水源（流动水不算）*/
		}
	}else if(is_PropValue(b,"waterlogged","true")||ni.base=="kelp"||ni.base=="kelp_plant"||
	         ni.base=="seagrass"||ni.base=="tall_seagrass"||ni.base=="bubble_column"){
		f|=BF_WATER_SRC;/*含水方块、水草海带里面的水也是水源（wiki青蛙卵：含水方块上也能放）*/
	}
	flags=(uint8_t)f;
}

/*---- 支持/形状的小判断（规则函数用） ----*/
static int wet_Below(const Blk_Info *b){/*下面那格"有水的流体"：水和含水方块、水草海带都算；
	珊瑚块是实心方块（自己不含水），睡莲放珊瑚块上是不行的，所以单拎出来排除*/
	return b&&(b->flags&BF_WATER)&&!end_With(b->base,"_coral_block");
}
static int water_Src(const Blk_Info *b){return b&&(b->flags&BF_WATER_SRC);}
static int lev_Of(const Blk_Info *b){return b?(int)b->lev:(int)S_NONE;}
static int solid_No(const Blk_Info *b){/*原版 isSolid()==false 的那几个"整方块"（Blocks.java 里 forceSolidOff 的）：
	仙人掌的 F_NOSIDE 照原版用 isSolid() 判，碰上它们不算"旁边有实心方块"。
	（8层雪也是 forceSolidOff，见 Blk_Info::fill 里的 layers 特判；梯子/末地烛/大垂滴叶本来就是薄片档，不用列）*/
	if(!b){
		return 0;
	}
	static const char *exact[]={"chorus_plant","chorus_flower","azalea","flowering_azalea","snow"};
	for(size_t i=0;i<sizeof(exact)/sizeof(exact[0]);i++){
		if(b->base==exact[i]){
			return 1;
		}
	}
	return 0;
}
static int same_Base(const Blk_Info *a,const Blk_Info *b){/*同一种方块（不比属性，原版 is(this) 就是这个意思）*/
	return a&&b&&!a->base.empty()&&a->base==b->base;
}

/*---- 支撑"按面"算：dir＝从支撑方块 b 指向被支撑的方块（也就是 b 的哪个面在顶着它） ----
  原版两档是 SupportType.FULL（isFaceFull(支撑形状, dir)）和 SupportType.CENTER
  （support shape 的那个面 + 中心 2x2x10 的柱子）。同一档支撑对不同的面结论可能不一样：
  · 整方块：六个面都算；
  · 台阶：只有"碰到格子边界"的那个面——上半砖的顶面、下半砖的底面（wiki Slab 原句：
    "Generally, the top face of top slabs, the bottom face of bottom slabs, and all faces of double slabs
     are handled as solid blocks"）；另一个竖直面形状压根没碰到那边（空的），侧面只有半张脸；
  · 楼梯：上半楼梯只有顶面、下半楼梯只有底面（wiki Stairs："Generally, the solid faces of stairs are handled
    as solid blocks"；朝 facing 的侧面其实也是满的，但内外角台阶会变形，先不认——方向是"宁可漏拆"）；
  · 活板门：关着的上半活板门只有顶面、下半只有底面（wiki Trapdoor："Lanterns ... can be placed on top of a
    closed trapdoor that occupies the top half of a block, and below a closed trapdoor that occupies the bottom
    half"）；开着是竖板，顶/底都不算；
  · 其余"有形状的"（栅栏/墙/箱子/玻璃板…）：形状随连接变，逐面算容易算错，保持以前的宽松判法
    （上下都当能中心支撑），方向仍是"宁可漏拆"。
  以前只有一个不带方向的 lev：上半砖和下半砖都当"上下都能撑"，结果
    · 上半砖上的门/海草（原版 isFaceSturdy(UP)=真）被误拆；
    · 下半砖下的钟乳石/吊灯（原版 isFaceSturdy(DOWN)=真）被误拆；
    · 反过来下半砖上的火把、上半砖下的吊灯（原版撑不住）又留着。
  所以 sup_OK 现在必须带方向；调用点里的 dir 就是"支撑块朝自己那一面"（rule_Below 是 NB_U，rule_Above 是 NB_D…）。*/
static int slab_Face(const Blk_Info *b,int dir){/*台阶：这个面能不能撑（上半砖顶面/下半砖底面/双台阶哪面都行）*/
	if(is_PropValue(b->blk,"type","double")){
		return 1;/*双台阶是整方块（lev 已经被填成 S_FULL，这里是保险）*/
	}
	if(dir==NB_U){
		return is_PropValue(b->blk,"type","top");
	}
	if(dir==NB_D){
		return is_PropValue(b->blk,"type","bottom");
	}
	return 0;
}
static int face_Full(const Blk_Info *b,int dir){/*这个面够不够"整面"（SupportType.FULL）*/
	if(!b||b->lev<S_PART){
		return 0;
	}
	if(b->lev>=S_FULL){
		return 1;
	}
	if(end_With(b->base,"_slab")){
		return slab_Face(b,dir);
	}
	if(end_With(b->base,"_stairs")){
		if(dir==NB_U){
			return is_PropValue(b->blk,"half","top");
		}
		if(dir==NB_D){
			return is_PropValue(b->blk,"half","bottom");
		}
		return 0;
	}
	if(end_With(b->base,"_trapdoor")){
		if(is_PropValue(b->blk,"open","true")){
			return 0;
		}
		if(dir==NB_U){
			return is_PropValue(b->blk,"half","top");
		}
		if(dir==NB_D){
			return is_PropValue(b->blk,"half","bottom");
		}
		return 0;
	}
	return 0;/*其余"有形状的"面都不算满（栅栏/墙/箱子/玻璃板…）*/
}
static int face_Center(const Blk_Info *b,int dir){/*这个面够不够"中心支撑"（SupportType.CENTER；火把/灯笼/告示牌那一档）*/
	if(!b||b->lev<S_PART){
		return 0;
	}
	if(b->lev>=S_FULL){
		return 1;
	}
	if(end_With(b->base,"_slab")){
		return slab_Face(b,dir);
	}
	if(end_With(b->base,"_stairs")||end_With(b->base,"_trapdoor")){
		if(dir==NB_U||dir==NB_D){/*竖直面按原版：上半只有顶面、下半只有底面*/
			return face_Full(b,dir);
		}
		return 1;/*侧面：直楼梯朝 facing 那面是满的、别的面不是，内外角还会变——宁可漏拆*/
	}
	if(end_With(b->base,"_fence_gate")){
		return 0;/*1.19.3起栅栏门不再提供center support（Wiki Fence Gate §History）*/
	}
	return 1;/*其余"有形状的"：上下左右都当能中心支撑（栅栏顶上插火把就是这条），旧行为不变*/
}
static int sup_OK(const Fix_Def &d,const Blk_Info *b,int dir){/*b 的 dir 面够不够格当支持*/
	if(!b){
		return 0;
	}
	if(d.set&&(b->set_mask&d.set_bit)&&!(d.flags&F_AMETHYST)){
		return 1;/*支持集是标签（#supports_vegetation 这种）：原版整块比名字，不分方向。
		          紫水晶例外：set_amethyst 里还列了芽/簇，不能靠支持集跳过完整面检查*/
	}
	if(!d.lev){
		return 0;
	}
	if(d.lev==S_THIN){
		return b->lev>=S_THIN;/*"只要有东西"（垂泪藤这类）*/
	}
	if(d.flags&F_SOLID){
		return b->lev>=S_FULL;/*原版 isSolidRender（蘑菇）：整块实心，不分方向*/
	}
	if(d.lev>=S_FULL){
		return face_Full(b,dir);
	}
	return face_Center(b,dir);
}
static int in_Set(const char *const *set,const Blk_Info *b){/*b的basename在不在这个集合里（给几个特判用）*/
	if(!b||!set){
		return 0;
	}
	for(int i=0;set[i];i++){
		if(pat_Match(set[i],b->base)){
			return 1;
		}
	}
	return 0;
}
static int hollow_OK(const Blk_Info *b){/*原版"中空方形"：堆肥桶/炼药锅（铁轨、中继器、比较器、红石线认这个）*/
	if(!b){
		return 0;
	}
	return b->base=="composter"||b->base=="cauldron"||b->base=="water_cauldron"||
	       b->base=="lava_cauldron"||b->base=="powder_snow_cauldron";
}
static int rim_OK(const Blk_Info *b){/*原版"顶面带边"：整方块，或者漏斗/上半台阶/倒楼梯/上半活板门*/
	if(!b||b->lev<S_PART){
		return 0;
	}
	if(b->lev>=S_FULL){
		return 1;
	}
	if(b->base=="hopper"){
		return 1;
	}
	if(end_With(b->base,"_slab")){
		return is_PropValue(b->blk,"type","top");
	}
	if(end_With(b->base,"_stairs")){
		return is_PropValue(b->blk,"half","top");
	}
	if(end_With(b->base,"_trapdoor")){
		if(is_PropValue(b->blk,"open","true")){
			return 0;/*开着的活板门不是实体面，撑不了铁轨/雪/红石/落叶层/海带*/
		}
		return is_PropValue(b->blk,"half","top");
	}
	return 0;
}
static int vine_Side(const Blk_Info *b){/*藤蔓能贴的侧面：整方块；另外原型lists.py的vine_attachable里
	把耕地/土径/木桶也算"能贴"，我们这几个归成了"有形状的"，这里单独放行（宁可放过，别关掉合法藤蔓的面）*/
	if(!b){
		return 0;
	}
	if(b->lev>=S_FULL){
		return 1;
	}
	return b->base=="farmland"||b->base=="dirt_path"||b->base=="barrel";
}

/*==================== 5. 判一条规则 ====================*/
static int nb_Opp_Idx(int dir){/*反方向的下标（dir 从 A 指向 B，对面就是 B 指向 A）*/
	static const int o[6]={NB_W,NB_E,NB_D,NB_U,NB_N,NB_S};
	return dir>=0&&dir<6?o[dir]:-1;
}
static int nb_Idx(const char *dir){/*方向名→NB_* 下标；不认识给-1*/
	if(!dir){
		return -1;
	}
	if(!strcmp(dir,"east")){
		return NB_E;
	}
	if(!strcmp(dir,"west")){
		return NB_W;
	}
	if(!strcmp(dir,"up")){
		return NB_U;
	}
	if(!strcmp(dir,"down")){
		return NB_D;
	}
	if(!strcmp(dir,"south")){
		return NB_S;
	}
	if(!strcmp(dir,"north")){
		return NB_N;
	}
	return -1;
}
static const int nb_h[4]={NB_E,NB_W,NB_S,NB_N};/*四个水平方向*/
static const Blk_Info *nb_Get(Rules_Ctx &c,int dir){/*只看fixer填好的六格；没填/读不出来就作废这格*/
	if(dir<0||dir>5){
		return NULL;
	}
	if(!(c.got&(1<<dir))||(c.bad&(1<<dir))){
		c.blind=1;
		return NULL;
	}
	return c.nb[dir];
}
static const Blk_Info *nb_Dir(Rules_Ctx &c,const char *dir){/*按方向名（facing那种）取邻居*/
	if(!dir){
		return NULL;
	}
	if(!strcmp(dir,"east")){
		return nb_Get(c,NB_E);
	}
	if(!strcmp(dir,"west")){
		return nb_Get(c,NB_W);
	}
	if(!strcmp(dir,"up")){
		return nb_Get(c,NB_U);
	}
	if(!strcmp(dir,"down")){
		return nb_Get(c,NB_D);
	}
	if(!strcmp(dir,"south")){
		return nb_Get(c,NB_S);
	}
	if(!strcmp(dir,"north")){
		return nb_Get(c,NB_N);
	}
	return NULL;
}
static const Blk_Info *nb_Opp(Rules_Ctx &c,const char *dir){/*反方向的邻居（facing指哪边，支撑就在对面）*/
	if(!dir){
		return NULL;
	}
	if(!strcmp(dir,"east")){
		return nb_Get(c,NB_W);
	}
	if(!strcmp(dir,"west")){
		return nb_Get(c,NB_E);
	}
	if(!strcmp(dir,"up")){
		return nb_Get(c,NB_D);
	}
	if(!strcmp(dir,"down")){
		return nb_Get(c,NB_U);
	}
	if(!strcmp(dir,"south")){
		return nb_Get(c,NB_N);
	}
	if(!strcmp(dir,"north")){
		return nb_Get(c,NB_S);
	}
	return NULL;
}
static int touches_Water(Rules_Ctx &c){/*混凝土粉末碰到水（26.1.1 ConcretePowderBlock.touchesLiquid 的意思）：
	六邻里凡是"含水的"（水方块、含水方块、水草海带…），只要它朝我们这一面不是满的，水就能碰到粉末 → 凝固。
	满面的含水方块（比如泡水的楼梯那半面）水碰不过来，不算。*/
	static const int dirs[6]={NB_U,NB_D,NB_E,NB_W,NB_S,NB_N};
	for(int i=0;i<6;i++){
		const Blk_Info *n=nb_Get(c,dirs[i]);
		if(!n){
			continue;/*读不出来：blind已经置上，这一格结论作废*/
		}
		int wet=(n->base=="water")||(n->flags&BF_WATER);
		if(wet&&!face_Full(n,nb_Opp_Idx(dirs[i]))){
			return 1;
		}
	}
	return 0;
}
static int fx_Set(Rules_Ctx &c,nbt_Any *b,int keep){/*新状态交给主机写，写完主机free*/
	c.out_block=b;
	c.out_keep_be=keep;
	return FX_SET;
}
static nbt_Any *state_Edit(nbt_Any *self,const char *const *keys,const char *const *vals,int n){/*复制一份把属性改掉（藤蔓关面用）*/
	nbt_Any *copy;
	nbt_Any *p;
	if(!self){
		return NULL;
	}
	copy=self->copy();
	if(copy->type!=0x0a){/*自己不是compound：原样拿回去（外面会当"没改"，不会白写一次）*/
		return copy;
	}
	p=((nbt_Compound*)copy)->get("Properties");
	if(!p||p->type!=0x0a){/*Properties不是compound（坏存档）：别硬转过去set_String，那是在写别人的内存*/
		return copy;
	}
	nbt_Compound *pr=(nbt_Compound*)p;
	for(int i=0;i<n;i++){
		pr->set_String(keys[i],vals[i]);
	}
	return copy;
}
static int rule_Below(const Fix_Def &d,Rules_Ctx &c){/*下面要有支持（最常见的一条）；支撑块朝上那一面（NB_U）*/
	const Blk_Info *b=nb_Get(c,NB_D);
	int self_ok=(d.flags&F_SELF)&&same_Base(b,c.self);
	int ok=sup_OK(d,b,NB_U)||self_ok;
	if(!ok&&(d.flags&F_NONAIR)){/*只要不是空气（水也算）*/
		ok=b&&!blk_Air(b);
	}
	if(!ok&&(d.flags&F_WET_D)){/*下面得是水：wiki说睡莲能放在"水或含水方块"上，海草海带这些泡在水里的也算水*/
		ok=wet_Below(b);
	}
	if(!ok){
		return FX_AIR;
	}
	if((d.flags&F_NO_ICE)&&b&&(b->flags&BF_ICE)){/*蘑菇这种"要完整不透明方块"的：冰是整方块但不能长*/
		return FX_AIR;
	}
	if((d.flags&F_NO_LEAVES)&&b&&(b->flags&BF_LEAVES)){
		return FX_AIR;/*压力板不能放在树叶上（孢子花那条是 rule_Above，单独查）*/
	}
	if((d.flags&F_NO_TNT)&&b&&b->base=="tnt"){
		return FX_AIR;/*压力板不能放在TNT上*/
	}
	if(d.flags&F_NOSIDE){/*仙人掌旁边不能有"实心方块"：这条要先查，叠同类也不能免。
	                        原版判的是 neighbor.isSolid()，而 isSolid() 对 forceSolidOff 的方块是 false
	                        （见 solid_No），所以不能只看"是不是整方块"*/
		for(int i=0;i<4;i++){
			const Blk_Info *s=nb_Get(c,nb_h[i]);
			if(lev_Of(s)>=S_FULL&&!solid_No(s)){
				return FX_AIR;
			}
		}
	}
	if(self_ok){
		return FX_KEEP;/*下面就是同类（竹子叠竹子/海带叠海带）：原版直接算数，不用再查别的*/
	}
	if((d.flags&F_NO_MAGMA)&&b&&(b->flags&BF_MAGMA)){
		return FX_AIR;
	}
	if((d.flags&F_SRC)&&!water_Src(b)){/*青蛙卵要水源，流动水不行*/
		return FX_AIR;
	}
	if((d.flags&F_WATER)&&!(c.self->flags&BF_WATER)){/*自己不是水里的方块（海草/海带/气泡柱本身都含水）*/
		return FX_AIR;
	}
	if((d.flags&F_AIR_U)&&!blk_Air(nb_Get(c,NB_U))){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_2X(const Fix_Def &d,Rules_Ctx &c){/*两格高：上下半截互查*/
	const char *half=get_Prop(c.self->blk,"half");
	if(!half){
		return FX_KEEP;/*没有half属性就当普通方块，不动它*/
	}
	if(!strcmp(half,"upper")){
		const Blk_Info *b=nb_Get(c,NB_D);
		if(!same_Base(b,c.self)||!is_PropValue(b->blk,"half","lower")){
			return FX_AIR;
		}
		return FX_KEEP;
	}
	const Blk_Info *u=nb_Get(c,NB_U);
	if(!same_Base(u,c.self)||!is_PropValue(u->blk,"half","upper")){
		return FX_AIR;
	}
	const Blk_Info *b=nb_Get(c,NB_D);
	if(!sup_OK(d,b,NB_U)&&!((d.flags&F_SELF)&&same_Base(b,c.self))){
		return FX_AIR;
	}
	if((d.flags&F_NO_MAGMA)&&b&&(b->flags&BF_MAGMA)){/*先查b再读别的格，b的指针更稳*/
		return FX_AIR;
	}
	if((d.flags&F_WATER)&&!(c.self->flags&BF_WATER)){/*自己不是水里的方块（海草/海带/气泡柱本身都含水）*/
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Vine(const Fix_Def &d,Rules_Ctx &c){/*藤蔓：wiki:贴在"填满整格且挡住移动"的方块侧面，或者方块底面*/
	static const char *faces[]={"north","south","east","west","up"};
	int keep_cnt=0;
	const char *bad[5];
	int bad_cnt=0;
	for(int i=0;i<5;i++){
		if(!is_PropValue(c.self->blk,faces[i],"true")){
			continue;
		}
		int ok;
		if(!strcmp(faces[i],"up")){/*贴在方块底面（wiki:不能贴藤蔓底面）*/
			ok=lev_Of(nb_Get(c,NB_U))>=S_PART;
		}else{/*侧面：背后是整方块；或者那一面旁边/上面那格是带同面的藤蔓（原版canSupportAtFace这两条都认）*/
			const Blk_Info *side=nb_Dir(c,faces[i]);
			ok=vine_Side(side);
			if(!ok&&same_Base(side,c.self)&&is_PropValue(side->blk,faces[i],"true")){
				ok=1;
			}
			if(!ok){
				const Blk_Info *up=nb_Get(c,NB_U);
				if(same_Base(up,c.self)&&is_PropValue(up->blk,faces[i],"true")){
					ok=1;
				}
			}
		}
		if(ok){
			keep_cnt++;
		}else{
			bad[bad_cnt++]=faces[i];
		}
	}
	if(!keep_cnt){
		return FX_AIR;
	}
	if(bad_cnt){
		const char *vals[5];
		for(int i=0;i<bad_cnt;i++){
			vals[i]="false";
		}
		return fx_Set(c,state_Edit(c.self->blk,bad,vals,bad_cnt),1);/*关掉撑不住的面，保留方块实体*/
	}
	(void)d;
	return FX_KEEP;
}
static int rule_Face6(const Fix_Def &d,Rules_Ctx &c){/*六个面各查一遍（发光地衣/幽匿脉络）*/
	static const char *faces[]={"down","up","north","south","east","west"};
	int keep_cnt=0;
	const char *bad[6];
	int bad_cnt=0;
	for(int i=0;i<6;i++){
		if(!is_PropValue(c.self->blk,faces[i],"true")){
			continue;
		}
		int fi=nb_Idx(faces[i]);/*支撑块在 faces[i] 那边，它朝我们的是反方向那一面*/
		if(fi>=0&&sup_OK(d,nb_Get(c,fi),nb_Opp_Idx(fi))){
			keep_cnt++;
		}else{
			bad[bad_cnt++]=faces[i];
		}
	}
	if(!keep_cnt){
		return FX_AIR;
	}
	if(bad_cnt){
		const char *vals[6];
		for(int i=0;i<bad_cnt;i++){
			vals[i]="false";
		}
		return fx_Set(c,state_Edit(c.self->blk,bad,vals,bad_cnt),1);
	}
	return FX_KEEP;
}
static int rule_Side(const Fix_Def &d,Rules_Ctx &c){/*贴一面：facing的反方向（face=floor/ceiling时看下/上）*/
	const char *face=get_Prop(c.self->blk,"face");
	const char *facing=get_Prop(c.self->blk,"facing");
	const Blk_Info *sup=NULL;
	int dir=NB_U;/*支撑块朝我们的是哪一面（dir=从支撑块指向自己）*/
	if(face&&!strcmp(face,"floor")){
		sup=nb_Get(c,NB_D);
		dir=NB_U;
	}else if(face&&!strcmp(face,"ceiling")){
		sup=nb_Get(c,NB_U);
		dir=NB_D;
	}else if(!facing){
		return FX_KEEP;/*没有facing属性（坏存档/以后版本改了属性名）：判断不了贴在哪面，宁可漏拆也不猜着拆*/
	}else{
		int fi=nb_Idx(facing);
		if(fi<0){
			return FX_KEEP;/*facing的值不认识：同上，宁可漏拆*/
		}
		sup=nb_Opp(c,facing);/*贴着的块在 facing 的反方向；它朝我们的是 facing 那一面*/
		dir=fi;
	}
	if(!sup_OK(d,sup,dir)){
		return FX_AIR;
	}
	if((d.flags&F_AMETHYST)&&sup&&((sup->flags&BF_LEAVES)||sup->base=="honey_block")){
		return FX_AIR;/*Wiki Amethyst Cluster §Decoration：树叶/蜂蜜块不能长紫水晶*/
	}
	return FX_KEEP;
}
static int rule_Above(const Fix_Def &d,Rules_Ctx &c){/*上面要支持（孢子花、垂根、洞穴藤蔓）*/
	const Blk_Info *u=nb_Get(c,NB_U);
	if(!sup_OK(d,u,NB_D)){/*支撑块在上面，朝下那一面（NB_D）*/
		return FX_AIR;
	}
	if((d.flags&F_NO_LEAVES)&&u&&(u->flags&BF_LEAVES)){
		return FX_AIR;/*wiki:孢子花不能贴树叶底下*/
	}
	return FX_KEEP;
}
static int rule_Rim(const Fix_Def &d,Rules_Ctx &c){/*下面要顶面带边（铁轨、中继器、红石线、雪、海带）*/
	const Blk_Info *b=nb_Get(c,NB_D);
	if(!sup_OK(d,b,NB_U)&&!rim_OK(b)&&!((d.flags&F_HOLLOW)&&hollow_OK(b))){
		return FX_AIR;
	}
	if((d.flags&F_NO_ICE)&&b&&(b->flags&BF_ICE)){
		return FX_AIR;
	}
	if((d.flags&F_NO_MAGMA)&&b&&(b->flags&BF_MAGMA)){
		return FX_AIR;
	}
	if((d.flags&F_WATER)&&!(c.self->flags&BF_WATER)){/*自己不是水里的方块（海草/海带/气泡柱本身都含水）*/
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Hang(const Fix_Def &d,Rules_Ctx &c){/*看属性决定挂上面还是站下面*/
	const char *v=get_Prop(c.self->blk,d.key?d.key:"hanging");
	int hang=v&&!strcmp(v,"true");
	const Blk_Info *sup=hang?nb_Get(c,NB_U):nb_Get(c,NB_D);
	if(hang&&sup&&sup->base=="turtle_egg"){
		return FX_KEEP;/*wiki:龟蛋能撑住挂在它下面的灯笼；龟蛋本身是薄片，不能因此让它撑别的东西*/
	}
	if(!sup_OK(d,sup,hang?NB_D:NB_U)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Hang2(const Fix_Def &d,Rules_Ctx &c){/*悬挂告示牌：上下有一边搭着就行，都空才拆*/
	if(sup_OK(d,nb_Get(c,NB_U),NB_D)||sup_OK(d,nb_Get(c,NB_D),NB_U)){
		return FX_KEEP;
	}
	return FX_AIR;
}
static int rule_Door(const Fix_Def &d,Rules_Ctx &c){/*门：上半截看下半截，下半截看下面整方块*/
	const char *half=get_Prop(c.self->blk,"half");
	if(half&&!strcmp(half,"upper")){
		const Blk_Info *b=nb_Get(c,NB_D);
		if(!same_Base(b,c.self)||!is_PropValue(b->blk,"half","lower")){
			return FX_AIR;
		}
		return FX_KEEP;
	}
	if(!sup_OK(d,nb_Get(c,NB_D),NB_U)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Cocoa(const Fix_Def &d,Rules_Ctx &c){/*可可豆：facing那一面是丛林原木*/
	const char *facing=get_Prop(c.self->blk,"facing");
	if(!facing){
		return FX_KEEP;/*没有facing：判断不了贴哪面，宁可漏拆（正常存档一定有）*/
	}
	int fi=nb_Idx(facing);/*可可豆在 facing 那一边的方块上；那个方块朝我们的是 facing 的反面*/
	if(!sup_OK(d,fi>=0?nb_Get(c,fi):NULL,fi>=0?nb_Opp_Idx(fi):NB_U)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Stem(const Fix_Def &d,Rules_Ctx &c){/*结果的瓜茎：facing那面是西瓜/南瓜 + 下面耕地。
	注意这里和原版有个**故意的不等价**：AttachedStemBlock.updateShape 在果实没了时是"变回普通瓜茎(age=7)"，
	我们是直接拆掉（要完全等价得改状态换方块名，而fx_Set只改属性）。影响很小（正常存档里不会出现
	"结果的茎却没果实"），而且方向更保守*/
	if(!sup_OK(d,nb_Get(c,NB_D),NB_U)){
		return FX_AIR;
	}
	const char *facing=get_Prop(c.self->blk,"facing");
	if(!facing){
		return FX_KEEP;/*没有facing：不知道瓜在哪面，宁可漏拆（正常存档一定有）*/
	}
	const Blk_Info *fruit=nb_Dir(c,facing);
	if(!fruit||!d.key||fruit->base!=d.key){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Drip(const Fix_Def &d,Rules_Ctx &c){/*滴水石锥：朝下（钟乳石）看上，朝上（石笋）看下*/
	const char *v=get_Prop(c.self->blk,"vertical_direction");
	int up=v&&!strcmp(v,"up");/*石笋朝上→下面撑着；钟乳石朝下→上面挂着（PointedDripstoneBlock.isValidPointedDripstonePlacement 的 tipDirection）*/
	const Blk_Info *sup=up?nb_Get(c,NB_D):nb_Get(c,NB_U);
	if(!sup_OK(d,sup,up?NB_U:NB_D)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Bell(const Fix_Def &d,Rules_Ctx &c){/*钟：attachment=floor/ceiling/single_wall/double_wall*/
	const char *at=get_Prop(c.self->blk,"attachment");
	if(at&&!strcmp(at,"floor")){
		if(!sup_OK(d,nb_Get(c,NB_D),NB_U)){
			return FX_AIR;
		}
		return FX_KEEP;
	}
	if(at&&!strcmp(at,"ceiling")){
		if(!sup_OK(d,nb_Get(c,NB_U),NB_D)){
			return FX_AIR;
		}
		return FX_KEEP;
	}/*墙钟：facing是钟面朝的方向，贴着的支撑块在facing的反方向（Wiki Bell §Block states）；
	   只认这一侧，不能把钟面前方那块也当支撑。*/
	const char *f=get_Prop(c.self->blk,"facing");
	int fi=nb_Idx(f);
	if(fi>=0&&sup_OK(d,nb_Opp(c,f),fi)){
		return FX_KEEP;
	}
	return FX_AIR;
}
static int rule_Scaffolding(const Fix_Def &d,Rules_Ctx &c){/*脚手架：下面顶得住，或者旁边也是脚手架；
	但 distance=7 按Wiki是“变成下落方块”的极限，不能再靠旁边那格硬撑。*/
	if(sup_OK(d,nb_Get(c,NB_D),NB_U)){
		return FX_KEEP;
	}
	const char *dist=get_Prop(c.self->blk,"distance");
	if(dist&&!strcmp(dist,"7")){
		return FX_AIR;/*distance=7：没有下方支撑就必掉*/
	}
	for(int i=0;i<4;i++){
		const Blk_Info *b=nb_Get(c,nb_h[i]);
		if(b&&b->base=="scaffolding"){
			return FX_KEEP;
		}
	}
	return FX_AIR;
}
static int rule_SmallDripleaf(const Fix_Def &d,Rules_Ctx &c){/*小垂滴叶：黏土/苔藓直接放，泥土类要这格有水*/
	(void)d;
	const char *half=get_Prop(c.self->blk,"half");
	if(half&&!strcmp(half,"upper")){
		const Blk_Info *b=nb_Get(c,NB_D);
		if(!same_Base(b,c.self)||!is_PropValue(b->blk,"half","lower")){
			return FX_AIR;
		}
		return FX_KEEP;
	}
	const Blk_Info *b=nb_Get(c,NB_D);
	std::string_view n=b?b->base:std::string_view();/*读不出来时给空串：下面两个集合都不命中，交给blind作废*/
	for(int i=0;set_small_dripleaf[i];i++){
		if(n==set_small_dripleaf[i]){
			return FX_KEEP;
		}
	}
	for(int i=0;set_veg[i];i++){/*原版第二段走的是 VegetationBlock.mayPlaceOn=#supports_vegetation（比substrate多耕地）*/
		if(n==set_veg[i]){
			return is_PropValue(c.self->blk,"waterlogged","true")?FX_KEEP:FX_AIR;
		}
	}
	return FX_AIR;
}
static int rule_Propagule(const Fix_Def &d,Rules_Ctx &c){/*胎生苗：挂着就贴树叶，种着就看下面*/
	if(is_PropValue(c.self->blk,"hanging","true")){
		const Blk_Info *u=nb_Get(c,NB_U);/*原版：上面必须是 #supports_hanging_mangrove_propagule（=红树叶），
		                                    挂在别的树叶下面原版会掉（以前这里写成"是树叶就行"，漏拆）*/
		if(!u||!in_Set(set_mangrove_leaves,u)){
			return FX_AIR;
		}
		return FX_KEEP;
	}
	if(!sup_OK(d,nb_Get(c,NB_D),NB_U)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static int rule_Chorus(const Fix_Def &d,Rules_Ctx &c){/*紫颂花（26.1.1 的 ChorusFlowerBlock.canSurvive 原样搬过来）：
	下面必须是紫颂植株或 #supports_chorus_flower（=末地石）；下面真是空气时，才轮到
	"水平方向正好一个紫颂植株"，而且其余水平方向必须全是空气（旁边杵着别的方块一样碎）。
	都不满足就碎掉（不掉落）。*/
	(void)d;
	const Blk_Info *b=nb_Get(c,NB_D);
	if(b&&!blk_Air(b)){/*下面有东西：只认紫颂植株/末地石（紫颂花在下面也不行，原版就是碎的）*/
		return (b->base=="chorus_plant"||in_Set(set_chorus,b))?FX_KEEP:FX_AIR;
	}
	int cnt=0;
	for(int i=0;i<4;i++){
		const Blk_Info *s=nb_Get(c,nb_h[i]);
		if(s&&s->base=="chorus_plant"){
			if(++cnt>1){
				return FX_AIR;/*"正好一个"：两个以上原版也不长*/
			}
		}else if(!blk_Air(s)){
			return FX_AIR;/*旁边不是空气也不是紫颂植株：原版照样碎*/
		}
	}
	return cnt==1?FX_KEEP:FX_AIR;/*0个：没靠山*/
}
/*紫颂植株的第1条（"水平挨着紫颂、上下都不是空气 → 碎"）要不要照原版执行？
  1=照原版：末地自然生成的地形里就有极少数这种格子——旁边是另一株"种子"长出来的矮桩、
    上面又是自己那截树干。原版的下一次方块更新（谁在旁边放/拆一格）确实会把它连着一截树干拆掉；
    照原版执行＝fixer 替原版把这棵树的"塌"提前做完，代价是把原版生成的地形改掉（用户看见一整棵树没了）。
  0=宁可漏拆（默认）：这种格子留着，只在"一点靠山都没有"（下面不是末地石/紫颂、旁边也没有紫颂）时才拆。
  两种只差这一点：真图实测（1.21.5/26.1.1 原版预生成的末地，1024 个区块）1=171 格被拆（含连锁），0=0 格。*/
#define CHORUS_PLANT_STRICT 0
static int rule_ChorusPlant(const Fix_Def &d,Rules_Ctx &c){/*紫颂植株（26.1.1 的 ChorusPlantBlock.canSurvive 的白话版）：
		fixer 只给六个面，所以"旁边那株的下面"（斜下方）看不到。
		for(水平四个方向){
		    那个方向是紫颂植株吗？是的话：
		        上面和下面都不是空气 → 碎（wiki第1条，见 CHORUS_PLANT_STRICT）
		        保留它（宁可漏拆；没法再确认那株有没有根）
		}
		四个方向都没给出"留"的理由 → 下面是不是紫颂植株/末地石：是就留，不是就碎（wiki第2条前半句）
	说明：原版第2条后半句会看旁边那株的下面是不是根；现在拿不到斜下方，只能把"旁边有紫颂植株"
	当成留的理由。这会把极少数的无根横枝留下（漏拆），但不会拆错。*/
	(void)d;
	const Blk_Info *b=nb_Get(c,NB_D);
	int below_ok=b&&(b->base=="chorus_plant"||in_Set(set_chorus,b));
#if CHORUS_PLANT_STRICT
	int above_nonair=!blk_Air(nb_Get(c,NB_U));
	int below_nonair=!blk_Air(b);
#endif
	for(int i=0;i<4;i++){
		const Blk_Info *s=nb_Get(c,nb_h[i]);
		if(!s||s->base!="chorus_plant"){
			continue;
		}
#if CHORUS_PLANT_STRICT
		if(above_nonair&&below_nonair){/*第1条：水平连着紫颂，上下都不是空气*/
			return FX_AIR;
		}
#endif
		return FX_KEEP;/*旁边那株的根在斜下方，六面里看不到；宁可留着*/
	}
	return below_ok?FX_KEEP:FX_AIR;
}
static int rule_Snow(const Fix_Def &d,Rules_Ctx &c){/*雪层：26.1 的两个标签先看
	（#cannot_support_snow_layer={barrier,ice,packed_ice} 优先于 #support_override_snow_layer={honey_block,mud,soul_sand}），
	其余情况要下面那格"顶面完整"（原版 full top face；不是铁轨的"顶面带边"）*/
	(void)d;
	const Blk_Info *b=nb_Get(c,NB_D);
	if(!b){/*读不出来：blind已经置上了，外面会整格丢掉（这里给什么都会被忽略）*/
		return FX_AIR;
	}
	if(in_Set(set_snow_no,b)){
		return FX_AIR;
	}
	if(in_Set(set_snow_over,b)){
		return FX_KEEP;
	}
	return face_Full(b,NB_U)?FX_KEEP:FX_AIR;
}
static nbt_Any *state_Concrete(nbt_Any *self){/*混凝土粉末→对应的混凝土（white_concrete_powder→white_concrete）：
	照原版 ConcretePowderBlock.onLand/updateShape，碰到水就变混凝土；混凝土是整方块、不在规则表里，
	换过去仍然"越判越少"（M 至少减1，合约见 rules.hpp 文件头）。属性整份复制再改 Name，别丢命名空间*/
	nbt_Any *copy=self->copy();
	if(copy->type!=0x0a){
		return copy;/*不是compound（坏数据）：原样拿回去，外面会当"没改"（cmp相同）*/
	}
	std::string n(get_Name(self));
	size_t p=n.find("_powder");
	if(p!=std::string::npos){
		n.erase(p,7);
	}
	nbt_Compound *c=(nbt_Compound*)copy;
	c->set_String("Name",n);
	c->erase("Properties");/*混凝土没有方块属性（粉末也没有）；万一以后版本给粉末加了属性，也别把混凝土写成一个不存在的状态*/
	return copy;
}
static int rule_Falling(const Fix_Def &d,Rules_Ctx &c){/*重力方块：正下方是空气/水/岩浆/火才拆；别的可替换方块不拆。
	fixer不生成掉落物实体，替原版把这次更新做完就是拆掉（和拆别的浮空方块一个意思）。
	混凝土粉末特殊：原版先看"碰没碰到水"（updateShape→touchesLiquid），碰到就凝固成混凝土、根本不会掉；
	所以先判水，再判掉不掉。*/
	(void)d;
	const Blk_Info *b=nb_Get(c,NB_D);
	if(!b){/*读不出来：blind已经置上，外面会整格丢掉（这里给什么都会被忽略）*/
		return FX_AIR;
	}
	if(end_With(c.self->base,"_concrete_powder")&&touches_Water(c)){
		return fx_Set(c,state_Concrete(c.self->blk),0);
	}
	if(free_Below(b)){
		return FX_AIR;
	}
	return FX_KEEP;
}
static nbt_Any *state_Water(){/*水源（level=0）：和地图里别的水、以及 fixer.cpp 拆含水方块时写的水一模一样。
	每次现造一份（主机会free，别给静态的，免得被free两次）*/
	nbt_Compound *b=new nbt_Compound;
	b->set_String("Name","minecraft:water");
	nbt_Compound *p=new nbt_Compound;
	p->set_String("level","0");
	b->set("Properties",p);
	return b;
}
static int rule_Frost(const Fix_Def &d,Rules_Ctx &c){/*霜冰——不看支撑、不看光照，见到就换成水。
	依据（minecraft.wiki/w/Frosted_Ice，原始HTML留在 test/work/wiki/Frosted_Ice.html）：
	  · "Frosted ice is a translucent block created when a mob or entity wearing Frost Walker-enchanted boots
	     moves over water"：它本来就只是"冰霜行者踩过水面"的临时产物，不是该长期存在的地形；
	  · "Frosted ice naturally melts fairly quickly ... When the age reaches 4, the block turns into water"：
	    融化的终点就是水（Fixer 写的是水源 level=0，和周围的水一样）；
	  · "in places with low light levels, such as underground, at night or in the End, it never melts
	     unless broken"：原版判融化要读内部光照（我们读不到光照图），而光照低的地方它本来就赖着不化——
	    所以这里**不判光照**：见到霜冰就当它该化了（用户的服务端bug会往存档里写一大片霜冰，正是要清掉的）。
	代价：玩家穿着冰霜行者靴子刚踩出来、还没化的霜冰也会被一起化掉（那本来就该化，只是化得早了点）。
	换成水仍然"越判越少"：霜冰是整方块（S_FULL）且在规则表里，水谁都不管（S_NONE），
	所以 M＝要管的方块数+true面数 至少减1，fixer 的收敛证明不受影响（合约见 rules.hpp 文件头）*/
	(void)d;
	return fx_Set(c,state_Water(),0);
}
typedef int (*Fix_Fn)(const Fix_Def&,Rules_Ctx&);
static const Fix_Fn rule_Fn[RULE_COUNT]={
	NULL,rule_Below,rule_2X,rule_Vine,rule_Face6,rule_Side,rule_Above,rule_Rim,rule_Hang,rule_Hang2,rule_Door,
	rule_Cocoa,rule_Stem,rule_Drip,rule_Bell,rule_Scaffolding,rule_SmallDripleaf,rule_Propagule,rule_Chorus,
	rule_ChorusPlant,rule_Snow,rule_Frost,rule_Falling
};
static int def_Call(const Fix_Def &d,Rules_Ctx &c){
	if(d.rule==RULE_NONE||d.rule>=RULE_COUNT||!rule_Fn[d.rule]){
		return FX_KEEP;
	}
	return rule_Fn[d.rule](d,c);
}

/*==================== 5.5 POI 默认记录表 ====================
 *	用户要求：rules 管“哪些方块应该产生什么 POI”，fixer 只负责按这张表补/删。
 *	票数按 Wiki 的 PoiType 上限：工作站/床是 1，钟是 32，蜜蜂/磁石/避雷针/传送门这类是 0
 *	（0 不是“没票”，是这类 POI 本来就不发村民票，free_tickets 也写 0）。
 *	固定写 26.1.1 的数据；换版本重编 rules，不在这里按 DataVersion 分支。*/
struct Poi_Def{const char *block;const Poi_Info *info;};
static const Poi_Info poi_Home={"minecraft:home",1,1};
static const Poi_Info poi_Meeting={"minecraft:meeting",32,32};
static const Poi_Info poi_Farmer={"minecraft:farmer",1,1};
static const Poi_Info poi_Fisherman={"minecraft:fisherman",1,1};
static const Poi_Info poi_Butcher={"minecraft:butcher",1,1};
static const Poi_Info poi_Armorer={"minecraft:armorer",1,1};
static const Poi_Info poi_Cartographer={"minecraft:cartographer",1,1};
static const Poi_Info poi_Cleric={"minecraft:cleric",1,1};
static const Poi_Info poi_Librarian={"minecraft:librarian",1,1};
static const Poi_Info poi_Mason={"minecraft:mason",1,1};
static const Poi_Info poi_Shepherd={"minecraft:shepherd",1,1};
static const Poi_Info poi_Toolsmith={"minecraft:toolsmith",1,1};
static const Poi_Info poi_Weaponsmith={"minecraft:weaponsmith",1,1};
static const Poi_Info poi_Fletcher={"minecraft:fletcher",1,1};
static const Poi_Info poi_Leatherworker={"minecraft:leatherworker",1,1};
static const Poi_Info poi_BeeNest={"minecraft:bee_nest",0,0};
static const Poi_Info poi_Beehive={"minecraft:beehive",0,0};
static const Poi_Info poi_Lodestone={"minecraft:lodestone",0,0};
static const Poi_Info poi_LightningRod={"minecraft:lightning_rod",0,0};
static const Poi_Info poi_NetherPortal={"minecraft:nether_portal",0,0};
static const Poi_Info poi_TestInstance={"minecraft:test_instance",0,0};
static const Poi_Def poi_Defs[]={
	{"bell",&poi_Meeting},
	{"composter",&poi_Farmer},
	{"barrel",&poi_Fisherman},
	{"smoker",&poi_Butcher},
	{"blast_furnace",&poi_Armorer},
	{"cartography_table",&poi_Cartographer},
	{"brewing_stand",&poi_Cleric},
	{"lectern",&poi_Librarian},
	{"stonecutter",&poi_Mason},
	{"loom",&poi_Shepherd},
	{"smithing_table",&poi_Toolsmith},
	{"grindstone",&poi_Weaponsmith},
	{"fletching_table",&poi_Fletcher},
	{"cauldron",&poi_Leatherworker},
	{"water_cauldron",&poi_Leatherworker},
	{"lava_cauldron",&poi_Leatherworker},
	{"powder_snow_cauldron",&poi_Leatherworker},
	{"bee_nest",&poi_BeeNest},
	{"beehive",&poi_Beehive},
	{"lodestone",&poi_Lodestone},
	{"lightning_rod",&poi_LightningRod},
	{"exposed_lightning_rod",&poi_LightningRod},
	{"weathered_lightning_rod",&poi_LightningRod},
	{"oxidized_lightning_rod",&poi_LightningRod},
	{"waxed_lightning_rod",&poi_LightningRod},
	{"waxed_exposed_lightning_rod",&poi_LightningRod},
	{"waxed_weathered_lightning_rod",&poi_LightningRod},
	{"waxed_oxidized_lightning_rod",&poi_LightningRod},
	{"nether_portal",&poi_NetherPortal},
	{"test_instance_block",&poi_TestInstance},
};
static int is_Vanilla_Blk(const Blk_Info *b){/*只认minecraft命名空间（没冒号也算），mod方块不猜*/
	const char *nm=b&&b->blk?get_Name(b->blk):NULL;
	if(!nm||!nm[0]){
		return 0;
	}
	const char *c=strchr(nm,':');
	return !c||((size_t)(c-nm)==9&&!strncmp(nm,"minecraft",9));
}
static const Poi_Info *api_poi_Get(const Blk_Info *b){
	if(!is_Vanilla_Blk(b)||b->base.empty()){
		return NULL;
	}
	if(end_With(b->base,"_bed")){/*床只有head那半产生home记录；脚那半返回NULL*/
		return is_PropValue(b->blk,"part","head")?&poi_Home:NULL;
	}
	for(size_t i=0;i<sizeof(poi_Defs)/sizeof(poi_Defs[0]);i++){
		if(b->base==poi_Defs[i].block){
			return poi_Defs[i].info;
		}
	}
	return NULL;
}

/*==================== 6. 插件出口 ====================*/
static int api_rule_Count(){return RULE_COUNT;}
static const char *api_rule_Name(int r){return r>=0&&r<RULE_COUNT?rule_Name[r]:"?";}
static int api_group_Count(){return GROUP_CNT;}
static const char *api_group_Name(int g){return g>=0&&g<GROUP_CNT?group_Name_Tab[g]:"?";}
static void api_reset(){/*清分类缓存：名字池里的string_view会被外面拿着（Blk_Info.base），所以只能在没有Blk_Info活着的时候调*/
	def_Map.clear();
	def_Pat.clear();
	name_Map.clear();
	name_Pool.clear();
	def_Init();
}
static void api_blk_Fill(Blk_Info *o,nbt_Any *blk){/*fixer建段表时每条调色板条目调一次*/
	o->fill(blk);
}
static int api_blk_Rule(const Blk_Info *b,uint32_t mask){/*这条要管吗：给规则表下标，-1=不管（mod方块、明确的不管的、被rules=关掉的家族）*/
	int di=b?(int)b->def:-1;
	if(di<0||di>=defs_len||defs[di].rule==RULE_NONE){
		return -1;
	}
	if(defs[di].set&&!defs[di].set_bit){
		/*这个方块的支撑集没登记进set_List（改规则表时加了个新数组忘了登记）：set_bit=0会让
		  "下面是不是支持集里的一员"永远判不成立⇒这类方块全被当成悬空拆掉。宁可不判这一条
		  （def_Init里已经打印过一行提示），也绝不能因为表写错就整类清空*/
		return -1;
	}
	return (mask&def_Group(defs[di]))?di:-1;
}
static int api_def_Rule(int di){/*规则表下标→规则号（report按规则分类计数用）*/
	return di>=0&&di<defs_len?(int)defs[di].rule:(int)RULE_NONE;
}
static int api_judge(const Blk_Info *self,int di,Rules_Ctx *c){/*判一格：fixer会填好self/x/y/z和六个面*/
	if(!self||!c||di<0||di>=defs_len){
		return FX_KEEP;
	}
	return def_Call(defs[di],*c);
}
static const Rules_Api api={
	RULES_ABI,
	(uint32_t)sizeof(Rules_Api),
	"minecraft浮空物规则",/*插件名字（fixer启动时打一行）*/
	"26.1.1",              /*规则版本：方块/标签按 Java Edition 26.1.1 核对*/
	4788,                  /*minecraft_version：fixer的DataVersion必须正好等于它*/
	api_rule_Count,api_rule_Name,api_group_Count,api_group_Name,api_reset,
	api_blk_Fill,api_blk_Rule,api_def_Rule,api_poi_Get,api_judge
};
extern "C" const Rules_Api *rules_Entry(uint32_t abi,uint32_t size){/*fixer用dlsym找这个符号*/
	if(abi!=RULES_ABI||size!=sizeof(Rules_Api)){
		printf("rules: 接口对不上（本插件abi=0x%08x size=%u，问的是0x%08x size=%u）\n",
			RULES_ABI,(unsigned)sizeof(Rules_Api),abi,size);
		return NULL;
	}
	return &api;
}
