#include "../nbtrw.hpp"
#include <stdlib.h>
/*checkzero：检查指定MCA里某个区块还在不在。
  用法：checkzero <mca> <chunkX> <chunkZ> <expect:0|1>
  文件不存在时按“没有这个区块”算，正好用来验证0字节region先删、POI/实体跟着清。*/
int main(int argc,char **argv){
	if(argc<5){
		fprintf(stderr,"用法：%s <mca> <chunkX> <chunkZ> <0|1>\n",argv[0]);
		return 2;
	}
	MCA m(argv[1]);
	bool has=m.has_Chunk((uint32_t)atoi(argv[2]),(uint32_t)atoi(argv[3]));
	int expect=atoi(argv[4]);
	printf("%s chunk(%s,%s)=%d（期望%d）\n",argv[1],argv[2],argv[3],has?1:0,expect);
	return (has?1:0)==expect?0:1;
}
