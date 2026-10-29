# SysDVR-PS4 - build under Linux / WSL with the OpenOrbis toolchain.
#   make            -> builds the .pkg (on Windows use build.ps1 instead)
#   make clean
# Variables: OO_PS4_TOOLCHAIN (OpenOrbis toolchain), FFMPEG (PS4 FFmpeg build, see toolchain/build_ffmpeg.sh)

TITLE       := SysDVR PS4
VERSION     := 1.20
TITLE_ID    := SDVR00001
CONTENT_ID  := IV0000-SDVR00001_00-SYSDVRPS4CLIENT0

TOOLCHAIN   ?= $(if $(OO_PS4_TOOLCHAIN),$(OO_PS4_TOOLCHAIN),$(HOME)/ps4build/oo)
FFMPEG      ?= $(HOME)/ps4build/ffmpeg-ps4
BUILD       ?= build
TOOLS       := $(TOOLCHAIN)/bin/linux
export OO_PS4_TOOLCHAIN := $(TOOLCHAIN)
# PkgTool.Core (.NET) crashes without libicu otherwise
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1

CC          := clang
LD          := ld.lld

SRCS        := $(wildcard src/*.c)
OBJS        := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wno-unused-function \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -Isrc/compat -I$(FFMPEG)/include
LIBS        := $(FFMPEG)/lib/libavcodec.a $(FFMPEG)/lib/libswscale.a $(FFMPEG)/lib/libavutil.a \
               -lc -lkernel -lSceVideoOut -lSceAudioOut -lScePad -lSceUserService -lSceSystemService -lSceSysmodule -lSceUsbd
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib

PKG_FILES   := eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png sce_module/libc.prx sce_module/libSceFios2.prx

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLS)/PkgTool.Core pkg_build $< .
	@echo "==> Package built: $@"

pkg.gp4: $(PKG_FILES)
	$(TOOLS)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$(PKG_FILES)"

sce_sys/param.sfo: Makefile
	$(TOOLS)/PkgTool.Core sfo_new $@
	$(TOOLS)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLS)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(BUILD)/sysdvr.elf
	$(TOOLS)/create-fself -in=$< -out=$(BUILD)/sysdvr.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(BUILD)/sysdvr.elf: $(OBJS)
	$(LD) $(OBJS) -o $@ $(LDFLAGS) $(LIBS) $(TOOLCHAIN)/lib/crt1.o

$(BUILD)/%.o: src/%.c src/*.h src/compat/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Native test: runs the real video/audio/protocol code against tests/fake_switch.py (needs libavcodec-dev, libswscale-dev, python3)
TEST_H264 ?= $(BUILD)/test.h264
test:
	mkdir -p $(BUILD)
	gcc -O1 -g -Wall -Wno-unused-function -DSYSDVR_TEST -Itests/stubs -o $(BUILD)/harness tests/harness.c src/video.c src/audio.c src/sysdvr_proto.c -lavcodec -lswscale -lavutil -lpthread
	[ -f $(TEST_H264) ] || ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 8 -c:v libx264 -profile:v high -bf 0 -g 30 -b:v 5M -f h264 $(TEST_H264)
	@echo "==> Real-time decoder"
	python3 tests/fake_switch.py $(TEST_H264) & PID=$$!; sleep 1; cd $(BUILD) && ./harness; RC=$$?; kill $$PID; exit $$RC
	@echo "==> Decoder too slow for the stream (must skip frames, not build up latency)"
	python3 tests/fake_switch.py $(TEST_H264) & PID=$$!; sleep 1; cd $(BUILD) && SYSDVR_TEST_DECODE_DELAY_MS=45 ./harness; RC=$$?; kill $$PID; exit $$RC

clean:
	rm -rf $(BUILD) eboot.bin pkg.gp4 sce_sys/param.sfo $(CONTENT_ID).pkg

.PHONY: all clean test
