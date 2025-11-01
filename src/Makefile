APP = itsa

ITSA_VER = 001

DEPDIR  := .d
$(shell mkdir -p $(DEPDIR) >/dev/null)
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$(@F).Td

CC	= gcc
CFLAGS += -Wall -Wextra -Wdeclaration-after-statement -Wvla \
	  -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
	  -std=gnu11 -g -O2 -Wp,-D_FORTIFY_SOURCE=2 --param=ssp-buffer-size=4 \
	  -fno-common -fstack-protector -fPIE -fexceptions \
	  -I../../libmtdac/include -DITSA_VER=\"${ITSA_VER}\" -pipe
LDFLAGS += -L../../libmtdac/src -Wl,-z,now,-z,defs,-z,relro,--as-needed -pie
LIBS	+= -lmtdac -lac -lsqlite3 -ljansson
POSTCOMPILE = @mv -f $(DEPDIR)/$(@F).Td $(DEPDIR)/$(@F).d && touch $@

ifeq ($(CC),gcc)
        GCC_MAJOR  := $(shell gcc -dumpfullversion -dumpversion | cut -d . -f 1)
        GCC_MINOR  := $(shell gcc -dumpfullversion -dumpversion | cut -d . -f 2)
        GCC_SUB	   := $(shell gcc -dumpfullversion -dumpversion | cut -d . -f 3)
        GCC_VER_OK := $(shell test $(GCC_MAJOR) -ge 5 -a $(GCC_MINOR) -ge 1 \
			-a $(GCC_SUB) -ge 1 && echo 1)
        ifneq "$(GCC_VER_OK)" "1"
               # For GCC < 5.1.1
               CFLAGS += -Wno-missing-field-initializers
        endif
endif

UNAME_S := $(shell uname -s | tr A-Z a-z)
ifeq ($(UNAME_S),freebsd)
        CFLAGS	+= -I/usr/local/include
        LIBS	:= -L/usr/local/lib $(LIBS)
endif

sources	= $(wildcard *.c)
objects	= $(sources:.c=.o)

ifeq ($(ASAN),1)
        override ASAN = -fsanitize=address -fno-omit-frame-pointer
endif

v = @
ifeq ($V,1)
	v =
endif

.PHONY: all
all: $(APP)

$(APP): $(objects)
	@echo "  LNK  $@"
	$(v)$(CC) $(LDFLAGS) $(ASAN) -o $@ $(objects) $(LIBS)

%.o: %.c
%.o: %.c $(DEPDIR)/%.o.d
	@echo "  CC   $@"
	$(v)$(CC) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<
	$(POSTCOMPILE)

headers := $(wildcard *.h)
hdrobjs := $(headers:.h=.gch)

hdrchk: $(hdrobjs)
%.gch: %.h
%.gch: %.h $(DEPDIR)/%.gch.d
	@echo "  CC   $<"
	$(v)$(CC) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<
	$(POSTCOMPILE)

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

include $(wildcard $(patsubst %,$(DEPDIR)/%.o.d,$(basename $(sources))))
include $(wildcard $(patsubst %,$(DEPDIR)/%.gch.d,$(basename $(headers))))

.PHONY: clean
clean:
	$(v)rm -f $(objects) $(hdrobjs) $(APP)
	$(v)rm -f $(DEPDIR)/*
	$(v)rmdir $(DEPDIR)
