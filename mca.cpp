#include "nbtrw.hpp"
#include <zlib.h>/*编译加-lz*/
#include <time.h>
#include <sys/stat.h>
/*mca.cpp：区域文件读写 + zlib/gzip
 *	MCA = 一个.mca文件的内存镜像。构造：MCA()是空区域（8KB表头），MCA(path)读文件。
 *	读不到的mca：文件不在→data空、bad=false（正常，这片没生成）；文件在但读不出来→bad=true（绝不能当成空区域写回去）。
 *	读不到的区块：get_Chunk返回NULL（表里没这格/压缩坏/NBT坏）。
 *	format()按8K头索引顺序（0,0 < 1,0 < … < 31,0 < 0,1 < … < 31,31）重排数据区，消除空洞、稠密存储。
 */

std::vector<byte> inflate_Data(const byte *in,size_t in_len){/*gzip/zlib都认；出错返回空*/
	std::vector<byte> out;
	if(!in&&in_len){
		return out;
	}
	z_stream s{};
	if(inflateInit2(&s,47)!=Z_OK){/*47=自动认gzip还是zlib*/
		return out;
	}
	byte buf[1<<16];
	size_t in_pos=0;
	int ret=Z_OK;
	while(ret!=Z_STREAM_END){
		if(s.avail_in==0){
			if(in_pos>=in_len){
				break;
			}
			size_t remain=in_len-in_pos;
			uInt chunk=remain>UINT_MAX?UINT_MAX:(uInt)remain;
			s.next_in=(Bytef*)(in+in_pos);
			s.avail_in=chunk;
			in_pos+=chunk;
		}
		s.next_out=buf;
		s.avail_out=(uInt)sizeof(buf);
		ret=inflate(&s,Z_NO_FLUSH);
		if(ret!=Z_OK&&ret!=Z_STREAM_END){
			break;
		}
		size_t got=sizeof(buf)-s.avail_out;
		out.insert(out.end(),buf,buf+got);
		if(ret==Z_OK&&got==0&&s.avail_in==0&&in_pos>=in_len){/*没有进展：输入被截断*/
			ret=Z_BUF_ERROR;
			break;
		}
	}
	inflateEnd(&s);
	if(ret!=Z_STREAM_END){
		return std::vector<byte>();
	}
	return out;
}
std::vector<byte> deflate_Data(const byte *in,size_t in_len){/*zlib默认级别*/
	std::vector<byte> out;
	if(!in&&in_len){
		return out;
	}
	z_stream s{};
	if(deflateInit(&s,Z_DEFAULT_COMPRESSION)!=Z_OK){
		return out;
	}
	byte buf[1<<16];
	size_t in_pos=0;
	int ret=Z_OK;
	while(ret!=Z_STREAM_END){
		if(s.avail_in==0&&in_pos<in_len){
			size_t remain=in_len-in_pos;
			uInt chunk=remain>UINT_MAX?UINT_MAX:(uInt)remain;
			s.next_in=(Bytef*)(in+in_pos);
			s.avail_in=chunk;
			in_pos+=chunk;
		}
		int flush=in_pos>=in_len?Z_FINISH:Z_NO_FLUSH;
		s.next_out=buf;
		s.avail_out=(uInt)sizeof(buf);
		ret=deflate(&s,flush);
		if(ret!=Z_OK&&ret!=Z_STREAM_END){
			deflateEnd(&s);
			return std::vector<byte>();
		}
		out.insert(out.end(),buf,buf+sizeof(buf)-s.avail_out);
	}
	deflateEnd(&s);
	return out;
}
std::vector<byte> gzip_Data(const byte *in,size_t in_len){/*gzip（level.dat用）*/
	std::vector<byte> out;
	if(!in&&in_len){
		return out;
	}
	z_stream s{};
	if(deflateInit2(&s,Z_DEFAULT_COMPRESSION,Z_DEFLATED,15+16,8,Z_DEFAULT_STRATEGY)!=Z_OK){
		return out;
	}
	byte buf[1<<16];
	size_t in_pos=0;
	int ret=Z_OK;
	while(ret!=Z_STREAM_END){
		if(s.avail_in==0&&in_pos<in_len){
			size_t remain=in_len-in_pos;
			uInt chunk=remain>UINT_MAX?UINT_MAX:(uInt)remain;
			s.next_in=(Bytef*)(in+in_pos);
			s.avail_in=chunk;
			in_pos+=chunk;
		}
		int flush=in_pos>=in_len?Z_FINISH:Z_NO_FLUSH;
		s.next_out=buf;
		s.avail_out=(uInt)sizeof(buf);
		ret=deflate(&s,flush);
		if(ret!=Z_OK&&ret!=Z_STREAM_END){
			deflateEnd(&s);
			return std::vector<byte>();
		}
		out.insert(out.end(),buf,buf+sizeof(buf)-s.avail_out);
	}
	deflateEnd(&s);
	return out;
}

std::string get_MCAPath(const char *dir,int32_t x,int32_t z){/*区域文件的路径只有这一个拼法*/
	return std::string(dir?dir:"")+"/r."+std::to_string(x)+"."+std::to_string(z)+".mca";
}
MCA::MCA(){/*空区域：8KB表头，可以直接往里写区块*/
	data.resize(8192,0);
}
MCA::MCA(const char *p):path(p?p:""){/*读文件；文件不在=空镜像，文件坏=bad*/
	if(!read_File(path.c_str(),data)){
		/*读不出来要分两种：文件**根本不在**（正常：这个区域还没生成）＝空镜像；
		  文件在、但读不出来（权限、IO错、是个目录、不是普通文件）＝bad，绝不能当成“这里没有区块”*/
		struct stat st;
		if(stat(path.c_str(),&st)==0||errno!=ENOENT){/*存在但打不开/是目录：坏；只有真不存在才算空*/
			bad=true;
		}
	}else if(data.size()<8192){
		/*文件在、也读到了，但连8KB表头都不够（包括0字节）：按坏文件处理，绝不能当空区域覆盖*/
		bad=true;
	}
}
MCA::MCA(const std::string &p):MCA(p.c_str()){}
bool MCA::has_Chunk(uint32_t x,uint32_t z)const{/*表头那一格的偏移不是0就是有区块*/
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	size_t loc=4*(x+z*32);
	return (data[loc]|data[loc+1]|data[loc+2])!=0;
}
bool MCA::is_Empty()const{/*一格都没有：这个文件等于不存在*/
	if(data.size()<8192){
		return true;
	}
	for(size_t i=0;i<1024;i++){
		if(data[i*4]|data[i*4+1]|data[i*4+2]){
			return false;
		}
	}
	return true;
}
bool MCA::erase_Chunk(uint32_t x,uint32_t z){/*把这一格从表头删掉：数据区留着当空洞（format会收拾）*/
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	size_t loc=4*(x+z*32);
	if(!(data[loc]|data[loc+1]|data[loc+2]|data[loc+3])){
		return false;/*本来就没有*/
	}
	memset(data.data()+loc,0,4);
	memset(data.data()+4096+loc,0,4);/*时间戳一起清掉*/
	return true;
}
/*把一个区块的原始字节（已解压）取出来；false=这一格没有/文件被截短/压缩坏*/
static bool read_ChunkRaw(const std::vector<byte> &data,uint32_t x,uint32_t z,std::vector<byte> &raw){
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	size_t loc=4*(x+z*32);
	size_t pos=((size_t)data[loc]<<16|data[loc+1]<<8|data[loc+2])*4096;
	if(pos==0){
		return false;/*表头这一格是空的：这一格本来就没有区块（正常，不是坏数据）*/
	}
	if(pos+5>data.size()){/*偏移指到文件外*/
		return false;
	}
	uint32_t len=(uint32_t)data[pos]<<24|(uint32_t)data[pos+1]<<16|(uint32_t)data[pos+2]<<8|data[pos+3];
	byte type=data[pos+4];
	if(len<1||(size_t)(len-1)>data.size()-pos-5){/*长度字段不合理*/
		return false;
	}
	if(type==0x01||type==0x02){
		raw=inflate_Data(data.data()+pos+5,len-1);
		if(raw.empty()){/*压缩数据坏了*/
			return false;
		}
	}else if(type==0x03){
		raw.assign(data.begin()+(ptrdiff_t)(pos+5),data.begin()+(ptrdiff_t)(pos+5+(len-1)));
	}else{
		return false;/*不认识的压缩类型：跳过这个区块*/
	}
	return true;
}
Chunk *MCA::get_Chunk(uint32_t x,uint32_t z)const{
	if(bad){
		return NULL;/*这个文件没读出来：不能拿半截镜像去解析*/
	}
	std::vector<byte> raw;
	if(!read_ChunkRaw(data,x,z,raw)){
		return NULL;
	}
	nbt_Raw nbt;
	nbt.data=std::move(raw);
	return Chunk::read(&nbt);/*里面有坏NBT的话会返回NULL*/
}
nbt_Compound *MCA::get_Compound(uint32_t x,uint32_t z)const{/*实体/POI区块：就是个普通compound*/
	if(bad){
		return NULL;
	}
	std::vector<byte> raw;
	if(!read_ChunkRaw(data,x,z,raw)){
		return NULL;
	}
	nbt_Raw nbt;
	nbt.data=std::move(raw);
	return nbt.read_Root();
}
static bool write_CompoundAt(std::vector<byte> &data,bool *bad,const nbt_Compound &chunk,uint32_t x,uint32_t z){
	if(bad&&*bad){/*文件没读出来：绝不写*/
		return false;
	}
	if(x>31||z>31){
		return false;
	}
	if(data.size()<8192){/*空镜像（新区域）：先建8KB表头*/
		data.assign(8192,0);
	}
	nbt_Raw raw=chunk.to_Raw();
	if(raw.fail){
		return false;
	}
	std::vector<byte> zip=deflate_Data(raw.data.data(),raw.data.size());
	if(zip.empty()){
		return false;
	}
	if(zip.size()>SIZE_MAX-5){
		return false;
	}
	size_t need=5+zip.size(),sectors=(need+4095)/4096;
	if(sectors>255){/*MCA格式：扇区数是1字节*/
		return false;
	}
	size_t loc=4*(x+z*32);
	size_t pos=((size_t)data[loc]<<16|data[loc+1]<<8|data[loc+2])*4096;
	size_t old_sectors=(size_t)data[loc+3];
	/*“原sector放得下就原地写”：除了长度够，还必须整个旧sector都在文件里，
	  否则按坏偏移去memset就是往堆外写*/
	if(pos&&pos+old_sectors*4096<=data.size()&&old_sectors*4096>=need){
		memset(data.data()+pos,0,old_sectors*4096);
	}else{/*放不下就追加到文件末尾，旧sector留成空洞*/
		/*表头里的偏移是“第几个4096字节”，所以新数据的起点必须对齐到4096*/
		size_t pad=(4096-data.size()%4096)%4096;
		pos=data.size()+pad;
		if(sectors>SIZE_MAX/4096||pos>SIZE_MAX-sectors*4096){
			return false;
		}
		data.resize(pos+sectors*4096,0);
	}
	byte *p=data.data()+pos;
	size_t total=zip.size()+1;/*长度字段含类型字节*/
	p[0]=(byte)(total>>24);p[1]=(byte)(total>>16);p[2]=(byte)(total>>8);p[3]=(byte)total;
	p[4]=0x02;/*zlib*/
	memcpy(p+5,zip.data(),zip.size());
	pos/=4096;
	if(pos>0xFFFFFFu){/*表头里偏移只有3字节*/
		return false;
	}
	data[loc]=(byte)(pos>>16);data[loc+1]=(byte)(pos>>8);data[loc+2]=(byte)pos;
	data[loc+3]=(byte)sectors;
	uint32_t now=(uint32_t)time(NULL);
	byte *ts=data.data()+4096+4*(x+z*32);
	ts[0]=(byte)(now>>24);ts[1]=(byte)(now>>16);ts[2]=(byte)(now>>8);ts[3]=(byte)now;
	return true;
}
bool MCA::set_Chunk(const Chunk &chunk,uint32_t x,uint32_t z){
	return write_CompoundAt(data,&bad,chunk,x,z);
}
bool MCA::set_Chunk(const Chunk &chunk){/*坐标从chunk自己的xPos/zPos取*/
	const nbt_Any *xPos=chunk.get("xPos");
	const nbt_Any *zPos=chunk.get("zPos");
	if(!xPos||xPos->type!=NBT_INT||!zPos||zPos->type!=NBT_INT){
		return false;
	}
	return set_Chunk(chunk,(uint32_t)xPos->as_Int()&31,(uint32_t)zPos->as_Int()&31);
}
bool MCA::set_Compound(const nbt_Compound &chunk,uint32_t x,uint32_t z){
	return write_CompoundAt(data,&bad,chunk,x,z);
}
bool MCA::format(){/*整理：按8K头索引顺序重排数据区，消除空洞、使区块稠密存储（只改内存）*/
	if(bad||data.size()<8192){
		return false;
	}
	struct Item{int index;size_t src;uint32_t len;uint32_t ts;size_t sectors;};
	std::vector<Item> items;
	items.reserve(1024);
	for(int i=0;i<1024;i++){
		size_t loc=4*(size_t)i;
		size_t off=(size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2];
		size_t cnt=data[loc+3];
		if(off==0&&cnt==0){
			continue;/*空格子*/
		}
		if(off<2||cnt==0){/*偏移指到表头里/扇区数0：表头坏了，不动*/
			return false;
		}
		size_t src=off*4096;
		if(src+5>data.size()){/*偏移指到文件外*/
			return false;
		}
		uint32_t len=(uint32_t)data[src]<<24|(uint32_t)data[src+1]<<16|(uint32_t)data[src+2]<<8|data[src+3];
		if(len<1||(size_t)(len-1)>data.size()-src-5){/*长度字段越界*/
			return false;
		}
		size_t bytes=4+(size_t)len;/*4字节长度 + 1字节类型 + len-1字节payload*/
		size_t need=(bytes+4095)/4096;
		if(need>255||cnt<need){/*表头说的扇区数装不下这一块：坏了*/
			return false;
		}
		uint32_t ts=(uint32_t)data[4096+loc]<<24|(uint32_t)data[4096+loc+1]<<16|
			(uint32_t)data[4096+loc+2]<<8|uint32_t(data[4096+loc+3]);
		items.push_back(Item{i,src,len,ts,need});
	}
	std::vector<byte> out(8192,0);/*新文件：8KB表头 + 按索引顺序一个接一个的区块*/
	uint32_t sector=2;/*0和1是表头*/
	for(const Item &it:items){
		if((size_t)sector+it.sectors>0xFFFFFFu){/*3字节偏移装不下*/
			return false;
		}
		size_t dst=(size_t)sector*4096;
		out.resize(dst+it.sectors*4096,0);
		memcpy(out.data()+dst,data.data()+it.src,4+(size_t)it.len);
		size_t loc=4*(size_t)it.index;
		out[loc]=(byte)(sector>>16);out[loc+1]=(byte)(sector>>8);out[loc+2]=(byte)sector;
		out[loc+3]=(byte)it.sectors;
		out[4096+loc]=(byte)(it.ts>>24);out[4096+loc+1]=(byte)(it.ts>>16);
		out[4096+loc+2]=(byte)(it.ts>>8);out[4096+loc+3]=(byte)it.ts;
		sector+=(uint32_t)it.sectors;
	}
	data.swap(out);
	return true;
}
bool MCA::save(const char *p)const{/*先写.tmp再rename，避免写坏存档*/
	if(bad){
		return false;
	}
	if(!p||!p[0]){
		return false;
	}
	std::string tmp=std::string(p)+".tmp";
	FILE *fp=fopen(tmp.c_str(),"wb");
	if(!fp){
		return false;
	}
	static const byte zero[8192]={0};
	const byte *src=data.empty()?zero:data.data();
	size_t size=data.empty()?sizeof(zero):data.size();
	size_t ret=fwrite(src,1,size,fp);
	bool ok=fclose(fp)==0;
	if(!ok||ret!=size||rename(tmp.c_str(),p)){
		remove(tmp.c_str());
		return false;
	}
	return true;
}
