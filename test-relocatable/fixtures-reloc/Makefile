# fixtures-reloc — the ELF32 *relocatable* (ET_REL) module fixtures.
#
# Unlike test/fixtures (ET_DYN .so built -fPIC -shared for the GOT/PLT loaders),
# these are built the "relocatable module" way for scl::reloc:
#
#   compile:  -mlong-calls -fno-pic   (every call becomes ldr rN,=target ; blx rN,
#                                       so the target is an R_ARM_ABS32 literal —
#                                       reaches flash<->RAM regardless of >BL range)
#   link:     -r                      (partial link: keep it ET_REL, leave symbols
#                                       UND and relocations unresolved for the loader)
#
# Result: each module carries only R_ARM_ABS32 (+ R_ARM_TARGET1 == ABS32)
# relocations against .symtab — the loader places the SHF_ALLOC sections in a RAM
# arena and writes `*(sec_base+off) = S + A`. No PLT, no GOT, no Thumb-BL patching.
#
#   make            # -> build/<name>.rel.o for every fixture
#   make clean

TOOLCHAIN_PATH ?= $(shell dirname $$(which arm-none-eabi-gcc))
CC   := $(TOOLCHAIN_PATH)/arm-none-eabi-gcc
CXX  := $(TOOLCHAIN_PATH)/arm-none-eabi-g++
LD   := $(TOOLCHAIN_PATH)/arm-none-eabi-ld
READELF := $(TOOLCHAIN_PATH)/arm-none-eabi-readelf

# Purpose-built reloc sources live here (src/), clean of the ET_DYN GOT/PLT/r10
# model. The main ABI header (trace_event/fm_callback/…) is shared with the main
# test tree's fixtures-src.
SRC := src
INC := -I../../test/fixtures-src/include

# Hard-float M4 to match the freestanding platform. -mlong-calls -fno-pic is the
# whole point; -ffreestanding since these never see a libc; keep sections merged
# (no -ffunction-sections) so each module has one .text/.data/.bss/.rodata.
MCU    := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
RELOC  := -mlong-calls -fno-pic
COMMON := $(MCU) $(RELOC) -Os -ffreestanding -fno-common -Wall -Wextra
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=c++23 -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit

BUILD := build

# Two module groups, both built ET_REL the same way:
#  1. Purpose-built reloc demo modules (src/<name>.c): relprov + relmod.
#  2. The shared fixture library set (../../test/fixtures-src/<name>/) reused as
#     ET_REL modules for the full battery. These compile cleanly to ABS32 (+ a
#     few local THM_CALL); their ET_DYN GOT/PLT/r10 *comments* do not apply here.
C_FIXTURES     := relprov relmod mylib1 mylib2
SHARED_C       := c1 c2 libcons libprov
SHARED_CXX     := cpp3
CXX_FIXTURES   :=

SHARED_SRC := ../../test/fixtures-src
#  3. A multi-FILE bundle (src/multi/*.cpp): three C++ files partial-linked with
#     `ld -r` into ONE multi.rel.o — demonstrates inter-file symbol resolution.
MULTI_SRC := multi_a multi_b multi_c
#  4. flatfs plugin modules (src/flatfs/*.c): tiny self-contained modules that the
#     classic demo embeds in flash and loads from there (no semihosting FS).
FLATFS_C  := fsmod1 fsmod2
ALL_NAMES  := $(C_FIXTURES) $(CXX_FIXTURES) $(SHARED_C) $(SHARED_CXX) multi $(FLATFS_C)
ALL_OBJS   := $(addprefix $(BUILD)/,$(addsuffix .rel.o,$(ALL_NAMES)))

all: $(ALL_OBJS)
	@echo "== fixtures-reloc: $(words $(ALL_NAMES)) ET_REL modules in $(BUILD)/ =="

# Rule template: compile then partial-link (-r) to ET_REL.
# $(1)=name $(2)=compiler $(3)=flags $(4)=ext $(5)=source path
define RELOC_RULE
$(BUILD)/$(1).rel.o: $(5) | $(BUILD)
	$(2) $(3) $(INC) -c $$< -o $(BUILD)/$(1).o
	$(LD) -r $(BUILD)/$(1).o -o $$@
	@$(READELF) -h $$@ | grep -q "REL (Relocatable" && echo "  [ET_REL] $$@"
endef

# demo modules (flat src/<name>.c) + shared fixtures (fixtures-src/<name>/<name>.c[pp])
$(foreach n,$(C_FIXTURES),  $(eval $(call RELOC_RULE,$(n),$(CC), $(CFLAGS),  c,  $(SRC)/$(n).c)))
$(foreach n,$(CXX_FIXTURES),$(eval $(call RELOC_RULE,$(n),$(CXX),$(CXXFLAGS),cpp,$(SRC)/$(n).cpp)))
$(foreach n,$(SHARED_C),    $(eval $(call RELOC_RULE,$(n),$(CC), $(CFLAGS),  c,  $(SHARED_SRC)/$(n)/$(n).c)))
$(foreach n,$(SHARED_CXX),  $(eval $(call RELOC_RULE,$(n),$(CXX),$(CXXFLAGS),cpp,$(SHARED_SRC)/$(n)/$(n).cpp)))
$(foreach n,$(FLATFS_C),    $(eval $(call RELOC_RULE,$(n),$(CC), $(CFLAGS),  c,  $(SRC)/flatfs/$(n).c)))

# Multi-file bundle: compile each .cpp, then partial-link the 3 objects into ONE
# ET_REL. `ld -r` merges their sections and resolves the a->b->c cross-file
# references internally; only host imports (trace_u32) stay UND.
$(BUILD)/multi.rel.o: $(addprefix $(SRC)/multi/,$(addsuffix .cpp,$(MULTI_SRC))) | $(BUILD)
	@for f in $(MULTI_SRC); do $(CXX) $(CXXFLAGS) $(INC) -c $(SRC)/multi/$$f.cpp -o $(BUILD)/$$f.o; done
	$(LD) -r $(addprefix $(BUILD)/,$(addsuffix .o,$(MULTI_SRC))) -o $@
	@$(READELF) -h $@ | grep -q "REL (Relocatable" && echo "  [ET_REL bundle] $@ (3 .cpp -> one .rel.o)"

$(BUILD):
	mkdir -p $(BUILD)

# Show the relocation types each module carries (ABS32 / TARGET1 / THM_CALL only).
check: all
	@for m in $(ALL_NAMES); do \
	  echo "== $$m =="; \
	  $(READELF) -r $(BUILD)/$$m.rel.o | grep -oE "R_ARM_[A-Z0-9_]+" | sort | uniq -c; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
