#include "nbtrw.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
/*nbt.cpp：NBT那一套（读、写、编码、深拷贝、比较、转换、打印、释放、dat）
 *	按结构体分组放：nbt_Raw（一段字节+游标）→ nbt_Any（基类）→ 标量/String/数组 → nbt_List →
 *	nbt_AnyRef → nbt_Compound → dat的gzip/zlib读写
 */

/*---- nbt_Raw：read_X建树、write_X/encode写回去、load/save文件 ----*/
bool nbt_Raw::can_Read(size_t need){/*“接下来要读need个字节”，够不够先看：长度字段来自文件*/
	if(pos>data.size()||need>data.size()-pos){/*先判pos，避免data.size()-pos下溢；也不用pos+need（会溢出）*/
		fail=true;
		return false;
	}
	return true;
}
void nbt_Raw::read_Bytes(void *ret,size_t size,size_t len,bool direction){
	if(size&&len>SIZE_MAX/size){/*size*len先防溢出*/
		fail=true;
		return;
	}
	size_t total=size*len;
	if(total&&(pos>data.size()||total>data.size()-pos)){/*同can_Read：不写pos+total，防溢出*/
		fail=true;
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
	/*文件里是大端，内存里要小端，所以每个元素内部反序*/
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

bool nbt_Raw::write_Bytes(const void *src,size_t size,size_t len,bool direction){
	if(size&&len>SIZE_MAX/size){/*size*len先防溢出*/
		fail=true;
		return false;
	}
	size_t total=size*len;
	if(pos>data.size()||total>data.size()-pos){
		fail=true;
		return false;
	}
	if(!total){
		return true;
	}
	byte *dst=data.data()+pos;
	const byte *s=(const byte*)src;
	pos+=total;
	if(size==1||direction){
		memcpy(dst,s,total);
		return true;
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
	return true;
}
nbt_Byte *nbt_Raw::read_Byte(){
	nbt_Byte *con=new nbt_Byte;
	read_Bytes(&con->con,1,1,0);
	return con;
}
nbt_Short *nbt_Raw::read_Short(){
	nbt_Short *con=new nbt_Short;
	read_Bytes(&con->con,2,1,0);
	return con;
}
nbt_Int *nbt_Raw::read_Int(){
	nbt_Int *con=new nbt_Int;
	read_Bytes(&con->con,4,1,0);
	return con;
}
nbt_Long *nbt_Raw::read_Long(){
	nbt_Long *con=new nbt_Long;
	read_Bytes(&con->con,8,1,0);
	return con;
}
nbt_Float *nbt_Raw::read_Float(){
	nbt_Float *con=new nbt_Float;
	read_Bytes(&con->con,4,1,0);
	return con;
}
nbt_Double *nbt_Raw::read_Double(){
	nbt_Double *con=new nbt_Double;
	read_Bytes(&con->con,8,1,0);
	return con;
}
nbt_Byte_Array *nbt_Raw::read_Byte_Array(){
	nbt_Byte_Array *con=new nbt_Byte_Array;
	uint32_t len;
	read_Bytes(&len,4,1,0);
	if(!can_Read(len)){/*每个元素1字节：先看够不够，别照着长度字段一口气resize几个G*/
		return con;
	}
	con->con.resize(len);
	if(len){
		read_Bytes(con->con.data(),1,len,0);
	}
	return con;
}
nbt_String *nbt_Raw::read_String(){
	nbt_String *con=new nbt_String;
	uint16_t len;
	read_Bytes(&len,2,1,0);
	con->con.resize(len);
	if(len){
		read_Bytes(&con->con[0],1,len,0);
	}
	return con;
}
nbt_List *nbt_Raw::read_List(){
	nbt_List *con=new nbt_List;
	read_Bytes(&con->elem,1,1,0);
	uint32_t len;
	read_Bytes(&len,4,1,0);
	if(!can_Read(len)){/*元素至少占1字节，先看长度像不像话*/
		return con;
	}
	if(con->elem==NBT_END&&len){/*空list才是elem=0；elem=0却有元素是坏数据*/
		fail=true;
		return con;
	}
	con->con.reserve(len);
	for(uint32_t i=0;i<len;i++){
		nbt_Any *e=read_Any(con->elem);
		if(!e){
			break;/*里面读到坏数据/坏tag了：这一块整体作废*/
		}
		if(e->type!=con->elem){/*类型对不上（正常不会：read_Any就是按elem建的）*/
			e->free();
			fail=true;
			break;
		}
		con->con.push_back(e);/*读路径直接进vector：这里是新节点，不用push_back的重复检查*/
	}
	return con;
}
nbt_Compound *nbt_Raw::read_Compound(){
	nbt_Compound *con=new nbt_Compound;
	con->read_Members(this);
	if(fail){/*读到一半坏了：整棵树扔掉（半截数据比没有更危险）*/
		con->free();
		return NULL;
	}
	return con;
}
nbt_Int_Array *nbt_Raw::read_Int_Array(){
	nbt_Int_Array *con=new nbt_Int_Array;
	uint32_t len;
	read_Bytes(&len,4,1,0);
	if(!can_Read((size_t)len*4)){
		return con;
	}
	con->con.resize(len);
	if(len){
		read_Bytes(con->con.data(),4,len,0);
	}
	return con;
}
nbt_Long_Array *nbt_Raw::read_Long_Array(){
	nbt_Long_Array *con=new nbt_Long_Array;
	uint32_t len;
	read_Bytes(&len,4,1,0);
	if(!can_Read((size_t)len*8)){
		return con;
	}
	con->con.resize(len);
	if(len){
		read_Bytes(con->con.data(),8,len,0);
	}
	return con;
}
nbt_Any *nbt_Raw::read_Any(uint8_t type){
	if(type==NBT_END) return new nbt_Any(NBT_END);
	else if(type==NBT_BYTE) return read_Byte();
	else if(type==NBT_SHORT) return read_Short();
	else if(type==NBT_INT) return read_Int();
	else if(type==NBT_LONG) return read_Long();
	else if(type==NBT_FLOAT) return read_Float();
	else if(type==NBT_DOUBLE) return read_Double();
	else if(type==NBT_BYTE_ARRAY) return read_Byte_Array();
	else if(type==NBT_STRING) return read_String();
	else if(type==NBT_LIST) return read_List();
	else if(type==NBT_COMPOUND) return read_Compound();
	else if(type==NBT_INT_ARRAY) return read_Int_Array();
	else if(type==NBT_LONG_ARRAY) return read_Long_Array();
	else{
		fail=true;/*不认识的tag：这一块作废*/
		return NULL;
	}
}
bool nbt_Raw::read_RootHeader(){/*文件开头是 根tag + 名字长度 + 名字（MC写的是空）*/
	if(pos>=data.size()){/*空文件（0字节，或者gzip解出来是空的）：data[0]都不该读*/
		fail=true;
		return false;
	}
	uint8_t type=data[pos++];
	uint16_t len;
	read_Bytes(&len,2,1,0);
	if(!can_Read(len)){
		return false;
	}
	pos+=len;
	if(type!=NBT_COMPOUND){/*根不是compound：这一块作废*/
		fail=true;
		return false;
	}
	return true;
}
nbt_Compound *nbt_Raw::read_Root(){
	if(!read_RootHeader()||fail){/*坏数据：不建树，返回NULL*/
		return NULL;
	}
	return read_Compound();
}
bool read_File(const char *path,std::vector<byte> &out){/*把整个文件读进out：打不开、不是普通文件、读不回来都返回false。
	必须用fstat认“是不是普通文件”：fopen对目录也能成功，而目录的ftell能给出个天文数字*/
	struct stat st;
	out.clear();/*先把out清空：失败返回时调用者手里的out不能还是上一个文件的内容*/
	FILE *fp=fopen(path,"rb");
	if(!fp){
		return false;
	}
	if(fstat(fileno(fp),&st)||!S_ISREG(st.st_mode)){
		fclose(fp);
		return false;
	}
	if(st.st_size>0){
		out.resize((size_t)st.st_size);
		size_t got=fread(out.data(),1,out.size(),fp);
		out.resize(got);/*文件被截短过就按真读回来的算*/
	}
	fclose(fp);
	return true;
}
nbt_Raw nbt_Raw::load(const char *path){
	nbt_Raw raw;
	read_File(path,raw.data);/*读不出来就是空的（调用者看空不空）*/
	return raw;
}
nbt_Raw nbt_Raw::load_Dat(const char *path){
	nbt_Raw raw=load(path);
	if(raw.data.size()>1&&(raw.data[0]==0x1f||raw.data[0]==0x78)){/*1f8b=gzip，78xx=zlib*/
		std::vector<byte> out=inflate_Data(raw.data.data(),raw.data.size());
		nbt_Raw ret;
		ret.data=std::move(out);
		return ret;
	}
	return raw;/*裸数据*/
}
bool nbt_Raw::save(const char *path)const{
	if(!path||!path[0]){
		return false;
	}
	FILE *fp=fopen(path,"wb");
	if(!fp){
		return false;
	}
	size_t ret=data.empty()?0:fwrite(data.data(),1,data.size(),fp);
	bool ok=fclose(fp)==0;
	return ok&&ret==data.size();
}
int get_DatCompression(const char *path){/*0=裸、1=gzip、2=zlib、-1=读不到；set_Dat按原样写回*/
	FILE *fp=fopen(path,"rb");
	if(!fp){
		return -1;
	}
	byte b[2]={0,0};
	size_t n=fread(b,1,2,fp);
	fclose(fp);
	if(n>=2&&b[0]==0x1f&&b[1]==0x8b){
		return 1;
	}
	if(n>=1&&b[0]==0x78){
		return 2;
	}
	return 0;
}

/*---- nbt_Any：基类，按tag分派（没有虚函数，靠type） ----*/
bool nbt_Any::free(){/*没有虚析构，按tag分派到具体类型的delete；容器的析构函数会先删掉自己的孩子*/
	switch(type){
		case NBT_END: delete this; return true;
		case NBT_BYTE: delete (nbt_Byte*)this; return true;
		case NBT_SHORT: delete (nbt_Short*)this; return true;
		case NBT_INT: delete (nbt_Int*)this; return true;
		case NBT_LONG: delete (nbt_Long*)this; return true;
		case NBT_FLOAT: delete (nbt_Float*)this; return true;
		case NBT_DOUBLE: delete (nbt_Double*)this; return true;
		case NBT_BYTE_ARRAY: delete (nbt_Byte_Array*)this; return true;
		case NBT_STRING: delete (nbt_String*)this; return true;
		case NBT_LIST: delete (nbt_List*)this; return true;
		case NBT_COMPOUND: delete (nbt_Compound*)this; return true;
		case NBT_INT_ARRAY: delete (nbt_Int_Array*)this; return true;
		case NBT_LONG_ARRAY: delete (nbt_Long_Array*)this; return true;
		default: return false;/*tag不认识：不删（调用者可以自己决定怎么处理）*/
	}
}

size_t nbt_Any::get_ByteSize()const{/*序列化字节数，不含tag和名字*/
	const nbt_List *list;
	const nbt_Compound *con;
	size_t total;
	switch(type){
		case NBT_END: return 0;
		case NBT_BYTE: return 1;
		case NBT_SHORT: return 2;
		case NBT_INT: case NBT_FLOAT: return 4;
		case NBT_LONG: case NBT_DOUBLE: return 8;
		case NBT_BYTE_ARRAY: return 4+((const nbt_Byte_Array*)this)->con.size();
		case NBT_STRING: return 2+((const nbt_String*)this)->con.size();
		case NBT_LIST:
			list=(const nbt_List*)this;
			total=5;
			for(nbt_Any *p:list->con){
				if(p){
					total+=p->get_ByteSize();
				}
			}
			return total;
		case NBT_COMPOUND:
			con=(const nbt_Compound*)this;
			total=1;
			for(const auto &kv:con->con){
				if(!kv.second||kv.second->type==NBT_END){/*NBT_END不是值：空指针/End成员都不写*/
					continue;
				}
				total+=3+kv.first.size()+kv.second->get_ByteSize();
			}
			return total;
		case NBT_INT_ARRAY: return 4+4*((const nbt_Int_Array*)this)->con.size();
		case NBT_LONG_ARRAY: return 4+8*((const nbt_Long_Array*)this)->con.size();
	}
	return 0;
}
bool nbt_Any::encode(nbt_Raw *raw)const{/*只写payload，tag和名字由上层写；写不下返回false*/
	if(!raw||raw->fail){
		return false;
	}
	const nbt_Byte_Array *ba;
	const nbt_String *str;
	const nbt_List *list;
	const nbt_Compound *con;
	uint16_t len;
	byte end=NBT_END;
	switch(type){
		case NBT_END: return true;
		case NBT_BYTE: return raw->write_Bytes(&((const nbt_Byte*)this)->con,1,1,0);
		case NBT_SHORT: return raw->write_Bytes(&((const nbt_Short*)this)->con,2,1,0);
		case NBT_INT: return raw->write_Bytes(&((const nbt_Int*)this)->con,4,1,0);
		case NBT_LONG: return raw->write_Bytes(&((const nbt_Long*)this)->con,8,1,0);
		case NBT_FLOAT: return raw->write_Bytes(&((const nbt_Float*)this)->con,4,1,0);
		case NBT_DOUBLE: return raw->write_Bytes(&((const nbt_Double*)this)->con,8,1,0);
		case NBT_BYTE_ARRAY:
			ba=(const nbt_Byte_Array*)this;
			if(ba->con.size()>0xFFFFFFFFu){
				raw->fail=true;
				return false;
			}
			{
				uint32_t len32=(uint32_t)ba->con.size();
				if(!raw->write_Bytes(&len32,4,1,0)){
					return false;
				}
			}
			return raw->write_Bytes(ba->con.data(),1,ba->con.size(),0);
		case NBT_STRING:
			str=(const nbt_String*)this;
			if(str->con.size()>0xffff){/*NBT格式：字符串长度是2字节*/
				raw->fail=true;
				return false;
			}
			len=(uint16_t)str->con.size();
			return raw->write_Bytes(&len,2,1,0)&&raw->write_Bytes(str->con.data(),1,len,0);
		case NBT_LIST:
			list=(const nbt_List*)this;
			if(list->con.size()>0xFFFFFFFFu){/*NBT格式：list长度是4字节*/
				raw->fail=true;
				return false;
			}
			if(!raw->write_Bytes(&list->elem,1,1,0)){
				return false;
			}
			{
				uint32_t len32=(uint32_t)list->con.size();
				if(!raw->write_Bytes(&len32,4,1,0)){
					return false;
				}
			}
			for(nbt_Any *p:list->con){
				if(p&&!p->encode(raw)){
					return false;
				}
			}
			return true;
		case NBT_COMPOUND:
			con=(const nbt_Compound*)this;
			for(const auto &kv:con->con){
				if(!kv.second||kv.second->type==NBT_END){
					continue;
				}
				byte t=kv.second->type;
				if(kv.first.size()>0xffff){/*成员名字也是2字节长度*/
					raw->fail=true;
					return false;
				}
				len=(uint16_t)kv.first.size();
				if(!raw->write_Bytes(&t,1,1,0)||!raw->write_Bytes(&len,2,1,0)||
				   !raw->write_Bytes(kv.first.data(),1,len,0)||!kv.second->encode(raw)){
					return false;
				}
			}
			return raw->write_Bytes(&end,1,1,0);
		case NBT_INT_ARRAY:
			{
				const nbt_Int_Array *ia=(const nbt_Int_Array*)this;
				if(ia->con.size()>0xFFFFFFFFu){
					raw->fail=true;
					return false;
				}
				uint32_t len32=(uint32_t)ia->con.size();
				return raw->write_Bytes(&len32,4,1,0)&&raw->write_Bytes(ia->con.data(),4,ia->con.size(),0);
			}
		case NBT_LONG_ARRAY:
			{
				const nbt_Long_Array *la=(const nbt_Long_Array*)this;
				if(la->con.size()>0xFFFFFFFFu){
					raw->fail=true;
					return false;
				}
				uint32_t len32=(uint32_t)la->con.size();
				return raw->write_Bytes(&len32,4,1,0)&&raw->write_Bytes(la->con.data(),8,la->con.size(),0);
			}
		default:
			raw->fail=true;
			return false;
	}
}
nbt_Any *nbt_Any::copy()const{/*多态深拷贝；List/Compound的拷贝构造会往下递归*/
	switch(type){
		case NBT_END: return new nbt_Any(NBT_END);
		case NBT_BYTE: return new nbt_Byte(((const nbt_Byte*)this)->con);
		case NBT_SHORT: return new nbt_Short(((const nbt_Short*)this)->con);
		case NBT_INT: return new nbt_Int(((const nbt_Int*)this)->con);
		case NBT_LONG: return new nbt_Long(((const nbt_Long*)this)->con);
		case NBT_FLOAT: return new nbt_Float(((const nbt_Float*)this)->con);
		case NBT_DOUBLE: return new nbt_Double(((const nbt_Double*)this)->con);
		case NBT_BYTE_ARRAY: return new nbt_Byte_Array(((const nbt_Byte_Array*)this)->con);
		case NBT_STRING: return new nbt_String(((const nbt_String*)this)->con);
		case NBT_LIST: return new nbt_List(*(const nbt_List*)this);
		case NBT_COMPOUND: return new nbt_Compound(*(const nbt_Compound*)this);
		case NBT_INT_ARRAY: return new nbt_Int_Array(((const nbt_Int_Array*)this)->con);
		case NBT_LONG_ARRAY: return new nbt_Long_Array(((const nbt_Long_Array*)this)->con);
	}
	return NULL;
}

/*---- 比较：同类型按C++比较；整数/浮点/数组跨精度先转高精度；List/Compound只比相等 ----*/
static int get_NBTFamily(uint8_t t){/*1=整数 2=浮点 3=数组 0=只能比相等*/
	if(t>=NBT_BYTE&&t<=NBT_LONG){
		return 1;
	}
	if(t==NBT_FLOAT||t==NBT_DOUBLE){
		return 2;
	}
	if(t==NBT_BYTE_ARRAY||t==NBT_INT_ARRAY||t==NBT_LONG_ARRAY){
		return 3;
	}
	return 0;
}
static std::partial_ordering ord_I64(int64_t a,int64_t b){
	return a<b?std::partial_ordering::less:(a>b?std::partial_ordering::greater:std::partial_ordering::equivalent);
}
static int64_t get_ArrayValue(const nbt_Any *p,size_t i){
	if(p->type==NBT_BYTE_ARRAY){
		return ((const nbt_Byte_Array*)p)->con[i];
	}
	if(p->type==NBT_INT_ARRAY){
		return ((const nbt_Int_Array*)p)->con[i];
	}
	return ((const nbt_Long_Array*)p)->con[i];
}
static size_t get_ArraySize(const nbt_Any *p){
	if(p->type==NBT_BYTE_ARRAY){
		return ((const nbt_Byte_Array*)p)->con.size();
	}
	if(p->type==NBT_INT_ARRAY){
		return ((const nbt_Int_Array*)p)->con.size();
	}
	return ((const nbt_Long_Array*)p)->con.size();
}
static bool is_ListEqual(const nbt_List *a,const nbt_List *b){
	if(a->con.size()!=b->con.size()){
		return false;
	}
	for(size_t i=0;i<a->con.size();i++){
		const nbt_Any *x=a->con[i],*y=b->con[i];
		if(!x||!y){
			if(x!=y){
				return false;
			}
			continue;
		}
		if(!(*x==*y)){
			return false;
		}
	}
	return true;
}
static bool is_CompoundEqual(const nbt_Compound *a,const nbt_Compound *b){
	if(a->con.size()!=b->con.size()){
		return false;
	}
	for(const auto &kv:a->con){
		auto it=b->con.find(kv.first);
		if(it==b->con.end()){
			return false;
		}
		const nbt_Any *x=kv.second,*y=it->second;
		if(!x||!y){
			if(x!=y){
				return false;
			}
			continue;
		}
		if(!(*x==*y)){
			return false;
		}
	}
	return true;
}
std::partial_ordering nbt_Any::compare(const nbt_Any &b)const{
	if(type==b.type){/*同类型：标量/字符串/数组直接用C++的比较；List/Compound只比相等*/
		switch(type){
			case NBT_END: return std::partial_ordering::equivalent;
			case NBT_BYTE: return ord_I64(((const nbt_Byte*)this)->con,((const nbt_Byte*)&b)->con);
			case NBT_SHORT: return ord_I64(((const nbt_Short*)this)->con,((const nbt_Short*)&b)->con);
			case NBT_INT: return ord_I64(((const nbt_Int*)this)->con,((const nbt_Int*)&b)->con);
			case NBT_LONG: return ord_I64(((const nbt_Long*)this)->con,((const nbt_Long*)&b)->con);
			case NBT_FLOAT: return ((const nbt_Float*)this)->con<=>((const nbt_Float*)&b)->con;
			case NBT_DOUBLE: return ((const nbt_Double*)this)->con<=>((const nbt_Double*)&b)->con;
			case NBT_STRING:{
				int c=((const nbt_String*)this)->con.compare(((const nbt_String*)&b)->con);
				return c<0?std::partial_ordering::less:(c>0?std::partial_ordering::greater:std::partial_ordering::equivalent);
			}
			case NBT_BYTE_ARRAY:{
				std::strong_ordering c=((const nbt_Byte_Array*)this)->con<=>((const nbt_Byte_Array*)&b)->con;
				return c<0?std::partial_ordering::less:(c>0?std::partial_ordering::greater:std::partial_ordering::equivalent);
			}
			case NBT_INT_ARRAY:{
				std::strong_ordering c=((const nbt_Int_Array*)this)->con<=>((const nbt_Int_Array*)&b)->con;
				return c<0?std::partial_ordering::less:(c>0?std::partial_ordering::greater:std::partial_ordering::equivalent);
			}
			case NBT_LONG_ARRAY:{
				std::strong_ordering c=((const nbt_Long_Array*)this)->con<=>((const nbt_Long_Array*)&b)->con;
				return c<0?std::partial_ordering::less:(c>0?std::partial_ordering::greater:std::partial_ordering::equivalent);
			}
			case NBT_LIST:/*元素是指针，不能直接比大小；遍历比相等*/
				return is_ListEqual((const nbt_List*)this,(const nbt_List*)&b)?std::partial_ordering::equivalent:std::partial_ordering::unordered;
			case NBT_COMPOUND:/*顺序无关；只比相等*/
				return is_CompoundEqual((const nbt_Compound*)this,(const nbt_Compound*)&b)?std::partial_ordering::equivalent:std::partial_ordering::unordered;
		}
		return std::partial_ordering::unordered;
	}
	int fa=get_NBTFamily(type),fb=get_NBTFamily(b.type);
	if(fa&&fa==fb){/*同一家族、不同精度：先转高精度再比*/
		if(fa==1){
			int64_t a=as_Long(),c=b.as_Long();
			return ord_I64(a,c);
		}
		if(fa==2){/*只有float→double；float和double都已转成double，值不变*/
			double a=as_Double(),c=b.as_Double();
			return a<=>c;
		}
		size_t na=get_ArraySize(this),nb=get_ArraySize(&b);
		size_t n=na<nb?na:nb;
		for(size_t i=0;i<n;i++){
			std::partial_ordering o=ord_I64(get_ArrayValue(this,i),get_ArrayValue(&b,i));
			if(o!=std::partial_ordering::equivalent){
				return o;
			}
		}
		return na<nb?std::partial_ordering::less:(na>nb?std::partial_ordering::greater:std::partial_ordering::equivalent);
	}
	return std::partial_ordering::unordered;/*不同家族/不可排序的两种类型：没有大小，也不相等*/
}
bool nbt_Any::operator==(const nbt_Any &b)const{
	return compare(b)==std::partial_ordering::equivalent;
}
bool nbt_Any::operator!=(const nbt_Any &b)const{
	return !(*this==b);
}
bool nbt_Any::is_Equal(const nbt_Any *b)const{/*相等true；空指针false*/
	return b&&(*this==*b);
}

/*---- 转换：只提供低精度→高精度 ----*/
bool nbt_Any::can_Convert(uint8_t to)const{
	if(type==to){/*同类型算允许（深拷贝一份）*/
		return true;
	}
	switch(type){
		case NBT_BYTE: return to==NBT_SHORT||to==NBT_INT||to==NBT_LONG;
		case NBT_SHORT: return to==NBT_INT||to==NBT_LONG;
		case NBT_INT: return to==NBT_LONG;
		case NBT_FLOAT: return to==NBT_DOUBLE;
		case NBT_BYTE_ARRAY: return to==NBT_INT_ARRAY||to==NBT_LONG_ARRAY;
		case NBT_INT_ARRAY: return to==NBT_LONG_ARRAY;
		default: return false;/*String/List/Compound/End没有精度转换*/
	}
}
nbt_Any *nbt_Any::convert(uint8_t to)const{
	if(!can_Convert(to)){
		return NULL;
	}
	if(type==to){
		return copy();
	}
	if(type==NBT_BYTE){
		int64_t v=((const nbt_Byte*)this)->con;
		if(to==NBT_SHORT){
			return new nbt_Short((int16_t)v);
		}
		if(to==NBT_INT){
			return new nbt_Int((int32_t)v);
		}
		if(to==NBT_LONG){
			return new nbt_Long(v);
		}
	}else if(type==NBT_SHORT){
		int64_t v=((const nbt_Short*)this)->con;
		if(to==NBT_INT){
			return new nbt_Int((int32_t)v);
		}
		if(to==NBT_LONG){
			return new nbt_Long(v);
		}
	}else if(type==NBT_INT){
		if(to==NBT_LONG){
			return new nbt_Long(((const nbt_Int*)this)->con);
		}
	}else if(type==NBT_FLOAT){
		if(to==NBT_DOUBLE){
			return new nbt_Double(((const nbt_Float*)this)->con);
		}
	}else if(type==NBT_BYTE_ARRAY){
		const std::vector<int8_t> &src=((const nbt_Byte_Array*)this)->con;
		if(to==NBT_INT_ARRAY){
			std::vector<int32_t> v;
			v.reserve(src.size());
			for(int8_t x:src){
				v.push_back(x);
			}
			return new nbt_Int_Array(std::move(v));
		}
		if(to==NBT_LONG_ARRAY){
			std::vector<int64_t> v;
			v.reserve(src.size());
			for(int8_t x:src){
				v.push_back(x);
			}
			return new nbt_Long_Array(std::move(v));
		}
	}else if(type==NBT_INT_ARRAY){
		if(to==NBT_LONG_ARRAY){
			const std::vector<int32_t> &src=((const nbt_Int_Array*)this)->con;
			std::vector<int64_t> v;
			v.reserve(src.size());
			for(int32_t x:src){
				v.push_back(x);
			}
			return new nbt_Long_Array(std::move(v));
		}
	}
	return NULL;
}

/*---- 取值：只做低精度→高精度的读取，不截断 ----*/
int8_t nbt_Any::as_Byte()const{
	return type==NBT_BYTE?((const nbt_Byte*)this)->con:0;
}
int16_t nbt_Any::as_Short()const{
	if(type==NBT_BYTE){
		return (int16_t)((const nbt_Byte*)this)->con;
	}
	return type==NBT_SHORT?((const nbt_Short*)this)->con:0;
}
int32_t nbt_Any::as_Int()const{
	if(type==NBT_BYTE){
		return ((const nbt_Byte*)this)->con;
	}
	if(type==NBT_SHORT){
		return ((const nbt_Short*)this)->con;
	}
	return type==NBT_INT?((const nbt_Int*)this)->con:0;
}
int64_t nbt_Any::as_Long()const{
	if(type==NBT_BYTE){
		return ((const nbt_Byte*)this)->con;
	}
	if(type==NBT_SHORT){
		return ((const nbt_Short*)this)->con;
	}
	if(type==NBT_INT){
		return ((const nbt_Int*)this)->con;
	}
	return type==NBT_LONG?((const nbt_Long*)this)->con:0;
}
float nbt_Any::as_Float()const{
	return type==NBT_FLOAT?((const nbt_Float*)this)->con:0;
}
double nbt_Any::as_Double()const{
	if(type==NBT_FLOAT){
		return ((const nbt_Float*)this)->con;
	}
	return type==NBT_DOUBLE?((const nbt_Double*)this)->con:0;
}
const char *nbt_Any::as_Str()const{
	return type==NBT_STRING?((const nbt_String*)this)->con.c_str():NULL;
}
bool nbt_Any::is(const char *str)const{/*是String且内容相同*/
	return type==NBT_STRING&&str&&((const nbt_String*)this)->con==str;
}

/*---- 打印（SNBT风格） ----*/
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
		case NBT_END:break;
		case NBT_BYTE:printf("%d",(int)((const nbt_Byte*)this)->con);break;
		case NBT_SHORT:printf("%d",(int)((const nbt_Short*)this)->con);break;
		case NBT_INT:printf("%d",(int)((const nbt_Int*)this)->con);break;
		case NBT_LONG:printf("%lld",(long long)((const nbt_Long*)this)->con);break;
		case NBT_FLOAT:printf("%g",(double)((const nbt_Float*)this)->con);break;
		case NBT_DOUBLE:printf("%g",((const nbt_Double*)this)->con);break;
		case NBT_BYTE_ARRAY:{
			const nbt_Byte_Array *con=(const nbt_Byte_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%d%s",(int)con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case NBT_STRING:print_String(((const nbt_String*)this)->con);break;
		case NBT_LIST:{
			const nbt_List *con=(const nbt_List*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				if(con->con[i]){
					con->con[i]->print(depth+1);
				}
				printf("%s",i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case NBT_COMPOUND:{
			const nbt_Compound *con=(const nbt_Compound*)this;
			putchar('{');
			size_t i=0;
			for(const auto &kv:con->con){/*unordered_map的遍历顺序，和文件里的顺序无关*/
				print_String(kv.first);
				putchar(':');
				if(kv.second){
					kv.second->print(depth+1);
				}
				printf("%s",++i<con->con.size()?",":"");
			}
			putchar('}');
			break;
		}
		case NBT_INT_ARRAY:{
			const nbt_Int_Array *con=(const nbt_Int_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%d%s",(int)con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		case NBT_LONG_ARRAY:{
			const nbt_Long_Array *con=(const nbt_Long_Array*)this;
			putchar('[');
			for(size_t i=0;i<con->con.size();i++){
				printf("%lld%s",(long long)con->con[i],i+1<con->con.size()?",":"");
			}
			putchar(']');
			break;
		}
		default:{
			break;
		}
	}
}
void nbt_Any::print()const{
	print(0);
}

/*---- nbt_List：元素是nbt_Any*，类型必须统一 ----*/
void free_AnyVector(std::vector<nbt_Any*> &v){
	for(nbt_Any *p:v){
		if(p){
			p->free();
		}
	}
	v.clear();
}
void free_AnyMap(std::unordered_map<std::string,nbt_Any*> &m){
	for(auto &kv:m){
		if(kv.second){
			kv.second->free();
		}
	}
	m.clear();
}
static uint8_t get_UniformType(const std::vector<nbt_Any*> &v){/*元素类型统一→返回那个类型；空/全NULL→0；不统一→0（调用者当空list）*/
	uint8_t t=NBT_END;
	for(nbt_Any *p:v){
		if(!p){
			continue;
		}
		if(t==NBT_END){
			t=p->type;
		}else if(p->type!=t){
			return NBT_END;
		}
	}
	return t;
}
nbt_List::nbt_List(const std::vector<nbt_Any*> &v):nbt_Any(NBT_LIST),elem(NBT_END){
	elem=get_UniformType(v);/*不统一就是NBT_END（空list）*/
	if(elem==NBT_END){
		return;
	}
	con.reserve(v.size());
	for(nbt_Any *p:v){
		if(p){
			con.push_back(p->copy());/*深拷贝：传入的指针仍归调用者*/
		}
	}
}
nbt_List::nbt_List(std::vector<nbt_Any*> &&v):nbt_Any(NBT_LIST),elem(NBT_END){
	elem=get_UniformType(v);
	if(elem==NBT_END){/*不统一：接管了就全释放，返回空list*/
		free_AnyVector(v);
		return;
	}
	con.reserve(v.size());
	for(nbt_Any *p:v){
		if(p){
			con.push_back(p);
		}
	}
	v.clear();
}
nbt_List::~nbt_List(){
	clear();
}
nbt_List::nbt_List(const nbt_List &o):nbt_Any(NBT_LIST),elem(o.elem){
	con.reserve(o.con.size());
	for(nbt_Any *p:o.con){
		if(p){
			con.push_back(p->copy());
		}
	}
}
nbt_List::nbt_List(nbt_List &&o)noexcept:nbt_Any(NBT_LIST),elem(o.elem),con(std::move(o.con)){
	o.elem=NBT_END;
	o.con.clear();
}
nbt_List &nbt_List::operator=(const nbt_List &o){
	if(this==&o){
		return *this;
	}
	clear();
	elem=o.elem;
	con.reserve(o.con.size());
	for(nbt_Any *p:o.con){
		if(p){
			con.push_back(p->copy());
		}
	}
	return *this;
}
nbt_List &nbt_List::operator=(nbt_List &&o)noexcept{
	if(this==&o){
		return *this;
	}
	clear();
	elem=o.elem;
	con=std::move(o.con);
	o.elem=NBT_END;
	o.con.clear();
	return *this;
}
std::vector<nbt_Any*> nbt_List::data()const{/*深拷贝每个元素；返回的指针归调用者*/
	std::vector<nbt_Any*> out;
	out.reserve(con.size());
	for(nbt_Any *p:con){
		if(p){
			out.push_back(p->copy());
		}
	}
	return out;
}
nbt_Any *nbt_List::at(size_t i)const{
	if(i>=con.size()){
		throw std::out_of_range("nbt_List::at");
	}
	return con[i];
}
nbt_AnyRef nbt_List::operator[](size_t i){
	return nbt_AnyRef(this,i);
}
nbt_AnyRef nbt_List::ref(size_t i){
	return nbt_AnyRef(this,i);
}
bool nbt_List::push_back(nbt_Any *any){/*接管any；类型不符/空指针/End→释放并返回false*/
	if(!any){
		return false;
	}
	for(nbt_Any *p:con){
		if(p==any){/*已经在表里：再塞一次会double free；不释放（它归List）*/
			return false;
		}
	}
	if(any->type==NBT_END){
		any->free();
		return false;
	}
	if(con.empty()){
		if(elem==NBT_END){/*空list：第一个元素定类型*/
			elem=any->type;
		}else if(any->type!=elem){/*显式给了elem（比如空compound list）：类型必须对*/
			any->free();
			return false;
		}
		con.push_back(any);
		return true;
	}
	if(any->type!=elem){
		any->free();
		return false;
	}
	con.push_back(any);
	return true;
}
bool nbt_List::insert(size_t i,nbt_Any *any){
	if(!any){
		return false;
	}
	if(i>con.size()){
		any->free();
		return false;
	}
	for(nbt_Any *p:con){
		if(p==any){/*已经在表里：不能再插一次*/
			return false;
		}
	}
	if(any->type==NBT_END){
		any->free();
		return false;
	}
	if(con.empty()){
		if(elem==NBT_END){
			elem=any->type;
		}else if(any->type!=elem){
			any->free();
			return false;
		}
	}else if(any->type!=elem){
		any->free();
		return false;
	}
	con.insert(con.begin()+(ptrdiff_t)i,any);
	return true;
}
bool nbt_List::set(size_t i,nbt_Any *any){
	if(i>=con.size()){
		if(any){
			any->free();
		}
		return false;
	}
	if(!any){/*NULL=删掉这一格*/
		erase(i);
		return true;
	}
	for(size_t k=0;k<con.size();k++){
		if(con[k]==any){/*同一个指针已经在表里：同一个槽=没事，别的槽=拒绝（防double free）*/
			if(k!=i){
				return false;
			}
			if(any->type==NBT_END){/*End写在原槽上=删掉*/
				erase(i);
			}
			return true;
		}
	}
	if(any->type==NBT_END){/*不在表里的End：释放并删掉这一格*/
		any->free();
		erase(i);
		return true;
	}
	if(any->type!=elem){
		any->free();
		return false;
	}
	if(con[i]){
		con[i]->free();
	}
	con[i]=any;
	return true;
}
void nbt_List::pop_back(){
	if(con.empty()){
		return;
	}
	if(con.back()){
		con.back()->free();
	}
	con.pop_back();
	if(con.empty()){
		elem=NBT_END;
	}
}
void nbt_List::erase(size_t i){
	if(i>=con.size()){
		return;
	}
	if(con[i]){
		con[i]->free();
	}
	con.erase(con.begin()+(ptrdiff_t)i);
	if(con.empty()){
		elem=NBT_END;
	}
}
void nbt_List::clear(){
	free_AnyVector(con);
	elem=NBT_END;
}
nbt_AnyRef &nbt_AnyRef::operator=(nbt_Any *any){
	list->set(i,any);
	return *this;
}

/*---- nbt_Compound：成员是unordered_map<string,nbt_Any*> ----*/
namespace{
struct comp_Copy{/*unordered_map构造函数用：深拷贝每个值*/
	static std::unordered_map<std::string,nbt_Any*> make(const std::unordered_map<std::string,nbt_Any*> &m){
		std::unordered_map<std::string,nbt_Any*> out;
		out.reserve(m.size());
		for(const auto &kv:m){
			if(kv.second&&kv.second->type!=NBT_END){
				out.emplace(kv.first,kv.second->copy());
			}
		}
		return out;
	}
};
}
nbt_Compound::nbt_Compound(const std::unordered_map<std::string,nbt_Any*> &m):nbt_Any(NBT_COMPOUND),con(comp_Copy::make(m)){}
nbt_Compound::nbt_Compound(std::unordered_map<std::string,nbt_Any*> &&m):nbt_Any(NBT_COMPOUND),con(std::move(m)){}
nbt_Compound::~nbt_Compound(){
	clear();
}
nbt_Compound::nbt_Compound(const nbt_Compound &o):nbt_Any(NBT_COMPOUND),con(comp_Copy::make(o.con)){}
nbt_Compound::nbt_Compound(nbt_Compound &&o)noexcept:nbt_Any(NBT_COMPOUND),con(std::move(o.con)){
	o.con.clear();
}
nbt_Compound &nbt_Compound::operator=(const nbt_Compound &o){
	if(this==&o){
		return *this;
	}
	clear();
	con=comp_Copy::make(o.con);
	return *this;
}
nbt_Compound &nbt_Compound::operator=(nbt_Compound &&o)noexcept{
	if(this==&o){
		return *this;
	}
	clear();
	con=std::move(o.con);
	o.con.clear();
	return *this;
}
std::unordered_map<std::string,nbt_Any*> nbt_Compound::data()const{/*深拷贝每个值；返回的指针归调用者*/
	return comp_Copy::make(con);
}
void nbt_Compound::read_Members(nbt_Raw *raw){/*读成员直到End；每个成员是 tag+名字长度+名字+payload*/
	while(1){
		uint8_t type;
		raw->read_Bytes(&type,1,1,0);
		if(type==NBT_END){
			break;
		}
		uint16_t name_len;
		raw->read_Bytes(&name_len,2,1,0);
		std::string name(name_len,0);
		if(name_len){
			raw->read_Bytes(&name[0],1,name_len,0);
		}
		set(name,raw->read_Any(type));/*读坏了给NULL：set就当没有这个成员，fail已经置上*/
	}
}
nbt_Compound *nbt_Compound::read(const char *path){
	nbt_Raw raw=nbt_Raw::load(path);
	if(raw.data.empty()){
		return NULL;
	}
	return raw.read_Root();/*坏数据在read_Root里已经计过数并扔掉了*/
}
nbt_Compound *nbt_Compound::read_Dat(const char *path){
	nbt_Raw raw=nbt_Raw::load_Dat(path);
	if(raw.data.empty()){
		return NULL;
	}
	return raw.read_Root();
}
nbt_Raw nbt_Compound::to_Raw()const{/*整棵树编成文件里的样子：根tag+空名字+成员+End；游标回到0*/
	nbt_Raw raw;
	size_t n=get_ByteSize();
	if(n>SIZE_MAX-3){/*3字节根头+payload先防溢出*/
		raw.fail=true;
		return raw;
	}
	raw.data.resize(3+n);
	byte type=NBT_COMPOUND;
	uint16_t len=0;
	if(!raw.write_Bytes(&type,1,1,0)||!raw.write_Bytes(&len,2,1,0)||!encode(&raw)){
		raw.fail=true;
	}
	raw.pos=0;
	return raw;
}
static bool write_FileAtomic(const char *path,const std::vector<byte> &out){/*先写.tmp再rename，写一半不会把原文件弄坏*/
	if(!path||!path[0]){
		return false;
	}
	std::string tmp=std::string(path)+".tmp";
	FILE *fp=fopen(tmp.c_str(),"wb");
	if(!fp){
		return false;
	}
	size_t ret=out.empty()?0:fwrite(out.data(),1,out.size(),fp);
	bool ok=fclose(fp)==0;
	if(!ok||ret!=out.size()||rename(tmp.c_str(),path)){
		remove(tmp.c_str());
		return false;
	}
	return true;
}
bool nbt_Compound::save(const char *path)const{
	nbt_Raw raw=to_Raw();
	if(raw.fail){
		return false;
	}
	return write_FileAtomic(path,raw.data);
}
bool nbt_Compound::save_Dat(const char *path,int compression)const{
	if(compression<0||compression>2){
		return false;
	}
	nbt_Raw raw=to_Raw();
	if(raw.fail){
		return false;
	}
	std::vector<byte> out;
	if(compression==1){
		out=gzip_Data(raw.data.data(),raw.data.size());
	}else if(compression==2){
		out=deflate_Data(raw.data.data(),raw.data.size());
	}else{
		out=std::move(raw.data);
	}
	if(compression!=0&&out.empty()){
		return false;
	}
	return write_FileAtomic(path,out);
}
nbt_Any *nbt_Compound::get(const std::string &key)const{/*没有这个成员返回NULL*/
	auto it=con.find(key);
	return it==con.end()?NULL:it->second;
}
nbt_Any *nbt_Compound::set(const std::string &key,nbt_Any *any){/*接管any；同名的旧成员删掉；any为NULL/End等于erase*/
	if(!any){
		erase(key);
		return NULL;
	}
	if(any->type==NBT_END){
		auto it=con.find(key);
		nbt_Any *old=it==con.end()?NULL:it->second;
		if(any!=old){/*End指针已经存在别的成员下：别free（它归那个成员）*/
			for(const auto &kv:con){
				if(kv.second==any&&kv.first!=key){
					erase(key);
					return NULL;
				}
			}
		}
		erase(key);
		if(any!=old){
			any->free();
		}
		return NULL;
	}
	auto it=con.find(key);
	if(it==con.end()){
		con[key]=any;
	}else{
		if(it->second==any){/*同一个指针：别free了再存回去*/
			return any;
		}
		if(it->second){
			it->second->free();
		}
		it->second=any;
	}
	return any;
}
size_t nbt_Compound::erase(const std::string &key){
	auto it=con.find(key);
	if(it==con.end()){
		return 0;
	}
	if(it->second){
		it->second->free();
	}
	con.erase(it);
	return 1;
}
void nbt_Compound::clear(){
	free_AnyMap(con);
}
nbt_Int *nbt_Compound::set_Int(const std::string &key,int32_t v){
	nbt_Int *ret=new nbt_Int(v);
	set(key,ret);
	return ret;
}
nbt_Long *nbt_Compound::set_Long(const std::string &key,int64_t v){
	nbt_Long *ret=new nbt_Long(v);
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
