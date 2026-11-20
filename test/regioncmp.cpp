#include "../nbtrw.hpp"
#include <string>
#include <vector>
/*regioncmp.cpp：和游戏的 RegionFile 对拍区域文件（test/mcdump.sh region 调它，Java 那边是 test/mc/RegionProbe.java）。
 *	regioncmp prep  <目录>  造两个区块：small.nbt（普通大小）、big.nbt（1.5MB 随机字节，压不小，游戏/库都要放外置 .mcc）；
 *	                        再用 libnbtrw 按 gzip/deflate/none/lz4 各写一个 lib_<压缩>/r.0.0.mca（small 在(1,2)，big 在(3,4)）
 *	regioncmp check <目录>  ① 游戏写的 java_<压缩>/r.0.0.mca，libnbtrw 读出来要和原来逐值相同，big 要是外置；
 *	                        ② 游戏读 lib_<压缩> 存下来的 out_<压缩>_small.nbt / out_<压缩>_big.nbt 要和原来逐值相同；
 *	                        ③ span.nbt（libnbtrw 按 2527 以前紧排写的下标）经游戏的 BitStorageAlignFix 转成 aligned.nbt，
 *	                          libnbtrw 按老格式读前者、按新格式读后者，每一格都要一样。*/
static const char *comps[4]={"gzip","deflate","none","lz4"};
static const uint8_t types[4]={MCA_GZIP,MCA_ZLIB,MCA_NONE,MCA_LZ4};
static int bad=0;
static void check_Case(bool c,const std::string &msg){/*一项核对：打 ok/FAIL，记失败数*/
	printf("%s %s\n",c?"ok  ":"FAIL",msg.c_str());
	if(!c){
		bad++;
	}
}
static nbt_Compound make_Small(){/*一个很小的区块（压完不到一个扇区）*/
	nbt_Compound c;
	c.set_Int("DataVersion",4788);
	c.set_Int("xPos",1);
	c.set_Int("zPos",2);
	c.set_String("Status","minecraft:full");
	std::vector<int64_t> w(256);
	for(size_t i=0;i<w.size();i++){
		w[i]=(int64_t)(i*0x9E3779B97F4A7C15ull);
	}
	c.set("data",new nbt_Long_Array(std::move(w)));
	return c;
}
static nbt_Compound make_Big(){/*一个压不小的大区块（1.5MB 随机字节：压完超过 255 个扇区，要放外置 .mcc）*/
	nbt_Compound c;
	c.set_Int("xPos",3);
	c.set_Int("zPos",4);
	std::vector<int8_t> r(1536*1024);
	uint64_t s=12345;
	for(size_t i=0;i<r.size();i++){
		s=s*6364136223846793005ull+1442695040888963407ull;
		r[i]=(int8_t)(s>>56);
	}
	c.set("noise",new nbt_Byte_Array(std::move(r)));
	return c;
}
static bool same_File(const std::string &path,const nbt_Compound &want){/*path 这个裸 NBT 文件读出来是不是和 want 逐值相等*/
	nbt_Compound *c=nbt_Compound::read(path.c_str());
	bool ok=c&&*c==want;
	if(c){
		c->free();
	}
	return ok;
}
int main(int argc,char **argv){/*prep / check 两个子命令，做什么见文件头*/
	if(argc<3){
		printf("用法：regioncmp prep|check <目录>\n");
		return 2;
	}
	std::string dir=argv[2],cmd=argv[1];
	nbt_Compound small=make_Small(),big=make_Big();
	static const uint32_t span_bits[]={4,5,6,7,9,11,13};
	if(cmd=="prep"){
		check_Case(small.save((dir+"/small.nbt").c_str())&&big.save((dir+"/big.nbt").c_str()),"写 small.nbt / big.nbt");
		nbt_Compound sp;
		for(uint32_t bits:span_bits){
			std::vector<int64_t> w(get_PackedWords(4096,bits,true),0);
			for(uint32_t i=0;i<4096;i++){
				set_PackedIndex(w,i,bits,true,(i*2654435761u)>>(32-bits));
			}
			sp.set(std::to_string(bits),new nbt_Long_Array(std::move(w)));
		}
		check_Case(sp.save((dir+"/span.nbt").c_str()),"写 span.nbt（紧排跨 long 的下标）");
		for(int i=0;i<4;i++){
			std::string d=dir+"/lib_"+comps[i];
			if(system(("mkdir -p "+d).c_str())){
				return 1;
			}
			MCA m;
			m.compression=types[i];
			check_Case(m.set_Compound(small,1,2)&&m.set_Compound(big,3,4),std::string("libnbtrw 按 ")+comps[i]+" 写两格");
			check_Case(m.ext.count(3+4*32)==1&&m.ext.count(1+2*32)==0,std::string(comps[i])+"：big 放外置、small 不放");
			check_Case(m.save((d+"/r.0.0.mca").c_str()),std::string(comps[i])+"：存盘（连外置 .mcc）");
		}
		return bad?1:0;
	}
	if(cmd=="check"){
		for(int i=0;i<4;i++){
			std::string d=dir+"/java_"+comps[i];
			MCA m(d+"/r.0.0.mca");
			std::optional<nbt_Compound> a=m.get_Compound(1,2),b=m.get_Compound(3,4);
			check_Case(a&&*a==small,std::string("游戏按 ")+comps[i]+" 写的 small，libnbtrw 读回来一样");
			check_Case(b&&*b==big,std::string("游戏按 ")+comps[i]+" 写的 big，libnbtrw 读回来一样（last_error="+std::to_string(m.last_error)+"）");
			FILE *fp=fopen((d+"/c.3.4.mcc").c_str(),"rb");
			check_Case(fp!=NULL,std::string(comps[i])+"：游戏把 big 放进了 c.3.4.mcc");
			if(fp){
				fclose(fp);
			}
			check_Case(same_File(dir+"/out_"+comps[i]+"_small.nbt",small),std::string("libnbtrw 按 ")+comps[i]+" 写的 small，游戏读回来一样");
			check_Case(same_File(dir+"/out_"+comps[i]+"_big.nbt",big),std::string("libnbtrw 按 ")+comps[i]+" 写的 big（外置），游戏读回来一样");
		}
		nbt_Compound *sp=nbt_Compound::read((dir+"/span.nbt").c_str()),*al=nbt_Compound::read((dir+"/aligned.nbt").c_str());
		for(uint32_t bits:span_bits){
			std::string k=std::to_string(bits);
			const nbt_Any *a=sp?sp->get(k):NULL,*b=al?al->get(k):NULL;
			bool same=a&&b&&a->type==NBT_LONG_ARRAY&&b->type==NBT_LONG_ARRAY&&
				((const nbt_Long_Array*)b)->con.size()==get_PackedWords(4096,bits,false);
			for(uint32_t i=0;same&&i<4096;i++){
				same=get_PackedIndex(((const nbt_Long_Array*)a)->con,i,bits,true)==get_PackedIndex(((const nbt_Long_Array*)b)->con,i,bits,false)&&
					get_PackedIndex(((const nbt_Long_Array*)a)->con,i,bits,true)==(i*2654435761u)>>(32-bits);
			}
			check_Case(same,k+" 位：libnbtrw 读紧排的下标，和游戏 BitStorageAlignFix 转出来的不跨 long 下标每格一样");
		}
		if(sp){
			sp->free();
		}
		if(al){
			al->free();
		}
		printf("%s：%d 处不对\n",bad?"FAIL":"OK",bad);
		return bad?1:0;
	}
	printf("不认识的命令：%s\n",cmd.c_str());
	return 2;
}
