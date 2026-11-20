#include "nbtrw.hpp"
#include <cmath>
/*atom.cpp：游戏的最小单位——方块（Block + 方块状态属性小工具）、POI（一条兴趣点记录）、Entity（一个实体）
 *	三个结构体都按值持有、和区块树无关；放进/取出容器（区块、POI 区块、实体区块）的函数在 chunk.cpp。
 *	section级的调色板读写属于区块级，放在chunk.cpp。
 *	Block按值持有三个nbt结构：id（Name）、properties（Properties）、block_entities（这一格的方块实体）：
 *	  - 空id＝读不到（区块不完整/调色板坏了）；id=="minecraft:air"才是空气；
 *	  - 拷贝Block会深拷贝这三个nbt结构，所以Block是独立的，不指在区块树里；
 *	  - 直接改成员变量（b.properties["..."] = ...）就是在原位改，不会额外拷贝。
 *	调色板条目的编解码（get_StateName/get_StateProps/make_State…）也在这里，格式由调用者按区块的DataVersion给。
 */

/*---- 方块状态的编码（调色板条目）：两种格式见 nbtrw.hpp，格式由调用者按区块的 DataVersion 给 ----*/
const nbt_Any *get_StateNameTag(const nbt_Any *e,State_Format f){/*调色板条目里名字那个 String（新格式字符串条目就是它自己）；认不出来给 NULL*/
	if(!e){
		return NULL;
	}
	const nbt_Any *n=NULL;
	if(f==STATE_ID){
		if(e->type==NBT_STRING){
			n=e;/*默认状态：整条就是方块ID*/
		}else if(e->type==NBT_COMPOUND){
			const nbt_Compound *c=(const nbt_Compound*)e;
			n=c->get("id");
			if(!n){
				n=c->get("");/*混在复合列表里的默认状态：{"":ID}*/
			}
		}
	}else if(e->type==NBT_COMPOUND){
		n=((const nbt_Compound*)e)->get("Name");
	}
	return n&&n->type==NBT_STRING?n:NULL;
}
const char *get_StateName(const nbt_Any *e,State_Format f){/*调色板条目的方块名（C 字符串，指在树里）；这种格式下认不出来给空串*/
	const nbt_Any *n=get_StateNameTag(e,f);
	return n?((const nbt_String*)n)->con.c_str():"";
}
const nbt_Compound *get_StateProps(const nbt_Any *e,State_Format f){/*调色板条目的属性表；没写、不是 compound、条目是字符串都给 NULL*/
	if(!e||e->type!=NBT_COMPOUND){
		return NULL;/*新格式的字符串条目：没有属性（全默认）*/
	}
	const nbt_Compound *c=(const nbt_Compound*)e;
	const nbt_Any *p=NULL;
	if(f==STATE_ID){
		p=c->get("id")?c->get("properties"):NULL;/*{"":ID}那种包起来的字符串没有属性*/
	}else{
		p=c->get("Properties");
	}
	return p&&p->type==NBT_COMPOUND?(const nbt_Compound*)p:NULL;
}
const char *get_PropValue(const nbt_Compound *props,const char *key){/*属性表里某个属性的值（属性值都是 String）；没有给 NULL*/
	const nbt_Any *v=props&&key?props->get(key):NULL;
	return v&&v->type==NBT_STRING?((const nbt_String*)v)->con.c_str():NULL;
}
nbt_Any *make_State(const Block &b,State_Format f){/*Block → 一条调色板条目（堆上新树，调用者接管）；方块实体不在调色板里，不管*/
	if(!b.valid()){
		return NULL;
	}
	if(f==STATE_ID&&b.properties.empty()){
		return new nbt_String(b.id.con);/*没写属性＝默认状态，游戏也存成字符串*/
	}
	nbt_Compound *st=new nbt_Compound;
	st->set_String(f==STATE_ID?"id":"Name",b.id.con);
	if(!b.properties.empty()){
		st->set(f==STATE_ID?"properties":"Properties",b.properties.copy());/*copy()返回堆上新树，set接管*/
	}
	return st;
}
Block make_StateBlock(const nbt_Any *e,State_Format f){/*调色板条目 → Block（名字 + 属性的深拷贝，不带方块实体）；认不出名字给空 id*/
	Block b;
	const nbt_Any *n=get_StateNameTag(e,f);
	if(!n||((const nbt_String*)n)->con.empty()){
		return b;
	}
	b.id=nbt_String(((const nbt_String*)n)->con);
	const nbt_Compound *p=get_StateProps(e,f);
	if(p){
		b.properties=*p;/*深拷贝*/
	}
	return b;
}
bool is_StateSame(const nbt_Any *a,const nbt_Any *b,State_Format f){/*两条调色板条目是不是同一个方块状态：名字相同、写出来的属性相同（没有属性表＝空属性表）*/
	if(a==b){
		return true;
	}
	const nbt_Any *na=get_StateNameTag(a,f),*nb=get_StateNameTag(b,f);
	if(!na||!nb){
		return a&&b&&*a==*b;/*认不出来的条目：只认一模一样*/
	}
	if(((const nbt_String*)na)->con!=((const nbt_String*)nb)->con){
		return false;
	}
	const nbt_Compound *pa=get_StateProps(a,f),*pb=get_StateProps(b,f);
	if(pa&&pb){
		return *pa==*pb;
	}
	const nbt_Compound *p=pa?pa:pb;
	return !p||p->con.empty();/*没有属性表＝空属性表（和Block一样不区分）*/
}

/*---- Block：三个nbt结构按值持有 ----*/
Block::Block():id(),properties(),block_entities(){}/*空 id＝读不到*/
Block::Block(nbt_String id,nbt_Compound properties,nbt_Compound block_entities):
	id(std::move(id)),properties(std::move(properties)),block_entities(std::move(block_entities)){}/*三样都接管*/
Block::Block(const char *name):id(name?name:""),properties(),block_entities(){}/*只给名字*/
/*---- 名字的两段（Block/POI/Entity 共用）：正式namespace是关键字，所以叫spacename ----*/
static std::string_view get_Spacename(std::string_view n){/*冒号前那段；没有冒号算minecraft*/
	size_t p=n.find(':');
	return p==std::string_view::npos?"minecraft":n.substr(0,p);
}
static std::string_view get_Basename(std::string_view n){/*冒号后那段；没有冒号就是整个名字*/
	size_t p=n.find(':');
	return p==std::string_view::npos?n:n.substr(p+1);
}
static std::string get_Strid(std::string_view n){/*spacename+':'+basename；本来没冒号的会被补上minecraft:*/
	return std::string(get_Spacename(n))+":"+std::string(get_Basename(n));
}
std::string_view Block::spacename()const{return get_Spacename(name());}/*冒号前那段*/
std::string_view Block::basename()const{return get_Basename(name());}/*冒号后那段*/
std::string Block::strid()const{return get_Strid(name());}/*补全命名空间的名字*/
bool Block::operator==(const Block &b)const{/*三样都逐值相等才相等（方块实体也算：箱子里的东西不一样就是两个 Block）*/
	return id==b.id&&properties==b.properties&&block_entities==b.block_entities;
}

/*---- POI：一条兴趣点记录 ----*/
POI::POI():type(){}/*空 type＝读不到*/
POI::POI(const char *type,int32_t free_tickets):type(type?type:""),free_tickets(free_tickets){}/*只给类型和票数（位置由 set_POI 定）*/
std::string_view POI::spacename()const{return get_Spacename(name());}/*同 Block*/
std::string_view POI::basename()const{return get_Basename(name());}/*同 Block*/
std::string POI::strid()const{return get_Strid(name());}/*同 Block*/
void POI::set_Pos(int32_t x,int32_t y,int32_t z){/*方块的绝对坐标*/
	this->x=x;
	this->y=y;
	this->z=z;
}
void POI::set_Type(std::string_view type){/*原样存，不补命名空间*/
	this->type.con.assign(type);
}
void POI::set_FreeTickets(int32_t free_tickets){/*不按类型的票上限夹（那是规则插件的事）*/
	this->free_tickets=free_tickets;
}
bool POI::operator==(const POI &p)const{/*位置、类型、票数都相等*/
	return x==p.x&&y==p.y&&z==p.z&&type==p.type&&free_tickets==p.free_tickets;
}
static bool get_IntStreamN(const nbt_Any *a,size_t n,int32_t *out){/*Codec.INT_STREAM 定长 n（BlockPos 3 个、UUID 4 个）*/
	std::vector<int32_t> v;
	if(!get_IntStream(a,&v)||v.size()!=n){
		return false;
	}
	for(size_t i=0;i<n;i++){
		out[i]=v[i];
	}
	return true;
}
bool get_POIRecordPos(const nbt_Any *rec,int32_t *x,int32_t *y,int32_t *z){/*只读位置：type 坏了的记录也能知道它在哪（fixer 靠它判在不在范围里）*/
	int32_t p[3];
	if(!rec||rec->type!=NBT_COMPOUND||!get_IntStreamN(((const nbt_Compound*)rec)->get("pos"),3,p)){
		return false;
	}
	*x=p[0];
	*y=p[1];
	*z=p[2];
	return true;
}
bool get_POIRecord(const nbt_Any *rec,POI *out){/*照游戏的编解码读一条 POI 记录*/
	int32_t x=0,y=0,z=0;
	if(!out||!get_POIRecordPos(rec,&x,&y,&z)){
		return false;
	}
	const nbt_Compound *e=(const nbt_Compound*)rec;
	const nbt_Any *t=e->get("type");
	if(!t||t->type!=NBT_STRING||!check_Identifier(((const nbt_String*)t)->con)){
		return false;
	}
	out->set_Pos(x,y,z);
	out->type=*(const nbt_String*)t;
	out->free_tickets=get_NumberInt(e->get("free_tickets")).value_or(0);/*orElse(0)：没有/不是数值当0，不报错*/
	return true;
}
nbt_Compound *make_POIRecord(const POI &p,int32_t x,int32_t y,int32_t z){/*PoiRecord.Packed 的写法*/
	nbt_Compound *rec=new nbt_Compound;
	nbt_Int_Array *pos=new nbt_Int_Array;
	pos->push_back(x);
	pos->push_back(y);
	pos->push_back(z);
	rec->set("pos",pos);
	rec->set("type",new nbt_String(p.type));
	rec->set_Int("free_tickets",p.free_tickets);
	return rec;
}

/*---- Entity：一个实体 ----*/
Entity::Entity():con(){}/*空实体（valid() 为假）*/
Entity::Entity(const nbt_Compound &c):con(c){}/*深拷贝*/
Entity::Entity(nbt_Compound &&c):con(std::move(c)){}/*接管*/
Entity::Entity(const char *id):con(){/*只写 id*/
	if(id&&id[0]){
		con.set_String("id",id);
	}
}
bool Entity::valid()const{return check_EntityRecord(&con);}/*游戏读得进来吗*/
std::string_view Entity::name()const{/*id 原样；没有/不是 String 给空*/
	const nbt_Any *id=con.get("id");
	return id&&id->type==NBT_STRING?std::string_view(((const nbt_String*)id)->con):std::string_view();
}
std::string_view Entity::spacename()const{return get_Spacename(name());}/*同 Block*/
std::string_view Entity::basename()const{return get_Basename(name());}/*同 Block*/
std::string Entity::strid()const{return get_Strid(name());}/*同 Block*/
std::string Entity::uuid()const{return get_EntityUUID(&con);}/*字符串形式；没有给空串*/
static bool get_UUIDWords(const nbt_Compound *e,uint64_t *most,uint64_t *least){/*UUID 的高低 64 位；两种写法见 nbtrw.hpp 的 Entity*/
	int32_t v[4];
	if(get_IntStreamN(e->get("UUID"),4,v)){/*UUIDUtil.CODEC：[高高, 高低, 低高, 低低]*/
		*most=((uint64_t)(uint32_t)v[0]<<32)|(uint32_t)v[1];
		*least=((uint64_t)(uint32_t)v[2]<<32)|(uint32_t)v[3];
		return true;
	}
	std::optional<int64_t> m=get_TagLong(e->get("UUIDMost")),l=get_TagLong(e->get("UUIDLeast"));/*老游戏的 hasUUID：两个都是数值*/
	if(m&&l){
		*most=(uint64_t)*m;
		*least=(uint64_t)*l;
		return true;
	}
	return false;
}
std::string get_EntityUUID(const nbt_Compound *entity){/*照 Java UUID.toString 拼 8-4-4-4-12*/
	uint64_t m=0,l=0;
	if(!entity||entity->type!=NBT_COMPOUND||!get_UUIDWords(entity,&m,&l)){
		return std::string();
	}
	char buf[40];
	snprintf(buf,sizeof(buf),"%08x-%04x-%04x-%04x-%012llx",(unsigned)(m>>32),(unsigned)((m>>16)&0xFFFFu),(unsigned)(m&0xFFFFu),
		(unsigned)(l>>48),(unsigned long long)(l&0xFFFFFFFFFFFFull));
	return buf;
}
static bool parse_UUID(std::string_view s,uint64_t *most,uint64_t *least){/*8-4-4-4-12 的十六进制（大小写都认）*/
	if(s.size()!=36){
		return false;
	}
	uint64_t w[2]={0,0};
	int n=0;
	for(size_t i=0;i<36;i++){
		char c=s[i];
		if(i==8||i==13||i==18||i==23){
			if(c!='-'){
				return false;
			}
			continue;
		}
		int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
		if(d<0){
			return false;
		}
		w[n/16]=(w[n/16]<<4)|(uint64_t)d;
		n++;
	}
	*most=w[0];
	*least=w[1];
	return true;
}
bool Entity::set_UUID(std::string_view uuid){/*按实体原来的写法写（见 nbtrw.hpp）*/
	uint64_t m=0,l=0;
	if(!parse_UUID(uuid,&m,&l)){
		return false;
	}
	if(!con.get("UUID")&&con.get("UUIDMost")&&con.get("UUIDLeast")){/*老写法的实体：照它原来的写法写*/
		con.set_Long("UUIDMost",(int64_t)m);
		con.set_Long("UUIDLeast",(int64_t)l);
		return true;
	}
	nbt_Int_Array *a=new nbt_Int_Array;
	a->push_back((int32_t)(uint32_t)(m>>32));
	a->push_back((int32_t)(uint32_t)m);
	a->push_back((int32_t)(uint32_t)(l>>32));
	a->push_back((int32_t)(uint32_t)l);
	con.set("UUID",a);
	con.erase("UUIDMost");
	con.erase("UUIDLeast");
	return true;
}
/*Codec.DOUBLE/FLOAT 的定长列表（Vec3/Vec2.CODEC）：正好 n 个数值*/
static bool get_NumList(const nbt_Any *a,size_t n,double *out){
	if(!a||a->type!=NBT_LIST||((const nbt_List*)a)->con.size()!=n){
		return false;
	}
	for(size_t i=0;i<n;i++){
		std::optional<double> v=get_NumberDouble(((const nbt_List*)a)->con[i]);
		if(!v){
			return false;
		}
		out[i]=*v;
	}
	return true;
}
bool Entity::get_Pos(double *x,double *y,double *z)const{/*Pos 照 Vec3.CODEC：正好3个数值（任何数值 tag）*/
	double v[3];
	if(!get_NumList(con.get("Pos"),3,v)){
		return false;
	}
	*x=v[0];
	*y=v[1];
	*z=v[2];
	return true;
}
void Entity::set_Pos(double x,double y,double z){/*游戏写的是 Double 列表*/
	nbt_List *l=new nbt_List(NBT_DOUBLE);
	l->push_back(new nbt_Double(x));
	l->push_back(new nbt_Double(y));
	l->push_back(new nbt_Double(z));
	con.set("Pos",l);
}
bool check_EntityRecord(const nbt_Any *entity){/*单条实体游戏读得进来吗（规矩见 nbtrw.hpp 的声明）*/
	if(!entity||entity->type!=NBT_COMPOUND){
		return false;
	}
	const nbt_Compound *e=(const nbt_Compound*)entity;
	const nbt_Any *id=e->get("id");
	if(!id||id->type!=NBT_STRING||!check_Identifier(((const nbt_String*)id)->con)){
		return false;/*Skipping Entity with id（WARN）*/
	}
	int32_t u[4];
	const nbt_Any *uuid=e->get("UUID");/*没有 UUID 游戏随机给一个，不报错；在就得是 4 个数（Codec.INT_STREAM）*/
	if(uuid&&!get_IntStreamN(uuid,4,u)){
		return false;
	}
	double v[3];
	const nbt_Any *pos=e->get("Pos");/*没有就是(0,0,0)，不报错；形状不对 WARN；NaN 抛 Entity has invalid position*/
	if(pos&&(!get_NumList(pos,3,v)||std::isnan(v[0])||std::isnan(v[1])||std::isnan(v[2]))){
		return false;
	}
	const nbt_Any *mo=e->get("Motion");
	if(mo&&!get_NumList(mo,3,v)){
		return false;
	}
	const nbt_Any *rot=e->get("Rotation");/*不是有限数：Invalid entity rotation（ERROR）*/
	if(rot&&(!get_NumList(rot,2,v)||!std::isfinite((float)v[0])||!std::isfinite((float)v[1]))){
		return false;
	}
	return true;
}
