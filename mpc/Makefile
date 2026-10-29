# Device tools for the MPC Key 37 (armv7, Cortex-A17), built with Zig from PyPI: no cross toolchain or Docker.
#   make -C mpc arm      -> mpc/build/arm/seqtool, mpcstate, midimixd, midimixctl (static musl)
#   make -C mpc deploy   -> copies them to /tmp on the MPC (tmpfs: gone after a reboot)
#   make -C mpc install  -> installs midimixd for good: systemd, /data/midimixd (midimixd/install.sh)
#   make -C mpc uninstall
#   make -C mpc plugin-host  -> build/plugin-host: the editor plugin in a stand-in host, with ASan (tests/test_plugin.py)
#   make -C mpc plugin       -> plugin/build/plugin/ygn - VST - Midimix Editor/: the editor plugin's folder for the MPC
#                               (armv7, glibc <= 2.31), with its skin (mpc-vst-plugins' generator, drawn by the browser
#                               renderer on the system Chromium: plugin/html_art_local; needs `git submodule update --init`)

ZIG ?= uvx --from ziglang==0.13.0 python -m ziglang
MPC ?= root@mpc-key-37.lan.arpa
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

TOOLS = seqtool mpcstate midimixd midimixctl

# Sources besides <tool>/<tool>.c
mpcstate_SRC = mpcread/mpcread.c
midimixd_SRC = midimixd/api.c midimixd/json.c midimixd/ctl.c midimixd/map.c

arm: $(TOOLS:%=build/arm/%)

.SECONDEXPANSION:
build/arm/%: $$*/$$*.c $$($$*_SRC) $(wildcard */*.h)
	@mkdir -p $(@D)
	$(ZIG) cc -target arm-linux-musleabihf -mcpu=cortex_a17 -static $(CFLAGS) -o $@ $(filter %.c,$^)

deploy: arm
	for t in $(TOOLS); do ssh $(MPC) "cat > /tmp/$$t.new && chmod +x /tmp/$$t.new && mv -f /tmp/$$t.new /tmp/$$t" < build/arm/$$t || exit 1; done

INSTALL_FILES = install.sh uninstall.sh midimixd.service acvs-midimixd.conf

install: build/arm/midimixd $(INSTALL_FILES:%=midimixd/%)
	tar -cf - -C build/arm midimixd -C ../../midimixd $(INSTALL_FILES) | ssh $(MPC) \
	    'rm -rf /tmp/midimixd-install && mkdir /tmp/midimixd-install && cd /tmp/midimixd-install && tar -xf - && sh install.sh; s=$$?; rm -rf /tmp/midimixd-install; exit $$s'

uninstall:
	ssh $(MPC) 'sh /data/midimixd/uninstall.sh'

# The editor plugin (plugin/): a silent instrument on mpc-vst-plugins' VST2 wrapper, as torpedo's.
MV = mpc-vst-plugins
PLUGIN_SRC = plugin/engine.c plugin/mmd.c midimixd/map.c $(MV)/wrapper/vst2_wrap.c
PLUGIN_DEPS = $(PLUGIN_SRC) plugin/mmd.h midimixd/map.h midimixd/ctl.h plugin/build/params.h
PLUGIN_FLAGS = -std=gnu11 -Imidimixd -Iplugin -Iplugin/build -I$(MV)/wrapper
PLUGIN_DIR = plugin/build/plugin/ygn - VST - Midimix Editor
GEN_VST = SHADOW_ART=$(CURDIR)/plugin/html_art_local uv run --with pillow python $(MV)/tools/gen_vst.py plugin/vst.json

plugin/build/params.h: plugin/vst.json plugin/params.json plugin/layout.conf
	$(GEN_VST) --params-h

build/plugin-host: plugin/test_host.c $(PLUGIN_DEPS)
	@mkdir -p $(@D)
	$(CC) -O1 -g -Wall -Wextra -Wno-unused-parameter $(PLUGIN_FLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ plugin/test_host.c $(PLUGIN_SRC) -lpthread -lm

plugin-host: build/plugin-host

plugin/build/midimix_editor.so: $(PLUGIN_DEPS)
	$(ZIG) cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a17 -O2 -Wall -Wextra -Wno-unused-parameter -fPIC -shared \
		-fvisibility=hidden -s $(PLUGIN_FLAGS) -o $@ $(PLUGIN_SRC) -lpthread -lm -Wl,--no-undefined
	@echo "highest glibc: $$(readelf -V $@ | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
	@echo "exported: $$(readelf --dyn-syms -W $@ | grep -E ' GLOBAL .* [0-9]+ [A-Za-z]' | grep -v UND | awk '{print $$8}')"

plugin: plugin/build/midimix_editor.so plugin/images/*.svg
	$(GEN_VST)
	rm -rf plugin/build/plugin && mkdir -p plugin/build/plugin
	cp -r plugin/build/skin/* plugin/build/plugin/
	cp plugin/build/midimix_editor.so "$(PLUGIN_DIR)/"
	@# the plugin-list entry with the Synths folder left as %payload-path%: mpc-store.sh sync registers a folder from it
	sed 's|file="/sdcard/Synths/|file="%payload-path%/|' plugin/build/pluginlist-entry.xml > "$(PLUGIN_DIR)/plugin-meta.xml"

clean:
	rm -rf build plugin/build

.PHONY: arm deploy install uninstall clean plugin-host plugin
