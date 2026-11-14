/* Prueba minima de video: solo limpia la pantalla con colores que cambian.
 * Sin USB, sin archivos. Rojo -> verde -> azul, cada ~1 s. Botón PS: salir. */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/sysutil.h>
#include "rsxutil.h"

static volatile int g_running = 1;

static void cb(u64 status, u64 param, void *ud)
{
    (void)param; (void)ud;
    if (status == SYSUTIL_EXIT_GAME)
        g_running = 0;
}

int main(void)
{
    static const u32 colors[3] = { 0x00CC0000, 0x0000AA00, 0x000000CC };
    u32 frame = 0;
    void *host_addr = memalign(1024 * 1024, HOST_SIZE);

    init_screen(host_addr, HOST_SIZE);
    sysUtilRegisterCallback(0, cb, NULL);
    set_render_target(curr_fb);

    while (g_running) {
        sysUtilCheckCallback();
        rsxSetClearColor(context, colors[(frame / 60) % 3]);
        rsxSetClearDepthStencil(context, 0xffff);
        rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                                 GCM_CLEAR_S | GCM_CLEAR_Z);
        flip();
        frame++;
    }

    sysUtilUnregisterCallback(0);
    return 0;
}
