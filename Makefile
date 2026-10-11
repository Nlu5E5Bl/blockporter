# nbtrw的编译：底层库libnbt.so + 规则插件rules.so.26.1 + 三个主程序(merge/fixer/prune) + 库的demo(nbtrw)
#	统一用c++23（仓库里那几个老的.c文件要编的话用c23：gcc -std=c23 …）
#	全都带-fPIC；程序/插件用 -Wl,-rpath,'$ORIGIN' 找同目录下的libnbt.so，整个目录拷到哪都能跑
#	规则单独一个.so：改规则只要 make rules（重编一个.so就行），merge/fixer/nbtrw都不用动
#	规则文件名带版本（rules.so.26.1），rules.so 是指向它的符号链接：换26.2的规则时，
#	  编出 rules.so.26.2 再把链接指过去就行，fixer里认的名字始终是 rules.so
#	用法：make / make clean / make rules
CXX=g++
CXXFLAGS=-std=c++23 -O2 -Wall -Wextra -pedantic -fPIC
LDFLAGS=-L. -Wl,-rpath,'$$ORIGIN'
LIBS=-lnbt -lz

RULES_VER=26.1
LIB_SRC=nbt.cpp block.cpp chunk.cpp mca.cpp level.cpp

all: libnbt.so rules merge fixer prune nbtrw

libnbt.so: $(LIB_SRC) nbt.hpp
	$(CXX) $(CXXFLAGS) -shared $(LIB_SRC) -o $@ -lz

rules: rules.so.$(RULES_VER)
	ln -sf rules.so.$(RULES_VER) rules.so

rules.so.$(RULES_VER): rules.cpp rules.hpp nbt.hpp libnbt.so
	$(CXX) $(CXXFLAGS) -shared rules.cpp -o $@ $(LDFLAGS) $(LIBS)

merge: merge.cpp nbt.hpp libnbt.so
	$(CXX) $(CXXFLAGS) merge.cpp -o $@ $(LDFLAGS) $(LIBS)

fixer: fixer.cpp rules.hpp nbt.hpp libnbt.so
	$(CXX) $(CXXFLAGS) fixer.cpp -o $@ $(LDFLAGS) $(LIBS) -ldl

prune: prune.cpp nbt.hpp libnbt.so
	$(CXX) $(CXXFLAGS) prune.cpp -o $@ $(LDFLAGS) $(LIBS)

nbtrw: nbtrw.cpp nbt.hpp libnbt.so
	$(CXX) $(CXXFLAGS) nbtrw.cpp -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f libnbt.so rules.so rules.so.$(RULES_VER) merge fixer prune nbtrw
.PHONY: all clean rules
