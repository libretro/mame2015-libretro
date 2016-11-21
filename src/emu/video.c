// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    video.c

    Core MAME video routines.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "debugger.h"
#include "ui/ui.h"
#include "aviio.h"
#include "crsshair.h"
#include "output.h"

#include "snap.lh"

#include "osdepend.h"

//**************************************************************************
//  DEBUGGING
//**************************************************************************

//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

// frameskipping tables
const UINT8 video_manager::s_skiptable[FRAMESKIP_LEVELS][FRAMESKIP_LEVELS] =
{
	{ 0,0,0,0,0,0,0,0,0,0,0,0 },
	{ 0,0,0,0,0,0,0,0,0,0,0,1 },
	{ 0,0,0,0,0,1,0,0,0,0,0,1 },
	{ 0,0,0,1,0,0,0,1,0,0,0,1 },
	{ 0,0,1,0,0,1,0,0,1,0,0,1 },
	{ 0,1,0,0,1,0,1,0,0,1,0,1 },
	{ 0,1,0,1,0,1,0,1,0,1,0,1 },
	{ 0,1,0,1,1,0,1,0,1,1,0,1 },
	{ 0,1,1,0,1,1,0,1,1,0,1,1 },
	{ 0,1,1,1,0,1,1,1,0,1,1,1 },
	{ 0,1,1,1,1,1,0,1,1,1,1,1 },
	{ 0,1,1,1,1,1,1,1,1,1,1,1 }
};



//**************************************************************************
//  VIDEO MANAGER
//**************************************************************************

static void video_notifier_callback(const char *outname, INT32 value, void *param)
{
	video_manager *vm = (video_manager *)param;

	vm->set_output_changed();
}


//-------------------------------------------------
//  video_manager - constructor
//-------------------------------------------------

video_manager::video_manager(running_machine &machine)
	: m_machine(machine),
		m_screenless_frame_timer(NULL),
		m_output_changed(false),
		m_throttle_last_ticks(0),
		m_throttle_realtime(attotime::zero),
		m_throttle_emutime(attotime::zero),
		m_throttle_history(0),
		m_speed_last_realtime(0),
		m_speed_last_emutime(attotime::zero),
		m_speed_percent(1.0),
		m_overall_real_seconds(0),
		m_overall_real_ticks(0),
		m_overall_emutime(attotime::zero),
		m_overall_valid_counter(0),
		m_throttled(machine.options().throttle()),
		m_throttle_rate(1.0f),
		m_fastforward(false),
		m_seconds_to_run(machine.options().seconds_to_run()),
		m_auto_frameskip(machine.options().auto_frameskip()),
		m_speed(original_speed_setting()),
		m_empty_skip_count(0),
		m_frameskip_level(machine.options().frameskip()),
		m_frameskip_counter(0),
		m_frameskip_adjust(0),
		m_skipping_this_frame(false),
		m_average_oversleep(0),
		m_snap_target(NULL),
		m_snap_native(true),
		m_snap_width(0),
		m_snap_height(0),
		m_dummy_recording(false)
{
	// request a callback upon exiting
	machine.add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(FUNC(video_manager::exit), this));
	machine.save().register_postload(save_prepost_delegate(FUNC(video_manager::postload), this));

	// extract initial execution state from global configuration settings
	update_refresh_speed();

	// create a render target for snapshots
	const char *viewname = machine.options().snap_view();
	m_snap_native = (machine.first_screen() != NULL && (viewname[0] == 0 || strcmp(viewname, "native") == 0));

	// the native target is hard-coded to our internal layout and has all options disabled
	if (m_snap_native)
	{
		m_snap_target = machine.render().target_alloc(layout_snap, RENDER_CREATE_SINGLE_FILE | RENDER_CREATE_HIDDEN);
		m_snap_target->set_backdrops_enabled(false);
		m_snap_target->set_overlays_enabled(false);
		m_snap_target->set_bezels_enabled(false);
		m_snap_target->set_cpanels_enabled(false);
		m_snap_target->set_marquees_enabled(false);
		m_snap_target->set_screen_overlay_enabled(false);
		m_snap_target->set_zoom_to_screen(false);
	}

	// other targets select the specified view and turn off effects
	else
	{
		m_snap_target = machine.render().target_alloc(NULL, RENDER_CREATE_HIDDEN);
		m_snap_target->set_view(m_snap_target->configured_view(viewname, 0, 1));
		m_snap_target->set_screen_overlay_enabled(false);
	}

	// extract snap resolution if present
	if (sscanf(machine.options().snap_size(), "%dx%d", &m_snap_width, &m_snap_height) != 2)
		m_snap_width = m_snap_height = 0;

#ifdef MAME_DEBUG
	m_dummy_recording = machine.options().dummy_write();
#endif

	// if no screens, create a periodic timer to drive updates
	if (machine.first_screen() == NULL)
	{
		m_screenless_frame_timer = machine.scheduler().timer_alloc(timer_expired_delegate(FUNC(video_manager::screenless_update_callback), this));
		m_screenless_frame_timer->adjust(screen_device::DEFAULT_FRAME_PERIOD, 0, screen_device::DEFAULT_FRAME_PERIOD);
		output_set_notifier(NULL, video_notifier_callback, this);
	}
}


//-------------------------------------------------
//  set_frameskip - set the current actual
//  frameskip (-1 means autoframeskip)
//-------------------------------------------------

void video_manager::set_frameskip(int frameskip)
{
	// -1 means autoframeskip
	if (frameskip == -1)
	{
		m_auto_frameskip = true;
		m_frameskip_level = 0;
	}

	// any other level is a direct control
	else if (frameskip >= 0 && frameskip <= MAX_FRAMESKIP)
	{
		m_auto_frameskip = false;
		m_frameskip_level = frameskip;
	}
}


//-------------------------------------------------
//  frame_update - handle frameskipping and UI,
//  plus updating the screen during normal
//  operations
//-------------------------------------------------

void video_manager::frame_update(bool debug)
{
	// only render sound and video if we're in the running phase
	int phase = machine().phase();
	bool skipped_it = m_skipping_this_frame;
	if (phase == MACHINE_PHASE_RUNNING && (!machine().paused() || machine().options().update_in_pause()))
	{
		bool anything_changed = finish_screen_updates();

		// if none of the screens changed and we haven't skipped too many frames in a row,
		// mark this frame as skipped to prevent throttling; this helps for games that
		// don't update their screen at the monitor refresh rate
		if (!anything_changed && !m_auto_frameskip && m_frameskip_level == 0 && m_empty_skip_count++ < 3)
			skipped_it = true;
		else
			m_empty_skip_count = 0;
	}

	// draw the user interface
	machine().ui().update_and_render(&machine().render().ui_container());

	// ask the OSD to update
	machine().osd().update(!debug && skipped_it);

	// perform tasks for this frame
	if (!debug)
		machine().call_notifiers(MACHINE_NOTIFY_FRAME);

	// update frameskipping
	if (!debug)
		update_frameskip();

	// call the end-of-frame callback
	if (phase == MACHINE_PHASE_RUNNING)
	{
		// reset partial updates if we're paused or if the debugger is active
		if (machine().first_screen() != NULL && (machine().paused() || debug || debugger_within_instruction_hook(machine())))
			machine().first_screen()->reset_partial_updates();
	}
}


//-------------------------------------------------
//  speed_text - print the text to be displayed
//  into a string buffer
//-------------------------------------------------

astring &video_manager::speed_text(astring &string)
{
	string.reset();

	// if we're paused, just display Paused
	bool paused = machine().paused();
	if (paused)
		string.cat("paused");

	// if we're fast forwarding, just display Fast-forward
	else if (m_fastforward)
		string.cat("fast ");

	// if we're auto frameskipping, display that plus the level
	else if (effective_autoframeskip())
		string.catprintf("auto%2d/%d", effective_frameskip(), MAX_FRAMESKIP);

	// otherwise, just display the frameskip plus the level
	else
		string.catprintf("skip %d/%d", effective_frameskip(), MAX_FRAMESKIP);

	// append the speed for all cases except paused
	if (!paused)
		string.catprintf("%4d%%", (int)(100 * m_speed_percent + 0.5));

	// display the number of partial updates as well
	int partials = 0;
	screen_device_iterator iter(machine().root_device());
	for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
		partials += screen->partial_updates();
	if (partials > 1)
		string.catprintf("\n%d partial updates", partials);

	return string;
}


//-------------------------------------------------
//  video_exit - close down the video system
//-------------------------------------------------

void video_manager::exit()
{
	// free the snapshot target
	machine().render().target_free(m_snap_target);
	m_snap_bitmap.reset();

	// print a final result if we have at least 2 seconds' worth of data
	if (m_overall_emutime.seconds >= 1)
	{
		osd_ticks_t tps = osd_ticks_per_second();
		double final_real_time = (double)m_overall_real_seconds + (double)m_overall_real_ticks / (double)tps;
		double final_emu_time = m_overall_emutime.as_double();
		osd_printf_info("Average speed: %.2f%% (%d seconds)\n", 100 * final_emu_time / final_real_time, (m_overall_emutime + attotime(0, ATTOSECONDS_PER_SECOND / 2)).seconds);
	}
}


//-------------------------------------------------
//  screenless_update_callback - update generator
//  when there are no screens to drive it
//-------------------------------------------------

void video_manager::screenless_update_callback(void *ptr, int param)
{
	// force an update
	frame_update(false);
}


//-------------------------------------------------
//  postload - callback for resetting things after
//  state has been loaded
//-------------------------------------------------

void video_manager::postload()
{
}

//-------------------------------------------------
//  effective_autoframeskip - return the effective
//  autoframeskip value, accounting for fast
//  forward
//-------------------------------------------------

inline int video_manager::effective_autoframeskip() const
{
	// if we're fast forwarding or paused, autoframeskip is disabled
	if (m_fastforward || machine().paused())
		return false;

	// otherwise, it's up to the user
	return m_auto_frameskip;
}


//-------------------------------------------------
//  effective_frameskip - return the effective
//  frameskip value, accounting for fast
//  forward
//-------------------------------------------------

inline int video_manager::effective_frameskip() const
{
	// if we're fast forwarding, use the maximum frameskip
	if (m_fastforward)
		return FRAMESKIP_LEVELS - 1;

	// otherwise, it's up to the user
	return m_frameskip_level;
}


//-------------------------------------------------
//  effective_throttle - return the effective
//  throttle value, accounting for fast
//  forward and user interface
//-------------------------------------------------

inline bool video_manager::effective_throttle() const
{
	// if we're paused, or if the UI is active, we always throttle
	if (machine().paused() || machine().ui().is_menu_active())
		return true;

	// if we're fast forwarding, we don't throttle
	if (m_fastforward)
		return false;

	// otherwise, it's up to the user
	return throttled();
}


//-------------------------------------------------
//  original_speed_setting - return the original
//  speed setting
//-------------------------------------------------

inline int video_manager::original_speed_setting() const
{
	return machine().options().speed() * 1000.0 + 0.5;
}


//-------------------------------------------------
//  finish_screen_updates - finish updating all
//  the screens
//-------------------------------------------------

bool video_manager::finish_screen_updates()
{
	// finish updating the screens
	screen_device_iterator iter(machine().root_device());

	for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
		screen->update_partial(screen->visible_area().max_y);

	// now add the quads for all the screens
	bool anything_changed = m_output_changed;
	m_output_changed = false;
	for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
		if (screen->update_quads())
			anything_changed = true;

	// update our movie recording and burn-in state
	if (!machine().paused())
	{
		// iterate over screens and update the burnin for the ones that care
		for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
			screen->update_burnin();
	}

	// draw any crosshairs
	for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
		crosshair_render(*screen);

	return anything_changed;
}

//  update_frameskip - update frameskipping
//  counters and periodically update autoframeskip
//-------------------------------------------------

void video_manager::update_frameskip()
{
	// if we're throttling and autoframeskip is on, adjust
	if (effective_throttle() && effective_autoframeskip() && m_frameskip_counter == 0)
	{
		// calibrate the "adjusted speed" based on the target
		double adjusted_speed_percent = m_speed_percent / m_throttle_rate;

		// if we're too fast, attempt to increase the frameskip
		double speed = m_speed * 0.001;
		if (adjusted_speed_percent >= 0.995 * speed)
		{
			// but only after 3 consecutive frames where we are too fast
			if (++m_frameskip_adjust >= 3)
			{
				m_frameskip_adjust = 0;
				if (m_frameskip_level > 0)
					m_frameskip_level--;
			}
		}

		// if we're too slow, attempt to increase the frameskip
		else
		{
			// if below 80% speed, be more aggressive
			if (adjusted_speed_percent < 0.80 *  speed)
				m_frameskip_adjust -= (0.90 * speed - m_speed_percent) / 0.05;

			// if we're close, only force it up to frameskip 8
			else if (m_frameskip_level < 8)
				m_frameskip_adjust--;

			// perform the adjustment
			while (m_frameskip_adjust <= -2)
			{
				m_frameskip_adjust += 2;
				if (m_frameskip_level < MAX_FRAMESKIP)
					m_frameskip_level++;
			}
		}
	}

	// increment the frameskip counter and determine if we will skip the next frame
	m_frameskip_counter = (m_frameskip_counter + 1) % FRAMESKIP_LEVELS;
	m_skipping_this_frame = s_skiptable[effective_frameskip()][m_frameskip_counter];
}


//-------------------------------------------------
//  update_refresh_speed - update the m_speed
//  based on the maximum refresh rate supported
//-------------------------------------------------

void video_manager::update_refresh_speed()
{
	// only do this if the refreshspeed option is used
	if (machine().options().refresh_speed())
	{
		float minrefresh = machine().render().max_update_rate();
		if (minrefresh != 0)
		{
			// find the screen with the shortest frame period (max refresh rate)
			// note that we first check the token since this can get called before all screens are created
			attoseconds_t min_frame_period = ATTOSECONDS_PER_SECOND;
			screen_device_iterator iter(machine().root_device());
			for (screen_device *screen = iter.first(); screen != NULL; screen = iter.next())
			{
				attoseconds_t period = screen->frame_period().attoseconds;
				if (period != 0)
					min_frame_period = MIN(min_frame_period, period);
			}

			// compute a target speed as an integral percentage
			// note that we lop 0.25Hz off of the minrefresh when doing the computation to allow for
			// the fact that most refresh rates are not accurate to 10 digits...
			UINT32 target_speed = floor((minrefresh - 0.25f) * 1000.0 / ATTOSECONDS_TO_HZ(min_frame_period));
			UINT32 original_speed = original_speed_setting();
			target_speed = MIN(target_speed, original_speed);

			// if we changed, log that verbosely
			if (target_speed != m_speed)
			{
				osd_printf_verbose("Adjusting target speed to %.1f%% (hw=%.2fHz, game=%.2fHz, adjusted=%.2fHz)\n", target_speed / 10.0, minrefresh, ATTOSECONDS_TO_HZ(min_frame_period), ATTOSECONDS_TO_HZ(min_frame_period * 1000.0 / target_speed));
				m_speed = target_speed;
			}
		}
	}
}

//  open_next - open the next non-existing file of
//  type filetype according to our numbering
//  scheme
//-------------------------------------------------

file_error video_manager::open_next(emu_file &file, const char *extension)
{
	UINT32 origflags = file.openflags();

	// handle defaults
	const char *snapname = machine().options().snap_name();

	if (snapname == NULL || snapname[0] == 0)
		snapname = "%g/%i";
	astring snapstr(snapname);

	// strip any extension in the provided name
	int index = snapstr.rchr(0, '.');
	if (index != -1)
		snapstr.substr(0, index);

	// handle %d in the template (for image devices)
	astring snapdev("%d_");
	int pos = snapstr.find(0, snapdev);

	if (pos != -1)
	{
		// if more %d are found, revert to default and ignore them all
		if (snapstr.find(pos + 3, snapdev) != -1)
			snapstr.cpy("%g/%i");
		// else if there is a single %d, try to create the correct snapname
		else
		{
			int name_found = 0;

			// find length of the device name
			int end1 = snapstr.find(pos + 3, "/");
			int end2 = snapstr.find(pos + 3, "%");
			int end = -1;

			if ((end1 != -1) && (end2 != -1))
				end = MIN(end1, end2);
			else if (end1 != -1)
				end = end1;
			else if (end2 != -1)
				end = end2;
			else
				end = snapstr.len();

			if (end - pos < 3)
				fatalerror("Something very wrong is going on!!!\n");

			// copy the device name to an astring
			astring snapdevname;
			snapdevname.cpysubstr(snapstr, pos + 3, end - pos - 3);
			//printf("check template: %s\n", snapdevname.cstr());

			// verify that there is such a device for this system
			image_interface_iterator iter(machine().root_device());
			for (device_image_interface *image = iter.first(); image != NULL; image = iter.next())
			{
				// get the device name
				astring tempdevname(image->brief_instance_name());
				//printf("check device: %s\n", tempdevname.cstr());

				if (snapdevname.cmp(tempdevname) == 0)
				{
					// verify that such a device has an image mounted
					if (image->basename() != NULL)
					{
						astring filename(image->basename());

						// strip extension
						filename.substr(0, filename.rchr(0, '.'));

						// setup snapname and remove the %d_
						snapstr.replace(0, snapdevname, filename);
						snapstr.del(pos, 3);
						//printf("check image: %s\n", filename.cstr());

						name_found = 1;
					}
				}
			}

			// or fallback to default
			if (name_found == 0)
				snapstr.cpy("%g/%i");
		}
	}

	// add our own extension
	snapstr.cat(".").cat(extension);

	// substitute path and gamename up front
	snapstr.replace(0, "/", PATH_SEPARATOR);
	snapstr.replace(0, "%g", machine().basename());

	// determine if the template has an index; if not, we always use the same name
	astring fname;
	if (snapstr.find(0, "%i") == -1)
		fname.cpy(snapstr);

	// otherwise, we scan for the next available filename
	else
	{
		// try until we succeed
		astring seqtext;
		file.set_openflags(OPEN_FLAG_READ);
		for (int seq = 0; ; seq++)
		{
			// build up the filename
			fname.cpy(snapstr).replace(0, "%i", seqtext.format("%04d", seq).cstr());

			// try to open the file; stop when we fail
			file_error filerr = file.open(fname);
			if (filerr != FILERR_NONE)
				break;
		}
	}

	// create the final file
	file.set_openflags(origflags);
	return file.open(fname);
}


//-------------------------------------------------
//  toggle_throttle
//-------------------------------------------------

void video_manager::toggle_throttle()
{
	set_throttled(!throttled());
}
