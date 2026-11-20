#include "../../rules.hpp"
#include <dlfcn.h>
#include <deque>
#include <string>
#include <vector>
#include <algorithm>
/*rules_shim.cpp：给 RulesCmp.java 用的 C 接口（Java 用 FFM 调；rules.hpp 里有 string_view/Block，Java 直接调不了）。
 *	shim_Open(rules.so 路径)        装插件，成功给 true
 *	shim_AddState("minecraft:x[k=v,...]") 登记一个方块状态（按 Java 那边 McWorld.all 的顺序），返回下标
 *	shim_GetRule(下标)              get_BlkRule(全家族)：规则表下标，-1=不管
 *	shim_GetRuleKind(规则表下标)    get_DefRule：规则号（RULE_*）
 *	shim_GetRuleName(规则号)        get_RuleName：规则的中文名
 *	shim_Judge(自己,规则表下标,六邻下标[NB_*顺序],输出缓冲,长度)
 *	                                返回 FX_KEEP/FX_AIR/FX_SET，blind 给 3；FX_SET/FX_AIR 时把 out_block 按
 *	                                “名字[属性按名字排序]”写进缓冲（空 id 写空串）
 *	编译：g++ -std=c++23 -O2 -fPIC -shared -I. test/mc/rules_shim.cpp -o test/work/librules_shim.so -L. -lnbtrw -lz -ldl -Wl,-rpath,$PWD
 *	线程：shim_AddState 只在开始时单线程调；shim_Judge 只读登记好的 Blk_Info，可以多线程调。
 */
struct Shim_State{
	std::string name;
	nbt_Compound props;
	Blk_Info info;
};
static const Rules_Api *api=NULL;
static std::deque<Shim_State> states;/*deque：加元素不搬家，Blk_Info 里的 string_view/指针一直有效*/

extern "C" bool shim_Open(const char *path){/*装 rules.so（RulesCmp 一开始调一次）*/
	void *h=dlopen(path,RTLD_NOW|RTLD_LOCAL);
	Rules_Entry_Fn fn=h?(Rules_Entry_Fn)dlsym(h,RULES_ENTRY):NULL;
	api=fn?fn(RULES_ABI,(uint32_t)sizeof(Rules_Api)):NULL;
	return api!=NULL;
}
extern "C" int shim_AddState(const char *spec){/*登记一个方块状态（"名字[属性=值,…]"），让插件分好类；给它的编号*/
	std::string t(spec),p;
	size_t b=t.find('[');
	if(b!=std::string::npos){
		p=t.substr(b+1,t.size()-b-2);
		t=t.substr(0,b);
	}
	Shim_State &s=states.emplace_back();
	s.name=t;
	size_t pos=0;
	while(pos<p.size()){
		size_t q=p.find(',',pos);
		if(q==std::string::npos){
			q=p.size();
		}
		std::string kv=p.substr(pos,q-pos);
		size_t eq=kv.find('=');
		if(eq!=std::string::npos){
			s.props.set_String(kv.substr(0,eq),kv.substr(eq+1));
		}
		pos=q+1;
	}
	s.info.name=s.name;
	s.info.props=s.props.empty()?NULL:&s.props;
	api->fill_BlkInfo(&s.info);
	return (int)states.size()-1;
}
extern "C" int shim_GetRule(int self){/*这个状态归哪条规则管：规则表下标，-1=不管*/
	return api->get_BlkRule(&states[(size_t)self].info,0xFFFFFFFFu);
}
extern "C" int shim_GetRuleKind(int def){/*规则表下标 → 规则号*/
	return api->get_DefRule(def);
}
extern "C" const char *shim_GetRuleName(int rule){/*规则号 → 规则的中文名*/
	return api->get_RuleName(rule);
}
static void write_Block(const Block &b,char *out,int len){/*Block 写成 "名字[属性=值,…]"（属性按名字排序，和 Java 那边的写法一样）*/
	std::string o(b.name());
	if(!o.empty()&&!b.properties.empty()){
		std::vector<std::string> kv;
		for(const auto &e:b.properties){
			kv.push_back(e.first+"="+std::string(e.second->as_String().value_or("?")));
		}
		std::sort(kv.begin(),kv.end());
		o+="[";
		for(size_t i=0;i<kv.size();i++){
			o+=(i?",":"")+kv[i];
		}
		o+="]";
	}
	if(len>0){
		size_t n=std::min(o.size(),(size_t)len-1);
		memcpy(out,o.data(),n);
		out[n]=0;
	}
}
extern "C" int shim_Judge(int self,int def,const int *nb6,char *out,int len){/*判一格：self 和六个邻居都是 shim_AddState 给的编号；返回 FX_*，改成/剩下的状态写进 out*/
	Rules_Ctx c;
	c.self=&states[(size_t)self].info;
	c.x=0;c.y=64;c.z=0;/*和 McWorld.O 同一个位置（竹子/滴水石锥的碰撞箱偏移、藤头 age 都按坐标算）*/
	for(int i=0;i<6;i++){
		c.nb[i]=&states[(size_t)nb6[i]].info;
	}
	c.got=0x3F;
	int r=api->judge_Blk(c.self,def,&c);
	if(c.blind){
		return 3;
	}
	if(r!=FX_KEEP&&out){
		write_Block(c.out_block,out,len);
	}
	return r;
}
