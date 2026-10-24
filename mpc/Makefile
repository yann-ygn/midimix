# Device tools for the MPC Key 37 (armv7, Cortex-A17), built with Zig from PyPI: no cross toolchain or Docker.
#   make -C mpc arm      -> mpc/build/arm/seqtool, mpcstate (static musl)
#   make -C mpc deploy   -> copies them to /tmp on the MPC (tmpfs: gone after a reboot)

ZIG ?= uvx --from ziglang==0.13.0 python -m ziglang
MPC ?= root@mpc-key-37.lan.arpa
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L

TOOLS = seqtool mpcstate

arm: $(TOOLS:%=build/arm/%)

.SECONDEXPANSION:
build/arm/%: $$*/$$*.c
	@mkdir -p $(@D)
	$(ZIG) cc -target arm-linux-musleabihf -mcpu=cortex_a17 -static $(CFLAGS) -o $@ $<

deploy: arm
	for t in $(TOOLS); do ssh $(MPC) "cat > /tmp/$$t && chmod +x /tmp/$$t" < build/arm/$$t || exit 1; done

clean:
	rm -rf build

.PHONY: arm deploy clean
