import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.NbtIo;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.datafix.fixes.BitStorageAlignFix;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.storage.RegionFile;
import net.minecraft.world.level.chunk.storage.RegionFileVersion;
import net.minecraft.world.level.chunk.storage.RegionStorageInfo;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.file.Path;

/*RegionProbe.java：拿游戏自己的 RegionFile 读写区域文件，和 libnbtrw 的 MCA 对拍（压缩类型、外置 .mcc）。
 *	write <目录> <rx> <rz> <gzip|deflate|none|lz4> <a.nbt> <cx> <cz> [<b.nbt> <cx> <cz> ...]
 *	      把每个裸 NBT 文件写进 r.<rx>.<rz>.mca（游戏按压缩后大小自己决定要不要放外置 .mcc）
 *	read  <目录> <rx> <rz> <cx> <cz> <out.nbt>
 *	      游戏读这一格；读不到打印 NULL（游戏同时在日志里报原因），读到就存成裸 NBT
 *	align <in.nbt> <out.nbt>
 *	      in.nbt 里每个 Long_Array 成员（名字是位宽，比如 "5"）按 BitStorageAlignFix.addPadding（2527 那次升级）
 *	      从“紧排、跨 long”转成“不跨 long”，同名存进 out.nbt
 *	只读 jar；结果都写在调用者给的目录里。*/
public class RegionProbe {
	static RegionFileVersion get_Version(String name){
		switch(name){
		case "gzip":return RegionFileVersion.VERSION_GZIP;
		case "deflate":return RegionFileVersion.VERSION_DEFLATE;
		case "none":return RegionFileVersion.VERSION_NONE;
		case "lz4":return RegionFileVersion.VERSION_LZ4;
		default:throw new IllegalArgumentException("不认识的压缩："+name);
		}
	}
	public static void main(String[] a) throws Exception {
		if(a[0].equals("align")){
			CompoundTag in=NbtIo.read(Path.of(a[1]));
			CompoundTag out=new CompoundTag();
			for(String k:in.keySet()){
				int bits=Integer.parseInt(k);
				long[] data=in.getLongArray(k).orElseThrow();
				/*游戏（BitStorageAlignFix.updateSections）只对不是2的幂的位宽转；2的幂两种排法本来一样*/
				out.putLongArray(k,(bits&(bits-1))==0?data:BitStorageAlignFix.addPadding(4096,bits,data));
			}
			NbtIo.write(out,Path.of(a[2]));
			System.out.println("OK");
			return;
		}
		SharedConstants.tryDetectVersion();
		Bootstrap.bootStrap();
		Path dir=Path.of(a[1]);
		int rx=Integer.parseInt(a[2]),rz=Integer.parseInt(a[3]);
		Path file=dir.resolve("r."+rx+"."+rz+".mca");
		RegionStorageInfo info=new RegionStorageInfo("probe",Level.OVERWORLD,"chunk");
		if(a[0].equals("write")){
			try(RegionFile rf=new RegionFile(info,file,dir,get_Version(a[4]),false)){
				for(int i=5;i+2<a.length;i+=3){
					CompoundTag tag=NbtIo.read(Path.of(a[i]));
					ChunkPos pos=new ChunkPos(Integer.parseInt(a[i+1]),Integer.parseInt(a[i+2]));
					try(DataOutputStream out=rf.getChunkDataOutputStream(pos)){
						NbtIo.write(tag,out);
					}
				}
			}
			System.out.println("OK");
		}else if(a[0].equals("read")){
			try(RegionFile rf=new RegionFile(info,file,dir,false)){
				ChunkPos pos=new ChunkPos(Integer.parseInt(a[4]),Integer.parseInt(a[5]));
				try(DataInputStream in=rf.getChunkDataInputStream(pos)){
					if(in==null){
						System.out.println("NULL");
						return;
					}
					NbtIo.write(NbtIo.read(in),Path.of(a[6]));
				}
			}
			System.out.println("OK");
		}else{
			throw new IllegalArgumentException("不认识的命令："+a[0]);
		}
	}
}
