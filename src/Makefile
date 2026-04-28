# Define phony targets so Make doesn't look for files named "all" or "clean"
.PHONY: all clean run

# Default target: pass 'make' down to the build directory
all:
	@$(MAKE) -C build

# Pass 'make clean' down to the build directory
clean:
	@$(MAKE) -C build clean

# Bonus shortcut: compile and run the probe against the bundled torrent file
run: all
	./build/btclient -f big.torrent -t 30 -p 51413
