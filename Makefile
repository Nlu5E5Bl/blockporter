# nbtrw 的编译
#	make / make all   全编：nbtrw（libnbtrw.so + demo）和 blockporter（规则插件 + merge + fixer）
#	make nbtrw        只编 nbtrw：底层库 libnbtrw.so + demo 程序 nbtrw
#	make blockporter  只编 blockporter：规则插件 + merge + fixer（库没编会先编库）
#	make rules        只重编规则插件：每个 rules.cpp.<版本> 编出一个 rules.so.<版本>（不链 rules.so；
#	                  fixer 的最后一个参数给插件路径，不给就找当前目录的 ./rules.so，要用哪个自己链/拷）
#	make rules.so.26.3  只编某一个版本的
#	make clear        清掉上面这些编译产物（不碰 test/work/）
#	统一用 c++23；全都带 -fPIC；程序用 -Wl,-rpath,'$ORIGIN' 找同目录下的 libnbtrw.so
CXX=g++
CXXFLAGS=-std=c++23 -O2 -Wall -Wextra -pedantic -fPIC
LDFLAGS=-L. -Wl,-rpath,'$$ORIGIN'
LIBS=-lnbtrw -lz

#库自己要的压缩库（lz4-devel、xxhash-devel）；程序只链 libnbtrw，用不着它们
LIB_LIBS=-lz -llz4 -lxxhash

#规则插件：一个版本一个源文件 rules.cpp.<版本>，通配出 rules.so.<版本>
RULES_SRC=$(wildcard rules.cpp.*)
RULES_SO=$(patsubst rules.cpp.%,rules.so.%,$(RULES_SRC))
LIB_SRC=nbt.cpp atom.cpp chunk.cpp mca.cpp level.cpp

all: nbtrw blockporter

#---- nbtrw：库 + demo（目标 nbtrw 就是 demo 程序，它依赖库） ----
libnbtrw.so: $(LIB_SRC) nbtrw.hpp
	$(CXX) $(CXXFLAGS) -shared $(LIB_SRC) -o $@ $(LIB_LIBS)

nbtrw: nbtrw.cpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) nbtrw.cpp -o $@ $(LDFLAGS) $(LIBS)

#---- blockporter：merge / fixer / 规则插件 ----
blockporter: rules merge fixer

rules: $(RULES_SO)

#源文件的扩展名是版本号，要用 -x c++ 告诉编译器这是 C++（后面的 -x none 把它关掉，免得影响别的输入）
rules.so.%: rules.cpp.% rules.hpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) -shared -x c++ $< -x none -o $@ $(LDFLAGS) $(LIBS)

merge: merge.cpp blockporter.hpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) merge.cpp -o $@ $(LDFLAGS) $(LIBS)

fixer: fixer.cpp blockporter.hpp rules.hpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) fixer.cpp -o $@ $(LDFLAGS) $(LIBS) -ldl

clear:
	rm -f libnbtrw.so nbtrw merge fixer rules.so rules.so.*

.PHONY: all blockporter rules clear
