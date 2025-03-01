
PROGRAM = nanotts
PICO_LIBRARY = svoxpico/.libs/libttspico.a

PREFIX?=.
DESTDIR?=
PICO_ROOT := ${DESTDIR}${PREFIX}
PICO_LANG_ROOT := ${PICO_ROOT}/share/pico
PICO_LANG_LOCATION := $(PICO_LANG_ROOT)/lang/

CFLAGS = -Wall -DPICO_LANG_LOCATION=${PICO_LANG_LOCATION}
CFLAGS_DEBUG = -g
CFLAGS_OPT = -O2
SHELL := /bin/bash

#LINKER_FLAGS := -lasound -lao
#LINKER_FLAGS := -lasound -lm
LINKER_FLAGS := -lm

all: $(PROGRAM)

OBJECTS_DIR = objs

#    $(OBJECTS_DIR)/player_ao.o                  \

OBJECTS = \
    $(OBJECTS_DIR)/mmfile.o                     \
    $(OBJECTS_DIR)/main.o                       \
    $(OBJECTS_DIR)/wav.o                        \
    $(OBJECTS_DIR)/lowest_file_number.o         \
    $(OBJECTS_DIR)/StreamHandler.o              \



ALSA_OBJECT := $(OBJECTS_DIR)/Player_Alsa.o
ALSA_SOURCE := src/Player_Alsa.cpp


ifeq ($(MAKECMDGOALS),noalsa)
else ifeq ($(MAKECMDGOALS),noalsa debug)
else ifeq ($(MAKECMDGOALS),debug noalsa)
else
    OBJECTS += $(ALSA_OBJECT)
    CFLAGS += -D_USE_ALSA
    LINKER_FLAGS := -lasound -lm
endif

ifeq ($(MAKECMDGOALS),debug)
    override CFLAGS += $(CFLAGS_DEBUG)
else ifeq ($(MAKECMDGOALS),debug noalsa)
    override CFLAGS += $(CFLAGS_DEBUG)
else ifeq ($(MAKECMDGOALS),noalsa debug)
    override CFLAGS += $(CFLAGS_DEBUG)
else
    override CFLAGS += $(CFLAGS_OPT)
endif


.PHONY: noalsa
noalsa: $(PROGRAM)

$(ALSA_OBJECT): $(ALSA_SOURCE)
	$(CXX) $(CFLAGS) -c -o $@ $^ -lasound

$(OBJECTS_DIR)/%.o: ./src/%.cpp
	$(CXX) -I. -I./svoxpico $(CFLAGS) -c $^ -o $@  $(LINKER_FLAGS)

$(OBJECTS_DIR):
	@[ -d $(OBJECTS_DIR) ] || mkdir $(OBJECTS_DIR)

$(PICO_LIBRARY):
	cd svoxpico; ./autogen.sh && ./configure --host `uname -m` && make -j

$(PROGRAM): update_build_version $(PICO_LIBRARY) $(OBJECTS_DIR) $(OBJECTS)
	$(CXX) -L./svoxpico/.libs $(OBJECTS) $(PICO_LIBRARY) $(CFLAGS) -o $(PROGRAM) $(LINKER_FLAGS)

debug: update_build_version $(PICO_LIBRARY) $(OBJECTS_DIR) $(OBJECTS)
	$(CXX) -L./svoxpico/.libs $(OBJECTS) $(PICO_LIBRARY) $(CFLAGS) -o $(PROGRAM) $(LINKER_FLAGS)

clean:
	@for file in $(OBJECTS) $(PROGRAM) pico2wave.o pico2wave build_version.h; do if [ -f $${file} ]; then rm $${file}; echo rm $${file}; fi; done
	@if [ -d $(OBJECTS_DIR) ]; then rmdir $(OBJECTS_DIR) ; fi
	@echo "use \"make distclean\" to also cleanup svoxpico directory"

distclean: clean
	cd svoxpico; make clean ; ./clean.sh

pico: $(PICO_LIBRARY)
	$(CC) -I. -I./svoxpico -Wall -g $(OPT_FLAG) -c -o pico2wave.o src/pico2wave.c
	$(CC) -I./svoxpico -Wall -g $(OPT_FLAG) pico2wave.o svoxpico/.libs/libttspico.a -o pico2wave -lm -lpopt

both: $(PROGRAM) pico

install:
	@if [ ! -d ${PICO_ROOT}/bin ]; then echo mkdir -p -m 755 ${PICO_ROOT}/bin ; mkdir -p -m 755 ${PICO_ROOT}/bin; fi
	install -m 0755 $(PROGRAM) ${PICO_ROOT}/bin/
	@if [ ! -d $(PICO_LANG_LOCATION) ]; then echo mkdir -p -m 755 $(PICO_LANG_LOCATION); mkdir -p -m 755 $(PICO_LANG_LOCATION); fi
	@for file in ./lang/* ; do echo install -m 0644 $${file} $(PICO_LANG_LOCATION); install -m 0644 $${file} $(PICO_LANG_LOCATION); done

uninstall:
	@if [ -e ${PICO_ROOT}/bin/$(PROGRAM) ]; then echo rm ${PICO_ROOT}/bin/$(PROGRAM); rm ${PICO_ROOT}/bin/$(PROGRAM); fi
	@if [ -e $(PICO_LANG_ROOT) ]; then echo rm -rf $(PICO_LANG_ROOT); rm -rf $(PICO_LANG_ROOT) ; fi

update_build_version:
	@./update_build_version.sh

