# C shared library build — local sources in subdirectories
# HF=0 (default): soft-float, HF=1: hard-float (-mfloat-abi=hard -mfpu=fpv4-sp-d16)

ARM_CC     ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-gcc
HF        ?= 0
ifeq ($(HF),1)
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
else
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
endif
ARM_OPT     = -O0 -ffunction-sections -fdata-sections -fno-inline-functions
SO_FLAGS    = -fPIC -shared -nostartfiles -Wl,-E

LIBS_OUT   = ../build$(if $(filter 1,$(HF)),_hf)
MYLIB1     = mylib1
MYLIB2     = mylib2

.PHONY: all mylib1 mylib2 gotplt cov1 cov2 clean
all: mylib1 mylib2 complex gotplt cov1 cov2

mylib1: $(LIBS_OUT)/mylib1.so
$(LIBS_OUT)/mylib1.so: $(MYLIB1)/mylib1.c $(MYLIB1)/mylib1.h \
                         $(MYLIB1)/gcc.ld $(MYLIB1)/mem.ld \
                         $(MYLIB1)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(MYLIB1)/gcc.ld \
		-L $(MYLIB1) -o $@ $(MYLIB1)/mylib1.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

mylib2: $(LIBS_OUT)/mylib2.so

$(LIBS_OUT)/mylib2.so: $(MYLIB2)/mylib2.c $(MYLIB2)/mylib2.h \
                         $(MYLIB2)/gcc.ld $(MYLIB2)/mem.ld \
                         $(MYLIB2)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(MYLIB2)/gcc.ld \
		-L $(MYLIB2) -o $@ $(MYLIB2)/mylib2.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)
GOTPLT   = gotplt_test
gotplt: $(LIBS_OUT)/gotplt_test.so
$(LIBS_OUT)/gotplt_test.so: $(GOTPLT)/gotplt-test.c $(GOTPLT)/gotplt-test.h \
                              $(GOTPLT)/gcc.ld $(GOTPLT)/mem.ld \
                              $(GOTPLT)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(GOTPLT)/gcc.ld \
		-L $(GOTPLT) -o $@ $(GOTPLT)/gotplt-test.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

COV1     = cov1
cov1: $(LIBS_OUT)/cov1.so
$(LIBS_OUT)/cov1.so: $(COV1)/cov1.c $(COV1)/cov1.h \
                       $(COV1)/gcc.ld $(COV1)/mem.ld \
                       $(COV1)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(COV1)/gcc.ld \
		-L $(COV1) -o $@ $(COV1)/cov1.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

COV2     = cov2
cov2: $(LIBS_OUT)/cov2.so
$(LIBS_OUT)/cov2.so: $(COV2)/cov2.c $(COV2)/cov2.h \
                       $(COV2)/gcc.ld $(COV2)/mem.ld \
                       $(COV2)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(COV2)/gcc.ld \
		-L $(COV2) -o $@ $(COV2)/cov2.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)

deploy: all gotplt cov1 cov2
	cp $(LIBS_OUT)/complex_test.so /tmp/ 2>/dev/null || true
	cp $(LIBS_OUT)/gotplt_test.so /tmp/ 2>/dev/null || true
	cp $(LIBS_OUT)/cov1.so $(LIBS_OUT)/cov2.so /tmp/ 2>/dev/null || true
	cp $(LIBS_OUT)/mylib1.so $(LIBS_OUT)/mylib2.so /tmp/ 2>/dev/null || true
	@echo "  .so copied to /tmp/"

clean:
	rm -f $(LIBS_OUT)/mylib1.so $(LIBS_OUT)/mylib2.so \
	      $(LIBS_OUT)/mylib1.map $(LIBS_OUT)/mylib2.map \
	      $(LIBS_OUT)/mylib1.lst $(LIBS_OUT)/mylib2.lst \
	      $(LIBS_OUT)/complex_test.so $(LIBS_OUT)/complex_test.map $(LIBS_OUT)/complex_test.lst \
	      $(LIBS_OUT)/gotplt_test.so $(LIBS_OUT)/gotplt_test.map $(LIBS_OUT)/gotplt_test.lst \
	      $(LIBS_OUT)/cov1.so $(LIBS_OUT)/cov1.map $(LIBS_OUT)/cov1.lst \
	      $(LIBS_OUT)/cov2.so $(LIBS_OUT)/cov2.map $(LIBS_OUT)/cov2.lst

COMPLEX  = complex_test
complex: $(LIBS_OUT)/complex_test.so
$(LIBS_OUT)/complex_test.so: $(COMPLEX)/complex-test.c $(COMPLEX)/complex-test.h \
                               $(COMPLEX)/gcc.ld $(COMPLEX)/mem.ld \
                               $(COMPLEX)/sections2.ld
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_MCU) $(ARM_OPT) $(SO_FLAGS) \
		-Wl,-Map=$(@:.so=.map) -T $(COMPLEX)/gcc.ld \
		-L $(COMPLEX) -o $@ $(COMPLEX)/complex-test.c
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst)
