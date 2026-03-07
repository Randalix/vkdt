pipe/modules/i-mlv/libi-mlv.so: pipe/modules/i-mlv/mlv.h pipe/modules/i-mlv/raw.h pipe/modules/i-mlv/video_mlv.c pipe/modules/i-mlv/video_mlv.h pipe/modules/i-mlv/liblj92/lj92.c
ifneq ($(findstring clang,$(CC)),)
MOD_LDFLAGS+=-L/opt/homebrew/opt/libomp/lib -lomp
else ifneq ($(findstring gcc,$(CC)),)
MOD_LDFLAGS+=-lgomp
endif
