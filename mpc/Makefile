# Device tools for the MPC Key 37 (armv7, Cortex-A17), built with Zig from PyPI: no cross toolchain or Docker.
#   make -C mpc arm      -> mpc/build/arm/seqtool (static musl)
#   make -C mpc deploy   -> copies it to /tmp on the MPC (tmpfs: gone after a reboot)

ZIG ?= uvx --from ziglang==0.13.0 python -m ziglang
MPC ?= root@mpc-key-37.lan.arpa
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

arm: build/arm/seqtool

build/arm/seqtool: seqtool/seqtool.c
	@mkdir -p $(@D)
	$(ZIG) cc -target arm-linux-musleabihf -mcpu=cortex_a17 -static $(CFLAGS) -o $@ $<

deploy: build/arm/seqtool
	ssh $(MPC) 'cat > /tmp/seqtool && chmod +x /tmp/seqtool' < build/arm/seqtool

clean:
	rm -rf build

.PHONY: arm deploy clean
