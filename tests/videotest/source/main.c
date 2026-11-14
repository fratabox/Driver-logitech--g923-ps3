/* Prueba de video con registro detallado: /dev_hdd0/tmp/videotest_log.txt
 * Rojo -> verde -> azul cada ~1 s. Boton PS: salir. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/sysutil.h>
#include "rsxutil.h"

static FILE *g_log = NULL;
static volatile int g_running = 1;

void vlog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void cb(u64 status, u64 param, void *ud)
{
    (void)param; (void)ud;
    vlog("sysutil evento 0x%x", (unsigned)status);
    if (status == SYSUTIL_EXIT_GAME)
        g_running = 0;
}

int main(void)
{
    static const u32 colors[3] = { 0x00CC0000, 0x0000AA00, 0x000000CC };
    u32 frame = 0;
    void *host_addr;
    s32 r;

    g_log = fopen("/dev_hdd0/tmp/videotest_log.txt", "w");
    vlog("VideoTest v2 inicio");

    host_addr = memalign(1024 * 1024, HOST_SIZE);
    vlog("memalign = %p", host_addr);
    if (!host_addr) {
        vlog("sin memoria para el video; salgo");
        return 1;
    }

    init_screen(host_addr, HOST_SIZE);
    r = sysUtilRegisterCallback(0, cb, NULL);
    vlog("sysUtilRegisterCallback = %d", (int)r);
    set_render_target(curr_fb);
    vlog("render target puesto; entro al bucle");

    while (g_running && frame < 60 * 600) {
        sysUtilCheckCallback();
        rsxSetClearColor(context, colors[(frame / 60) % 3]);
        rsxSetClearDepthStencil(context, 0xffff);
        rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                                 GCM_CLEAR_S | GCM_CLEAR_Z);
        flip();
        if (frame < 5 || (frame % 120) == 0)
            vlog("frame %u ok", (unsigned)frame);
        frame++;
    }

    vlog("saliendo (frames=%u)", (unsigned)frame);
    sysUtilUnregisterCallback(0);
    if (g_log)
        fclose(g_log);
    return 0;
}
