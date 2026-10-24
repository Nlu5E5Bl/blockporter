/*	nbtrw.hpp —— libnbtrw 的全部声明
 *
 *	项目分两部分：
 *	  nbtrw（libnbtrw）     底层库：NBT读写、方块/区块、区域文件、存档目录（就是本头文件）
 *	  blockporter           merge/fixer/rules（方块搬运工）；本头文件不包含它们的实现
 *
 *	nbtrw = NBT Read and Write，只做底层操作：
 *	  nbt.cpp     nbt_Raw（一段字节+游标）、nbt_Any（所有nbt格式的父struct，也当泛型用）及11种tag、
 *	              nbt_List、nbt_Compound；普通dat读写（gzip/zlib/裸数据都认）
 *	  block.cpp   Block（方块id+属性+方块实体，三个nbt结构按值持有）、方块状态属性小工具
 *	  chunk.cpp   Chunk（一个区块，继承nbt_Compound）、Chunk_Cursor、section调色板、计划刻、POI、实体区块检查
 *	  mca.cpp     MCA（一个.mca文件的内存镜像）、zlib压缩解压、整理（消除空段、稠密存储）
 *	  level.cpp   Level（一个存档：dat/mca/chunk缓存、按维度参数和DataVersion定目录、落盘）
 *	  → libnbtrw.so
 *	  nbtrw.cpp   → nbtrw（这套库的demo，不是库的一部分）
 *
 *	nbt结构体的约定（重要）：
 *	  - 数组成员用vector实际存储，仿vector接口；String用string，仿string接口；
 *	    Compound用unordered_map，仿map接口。成员变量con是公开的，直接改它就是在原位改。
 *	  - data() 返回对应C++类型的**深拷贝**（byte→int8_t、float→float、int_array→vector<int32_t>、
 *	    list→vector<nbt_Any*>、compound→unordered_map<string,nbt_Any*>），可以直接拿去构造新结构体；
 *	    从C++类型构造nbt结构体也是全量深拷贝（右值容器则接管指针，不额外复制）。
 *	    List/Compound 的 data() 返回的指针是新拷贝，归调用者；不想深拷贝就直接改 con。
 *	  - copy 是深拷贝：拷贝构造/赋值都深拷贝；多态拷贝用 nbt_Any::copy()（返回堆上的新树）。
 *	  - 比较：同类型按C++比较；整数跨精度先转高精度（byte→short→int→long），float→double；
 *	    数组、String按字典序；List/Compound顺序无关，只能比相等，没有大小（compare返回unordered）。
 *	  - 转换只提供低精度→高精度，不截断：byte/short/int→long、byte/short→int、byte→short、
 *	    float→double、byte_array/int_array→long_array、byte_array→int_array。
 *	  - List的元素类型必须统一：空List的elem=0，第一个元素进来时自动定；加元素时类型不符会被拒绝。
 *
 *	一个虚函数都没加：type在每个结构体里都还在偏移0（没有虚表），(nbt_Compound*)any 这类强转照样成立。
 *	内存全用new/delete，容器用STL，nbt_Raw/MCA自己管自己的内存，不用手动free。
 *	库不记录错误计数、不打错误日志、不exit：成功/失败都用返回值（bool/NULL/空vector）交给上层。
 *	Chunk带serial（全局只增的uint64_t）；判断两个Chunk是不是同一个只看serial，不看地址。
 *	不设人为的大小/深度/缓存上限；只保留NBT/MCA文件格式自身的长度字段限制。
 *	注意：编码时compound成员按unordered_map的遍历顺序写，和MC原始文件的顺序不同（NBT无序，MC照读），
 *	      所以“读进来再写回去”字节不再逐位相同，但 get_ByteSize() 一定不变，逐值比较也一定相同。
 *	      要比两个区块一不一样，用 compare()/==，别比字节。
 *
 *	需要C++23；用了STL所以要用g++链接。
 *	编译：make（见Makefile）。手工编的话：
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -shared nbt.cpp block.cpp chunk.cpp mca.cpp level.cpp -o libnbtrw.so -lz
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic nbtrw.cpp -o nbtrw -L. -lnbtrw -lz -Wl,-rpath,'$ORIGIN'
 */
#ifndef NBTRW_HPP
#define NBTRW_HPP
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <compare>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

typedef uint8_t byte;

/*NBT的tag号（wiki: NBT format）。代码里用名字，别再用0x0a这种数字*/
enum nbt_Type:uint8_t{
	NBT_END=0x00,NBT_BYTE=0x01,NBT_SHORT=0x02,NBT_INT=0x03,NBT_LONG=0x04,
	NBT_FLOAT=0x05,NBT_DOUBLE=0x06,NBT_BYTE_ARRAY=0x07,NBT_STRING=0x08,
	NBT_LIST=0x09,NBT_COMPOUND=0x0a,NBT_INT_ARRAY=0x0b,NBT_LONG_ARRAY=0x0c
};

struct nbt_Raw;struct nbt_Any;struct nbt_Compound;struct nbt_List;struct nbt_AnyRef;
struct Chunk;struct Chunk_Cursor;struct MCA;

/*==================== nbt_Any：所有tag的父struct ====================*/
struct nbt_Any{
	uint8_t type;/*偏移0，不能挪；派生结构的强转全靠它*/
	nbt_Any(uint8_t type=NBT_END):type(type){}
	bool free();/*删掉自己和下面所有节点（没有虚析构，按tag分派）；未知tag返回false*/
	void print(size_t depth)const;void print()const;/*NBT风格的snbt（成员顺序不固定）*/
	size_t get_ByteSize()const;/*序列化字节数，不含tag和名字*/
	bool encode(nbt_Raw *raw)const;/*只写payload，tag和名字由上层写；写不下返回false*/
	nbt_Any *copy()const;/*多态深拷贝：返回堆上的新树，调用者free()；值拷贝用拷贝构造*/

	/*逐值比较/排序。List/Compound只能比相等：相等给equivalent，不等给unordered；
	  不同类型但同属“可扩宽”的家族（整数、浮点、数组）先转高精度再比，别的一律unordered*/
	std::partial_ordering compare(const nbt_Any &b)const;
	bool operator==(const nbt_Any &b)const;
	bool operator!=(const nbt_Any &b)const;
	std::partial_ordering operator<=>(const nbt_Any &b)const{return compare(b);}
	bool is_Equal(const nbt_Any *b)const;/*相等返回true，空指针返回false*/

	/*转换：只提供低精度→高精度（见文件头），不允许返回NULL；同类型算允许（深拷贝一份）*/
	bool can_Convert(uint8_t to)const;
	nbt_Any *convert(uint8_t to)const;
	/*取值便利：只做低精度→高精度的读取，类型不在允许范围就给0/NULL（不截断）*/
	int8_t as_Byte()const;int16_t as_Short()const;int32_t as_Int()const;int64_t as_Long()const;
	float as_Float()const;double as_Double()const;
	const char *as_Str()const;bool is(const char *str)const;
};
static_assert(sizeof(nbt_Any)==1,"nbt_Any不能加虚函数：type必须在偏移0，派生结构的强转才成立");

/*---- 标量：con就是C++类型本身，data()返回它的拷贝 ----*/
struct nbt_Byte:nbt_Any{
	int8_t con;
	nbt_Byte(int8_t con=0):nbt_Any(NBT_BYTE),con(con){}
	int8_t data()const{return con;}
};
struct nbt_Short:nbt_Any{
	int16_t con;
	nbt_Short(int16_t con=0):nbt_Any(NBT_SHORT),con(con){}
	int16_t data()const{return con;}
};
struct nbt_Int:nbt_Any{
	int32_t con;
	nbt_Int(int32_t con=0):nbt_Any(NBT_INT),con(con){}
	int32_t data()const{return con;}
};
struct nbt_Long:nbt_Any{
	int64_t con;
	nbt_Long(int64_t con=0):nbt_Any(NBT_LONG),con(con){}
	int64_t data()const{return con;}
};
struct nbt_Float:nbt_Any{
	float con;
	nbt_Float(float con=0):nbt_Any(NBT_FLOAT),con(con){}
	float data()const{return con;}
};
struct nbt_Double:nbt_Any{
	double con;
	nbt_Double(double con=0):nbt_Any(NBT_DOUBLE),con(con){}
	double data()const{return con;}
};

/*---- String：仿std::string接口；data()按约定返回string（深拷贝），要const char*用c_str() ----*/
struct nbt_String:nbt_Any{
	std::string con;
	nbt_String():nbt_Any(NBT_STRING){}
	nbt_String(const std::string &con):nbt_Any(NBT_STRING),con(con){}
	nbt_String(std::string &&con):nbt_Any(NBT_STRING),con(std::move(con)){}
	nbt_String(const char *con):nbt_Any(NBT_STRING),con(con?con:""){}
	std::string data()const{return con;}
	size_t size()const{return con.size();}size_t length()const{return con.size();}
	bool empty()const{return con.empty();}
	void clear(){con.clear();}void reserve(size_t n){con.reserve(n);}void resize(size_t n){con.resize(n);}
	char &operator[](size_t i){return con[i];}const char &operator[](size_t i)const{return con[i];}
	char &at(size_t i){return con.at(i);}const char &at(size_t i)const{return con.at(i);}
	char &front(){return con.front();}const char &front()const{return con.front();}
	char &back(){return con.back();}const char &back()const{return con.back();}
	const char *c_str()const{return con.c_str();}
	nbt_String &append(const std::string &s){con+=s;return *this;}
	nbt_String &append(const char *s){con+=s;return *this;}
	nbt_String &operator+=(const std::string &s){con+=s;return *this;}
	nbt_String &operator+=(const char *s){con+=s;return *this;}
	nbt_String &operator+=(char c){con+=c;return *this;}
	void push_back(char c){con.push_back(c);}void pop_back(){con.pop_back();}
	nbt_String &insert(size_t pos,const std::string &s){con.insert(pos,s);return *this;}
	nbt_String &erase(size_t pos=0,size_t n=std::string::npos){con.erase(pos,n);return *this;}
	nbt_String &replace(size_t pos,size_t n,const std::string &s){con.replace(pos,n,s);return *this;}
	std::string operator+(const std::string &s)const{return con+s;}
	size_t find(const std::string &s,size_t pos=0)const{return con.find(s,pos);}
	size_t find(const char *s,size_t pos=0)const{return con.find(s,pos);}
	size_t find(char c,size_t pos=0)const{return con.find(c,pos);}
	size_t rfind(const std::string &s,size_t pos=std::string::npos)const{return con.rfind(s,pos);}
	std::string substr(size_t pos=0,size_t n=std::string::npos)const{return con.substr(pos,n);}
	int compare(const std::string &s)const{return con.compare(s);}
	typedef std::string::iterator iterator;typedef std::string::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};

/*---- 数组：仿std::vector接口（data()按约定返回整个vector的深拷贝，不是元素指针） ----*/
struct nbt_Byte_Array:nbt_Any{
	std::vector<int8_t> con;
	nbt_Byte_Array():nbt_Any(NBT_BYTE_ARRAY){}
	nbt_Byte_Array(const std::vector<int8_t> &con):nbt_Any(NBT_BYTE_ARRAY),con(con){}
	nbt_Byte_Array(std::vector<int8_t> &&con):nbt_Any(NBT_BYTE_ARRAY),con(std::move(con)){}
	std::vector<int8_t> data()const{return con;}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	int8_t &operator[](size_t i){return con[i];}const int8_t &operator[](size_t i)const{return con[i];}
	int8_t &at(size_t i){return con.at(i);}const int8_t &at(size_t i)const{return con.at(i);}
	int8_t &front(){return con.front();}const int8_t &front()const{return con.front();}
	int8_t &back(){return con.back();}const int8_t &back()const{return con.back();}
	void push_back(int8_t v){con.push_back(v);}void pop_back(){con.pop_back();}
	void insert(size_t i,int8_t v){con.insert(con.begin()+(ptrdiff_t)i,v);}
	void erase(size_t i){con.erase(con.begin()+(ptrdiff_t)i);}
	void clear(){con.clear();}void resize(size_t n){con.resize(n);}void reserve(size_t n){con.reserve(n);}
	size_t capacity()const{return con.capacity();}void shrink_to_fit(){con.shrink_to_fit();}
	typedef std::vector<int8_t>::iterator iterator;typedef std::vector<int8_t>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};
struct nbt_Int_Array:nbt_Any{
	std::vector<int32_t> con;
	nbt_Int_Array():nbt_Any(NBT_INT_ARRAY){}
	nbt_Int_Array(const std::vector<int32_t> &con):nbt_Any(NBT_INT_ARRAY),con(con){}
	nbt_Int_Array(std::vector<int32_t> &&con):nbt_Any(NBT_INT_ARRAY),con(std::move(con)){}
	std::vector<int32_t> data()const{return con;}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	int32_t &operator[](size_t i){return con[i];}const int32_t &operator[](size_t i)const{return con[i];}
	int32_t &at(size_t i){return con.at(i);}const int32_t &at(size_t i)const{return con.at(i);}
	int32_t &front(){return con.front();}const int32_t &front()const{return con.front();}
	int32_t &back(){return con.back();}const int32_t &back()const{return con.back();}
	void push_back(int32_t v){con.push_back(v);}void pop_back(){con.pop_back();}
	void insert(size_t i,int32_t v){con.insert(con.begin()+(ptrdiff_t)i,v);}
	void erase(size_t i){con.erase(con.begin()+(ptrdiff_t)i);}
	void clear(){con.clear();}void resize(size_t n){con.resize(n);}void reserve(size_t n){con.reserve(n);}
	size_t capacity()const{return con.capacity();}void shrink_to_fit(){con.shrink_to_fit();}
	typedef std::vector<int32_t>::iterator iterator;typedef std::vector<int32_t>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};
struct nbt_Long_Array:nbt_Any{
	std::vector<int64_t> con;
	nbt_Long_Array():nbt_Any(NBT_LONG_ARRAY){}
	nbt_Long_Array(const std::vector<int64_t> &con):nbt_Any(NBT_LONG_ARRAY),con(con){}
	nbt_Long_Array(std::vector<int64_t> &&con):nbt_Any(NBT_LONG_ARRAY),con(std::move(con)){}
	std::vector<int64_t> data()const{return con;}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	int64_t &operator[](size_t i){return con[i];}const int64_t &operator[](size_t i)const{return con[i];}
	int64_t &at(size_t i){return con.at(i);}const int64_t &at(size_t i)const{return con.at(i);}
	int64_t &front(){return con.front();}const int64_t &front()const{return con.front();}
	int64_t &back(){return con.back();}const int64_t &back()const{return con.back();}
	void push_back(int64_t v){con.push_back(v);}void pop_back(){con.pop_back();}
	void insert(size_t i,int64_t v){con.insert(con.begin()+(ptrdiff_t)i,v);}
	void erase(size_t i){con.erase(con.begin()+(ptrdiff_t)i);}
	void clear(){con.clear();}void resize(size_t n){con.resize(n);}void reserve(size_t n){con.reserve(n);}
	size_t capacity()const{return con.capacity();}void shrink_to_fit(){con.shrink_to_fit();}
	typedef std::vector<int64_t>::iterator iterator;typedef std::vector<int64_t>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};

/*---- List：元素是nbt_Any*，类型必须统一；空list的elem=0，第一个元素进来时自动定 ----*/
struct nbt_List:nbt_Any{
	uint8_t elem;/*元素类型；0=还没定（空list）*/
	std::vector<nbt_Any*> con;
	nbt_List(uint8_t elem=NBT_END):nbt_Any(NBT_LIST),elem(elem){}
	nbt_List(const std::vector<nbt_Any*> &v);/*深拷贝；类型不统一→空list（传入的指针仍归调用者）*/
	nbt_List(std::vector<nbt_Any*> &&v);/*接管指针；类型不统一→释放全部、空list*/
	~nbt_List();
	nbt_List(const nbt_List &o);/*深拷贝*/
	nbt_List(nbt_List &&o)noexcept;
	nbt_List &operator=(const nbt_List &o);
	nbt_List &operator=(nbt_List &&o)noexcept;
	std::vector<nbt_Any*> data()const;/*深拷贝每个元素，返回的指针归调用者*/
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	nbt_Any *at(size_t i)const;/*越界抛out_of_range*/
	nbt_Any *operator[](size_t i)const{return con[i];}
	nbt_AnyRef operator[](size_t i);
	nbt_Any *ref(size_t i)const{return con[i];}/*指针上的写法*/
	nbt_AnyRef ref(size_t i);
	nbt_Any *front()const{return con.front();}nbt_Any *back()const{return con.back();}
	bool push_back(nbt_Any *any);/*接管所有权；类型不符/空指针/End→释放并返回false；同一个指针只能进一次*/
	bool insert(size_t i,nbt_Any *any);/*同上*/
	bool set(size_t i,nbt_Any *any);/*改第i个；any为NULL/End=删掉；类型不符→释放any并返回false*/
	void pop_back();/*删掉最后一个（空list不动）*/
	void erase(size_t i);
	void clear();/*全删掉，elem归0*/
	void reserve(size_t n){con.reserve(n);}size_t capacity()const{return con.capacity();}
	void shrink_to_fit(){con.shrink_to_fit();}
	typedef std::vector<nbt_Any*>::iterator iterator;
	typedef std::vector<nbt_Any*>::const_iterator const_iterator;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
};

struct nbt_AnyRef{/*list[i]的代理：赋值会检查类型、删掉旧的；不写就别管它*/
	nbt_List *list;size_t i;
	nbt_AnyRef(nbt_List *list,size_t i):list(list),i(i){}
	nbt_Any *get()const{return list->con[i];}
	operator nbt_Any*()const{return get();}
	template<class T> operator T*()const{return (T*)get();}
	nbt_Any *operator->()const{return get();}
	nbt_Any &operator*()const{return *get();}
	nbt_AnyRef &operator=(nbt_Any *any);
	/*比较直接靠 operator nbt_Any*（别再加成员 ==/!=：和内置指针比较会二义）*/
};

/*---- Compound：仿std::unordered_map接口（顺序无关） ----*/
struct nbt_Compound:nbt_Any{
	std::unordered_map<std::string,nbt_Any*> con;
	nbt_Compound():nbt_Any(NBT_COMPOUND){}
	nbt_Compound(const std::unordered_map<std::string,nbt_Any*> &m);/*深拷贝*/
	nbt_Compound(std::unordered_map<std::string,nbt_Any*> &&m);/*接管指针*/
	~nbt_Compound();
	nbt_Compound(const nbt_Compound &o);/*深拷贝*/
	nbt_Compound(nbt_Compound &&o)noexcept;
	nbt_Compound &operator=(const nbt_Compound &o);
	nbt_Compound &operator=(nbt_Compound &&o)noexcept;
	/*按nbt_Compound*删Chunk时也会走到这两个：忽略size，别让sized delete拿基类大小去释放*/
	void operator delete(void *p)noexcept{::operator delete(p);}
	void operator delete(void *p,size_t)noexcept{::operator delete(p);}
	std::unordered_map<std::string,nbt_Any*> data()const;/*深拷贝每个值，返回的指针归调用者*/
	typedef std::unordered_map<std::string,nbt_Any*>::iterator iterator;
	typedef std::unordered_map<std::string,nbt_Any*>::const_iterator const_iterator;
	typedef std::string key_type;typedef nbt_Any *mapped_type;typedef std::pair<const std::string,nbt_Any*> value_type;
	iterator begin(){return con.begin();}iterator end(){return con.end();}
	const_iterator begin()const{return con.begin();}const_iterator end()const{return con.end();}
	size_t size()const{return con.size();}bool empty()const{return con.empty();}
	size_t count(const std::string &key)const{return con.count(key);}
	bool contains(const std::string &key)const{return con.count(key)!=0;}
	iterator find(const std::string &key){return con.find(key);}
	const_iterator find(const std::string &key)const{return con.find(key);}
	nbt_Any *at(const std::string &key)const{return con.at(key);}/*没有会抛out_of_range*/
	nbt_Any *get(const std::string &key)const;/*没有返回NULL*/
	nbt_Any *set(const std::string &key,nbt_Any *any);/*接管所有权，同名的旧的删掉；any为NULL/End等于erase；一个指针别塞进两个成员*/
	size_t erase(const std::string &key);/*删掉了返回1*/
	void clear();/*全删掉*/
	void reserve(size_t n){con.reserve(n);}
	nbt_Int *set_Int(const std::string &key,int32_t v);
	nbt_Long *set_Long(const std::string &key,int64_t v);
	nbt_String *set_String(const std::string &key,const std::string &str);
	struct Ref{/*c["key"]的代理：读不存在的key不会插空成员，赋值会先删掉旧的*/
		nbt_Compound *self;std::string key;
		Ref(nbt_Compound *self,const std::string &key):self(self),key(key){}
		operator nbt_Any*()const{return self->get(key);}
		template<class T> operator T*()const{return (T*)self->get(key);}
		nbt_Any *get()const{return self->get(key);}
		nbt_Any *operator->()const{return self->get(key);}
		nbt_Any &operator*()const{return *self->get(key);}
		Ref &operator=(nbt_Any *any);
		/*比较直接靠 operator nbt_Any*（别再加成员 ==/!=：和内置指针比较会二义）*/
	};
	Ref operator[](const std::string &key){return Ref(this,key);}
	const nbt_Any *operator[](const std::string &key)const{return get(key);}
	/*指针上的写法：c是nbt_Compound*时 c["k"] 会变成指针下标，用 c->ref("k") 代替(*c)["k"]*/
	Ref ref(const std::string &key){return Ref(this,key);}
	const nbt_Any *ref(const std::string &key)const{return get(key);}
	/*整棵树*/
	void read_Members(nbt_Raw *raw);/*内部：读成员直到End（tag和名字由上层读）*/
	static nbt_Compound *read(const char *path);/*读.nbt文件（根tag+名字自动跳过），失败返回NULL*/
	static nbt_Compound *read_Dat(const char *path);/*level.dat，gzip/zlib/裸数据都认*/
	nbt_Raw to_Raw()const;/*编码成文件里的样子（根tag+空名字），给save/MCA用；失败时raw.fail=true*/
	bool save(const char *path)const;/*编码后落盘（裸NBT）*/
	bool save_Dat(const char *path,int compression=1)const;/*0=裸、1=gzip（默认）、2=zlib*/
};

/*释放 data() 返回的指针容器（调用者接管的那批）*/
void free_AnyVector(std::vector<nbt_Any*> &v);
void free_AnyMap(std::unordered_map<std::string,nbt_Any*> &m);

/*==================== Block：方块（id + 属性 + 方块实体） ====================*/
/*三个nbt结构按值持有：get_Block返回的Block是独立的，set_Block会深拷贝进区块。
  空id＝“读不到”（区块不完整/调色板坏了）；id=="minecraft:air"才是空气。*/
struct Block{
	nbt_String id;
	nbt_Compound properties;
	nbt_Compound block_entities;/*这一格的方块实体（一个compound；没有就是空）*/
	Block();
	Block(nbt_String id,nbt_Compound properties=nbt_Compound(),nbt_Compound block_entities=nbt_Compound());
	Block(const char *name);/*只给id的便利构造*/
	bool valid()const{return !id.con.empty();}/*空id=读不到；空气是有效的*/
	explicit operator bool()const{return valid();}
	std::string_view name()const{return id.con;}/*Name原样*/
	std::string_view spacename()const;/*冒号前那段（它所在的命名空间的名字），没冒号算minecraft*/
	std::string_view basename()const;/*冒号后那段，没冒号就是整个name*/
	std::string strid()const;/*spacename+':'+basename*/
	bool has_Properties()const{return !properties.empty();}
	bool has_BlockEntity()const{return !block_entities.empty();}
	bool operator==(const Block &b)const;
	bool operator!=(const Block &b)const;
};

/*方块状态的小工具（block.cpp）：Name/Properties都从nbt现取，返回的指针都指在树里*/
const char *get_Name(const nbt_Any *b);/*block里的Name原样；不是compound/没有就给空串*/
const char *get_Basename(const nbt_Any *b);/*冒号后面那段；没有冒号就是整个name*/
const char *get_Prop(const nbt_Any *b,const char *key);/*Properties.<key>的字符串值，没有给NULL*/
bool is_PropValue(const nbt_Any *b,const char *key,const char *val);/*属性正好等于val？*/

/*==================== Chunk：一个区块（继承nbt_Compound） ====================*/
struct Chunk_Cursor{/*连着读写同一个区块时的游标：把“sy→section→调色板→data”记住
					   里面的指针都指在树里；换区块、section列表长度变了，它自己会发现重查*/
	uint64_t serial=0;/*0=还没跟任何区块；只和Chunk::serial比较，不看地址*/
	nbt_List *secs=NULL;
	size_t secs_len=0;
	int32_t sy=INT32_MIN;
	nbt_Compound *sec=NULL;
	nbt_Compound *states_sec=NULL;
	size_t states_members=0;
	nbt_Compound *states=NULL;
	nbt_List *palette=NULL;
	nbt_Long_Array *data=NULL;
	size_t data_len=0;
	nbt_List *bes=NULL;
	bool bes_ok=false;
	void reset();/*换区块/自己大改过树就清一下*/
	bool follows(const Chunk *c)const;/*这游标跟的是不是这个区块：serial相同才算，不看地址*/
	nbt_Compound *find_Section(const Chunk *c,int32_t sy);
	nbt_Compound *get_States(nbt_Compound *section);/*section的block_states（连palette/data一起记住）*/
	nbt_List *get_BlockEntities(const Chunk *c);
};

/*section级的方块状态（section本身就是个compound，和Chunk无关）；带cur的版本用游标少查几次*/
nbt_Any *get_BlockData(nbt_Compound *section,int lx,int ly,int lz);
nbt_Any *get_BlockData(nbt_Compound *section,Chunk_Cursor *cur,int lx,int ly,int lz);
bool set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz);
bool set_BlockData(nbt_Compound *section,nbt_Any *block,int lx,int ly,int lz,Chunk_Cursor *cur);
bool is_SectionConst(nbt_Compound *section,Chunk_Cursor *cur,nbt_Any **block);/*整段都是一个方块？空气算NULL*/

struct Chunk:nbt_Compound{
	uint64_t serial=0;/*区块身份号：全局只增不减（从1开始），每新建/复制/读一个Chunk换新号；0=无效*/
	Chunk();
	Chunk(int32_t cx,int32_t cz);/*手动新建：只写xPos/zPos；Status/sections要自己补，没Status仍算未生成*/
	explicit Chunk(const nbt_Compound &c);/*深拷贝一份；换一个新serial（游标靠它认区块）*/
	explicit Chunk(nbt_Compound &&c);
	Chunk(const std::unordered_map<std::string,nbt_Any*> &m);/*按nbt_Compound的方式构造*/
	Chunk(std::unordered_map<std::string,nbt_Any*> &&m);
	Chunk(const Chunk &o);
	Chunk(Chunk &&o)noexcept;
	Chunk &operator=(const Chunk &o);
	Chunk &operator=(Chunk &&o)noexcept;
	static Chunk *read(nbt_Raw *raw);/*从一个NBT流读一个区块*/
	static Chunk *read(const char *path);/*读单区块文件，失败返回NULL*/
	bool is_Full()const;/*Status=="minecraft:full"？没有Status也算不完整（和merge的规矩一致）*/
	nbt_Any *get_Section(int32_t sy)const;/*线性扫；连着读同一段就给游标*/
	nbt_Any *get_Section(int32_t sy,Chunk_Cursor *cur)const;
	int format_Chunk();/*整理区块：同一Y有多段时只留列表里最前面的那段（其余释放），返回删掉的段数；
	                    sections不是List返回-1。只做这一件事；调用后原来的Chunk_Cursor作废，要reset*/
	/*x,z相对0~15，y是世界绝对Y。区块不完整→返回空id的Block，不读也不写*/
	Block get_Block(int x,int y,int z)const;
	Block get_Block(int x,int y,int z,Chunk_Cursor *cur)const;
	bool set_Block(const Block &block,int x,int y,int z);/*返回是否成功*/
	bool set_Block(const Block &block,int x,int y,int z,Chunk_Cursor *cur);
	/*方块实体：entity里原来的x/y/z一律丢弃，按区块xPos/zPos和放置坐标重算；entity为空=删掉这一格的方块实体*/
	bool set_BlockEntity(const nbt_Compound &entity,int x,int y,int z);
	int erase_BlockEntity(int x,int y,int z);/*删掉了几条*/
	nbt_Compound *get_BlockEntity(int x,int y,int z)const;/*指在树里；没有返回NULL*/
};/*Chunk只许加trivial字段：free()是按nbt_Compound*删的，别让Chunk多出需要析构的成员*/

/*“这个区块生成完了没有/是不是空壳”两个判断（chunk.cpp）*/
enum Chunk_Full_Status{CHUNK_NO_STATUS=-1,CHUNK_NOT_FULL=0,CHUNK_FULL=1};
bool has_Block(const Chunk *c);/*有任意一个非空气方块？形状认不出来时也给true，宁可信其有*/
Chunk_Full_Status get_ChunkStatus(const Chunk *c);/*Status=="minecraft:full"？没有Status字段=CHUNK_NO_STATUS*/

/*---- 计划刻（block_ticks / fluid_ticks）：区块里“到点还要再判一次”的方块/流体 ----
  元素是compound：i:String、x/y/z:Int（绝对世界坐标）、t:Int（还有几tick，可负）、p:Int（优先级）。
  表不存在＝0条；表在但元素形状不对＝坏数据（返回-1，调用者别去改它）。fluid=0是block_ticks，1是fluid_ticks。*/
struct Chunk_Tick{
	std::string_view type;/*指在树里，树没了就作废*/
	int32_t x=0,y=0,z=0,delay=0,priority=0;
};
int count_ChunkTicks(const nbt_Compound *chunk,int fluid);/*几条；-1=表形状不对*/
bool get_ChunkTick(const nbt_Compound *chunk,int fluid,int i,Chunk_Tick *out);/*true=取到；false=形状不对*/
int del_ChunkTick(nbt_Compound *chunk,int fluid,int32_t x,int32_t y,int32_t z);/*删了几条；-1=坏表没动*/
bool add_ChunkTick(nbt_Compound *chunk,int fluid,const char *type,int32_t x,int32_t y,int32_t z,
	int32_t delay,int32_t priority);/*true=加了，false=没加/坏表*/
typedef int (*Chunk_Tick_Keep_Fn)(void *ud,const Chunk_Tick *t);
int filter_ChunkTick(nbt_Compound *chunk,int fluid,Chunk_Tick_Keep_Fn keep,void *ud);
	/*keep返回0的刻删掉；返回删了几条；-1=坏表，一条没动*/

/*==================== MCA：一个.mca文件的内存镜像 ====================*/
struct MCA{
	std::vector<byte> data;
	bool bad=false;/*文件**存在但读不出来**：data可能是空的，但绝不能说“这个区域没有区块”*/
	std::string path;/*从文件构造时记下来，save()不带参数时用*/
	MCA();/*空区域：8KB表头，可以直接往里写区块*/
	explicit MCA(const char *path);/*读文件；文件不在=空镜像（bad=false），文件坏=bad=true*/
	explicit MCA(const std::string &path);
	static MCA read(const char *path){return MCA(path);}
	static MCA empty(){return MCA();}
	Chunk *get_Chunk(uint32_t x,uint32_t z)const;/*读出来的Chunk要自己free；读不到返回NULL*/
	nbt_Compound *get_Compound(uint32_t x,uint32_t z)const;/*实体/POI那种裸compound*/
	bool set_Chunk(const Chunk &chunk,uint32_t x,uint32_t z);/*写进内存镜像，返回是否成功*/
	bool set_Chunk(const Chunk &chunk);/*坐标从chunk自己的xPos/zPos取（必须是Int）*/
	bool set_Compound(const nbt_Compound &chunk,uint32_t x,uint32_t z);/*实体/POI区块用*/
	bool has_Chunk(uint32_t x,uint32_t z)const;
	bool erase_Chunk(uint32_t x,uint32_t z);/*删掉表头这一格；删了返回true*/
	bool is_Empty()const;/*表头一格都没有*/
	/*整理：按8K头索引顺序（0,0 < 1,0 < … < 31,0 < 0,1 < … < 31,31）重排数据区，
	  消除空洞，使区块一个接一个稠密存储。只改内存，不落盘；坏区域/坏表头返回false且一个字节不动。*/
	bool format();
	bool save(const char *path)const;/*先写.tmp再rename；bad时拒绝写*/
	bool save()const{return path.empty()?false:save(path.c_str());}
};
std::string get_MCAPath(const char *dir,int32_t x,int32_t z);/*拼dir/r.x.z.mca*/

/*==================== POI（poi/目录里的方块索引）和实体区块 ====================*/
const char *get_POIType(const nbt_Any *block);/*方块状态→POI类型全名；不是POI/认不出返回NULL*/
int32_t get_POITicketMax(const char *type);/*这个POI类型最多几张票*/
bool is_POIShapeOk(const nbt_Compound *chunk);/*容器形状对不对；不对返回false（整块跳过，别当空）*/
int count_POI(const nbt_Compound *chunk);
bool get_POIAt(const nbt_Compound *chunk,int idx,int32_t *x,int32_t *y,int32_t *z,const char **type,int32_t *free_tickets);
const nbt_Any *get_POI(const nbt_Compound *chunk,int32_t x,int32_t y,int32_t z);
bool set_POI(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z,const char *type,int32_t free_tickets);
int del_POI(nbt_Compound *chunk,int32_t x,int32_t y,int32_t z);
int clean_POI(nbt_Compound *chunk);
bool check_Entity(const nbt_Compound *chunk,int32_t cx,int32_t cz);

/*==================== Level：一个存档（所有维度） ====================*/
/*wiki: “世界（World），又称存档（Level），是指游戏内所有维度组成的游戏空间。”
  所以 Level 不绑定维度：构造只给存档目录，每个读写操作都带一个 dimension 参数。
  维度参数有两种写法（大小写敏感，必须完全匹配）：
    · 原版维度名：overworld / the_nether / the_end，可带 minecraft: 前缀；
      按 DataVersion 自动选布局（新：dimensions/minecraft/<name>/，老：region/、DIM-1/、DIM1/）；
    · 其它字符串：当作相对存档根的维度目录（如 DIM-1、dimensions/minecraft/custom），
      后面自动拼 region/、entities/、poi/；不许绝对路径、不许 ..、不许空路径段。
  存档结构在26.1快照6（DataVersion 4774）改过一次：维度从region/、DIM-1/、DIM1/ 挪到
  dimensions/minecraft/<dim>/region/。Level按DataVersion严格判，不按目录存在与否猜。*/
#define DAT_NEW_LAYOUT 4774
struct Level{
	/*基本状态（存档级，与维度无关）*/
	std::string path;/*存档目录（结尾不带/）*/
	int32_t data_version=0;/*level.dat里的Data.DataVersion；读不到是0，看dat_ok*/
	bool dat_ok=false;/*level.dat读到了、DataVersion也在？false表示这目录不像存档*/
	bool new_layout=false;/*DataVersion>=DAT_NEW_LAYOUT：用dimensions/新布局，否则老布局*/
	std::string version_name;/*level.dat里Version.Name，只用来打印*/

	/*缓存：dat/mca/chunk都在这里自动管理；满了按LRU落盘再扔，调用者不用自己实现缓存。
	  cache_cnt=区块缓存数，region_cnt=区域镜像数，dat_cnt=dat缓存数；不设上限。
	  chunk/MCA 按“解析后的维度目录 + 坐标”分桶，所以不同维度同坐标不会串。*/
	size_t cache_cnt=64,region_cnt=8,dat_cnt=16;
	uint64_t use_cnt=0;
	struct Chunk_Entry{
		std::string dimension;/*解析后的维度目录（空=老布局主世界根目录）*/
		int32_t cx=0,cz=0;
		Chunk *chunk=NULL;
		bool changed=false;
		int pin=0;/*pin>0时LRU不淘汰这一格；调用者拿着的chunk指针在unpin之前一直有效*/
		int why=0;/*chunk==NULL时是GET_NO_CHUNK/GET_BAD_CHUNK；缓存住，别每次都猜*/
		Chunk_Cursor cur;
		uint64_t use=0;
	};
	struct MCA_Entry{
		std::string dimension;/*解析后的维度目录*/
		int32_t rx=0,rz=0;
		MCA mca;
		bool changed=false;
		uint64_t use=0;
	};
	struct Dat_Entry{
		std::string rel;
		nbt_Compound *dat=NULL;
		bool changed=false;
		int compression=1;/*0=裸、1=gzip、2=zlib（读进来是什么，写回去还是什么）*/
		uint64_t use=0;
	};
	/*第一层键是解析后的维度目录；第二层是区块/区域坐标。*/
	std::unordered_map<std::string,std::unordered_map<uint64_t,Chunk_Entry>> chunks;
	std::unordered_map<std::string,std::unordered_map<uint64_t,MCA_Entry>> mcas;
	std::unordered_map<std::string,Dat_Entry> dats;

	/*get_Chunk/get_MCA最近一次为什么没读到（成功时是GET_OK）*/
	enum{GET_OK=0,GET_NO_FILE=1,GET_NO_CHUNK=2,GET_BAD_FILE=3,GET_BAD_CHUNK=4,GET_ARG=5,GET_CACHE=6};
	int miss_why=GET_OK;

	/*存档目录；cache_cnt/region_cnt/dat_cnt分别是chunk/MCA/dat缓存容量，0按默认值*/
	Level(const char *path,size_t cache_cnt=64,size_t region_cnt=8,size_t dat_cnt=16);
	Level(const Level &)=delete;/*缓存里是裸指针：不许拷贝，免得double free*/
	Level &operator=(const Level &)=delete;
	~Level();/*自动close*/
	bool close();/*落盘+把缓存全放掉；可以重复调，之后还能接着用；返回落盘是否全成功*/
	bool save();/*脏chunk→MCA，脏MCA→文件，脏dat→文件；返回是否全成功*/

	/*维度解析（不读盘，只看名字/路径和 new_layout）*/
	static bool is_DimensionName(const char *dimension);/*是不是三个原版维度名之一（可带 minecraft:）*/
	std::optional<std::string> get_DimensionBase(const char *dimension) const;
		/*维度参数→相对存档根的维度目录；空字符串=老布局主世界；无效返回 nullopt*/
	std::string get_DimensionPath(const char *dimension,const char *kind) const;
		/*维度参数+kind(region/entities/poi)→相对存档根的路径；无效返回空串*/

	/*坐标都是世界绝对坐标。返回的指针都指在缓存里：下一次取别的对象、save()、close()后可能失效；
	  要留着就copy()。Level自己按LRU落盘，调用者不要另外实现缓存。*/
	MCA *get_MCA(const char *dimension,int32_t rx,int32_t rz);/*没有这个区域文件返回NULL；文件在但读不出来返回bad=true的对象（别写它）*/
	bool set_MCA(const char *dimension,int32_t rx,int32_t rz,const MCA &mca);/*放进缓存并标脏（没有就建）*/
	bool format_MCA(const char *dimension,int32_t rx,int32_t rz);/*整理这个区域（内存里），标脏；没有/坏了返回false*/
	Chunk *get_Chunk(const char *dimension,int32_t cx,int32_t cz);/*没有返回NULL*/
	/*钉住一个区块：返回的指针在unpin之前不会被LRU淘汰（可以安全地跨多次get_Block/邻居读取使用）。
	   缓存腾位置时只淘汰pin=0的；缓冲区全被钉住时get_Chunk/pin会失败（GET_CACHE），不会硬抢。
	   用完必须unpin_Chunk；close()/析构前必须全部unpin。*/
	Chunk *pin_Chunk(const char *dimension,int32_t cx,int32_t cz);
	bool unpin_Chunk(const char *dimension,int32_t cx,int32_t cz);
	bool erase_Chunk(const char *dimension,int32_t cx,int32_t cz);/*把区块从MCA表头删掉（fixer用）；本来没有也算成功*/
	bool set_Chunk(const char *dimension,int32_t cx,int32_t cz,const Chunk &chunk);/*深拷贝进缓存并标脏；同一个serial不重复拷*/
	bool set_Chunk(const char *dimension,const Chunk &chunk);/*坐标从chunk自己的xPos/zPos取（必须是Int）*/
	Block get_Block(const char *dimension,int x,int y,int z);/*读不到返回空id的Block*/
	bool set_Block(const char *dimension,int x,int y,int z,const Block &block);/*返回是否成功；未生成的区块不写*/
	nbt_Compound *get_Dat(const char *rel);/*rel是相对存档根的路径，如"level.dat"；没有返回NULL*/
	bool set_Dat(const char *rel,const nbt_Compound &dat);/*深拷贝进缓存并标脏*/
	/*下面几个是内部用的，指针下次取就可能作废*/
	MCA_Entry *get_MCAEntry(const char *dimension,int32_t rx,int32_t rz,int create);
	Chunk_Entry *get_ChunkEntry(const char *dimension,int32_t cx,int32_t cz,int create);
	MCA_Entry *get_MCAEntryBase(const std::string &base,int32_t rx,int32_t rz,int create);
	Chunk_Entry *get_ChunkEntryBase(const std::string &base,int32_t cx,int32_t cz,int create);
	Dat_Entry *get_DatEntry(const char *rel,int create);
	bool flush_Chunk(Chunk_Entry &e);bool flush_MCA(MCA_Entry &e);bool flush_Dat(Dat_Entry &e);
};

/*==================== nbt_Raw：一段NBT字节+游标 ====================*/
struct nbt_Raw{
	std::vector<byte> data;size_t pos=0;
	bool fail=false;/*读到/写到坏数据了：这一块作废，读出来的入口会给NULL*/
	nbt_Raw(){}
	nbt_Raw(const std::vector<byte> &src):data(src){}
	void read_Bytes(void *ret,size_t size,size_t len,bool direction=false);/*direction=false时元素内反序（大端）*/
	bool can_Read(size_t need);/*读之前先看剩下的字节够不够*/
	bool write_Bytes(const void *src,size_t size,size_t len,bool direction=false);/*写不下返回false并置fail*/
	nbt_Any *read_Any(uint8_t type);/*建树；坏tag返回NULL*/
	nbt_Byte *read_Byte();nbt_Short *read_Short();nbt_Int *read_Int();nbt_Long *read_Long();
	nbt_Float *read_Float();nbt_Double *read_Double();nbt_Byte_Array *read_Byte_Array();nbt_String *read_String();
	nbt_List *read_List();nbt_Compound *read_Compound();nbt_Int_Array *read_Int_Array();nbt_Long_Array *read_Long_Array();
	bool read_RootHeader();/*跳过根tag+名字，检查确实是compound*/
	nbt_Compound *read_Root();/*读一个根compound；坏数据返回NULL*/
	static nbt_Raw load(const char *path);/*读整个文件；失败返回空*/
	static nbt_Raw load_Dat(const char *path);/*level.dat，gzip/zlib/裸数据都认*/
	bool save(const char *path)const;/*写data.size()个字节*/
};

/*zlib/gzip和文件小工具（nbt.cpp/mca.cpp）*/
std::vector<byte> inflate_Data(const byte *in,size_t in_len);/*gzip/zlib都认；出错返回空*/
std::vector<byte> deflate_Data(const byte *in,size_t in_len);/*zlib默认级别*/
std::vector<byte> gzip_Data(const byte *in,size_t in_len);/*gzip（level.dat用）*/
bool read_File(const char *path,std::vector<byte> &out);/*整个文件读进out；不是普通文件返回false*/
int get_DatCompression(const char *path);/*0=裸、1=gzip、2=zlib、-1=读不到；给set_Dat原样写回用*/

/*世界Y→段号（y/16，向下取整）；y=INT32_MIN时也不溢出*/
inline int32_t get_SectionY(int32_t y){
	return (int32_t)(((int64_t)y>=0)?((int64_t)y/16):(((int64_t)y-15)/16));
}

/*命令行上的整数参数：整串必须正好是一个int32十进制数，成功返回true*/
inline bool parse_Int(const char *s,int32_t *out){
	if(!s||!*s){
		return false;
	}
	errno=0;
	char *end=NULL;
	long long v=strtol(s,&end,10);
	if(errno||!end||end==s||*end||v<INT32_MIN||v>INT32_MAX){
		return false;
	}
	*out=(int32_t)v;
	return true;
}
#endif
