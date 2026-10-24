# nbtrw 的编译：底层库 libnbtrw.so + demo(nbtrw)
#	blockporter（merge/fixer/rules）默认不编；要编用 make blockporter
#	prune 已并进 fixer，不再单独编
#	统一用c++23（仓库里那几个老的.c文件要编的话用c23：gcc -std=c23 …）
#	全都带-fPIC；程序用 -Wl,-rpath,'$ORIGIN' 找同目录下的 libnbtrw.so
#	用法：make / make clean / make blockporter
CXX=g++
CXXFLAGS=-std=c++23 -O2 -Wall -Wextra -pedantic -fPIC
LDFLAGS=-L. -Wl,-rpath,'$$ORIGIN'
LIBS=-lnbtrw -lz

RULES_VER=26.1.1
LIB_SRC=nbt.cpp block.cpp chunk.cpp mca.cpp level.cpp

all: libnbtrw.so nbtrw

libnbtrw.so: $(LIB_SRC) nbtrw.hpp
	$(CXX) $(CXXFLAGS) -shared $(LIB_SRC) -o $@ -lz

nbtrw: nbtrw.cpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) nbtrw.cpp -o $@ $(LDFLAGS) $(LIBS)

#---- blockporter（merge/fixer/rules；prune 已并进 fixer） ----
blockporter: rules merge fixer

rules: rules.so.$(RULES_VER)
	ln -sf rules.so.$(RULES_VER) rules.so

rules.so.$(RULES_VER): rules.cpp rules.hpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) -shared rules.cpp -o $@ $(LDFLAGS) $(LIBS)

merge: merge.cpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) merge.cpp -o $@ $(LDFLAGS) $(LIBS)

fixer: fixer.cpp rules.hpp nbtrw.hpp libnbtrw.so
	$(CXX) $(CXXFLAGS) fixer.cpp -o $@ $(LDFLAGS) $(LIBS) -ldl

clean:
	rm -f libnbtrw.so libnbt.so nbtrw merge fixer rules.so rules.so.* *.o

.PHONY: all blockporter clean rules
