/*	fixer.cpp —— 修图（单独编成fixer，规则从rules.so来）
 *
 *	干什么：在范围里逐格扫，按方块种类查"它靠什么才能存在"（原版 canSurvive），
 *	靠不住的按规则处理：多数是拆掉，藤蔓/发光地衣/幽匿脉络这种就把撑不住的那几个面关掉。
 *	拆的时候：原来是含水物（waterlogged=true，或者海草/海带/珊瑚这种泡在水里的）就换成水、不换空气——
 *	水是一整片，写空气会在水底留洞，还会让旁边靠这格水活着的东西白丢水源（MC自己拆含水方块也是留水）。
 *
 *	怎么判全部交给规则插件（rules.so，接口见rules.hpp，规则本身在rules.cpp）：这个文件只管
 *	  · 读存档、按坐标认区块（Fix_Cache）、一段section的调色板建表（Fix_Sec）；
 *	  · 逐格调用插件的judge，按它说的拆/关面；改动就地连锁补判（Fix_Work）；
 *	  · 落盘和统计报告。
 *	换规则（改rules.cpp重编rules.so，或者干脆换一个.so）不用动这个文件。
 *
 *	改动会连锁（拆了下面的，上面的草也得跟着没），所以每改一格就往它的6个严格相邻接着看（fixer.py就是这么写的：
 *	  改完 -> for p in 6个相邻: 再判p），扫完一个区块把队列清空（work_Push/work_Drain）——链子当场就地拆完，
 *	不用靠"再整段重扫一遍"去发现。队列空了就等于全图到了不动点：全量扫的时候每格都判过一次，之后每次改动
 *	又把它影响到的格子重新判过，所以"最后一次可能影响它的改动"之后它一定被判过了（这就是"扫一遍就够"的理由）。
 *
 *	为什么必然收敛（所以可以放心让它跑到停；"规则只会把东西变少"是插件的合约，见rules.hpp）：
 *	  规则只会做三件事——①把方块拆掉（含水物换成水，其余换空气）；②把藤蔓/发光地衣/幽匿脉络身上
 *	  "撑不住的那几个true面"改成false；③霜冰整块换成水（RULE_FROST，也是"表里的方块→表外的水"）。
 *	  取一个量 M = 范围内"要管的方块"个数 + 这些方块身上"属性为true的方向面"总数，M是个非负整数：
 *	    ①每拆掉一个方块，方块个数必减1（拆之前先看过它不是空气，见fix_At里的兜底判断，所以绝不会"把空气拆成空气"）；
 *	    ②每关掉一个面，true面总数至少减1（bad[]里只收当前确实是true的面，一条都没有时返回的是"不动"）；
 *	  也就是说每次改动都让M至少减1，而M永远不会变大（没有任何一条规则会放方块、也不会把false改成true），
 *	  M有下界0 ⇒ 改动次数有限 ⇒ 队列一定会空。链子有多长就多判几轮（比如上下一条悬空藤蔓链），
 *	  但每轮只判6格、而且都在缓存里热着，比"整段重扫一遍"便宜得多。
 *	  顺序还不影响结果：只会拆、只会把面关小，支撑只会越来越少（单调），所以先拆哪个，最后都是同一个不动点。
 *	  所以本程序**只全量扫一遍**（没有"再扫N遍"那种参数）：扫完 + 队列判空 = 已到不动点，输出里会写"已收敛"。
 *
 *	为什么"改一格看6个相邻(+4个斜上方)"就够（以后加规则要照着这个检查）：
 *	  规则读的全是自己那6个面（Rules_Ctx.nb，rules.cpp里nb_Get只走这6个方向），再加一处**斜下方**：
 *	  紫颂植株要读"水平邻居的下面那格"（rules.cpp的nb_Below_Of）——那是(x±1,y-1,z)和(x,y-1,z±1)。
 *	  所以"谁可能被这次改动影响"是两类格子：
 *	    ①6个严格相邻（读面）；
 *	    ②4个"斜上方"：(x±1,y+1,z)、(x,y+1,z±1)——这一格的"水平邻居的下面"正好是刚改的(x,y,z)，
 *	      紫颂植株就是靠这一格判断"旁边那株还有没有根"（少了②会漏：中间那株靠别的方向还站得住、
 *	      自己不用改，于是改动传不到它旁边的植株上，见test/mkfix.cpp里那组"斜上方连锁"用例）。
 *	  以后要是加了读别处的规则（比如再远一格、或者往上的斜角），work_Push 这里必须跟着加，
 *	  不然"扫完+队列判空=不动点"这句话就不成立（会漏判、还要靠再扫一遍才发现）。
 *	  这条对插件也一样：rules.so 换成一个读别处的，就得把这里的work_Push一起换掉。
 *
 *	快在哪（这几条就是这版的重点，改的时候别把这些结构拆了）：
 *	  · 方块按"名字"分类一次就记住（支撑档/支持集/规则号/是不是水…，在rules.so里），扫的时候只比一个字节；
 *	  · 一段section的"调色板下标→规则号"表建一次就留在区块缓存槽里，改完只补新加的那几条，不整段重算；
 *	  · 区块缓存自己拿着Chunk*，按坐标查槽（顺带记住"这个坐标读不出来"，别一遍遍重试——那正是最费时间的地方）；
 *	    正在扫的那个区块会把自己那个槽钉住，读邻居不会把它挤掉，手里的指针就一直有效；
 *	  · 规则要哪个方向的邻居才读哪个（"下面要有支持"就只读下面那格），读不出来才放弃这一格；
 *	  · 改动就地连锁：改一格只把6个严格相邻排进队列补判（不整段重扫），所以一趟到不动点，不用扫第二遍。
 *
 *	编译：make fixer（或者）
 *	      g++ -std=c++23 -O2 -Wall -Wextra fixer.cpp -o fixer -L. -lnbt -lz -ldl -Wl,-rpath,'$ORIGIN'
 *	用法：./fixer <path> <x1> <y1> <z1> <x2> <y2> <z2> <chunk_unit> [dims]
 *	      path      要处理的地图目录（同时也是输出，会被改写）
 *	      x1 y1 z1 x2 y2 z2  方块范围的两个角，谁大谁小都行（内部取min/max），含两头临界值
 *	      chunk_unit=1 以区块为单位（x/z不裁）；=0 裁到上面的范围里
 *	      dims      逗号隔开的维度名，默认 minecraft:overworld，可以给多个（每个维度各扫一遍）
 *	  别的一律不给命令行：区块缓存256槽、区域镜像16个、规则家族全开、每个维度扫一遍就够，
 *	  这些都是fix_Config里的常量；规则插件固定找本程序旁边的 rules.so（rules.so.26.1的符号链接）。
 *
 *	不处理的（宁可放过不可破坏，wiki上找不到"失支撑会自己破"的就不动它）：
 *	          沙子/沙砾/铁砧这些会掉下去的（MC自己会落）、珊瑚缺水变死珊瑚、树叶腐烂、冰融化、
 *	          活板门/栅栏门/花盆/铁链/末地烛/避雷针/床（wiki明确写了它们不需要支撑，能悬空）、
 *	          下界传送门（"必须成结构"要读整个门框，不是6个面能判的）、蘑菇方块（那6个面是贴图不是合法性）。
 *	          （霜冰是个例外，它不判支撑、直接化成水：那是rules.so里的一条规则RULE_FROST，见rules.cpp）
 */
#include "nbt.hpp"
#include "rules.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <errno.h>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

/*==================== 1. 一段section：调色板 + 每个条目的规则 ====================*/
struct Fix_Sec{
	nbt_Compound *states=NULL;/*block_states，NULL=这段没有（＝全是空气）*/
	nbt_List *palette=NULL;   /*states.palette，NULL=direct调色板（读不出来）*/
	nbt_Long_Array *data=NULL;
	uint32_t bits=0,per=0,words=0;
	std::vector<int16_t> rule;/*调色板下标→规则表下标（-1=这段里这个条目不用管，已经按rules=掩码滤过）*/
	std::deque<Blk_Info> info;/*调色板下标→方块信息（用deque：加条目时已有元素的地址不会变）*/
	int cand=0;               /*有几个条目要管（0=这整段跳过）*/
};
static void sec_Bits(Fix_Sec &s){/*位宽/每个long装几个/有几个long：MC按调色板长度取最小位宽，能反推*/
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	s.bits=4;
	while((1u<<s.bits)<len){
		s.bits++;
	}
	s.per=64/s.bits;
	s.words=s.data?(uint32_t)s.data->con.size():0;
}
static void sec_Add(Fix_Sec &s,const Rules_Api *api,uint32_t mask){
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	if(s.info.size()>(size_t)len){/*理论不会缩，保险*/
		s.info.resize(len);
		s.rule.resize(len);
	}
	size_t old=s.info.size();
	if(old>=(size_t)len){
		return;/*没有新条目（改完属性写回去时常见：palette里已经有同一个状态了）*/
	}
	s.info.resize(len);
	s.rule.resize(len);
	for(size_t i=old;i<(size_t)len;i++){
		api->blk_Fill(&s.info[i],s.palette->con[i]);/*分类（名字池在插件里，建一次就一直有效）*/
		s.rule[i]=(int16_t)api->blk_Rule(&s.info[i],mask);/*这条要管吗（-1=不管：mod方块/明确不管的/被rules=关掉的家族）*/
		if(s.rule[i]>=0){
			s.cand++;
		}
	}
}
static void sec_Build(Fix_Sec &s,nbt_Compound *sec,Chunk_Cursor *cur,const Rules_Api *api,uint32_t mask){
	s.states=cur->states_Of(sec);
	s.palette=s.states?cur->palette:NULL;
	s.data=NULL;
	s.bits=0;
	s.per=0;
	s.words=0;
	s.info.clear();
	s.rule.clear();
	s.cand=0;
	if(!s.states){
		return;/*段在但没有block_states（纯光段）＝空气*/
	}
	nbt_Any *d=s.states->get("data");
	s.data=d&&d->type==0x0c?(nbt_Long_Array*)d:NULL;
	if(s.palette&&s.palette->con.size()>256){/*direct调色板：这一段整个读不出来（也不能当空气，会连累邻居）*/
		s.palette=NULL;
		s.data=NULL;
		return;
	}
	sec_Bits(s);
	sec_Add(s,api,mask);
}
static void sec_Refresh(Fix_Sec &s,nbt_Compound *sec,Chunk_Cursor *cur,const Rules_Api *api,uint32_t mask){/*刚写过这一格：调色板和data可能变了*/
	s.states=cur->states_Of(sec);
	s.palette=s.states?cur->palette:NULL;
	nbt_Any *d=s.states?s.states->get("data"):NULL;
	s.data=d&&d->type==0x0c?(nbt_Long_Array*)d:NULL;
	sec_Bits(s);
	sec_Add(s,api,mask);/*只补新加的条目：老条目的规则号没变*/
}
#define IDX_BAD 0xFFFFFFFFu/*"这一段读不出来"：data比该有的短、或者调色板是direct那种，别猜*/
static inline uint32_t sec_Idx(const Fix_Sec &s,uint32_t i){/*i=ly*256+lz*16+lx → 调色板下标；IDX_BAD=读不出来*/
	uint32_t len=s.palette?(uint32_t)s.palette->con.size():0;
	if(!s.data||!s.per){
		return len<=1?0:IDX_BAD;/*没有data：只有一条调色板时整段都是palette[0]，多条就是存档不对*/
	}
	uint32_t w=i/s.per;
	if(w>=s.words){
		return IDX_BAD;/*data比该有的短（存档不该这样）*/
	}
	uint32_t idx=(uint32_t)(s.data->con[w]>>((i%s.per)*s.bits))&((1u<<s.bits)-1);
	if(idx>=len){/*下标比调色板还大（调色板被改小、data里留着旧值）：这是坏数据，不是"这格不用管"——
	                以前这里原样返回，初扫当成"没规则"静默跳过，现在和"data缺/短"一样计 pos_bad、进退出码；
	                被当邻居读的时候本来就算读不出来（blind）*/
		return IDX_BAD;
	}
	return idx;
}

static Blk_Info info_Air_Make(){/*"没有这一段"的占位：basename空、空气、没有规则*/
	Blk_Info b;
	b.flags=BF_AIR;
	return b;
}
static const Blk_Info info_Air=info_Air_Make();

/*==================== 2. 区块缓存（坐标对号入座，绝不留两份；正在扫的钉住） ====================*/
struct Fix_Cache{
	level *lv;
	std::string world;
	const Rules_Api *api;
	uint32_t rules_mask;
	struct Slot{
		int32_t cx=INT32_MIN,cz=INT32_MIN;/*INT32_MIN=空槽*/
		uint64_t use=0;
		Chunk *chunk=NULL;/*NULL=这个坐标读不出来（记着别再试）；坐标是空槽就另说*/
		std::unordered_map<nbt_Compound*,Fix_Sec> secs;/*这一区块里建过的段表*/
		nbt_Compound *last_sec=NULL;/*上一个用的段（连着读同一段就不打表了）*/
		Fix_Sec *last_info=NULL;
	};
	std::vector<Slot> slot;
	uint64_t use_cnt=0;
	int last=0;
	int pin=-1;/*正在扫的那个槽：不许被换掉，不然手里的Fix_Sec/Blk_Info指针就悬了*/
	int64_t loads=0;/*真读到区块的次数（看缓存好不好用）*/
	int64_t gone=0; /*读不到、记下来不再重试的坐标数（区块没生成）*/
	Fix_Cache(level *lv,const char *world,int n,const Rules_Api *api,uint32_t mask):lv(lv),world(world?world:"minecraft:overworld"),
		api(api),rules_mask(mask){
		slot.resize((size_t)(n>0?n:1));
	}
	enum{FOUND=1,GONE=-2};/*GONE=记住这个坐标读不出来*/
	int find(int32_t cx,int32_t cz,int *out){
		Slot &sl=slot[last];
		if(sl.cx==cx&&sl.cz==cz&&sl.cx!=INT32_MIN){
			*out=last;
			return sl.chunk?FOUND:GONE;
		}
		for(size_t i=0;i<slot.size();i++){
			if(slot[i].cx==cx&&slot[i].cz==cz&&slot[i].cx!=INT32_MIN){
				last=(int)i;
				*out=(int)i;
				return slot[i].chunk?FOUND:GONE;
			}
		}
		return 0;
	}
	int load(int32_t cx,int32_t cz){/*没有就换掉最久没用过的那个槽（level会把旧的写回区域镜像）*/
		size_t old=slot.size();
		for(size_t i=0;i<slot.size();i++){
			if((int)i==pin){
				continue;/*正在扫的不能换*/
			}
			if(old==slot.size()||slot[i].use<slot[old].use){
				old=i;
			}
		}
		if(old>=slot.size()){
			return -1;/*所有槽都被钉住了（cache_cnt太小）：当读不出来*/
		}
		Chunk *c=lv->get_Chunk(cx,cz,world.c_str(),(int)old);
		slot[old].cx=cx;
		slot[old].cz=cz;
		slot[old].use=++use_cnt;
		slot[old].chunk=c;
		slot[old].secs.clear();/*换了区块：段表全作废（里面的指针都指在旧树上）*/
		slot[old].last_sec=NULL;
		slot[old].last_info=NULL;
		last=(int)old;
		if(c){
			loads++;
		}else{
			gone++;
		}
		return c?(int)old:-1;
	}
	int get(int32_t cx,int32_t cz){
		int k=-1;
		int st=find(cx,cz,&k);
		if(st==FOUND){
			slot[k].use=++use_cnt;
			return k;
		}
		if(st==GONE){
			return -1;/*以前读过就没有，别再费劲了*/
		}
		return load(cx,cz);
	}
	Chunk *chunk_At(int32_t cx,int32_t cz,int *slot_out){/*取区块扫（顺便把它钉住）*/
		int k=get(cx,cz);
		pin=k;
		*slot_out=k;
		return k>=0?slot[k].chunk:NULL;
	}
	void unpin(){pin=-1;}
	Fix_Sec *sec_Of(int k,nbt_Compound *sec){/*这一区块的这一段：建一次就留着*/
		Slot &sl=slot[k];
		if(sl.last_sec==sec){
			return sl.last_info;
		}
		std::unordered_map<nbt_Compound*,Fix_Sec>::iterator it=sl.secs.find(sec);
		if(it==sl.secs.end()){
			Fix_Sec si;
			sec_Build(si,sec,lv->cursor(k),api,rules_mask);
			it=sl.secs.emplace(sec,std::move(si)).first;
		}
		sl.last_sec=sec;
		sl.last_info=&it->second;
		return sl.last_info;
	}
	int read(int x,int y,int z,const Blk_Info **out){/*读一个位置（插件的读邻居回调也是走这里）：
			1=读到了（*out=那个方块的信息，空气也有信息）；0=读不出来（区块没生成/direct调色板/下标越界）*/
		*out=NULL;
		int k=get(x>>4,z>>4);
		if(k<0){
			return 0;
		}
		int sy=section_Y(y);
		nbt_Compound *sec=lv->cursor(k)->find_Section(slot[k].chunk,sy);
		if(!sec){
			*out=&info_Air;/*这一段压根没有＝空气*/
			return 1;
		}
		Fix_Sec *s=sec_Of(k,sec);
		if(!s->states){
			*out=&info_Air;/*段在但没有block_states＝空气*/
			return 1;
		}
		if(!s->palette||s->palette->con.empty()||s->palette->con.size()>256){
			return 0;/*direct调色板（>256条，里面存的是全局id不是方块状态）：读不出来，别猜*/
		}
		uint32_t i=(uint32_t)(y-sy*16)*256+(uint32_t)(z&15)*16+(uint32_t)(x&15);
		uint32_t idx=sec_Idx(*s,i);
		if(idx==IDX_BAD||idx>=s->info.size()){
			return 0;
		}
		*out=&s->info[idx];
		return 1;
	}
	void sec_Wrote(Fix_Sec &s,nbt_Compound *sec,int k){/*刚用set_Block写过这一格*/
		sec_Refresh(s,sec,lv->cursor(k),api,rules_mask);
	}
	int set_Block(int x,int y,int z,nbt_Any *block,nbt_Any *entity){
		int k=get(x>>4,z>>4);
		if(k<0){
			return 0;
		}
		Block b={block,entity};
		if(!slot[k].chunk->set_Block(b,x&15,y,z&15,lv->cursor(k))){
			return 0;
		}
		lv->mark_Changed(x>>4,z>>4,world.c_str(),k,1);/*走Chunk::set_Block写的，游标没坏，别清*/
		return 1;
	}
	nbt_Any *get_Entity(int x,int y,int z){/*要保留方块实体时才来查（绝大部分改动不需要）*/
		int k=get(x>>4,z>>4);
		if(k<0){
			return NULL;
		}
		return slot[k].chunk->get_Block(x&15,y,z&15,lv->cursor(k)).entity;
	}
};

/*==================== 3. 就地连锁队列 + 统计 ====================*/
/*改一格就往"相邻（含4个斜上方）"里递归（和fixer.py一样）：谁可能被这次改动影响，就把它排进队列再判一遍。
  队列里只存坐标：判一格要读邻居，读邻居可能把别的区块挤出缓存、连带把区块树和调色板表都释放掉，
  这时候存 Chunk*、Fix_Sec*、Blk_Info* 全成野指针。判之前把这一格的区块钉住，判完松开。
  允许重复（同一格被几个改动一起影响就会进队几次）：不做去重，省得为了去重再引一张表；重复只是多判几次，不影响结果。
  死循环不用担心：每次改动都让文件头那个 M 至少减1，M 有下界0，所以改动次数有限、队列一定会空。
  到底排哪些格：见 work_Push（6个面 + 4个斜上方）和本文件开头那段——"排哪些"必须覆盖"规则会读哪些格"，
  现在规则只读6个面 + 紫颂植株的"水平邻居的下面"（斜下方），所以这10个是精确的、不用多排。*/
struct Fix_Work{
	std::vector<int32_t> st;/*每3个一组：x,y,z。用栈（后进先出）=DFS：顺着一条链一路判到底，缓存更热、栈也浅*/
	int64_t judged=0,changed=0;/*判过多少个位置、其中又改出多少个（下面统计要打）*/
	void push(int32_t x,int32_t y,int32_t z){
		st.push_back(x);
		st.push_back(y);
		st.push_back(z);
	}
	int empty()const{
		return st.empty();
	}
	void pop(int32_t *x,int32_t *y,int32_t *z){
		*z=st.back();
		st.pop_back();
		*y=st.back();
		st.pop_back();
		*x=st.back();
		st.pop_back();
	}
};
struct Fix_Stat{
	int64_t chunks=0,sec_all=0,sec_hit=0,pos=0,hit=0,air=0,wet=0,set=0,miss=0,blind=0,noop=0,warp=0;
	int64_t dup_y=0;   /*同一区块里两段Y相同（坏数据）：整块跳过没动*/
	int64_t sec_direct=0;/*有block_states但读不出调色板的段（direct/>256条/成员类型不对）：只报数不改*/
	int64_t pos_bad=0; /*调色板有、data缺/短的格子（坏数据）：跳过了，进退出码*/
	std::vector<int64_t> rule_cnt,rule_hit;/*按规则分类（大小=插件的规则条数；下标就是插件给的规则号）*/
	/*每条规则改动的方块名（最多留前若干种，够看出"这条规则到底在拆什么"）：report里打出来，
	  规则表收错方块/漏掉一条支撑关系时一眼能看见（大垂滴叶那次就是靠这个抓出来的）*/
	std::vector<std::unordered_map<std::string,int64_t>> rule_blk;
	void init(int n){
		rule_cnt.assign((size_t)(n>0?n:1),0);
		rule_hit.assign((size_t)(n>0?n:1),0);
		rule_blk.resize((size_t)(n>0?n:1));
	}
	void blk_Hit(int ru,std::string_view base){/*按规则记一笔"改了什么方块"*/
		if(ru<0||(size_t)ru>=rule_blk.size()||base.empty()){
			return;
		}
		rule_blk[ru][std::string(base)]++;
	}
};
struct Poi_Del{int32_t x,y,z;};/*要删记录的那一格（世界坐标）*/
/*把拆掉的POI方块的记录删掉：记录必须和方块对得上，留着就是"指向空气的索引"（村民会去找一个不存在的
  工作站/床）。票不用管：方块都没了，这条记录直接删（不是标脏让游戏重算，理由见ReadMeDeveloper）。
  按区域分组：一个区域读一次POI镜像，删完空的区块就删区块，整个文件空了就删文件*/
static int64_t poi_Del_Apply(const std::string &dir,std::vector<Poi_Del> &dels,int64_t *chunk_del,int64_t *file_del){
	if(dels.empty()){
		return 0;
	}
	std::sort(dels.begin(),dels.end(),[](const Poi_Del &a,const Poi_Del &b){
		if((a.x>>9)!=(b.x>>9)){
			return (a.x>>9)<(b.x>>9);
		}
		return (a.z>>9)<(b.z>>9);
	});
	int64_t done=0;
	size_t i=0;
	while(i<dels.size()){
		int32_t rx=dels[i].x>>9,rz=dels[i].z>>9;
		mca_Region mca=mca_Region::read(dir.c_str(),rx,rz);
		if(mca.bad){/*这个POI文件读不出来：方块已经拆了、记录删不掉，必须报出来（不然村民会指着空气认领）*/
			printf("fixer: POI区域文件 r.%d.%d 读不出来，这个区域里拆掉的POI方块没能删记录（文件一个字节都没动）\n",
				(int)rx,(int)rz);
			return -1;
		}
		int changed=0;
		size_t j=i;
		for(;j<dels.size()&&(dels[j].x>>9)==rx&&(dels[j].z>>9)==rz;j++){
			/*表头里的槽位是"区块在区域里的坐标"（0~31），不是方块坐标：世界坐标要先 >>4 变成区块坐标*/
			uint32_t lx=(uint32_t)((dels[j].x>>4)&31),lz=(uint32_t)((dels[j].z>>4)&31);
			if(!mca.has_Chunk(lx,lz)){/*这一块本来就没有POI数据*/
				continue;
			}
			nbt_Compound *pc=mca.read_Compound(lx,lz);
			if(!pc){
				continue;/*读不出来：跳过（已经计过数）*/
			}
			int del=poi_Del(pc,dels[j].x,dels[j].y,dels[j].z);
			if(del){
				done+=del;
				changed=1;
				if(poi_Count(pc)==0){/*这一块一条记录都不剩：删区块（内存镜像也算改了，要落盘）*/
					if(mca.erase_Chunk(lx,lz)){
						(*chunk_del)++;
					}
				}else if(!mca.write_ChunkAt(pc,lx,lz)){
					/*树是改过了，可镜像里还是老字节：得把这一块重新编码写回镜像，
					  不然write_File落盘的还是"删之前"的那份（这个坑踩过一次）*/
					printf("fixer: POI区块(%d,%d)写不回区域镜像\n",(int)lx,(int)lz);
					pc->free();
					return -1;
				}
			}
			pc->free();
		}
		if(changed){
			char file[4096];
			mca_Path(file,sizeof(file),dir.c_str(),rx,rz);
			if(mca.is_Empty()){/*整个文件空了：删文件*/
				if(!remove(file)){
					(*file_del)++;
				}else{
					/*删不掉（权限/只读/别人的文件）：以前当没发生，文件留在那儿、报告一个字不提，
					  下次还得再来一遍。报出来并让退出码非0*/
					printf("fixer: POI空文件删不掉 %s（%s）\n",file,strerror(errno));
					return -1;
				}
			}else if(!mca.write_File(file)){
				return -1;
			}
		}
		i=j;
	}
	return done;
}
struct Fix_Job{
	level *lv;
	std::string world;
	const Rules_Api *api;
	Fix_Cache cache;
	int32_t x0,x1,y0,y1,z0,z1;
	int chunk_unit;
	int64_t samples;/*-1=全打（verbose=1），0=不打*/
	Fix_Stat st;
	Fix_Work work;/*就地连锁的待判队列*/
	std::vector<Poi_Del> poi_del;/*这一维度里拆掉的POI方块（床/工作站/钟/传送门…），最后统一把它们的记录删掉*/
	int64_t sample_cnt=0;
	Fix_Job(level *lv,const char *world,int cache_cnt,const Rules_Api *api,uint32_t mask):lv(lv),world(world?world:""),
		api(api),cache(lv,world,cache_cnt,api,mask),x0(0),x1(0),y0(0),y1(0),z0(0),z1(0),chunk_unit(1),samples(0){
		st.init(api->rule_Count());
	}
	int in_Range(int32_t x,int32_t y,int32_t z)const{/*这一格在不在"会去改"的范围里（和初扫的范围一致）*/
		if(y<y0||y>y1){/*y 一律按参数裁*/
			return 0;
		}
		if(chunk_unit){/*整区块处理：x/z 按参数对齐到区块边界，跟 scan_Chunk 一样*/
			return x>=((x0>>4)<<4)&&x<=((((x1>>4)+1)<<4)-1)&&z>=((z0>>4)<<4)&&z<=((((z1>>4)+1)<<4)-1);
		}
		return x>=x0&&x<=x1&&z>=z0&&z<=z1;
	}
};

/*==================== 4. 判一个位置 ====================*/
static int nb_Read_Cb(void *ud,int x,int y,int z,const Blk_Info **out){/*给规则插件的读邻居回调*/
	return ((Fix_Job*)ud)->cache.read(x,y,z,out);
}
static std::string nb_Show(Fix_Job &j,int x,int y,int z){/*样例里打邻居用：读不出来打?，空气打"空气"*/
	const Blk_Info *b=NULL;
	if(!j.cache.read(x,y,z,&b)){
		return "?";
	}
	if(blk_Air(b)){
		return "空气";
	}
	return std::string(b->base);
}
/*刚改了(x,y,z)：把"可能被这次改动影响"的格子排上队（和fixer.py一样：改完 -> 相邻的各再判一遍）。
  两类：①6个严格相邻（规则读面）；②4个"斜上方"(x±1,y+1,z)/(x,y+1,z±1)——紫颂植株读"水平邻居的下面"，
  刚改的这格正好可能被它们当成"旁边那株的根"（见本文件开头那段说明）。
  允许重复（不做去重）：同一格被几个改动一起影响就会进队几次，多判几次而已，不影响结果，也省掉一张去重表。
  范围外的直接不排：范围是"只改这些方块"的约定，连锁不往范围外扩（fixer.py是不管的，这里按范围收着）。*/
static void work_Push(Fix_Job &j,int32_t x,int32_t y,int32_t z){
	static const int32_t off[10][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
		{1,1,0},{-1,1,0},{0,1,1},{0,1,-1}};
	for(int i=0;i<10;i++){
		int32_t nx=x+off[i][0],ny=y+off[i][1],nz=z+off[i][2];
		if(j.in_Range(nx,ny,nz)){
			j.work.push(nx,ny,nz);
		}
	}
}
static std::string prop_Brief(nbt_Any *b){/*把几个关键属性拼成 "(half=lower,facing=north)" 这样，样例里用*/
	static const char *keys[]={"half","part","facing","face","type","axis","attachment","attached","hanging",
		"vertical_direction","thickness","waterlogged","open","layers","distance","age"};
	static const char *show[]={"facing","half","type","axis","attachment","vertical_direction","thickness"};
	std::string out;
	for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
		const char *v=get_Prop(b,keys[i]);
		if(!v){/*这个属性没有*/
			continue;
		}
		int ok=!strcmp(v,"true")||!strcmp(v,"false");/*true/false总是打*/
		for(size_t k=0;!ok&&k<sizeof(show)/sizeof(show[0]);k++){
			ok=!strcmp(keys[i],show[k]);
		}
		if(!ok){
			continue;
		}
		out+=out.empty()?" (":",";
		out+=keys[i];
		out+="=";
		out+=v;
	}
	if(!out.empty()){
		out+=")";
	}
	return out;
}
static nbt_Compound *air_State(){/*拆方块时写的那个空气（引擎里只读，不释放；程序退出时留着不管）*/
	static nbt_Compound *a=NULL;
	if(!a){
		a=new nbt_Compound;
		a->set_String("Name","minecraft:air");
	}
	return a;
}
static nbt_Compound *water_State(){/*拆"含水方块"时写的水：waterlogged=true的方块，和海草/海带/珊瑚这种泡在水里的。
	地图里的水是一整片（level=0的源），MC把含水方块打掉也是留水；要是写空气，水底就多个空气洞，
	而且旁边靠这格水活着的东西（海草、青蛙卵之类）会白丢水源。属性写成level=0，和地图里别的水一模一样*/
	static nbt_Compound *w=NULL;
	if(!w){
		w=new nbt_Compound;
		w->set_String("Name","minecraft:water");
		nbt_Compound *p=new nbt_Compound;
		p->set_String("level","0");
		w->set("Properties",p);
	}
	return w;
}
static int fix_At(Fix_Job &j,Fix_Sec &s,int slot,nbt_Compound *sec,int x,int y,int z,int di,uint32_t idx){
	const Blk_Info *self=&s.info[idx];
	int ru=j.api->def_Rule(di);/*哪条规则（报告要按规则分类）*/
	j.st.hit++;
	if(ru>=0&&ru<(int)j.st.rule_cnt.size()){
		j.st.rule_cnt[ru]++;
	}
	Rules_Ctx c;
	c.self=self;
	c.x=x;
	c.y=y;
	c.z=z;
	c.read=nb_Read_Cb;/*插件要哪个面就读哪个面*/
	c.ud=&j;
	int act=j.api->judge(self,di,&c);
	if(c.blind){/*有邻居读不出来：结论不算（宁可漏拆）。规则可能已经造了个新状态（藤蔓关面那种），得自己释放*/
		j.st.blind++;
		if(c.out_block){
			c.out_block->free();
		}
		return 0;
	}
	if(act==FX_KEEP){
		if(c.out_block){/*插件"造了状态又说不用改"：那也是它new出来的，得我们释放*/
			c.out_block->free();
		}
		return 0;
	}
	if(j.samples<0||j.sample_cnt<j.samples){/*打样例：先把自己那几个串拼好，再读邻居（读邻居可能换缓存）*/
		std::string me=std::string(self->base)+prop_Brief(self->blk);
		printf("  样例：%s (%d,%d,%d) 规则=%s 下=%s 上=%s 东=%s 西=%s 南=%s 北=%s\n",
			me.c_str(),x,y,z,j.api->rule_Name(ru),nb_Show(j,x,y-1,z).c_str(),nb_Show(j,x,y+1,z).c_str(),
			nb_Show(j,x+1,y,z).c_str(),nb_Show(j,x-1,y,z).c_str(),nb_Show(j,x,y,z+1).c_str(),
			nb_Show(j,x,y,z-1).c_str());
		j.sample_cnt++;
	}
	if(act==FX_AIR){
		if(self->flags&BF_AIR){/*兜底：规则表把一个本来就是空气的位置也收进来了。真要写下去就是"空气拆空气"，
		                          改了等于没改、下一遍还会再来，转不完；所以这里不算改动，只记一笔报警*/
			j.st.noop++;
			return 0;
		}
		int wet=(self->flags&BF_WATER)!=0;/*原来是含水物：拆完要留水（见water_State的说明）*/
		nbt_Any *target=wet?water_State():air_State();
		if(wet&&self->blk&&target->cmp(self->blk)){/*"水拆成水"：写下去等于没改、队列会转不完，
		              破坏文件头那条 M 至少减1的收敛证明。和下面FX_SET那条一样当"不动"处理
		              （插件把water本身收进规则表才会走到；现有rules.so跑不到：water不在defs里）*/
			j.st.noop++;
			return 0;
		}
		/*拆之前先看它是不是POI方块（床/工作站/钟/传送门…）：拆成功了要把那一格的POI记录删掉。
		  必须在这之前取，set_Block可能重排调色板，self->blk就不能再用了*/
		int is_poi=poi_Type_Of_Block(self->blk)!=NULL;
		if(j.cache.set_Block(x,y,z,target,NULL)){
			if(is_poi){
				Poi_Del d;
				d.x=x;d.y=y;d.z=z;
				j.poi_del.push_back(d);
			}
			j.st.air++;
			if(wet){
				j.st.wet++;
			}
			if(ru>=0&&ru<(int)j.st.rule_hit.size()){
				j.st.rule_hit[ru]++;
			}
			j.st.blk_Hit(ru,self->base);/*记一笔"这条规则拆的是什么方块"（报告里打，方便核对规则表）*/
			j.cache.sec_Wrote(s,sec,slot);/*这一段可能被重排，重新看一眼再接着扫*/
			work_Push(j,x,y,z);/*改了它：6个严格相邻各再判一遍（连锁就在这儿就地做完）*/
			return 1;
		}
		j.st.miss++;
		return 0;
	}
	if(act!=FX_SET){/*插件返回了不认识的动作：接口对不上。当"不动"处理（绝不能顺手当FX_SET往下走，
	                  那会把没初始化的out_block写进存档），它造出来的状态也要释放掉，别漏内存*/
		if(c.out_block){
			c.out_block->free();
		}
		static int warned=0;
		if(!warned){
			warned=1;
			printf("fixer: 规则插件返回了不认识的动作%d，按'不动'处理（插件和rules.hpp对不上？）\n",act);
		}
		return 0;
	}
	if(!c.out_block){/*说FX_SET却没给新状态：也当"不动"*/
		return 0;
	}
	if(self->blk&&c.out_block->cmp(self->blk)){/*给的新状态和现在一模一样：写下去等于没改，还会破坏
	              "每次改动都让 M 至少减1"这个前提（队列可能转不完）。当"不动"处理——这是插件或规则表
	              写错时的兜底，正常情况下走不到（rules.cpp只在真有面要关掉时才返回FX_SET）*/
		j.st.noop++;
		c.out_block->free();
		return 0;
	}
	/*改属性：保留方块实体；新状态是插件现造的，写完要自己释放*/
	nbt_Any *keep=c.out_keep_be?j.cache.get_Entity(x,y,z):NULL;/*写之前先拿：写完palette可能重排，self不能再用*/
	int ok=j.cache.set_Block(x,y,z,c.out_block,keep);
	if(c.out_block){
		c.out_block->free();
	}
	if(!ok){
		j.st.miss++;
		return 0;
	}
	j.st.set++;
	if(ru>=0&&ru<(int)j.st.rule_hit.size()){
		j.st.rule_hit[ru]++;
	}
	j.st.blk_Hit(ru,self->base);/*记一笔"这条规则改的是什么方块"*/
	j.cache.sec_Wrote(s,sec,slot);
	work_Push(j,x,y,z);/*改了它：6个严格相邻各再判一遍*/
	return 1;
}
static int chunk_Warp(const Chunk *c,int32_t cx,int32_t cz){/*这个区块的xPos/zPos和文件位置对不上？
	（读按xPos算世界坐标、写按x>>4找区块，两套坐标会打架：方块实体会搬到错位置、同一个区块在缓存里
	  出现两份、落盘还会互相覆盖。用as_Int之前先认类型：不是Int就当没有这个成员，
	  as_Int对别的类型返回0，别让它和真坐标0混起来）*/
	const nbt_Any *xp=c->get("xPos");
	const nbt_Any *zp=c->get("zPos");
	return (xp&&xp->type==0x03&&xp->as_Int()!=cx)||(zp&&zp->type==0x03&&zp->as_Int()!=cz);
}
static int fix_Pos(Fix_Job &j,int32_t x,int32_t y,int32_t z){/*按坐标判一格（连锁用；初扫走scan_Chunk那条快路）*/
	if(!j.in_Range(x,y,z)){
		return 0;
	}
	int slot=-1;
	Chunk *chunk=j.cache.chunk_At(x>>4,z>>4,&slot);/*钉住它，免得判到一半被邻居读挤掉*/
	if(!chunk){
		j.cache.unpin();
		return 0;
	}
	/*xPos/zPos对不上的区块整块不动：scan_Chunk里有这道检查，连锁这条路以前漏了——
	  于是报告说"整块跳过了"，实际上它被邻居的连锁改动拆了（warp那个计数只保证报告里有一句话）*/
	if(chunk_Warp(chunk,x>>4,z>>4)){
		j.cache.unpin();
		return 0;
	}
	int32_t sy=section_Y(y);
	nbt_Compound *sec=j.lv->cursor(slot)->find_Section(chunk,sy);
	Fix_Sec *s=sec?j.cache.sec_Of(slot,sec):NULL;
	if(!s||!s->cand){/*没有这一段/这段没要管的方块*/
		j.cache.unpin();
		return 0;
	}
	uint32_t i=(uint32_t)(y-sy*16)*256+(uint32_t)(z&15)*16+(uint32_t)(x&15);
	uint32_t idx=sec_Idx(*s,i);
	if(idx==IDX_BAD){/*data缺/短：坏数据，计数（初扫那条路也一样）*/
		j.st.pos_bad++;
		j.cache.unpin();
		return 0;
	}
	if(idx>=s->rule.size()||s->rule[idx]<0){/*这一格不用管*/
		j.cache.unpin();
		return 0;
	}
	int ch=fix_At(j,*s,slot,sec,x,y,z,s->rule[idx],idx);
	j.cache.unpin();
	return ch;
}
static int64_t work_Drain(Fix_Job &j){/*把队列判到空：每判出一处改动，它的6个相邻又被排上队*/
	int64_t changed=0;
	while(!j.work.empty()){
		int32_t x=0,y=0,z=0;
		j.work.pop(&x,&y,&z);
		j.work.judged++;
		if(fix_Pos(j,x,y,z)){
			changed++;
			j.work.changed++;
		}
	}
	return changed;
}

/*==================== 5. 扫一个区块 ====================*/
/*初扫：把这个区块里所有"要管的"格子判一遍。改了一格以后不用重扫整段，连锁由 work_Push/work_Drain
  按坐标就地补判——谁受影响是精确知道的，不用整段重扫，也不用再扫第二遍。*/
static int64_t scan_Chunk(Fix_Job &j,int32_t cx,int32_t cz){/*扫这个区块该管的格子；改动就地连锁（见work_Push）*/
	int slot=-1;
	Chunk *chunk=j.cache.chunk_At(cx,cz,&slot);/*扫的时候这个槽被钉住，读邻居挤不掉它*/
	if(!chunk){
		j.cache.unpin();
		return 0;
	}
	j.st.chunks++;
	nbt_List *secs=(nbt_List*)chunk->get("sections");
	if(!secs||secs->type!=0x09){
		j.cache.unpin();
		return 0;
	}
	if(chunk_Warp(chunk,cx,cz)){
		j.st.warp++;
		j.cache.unpin();
		return 0;
	}
	int32_t wx=cx*16;/*区块原点就用文件位置算，和set_Block找区块的方式一致*/
	int32_t wz=cz*16;
	int32_t lx0=0,lx1=15,lz0=0,lz1=15;/*不整区块处理时把x/z裁到方块范围里*/
	if(!j.chunk_unit){
		lx0=std::max(0,j.x0-wx);
		lx1=std::min(15,j.x1-wx);
		lz0=std::max(0,j.z0-wz);
		lz1=std::min(15,j.z1-wz);
		if(lx0>lx1||lz0>lz1){
			j.cache.unpin();
			return 0;
		}
	}
	int32_t sy0=section_Y(j.y0),sy1=section_Y(j.y1);
	int64_t changed=0;
	/*同一个区块里出现两段Y相同的section＝坏数据。必须整块跳过：初扫是按"段"建表逐段判的，
	  可写回走的是 Chunk::set_Block→find_Section，它只认**第一个**Y匹配的段——
	  会变成"判的是B段、改的是A段"：删掉一个根本没判过的方块（可能是石头/箱子），
	  而真正悬空的那个还留着，报告还记成一次成功的改动*/
	{
		uint8_t seen[256]={0};
		for(size_t k=0;k<secs->con.size();k++){
			if(secs->con[k]->type!=0x0a){
				continue;
			}
			const nbt_Any *yv=((nbt_Compound*)secs->con[k])->get("Y");
			if(!yv){
				continue;
			}
			int syi=(int)(int8_t)yv->as_Int()&0xff;
			if(seen[syi]){
				j.st.dup_y++;
				j.cache.unpin();
				return 0;
			}
			seen[syi]=1;
		}
	}
	for(size_t k=0;k<secs->con.size();k++){
		if(secs->con[k]->type!=0x0a){/*sections里的元素本该都是compound：不是就当没这段，别硬转*/
			continue;
		}
		nbt_Compound *sec=(nbt_Compound*)secs->con[k];
		nbt_Any *yv=sec->get("Y");
		if(!yv){
			continue;
		}
		int32_t sy=(int32_t)(int8_t)yv->as_Int();
		if(sy<sy0||sy>sy1){
			continue;
		}
		j.st.sec_all++;
		Fix_Sec *s=j.cache.sec_Of(slot,sec);
		if(!s->cand){
			/*这里以前是静默continue：整段读不出来（direct调色板/没有palette）和"这段里没有要管的方块"
			  混成一个数，报告只写"扫了N段（有要管的M段）"，看不出有段根本没判过*/
			if(s->states&&!s->palette){
				j.st.sec_direct++;/*有block_states但读不出调色板：direct（>256条）或者成员类型不对，只报数*/
			}
			continue;
		}
		j.st.sec_hit++;
		int32_t ly0=std::max(0,j.y0-sy*16),ly1=std::min(15,j.y1-sy*16);
		for(int32_t lx=lx0;lx<=lx1;lx++){
			for(int32_t lz=lz0;lz<=lz1;lz++){
				for(int32_t ly=ly0;ly<=ly1;ly++){
					uint32_t i=(uint32_t)ly*256+(uint32_t)lz*16+(uint32_t)lx;
					uint32_t idx=sec_Idx(*s,i);
					if(idx==IDX_BAD){/*调色板有、data却缺/短：这是坏数据（不是"不用管"），要计数并让退出码非0*/
						j.st.pos_bad++;
						continue;
					}
					if(idx>=s->rule.size()||s->rule[idx]<0){
						continue;/*这一格不用管*/
					}
					j.st.pos++;
					if(fix_At(j,*s,slot,sec,wx+lx,sy*16+ly,wz+lz,s->rule[idx],idx)){
						changed++;
					}
				}
			}
		}
	}
	j.cache.unpin();
	return changed;
}

/*==================== 6. 跑整个范围 ====================*/
/*命令行的范围参数合理性上限：一次最多去碰多少个区域文件（见下面 fix_World 里的说明，防手滑多打几个0）*/
#define FIX_REGION_MAX (1<<22)
/*y方向的合理范围：MC自定义维度的 min_y 最低-2032、height 最高4064（wiki: Java Edition level format /
  dimension type），也就是世界最高到 y=2031。给到±4096足够，再往外就是坐标写错了：
  不卡这道的话，y范围多打几个0会让 (sy+1)*16、(y0-15)/16 这些算法整数溢出，还会白扫上亿层*/
#define FIX_Y_MAX 4096
/*x/z方向的合理范围：世界边界是±29999984，这里放到±3亿（比边界宽10倍）。
  卡这道是防坐标溢出（UB）和手滑多打几个0：cx*16、((x1>>4)+1)<<4 这类算式只有|x|超过2^27才可能溢出*/
#define FIX_XZ_MAX 300000000
struct fix_Config{
	std::string path;      /*要处理的地图（同时也是输出）*/
	int32_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;/*方块范围的两个角，谁大谁小都行（内部取min/max），含两头临界值*/
	std::vector<std::string> dims;/*要处理的维度（默认主世界）*/
	int chunk_unit=1;      /*整区块处理，不按方块范围裁x/z*/
	/*下面这些不给命令行，都在程序里定死（改要重编）*/
	int cache_cnt=256;     /*区块缓存槽数：判一格最多碰自己和6个面=7个区块，所以别小于16（见Fix_Cache的说明）；
	                         越大读盘越少（每个区块要解压+建NBT树，挺贵），256往上就不怎么涨了*/
	int region_cnt=16;     /*区域镜像最多留几个（一个8MB）*/
	int progress=1;        /*终端上画进度条，非终端每个区域打一行统计*/
	int verbose=0;         /*1=每个改动都打一行（默认只打统计和最前面的几个样例）*/
	int samples=8;         /*打前几个改动的样例，便于肉眼核对规则*/
	uint32_t rules_mask=0xFFFFFFFFu;/*用哪几个规则家族（位是插件给的）；全开*/
};
static double secs_Now(std::chrono::steady_clock::time_point t){
	return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();
}
static int dir_OK(const std::string &path){
	struct stat st;
	return !stat(path.c_str(),&st)&&S_ISDIR(st.st_mode);
}
static void print_Bar(int64_t done,int64_t total,double secs){
	if(!isatty(1)){
		return;
	}
	int w=30,fill=total?(int)(done*w/total):w;
	printf("\r%6.2f%% [",total?done*100.0/total:100.0);
	for(int i=0;i<w;i++){
		putchar(i<fill?'#':'-');
	}
	printf("] %lld/%lld chunk(s), %.1fs   ",(long long)done,(long long)total,secs);
	fflush(stdout);
}
/*一个维度扫一遍（外层fix_World按维度循环，就是调它）；返回改了多少个，出错返回-1*/
static int fix_Dim(const fix_Config &cfg,const Rules_Api *api,level &lv,int dim,int cache_cnt,
		int32_t x0,int32_t x1,int32_t y0,int32_t y1,int32_t z0,int32_t z1){
	const char *world=level::dim_Name(dim);
	api->reset();/*清插件的分类缓存（要在这个维度里任何一个Blk_Info出来之前调）*/
	int64_t total=(int64_t)((x1>>4)-(x0>>4)+1)*((z1>>4)-(z0>>4)+1);
	printf("fixer: [%s] x[%d,%d] y[%d,%d] z[%d,%d] 区域目录 %s\n",
		world,x0,x1,y0,y1,z0,z1,(cfg.path+"/"+lv.region_dir[dim]).c_str());
	Fix_Job j(&lv,world,cache_cnt,api,cfg.rules_mask);
	j.x0=x0;
	j.x1=x1;
	j.y0=y0;
	j.y1=y1;
	j.z0=z0;
	j.z1=z1;
	j.chunk_unit=cfg.chunk_unit;
	j.samples=cfg.verbose?-1:cfg.samples;/*verbose=1就是"样例不限个数"*/
	int64_t all_changed=0;
	int64_t visited=0,chain=0;
	int64_t bad_nbt0=nbt_bad_nbt,bad_chunk0=nbt_bad_chunk;/*这一维度开始时坏数据的计数（跑完看差值）*/
	int64_t bad_poi0=nbt_bad_poi;
	std::chrono::steady_clock::time_point t_all=std::chrono::steady_clock::now();
	/*全量扫一遍就够（改动就地连锁补判，队空即不动点，理由见文件头）。每扫完一个区块就把队列清空：
	  改动都在这附近，区块还在缓存里，判得快*/
	for(int32_t rx=x0>>9;rx<=x1>>9;rx++){
		for(int32_t rz=z0>>9;rz<=z1>>9;rz++){
			int32_t cx_begin=std::max(x0>>4,rx*32),cx_end=std::min((x1>>4)+1,rx*32+32);
			int32_t cz_begin=std::max(z0>>4,rz*32),cz_end=std::min((z1>>4)+1,rz*32+32);
			for(int32_t cx=cx_begin;cx<cx_end;cx++){
				for(int32_t cz=cz_begin;cz<cz_end;cz++){
					visited++;
					all_changed+=scan_Chunk(j,cx,cz);
					/*每扫完一个区块就把连锁队列清空：改动都在这附近，区块还在缓存里，判得快*/
					int64_t cw=work_Drain(j);
					all_changed+=cw;
					chain+=cw;
					if((visited&15)==0){
						print_Bar(visited,total,secs_Now(t_all));
					}
				}
			}
			lv.save();/*一个区域存一次，跟merge一样*/
		}
	}
	if(isatty(1)&&cfg.progress){
		putchar('\n');
	}
	lv.save();
	if(!j.poi_del.empty()){/*拆掉的POI方块：把它们的记录删掉（见poi_Del_Apply的说明）*/
		int64_t chunk_del=0,file_del=0;
		int64_t n=poi_Del_Apply(cfg.path+"/"+lv.poi_dir[dim],j.poi_del,&chunk_del,&file_del);
		if(n<0){
			printf("fixer: [%s] 删POI记录时写盘失败，这一批没落全\n",world);
			lv.err_cnt++;
		}else{
			printf("fixer: [%s] 拆掉的POI方块：删了 %lld 条记录，%lld 个区块清空后删掉，%lld 个空文件删掉\n",
				world,(long long)n,(long long)chunk_del,(long long)file_del);
		}
	}
	Fix_Stat &st=j.st;
	printf("fixer: [%s] 扫了 %lld 个区块、%lld 段（有要管的方块的 %lld 段）、初扫判了 %lld 个位置（算上连锁一共判 %lld 个）；"
		"拆掉 %lld（其中 %lld 个原来含水，换成水不是空气）、改状态 %lld、写不进去 %lld，共 %lld 次改动，用时 %.1fs\n",
		world,(long long)st.chunks,(long long)st.sec_all,(long long)st.sec_hit,(long long)st.pos,(long long)st.hit,
		(long long)st.air,(long long)st.wet,(long long)st.set,(long long)st.miss,(long long)all_changed,secs_Now(t_all));
	/*收敛情况单独一行说清楚（"已收敛"这几个字别改，.wtest/conv.sh要认它）：
	  全量扫完了、连锁队列也空了 ⇒ 已经是不动点：每个格子都在"最后一次可能影响它的改动"之后判过一遍，
	  所以不用再靠"第二遍0改动"来确认*/
	printf("fixer: [%s] 全量扫完+就地连锁补完，已收敛（不用复扫）；连锁就地补判 %lld 个位置、又改出 %lld 个；"
		"真读了 %lld 次区块（缓存%d槽；另有 %lld 个坐标读不出来，区块没生成/数据坏了，记下来不重试）\n",
		world,(long long)j.work.judged,(long long)j.work.changed,
		(long long)j.cache.loads,cache_cnt,(long long)j.cache.gone);
	if(nbt_bad_nbt>bad_nbt0||nbt_bad_chunk>bad_chunk0||nbt_bad_poi>bad_poi0){/*坏区块只跳过它自己，不中断整趟；这些区块一个字没改*/
		printf("fixer: [%s] 有 %lld 个区块的数据坏了（NBT坏 %lld、压缩坏 %lld），按\"读不出来\"跳过了，这些区块一个字没改\n",
			world,(long long)((nbt_bad_nbt-bad_nbt0)+(nbt_bad_chunk-bad_chunk0)+(nbt_bad_poi-bad_poi0)),
			(long long)(nbt_bad_nbt-bad_nbt0),(long long)(nbt_bad_chunk-bad_chunk0));
	}
	if(st.blind){
		printf("fixer: [%s] 有 %lld 个位置要用的邻居读不出来（区块没生成/调色板读不出），这些位置原样留着没判\n",
			world,(long long)st.blind);
	}
	if(st.warp){/*xPos/zPos 和文件位置对不上的区块：整块跳过没动*/
		printf("fixer: [%s] 有 %lld 个区块的xPos/zPos和文件位置对不上，整块跳过了（存档有毛病，宁可不动）\n",
			world,(long long)st.warp);
	}
	if(st.dup_y){/*同一区块里两段Y相同：整块跳过（判一段写另一段会把没判过的方块删掉）*/
		printf("fixer: [%s] 有 %lld 个区块里出现了两段Y相同的section，整块跳过了（坏数据：这种情况写回会写错段）\n",
			world,(long long)st.dup_y);
	}
	if(st.sec_direct||st.pos_bad){/*读不出来的东西：以前是静默少判，现在报出来*/
		printf("fixer: [%s] 有 %lld 段读不出调色板（direct/>256条，本程序有意不读）、%lld 个位置的数据缺/短（坏数据）；"
			"这些位置一个都没判\n",world,(long long)st.sec_direct,(long long)st.pos_bad);
	}
	if(st.noop){/*兜底判断救下来的次数：本该是0，不是0说明规则表收错了方块、或者插件给的状态和现在一样*/
		printf("fixer: [%s] 有 %lld 次要改的东西本来就那样（空气拆空气/拆完还是水/关面后状态没变，规则表或插件写错了；"
			"已经跳过没写，不跳会转不完）\n",world,(long long)st.noop);
	}
	/*按规则分：每条规则判过多少格、改了多少格，以及改掉的方块名（改动最多的前几个）。
	  规则表收错方块、或者漏了一条支撑关系时，这里一眼能看出来——大垂滴叶那次（叶子被当成悬空全拆了）
	  就是靠"下面要有支持 1572 次、全是 big_dripleaf"这种账发现的*/
	{
		int show=0;
		for(int i=1;i<api->rule_Count()&&i<(int)st.rule_cnt.size();i++){
			if(st.rule_cnt[i]||st.rule_hit[i]){
				if(!show){
					printf("fixer: [%s] 按规则分（判过/改动）：\n",world);
					show=1;
				}
				printf("fixer: [%s]   %-24s %lld / %lld\n",world,api->rule_Name(i),(long long)st.rule_cnt[i],(long long)st.rule_hit[i]);
				if(i<(int)st.rule_blk.size()&&!st.rule_blk[i].empty()){
					std::vector<std::pair<std::string,int64_t>> v(st.rule_blk[i].begin(),st.rule_blk[i].end());
					std::sort(v.begin(),v.end(),[](const std::pair<std::string,int64_t> &a,const std::pair<std::string,int64_t> &b){
						return a.second!=b.second?a.second>b.second:a.first<b.first;
					});
					std::string one;
					for(size_t k=0;k<v.size()&&k<6;k++){
						char buf[128];
						snprintf(buf,sizeof(buf),"%s%s×%lld",k?", ":"",v[k].first.c_str(),(long long)v[k].second);
						one+=buf;
					}
					if(v.size()>6){
						one+="…";
					}
					printf("fixer: [%s]     └ 改的是：%s\n",world,one.c_str());
				}
			}
		}
	}
	{/*坏数据也算"有失败"：坏区块被跳过了，那些区块没修，退出码要非0让人知道*/
		int64_t bad=(nbt_bad_nbt-bad_nbt0)+(nbt_bad_chunk-bad_chunk0)+(nbt_bad_poi-bad_poi0);
		if(st.miss||lv.err_cnt||bad||st.pos_bad){
			printf("fixer: [%s] 有失败（写不进去 %lld 个、读写出错 %d 次、坏数据跳过 %lld 个区块 + %lld 个位置），"
				"写不进多半是那段palette满256条了；坏数据在上面那几行\n",
				world,(long long)st.miss,lv.err_cnt,(long long)bad,(long long)st.pos_bad);
			return -1;
		}
	}
	if(!st.sec_hit&&st.sec_all){
		printf("fixer: [%s] 提醒：扫过的段里一个要管的方块都没有，范围/维度是不是给错了？\n",world);
	}
	return (int)all_changed;
}

/*==================== 6.1 跑完所有维度 ====================*/
int fix_World(const fix_Config &cfg,const Rules_Api *api){
	if(!dir_OK(cfg.path)){
		printf("fixer: 目录不对 %s\n",cfg.path.c_str());
		return -1;
	}
	int32_t x0=cfg.x0,x1=cfg.x1,y0=cfg.y0,y1=cfg.y1,z0=cfg.z0,z1=cfg.z1;
	if(x0>x1){
		std::swap(x0,x1);
	}
	if(y0>y1){
		std::swap(y0,y1);
	}
	if(z0>z1){
		std::swap(z0,z1);
	}
	/*范围合理性：坐标打错一个0（比如把 1000 打成 1000000000）就会去翻上百万个区域文件，
	  看着像卡死。这里按"要碰多少个区域文件"卡一道，超了直接不干，别让手滑变成事故*/
	int64_t regions=(int64_t)((x1>>9)-(x0>>9)+1)*((z1>>9)-(z0>>9)+1);
	if(regions>FIX_REGION_MAX){
		printf("fixer: 范围太大（x[%d,%d] z[%d,%d] 要碰 %lld 个区域文件，上限 %d），坐标是不是写错了？什么都没干\n",
			x0,x1,z0,z1,(long long)regions,FIX_REGION_MAX);
		return -1;
	}
	if(y0<-FIX_Y_MAX||y1>FIX_Y_MAX){/*y范围也有个谱：见FIX_Y_MAX的说明*/
		printf("fixer: y范围不对（[%d,%d]，只在 ±%d 以内），坐标是不是写错了？什么都没干\n",y0,y1,FIX_Y_MAX);
		return -1;
	}
	if(x0<-FIX_XZ_MAX||x1>FIX_XZ_MAX||z0<-FIX_XZ_MAX||z1>FIX_XZ_MAX){
		/*x/z同理：世界边界是±29999984，这里放到±3亿。不卡的话 ((x1>>4)+1)<<4、cx*16 这些算式
		  在 x1=INT32_MAX 时会溢出（UB），而且范围检查拦不住"单个区域里的天价坐标"*/
		printf("fixer: x/z范围不对（x[%d,%d] z[%d,%d]，只在 ±%d 以内），坐标是不是写错了？什么都没干\n",
			x0,x1,z0,z1,FIX_XZ_MAX);
		return -1;
	}
	int cache_cnt=cfg.cache_cnt<16?16:cfg.cache_cnt;/*太小了会一边扫一边把自己挤掉*/
	if(cache_cnt!=cfg.cache_cnt){
		printf("fixer: cache_cnt=%d太小，按16算\n",cfg.cache_cnt);
	}
	level lv(cfg.path.c_str(),cache_cnt,cfg.region_cnt);
	if(!lv.dat_ok){/*读不到level.dat就没法按DataVersion定维度目录，宁可不干*/
		printf("fixer: %s 读不到 level.dat（不是存档目录？），什么都没干\n",cfg.path.c_str());
		return -1;
	}
	printf("fixer: %s DataVersion=%d（%s），存档结构=%s（阈值 %d，见level.cpp）\n",cfg.path.c_str(),
		lv.data_version,lv.version_name.empty()?"版本名未知":lv.version_name.c_str(),
		lv.new_layout?"dimensions/新":"region/老",DAT_NEW_LAYOUT);
	int ret=0,fail=0;
	for(size_t d=0;d<cfg.dims.size();d++){
		int dim=level::dim_Of(cfg.dims[d].c_str());
		int n=fix_Dim(cfg,api,lv,dim,cache_cnt,x0,x1,y0,y1,z0,z1);
		if(n<0){
			fail=1;/*失败要"粘住"：以前 ret=-1 之后还继续 ret+=n，后面维度只要改了>=1处就把失败加成正数、
			         退出码变0——多维度跑的时候坏数据会被当成成功*/
		}else if(!fail){
			ret+=n;
		}
	}
	lv.close();
	return fail?-1:ret;
}

/*==================== 7. 规则插件（dlopen） + 命令行 ====================*/
static int file_OK(const std::string &p){/*是个普通文件？*/
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISREG(st.st_mode);
}
static std::string rules_Find(){/*规则插件固定找"fixer自己那个目录下的rules.so"（那是rules.so.26.1的符号链接），再退到当前目录*/
	char buf[4096];
	ssize_t n=readlink("/proc/self/exe",buf,sizeof(buf)-1);
	if(n>0){
		buf[n]=0;
		char *p=strrchr(buf,'/');
		if(p){
			*p=0;
			std::string s=std::string(buf)+"/rules.so";
			if(file_OK(s)){
				return s;
			}
		}
	}
	return "rules.so";
}
static const Rules_Api *rules_Load(const std::string &path){/*装规则插件（装好就不卸了：进程活到头都用它）*/
	void *h=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
	if(!h){
		printf("fixer: 装不上规则插件 %s：%s\n",path.c_str(),dlerror());
		return NULL;
	}
	Rules_Entry_Fn fn=NULL;
	*(void**)(&fn)=dlsym(h,RULES_ENTRY);/*dlsym给的void*转到函数指针，按规矩得先memcpy/强转一下地址*/
	if(!fn){
		printf("fixer: %s 里没有 %s 这个符号（不是规则插件？）\n",path.c_str(),RULES_ENTRY);
		dlclose(h);
		return NULL;
	}
	const Rules_Api *api=fn(RULES_ABI,(uint32_t)sizeof(Rules_Api));
	if(!api||!api->judge||!api->blk_Fill||!api->blk_Rule||!api->rule_Count||!api->rule_Name||
	   !api->group_Count||!api->group_Name||!api->reset||!api->def_Rule){
		printf("fixer: %s 的接口和本程序对不上（重新编一个rules.so.26.1，见rules.hpp）\n",path.c_str());
		dlclose(h);
		return NULL;
	}
	return api;/*装上的不卸：进程活到头都用它*/
}
static void split_List(const char *s,std::vector<std::string> &out){/*逗号隔开的一串，空段跳过*/
	out.clear();
	if(!s){
		return;
	}
	std::string v=s;
	size_t p=0;
	while(p<v.size()){
		size_t q=v.find(',',p);
		if(q==std::string::npos){
			q=v.size();
		}
		std::string one=v.substr(p,q-p);
		if(!one.empty()){
			out.push_back(one);
		}
		p=q+1;
	}
}
static void usage(const char *me){
	printf("用法：%s <path> <x1> <y1> <z1> <x2> <y2> <z2> <chunk_unit> [dims]\n",me);
	printf("  path       要处理的地图目录（同时也是输出，会被改写！）\n");
	printf("  x1 y1 z1 x2 y2 z2  方块范围的两个角（谁大谁小都行，内部取min/max），含两头临界值\n");
	printf("  chunk_unit 1=以区块为单位（x/z 不裁，整块整块地扫）；0=裁到上面的范围里\n");
	printf("  dims       可选，逗号隔开的维度名，默认 minecraft:overworld\n");
	printf("             例：minecraft:overworld,minecraft:the_nether,minecraft:the_end\n");
	printf("例：%s ./new -1000 -63 -1000 1000 319 1000 1\n",me);
	printf("  规则插件固定找本程序旁边的 rules.so（那是 rules.so.26.1 的符号链接）；找不到就什么都不改\n");
}
int main(int argc,char **argv){
	if(argc<9){/*path+6个范围+chunk_unit=8个参数，维度可选*/
		usage(argv[0]);
		return -1;
	}
	std::string rpath=rules_Find();
	const Rules_Api *api=rules_Load(rpath);
	if(!api){
		printf("fixer: 规则插件没装上，什么都没改\n");
		return -1;
	}
	printf("fixer: 规则插件 %s（版本%s，%d条规则、%d个规则家族）\n",rpath.c_str(),api->version,api->rule_Count(),api->group_Count());
	fix_Config cfg;
	cfg.path=argv[1];
	int32_t *nums[6]={&cfg.x0,&cfg.y0,&cfg.z0,&cfg.x1,&cfg.y1,&cfg.z1};
	static const char *nname[6]={"x1","y1","z1","x2","y2","z2"};
	for(int i=0;i<6;i++){
		if(!arg_Int(argv[2+i],nums[i])){/*非数字/超int32：直接报错，别静默按0跑（就地改的工具尤其危险）*/
			printf("fixer: %s 不是整数：%s\n",nname[i],argv[2+i]);
			return -1;
		}
	}
	int32_t cu=0;
	if(!arg_Int(argv[8],&cu)||(cu!=0&&cu!=1)){/*只认0/1*/
		printf("fixer: chunk_unit 只能是 0 或 1：%s\n",argv[8]);
		return -1;
	}
	cfg.chunk_unit=(int)cu;
	if(argc>9){
		split_List(argv[9],cfg.dims);
	}
	if(cfg.dims.empty()){
		cfg.dims.push_back("minecraft:overworld");
	}
	for(size_t i=0;i<cfg.dims.size();i++){
		const char *w=cfg.dims[i].c_str();
		if(level::dim_Of(w)==0&&strcmp(w,"minecraft:overworld")&&strcmp(w,"overworld")){
			printf("fixer: 维度名 %s 认不出来（只认 overworld/the_nether/the_end 这三种），改用主世界\n",w);
			cfg.dims[i]="minecraft:overworld";
		}
	}
	int ret=fix_World(cfg,api);
	return ret<0?-1:0;
}
