import net.minecraft.core.Direction;
import net.minecraft.world.level.block.BambooSaplingBlock;
import net.minecraft.world.level.block.BambooStalkBlock;
import net.minecraft.world.level.block.BigDripleafBlock;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.ChestBlock;
import net.minecraft.world.level.block.ChorusPlantBlock;
import net.minecraft.world.level.block.DoorBlock;
import net.minecraft.world.level.block.FenceBlock;
import net.minecraft.world.level.block.FenceGateBlock;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.level.block.GrowingPlantBlock;
import net.minecraft.world.level.block.GrowingPlantHeadBlock;
import net.minecraft.world.level.block.HugeMushroomBlock;
import net.minecraft.world.level.block.IronBarsBlock;
import net.minecraft.world.level.block.LeavesBlock;
import net.minecraft.world.level.block.MossyCarpetBlock;
import net.minecraft.world.level.block.NoteBlock;
import net.minecraft.world.level.block.PointedDripstoneBlock;
import net.minecraft.world.level.block.ScaffoldingBlock;
import net.minecraft.world.level.block.SnowyBlock;
import net.minecraft.world.level.block.SoulFireBlock;
import net.minecraft.world.level.block.WallBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BooleanProperty;
import net.minecraft.world.level.block.state.properties.Property;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

/*RulesCmp.java：游戏判定 vs rules.so，逐个方块状态、逐个邻居穷举对拍。由 test/mcdump.sh cmp 编译运行。
 *	java ... RulesCmp <librules_shim.so> <rules.so> [方块id,方块id...]   不给方块就测全部
 *	对象：游戏里空气中会被移除的方块（updateShape/计划刻/neighborChanged 三条路），加上 rules.so 规则表管的方块。
 *	做法：
 *	  1. 找“见证”：从一小组常见方块（加上自己这种方块的全部状态、头/茎这类搭档）里，逐个方向换邻居，
 *	     找一个让游戏判“留着、状态不变”的六邻配置（找不到就用最接近的，记“无见证”）；
 *	  2. 在见证上把六个邻居轮流换成全部方块状态，每个配置同时问游戏（McWorld.judge_Game）和 rules.so（judge_Blk）。
 *	结论分类（每类按 方块+方向 汇总，给几个例子）：
 *	  误拆   游戏留、规则拆（必须修）           误改   游戏状态不变、规则改了状态
 *	  漏拆   游戏拆、规则留（宁可漏拆，能修就修）少关面 游戏关了藤蔓/地衣的面、规则没关
 *	  改法不同 两边都改了状态但改得不一样        漏改   游戏换成别的方块（混凝土粉末遇水）、规则没换；
 *	         规则整个状态都管的方块（门、火、藤头藤身、竹子、大型垂滴叶、栅栏、墙、玻璃板/铁栏杆、栅栏门、箱子、雪地草类、
 *	         紫颂植株、苍白苔藓地毯、蘑菇方块、音符盒、树叶、脚手架、滴水石锥，见 is_Exact）改了属性规则没跟也算
 *	  别的方块只改外观属性（楼梯形状、红石线连接…）规则不管，不比
 *	  看不到 游戏读了六邻以外的格子（甘蔗旁边的水、紫颂旁边那株的根…），规则只有六邻，不算差异，只计数
 *	光照：见 McWorld（亮暗有一种能活就算活）。*/
public class RulesCmp {
	static MethodHandle mh_Open,mh_AddState,mh_GetRule,mh_GetRuleKind,mh_GetRuleName,mh_Judge;
	static final int[] nb_OfDir=new int[6];/*Direction.ordinal → rules.hpp 的 NB_*（东0 西1 上2 下3 南4 北5）*/
	static final String[] dir_Name={"下","上","北","南","西","东"};
	static final Set<String> face_Rules=Set.of("藤蔓","六面附着");/*按面只关不开的规则（看 get_RuleName）*/
	static boolean[] faceKind;
	static int[] def;
	static int[] kind;
	static String[] names;

	static final class Agg{
		long count;
		final Set<String> sStates=new TreeSet<>();
		final Set<String> nBlocks=new TreeSet<>();
		final List<String> examples=new ArrayList<>();
	}
	static final class Job_Result{
		final Map<String,Agg> agg=new HashMap<>();
		final Map<String,Long> unseen=new HashMap<>();
		final Set<String> noWitness=new TreeSet<>();
		final Set<String> errors=new TreeSet<>();
		long configs;
		void add_Diff(String key,String s,String n,String example){
			Agg a=agg.computeIfAbsent(key,k->new Agg());
			a.count++;
			if(a.sStates.size()<4){
				a.sStates.add(s);
			}
			if(a.nBlocks.size()<12){
				a.nBlocks.add(n);
			}
			if(a.examples.size()<3){
				a.examples.add(example);
			}
		}
	}

	public static void main(String[] args) throws Throwable{
		McWorld.init_Game();
		Linker linker=Linker.nativeLinker();
		SymbolLookup lib=SymbolLookup.libraryLookup(Path.of(args[0]),Arena.global());
		mh_Open=linker.downcallHandle(lib.find("shim_Open").orElseThrow(),FunctionDescriptor.of(ValueLayout.JAVA_BOOLEAN,ValueLayout.ADDRESS));
		mh_AddState=linker.downcallHandle(lib.find("shim_AddState").orElseThrow(),FunctionDescriptor.of(ValueLayout.JAVA_INT,ValueLayout.ADDRESS));
		mh_GetRule=linker.downcallHandle(lib.find("shim_GetRule").orElseThrow(),FunctionDescriptor.of(ValueLayout.JAVA_INT,ValueLayout.JAVA_INT));
		mh_GetRuleKind=linker.downcallHandle(lib.find("shim_GetRuleKind").orElseThrow(),FunctionDescriptor.of(ValueLayout.JAVA_INT,ValueLayout.JAVA_INT));
		mh_GetRuleName=linker.downcallHandle(lib.find("shim_GetRuleName").orElseThrow(),FunctionDescriptor.of(ValueLayout.ADDRESS,ValueLayout.JAVA_INT));
		mh_Judge=linker.downcallHandle(lib.find("shim_Judge").orElseThrow(),
			FunctionDescriptor.of(ValueLayout.JAVA_INT,ValueLayout.JAVA_INT,ValueLayout.JAVA_INT,ValueLayout.ADDRESS,ValueLayout.ADDRESS,ValueLayout.JAVA_INT),
			Linker.Option.critical(false));
		try(Arena a=Arena.ofConfined()){
			if(!(boolean)mh_Open.invokeExact(a.allocateFrom(args[1]))){
				System.err.println("RulesCmp: 装不上 "+args[1]);
				System.exit(1);
			}
		}
		Set<String> only=new HashSet<>();
		if(args.length>2){
			for(String x:args[2].split(",")){
				only.add(x.contains(":")?x:"minecraft:"+x);
			}
		}
		nb_OfDir[Direction.DOWN.ordinal()]=3;
		nb_OfDir[Direction.UP.ordinal()]=2;
		nb_OfDir[Direction.NORTH.ordinal()]=5;
		nb_OfDir[Direction.SOUTH.ordinal()]=4;
		nb_OfDir[Direction.WEST.ordinal()]=1;
		nb_OfDir[Direction.EAST.ordinal()]=0;
		int n=McWorld.all.size();
		def=new int[n];
		kind=new int[n];
		names=new String[n];
		try(Arena a=Arena.ofConfined()){
			for(int i=0;i<n;i++){
				names[i]=McWorld.get_StateName(McWorld.all.get(i));
				int k=(int)mh_AddState.invokeExact(a.allocateFrom(names[i]));
				if(k!=i){
					throw new IllegalStateException("shim 下标对不上");
				}
				def[i]=(int)mh_GetRule.invokeExact(i);
				kind[i]=def[i]>=0?(int)mh_GetRuleKind.invokeExact(def[i]):0;
			}
		}
		faceKind=new boolean[64];
		for(int r=0;r<64;r++){
			MemorySegment nm=(MemorySegment)mh_GetRuleName.invokeExact(r);
			faceKind[r]=face_Rules.contains(nm.reinterpret(256).getString(0));
		}
		/*要测的方块：空气里会被游戏移除的，或者规则表管的*/
		McWorld probe=new McWorld();
		List<Integer> targets=new ArrayList<>();
		Map<Block,Boolean> blockNeeds=new LinkedHashMap<>();
		for(int i=0;i<n;i++){
			BlockState s=McWorld.all.get(i);
			Block b=s.getBlock();
			if(s.isAir()||!only.isEmpty()&&!only.contains(McWorld.get_BlockName(b))){
				continue;
			}
			boolean need=def[i]>=0;
			if(!need){
				probe.clear_World();
				try{
					need=probe.judge_Game(s,McWorld.get_NcKind(b))==McWorld.G_DEAD;
				}catch(RuntimeException e){
					need=false;
				}
			}
			if(need){
				blockNeeds.put(b,true);
			}
		}
		for(int i=0;i<n;i++){
			if(blockNeeds.containsKey(McWorld.all.get(i).getBlock())){
				targets.add(i);
			}
		}
		int[] basePalette=make_Palette();
		System.err.println("RulesCmp: "+blockNeeds.size()+" 种方块、"+targets.size()+" 个状态，每个状态 6×"+n+" 个配置");
		int threads=Runtime.getRuntime().availableProcessors();
		ExecutorService pool=Executors.newFixedThreadPool(threads);
		ThreadLocal<McWorld> worlds=ThreadLocal.withInitial(McWorld::new);
		ThreadLocal<Arena> arenas=ThreadLocal.withInitial(Arena::ofConfined);
		List<Future<Job_Result>> futs=new ArrayList<>();
		AtomicInteger done=new AtomicInteger();
		long t0=System.currentTimeMillis();
		for(int si:targets){
			futs.add(pool.submit(()->{
				Job_Result r=new Job_Result();
				try{
					test_State(si,worlds.get(),arenas.get(),basePalette,r);
				}catch(Throwable e){
					r.errors.add(names[si]+": "+e);
				}
				int d=done.incrementAndGet();
				if(d%2000==0){
					System.err.println("  "+d+"/"+targets.size()+" "+(System.currentTimeMillis()-t0)/1000+"s");
				}
				return r;
			}));
		}
		Job_Result all=new Job_Result();
		for(Future<Job_Result> f:futs){
			Job_Result r=f.get();
			all.configs+=r.configs;
			all.noWitness.addAll(r.noWitness);
			all.errors.addAll(r.errors);
			r.unseen.forEach((k,v)->all.unseen.merge(k,v,Long::sum));
			r.agg.forEach((k,v)->{
				Agg x=all.agg.computeIfAbsent(k,kk->new Agg());
				x.count+=v.count;
				for(String s:v.sStates){
					if(x.sStates.size()<4){
						x.sStates.add(s);
					}
				}
				for(String s:v.nBlocks){
					if(x.nBlocks.size()<12){
						x.nBlocks.add(s);
					}
				}
				for(String s:v.examples){
					if(x.examples.size()<3){
						x.examples.add(s);
					}
				}
			});
		}
		pool.shutdown();
		print_Report(all,blockNeeds.size(),targets.size(),System.currentTimeMillis()-t0);
	}

	static int[] make_Palette(){/*找见证用的常见邻居*/
		String[] ids={"air","stone","grass_block","dirt","coarse_dirt","farmland","sand","red_sand","gravel","clay","moss_block",
			"mud","mycelium","podzol","netherrack","soul_sand","soul_soil","crimson_nylium","warped_nylium","end_stone","oak_log",
			"jungle_log","oak_leaves","mangrove_leaves","water","lava","ice","magma_block","glass","oak_fence","cobblestone_wall",
			"hopper","melon","pumpkin","obsidian","chorus_plant","cactus","honey_block","oak_planks","bedrock","snow_block",
			"oak_slab","melon_stem","pumpkin_stem","sculk","pointed_dripstone","scaffolding","big_dripleaf_stem"};
		List<Integer> p=new ArrayList<>();
		for(String id:ids){
			Block b=McWorld.get_Block(id);
			p.add(McWorld.index.get(b.defaultBlockState()));
		}
		int[] r=new int[p.size()];
		for(int i=0;i<r.length;i++){
			r[i]=p.get(i);
		}
		return r;
	}
	static List<BlockState> get_Partners(Block b){/*自己的全部状态 + 头/茎/果实这类搭档的全部状态*/
		String id=McWorld.get_BlockName(b).substring("minecraft:".length());
		Set<String> ids=new LinkedHashSet<>();
		ids.add(id);
		ids.add(id+"_plant");
		ids.add(id+"_stem");
		if(id.endsWith("_plant")){
			ids.add(id.substring(0,id.length()-6));
		}
		if(id.endsWith("_stem")){
			ids.add(id.substring(0,id.length()-5));
		}
		if(id.startsWith("attached_")&&id.endsWith("_stem")){
			ids.add(id.substring(9,id.length()-5));
		}
		List<BlockState> r=new ArrayList<>();
		for(String x:ids){
			Block p=McWorld.get_Block(x);
			if(p!=null&&!p.defaultBlockState().isAir()){
				r.addAll(p.getStateDefinition().getPossibleStates());
			}
		}
		return r;
	}
	static int score_Config(McWorld w,BlockState s,int nc){/*见证搜索的打分：整体能活最重要，其次单方向不变、canSurvive*/
		int sc=0;
		w.clear_Marks();
		int g=w.judge_Game(s,nc);
		sc+=g==McWorld.G_SAME?1000:g!=McWorld.G_DEAD?500:0;
		for(Direction d:McWorld.DIRS){
			w.self=s;
			BlockState o=s.updateShape(w.level,w.ticks,McWorld.O,d,McWorld.O.relative(d),w.nb[d.ordinal()],w.rnd);
			if(o==s){
				sc+=10;
			}
		}
		w.self=s;
		if(s.canSurvive(w.level,McWorld.O)){
			sc+=30;
		}
		return sc;
	}
	static void find_Witness(McWorld w,BlockState s,int nc,int[] basePalette){
		List<BlockState> cands=new ArrayList<>();
		for(int i:basePalette){
			cands.add(McWorld.all.get(i));
		}
		cands.addAll(get_Partners(s.getBlock()));
		w.clear_World();
		int cur=score_Config(w,s,nc);
		for(int pass=0;pass<3&&cur<1060+30;pass++){
			boolean moved=false;
			for(Direction d:McWorld.DIRS){
				BlockState best=w.nb[d.ordinal()];
				for(BlockState c:cands){
					w.nb[d.ordinal()]=c;
					int sc;
					try{
						sc=score_Config(w,s,nc);
					}catch(RuntimeException e){
						continue;
					}
					if(sc>cur){
						cur=sc;
						best=c;
						moved=true;
					}
				}
				w.nb[d.ordinal()]=best;
			}
			if(!moved){
				break;
			}
		}
	}
	static String show_Config(McWorld w,int changedDir,BlockState n){
		StringBuilder sb=new StringBuilder();
		for(Direction d:McWorld.DIRS){
			BlockState x=d.ordinal()==changedDir?n:w.nb[d.ordinal()];
			if(x.isAir()){
				continue;
			}
			sb.append(' ').append(dir_Name[d.ordinal()]).append('=').append(McWorld.get_StateName(x).replace("minecraft:",""));
		}
		return sb.length()==0?" 四周全空":sb.toString();
	}
	static final Set<String> face_Props=Set.of("north","south","east","west","up","down");
	static boolean has_FaceOff(BlockState from,BlockState to){/*游戏把哪个“面”关掉了：布尔面 true→false（藤蔓/地衣），
		墙那种 none/low/tall 面变成 none（苍白苔藓地毯）*/
		for(Property<?> p:from.getProperties()){
			if(!face_Props.contains(p.getName())){
				continue;
			}
			String a=McWorld.get_Value(from,p),b=McWorld.get_Value(to,p);
			if(!a.equals(b)&&(b.equals("false")||b.equals("none"))){
				return true;
			}
		}
		return false;
	}
	static boolean is_On(String v){
		return v!=null&&!v.equals("false")&&!v.equals("none");
	}
	static Map<String,String> parse_Props(String st){
		Map<String,String> m=new HashMap<>();
		int b=st.indexOf('[');
		if(b>=0){
			for(String kv:st.substring(b+1,st.length()-1).split(",")){
				int e=kv.indexOf('=');
				if(e>0){
					m.put(kv.substring(0,e),kv.substring(e+1));
				}
			}
		}
		return m;
	}
	/*两边“改法”一样吗：只比原来开着的面——游戏关了的规则也得关、游戏留着的规则也得留（藤蔓/地衣这种只关不开的）*/
	/*规则管整个状态的方块：游戏改了哪个属性规则都得跟着改（别的方块只比面属性）*/
	static boolean is_Exact(Block b){
		return b instanceof DoorBlock||b instanceof FireBlock||b instanceof SoulFireBlock||b instanceof GrowingPlantBlock
			||b instanceof BambooStalkBlock||b instanceof BambooSaplingBlock||b instanceof BigDripleafBlock
			||b instanceof FenceBlock||b instanceof WallBlock||b instanceof MossyCarpetBlock||b instanceof IronBarsBlock
			||b instanceof FenceGateBlock||b instanceof ChestBlock||b instanceof SnowyBlock||b instanceof ChorusPlantBlock
			||b instanceof HugeMushroomBlock||b instanceof NoteBlock||b instanceof LeavesBlock||b instanceof ScaffoldingBlock
			||b instanceof PointedDripstoneBlock;
	}
	/*游戏结果 g 和规则给的状态 o 一样吗；藤头的 age 是游戏随机给的（getStateForPlacement(random)），不比*/
	static boolean same_State(BlockState g,String o){
		String gs=McWorld.get_StateName(g);
		if(g.getBlock() instanceof GrowingPlantHeadBlock){
			gs=gs.replaceAll("age=\\d+","age=*");
			o=o.replaceAll("age=\\d+","age=*");
		}
		return gs.equals(o);
	}
	static boolean same_Faces(BlockState s,BlockState g,String rules){
		Map<String,String> r=parse_Props(rules);
		for(Property<?> p:s.getProperties()){
			if(!face_Props.contains(p.getName())||!is_On(McWorld.get_Value(s,p))){
				continue;
			}
			if(is_On(McWorld.get_Value(g,p))!=is_On(r.get(p.getName()))){
				return false;
			}
		}
		return true;
	}
	static void test_State(int si,McWorld w,Arena arena,int[] basePalette,Job_Result r) throws Throwable{
		BlockState s=McWorld.all.get(si);
		Block b=s.getBlock();
		int nc=McWorld.get_NcKind(b);
		find_Witness(w,s,nc,basePalette);
		w.clear_Marks();
		int g0=w.judge_Game(s,nc);
		String bn=McWorld.get_BlockName(b).replace("minecraft:","");
		if(g0==McWorld.G_DEAD){
			r.noWitness.add(bn);
		}
		MemorySegment nbSeg=arena.allocate(ValueLayout.JAVA_INT,6);
		MemorySegment out=arena.allocate(512);
		int[] wIdx=new int[6];
		for(Direction d:McWorld.DIRS){
			wIdx[d.ordinal()]=McWorld.index.get(w.nb[d.ordinal()]);
			nbSeg.setAtIndex(ValueLayout.JAVA_INT,nb_OfDir[d.ordinal()],wIdx[d.ordinal()]);
		}
		int n=McWorld.all.size();
		String sName=names[si].replace("minecraft:","");
		for(Direction d:McWorld.DIRS){
			int di=d.ordinal();
			BlockState keep=w.nb[di];
			for(int ni=0;ni<n;ni++){
				BlockState nst=McWorld.all.get(ni);
				w.nb[di]=nst;
				nbSeg.setAtIndex(ValueLayout.JAVA_INT,nb_OfDir[di],ni);
				r.configs++;
				w.clear_Marks();
				int g;
				try{
					g=w.judge_Game(s,nc);
				}catch(RuntimeException e){
					r.errors.add(bn+" 游戏报错 "+e.getClass().getSimpleName()+" "+e.getMessage());
					continue;
				}
				boolean outside=w.outside;
				BlockState gr=w.result;
				int rr=def[si]<0?0:(int)mh_Judge.invokeExact(si,def[si],nbSeg,out,512);
				String key=null;
				if(rr==3){
					key="规则blind";
				}else if(g==McWorld.G_DEAD){
					if(rr!=1){
						key="漏拆";
					}
				}else if(rr==1){
					key="误拆";
				}else if(rr==2){
					String o=out.getString(0);
					if(g==McWorld.G_SAME){
						key="误改";
					}else if(g==McWorld.G_OTHER||is_Exact(b)?!same_State(gr,o):!same_Faces(s,gr,o)){
						key="改法不同";
					}
				}else if(g==McWorld.G_OTHER){
					key="漏改";
				}else if(g==McWorld.G_CHANGED&&is_Exact(b)){
					key="漏改";
				}else if(g==McWorld.G_CHANGED&&kind[si]>=0&&kind[si]<64&&faceKind[kind[si]]&&has_FaceOff(s,gr)){
					key="少关面";
				}
				if(key==null){
					continue;
				}
				if(outside){
					r.unseen.merge(bn,1L,Long::sum);
					continue;
				}
				String nb=McWorld.get_BlockName(nst.getBlock()).replace("minecraft:","");
				String ex=sName+" |"+show_Config(w,di,nst)+" | 游戏="+(g==McWorld.G_DEAD?"拆":g==McWorld.G_SAME?"留":McWorld.get_StateName(gr).replace("minecraft:",""))
					+" 规则="+(rr==0?"留":rr==1?"拆":out.getString(0).replace("minecraft:",""));
				r.add_Diff(key+"\t"+bn+"\t"+dir_Name[di]+(def[si]<0?"\t(规则不管)":""),sName,nb,ex);
			}
			w.nb[di]=keep;
			nbSeg.setAtIndex(ValueLayout.JAVA_INT,nb_OfDir[di],wIdx[di]);
		}
	}
	/*已知的、故意和游戏不一样的地方（结论类型 正则, 方块 正则, 原因）：报告里单独列，不算差异。改了规则这里要跟着改*/
	static final String[][] known={
		{"误改","frosted_ice","霜冰见到就化成水（用户要求；原版要看光照慢慢化）"},
		{".*","water|lava","规则不管：流体自己会流（流动水靠周围的水源，原版按流体刻慢慢算）"},
	};
	static String get_Known(String key){
		String[] f=key.split("\t");
		for(String[] k:known){
			if(f[0].matches(k[0])&&f[1].matches(k[1])){
				return k[2];
			}
		}
		return null;
	}
	static void print_Report(Job_Result all,int blocks,int states,long ms){
		StringBuilder sb=new StringBuilder();
		sb.append("#RulesCmp ").append(blocks).append(" 种方块 ").append(states).append(" 个状态 ").append(all.configs).append(" 个配置 用时 ").append(ms/1000).append("s\n");
		String[] order={"误拆","误改","改法不同","规则blind","漏改","少关面","漏拆"};
		Map<String,Long> total=new LinkedHashMap<>();
		List<String> keys=new ArrayList<>(all.agg.keySet());
		keys.sort(Comparator.comparing((String k)->Arrays.asList(order).indexOf(k.split("\t")[0])).thenComparing(k->-all.agg.get(k).count));
		Map<String,Long> knownTotal=new LinkedHashMap<>();
		Map<String,Set<String>> knownBlocks=new LinkedHashMap<>();
		for(String k:keys){
			String why=get_Known(k);
			String[] f=k.split("\t");
			if(why==null){
				total.merge(f[0],all.agg.get(k).count,Long::sum);
			}else{
				knownTotal.merge(f[0]+" "+why,all.agg.get(k).count,Long::sum);
				knownBlocks.computeIfAbsent(f[0]+" "+why,x->new TreeSet<>()).add(f[1]);
			}
		}
		sb.append("#差异合计 ").append(total.isEmpty()?"{}（没有）":total).append("\n");
		for(var e:knownTotal.entrySet()){
			sb.append("#已知 ").append(e.getKey()).append(" ").append(e.getValue()).append(" 个配置：").append(knownBlocks.get(e.getKey())).append("\n");
		}
		sb.append("#无见证（游戏在常见邻居里找不到能活的配置）: ").append(all.noWitness).append("\n");
		sb.append("#看不到（游戏读了六邻以外，差异不计）: ").append(all.unseen).append("\n");
		if(!all.errors.isEmpty()){
			sb.append("#报错: ").append(all.errors.size()).append(" ").append(all.errors.stream().limit(10).toList()).append("\n");
		}
		for(String k:keys){
			if(get_Known(k)!=null){
				continue;
			}
			Agg a=all.agg.get(k);
			sb.append(k).append('\t').append(a.count).append("\t状态例: ").append(a.sStates).append("\t邻居: ").append(a.nBlocks).append('\n');
			for(String e:a.examples){
				sb.append("\t\t").append(e).append('\n');
			}
		}
		System.out.print(sb);
	}
}
