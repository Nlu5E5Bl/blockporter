import net.minecraft.SharedConstants;
import net.minecraft.tags.BlockTags;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.WallBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
/*ShapeDump.java：手动工具——在原版服务端 jar 上初始化注册表，让游戏自己算每个方块状态的支撑面，给 shapecmp.cpp 对拍 rules.so。
 *	由 test/mcdump.sh shape 编译、运行；只认 26.x（不混淆的版本，类名和反编译源码一致）。
 *	输出一行一个状态，tab 分隔：
 *	  状态（minecraft:名字[属性=值,...]，属性按名字排序）
 *	  下 上 北 南 西 东 六列：F=isFaceSturdy FULL，C=CENTER，R=RIGID，K=碰撞箱这一面是满的（Block.isFaceFull），都没有写 -
 *	  solid（legacy isSolid）  render（isSolidRender）  nocoll/coll/fullcoll（碰撞箱空/有/整格）  topcoll（碰撞箱顶面不空）
 *	  replace（canBeReplaced()：FallingBlock.isFree 用）  liquid（BlockState.liquid()）  burn（FireBlock.canBurn：火能不能靠它活）
 *	  under（这格在墙上面时，墙看它碰撞箱底面盖没盖住哪几块：WallBlock.isCovered 拿 getFaceShape(DOWN) 和墙自己的
 *	        TEST_SHAPE_POST / TEST_SHAPES_WALL 比；P=中间的柱子、N/E/S/W=朝那边的那条臂，都没盖住写 -）
 *	  instr（音符盒放在它上面/下面时取的音色：NoteBlockInstrument 的序号，NoteBlock.setInstrument 用）
 *	  leafkeep（#prevents_nearby_leaf_decay：树叶看它当 distance 0，LeavesBlock.getOptionalDistanceAt）
 *	  default（这是方块的默认状态：26.3 起存档里没写的属性取默认值）
 *	和存档无关的状态（EmptyBlockGetter、原点）：依赖邻居/方块实体的动态形状在这里只是“孤零零放着”时的样子。
 *	最后再加几行 @off：碰撞箱随坐标偏移的两种（竹子、滴水石锥），在一批坐标上各导一遍 under
 *	  （@off \t 状态 \t x \t z \t under），shapecmp 拿 rules.so 的墙规则在同样的坐标上核对。
 */
public class ShapeDump {
	@SuppressWarnings("deprecation")
	public static void main(String[] args)throws Exception{
		McWorld.init_Game();/*要绑好原版数据包的标签（leafkeep 列）*/
		Direction[] dirs={Direction.DOWN,Direction.UP,Direction.NORTH,Direction.SOUTH,Direction.WEST,Direction.EAST};
		Method burn=FireBlock.class.getDeclaredMethod("canBurn",BlockState.class);/*protected：火只认 igniteOdds>0 且没含水的*/
		burn.setAccessible(true);
		java.lang.reflect.Field fPost=WallBlock.class.getDeclaredField("TEST_SHAPE_POST");/*private static：墙判“上面盖住没有”用的那几块*/
		java.lang.reflect.Field fArms=WallBlock.class.getDeclaredField("TEST_SHAPES_WALL");
		fPost.setAccessible(true);
		fArms.setAccessible(true);
		VoxelShape testPost=(VoxelShape)fPost.get(null);
		@SuppressWarnings("unchecked")
		Map<Direction,VoxelShape> testArms=(Map<Direction,VoxelShape>)fArms.get(null);
		Direction[] arms={Direction.NORTH,Direction.EAST,Direction.SOUTH,Direction.WEST};
		EmptyBlockGetter g=EmptyBlockGetter.INSTANCE;
		BlockPos p=BlockPos.ZERO;
		StringBuilder out=new StringBuilder();
		out.append("#").append(SharedConstants.getCurrentVersion().name())
			.append("\tD\tU\tN\tS\tW\tE\tsolid\trender\tcoll\ttopcoll\treplace\tliquid\tburn\tunder\tinstr\tleafkeep\tdefault\n");
		for(Block b:BuiltInRegistries.BLOCK){
			String name=BuiltInRegistries.BLOCK.getKey(b).toString();
			for(BlockState s:b.getStateDefinition().getPossibleStates()){
				StringBuilder sb=new StringBuilder(name);
				if(!s.getProperties().isEmpty()){
					List<String> kv=new ArrayList<>();
					for(Property<?> pr:s.getProperties()){
						kv.add(pr.getName()+"="+get_Value(s,pr));
					}
					Collections.sort(kv);
					sb.append('[').append(String.join(",",kv)).append(']');
				}
				VoxelShape coll=s.getCollisionShape(g,p);
				for(Direction d:dirs){
					String f="";
					if(s.isFaceSturdy(g,p,d,SupportType.FULL)){
						f+="F";
					}
					if(s.isFaceSturdy(g,p,d,SupportType.CENTER)){
						f+="C";
					}
					if(s.isFaceSturdy(g,p,d,SupportType.RIGID)){
						f+="R";
					}
					if(Block.isFaceFull(coll,d)){
						f+="K";
					}
					sb.append('\t').append(f.isEmpty()?"-":f);
				}
				sb.append('\t').append(s.isSolid()?"solid":"-");
				sb.append('\t').append(s.isSolidRender()?"render":"-");
				sb.append('\t').append(coll.isEmpty()?"nocoll":(Block.isShapeFullBlock(coll)?"fullcoll":"coll"));
				sb.append('\t').append(coll.isEmpty()||coll.getFaceShape(Direction.UP).isEmpty()?"-":"topcoll");
				sb.append('\t').append(s.canBeReplaced()?"replace":"-");
				sb.append('\t').append(s.liquid()?"liquid":"-");
				sb.append('\t').append((Boolean)burn.invoke(Blocks.FIRE,s)?"burn":"-");
				VoxelShape under=coll.getFaceShape(Direction.DOWN);/*WallBlock.updateShape：topNeighbour.getCollisionShape(level,topPos).getFaceShape(DOWN)*/
				String u=is_Covered(under,testPost)?"P":"";
				for(Direction d:arms){
					if(is_Covered(under,testArms.get(d))){
						u+=d.getName().substring(0,1).toUpperCase();
					}
				}
				sb.append('\t').append(u.isEmpty()?"-":u);
				sb.append('\t').append(s.instrument().ordinal());
				sb.append('\t').append(s.is(BlockTags.PREVENTS_NEARBY_LEAF_DECAY)?"leafkeep":"-");
				sb.append('\t').append(s==b.defaultBlockState()?"default":"-");
				out.append(sb).append('\n');
			}
		}
		/*碰撞箱随坐标偏移的：-8..7 的方格，再加几处 x*3129871 会溢出 int 的大坐标*/
		List<int[]> pos=new ArrayList<>();
		for(int x=-8;x<8;x++){
			for(int z=-8;z<8;z++){
				pos.add(new int[]{x,z});
			}
		}
		int[] big={-29999999,-1000,687,29999999};
		for(int x:big){
			for(int z:big){
				pos.add(new int[]{x,z});
			}
		}
		List<Block> offs=new ArrayList<>(List.of(Blocks.BAMBOO,Blocks.POINTED_DRIPSTONE));
		BuiltInRegistries.BLOCK.getOptional(net.minecraft.resources.Identifier.withDefaultNamespace("sulfur_spike")).ifPresent(offs::add);/*26.3 起*/
		for(Block b:offs){
			String name=BuiltInRegistries.BLOCK.getKey(b).toString();
			for(BlockState s:b.getStateDefinition().getPossibleStates()){
				List<String> kv=new ArrayList<>();
				for(Property<?> pr:s.getProperties()){
					kv.add(pr.getName()+"="+get_Value(s,pr));
				}
				Collections.sort(kv);
				String st=name+(kv.isEmpty()?"":"["+String.join(",",kv)+"]");
				for(int[] xz:pos){
					VoxelShape under=s.getCollisionShape(g,new BlockPos(xz[0],65,xz[1])).getFaceShape(Direction.DOWN);
					String u=is_Covered(under,testPost)?"P":"";
					for(Direction d:arms){
						if(is_Covered(under,testArms.get(d))){
							u+=d.getName().substring(0,1).toUpperCase();
						}
					}
					out.append("@off\t").append(st).append('\t').append(xz[0]).append('\t').append(xz[1]).append('\t')
						.append(u.isEmpty()?"-":u).append('\n');
				}
			}
		}
		System.out.print(out);
	}
	static boolean is_Covered(VoxelShape above,VoxelShape test){/*WallBlock.isCovered 原样*/
		return !Shapes.joinIsNotEmpty(test,above,BooleanOp.ONLY_FIRST);
	}
	@SuppressWarnings({"unchecked","rawtypes"})
	static String get_Value(BlockState s,Property pr){
		return pr.getName(s.getValue(pr));
	}
}
