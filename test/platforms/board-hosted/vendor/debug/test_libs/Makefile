# Makefile for test_libs: c_test1, c_test2, cpp_test1, cpp_test2
# HF=0 (default): soft-float, HF=1: hard-float (-mfloat-abi=hard -mfpu=fpv4-sp-d16)
# Output: libs/build (HF=0) or libs/build_hf (HF=1)

ARM_CC     ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-gcc
ARM_CXX    ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-g++
HF        ?= 0
ifeq ($(HF),1)
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
else
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
endif
ARM_OPT     = -O0 -ffunction-sections -fdata-sections -fno-inline-functions
SO_FLAGS    = -fPIC -shared -nostartfiles -Wl,-E
SO_FLAGS_CXX = -fPIC -shared -nostartfiles -nostdlib -nodefaultlibs -Wl,-E
LIBS_OUT    = ../../libs/build$(if $(filter 1,$(HF)),_hf)

C_DIR      = c
CPP_DIR    = cpp

.PHONY: all clean

all: $(LIBS_OUT)/c_test1.so $(LIBS_OUT)/c_test2.so \
     $(LIBS_OUT)/cpp_test1.so $(LIBS_OUT)/cpp_test2.so

# --- C libraries ---
$(LIBS_OUT)/c_test1.so: $(C_DIR)/c-test1/c-test1.c $(C_DIR)/c-test1/c-test1.h \
                         $(C_DIR)/c-test1/gcc.ld $(C_DIR)/c-test1/mem.ld $(C_DIR)/c-test1/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(C_DIR)/c-test1/gcc.ld -L $(C_DIR)/c-test1 \
		-o $@ $(C_DIR)/c-test1/c-test1.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

$(LIBS_OUT)/c_test2.so: $(C_DIR)/c-test2/c-test2.c $(C_DIR)/c-test2/c-test2.h \
                         $(C_DIR)/c-test2/gcc.ld $(C_DIR)/c-test2/mem.ld $(C_DIR)/c-test2/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(C_DIR)/c-test2/gcc.ld -L $(C_DIR)/c-test2 \
		-o $@ $(C_DIR)/c-test2/c-test2.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

# --- C++ libraries ---
$(LIBS_OUT)/cpp_test1.so: $(CPP_DIR)/cpp-test1/cpp-test1.cpp $(CPP_DIR)/cpp-test1/cpp-test1.h \
                           $(CPP_DIR)/cpp-test1/libcppsh.ld
	@mkdir -p $(dir $@)
	$(ARM_CXX) -std=c++23 $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS_CXX) \
		-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
		-Wl,-Map=$(@:.so=.map) -T $(CPP_DIR)/cpp-test1/libcppsh.ld \
		-I$(CPP_DIR)/cpp-test1 -I$(CPP_DIR)/cpp-test2 -o $@ $<
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst) 2>/dev/null

$(LIBS_OUT)/cpp_test2.so: $(CPP_DIR)/cpp-test2/cpp-test2.cpp $(CPP_DIR)/cpp-test2/cpp-test2.h \
                           $(CPP_DIR)/cpp-test1/cpp-test1.h $(CPP_DIR)/cpp-test2/libcppsh.ld
	@mkdir -p $(dir $@)
	$(ARM_CXX) -std=c++23 $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS_CXX) \
		-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics \
		-Wl,-Map=$(@:.so=.map) -T $(CPP_DIR)/cpp-test2/libcppsh.ld \
		-I$(CPP_DIR)/cpp-test1 -I$(CPP_DIR)/cpp-test2 -o $@ $<
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst) 2>/dev/null

clean:
	rm -f $(LIBS_OUT)/c_test*.so $(LIBS_OUT)/cpp_test*.so \
	      $(LIBS_OUT)/c_test*.map $(LIBS_OUT)/cpp_test*.map \
	      $(LIBS_OUT)/c_test*.lst $(LIBS_OUT)/cpp_test*.lst
