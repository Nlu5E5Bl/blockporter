/*	blockporter.hpp —— merge/fixer 共用的上层小工具（不是库的一部分：libnbtrw 不知道它，规则插件也不用它）
 *
 *	命令行（逗号隔开的列表、六个坐标）、方块范围（含按区块/区域取整）、维度去重、目录、终端进度条、实体在不在范围里。
 *	两个程序各自是一个编译单元，这里的函数都是 inline。
 */
#ifndef BLOCKPORTER_HPP
#define BLOCKPORTER_HPP
#include "nbtrw.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

/*---- 坐标 ----*/
inline int64_t floor_Div(int64_t v,int64_t d){/*向下取整的除法（负数也往小取：-1/16 是 -1）*/
	return v>=0?v/d:(v-(d-1))/d;
}
inline int32_t floor_Div16(int64_t v){return (int32_t)floor_Div(v,16);}/*方块坐标→区块坐标*/
inline int32_t floor_Div32(int64_t v){return (int32_t)floor_Div(v,32);}/*区块坐标→区域坐标*/

/*这一趟处理的方块范围（含头尾，已经规整成小的在前）。方块级的操作严格按它；区块级的按 x/z 碰到的区块（y 不管），
  区域文件级的按碰到的区域文件。范围外一律只读不写*/
struct Block_Range{
	int32_t x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;
	bool is_Inside(int64_t x,int64_t y,int64_t z)const{/*方块级：这一格在不在范围里*/
		return x>=x0&&x<=x1&&y>=y0&&y<=y1&&z>=z0&&z<=z1;
	}
	int32_t get_CX0()const{return floor_Div16(x0);}/*区块级：x/z 碰到的区块*/
	int32_t get_CX1()const{return floor_Div16(x1);}
	int32_t get_CZ0()const{return floor_Div16(z0);}
	int32_t get_CZ1()const{return floor_Div16(z1);}
	bool is_ChunkSelected(int32_t cx,int32_t cz)const{/*范围碰到这个区块（哪怕只有一格）*/
		return cx>=get_CX0()&&cx<=get_CX1()&&cz>=get_CZ0()&&cz<=get_CZ1();
	}
	int32_t get_RX0()const{return floor_Div32(get_CX0());}/*区域文件级：碰到的区域*/
	int32_t get_RX1()const{return floor_Div32(get_CX1());}
	int32_t get_RZ0()const{return floor_Div32(get_CZ0());}
	int32_t get_RZ1()const{return floor_Div32(get_CZ1());}
	bool is_RegionSelected(int32_t rx,int32_t rz)const{/*范围碰到这个区域*/
		return rx>=get_RX0()&&rx<=get_RX1()&&rz>=get_RZ0()&&rz<=get_RZ1();
	}
};
/*命令行上的 x1 y1 z1 x2 y2 z2（两个角，谁大谁小都行）：解析不了打一行“<who>: xx 不是整数”给 false*/
inline bool parse_Range(char **argv,const char *who,Block_Range *rg){
	static const char *nname[6]={"x1","y1","z1","x2","y2","z2"};
	int32_t v[6];
	for(int i=0;i<6;i++){
		if(!parse_Int(argv[i],&v[i])){
			printf("%s: %s 不是整数：%s\n",who,nname[i],argv[i]);
			return false;
		}
	}
	rg->x0=std::min(v[0],v[3]);rg->x1=std::max(v[0],v[3]);
	rg->y0=std::min(v[1],v[4]);rg->y1=std::max(v[1],v[4]);
	rg->z0=std::min(v[2],v[5]);rg->z1=std::max(v[2],v[5]);
	return true;
}
/*单条实体在不在范围里（方块级，Pos 向下取整到方块）。Pos 照游戏的 Vec3.CODEC 读：正好 3 个数值，任何数值 tag 都认。
  没有 Pos（游戏放原点）、形状不对、不是有限数的不知道在哪，算在范围里（它所在的实体表本来就在范围碰到的区块里：
  merge 跟着区块搬，fixer 坏的照删、好的照查重）。超出 int32 的有限坐标一定在范围外*/
inline bool is_EntityInRange(const nbt_Any *e,const Block_Range &rg){
	const nbt_Any *p=e&&e->type==NBT_COMPOUND?((const nbt_Compound*)e)->get("Pos"):NULL;
	if(!p||p->type!=NBT_LIST||((const nbt_List*)p)->con.size()!=3){
		return true;
	}
	const nbt_List *l=(const nbt_List*)p;
	double v[3];
	for(int i=0;i<3;i++){
		v[i]=std::floor(get_NumberDouble(l->con[(size_t)i]).value_or(NAN));
		if(!std::isfinite(v[i])){
			return true;
		}
		if(v[i]<(double)INT32_MIN||v[i]>(double)INT32_MAX){
			return false;/*double 超出 int64 再转是 UB：先挡掉*/
		}
	}
	return rg.is_Inside((int64_t)v[0],(int64_t)v[1],(int64_t)v[2]);
}

/*---- 命令行列表、维度 ----*/
inline void split_List(const char *s,std::vector<std::string> &out){/*逗号隔开的参数拆成几段（空段丢掉）*/
	if(!s){
		return;
	}
	std::string cur;
	for(const char *p=s;;p++){
		if(*p==','||!*p){
			if(!cur.empty()){
				out.push_back(cur);
				cur.clear();
			}
			if(!*p){
				break;
			}
		}else{
			cur+=*p;
		}
	}
}
/*维度去重：按每个存档解析后的目录比，不按字符串比（overworld 和 minecraft:overworld 是同一个维度；merge 跑第二遍时
  now 和 new 已经一样，InhabitedTime 会被改成 0，fixer 跑两遍会把第一遍的结果当输入再清理一次）。
  解析不了的原样留着，交给后面报“路径无效”。who 是打日志用的程序名*/
inline void dedupe_Dims(const std::vector<const Level*> &lvs,std::vector<std::string> &dims,const char *who){
	std::set<std::string> seen;
	std::vector<std::string> out;
	for(size_t i=0;i<dims.size();i++){
		std::string key="dir:";
		bool ok=true;
		for(const Level *lv:lvs){
			std::optional<std::string> b=lv->get_DimensionBase(dims[i].c_str());
			if(!b){
				ok=false;
				break;
			}
			key+=*b+"\n";
		}
		if(!ok){
			key="arg:"+dims[i];
		}
		if(!seen.insert(key).second){
			printf("%s: 维度 %s 和前面的维度是同一个目录，跳过\n",who,dims[i].c_str());
			continue;
		}
		out.push_back(dims[i]);
	}
	dims.swap(out);
}

/*---- 目录 ----*/
inline bool is_Dir(const std::string &p){/*是不是目录*/
	struct stat st;
	return !stat(p.c_str(),&st)&&S_ISDIR(st.st_mode);
}
inline bool is_SamePath(const std::string &a,const std::string &b){/*两个路径是不是同一个文件/目录（比设备号+inode，软链接、多写的斜杠都骗不过）*/
	struct stat sa,sb;
	return !stat(a.c_str(),&sa)&&!stat(b.c_str(),&sb)&&sa.st_dev==sb.st_dev&&sa.st_ino==sb.st_ino;
}
inline bool make_Dir(const std::string &p){/*mkdir -p（新建 poi/entities 文件前补目录）*/
	if(p.empty()||is_Dir(p)){
		return true;
	}
	size_t i=p.find('/',1);
	for(;;){
		if(i==std::string::npos){
			i=p.size();
		}
		std::string sub=p.substr(0,i);
		if(!sub.empty()&&!is_Dir(sub)&&mkdir(sub.c_str(),0777)&&!is_Dir(sub)){
			return false;
		}
		if(i==p.size()){
			return true;
		}
		i=p.find('/',i+1);
	}
}

/*---- 终端进度条：只在 stderr 是终端时画，测试/重定向时不污染日志；重画同一行 ----*/
struct Progress{
	const char *name;
	int64_t total,done,last;
	bool tty;
	Progress(const char *n,int64_t t):name(n),total(t>0?t:0),done(0),last(0),tty(isatty(STDERR_FILENO)!=0){}/*t=总数*/
	void tick(int64_t n=1){/*又做完 n 个：每 64 个（或者做完时）重画一次*/
		done+=n;
		if(done>total){
			done=total;
		}
		if(!tty||total<=0){
			return;
		}
		if(done!=total&&done-last<64){
			return;
		}
		last=done;
		int pct=(int)(done*100/total);
		int bar=(int)(done*24/total);
		fprintf(stderr,"\r%-8s [",name);
		for(int i=0;i<24;i++){
			fputc(i<bar?'#':'-',stderr);
		}
		fprintf(stderr,"] %lld/%lld %3d%%",(long long)done,(long long)total,pct);
		fflush(stderr);
	}
	void finish(){/*补满并换行*/
		if(tty&&total>0&&done<total){
			done=total;
			tick(0);
		}
		if(tty&&total>0){
			fputc('\n',stderr);
			fflush(stderr);
		}
	}
};
#endif
