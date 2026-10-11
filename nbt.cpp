#include "nbt.hpp"
#include <sys/stat.h>
#include <sys/types.h>
/*nbt.cpp：NBT那一套（读、写、编码、深拷贝、比较、打印、释放、level.dat）
 *	按结构体分组放：nbt_Raw（一段字节+游标）→ nbt_Any（基类）→ nbt_List → nbt_AnyRef → nbt_Compound
 */

/*---- nbt_Raw：read_X建树、write_X/encode写回去、load/save文件 ----*/
int64_t nbt_bad_nbt=0;/*读到坏NBT的次数（见nbt.hpp的说明）：坏数据只跳过这一块，不退出程序*/
#define BAD_SHOW 10/*同一个进程里最多打这么多条坏数据详情，之后的只计数（一个坏文件可能一大片）*/
static void bad_Nbt(const char *what,size_t a,size_t b){/*记一笔，并且最多打前BAD_SHOW条*/
	nbt_bad_nbt++;
	if(nbt_bad_nbt<=BAD_SHOW){
		printf("nbt bad:%s（%zu,%zu）：这一块跳过（后面同类只计数）\n",what,a,b);
	}
}
int nbt_Raw::read_Need(size_t need){/*"接下来要读need个字节"，够不够先看：数组/列表的长度字段来自文件，
	坏文件（或者位置读串了）会给个几十亿的长度，照着它resize会当场bad_alloc/被OOM杀掉。
	合法文件里这些字节一定都在，所以这个检查对正常数据没有任何影响。
	不够就返回0（并记一笔），调用者放弃这一块——是"跳过"，不再exit(-1)*/
	if(pos>data.size()||need>data.size()-pos){
		bad_Nbt("overrun",pos,need);
		fail=1;
		return 0;
	}
	return 1;
}
void nbt_Raw::read_bytes(void *ret,size_t size,size_t len,bool direction){
	size_t total=size*len;
	if(total&&pos+total>data.size()){/*不够读：清零给出去（上层读到0长度会自然收手），并放弃这一块*/
		bad_Nbt("overrun",pos,total);
		fail=1;
		memset(ret,0,total);/*别让上层拿着未初始化的长度/数值继续往下算*/
		return;
	}
	if(!total){
		return;
	}
	byte *dst=(byte*)ret;
	const byte *src=data.data()+pos;
	pos+=total;
	if(size==1||direction){/*字节数组/不要反序：整块搬*/
		memcpy(dst,src,total);
		return;
	}
	/*文件里是大端，内存里要小端，所以每个元素内部反序；这里按元素宽度整块做（小端机器上就是字节交换）*/
	if(size==2){
		for(size_t i=0;i<len;i++){
			uint16_t v;
			memcpy(&v,src+i*2,2);
			v=__builtin_bswap16(v);
			memcpy(dst+i*2,&v,2);
		}
	}else if(size==4){
		for(size_t i=0;i<len;i++){
			uint32_t v;
			memcpy(&v,src+i*4,4);
			v=__builtin_bswap32(v);
			memcpy(dst+i*4,&v,4);
		}
	}else if(size==8){
		for(size_t i=0;i<len;i++){
			uint64_t v;
			memcpy(&v,src+i*8,8);
			v=__builtin_bswap64(v);
			memcpy(dst+i*8,&v,8);
		}
	}else{/*别的宽度很少见，老老实实一个字节一个字节*/
		for(size_t i=0;i<len;i++){
			for(size_t j=0;j<size;j++){
				dst[i*size+j]=src[i*size+size-j-1];
			}
		}
	}
}

void nbt_Raw::write_bytes(const void *src,size_t size,size_t len,bool direction){
	size_t total=size*len;
	if(pos+total>data.size()){
		printf("nbt write overrun:%zu+%zu>%zu\n",pos,total,data.size());
		exit(-1);
	}
	byte *dst=data.data()+pos;
	const byte *s=(const byte*)src;
	pos+=total;
	if(!total){
		return;
	}
	if(size==1||direction){
		memcpy(dst,s,total);
		return;
	}
	if(size==2){
		for(size_t i=0;i<len;i++){
			uint16_t v;
			memcpy(&v,s+i*2,2);
			v=__builtin_bswap16(v);
			memcpy(dst+i*2,&v,2);
		}
	}else if(size==4){
		for(size_t i=0;i<len;i++){
			uint32_t v;
			memcpy(&v,s+i*4,4);
			v=__builtin_bswap32(v);
			memcpy(dst+i*4,&v,4);
		}
	}else if(size==8){
		for(size_t i=0;i<len;i++){
			uint64_t v;
			memcpy(&v,s+i*8,8);
			v=__builtin_bswap64(v);
			memcpy(dst+i*8,&v,8);
		}
	}else{
		for(size_t i=0;i<len;i++){
			for(size_t j=0;j<size;j++){
				dst[i*size+j]=s[i*size+size-j-1];
			}
		}
	}
}
nbt_Byte *nbt_Raw::read_Byte(){
	nbt_Byte *con=new nbt_Byte;
	read_bytes(&con->con,1,1,0);
	return con;
}
nbt_Short *nbt_Raw::read_Short(){
	nbt_Short *con=new nbt_Short;
	read_bytes(&con->con,2,1,0);
	return con;
}
nbt_Int *nbt_Raw::read_Int(){
	nbt_Int *con=new nbt_Int;
	read_bytes(&con->con,4,1,0);
	return con;
}
nbt_Long *nbt_Raw::read_Long(){
	nbt_Long *con=new nbt_Long;
	read_bytes(&con->con,8,1,0);
	return con;
}
nbt_Float *nbt_Raw::read_Float(){
	nbt_Float *con=new nbt_Float;
	read_bytes(&con->con,4,1,0);
	return con;
}
nbt_Double *nbt_Raw::read_Double(){
	nbt_Double *con=new nbt_Double;
	read_bytes(&con->con,8,1,0);
	return con;
}
nbt_Byte_Array *nbt_Raw::read_Byte_Array(){
	nbt_Byte_Array *con=new nbt_Byte_Array;
	uint32_t len;
	read_bytes(&len,4,1,0);
	if(!read_Need(len)){/*每个元素1字节：先看够不够，别照着长度字段一口气resize几个G*/
		return con;
	}
	con->con.resize(len);
	if(len){
		read_bytes(con->con.data(),1,len,0);
	}
	return con;
}
nbt_String *nbt_Raw::read_String(){
	nbt_String *con=new nbt_String;
	uint16_t len;
	read_bytes(&len,2,1,0);
	con->con.resize(len);
	if(len){
		read_bytes(&con->con[0],1,len,0);
	}
	return con;
}
nbt_List *nbt_Raw::read_List(){
	nbt_List *con=new nbt_List;
	read_bytes(&con->elem,1,1,0);
	uint32_t len;
	read_bytes(&len,4,1,0);
	if(!read_Need(len)){/*元素至少占1字节（除非是TAG_End，那在真存档里不会出现），先看长度像不像话*/
		return con;
	}
	con->con.reserve(len);
	if(con->elem==0x09||con->elem==0x0a){/*列表套列表/列表套compound也是递归：算一层深度*/
		if(++depth>NBT_DEPTH_MAX){
			bad_Nbt("deep",depth,0);
			fail=1;
			depth--;
			return con;
		}
		for(uint32_t i=0;i<len;i++){
			nbt_Any *e=read_Any(con->elem);
			if(!e){
				break;/*里面读到坏数据/坏tag了：这一块整体作废（read_Compound会把整棵树扔掉）*/
			}
			con->push_back(e);
		}
		depth--;
		return con;
	}
	for(uint32_t i=0;i<len;i++){
		nbt_Any *e=read_Any(con->elem);
		if(!e){
			break;/*里面读到坏数据/坏tag了：这一块整体作废（read_Compound会把整棵树扔掉）*/
		}
		con->push_back(e);
	}
	return con;
}
nbt_Compound *nbt_Raw::read_Compound(){
	if(++depth>NBT_DEPTH_MAX){/*嵌套太深：真存档的树深不过十几层，这里挡的是"别人给的/坏掉的文件"——
	                              递归读会把8MB栈打穿（一层 Compound 只要3字节），崩在写盘途中连报告都没有*/
		bad_Nbt("deep",depth,0);
		fail=1;
		depth--;
		return new nbt_Compound;/*返回空壳：read_Compound 看到 fail 会把整棵树扔掉*/
	}
	nbt_Compound *con=new nbt_Compound;
	con->read_Members(this);
	depth--;
	if(fail){/*读到一半坏了：整棵树扔掉（半截数据比没有更危险），返回NULL让调用者跳过这一块*/
		con->free();
		return NULL;
	}
	return con;
}
nbt_Int_Array *nbt_Raw::read_Int_Array(){
	nbt_Int_Array *con=new nbt_Int_Array;
	uint32_t len;
	read_bytes(&len,4,1,0);
	if(!read_Need((size_t)len*4)){
		return con;
	}
	con->con.resize(len);
	if(len){
		read_bytes(con->con.data(),4,len,0);
	}
	return con;
}
nbt_Long_Array *nbt_Raw::read_Long_Array(){
	nbt_Long_Array *con=new nbt_Long_Array;
	uint32_t len;
	read_bytes(&len,4,1,0);
	if(!read_Need((size_t)len*8)){
		return con;
	}
	con->con.resize(len);
	if(len){
		read_bytes(con->con.data(),8,len,0);
	}
	return con;
}
nbt_Any *nbt_Raw::read_Any(uint8_t type){
	if(type==0x00) return new nbt_Any(0x00);
	else if(type==0x01) return read_Byte();
	else if(type==0x02) return read_Short();
	else if(type==0x03) return read_Int();
	else if(type==0x04) return read_Long();
	else if(type==0x05) return read_Float();
	else if(type==0x06) return read_Double();
	else if(type==0x07) return read_Byte_Array();
	else if(type==0x08) return read_String();
	else if(type==0x09) return read_List();
	else if(type==0x0a) return read_Compound();
	else if(type==0x0b) return read_Int_Array();
	else if(type==0x0c) return read_Long_Array();
	else{
		bad_Nbt("tag",type,0);/*不认识的tag：这一块作废（原来是打印一句就exit）*/
		fail=1;
		return NULL;
	}
}
void nbt_Raw::read_Root_Header(){/*文件开头是 根tag + 名字长度 + 名字（MC写的是空）*/
	if(pos>=data.size()){/*空文件（0字节，或者gzip解出来是空的）：data[0]都不该读*/
		bad_Nbt("empty",pos,0);
		fail=1;
		return;
	}
	uint8_t type=data[pos++];
	uint16_t len;
	read_bytes(&len,2,1,0);
	if(!read_Need(len)){
		return;
	}
	pos+=len;
	if(type!=0x0a){
		bad_Nbt("root",type,0);/*根不是compound：这一块作废*/
		fail=1;
	}
}
nbt_Compound *nbt_Raw::read_Root(){
	read_Root_Header();
	if(fail){/*坏数据：不建树，返回NULL（坏在哪儿、坏了几次都已经记在 nbt_bad_nbt 里）*/
		return NULL;
	}
	return read_Compound();
}
int file_Read_All(const char *path,std::vector<byte> &out){/*把整个文件读进out（自己清空/填好）：打不开、
	不是普通文件（目录、fifo、/dev里的东西）、或者读不回来都返回0。
	必须用fstat认"是不是普通文件"：fopen对目录也能成功，而目录的ftell能给出个天文数字
	（ext4上就是LLONG_MAX），照着它resize会当场bad_alloc/被OOM杀掉——存档目录里出现一个叫
	level.dat 或者 r.0.0.mca 的目录时就会踩到*/
	struct stat st;
	out.clear();/*先把out清空：失败返回时调用者手里的out不能还是上一个文件的内容*/
	FILE *fp=fopen(path,"rb");
	if(!fp){
		return 0;
	}
	if(fstat(fileno(fp),&st)||!S_ISREG(st.st_mode)){
		fclose(fp);
		return 0;
	}
	if(st.st_size>0){
		out.resize((size_t)st.st_size);
		size_t got=fread(out.data(),1,out.size(),fp);
		out.resize(got);/*文件被截短过就按真读回来的算（后面读NBT自然会报越界）*/
	}
	fclose(fp);
	return 1;
}
nbt_Raw nbt_Raw::load(const char *path){/*整个文件读进来，后面读越界会直接报错（read_bytes里有检查）*/
	nbt_Raw raw;
	file_Read_All(path,raw.data);/*读不出来就是空的（调用者看空不空）*/
	return raw;
}
nbt_Raw nbt_Raw::load_Dat(const char *path){
	nbt_Raw raw=load(path);
	if(raw.data.size()>1&&(raw.data[0]==0x1f||raw.data[0]==0x78)){/*1f8b=gzip，78xx=zlib*/
		std::vector<byte> out=inflate_data(raw.data.data(),raw.data.size());
		nbt_Raw ret;
		ret.data=std::move(out);
		return ret;
	}
	return raw;/*裸数据*/
}
int nbt_Raw::save(const char *path)const{
	FILE *fp=fopen(path,"wb");
	if(!fp){
		return 0;
	}
	size_t ret=fwrite(data.data(),1,data.size(),fp);
	fclose(fp);
	return ret==data.size();
}

/*---- nbt_Any：基类，按tag分派（没有虚函数，靠type） ----*/
void nbt_Any::free(){/*没有虚析构，按tag分派到具体类型的delete；容器的析构函数会先删掉自己的孩子*/
	switch(type){
		case 0x00: delete this; break;
		case 0x01: delete (nbt_Byte*)this; break;
		case 0x02: delete (nbt_Short*)this; break;
		case 0x03: delete (nbt_Int*)this; break;
		case 0x04: delete (nbt_Long*)this; break;
		case 0x05: delete (nbt_Float*)this; break;
		case 0x06: delete (nbt_Double*)this; break;
		case 0x07: delete (nbt_Byte_Array*)this; break;
		case 0x08: delete (nbt_String*)this; break;
		case 0x09: delete (nbt_List*)this; break;
		case 0x0a: delete (nbt_Compound*)this; break;
		case 0x0b: delete (nbt_Int_Array*)this; break;
		case 0x0c: delete (nbt_Long_Array*)this; break;
		default:
			printf("unknown:%02x\n",type);
			exit(-1);
	}
}

size_t nbt_Any::byte_size()const{/*序列化字节数，不含tag和名字*/
	const nbt_List *list;
	const nbt_Compound *con;
	size_t total;
	switch(type){
		case 0x00: return 0;
		case 0x01: return 1;
		case 0x02: return 2;
		case 0x03: case 0x05: return 4;
		case 0x04: case 0x06: return 8;
		case 0x07: return 4+((const nbt_Byte_Array*)this)->con.size();
		case 0x08: return 2+((const nbt_String*)this)->con.size();
		case 0x09:
			list=(const nbt_List*)this;
			total=5;
			for(nbt_Any *p:list->con){
				total+=p->byte_size();
			}
			return total;
		case 0x0a:
			con=(const nbt_Compound*)this;
			total=1;
			for(const auto &kv:con->con){
				total+=3+kv.first.size()+kv.second->byte_size();
			}
			return total;
		case 0x0b: return 4+4*((const nbt_Int_Array*)this)->con.size();
		case 0x0c: return 4+8*((const nbt_Long_Array*)this)->con.size();
	}
	return 0;
}
void nbt_Any::encode(nbt_Raw *raw)const{/*只写payload，tag和名字由上层写*/
	const nbt_Byte_Array *ba;
	const nbt_String *str;
	const nbt_List *list;
	const nbt_Compound *con;
	uint16_t len;
	byte end=0x00;
	switch(type){
		case 0x01: raw->write_bytes(&((const nbt_Byte*)this)->con,1,1,0); break;
		case 0x02: raw->write_bytes(&((const nbt_Short*)this)->con,2,1,0); break;
		case 0x03: raw->write_bytes(&((const nbt_Int*)this)->con,4,1,0); break;
		case 0x04: raw->write_bytes(&((const nbt_Long*)this)->con,8,1,0); break;
		case 0x05: raw->write_bytes(&((const nbt_Float*)this)->con,4,1,0); break;
		case 0x06: raw->write_bytes(&((const nbt_Double*)this)->con,8,1,0); break;
		case 0x07:
			ba=(const nbt_Byte_Array*)this;
			{
				uint32_t len32=(uint32_t)ba->con.size();
				raw->write_bytes(&len32,4,1,0);
			}
			raw->write_bytes(ba->con.data(),1,ba->con.size(),0);
			break;
		case 0x08:
			str=(const nbt_String*)this;
			if(str->con.size()>0xffff){
				printf("string too long:%zu\n",str->con.size());
				exit(-1);
			}
			len=(uint16_t)str->con.size();
			raw->write_bytes(&len,2,1,0);
			raw->write_bytes(str->con.data(),1,len,0);
			break;
		case 0x09:
			list=(const nbt_List*)this;
			raw->write_bytes(&list->elem,1,1,0);
			{
				uint32_t len32=(uint32_t)list->con.size();
				raw->write_bytes(&len32,4,1,0);
			}
			for(nbt_Any *p:list->con){
				p->encode(raw);
			}
			break;
		case 0x0a:
			con=(const nbt_Compound*)this;
			for(const auto &kv:con->con){
				byte t=kv.second->type;
				if(kv.first.size()>0xffff){/*成员名字也是2字节长度：超了必须报错。硬截断写出去是坏文件，
				                             而且byte_size()算的长度会和真写下去的字节数对不上（后面留一串0）*/
					printf("name too long:%zu\n",kv.first.size());
					exit(-1);
				}
				len=(uint16_t)kv.first.size();
				raw->write_bytes(&t,1,1,0);
				raw->write_bytes(&len,2,1,0);
				raw->write_bytes(kv.first.data(),1,len,0);
				kv.second->encode(raw);
			}
			raw->write_bytes(&end,1,1,0);
			break;
		case 0x0b:
			{
				const nbt_Int_Array *ia=(const nbt_Int_Array*)this;
				uint32_t len32=(uint32_t)ia->con.size();
				raw->write_bytes(&len32,4,1,0);
				raw->write_bytes(ia->con.data(),4,ia->con.size(),0);
			}
			break;
		case 0x0c:
			{
				const nbt_Long_Array *la=(const nbt_Long_Array*)this;
				uint32_t len32=(uint32_t)la->con.size();
				raw->write_bytes(&len32,4,1,0);
				raw->write_bytes(la->con.data(),8,la->con.size(),0);
			}
			break;
	}
}
static nbt_Byte *copy_Byte(const nbt_Byte *con){
	return new nbt_Byte(con->con);
}
static nbt_Short *copy_Short(const nbt_Short *con){
	return new nbt_Short(con->con);
}
static nbt_Int *copy_Int(const nbt_Int *con){
	return new nbt_Int(con->con);
}
static nbt_Long *copy_Long(const nbt_Long *con){
	return new nbt_Long(con->con);
}
static nbt_Float *copy_Float(const nbt_Float *con){
	return new nbt_Float(con->con);
}
static nbt_Double *copy_Double(const nbt_Double *con){
	return new nbt_Double(con->con);
}
static nbt_Byte_Array *copy_Byte_Array(const nbt_Byte_Array *con){
	nbt_Byte_Array *ret=new nbt_Byte_Array;
	ret->con=con->con;
	return ret;
}
static nbt_String *copy_String(const nbt_String *con){
	return new nbt_String(con->con);
}
static nbt_List *copy_List(const nbt_List *con){
	nbt_List *ret=new nbt_List(con->elem);
	ret->con.reserve(con->con.size());
	for(nbt_Any *p:con->con){
		ret->con.push_back(p->copy());
	}
	return ret;
}
static nbt_Compound *copy_Compound(const nbt_Compound *con){
	nbt_Compound *ret=new nbt_Compound;
	for(const auto &kv:con->con){
		ret->set(kv.first,kv.second->copy());
	}
	return ret;
}
static nbt_Int_Array *copy_Int_Array(const nbt_Int_Array *con){
	nbt_Int_Array *ret=new nbt_Int_Array;
	ret->con=con->con;
	return ret;
}
static nbt_Long_Array *copy_Long_Array(const nbt_Long_Array *con){
	nbt_Long_Array *ret=new nbt_Long_Array;
	ret->con=con->con;
	return ret;
}
nbt_Any *nbt_Any::copy()const{/*深拷贝，数组按值复制*/
	switch(type){
		case 0x00: return new nbt_Any(0x00);
		case 0x01: return copy_Byte((const nbt_Byte*)this);
		case 0x02: return copy_Short((const nbt_Short*)this);
		case 0x03: return copy_Int((const nbt_Int*)this);
		case 0x04: return copy_Long((const nbt_Long*)this);
		case 0x05: return copy_Float((const nbt_Float*)this);
		case 0x06: return copy_Double((const nbt_Double*)this);
		case 0x07: return copy_Byte_Array((const nbt_Byte_Array*)this);
		case 0x08: return copy_String((const nbt_String*)this);
		case 0x09: return copy_List((const nbt_List*)this);
		case 0x0a: return copy_Compound((const nbt_Compound*)this);
		case 0x0b: return copy_Int_Array((const nbt_Int_Array*)this);
		case 0x0c: return copy_Long_Array((const nbt_Long_Array*)this);
	}
	return NULL;
}

static bool cmp_Bytes(const void *a,const void *b,size_t len){
	return !len||!memcmp(a,b,len);
}
int nbt_Any::cmp(const nbt_Any *b)const{/*逐值比较；compound按名字找，与成员顺序无关*/
	const nbt_List *la,*lb;
	const nbt_Compound *ca,*cb;
	if(type!=b->type){
		return 0;
	}
	switch(type){
		case 0x00: return 1;
		case 0x01: return ((const nbt_Byte*)this)->con==((const nbt_Byte*)b)->con;
		case 0x02: return ((const nbt_Short*)this)->con==((const nbt_Short*)b)->con;
		case 0x03: return ((const nbt_Int*)this)->con==((const nbt_Int*)b)->con;
		case 0x04: return ((const nbt_Long*)this)->con==((const nbt_Long*)b)->con;
		case 0x05: return cmp_Bytes(&((const nbt_Float*)this)->con,&((const nbt_Float*)b)->con,4);
		case 0x06: return cmp_Bytes(&((const nbt_Double*)this)->con,&((const nbt_Double*)b)->con,8);
		case 0x07: return ((const nbt_Byte_Array*)this)->con==((const nbt_Byte_Array*)b)->con;
		case 0x08: return ((const nbt_String*)this)->con==((const nbt_String*)b)->con;
		case 0x09:
			la=(const nbt_List*)this;
			lb=(const nbt_List*)b;
			if(la->con.size()!=lb->con.size()){
				return 0;
			}
			for(size_t i=0;i<la->con.size();i++){
				if(!la->con[i]->cmp(lb->con[i])){
					return 0;
				}
			}
			return 1;
		case 0x0a:
			ca=(const nbt_Compound*)this;
			cb=(const nbt_Compound*)b;
			if(ca->con.size()!=cb->con.size()){
				return 0;
			}
			for(const auto &kv:ca->con){
				auto it=cb->con.find(kv.first);
				if(it==cb->con.end()||!kv.second->cmp(it->second)){
					return 0;
				}
			}
			return 1;
		case 0x0b: return ((const nbt_Int_Array*)this)->con==((const nbt_Int_Array*)b)->con;
		case 0x0c: return ((const nbt_Long_Array*)this)->con==((const nbt_Long_Array*)b)->con;
	}
	return 0;
}
bool nbt_Any::operator==(const nbt_Any &b)const{
	return cmp(&b);
}
bool nbt_Any::operator!=(const nbt_Any &b)const{
	return !cmp(&b);
}

static void print_String(const std::string &con){
	putchar('"');
	for(char c:con){
		if(c=='"'||c=='\\')printf("\\%c",c);
		else if(c=='\n')printf("\\n");
		else if(c=='\t')printf("\\t");
		else if(c=='\r')printf("\\r");
		else if((byte)c<0x20)printf("\\u%04x",(byte)c);
		else putchar((byte)c);
	}
	putchar('"');
}
void nbt_Any::print(size_t depth)const{
	switch(type){
		case 0x00:break;
		case 0x01:printf("%u",((const nbt_Byte*)this)->con);break;
		case 0x02:printf("%u",((const nbt_Short*)this)->con);break;
		case 0x03:printf("%u",((const nbt_Int*)this)->con);break;
		case 0x04:printf("%lu",((const nbt_Long*)this)->con);break;
		case 0x05:printf("%g",((const nbt_Float*)this)->con);break;
		case 0x06:printf("%g",((const nbt_Double*)this)->con);break;
		case 0x07:{
			const nbt_Byte_Array *con=(const nbt_Byte_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%u%s",con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case 0x08:print_String(((const nbt_String*)this)->con);break;
		case 0x09:{
			const nbt_List *con=(const nbt_List*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				con->con[i]->print(depth+1);
				printf("%s",i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case 0x0a:{
			const nbt_Compound *con=(const nbt_Compound*)this;
			putchar('{');
			size_t i=0;
			for(const auto &kv:con->con){/*unordered_map的遍历顺序，和文件里的顺序无关*/
				print_String(kv.first);
				putchar(':');
				kv.second->print(depth+1);
				printf("%s",++i<con->con.size()?",":"");
			}
			putchar('}');
			break;
		}
		case 0x0b:{
			const nbt_Int_Array *con=(const nbt_Int_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%u%s",con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case 0x0c:{
			const nbt_Long_Array *con=(const nbt_Long_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%lu%s",con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		default:{
			printf("unknown:%02x\n",type);
			exit(-1);
		}
	}
}
void nbt_Any::print()const{
	print(0);
}

int32_t nbt_Any::as_Int()const{/*Byte/Short/Int的数值，其它类型返回0*/
	if(type==0x01) return (int8_t)((const nbt_Byte*)this)->con;
	if(type==0x02) return (int16_t)((const nbt_Short*)this)->con;
	if(type==0x03) return (int32_t)((const nbt_Int*)this)->con;
	return 0;
}
int64_t nbt_Any::as_Long()const{/*Long的数值，其它类型退回as_Int*/
	if(type==0x04) return (int64_t)((const nbt_Long*)this)->con;
	return as_Int();
}
const char *nbt_Any::as_Str()const{
	return type==0x08?((const nbt_String*)this)->con.c_str():NULL;
}
bool nbt_Any::is(const char *str)const{/*是String且内容相同*/
	return type==0x08&&((const nbt_String*)this)->con==str;
}

/*---- nbt_List：元素是nbt_Any*，push_back接管所有权 ----*/
nbt_Any *nbt_List::at(size_t i)const{
	if(i>=con.size()){
		throw std::out_of_range("nbt_List::at");
	}
	return con[i];
}
void nbt_List::push_back(nbt_Any *any){/*接管any的所有权*/
	if(!any){/*读坏了会给NULL：当没这个元素（树上不留空指针，别的接口就不用到处判空）*/
		return;
	}
	if(con.empty()){
		elem=any->type;/*MC写的空列表元素类型是乱的（比如block_entities是1），第一个元素进来时定正*/
	}
	con.push_back(any);
}
nbt_Compound *nbt_List::push_Compound(){
	nbt_Compound *ret=new nbt_Compound;
	push_back(ret);
	return ret;
}
void nbt_List::pop_back(){
	if(con.empty()){
		return;
	}
	con.back()->free();
	con.pop_back();
}
void nbt_List::erase(size_t i){
	if(i>=con.size()){
		return;
	}
	con[i]->free();
	con.erase(con.begin()+i);
}
void nbt_List::insert(size_t i,nbt_Any *any){
	if(i>con.size()||!any){
		return;
	}
	if(con.empty()){
		elem=any->type;/*和push_back一样：空list的元素类型由第一个进来的元素定*/
	}
	con.insert(con.begin()+i,any);
}
void nbt_List::clear(){
	for(nbt_Any *p:con){
		p->free();
	}
	con.clear();
}
nbt_List::~nbt_List(){
	clear();
}

/*---- nbt_AnyRef：l[i]的代理 ----*/
nbt_AnyRef &nbt_AnyRef::operator=(nbt_Any *any){
	if(*slot){
		(*slot)->free();
	}
	*slot=any;
	return *this;
}
/*---- nbt_Compound：成员是unordered_map<string,nbt_Any*> ----*/
void nbt_Compound::read_Members(nbt_Raw *raw){/*读成员直到End；每个成员是 tag+名字长度+名字+payload*/
	while(1){
		uint8_t type;
		raw->read_bytes(&type,1,1,0);
		if(type==0x00){
			break;
		}
		uint16_t name_len;
		raw->read_bytes(&name_len,2,1,0);
		std::string name(name_len,0);
		if(name_len){
			raw->read_bytes(&name[0],1,name_len,0);
		}
		set(name,raw->read_Any(type));
	}
}
nbt_Compound *nbt_Compound::read(const char *path){
	nbt_Raw raw=nbt_Raw::load(path);
	if(raw.data.empty()){
		return NULL;
	}
	return raw.read_Root();/*坏数据在read_Root里已经计过数并扔掉了，这里拿到的是NULL*/
}

nbt_Compound *nbt_Compound::read_Dat(const char *path){
	nbt_Raw raw=nbt_Raw::load_Dat(path);
	if(raw.data.empty()){
		return NULL;
	}
	return raw.read_Root();
}

nbt_Raw nbt_Compound::to_Raw()const{/*整棵树编成文件里的样子：根tag+空名字+成员+End；游标回到0，可以直接再读*/
	nbt_Raw raw;
	raw.data.resize(3+byte_size());
	byte type=0x0a;
	uint16_t len=0;
	raw.write_bytes(&type,1,1,0);
	raw.write_bytes(&len,2,1,0);
	encode(&raw);
	raw.pos=0;
	return raw;
}
int nbt_Compound::save(const char *path)const{
	nbt_Raw raw=to_Raw();
	return raw.save(path);
}

nbt_Any *nbt_Compound::get(const std::string &key)const{/*没有这个成员返回NULL*/
	auto it=con.find(key);
	return it==con.end()?NULL:it->second;
}
nbt_Any *nbt_Compound::set(const std::string &key,nbt_Any *any){/*接管any的所有权，同名的旧成员删掉；any为NULL等于erase*/
	if(!any){
		erase(key);
		return NULL;
	}
	auto it=con.find(key);
	if(it==con.end()){
		con[key]=any;
	}else{
		it->second->free();
		it->second=any;
	}
	return any;
}
size_t nbt_Compound::erase(const std::string &key){
	auto it=con.find(key);
	if(it==con.end()){
		return 0;
	}
	it->second->free();
	con.erase(it);
	return 1;
}
void nbt_Compound::clear(){
	for(auto &kv:con){
		kv.second->free();
	}
	con.clear();
}
nbt_Int *nbt_Compound::set_Int(const std::string &key,int32_t v){
	nbt_Int *ret=new nbt_Int((uint32_t)v);
	set(key,ret);
	return ret;
}
nbt_Long *nbt_Compound::set_Long(const std::string &key,int64_t v){
	nbt_Long *ret=new nbt_Long((uint64_t)v);
	set(key,ret);
	return ret;
}
nbt_String *nbt_Compound::set_String(const std::string &key,const std::string &str){
	nbt_String *ret=new nbt_String(str);
	set(key,ret);
	return ret;
}
nbt_Compound::Ref &nbt_Compound::Ref::operator=(nbt_Any *any){
	self->set(key,any);
	return *this;
}

nbt_Compound::~nbt_Compound(){
	clear();
}
