"""poitab.py：方块状态 → POI 类型/票上限 的**测试侧**独立表（给 check_poitab.py、check_poi_scan.py 用）
 *	来源：minecraft.wiki `Point_of_Interest.html` §List of points of interest（Block / Max Entity Count 两列）
 *	      和它的脚注1"床存的是 head 那一半"。
 *	这张表是照着 wiki **手工抄**的，故意不看 rules.cpp 的 poi_Defs：
 *	rules 的表要是漏了哪个方块或者对错了类型/票数，check_poitab.py 对拍就会对不上。
"""
# 方块basename → (POI类型, 票上限)
BLOCK = {
	# 13 个村民工作站（Max Entity Count = 1）
	'blast_furnace': ('minecraft:armorer', 1),
	'smoker': ('minecraft:butcher', 1),
	'cartography_table': ('minecraft:cartographer', 1),
	'brewing_stand': ('minecraft:cleric', 1),
	'composter': ('minecraft:farmer', 1),
	'barrel': ('minecraft:fisherman', 1),
	'fletching_table': ('minecraft:fletcher', 1),
	'cauldron': ('minecraft:leatherworker', 1),
	'water_cauldron': ('minecraft:leatherworker', 1),
	'lava_cauldron': ('minecraft:leatherworker', 1),
	'powder_snow_cauldron': ('minecraft:leatherworker', 1),
	'lectern': ('minecraft:librarian', 1),
	'stonecutter': ('minecraft:mason', 1),
	'loom': ('minecraft:shepherd', 1),
	'smithing_table': ('minecraft:toolsmith', 1),
	'grindstone': ('minecraft:weaponsmith', 1),
	# 钟和床（村民，1 / 32）
	'bell': ('minecraft:meeting', 32),
	# 不认领的（Max Entity Count = 0）
	'bee_nest': ('minecraft:bee_nest', 0),
	'beehive': ('minecraft:beehive', 0),
	'lightning_rod': ('minecraft:lightning_rod', 0),
	'exposed_lightning_rod': ('minecraft:lightning_rod', 0),
	'weathered_lightning_rod': ('minecraft:lightning_rod', 0),
	'oxidized_lightning_rod': ('minecraft:lightning_rod', 0),
	'waxed_lightning_rod': ('minecraft:lightning_rod', 0),
	'waxed_exposed_lightning_rod': ('minecraft:lightning_rod', 0),
	'waxed_weathered_lightning_rod': ('minecraft:lightning_rod', 0),
	'waxed_oxidized_lightning_rod': ('minecraft:lightning_rod', 0),
	'nether_portal': ('minecraft:nether_portal', 0),
	'lodestone': ('minecraft:lodestone', 0),
	'test_instance_block': ('minecraft:test_instance', 0),	# 类型名是 test_instance（不是 test_instance_block）
}
BED = ('minecraft:home', 1)
TYPES = set(t for t, _ in BLOCK.values()) | {BED[0]}


def base_of(name):
	"""去命名空间；不是 minecraft 的返回 None（这些检查里就当"认不出"）"""
	if not name:
		return None
	if ':' in name:
		ns, b = name.split(':', 1)
		return b if ns == 'minecraft' else None
	return name


def poi_of(block):
	"""方块状态（nbt.py 解出来的dict）→(POI类型, 票上限)；不是POI方块/认不出给None"""
	if block is None:
		return None
	base = base_of(block.get('Name', ''))
	if not base:
		return None
	if base.endswith('_bed'):		# 只有 head 那半有记录
		return BED if (block.get('Properties') or {}).get('part') == 'head' else None
	return BLOCK.get(base)
