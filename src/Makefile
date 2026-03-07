# Define phony targets so Make doesn't look for files named "all" or "clean"
.PHONY: all clean run

# Default target: pass 'make' down to the build directory
all:
	@$(MAKE) -C build

# Pass 'make clean' down to the build directory
clean:
	@$(MAKE) -C build clean

# Bonus shortcut: compile and immediately run the seeder
run: all
	./build/btclient test-file.torrent -p 51413 -i 10.20.107.50