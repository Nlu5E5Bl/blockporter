#include "nbt.hpp"
#include <zlib.h>/*编译加-lz*/
#include <time.h>
#include <sys/stat.h>
/*mca.cpp：区域文件读写*/

int64_t nbt_bad_chunk=0;/*读不出来的区块数（压缩坏/压缩类型不认识）：跳过这一块，不退出程序*/
static void bad_Chunk(const char *what,size_t a,size_t b){/*记一笔，并最多打前BAD_SHOW条（BAD_SHOW在nbt.hpp里）*/
	nbt_bad_chunk++;
	if(nbt_bad_chunk<=BAD_SHOW){
		printf("chunk bad:%s（%zu,%zu）：这个区块跳过（后面同类只计数）\n",what,a,b);
	}
}

std::vector<byte> inflate_data(const byte *in,size_t in_len){/*出错返回空（并计nbt_bad_chunk），调用者当"读不出来"*/
	std::vector<byte> none;
	if(in_len>0xFFFFFFFFu){/*z_stream的avail_in是32位：真到这一步说明上游的长度算错了*/
		bad_Chunk("input too big",in_len,0);
		return none;
	}
	if(in_len>(1u<<26)){/*>64MB 的区块在真存档里不存在（区块解压上限才256MB，压缩后不该这么大）：
	                        下面那个"按4倍预估"的首次分配会先要 >256MB，bad_alloc 没人兜就是 terminate*/
		bad_Chunk("compressed too big",in_len,0);
		return none;
	}
	z_stream s{};
	s.next_in=(byte*)in;s.avail_in=(uInt)in_len;
	/*首次别按4倍给：坏数据（或者被改过的length字段）会让 in_len 很大，而256MB的上限只在翻倍那一步查。
	  先给个小的（≤1MB），让下面的翻倍循环自己涨上去，涨过头照样在上限那儿刹住*/
	size_t init=in_len*4+4096;
	if(init>(1u<<20)){
		init=1u<<20;
	}
	std::vector<byte> out(init);
	s.next_out=out.data();s.avail_out=(uInt)out.size();
	inflateInit2(&s,47);/*47=自动认gzip还是zlib*/
	while(inflate(&s,Z_NO_FLUSH)!=Z_STREAM_END){
		if(s.avail_out==0){
			size_t used=s.total_out;
			if(out.size()*2>(1u<<28)){/*上限256MB：合法区块压得再好也到不了这个量级，
			                             再往下涨就是"压缩炸弹"（1MB的坏数据解出几个G，当场被OOM杀掉）*/
				inflateEnd(&s);
				bad_Chunk("output too big",out.size(),in_len);
				return none;
			}
			out.resize(out.size()*2);
			s.next_out=out.data()+used;s.avail_out=(uInt)(out.size()-used);
		}else{/*压缩数据坏了：这一块读不出来*/
			inflateEnd(&s);
			bad_Chunk("inflate",in_len,s.total_out);
			return none;
		}
	}
	inflateEnd(&s);
	out.resize(s.total_out);
	return out;
}
std::vector<byte> deflate_data(const byte *in,size_t in_len){/*write_Chunk默认用zlib*/
	uLongf len=compressBound(in_len);
	std::vector<byte> out(len);
	if(compress2(out.data(),&len,in,in_len,Z_DEFAULT_COMPRESSION)!=Z_OK){
		return std::vector<byte>();
	}
	out.resize(len);
	return out;
}

void mca_Path(char *out,size_t size,const char *dir,int32_t x,int32_t z){/*区域文件的路径只有这一个拼法（读/写/删都用它）*/
	int n=snprintf(out,size,"%s/r.%d.%d.mca",dir,x,z);
	if(n<0||(size_t)n>=size){/*路径长到装不下：给个空串，让后面的fopen/remove干净地失败，
	                            总比用截断出来的名字去读写（甚至删）别的文件强*/
		printf("mca: 路径太长装不下（%d字节，缓冲%zu）%s\n",n,size,dir);
		if(size){
			out[0]='\0';
		}
	}
}
mca_Region mca_Region::read(const char *path,int32_t x,int32_t z){/*path是目录*/
	char file[4096];
	mca_Path(file,sizeof(file),path,x,z);
	mca_Region mca;
	if(!file_Read_All(file,mca.data)){
		/*读不出来要分两种：文件**根本不在**（正常：这个区域还没生成）＝空镜像；
		  文件在、但读不出来（权限、IO错、是个目录、不是普通文件）＝bad，绝不能当成"这里没有区块"——
		  调用者一旦拿空镜像去写盘，就会把原文件里没碰过的区块整片抹掉（merge的entities/poi就这样踩过）。
		  这里用 stat 认"路径存不存在"：file_Read_All 对目录/设备文件也会失败，那些同样算 bad*/
		struct stat st;
		if(!stat(file,&st)){
			mca.bad=1;
			nbt_bad_chunk++;
			if(nbt_bad_chunk<=BAD_SHOW){
				printf("chunk bad: 区域文件 %s 存在但读不出来（权限/IO错误/不是普通文件？）：这个区域一个字节都不会动\n",file);
			}
		}
	}else if(mca.data.size()>0&&mca.data.size()<8192){
		/*文件在、也读到了，但连8KB表头都不够：表头被截断，里面声称有区块的格子其实读不出来。
		  同样算 bad——不然调用者会拿它当空镜像把整片覆盖掉。0字节是另一回事（游戏自己会留这种空文件，
		  merge 会把它当"空"来写/删，那是正常的）*/
		mca.bad=1;
		nbt_bad_chunk++;
		if(nbt_bad_chunk<=BAD_SHOW){
			printf("chunk bad: 区域文件 %s 只有 %zu 字节（不足8KB表头），按坏文件处理：一个字节都不会动\n",file,mca.data.size());
		}
	}
	return mca;
}
mca_Region mca_Region::empty(){/*新建空区域（8KB头），用来写还不存在的mca*/
	mca_Region mca;
	mca.data.resize(8192,0);
	return mca;
}
int mca_Region::has_Chunk(uint32_t x,uint32_t z)const{/*表头那一格的偏移不是0就是有区块*/
	if(data.size()<8192||x>31||z>31){
		return 0;
	}
	size_t loc=4*(x+z*32);
	return (data[loc]|data[loc+1]|data[loc+2])!=0;
}
int mca_Region::is_Empty()const{/*一格都没有：这个文件等于不存在（比"文件在不在"更准，0字节/只有头也算空）*/
	if(data.size()<8192){
		return 1;
	}
	for(size_t i=0;i<1024;i++){
		if(data[i*4]|data[i*4+1]|data[i*4+2]){
			return 0;
		}
	}
	return 1;
}
int mca_Region::erase_Chunk(uint32_t x,uint32_t z){/*把这一格从表头删掉：数据区留着当空洞（不重排文件，省得动别的区块）*/
	if(data.size()<8192||x>31||z>31){
		return 0;
	}
	size_t loc=4*(x+z*32);
	if(!(data[loc]|data[loc+1]|data[loc+2]|data[loc+3])){
		return 0;/*本来就没有*/
	}
	memset(data.data()+loc,0,4);
	memset(data.data()+4096+loc,0,4);/*时间戳一起清掉，免得看着像有东西*/
	return 1;
}
/*把一个区块的原始字节（已解压）取出来；0=这一格没有/文件被截短/压缩坏（坏数据在里面已经计过数）*/
static int chunk_Raw(const std::vector<byte> &data,uint32_t x,uint32_t z,std::vector<byte> &raw){
	if(data.size()<8192||x>31||z>31){
		return 0;
	}
	size_t loc=4*(x+z*32);
	size_t pos=((size_t)data[loc]<<16|data[loc+1]<<8|data[loc+2])*4096;
	if(pos==0){
		return 0;/*表头这一格是空的：这一格本来就没有区块（正常，不是坏数据）*/
	}
	if(pos+5>data.size()){
		/*表头说这里有区块，但偏移指到文件外（文件被截短、或者表头被人改坏）：这是坏数据，要计数——
		  不计数的话"区块凭空消失"会被当成"这里本来就没区块"，merge/fixer会一路报成功还退出码0*/
		bad_Chunk("header out of file",pos,data.size());
		return 0;
	}
	uint32_t len=(uint32_t)data[pos]<<24|(uint32_t)data[pos+1]<<16|(uint32_t)data[pos+2]<<8|data[pos+3];/*这个长度含类型字节，所以压缩数据是len-1*/
	byte type=data[pos+4];
	if(len<1||pos+5+(size_t)(len-1)>data.size()){
		bad_Chunk("bad length",len,pos);/*长度字段不合理（0，或者超出文件）：同样算坏数据*/
		return 0;
	}
	if(type==0x01||type==0x02){
		raw=inflate_data(data.data()+pos+5,len-1);
		if(raw.empty()){/*压缩数据坏了（inflate_data已经记过数）：这个区块读不出来*/
			return 0;
		}
	}else if(type==0x03){
		raw.assign(data.begin()+pos+5,data.begin()+pos+5+(len-1));
	}else{
		bad_Chunk("compression type",type,len);/*不认识的压缩类型：跳过这个区块*/
		return 0;
	}
	return 1;
}
Chunk *mca_Region::read_Chunk(uint32_t x,uint32_t z)const{
	std::vector<byte> raw;
	if(!chunk_Raw(data,x,z,raw)){
		return NULL;
	}
	nbt_Raw nbt;
	nbt.data=std::move(raw);
	return Chunk::read(&nbt);/*里面有坏NBT的话会返回NULL并计nbt_bad_nbt*/
}
nbt_Compound *mca_Region::read_Compound(uint32_t x,uint32_t z)const{/*实体/POI区块：就是个普通compound，没有xPos/zPos那套*/
	std::vector<byte> raw;
	if(!chunk_Raw(data,x,z,raw)){
		return NULL;
	}
	nbt_Raw nbt;
	nbt.data=std::move(raw);
	return nbt.read_Root();/*坏数据返回NULL并计nbt_bad_nbt*/
}
int mca_Region::write_ChunkAt(nbt_Compound *chunk,uint32_t x,uint32_t z){/*写入内存里的区域镜像，返回是否成功*/
	if(data.size()<8192||!chunk||x>31||z>31){
		printf("write_Chunk: bad region or chunk %u %u\n",x,z);
		return 0;
	}
	nbt_Raw raw=chunk->to_Raw();
	std::vector<byte> zip=deflate_data(raw.data.data(),raw.data.size());
	if(zip.empty()){
		printf("write_Chunk: deflate error\n");
		return 0;
	}
	size_t need=5+zip.size(),sectors=(need+4095)/4096;
	if(sectors>255){
		printf("write_Chunk: too big %zu\n",need);
		return 0;
	}
	size_t loc=4*(x+z*32);
	size_t pos=((size_t)data[loc]<<16|data[loc+1]<<8|data[loc+2])*4096;
	size_t old_sectors=(size_t)data[loc+3];
	/*"原sector放得下就原地写"：除了长度够，还必须**整个旧sector都在文件里**。
	  表头的3字节偏移是文件里存着的数，文件被截短过（或者表头被人改坏）时它可能指到文件外，
	  这时按它去memset就是往堆外写——所以这里一定要连pos一起查，不合规就当成"放不下"，追加到末尾*/
	if(pos&&pos+old_sectors*4096<=data.size()&&old_sectors*4096>=need){
		memset(data.data()+pos,0,old_sectors*4096);
	}else{/*放不下就追加到文件末尾，旧sector留成空洞*/
		/*表头里的偏移是"第几个4096字节"，所以新数据的起点必须**对齐到4096**：
		  文件长度不是4096整数倍时（拷贝中断/被截短/别的工具写的），直接按 data.size() 追加会让
		  表头记成 size/4096（向下取整）＝指到新数据前面去——刚写的区块立刻读不回来，而且
		  write_ChunkAt 还返回1，整个链路一声不响（实测过：写成功的区块重新打开是空的，err_cnt=0、bad计数=0）。
		  补几个字节的0对齐，最多浪费4095字节*/
		size_t pad=(4096-data.size()%4096)%4096;
		pos=data.size()+pad;
		data.resize(pos+sectors*4096,0);
	}
	byte *p=data.data()+pos;
	size_t total=zip.size()+1;/*长度字段含类型字节*/
	p[0]=(byte)(total>>24);p[1]=(byte)(total>>16);p[2]=(byte)(total>>8);p[3]=(byte)total;
	p[4]=0x02;/*zlib*/
	memcpy(p+5,zip.data(),zip.size());
	pos/=4096;
	if(pos>0xFFFFFFu){/*表头里偏移只有3字节（最多256MB/4KB=64K个sector）：真到这一步就写不下了*/
		printf("write_Chunk: region too big %zu\n",pos);
		return 0;
	}
	data[loc]=(byte)(pos>>16);data[loc+1]=(byte)(pos>>8);data[loc+2]=(byte)pos;
	data[loc+3]=(byte)sectors;
	uint32_t now=(uint32_t)time(NULL);
	byte *ts=data.data()+4096+4*(x+z*32);
	ts[0]=(byte)(now>>24);ts[1]=(byte)(now>>16);ts[2]=(byte)(now>>8);ts[3]=(byte)now;
	return 1;
}
void mca_Region::write_Chunk(nbt_Compound *chunk){/*不返回，坐标从chunk自己的xPos/zPos取*/
	if(!chunk){
		printf("write_Chunk: no chunk\n");
		return;
	}
	const nbt_Any *xPos=chunk->get("xPos");
	const nbt_Any *zPos=chunk->get("zPos");
	/*必须确认是Int：as_Int()对别的类型返回0，那样会把区块写到(0,0)那个槽上，覆盖掉别人的区块*/
	if(!xPos||xPos->type!=0x03||!zPos||zPos->type!=0x03){
		printf("write_Chunk: no xPos/zPos(Int)\n");
		return;
	}
	write_ChunkAt(chunk,(uint32_t)xPos->as_Int()&31,(uint32_t)zPos->as_Int()&31);
}
int mca_Region::write_File(const char *file)const{/*先写.tmp再rename，避免写坏存档*/
	std::string tmp=std::string(file)+".tmp";
	FILE *fp=fopen(tmp.c_str(),"wb");
	if(!fp){
		printf("write_Region: cannot open %s\n",tmp.c_str());
		return 0;
	}
	size_t ret=fwrite(data.data(),1,data.size(),fp);
	fclose(fp);
	if(ret!=data.size()||rename(tmp.c_str(),file)){
		printf("write_Region: cannot write %s\n",file);
		remove(tmp.c_str());/*写坏了/改不了名就把半截的.tmp删掉，别在存档目录里留垃圾*/
		return 0;
	}
	return 1;
}
int mca_Region::write(const char *path,uint32_t x,uint32_t z)const{/*path是目录，x/z是region坐标，写法同read*/
	char file[4096];
	mca_Path(file,sizeof(file),path,(int32_t)x,(int32_t)z);
	return write_File(file);
}
