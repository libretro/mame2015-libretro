/*********************************************************************

    debugger.c

    Front-end debugger interfaces.

    Copyright Nicola Salmoria and the MAME Team.
    Visit http://mamedev.org for licensing and usage restrictions.

*********************************************************************/

#include "emu.h"
#include "debugger.h"
#include "osdepend.h"
#include "debug/debugcpu.h"
#include "debug/debugcmd.h"
#include "debug/debugcon.h"
#include "debug/express.h"
#include "debug/debugvw.h"
#include <ctype.h>



/***************************************************************************
    TYPE DEFINITIONS
***************************************************************************/

struct machine_entry
{
	machine_entry *     next;
	running_machine *   machine;
};



/***************************************************************************
    GLOBAL VARIABLES
***************************************************************************/

static machine_entry *machine_list;
static int atexit_registered;



/***************************************************************************
    FUNCTION PROTOTYPES
***************************************************************************/

static void debugger_exit(running_machine &machine);



/***************************************************************************
    CENTRAL INITIALIZATION POINT
***************************************************************************/

/*-------------------------------------------------
    debugger_init - start up all subsections
-------------------------------------------------*/

void debugger_init(running_machine &machine)
{
}


/*-------------------------------------------------
    debugger_refresh_display - redraw the current
    video display
-------------------------------------------------*/

void debugger_refresh_display(running_machine &machine)
{
}


/*-------------------------------------------------
    debugger_exit - remove ourself from the
    global list of active machines for cleanup
-------------------------------------------------*/

static void debugger_exit(running_machine &machine)
{
}


/*-------------------------------------------------
    debugger_flush_all_traces_on_abnormal_exit -
    flush any traces in the event of an aborted
    execution
-------------------------------------------------*/

void debugger_flush_all_traces_on_abnormal_exit(void)
{
}
