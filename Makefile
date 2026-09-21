CC      = i586-pc-msdosdjgpp-gcc
CFLAGS  = -O2 -Wall -march=i386
LDFLAGS = -s

MAIN_OBJS = dos_main.o dos_video.o dos_input.o dos_file.o dos_time.o dos_font.o
TEST_OBJS = vesa_test.o

all: dos_main.exe vesa_test.exe

dos_main.exe: $(MAIN_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

vesa_test.exe: $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(MAIN_OBJS) $(TEST_OBJS) dos_main.exe vesa_test.exe

.PHONY: all clean
