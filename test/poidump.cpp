#include "../nbtrw.hpp"
#include "../rules.hpp"
#include <dlfcn.h>
#include <string>
/*poidump.cpp：把 rules.so 的“方块→默认POI”表按行打出来，给 check_poitab.py 和 poitab.py 那张手抄表对拍。
 *	stdin 每行：<方块名>[\t<属性 k=v,k=v>]；stdout 每行：<方块名>\t<属性>\t<POI类型或->\t<free_tickets>\t<max_tickets>
 *	用法：test/work/poidump <rules.so>
 */
int main(int argc,char **argv){
	if(argc<2){
		printf("用法：%s <rules.so>\n",argv[0]);
		return 1;
	}
	void *h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
	if(!h){
		printf("poidump: 装不上 %s：%s\n",argv[1],dlerror());
		return 1;
	}
	Rules_Entry_Fn fn=(Rules_Entry_Fn)dlsym(h,RULES_ENTRY);
	const Rules_Api *api=fn?fn(RULES_ABI,(uint32_t)sizeof(Rules_Api)):NULL;
	if(!api||!api->get_POIInfo||!api->fill_BlkInfo){
		printf("poidump: 插件接口对不上\n");
		return 1;
	}
	char line[1024];
	while(fgets(line,sizeof(line),stdin)){
		std::string s(line);
		while(!s.empty()&&(s.back()=='\n'||s.back()=='\r')){
			s.pop_back();
		}
		if(s.empty()){
			continue;
		}
		size_t tab=s.find('\t');
		std::string name=s.substr(0,tab),props=tab==std::string::npos?"":s.substr(tab+1);
		nbt_Compound st;
		st.set_String("Name",name);
		if(!props.empty()){
			nbt_Compound *p=new nbt_Compound;
			size_t pos=0;
			while(pos<props.size()){
				size_t q=props.find(',',pos);
				if(q==std::string::npos){
					q=props.size();
				}
				std::string kv=props.substr(pos,q-pos);
				size_t eq=kv.find('=');
				if(eq!=std::string::npos){
					p->set_String(kv.substr(0,eq),kv.substr(eq+1));
				}
				pos=q+1;
			}
			st.set("Properties",p);
		}
		Blk_Info b;
		b.blk=&st;
		b.name=get_StateName(&st,STATE_NAME);
		b.props=get_StateProps(&st,STATE_NAME);
		api->fill_BlkInfo(&b);
		const Poi_Info *poi=api->get_POIInfo(&b);
		if(poi){
			printf("%s\t%s\t%s\t%d\t%d\n",name.c_str(),props.c_str(),poi->type,poi->free_tickets,poi->max_tickets);
		}else{
			printf("%s\t%s\t-\t0\t0\n",name.c_str(),props.c_str());
		}
	}
	return 0;
}
