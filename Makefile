# Compatibility shim - the real build system is Ninja.
# Run ./configure first, then use ninja directly for full control.

.PHONY: all clean distclean

all:
	@ninja

clean:
	@ninja -t clean

distclean:
	rm -f build.ninja
	rm -rf build/ bin/
