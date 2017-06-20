PROGS	= mqttalrm
PROGS	+= mqttimer
PROGS	+= mqttimesw
PROGS	+= mqttimport
PROGS	+= mqttled
PROGS	+= mqttlogic
PROGS	+= mqttmaclight
PROGS	+= mqttnow
PROGS	+= mqttsun
default	: $(PROGS)

PREFIX	= /usr/local

CC	= gcc
CFLAGS	= -Wall
CPPFLAGS= -D_GNU_SOURCE
LDLIBS	= -lmosquitto
INSTOPTS= -s

VERSION := $(shell git describe --tags --always)

-include config.mk

CPPFLAGS += -DVERSION=\"$(VERSION)\"

mqttalrm: lib/libt.o common.o

mqttimer: lib/libt.o

mqttimesw: lib/libt.o common.o

mqttimport: lib/libt.o

mqttled: lib/libt.o

mqttlogic: LDLIBS+=-lm
mqttlogic: lib/libt.o rpnlogic.o

mqttmaclight: lib/libt.o

mqttnow: lib/libt.o

mqttsun: LDLIBS+=-lm
mqttsun: lib/libt.o sunposition.o

rpntest: LDLIBS+=-lm
rpntest: rpnlogic.o

install: $(PROGS)
	$(foreach PROG, $(PROGS), install -vp -m 0777 $(INSTOPTS) $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG);)

clean:
	rm -rf $(wildcard *.o lib/*.o) $(PROGS)
