CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra
PLUGIN := wlxmpv.wlx
INSTALL_DIR := $(HOME)/.config/doublecmd/plugins/wlx/wlxmpv

QT_CFLAGS := $(shell pkg-config --cflags Qt5Widgets)
QT_LIBS := $(shell pkg-config --libs Qt5Widgets)
MPV_CFLAGS := $(shell pkg-config --cflags mpv)

$(PLUGIN): wlxmpv.cpp
	$(CXX) $(CXXFLAGS) -std=c++17 -shared -fPIC -pthread -fvisibility=hidden -Wl,--no-undefined \
		$(QT_CFLAGS) $(MPV_CFLAGS) -o $@ $< $(QT_LIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(INSTALL_DIR)/$(PLUGIN)

clean:
	rm -f $(PLUGIN)

.PHONY: install clean
