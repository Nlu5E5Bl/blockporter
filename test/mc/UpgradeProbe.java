import com.mojang.serialization.Codec;
import com.mojang.serialization.DataResult;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.ListTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.NbtOps;
import net.minecraft.nbt.NbtUtils;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.datafix.DataFixTypes;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.util.datafix.fixes.ChunkHeightAndBiomeFix;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.PalettedContainer;
import net.minecraft.world.level.chunk.Strategy;
import java.io.PrintWriter;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/*UpgradeProbe.java：游戏怎么读老版本的区块——先升级、再读（test/mcdump.sh upgrade 调它，C++ 那边是 test/upgradecmp.cpp）。
 *	UpgradeProbe <目录>
 *	      目录里每个 in_<名字>.nbt（老 DataVersion 的区块）照 ChunkMap.upgradeChunkTag：塞 __context（主世界、minecraft:noise），
 *	      用游戏的 DataFixers 升到当前版本，存 up_<名字>.nbt；再照 SerializableChunkData.parse 用游戏的 block_states 编解码
 *	      （codecRW + promotePartial）读每一段，写 out_<名字>.txt：
 *	        UPGRADE_FAIL <异常>               升级就抛了（游戏：这一块载入失败，重新生成）
 *	        Y <y> FATAL <原因>                 这一段读不了（游戏：整块载入失败）
 *	        Y <y> OK <部分错误数> <4096 个方块 ID>  读得了；下标 i=(y*16+z)*16+x
 *	        Y <y> NONE                        这一段没有 block_states（游戏当全空气）
 *	UpgradeProbe ceillog2 <out.txt>
 *	      写 ChunkHeightAndBiomeFix.ceillog2(2^k)（它用 Math.log 算，2 的幂会不会多出 1）
 *	只读 jar；结果都写在调用者给的目录里。*/
public class UpgradeProbe {
	public static void main(String[] a) throws Exception {
		SharedConstants.tryDetectVersion();
		Bootstrap.bootStrap();
		if(a[0].equals("ceillog2")){/*Bootstrap 以后 System.out 走 log4j，结果写文件*/
			try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(Path.of(a[1])))){
				for(int k=0;k<=30;k++){
					out.println((1<<k)+"\t"+ChunkHeightAndBiomeFix.ceillog2(1<<k));
				}
			}
			return;
		}
		Path dir=Path.of(a[0]);
		int target=SharedConstants.getCurrentVersion().dataVersion().version();
		Codec<PalettedContainer<BlockState>> codec=PalettedContainer.codecRW(BlockState.CODEC,
			Strategy.createForBlockStates(Block.BLOCK_STATE_REGISTRY),Blocks.AIR.defaultBlockState());
		List<Path> ins=new ArrayList<>();
		try(DirectoryStream<Path> ds=Files.newDirectoryStream(dir,"in_*.nbt")){
			for(Path p:ds){
				ins.add(p);
			}
		}
		for(Path p:ins){
			String name=p.getFileName().toString();
			name=name.substring(3,name.length()-4);
			CompoundTag tag=NbtIo.read(p);
			int version=NbtUtils.getDataVersion(tag,-1);
			CompoundTag ctx=new CompoundTag();
			ctx.putString("dimension","minecraft:overworld");
			ctx.putString("generator","minecraft:noise");
			try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(dir.resolve("out_"+name+".txt")))){
				try{
					tag.put("__context",ctx);
					tag=DataFixTypes.CHUNK.update(DataFixers.getDataFixer(),tag,version,target);
					tag.remove("__context");
					NbtUtils.addDataVersion(tag,target);
				}catch(Exception e){
					out.println("UPGRADE_FAIL "+e.toString().replace('\n',' '));
					continue;
				}
				NbtIo.write(tag,dir.resolve("up_"+name+".nbt"));
				ListTag secs=tag.getListOrEmpty("sections");
				for(int i=0;i<secs.size();i++){
					Optional<CompoundTag> st=secs.getCompound(i);
					if(st.isEmpty()){
						continue;
					}
					int y=st.get().getByteOr("Y",(byte)0);
					Optional<CompoundTag> bs=st.get().getCompound("block_states");
					if(bs.isEmpty()){
						out.println("Y "+y+" NONE");
						continue;
					}
					List<String> partial=new ArrayList<>();
					DataResult<PalettedContainer<BlockState>> r;
					try{
						r=codec.parse(NbtOps.INSTANCE,bs.get()).promotePartial(partial::add);
					}catch(RuntimeException e){/*解码时抛的异常：SerializableChunkData.parse 也接不住，整块载入失败*/
						out.println("Y "+y+" FATAL throw "+e.toString().replace('\n',' '));
						continue;
					}
					Optional<PalettedContainer<BlockState>> c=r.result();
					if(c.isEmpty()){
						out.println("Y "+y+" FATAL "+r.error().map(e->e.message()).orElse("?").replace('\n',' '));
						continue;
					}
					StringBuilder sb=new StringBuilder("Y "+y+" OK "+partial.size());
					try{
						for(int k=0;k<4096;k++){
							BlockState s=c.get().get(k&15,k>>8,(k>>4)&15);
							sb.append(' ').append(BuiltInRegistries.BLOCK.getKey(s.getBlock()));
						}
					}catch(RuntimeException e){/*解得出来、取格子时才抛*/
						out.println("Y "+y+" GETTHROW "+e.toString().replace('\n',' '));
						continue;
					}
					out.println(sb);
				}
			}
		}
		System.out.println("OK "+ins.size());
	}
}
