# Makefile for hard-float test libs: hf_test1, hf_test2
# Always builds hard-float, outputs to libs/build_hf/
# firmware Makefiles call with HF=1 to match the build_hf/ directory

ARM_CC     ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-gcc
ARM_MCU     = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
ARM_OPT     = -O0 -ffunction-sections -fdata-sections -fno-inline-functions
SO_FLAGS    = -fPIC -shared -nostartfiles -Wl,-E
LIBS_OUT    = ../../libs/build_hf

C_DIR      = c

.PHONY: all clean

all: $(LIBS_OUT)/hf_test1.so $(LIBS_OUT)/hf_test2.so

ARM_OBJDUMP ?= $(dir $(ARM_CC))arm-none-eabi-objdump

$(LIBS_OUT)/hf_test1.so: $(C_DIR)/hf-test1/hf-test1.c $(C_DIR)/hf-test1/hf-test1.h \
                          $(C_DIR)/hf-test1/gcc.ld $(C_DIR)/hf-test1/mem.ld $(C_DIR)/hf-test1/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(C_DIR)/hf-test1/gcc.ld -L $(C_DIR)/hf-test1 \
		-o $@ $(C_DIR)/hf-test1/hf-test1.c
	$(ARM_OBJDUMP) -d -S $@ > $(@:.so=.lst) 2>/dev/null; true

$(LIBS_OUT)/hf_test2.so: $(C_DIR)/hf-test2/hf-test2.c $(C_DIR)/hf-test2/hf-test2.h \
                          $(C_DIR)/hf-test2/gcc.ld $(C_DIR)/hf-test2/mem.ld $(C_DIR)/hf-test2/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(C_DIR)/hf-test2/gcc.ld -L $(C_DIR)/hf-test2 \
		-o $@ $(C_DIR)/hf-test2/hf-test2.c
	$(ARM_OBJDUMP) -d -S $@ > $(@:.so=.lst) 2>/dev/null; true

clean:
	rm -f $(LIBS_OUT)/hf_test*.so $(LIBS_OUT)/hf_test*.map $(LIBS_OUT)/hf_test*.lst
