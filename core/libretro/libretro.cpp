#include <cstdio>
#include <cstdarg>
#include <math.h>
#include "types.h"

#include <sys/stat.h>
#include <retro_stat.h>

#include <libretro.h>

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include <glsm/glsm.h>
#endif
#include "../rend/rend.h"
#include "../hw/sh4/sh4_mem.h"
#include "keyboard_map.h"
#include "../hw/maple/maple_devs.h"

#if defined(_XBOX) || defined(_WIN32)
char slash = '\\';
#else
char slash = '/';
#endif

#define RETRO_ENVIRONMENT_RETROARCH_START_BLOCK 0x800000

#define RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND (2 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
                                            /* bool * --
                                            * Boolean value that tells the front end to save states in the
                                            * background or not.
                                            */

#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
                                            /* retro_environment_t * --
                                            * Provides the callback to the frontend method which will cancel
                                            * all currently waiting threads.  Used when coordination is needed
                                            * between the core and the frontend to gracefully stop all threads.
                                            */


u32 fskip;
extern int screen_width;
extern int screen_height;
char save_dir[PATH_MAX];
char eeprom_file[PATH_MAX];
char nvmem_file[PATH_MAX];
bool boot_to_bios;

static int astick_deadzone = 0;
static int trigger_deadzone = 0;
static bool digital_triggers = false;
static bool allow_service_buttons = false;

u16 kcode[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
u8 rt[4] = {0, 0, 0, 0};
u8 lt[4] = {0, 0, 0, 0};
u32 vks[4];
s8 joyx[4], joyy[4];
extern f32 mo_x_abs[4];
extern f32 mo_y_abs[4];
extern u32 mo_buttons[4];
extern f32 mo_x_delta[4];
extern f32 mo_y_delta[4];
extern f32 mo_wheel_delta[4];

bool enable_purupuru = true;

static bool first_run = true;

enum DreamcastController
{
   DC_BTN_C       = 1,
   DC_BTN_B       = 1<<1,
   DC_BTN_A       = 1<<2,
   DC_BTN_START   = 1<<3,
   DC_DPAD_UP     = 1<<4,
   DC_DPAD_DOWN   = 1<<5,
   DC_DPAD_LEFT   = 1<<6,
   DC_DPAD_RIGHT  = 1<<7,
   DC_BTN_Z       = 1<<8,
   DC_BTN_Y       = 1<<9,
   DC_BTN_X       = 1<<10,
   DC_BTN_D       = 1<<11,
   DC_DPAD2_UP    = 1<<12,
   DC_DPAD2_DOWN  = 1<<13,
   DC_DPAD2_LEFT  = 1<<14,
   DC_DPAD2_RIGHT = 1<<15,

   DC_AXIS_LT = 0X10000,
   DC_AXIS_RT = 0X10001,
   DC_AXIS_X  = 0X20000,
   DC_AXIS_Y  = 0X20001,
};

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

// Callbacks
retro_log_printf_t         log_cb = NULL;
retro_video_refresh_t      video_cb = NULL;
retro_input_poll_t         poll_cb = NULL;
retro_input_state_t        input_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t        environ_cb = NULL;
retro_environment_t        frontend_clear_thread_waits_cb = NULL;
static retro_rumble_interface rumble;

int dc_init(int argc,wchar* argv[]);
void dc_run();
void dc_term(void);
void dc_stop();
void dc_start();
void bm_Reset() ;	// Sh4 dynarec block manager
void FlushCache();	// Arm dynarec (arm and x86 only)
bool dc_is_running();
extern Renderer* renderer;
bool rend_single_frame();
void rend_cancel_emu_wait();

static int co_argc;
static wchar** co_argv;

char *game_data;
char g_base_name[128];
char game_dir[1024];
char game_dir_no_slash[1024];
static bool emu_inited = false;
static bool emu_in_thread = false;
static bool performed_serialization = false;
#if !defined(TARGET_NO_THREADS)
static void *emu_thread_func(void *);
static cThread emu_thread(&emu_thread_func, 0);
static cMutex mtx_serialization ;
static cMutex mtx_mainloop ;
static bool gl_ctx_resetting = false;

static void *emu_thread_func(void *)
{
    char* argv[] = { "reicast" };
    
    if (dc_init(1, argv))
    {
    	if (log_cb)
    	   log_cb(RETRO_LOG_ERROR, "Reicast emulator initialization failed\n");

    	return NULL;
    }
    
    emu_inited = true;
    emu_in_thread = true ;
    while ( true )
    {
    	performed_serialization = false ;
    	mtx_mainloop.Lock() ;
    	rend_cancel_emu_wait() ;
        dc_run();
        mtx_mainloop.Unlock() ;

    	mtx_serialization.Lock() ;
    	mtx_serialization.Unlock() ;

    	if (!performed_serialization)
    		break ;
    }

	rend_cancel_emu_wait() ;
    dc_term();

    emu_in_thread = false ;

    return NULL;
}
#endif


void co_dc_yield(void)
{
#if !defined(TARGET_NO_THREADS)
    if (!settings.rend.ThreadedRendering)
#endif
    {
    	if (settings.UpdateMode || settings.UpdateModeForced)
    		return;
    	dc_stop();
    }
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   // Nothing to do here
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

static void input_set_deadzone_stick( int percent )
{
   if ( percent >= 0 && percent <= 100 )
      astick_deadzone = (int)( percent * 0.01f * 0x8000);
}

static void input_set_deadzone_trigger( int percent )
{
   if ( percent >= 0 && percent <= 100 )
      trigger_deadzone = (int)( percent * 0.01f * 0x8000);
}

#define COLORS_STRING "BLACK 02|" \
		"BLUE 03|" \
		"LIGHT_BLUE 04|" \
		"GREEN 05|" \
		"CYAN 06|" \
		"CYAN_BLUE 07|" \
		"LIGHT_GREEN 08|" \
		"CYAN_GREEN 09|" \
		"LIGHT_CYAN 10|" \
		"RED 11|" \
		"PURPLE 12|" \
		"LIGHT_PURPLE 13|" \
		"YELLOW 14|" \
		"GRAY 15|" \
		"LIGHT_PURPLE_2 16|" \
		"LIGHT_GREEN_2 17|" \
		"LIGHT_GREEN_3 18|" \
		"LIGHT_CYAN_2 19|" \
		"LIGHT_RED_2 20|" \
		"MAGENTA 21|" \
		"LIGHT_PURPLE_2 22|" \
		"LIGHT_ORANGE 23|" \
		"ORANGE 24|" \
		"LIGHT_PURPLE_3 25|" \
		"LIGHT_YELLOW 26|" \
		"LIGHT_YELLOW_2 27|" \
		"WHITE 28" \

#define VMU_SCREEN_PARAMS(num)       { \
"reicast_vmu" #num "_screen_display", \
"VMU Screen " #num " Display; disabled|enabled" \
}, \
{ \
"reicast_vmu" #num "_screen_position", \
"VMU Screen " #num " Position; Upper Left|Upper Right|Lower Left|Lower Right" \
}, \
{ \
"reicast_vmu" #num "_screen_size_mult", \
"VMU Screen " #num " Size; 1x|2x|3x|4x|5x" \
}, \
{ \
"reicast_vmu" #num "_pixel_on_color", \
"VMU Screen " #num " Pixel On Color; " "DEFAULT_ON 00|DEFAULT_OFF 01|" \
COLORS_STRING \
}, \
{ \
"reicast_vmu" #num "_pixel_off_color", \
"VMU Screen " #num " Pixel Off Color; " "DEFAULT_OFF 01|DEFAULT_ON 00|" \
COLORS_STRING \
}, \
{ \
"reicast_vmu" #num "_screen_opacity", \
"VMU Screen " #num " Opacity; 100%|90%|80%|70%|60%|50%|40%|30%|20%|10%" \
}, \


void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_variable variables[] = {
#if ((FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_X64) || TARGET_NO_JIT)
      {
         "reicast_cpu_mode",
         "CPU Mode (restart); "
#if (FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_X64)
            "dynamic_recompiler"
#endif
#ifdef TARGET_NO_JIT
            "|"
            "generic_recompiler"
#endif
            ,
      },
#endif
      {
         "reicast_boot_to_bios",
         "Boot to BIOS (restart); disabled|enabled",
      },
      {
         "reicast_system",
         "System type (restart); auto|dreamcast|naomi",
      },
#ifdef HAVE_OIT
      {
         "reicast_oit_abuffer_size",
         "Accumulation pixel buffer size (restart); 512MB|1GB|2GB",
      },
#endif
      {
         "reicast_internal_resolution",
         "Internal resolution (restart); 640x480|320x240|1280x960|1920x1440|2560x1920|3200x2400|3840x2880|4480x3360|5120x3840|5760x4320|6400x4800|7040x5280|7680x5760|8320x6240|8960x6720|9600x7200|10240x7680|10880x8160|11520x8640|12160x9120|12800x9600",
      },
      {
         "reicast_screen_rotation",
         "Screen orientation; horizontal|vertical",
      },
      {
    	 "reicast_alpha_sorting",
#ifdef HAVE_OIT
         "Alpha sorting; per-triangle (normal)|per-strip (fast, least accurate)|per-pixel (accurate)",
#else
         "Alpha sorting; per-triangle (normal)|per-strip (fast, least accurate)",
#endif
      },
      {
         "reicast_extra_depth_scale",
         "Extra depth scaling; auto|disabled|enabled",
      },
      {
         "reicast_gdrom_fast_loading",
         "GDROM Fast Loading (inaccurate); disabled|enabled",
      },
      {
         "reicast_mipmapping",
         "Mipmapping; enabled|disabled",
      },
      {
         "reicast_volume_modifier_enable",
         "Volume modifier; enabled|disabled",
      },
      {
         "reicast_widescreen_hack",
         "Widescreen hack (restart); disabled|enabled",
      },
      {
         "reicast_audio_buffer_size",
         "Audio buffer size; 1024|2048",
      },
      {
         "reicast_cable_type",
         "Cable type; TV (RGB)|TV (VBS/Y+S/C)|VGA (RGB)",
      },
      {
         "reicast_broadcast",
         "Broadcast; PAL_M|PAL_N|NTSC|PAL|Default",
      },
      {
         "reicast_framerate",
         "Framerate; fullspeed|normal",
      },
      {
         "reicast_region",
         "Region; Default|Japan|USA|Europe",
      },
      {
         "reicast_div_matching",
         "DIV matching (performance, less accurate); disabled|enabled|auto",
      },
      {
         "reicast_analog_stick_deadzone",
         "Analog Stick Deadzone; 15%|20%|25%|30%|0%|5%|10%"
      },
      {
         "reicast_trigger_deadzone",
         "Trigger Deadzone; 0%|5%|10%|15%|20%|25%|30%"
      },
      {
         "reicast_digital_triggers",
         "Digital Triggers; disabled|enabled",
      },
      {
         "reicast_enable_dsp",
         "Enable DSP; enabled|disabled",
      },
#ifdef HAVE_TEXUPSCALE
      {
         "reicast_texupscale",
         "Texture upscaling (xBRZ); off|2x|4x|6x",
      },
      {
         "reicast_texupscale_max_filtered_texture_size",
         "Texture upscaling max filtered size; 256|512|1024",
      },
#endif
      {
         "reicast_enable_rtt",
         "Enable RTT (Render To Texture); enabled|disabled",
      },
      {
         "reicast_enable_rttb",
         "Enable RTT (Render To Texture) Buffer; disabled|enabled",
      },
      {
         "reicast_render_to_texture_upscaling",
         "Render To Texture Upscaling; 1x|2x|3x|4x|8x",
      },
#if !defined(TARGET_NO_THREADS)
      {
         "reicast_threaded_rendering",
         "Threaded rendering (restart); disabled|enabled",
      },
#endif
      {
         "reicast_enable_purupuru",
         "Purupuru Pack (restart); enabled|disabled"
      },
      {
         "reicast_allow_service_buttons",
         "Allow Naomi service buttons; disabled|enabled"
      },
	  VMU_SCREEN_PARAMS(1)
	  VMU_SCREEN_PARAMS(2)
	  VMU_SCREEN_PARAMS(3)
	  VMU_SCREEN_PARAMS(4)
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

   static const struct retro_controller_description ports_default[] =
   {
		 { "Gamepad",		RETRO_DEVICE_JOYPAD },
		 { "Keyboard",		RETRO_DEVICE_KEYBOARD },
		 { "Mouse",			RETRO_DEVICE_MOUSE },
		 { "Light Gun",		RETRO_DEVICE_LIGHTGUN },
		 { "Disconnected",	RETRO_DEVICE_NONE },
		 { 0 },
   };
   static const struct retro_controller_info ports[] = {
           { ports_default,  4 },
           { ports_default,  4 },
           { ports_default,  4 },
           { ports_default,  4 },
           { 0 },
   };
   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers);

// Now comes the interesting stuff
void retro_init(void)
{
   screen_width  = 640;
   screen_height = 480;
   // Logging
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;
   else
      perf_get_cpu_features_cb = NULL;

   // Set color mode
   unsigned color_mode = RETRO_PIXEL_FORMAT_XRGB8888;
   environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &color_mode);

   environ_cb(RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB, &frontend_clear_thread_waits_cb);

   init_kb_map();
   struct retro_keyboard_callback kb_callback = { &retro_keyboard_event };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_callback);
}

void retro_deinit(void)
{
   first_run = true;

   //When auto-save states are enabled this is needed to prevent the core from shutting down before
   //any save state actions are still running - which results in partial saves
   mtx_serialization.Lock() ;
   mtx_serialization.Unlock() ;

}

static bool is_dupe = false;
extern int GDROM_TICK;
static bool rotate_screen = false;

static void update_variables(bool first_startup)
{
   struct retro_variable var;
   int i ;
   char key[256] ;

   var.key = "reicast_widescreen_hack";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         settings.rend.WideScreen = 1;
      else
         settings.rend.WideScreen = 0;
   }
   else
      settings.rend.WideScreen = 0;

   var.key = "reicast_screen_rotation";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp("vertical", var.value))
   {
	  rotate_screen = true;
	  settings.rend.WideScreen = 0;
   }

   var.key = "reicast_internal_resolution";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char *pch;
      char str[100];
      snprintf(str, sizeof(str), "%s", var.value);

      pch = strtok(str, "x");
      if (pch)
         screen_width = strtoul(pch, NULL, 0);
      pch = strtok(NULL, "x");
      if (pch)
         screen_height = strtoul(pch, NULL, 0);

      if (settings.rend.WideScreen)
      {
         if (!strcmp(var.value, "640x480"))
         {
            screen_width  = 854;
         }
         else if (!strcmp(var.value, "1280x960"))
         {
            screen_width  = 1536;
         }
         else if (!strcmp(var.value, "1920x1440"))
            screen_height = 1200;
         else if (!strcmp(var.value, "2560x1920"))
            screen_width = 3200;
      }

      fprintf(stderr, "[reicast]: Got size: %u x %u.\n", screen_width, screen_height);
   }


   var.key = "reicast_cpu_mode";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "dynamic_recompiler"))
         settings.dynarec.Type = 0;
      else if (!strcmp(var.value, "generic_recompiler"))
         settings.dynarec.Type = 1;
   }

   var.key = "reicast_boot_to_bios";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         boot_to_bios = true;
      else if (!strcmp(var.value, "disabled"))
         boot_to_bios = false;
   }
   else
      boot_to_bios = false;

   var.key = "reicast_extra_depth_scale";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
      {
         settings.rend.AutoExtraDepthScale    = true;
         settings.rend.ExtraDepthScale        = 1.f;
      }
      else if (!strcmp(var.value, "enabled"))
      {
         settings.rend.AutoExtraDepthScale    = false;
         settings.rend.ExtraDepthScale        = 1e26;
      }
      else
      {
         settings.rend.AutoExtraDepthScale    = false;
         settings.rend.ExtraDepthScale        = 1.f;
      }
   }

   var.key = "reicast_gdrom_fast_loading";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         GDROM_TICK      = 0001000;
      else if (!strcmp(var.value, "disabled"))
         GDROM_TICK      = 1500000;
   }
   else
      GDROM_TICK      = 1500000;

   var.key = "reicast_alpha_sorting";
   int previous_renderer = settings.pvr.rend;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "per-strip (fast, least accurate)"))
      {
    	 settings.pvr.rend = 0;
         settings.pvr.Emulation.AlphaSortMode = 1;
      }
      else if (!strcmp(var.value, "per-triangle (normal)"))
      {
    	 settings.pvr.rend = 0;
         settings.pvr.Emulation.AlphaSortMode = 0;
      }
      else if (!strcmp(var.value, "per-pixel (accurate)"))
      {
    	 settings.pvr.rend = 3;
         settings.pvr.Emulation.AlphaSortMode = 0;	// Not used
      }
   }
   else
   {
	  settings.pvr.rend = 0;
      settings.pvr.Emulation.AlphaSortMode = 0;
   }
   if (!first_startup && previous_renderer != settings.pvr.rend)
	  renderer_changed = true;

   var.key = "reicast_mipmapping";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         settings.rend.UseMipmaps      = 1;
      else if (!strcmp(var.value, "disabled"))
         settings.rend.UseMipmaps      = 0;
   }
   else
      settings.rend.UseMipmaps      = 1;

   if (first_startup)
   {
      var.key = "reicast_system";

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "auto"))
            settings.System = DC_PLATFORM_DREAMCAST;
         else if (!strcmp(var.value, "dreamcast"))
            settings.System = DC_PLATFORM_DREAMCAST;
         else if (!strcmp(var.value, "naomi"))
            settings.System = DC_PLATFORM_NAOMI;
      }
      else
         settings.System = DC_PLATFORM_DREAMCAST;

#ifdef HAVE_OIT
      extern GLuint pixel_buffer_size;
      var.key = "reicast_oit_abuffer_size";

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "512MB"))
            pixel_buffer_size = 0x20000000u;
         else if (!strcmp(var.value, "1GB"))
            pixel_buffer_size = 0x40000000u;
         else if (!strcmp(var.value, "2GB"))
            pixel_buffer_size = 0x80000000u;
         else
            pixel_buffer_size = 0x20000000u;
      }
      else
         pixel_buffer_size = 0x20000000u;
#endif
   }

   var.key = "reicast_volume_modifier_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         settings.pvr.Emulation.ModVol      = false;
      else if (!strcmp(var.value, "enabled"))
         settings.pvr.Emulation.ModVol      = true;
   }
   else
      settings.pvr.Emulation.ModVol      = true;


   var.key = "reicast_audio_buffer_size";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      settings.aica.BufferSize = atoi(var.value);
   }
   else
      settings.aica.BufferSize = 1024;

   var.key = "reicast_cable_type";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("VGA (RGB)", var.value))
         settings.dreamcast.cable = 0;
      else if (!strcmp("TV (RGB)", var.value))
         settings.dreamcast.cable = 2;
      else if (!strcmp("TV (VBS/Y+S/C)", var.value))
         settings.dreamcast.cable = 3;
   }

   var.key = "reicast_broadcast";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("NTSC", var.value))
         settings.dreamcast.broadcast = 0;
      else if (!strcmp("PAL", var.value))
         settings.dreamcast.broadcast = 1;
      else if (!strcmp("PAL_M", var.value))
         settings.dreamcast.broadcast = 2;
      else if (!strcmp("PAL_N", var.value))
         settings.dreamcast.broadcast = 3;
      else if (!strcmp("Default", var.value))
         settings.dreamcast.broadcast = 4;
   }
   else
         settings.dreamcast.broadcast = 4;

   var.key = "reicast_framerate";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("normal", var.value))
         settings.UpdateMode = 0;
      else if (!strcmp("fullspeed", var.value))
         settings.UpdateMode = 1;
   }
   else
      settings.UpdateMode = 0;

   var.key = "reicast_region";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("Japan", var.value))
         settings.dreamcast.region = 0;
      else if (!strcmp("USA", var.value))
         settings.dreamcast.region = 1;
      else if (!strcmp("Europe", var.value))
         settings.dreamcast.region = 2;
      else if (!strcmp("Default", var.value))
         settings.dreamcast.region = 3;
   }
   else
         settings.dreamcast.region = 3;

   var.key = "reicast_div_matching";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("auto", var.value))
      {
         settings.dynarec.DisableDivMatching = 0;
         settings.dynarec.AutoDivMatching = true;
      }
      else if (!strcmp("enabled", var.value))
      {
         settings.dynarec.DisableDivMatching = 0;
         settings.dynarec.AutoDivMatching = false;
      }
      else if (!strcmp("disabled", var.value))
      {
         settings.dynarec.DisableDivMatching = 1;
         settings.dynarec.AutoDivMatching = false;
      }
   }

#ifdef HAVE_TEXUPSCALE
   var.key = "reicast_texupscale";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("off", var.value))
         settings.rend.TextureUpscale = 1;
      else if (!strcmp("2x", var.value))
         settings.rend.TextureUpscale = 2;
      else if (!strcmp("4x", var.value))
         settings.rend.TextureUpscale = 4;
      else if (!strcmp("6x", var.value))
         settings.rend.TextureUpscale = 6;
   }
   else if (first_startup)
      settings.rend.TextureUpscale = 1;

   var.key = "reicast_texupscale_max_filtered_texture_size";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("256", var.value))
         settings.rend.MaxFilteredTextureSize = 256;
      else if (!strcmp("512", var.value))
         settings.rend.MaxFilteredTextureSize = 512;
      else if (!strcmp("1024", var.value))
         settings.rend.MaxFilteredTextureSize = 1024;
   }
   else
      settings.rend.MaxFilteredTextureSize = 256;
#endif

   var.key = "reicast_enable_rtt";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("enabled", var.value))
         settings.rend.RenderToTexture = true;
      else
         settings.rend.RenderToTexture = false;
   }
   else
         settings.rend.RenderToTexture = true;

   var.key = "reicast_enable_rttb";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("enabled", var.value))
         settings.rend.RenderToTextureBuffer = true;
      else
         settings.rend.RenderToTextureBuffer = false;
   }
   else
      settings.rend.RenderToTextureBuffer = false;

   var.key = "reicast_render_to_texture_upscaling";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char *pch;
      char str[100];
      sprintf(str, "%s", var.value);

      pch = strtok(str, "x");
      if (pch)
         settings.rend.RenderToTextureUpscale = strtoul(pch, NULL, 0);
   }
   else if (first_startup)
      settings.rend.RenderToTextureUpscale = 1;

#if !defined(TARGET_NO_THREADS)
   if (first_startup)
   {
	   bool save_state_in_background = true ;
	   var.key = "reicast_threaded_rendering";

	   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	   {
		   if (!strcmp("enabled", var.value))
			   settings.rend.ThreadedRendering = true;
		   else
			   settings.rend.ThreadedRendering = false;
	   }
	   else
		   settings.rend.ThreadedRendering = false;

	   if ( settings.rend.ThreadedRendering  )
		   environ_cb(RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND, &save_state_in_background);

   }
#endif

   var.key = "reicast_enable_purupuru";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      enable_purupuru = (strcmp("enabled", var.value) == 0);

   var.key = "reicast_analog_stick_deadzone";
   var.value = NULL;

   if ( environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value )
      input_set_deadzone_stick( atoi( var.value ) );

   var.key = "reicast_trigger_deadzone";
   var.value = NULL;

   if ( environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value )
      input_set_deadzone_trigger( atoi( var.value ) );

   var.key = "reicast_enable_dsp";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("enabled", var.value))
      {
         settings.aica.DSPEnabled = true;
         settings.aica.NoBatch    = 1;
      }
      else
      {
         settings.aica.DSPEnabled = false;
         settings.aica.NoBatch    = 0;
      }
   }
   else if (first_run)
   {
      settings.aica.DSPEnabled = true;
      settings.aica.NoBatch    = 1;
   }

   var.key = "reicast_digital_triggers";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("enabled", var.value))
         digital_triggers = true;
      else
         digital_triggers = false;
   }
   else
      digital_triggers = false;

   var.key = "reicast_allow_service_buttons";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("enabled", var.value))
         allow_service_buttons = true;
      else
         allow_service_buttons = false;
   }
   else
      allow_service_buttons = false;

   key[0] = '\0' ;

   var.key = key ;
   for ( i = 0 ; i < 4 ; i++)
   {
      vmu_screen_params[i].vmu_screen_display = false ;
      vmu_screen_params[i].vmu_screen_position = UPPER_LEFT ;
      vmu_screen_params[i].vmu_screen_size_mult = 1 ;
      vmu_screen_params[i].vmu_pixel_on_R = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_ON].r ;
      vmu_screen_params[i].vmu_pixel_on_G = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_ON].g ;
      vmu_screen_params[i].vmu_pixel_on_B = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_ON].b ;
      vmu_screen_params[i].vmu_pixel_off_R = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_OFF].r ;
      vmu_screen_params[i].vmu_pixel_off_G = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_OFF].g ;
      vmu_screen_params[i].vmu_pixel_off_B = VMU_SCREEN_COLOR_MAP[VMU_DEFAULT_OFF].b ;
      vmu_screen_params[i].vmu_screen_opacity = 0xFF ;
      vmu_screen_params[i].vmu_screen_needs_update = true ;

      snprintf(key, sizeof(key), "reicast_vmu%d_screen_display", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp("enabled", var.value) )
       	 vmu_screen_params[i].vmu_screen_display = true ;


      snprintf(key, sizeof(key), "reicast_vmu%d_screen_position", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value  )
      {
         if (!strcmp("Upper Left", var.value))
        	 vmu_screen_params[i].vmu_screen_position = UPPER_LEFT;
         else if (!strcmp("Upper Right", var.value))
        	 vmu_screen_params[i].vmu_screen_position = UPPER_RIGHT;
         else if (!strcmp("Lower Left", var.value))
        	 vmu_screen_params[i].vmu_screen_position = LOWER_LEFT;
         else if (!strcmp("Lower Right", var.value))
        	 vmu_screen_params[i].vmu_screen_position = LOWER_RIGHT;
      }

      snprintf(key, sizeof(key), "reicast_vmu%d_screen_size_mult", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value  )
      {
         if (!strcmp("1x", var.value))
        	 vmu_screen_params[i].vmu_screen_size_mult = 1;
         else if (!strcmp("2x", var.value))
        	 vmu_screen_params[i].vmu_screen_size_mult = 2;
         else if (!strcmp("3x", var.value))
        	 vmu_screen_params[i].vmu_screen_size_mult = 3;
         else if (!strcmp("4x", var.value))
        	 vmu_screen_params[i].vmu_screen_size_mult = 4;
         else if (!strcmp("5x", var.value))
        	 vmu_screen_params[i].vmu_screen_size_mult = 5;
      }

      snprintf(key, sizeof(key), "reicast_vmu%d_screen_opacity", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value  )
      {
         if (!strcmp("100%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 255;
         else if (!strcmp("90%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 9*25.5;
         else if (!strcmp("80%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 8*25.5;
         else if (!strcmp("70%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 7*25.5;
         else if (!strcmp("60%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 6*25.5;
         else if (!strcmp("50%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 5*25.5;
         else if (!strcmp("40%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 4*25.5;
         else if (!strcmp("30%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 3*25.5;
         else if (!strcmp("20%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 2*25.5;
         else if (!strcmp("10%", var.value))
        	 vmu_screen_params[i].vmu_screen_opacity = 1*25.5;
      }

      snprintf(key, sizeof(key), "reicast_vmu%d_pixel_on_color", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strlen(var.value)>1 )
      {
    	  int color_idx = atoi(var.value+(strlen(var.value)-2)) ;
    	  vmu_screen_params[i].vmu_pixel_on_R = VMU_SCREEN_COLOR_MAP[color_idx].r ;
    	  vmu_screen_params[i].vmu_pixel_on_G = VMU_SCREEN_COLOR_MAP[color_idx].g ;
    	  vmu_screen_params[i].vmu_pixel_on_B = VMU_SCREEN_COLOR_MAP[color_idx].b ;
      }

      snprintf(key, sizeof(key), "reicast_vmu%d_pixel_off_color", i+1) ;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strlen(var.value)>1 )
      {
    	  int color_idx = atoi(var.value+(strlen(var.value)-2)) ;
    	  vmu_screen_params[i].vmu_pixel_off_R = VMU_SCREEN_COLOR_MAP[color_idx].r ;
    	  vmu_screen_params[i].vmu_pixel_off_G = VMU_SCREEN_COLOR_MAP[color_idx].g ;
    	  vmu_screen_params[i].vmu_pixel_off_B = VMU_SCREEN_COLOR_MAP[color_idx].b ;
      }


   }

}

void retro_run (void)
{
   bool fastforward = false;
   bool updated     = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fastforward) && settings.rend.ThreadedRendering)
      settings.aica.LimitFPS = !fastforward;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering)
   {
	   // On the first call, we start the emulator thread
	   if (first_run)
	   {
		   emu_thread.Start();
		   first_run = false;
	   }
	   // Then we wait until the emulator has initialized
	   if (!emu_inited)
		   return;

	   poll_cb();

	   glsm_ctl(GLSM_CTL_STATE_BIND, NULL);

	   // Render
	   is_dupe = !rend_single_frame();

	   glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
   }
   else
#endif
   {
	   if (first_run)
	   {
		   if (dc_init(co_argc,co_argv))
		   {
			  if (log_cb)
				 log_cb(RETRO_LOG_ERROR, "Reicast emulator initialization failed\n");
			  return;
		   }
		   dc_run();
		   first_run = false;
		   return;
	   }
	   dc_run();
   }
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   video_cb(is_dupe ? 0 : RETRO_HW_FRAME_BUFFER_VALID, screen_width, screen_height, 0);
#endif
#if !defined(TARGET_NO_THREADS)
   if (!settings.rend.ThreadedRendering)
#endif
	   is_dupe = true;
}

void retro_reset (void)
{
#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering)
   {
	   dc_stop();
	   emu_inited = false;
   }
   else
#endif
	   dc_term();
   first_run = true;
   settings.dreamcast.cable = 3;
   update_variables(false);
}

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
static void context_reset(void)
{
   printf("context_reset.\n");
   gl_ctx_resetting = false;
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);
   glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);
}

static void context_destroy(void)
{
   gl_ctx_resetting = true;
   renderer_changed = true;
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
}
#endif

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

extern void dc_prepare_system(void);

static void set_input_descriptors()
{
   struct retro_input_descriptor desc[18 * 4 + 1];
   int descriptor_index = 0;
   if (settings.System == DC_PLATFORM_NAOMI)
   {
      for (unsigned i = 0; i < 4; i++)
      {
    	 switch (maple_devices[i])
    	 {
    	 case MDT_LightGun:
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT,  "D-Pad Left" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP,    "D-Pad Up" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN,  "D-Pad Down" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT, "D-Pad Right" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER,	   "Trigger" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A,      "Button 2" },
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B,      "Button 3" },
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_C,      "Button 4" },
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD,     "Button 5" },
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT,     "Coin" },
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START,      "Start" };
    		break;

    	 default:
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Button 1" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Button 2" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Button 3" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Button 4" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Button 5" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Button 6" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Test" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Service" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Axis 1" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Axis 2" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Axis 3" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Axis 4" };
    		break;
    	 }
      }
   }
   else
   {
      for (unsigned i = 0; i < 4; i++)
      {
    	 switch (maple_devices[i])
    	 {
    	 case MDT_SegaController:
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L Trigger" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R Trigger" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" };
    		break;

    	 case MDT_LightGun:
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT,  "D-Pad Left" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP,    "D-Pad Up" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN,  "D-Pad Down" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT, "D-Pad Right" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER,	   "A" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START,      "Start" };
    		desc[descriptor_index++] = { i, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A,      "B" };
    		break;
    	 }
      }
   }
   desc[descriptor_index++] = { 0 };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, slash);
   if (!base)
      base = path;

   if (*base == slash)
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';
}

// Loading/unloading games
bool retro_load_game(const struct retro_game_info *game)
{
   glsm_ctx_params_t params = {0};
   const char *dir = NULL;
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

   extract_basename(g_base_name, game->path, sizeof(g_base_name));
   extract_directory(game_dir, game->path, sizeof(game_dir));

   // Storing rom dir for later use
   char g_roms_dir[PATH_MAX];
   snprintf(g_roms_dir, sizeof(g_roms_dir), "%s%c", game_dir, slash);

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && log_cb)
        log_cb(RETRO_LOG_INFO, "Rumble interface supported!\n");

   if (!(environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir))
      dir = game_dir;

   snprintf(game_dir, sizeof(game_dir), "%s%cdc%c", dir, slash, slash);
   snprintf(game_dir_no_slash, sizeof(game_dir_no_slash), "%s%cdc", dir, slash);

   settings.dreamcast.cable = 3;
   update_variables(true);

   {
      /* Check for extension .lst. If found, we will set the system type
       * automatically to Naomi. */
      char *ext = strrchr(g_base_name, '.');
      if (ext)
      {
         log_cb(RETRO_LOG_INFO, "File extension is: %s\n", ext);
         if (!strcmp(".lst", ext))
            settings.System = DC_PLATFORM_NAOMI;
      }
   }

   if (game->path[0] == '\0')
      boot_to_bios = true;

   if (!boot_to_bios)
      game_data = strdup(game->path);

   {
      char data_dir[1024];

      snprintf(data_dir, sizeof(data_dir), "%s%s", game_dir, "data");

      printf("Creating dir: %s\n", data_dir);
      struct stat buf;
      if (stat(data_dir, &buf) < 0)
      {
         mkdir_norecurse(data_dir);
      }
   }
   int rotation = rotate_screen ? 1 : 0;
   environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

   params.context_type          = RETRO_HW_CONTEXT_NONE;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   params.context_reset         = context_reset;
   params.context_destroy       = context_destroy;
   params.environ_cb            = environ_cb;
#ifdef TARGET_NO_STENCIL
   params.stencil               = false;
#else
   params.stencil               = true;
#endif
   params.imm_vbo_draw          = NULL;
   params.imm_vbo_disable       = NULL;
#ifdef HAVE_OIT
   if (settings.pvr.rend == 3)
   {
	  params.context_type          = RETRO_HW_CONTEXT_OPENGL_CORE;
	  params.major                 = 4;
	  params.minor                 = 3;
   }
   else
   {
	   params.context_type          = RETRO_HW_CONTEXT_OPENGL_CORE;
	   params.major                 = 3;
	   params.minor                 = 0;
   }
#elif defined(HAVE_GL3)
   params.context_type          = RETRO_HW_CONTEXT_OPENGL_CORE;
   params.major                 = 3;
   params.minor                 = 0;
#endif

   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
   {
#if defined(HAVE_GL3)
      params.context_type       = RETRO_HW_CONTEXT_OPENGL_CORE;
      params.major              = 3;
      params.minor              = 0;
#else
      params.context_type       = RETRO_HW_CONTEXT_OPENGL;
      params.major              = 0;
      params.minor              = 0;
#endif
      if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
         return false;
   }
#endif

   if (settings.System == DC_PLATFORM_NAOMI)
   {
      if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) {
         char g_save_dir[PATH_MAX];
         strncpy(g_save_dir, dir, sizeof(g_save_dir));
         if(strcmp(g_save_dir,g_roms_dir) != 0)
            snprintf(save_dir, sizeof(save_dir), "%s%creicast%c", g_save_dir, slash, slash);
         else
            strncpy(save_dir, g_roms_dir, sizeof(save_dir));
         struct stat buf;
         if (stat(save_dir, &buf) < 0)
         {
            log_cb(RETRO_LOG_INFO, "Creating dir: %s\n", save_dir);
            mkdir_norecurse(save_dir);
         }
      } else {
         strncpy(save_dir, g_roms_dir, sizeof(save_dir));
      }
      log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", save_dir);
      snprintf(eeprom_file, sizeof(eeprom_file), "%s%s.eeprom", save_dir, g_base_name);
      snprintf(nvmem_file, sizeof(nvmem_file), "%s%s.nvmem", save_dir, g_base_name);
   }

   dc_prepare_system();

   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   return false; //TODO (?)
}

void retro_unload_game(void)
{
	printf("reicast unloading game\n") ;
   if (game_data)
      free(game_data);
   game_data = NULL;

   dc_stop();
#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering)
   {
	   rend_cancel_emu_wait();
	   printf("Waiting for emu thread......\n");
	   if ( emu_in_thread )
	   {
		   frontend_clear_thread_waits_cb(1,NULL) ;
		   printf("Waiting for emu thread to end...\n");
		   emu_thread.WaitToEnd();
		   frontend_clear_thread_waits_cb(0,NULL) ;
	   }
	   printf("...Done\n");
   }
   else
#endif
	   dc_term();
}


// Memory/Serialization
void *retro_get_memory_data(unsigned type)
{
#if !defined(TARGET_NO_THREADS)
   if ( settings.rend.ThreadedRendering && !emu_inited )
	   return 0 ;
#endif

   if ( type == RETRO_MEMORY_SYSTEM_RAM )
      return mem_b.data;
   return 0; //TODO
}

size_t retro_get_memory_size(unsigned type)
{
#if !defined(TARGET_NO_THREADS)
   if ( settings.rend.ThreadedRendering && !emu_inited )
	   return 0 ;
#endif
   if ( type == RETRO_MEMORY_SYSTEM_RAM )
      return mem_b.size;
   return 0; //TODO
}

size_t retro_serialize_size (void)
{
   unsigned int total_size = 0 ;
   void *data = NULL ;

   dc_serialize(&data, &total_size) ;

   return total_size;
}

bool wait_until_dc_running()
{
	retro_time_t start_time = perf_cb.get_time_usec() ;
	const retro_time_t FIVE_SECONDS = 5*1000000 ;
	while(!dc_is_running())
	{
		if ( start_time+FIVE_SECONDS < perf_cb.get_time_usec() )
		{
			//timeout elapsed - dc not getting a chance to run - just bail
			return false ;
		}
	}
	return true ;
}

bool acquire_mainloop_lock()
{
	bool result = false ;
	retro_time_t start_time = perf_cb.get_time_usec() ;
	const retro_time_t FIVE_SECONDS = 5*1000000 ;

    while ( ( start_time+FIVE_SECONDS > perf_cb.get_time_usec() ) && !(result = mtx_mainloop.TryLock())  )
   	{
    	rend_cancel_emu_wait();
   	}

    return result ;
}

bool retro_serialize(void *data, size_t size)
{
   unsigned int total_size = 0 ;
   void *data_ptr = data ;
   bool result = false ;

#if !defined(TARGET_NO_THREADS)
   if ( settings.rend.ThreadedRendering && !emu_inited )
	   return false ;

    if (settings.rend.ThreadedRendering && emu_inited)
    {
    	mtx_serialization.Lock() ;
    	if ( !wait_until_dc_running()) {
        	mtx_serialization.Unlock() ;
        	return false ;
    	}

  		dc_stop() ;
  		if ( !acquire_mainloop_lock() )
  		{
  			dc_start() ;
        	mtx_serialization.Unlock() ;
  			return false ;
  		}
    }
#endif

   result = dc_serialize(&data_ptr, &total_size) ;
   performed_serialization = true ;

#if !defined(TARGET_NO_THREADS)
    if (settings.rend.ThreadedRendering && emu_inited)
    {
    	mtx_mainloop.Unlock() ;
    	mtx_serialization.Unlock() ;
    }
#endif

    return result ;
}

bool retro_unserialize(const void * data, size_t size)
{
   unsigned int total_size = 0 ;
   void *data_ptr = (void*)data ;
   bool result = false ;
   int i ;

#if !defined(TARGET_NO_THREADS)
   if ( settings.rend.ThreadedRendering && !emu_inited )
	   return false ;
#endif

#if !defined(TARGET_NO_THREADS)
    if (settings.rend.ThreadedRendering && emu_inited)
    {
    	mtx_serialization.Lock() ;
    	if ( !wait_until_dc_running()) {
        	mtx_serialization.Unlock() ;
        	return false ;
    	}
  		dc_stop() ;
  		if ( !acquire_mainloop_lock() )
  		{
  			dc_start() ;
        	mtx_serialization.Unlock() ;
  			return false ;
  		}
    }
#endif

    bm_Reset() ;
#if FEAT_AREC == DYNAREC_JIT
    FlushCache();
#endif

    result = dc_unserialize(&data_ptr, &total_size, size) ;

    for ( i = 0 ; i < 4 ; i++)
       vmu_screen_params[i].vmu_screen_needs_update = true ;

    performed_serialization = true ;

#if !defined(TARGET_NO_THREADS)
    if (settings.rend.ThreadedRendering && emu_inited)
    {
    	mtx_mainloop.Unlock() ;
    	mtx_serialization.Unlock() ;
    }
#endif

    return result ;
}

// Cheats
void retro_cheat_reset(void)
{
   // Nothing to do here
}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2)
{
   // Nothing to do here
}


// Get info
const char* retro_get_system_directory(void)
{
   const char* dir;
   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);
   return dir ? dir : ".";
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "Reicast";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = "0.1" GIT_VERSION;
   info->valid_extensions = "chd|cdi|iso|elf|bin|cue|gdi|lst";
   info->need_fullpath = true;
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   /*                        00=VGA    01=NTSC   10=PAL,   11=illegal/undocumented */
   const int spg_clks[4] = { 26944080, 13458568, 13462800, 26944080 };
   u32 pixel_clock= spg_clks[(SPG_CONTROL.full >> 6) & 3];

   info->geometry.aspect_ratio = settings.rend.WideScreen ? (16.0 / 9.0) : (4.0 / 3.0);
   if (rotate_screen)
   {
	  info->geometry.base_width   = screen_height;
	  info->geometry.base_height  = screen_width;
	  info->geometry.max_width    = screen_height;
	  info->geometry.max_height   = screen_width;
	  info->geometry.aspect_ratio = 1 / info->geometry.aspect_ratio;
   }
   else
   {
	  info->geometry.base_width   = screen_width;
	  info->geometry.base_height  = screen_height;
	  info->geometry.max_width    = screen_width;
	  info->geometry.max_height   = screen_height;
   }

   switch (pixel_clock)
   {
      case 26944080:
         info->timing.fps = 60.00; /* (VGA  480 @ 60.00) */
         break;
      case 26917135:
         info->timing.fps = 59.94; /* (NTSC 480 @ 59.94) */
         break;
      case 13462800:
         info->timing.fps = 50.00; /* (PAL 240  @ 50.00) */
         break;
      case 13458568:
         info->timing.fps = 59.94; /* (NTSC 240 @ 59.94) */
         break;
      case 25925600:
         info->timing.fps = 50.00; /* (PAL 480  @ 50.00) */
         break;
   }

   info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region (void)
{
   return RETRO_REGION_NTSC; //TODO
}


// Controller
void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   if (in_port < MAPLE_PORTS)
   {
	  switch (device)
	  {
	  case RETRO_DEVICE_JOYPAD:
		 maple_devices[in_port] = MDT_SegaController;
		 break;
	  case RETRO_DEVICE_KEYBOARD:
		 maple_devices[in_port] = MDT_Keyboard;
		 break;
	  case RETRO_DEVICE_MOUSE:
		 maple_devices[in_port] = MDT_Mouse;
		 break;
	  case RETRO_DEVICE_LIGHTGUN:
		 maple_devices[in_port] = MDT_LightGun;
		 break;
	  default:
		 maple_devices[in_port] = MDT_None;
		 break;
	  }
   }
   //TODO
   if (rumble.set_rumble_state)
   {
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_STRONG, 0);
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_WEAK,   0);
   }
   set_input_descriptors();
}


// API version (to detect version mismatch)
unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

//Reicast stuff
void os_DoEvents(void)
{
#if !defined(TARGET_NO_THREADS)
	if (!settings.rend.ThreadedRendering)
#endif
	{
		is_dupe = false;
		poll_cb();

		if (settings.UpdateMode || settings.UpdateModeForced)
		{
			rend_end_render();
			dc_stop();
		}
	}
}

void os_CreateWindow()
{
   // Nothing to do here
}

static void get_analog_stick( retro_input_state_t input_state_cb,
                       int player_index,
                       int stick,
                       s8* p_analog_x,
                       s8* p_analog_y )
{
   int analog_x, analog_y;
   analog_x = input_state_cb( player_index, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_X );
   analog_y = input_state_cb( player_index, RETRO_DEVICE_ANALOG, stick, RETRO_DEVICE_ID_ANALOG_Y );

   // Analog stick deadzone (borrowed code from parallel-n64 core)
   if ( astick_deadzone > 0 )
   {
      static const int ASTICK_MAX = 0x8000;

      // Convert cartesian coordinate analog stick to polar coordinates
      double radius = sqrt(analog_x * analog_x + analog_y * analog_y);
      double angle = atan2(analog_y, analog_x);

      if (radius > astick_deadzone)
      {
         // Re-scale analog stick range to negate deadzone (makes slow movements possible)
         radius = (radius - astick_deadzone)*((float)ASTICK_MAX/(ASTICK_MAX - astick_deadzone));

         // Convert back to cartesian coordinates
         analog_x = (int)round(radius * cos(angle));
         analog_y = (int)round(radius * sin(angle));

         // Clamp to correct range
         if (analog_x > +32767) analog_x = +32767;
         if (analog_x < -32767) analog_x = -32767;
         if (analog_y > +32767) analog_y = +32767;
         if (analog_y < -32767) analog_y = -32767;
      }
      else
      {
         analog_x = 0;
         analog_y = 0;
      }
   }

   // output
   *p_analog_x = (s8)(analog_x >> 8);
   *p_analog_y = (s8)(analog_y >> 8);
}

static uint16_t apply_trigger_deadzone( uint16_t input )
{
   if ( trigger_deadzone > 0 )
   {
      static const int TRIGGER_MAX = 0x8000;
      const float scale = ((float)TRIGGER_MAX/(float)(TRIGGER_MAX - trigger_deadzone));

      if ( input > trigger_deadzone )
      {
         // Re-scale analog range
         float scaled = (input - trigger_deadzone)*scale;

         input = (int)round(scaled);
         if (input > +32767) {
            input = +32767;
         }
      }
      else
      {
         input = 0;
      }
   }

   return input;
}

static uint16_t get_analog_trigger( retro_input_state_t input_state_cb,
                           int player_index,
                           int id )
{
   uint16_t trigger;

   // NOTE: Analog triggers were added Nov 2017. Not all front-ends support this
   // feature (or pre-date it) so we need to handle this in a graceful way.

   // First, try and get an analog value using the new libretro API constant
   trigger = input_state_cb( player_index,
                       RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                       id );

   if ( trigger == 0 )
   {
      // If we got exactly zero, we're either not pressing the button, or the front-end
      // is not reporting analog values. We need to do a second check using the classic
      // digital API method, to at least get some response - better than nothing.

      // NOTE: If we're really just not holding the trigger, we're still going to get zero.

      trigger = input_state_cb( player_index,
                          RETRO_DEVICE_JOYPAD,
                          0,
                          id ) ? 0x7FFF : 0;
   }
   else
   {
      // We got something, which means the front-end can handle analog buttons.
      // So we apply a deadzone to the input and use it.

      trigger = apply_trigger_deadzone( trigger );
   }

   return trigger;
}

static void setDeviceButtonState(u32 port, int deviceType, int btnId, const uint16_t joymap[])
{
   uint16_t dc_key = joymap[btnId];
   bool is_down = input_cb(port, deviceType, 0, btnId);
   if (is_down)
	  kcode[port] &= ~dc_key;
   else
	  kcode[port] |= dc_key;
}

static void UpdateInputStateNaomi(u32 port)
{
   int id;
   int max_id;

   static const uint16_t joymap[] =
   {
      /* JOYPAD_B      */ NAOMI_BTN0_KEY, /* BTN1 */
      /* JOYPAD_Y      */ NAOMI_BTN2_KEY, /* BTN3 */
      /* JOYPAD_SELECT */ NAOMI_COIN_KEY,
      /* JOYPAD_START  */ NAOMI_START_KEY,
      /* JOYPAD_UP     */ NAOMI_UP_KEY,
      /* JOYPAD_DOWN   */ NAOMI_DOWN_KEY,
      /* JOYPAD_LEFT   */ NAOMI_LEFT_KEY,
      /* JOYPAD_RIGHT  */ NAOMI_RIGHT_KEY,
      /* JOYPAD_A      */ NAOMI_BTN1_KEY, /* BTN2 */
      /* JOYPAD_X      */ NAOMI_BTN3_KEY, /* BTN4 */
      /* JOYPAD_L      */ NAOMI_BTN5_KEY, /* BTN6 */
      /* JOYPAD_R      */ NAOMI_BTN4_KEY, /* BTN5 */
      /* JOYPAD_L2     */ 0,
      /* JOYPAD_R2     */ 0,
      /* JOYPAD_L3     */ NAOMI_TEST_KEY,
      /* JOYPAD_R3     */ NAOMI_SERVICE_KEY,
   };

   static const uint16_t lg_joymap[] =
   {
	   /* deprecated */ 			0,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_TRIGGER */	NAOMI_BTN0_KEY,
	   /* LIGHTGUN_AUX_A */		NAOMI_BTN1_KEY,
	   /* LIGHTGUN_AUX_B */ 	NAOMI_BTN2_KEY,
	   /* deprecated */ 			0,
	   /* LIGHTGUN_START */		NAOMI_START_KEY,
	   /* LIGHTGUN_SELECT */ 	NAOMI_COIN_KEY,
	   /* LIGHTGUN_AUX_C */		NAOMI_BTN3_KEY,
	   /* LIGHTGUN_UP   */ 		NAOMI_UP_KEY,
	   /* LIGHTGUN_DOWN   */ 	NAOMI_DOWN_KEY,
	   /* LIGHTGUN_LEFT   */ 	NAOMI_LEFT_KEY,
	   /* LIGHTGUN_RIGHT  */ 	NAOMI_RIGHT_KEY,
   };

   switch (maple_devices[port])
   {
   case MDT_LightGun:
	  {
		 //
		 // -- buttons
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_A, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_B, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_C, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_START, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_SELECT, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT, lg_joymap);

		 bool force_offscreen = false;

		 if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
		 {
			force_offscreen = true;
			kcode[port] &= ~NAOMI_BTN0_KEY;
		 }

		 if (force_offscreen || input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
		 {
			mo_x_abs[port] = -1;
			mo_y_abs[port] = -1;
		 }
		 else
		 {
			int x = input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
			int y = input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
			mo_x_abs[port] = (x + 0x8000) * 640.f / 0x10000;
			mo_y_abs[port] = (y + 0x8000) * 480.f / 0x10000;
		 }
	  }
	  break;

   default:
	  //
	  // -- buttons

	  max_id = (allow_service_buttons ? RETRO_DEVICE_ID_JOYPAD_R3 : RETRO_DEVICE_ID_JOYPAD_R2);
	  for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= max_id; ++id)
	  {
		 setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, id, joymap);
	  }
	  //
	  // -- analog stick

	  get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_LEFT, &(joyx[port]), &(joyy[port]) );
	  get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, (s8*)&rt[port], (s8 *)&lt[port] );

	  // -- mouse, for rotary encoders
	  mo_x_delta[port] = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	  mo_y_delta[port] = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
	  break;
   }
}

void UpdateInputState(u32 port)
{
   if (gl_ctx_resetting)
	  return;

   if (settings.System == DC_PLATFORM_NAOMI)
   {
      UpdateInputStateNaomi(0);
      UpdateInputStateNaomi(1);
      UpdateInputStateNaomi(2);
      UpdateInputStateNaomi(3);
      return;
   }

   static const uint16_t joymap[] =
   {
      /* JOYPAD_B      */ DC_BTN_A,
      /* JOYPAD_Y      */ DC_BTN_X,
      /* JOYPAD_SELECT */ 0,
      /* JOYPAD_START  */ DC_BTN_START,
      /* JOYPAD_UP     */ DC_DPAD_UP,
      /* JOYPAD_DOWN   */ DC_DPAD_DOWN,
      /* JOYPAD_LEFT   */ DC_DPAD_LEFT,
      /* JOYPAD_RIGHT  */ DC_DPAD_RIGHT,
      /* JOYPAD_A      */ DC_BTN_B,
      /* JOYPAD_X      */ DC_BTN_Y,
   };

   switch (maple_devices[port])
   {
	  case MDT_SegaController:
	  {
		   int id;
		   //
		   // -- buttons

		   for (id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_X; ++id)
		   {
			  setDeviceButtonState(port, RETRO_DEVICE_JOYPAD, id, joymap);
		   }

		   //
		   // -- analog stick

		   get_analog_stick( input_cb, port, RETRO_DEVICE_INDEX_ANALOG_LEFT, &(joyx[port]), &(joyy[port]) );


		   //
		   // -- triggers

		   if ( digital_triggers )
		   {
		      // -- digital left trigger
		      if ( input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) )
		         lt[port]=0xFF;
		      else if ( input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) )
		         lt[port]=0x7F;
		      else
		         lt[port]=0;
		      // -- digital right trigger
		      if ( input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) )
		         rt[port]=0xFF;
		      else if ( input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) )
		         rt[port]=0x7F;
		      else
		         rt[port]=0;
		   }
		   else
		   {
			   // -- analog triggers
			   lt[port] = get_analog_trigger( input_cb, port, RETRO_DEVICE_ID_JOYPAD_L2 ) / 128;
			   rt[port] = get_analog_trigger( input_cb, port, RETRO_DEVICE_ID_JOYPAD_R2 ) / 128;
		   }
	  }
	  break;

	  case MDT_LightGun:
	  {
		 static const uint16_t lg_joymap[] =
		 {
			  /* deprecated */ 			0,
		      /* deprecated */ 			0,
		      /* LIGHTGUN_TRIGGER */	DC_BTN_A,
		      /* LIGHTGUN_AUX_A */		DC_BTN_B,
		      /* LIGHTGUN_AUX_B */ 		0,
			  /* deprecated */ 			0,
			  /* LIGHTGUN_START */		DC_BTN_START,
			  /* LIGHTGUN_SELECT */ 	0,
			  /* LIGHTGUN_AUX_C */		0,
		      /* LIGHTGUN_UP   */ 		DC_DPAD_UP,
		      /* LIGHTGUN_DOWN   */ 	DC_DPAD_DOWN,
		      /* LIGHTGUN_LEFT   */ 	DC_DPAD_LEFT,
		      /* LIGHTGUN_RIGHT  */ 	DC_DPAD_RIGHT,
		 };

		 //
		 // -- buttons
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_AUX_A, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_START, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT, lg_joymap);
		 setDeviceButtonState(port, RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT, lg_joymap);

		 bool force_offscreen = false;

		 if (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD))
		 {
			force_offscreen = true;
			kcode[port] &= ~DC_BTN_A;
		 }

		 if (force_offscreen || input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
		 {
			mo_x_abs[port] = -1;
			mo_y_abs[port] = -1;
		 }
		 else
		 {
			mo_x_abs[port] = (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X) + 0x8000) * 640.f / 0x10000;
			mo_y_abs[port] = (input_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) + 0x8000) * 480.f / 0x10000;
		 }
	  }
	  break;

	  case MDT_Mouse:
	  {
		 mo_x_delta[port] = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		 mo_y_delta[port] = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

		 bool btn_state = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
		 if (btn_state)
			mo_buttons[port] &= ~(1 << 2);
		 else
			mo_buttons[port] |= 1 << 2;
		 btn_state = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
		 if (btn_state)
			mo_buttons[port] &= ~(1 << 1);
		 else
			mo_buttons[port] |= 1 << 1;
		 btn_state = input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE);
		 if (btn_state)
			mo_buttons[port] &= ~(1 << 0);
		 else
			mo_buttons[port] |= 1 << 0;
		 if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
			mo_wheel_delta[port] -= 10;
		 else if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))
			mo_wheel_delta[port] += 10;
	  }
	  break;
   }
}

void UpdateVibration(u32 port, u32 value)
{
   if (!rumble.set_rumble_state)
      return;

   u8 POW_POS = (value >> 8) & 0x3;
   u8 POW_NEG = (value >> 12) & 0x3;
   u8 FREQ = (value >> 16) & 0xFF;

   double pow = (POW_POS + POW_NEG) / 7.0;
   double pow_l = pow * (0x3B - FREQ) / 17.0;
   double pow_r = pow * (FREQ - 0x07) / 15.0;

   if (pow_l > 1.0) pow_l = 1.0;
   if (pow_r > 1.0) pow_r = 1.0;

   rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, (u16)(65535 * pow_l));
   rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK,   (u16)(65535 * pow_r));
}

extern u8 kb_shift; 		// shift keys pressed (bitmask)
extern u8 kb_key[6];		// normal keys pressed (up to 6)
static int kb_used;

static void release_key(unsigned dc_keycode)
{
   if (dc_keycode == 0)
	  return;

   if (kb_used > 0)
   {
	  for (int i = 0; i < 6; i++)
	  {
		 if (kb_key[i] == dc_keycode)
		 {
			kb_used--;
			for (int j = i; j < 5; j++)
			   kb_key[j] = kb_key[j + 1];
			kb_key[5] = 0;
		 }
	  }
   }
}

void retro_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
   // Dreamcast keyboard emulation
   if (keycode == RETROK_LSHIFT || keycode == RETROK_RSHIFT)
	  if (!down)
		 kb_shift &= ~(0x02 | 0x20);
	  else
		 kb_shift |= (0x02 | 0x20);
   if (keycode == RETROK_LCTRL || keycode == RETROK_RCTRL)
	  if (!down)
		 kb_shift &= ~(0x01 | 0x10);
	  else
		 kb_shift |= (0x01 | 0x10);

   // Make sure modifier keys are released
   if ((key_modifiers & RETROKMOD_SHIFT) == 0)
   {
	  release_key(kb_map[RETROK_LSHIFT]);
	  release_key(kb_map[RETROK_LSHIFT]);
   }
   if ((key_modifiers & RETROKMOD_CTRL) == 0)
   {
	  release_key(kb_map[RETROK_LCTRL]);
	  release_key(kb_map[RETROK_RCTRL]);
   }

   u8 dc_keycode = kb_map[keycode];
   if (dc_keycode != 0)
   {
	  if (down)
	  {
		 if (kb_used < 6)
		 {
			bool found = false;
			for (int i = 0; !found && i < 6; i++)
			{
			   if (kb_key[i] == dc_keycode)
				  found = true;
			}
			if (!found)
			{
			   kb_key[kb_used] = dc_keycode;
			   kb_used++;
			}
		 }
	  }
	  else
	  {
		 release_key(dc_keycode);
	  }
   }
}

void* libPvr_GetRenderTarget()
{
   return NULL;
}

void* libPvr_GetRenderSurface()
{
   return NULL;
}

int msgboxf(const char* text, unsigned int type, ...)
{
   if (log_cb)
   {
      va_list args;

      char temp[2048];

      va_start(args, type);
      vsprintf(temp, text, args);
      va_end(args);

      log_cb(RETRO_LOG_INFO, temp);
   }
   return 0;
}

int get_mic_data(u8* buffer) { return 0; }
int push_vmu_screen(u8* buffer) { return 0; }

void os_DebugBreak(void)
{
   printf("DEBUGBREAK!\n");
   //exit(-1);
   __builtin_trap();
}
