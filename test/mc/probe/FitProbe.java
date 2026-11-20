import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.tags.FluidTags;
import net.minecraft.tags.TagKey;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.Fluids;
import net.minecraft.world.phys.shapes.VoxelShape;
import java.util.*;

/*FitProbe.java：原型（规则生成方案的第二步，见 ReadMeDeveloper 1.9）——“只靠一个邻居活”的方块，支撑条件能不能自动写出来。
 *	对每个在空气里活不了、而且判的时候只读了一个邻居的方块状态：把那个邻居换成全部方块状态，问游戏活不活，
 *	得到支撑集合 K；再找能**精确**表示 K 的条件：单个谓词/标签，或者两项的并/差。由 test/mcdump.sh fit 运行（和 test/mc/McWorld.java 一起编）。
 *	谓词（每个面都有，所以“藤蔓看上方顶面”这种怪规则也找得出来）：
 *	  FULL_面 CENTER_上下 RIGID_UP ATTACH_面（支撑或碰撞面满） COLL_UP SOLID RENDER TOPCOLL
 *	  WATER（流体是水） WATER_SRC（水源） LAVA NOT_AIR SAME（同一种方块），外加全部方块标签 #xxx
 *	输出一行一种方块：名字 {方向: 拟合结果=状态数, ...}；拟合不上给最接近的一项和多/少了哪些方块。
 *	两格高（门/高花）的另一半、要看几个邻居的（藤蔓的多个面、仙人掌）这里只能看到一部分，要手写模板（见 RulesCmp）。*/
public class FitProbe {
	public static void main(String[] args){
		McWorld.init_Game();
		McWorld w=new McWorld();
		List<BlockState> all=McWorld.all;
		int n=all.size();
		var g=EmptyBlockGetter.INSTANCE;
		var p0=net.minecraft.core.BlockPos.ZERO;
		Map<String,BitSet> pred=new LinkedHashMap<>();
		for(Direction f:McWorld.DIRS){
			BitSet full=new BitSet(),center=new BitSet(),attach=new BitSet();
			for(int i=0;i<n;i++){
				BlockState s=all.get(i);
				boolean F=s.isFaceSturdy(g,p0,f,SupportType.FULL);
				if(F){
					full.set(i);
				}
				if(s.isFaceSturdy(g,p0,f,SupportType.CENTER)){
					center.set(i);
				}
				if(F||Block.isFaceFull(s.getCollisionShape(g,p0),f)){
					attach.set(i);
				}
			}
			pred.put("FULL_"+f,full);
			if(f.getAxis()==Direction.Axis.Y){
				pred.put("CENTER_"+f,center);
			}
			pred.put("ATTACH_"+f,attach);
		}
		BitSet rigid=new BitSet(),collUp=new BitSet(),solid=new BitSet(),render=new BitSet(),topcoll=new BitSet();
		BitSet water=new BitSet(),waterSrc=new BitSet(),lava=new BitSet(),notAir=new BitSet();
		for(int i=0;i<n;i++){
			BlockState s=all.get(i);
			VoxelShape c=s.getCollisionShape(g,p0);
			if(s.isFaceSturdy(g,p0,Direction.UP,SupportType.RIGID)){
				rigid.set(i);
			}
			if(Block.isFaceFull(c,Direction.UP)){
				collUp.set(i);
			}
			if(s.isSolid()){
				solid.set(i);
			}
			if(s.isSolidRender()){
				render.set(i);
			}
			if(!c.isEmpty()&&!c.getFaceShape(Direction.UP).isEmpty()){
				topcoll.set(i);
			}
			if(s.getFluidState().is(FluidTags.WATER)){
				water.set(i);
			}
			if(s.getFluidState().is(Fluids.WATER)){
				waterSrc.set(i);
			}
			if(s.getFluidState().is(FluidTags.LAVA)){
				lava.set(i);
			}
			if(!s.isAir()){
				notAir.set(i);
			}
		}
		pred.put("RIGID_UP",rigid);
		pred.put("COLL_UP",collUp);
		pred.put("SOLID",solid);
		pred.put("RENDER",render);
		pred.put("TOPCOLL",topcoll);
		pred.put("WATER",water);
		pred.put("WATER_SRC",waterSrc);
		pred.put("LAVA",lava);
		pred.put("NOT_AIR",notAir);
		Map<String,BitSet> tags=new LinkedHashMap<>();
		BuiltInRegistries.BLOCK.getTags().forEach(named->{
			BitSet bs=new BitSet();
			TagKey<Block> key=named.key();
			for(int i=0;i<n;i++){
				if(all.get(i).is(key)){
					bs.set(i);
				}
			}
			if(!bs.isEmpty()){
				tags.put("#"+key.location().getPath(),bs);
			}
		});
		Map<Block,BitSet> same=new HashMap<>();
		for(int i=0;i<n;i++){
			same.computeIfAbsent(all.get(i).getBlock(),k->new BitSet()).set(i);
		}
		Map<BitSet,String> cache=new HashMap<>();
		StringBuilder out=new StringBuilder();
		for(Block b:BuiltInRegistries.BLOCK){
			int nc=McWorld.get_NcKind(b);
			Map<String,Integer> res=new TreeMap<>();
			for(BlockState s:b.getStateDefinition().getPossibleStates()){
				if(s.isAir()){
					continue;
				}
				w.clear_World();
				w.clear_Marks();
				try{
					if(w.judge_Game(s,nc)!=McWorld.G_DEAD){
						continue;
					}
				}catch(RuntimeException e){
					res.merge("报错",1,Integer::sum);
					continue;
				}
				if(Integer.bitCount(w.read_mask)!=1||w.outside){
					res.merge("不是单邻居（读了"+Integer.bitCount(w.read_mask)+"个"+(w.outside?"、还读了六邻以外":"")+"）",1,Integer::sum);
					continue;
				}
				Direction dep=McWorld.DIRS[Integer.numberOfTrailingZeros(w.read_mask)];
				BitSet k=new BitSet();
				w.clear_Marks();
				try{
					for(int i=0;i<n;i++){
						w.nb[dep.ordinal()]=all.get(i);
						if(w.judge_Game(s,nc)!=McWorld.G_DEAD){
							k.set(i);
						}
					}
				}catch(RuntimeException e){
					res.merge("穷举报错",1,Integer::sum);
					continue;
				}
				String flags=(Integer.bitCount(w.read_mask)>1||w.outside?" 还读了别的格":"")+(w.light_used?" 用了光照":"");
				Map<String,BitSet> atoms=new LinkedHashMap<>(pred);
				atoms.put("SAME",same.get(b));
				String fit=cache.computeIfAbsent(k,kk->fit_Set(kk,atoms,tags,n,all));
				res.merge(dep+": "+fit+flags,1,Integer::sum);
			}
			if(!res.isEmpty()){
				out.append(McWorld.get_BlockName(b).replace("minecraft:","")).append('\t').append(res).append('\n');
			}
		}
		System.out.print(out);
	}
	static String fit_Set(BitSet k,Map<String,BitSet> atoms,Map<String,BitSet> tags,int n,List<BlockState> all){
		if(k.isEmpty()){
			return "永远活不了（另一半/别的邻居没给）";
		}
		if(k.cardinality()==n){
			return "永远活";
		}
		for(var e:atoms.entrySet()){
			if(e.getValue().equals(k)){
				return e.getKey();
			}
		}
		for(var e:tags.entrySet()){
			if(e.getValue().equals(k)){
				return e.getKey();
			}
		}
		Map<String,BitSet> seconds=new LinkedHashMap<>(atoms);
		seconds.putAll(tags);
		BitSet t=new BitSet();
		for(var a:atoms.entrySet()){
			for(var e:seconds.entrySet()){
				if(a.getKey().equals(e.getKey())){
					continue;
				}
				t.clear();
				t.or(a.getValue());
				t.or(e.getValue());
				if(t.equals(k)){
					return a.getKey()+"|"+e.getKey();
				}
				t.clear();
				t.or(a.getValue());
				t.andNot(e.getValue());
				if(t.equals(k)){
					return a.getKey()+"-"+e.getKey();
				}
				t.clear();
				t.or(e.getValue());
				t.andNot(a.getValue());
				if(t.equals(k)){
					return e.getKey()+"-"+a.getKey();
				}
			}
		}
		String best=null;
		int bd=Integer.MAX_VALUE;
		BitSet bb=null;
		for(var e:seconds.entrySet()){
			t.clear();
			t.or(e.getValue());
			t.xor(k);
			int c=t.cardinality();
			if(c<bd){
				bd=c;
				best=e.getKey();
				bb=e.getValue();
			}
		}
		BitSet miss=(BitSet)k.clone();
		miss.andNot(bb);
		BitSet over=(BitSet)bb.clone();
		over.andNot(k);
		return "≈"+best+" 少"+miss.cardinality()+get_Names(miss,all)+" 多"+over.cardinality()+get_Names(over,all);
	}
	static String get_Names(BitSet b,List<BlockState> all){
		Set<String> s=new TreeSet<>();
		for(int i=b.nextSetBit(0);i>=0&&s.size()<6;i=b.nextSetBit(i+1)){
			s.add(McWorld.get_BlockName(all.get(i).getBlock()).replace("minecraft:",""));
		}
		return s.toString();
	}
}
