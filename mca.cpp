#include <algorithm>
#include <dirent.h>
#include "nbtrw.hpp"
#include <zlib.h>/*编译加-lz*/
#include <lz4.h>/*编译加-llz4（lz4-devel）*/
#include <xxhash.h>/*编译加-lxxhash（xxhash-devel）：lz4-java 的块校验是 XXHash32*/
#include <time.h>
#include <sys/stat.h>
/*mca.cpp：区域文件读写 + zlib/gzip/lz4
 *	MCA = 一个.mca文件的内存镜像。构造：MCA()是空区域（8KB表头），MCA(path)读文件。
 *	读不到的mca：文件不在→data空、bad=false（正常，这片没生成）；文件在但读不出来→bad=true（绝不能当成空区域写回去）。
 *	读写的都是 nbt_Compound（region 区块/实体/POI 一样）：读不到的给 nullopt（表里没这格/压缩坏/NBT坏，原因看 last_error）；
 *	要当区块用由调用者 Chunk c(std::move(*r))，写的时候传 c.con。
 *	读一格照游戏的 RegionFile.getChunkDataInputStream 走（wiki Region_file_format）：
 *	  压缩类型 1=gzip、2=zlib、3=不压、4=lz4（lz4-java 的 LZ4Block 流）、127=自定义（模组的，本库和原版游戏都读不了）；
 *	  类型的最高位(128)=数据在外置文件 c.<区块x>.<区块z>.mcc（和 .mca 同一目录，坐标是区块绝对坐标），表里只有5字节的桩。
 *	  游戏是边解压边读 NBT，读完根 compound 就不往后读：所以压缩流后面坏了/没结束块不算错，只要 NBT 能完整读出来。
 *	写一格：按 compression 压缩；压完要占 256 个扇区及以上（1MB）就像游戏一样放外置 .mcc，save() 时一起写。
 *	format()按8K头索引顺序（0,0 < 1,0 < … < 31,0 < 0,1 < … < 31,31）重排数据区，消除空洞、稠密存储。
 */

/*zlib 解压的公共部分：window_bits 15=zlib、31=gzip、47=两种都认。complete 非空时出错也给已经解出来的前缀*/
static std::vector<byte> inflate_With(const byte *in,size_t in_len,int window_bits,bool *complete){
	std::vector<byte> out;
	if(complete){
		*complete=false;
	}
	if(!in&&in_len){
		return out;
	}
	z_stream s{};
	if(inflateInit2(&s,window_bits)!=Z_OK){
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
		return complete?out:std::vector<byte>();
	}
	if(complete){
		*complete=true;
	}
	return out;
}
std::vector<byte> inflate_Data(const byte *in,size_t in_len){/*gzip/zlib都认；出错返回空*/
	return inflate_With(in,in_len,47,NULL);
}
std::vector<byte> inflate_Prefix(const byte *in,size_t in_len,bool gzip,bool *complete){/*只认一种格式（gzip 或 zlib）；流坏在半路也把已经解出来的前缀交出去，*complete 说流是不是完整结束*/
	return inflate_With(in,in_len,gzip?31:15,complete);
}

/*---- lz4：lz4-java 的 LZ4Block 流（游戏的 region 压缩类型 4 用 LZ4BlockInputStream/OutputStream） ----
  每块：魔数"LZ4Block" + token(方法|级别) + 压缩长度/原长度/校验（各4字节小端） + 数据。
  方法 0x10=原样、0x20=lz4；级别 = log2(块大小上限)-10；校验 = XXHash32(种子0x9747b28c) & 0x0FFFFFFF；
  最后一块长度都是0、校验0（结束块）。读法照 lz4-java 1.10.1（26.1.1 的 jar 里带的）的 LZ4BlockInputStream.refill。*/
static const byte lz4_Magic[8]={'L','Z','4','B','l','o','c','k'};
#define LZ4B_HEADER 21
#define LZ4B_SEED 0x9747b28cu
#define LZ4B_BLOCK (1<<16)/*LZ4BlockOutputStream 默认块大小*/
static uint32_t read_LE32(const byte *p){
	return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void write_LE32(byte *p,uint32_t v){/*小端写 4 字节（LZ4Block 的块头是小端，和 NBT/MCA 的大端相反）*/
	p[0]=(byte)v;p[1]=(byte)(v>>8);p[2]=(byte)(v>>16);p[3]=(byte)(v>>24);
}
static uint32_t get_Lz4Check(const byte *p,size_t n){/*LZ4Block 的块校验：XXHash32 取低 28 位*/
	return (uint32_t)XXH32(p,n,LZ4B_SEED)&0x0FFFFFFFu;
}
std::vector<byte> unlz4_Data(const byte *in,size_t in_len,bool *complete){/*解 LZ4Block 流：一块一块解，验过校验才接到输出后面；哪一块不对就停，给前面解好的（*complete=false）*/
	std::vector<byte> out;
	if(complete){
		*complete=false;
	}
	if(!in&&in_len){
		return out;
	}
	size_t pos=0;
	std::vector<byte> block;
	for(;;){
		if(in_len-pos<LZ4B_HEADER){/*读不满一个块头：lz4-java 在这里抛“Stream ended prematurely”*/
			return out;
		}
		const byte *h=in+pos;
		if(memcmp(h,lz4_Magic,8)){
			return out;
		}
		uint32_t token=h[8];
		uint32_t method=token&0xF0,level=10+(token&0x0F);
		if(method!=0x10&&method!=0x20){
			return out;
		}
		int32_t clen=(int32_t)read_LE32(h+9),olen=(int32_t)read_LE32(h+13);
		uint32_t check=read_LE32(h+17);
		if((int64_t)olen>((int64_t)1<<level)||olen<0||clen<0||(olen==0&&clen!=0)||(olen!=0&&clen==0)||
		   (method==0x10&&olen!=clen)){
			return out;
		}
		pos+=LZ4B_HEADER;
		if(olen==0&&clen==0){/*结束块*/
			if(check!=0){
				return out;
			}
			if(complete){
				*complete=true;
			}
			return out;
		}
		if(in_len-pos<(size_t)clen){
			return out;
		}
		block.resize((size_t)olen);
		if(method==0x10){
			memcpy(block.data(),in+pos,(size_t)olen);
		}else{
			/*lz4-java 用 fast 解压器：按原长度解，要求正好用掉 clen 个字节；safe 解压器给的输入就是 clen 字节，
			  解出来正好 olen 个就等价*/
			int got=LZ4_decompress_safe((const char*)(in+pos),(char*)block.data(),clen,olen);
			if(got!=olen){
				return out;
			}
		}
		if(get_Lz4Check(block.data(),(size_t)olen)!=check){/*校验不对：这一块不交出去*/
			return out;
		}
		pos+=(size_t)clen;
		out.insert(out.end(),block.begin(),block.end());
	}
}
std::vector<byte> lz4_Data(const byte *in,size_t in_len){/*压成 LZ4Block 流：64KB 一块，压不小的块原样存，最后跟一个结束块*/
	std::vector<byte> out;
	if(!in&&in_len){
		return out;
	}
	const uint32_t level=6;/*LZ4B_BLOCK=2^16：log2-10*/
	std::vector<char> buf((size_t)LZ4_compressBound(LZ4B_BLOCK));
	for(size_t pos=0;pos<in_len;){
		size_t n=in_len-pos<(size_t)LZ4B_BLOCK?in_len-pos:(size_t)LZ4B_BLOCK;
		int c=LZ4_compress_default((const char*)(in+pos),buf.data(),(int)n,(int)buf.size());
		bool raw=c<=0||(size_t)c>=n;/*压不小就原样存（和 lz4-java 一样）*/
		size_t clen=raw?n:(size_t)c;
		size_t at=out.size();
		out.resize(at+LZ4B_HEADER+clen);
		byte *h=out.data()+at;
		memcpy(h,lz4_Magic,8);
		h[8]=(byte)((raw?0x10:0x20)|level);
		write_LE32(h+9,(uint32_t)clen);
		write_LE32(h+13,(uint32_t)n);
		write_LE32(h+17,get_Lz4Check(in+pos,n));
		memcpy(h+LZ4B_HEADER,raw?(const char*)(in+pos):buf.data(),clen);
		pos+=n;
	}
	size_t at=out.size();/*结束块*/
	out.resize(at+LZ4B_HEADER,0);
	memcpy(out.data()+at,lz4_Magic,8);
	out[at+8]=(byte)(0x10|level);
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
		/*文件在、也读到了，但连8KB表头都不够：游戏（RegionFile 构造）只 WARN “truncated header”，照读读到的那部分，
		  没读到的格子当没有（0字节连 WARN 都没有）。照游戏：补0到8KB，记下来给 fixer 报；存盘时表头就写全了*/
		truncated=!data.empty();
		data.resize(8192,0);
	}
}
MCA::MCA(const std::string &p):MCA(p.c_str()){}
/*表头一格（3字节偏移+1字节扇区数）的三种样子，照游戏的 RegionFile 构造函数：
  全是0＝空；偏移指在表头里（<2）、扇区数是0、偏移在文件外＝偏移异常，游戏 WARN 一声、把这一格当成 0（没有这个区块）；
  别的＝有区块（数据读不读得出来是另一回事）*/
enum{SLOT_EMPTY=0,SLOT_OK=1,SLOT_BAD=2};
static int get_SlotState(const std::vector<byte> &data,size_t idx){/*表头第 idx 格是 SLOT_*（规矩见上）*/
	size_t loc=4*idx;
	size_t sector=(size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2];
	size_t cnt=data[loc+3];
	if(!sector&&!cnt){
		return SLOT_EMPTY;
	}
	if(sector<2||!cnt||sector*4096>data.size()){
		return SLOT_BAD;
	}
	return SLOT_OK;
}
bool MCA::has_Chunk(uint32_t x,uint32_t z)const{/*表头这一格有区块（偏移正常）。偏移异常的格子游戏当没有，这里也给 false
	（要分清用 has_BadOffset；format()/clean_Offsets() 把它们清成 0）*/
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	return get_SlotState(data,x+z*32)==SLOT_OK;
}
bool MCA::has_BadOffset(uint32_t x,uint32_t z)const{/*表头这一格不是空的、但偏移异常（游戏当没有这个区块）*/
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	return get_SlotState(data,x+z*32)==SLOT_BAD;
}
int MCA::clean_Offsets(){/*把偏移异常的格子清成 0（连时间戳），和游戏打开文件时做的一样；给清了几格*/
	if(bad||data.size()<8192){
		return 0;
	}
	int n=0;
	for(uint32_t i=0;i<1024;i++){
		if(get_SlotState(data,i)==SLOT_BAD){
			memset(data.data()+4*i,0,4);
			memset(data.data()+4096+4*i,0,4);
			ext.erase(i);
			touched.insert(i);
			n++;
		}
	}
	return n;
}
bool MCA::is_Empty()const{/*一个区块都没有（偏移异常的格子不算）：这个文件等于不存在*/
	if(data.size()<8192){
		return true;
	}
	for(size_t i=0;i<1024;i++){
		if(get_SlotState(data,i)==SLOT_OK){
			return false;
		}
	}
	return true;
}
bool MCA::erase_Chunk(uint32_t x,uint32_t z){/*把这一格从表头删掉：数据区留着当空洞（format会收拾）。偏移异常的格子也清得掉*/
	if(data.size()<8192||x>31||z>31){
		return false;
	}
	size_t loc=4*(x+z*32);
	if(!(data[loc]|data[loc+1]|data[loc+2]|data[loc+3])){
		return false;/*本来就没有*/
	}
	memset(data.data()+loc,0,4);
	memset(data.data()+4096+loc,0,4);/*时间戳一起清掉*/
	uint32_t idx=x+z*32;
	ext.erase(idx);
	touched.insert(idx);/*save() 时把这一格的外置 .mcc 删掉（游戏 clear() 也删）*/
	return true;
}
/*---- 外置 .mcc ----*/
/*文件名 r.<x>.<z>.mca 里的区域坐标。只认游戏自己拼的写法（RegionFileStorage："r."+x+"."+z+".mca"，十进制、没有多余的
  0 和正号）：r.01.0.mca、r.+1.0.mca 游戏不读，也不能和 r.1.0.mca 算成同一个区域（列目录时会把一个区域走两遍）*/
static bool get_RegionPos(const std::string &path,int32_t *rx,int32_t *rz){
	size_t slash=path.rfind('/');
	std::string name=slash==std::string::npos?path:path.substr(slash+1);
	int n=0;
	long long x=0,z=0;
	if(sscanf(name.c_str(),"r.%lld.%lld.mca%n",&x,&z,&n)!=2||(size_t)n!=name.size()||
	   x<INT32_MIN/32||x>INT32_MAX/32||z<INT32_MIN/32||z>INT32_MAX/32||
	   name!="r."+std::to_string(x)+"."+std::to_string(z)+".mca"){
		return false;
	}
	*rx=(int32_t)x;
	*rz=(int32_t)z;
	return true;
}
std::string get_ExternalPath(const std::string &mca_path,uint32_t x,uint32_t z){/*这一格的外置文件路径：和 .mca 同目录的 c.<区块绝对x>.<区块绝对z>.mcc；.mca 的文件名不合规矩给空串*/
	int32_t rx=0,rz=0;
	if(x>31||z>31||!get_RegionPos(mca_path,&rx,&rz)){
		return std::string();
	}
	size_t slash=mca_path.rfind('/');
	std::string dir=slash==std::string::npos?std::string("."):mca_path.substr(0,slash);
	return dir+"/c."+std::to_string((int64_t)rx*32+x)+"."+std::to_string((int64_t)rz*32+z)+".mcc";
}
std::vector<std::pair<int32_t,int32_t>> list_MCAFiles(const char *dir){/*目录里的 r.<x>.<z>.mca（普通文件），按坐标排序*/
	std::vector<std::pair<int32_t,int32_t>> out;
	DIR *d=dir?opendir(dir):NULL;
	if(!d){
		return out;
	}
	while(struct dirent *e=readdir(d)){
		int32_t rx=0,rz=0;
		if(!get_RegionPos(e->d_name,&rx,&rz)){
			continue;
		}
		std::string f=std::string(dir)+"/"+e->d_name;
		struct stat st;
		if(!stat(f.c_str(),&st)&&S_ISREG(st.st_mode)){
			out.push_back({rx,rz});
		}
	}
	closedir(d);
	std::sort(out.begin(),out.end());
	return out;
}
/*表头这一格指的是不是外置桩（类型字节最高位）；表头/偏移坏了给false*/
static bool is_ExternalStub(const std::vector<byte> &data,uint32_t idx){
	if(data.size()<8192||idx>=1024){
		return false;
	}
	size_t loc=4*(size_t)idx;
	size_t sector=(size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2];
	if(get_SlotState(data,idx)!=SLOT_OK||sector*4096+5>data.size()){
		return false;
	}
	return (data[sector*4096+4]&0x80)!=0;
}
/*解压一格的压缩流：返回MCA_READ_*；raw 是能解出来的前缀，*complete=流是不是完整结束的*/
static int decode_Stream(uint8_t type,const byte *p,size_t n,std::vector<byte> &raw,bool *complete){
	*complete=false;
	switch(type){
	case MCA_GZIP:
		raw=inflate_Prefix(p,n,true,complete);
		return MCA_READ_OK;
	case MCA_ZLIB:
		raw=inflate_Prefix(p,n,false,complete);
		return MCA_READ_OK;
	case MCA_NONE:
		raw.assign(p,p+n);
		*complete=true;
		return MCA_READ_OK;
	case MCA_LZ4:
		raw=unlz4_Data(p,n,complete);
		return MCA_READ_OK;
	case MCA_CUSTOM:
		return MCA_READ_UNSUPPORTED_COMPRESSION;/*模组的自定义压缩：本库和原版游戏都读不了*/
	default:
		return MCA_READ_BAD;/*游戏：invalid chunk stream version（ERROR）*/
	}
}
/*把一个区块的原始字节（已解压的前缀）取出来；返回MCA_READ_*。检查的顺序和游戏一样：
  表头偏移异常（在表头里/扇区数0/在文件外：游戏打开文件时 WARN、当这格没有）给 NO_CHUNK；
  不够5字节、长度0（WARN）、外置文件不在（ERROR）、长度超出扇区/负数（ERROR）、压缩类型不认识（ERROR）都是 BAD；127 自定义压缩是 UNSUPPORTED。*/
static int read_ChunkRaw(const MCA &m,uint32_t x,uint32_t z,std::vector<byte> &raw,bool *complete){
	const std::vector<byte> &data=m.data;
	*complete=false;
	if(x>31||z>31){
		return MCA_READ_BAD;
	}
	if(data.empty()){/*文件不在的空镜像：这个区域一个区块都没有（坏文件在调用前已经按bad挡掉）*/
		return MCA_READ_NO_CHUNK;
	}
	if(data.size()<8192){
		return MCA_READ_BAD;
	}
	uint32_t idx=x+z*32;
	size_t loc=4*(size_t)idx;
	size_t sector=(size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2];
	size_t cnt=data[loc+3];
	if(get_SlotState(data,idx)!=SLOT_OK){
		return MCA_READ_NO_CHUNK;/*表头这一格是空的，或者偏移异常（游戏当它是空的）：没有这个区块，不是坏数据*/
	}
	size_t pos=sector*4096;
	size_t avail=data.size()-pos;
	if(avail>cnt*4096){
		avail=cnt*4096;
	}
	if(avail<5){
		return MCA_READ_BAD;
	}
	int32_t len=(int32_t)((uint32_t)data[pos]<<24|(uint32_t)data[pos+1]<<16|(uint32_t)data[pos+2]<<8|data[pos+3]);
	uint8_t type=data[pos+4];
	if(len==0){
		return MCA_READ_BAD;
	}
	if(type&MCA_EXTERNAL){/*外置：表里的长度应该是1（不是也照读外置文件，游戏只 WARN）*/
		auto it=m.ext.find(idx);
		if(it!=m.ext.end()){
			return decode_Stream(type&0x7F,it->second.data(),it->second.size(),raw,complete);
		}
		std::string file=get_ExternalPath(m.path,x,z);
		std::vector<byte> buf;
		if(file.empty()||!read_File(file.c_str(),buf)){
			return MCA_READ_BAD;
		}
		return decode_Stream(type&0x7F,buf.data(),buf.size(),raw,complete);
	}
	if(len<1||(size_t)(len-1)>avail-5){/*负数/超出这几个扇区：别让它跨读下一个区块*/
		return MCA_READ_BAD;
	}
	return decode_Stream(type,data.data()+pos+5,(size_t)(len-1),raw,complete);
}
std::optional<nbt_Compound> MCA::get_Compound(uint32_t x,uint32_t z)const{/*一格就是一个根compound*/
	last_error=MCA_READ_OK;
	if(bad){
		last_error=MCA_READ_BAD;
		return std::nullopt;/*这个文件没读出来：不能拿半截镜像去解析*/
	}
	std::vector<byte> raw;
	bool complete=false;
	int why=read_ChunkRaw(*this,x,z,raw,&complete);
	if(why!=MCA_READ_OK){
		last_error=why;
		return std::nullopt;
	}
	nbt_Raw nbt;
	nbt.data=std::move(raw);
	nbt_Compound *c=nbt.read_Root();/*只读根 compound，后面多出来的不管（游戏也不往后读）*/
	if(!c){
		last_error=complete?MCA_READ_BAD_NBT:MCA_READ_BAD;/*流本身就坏在半路：算压缩坏*/
		return std::nullopt;
	}
	std::optional<nbt_Compound> r(std::move(*c));/*把树挪出来，空壳释放掉*/
	c->free();
	return r;
}
static bool has_SectorOverlap(const std::vector<byte> &data,size_t self_loc,size_t pos,size_t bytes){/*表头里除了自己（self_loc）以外，还有没有别的格子占着 [pos,pos+bytes) 这段*/
	/*表头里别的区块占了这整段旧扇区？占着就不能原地memset，否则会把下一块擦掉。
	   这种表头本身是坏的；改成追加写，保留邻居，旧扇区留成洞。*/
	if(data.size()<8192){
		return false;
	}
	size_t end=pos+bytes;
	for(size_t i=0;i<1024;i++){
		size_t loc=i*4;
		if(loc==self_loc){
			continue;
		}
		size_t p=((size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2])*4096;
		size_t cnt=(size_t)data[loc+3];
		if(get_SlotState(data,i)!=SLOT_OK){
			continue;/*空格子、偏移异常的格子（游戏当没有）不占地方*/
		}
		size_t s=p,e=s+cnt*4096;
		if(pos<e&&s<end){
			return true;
		}
	}
	return false;
}
/*按压缩类型压一格（1/2/3/4）；不认识的类型给 nullopt*/
static std::optional<std::vector<byte>> encode_Stream(uint8_t type,const std::vector<byte> &raw){
	switch(type){
	case MCA_GZIP:{
		std::vector<byte> z=gzip_Data(raw.data(),raw.size());
		if(z.empty()){
			return std::nullopt;
		}
		return z;
	}
	case MCA_ZLIB:{
		std::vector<byte> z=deflate_Data(raw.data(),raw.size());
		if(z.empty()){
			return std::nullopt;
		}
		return z;
	}
	case MCA_NONE:
		return raw;
	case MCA_LZ4:
		return lz4_Data(raw.data(),raw.size());
	default:
		return std::nullopt;
	}
}
static bool write_CompoundAt(MCA &m,const nbt_Compound &chunk,uint32_t x,uint32_t z){/*编码+压缩，写进内存镜像的 (x,z) 这一格：旧扇区放得下就原地写，不然追加到末尾；太大的放外置*/
	std::vector<byte> &data=m.data;
	if(m.bad){/*文件没读出来：绝不写*/
		return false;
	}
	if(x>31||z>31){
		return false;
	}
	nbt_Raw raw=chunk.to_Raw();
	if(raw.fail){
		return false;
	}
	std::optional<std::vector<byte>> zip=encode_Stream(m.compression,raw.data);
	if(!zip||zip->size()>0x7FFFFFFEu){/*长度字段是有符号4字节*/
		return false;
	}
	if(data.size()<8192){/*空镜像（新区域）：先建8KB表头*/
		data.assign(8192,0);
	}
	uint32_t idx=x+z*32;
	size_t need=5+zip->size(),sectors=(need+4095)/4096;
	bool external=sectors>=256;/*MCA格式：扇区数是1字节；游戏 >=256 个扇区就放外置 .mcc*/
	if(external){
		need=5;
		sectors=1;
	}
	size_t loc=4*(size_t)idx;
	size_t pos=((size_t)data[loc]<<16|data[loc+1]<<8|data[loc+2])*4096;
	size_t old_sectors=(size_t)data[loc+3];
	/*“原sector放得下就原地写”：除了长度够，还必须整个旧sector都在文件里，
	  否则按坏偏移去memset就是往堆外写；旧sector若和别的区块重叠也只能追加。
	  偏移指在表头里（第0、1个扇区）的是坏表头：原地写会把时间戳表当数据区覆盖掉，写完偏移还是<2，游戏照样不认，
	  所以也只能追加*/
	bool in_place=pos>=8192&&pos+old_sectors*4096<=data.size()&&old_sectors*4096>=need&&
		!has_SectorOverlap(data,loc,pos,old_sectors*4096);
	if(in_place){
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
	if(pos/4096>0xFFFFFFu){/*表头里偏移只有3字节*/
		return false;
	}
	byte *p=data.data()+pos;
	size_t total=external?1:zip->size()+1;/*长度字段含类型字节；外置桩只有类型字节*/
	p[0]=(byte)(total>>24);p[1]=(byte)(total>>16);p[2]=(byte)(total>>8);p[3]=(byte)total;
	p[4]=(byte)(m.compression|(external?MCA_EXTERNAL:0));
	if(external){
		m.ext[idx]=std::move(*zip);/*save() 时写成 c.<x>.<z>.mcc*/
	}else{
		memcpy(p+5,zip->data(),zip->size());
		m.ext.erase(idx);
	}
	m.touched.insert(idx);
	size_t sec=pos/4096;
	data[loc]=(byte)(sec>>16);data[loc+1]=(byte)(sec>>8);data[loc+2]=(byte)sec;
	data[loc+3]=(byte)sectors;
	uint32_t now=(uint32_t)time(NULL);
	byte *ts=data.data()+4096+loc;
	ts[0]=(byte)(now>>24);ts[1]=(byte)(now>>16);ts[2]=(byte)(now>>8);ts[3]=(byte)now;
	return true;
}
bool MCA::set_Compound(const nbt_Compound &chunk,uint32_t x,uint32_t z){/*写一格（只改内存镜像，save() 才落盘）*/
	return write_CompoundAt(*this,chunk,x,z);
}
bool MCA::format(){/*整理：偏移异常的格子清成 0，其余按8K头索引顺序重排数据区，消除空洞、使区块稠密存储（只改内存）*/
	if(bad||data.size()<8192){
		return false;
	}
	clean_Offsets();/*游戏打开文件时也是先把这些格子当 0*/
	struct Item{int index;size_t src;size_t bytes;uint32_t ts;size_t sectors;};
	std::vector<Item> items;
	items.reserve(1024);
	for(int i=0;i<1024;i++){
		if(get_SlotState(data,(size_t)i)!=SLOT_OK){
			continue;/*空格子*/
		}
		size_t loc=4*(size_t)i;
		size_t off=(size_t)data[loc]<<16|(size_t)data[loc+1]<<8|data[loc+2];
		size_t cnt=data[loc+3];
		size_t src=off*4096;
		size_t avail=std::min(data.size()-src,cnt*4096);
		size_t bytes=avail,need=cnt;
		if(avail>=5){
			uint32_t len=(uint32_t)data[src]<<24|(uint32_t)data[src+1]<<16|(uint32_t)data[src+2]<<8|data[src+3];
			if(len>=1&&(size_t)(len-1)<=avail-5){/*长度字段对得上：只搬用到的那几个扇区*/
				bytes=4+(size_t)len;/*4字节长度 + 1字节类型 + len-1字节payload*/
				need=(bytes+4095)/4096;
			}
		}
		/*长度字段对不上的（不够5字节、长度0/负数/超出扇区：游戏读这一格会报错）是区块数据坏，不是表头坏：
		  原样把它占的扇区搬过去（文件里有多少搬多少），删不删由调用者定*/
		uint32_t ts=(uint32_t)data[4096+loc]<<24|(uint32_t)data[4096+loc+1]<<16|
			(uint32_t)data[4096+loc+2]<<8|uint32_t(data[4096+loc+3]);
		items.push_back(Item{i,src,bytes,ts,need});
	}
	std::vector<byte> out(8192,0);/*新文件：8KB表头 + 按索引顺序一个接一个的区块*/
	uint32_t sector=2;/*0和1是表头*/
	for(const Item &it:items){
		if((size_t)sector+it.sectors>0xFFFFFFu){/*3字节偏移装不下*/
			return false;
		}
		size_t dst=(size_t)sector*4096;
		out.resize(dst+it.sectors*4096,0);
		memcpy(out.data()+dst,data.data()+it.src,it.bytes);
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
static bool save_FileAtomic(const std::string &p,const byte *src,size_t size){/*先写.tmp再rename*/
	std::string tmp=p+".tmp";
	FILE *fp=fopen(tmp.c_str(),"wb");
	if(!fp){
		return false;
	}
	size_t ret=size?fwrite(src,1,size,fp):0;
	bool ok=fclose(fp)==0;
	if(!ok||ret!=size||rename(tmp.c_str(),p.c_str())){
		remove(tmp.c_str());
		return false;
	}
	return true;
}
bool MCA::save(const char *p)const{/*先写外置 .mcc，再写 .mca（都是先写.tmp再rename），最后删掉不再用的 .mcc*/
	if(bad){
		return false;
	}
	if(!p||!p[0]){
		return false;
	}
	std::string target(p);
	for(uint32_t idx=0;idx<1024;idx++){
		if(!is_ExternalStub(data,idx)){
			continue;
		}
		std::string to=get_ExternalPath(target,idx%32,idx/32);
		if(to.empty()){/*目标文件名不是 r.<x>.<z>.mca：外置文件不知道该叫什么*/
			return false;
		}
		auto it=ext.find(idx);
		if(it!=ext.end()){
			if(!save_FileAtomic(to,it->second.data(),it->second.size())){
				return false;
			}
			continue;
		}
		std::string from=get_ExternalPath(path,idx%32,idx/32);
		if(from.empty()||from==to){/*存回原处：外置文件本来就在那（没改过）*/
			continue;
		}
		std::vector<byte> buf;
		if(read_File(from.c_str(),buf)&&!save_FileAtomic(to,buf.data(),buf.size())){/*另存别处：外置文件跟着抄过去*/
			return false;
		}
	}
	static const byte zero[8192]={0};
	if(!save_FileAtomic(target,data.empty()?zero:data.data(),data.empty()?sizeof(zero):data.size())){
		return false;
	}
	for(uint32_t idx:touched){/*改成内嵌/删掉的格子：旧的外置文件不要了（游戏写/清一格时也删）*/
		if(is_ExternalStub(data,idx)){
			continue;
		}
		std::string to=get_ExternalPath(target,idx%32,idx/32);
		if(!to.empty()){
			remove(to.c_str());
		}
	}
	return true;
}
