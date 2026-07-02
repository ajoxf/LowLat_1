# Makefile for building the strategy against the REAL iRage ConnectLib on the
# iRage production box (RHEL 9.4). Mirrors the structure in the API v3.4 doc,
# section 3.1 -- adjust LIB_VER and paths to the library version installed on
# your box (ask iRage which version is deployed).
#
# NOTE: local development/testing in this repo does NOT use this Makefile; it
# builds mystrat.cpp against irage/mock/ConnectHeader.h via CMake (target
# irage_strat + test_irage_strat).

LIB_VER  := v3.4
LIB_DIR  := /usr/local/lib/$(LIB_VER)/nse/
INC_DIR  := /usr/local/include/$(LIB_VER)/nse/

LIBS := -L/usr/gcc-7.1/lib64/ -L$(LIB_DIR) -lirz_connect.nse.unix.$(LIB_VER) -lpciaccess \
        -lpthread -ldl -lrt -lonload_zf
CXX  := /usr/gcc-7.1/bin/g++ -std=c++1z
# -I.. lets mystrat.cpp include "strategy/quoting_math.hpp" from this repo's
# src/ tree (copy src/strategy/quoting_math.hpp alongside if deploying only
# the irage/ directory).
INCS := -I$(INC_DIR) -I$(INC_DIR)logger/include/ -I../src -I.
SRCS := main.cpp mystrat.cpp

# For BSE and MCX add -D_ALPHA_SYMTYPE64_
CXXFLAGS := -O2 -g -ggdb3 -D_MTICK_LTP_ -D_MTICK_LTQ_ -D_GLIBCXX_USE_CXX11_ABI=0 \
            -D_USE_CUSTOM_LOGGER_V2_ -D_MUTLI_
OUT_EXE  := my_strat_binary
SRCS_EXE := ${SRCS}

OBJS     := $(addsuffix .o,$(basename ${SRCS}))
OBJS_EXE := $(addsuffix .o,$(basename ${SRCS_EXE}))

${OUT_EXE}: ${OBJS_EXE}
	g++ -g -o $@ ${OBJS_EXE} ${LIBS}

%.o : %.cpp
	${CXX} -c ${CXXFLAGS} ${MAIN_DEF} ${INCS} $< -o $@

clean:
	rm -f core ${OBJS_EXE} ${OUT_EXE}
