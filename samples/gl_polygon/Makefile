TARGET = paletted.elf
OBJS = main.o
LIBS = -lGLdc

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.* 1ST_READ.BIN paletted.bin

rm-elf:
	-rm -f $(TARGET) romdisk.* 1ST_READ.BIN paletted.bin

$(TARGET): $(OBJS) romdisk.o
	$(KOS_CC) $(KOS_CFLAGS) $(KOS_LDFLAGS) -o $(TARGET) $(KOS_START) \
		$(OBJS) romdisk.o $(OBJEXTRA) $(LIBS) -lm -lkosutils $(KOS_LIBS) 

romdisk.img:
	$(KOS_GENROMFS) -f romdisk.img -d romdisk -v

romdisk.o: romdisk.img
	$(KOS_BASE)/utils/bin2o/bin2o romdisk.img romdisk romdisk.o

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	rm -f $(OBJS) romdisk.o romdisk.img
	$(KOS_STRIP) $(TARGET)
	strip-bin paletted
	scramble paletted.bin 1ST_READ.BIN
