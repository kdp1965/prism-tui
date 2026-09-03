PROJECT_NAME ?= prism_tui

# runtime.c is the LOCAL (ttsky25a) copy of the SDK runtime: it carries
# the host-filesystem syscall glue (weak __tinyqv_fs_* hooks + fd>=3
# routing in _open/_read/_write) that upstream tinyQV-sdk lacks.  Linking
# it here keeps the submodule pristine - our runtime.o resolves the
# syscalls first, so the archive's runtime.o is never pulled.
PROJECT_SOURCES ?= main.c console.c play.c flashy.c synth.c synth_demos.c \
                   midi.c chroma_dutymeter.c cpptest.cpp cxxrt.cpp prism_isr.s \
                   leddemo.c runtime.c tqv_fs.c

# Ported ncurses TUI stack (see tui/): termcurses VT100 backend over the
# UART, pdcurses 3.4 core, bare-metal platform layer, and the CTui/CPrism
# application classes.  Sources build with tui/include compat headers
# standing in for the NuttX ones.
TUI_SOURCES = $(wildcard tui/*.c) \
              $(wildcard tui/termcurses/*.c) \
              $(wildcard tui/pdcurses/*.c) \
              $(wildcard tui/platform/*.c) \
              $(wildcard tui/*.cxx)
PROJECT_SOURCES += $(TUI_SOURCES)

# Firmware C/C++/asm sources live in src/; VPATH lets PROJECT_SOURCES
# stay as bare names and objects stay flat in obj/.
VPATH = src

RISCV_TOOLCHAIN ?= /opt/tinyQV

CC = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-gcc
CXX = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-g++
AS = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-as
AR = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-ar
LD = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-ld
OBJCOPY = $(RISCV_TOOLCHAIN)/bin/riscv32-unknown-elf-objcopy

TINYQV_SDK ?= tinyQV-sdk

# Objects are built into (and linked from) OBJDIR.  -MMD -MP emits header
# dependency files alongside them so edits to prism_tui.h / synth.h
# rebuild whatever includes them.
OBJDIR ?= obj
PROJECT_OBJS = $(addprefix $(OBJDIR)/,$(patsubst %.s,%.o,$(patsubst %.cxx,%.o,$(patsubst %.cpp,%.o,$(PROJECT_SOURCES:.c=.o)))))

# FULL_SONG keeps the complete DSM demo track in the build (see the
# ifdef in sharp_dressed_man.c).  On by default; 'make FULL_SONG=0'
# builds the small fast-flash image for debug cycles.
FULL_SONG ?= 0
ifneq ($(FULL_SONG),0)
SONG_FLAGS = -DFULL_SONG
endif
CFLAGS = -O2 $(SONG_FLAGS) -Isrc -I$(TINYQV_SDK) -march=rv32ec_zicsr_zcb_zicond_zilsd -mabi=ilp32e -mno-strict-align -nostdlib -nostartfiles -ffreestanding -ffunction-sections -fdata-sections -Wall -Werror -MMD -MP

all: $(PROJECT_NAME).bin $(PROJECT_NAME).hex chromas

.PHONY: all clean chromas

clean:
	@rm -rf $(OBJDIR) $(PROJECT_NAME).elf $(PROJECT_NAME).bin $(PROJECT_NAME).hex $(PROJECT_NAME).map

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# TUI library sources: NuttX compat headers from tui/include, and the
# imported code keeps its original style so a few benign warnings are
# tolerated (the project's own files stay -Werror).
# -include nuttx/config.h mirrors how the NuttX build force-feeds the
# config into every translation unit (headers guard on CONFIG_ symbols
# without including it themselves).
TUI_INC = -Itui/include -Itui/platform -Itui/termcurses -include nuttx/config.h -D_GNU_SOURCE
TUI_CFLAGS = $(CFLAGS) $(TUI_INC) -Wno-error
TUI_CXXFLAGS = $(CXXFLAGS) $(TUI_INC) -Wno-error

$(OBJDIR)/tui/%.o: tui/%.c | $(OBJDIR)
	@mkdir -p $(@D)
	@echo "Compiling $(notdir $<)..."
	@$(CC) $(TUI_CFLAGS) -c $< -o $@

$(OBJDIR)/tui/%.o: tui/%.cxx | $(OBJDIR)
	@mkdir -p $(@D)
	@echo "Compiling $(notdir $<)..."
	@$(CXX) $(TUI_CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@echo "Compiling $(notdir $<)..."
	@$(CC) $(CFLAGS) -lc -c $< -o $@

# Embedded C++ subset: no exceptions/RTTI (no unwinder or type info in
# flash), no thread-safe static guards (single core), no atexit dtors
# (firmware never exits).  Global constructors DO run - the SDK's
# __runtime_init walks .init_array before main.  operator new/delete
# forward to newlib malloc (cxxrt.cpp) over the ram_a heap span;
# -fcheck-new makes new-expressions NULL-safe since new can't throw.
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fcheck-new

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	@echo "Compiling $(notdir $<)..."
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.s | $(OBJDIR)
	@echo "Assembling $(notdir $<)..."
	@$(AS) -march=rv32ec_zicsr_zcb_zicond_zilsd -mabi=ilp32e -I$(TINYQV_SDK) $< -o $@

# Chroma .lst listings and .v sources for the TUI's 'open' tabs.  These
# are SERVED, not linked: tqv.py hands the design its --fs-root (default
# tinyQV-sdk/tqvfs) and the TUI reads them with fopen, so a re-synthesized
# chroma shows up without rebuilding the firmware (it used to cost 52KB of
# flash and went stale the moment the chroma tree changed).  Staged here
# so the served copies track the sources; 'make chromas' alone re-stages.
# Optional: re-stage chroma .lst/.v into tqvfs from the chroma repo
# (a sibling checkout).  The wildcards no-op when it is absent - the
# served copies already ship in tqvfs/chromas.
CHROMA_LST_DIR = ../tinyqv-prism-lite/chromas/output
CHROMA_V_DIR = ../tinyqv-prism-lite/chromas
# prism-tui's OWN served filesystem (not the submodule's)
TQVFS_DIR ?= tqvfs
CHROMA_FS_DIR = $(TQVFS_DIR)/chromas
CHROMA_FS_FILES = \
  $(patsubst $(CHROMA_LST_DIR)/%,$(CHROMA_FS_DIR)/%,$(wildcard $(CHROMA_LST_DIR)/chroma_*.lst)) \
  $(patsubst $(CHROMA_V_DIR)/%,$(CHROMA_FS_DIR)/%,$(wildcard $(CHROMA_V_DIR)/chroma_*.v))

chromas: $(CHROMA_FS_FILES)

$(CHROMA_FS_DIR):
	@mkdir -p $@

$(CHROMA_FS_DIR)/%.lst: $(CHROMA_LST_DIR)/%.lst | $(CHROMA_FS_DIR)
	@echo "Serving $(notdir $<)..."
	@cp $< $@

$(CHROMA_FS_DIR)/%.v: $(CHROMA_V_DIR)/%.v | $(CHROMA_FS_DIR)
	@echo "Serving $(notdir $<)..."
	@cp $< $@

# tqv_fs.c (host filesystem client) and runtime.c are bundled LOCALLY
# rather than pulled from the SDK: they are not committed to the SDK
# submodule, and linking them here provides the strong __tinyqv_fs_*
# definitions over runtime.c's weak stubs - what turns fopen/fgets/
# fprintf into real host file access ('ls'/'cat', prefs, songs).

# Host side L4Z compressor (tools/l4z <song.c> <seconds> <out.c> <name>)
tools/l4z: tools/l4z.c
	@echo "Compiling l4z.c (host)..."
	@cc -O2 -o $@ $<

# The SDK submodule builds on demand, so a fresh clone links without a
# manual 'cd tinyQV-sdk && make' first.  A just-cloned superproject has an
# empty submodule dir - populate it.
$(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a:
	@test -f $(TINYQV_SDK)/Makefile || git submodule update --init $(TINYQV_SDK)
	@echo "Building $(TINYQV_SDK)..."
	@$(MAKE) -C $(TINYQV_SDK) tinyQV.a start.o

$(PROJECT_NAME).elf: $(PROJECT_OBJS) $(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a memmap
	@echo "Linking $@..."
	@$(LD) $(PROJECT_OBJS) $(TINYQV_SDK)/start.o $(TINYQV_SDK)/tinyQV.a $(RISCV_TOOLCHAIN)/riscv32-unknown-elf/lib/libc.a $(RISCV_TOOLCHAIN)/lib/gcc/riscv32-unknown-elf/*/libgcc.a  -T memmap --gc-sections -Map=$(PROJECT_NAME).map -o $@

$(PROJECT_NAME).bin: $(PROJECT_NAME).elf
	@echo "Creating $@..."
	@$(OBJCOPY) $< -O binary $@

$(PROJECT_NAME).hex: $(PROJECT_NAME).bin
	@echo "Creating $@..."
	@od -An -t x1 -w4 -v $< > $@

-include $(wildcard $(OBJDIR)/*.d $(OBJDIR)/*/*.d $(OBJDIR)/*/*/*.d)
