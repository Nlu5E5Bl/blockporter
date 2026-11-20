#include "../rules.hpp"
#include <dlfcn.h>
#include <string>
#include <vector>
/*judge.cpp：直接装 rules.so 判一格（不读存档），给 check_placement.py 对拍 wiki 放置表用
 *	judge [rules.so]      默认 ./rules.so（check_placement.py 传 rules.so.26.1.1）
 *	标准输入一行一格，tab 分隔 7 个方块：自己 下 上 东 西 南 北
 *	  方块写成 name 或 name[k=v,k=v]，不带命名空间的补 minecraft:；"-" 或空 = 空气
 *	输出一行一个结论：KEEP / AIR / SET <名字>[属性] / NONE（规则表不管它）
 *	只判这一格、不连锁：旁边的支撑块自己合不合法不管。
 *	编译：g++ -std=c++23 -O2 -I. test/judge.cpp -o test/work/judge -L. -lnbtrw -lz -ldl -Wl,-rpath,$PWD
 */
struct Spec{
	std::string name;
	nbt_Compound props;
	Blk_Info info;
};
static void parse_Spec(const std::string &t,Spec &s,const Rules_Api *api){
	std::string n=t;
	size_t b=n.find('[');
	std::string p;
	if(b!=std::string::npos){
		p=n.substr(b+1,n.size()-b-2);
		n=n.substr(0,b);
	}
	if(n.empty()||n=="-"){
		n="air";
	}
	if(n.find(':')==std::string::npos){
		n="minecraft:"+n;
	}
	s.name=n;
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
	s.info=Blk_Info();
	s.info.name=s.name;
	s.info.props=s.props.empty()?NULL:&s.props;
	api->fill_BlkInfo(&s.info);
}
int main(int argc,char **argv){
	const char *path=argc>1?argv[1]:"./rules.so";
	void *h=dlopen(path,RTLD_NOW|RTLD_LOCAL);
	Rules_Entry_Fn fn=h?(Rules_Entry_Fn)dlsym(h,RULES_ENTRY):NULL;
	const Rules_Api *api=fn?fn(RULES_ABI,(uint32_t)sizeof(Rules_Api)):NULL;
	if(!api){
		fprintf(stderr,"judge: 装不上 %s\n",path);
		return 1;
	}
	static const int order[6]={NB_D,NB_U,NB_E,NB_W,NB_S,NB_N};/*输入里 下 上 东 西 南 北 的顺序*/
	char line[8192];
	while(fgets(line,sizeof(line),stdin)){
		std::string l(line);
		while(!l.empty()&&(l.back()=='\n'||l.back()=='\r')){
			l.pop_back();
		}
		std::vector<std::string> f;
		size_t pos=0;
		while(true){
			size_t q=l.find('\t',pos);
			f.push_back(l.substr(pos,q==std::string::npos?std::string::npos:q-pos));
			if(q==std::string::npos){
				break;
			}
			pos=q+1;
		}
		f.resize(7);
		std::vector<Spec> sp(7);
		for(int i=0;i<7;i++){
			parse_Spec(f[i],sp[i],api);
		}
		int def=api->get_BlkRule(&sp[0].info,0xFFFFFFFFu);
		if(def<0){
			printf("NONE\n");
			continue;
		}
		Rules_Ctx c;
		c.self=&sp[0].info;
		for(int i=0;i<6;i++){
			c.nb[order[i]]=&sp[i+1].info;
		}
		c.got=0x3F;
		int r=api->judge_Blk(&sp[0].info,def,&c);
		if(r==FX_KEEP){
			printf("KEEP\n");
		}else if(r==FX_AIR){
			printf("AIR\n");
		}else{
			std::string o=std::string(c.out_block.name())+"[";
			bool first=true;
			for(const auto &kv:c.out_block.properties){
				o+=(first?"":",")+kv.first+"="+std::string(kv.second->as_String().value_or("?"));
				first=false;
			}
			printf("SET %s]\n",o.c_str());
		}
		fflush(stdout);
	}
	return 0;
}
