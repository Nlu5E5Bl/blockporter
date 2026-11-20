import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import java.util.Map;
import java.util.TreeMap;

/*SurviveProbe.java：原型（规则生成方案的第一步，见 ReadMeDeveloper 1.9）——每种方块“靠哪些邻居活”。
 *	每个方块状态放进六个邻居全是空气的假世界（McWorld），问游戏留不留，并记下判的时候读了哪几个邻居、
 *	有没有读六邻以外的格子、用没用光照/方块实体。由 test/mcdump.sh probe 运行（和 test/mc/McWorld.java 一起编）。
 *	输出一行一种方块（tab 分隔）：
 *	  名字 类名 状态数 结果计数（SAME/DEAD/CHANGED/OTHER/EXC） 单个状态最多读几个邻居 读过的邻居 其它（六邻以外/光照/…）
 *	注意：空气邻居下走不到的分支（比如甘蔗下面是沙子才去看旁边的水）这里看不到，要靠 FitProbe 穷举时补记。*/
public class SurviveProbe {
	static final String[] dir_Name={"下","上","北","南","西","东"};
	public static void main(String[] args){
		McWorld.init_Game();
		McWorld w=new McWorld();
		StringBuilder out=new StringBuilder();
		out.append("#名字\t类名\t状态数\t空气里的结果\t单状态最多读几个邻居\t读过的邻居\t其它\n");
		for(Block b:BuiltInRegistries.BLOCK){
			Map<String,Integer> cnt=new TreeMap<>();
			int maxNb=0,allMask=0;
			boolean outside=false,light=false;
			java.util.Set<String> special=new java.util.TreeSet<>();
			int nc=McWorld.get_NcKind(b);
			for(BlockState s:b.getStateDefinition().getPossibleStates()){
				w.clear_World();
				w.clear_Marks();
				String r;
				try{
					int g=w.judge_Game(s,nc);
					r=g==McWorld.G_SAME?"SAME":g==McWorld.G_DEAD?"DEAD":g==McWorld.G_CHANGED?"CHANGED":"OTHER";
				}catch(RuntimeException e){
					r="EXC";
				}
				cnt.merge(r,1,Integer::sum);
				maxNb=Math.max(maxNb,Integer.bitCount(w.read_mask));
				allMask|=w.read_mask;
				outside|=w.outside;
				light|=w.light_used;
				special.addAll(w.special);
			}
			StringBuilder nb=new StringBuilder();
			for(Direction d:McWorld.DIRS){
				if((allMask&(1<<d.ordinal()))!=0){
					nb.append(dir_Name[d.ordinal()]);
				}
			}
			if(outside){
				special.add("六邻以外");
			}
			if(light){
				special.add("光照");
			}
			out.append(McWorld.get_BlockName(b).replace("minecraft:","")).append('\t').append(b.getClass().getSimpleName()).append('\t')
				.append(b.getStateDefinition().getPossibleStates().size()).append('\t').append(cnt).append('\t').append(maxNb).append('\t')
				.append(nb.length()==0?"-":nb).append('\t').append(special.isEmpty()?"-":String.join(" ",special)).append('\n');
		}
		System.out.print(out);
	}
}
