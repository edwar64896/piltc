IDIR =../include
CC=gcc
CFLAGS=-I$(IDIR) -O3

ODIR=obj
LDIR =../lib

LIBS=-lm  -lrt -lpthread -lltc -lwiringPi -lncurses

_DEPS =
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_timerOBJ = timer.o ringbuf.o
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))


$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

timer: $(_timerOBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~  timer

all: timer 
