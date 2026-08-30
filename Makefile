TARGET = tankobon
OBJS = main.o core.o
.DEFAULT_GOAL := all
VERSION := $(shell cat VERSION)

CFLAGS = -O2 -G0 -Wall -DTANKOBON_VERSION=\"$(VERSION)\"

# make DEBUG=1 -> writes a boot trace to ms0:/tankobon_boot.log and paints
# colour markers on screen so an early freeze can be located.
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG_BOOT
endif

# make PROBE=1 -> boot trace plus a four-colour display probe at startup.
ifeq ($(PROBE),1)
CFLAGS += -DDEBUG_BOOT -DDEBUG_DISPLAY_PROBE
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -ljpeg -lpng -lpsppower -lpsprtc -lz -lpspgu -lpspctrl -lpspdebug -lpspdisplay

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Tankobon

# Optional XMB artwork. Editable sources live in assets/; each exported PNG is
# picked up on the next build, and anything missing is left out of the EBOOT.
# The PIC0 slot is spelled PSP_EBOOT_UNKPNG in pspsdk's build.mak -- setting
# PSP_EBOOT_PIC0 does nothing, pack-pbp never sees it.
ifneq ($(wildcard assets/ICON0.PNG),)
PSP_EBOOT_ICON = assets/ICON0.PNG
endif
ifneq ($(wildcard assets/PIC0.PNG),)
PSP_EBOOT_UNKPNG = assets/PIC0.PNG
endif
ifneq ($(wildcard assets/PIC1.PNG),)
PSP_EBOOT_PIC1 = assets/PIC1.PNG
endif

.PHONY: test-host
test-host:
	cc -std=c99 -Wall -Wextra -Werror core.c tests/test_core.c -o /tmp/tankobon-core-tests
	/tmp/tankobon-core-tests

ifneq ($(MAKECMDGOALS),test-host)
PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
endif
