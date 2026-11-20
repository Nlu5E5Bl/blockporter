/*	nbtrw.hpp —— libnbtrw 的全部声明
 *
 *	项目分两部分：
 *	  nbtrw（libnbtrw）     底层库：NBT读写、方块/区块、区域文件、存档目录（就是本头文件）
 *	  blockporter           merge/fixer/rules（方块搬运工）；本头文件不包含它们的实现
 *
 *	nbtrw = NBT Read and Write，只做底层操作：
 *	  nbt.cpp     nbt_Raw（一段字节+游标）、nbt_Any（所有nbt格式的父struct，也当泛型用）及11种tag、
 *	              nbt_List、nbt_Compound；普通dat读写（gzip/zlib/裸数据都认）
 *	  atom.cpp    游戏的最小单位：Block（方块id+属性+方块实体，三个nbt结构按值持有）、方块状态属性小工具、
 *	              POI（一条兴趣点记录）、Entity（一个实体）；都按值持有，和容器无关
 *	  chunk.cpp   Chunk（一个区块：按值持有的 nbt_Compound + 索引）、section调色板、计划刻、
 *	              POI 区块和实体表的读写（get/set/erase_POI、get/add/erase_ChunkEntit*，直接对 nbt_Compound）
 *	  mca.cpp     MCA（一个.mca文件的内存镜像，读写的是 nbt_Compound；外置 .mcc）、zlib/gzip/lz4 压缩解压、整理（消除空段、稠密存储）
 *	  level.cpp   Level（一个存档：dat/mca/chunk缓存（按值持有，都能pin）、按维度参数和DataVersion定目录、落盘）
 *	  → libnbtrw.so
 *	  nbtrw.cpp   → nbtrw（这套库的demo，不是库的一部分）
 *
 *	nbt结构体的约定（重要）：
 *	  - 数组成员用vector实际存储，仿vector接口；String用string，仿string接口；
 *	    Compound用unordered_map，仿map接口。成员变量con是公开的，直接改它就是在原位改。
 *	  - data() 返回指向 con 的指针（不拷贝）：int8_t*、std::string*、std::vector<int32_t>*、
 *	    std::vector<nbt_Any*>*、std::unordered_map<std::string,nbt_Any*>*；const 对象给 const 指针。
 *	  - 拷贝都是深拷贝：拷贝构造/赋值深拷贝（移动构造/赋值接管，不复制）；多态拷贝用 nbt_Any::copy()（返回堆上的新树）。
 *	    nbt_Any 本身不能按值拷（会切片，拷贝构造是 protected），要拷就用 copy()。
 *	    从C++类型构造nbt结构体也是全量深拷贝（右值容器则接管指针，不额外复制）。
 *	  - 取值：nbt_Any 的 as_Byte/as_Short/as_Int/as_Long/as_Float/as_Double/as_String/as_Byte_Array/as_Int_Array/as_Long_Array
 *	    是函数，只做低精度→高精度，类型不在允许范围给 std::nullopt（“空值”，不截断、不给0）。
 *	    具体类型（nbt_Int 这种）可以隐式转成自己的C++类型：nbt_Int→int32_t、nbt_String→const std::string&、
 *	    nbt_Byte_Array→const std::vector<int8_t>&（数组还能隐式转成更高精度的 vector：byte→int/long、int→long）。
 *	    标量只给“转成自己那个类型”一个隐式转换（再往宽转交给C++自己的整数提升），给多个会让 a+b 这种写法二义；
 *	    注意C++也允许 int64_t→int8_t 这种隐式截断，标量要宽转窄编译器不拦，别这么写。
 *	  - 比较：== 和 <=>（!= 由C++20从 == 推出来）。同类型按C++比较；整数跨精度先转高精度（byte→short→int→long），
 *	    float→double；数组、String按字典序；List/Compound顺序无关，只能比相等，没有大小（<=> 给 unordered）。
 *	    浮点两边都是 NaN 算相等（比的是“存的是不是同一个值”：带 NaN 的树和自己的拷贝要相等；游戏的 FloatTag/DoubleTag 也是）。
 *	    nbt_String 还能直接和字符串比（s=="minecraft:air"）。
 *	  - 转换只提供低精度→高精度，不截断：byte/short/int→long、byte/short→int、byte→short、
 *	    float→double、byte_array/int_array→long_array、byte_array→int_array。
 *	  - List的元素类型必须统一：空List的elem=0，第一个元素进来时自动定；加元素时类型不符会被拒绝。
 *
 *	一个虚函数都没加：type在每个结构体里都还在偏移0（没有虚表），(nbt_Compound*)any 这类强转照样成立。
 *	内存全用new/delete，容器用STL，nbt_Raw/MCA自己管自己的内存，不用手动free。
 *	库不记录错误计数、不打错误日志、不exit：成功/失败都用返回值（bool/NULL/nullopt/空vector）交给上层。
 *	不设人为的大小/深度/缓存上限；只保留NBT/MCA文件格式自身的长度字段限制。
 *	注意：编码时compound成员按unordered_map的遍历顺序写，和MC原始文件的顺序不同（NBT无序，MC照读），
 *	      所以“读进来再写回去”字节不再逐位相同，但 get_ByteSize() 一定不变，逐值比较也一定相同。
 *	      要比两个区块一不一样，用 ==，别比字节。
 *
 *	需要C++23；用了STL所以要用g++链接。
 *	编译：make nbtrw 只编库和 demo，make 全编（见 Makefile）。手工编的话：
 *	      g++ -std=c++23 -O2 -Wall -Wextra -pedantic -fPIC -shared nbt.cpp atom.cpp chunk.cpp mca.cpp level.cpp -o libnbtrw.so -lz -llz4 -lxxhash
 *	      （要装 zlib-devel、lz4-devel、xxhash-devel）
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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
struct Chunk;struct MCA;

/*==================== nbt_Any：所有tag的父struct ====================*/
struct nbt_Any{
	uint8_t type;/*偏移0，不能挪；派生结构的强转全靠它*/
	explicit nbt_Any(uint8_t type=NBT_END):type(type){}
	bool free();/*删掉自己和下面所有节点（没有虚析构，按tag分派）；未知tag返回false*/
	void print(size_t depth)const;void print()const;/*NBT风格的snbt（成员顺序不固定）*/
	size_t get_ByteSize()const;/*序列化字节数，不含tag和名字*/
	bool encode(nbt_Raw *raw)const;/*只写payload，tag和名字由上层写；写不下返回false*/
	nbt_Any *copy()const;/*多态深拷贝：返回堆上的新树，调用者free()；值拷贝用派生类型的拷贝构造*/

	/*逐值比较/排序。List/Compound只能比相等：相等给equivalent，不等给unordered；
	  不同类型但同属“可扩宽”的家族（整数、浮点、数组）先转高精度再比，别的一律unordered*/
	std::partial_ordering operator<=>(const nbt_Any &b)const;
	bool operator==(const nbt_Any &b)const;

	/*转换：只提供低精度→高精度（见文件头），不允许时返回NULL；同类型算允许（深拷贝一份）*/
	bool can_Convert(uint8_t to)const;
	nbt_Any *convert(uint8_t to)const;
	/*取值：只做低精度→高精度的读取，类型不在允许范围给 std::nullopt（不截断）。
	  as_String 给的 string_view 指在树里（节点在它就在）；数组给的是新拷贝*/
	std::optional<int8_t> as_Byte()const;std::optional<int16_t> as_Short()const;
	std::optional<int32_t> as_Int()const;std::optional<int64_t> as_Long()const;
	std::optional<float> as_Float()const;std::optional<double> as_Double()const;
	std::optional<std::string_view> as_String()const;
	std::optional<std::vector<int8_t>> as_Byte_Array()const;
	std::optional<std::vector<int32_t>> as_Int_Array()const;
	std::optional<std::vector<int64_t>> as_Long_Array()const;
protected:
	nbt_Any(const nbt_Any &)=default;/*只给派生类用：nbt_Any 按值拷会切片*/
	nbt_Any &operator=(const nbt_Any &)=default;
};
static_assert(sizeof(nbt_Any)==1,"nbt_Any不能加虚函数：type必须在偏移0，派生结构的强转才成立");

/*---- 标量：con就是C++类型本身；data()给 con 的指针；能隐式转成自己的C++类型 ----*/
struct nbt_Byte:nbt_Any{
	int8_t con;
	nbt_Byte(int8_t con=0):nbt_Any(NBT_BYTE),con(con){}
	int8_t *data(){return &con;}const int8_t *data()const{return &con;}
	operator int8_t()const{return con;}
};
struct nbt_Short:nbt_Any{
	int16_t con;
	nbt_Short(int16_t con=0):nbt_Any(NBT_SHORT),con(con){}
	int16_t *data(){return &con;}const int16_t *data()const{return &con;}
	operator int16_t()const{return con;}
};
struct nbt_Int:nbt_Any{
	int32_t con;
	nbt_Int(int32_t con=0):nbt_Any(NBT_INT),con(con){}
	int32_t *data(){return &con;}const int32_t *data()const{return &con;}
	operator int32_t()const{return con;}
};
struct nbt_Long:nbt_Any{
	int64_t con;
	nbt_Long(int64_t con=0):nbt_Any(NBT_LONG),con(con){}
	int64_t *data(){return &con;}const int64_t *data()const{return &con;}
	operator int64_t()const{return con;}
};
struct nbt_Float:nbt_Any{
	float con;
	nbt_Float(float con=0):nbt_Any(NBT_FLOAT),con(con){}
	float *data(){return &con;}const float *data()const{return &con;}
	operator float()const{return con;}
};
struct nbt_Double:nbt_Any{
	double con;
	nbt_Double(double con=0):nbt_Any(NBT_DOUBLE),con(con){}
	double *data(){return &con;}const double *data()const{return &con;}
	operator double()const{return con;}
};

/*---- String：仿std::string接口；data()给 con（std::string）的指针，要const char*用c_str() ----*/
struct nbt_String:nbt_Any{
	std::string con;
	nbt_String():nbt_Any(NBT_STRING){}
	/*从字符串造节点要显式写：不然 s=="x" 里的 "x" 既能变 nbt_String 又能变 string_view，二义*/
	explicit nbt_String(const std::string &con):nbt_Any(NBT_STRING),con(con){}
	explicit nbt_String(std::string &&con):nbt_Any(NBT_STRING),con(std::move(con)){}
	explicit nbt_String(const char *con):nbt_Any(NBT_STRING),con(con?con:""){}
	std::string *data(){return &con;}const std::string *data()const{return &con;}
	operator const std::string&()const{return con;}
	using nbt_Any::operator==;/*和别的nbt比还是 nbt_Any 的 ==（下面两个会把它藏住，要拿回来）*/
	bool operator==(const nbt_String &o)const{return con==o.con;}/*同类型：不写这个，C++20 的反向候选会二义*/
	bool operator==(std::string_view s)const{return con==s;}/*和普通字符串比*/
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

/*---- 数组：仿std::vector接口；data()给 con（整个vector）的指针，不是元素指针；能隐式转成自己的vector，
  也能隐式转成更高精度的vector（新拷贝） ----*/
struct nbt_Byte_Array:nbt_Any{
	std::vector<int8_t> con;
	nbt_Byte_Array():nbt_Any(NBT_BYTE_ARRAY){}
	nbt_Byte_Array(const std::vector<int8_t> &con):nbt_Any(NBT_BYTE_ARRAY),con(con){}
	nbt_Byte_Array(std::vector<int8_t> &&con):nbt_Any(NBT_BYTE_ARRAY),con(std::move(con)){}
	std::vector<int8_t> *data(){return &con;}const std::vector<int8_t> *data()const{return &con;}
	operator const std::vector<int8_t>&()const{return con;}
	operator std::vector<int32_t>()const{return std::vector<int32_t>(con.begin(),con.end());}
	operator std::vector<int64_t>()const{return std::vector<int64_t>(con.begin(),con.end());}
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
	std::vector<int32_t> *data(){return &con;}const std::vector<int32_t> *data()const{return &con;}
	operator const std::vector<int32_t>&()const{return con;}
	operator std::vector<int64_t>()const{return std::vector<int64_t>(con.begin(),con.end());}
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
	std::vector<int64_t> *data(){return &con;}const std::vector<int64_t> *data()const{return &con;}
	operator const std::vector<int64_t>&()const{return con;}
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
	std::vector<nbt_Any*> *data(){return &con;}/*con的指针：直接改元素要自己守住“类型统一”*/
	const std::vector<nbt_Any*> *data()const{return &con;}
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
	std::unordered_map<std::string,nbt_Any*> *data(){return &con;}/*con的指针：值归树所有，换值记得free旧的*/
	const std::unordered_map<std::string,nbt_Any*> *data()const{return &con;}
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

/*---- 照游戏（Java）读数值：不是数值 tag 给 nullopt ----
  游戏读档时数值字段大多不管具体是哪种数值 tag：
    get_TagInt     NumericTag.intValue()（CompoundTag.getIntOr 用，xPos/方块实体坐标/DataVersion…）：long 取低32位，浮点先向下取整；
    get_TagLong    NumericTag.longValue()（getLongOr，InhabitedTime…）：float 向零取整，double 先向下取整；
    get_NumberInt  Number.intValue()（Codec.INT 读字段用，计划刻的 x/y/z/t/p…）：浮点向零取整；
    get_NumberDouble Number.doubleValue()（Codec.DOUBLE/FLOAT，实体的 Pos/Rotation…）。
  浮点→整数越界时饱和、NaN 给0（Java 的规矩）。*/
std::optional<int32_t> get_TagInt(const nbt_Any *v);
std::optional<int64_t> get_TagLong(const nbt_Any *v);
std::optional<int32_t> get_NumberInt(const nbt_Any *v);
std::optional<double> get_NumberDouble(const nbt_Any *v);
/*DFU 的 NbtOps 把 tag 当集合/long 序列读（游戏的编解码字段，比如调色板容器的 palette/data，都走这里）：
    get_NumberLong     Number.longValue()：整数照值，浮点向零取整；
    get_CollectionSize getStream：List 和 Byte/Int/Long_Array 都是集合，给元素个数；别的不是（nullopt）；
    get_LongStream     getLongStream：Long_Array 原样；别的集合每个元素都得是数值，按 Number.longValue 转；
                       不是集合、有不是数值的元素都读不了（false，*out 清空）。*/
std::optional<int64_t> get_NumberLong(const nbt_Any *v);
std::optional<size_t> get_CollectionSize(const nbt_Any *v);
bool get_LongStream(const nbt_Any *v,std::vector<int64_t> *out);
bool get_IntStream(const nbt_Any *v,std::vector<int32_t> *out);/*getIntStream（BlockPos/UUID 的 Codec.INT_STREAM）：Int_Array 原样，别的同上、按 Number.intValue 转*/
/*名字（方块/方块实体/结构…的 id）合不合法：照 Identifier.tryParse，命名空间 [a-z0-9_.-]（不能是".."）、
  路径 [a-z0-9_./-]，没有冒号算 minecraft:。只看写法，不看游戏认不认识这个名字*/
bool check_Identifier(std::string_view s);
/*游戏的 NbtIo 读得了吗：字符串和键名要是 DataInputStream.readUTF 认的 modified UTF-8，compound/list 嵌套不超过
  max_depth 层（根算第1层；游戏读区块/实体/POI 用的 NbtAccounter 是512）。读不了的话游戏整块读失败*/
bool check_ModifiedUTF8(std::string_view s);
bool check_NBTReadable(const nbt_Compound *root,int max_depth);
int prune_NBTUnreadable(nbt_Compound *root,int max_depth);/*把读不了的 tag 按最小单位删掉（坏键名/坏字符串所在的成员或元素、
                                                         太深的那一层容器）；返回删了几个*/

/*释放一批调用者自己拿着的节点（比如还没塞进 nbt_List/nbt_Compound 构造函数的 vector/map）*/
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
	bool operator==(const Block &b)const;/*!= 由C++20推出来*/
};

/*==================== POI：一条兴趣点记录（poi/ 下 POI 区块里的一条） ====================*/
/*照游戏的 PoiRecord.Packed：pos（方块的绝对坐标）、type（兴趣点类型名）、free_tickets（剩余名额）。
  按值持有，和 POI 区块无关；空 type＝读不到。写进 POI 区块（set_POI）时 pos 按写入的位置重写，x/y/z 只是读出来时的位置。*/
struct POI{
	int32_t x=0,y=0,z=0;
	nbt_String type;
	int32_t free_tickets=0;
	POI();
	POI(const char *type,int32_t free_tickets=0);
	bool valid()const{return !type.con.empty();}
	explicit operator bool()const{return valid();}
	std::string_view name()const{return type.con;}/*type 原样*/
	std::string_view spacename()const;/*冒号前那段，没冒号算minecraft*/
	std::string_view basename()const;/*冒号后那段，没冒号就是整个name*/
	std::string strid()const;/*spacename+':'+basename*/
	void set_Pos(int32_t x,int32_t y,int32_t z);
	void set_Type(std::string_view type);
	void set_FreeTickets(int32_t free_tickets);
	bool operator==(const POI &p)const;/*五样都相等*/
};
/*一条 POI 记录照游戏的 PoiRecord.Packed.CODEC 读：pos 是3个数（Codec.INT_STREAM：IntArray 或数值数组/列表）、
  type 是写法合法的名字；free_tickets 没有/不是数值当0（orElse(0)，不报错）。false=游戏读不进来（ERROR、这一条不要），*out 不动*/
bool get_POIRecord(const nbt_Any *rec,POI *out);
bool get_POIRecordPos(const nbt_Any *rec,int32_t *x,int32_t *y,int32_t *z);/*只要位置（同上的 pos 规矩）：type 坏了也知道它在哪*/
nbt_Compound *make_POIRecord(const POI &p,int32_t x,int32_t y,int32_t z);/*按游戏的写法 {pos:IntArray,type,free_tickets} 造一条（堆上新树，调用者接管）；
                                                                       pos 用给的 x/y/z，不用 p 里的*/

/*==================== Entity：一个实体（实体表里的一条 compound） ====================*/
/*整条实体按值持有（con：实体表里那个 compound 原样，含乘客）；拷贝是深拷贝，和区块树无关。内容原样搬，不按版本改写。
  UUID 两种写法都认：UUID（4 个数，1.16 起，Codec.INT_STREAM）、UUIDMost+UUIDLeast（数值，以前）；两样都有时看 UUID。
  字符串形式照 Java 的 UUID.toString：小写十六进制 8-4-4-4-12；没有/读不出来是空串。*/
struct Entity{
	nbt_Compound con;
	Entity();
	explicit Entity(const nbt_Compound &c);/*深拷贝*/
	explicit Entity(nbt_Compound &&c);/*接管*/
	explicit Entity(const char *id);/*只给id的便利构造*/
	nbt_Compound *data(){return &con;}
	const nbt_Compound *data()const{return &con;}
	bool valid()const;/*游戏读得进来吗（check_EntityRecord）*/
	explicit operator bool()const{return valid();}
	std::string_view name()const;/*id 原样；没有/不是 String 给空*/
	std::string_view spacename()const;
	std::string_view basename()const;
	std::string strid()const;
	std::string uuid()const;/*get_EntityUUID*/
	bool set_UUID(std::string_view uuid);/*写成它原来的写法（只有 UUIDMost/UUIDLeast 的写那两个，否则写 UUID 并删掉老的两个）；
	                                      不是 8-4-4-4-12 的十六进制给 false、不改*/
	bool get_Pos(double *x,double *y,double *z)const;/*Pos 正好3个数；没有/形状不对给 false*/
	void set_Pos(double x,double y,double z);/*写 Pos:[x,y,z]（Double 列表，游戏的写法）*/
	bool operator==(const Entity &e)const{return con==e.con;}
};
std::string get_EntityUUID(const nbt_Compound *entity);/*实体的 UUID（规矩见 Entity）；没有/读不出来给空串*/
bool check_EntityRecord(const nbt_Any *entity);/*单条实体游戏读得进来吗：compound、id 是写法合法的名字；UUID/Pos/Motion/Rotation
                                              有就得形状对（Pos 不能 NaN、Rotation 要是有限数）。没有 UUID/Pos 游戏照读，不算坏。
                                              不查乘客（Passengers 里坏一个游戏只丢那一个）*/

/*==================== 方块状态的编码（调色板条目）：按区块的 DataVersion 选 ====================*/
/*26.3快照7（DataVersion 5009）换过一次编码（wiki Chunk_format §History、zh 模板 Nbt inherit/block state/source）：
  STATE_NAME（<5009）：{Name:"minecraft:oak_slab",Properties:{type:"top",...}}，没有属性就没有 Properties；
  STATE_ID（>=5009）： "minecraft:stone"                    默认状态（游戏对属性全是默认值的状态就这么存）；
                      {id:"minecraft:oak_slab",properties:{type:"top"}}   没写的属性取默认值；
                      {"":"minecraft:stone"}                字符串混进复合列表时包成这样（NBT 列表只能装一种类型）。
  库只管编码，不知道每种方块的默认属性：新格式里没写的属性就是“没有”，要补默认值是规则插件的事。
  返回的指针都指在树里。*/
#define STATE_ID_FORMAT 5009
enum State_Format{STATE_NAME=0,STATE_ID=1};
inline State_Format get_StateFormat(int32_t data_version){return data_version>=STATE_ID_FORMAT?STATE_ID:STATE_NAME;}
const nbt_Any *get_StateNameTag(const nbt_Any *e,State_Format f);/*名字那个String；这种格式下认不出来给NULL*/
const char *get_StateName(const nbt_Any *e,State_Format f);/*名字原样；认不出来给空串*/
const nbt_Compound *get_StateProps(const nbt_Any *e,State_Format f);/*属性表；没写/不是compound给NULL*/
const char *get_PropValue(const nbt_Compound *props,const char *key);/*属性值；props为NULL/没有/不是String给NULL*/
nbt_Any *make_State(const Block &b,State_Format f);/*按格式造一条调色板条目（堆上新树，调用者接管）；空id给NULL*/
Block make_StateBlock(const nbt_Any *e,State_Format f);/*调色板条目→Block（不带方块实体）；认不出来给空id*/
bool is_StateSame(const nbt_Any *a,const nbt_Any *b,State_Format f);/*同一个方块状态？名字和写出来的属性都一样
                                                                     （没有属性表＝空属性表；不补默认值）*/

/*==================== Chunk：一个区块（nbt_Compound 按值持有 + 索引） ====================*/
/*只读的方块视图：指在区块树里，不深拷贝。
  state==NULL 表示“没有section/没有block_states”那种空气（按get_Block的规矩，此时也不看方块实体）；
  state!=NULL 是调色板条目（老格式是compound，新格式也可能是String）；be是同一格的方块实体（没有就是NULL）。
  只用于立刻比较/读取，别跨set_Block或缓存淘汰保存。*/
struct Block_Ref{
	const nbt_Any *state=NULL;/*NULL=缺段/纯光段的空气；此时name="minecraft:air"、props/be都空*/
	const char *name=NULL;/*名字原样；指在树里，配着state用*/
	const nbt_Any *props=NULL;/*属性compound；没有/类型不对就是NULL（和get_Block一致）*/
	const nbt_Compound *be=NULL;/*这一格的方块实体；没有就是NULL*/
};
bool equal_BlockRef(const Block_Ref &a,const Block_Ref &b);/*按get_Block的方块语义比：名字/属性/方块实体（两边格式可以不同）*/
bool equal_BlockRef(const Block_Ref &a,const Block &b);

/*section级的方块状态（调色板+data）：都要传区块——段表/调色板/data 走区块的索引，方块状态的编码按区块的 DataVersion。
  sy 是段号（世界Y按16向下取整），lx/ly/lz 是段内 0~15。只管这一段，不看生成状态（那是 get_Block/set_Block 的事）。
  block 是调色板条目（按区块的格式编码，调用者自己留着所有权）。*/
const nbt_Any *get_BlockData(const Chunk *c,int32_t sy,int lx,int ly,int lz);/*这一格的调色板条目；缺段/纯光段/读不出来给NULL*/
bool set_BlockData(Chunk *c,int32_t sy,const nbt_Any *block,int lx,int ly,int lz);/*缺段/没有block_states不自动建，给false*/
bool is_SectionConst(const Chunk *c,int32_t sy,const nbt_Any **block);/*整段都是一个方块？是就给true、方块在*block（空气，
                                                                      含缺段/纯光段，给NULL）；不止一种/读不出来给false*/

/*一段的只读视图（merge 逐格比较用）：Chunk::view_Section 打开时把区块级要现读的
  （DataVersion/生成状态/sections/block_entities/xPos/zPos）读一次、核对一遍区块的索引，之后逐格读只走索引。
  和 Block_Ref 一样只用于立刻读取：不存在 Chunk 里；别跨改树的调用（set_Block/format_Chunk/直接改树）保存，改完重新打开。*/
struct Section_View{
	const Chunk *chunk=NULL;
	int32_t sy=0;
	bool ok=false;            /*false=读不出来（不完整区块/sections坏）*/
	State_Format fmt=STATE_NAME;
	bool bes=false;           /*有方块实体可查（block_entities 非空、xPos/zPos 读得到）*/
	/*这一段调色板每条的名字/属性指针：第一次读的时候建，palette 换了或长度变了就重建（省掉逐格两次哈希）*/
	mutable const nbt_List *meta_pal=NULL;
	mutable std::vector<const nbt_Any*> meta_name,meta_props;
	bool get_BlockRef(int x,int ly,int z,Block_Ref *out)const;/*x/z 0~15，ly 段内 0~15；语义同 Chunk::get_BlockRef*/
};

/*方块实体索引的键：x/z 是区块内 0~15 的相对坐标，y 是世界绝对坐标*/
inline uint64_t get_BlockEntityKey(int lx,int32_t y,int lz){
	return ((uint64_t)(uint32_t)y<<8)|((uint64_t)(uint32_t)(lz&15)<<4)|(uint64_t)(uint32_t)(lx&15);
}

/*Chunk = 区块的 nbt_Compound（con，按值持有）+ 索引。索引只记要扫列表才能得到的东西，都指在 con 树里：
    sec_by_y[256]      段的 Y 按 uint8 存：0~127 在前，负的从后往前（-1→255、-128→128）。Y 照游戏读（read_SectionY：
                       截断成byte、缺了当0），同一Y有几段时记最后那段（游戏也是后面的覆盖前面的）
    palette_by_y[256]  那一段的方块调色板（block_states.palette；2832 以前是段里的 Palette）（没有/不是List给NULL）
    data_by_y[256]     那一段的下标（block_states.data；2832 以前是段里的 BlockStates）（没有/不是Long_Array给NULL）
    states_by_y[256]   那一段游戏读的时候调色板有几条（get_StatesView 的 n_game）：库能原样读写时 >0；
                       0＝干净的空气段（没有方块状态）；-1＝库不读（游戏读不了，或者读得了但不是库能原样读写的样子）
    be_by_pos          方块实体：get_BlockEntityKey(x,y,z) → block_entities 里的下标。照游戏（BlockEntity.getPosFromTag）：
                       坐标按 getIntOr 读（任何数值，不是数值当0），区块内位置取 x/z 的低4位（不在本区块的游戏会折回来），
                       同一格有几条时记最后那条（游戏后放的顶掉前面的）
  能 O(1) 从树里读的（生成状态、DataVersion、xPos/zPos、block_states 成员本身）不缓存，每次现读。
  读接口是const的，所以索引是mutable；同一个Chunk别在多个线程里同时用。
  reindex() 强制重建：库接口增删 section/方块实体、换调色板时自己会调。直接改树后要自己调——
  sections/block_entities 列表被换掉或长度变了，下次读的时候能看出来并自动重建，别的（换掉某段的
  block_states/palette/data、改方块实体的x/y/z）看不出来。*/
struct Chunk{
	nbt_Compound con;/*区块的NBT（就是.mca里那棵树）；可以直接读，直接改了结构要 reindex()*/
	mutable nbt_Compound *sec_by_y[256]={};
	mutable nbt_List *palette_by_y[256]={};
	mutable nbt_Long_Array *data_by_y[256]={};
	mutable int32_t states_by_y[256]={};
	mutable std::unordered_map<uint64_t,uint32_t> be_by_pos;
	/*建索引时看到的 DataVersion、sections/block_entities 列表和长度：现读的对不上就自动重建*/
	mutable int32_t idx_dv=0;
	mutable bool idx_states=true,idx_span=false;/*建索引时的格式（Chunk_Layout 的 states/span）：读格子时用*/
	mutable const nbt_Any *idx_secs_any=NULL;/*建索引时的 sections 成员（任何类型）*/
	mutable const nbt_List *idx_secs=NULL;
	mutable size_t idx_secs_len=0;
	mutable const nbt_List *idx_bes=NULL;
	mutable size_t idx_bes_len=0;

	Chunk(){}
	Chunk(int32_t cx,int32_t cz);/*手动新建：只写xPos/zPos；DataVersion/Status/sections要自己补，没Status仍算未生成*/
	explicit Chunk(const nbt_Compound &c):con(c){reindex();}/*深拷贝*/
	explicit Chunk(nbt_Compound &&c):con(std::move(c)){reindex();}/*接管（从MCA读出来的就这么用）*/
	Chunk(const Chunk &o):con(o.con){reindex();}/*深拷贝con，索引按新树重建*/
	Chunk(Chunk &&o)noexcept:con(std::move(o.con)){take_Index(o);}/*树的节点没动，索引直接接过来*/
	Chunk &operator=(const Chunk &o);/*先拷一份再换：拷贝失败原对象不动*/
	Chunk &operator=(Chunk &&o)noexcept;
	nbt_Compound *data(){return &con;}/*就是con；直接改了结构要 reindex()*/
	const nbt_Compound *data()const{return &con;}
	explicit operator nbt_Compound&()&{return con;}/*强转成nbt_Compound就是con*/
	explicit operator const nbt_Compound&()const&{return con;}
	explicit operator nbt_Compound()&&{clear_Index();return std::move(con);}
	void reindex()const;/*强制重建全部索引*/
	bool is_Full()const;/*生成状态=="minecraft:full"？没有这个字段也算不完整（和merge的规矩一致）*/
	nbt_Compound *get_Section(int32_t sy)const;/*按Y找section（走索引）；同一Y有多段时给最后那段（和游戏一样）*/
	int format_Chunk(int *bad_y=NULL,int *palette_cleaned=NULL);
		/*整理区块（fixer用，merge不调用），改完游戏读出来的东西不变：
		  · Y 不是 Byte 的照游戏的读法改成 Byte（别的数值截断，缺了/不是数值补0）；
		  · 同一Y有多段时（改Y以后撞上的也算）只留最后那段（游戏取的就是它）；
		  · block_states 的 palette 里有没被 data 引用的条目时压紧，并重写全部4096个下标；
		  · 返回删掉的段数；sections不是List时返回-1（不碰）。
		  bad_y/palette_cleaned 可选，分别返回“改了Y的段数/压紧调色板的段数”。*/
	/*x,z相对0~15，y是世界绝对Y。区块不完整→返回空id的Block，不读也不写*/
	Block get_Block(int x,int y,int z)const;
	/*get_Block的零拷贝版：true=读到了，结果在out；false=读不到（不完整区块/坏调色板/坏下标）。
	  out->state==NULL时是空气。merge拿它逐格比较，省掉Block的深拷贝。*/
	bool get_BlockRef(int x,int y,int z,Block_Ref *out)const;
	Section_View view_Section(int32_t sy)const;/*按段逐格读（见 Section_View）*/
	bool set_Block(const Block &block,int x,int y,int z);/*返回是否成功；失败不改区块；调色板/方块实体表先预检，成功才原位写*/
	/*方块实体：entity里原来的x/y/z一律丢弃，按区块xPos/zPos和放置坐标重算；entity为空=删掉这一格的方块实体*/
	bool set_BlockEntity(const nbt_Compound &entity,int x,int y,int z);
	int erase_BlockEntity(int x,int y,int z);/*删掉了几条*/
	nbt_Compound *get_BlockEntity(int x,int y,int z)const;/*指在树里；没有返回NULL*/

	/*---- 下面是索引的内部操作（库自己用） ----*/
	void index_Sections()const;/*重建 Y 表和每段的 palette/data*/
	void index_Slot(uint8_t i)const;/*重读 sec_by_y[i] 那一段的 palette/data*/
	void index_BlockEntities()const;/*重建方块实体索引*/
	void check_Sections(const nbt_Any *secs,int32_t dv)const;/*secs/dv 是现读的 sections 成员和 DataVersion：和建索引时的对不上就重建*/
	void check_Sections()const;/*自己现读一遍再核对*/
	const nbt_Any *get_Sections()const;/*sections 成员（任何类型，没有给NULL；老格式是 Level.Sections），顺便核对索引*/
	bool check_BlockEntities(const nbt_Any *bes)const;/*同上（方块实体）；false=表空/不是List*/
	nbt_Compound *find_BlockEntityAt(int x,int32_t y,int z)const;/*走索引；调用者先 check_BlockEntities 过*/
	void take_Index(Chunk &o)noexcept;/*移动用：接过o的索引，o清空*/
	void clear_Index()const noexcept;
};
/*区块按自己根上的 DataVersion 定格式（一个存档里可以混着没升级的老区块，不看 level.dat）。
  1.13 扁平化（17w47a，DataVersion 1451）以后的格式变化，分界照 26.1.1 的 DataFixers（游戏自己升级老区块用的）：
    1466 18w06a ChunkToProtochunkFix：Level.Status 代替 TerrainPopulated（之前 TerrainPopulated 为真算生成完）
    1905 ChunkStatusFix：完整的区块 postprocessed → fullchunk；1911 ChunkStatusFix2：改成 1.14 的名字（fullchunk → full）
    2527 20w17a BitStorageAlignFix：段的 BlockStates 下标不再跨 long（之前紧排、跨 long）
    2832 21w37a ChunkHeightAndBiomeFix：段里的 Palette/BlockStates 改成 block_states{palette,data}，生物群系拆进每段 biomes
    2841 ChunkProtoTickListFix：LiquidTicks 改名 fluid_ticks
    2842 21w43a ChunkRenamesFix：去掉 Level 外壳；TileEntities→block_entities、TileTicks→block_ticks、Sections→sections、
         Structures→structures（里面 Starts→starts）
    3450 RemapChunkStatusFix：生成状态写成带命名空间的（minecraft:full）；游戏按名字解析，full 和 minecraft:full 一样
    5009 26.3快照7：调色板条目编码（State_Format）；5119 26.4快照1：Status→status、生物群系每格一个*/
int32_t get_ChunkDataVersion(const nbt_Compound *chunk);/*根上的DataVersion（任何数值，getIntOr）；没有/不是数值给0（当最新格式读，见 is_ChunkVersionKnown）*/
inline State_Format get_ChunkStateFormat(const nbt_Compound *chunk){return get_StateFormat(get_ChunkDataVersion(chunk));}
#define CHUNK_FLATTEN 1451
/*这个 DataVersion 的区块库认得吗：1.13 扁平化（1451）以后的才认；没写 DataVersion（0）的当最新格式读，但不算认得
  （游戏当 -1、从最老的格式升级），fixer 这种会删东西的不该碰它*/
inline bool is_ChunkVersionKnown(int32_t data_version){return data_version>=CHUNK_FLATTEN;}
#define CHUNK_PROTO 1466
#define CHUNK_FULLCHUNK 1905
#define CHUNK_STATUS_FULL 1911
#define CHUNK_ALIGN 2527
#define CHUNK_STATES 2832
#define CHUNK_FLUID_TICKS 2841
#define CHUNK_FLAT 2842
#define CHUNK_STATUS_LOWER 5119
inline const char *get_StatusKey(int32_t data_version){return data_version>=CHUNK_STATUS_LOWER?"status":"Status";}
struct Chunk_Layout{/*这个 DataVersion 的区块长什么样*/
	int32_t dv=0;
	bool level=false;/*dv<2842：区块的字段都在根的 Level 里（DataVersion 在根上）*/
	bool states=true;/*dv>=2832：段里是 block_states{palette,data}；否则 Palette/BlockStates 直接在段里*/
	bool span=false;/*dv<2527：BlockStates 的下标紧排、跨 long*/
	State_Format fmt=STATE_NAME;
	const char *sections="sections",*block_entities="block_entities",*block_ticks="block_ticks",*fluid_ticks="fluid_ticks";
	const char *status="Status",*structures="structures",*starts="starts";
	const char *palette="palette",*data="data";/*段的方块状态容器里调色板/下标的键（老格式是 Palette/BlockStates）*/
	const char *entities="entities";/*区块里的实体表（2842 以前是 Level.Entities）；2681 起游戏把实体存到 entities/，这张表只剩原型区块/没迁走的老区块用*/
};
Chunk_Layout get_ChunkLayout(int32_t data_version);
inline Chunk_Layout get_ChunkLayout(const nbt_Compound *chunk){return get_ChunkLayout(get_ChunkDataVersion(chunk));}
/*一段方块调色板的位宽：至少4位；新格式单条调色板是0位（游戏不看 data），2832 以前单条也是4位、BlockStates 要有*/
inline uint32_t get_PaletteBitsFor(size_t n,bool states){
	if(n<=1){
		return states?0:4;
	}
	uint32_t b=0;
	for(size_t v=n-1;v;v>>=1){
		b++;
	}
	return b<4?4:b;
}
/*下标存储（方块状态的 data/BlockStates、生物群系的 data）：2527 起不跨 long（每个 long 放 64/bits 个），之前紧排、跨 long*/
inline uint32_t get_PackedWords(uint32_t entries,uint32_t bits,bool span){
	if(!bits){
		return 0;
	}
	if(span){
		return (uint32_t)(((uint64_t)entries*bits+63)/64);
	}
	uint32_t per=64/bits;
	return (entries+per-1)/per;
}
inline uint32_t get_PackedIndex(const std::vector<int64_t> &w,uint32_t i,uint32_t bits,bool span){/*调用者保证长度够（get_PackedWords）*/
	uint64_t mask=((uint64_t)1<<bits)-1;
	if(span){
		uint64_t bit=(uint64_t)i*bits;
		size_t k=(size_t)(bit>>6);
		uint32_t off=(uint32_t)(bit&63);
		uint64_t v=(uint64_t)w[k]>>off;
		if(off+bits>64){
			v|=(uint64_t)w[k+1]<<(64-off);
		}
		return (uint32_t)(v&mask);
	}
	uint32_t per=64/bits;
	return (uint32_t)(((uint64_t)w[i/per]>>((i%per)*bits))&mask);
}
inline void set_PackedIndex(std::vector<int64_t> &w,uint32_t i,uint32_t bits,bool span,uint32_t val){
	uint64_t mask=((uint64_t)1<<bits)-1,v=(uint64_t)val&mask;
	if(span){
		uint64_t bit=(uint64_t)i*bits;
		size_t k=(size_t)(bit>>6);
		uint32_t off=(uint32_t)(bit&63);
		w[k]=(int64_t)(((uint64_t)w[k]&~(mask<<off))|(v<<off));
		if(off+bits>64){
			uint64_t hm=((uint64_t)1<<(off+bits-64))-1;
			w[k+1]=(int64_t)(((uint64_t)w[k+1]&~hm)|(v>>(64-off)));
		}
		return;
	}
	uint32_t per=64/bits,shift=(i%per)*bits;
	w[i/per]=(int64_t)(((uint64_t)w[i/per]&~(mask<<shift))|(v<<shift));
}
/*区块字段在的那一层：老格式（dv<2842）是根的 Level（没有/不是 compound 给 NULL），新格式就是根。
  xPos/zPos/Status/InhabitedTime/sections/block_entities/计划刻/Heightmaps/isLightOn/structures 都在这一层*/
const nbt_Compound *get_ChunkBody(const nbt_Compound *chunk);
nbt_Compound *get_ChunkBody(nbt_Compound *chunk);
/*段的方块状态容器：新格式是段里的 block_states（没有/不是 compound 给 NULL），老格式就是段本身*/
const nbt_Compound *get_SectionStates(const nbt_Compound *section,const Chunk_Layout &layout);
nbt_Compound *get_SectionStates(nbt_Compound *section,const Chunk_Layout &layout);
/*段的 biomes：26.4快照1（DataVersion 5119）起从 4×4×4 一格改成每格一个，data 从64个下标变成4096个
  （wiki Chunk_format §History、zh 区块格式）。两边格式不同的 biomes 不能原样抄。*/
#define BIOME_PER_BLOCK 5119
inline bool is_BiomePerBlock(int32_t data_version){return data_version>=BIOME_PER_BLOCK;}

/*“这个区块生成完了没有/是不是空壳/结构坏没坏”几个判断（chunk.cpp）：参数是区块根（Chunk 给 &c->con）*/
enum Chunk_Full_Status{CHUNK_NO_STATUS=-1,CHUNK_NOT_FULL=0,CHUNK_FULL=1};
bool has_Block(const nbt_Compound *chunk);/*有任意一个非空气方块？形状认不出来时也给true，宁可信其有*/
Chunk_Full_Status get_ChunkStatus(const nbt_Compound *chunk);/*生成完了没有，按区块 DataVersion：1466 以前看 TerrainPopulated；
                                                    1905 以前 postprocessed/fullchunk、1911 以前 fullchunk、之后 full（按名字比，
                                                    full 和 minecraft:full 一样）；字段名 Status/status。没有这个字段=CHUNK_NO_STATUS*/
/*段里的 block_states / biomes 照游戏的 PalettedContainer 编解码判（26.1.1，test/mcdump.sh upgrade 和游戏对拍过）：
  true=游戏读得进来（条目认不出来的，游戏换成默认值、只打 ERROR，不算这里的坏）；false=游戏整块载入失败：
  palette 缺/不是集合/空、多条却没有 data（data 走 getLongStream：Long_Array，或者元素全是数值的 List/Int_Array/Byte_Array；
  别的类型游戏当没有）、data 长度不是正好该有的、下标指到 palette 外。
  entries=格数（block_states 4096、DataVersion<5119 的 biomes 64）；blocks=方块的位宽（至少4位），false=生物群系（ceil(log2)）*/
bool check_PaletteContainer(const nbt_Compound *container,uint32_t entries,bool blocks);
/*游戏眼里的一段方块状态。2832 以前的区块游戏是先用 DataFixers 升级再读的，照着算：
    2527 BitStorageAlignFix：位宽不是2的幂时把紧排跨 long 的下标转成不跨 long（长度按位宽正好，短了补0、长了扔掉）；
    2832 ChunkHeightAndBiomeFix：Palette 和 BlockStates 两样都在才要（少一样＝空气段）；调色板只有一条就扔掉 data；
         按 data 长度估的位宽比调色板要的大，就在调色板后面补空气，补到 2^(位宽-1)+1 条（下标落在补的那段是空气）；
    然后按 26.1.1 读（同 check_PaletteContainer）。
  canonical＝库能原样读写：palette 是 List、data 是 Long_Array，长度按这个版本的排法正好、位宽和游戏读的一样
  （游戏读得了、但不是这样的，库当读不出来：merge 跳过、规则 blind、fixer 不动）。*/
enum States_Kind:uint8_t{
	STATES_AIR,/*没有方块状态：游戏当全空气*/
	STATES_OK,/*游戏读得了*/
	STATES_BAD/*游戏整块载入失败*/
};
struct States_View{
	States_Kind kind=STATES_AIR;
	bool canonical=true;/*AIR 时：两样（新格式 block_states）都没有或者类型对＝干净的空气段；不然库当读不出来*/
	uint32_t n=0;/*调色板条数*/
	uint32_t n_game=0;/*游戏读的时候的条数（老格式可能补了空气，下标在 [n,n_game) 是空气）*/
	uint32_t bits=0;/*游戏读下标的位宽（不跨 long；0＝单值，不看 data）*/
};
/*scan=true 时把 4096 格的下标都查一遍（越界算 STATES_BAD）；false 只看长度/类型，O(1)*/
States_View get_StatesView(const nbt_Compound *section,const Chunk_Layout &layout,bool scan);
/*一段的方块状态游戏读得进来吗：get_StatesView(section,layout,true).kind!=STATES_BAD。false=游戏整块载入失败（fixer 删这一段的方块状态）*/
bool check_SectionStates(const nbt_Compound *section,const Chunk_Layout &layout);
bool check_Chunk(const nbt_Compound *chunk);/*严格：NBT成员类型/必需结构/每条计划刻都完整（Y不查，游戏照读）；false=merge跳过*/
bool check_ChunkBase(const nbt_Compound *chunk);/*fixer用：只查游戏会让整块载入失败的（段的 block_states/biomes 解不出来）；
                                         这些 fixer 先按最小单位修掉（删那一段的 block_states/biomes），别的游戏静默当没有的不算*/

/*---- 计划刻（block_ticks / fluid_ticks）：区块里“到点还要再判一次”的方块/流体 ----
  元素是compound：i:String（方块/流体名）、x/y/z（绝对世界坐标）、t（还有几tick，可负）、p（优先级）；
  照游戏的 SavedTick.codec，x/y/z/t/p 任何数值 tag 都认（Codec.INT），写的时候写 Int。
  表不存在＝0条；表在但有一条游戏读不进来＝坏数据（返回-1，调用者别去改它）。fluid=false是block_ticks，true是fluid_ticks。*/
struct Chunk_Tick{
	std::string_view type;/*指在树里，树没了就作废*/
	int32_t x=0,y=0,z=0,delay=0,priority=0;
};
bool check_ChunkTickRecord(const nbt_Any *tick);/*这一条游戏读得进来吗（i 是写法合法的名字，x/y/z/t/p 是数值；不看名字认不认识）*/
int count_ChunkTicks(const nbt_Compound *chunk,bool fluid);/*几条；-1=表形状不对*/
bool get_ChunkTick(const nbt_Compound *chunk,bool fluid,int i,Chunk_Tick *out);/*true=取到；false=形状不对*/
int del_ChunkTick(nbt_Compound *chunk,bool fluid,int32_t x,int32_t y,int32_t z);/*删了几条；-1=坏表没动*/
bool add_ChunkTick(nbt_Compound *chunk,bool fluid,const char *type,int32_t x,int32_t y,int32_t z,
	int32_t delay,int32_t priority);/*追加一条（Int 写）；表没有就建。顺带删掉表里游戏读不进来的条目（最小单位删）。
	                                  同一格已经有同名的刻不再加（游戏也按位置+类型去重）。true=加了，false=重复/坏表/老格式没有 Level*/
typedef bool (*Chunk_Tick_Keep_Fn)(void *ud,const Chunk_Tick *t);
int filter_ChunkTick(nbt_Compound *chunk,bool fluid,Chunk_Tick_Keep_Fn keep,void *ud);
	/*keep返回false的刻删掉；返回删了几条；-1=坏表，一条没动*/

/*==================== MCA：一个.mca文件的内存镜像（读写的都是 nbt_Compound；Chunk 先转 nbt_Compound 再写） ====================*/
/*区块数据的压缩类型：每块数据前面那1字节（wiki Region_file_format、游戏 RegionFileVersion）。
  最高位 MCA_EXTERNAL=数据放在外置文件 c.<区块x>.<区块z>.mcc（和 .mca 同一目录，坐标是区块绝对坐标），表里只留5字节的桩。*/
enum MCA_Compression:uint8_t{MCA_GZIP=1,MCA_ZLIB=2,MCA_NONE=3,MCA_LZ4=4,MCA_CUSTOM=127,MCA_EXTERNAL=128};
enum MCA_Read_Status{MCA_READ_OK=0,MCA_READ_NO_CHUNK=1,MCA_READ_BAD=2,
	MCA_READ_UNSUPPORTED_COMPRESSION=3,/*127 自定义压缩（模组的）：本库和原版游戏都读不了*/
	MCA_READ_BAD_NBT=4};
struct MCA{
	std::vector<byte> data;
	bool bad=false;/*文件**存在但读不出来**（不是普通文件/权限/IO错）：data可能是空的，但绝不能说“这个区域没有区块”*/
	bool truncated=false;/*文件比8KB表头短（游戏 WARN 照读）：读进来时补了0，存盘时表头写全*/
	mutable int last_error=MCA_READ_OK;/*最近一次get_Compound失败原因；成功也是OK*/
	std::string path;/*从文件构造时记下来，save()不带参数时用；外置 .mcc 也按它（文件名 r.<x>.<z>.mca）找*/
	uint8_t compression=MCA_ZLIB;/*set_Compound 用的压缩：MCA_GZIP/MCA_ZLIB/MCA_NONE/MCA_LZ4（游戏默认 zlib）*/
	/*set_Compound 压完要占 256 个扇区及以上时，数据和游戏一样放外置 .mcc：先放在这里（下标 x+z*32 → 压缩流），save() 时写出去*/
	std::unordered_map<uint32_t,std::vector<byte>> ext;
	std::unordered_set<uint32_t> touched;/*写过/删过的格子：save() 时删掉它们不再用的外置 .mcc*/
	MCA();/*空区域：8KB表头，可以直接往里写区块*/
	explicit MCA(const char *path);/*读文件；文件不在=空镜像（bad=false，取区块给MCA_READ_NO_CHUNK），读不出来=bad=true；
	                               比8KB表头短（含0字节）照游戏补0照读，truncated=true（0字节不算）*/
	explicit MCA(const std::string &path);
	/*读一格（region 区块/实体/POI 都一样是一个根compound）：读不到给 nullopt，原因看 last_error。
	  检查照游戏的 RegionFile：表头这一格空着或者偏移异常 = MCA_READ_NO_CHUNK；长度0/长度越界/外置文件不在/压缩类型不认识 = MCA_READ_BAD；
	  压缩流后面坏了不算错，只要根 compound 能完整读出来（游戏读完根就不往后读）。要当区块用就 Chunk c(std::move(*r))*/
	std::optional<nbt_Compound> get_Compound(uint32_t x,uint32_t z)const;
	bool set_Compound(const nbt_Compound &chunk,uint32_t x,uint32_t z);/*写进内存镜像，返回是否成功；Chunk 传 c.con*/
	/*表头一格照游戏的 RegionFile：偏移指在表头里（<2）、扇区数是0、偏移在文件外是“偏移异常”，游戏打开文件时 WARN 一声、
	  当这一格没有区块。库也这样：has_Chunk 给 false、get_Compound 给 MCA_READ_NO_CHUNK；format()/clean_Offsets() 把它们清成 0*/
	bool has_Chunk(uint32_t x,uint32_t z)const;/*这一格有区块（偏移正常；数据读不读得出来另说）*/
	bool has_BadOffset(uint32_t x,uint32_t z)const;/*这一格不是空的、但偏移异常*/
	int clean_Offsets();/*把偏移异常的格子清成 0（连时间戳）；给清了几格。只改内存*/
	bool erase_Chunk(uint32_t x,uint32_t z);/*删掉表头这一格（偏移异常的也清得掉）；删了返回true（外置 .mcc 在 save() 时删）*/
	bool is_Empty()const;/*一个区块都没有（偏移异常的格子不算）*/
	/*整理：先 clean_Offsets()，再按8K头索引顺序（0,0 < 1,0 < … < 31,0 < 0,1 < … < 31,31）重排数据区，
	  消除空洞，使区块一个接一个稠密存储。数据坏的格子（长度字段对不上）原样搬它占的扇区。只改内存，不落盘；
	  坏文件（bad）/3 字节偏移装不下返回false且一个字节不动。*/
	bool format();
	bool save(const char *path)const;/*先写外置 .mcc，再写 .mca（都先写.tmp再rename），最后删掉不用的 .mcc；bad时拒绝写*/
	bool save()const{return path.empty()?false:save(path.c_str());}
};
std::string get_MCAPath(const char *dir,int32_t x,int32_t z);/*拼dir/r.x.z.mca*/
/*列出目录里实际存在的区域文件（文件名正好是游戏拼出来的 r.<x>.<z>.mca 的普通文件，含 0 字节的；r.01.0.mca 这种游戏不读的不算），按 (x,z) 排好序；
  目录不在/读不出来给空。范围给到整个世界时，用它代替逐个区域坐标去 stat（±3000 万格有上百亿个区域坐标）*/
std::vector<std::pair<int32_t,int32_t>> list_MCAFiles(const char *dir);
std::string get_ExternalPath(const std::string &mca_path,uint32_t x,uint32_t z);/*这一格的外置文件 c.<x>.<z>.mcc；
                                                                             文件名不是 r.<x>.<z>.mca 给空串*/

/*==================== POI 区块和实体表：不定义结构体，直接对 nbt_Compound 操作 ====================*/
/*POI 区块（poi/ 下）的写法按它自己的 DataVersion：
    <1935（19w09a 以前，1.13 在内）：没有 POI，也没有 poi/ 目录（has_POIFile）；
    1935..1945（ReorganizePoi 以前，只有 1.14 快照）：根上 "0".."15" 直接是各段的记录表（段Y只能 0..15）；
    >=1946：Sections{"<段Y>":{Records:[...],Valid:1b}}（2218 ForcePoiRebuild 删过一次 Valid，游戏照样写）。
  DataVersion 没有/不是数值时库当最新的写法（和区块一样：工具造的常常不写）；游戏会当它是 1945
  （SectionStorage 的默认版本）走一遍 ReorganizePoi，Sections 被根上 "0".."15" 拼出来的表整个盖掉，所以落盘前要写上（make_POIChunk 会写）。
  游戏读 POI 区块很宽（SectionStorage.PackedChunk.parse）：Sections 不是 compound 当没有、段键只按 Y 找、段解不出来整段不要、
  记录表里读不进来的那条不要。get/set/erase 照这个读：游戏读不出来的部分当没有，set 时直接换掉。
  一格只认一条（PoiSection 按段内位置存）：同一格几条时，同类型留先来的、不同类型后来的顶掉（PoiSection.add）。
  坐标都是方块的绝对坐标。*/
#define POI_FILE 1935
#define POI_SECTIONS 1946
inline bool has_POIFile(int32_t dv){return dv<=0||dv>=POI_FILE;}/*这个版本的游戏有 poi/（没有 DataVersion 当最新）*/
int32_t get_POIDataVersion(const nbt_Compound *poi);/*POI 区块自己的 DataVersion；没有/不是数值给 0（当最新）*/
nbt_Compound make_POIChunk(int32_t dv);/*空 POI 区块：{DataVersion:dv} + 新写法的空 Sections；dv<=0 不写 DataVersion*/
bool check_POI(const nbt_Compound *poi);/*按它的写法查容器形状（新写法：Sections/段键/段/Records 类型；老写法：根上 "0".."15" 是 List）；
                                         单条记录坏不算。不对返回 false（库的严格判法，比游戏严；merge 整块跳过）*/
POI get_POI(const nbt_Compound *poi,int32_t x,int32_t y,int32_t z);/*游戏眼里这一格的那条（只看 y 所在的段）；没有给空 type*/
/*放/换这一格的记录：先删掉所有段里位置在 (x,y,z) 的记录，再在 y 所在的段里按 (x,y,z) 写一条 p（pos 重写，type/free_tickets 照 p）。
  新写法写过的段 Valid=1。p 没有 type/写法不合法、poi 不是 compound、老写法装不下（段Y不在 0..15）给 false，一点不改*/
bool set_POI(nbt_Compound *poi,int32_t x,int32_t y,int32_t z,const POI &p);
int erase_POI(nbt_Compound *poi,int32_t x,int32_t y,int32_t z);/*删掉所有段里位置在 (x,y,z) 的记录；给删了几条*/
/*全部记录按顺序追加到 out（游戏读不进来的那条跳过），给跳过了几条；check_POI 不过给 -1、out 不动*/
int list_POI(const nbt_Compound *poi,std::vector<POI> *out);
int count_POI(const nbt_Compound *poi);/*所有记录表的条数加起来（含读不进来的；读不出来的表不算）*/
int clean_POI(nbt_Compound *poi);/*没有记录/读不出来的段直接删掉，给删了几段*/
/*删掉游戏解不出来的段（游戏 ERROR、整段不要；别的段照留），给删了几段：新写法是段不是 compound、没有 Records 或不是 List；
  老写法是根上 "0".."15" 不是 List（游戏升级时把它包成 {Records:它}，照样解不出来）。段键不是数字、Sections 不是 compound 游戏当没有，不动*/
int repair_POI(nbt_Compound *poi);
/*所有游戏读得出来的记录表（老写法按 "0".."15"，新写法按 Sections 的顺序），指在树里；单条记录要自己按条处理时用（fixer 按条删坏记录）*/
std::vector<nbt_List*> get_POILists(nbt_Compound *poi);

/*实体：2681（20w45a）起存在 entities/ 下的实体区块 {DataVersion,Position:[cx,cz],Entities:[...]}；以前在区块的实体表里
  （2842 以前是 Level.Entities，之后叫 entities，即 Chunk_Layout::entities）。
  游戏两个都读：实体区块由 EntityStorage 读；区块里的实体表载入区块时也会生成出来（老区块的实体就是这么迁走的）。
  get/add/erase_ChunkEntit* 两种都收：老写法传区块（Chunk.data()），新写法传从 entities/ 的 mca 里解出来的实体区块；
  根上有 Position 的当实体区块，否则当区块（按区块的 DataVersion 找实体表那一层和键名）。空 compound 两种都不是。
  只管顶层实体：乘客（Passengers）跟着它骑的那个走。*/
#define ENTITY_FILE 2681
inline bool has_EntityFile(int32_t dv){return dv<=0||dv>=ENTITY_FILE;}/*这个版本的游戏把实体写在 entities/ 里（没有 DataVersion 当最新）*/
nbt_Compound make_EntityChunk(int32_t cx,int32_t cz,int32_t dv);/*空实体区块 {DataVersion:dv,Position:[cx,cz],Entities:[]}；dv<=0 不写 DataVersion*/
/*实体表的每条追加到 out，给条数；区块没有实体表给 0；认不出是哪种、表不是 List、元素不是 compound、实体区块没有 Entities 给 -1、out 不动。
  实体区块的 Position 只查形状（IntArray[2]），对不对得上槽位用 check_EntityContainer 查*/
int get_ChunkEntities(const nbt_Compound *chunk,std::vector<Entity> *out);
/*追加一条（深拷贝）；表没有就建。e 游戏读不进来（check_EntityRecord）、认不出是哪种、表的形状不对给 false，一点不改*/
bool add_ChunkEntity(nbt_Compound *chunk,const Entity &e);
/*删掉 UUID（字符串形式，见 Entity）等于 uuid 的所有顶层实体：没有 UUID 的算 ""，重复的全删；给删了几条（表不对给 0）*/
int erase_ChunkEntity(nbt_Compound *chunk,std::string_view uuid);
bool check_Entity(const nbt_Compound *chunk,int32_t cx,int32_t cz);/*实体区块严格查：容器+每条实体都完整；false=merge跳过*/
bool check_EntityContainer(const nbt_Compound *chunk,int32_t cx,int32_t cz);/*只查实体区块的 Position（等于 [cx,cz]）/Entities 表；
                                                                           DataVersion 可以没有（游戏当 -1），在就得是Int*/

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

	/*缓存：dat/mca/chunk都在这里自动管理，全按值持有；满了按LRU落盘再扔，调用者不用自己实现缓存。
	  cache_cnt=区块缓存数，region_cnt=区域镜像数，dat_cnt=dat缓存数；不设上限。
	  chunk/MCA 按“解析后的维度目录 + 坐标”分桶，所以不同维度同坐标不会串。
	  指针的有效期：
	    · 默认（没钉住）：get_* 给的指针只能用到下一次调用 Level 的接口为止（任何取/写/close 都可能把它淘汰掉），
	      每次要用都重新 get_*；
	    · pin_*：钉住的在 unpin_* 之前一直有效——LRU 不淘汰、close() 也不放掉（只有析构会）；可以钉好几次，unpin 同样次数才松开。
	      dat/mca/chunk 都能钉。缓存腾位置只淘汰没钉住的；全被钉住时取新对象失败（GET_CACHE），不硬抢。*/
	size_t cache_cnt=64,region_cnt=8,dat_cnt=16;
	uint64_t use_cnt=0;
	struct Chunk_Entry{
		std::string dimension;/*解析后的维度目录（空=老布局主世界根目录）*/
		int32_t cx=0,cz=0;
		Chunk chunk;/*按值持有；loaded=false时是空的*/
		bool loaded=false;/*true=chunk是读到/写进来的区块；false=没这块或坏块，原因在why（缓存住，别每次都猜）*/
		int why=0;/*loaded=false时是GET_NO_CHUNK/GET_BAD_CHUNK/GET_UNSUPPORTED_COMPRESSION*/
		bool changed=false;
		int pin=0;/*pin>0时LRU不淘汰*/
		uint64_t use=0;
	};
	struct MCA_Entry{
		std::string dimension;/*解析后的维度目录*/
		int32_t rx=0,rz=0;
		MCA mca;
		bool changed=false;
		int pin=0;
		uint64_t use=0;
	};
	struct Dat_Entry{
		std::string rel;
		nbt_Compound dat;/*按值持有*/
		bool changed=false;
		int compression=1;/*0=裸、1=gzip、2=zlib（读进来是什么，写回去还是什么）*/
		int pin=0;
		uint64_t use=0;
	};
	/*第一层键是解析后的维度目录；第二层是区块/区域坐标（unordered_map 的节点不搬家，钉住的指针一直有效）。*/
	std::unordered_map<std::string,std::unordered_map<uint64_t,Chunk_Entry>> chunks;
	std::unordered_map<std::string,std::unordered_map<uint64_t,MCA_Entry>> mcas;
	std::unordered_map<std::string,Dat_Entry> dats;

	/*get_Chunk/get_MCA最近一次为什么没读到（成功时是GET_OK）*/
	enum{GET_OK=0,GET_NO_FILE=1,GET_NO_CHUNK=2,GET_BAD_FILE=3,GET_BAD_CHUNK=4,GET_ARG=5,GET_CACHE=6,
		GET_UNSUPPORTED_COMPRESSION=7};/*文件能读，但区块用了本库不认识的压缩类型：别删*/
	int miss_why=GET_OK;

	/*存档目录；cache_cnt/region_cnt/dat_cnt分别是chunk/MCA/dat缓存容量，0按默认值*/
	Level(const char *path,size_t cache_cnt=64,size_t region_cnt=8,size_t dat_cnt=16);
	Level(const Level &)=delete;/*两份缓存会各自落盘、把同一个文件写两遍：不许拷贝*/
	Level &operator=(const Level &)=delete;
	~Level();/*自动close；钉住的也一起放掉*/
	bool close();/*落盘；成功才把没钉住的缓存放掉（钉住的留着，指针照样有效）；失败保留缓存可重试；可以重复调*/
	bool save();/*脏chunk→MCA，脏MCA→文件，脏dat→文件；返回是否全成功*/

	/*维度解析（不读盘，只看名字/路径和 new_layout）*/
	static bool is_DimensionName(const char *dimension);/*是不是三个原版维度名之一（可带 minecraft:）*/
	std::optional<std::string> get_DimensionBase(const char *dimension)const;
		/*维度参数→相对存档根的维度目录；空字符串/绝对路径/.. 都无效，返回 nullopt*/
	std::string get_DimensionPath(const char *dimension,const char *kind)const;
		/*维度参数+kind(region/entities/poi)→相对存档根的路径；无效返回空串*/

	/*坐标都是世界绝对坐标。返回的指针都指在缓存里，有效期见上面“指针的有效期”；要留着就钉住或者拷一份。*/
	MCA *get_MCA(const char *dimension,int32_t rx,int32_t rz);/*没有这个区域文件返回NULL；文件在但读不出来返回bad=true的对象（别写它）*/
	MCA *pin_MCA(const char *dimension,int32_t rx,int32_t rz);/*同get_MCA，再钉住*/
	bool unpin_MCA(const char *dimension,int32_t rx,int32_t rz);
	bool set_MCA(const char *dimension,int32_t rx,int32_t rz,const MCA &mca);/*放进缓存并标脏（没有就建）；拷进原对象，地址不变*/
	bool format_MCA(const char *dimension,int32_t rx,int32_t rz);/*整理这个区域（内存里），标脏；没有/坏了返回false*/
	Chunk *get_Chunk(const char *dimension,int32_t cx,int32_t cz);/*没有返回NULL*/
	Chunk *pin_Chunk(const char *dimension,int32_t cx,int32_t cz);/*同get_Chunk，再钉住（可以安全地跨多次取别的区块使用）*/
	bool unpin_Chunk(const char *dimension,int32_t cx,int32_t cz);
	bool erase_Chunk(const char *dimension,int32_t cx,int32_t cz);/*把区块从MCA表头删掉（fixer用）；本来没有也算成功；钉住的删不了*/
	bool set_Chunk(const char *dimension,int32_t cx,int32_t cz,const Chunk &chunk);/*深拷贝进缓存（拷进原对象，地址不变）并标脏；
	                                                                       传的就是缓存里那个对象时只标脏*/
	bool set_Chunk(const char *dimension,const Chunk &chunk);/*坐标从chunk自己的xPos/zPos取（必须是Int；2842 以前的老区块在 Level 里）*/
	Block get_Block(const char *dimension,int x,int y,int z);/*读不到返回空id的Block*/
	bool set_Block(const char *dimension,int x,int y,int z,const Block &block);/*返回是否成功；未生成的区块不写*/
	nbt_Compound *get_Dat(const char *rel);/*rel是相对存档根的路径，如"level.dat"；没有返回NULL*/
	nbt_Compound *pin_Dat(const char *rel);/*同get_Dat，再钉住*/
	bool unpin_Dat(const char *rel);
	bool set_Dat(const char *rel,const nbt_Compound &dat);/*深拷贝进缓存（拷进原对象，地址不变）并标脏*/
	/*下面几个是内部用的，指针的有效期同上。
	  create=true：文件不在时建空条目（空区域/空dat），给写入用；false：文件不在就返回NULL（GET_NO_FILE）。
	  区块条目读不到（没这块/坏块）时也会缓存原因，下次不再翻盘。*/
	MCA_Entry *get_MCAEntry(const char *dimension,int32_t rx,int32_t rz,bool create);
	Chunk_Entry *get_ChunkEntry(const char *dimension,int32_t cx,int32_t cz,bool create);
	MCA_Entry *get_MCAEntryBase(const std::string &base,int32_t rx,int32_t rz,bool create);
	Chunk_Entry *get_ChunkEntryBase(const std::string &base,int32_t cx,int32_t cz,bool create);
	Dat_Entry *get_DatEntry(const char *rel,bool create);
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
std::vector<byte> inflate_Prefix(const byte *in,size_t in_len,bool gzip,bool *complete);/*只认一种（gzip/zlib）；
                                     出错也给已经解出来的前缀，*complete=流是不是完整结束的（region 读一格用）*/
std::vector<byte> unlz4_Data(const byte *in,size_t in_len,bool *complete);/*lz4-java 的 LZ4Block 流；规矩同 inflate_Prefix*/
std::vector<byte> lz4_Data(const byte *in,size_t in_len);/*压成 LZ4Block 流（64KB 一块，压不小的块原样存，最后一个结束块）*/
std::vector<byte> deflate_Data(const byte *in,size_t in_len);/*zlib默认级别*/
std::vector<byte> gzip_Data(const byte *in,size_t in_len);/*gzip（level.dat用）*/
bool read_File(const char *path,std::vector<byte> &out);/*整个文件读进out；不是普通文件返回false*/
int get_DatCompression(const char *path);/*0=裸、1=gzip、2=zlib、-1=读不到；给set_Dat原样写回用*/

/*段的Y，照游戏读（SerializableChunkData.parse 的 getByteOr("Y",0)）：任何数值截成byte（浮点先向下取整，
  越界按Java的int饱和、NaN当0）；没有/不是数值给0*/
int8_t read_SectionY(const nbt_Compound *section);
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
