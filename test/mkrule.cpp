#include "../nbtrw.hpp"
#include <map>
#include <string>
/*mkrule.cpp：给 rules.sh 造一个“一条规则一个例子”的世界（26.1.1，DataVersion 4788）
 *	每个用例占一列：世界坐标 x=i*4+1、z=0；y=-59 放支撑方块，y=-58 放被测方块，
 *	y=-57 是“上面”的支撑，x±1/z±1 里最多放两个水平邻居（按用例指定方向）。
 *	间距是4不是2：紫颂那几条要放水平邻居，间距2时“上一个用例的侧面方块”会贴到下一个用例上互相干扰；
 *	侧面方块自己下面的方块用 side_below 指定。y=-60 统一垫一块石头（支撑块自己也要站得住）。
 *	预期结果直接写在表里：0=留着（含“只关掉某个面”）、1=拆掉（空气或水）、2=变成水源（霜冰）、
 *	3=混凝土粉末凝固成混凝土、4=结果的瓜茎变回普通瓜茎（age=7）、5=变成 expect 写的状态（门照另一半改、藤头藤身/竹子/
 *	火的外观转换、栅栏/墙/玻璃板/铁栏杆/苍白苔藓地毯/紫颂植株的连接、栅栏门贴墙、箱子配对、雪地草的重算；属性值写 * 的不比，比如藤头的 age 原版是随机的）。依据写在每条的 why 里（26.1.1 游戏判定，
 *	test/mcdump.sh cmp 对拍过；以前照 wiki 写的已经按游戏改过）。
 *	所有区块写 InhabitedTime=1：现行 fixer 会删“InhabitedTime 明确为0且整块在范围内”的区块，
 *	用例世界要能连跑两遍（第二遍必须0改动）。
 *	写完把用例表写到 <世界>/cases.tsv，check_rules.py 拿它和 fixer 跑完的结果对。
 *	用法：test/work/mkrule <世界目录>（rules.sh 传 test/work/rules；目录要先清空）
 */
struct Case{
	const char *self;      /*被测方块*/
	const char *props;     /*自身属性 "half=upper,facing=north"，没有就给NULL*/
	const char *below;     /*y=-59*/
	const char *below_props;
	const char *above;     /*y=-57*/
	const char *side;      /*水平邻居（没有给NULL）*/
	int side_dir;          /*0=东(+x) 1=西(-x) 2=南(+z) 3=北(-z)*/
	int del;               /*1=预期被拆成空气；0=预期留着（含"只关掉某个面"）；2=预期变成水源（霜冰化成水那种）；
	                         3=混凝土粉末凝固；4=结果的瓜茎变回普通瓜茎 age=7*/
	const char *why;       /*依据（wiki上哪条）*/
	/*下面三个只给特殊用例用，普通用例不写（写了的才填，没写的留NULL）*/
	const char *above_props=NULL;/*y=-57那个方块的属性（两格结构要用 half=upper）*/
	const char *below2=NULL;     /*y=-60的方块（堆叠用例：下面那格自己也得有靠山）*/
	const char *above2=NULL;     /*y=-56的方块（洞穴藤蔓那种一串的）*/
	/*紫颂专用：水平邻居"自己下面那格"和第二、第三个水平邻居（紫颂要看"旁边那株有没有根"、"旁边是不是空气"）*/
	const char *side_below=NULL; /*侧面那格下面的方块（y=-59，和侧面同一列；紫颂植株靠它判"旁边那株有根吗"）*/
	const char *side2=NULL;      /*第二个水平邻居（和被测方块同一高度）*/
	int side2_dir=0;             /*0=东 1=西 2=南 3=北*/
	const char *side_props=NULL; /*侧面那个方块的属性（仙人掌旁边要放8层雪这种）*/
	const char *expect=NULL;     /*del=5：预期变成的状态 "minecraft:名字[属性=值,...]"（属性按名字排序，值写 * 不比）*/
	const char *side_above=NULL; /*侧面那格上面的方块（y=-57，和侧面同一列；苍白苔藓地毯“上面那截”的侧面要贴着它）*/
};
/*支撑档对照（rules.cpp的S_NONE/S_THIN/S_PART/S_FULL）：空气0、水0、火把1、台阶/栅栏2、石头/玻璃3*/
static const Case cases[]={
	/*---- 植被：#supports_vegetation = #substrate_overworld + 耕地 ----*/
	{"short_grass",NULL,"dirt",NULL,NULL,NULL,0,0,"supports_vegetation含泥土"},
	{"short_grass",NULL,"sand",NULL,NULL,NULL,0,1,"沙子不在supports_vegetation里"},
	{"short_grass",NULL,"farmland",NULL,NULL,NULL,0,0,"耕地是植被支撑"},
	{"dandelion",NULL,"grass_block",NULL,NULL,NULL,0,0,"草方块是植被支撑"},
	{"dandelion",NULL,"stone",NULL,NULL,NULL,0,1,"石头不是植被支撑"},
	{"golden_dandelion",NULL,"coarse_dirt",NULL,NULL,NULL,0,0,"26.1新增的花，按supports_vegetation判"},
	{"open_eyeblossom",NULL,"podzol",NULL,NULL,NULL,0,0,"眼眸花按supports_vegetation判"},
	{"closed_eyeblossom",NULL,"clay",NULL,NULL,NULL,0,1,"黏土不在supports_vegetation里"},
	{"sweet_berry_bush",NULL,"moss_block",NULL,NULL,NULL,0,0,"苔藓块是植被支撑"},
	{"oak_sapling",NULL,"dirt",NULL,NULL,NULL,0,0,"树苗按supports_vegetation判"},
	{"oak_sapling",NULL,"gravel",NULL,NULL,NULL,0,1,"沙砾不是植被支撑"},
	{"bush",NULL,"mud",NULL,NULL,NULL,0,0,"泥是植被支撑"},
	{"firefly_bush",NULL,"mycelium",NULL,NULL,NULL,0,0,"菌丝是植被支撑"},
	{"pink_petals",NULL,"rooted_dirt",NULL,NULL,NULL,0,0,"缠根泥土是植被支撑"},
	/*---- 干植被：#supports_dry_vegetation ----*/
	{"dead_bush",NULL,"sand",NULL,NULL,NULL,0,0,"枯灌木长在沙子上"},
	{"dead_bush",NULL,"orange_terracotta",NULL,NULL,NULL,0,0,"陶瓦在supports_dry_vegetation里"},
	{"short_dry_grass",NULL,"dirt",NULL,NULL,NULL,0,0,"干草长在泥土上"},
	{"tall_dry_grass",NULL,"stone",NULL,NULL,NULL,0,1,"石头不在supports_dry_vegetation里"},
	/*---- 菌类/下界 ----*/
	{"brown_mushroom",NULL,"stone",NULL,NULL,NULL,0,0,"蘑菇要完整顶面，不挑土"},
	{"brown_mushroom",NULL,"ice",NULL,NULL,NULL,0,1,"蘑菇不能长在冰上"},
	{"crimson_fungus",NULL,"crimson_nylium",NULL,NULL,NULL,0,0,"菌岩是支撑"},
	{"crimson_fungus",NULL,"soul_soil",NULL,NULL,NULL,0,0,"灵魂土在supports_*_fungus里"},
	{"warped_fungus",NULL,"stone",NULL,NULL,NULL,0,1,"石头不是菌类支撑"},
	{"nether_sprouts",NULL,"warped_nylium",NULL,NULL,NULL,0,0,"菌岩是支撑"},
	{"wither_rose",NULL,"netherrack",NULL,NULL,NULL,0,0,"supports_wither_rose含下界岩"},
	{"wither_rose",NULL,"clay",NULL,NULL,NULL,0,1,"黏土不在supports_wither_rose里"},
	{"nether_wart",NULL,"soul_sand",NULL,NULL,NULL,0,0,"supports_nether_wart=灵魂沙"},
	{"nether_wart",NULL,"soul_soil",NULL,NULL,NULL,0,1,"灵魂土不行"},
	/*---- 杜鹃/红树 ----*/
	{"azalea",NULL,"clay",NULL,NULL,NULL,0,0,"supports_azalea含黏土"},
	{"flowering_azalea",NULL,"clay",NULL,NULL,NULL,0,0,"同supports_azalea"},
	{"mangrove_propagule",NULL,"dirt",NULL,NULL,NULL,0,0,"种下的胎生苗按supports_mangrove_propagule"},
	{"mangrove_propagule","hanging=true","dirt",NULL,"mangrove_leaves",NULL,0,0,"挂着的胎生苗看上面是红树叶"},
	{"mangrove_propagule","hanging=true","dirt",NULL,"dirt",NULL,0,1,"挂着的胎生苗上面不是红树叶"},
	/*---- 作物/茎 ----*/
	{"wheat",NULL,"farmland",NULL,NULL,NULL,0,0,"supports_crops=耕地"},
	{"wheat",NULL,"dirt",NULL,NULL,NULL,0,1,"泥土不行"},
	{"torchflower_crop",NULL,"farmland",NULL,NULL,NULL,0,0,"火把花作物要耕地"},
	{"pitcher_crop","half=lower","farmland",NULL,"pitcher_crop",NULL,0,0,"瓶子草作物下半+上面有上半","half=upper",NULL,NULL},
	{"pitcher_crop","half=lower","dirt",NULL,"pitcher_crop",NULL,0,1,"下面不是耕地"},
	{"pumpkin_stem",NULL,"farmland",NULL,NULL,NULL,0,0,"瓜茎要耕地"},
	{"attached_pumpkin_stem","facing=east","farmland",NULL,NULL,"pumpkin",0,0,"结果的瓜茎：facing那面有南瓜"},
	{"attached_pumpkin_stem","facing=east","farmland",NULL,NULL,"melon",0,4,"facing那面不是南瓜：原版变回普通南瓜茎 age=7，不拆（AttachedStemBlock.updateShape）"},
	/*---- 甘蔗/仙人掌/竹子：能叠 ----*/
	{"sugar_cane",NULL,"sand",NULL,NULL,NULL,0,0,"supports_sugar_cane含沙子"},
	{"sugar_cane",NULL,"sugar_cane",NULL,NULL,NULL,0,0,"甘蔗能叠在甘蔗上",NULL,"sand",NULL},
	{"sugar_cane",NULL,"farmland",NULL,NULL,NULL,0,1,"耕地不在supports_sugar_cane里"},
	{"cactus",NULL,"sand",NULL,NULL,NULL,0,0,"supports_cactus=沙子"},
	{"cactus",NULL,"dirt",NULL,NULL,NULL,0,1,"泥土不行"},
	{"cactus",NULL,"cactus",NULL,NULL,NULL,0,0,"仙人掌能叠",NULL,"sand",NULL},
	{"cactus_flower",NULL,"cactus",NULL,NULL,NULL,0,0,"support_override_cactus_flower含仙人掌",NULL,"sand",NULL},
	{"cactus_flower",NULL,"farmland",NULL,NULL,NULL,0,0,"override含耕地"},
	{"cactus_flower",NULL,"stone",NULL,NULL,NULL,0,0,"stone有完整顶面，也能放"},
	{"bamboo",NULL,"gravel",NULL,NULL,NULL,0,0,"supports_bamboo含沙砾"},
	{"bamboo",NULL,"bamboo",NULL,NULL,NULL,0,0,"竹子能叠",NULL,"gravel",NULL},
	{"bamboo",NULL,"stone",NULL,NULL,NULL,0,1,"石头不行"},
	/*---- 水里的 ----*/
	{"lily_pad",NULL,"water",NULL,NULL,NULL,0,0,"睡莲放水源上"},
	{"lily_pad",NULL,"ice",NULL,NULL,NULL,0,0,"supports_lily_pad含冰"},
	{"lily_pad",NULL,"stone",NULL,NULL,NULL,0,1,"石头不行"},
	{"frogspawn",NULL,"water",NULL,"air",NULL,0,0,"青蛙卵放水源上"},
	{"frogspawn",NULL,"water",NULL,"stone",NULL,0,0,"青蛙卵上面有方块也行（原版看的“上面没有流体”是青蛙卵自己那格）"},
	{"frogspawn",NULL,"water","level=1",NULL,NULL,0,1,"青蛙卵：流动水不行（#supports_frogspawn 流体标签只有水源）"},
	{"frogspawn",NULL,"oak_slab","type=bottom,waterlogged=true",NULL,NULL,0,0,"青蛙卵：含水方块里的水也是水源"},
	{"sea_pickle",NULL,"stone",NULL,NULL,NULL,0,0,"海泡菜下面有方块就行"},
	{"sea_pickle",NULL,"water",NULL,NULL,NULL,0,1,"海泡菜下面是水：水没有碰撞面也不是整面（SeaPickleBlock.mayPlaceOn，Opacity/Placement）"},
	/*---- 两格高 ----*/
	{"tall_grass","half=lower","dirt",NULL,"tall_grass",NULL,0,0,"高草下半+上面有上半","half=upper",NULL,NULL},
	{"tall_grass","half=upper","tall_grass","half=lower",NULL,NULL,0,0,"高草上半，下面有下半",NULL,"dirt",NULL},
	{"tall_grass","half=upper","dirt",NULL,NULL,NULL,0,1,"上半孤零零一个"},
	{"sunflower","half=lower","dirt",NULL,"sunflower",NULL,0,0,"向日葵两格","half=upper",NULL,NULL},
	{"pitcher_plant","half=lower","dirt",NULL,"pitcher_plant",NULL,0,0,"瓶子草两格","half=upper",NULL,NULL},
	/*---- 垂滴叶/苔藓毯/落叶 ----*/
	{"big_dripleaf",NULL,"clay",NULL,NULL,NULL,0,0,"supports_big_dripleaf含黏土"},
	{"big_dripleaf",NULL,"big_dripleaf_stem",NULL,NULL,NULL,0,0,"叶子坐在**自己茎**的顶上＝正常成株形态（wiki:茎互相叠着、最上面顶一片叶子；绝不能拆）",NULL,"clay",NULL},
	{"big_dripleaf",NULL,"stone",NULL,NULL,NULL,0,1,"叶子直接放石头上：supports_big_dripleaf不含石头"},
	{"big_dripleaf",NULL,"air",NULL,NULL,NULL,0,1,"叶子悬空"},
	{"big_dripleaf_stem",NULL,"clay",NULL,"big_dripleaf",NULL,0,0,"最下面那根茎坐在黏土上，上面是叶子"},
	{"big_dripleaf_stem",NULL,"clay",NULL,NULL,NULL,0,1,"光杆茎：上面不是茎也不是叶子（BigDripleafStemBlock.canSurvive）"},
	{"big_dripleaf_stem",NULL,"big_dripleaf_stem",NULL,"big_dripleaf",NULL,0,0,"茎能接在茎上",NULL,"clay",NULL},
	{"big_dripleaf_stem",NULL,"stone",NULL,NULL,NULL,0,1,"茎下面既不是茎也不是support集→拆"},
	{"small_dripleaf","half=lower,facing=north","clay",NULL,"small_dripleaf",NULL,0,0,"supports_small_dripleaf含黏土","half=upper,facing=north"},
	{"small_dripleaf","half=lower,facing=north","clay",NULL,NULL,NULL,0,1,"小型垂滴叶下半上面没有上半（DoublePlantBlock.updateShape）"},
	{"small_dripleaf","half=lower,facing=north","dirt",NULL,"small_dripleaf",NULL,0,1,"泥土类要泡在水里（这格没水）","half=upper,facing=north"},
	{"moss_carpet",NULL,"stone",NULL,NULL,NULL,0,0,"苔藓毯下面不是空气就行"},
	{"moss_carpet",NULL,"air",NULL,NULL,NULL,0,1,"悬空"},
	{"leaf_litter",NULL,"stone",NULL,NULL,NULL,0,0,"落叶要完整实心顶面"},
	{"leaf_litter",NULL,"air",NULL,NULL,NULL,0,1,"悬空"},
	/*---- 火把族/灯笼/按钮/压力板/牌子 ----*/
	{"torch",NULL,"stone",NULL,NULL,NULL,0,0,"顶放在整方块上"},
	{"torch",NULL,"oak_fence",NULL,NULL,NULL,0,0,"栅栏顶面也能插火把"},
	{"torch",NULL,"air",NULL,NULL,NULL,0,1,"悬空"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"stone",2,0,"墙火把：facing的反方向(north→南)有方块"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"air",2,1,"背后是空气"},
	{"lantern","hanging=true","air",NULL,"stone",NULL,0,0,"吊灯：上面有方块"},
	{"lantern","hanging=true","air",NULL,"air",NULL,0,1,"上面也是空气"},
	{"ladder","facing=north",NULL,NULL,NULL,"stone",2,0,"梯子背面(facing反方向)是整方块"},
	{"ladder","facing=north",NULL,NULL,NULL,"air",2,1,"背面是空气"},
	{"lever","face=floor","stone",NULL,NULL,NULL,0,0,"拉杆贴地面"},
	{"lever","face=floor","air",NULL,NULL,NULL,0,1,"下面没东西"},
	{"oak_button","face=floor","stone",NULL,NULL,NULL,0,0,"按钮贴地面"},
	{"oak_button","face=floor","air",NULL,NULL,NULL,0,1,"下面没东西"},
	{"stone_pressure_plate",NULL,"stone",NULL,NULL,NULL,0,0,"压力板要下方支撑"},
	{"stone_pressure_plate",NULL,"air",NULL,NULL,NULL,0,1,"悬空"},
	{"tripwire_hook","facing=north",NULL,NULL,NULL,"stone",2,0,"绊线钩贴方块"},
	{"oak_sign","rotation=0",NULL,NULL,NULL,NULL,0,1,"立牌下面什么都没有"},
	{"oak_sign","rotation=0","stone",NULL,NULL,NULL,0,0,"立牌站在方块上"},
	{"oak_hanging_sign","attached=true",NULL,NULL,"stone",NULL,0,0,"吊牌挂在方块底面"},
	{"oak_hanging_sign","attached=true",NULL,NULL,"air",NULL,0,1,"上面没东西"},
	{"oak_wall_hanging_sign","facing=north",NULL,NULL,NULL,"air",2,0,"墙挂吊牌自带托架：wiki明确不因邻块消失而破"},
	{"white_banner","rotation=0",NULL,NULL,NULL,NULL,0,1,"旗帜悬空"},
	{"white_banner","rotation=0","stone",NULL,NULL,NULL,0,0,"旗帜站方块上"},
	/*---- wiki明确不需要支撑的：怎么摆都留着 ----*/
	{"red_bed","facing=north,part=foot",NULL,NULL,NULL,"red_bed",3,0,"床不需要下方支撑（脚的 facing 那边接着头）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,part=head"},
	{"red_bed","facing=north,part=foot",NULL,NULL,NULL,NULL,0,1,"床只剩一半（BedBlock.updateShape：另一半没了变空气）"},
	{"red_bed","facing=north,part=foot",NULL,NULL,NULL,"blue_bed",3,1,"另一半是别的颜色的床也不行（is(this)）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,part=head"},
	{"iron_trapdoor","facing=north,half=bottom",NULL,NULL,NULL,NULL,0,0,"活板门不需要支撑"},
	{"oak_fence_gate","facing=north",NULL,NULL,NULL,NULL,0,0,"栅栏门可以悬空"},
	{"flower_pot",NULL,"air",NULL,NULL,NULL,0,0,"花盆不需要支撑"},
	{"turtle_egg",NULL,"air",NULL,NULL,NULL,0,0,"海龟蛋不需要支撑"},
	{"iron_chain",NULL,"air",NULL,NULL,NULL,0,0,"锁链能悬空"},
	{"end_rod","facing=up",NULL,NULL,NULL,NULL,0,0,"末地烛支撑没了也不破"},
	{"tripwire",NULL,"air",NULL,NULL,NULL,0,0,"绊线不需要支撑"},
	/*---- 红石/铁轨/雪/门/钟/脚手架/蜡烛 ----*/
	{"rail",NULL,"stone",NULL,NULL,NULL,0,0,"铁轨铺在整方块上"},
	{"rail",NULL,"air",NULL,NULL,NULL,0,1,"铁轨悬空"},
	{"redstone_wire",NULL,"stone",NULL,NULL,NULL,0,0,"红石线贴方块"},
	{"redstone_wire",NULL,"air",NULL,NULL,NULL,0,1,"红石线悬空"},
	{"repeater","facing=north",NULL,NULL,NULL,NULL,0,1,"中继器悬空"},
	{"repeater","facing=north","stone",NULL,NULL,NULL,0,0,"中继器贴方块"},
	{"snow",NULL,"stone",NULL,NULL,NULL,0,0,"雪层要完整顶面"},
	{"snow",NULL,"ice",NULL,NULL,NULL,0,1,"#cannot_support_snow_layer含冰"},
	{"snow",NULL,"honey_block",NULL,NULL,NULL,0,0,"#support_override_snow_layer含蜂蜜块"},
	/*---- 霜冰：不看支撑，一律化成水（del=2＝预期变成水源；依据见rules.cpp的RULE_FROST和wiki:Frosted_Ice）----*/
	{"frosted_ice","age=0","stone",NULL,NULL,NULL,0,2,"霜冰化成水（level=0的水源）"},
	{"frosted_ice","age=3","stone",NULL,NULL,NULL,0,2,"快到期的霜冰一样化成水"},
	{"frosted_ice",NULL,"ice",NULL,NULL,NULL,0,2,"霜冰不挑下面是什么，照样化"},
	{"frosted_ice",NULL,NULL,NULL,NULL,NULL,0,2,"悬空/水里的霜冰也化"},
	{"oak_door","half=lower,facing=north",NULL,NULL,NULL,NULL,0,1,"门下半悬空"},
	{"oak_door","half=lower,facing=north","stone",NULL,"oak_door",NULL,0,0,"门下半站在方块上，上面是上半","half=upper,facing=north"},
	{"oak_door","half=lower,facing=north","stone",NULL,NULL,NULL,0,1,"门只剩下半（DoorBlock.updateShape：上半没了下半也没）"},
	{"oak_door","half=lower,facing=north","stone",NULL,"iron_door",NULL,0,5,"另一半是别种门：DoorBlock.updateShape 把下半改成上半那种门（下半先被判，所以照上半改）","half=upper,facing=north",
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:iron_door[facing=north,half=lower,hinge=left,open=false,powered=false]"},
	{"oak_door","half=upper,facing=north","oak_door","half=lower",NULL,NULL,0,0,"门上半下面有下半",NULL,"stone",NULL},
	{"oak_door","half=upper,facing=north","stone",NULL,NULL,NULL,0,1,"门上半下面不是下半"},
	{"bell","attachment=ceiling",NULL,NULL,"stone",NULL,0,0,"钟吊在方块底下"},
	{"bell","attachment=ceiling",NULL,NULL,NULL,NULL,0,1,"钟上面没搭着→拆（它那条meeting记录由fixer的POI阶段跟着删）"},
	/*---- 兴趣点（POI）方块：wiki 上没有"支撑没了会掉"的依据 ⇒ 悬空也必须留着（rules.cpp 登记成"不管"）----
	  这些是村民认领的工作站/炼药锅：误拆一个就等于把村民的职业/认领记录弄没了，所以专门有反例盯着*/
	{"lectern",NULL,NULL,NULL,NULL,NULL,0,0,"讲台悬空不拆（wiki无支撑措辞；POI:librarian）"},
	{"composter",NULL,NULL,NULL,NULL,NULL,0,0,"堆肥桶悬空不拆（farmer）"},
	{"barrel",NULL,NULL,NULL,NULL,NULL,0,0,"木桶悬空不拆（fisherman）"},
	{"smoker",NULL,NULL,NULL,NULL,NULL,0,0,"烟熏炉悬空不拆（butcher）"},
	{"blast_furnace",NULL,NULL,NULL,NULL,NULL,0,0,"高炉悬空不拆（armorer）"},
	{"cartography_table",NULL,NULL,NULL,NULL,NULL,0,0,"制图台悬空不拆（cartographer）"},
	{"brewing_stand",NULL,NULL,NULL,NULL,NULL,0,0,"酿造台悬空不拆（cleric）"},
	{"stonecutter",NULL,NULL,NULL,NULL,NULL,0,0,"切石机悬空不拆（mason）"},
	{"loom",NULL,NULL,NULL,NULL,NULL,0,0,"织布机悬空不拆（shepherd）"},
	{"smithing_table",NULL,NULL,NULL,NULL,NULL,0,0,"锻造台悬空不拆（toolsmith）"},
	{"grindstone","face=floor",NULL,NULL,NULL,NULL,0,0,"砂轮悬空不拆（wiki原文：it doesn't need a supporting block；weaponsmith）"},
	{"fletching_table",NULL,NULL,NULL,NULL,NULL,0,0,"制箭台悬空不拆（fletcher；当初POI表还漏过它）"},
	{"cauldron",NULL,NULL,NULL,NULL,NULL,0,0,"炼药锅悬空不拆（leatherworker）"},
	{"water_cauldron",NULL,NULL,NULL,NULL,NULL,0,0,"装水的炼药锅悬空不拆"},
	{"lava_cauldron",NULL,NULL,NULL,NULL,NULL,0,0,"装岩浆的炼药锅悬空不拆"},
	{"powder_snow_cauldron",NULL,NULL,NULL,NULL,NULL,0,0,"装细雪的炼药锅悬空不拆"},
	{"bee_nest",NULL,NULL,NULL,NULL,NULL,0,0,"蜂巢悬空不拆（完整方块，wiki无支撑措辞）"},
	{"beehive",NULL,NULL,NULL,NULL,NULL,0,0,"蜂箱悬空不拆"},
	{"lodestone",NULL,NULL,NULL,NULL,NULL,0,0,"磁石悬空不拆（完整方块）"},
	{"nether_portal","axis=x",NULL,NULL,NULL,NULL,0,1,"传送门上下是空气：门框平面里的邻居既不是传送门也不是黑曜石，门一定不完整（PortalShape.findAnyShape）"},
	{"nether_portal","axis=x","obsidian",NULL,"obsidian","obsidian",0,0,"传送门四周平面里都是黑曜石：要看整个门框，六面看不到，留着",NULL,NULL,NULL,NULL,"obsidian",1},
	{"nether_portal","axis=x","obsidian",NULL,"obsidian","stone",0,1,"传送门东边（axis=x 的平面里）是石头：框不是黑曜石，门不完整",NULL,NULL,NULL,NULL,"obsidian",1},
	{"nether_portal","axis=z","obsidian",NULL,"obsidian","obsidian",2,0,"axis=z 的传送门东西两边是空气：那是另一个轴，原版不管（wrongAxis）",NULL,NULL,NULL,NULL,"obsidian",3},
	{"test_instance_block",NULL,NULL,NULL,NULL,NULL,0,0,"测试用方块不拆（wiki只说不能生存模式放置）"},
	{"scaffolding",NULL,"stone",NULL,NULL,NULL,0,0,"脚手架下面有支撑"},
	{"scaffolding",NULL,"air",NULL,NULL,NULL,0,1,"脚手架悬空"},
	{"white_candle",NULL,"stone",NULL,NULL,NULL,0,0,"蜡烛放方块上"},
	{"cake",NULL,"stone",NULL,NULL,NULL,0,0,"蛋糕放方块上"},
	/*---- 藤蔓/地衣/洞穴藤蔓/菌绳 ----*/
	{"vine","north=true",NULL,NULL,NULL,"stone",3,0,"藤蔓贴在整方块侧面"},
	{"vine","north=true",NULL,NULL,NULL,"air",3,1,"五个面都没有支撑"},
	{"glow_lichen","up=true",NULL,NULL,"stone",NULL,0,0,"发光地衣贴方块底面"},
	{"sculk_vein","up=true",NULL,NULL,"stone",NULL,0,0,"幽匿脉络贴方块底面"},
	{"hanging_roots",NULL,NULL,NULL,"stone",NULL,0,0,"垂根吊在方块底面"},
	{"hanging_roots",NULL,NULL,NULL,"air",NULL,0,1,"上面没东西"},
	{"cave_vines","berries=false",NULL,NULL,"cave_vines_plant",NULL,0,0,"洞穴藤蔓上面是同类（最上面压着石头当天花板）",NULL,NULL,"stone"},
	{"cave_vines","berries=false",NULL,NULL,"air",NULL,0,1,"上面没东西"},
	{"twisting_vines",NULL,"stone",NULL,NULL,NULL,0,0,"缠怨藤下面只要是方块就行"},
	{"twisting_vines",NULL,"air",NULL,NULL,NULL,0,1,"下面没有方块"},
	{"weeping_vines",NULL,NULL,NULL,"stone",NULL,0,0,"垂泪藤上面只要是方块就行"},
	{"weeping_vines",NULL,NULL,NULL,"air",NULL,0,1,"上面没有方块"},
	/*---- 紫水晶/滴水石锥/珊瑚扇 ----*/
	{"amethyst_cluster","facing=up",NULL,NULL,NULL,NULL,0,1,"紫水晶簇悬空"},
	{"amethyst_cluster","facing=up","stone",NULL,NULL,NULL,0,0,"紫水晶簇贴方块"},
	{"pointed_dripstone","vertical_direction=down,thickness=tip",NULL,NULL,"stone",NULL,0,0,"石钟乳挂在上面的方块上"},
	{"pointed_dripstone","vertical_direction=down,thickness=tip",NULL,NULL,"air",NULL,0,1,"上面没东西"},
	{"cocoa","facing=north,age=0",NULL,NULL,NULL,"jungle_log",3,0,"可可豆贴在丛林原木侧面"},
	{"cocoa","facing=north,age=0",NULL,NULL,NULL,"stone",3,1,"背面不是丛林原木"},
	/*---- 属性缺失（坏存档/以后版本改了属性名）：判断不了就留着，宁可漏拆 ----*/
	{"wall_torch",NULL,NULL,NULL,NULL,"stone",2,0,"墙火把没有facing属性：判断不了贴哪面，留着不拆"},
	{"cocoa",NULL,NULL,NULL,NULL,"jungle_log",3,0,"可可豆没有facing属性：留着不拆"},
	{"attached_pumpkin_stem",NULL,"farmland",NULL,NULL,"pumpkin",0,0,"结果的瓜茎没有facing属性：留着不拆"},
	/*---- 紫颂 ----*/
	{"chorus_flower","age=0","end_stone",NULL,"air",NULL,0,0,"紫颂花：下面末地石（ChorusFlowerBlock.canSurvive前半句）"},
	{"chorus_flower","age=0","chorus_plant",NULL,"air",NULL,0,0,"紫颂花：下面紫颂植株",NULL,"end_stone"},
	{"chorus_flower","age=0",NULL,NULL,"air","chorus_plant",0,0,"紫颂花：下面空气，水平正好一个紫颂植株（其余三面都是空气）",NULL,NULL,NULL,"end_stone"},
	{"chorus_flower","age=0",NULL,NULL,"air",NULL,0,1,"紫颂花：下面空气、旁边也没紫颂植株，碎"},
	{"chorus_flower","age=0",NULL,NULL,"air","chorus_plant",0,1,"紫颂花：旁边还杵着一块石头（原版这种也碎）",NULL,NULL,NULL,NULL,"stone",1},
	{"chorus_flower","age=0",NULL,NULL,"air","chorus_plant",0,1,"紫颂花：水平挨着两个紫颂植株，碎",NULL,NULL,NULL,NULL,"chorus_plant",3},
	{"chorus_flower","age=0","chorus_flower",NULL,"air",NULL,0,1,"紫颂花：下面是紫颂花不是紫颂植株，碎"},
	{"chorus_plant",NULL,"end_stone",NULL,NULL,NULL,0,0,"紫颂植株长在末地石上（正下方有根）"},
	{"chorus_plant",NULL,"stone",NULL,NULL,NULL,0,1,"紫颂植株：下面既不是末地石也不是紫颂植株，旁边又没东西，碎"},
	/*横枝：自己下面空着，靠"旁边那株有根"撑着——末地的紫颂树全是这么长的，以前漏了这条，把树拆掉一大半*/
	{"chorus_plant",NULL,NULL,NULL,NULL,"chorus_plant",0,0,"紫颂植株横枝：自己下面是空气，但东边那株下面（y=-59）是末地石",NULL,NULL,NULL,"end_stone"},
	{"chorus_plant",NULL,NULL,NULL,"chorus_plant","chorus_plant",0,0,"紫颂植株横枝（上面还有一截）：同样靠旁边那株撑着",NULL,NULL,NULL,"end_stone"},
	{"chorus_plant",NULL,NULL,NULL,NULL,"chorus_plant",0,0,"紫颂植株：旁边那株有没有根在斜下方，六个面看不到——横枝一律留着（宁可漏拆）",NULL,NULL,NULL},
	/*原版第1条（水平挨着紫颂+上下都不空就碎）：末地自然生成的地形里就有这种格子，我们按"宁可漏拆"留着*/
	{"chorus_plant",NULL,"end_stone",NULL,"chorus_plant","chorus_plant",0,1,"紫颂植株：水平连着紫颂、上下都不空——ChorusPlantBlock.canSurvive 第一条，碎",NULL,NULL,NULL,"end_stone"},
	/*---- 气泡柱 ----*/
	{"bubble_column","drag=true","soul_sand",NULL,"water",NULL,0,0,"气泡柱下面灵魂沙+自己泡在水里"},
	{"bubble_column","drag=true","stone",NULL,"water",NULL,0,1,"下面不是岩浆块/灵魂沙"},
	{"bubble_column","drag=false","bubble_column","drag=false","water",NULL,0,0,"气泡柱整列：第二格下面还是气泡柱（原版canSurvive第一句）",NULL,"soul_sand",NULL},
	/*---- 灵魂沙：原版 SoulSandBlock 的 getBlockSupportShape 是**整方块**（只有走路碰撞箱是14/16），
	      所以蘑菇（mayPlaceOn=isSolidRender()）、火把、红石、铁轨、藤蔓这些在它身上/旁边都是合法的。
	      以前把它归进"有形状的"，下界15298个蘑菇里144个长在灵魂沙上被误拆----*/
	{"brown_mushroom",NULL,"soul_sand",NULL,NULL,NULL,0,0,"灵魂沙 isSolidRender()=true：蘑菇能长在灵魂沙上"},
	{"torch",NULL,"soul_sand",NULL,NULL,NULL,0,0,"灵魂沙顶面算整方块：火把能放"},
	{"redstone_wire","power=0","soul_sand",NULL,NULL,NULL,0,0,"灵魂沙算整方块：红石线能放（顶面带边）"},
	{"rail","shape=east_west,waterlogged=false","soul_sand",NULL,NULL,NULL,0,0,"灵魂沙算整方块：铁轨能铺"},
	{"vine","east=true","stone",NULL,NULL,"soul_sand",0,0,"藤蔓贴灵魂沙侧面：原版 support shape 是整方块",NULL,NULL,NULL,"dirt"},
	/*---- 名字里带 lantern 的整方块：南瓜灯/海晶灯（以前被 is_Thin 的 "*_lantern" 当薄片吞了） ----*/
	{"torch",NULL,"jack_o_lantern",NULL,NULL,NULL,0,0,"南瓜灯是整方块：能往上放火把"},
	{"torch",NULL,"sea_lantern",NULL,NULL,NULL,0,0,"海晶灯是整方块：能往上放火把"},
	/*---- 8层雪就是整方块（SnowLayerBlock.getBlockSupportShape=SHAPES[layers]） ----*/
	{"snow","layers=1","snow","layers=8",NULL,NULL,0,0,"8层雪顶面是满的：上面能再放雪",NULL,"stone",NULL},
	{"torch",NULL,"snow","layers=8",NULL,NULL,0,0,"8层雪是整方块：能放火把",NULL,"stone",NULL},
	{"rail","shape=east_west,waterlogged=false","snow","layers=8",NULL,NULL,0,0,"8层雪顶面带边：铁轨能铺",NULL,"stone",NULL},
	/*---- 大垂滴叶：叶叠叶（原版 canSurvive 第一句 below.is(this)） + 小垂滴叶泡水耕地 ----*/
	{"big_dripleaf","facing=north,tilt=none,waterlogged=false","big_dripleaf","facing=north,tilt=none,waterlogged=false",NULL,NULL,0,0,"叶叠叶：原版 canSurvive 第一句就是 below.is(this)",NULL,"dirt",NULL},
	{"small_dripleaf","half=lower,facing=north,waterlogged=true","farmland",NULL,"small_dripleaf",NULL,0,0,"泡水的耕地：原版第二段走 #supports_vegetation（含耕地）","half=upper,facing=north"},
	{"small_dripleaf","half=lower,facing=north,waterlogged=false","farmland",NULL,"small_dripleaf",NULL,0,1,"耕地不泡水：泥土类那批要求这格有水","half=upper,facing=north"},
	/*---- 挂着的红树胎生苗：上面必须是红树叶（别的树叶原版会掉） ----*/
	{"mangrove_propagule","hanging=true","dirt",NULL,"oak_leaves",NULL,0,1,"挂着但上面是橡树叶：原版只认 #supports_hanging_mangrove_propagule"},
	/*---- 仙人掌 F_NOSIDE：原版判 neighbor.isSolid()，花（noCollision）不算实心 ----*/
	{"cactus",NULL,"sand",NULL,NULL,"dandelion",0,0,"仙人掌旁边是花：原版 isSolid(花)=false，不破",NULL,NULL,NULL,"dirt"},
	{"cactus",NULL,"sand",NULL,NULL,"stone",0,1,"仙人掌旁边是石头：破"},
	{"cactus",NULL,"sand",NULL,NULL,"snow",0,0,"仙人掌旁边是8层雪：forceSolidOff，原版 isSolid=false",NULL,NULL,NULL,NULL,NULL,0,"layers=8"},
	/*---- 台阶/楼梯/活板门的"按面支撑"（SupportType.FULL/CENTER，wiki Slab/Stairs/Trapdoor 那几句） ----
	  上半砖只有顶面算整面、下半砖只有底面算整面；以前不分方向，这四种方向全判错。*/
	{"oak_door","half=lower,facing=north","stone_slab","type=top","oak_door",NULL,0,0,"门要下面的整面：上半砖顶面是满的（DoorBlock.canSurvive=isFaceSturdy(UP)）","half=upper,facing=north"},
	{"oak_door","half=lower,facing=north","stone_slab","type=bottom",NULL,NULL,0,1,"下半砖顶面是空的（形状没碰到格子顶），门放不住"},
	{"oak_door","half=lower,facing=north","stone_slab","type=double","oak_door",NULL,0,0,"双台阶整个是实心方块","half=upper,facing=north"},
	{"seagrass",NULL,"stone_slab","type=top",NULL,NULL,0,0,"海草要 isFaceSturdy(UP)：上半砖顶面够"},
	{"seagrass",NULL,"stone_slab","type=bottom",NULL,NULL,0,1,"下半砖顶面撑不住海草"},
	{"pointed_dripstone","vertical_direction=down,thickness=tip",NULL,NULL,"stone_slab",NULL,0,0,"钟乳石看上方的底面：下半砖底面是满的","type=bottom"},
	{"pointed_dripstone","vertical_direction=down,thickness=tip",NULL,NULL,"stone_slab",NULL,0,1,"上半砖底面是空的，挂不住钟乳石","type=top"},
	{"torch",NULL,"stone_slab","type=top",NULL,NULL,0,0,"火把中心支撑：上半砖顶面够（wiki:Torch 历史 12w25a 能放 upside down slab 上）"},
	{"torch",NULL,"stone_slab","type=bottom",NULL,NULL,0,1,"下半砖顶面是空面，火把放不住（和上一行区分开）"},
	{"lantern","hanging=true",NULL,NULL,"stone_slab",NULL,0,0,"吊灯要上面 isFaceSturdy(DOWN)：下半砖底面够","type=bottom"},
	{"lantern","hanging=true",NULL,NULL,"stone_slab",NULL,0,1,"上半砖底面是空的，吊不住灯","type=top"},
	{"brown_mushroom",NULL,"stone_slab","type=top",NULL,NULL,0,1,"蘑菇要 isSolidRender（整块实心）：上半砖只是顶面满，不算（F_SOLID）"},
	{"oak_door","half=lower,facing=north","stone_stairs","half=top,facing=north,shape=straight,waterlogged=false","oak_door",NULL,0,0,"上半楼梯的顶面是满的，门能放","half=upper,facing=north"},
	{"oak_door","half=lower,facing=north","stone_stairs","half=bottom,facing=north,shape=straight,waterlogged=false",NULL,NULL,0,1,"下半楼梯顶面不是整面，门不能放"},
	{"oak_door","half=lower,facing=north","oak_trapdoor","half=top,open=false,facing=north","oak_door",NULL,0,0,"关着的上半活板门顶面是满的：门上得去","half=upper,facing=north"},
	{"oak_door","half=lower,facing=north","oak_trapdoor","half=bottom,open=false,facing=north",NULL,NULL,0,1,"关着的下半活板门顶面不是整面"},
	{"lantern","hanging=true",NULL,NULL,"oak_trapdoor",NULL,0,0,"关着的下半活板门底面够吊灯","half=bottom,open=false,facing=north"},
	{"lantern","hanging=true",NULL,NULL,"oak_trapdoor",NULL,0,1,"关着的上半活板门底面是空的","half=top,open=false,facing=north"},
	/*---- 重力方块（26.1.1 用户规则：正下方是空气/水/岩浆/火才拆；可替换方块、含水方块都不算） ----*/
	{"sand",NULL,"air",NULL,NULL,NULL,0,1,"沙子悬空：下面空气，原版会掉"},
	{"sand",NULL,"stone",NULL,NULL,NULL,0,0,"沙子放在石头上：合法"},
	{"sand",NULL,"water",NULL,NULL,NULL,0,1,"沙子下面是水：liquid 也算 isFree，会掉"},
	{"sand",NULL,"short_grass",NULL,NULL,NULL,0,1,"矮草是可替换方块：FallingBlock.isFree，沙子会掉",NULL,"dirt",NULL},
	{"sand",NULL,"torch",NULL,NULL,NULL,0,0,"火把不是可替换的：沙子被火把托住（wiki:Torch 页原句）"},
	{"gravel",NULL,"air",NULL,NULL,NULL,0,1,"沙砾悬空"},
	{"gravel",NULL,"cobblestone",NULL,NULL,NULL,0,0,"沙砾有支撑"},
	{"anvil",NULL,"air",NULL,NULL,NULL,0,1,"铁砧悬空（AnvilBlock extends FallingBlock）"},
	{"dragon_egg",NULL,"air",NULL,NULL,NULL,0,1,"龙蛋悬空：26.1.1 的 DragonEggBlock 也继承 FallingBlock"},
	{"dragon_egg",NULL,"end_stone",NULL,NULL,NULL,0,0,"龙蛋放在末地石上：合法"},
	{"white_concrete_powder",NULL,"water",NULL,NULL,NULL,0,1,"正下方的水不让粉末凝固（ConcretePowderBlock.touchesLiquid 跳过向下），下面是水就掉"},
	{"white_concrete_powder",NULL,"air",NULL,NULL,NULL,0,1,"没碰到水的混凝土粉末悬空：照掉"},
	{"lime_concrete_powder",NULL,"stone",NULL,NULL,NULL,0,0,"混凝土粉末放在石头上：合法"},
	{"white_concrete_powder",NULL,"air",NULL,NULL,"water",0,3,"侧面挨着水也会凝固（ConcretePowderBlock.updateShape→touchesLiquid）"},
	{"sand",NULL,"lava",NULL,NULL,NULL,0,1,"下面是岩浆：拆"},
	{"sand",NULL,"fire",NULL,NULL,NULL,0,1,"下面是火：拆"},
	{"sand",NULL,"soul_fire",NULL,NULL,NULL,0,1,"下面是灵魂火：拆"},
	{"sand",NULL,"snow","layers=1",NULL,NULL,0,1,"下面是雪层（可替换，canBeReplaced 不看层数）：会掉"},
	{"sand",NULL,"seagrass",NULL,NULL,NULL,0,1,"下面是海草（可替换）：会掉"},
	{"sand",NULL,"vine","east=false,north=true,south=false,up=false,west=false",NULL,NULL,0,1,"下面是藤蔓（可替换）：会掉"},
	{"gravel",NULL,"dead_bush",NULL,NULL,NULL,0,1,"下面是枯灌木（可替换）：会掉",NULL,"sand",NULL},
	{"sand",NULL,"bubble_column","drag=true",NULL,NULL,0,1,"下面是气泡柱（liquid）：会掉",NULL,"magma_block",NULL},
	{"sand",NULL,"oak_slab","type=bottom,waterlogged=true",NULL,NULL,0,0,"下面是含水台阶：含水方块不算水，不拆"},
	{"suspicious_sand",NULL,"air",NULL,NULL,NULL,0,1,"可疑沙也是重力方块"},
	{"suspicious_gravel",NULL,"stone",NULL,NULL,NULL,0,0,"可疑沙砾放在石头上：合法"},
	{"chipped_anvil",NULL,"water",NULL,NULL,NULL,0,1,"开裂的铁砧下面是水：拆"},
	/*---- 中低优先级 Wiki 差异（26.1.1） ----*/
	{"stone_pressure_plate",NULL,"oak_leaves",NULL,NULL,NULL,0,1,"压力板不能放在树叶上（Wiki Pressure Plate §Placement）"},
	{"oak_pressure_plate",NULL,"tnt",NULL,NULL,NULL,0,0,"TNT是完整方块，压力板能放（“不能放TNT”是1.6的旧历史，现行Placement只排除树叶）"},
	{"amethyst_cluster","facing=up","oak_leaves",NULL,NULL,NULL,0,1,"紫水晶不能长在树叶上"},
	{"small_amethyst_bud","facing=up","honey_block",NULL,NULL,NULL,0,1,"紫水晶不能长在蜂蜜块上"},
	{"large_amethyst_bud","facing=up","stone_slab","type=top",NULL,NULL,0,0,"上半砖顶面是完整面：紫水晶能长"},
	{"amethyst_cluster","facing=up","small_amethyst_bud","facing=up",NULL,NULL,0,1,"紫水晶不能长在芽上（芽不是完整面）"},
	{"glow_lichen","up=true",NULL,NULL,"oak_fence",NULL,0,1,"发光地衣要完整面：栅栏底面不够"},
	{"sculk_vein","down=true","stone",NULL,NULL,NULL,0,0,"幽匿脉络贴在完整顶面上"},
	{"torch",NULL,"oak_fence_gate","facing=north",NULL,NULL,0,0,"栅栏门顶上照样给中心支撑（22w43a 去掉的是“beneath them”，Opacity/Placement 顶面表）"},
	{"leaf_litter",NULL,"oak_fence",NULL,NULL,NULL,0,1,"落叶层要顶面完整：栅栏不行（25w06a 改中心支撑，25w09a 又改回 full solid top face）"},
	{"scaffolding","distance=7",NULL,NULL,NULL,"scaffolding",0,1,"脚手架distance=7、下面没支撑：旁边有脚手架也掉"},
	{"scaffolding","distance=1",NULL,NULL,NULL,"scaffolding",0,0,"旁边那块 distance=0，自己算出来 1<7（ScaffoldingBlock.getDistance）",NULL,NULL,NULL,"stone",NULL,0,"bottom=false,distance=0,waterlogged=false"},
	{"scaffolding","distance=1",NULL,NULL,NULL,"scaffolding",0,1,"旁边那块 distance=6，自己算出来 7：掉（看邻居存的 distance，不看自己的）",NULL,NULL,NULL,"stone",NULL,0,"bottom=false,distance=6,waterlogged=false"},
	{"kelp",NULL,"hopper",NULL,NULL,NULL,0,1,"海带要完整顶面：漏斗不算"},
	{"kelp",NULL,"stone",NULL,NULL,NULL,0,0,"海带放在石头上"},
	{"bell","attachment=single_wall,facing=north",NULL,NULL,NULL,"stone",3,0,"墙钟：支撑在 facing 那一侧（北）（BellBlock.canSurvive）"},
	{"bell","attachment=single_wall,facing=north",NULL,NULL,NULL,"stone",2,1,"墙钟：facing 反面（南）的方块不算支撑"},
	{"bell","attachment=double_wall,facing=north",NULL,NULL,NULL,"stone",2,0,"两面贴墙的钟：剩一边就行（原版先变成贴那一边的 single_wall）"},
	{"lantern","hanging=true",NULL,NULL,"turtle_egg",NULL,0,0,"龟蛋能撑住挂在它下面的灯笼"},
	{"torch",NULL,"turtle_egg",NULL,NULL,NULL,0,1,"龟蛋是薄片：撑不住火把"},
	{"torch",NULL,"copper_lantern",NULL,NULL,NULL,0,1,"铜灯笼归薄片：撑不住火把"},
	{"snow",NULL,"oak_trapdoor","half=top,open=true,facing=north",NULL,NULL,0,1,"开着的活板门顶面不完整：雪层放不住"},
	{"snow",NULL,"oak_trapdoor","half=top,open=false,facing=north",NULL,NULL,0,0,"关着的上半活板门顶面完整：雪层能放"},
	{"attached_melon_stem","facing=east","dirt",NULL,NULL,"melon",0,1,"附生瓜茎只认耕地"},
	{"attached_melon_stem","facing=east","farmland",NULL,NULL,"melon",0,0,"附生瓜茎：耕地+朝向那面有西瓜"},
	/*---- 按面支撑（第三轮：对照 wiki Opacity/Placement 三张表和各方块页面） ----*/
	{"lantern","hanging=true",NULL,NULL,"oak_fence_gate",NULL,0,1,"栅栏门底下不再给中心支撑（Wiki Fence Gate 22w43a）","facing=north"},
	{"stone_pressure_plate",NULL,"cauldron",NULL,NULL,NULL,0,0,"炼药锅顶面带边：压力板能放（Wiki Pressure Plate：rigid 或 center）"},
	{"torch",NULL,"cauldron",NULL,NULL,NULL,0,1,"炼药锅顶面只有一圈边、没有中心：火把不行"},
	{"lantern","hanging=false","cauldron",NULL,NULL,NULL,0,1,"炼药锅顶面没有中心：灯笼不行"},
	{"torch",NULL,"chorus_flower","age=0",NULL,NULL,0,1,"紫颂花 23w31a 起不再给支撑（中英文 Chorus Flower §History；英文放置表这格是旧的）",NULL,"end_stone",NULL},
	{"oak_sign","rotation=0","chorus_flower","age=0",NULL,NULL,0,1,"紫颂花不是 legacy solid：告示牌不行",NULL,"end_stone",NULL},
	{"amethyst_cluster","facing=up","scaffolding",NULL,NULL,NULL,0,0,"脚手架顶面整面：紫水晶能长"},
	{"torch",NULL,"end_rod","facing=up",NULL,NULL,0,0,"末地烛端面给中心支撑"},
	{"torch",NULL,"lightning_rod","facing=up",NULL,NULL,0,0,"避雷针端面给中心支撑"},
	{"lantern","hanging=true",NULL,NULL,"end_rod",NULL,0,0,"末地烛下端面给中心支撑：灯笼能挂","facing=up"},
	{"torch",NULL,"sniffer_egg",NULL,NULL,NULL,0,0,"嗅探兽蛋顶面中心支撑"},
	{"cake",NULL,"oak_stairs","half=bottom,facing=north",NULL,NULL,0,0,"楼梯是 legacy solid：蛋糕能放（Wiki Cake：solid block）"},
	{"cake",NULL,"bamboo","age=0",NULL,NULL,0,0,"竹子是 legacy solid：蛋糕能放",NULL,"sand",NULL},
	{"cake",NULL,"scaffolding",NULL,NULL,NULL,0,1,"脚手架不是 solid：蛋糕不行"},
	{"candle_cake","lit=false","stone",NULL,NULL,NULL,0,0,"插蜡烛的蛋糕和蛋糕一样"},
	{"candle_cake","lit=false","air",NULL,NULL,NULL,0,1,"插蜡烛的蛋糕悬空"},
	{"oak_sign","rotation=0","oak_fence",NULL,NULL,NULL,0,0,"告示牌能放栅栏上（Wiki Sign）"},
	{"oak_sign","rotation=0","oak_sign","rotation=0",NULL,NULL,0,0,"告示牌能叠在告示牌上（Wiki Sign）"},
	{"oak_sign","rotation=0","torch",NULL,NULL,NULL,0,1,"火把不是 solid：告示牌不行"},
	{"white_banner","rotation=0","scaffolding",NULL,NULL,NULL,0,1,"脚手架不是 solid：旗帜不行"},
	{"sea_pickle","pickles=1","oak_fence",NULL,NULL,NULL,0,0,"栅栏顶面有碰撞箱：海泡菜能放"},
	{"sea_pickle","pickles=1","farmland",NULL,NULL,NULL,0,1,"耕地顶面不到一格：海泡菜不行"},
	{"sea_pickle","pickles=1","oak_leaves",NULL,NULL,NULL,0,0,"树叶顶面有碰撞箱：海泡菜能放"},
	{"tube_coral_fan",NULL,"oak_fence",NULL,NULL,NULL,0,1,"珊瑚扇要顶面整面：栅栏不行"},
	{"tube_coral_fan",NULL,"stone",NULL,NULL,NULL,0,0,"珊瑚扇放在石头上"},
	{"tube_coral",NULL,"stone",NULL,NULL,NULL,0,0,"珊瑚放在石头上"},
	{"tube_coral",NULL,"air",NULL,NULL,NULL,0,1,"珊瑚悬空"},
	{"tube_coral_wall_fan","facing=south",NULL,NULL,NULL,"oak_fence",3,1,"墙扇贴的那面要整面：栅栏侧面不行"},
	{"bell","attachment=floor,facing=north","oak_fence",NULL,NULL,NULL,0,1,"落地钟要整面：栅栏不行（Opacity/Placement）"},
	{"bell","attachment=floor,facing=north","stone",NULL,NULL,NULL,0,0,"落地钟放在石头上"},
	{"bell","attachment=ceiling,facing=north",NULL,NULL,"oak_fence",NULL,0,0,"吊钟只要上面底面中心支撑：栅栏下面可以"},
	{"redstone_wire",NULL,"cauldron",NULL,NULL,NULL,0,1,"红石线不认炼药锅（Opacity/Placement）"},
	{"redstone_wire",NULL,"hopper","facing=down",NULL,NULL,0,0,"红石线能放漏斗上"},
	{"repeater","facing=north","cauldron",NULL,NULL,NULL,0,0,"中继器认炼药锅（顶面带边）"},
	{"twisting_vines",NULL,"oak_fence",NULL,NULL,NULL,0,1,"缠怨藤要顶面整面：栅栏不行（Wiki Twisting Vines）"},
	{"twisting_vines",NULL,"twisting_vines_plant",NULL,NULL,NULL,0,0,"缠怨藤藤头接在藤身上",NULL,"stone",NULL},
	{"weeping_vines",NULL,NULL,NULL,"weeping_vines_plant",NULL,0,0,"垂泪藤藤头挂在藤身下",NULL,NULL,"stone"},
	{"cave_vines","berries=false",NULL,NULL,"oak_fence",NULL,0,1,"洞穴藤蔓要上面底面整面：栅栏不行（Wiki Cave Vines）"},
	{"hanging_roots",NULL,NULL,NULL,"oak_fence",NULL,0,1,"垂根要上面底面整面：栅栏不行"},
	{"spore_blossom",NULL,NULL,NULL,"oak_fence",NULL,0,0,"孢子花只要中心支撑：栅栏底面可以"},
	{"oak_hanging_sign","attached=false",NULL,NULL,"oak_fence",NULL,0,0,"吊牌挂在栅栏下面可以"},
	{"oak_hanging_sign","attached=false",NULL,NULL,"oak_fence_gate",NULL,0,1,"吊牌挂在栅栏门下面不行","facing=north"},
	{"tripwire_hook","facing=south",NULL,NULL,NULL,"oak_fence",3,1,"绊线钩要整面：栅栏侧面不行"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"barrel",2,0,"木桶是完整方块：侧面能挂墙上火把",NULL,NULL,NULL,NULL,NULL,0,"facing=up"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"loom",2,0,"织布机是完整方块：侧面能挂墙上火把",NULL,NULL,NULL,NULL,NULL,0,"facing=north"},
	{"redstone_wall_torch","facing=north",NULL,NULL,NULL,"oak_fence",2,1,"红石墙上火把要整面：栅栏侧面不行"},
	{"redstone_wall_torch","facing=north",NULL,NULL,NULL,"glass",2,0,"玻璃侧面是整面"},
	{"amethyst_cluster","facing=down",NULL,NULL,"composter",NULL,0,0,"堆肥桶底面是整面：紫水晶能长在下面（Opacity/Placement 底面表）"},
	{"pointed_dripstone","vertical_direction=down,thickness=tip",NULL,NULL,"farmland",NULL,0,0,"耕地底面是整面：钟乳石能挂"},
	{"glow_lichen","up=true",NULL,NULL,"oak_leaves",NULL,0,0,"树叶碰撞箱是满的：地衣能贴"},
	{"vine","north=true",NULL,NULL,NULL,"oak_leaves",3,0,"藤蔓能贴树叶"},
	{"vine","north=true",NULL,NULL,NULL,"farmland",3,1,"耕地侧面不满：藤蔓不行（Opacity/Placement 侧面表）"},
	{"brown_mushroom",NULL,"packed_ice",NULL,NULL,NULL,0,0,"浮冰不透明：蘑菇能长（Opacity/Placement）"},
	{"brown_mushroom",NULL,"glass",NULL,NULL,NULL,0,1,"玻璃不是整块不透明：蘑菇不行"},
	{"cactus",NULL,"sand",NULL,NULL,"lava",0,1,"仙人掌旁边是岩浆：碎（Wiki Cactus §Usage）"},
	{"cactus",NULL,"sand",NULL,"water",NULL,0,1,"仙人掌上面是水：碎"},
	{"cactus",NULL,"sand",NULL,NULL,"oak_fence",0,1,"栅栏是 legacy solid：仙人掌碎"},
	{"lily_pad",NULL,"water","level=1",NULL,NULL,0,1,"睡莲：流动水不行（#supports_lily_pad 流体标签只有 minecraft:water 水源）"},
	{"lily_pad",NULL,"oak_slab","type=bottom,waterlogged=true",NULL,NULL,0,0,"睡莲：含水方块里的水是水源"},
	{"pitcher_crop","half=lower,age=1","farmland",NULL,NULL,NULL,0,0,"瓶子草作物 age<3 只有一格：不要求上半"},
	{"pitcher_crop","half=lower,age=3","farmland",NULL,NULL,NULL,0,1,"age>=3 是两格：上面没有上半就拆"},
	{"pointed_dripstone","vertical_direction=up,thickness=tip","pointed_dripstone","vertical_direction=up,thickness=frustum",NULL,NULL,0,0,"石笋接在同向的滴水石锥上"},
	{"resin_clump","down=true","stone",NULL,NULL,NULL,0,0,"树脂团贴在石头顶面（Wiki Resin Clump）"},
	{"resin_clump","up=true",NULL,NULL,"air",NULL,0,1,"树脂团贴的那面没东西"},
	{"snow","layers=1","oak_leaves",NULL,NULL,NULL,0,0,"树叶碰撞箱顶面满：能积雪（Opacity/Placement）"},
	{"torch",NULL,"oak_leaves",NULL,NULL,NULL,0,1,"树叶支撑形状是空的：火把不行"},
	{"oak_button","face=floor,facing=north","oak_leaves",NULL,NULL,NULL,0,1,"树叶上不能放按钮"},
	{"oak_door","half=lower,facing=north","oak_leaves",NULL,NULL,NULL,0,1,"树叶上不能放门"},
	/*---- 第四轮：中英文 wiki 对照（判定箱/放置页的支撑形状、实心方块名单、各方块页） ----*/
	{"repeater","facing=north","chorus_flower","age=0",NULL,NULL,0,1,"紫颂花顶面不给支撑：中继器不行",NULL,"end_stone",NULL},
	{"amethyst_cluster","facing=up","chorus_flower","age=0",NULL,NULL,0,1,"紫颂花顶面不给支撑：紫水晶不行（英文紫水晶页也这么写）",NULL,"end_stone",NULL},
	{"snow","layers=1","chorus_flower","age=0",NULL,NULL,0,0,"紫颂花碰撞箱是满的：能积雪",NULL,"end_stone",NULL},
	{"vine","north=true",NULL,NULL,NULL,"chorus_flower",3,0,"紫颂花碰撞箱是满的：藤蔓能贴",NULL,NULL,NULL,"end_stone",NULL,0,"age=0"},
	{"cactus",NULL,"sand",NULL,NULL,"chorus_flower",0,0,"紫颂花强制不实心：仙人掌旁边可以（中文 仙人掌）",NULL,NULL,NULL,"end_stone",NULL,0,"age=0"},
	{"cactus",NULL,"sand",NULL,NULL,"redstone_wire",0,0,"红石线不实心：仙人掌旁边可以（中文 仙人掌）",NULL,NULL,NULL,"stone"},
	{"cactus",NULL,"sand",NULL,NULL,"lantern",0,1,"灯笼强制实心：仙人掌旁边不行（Opacity/Placement §Solid block）",NULL,NULL,NULL,"stone",NULL,0,"hanging=false"},
	{"torch",NULL,"sculk_sensor",NULL,NULL,NULL,0,1,"幽匿感测体只有 8/16 高（中文 判定箱）：顶面不给支撑"},
	{"torch",NULL,"sculk_shrieker",NULL,NULL,NULL,0,1,"幽匿尖啸体只有 8/16 高：顶面不给支撑"},
	{"torch",NULL,"heavy_core",NULL,NULL,NULL,0,1,"沉重核心只有 8/16 高：顶面不给支撑"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"sculk_sensor",2,1,"幽匿感测体侧面不是整面"},
	{"oak_sign","rotation=0","sculk_sensor",NULL,NULL,NULL,0,0,"幽匿感测体按碰撞箱算是实心（16+8+16≥35）：告示牌能放"},
	{"pale_hanging_moss","tip=true",NULL,NULL,"stone",NULL,0,0,"苍白垂须挂在整面底下（中文 苍白垂须）"},
	{"pale_hanging_moss","tip=true",NULL,NULL,"air",NULL,0,1,"苍白垂须上面没东西"},
	{"pale_hanging_moss","tip=true",NULL,NULL,"pale_oak_leaves",NULL,0,0,"苍白垂须天生挂在树叶下：碰撞面整面也算","distance=1,persistent=true,waterlogged=false"},
	{"pale_hanging_moss","tip=true",NULL,NULL,"oak_fence",NULL,0,1,"栅栏底面不是整面：苍白垂须不行"},
	{"pale_hanging_moss","tip=true",NULL,NULL,"pale_hanging_moss",NULL,0,0,"苍白垂须接在另一条下面","tip=false",NULL,"stone"},
	{"oak_sign","rotation=0","lantern","hanging=false",NULL,NULL,0,0,"灯笼强制实心：告示牌能放",NULL,"stone",NULL},
	{"oak_sign","rotation=0","turtle_egg","eggs=1,hatch=0",NULL,NULL,0,0,"海龟蛋强制实心：告示牌能放"},
	{"oak_sign","rotation=0","stone_pressure_plate","powered=false",NULL,NULL,0,0,"压力板强制实心：告示牌能放",NULL,"stone",NULL},
	{"oak_sign","rotation=0","dead_tube_coral","waterlogged=false",NULL,NULL,0,0,"失活珊瑚强制实心：告示牌能放",NULL,"stone",NULL},
	{"oak_sign","rotation=0","big_dripleaf","facing=north,tilt=none,waterlogged=false",NULL,NULL,0,1,"大型垂滴叶强制不实心：告示牌不行",NULL,"dirt",NULL},
	{"oak_sign","rotation=0","end_rod","facing=up",NULL,NULL,0,1,"竖着的末地烛：Opacity/Placement 顶面表写告示牌放不了（不按碰撞箱条件算实心）"},
	{"vine","up=true,north=false,south=false,east=false,west=false",NULL,NULL,"farmland",NULL,0,1,"藤蔓挂在下面看的是上方方块的**顶面**：耕地顶面不满（VineBlock：isAcceptableNeighbour(上方,DOWN)）","moisture=0"},
	{"vine","up=true,north=false,south=false,east=false,west=false",NULL,NULL,"oak_slab",NULL,0,0,"上半砖顶面满：藤蔓能挂在它下面","type=top,waterlogged=false"},
	{"vine","up=true,north=false,south=false,east=false,west=false",NULL,NULL,"oak_slab",NULL,0,1,"下半砖底面满但顶面不满：藤蔓挂不住","type=bottom,waterlogged=false"},
	{"vine","up=true,north=false,south=false,east=false,west=false",NULL,NULL,"oak_leaves",NULL,0,0,"树叶下面能挂藤蔓","distance=1,persistent=true,waterlogged=false"},
	{"vine","up=true,north=false,south=false,east=false,west=false",NULL,NULL,"stone",NULL,0,0,"石头下面能挂藤蔓"},
	{"glow_lichen","up=true,down=false,north=false,south=false,east=false,west=false,waterlogged=false",NULL,NULL,"farmland",NULL,0,0,"发光地衣只要底面整面：耕地下面可以","moisture=0"},
	{"candle","candles=1,lit=false,waterlogged=false","air",NULL,NULL,NULL,0,0,"蜡烛支撑没了会悬着（中文 蜡烛）"},
	{"white_candle","candles=1,lit=false,waterlogged=false","air",NULL,NULL,NULL,0,0,"带颜色的蜡烛也一样"},
	{"oak_sign","rotation=0","end_rod","facing=east",NULL,NULL,0,1,"横着的末地烛 16+4+4<35：不实心"},
	{"oak_sign","rotation=0","iron_chain","axis=x,waterlogged=false",NULL,NULL,0,0,"锁链强制实心（横着也是）"},
	{"oak_sign","rotation=0","conduit","waterlogged=false",NULL,NULL,0,0,"潮涌核心强制实心"},
	{"oak_sign","rotation=0","oak_fence_gate","facing=north,open=true",NULL,NULL,0,0,"栅栏门强制实心（开着也是）"},
	{"wall_torch","facing=east",NULL,NULL,NULL,"oak_stairs",1,0,"朝北的 inner_right 内角楼梯：背面（北）和东面整面",NULL,NULL,NULL,NULL,NULL,0,"facing=north,half=bottom,shape=inner_right,waterlogged=false"},
	{"wall_torch","facing=east",NULL,NULL,NULL,"oak_stairs",1,1,"朝北的 inner_left 内角楼梯：整面是北和西，东面不是",NULL,NULL,NULL,NULL,NULL,0,"facing=north,half=bottom,shape=inner_left,waterlogged=false"},
	{"wall_torch","facing=east",NULL,NULL,NULL,"oak_stairs",1,1,"外角楼梯：侧面只剩一块，不是整面",NULL,NULL,NULL,NULL,NULL,0,"facing=north,half=bottom,shape=outer_left,waterlogged=false"},
	{"wall_torch","facing=east",NULL,NULL,NULL,"oak_stairs",1,1,"直楼梯的侧面不是整面",NULL,NULL,NULL,NULL,NULL,0,"facing=north,half=bottom,shape=straight,waterlogged=false"},
	{"wall_torch","facing=north",NULL,NULL,NULL,"oak_stairs",2,0,"直楼梯的背面（facing 那面）是整面",NULL,NULL,NULL,NULL,NULL,0,"facing=north,half=bottom,shape=straight,waterlogged=false"},
	{"torch",NULL,"oak_wall_hanging_sign","facing=north,waterlogged=false",NULL,NULL,0,0,"墙上吊牌支撑形状＝正常形状：顶上的横杆给中心支撑"},
	{"lantern","hanging=true",NULL,NULL,"oak_wall_hanging_sign",NULL,0,0,"墙上吊牌下面能挂灯笼","facing=north,waterlogged=false"},
	/*---- 火（FireBlock/SoulFireBlock）：活不下去拆；活得下去照六邻重算样子 ----*/
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false","stone",NULL,NULL,NULL,0,0,"地上的火：下面顶面整面，五个面全关"},
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false",NULL,NULL,NULL,NULL,0,1,"悬空的火：下面不整面、六邻也没有可燃的"},
	{"fire","age=3,east=true,north=true,south=true,up=true,west=true","stone",NULL,NULL,NULL,0,5,"下面顶面整面的火五个面都关（BaseFireBlock.getState），age 照留",
		NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:fire[age=3,east=false,north=false,south=false,up=false,west=false]"},
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false",NULL,NULL,NULL,"oak_planks",0,5,"悬空的火靠东边的木板活：east 面打开",
		NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:fire[age=0,east=true,north=false,south=false,up=false,west=false]"},
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false","oak_planks",NULL,NULL,"oak_planks",0,0,"下面可燃：算地上的火，面全关（侧面的木板不开面）"},
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false","soul_soil",NULL,NULL,NULL,0,5,"火放在灵魂土上：变灵魂火",
		NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:soul_fire"},
	{"soul_fire",NULL,"soul_sand",NULL,NULL,NULL,0,0,"灵魂火在灵魂沙上"},
	{"soul_fire",NULL,"stone",NULL,NULL,NULL,0,1,"灵魂火下面不是 #soul_fire_base_blocks"},
	{"fire","age=0,east=false,north=false,south=false,up=false,west=false",NULL,NULL,NULL,"oak_leaves",0,1,"含水的树叶不可燃（FireBlock.getIgniteOdds）",
		NULL,NULL,NULL,NULL,NULL,0,"distance=1,persistent=true,waterlogged=true"},
	/*---- 外观转换（活得下去以后按邻居换成同一族的另一种） ----*/
	{"kelp_plant",NULL,"stone",NULL,"water",NULL,0,5,"海带茎上面不是海带：变藤头（GrowingPlantBodyBlock.updateShape，age 原版随机）",
		"level=0",NULL,NULL,NULL,NULL,0,NULL,"minecraft:kelp[age=*]"},
	{"kelp","age=3","stone",NULL,"kelp",NULL,0,5,"海带头上面还有海带：变海带茎（GrowingPlantHeadBlock.updateShape）",
		"age=0",NULL,NULL,NULL,NULL,0,NULL,"minecraft:kelp_plant"},
	{"cave_vines_plant","berries=true",NULL,NULL,"stone",NULL,0,5,"洞穴藤蔓藤身下面没东西：变藤头，berries 照留",
		NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:cave_vines[age=*,berries=true]"},
	{"weeping_vines",NULL,"weeping_vines",NULL,"stone",NULL,0,5,"垂泪藤藤头下面还有藤头：变藤身",
		NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:weeping_vines_plant"},
	{"big_dripleaf","facing=east,tilt=none,waterlogged=false","clay",NULL,"big_dripleaf",NULL,0,5,"大型垂滴叶上面还有叶子：变茎（facing/waterlogged 照留）",
		"facing=east,tilt=none,waterlogged=false",NULL,NULL,NULL,NULL,0,NULL,"minecraft:big_dripleaf_stem[facing=east,waterlogged=false]"},
	{"bamboo_sapling",NULL,"dirt",NULL,"bamboo",NULL,0,5,"竹笋上面是竹子：变竹子（BambooSaplingBlock.updateShape）",
		"age=0,leaves=none,stage=0",NULL,NULL,NULL,NULL,0,NULL,"minecraft:bamboo[age=0,leaves=none,stage=0]"},
	{"bamboo","age=0,leaves=none,stage=0","dirt",NULL,"bamboo",NULL,0,5,"上面那节竹子 age 更大：自己 age 换成下一个（BambooStalkBlock.updateShape）",
		"age=1,leaves=none,stage=0",NULL,NULL,NULL,NULL,0,NULL,"minecraft:bamboo[age=1,leaves=none,stage=0]"},
	{"oak_door","facing=east,half=upper,hinge=left,open=false,powered=false","oak_door","facing=north,half=lower,hinge=right,open=true,powered=false",NULL,NULL,0,5,
		"两半朝向/铰链/开关不一致：下半（y=-59）先被判，照上半改成一样（DoorBlock.updateShape 两半都是照对方改，谁先判谁改），上半就不动了",
		NULL,"stone",NULL,NULL,NULL,0,NULL,"minecraft:oak_door[facing=east,half=upper,hinge=left,open=false,powered=false]"},
	/*---- 栅栏/墙/苍白苔藓地毯：连接照游戏重算（FenceBlock/WallBlock/MossyCarpetBlock.updateShape，mcdump.sh cmp 逐状态对拍过） ----*/
	{"oak_fence","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"stone",0,5,"栅栏东边是石头（朝这边整面）：east 连上（FenceBlock.connectsTo）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_fence[east=true,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_fence",1,5,"木栅栏连木栅栏；东边的下界砖栅栏不连（#wooden_fences 不一样）",NULL,NULL,NULL,NULL,"nether_brick_fence",0,NULL,"minecraft:oak_fence[east=false,north=false,south=false,waterlogged=false,west=true]"},
	{"oak_fence","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_fence_gate",0,5,"东边的栅栏门 facing=north（门板东西向横着）：连（FenceGateBlock.connectsToDirection）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,in_wall=false,open=false,powered=false","minecraft:oak_fence[east=true,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_fence_gate",0,5,"东边的栅栏门 facing=east（门板南北向）：不连",NULL,NULL,NULL,NULL,NULL,0,"facing=east,in_wall=false,open=false,powered=false","minecraft:oak_fence[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"pumpkin",0,5,"南瓜朝这边整面也不连（Block.isExceptionForConnection）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_fence[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_leaves",0,5,"树叶不连（支撑面是空的，也是 isExceptionForConnection）",NULL,NULL,NULL,NULL,NULL,0,"distance=1,persistent=true,waterlogged=false","minecraft:oak_fence[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"cobblestone_wall",0,5,"墙的侧面不是整面：栅栏不连墙",NULL,NULL,NULL,NULL,NULL,0,"east=none,north=none,south=none,up=true,waterlogged=false,west=none","minecraft:oak_fence[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence","east=false,north=false,south=false,waterlogged=true,west=false",NULL,NULL,NULL,"glass",3,5,"含水栅栏北边是玻璃（整面）：连上，含水照留",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_fence[east=false,north=true,south=false,waterlogged=true,west=false]"},
	{"oak_fence","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"modx:thing",0,5,"挨着模组方块：不知道它的标签/形状，整格不动",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_fence[east=true,north=false,south=false,waterlogged=false,west=false]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,NULL,NULL,0,5,"孤零零的墙：四边不连照样立柱子（WallBlock.shouldRaisePost）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:cobblestone_wall[east=none,north=none,south=none,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=low,north=none,south=none,up=false,waterlogged=false,west=low",NULL,NULL,NULL,NULL,0,5,"两边都没东西了：连接收回去，立柱子",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:cobblestone_wall[east=none,north=none,south=none,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,NULL,"stone",3,5,"南北两边接石头、上面空：直线不立柱子，两边 low",NULL,NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=low,south=low,up=false,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"stone","stone",3,5,"上面是石头（碰撞箱底面整面）：两边 tall，tall 直线不立柱子",NULL,NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=tall,south=tall,up=false,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"white_carpet","stone",3,5,"上面是地毯（碰撞箱贴底一层）：照样 tall",NULL,NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=tall,south=tall,up=false,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"oak_slab","stone",3,5,"上面是上半砖（碰撞箱不贴底）：low 直线","type=top,waterlogged=false",NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=low,south=low,up=false,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"torch","stone",3,5,"上面是火把（#wall_post_override）：立柱子",NULL,NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=low,south=low,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"lantern","stone",3,5,"上面是放在地上的灯笼（碰撞箱底面盖住中间那块）：立柱子，臂还是 low","hanging=false,waterlogged=false",NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=low,south=low,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,"cobblestone_wall","stone",3,5,"上面那段墙自己没连东西、会被重算成立柱子的：下面这段跟着立柱子（先判下面时上面还是直线，判完上面再判一次）","east=none,north=low,south=low,up=false,waterlogged=false,west=none",NULL,NULL,NULL,"stone",2,NULL,"minecraft:cobblestone_wall[east=none,north=low,south=low,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,NULL,"glass_pane",0,5,"东边是玻璃板（IronBarsBlock）：连，端头立柱子",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:cobblestone_wall[east=low,north=none,south=none,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,NULL,"stone",0,5,"东边石头、西边栅栏（墙不连栅栏）：端头立柱子",NULL,NULL,NULL,NULL,"oak_fence",1,NULL,"minecraft:cobblestone_wall[east=low,north=none,south=none,up=true,waterlogged=false,west=none]"},
	{"cobblestone_wall","east=none,north=none,south=none,up=true,waterlogged=false,west=none",NULL,NULL,NULL,"oak_fence_gate",3,5,"北边栅栏门 facing=east（门板南北向）、南边石头：一条直线",NULL,NULL,NULL,NULL,"stone",2,"facing=east,in_wall=false,open=false,powered=false","minecraft:cobblestone_wall[east=none,north=low,south=low,up=false,waterlogged=false,west=none]"},
	{"pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none","stone",NULL,NULL,"stone",0,5,"铺地的苍白苔藓地毯东边能贴：长出 low 面（MossyCarpetBlock.getUpdatedState，铺地的会开面）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:pale_moss_carpet[bottom=true,east=low,north=none,south=none,west=none]"},
	{"pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none","stone",NULL,"pale_moss_carpet","stone",0,5,"上面那截东边也有面：铺地那块的东边是 tall","bottom=false,east=low,north=none,south=none,west=none",NULL,NULL,NULL,NULL,0,NULL,"minecraft:pale_moss_carpet[bottom=true,east=tall,north=none,south=none,west=none]","stone"},
	{"pale_moss_carpet","bottom=true,east=low,north=none,south=none,west=none","stone",NULL,NULL,NULL,0,5,"东边没东西能贴：面收回去",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:pale_moss_carpet[bottom=true,east=none,north=none,south=none,west=none]"},
	{"pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none","stone",NULL,NULL,"oak_leaves",0,5,"树叶碰撞面满：能贴",NULL,NULL,NULL,NULL,NULL,0,"distance=1,persistent=true,waterlogged=false","minecraft:pale_moss_carpet[bottom=true,east=low,north=none,south=none,west=none]"},
	{"pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none",NULL,NULL,NULL,NULL,0,1,"铺地的下面是空气：拆",NULL,NULL,NULL,NULL,NULL,0,NULL,NULL},
	{"pale_moss_carpet","bottom=false,east=low,north=none,south=none,west=none","stone",NULL,NULL,"stone",0,1,"上面那截下面不是铺地的那块：拆",NULL,NULL,NULL,NULL,NULL,0,NULL,NULL},
	{"pale_moss_carpet","bottom=false,east=low,north=none,south=none,west=none","pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none",NULL,"stone",0,1,"上面那截东边能贴、但下面那块东边是 none：low 面没了，一个面不剩就拆",NULL,NULL,NULL,NULL,NULL,0,NULL,NULL},
	{"pale_moss_carpet","bottom=false,east=tall,north=none,south=none,west=none","pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none",NULL,"stone",0,5,"上面那截原来是 tall：原版只看能不能贴，不再看下面那块",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:pale_moss_carpet[bottom=false,east=tall,north=none,south=none,west=none]"},
	{"pale_moss_carpet","bottom=false,east=low,north=none,south=none,west=none","pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none",NULL,"stone",0,5,"上面那截：东边能贴、下面那块东边也长出了面，留着",NULL,NULL,NULL,"stone",NULL,0,NULL,"minecraft:pale_moss_carpet[bottom=false,east=low,north=none,south=none,west=none]"},
	{"pale_moss_carpet","bottom=false,east=low,north=none,south=none,west=none","pale_moss_carpet","bottom=true,east=none,north=none,south=none,west=none",NULL,NULL,0,1,"上面那截东边没东西能贴：拆",NULL,NULL,NULL,NULL,NULL,0,NULL,NULL},
	/*---- 玻璃板/铁栏杆、栅栏门贴墙、箱子配对、雪地草、紫颂植株的连接（IronBarsBlock/FenceGateBlock/ChestBlock/SnowyBlock/ChorusPlantBlock.updateShape） ----*/
	{"glass_pane","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"stone",0,5,"玻璃板东边是石头（整面）：连（IronBarsBlock.attachsTo）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:glass_pane[east=true,north=false,south=false,waterlogged=false,west=false]"},
	{"glass_pane","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"cobblestone_wall",1,5,"玻璃板西边是墙：连（墙也连玻璃板）",NULL,NULL,NULL,NULL,NULL,0,"east=none,north=none,south=none,up=true,waterlogged=false,west=none","minecraft:glass_pane[east=false,north=false,south=false,waterlogged=false,west=true]"},
	{"iron_bars","east=false,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"white_stained_glass_pane",2,5,"铁栏杆南边是染色玻璃板（都是 IronBarsBlock）：连",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:iron_bars[east=false,north=false,south=true,waterlogged=false,west=false]"},
	{"glass_pane","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_fence",0,5,"玻璃板不连栅栏（栅栏侧面不整面，也不在 attachsTo 里）",NULL,NULL,NULL,NULL,NULL,0,"east=false,north=false,south=false,waterlogged=false,west=false","minecraft:glass_pane[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"glass_pane","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"oak_fence_gate",0,5,"玻璃板不连栅栏门（只有栅栏和墙认栅栏门）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,in_wall=false,open=false,powered=false","minecraft:glass_pane[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"copper_bars","east=true,north=false,south=false,waterlogged=false,west=false",NULL,NULL,NULL,"melon",0,5,"西瓜整面也不连（isExceptionForConnection）",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:copper_bars[east=false,north=false,south=false,waterlogged=false,west=false]"},
	{"oak_fence_gate","facing=north,in_wall=false,open=false,powered=false",NULL,NULL,NULL,"cobblestone_wall",0,5,"栅栏门 facing=north、门板东头是墙：放低（in_wall=true）",NULL,NULL,NULL,NULL,NULL,0,"east=none,north=none,south=none,up=true,waterlogged=false,west=none","minecraft:oak_fence_gate[facing=north,in_wall=true,open=false,powered=false]"},
	{"oak_fence_gate","facing=north,in_wall=true,open=false,powered=false",NULL,NULL,NULL,"cobblestone_wall",3,5,"墙在门的正面（北）不算：in_wall=false",NULL,NULL,NULL,NULL,NULL,0,"east=none,north=none,south=none,up=true,waterlogged=false,west=none","minecraft:oak_fence_gate[facing=north,in_wall=false,open=false,powered=false]"},
	{"oak_fence_gate","facing=north,in_wall=true,open=false,powered=false",NULL,NULL,NULL,"stone",0,5,"门板东头是石头不是墙：in_wall=false",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_fence_gate[facing=north,in_wall=false,open=false,powered=false]"},
	{"chest","facing=north,type=single,waterlogged=false",NULL,NULL,NULL,"chest",0,5,"东边是大箱子的右半、指着自己：单箱补成左半（ChestBlock.updateShape）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,type=right,waterlogged=false","minecraft:chest[facing=north,type=left,waterlogged=false]"},
	{"chest","facing=north,type=left,waterlogged=false",NULL,NULL,NULL,NULL,0,5,"大箱子左半的另一半（东边）没了：变单箱",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:chest[facing=north,type=single,waterlogged=false]"},
	{"chest","facing=north,type=left,waterlogged=false",NULL,NULL,NULL,"trapped_chest",0,5,"另一半是陷阱箱：不是同一种箱子，变单箱",NULL,NULL,NULL,NULL,NULL,0,"facing=north,type=right,waterlogged=false","minecraft:chest[facing=north,type=single,waterlogged=false]"},
	{"chest","facing=north,type=left,waterlogged=false",NULL,NULL,NULL,"chest",0,5,"另一半是朝南的单箱：原版不动（只有不是箱子了才拆开）",NULL,NULL,NULL,NULL,NULL,0,"facing=south,type=single,waterlogged=false","minecraft:chest[facing=north,type=left,waterlogged=false]"},
	{"chest","facing=north,type=single,waterlogged=false",NULL,NULL,NULL,"chest",0,5,"东边是单箱：两个单箱不会自己合起来",NULL,NULL,NULL,NULL,NULL,0,"facing=north,type=single,waterlogged=false","minecraft:chest[facing=north,type=single,waterlogged=false]"},
	{"exposed_copper_chest","facing=north,type=left,waterlogged=false",NULL,NULL,NULL,"copper_chest",0,5,"铜箱子两半氧化程度不一样：先被判的这一半照另一半改（CopperChestBlock.updateShape）",NULL,NULL,NULL,NULL,NULL,0,"facing=north,type=right,waterlogged=false","minecraft:copper_chest[facing=north,type=left,waterlogged=false]"},
	{"grass_block","snowy=false","stone",NULL,"snow",NULL,0,5,"草方块上面是雪层：snowy=true（SnowyBlock.updateShape）","layers=1",NULL,NULL,NULL,NULL,0,NULL,"minecraft:grass_block[snowy=true]"},
	{"grass_block","snowy=true","stone",NULL,NULL,NULL,0,5,"上面没雪了：snowy=false",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:grass_block[snowy=false]"},
	{"podzol","snowy=false","stone",NULL,"powder_snow",NULL,0,5,"灰化土上面是细雪（#snow）：snowy=true",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:podzol[snowy=true]"},
	/*---- 只是样子的属性（2026-11-14 下半）：蘑菇方块的面、音符盒音色、树叶 distance、石锥粗细、脚手架 distance/bottom ----*/
	{"brown_mushroom_block","down=true,east=true,north=true,south=true,up=true,west=true","stone",NULL,"brown_mushroom_block",NULL,0,5,
		"上面是同一种蘑菇方块：up 关掉（HugeMushroomBlock.updateShape）","down=true,east=true,north=true,south=true,up=true,west=true",
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:brown_mushroom_block[down=true,east=true,north=true,south=true,up=false,west=true]"},
	{"note_block","instrument=harp,note=3,powered=false","sand",NULL,NULL,NULL,0,5,"音符盒下面是沙子：snare（NoteBlock.setInstrument）",NULL,
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:note_block[instrument=snare,note=3,powered=false]"},
	{"note_block","instrument=harp,note=0,powered=false","stone",NULL,"skeleton_skull",NULL,0,5,"上面是骷髅头：用头颅的音色（worksAboveNoteBlock）","powered=false,rotation=0",
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:note_block[instrument=skeleton,note=0,powered=false]"},
	{"oak_leaves","distance=7,persistent=false,waterlogged=false","stone",NULL,NULL,"oak_log",0,5,"旁边是原木：distance=1（LeavesBlock.updateDistance）",NULL,
		NULL,NULL,NULL,NULL,0,"axis=y","minecraft:oak_leaves[distance=1,persistent=false,waterlogged=false]"},
	{"oak_leaves","distance=1,persistent=false,waterlogged=false","stone",NULL,NULL,NULL,0,5,"四周没有原木/树叶：distance=7，不拆（游戏慢慢腐烂）",NULL,
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:oak_leaves[distance=7,persistent=false,waterlogged=false]"},
	{"pointed_dripstone","thickness=tip,vertical_direction=up,waterlogged=false","stone",NULL,"pointed_dripstone",NULL,0,5,
		"石笋上面接着一根同向的尖：自己变 frustum（calculateDripstoneThickness）","thickness=tip,vertical_direction=up,waterlogged=false",
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:pointed_dripstone[thickness=frustum,vertical_direction=up,waterlogged=false]"},
	{"scaffolding","bottom=true,distance=3,waterlogged=false","stone",NULL,NULL,NULL,0,5,"脚手架下面是石头：distance=0、bottom=false（ScaffoldingBlock.tick）",NULL,
		NULL,NULL,NULL,NULL,0,NULL,"minecraft:scaffolding[bottom=false,distance=0,waterlogged=false]"},
	{"mycelium","snowy=true","stone",NULL,"white_carpet",NULL,0,5,"菌丝上面是地毯：snowy=false",NULL,NULL,NULL,NULL,NULL,0,NULL,"minecraft:mycelium[snowy=false]"},
	{"chorus_plant","down=false,east=false,north=false,south=false,up=false,west=false","end_stone",NULL,"chorus_flower",NULL,0,5,"紫颂植株：下面末地石、上面紫颂花，连上下（ChorusPlantBlock.updateShape）","age=0",NULL,NULL,NULL,NULL,0,NULL,"minecraft:chorus_plant[down=true,east=false,north=false,south=false,up=true,west=false]"},
	{"chorus_plant","down=true,east=true,north=true,south=false,up=false,west=false","end_stone",NULL,NULL,"chorus_plant",0,5,"东边是紫颂植株：连东边，北边空了就断开",NULL,NULL,NULL,"end_stone",NULL,0,NULL,"minecraft:chorus_plant[down=true,east=true,north=false,south=false,up=false,west=false]"},
};
static const int case_cnt=(int)(sizeof(cases)/sizeof(cases[0]));

static const char *D="minecraft:overworld";
static std::map<std::pair<int32_t,int32_t>,Chunk*> chunks;/*区块坐标→内存里的区块（最后按区域落盘）*/
static Chunk *get_CaseChunk(int32_t cx,int32_t cz){/*没有就建一个完整区块：只有 Y=-4 一段全空气*/
	auto it=chunks.find({cx,cz});
	if(it!=chunks.end()){
		return it->second;
	}
	Chunk *c=new Chunk(cx,cz);
	c->con.set_String("Status","minecraft:full");
	c->con.set_Int("DataVersion",4788);
	c->con.set_Long("InhabitedTime",1);
	c->con.set("sections",new nbt_List(NBT_COMPOUND));
	chunks[{cx,cz}]=c;
	return c;
}
static void ensure_Section(Chunk *c,int32_t sy){/*这个区块没有 Y==sy 的段就补一段全空气的（用例要往里放方块）*/
	if(c->get_Section(sy)){
		return;
	}
	nbt_Compound *sec=new nbt_Compound;
	sec->set("Y",new nbt_Byte((int8_t)sy));
	nbt_Compound *states=new nbt_Compound;
	nbt_List *pal=new nbt_List(NBT_COMPOUND);
	nbt_Compound *air=new nbt_Compound;
	air->set_String("Name","minecraft:air");
	pal->push_back(air);
	states->set("palette",pal);
	sec->set("block_states",states);
	((nbt_List*)c->data()->get("sections"))->push_back(sec);/*data()先清游标，再直接改树*/
}
static nbt_Compound parse_Props(const char *props){/*"half=upper,facing=north" → Properties*/
	nbt_Compound p;
	std::string s=props?props:"";
	size_t pos=0;
	while(pos<s.size()){
		size_t q=s.find(',',pos);
		if(q==std::string::npos){
			q=s.size();
		}
		std::string kv=s.substr(pos,q-pos);
		size_t eq=kv.find('=');
		if(eq!=std::string::npos){
			p.set_String(kv.substr(0,eq),kv.substr(eq+1));
		}
		pos=q+1;
	}
	return p;
}
static void put_BlockAt(int32_t wx,int32_t wy,int32_t wz,const char *name,const char *props){/*按世界坐标摆方块（负坐标也对）*/
	if(!name||!name[0]){
		return;
	}
	int32_t cx=(int32_t)((wx>=0?wx:wx-15)/16),cz=(int32_t)((wz>=0?wz:wz-15)/16);
	Chunk *c=get_CaseChunk(cx,cz);
	ensure_Section(c,get_SectionY(wy));
	std::string full=strchr(name,':')?name:std::string("minecraft:")+name;
	Block b(nbt_String(full),parse_Props(props));
	if(!c->set_Block(b,wx-cx*16,wy,wz-cz*16)){
		printf("mkrule: set_Block失败 %s %d %d %d\n",name,wx,wy,wz);
		exit(1);
	}
}
static int32_t get_SideX(int32_t wx,int dir){return dir==0?wx+1:(dir==1?wx-1:wx);}/*侧面邻居的 x：dir 0 东 1 西，南北不变*/
static int32_t get_SideZ(int dir){return dir==2?1:(dir==3?-1:0);}/*侧面邻居的 z：dir 2 南 3 北（用例都在 z=0 那一排）*/
int main(int argc,char **argv){/*按用例表造世界：每条用例占一列，放好自己和要用到的邻居，同时写出 cases.tsv 给 check_rules.py 核对*/
	if(argc<2){
		printf("用法：%s <世界目录>\n",argv[0]);
		return 1;
	}
	std::string root=argv[1];
	const int y_self=-58,y_below=-59,y_above=-57,y_below2=-60,y_above2=-56;
	int last_x=1+(case_cnt-1)*4;
	int cx_max=last_x/16+1;
	for(int cx=0;cx<=cx_max;cx++){/*整片都建出来（空段也是“读得到的空气”），免得邻居读不出来被判blind*/
		for(int cz=-1;cz<=1;cz++){
			ensure_Section(get_CaseChunk(cx,cz),-4);
		}
	}
	for(int i=0;i<case_cnt;i++){
		const Case &k=cases[i];
		int wx=1+i*4;
		put_BlockAt(wx,y_below2,0,"stone",NULL);/*支撑块自己也要站得住（沙/仙人掌这些也受规则管）*/
		if(k.below2){
			put_BlockAt(wx,y_below2,0,k.below2,NULL);
			put_BlockAt(wx,y_below2-1,0,"stone",NULL);
		}
		put_BlockAt(wx,y_below,0,k.below,k.below_props);
		put_BlockAt(wx,y_self,0,k.self,k.props);
		put_BlockAt(wx,y_above,0,k.above,k.above_props);
		put_BlockAt(wx,y_above2,0,k.above2,NULL);
		if(k.side){
			put_BlockAt(get_SideX(wx,k.side_dir),y_self,get_SideZ(k.side_dir),k.side,k.side_props);
			put_BlockAt(get_SideX(wx,k.side_dir),y_below,get_SideZ(k.side_dir),k.side_below,NULL);
		}
		if(k.side2){
			put_BlockAt(get_SideX(wx,k.side2_dir),y_self,get_SideZ(k.side2_dir),k.side2,NULL);
		}
		if(k.side&&k.side_above){
			put_BlockAt(get_SideX(wx,k.side_dir),y_above,get_SideZ(k.side_dir),k.side_above,NULL);
		}
	}
	Level lv(root.c_str(),1024,16,2);
	nbt_Compound dat,data,version;
	version.set_Int("Id",4788);
	version.set_String("Name","26.1.1");
	data.set_Int("DataVersion",4788);
	data.set("Version",version.copy());
	dat.set("Data",data.copy());
	if(!lv.set_Dat("level.dat",dat)||!lv.save()){
		printf("mkrule: level.dat 写不了\n");
		return 1;
	}
	Level world(root.c_str(),1024,16,2);/*重新打开：按刚写的 DataVersion 选新布局目录*/
	for(auto &kv:chunks){
		if(!world.set_Chunk(D,kv.first.first,kv.first.second,*kv.second)){
			printf("mkrule: 区块(%d,%d)写不进去\n",kv.first.first,kv.first.second);
			return 1;
		}
		delete kv.second;
	}
	if(!world.close()){
		printf("mkrule: 落盘失败\n");
		return 1;
	}
	FILE *fp=fopen((root+"/cases.tsv").c_str(),"w");
	if(!fp){
		return 1;
	}
	fprintf(fp,"#cases\t%d\n",case_cnt);
	fprintf(fp,"x\tself\tdel\twhy\texpect\n");
	for(int i=0;i<case_cnt;i++){
		fprintf(fp,"%d\t%s\t%d\t%s\t%s\n",1+i*4,cases[i].self,cases[i].del,cases[i].why,cases[i].expect?cases[i].expect:"");
	}
	fclose(fp);
	printf("mkrule: %d 个用例，写到 %s（y=%d 是被测方块，x 最大 %d）\n",case_cnt,root.c_str(),y_self,last_x);
	return 0;
}
