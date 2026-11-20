#include "../nbtrw.hpp"
/*checkmerge.cpp：核对 mkmerge（多 old、方块/方块实体、高段、计划刻、POI/实体）。*/
static const char *D="minecraft:overworld";
int main(){/*mkmerge 造的世界让 merge 跑过以后逐项核对；有不对的返回 1*/
	Level lv("test/work/merge/new",4,2,2);
	const char *exp[7]={"","minecraft:diamond_block","minecraft:gold_block","minecraft:emerald_block","minecraft:air","minecraft:stone","minecraft:chest"};
	int bad=0;
	for(int i=1;i<=6;i++){
		Block b=lv.get_Block(D,i,-64,i);
		printf("%d,%d,%d = %s（期望 %s）\n",i,-64,i,std::string(b.name()).c_str(),exp[i]);
		if(b.name()!=exp[i]){
			bad++;
		}
	}
	Chunk *c=lv.get_Chunk(D,0,0);
	const nbt_Any *it=c?c->con.get("InhabitedTime"):NULL;
	printf("InhabitedTime=%lld（期望3：搬了1/4/6三格，8,200,8 缺段跳过不算）\n",(long long)(it?it->as_Long().value_or(-1):-1));
	if(!it||it->as_Long()!=3){
		bad++;
	}
	Block high=lv.get_Block(D,8,200,8);
	printf("8,200,8 = %s（期望 minecraft:air：new 缺这一段，merge 不补段、跳过）\n",std::string(high.name()).c_str());
	if(high.name()!="minecraft:air"){
		bad++;
	}
	nbt_Any *s12=c?c->get_Section(12):NULL;
	if(s12){
		printf("Y=12 的 section 被补出来了（merge 不该补段）\n");
		bad++;
	}
	{
		FILE *fp=fopen("test/work/merge.log","r");
		char line[4096];
		bool got=false;
		while(fp&&fgets(line,sizeof(line),fp)){
			if(strstr(line,"缺段跳过 ")&&!strstr(line,"缺段跳过 0；")){
				got=true;
			}
		}
		if(fp){
			fclose(fp);
		}
		printf("merge 日志报了缺段跳过（不是0）：%s\n",got?"是":"否");
		if(!got){
			bad++;
		}
	}
	Block chest=lv.get_Block(D,6,-64,6);
	const nbt_Any *cid=chest.block_entities.get("id");
	printf("箱子方块实体 id=%s\n",cid?std::string(cid->as_String().value_or("?")).c_str():"(无)");
	if(!cid||cid->as_String()!="minecraft:chest"){
		bad++;
	}
	Block b17=lv.get_Block(D,17,-64,1);
	printf("17,-64,1 = %s（期望 minecraft:diamond_block，确认区块(1,0)确实被merge处理）\n",std::string(b17.name()).c_str());
	if(b17.name()!="minecraft:diamond_block"){
		bad++;
	}
	int n=count_ChunkTicks(&c->con,false);
	printf("block_ticks=%d\n",n);
	bool got=false;
	for(int i=0;i<n;i++){
		Chunk_Tick t;
		if(get_ChunkTick(&c->con,false,i,&t)&&t.x==1&&t.y==-64&&t.z==1&&t.type=="minecraft:redstone_wire"&&t.delay==10){
			got=true;
		}
	}
	printf("找到now的红石计划刻：%s\n",got?"是":"否");
	if(!got){
		bad++;
	}
	/*merge 不清理重复Y：new 里预先放的那段重复段应该原样保留*/
	{
		const nbt_Any *sa=c?c->con.get("sections"):NULL;
		int sy=999,cnt=0;
		if(sa&&sa->type==NBT_LIST&&!((const nbt_List*)sa)->con.empty()){
			const nbt_Any *s=((const nbt_List*)sa)->con[0];
			const nbt_Any *y=s&&s->type==NBT_COMPOUND?((const nbt_Compound*)s)->get("Y"):NULL;
			if(y&&y->type==NBT_BYTE){
				sy=*(const nbt_Byte*)y;
			}
			for(auto a:((const nbt_List*)sa)->con){
				const nbt_Any *yy=a&&a->type==NBT_COMPOUND?((const nbt_Compound*)a)->get("Y"):NULL;
				if(yy&&yy->type==NBT_BYTE&&*(const nbt_Byte*)yy==sy){
					cnt++;
				}
			}
		}
		printf("new的Y=%d段数=%d（merge不清理，期望2）\n",sy,cnt);
		if(cnt!=2){
			bad++;
		}
	}
	lv.close();
	{
		MCA m("test/work/merge/new/dimensions/minecraft/overworld/entities/r.0.0.mca");
		std::optional<nbt_Compound> e=m.get_Compound(0,0);
		const nbt_Any *es=e?e->get("Entities"):NULL;
		int n=es&&es->type==NBT_LIST?(int)((nbt_List*)es)->con.size():-1;
		printf("new实体数=%d（期望1，new原本没有文件；Pos 用 Int 写、y 在范围外的那只不搬）\n",n);
		if(n!=1){
			bad++;
		}
		FILE *fp=fopen("test/work/merge/new/dimensions/minecraft/overworld/entities/r.0.0.mca","rb");
		if(!fp){
			bad++;
		}else{
			fclose(fp);
		}
	}
	{
		MCA m("test/work/merge/new/dimensions/minecraft/overworld/poi/r.0.0.mca");
		std::optional<nbt_Compound> p=m.get_Compound(0,0);
		int n=p?count_POI(&*p):-1;
		POI h=p?get_POI(&*p,1,-64,1):POI(),g=p?get_POI(&*p,2,-64,2):POI();
		printf("new POI数=%d（期望2，new原本没有文件）；(1,-64,1)=%s %d 票（期望 home 1：同类型留先来的）、(2,-64,2)=%s %d 票（期望 meeting 5：不同类型后来的顶掉）\n",
			n,h.type.c_str(),h.free_tickets,g.type.c_str(),g.free_tickets);
		if(n!=2||h.type!="minecraft:home"||h.free_tickets!=1||g.type!="minecraft:meeting"||g.free_tickets!=5){
			bad++;
		}
		FILE *fp=fopen("test/work/merge/new/dimensions/minecraft/overworld/poi/r.0.0.mca","rb");
		if(!fp){
			bad++;
		}else{
			fclose(fp);
		}
	}
	/*new里(1,0)的实体/POI原本是LZ4/type4坏压缩：merge必须跳过，不能当“没有文件”覆盖*/
	{
		MCA m("test/work/merge/new/dimensions/minecraft/overworld/entities/r.0.0.mca");
		std::optional<nbt_Compound> c=m.get_Compound(1,0);
		printf("new实体(1,0)=%s，last_error=%d（期望保留UNSUPPORTED=3）\n",c?"被覆盖":"保留",m.last_error);
		if(c||m.last_error!=MCA_READ_UNSUPPORTED_COMPRESSION){
			bad++;
		}
	}
	{
		MCA m("test/work/merge/new/dimensions/minecraft/overworld/poi/r.0.0.mca");
		std::optional<nbt_Compound> c=m.get_Compound(1,0);
		printf("new POI(1,0)=%s，last_error=%d（期望保留UNSUPPORTED=3）\n",c?"被覆盖":"保留",m.last_error);
		if(c||m.last_error!=MCA_READ_UNSUPPORTED_COMPRESSION){
			bad++;
		}
	}
	/*区块(32,0)：new 原来整个 poi/entities 文件都没有，merge 要新建（文件不在≠读不出来）*/
	{
		Level l2("test/work/merge/new",4,2,2);
		Block b=l2.get_Block(D,513,-64,1);
		printf("513,-64,1 = %s（期望 minecraft:lapis_block）\n",std::string(b.name()).c_str());
		if(b.name()!="minecraft:lapis_block"){
			bad++;
		}
		l2.close();
		MCA p("test/work/merge/new/dimensions/minecraft/overworld/poi/r.1.0.mca");
		std::optional<nbt_Compound> pc=p.get_Compound(0,0);
		bool poi_ok=pc&&count_POI(&*pc)==1&&get_POI(&*pc,513,-64,1);
		printf("new poi/r.1.0.mca 新建：%s\n",poi_ok?"是":"否");
		if(!poi_ok){
			bad++;
		}
		MCA e("test/work/merge/new/dimensions/minecraft/overworld/entities/r.1.0.mca");
		std::optional<nbt_Compound> ec=e.get_Compound(0,0);
		bool ent_ok=ec&&check_Entity(&*ec,32,0)&&((nbt_List*)ec->get("Entities"))->size()==1;
		printf("new entities/r.1.0.mca 新建：%s\n",ent_ok?"是":"否");
		if(!ent_ok){
			bad++;
		}
		/*区块(64,0)两边都没有 poi/entities：不新建文件*/
		FILE *fp=fopen("test/work/merge/new/dimensions/minecraft/overworld/poi/r.2.0.mca","rb");
		if(fp){
			fclose(fp);
			printf("poi/r.2.0.mca 不该被新建\n");
			bad++;
		}
		fp=fopen("test/work/merge/new/dimensions/minecraft/overworld/entities/r.2.0.mca","rb");
		if(fp){
			fclose(fp);
			printf("entities/r.2.0.mca 不该被新建\n");
			bad++;
		}
	}
	return bad?1:0;
}
