CXX      ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
FT_CFLAGS := $(shell $(PKG_CONFIG) --cflags freetype2 libpng 2>/dev/null || echo "-I/usr/include/freetype2")
FT_LIBS   := $(shell $(PKG_CONFIG) --libs freetype2 libpng 2>/dev/null || echo "-lfreetype -lpng")
LDFLAGS  += $(FT_LIBS) -lutil -lpthread

SRCS = main.cpp fb.cpp input.cpp pm3_session.cpp ui.cpp
OBJS = $(patsubst %.cpp,../build/obj/%.o,$(SRCS))
TARGET = ../build/czpm3-gui

all: $(TARGET)

../build/obj/%.o: %.cpp *.hpp
	@mkdir -p ../build/obj
	$(CXX) $(CXXFLAGS) $(FT_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@mkdir -p ../build
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf ../build/obj $(TARGET)

.PHONY: all clean
