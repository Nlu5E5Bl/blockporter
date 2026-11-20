import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.tags.TagLoader;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.ScheduledTickAccess;
import net.minecraft.world.level.block.BaseRailBlock;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.DiodeBlock;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.BrushableBlock;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.LeavesBlock;
import net.minecraft.world.level.block.LiquidBlock;
import net.minecraft.world.level.block.ScaffoldingBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.block.state.properties.RailShape;
import net.minecraft.world.ticks.LevelTickAccess;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.TreeSet;

/*McWorld.java：test/mc 下几个工具共用——在原版服务端 jar 上初始化注册表并绑好原版数据包的标签，
 *	再造一个“自己 + 六个邻居”的假世界（动态代理实现 LevelReader / ScheduledTickAccess），让游戏自己判。
 *	只认 26.x（不混淆）。一个线程一份 McWorld（里面的状态不共享）。
 *	六个邻居按 Direction.ordinal 存：下 上 北 南 西 东；六邻以外的格子一律是空气，读到了记 outside。
 *	光照：读到光照就记 light_used，judge_Game 先按亮（15）判，判死了再按暗（0）判一遍，有一种能活就算活
 *	  （存档里看不到光照，取宽松的一边）。*/
public class McWorld {
	public static final BlockPos O=new BlockPos(0,64,0);
	public static final Direction[] DIRS=Direction.values();
	public static BlockState AIR;
	public static final List<BlockState> all=new ArrayList<>();/*全部方块状态（注册表顺序，和 ShapeDump 的行一致）*/
	public static final IdentityHashMap<BlockState,Integer> index=new IdentityHashMap<>();
	public static RegistryAccess.Frozen registries;

	public static void init_Game(){
		SharedConstants.tryDetectVersion();
		Bootstrap.bootStrap();
		/*只跑 Bootstrap 的话 #dirt 这类标签全是空的：照 WorldLoader 的做法读原版数据包，把标签绑到内置注册表上*/
		Object vanilla=ServerPacksSource.createVanillaPackSource();/*26.1.1 是 PackResources，26.3 包了一层（fullResources() 给 PackResources）*/
		PackResources pack;
		try{
			pack=vanilla instanceof PackResources?(PackResources)vanilla:(PackResources)vanilla.getClass().getMethod("fullResources").invoke(vanilla);
		}catch(ReflectiveOperationException e){
			throw new IllegalStateException(e);
		}
		List<PackResources> packs=List.of(pack);
		MultiPackResourceManager rm=new MultiPackResourceManager(PackType.SERVER_DATA,packs);
		for(Registry.PendingTags<?> t:TagLoader.loadTagsForExistingRegistries(rm,RegistryLayer.createRegistryAccess().getLayer(RegistryLayer.STATIC))){
			t.apply();
		}
		registries=RegistryAccess.fromRegistryOfRegistries(BuiltInRegistries.REGISTRY);
		AIR=Blocks.AIR.defaultBlockState();
		for(Block b:BuiltInRegistries.BLOCK){
			for(BlockState s:b.getStateDefinition().getPossibleStates()){
				index.put(s,all.size());
				all.add(s);
			}
		}
	}
	public static String get_BlockName(Block b){
		return BuiltInRegistries.BLOCK.getKey(b).toString();
	}
	public static String get_StateName(BlockState s){/*minecraft:名字[属性=值,...]，属性按名字排序（和 ShapeDump/rules_shim 同一种写法）*/
		StringBuilder sb=new StringBuilder(get_BlockName(s.getBlock()));
		if(!s.getProperties().isEmpty()){
			List<String> kv=new ArrayList<>();
			for(Property<?> pr:s.getProperties()){
				kv.add(pr.getName()+"="+get_Value(s,pr));
			}
			Collections.sort(kv);
			sb.append('[').append(String.join(",",kv)).append(']');
		}
		return sb.toString();
	}
	@SuppressWarnings({"unchecked","rawtypes"})
	public static String get_Value(BlockState s,Property pr){
		return pr.getName(s.getValue(pr));
	}
	public static Block get_Block(String id){
		return BuiltInRegistries.BLOCK.getValue(Identifier.withDefaultNamespace(id));
	}
	/*neighborChanged 里会把自己移除的方块（26.1.1 源码里只有这三类；别的覆盖 neighborChanged 的只管红石信号）*/
	public static final int NC_NONE=0,NC_SURVIVE=1,NC_RAIL=2;
	static boolean is_RedstoneWire(Block b){/*红石线：26.1.1 叫 RedStoneWireBlock，26.3 改名 RedstoneWireBlock，按类名认（两个版本都能编）*/
		String n=b.getClass().getSimpleName();
		return n.equals("RedStoneWireBlock")||n.equals("RedstoneWireBlock");
	}
	public static int get_NcKind(Block b){
		if(b instanceof BaseRailBlock){
			return NC_RAIL;/*BaseRailBlock.shouldBeRemoved：下面带边，上坡的还要坡那一侧的邻居带边*/
		}
		if(b instanceof DiodeBlock||is_RedstoneWire(b)){
			return NC_SURVIVE;/*中继器/比较器/红石线：canSurvive 假就掉*/
		}
		return NC_NONE;
	}
	boolean is_RailRemoved(BlockState s){
		if(!Block.canSupportRigidBlock(level,O.below())){
			return true;
		}
		RailShape sh=s.getValue(((BaseRailBlock)s.getBlock()).getShapeProperty());
		switch(sh){
			case ASCENDING_EAST:return !Block.canSupportRigidBlock(level,O.east());
			case ASCENDING_WEST:return !Block.canSupportRigidBlock(level,O.west());
			case ASCENDING_NORTH:return !Block.canSupportRigidBlock(level,O.north());
			case ASCENDING_SOUTH:return !Block.canSupportRigidBlock(level,O.south());
			default:return false;
		}
	}

	/*---- 假世界（一个线程一份） ----*/
	public BlockState self;
	public final BlockState[] nb=new BlockState[6];
	public int read_mask;            /*读过哪几个邻居（位=Direction.ordinal；哪个方向的更新把它弄没了也算）*/
	public boolean outside;          /*读过六邻以外的格子*/
	public boolean light_used;
	public boolean self_tick;        /*给自己这格排了方块刻*/
	public final Set<String> special=new TreeSet<>();/*用过的其它东西（方块实体/海平面/不支持的方法…）*/
	public int light=15;
	public BlockState result;        /*judge_Game 判完的状态（G_CHANGED/G_OTHER 时有用）*/
	public final LevelReader level;
	public final ScheduledTickAccess ticks;
	final LevelTickAccess<?> tick_list;
	public final RandomSource rnd=RandomSource.create(0);

	public static class Unsupported extends RuntimeException{
		Unsupported(String s){
			super(s);
		}
	}
	public McWorld(){
		level=(LevelReader)Proxy.newProxyInstance(LevelReader.class.getClassLoader(),new Class<?>[]{LevelReader.class},this::handle_Level);
		ticks=(ScheduledTickAccess)Proxy.newProxyInstance(ScheduledTickAccess.class.getClassLoader(),new Class<?>[]{ScheduledTickAccess.class},this::handle_Ticks);
		tick_list=(LevelTickAccess<?>)Proxy.newProxyInstance(LevelTickAccess.class.getClassLoader(),new Class<?>[]{LevelTickAccess.class},this::handle_TickList);
		clear_World();
	}
	public void clear_World(){
		self=AIR;
		Arrays.fill(nb,AIR);
	}
	public void clear_Marks(){
		read_mask=0;
		outside=false;
		light_used=false;
		special.clear();
	}
	BlockState get_At(BlockPos p){
		if(p.equals(O)){
			return self;
		}
		int dx=p.getX()-O.getX(),dy=p.getY()-O.getY(),dz=p.getZ()-O.getZ();
		if(Math.abs(dx)+Math.abs(dy)+Math.abs(dz)==1){
			for(Direction d:DIRS){
				if(d.getStepX()==dx&&d.getStepY()==dy&&d.getStepZ()==dz){
					read_mask|=1<<d.ordinal();
					return nb[d.ordinal()];
				}
			}
		}
		outside=true;
		return AIR;
	}
	Object handle_Level(Object proxy,Method m,Object[] a) throws Throwable{
		String n=m.getName();
		switch(n){
			case "getBlockState":return get_At((BlockPos)a[0]);
			case "getFluidState":return get_At((BlockPos)a[0]).getFluidState();
			case "getBlockEntity":special.add("方块实体");return m.getReturnType()==Optional.class?Optional.empty():null;
			case "getMinY":return -64;
			case "getHeight":if(a==null||a.length==0){return 384;}special.add("高度图");throw new Unsupported(n);
			case "isClientSide":return false;
			case "getSeaLevel":special.add("海平面");return 63;
			case "registryAccess":return registries;
			case "getBrightness": case "getRawBrightness": case "getMaxLocalRawBrightness":light_used=true;return light;
			case "getSkyDarken":light_used=true;return 0;
			case "canSeeSky":light_used=true;return light==15;
			case "hashCode":return System.identityHashCode(proxy);
			case "equals":return proxy==a[0];
			case "toString":return "McWorld.level";
		}
		if(m.isDefault()){
			return InvocationHandler.invokeDefault(proxy,m,a);
		}
		special.add("不支持:"+n);
		throw new Unsupported(n);
	}
	Object handle_Ticks(Object proxy,Method m,Object[] a) throws Throwable{
		String n=m.getName();
		switch(n){
			case "scheduleTick":
				if(a[1] instanceof Block&&O.equals(a[0])){
					self_tick=true;
				}
				return null;
			case "getBlockTicks": case "getFluidTicks":return tick_list;
			case "hashCode":return System.identityHashCode(proxy);
			case "equals":return proxy==a[0];
			case "toString":return "McWorld.ticks";
		}
		special.add("不支持:"+n);
		throw new Unsupported(n);
	}
	Object handle_TickList(Object proxy,Method m,Object[] a) throws Throwable{/*计划刻表：一律“没排过”*/
		switch(m.getName()){
			case "willTickThisTick": case "hasScheduledTick":return false;
			case "count":return 0;
			case "schedule":return null;
			case "hashCode":return System.identityHashCode(proxy);
			case "equals":return proxy==a[0];
		}
		throw new Unsupported(m.getName());
	}

	/*---- 游戏怎么判 ----*/
	public static final int G_SAME=0,  /*留着，状态不变*/
	                        G_DEAD=1,  /*拆掉（变空气/变成纯流体）*/
	                        G_CHANGED=2,/*留着，但状态变了（藤蔓关面、栅栏改连接…），见 result*/
	                        G_OTHER=3; /*变成别的方块（混凝土粉末遇水），见 result*/
	static boolean is_Gone(BlockState o){
		return o.isAir()||o.getBlock() instanceof LiquidBlock;
	}
	/*六个邻居各给自己发一次更新（updateShape，按 下上北南西东 依次，状态接着往下传），
	  再按原版的另外几条移除路径补判：给自己排了方块刻的（紫颂花/仙人掌/甘蔗/苍白垂须…到刻里判 canSurvive；
	  重力方块到刻里看下面空不空），和自己写了 neighborChanged 的（铁轨/中继器/红石线…在那里判 canSurvive）。*/
	int run_Updates(BlockState s0,int nc){
		BlockState s=s0;
		boolean tick=false;
		try{
			for(Direction d:DIRS){
				self=s;
				self_tick=false;
				BlockState o=s.updateShape(level,ticks,O,d,O.relative(d),nb[d.ordinal()],rnd);
				tick|=self_tick;
				if(is_Gone(o)){
					read_mask|=1<<d.ordinal();/*这个方向的更新把它弄没了：它靠的是这个邻居（有的方块直接用传进来的邻居状态，不去读世界）*/
					return G_DEAD;
				}
				if(o.getBlock()!=s.getBlock()){
					result=o;
					return G_OTHER;
				}
				s=o;
			}
			self=s;
			if((tick||nc==NC_SURVIVE)&&!s.canSurvive(level,O)){
				return G_DEAD;
			}
			if(nc==NC_RAIL&&is_RailRemoved(s)){
				return G_DEAD;
			}
			Block b=s.getBlock();
			if(tick&&(b instanceof FallingBlock||b instanceof BrushableBlock)&&FallingBlock.isFree(nb[Direction.DOWN.ordinal()])){
				return G_DEAD;/*重力方块：刻里看正下方 FallingBlock.isFree，空了就掉（这格变空）*/
			}
			if(tick&&b instanceof LeavesBlock){/*树叶：刻里 updateDistance（tick 要 ServerLevel，这里照它的式子算：六邻 getOptionalDistanceAt+1 取小，封顶 7）*/
				int d=7;
				for(Direction dir:DIRS){
					d=Math.min(d,LeavesBlock.getOptionalDistanceAt(nb[dir.ordinal()]).orElse(7)+1);
				}
				s=s.setValue(LeavesBlock.DISTANCE,d);
			}
			if(tick&&b instanceof ScaffoldingBlock){/*脚手架：刻里 distance/bottom 换成现算的（distance=7 上面 canSurvive 已经判死了）*/
				int d=ScaffoldingBlock.getDistance(level,O);
				s=s.setValue(ScaffoldingBlock.DISTANCE,d).setValue(ScaffoldingBlock.BOTTOM,d>0&&!nb[Direction.DOWN.ordinal()].is(b));
			}
			result=s;
			return s==s0?G_SAME:G_CHANGED;
		}finally{
			self=s0;
		}
	}
	public int judge_Game(BlockState s,int nc){
		light=15;
		int r=run_Updates(s,nc);
		if(r==G_DEAD&&light_used){
			light=0;
			r=run_Updates(s,nc);
			light=15;
		}
		return r;
	}
}
