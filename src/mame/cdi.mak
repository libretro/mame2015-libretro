###########################################################################
#
#   cdi.mak
#
#   Philips CD-i makefile
#   Use make SUBTARGET=cdi to build
#
#   Copyright the MAME Team.
#   Visit  http://mamedev.org for licensing and usage restrictions.
#
###########################################################################

MAMESRC = $(SRC)/mame
MAMEOBJ = $(OBJ)/mame

AUDIO = $(MAMEOBJ)/audio
DRIVERS = $(MAMEOBJ)/drivers
LAYOUT = $(MAMEOBJ)/layout
MACHINE = $(MAMEOBJ)/machine
VIDEO = $(MAMEOBJ)/video

OBJDIRS += \
	$(AUDIO) \
	$(DRIVERS) \
	$(LAYOUT) \
	$(MACHINE) \
	$(VIDEO) \

#-------------------------------------------------
# Specify all the CPU cores necessary for the
# drivers referenced in tiny.c.
#-------------------------------------------------

CPUS += M680X0
#-------------------------------------------------
# Specify all the sound cores necessary for the
# drivers referenced in cdi.c.
#-------------------------------------------------

SOUNDS += SAMPLES
SOUNDS += DAC
SOUNDS += DISCRETE
SOUNDS += BEEP
SOUNDS += SPEAKER
SOUNDS += CDDA
SOUNDS += DMADAC
#-------------------------------------------------
# specify available video cores
#-------------------------------------------------

#-------------------------------------------------
# specify available machine cores
#-------------------------------------------------
MACHINES += TIMEKPR
MACHINES += SCC68070 
#-------------------------------------------------
# This is the list of files that are necessary
# for building all of the drivers referenced
# in tiny.c
#-------------------------------------------------

DRVLIBS = \
	$(DRIVERS)/cdi.o \
	$(MACHINE)/cdicdic.o \
	$(MACHINE)/cdislave.o \
	$(MACHINE)/cdi070.o \
	$(VIDEO)/mcd212.o \
