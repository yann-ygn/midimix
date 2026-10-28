# Device tools for the MPC Key 37 (armv7, Cortex-A17), built with Zig from PyPI: no cross toolchain or Docker.
#   make -C mpc arm      -> mpc/build/arm/seqtool, mpcstate, midimixd (static musl)
#   make -C mpc deploy   -> copies them to /tmp on the MPC (tmpfs: gone after a reboot)
#   make -C mpc install  -> installs midimixd for good: systemd, /data/midimixd (midimixd/install.sh)
#   make -C mpc uninstall

ZIG ?= uvx --from ziglang==0.13.0 python -m ziglang
MPC ?= root@mpc-key-37.lan.arpa
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

TOOLS = seqtool mpcstate midimixd

# Sources besides <tool>/<tool>.c
mpcstate_SRC = mpcread/mpcread.c
midimixd_SRC = midimixd/api.c midimixd/json.c

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

clean:
	rm -rf build

.PHONY: arm deploy install uninstall clean
