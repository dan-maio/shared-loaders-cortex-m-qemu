# C++ shared library build — libcppsh.so
# Sources are local: libcppsh/src/, libcppsh/include/
# HF=0 (default): soft-float, HF=1: hard-float (-mfloat-abi=hard -mfpu=fpv4-sp-d16)

ARM_CXX    ?= /home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-g++
HF        ?= 0
ifeq ($(HF),1)
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
else
  ARM_MCU   = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
endif
SO_FLAGS    = -std=c++23 -fPIC -shared -fno-exceptions -fno-rtti \
              -fno-threadsafe-statics -fno-use-cxa-atexit -Og -g -ffunction-sections \
              -nostartfiles -nostdlib -nodefaultlibs -Wl,-E,--gc-sections -Wl,-N

LIBS_OUT   = ../build$(if $(filter 1,$(HF)),_hf)
CPPSH_DIR   = libcppsh
CPPSH_SRC  = $(CPPSH_DIR)/src/cppsh-api.cpp $(CPPSH_DIR)/src/widget.cpp
CPPSH_INC  = -I$(CPPSH_DIR)/include

.PHONY: all clean deploy
all: $(LIBS_OUT)/libcppsh.so

$(LIBS_OUT)/libcppsh.so: $(CPPSH_SRC)
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_MCU) $(SO_FLAGS) $(CPPSH_INC) $(CPPSH_SRC) \
		-Wl,-Map=$(@:.so=.map) -T $(CPPSH_DIR)/libcppsh.ld -o $@
	/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin/arm-none-eabi-objdump -d -S $@ > $(@:.so=.lst) 2>/dev/null

deploy: all
	cp $(LIBS_OUT)/libcppsh.so /tmp/ 2>/dev/null || true
	@echo "  libcppsh.so copied to /tmp/"

clean:
	rm -f $(LIBS_OUT)/libcppsh.so $(LIBS_OUT)/libcppsh.map $(LIBS_OUT)/libcppsh.lst
