#include "../nbtrw.hpp"
#include <dirent.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
/*palstat.cpp：扫一个 region 目录里所有 section 的调色板，给写规则用（只读，不改存档）
 *	palstat count <region目录>   每种方块在多少条调色板项里出现（多的先做规则）
 *	palstat props <region目录>   每种方块出现过的“属性名=取值集合”（规则要照真实属性名写）
 *	坏区块/坏段只跳过并计数，不崩。调色板条目按区块自己的DataVersion认格式。
 *	编译：g++ -std=c++23 -O2 -I. test/palstat.cpp -o test/work/palstat -L. -lnbtrw -lz -Wl,-rpath,$PWD
 */
struct Stat{
	int64_t chunks=0,unreadable=0,secs=0,no_palette=0,states=0;
	std::unordered_map<std::string,int64_t> count;
	std::unordered_map<std::string,int64_t> with_props;
	std::map<std::string,std::map<std::string,std::set<std::string>>> props;
};
static void scan_Palette(const nbt_List *pal,State_Format f,Stat &st){/*把一段调色板里的方块名、属性名、属性值记进统计*/
	for(size_t q=0;q<pal->size();q++){
		const nbt_Any *b=pal->ref(q);
		const char *name=get_StateName(b,f);
		if(!name[0]){
			continue;
		}
		st.count[name]++;
		st.states++;
		std::map<std::string,std::set<std::string>> &row=st.props[name];/*没有属性就是个空表*/
		const nbt_Compound *pa=get_StateProps(b,f);
		if(!pa){
			continue;
		}
		st.with_props[name]++;
		for(const auto &kv:*pa){
			row[kv.first].insert(std::string(kv.second?kv.second->as_String().value_or("?"):"?"));
		}
	}
}
static void scan_Chunk(const nbt_Compound *c,Stat &st){/*只读树：直接看区块的 compound*/
	const nbt_Any *sa=c->get("sections");
	if(!sa||sa->type!=NBT_LIST){
		return;
	}
	const nbt_List *secs=(const nbt_List*)sa;
	for(size_t k=0;k<secs->size();k++){
		const nbt_Any *s=secs->ref(k);
		if(!s||s->type!=NBT_COMPOUND){
			continue;
		}
		st.secs++;
		const nbt_Any *bs=((const nbt_Compound*)s)->get("block_states");
		const nbt_Any *pa=bs&&bs->type==NBT_COMPOUND?((const nbt_Compound*)bs)->get("palette"):NULL;
		if(!pa||pa->type!=NBT_LIST){
			st.no_palette++;
			continue;
		}
		scan_Palette((const nbt_List*)pa,get_ChunkStateFormat(c),st);
	}
}
int main(int argc,char **argv){/*count / props 两个子命令：扫一个区域目录里所有区块的调色板，打每种方块出现的次数，或者每种方块见过的属性取值*/
	if(argc<3||(strcmp(argv[1],"count")&&strcmp(argv[1],"props"))){
		printf("用法：%s count|props <region目录>\n",argv[0]);
		return 1;
	}
	const char *dir=argv[2];
	DIR *dp=opendir(dir);
	if(!dp){
		printf("打不开 %s\n",dir);
		return 1;
	}
	Stat st;
	struct dirent *ent;
	while((ent=readdir(dp))){
		int rx=0,rz=0;
		if(sscanf(ent->d_name,"r.%d.%d.mca",&rx,&rz)!=2){
			continue;
		}
		MCA m(get_MCAPath(dir,rx,rz));
		if(m.bad){
			st.unreadable++;
			continue;
		}
		for(uint32_t i=0;i<32;i++){
			for(uint32_t j=0;j<32;j++){
				if(!m.has_Chunk(i,j)){
					continue;
				}
				std::optional<nbt_Compound> c=m.get_Compound(i,j);
				if(!c){
					st.unreadable++;
					continue;
				}
				st.chunks++;
				scan_Chunk(&*c,st);
			}
		}
	}
	closedir(dp);
	printf("%s: %lld区块（读不出来 %lld）%lld段 %lld条调色板项（没有调色板的段 %lld）\n",dir,(long long)st.chunks,
		(long long)st.unreadable,(long long)st.secs,(long long)st.states,(long long)st.no_palette);
	if(!strcmp(argv[1],"count")){
		std::vector<std::pair<std::string,int64_t>> v(st.count.begin(),st.count.end());
		std::sort(v.begin(),v.end(),[](const std::pair<std::string,int64_t> &a,const std::pair<std::string,int64_t> &b){
			return a.second!=b.second?a.second>b.second:a.first<b.first;
		});
		printf("不同方块 %zu 种\n",v.size());
		for(const auto &kv:v){
			printf("%8lld  %s%s\n",(long long)kv.second,kv.first.c_str(),st.with_props.count(kv.first)?"  [有Properties]":"");
		}
		return 0;
	}
	for(const auto &it:st.props){
		printf("%s",it.first.c_str());
		if(it.second.empty()){
			printf("  (无属性)\n");
			continue;
		}
		printf(":");
		for(const auto &p:it.second){
			printf(" %s=",p.first.c_str());
			int n=0;
			for(const std::string &v:p.second){
				printf("%s%s",n++?"|":"",v.c_str());
			}
		}
		printf("\n");
	}
	return 0;
}
