#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include "rsxutil.h"

#define GCM_LABEL_INDEX		255

extern void vlog(const char *fmt, ...);

videoResolution res;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;

u32 display_width;
u32 display_height;

u32 depth_pitch;
u32 depth_offset;
u32 *depth_buffer;

u32 color_pitch;
u32 color_offset[2];
u32 *color_buffer[2];

static u32 sLabelVal = 1;

static void wait_finish()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);

	rsxFlushBuffer(context);

	while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX)!=sLabelVal)
		usleep(30);

	++sLabelVal;
}

static void wait_rsx_idle()
{
	rsxSetWriteBackendLabel(context,GCM_LABEL_INDEX,sLabelVal);
	rsxSetWaitLabel(context,GCM_LABEL_INDEX,sLabelVal);

	++sLabelVal;

	wait_finish();
}

void set_render_target(u32 index)
{
	gcmSurface sf;

	sf.colorFormat		= GCM_SURFACE_X8R8G8B8;
	sf.colorTarget		= GCM_SURFACE_TARGET_0;
	sf.colorLocation[0]	= GCM_LOCATION_RSX;
	sf.colorOffset[0]	= color_offset[index];
	sf.colorPitch[0]	= color_pitch;

	sf.colorLocation[1]	= GCM_LOCATION_RSX;
	sf.colorLocation[2]	= GCM_LOCATION_RSX;
	sf.colorLocation[3]	= GCM_LOCATION_RSX;
	sf.colorOffset[1]	= 0;
	sf.colorOffset[2]	= 0;
	sf.colorOffset[3]	= 0;
	sf.colorPitch[1]	= 64;
	sf.colorPitch[2]	= 64;
	sf.colorPitch[3]	= 64;

	sf.depthFormat		= GCM_SURFACE_ZETA_Z16;
	sf.depthLocation	= GCM_LOCATION_RSX;
	sf.depthOffset		= depth_offset;
	sf.depthPitch		= depth_pitch;

	sf.type				= GCM_TEXTURE_LINEAR;
	sf.antiAlias		= GCM_SURFACE_CENTER_1;

	sf.width			= display_width;
	sf.height			= display_height;
	sf.x				= 0;
	sf.y				= 0;

	rsxSetSurface(context,&sf);
}

void init_screen(void *host_addr,u32 size)
{
	s32 r;
	u32 resid;
	videoState state;
	videoConfiguration vconfig;

	vlog("init_screen: host_addr=%p size=%u", host_addr, (unsigned)size);
	r = rsxInit(&context, CB_SIZE,size,host_addr);
	vlog("rsxInit = %d context=%p", (int)r, (void*)context);

	memset(&state,0,sizeof(state));
	r = videoGetState(0,0,&state);
	vlog("videoGetState = %d state=%d resolution=%d aspect=%d", (int)r, (int)state.state,
	     (int)state.displayMode.resolution, (int)state.displayMode.aspect);
	vlog("disponible 1080=%d 720=%d 480=%d",
	     (int)videoGetResolutionAvailability(0, VIDEO_RESOLUTION_1080, VIDEO_ASPECT_AUTO, 0),
	     (int)videoGetResolutionAvailability(0, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0),
	     (int)videoGetResolutionAvailability(0, VIDEO_RESOLUTION_480, VIDEO_ASPECT_AUTO, 0));

	resid = state.displayMode.resolution;
	if (resid == 0)
		resid = VIDEO_RESOLUTION_720;
	r = videoGetResolution(resid,&res);
	vlog("videoGetResolution(%u) = %d %ux%u", (unsigned)resid, (int)r, (unsigned)res.width, (unsigned)res.height);

	memset(&vconfig,0,sizeof(videoConfiguration));
	vconfig.resolution = resid;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width*sizeof(u32);

	wait_rsx_idle();
	vlog("wait_rsx_idle ok");

	r = videoConfigure(0,&vconfig,NULL,0);
	vlog("videoConfigure = %d", (int)r);
	r = videoGetState(0,0,&state);
	vlog("videoGetState2 = %d state=%d resolution=%d", (int)r, (int)state.state, (int)state.displayMode.resolution);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	display_width = res.width;
	display_height = res.height;

	color_pitch = display_width*sizeof(u32);
	color_buffer[0] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	color_buffer[1] = (u32*)rsxMemalign(64,(display_height*color_pitch));
	vlog("color_buffer = %p %p", (void*)color_buffer[0], (void*)color_buffer[1]);

	rsxAddressToOffset(color_buffer[0],&color_offset[0]);
	rsxAddressToOffset(color_buffer[1],&color_offset[1]);

	r = gcmSetDisplayBuffer(0,color_offset[0],color_pitch,display_width,display_height);
	vlog("gcmSetDisplayBuffer0 = %d", (int)r);
	r = gcmSetDisplayBuffer(1,color_offset[1],color_pitch,display_width,display_height);
	vlog("gcmSetDisplayBuffer1 = %d", (int)r);

	depth_pitch = display_width*sizeof(u32);
	depth_buffer = (u32*)rsxMemalign(64,(display_height*depth_pitch)*2);
	rsxAddressToOffset(depth_buffer,&depth_offset);
	vlog("init_screen terminado");
}

void waitflip()
{
	u32 n = 0;
	while(gcmGetFlipStatus()!=0) {
		usleep(200);
		if (++n > 5000) {		/* ~1 s: no esperar para siempre */
			vlog("waitflip: TIMEOUT (flip nunca termino)");
			break;
		}
	}
	gcmResetFlipStatus();
}

void flip()
{
	if(!first_fb) waitflip();
	else gcmResetFlipStatus();

	gcmSetFlip(context,curr_fb);
	rsxFlushBuffer(context);

	gcmSetWaitFlip(context);

	curr_fb ^= 1;
	set_render_target(curr_fb);

	first_fb = 0;
}
