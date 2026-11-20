#!/usr/bin/env python3
"""wiki2md.py：把 minecraft.wiki 的页面（正常渲染出来的 HTML）转成 Markdown，给规则核对/维护时阅读
	只抓页面本身：https://minecraft.wiki/w/<页面>（可带 ?oldid=<修订号>）。不用 ?action=raw/render
	等接口、不用第三方镜像——渲染后的 HTML 才是读者看到的内容（模板、标签展开都在里面）。
	只用 Python 标准库（urllib + html.parser），不依赖 bs4/html2text。

	用法（在 nbtrw/ 下）：
	  python3 -I test/wiki2md.py Frosted_Ice                       # 打印到终端
	  python3 -I test/wiki2md.py -o test/work/wiki Bell "Point of Interest"
	                                                               # 存 <名>.html（原文）和 <名>.md
	  python3 -I test/wiki2md.py --section "Block states" Slab     # 只看某一节（含它的小节）
	  python3 -I test/wiki2md.py --toc Chunk_format                # 只看目录
	  python3 -I test/wiki2md.py --oldid 2900000 Frosted_Ice       # 指定修订（核对旧依据用）
	  python3 -I test/wiki2md.py --lang zh 霜冰                    # zh.minecraft.wiki
	  python3 -I test/wiki2md.py https://minecraft.wiki/w/Bell     # 直接给页面网址
	  python3 -I test/wiki2md.py test/work/wiki/Bell.html          # 转换本地存好的 HTML（不联网）
	  python3 -I test/wiki2md.py -f pages.txt -o test/work/wiki    # 文件里一行一个页面

	-o 时先看目录里有没有存好的 <名>.html：有就直接转换（不联网），--refresh 才重新抓。
	每个 .md 开头记下网址、修订号、页面最后修改时间、抓取时间，引用 wiki 依据时照抄修订号即可。
	默认不输出内链网址、图片、导航框（读起来是噪音），需要时加 --links / --images / --navbox。
"""
import argparse, datetime, gzip, html, os, re, sys, time, urllib.error, urllib.parse, urllib.request
from html.parser import HTMLParser

USER_AGENT = 'nbtrw-wiki2md/1.0 (blockporter maintenance; python-urllib)'
ALLOWED_QUERY = {'oldid', 'curid', 'title', 'uselang', 'variant'}

sys.setrecursionlimit(20000)


# ==================== 1. 极简 DOM ====================
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'}


class Node:
	__slots__ = ('tag', 'attrs', 'children', 'parent')

	def __init__(self, tag, attrs, parent):
		self.tag = tag
		self.attrs = attrs
		self.children = []
		self.parent = parent

	def cls(self):
		return set((self.attrs.get('class') or '').split())

	def get(self, k, d=''):
		v = self.attrs.get(k)
		return d if v is None else v


class Builder(HTMLParser):
	def __init__(self):
		super().__init__(convert_charrefs=True)
		self.root = Node('#root', {}, None)
		self.cur = self.root

	def handle_starttag(self, tag, attrs):
		n = Node(tag, dict(attrs), self.cur)
		self.cur.children.append(n)
		if tag not in VOID:
			self.cur = n

	def handle_startendtag(self, tag, attrs):
		self.cur.children.append(Node(tag, dict(attrs), self.cur))

	def handle_endtag(self, tag):
		n = self.cur
		while n is not None and n.tag != tag:
			n = n.parent
		if n is not None and n.parent is not None:
			self.cur = n.parent

	def handle_data(self, data):
		ch = self.cur.children
		if ch and isinstance(ch[-1], str):
			ch[-1] += data
		else:
			ch.append(data)


def parse_Html(text):
	b = Builder()
	b.feed(text)
	b.close()
	return b.root


def find_Node(n, pred):
	"""深度优先找第一个满足条件的节点（不用递归，页面很深也不怕）"""
	stack = [n]
	while stack:
		x = stack.pop()
		if not isinstance(x, Node):
			continue
		if pred(x):
			return x
		stack.extend(reversed(x.children))
	return None


def find_All(n, pred):
	out, stack = [], [n]
	while stack:
		x = stack.pop()
		if not isinstance(x, Node):
			continue
		if pred(x):
			out.append(x)
		stack.extend(reversed(x.children))
	return out


def get_Text(n):
	"""节点里的全部文字（不含 script/style），原样空白"""
	if isinstance(n, str):
		return n
	if n.tag in ('script', 'style'):
		return ''
	if n.tag == 'br':
		return '\n'
	return ''.join(get_Text(c) for c in n.children)


# ==================== 2. HTML → Markdown ====================
BLOCK = {'address', 'article', 'aside', 'blockquote', 'caption', 'center', 'dd', 'details', 'div', 'dl', 'dt',
	'fieldset', 'figcaption', 'figure', 'footer', 'form', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hr', 'li',
	'main', 'nav', 'ol', 'p', 'pre', 'section', 'summary', 'table', 'tbody', 'td', 'tfoot', 'th', 'thead', 'tr', 'ul'}
SKIP_TAG = {'script', 'style', 'link', 'meta', 'noscript', 'audio', 'video', 'source', 'track', 'button', 'input',
	'select', 'textarea', 'template', 'map', 'svg'}
SKIP_CLASS = {'mw-editsection', 'history-json', 'toc', 'mw-cite-backlink', 'navbar', 'navbox-navbar', 'navbar-mini',
	'mw-empty-elt', 'toctogglecheckbox', 'catlinks', 'printfooter', 'mw-jump-link', 'tabber__header'}
ZERO_WIDTH = dict.fromkeys(map(ord, '​‌‍⁠﻿'), None)
HIDDEN_STYLE = re.compile(r'display\s*:\s*none', re.I)
BOLD_STYLE = re.compile(r'font-weight\s*:\s*(bold|[6-9]00)', re.I)


def wrap_Mark(s, mark):
	"""给文字加 **/*/~~ 之类的标记：首尾空白挪到标记外面，空的不加"""
	if not s.strip():
		return s
	l = len(s) - len(s.lstrip())
	r = len(s) - len(s.rstrip())
	return s[:l] + mark + s.strip() + mark + s[len(s) - r:]


def clean_Lines(s):
	"""行内文字收尾：去零宽字符、压空白、每行去首尾空白、去掉首尾空行"""
	s = s.translate(ZERO_WIDTH).replace('\xa0', ' ')
	lines = [re.sub(r'[ \t]+', ' ', x).strip() for x in s.split('\n')]
	while lines and not lines[0]:
		lines.pop(0)
	while lines and not lines[-1]:
		lines.pop()
	out = []
	for x in lines:
		if x or (out and out[-1]):
			out.append(x)
	return '\n'.join(out)


class Converter:
	def __init__(self, base, links=False, images=False, navbox=False):
		self.base = base
		self.links = links
		self.images = images
		self.navbox = navbox
		self.in_table = 0

	# ---- 要不要跳过 ----
	def is_Skip(self, n):
		if n.tag in SKIP_TAG:
			return True
		c = n.cls()
		if c & SKIP_CLASS:
			return True
		if any(x.endswith('-json') for x in c):  # history-json、chestcontents-json：给页面脚本用的数据，浏览器里看不见
			return True
		if n.get('id') in ('toc', 'catlinks'):
			return True
		if HIDDEN_STYLE.search(n.get('style')):
			return True
		if not self.navbox and ('navbox' in c or 'navbox-top' in c):
			return True
		return False

	def is_Block(self, n):
		if n.tag in BLOCK:
			return True
		return n.tag == 'span' and 'tabber' in n.cls()

	# ---- 块 ----
	def render_Children(self, n):
		"""子节点 → 块列表；连着的行内内容合成一个段落"""
		blocks, inline = [], []

		def flush():
			if inline:
				t = clean_Lines(''.join(inline))
				if t:
					blocks.append(t)
				inline.clear()
		for ch in n.children:
			if isinstance(ch, str):
				inline.append(re.sub(r'\s+', ' ', ch))
			elif self.is_Skip(ch):
				continue
			elif self.is_Block(ch):
				flush()
				blocks.extend(self.render_Block(ch))
			else:
				inline.append(self.render_Inline(ch))
		flush()
		return blocks

	def render_Block(self, n):
		t = n.tag
		c = n.cls()
		if t in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6'):  # 标题只留纯文字：目录和 --section 按它匹配
			text = clean_Lines(''.join(self.render_Inline(x) if isinstance(x, Node) else x for x in n.children
				if not (isinstance(x, Node) and self.is_Skip(x)))).replace('\n', ' ')
			text = re.sub(r'\*\*|~~|(?<!\w)\*|\*(?!\w)', '', text).strip()
			return ['#' * int(t[1]) + ' ' + text] if text else []
		if t == 'hr':
			return ['---']
		if t in ('ul', 'ol'):
			return self.render_List(n)
		if t == 'dl':
			return self.render_Dl(n)
		if t == 'table':
			return self.render_Table(n)
		if t == 'pre':
			return [self.render_Pre(n)]
		if t == 'blockquote' or (c & {'hatnote', 'msgbox', 'notice'}):
			inner = self.render_Children(n)
			return [self.quote_Blocks(inner)] if inner else []
		if 'tabber' in c:
			return self.render_Tabber(n)
		if t == 'figure' or (c & {'thumb', 'thumbinner'}):
			cap = find_Node(n, lambda x: x.tag == 'figcaption' or 'thumbcaption' in x.cls())
			if cap is not None:
				text = clean_Lines(self.render_Inline(cap)).replace('\n', ' ')
				return ['*图：' + text + '*'] if text else []
			return []
		if 'mcwiki-header' in c or 'infobox-title' in c:
			text = clean_Lines(self.render_Inline(n)).replace('\n', ' ')
			return ['**' + text + '**'] if text else []
		return self.render_Children(n)

	def quote_Blocks(self, blocks):
		lines = '\n\n'.join(blocks).split('\n')
		return '\n'.join('> ' + x if x else '>' for x in lines)

	def render_Tabber(self, n):
		out = []
		tabs = [x for x in n.children if isinstance(x, Node) and (x.cls() & {'tabbertab', 'tabber__panel'})]
		if not tabs:  # 新版 tabber 的面板可能包在 tabber__section 里
			tabs = find_All(n, lambda x: bool(x.cls() & {'tabbertab', 'tabber__panel'}))
		for tab in tabs:
			title = tab.get('data-title') or tab.get('data-mw-tabber-title') or tab.get('title')
			body = self.render_Children(tab)
			if title and body:  # 只放图片的页签（信息框的图）去掉图以后是空的，标题也不要
				out.append('**【页签：' + clean_Lines(title) + '】**')
			out.extend(body)
		return out

	def render_Pre(self, n):
		text = get_Text(n).translate(ZERO_WIDTH).strip('\n')
		fence = '~~~' if '```' in text else '```'
		return fence + '\n' + text + '\n' + fence

	def render_List(self, n):
		ordered = n.tag == 'ol'
		try:
			i = int(n.get('start', '1') or 1)
		except ValueError:
			i = 1
		lines = []
		for li in n.children:
			if not isinstance(li, Node) or li.tag != 'li' or self.is_Skip(li):
				continue
			body = '\n'.join(self.render_Children(li))
			marker = '%d.' % i if ordered else '-'
			i += 1
			sub = body.split('\n') if body else ['']
			pad = ' ' * (len(marker) + 1)
			lines.append(marker + ' ' + sub[0])
			lines.extend(pad + x if x else '' for x in sub[1:])
		return ['\n'.join(lines)] if lines else []

	def render_Dl(self, n):
		lines = []
		for ch in n.children:
			if not isinstance(ch, Node) or self.is_Skip(ch):
				continue
			if ch.tag == 'dt':
				t = clean_Lines(self.render_Inline(ch)).replace('\n', ' ')
				if t:
					lines.append('**' + t + '**')
			elif ch.tag == 'dd':
				body = '\n'.join(self.render_Children(ch))
				if body:
					sub = body.split('\n')
					lines.append(': ' + sub[0])
					lines.extend('  ' + x if x else '' for x in sub[1:])
		return ['\n'.join(lines)] if lines else []

	# ---- 表格 ----
	def get_Rows(self, t):
		rows = []
		for ch in t.children:
			if not isinstance(ch, Node) or self.is_Skip(ch):
				continue
			if ch.tag == 'tr':
				rows.append(ch)
			elif ch.tag in ('thead', 'tbody', 'tfoot'):
				rows.extend(x for x in ch.children if isinstance(x, Node) and x.tag == 'tr' and not self.is_Skip(x))
		return rows

	def render_Cell(self, cell):
		self.in_table += 1
		try:
			blocks = self.render_Children(cell)
		finally:
			self.in_table -= 1
		s = '<br>'.join(b.replace('\n', '<br>') for b in blocks)
		return s.replace('|', '\\|').strip()

	def render_Table(self, t):
		trs = self.get_Rows(t)
		cap = next((x for x in t.children if isinstance(x, Node) and x.tag == 'caption'), None)
		caption = clean_Lines(self.render_Inline(cap)).replace('\n', ' ') if cap is not None else ''
		grid, heads, pend = [], [], {}  # pend[(行,列)] = rowspan 往下复制的文字
		for r, tr in enumerate(trs):
			row, head, c = [], [], 0
			cells = [x for x in tr.children if isinstance(x, Node) and x.tag in ('td', 'th') and not self.is_Skip(x)]
			for cell in cells:
				while (r, c) in pend:
					txt, h = pend.pop((r, c))
					row.append(txt)
					head.append(h)
					c += 1
				txt = self.render_Cell(cell)
				try:
					cs = max(1, min(100, int(cell.get('colspan', '1') or 1)))
					rs = max(1, min(1000, int(cell.get('rowspan', '1') or 1)))
				except ValueError:
					cs, rs = 1, 1
				is_h = cell.tag == 'th'
				for dc in range(cs):  # colspan：文字只放第一格；rowspan：每一行都复制一份，单看一行也完整
					v = txt if dc == 0 else ''
					row.append(v)
					head.append(is_h)
					for dr in range(1, rs):
						pend[(r + dr, c)] = (v, is_h)
					c += 1
			while (r, c) in pend:
				txt, h = pend.pop((r, c))
				row.append(txt)
				head.append(h)
				c += 1
			if any(x for x in row):
				grid.append(row)
				heads.append(head)
		if not grid:
			return []
		if self.in_table:  # 表里套的表：压成一行文字
			return ['; '.join(' / '.join(x for x in row if x) for row in grid)]
		out = []
		if caption:
			out.append('*' + caption + '*')
		# “左边一格 th、右边一格 td”的属性表（信息框之类）：写成列表好读
		if all(len(row) <= 2 for row in grid) and any(len(row) == 2 and h[0] and not h[1] for row, h in zip(grid, heads)):
			lines = []
			for row, h in zip(grid, heads):
				cells = [x for x in row if x]
				if len(row) == 2 and row[0] and row[1]:
					lines.append('- **' + row[0].replace('<br>', ' ') + '**：' + row[1])
				elif len(cells) == 1:
					lines.append('- **' + cells[0] + '**' if h[0] else '- ' + cells[0])
				elif cells:
					lines.append('- ' + ' / '.join(cells))
			out.append('\n'.join(lines))
			return out
		ncol = max(len(r) for r in grid)
		for row in grid:
			row.extend([''] * (ncol - len(row)))
		lines = ['| ' + ' | '.join(grid[0]) + ' |', '|' + ' --- |' * ncol]
		lines.extend('| ' + ' | '.join(row) + ' |' for row in grid[1:])
		out.append('\n'.join(lines))
		return out

	# ---- 行内 ----
	def render_Inline(self, n):
		if isinstance(n, str):
			return re.sub(r'\s+', ' ', n)
		if self.is_Skip(n):
			return ''
		t = n.tag
		c = n.cls()
		if t == 'br':
			return '\n'
		if t == 'img':
			return self.render_Img(n)
		if 'invslot' in c:  # 合成格：物品名在里面某个 span 的 title 上，空格子写 [ ]
			items = find_All(n, lambda x: 'invslot-item' in x.cls())
			names = []
			for it in items:
				tn = find_Node(it, lambda x: x.tag == 'span' and x.get('title'))
				names.append(tn.get('title') if tn is not None else clean_Lines(get_Text(it)))
			names = [x for x in names if x]
			return '[' + ', '.join(names) + ']' if names else '[ ]'
		if 'sprite-file' in c:  # 小图标：名字在旁边的 sprite-text 里；单独出现时看图标的说明 title
			tn = find_Node(n, lambda x: x.tag == 'span' and x.get('title') and x is not n)
			if tn is not None and not find_Node(n, lambda x: 'sprite-text' in x.cls()):
				return '[' + tn.get('title') + ']'
		if t in ('code', 'kbd', 'samp', 'tt', 'var'):
			s = get_Text(n).translate(ZERO_WIDTH).replace('\n', ' ')
			s = re.sub(r'\s+', ' ', s).strip()
			if not s:
				return ''
			tick = '``' if '`' in s else '`'
			return tick + s + tick
		inner = ''.join(self.render_Inline(x) if isinstance(x, Node) and not self.is_Block(x)
			else ('\n' + '\n'.join(self.render_Block(x)) + '\n' if isinstance(x, Node)
			else re.sub(r'\s+', ' ', x)) for x in n.children if not (isinstance(x, Node) and self.is_Skip(x)))
		if t in ('b', 'strong') or (t == 'span' and BOLD_STYLE.search(n.get('style'))):
			return wrap_Mark(inner, '**')
		if t in ('i', 'em', 'cite', 'dfn'):
			return wrap_Mark(inner, '*')
		if t in ('s', 'del', 'strike'):
			return wrap_Mark(inner, '~~')
		if t == 'a':
			return self.render_Link(n, inner)
		if t == 'q':
			return '“' + inner + '”'
		return inner

	def render_Link(self, n, inner):
		href = n.get('href')
		text = inner
		if not href or href.startswith('#') or 'mw-selflink' in n.cls():
			return text
		url = urllib.parse.urljoin(self.base, href)
		host = urllib.parse.urlsplit(url).hostname or ''
		external = 'external' in n.cls() or not (host == 'minecraft.wiki' or host.endswith('.minecraft.wiki'))
		if not text.strip():
			return text
		if external or self.links:
			if text.strip() == url:
				return url
			return '[' + text.strip() + '](' + url.replace(' ', '%20').replace(')', '%29') + ')'
		return text

	def render_Img(self, n):
		if not self.images:
			return ''
		alt = (n.get('alt') or '').strip()
		src = n.get('src')
		if not alt or not src:
			return ''
		return '![' + alt.replace(']', ')') + '](' + urllib.parse.urljoin(self.base, src) + ')'


# ==================== 3. 抓页面 ====================
def get_Site(lang):
	return 'https://minecraft.wiki' if not lang or lang == 'en' else 'https://%s.minecraft.wiki' % lang


def make_Url(page, lang, oldid):
	"""页面名/网址 → (网址, 文件名用的名字)；只认 minecraft.wiki 的 /w/ 页面，拒绝 action=raw 这类接口"""
	if re.match(r'https?://', page):
		u = urllib.parse.urlsplit(page)
		host = u.hostname or ''
		if u.scheme != 'https' or not (host == 'minecraft.wiki' or host.endswith('.minecraft.wiki')):
			raise ValueError('只抓 https://minecraft.wiki（或它的语言子域名）的页面：%s' % page)
		q = urllib.parse.parse_qs(u.query)
		bad = set(q) - ALLOWED_QUERY
		if bad:
			raise ValueError('网址里有不认的参数 %s（只读正常页面，不用 action=raw 之类的接口）' % ','.join(sorted(bad)))
		if u.path.startswith('/w/'):
			title = urllib.parse.unquote(u.path[3:])
		elif 'title' in q:
			title = q['title'][0]
		else:
			raise ValueError('看不出页面名：%s' % page)
		sub = host.split('.')[0] if host != 'minecraft.wiki' else 'en'
		old = (q.get('oldid') or [oldid])[0]
		return page, make_Name(title, sub, old)
	title = page.strip().replace(' ', '_')
	if not title:
		raise ValueError('页面名是空的')
	url = get_Site(lang) + '/w/' + urllib.parse.quote(title, safe="/:()!,'*@$;=+~-._")
	if oldid:
		url += '?oldid=' + str(int(oldid))
	return url, make_Name(title, lang or 'en', oldid)


def make_Name(title, lang, oldid):
	name = title.replace(' ', '_').replace('/', '%2F').replace('\\', '%5C')
	if lang and lang != 'en':
		name = lang + '_' + name
	if oldid:
		name += '@' + str(oldid)
	return name


def fetch_Html(url, retries=4):
	req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': 'text/html',
		'Accept-Encoding': 'gzip', 'Accept-Language': 'en'})
	wait = 2.0
	for k in range(retries):
		try:
			with urllib.request.urlopen(req, timeout=30) as r:
				data = r.read()
				if r.headers.get('Content-Encoding') == 'gzip':
					data = gzip.decompress(data)
				return data.decode(r.headers.get_content_charset() or 'utf-8', 'replace')
		except urllib.error.HTTPError as e:
			if e.code == 404:
				raise RuntimeError('页面不存在（404）：%s' % url)
			if e.code not in (429, 500, 502, 503, 504) or k == retries - 1:
				raise RuntimeError('HTTP %d：%s' % (e.code, url))
			ra = e.headers.get('Retry-After')
			time.sleep(min(60.0, float(ra)) if ra and ra.isdigit() else wait)
		except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
			if k == retries - 1:
				raise RuntimeError('连不上（%s）：%s' % (e, url))
			time.sleep(wait)
		wait *= 2
	raise RuntimeError('抓取失败：%s' % url)


# ==================== 4. 整页 ====================
def convert_Page(text, url, opts):
	root = parse_Html(text)
	body = find_Node(root, lambda x: x.get('id') == 'mw-content-text')
	content = find_Node(body or root, lambda x: 'mw-parser-output' in x.cls())
	if content is None:  # 修订号不存在之类：wiki 回的是错误页，把它的原话报出来
		msg = clean_Lines(get_Text(body)).split('\n')[0][:200] if body is not None else ''
		raise RuntimeError('页面里没有正文' + ('：' + msg if msg else '（不像 wiki 页面）'))
	h1 = find_Node(root, lambda x: x.get('id') == 'firstHeading')
	title = clean_Lines(get_Text(h1)).replace('\n', ' ') if h1 is not None else ''
	canon = re.search(r'<link rel="canonical" href="([^"]+)"', text)
	canon = html.unescape(canon.group(1)) if canon else url
	rev = re.search(r'"wgRevisionId":(\d+)', text)
	cur = re.search(r'"wgCurRevisionId":(\d+)', text)
	lastmod = find_Node(root, lambda x: x.get('id') == 'footer-info-lastmod')
	lastmod = clean_Lines(get_Text(lastmod)).replace('\n', ' ') if lastmod is not None else ''
	redir = find_Node(root, lambda x: 'mw-redirectedfrom' in x.cls())
	redir = clean_Lines(get_Text(redir)).replace('\n', ' ') if redir is not None else ''
	conv = Converter(canon, links=opts.links, images=opts.images, navbox=opts.navbox)
	blocks = conv.render_Children(content)
	src = canon
	if rev and cur and cur.group(1) != rev.group(1) and 'oldid=' not in src:
		src += ('&' if '?' in src else '?') + 'oldid=' + rev.group(1)
	meta = ['来源：' + src]
	if rev:
		r = '修订：' + rev.group(1)
		if cur and cur.group(1) != rev.group(1):
			r += '（不是最新，最新修订 ' + cur.group(1) + '）'
		meta.append(r)
	if lastmod:
		meta.append(lastmod)
	if redir:
		meta.append(redir)
	meta.append('转换：test/wiki2md.py，' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d %H:%M UTC'))
	head = ['# ' + title, '\n'.join('> ' + x + ('  ' if i + 1 < len(meta) else '') for i, x in enumerate(meta))]
	cats = find_Node(root, lambda x: x.get('id') == 'mw-normal-catlinks')
	tail = []
	if cats is not None:
		names = [clean_Lines(get_Text(a)) for a in find_All(cats, lambda x: x.tag == 'a')][1:]
		if names:
			tail = ['---', '分类：' + ' · '.join(names)]
	md = '\n\n'.join(head + drop_EmptySections(blocks) + tail) + '\n'
	md = re.sub(r'\n{3,}', '\n\n', md)
	return md


def drop_EmptySections(blocks):
	"""去掉没有内容的节（比如导航框跳过以后只剩一个 Navigation 标题）；小节有内容就算有内容"""
	level = [len(m.group(1)) if m else 0 for m in (re.match(r'(#{1,6}) ', b) for b in blocks)]
	keep = []
	for i, b in enumerate(blocks):
		if level[i]:
			has = False
			for k in range(i + 1, len(blocks)):
				if level[k] and level[k] <= level[i]:
					break
				if not level[k]:
					has = True
					break
			if not has:
				continue
		keep.append(b)
	return keep


def split_Sections(md):
	"""Markdown → [(级别, 标题, 起始行, 结束行)]；代码块里的 # 不算标题"""
	lines = md.split('\n')
	heads, fence = [], None
	for i, x in enumerate(lines):
		m = re.match(r'(```|~~~)', x)
		if m:
			fence = None if fence == m.group(1) else (fence or m.group(1))
			continue
		if fence:
			continue
		m = re.match(r'(#{1,6}) (.*)', x)
		if m:
			heads.append([len(m.group(1)), m.group(2).strip(), i, len(lines)])
	for k, h in enumerate(heads):
		for h2 in heads[k + 1:]:
			if h2[0] <= h[0]:
				h[3] = h2[2]
				break
	return lines, heads


def norm_Title(s):
	return re.sub(r'[\s_]+', ' ', s).strip().casefold()


def pick_Sections(md, names):
	lines, heads = split_Sections(md)
	want = [norm_Title(x) for x in names]
	got, seen = [], set()
	for h in heads:
		if norm_Title(h[1]) in want and h[2] not in seen:
			got.append('\n'.join(lines[h[2]:h[3]]).rstrip())
			seen.update(range(h[2], h[3]))
	if not got:
		avail = ', '.join(h[1] for h in heads if h[0] >= 2)
		raise RuntimeError('没有这一节：%s；有的节：%s' % (' / '.join(names), avail))
	header = '\n\n'.join(md.split('\n\n')[:2])  # convert_Page 输出的头两块：“# 标题”和来源信息
	return header + '\n\n' + '\n\n'.join(got) + '\n'


def make_Toc(md):
	lines, heads = split_Sections(md)
	out = [lines[0]] if lines else []
	out += ['  ' * (h[0] - 2) + '- ' + h[1] for h in heads if h[0] >= 2]
	return '\n'.join(out) + '\n'


def run_One(page, opts):
	if os.path.isfile(page) and page.endswith(('.html', '.htm')):  # 本地存好的 HTML
		text = open(page, encoding='utf-8').read()
		url = 'file://' + os.path.abspath(page)
		name = os.path.splitext(os.path.basename(page))[0]
		out_dir = opts.out
	else:
		url, name = make_Url(page, opts.lang, opts.oldid)
		out_dir = opts.out
		cache = os.path.join(out_dir, name + '.html') if out_dir else None
		if cache and os.path.isfile(cache) and not opts.refresh:
			text = open(cache, encoding='utf-8').read()
			print('wiki2md: 用存好的 %s（要重新抓加 --refresh）' % cache, file=sys.stderr)
		else:
			if opts.fetched:
				time.sleep(opts.delay)
			text = fetch_Html(url)
			opts.fetched += 1
			if cache:
				os.makedirs(out_dir, exist_ok=True)
				with open(cache + '.tmp', 'w', encoding='utf-8') as f:
					f.write(text)
				os.replace(cache + '.tmp', cache)
	md = convert_Page(text, url, opts)
	if opts.section:
		return pick_Sections(md, opts.section), None
	if opts.toc:
		return make_Toc(md), None
	if out_dir:
		os.makedirs(out_dir, exist_ok=True)
		path = os.path.join(out_dir, name + '.md')
		with open(path + '.tmp', 'w', encoding='utf-8') as f:
			f.write(md)
		os.replace(path + '.tmp', path)
		return None, path
	return md, None


def main():
	ap = argparse.ArgumentParser(description='把 minecraft.wiki 页面的 HTML 转成 Markdown（只抓正常页面，不用 action=raw）')
	ap.add_argument('pages', nargs='*', help='页面名（Frosted_Ice / "Point of Interest"）、页面网址、或本地 .html 文件')
	ap.add_argument('-f', '--file', help='页面列表文件：一行一个，# 开头是注释')
	ap.add_argument('-o', '--out', help='输出目录：存 <名>.html（原文）和 <名>.md；不给就打印到终端')
	ap.add_argument('--refresh', action='store_true', help='-o 目录里已有 HTML 也重新抓')
	ap.add_argument('--lang', default='en', help='语言子站，如 zh（默认 en＝minecraft.wiki）')
	ap.add_argument('--oldid', help='指定修订号（只对页面名生效）')
	ap.add_argument('--section', action='append', help='只输出这一节和它的小节（可给多次），打印到终端')
	ap.add_argument('--toc', action='store_true', help='只输出目录，打印到终端')
	ap.add_argument('--links', action='store_true', help='保留站内链接网址（站外链接总是保留）')
	ap.add_argument('--images', action='store_true', help='保留图片（默认去掉，小图标本来就会换成文字）')
	ap.add_argument('--navbox', action='store_true', help='保留页面底部的导航框')
	ap.add_argument('--delay', type=float, default=1.0, help='连续抓多个页面时每两次之间等几秒（默认1）')
	opts = ap.parse_args()
	pages = list(opts.pages)
	if opts.file:
		with open(opts.file, encoding='utf-8') as f:
			pages += [x.strip() for x in f if x.strip() and not x.lstrip().startswith('#')]
	if not pages:
		ap.print_help()
		return 2
	opts.fetched = 0
	fail = 0
	printed = 0
	for p in pages:
		try:
			md, path = run_One(p, opts)
		except (RuntimeError, ValueError, OSError) as e:
			print('wiki2md: %s：%s' % (p, e), file=sys.stderr)
			fail += 1
			continue
		if path:
			print(path)
		else:
			if printed:
				sys.stdout.write('\n---\n\n')
			sys.stdout.write(md)
			printed += 1
	return 1 if fail else 0


if __name__ == '__main__':
	sys.exit(main())
