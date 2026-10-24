#include "nbtrw.hpp"
/*block.cpp：方块状态（Block + 方块状态属性小工具）
 *	section级的调色板读写属于区块级，放在chunk.cpp。
 *	Block按值持有三个nbt结构：id（Name）、properties（Properties）、block_entities（这一格的方块实体）：
 *	  - 空id＝读不到（区块不完整/调色板坏了）；id=="minecraft:air"才是空气；
 *	  - 拷贝Block会深拷贝这三个nbt结构，所以Block是独立的，不指在区块树里；
 *	  - 直接改成员变量（b.properties["..."] = ...）就是在原位改，不会额外拷贝。
 *	get_Name/get_Basename/get_Prop/is_PropValue是给外面（rules插件等）用的同一套取法。
 */

/*---- 方块状态的小工具：从nbt里现取 ----*/
const char *get_Name(const nbt_Any *b){/*block里的Name，不是compound或者没有就给空串*/
	const nbt_Any *n=b&&b->type==NBT_COMPOUND?((const nbt_Compound*)b)->get("Name"):NULL;
	const char *s=n?n->as_Str():NULL;
	return s?s:"";
}
const char *get_Basename(const nbt_Any *b){/*冒号后那段；没有冒号就是整个name*/
	const char *n=get_Name(b);
	const char *p=strchr(n,':');
	return p?p+1:n;
}
const char *get_Prop(const nbt_Any *b,const char *key){/*Properties.<key>，不是字符串或者没有就给NULL*/
	if(!b||b->type!=NBT_COMPOUND){
		return NULL;
	}
	const nbt_Any *p=((const nbt_Compound*)b)->get("Properties");
	if(!p||p->type!=NBT_COMPOUND){
		return NULL;
	}
	const nbt_Any *v=((const nbt_Compound*)p)->get(key);
	return v?v->as_Str():NULL;
}
bool is_PropValue(const nbt_Any *b,const char *key,const char *val){/*属性正好等于val？*/
	const char *s=get_Prop(b,key);
	return s&&val&&!strcmp(s,val);
}

/*---- Block：三个nbt结构按值持有 ----*/
Block::Block():id(),properties(),block_entities(){}
Block::Block(nbt_String id,nbt_Compound properties,nbt_Compound block_entities):
	id(std::move(id)),properties(std::move(properties)),block_entities(std::move(block_entities)){}
Block::Block(const char *name):id(name?name:""),properties(),block_entities(){}
std::string_view Block::spacename()const{/*冒号前那段；正式namespace是关键字，所以叫spacename；没有冒号算minecraft*/
	std::string_view n=name();
	size_t p=n.find(':');
	return p==std::string_view::npos?"minecraft":n.substr(0,p);
}
std::string_view Block::basename()const{/*冒号后那段；没有冒号就是整个name*/
	std::string_view n=name();
	size_t p=n.find(':');
	return p==std::string_view::npos?n:n.substr(p+1);
}
std::string Block::strid()const{/*spacename+':'+basename；本来没冒号的会被补上minecraft:*/
	return std::string(spacename())+":"+std::string(basename());
}
bool Block::operator==(const Block &b)const{
	return id==b.id&&properties==b.properties&&block_entities==b.block_entities;
}
bool Block::operator!=(const Block &b)const{
	return !(*this==b);
}
