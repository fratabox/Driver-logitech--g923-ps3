/*
 * G923 PS3 Initializer  v0.1  (PSL1GHT)
 *
 * Aplicacion de PRUEBA. No es todavia un controlador para juegos.
 *
 * Que hace:
 *   1. Se registra como driver USB para el G923 en modo Xbox (046D:C26D).
 *   2. Al detectarlo manda el comando de activacion  0F 00 01 01 42
 *      (el mismo que manda G HUB; salio de la captura g923.pcapng).
 *   3. El volante se reinicia y reaparece como 046D:C26E (HID). Este
 *      programa tambien se registra para ese ID, abre el endpoint de
 *      entrada 0x81 y lee los reportes de 11 bytes.
 *   4. Escribe un diario en /dev_hdd0/tmp/g923_log.txt (se lee por FTP
 *      con webMAN) con los valores del volante, pedales y botones.
 *
 * Datos tomados de la captura real del G923:
 *   - C26D: clase FF/47/D0, endpoints 0x01 (OUT) y 0x81 (IN), 64 bytes.
 *   - C26E: dos interfaces HID; interfaz 0 con endpoints 0x81 / 0x01.
 *   - Reporte de entrada: 11 bytes, ID 0x01, ~500 Hz, solo cuando cambia.
 *
 * NO PROBADO en una PS3 real. Escrito sin poder compilarlo con el
 * toolchain de PS3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef STAGE
#define STAGE 3
#endif
#include <unistd.h>
#include <sysmodule/sysmodule.h>
#include <io/pad.h>
#include <malloc.h>
#include "rsxutil.h"
#if defined(__has_include)
#  if __has_include(<sys/systime.h>)
#    include <sys/systime.h>
#    define G923_SLEEP_US(us) sysUsleep(us)
#  endif
#endif
#ifndef G923_SLEEP_US
#  define G923_SLEEP_US(us) usleep(us)
#endif
#include <sysutil/sysutil.h>
#include <usb/usb.h>

#define G923_VID      0x046d
#define G923_PID_XBOX 0xc26d   /* modo Xbox One: hay que despertarlo */
#define G923_PID_HID  0xc26e   /* modo HID, despues del comando      */

#define EP_OUT        0x01
#define EP_IN         0x81
#define LOG_PATH      "/dev_hdd0/tmp/g923_log.txt"

/* Los descriptores estaticos que entrega la PS3 vienen en little-endian
 * y el PPU es big-endian, por eso hace falta intercambiar los bytes. */
#define SWAP16(x) ((u16)((((x) & 0x00FF) << 8) | (((x) & 0xFF00) >> 8)))

static const u8 WAKE_CMD[5] = { 0x0f, 0x00, 0x01, 0x01, 0x42 };

/* ------------------------------------------------------------------ */
/* Diario                                                              */
/* ------------------------------------------------------------------ */

static FILE *g_log = NULL;

void vlog(const char *fmt, ...);
static void logmsg(const char *fmt, ...)
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

/* ------------------------------------------------------------------ */
/* Decodificador del reporte HID de 11 bytes                           */
/* ------------------------------------------------------------------ */

typedef struct {
    u8  hat;        /* 0..7 direcciones, 8 = reposo           */
    u32 buttons;    /* 23 bits                                */
    u16 steering;   /* 0..0xFFFF                              */
    u8  axis_y;     /* pedal (en reposo 0xFF, pisado -> 0)    */
    u8  axis_z;     /* pedal                                  */
    u8  axis_rz;    /* pedal                                  */
    u8  extra;      /* 3 bits utiles                          */
} G923Report;

static int g923_decode(const u8 *r, size_t len, G923Report *out)
{
    if (len < 11 || r[0] != 0x01)
        return 0;

    out->hat = r[1] & 0x0F;
    out->buttons = ((u32)(r[1] >> 4)) |
                   ((u32)r[2] << 4)   |
                   ((u32)r[3] << 12)  |
                   ((u32)(r[4] & 0x07) << 20);
    out->steering = (u16)r[5] | ((u16)r[6] << 8);
    out->axis_y   = r[7];
    out->axis_z   = r[8];
    out->axis_rz  = r[9];
    out->extra    = r[10] & 0x07;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Estado compartido (callbacks USB -> bucle principal)                */
/* ------------------------------------------------------------------ */

typedef struct {
    s32 dev_id;
    s32 c_pipe;     /* control    */
    s32 i_pipe;     /* IN  0x81   */
    s32 o_pipe;     /* OUT 0x01   */
    u8 *rx;         /* buffer USB (usbAllocateMemory) */
    u8 *tx;
    int payload;
    volatile int alive;
} g923_unit;

static g923_unit g_wake;
static g923_unit g_hid;

static volatile u8  g_last[64];
static volatile u32 g_last_len   = 0;
static volatile u32 g_reports    = 0;   /* reportes de 11 bytes recibidos */
static volatile u32 g_other      = 0;   /* otros (p.ej. HID++ de 64 bytes) */
static volatile int g_running    = 1;


/* usbAllocateMemory devuelve una direccion de 32 bits (PRX). Si se pasa la
 * direccion de un puntero de 64 bits, la direccion queda en la mitad alta
 * (se vio tx=0x200a000000000000 en el log). Se lee en un u64 y se corrige. */
static u8 *usb_alloc(size_t n)
{
    u64 a = 0;
    u32 addr;
    if (usbAllocateMemory((void **)&a, n) < 0)
        return NULL;
    addr = (u32)(a >> 32);
    if (addr == 0)
        addr = (u32)a;
    return addr ? (u8 *)(uintptr_t)addr : NULL;
}

/* ------------------------------------------------------------------ */
/* Utilidades de descriptores                                          */
/* ------------------------------------------------------------------ */

/* Busca el endpoint con la direccion pedida dentro de la primera
 * interfaz. Devuelve NULL si no esta. */
static usbEndpointDescriptor *find_endpoint(s32 dev_id, usbInterfaceDescriptor *idesc, u8 addr)
{
    usbEndpointDescriptor *e = NULL;
    int i;

    for (i = 0; i < idesc->bNumEndpoints; i++) {
        e = (usbEndpointDescriptor *)usbScanStaticDescriptor(dev_id, e ? (void *)e : (void *)idesc,
                                                             USB_DESCRIPTOR_TYPE_ENDPOINT);
        if (!e)
            return NULL;
        if (e->bEndpointAddress == addr)
            return e;
    }
    return NULL;
}

static int get_descriptors(s32 dev_id, usbConfigDescriptor **cdesc, usbInterfaceDescriptor **idesc)
{
    *cdesc = (usbConfigDescriptor *)usbScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIG);
    if (!*cdesc)
        return 0;
    *idesc = (usbInterfaceDescriptor *)usbScanStaticDescriptor(dev_id, *cdesc, USB_DESCRIPTOR_TYPE_INTERFACE);
    return *idesc != NULL;
}

/* ------------------------------------------------------------------ */
/* Etapa 1: 046D:C26D (modo Xbox) -> mandar el comando de activacion   */
/* ------------------------------------------------------------------ */

static void wake_sent_done(s32 result, s32 count, void *arg)
{
    (void)arg;
    logmsg("[wake] comando enviado: result=%d bytes=%d (el volante deberia reiniciarse y calibrar)",
           (int)result, (int)count);
}

static void wake_cfg_done(s32 result, s32 count, void *arg)
{
    g923_unit *u = (g923_unit *)arg;
    s32 r;
    (void)count;

    logmsg("[wake] SET_CONFIGURATION terminado: result=%d", (int)result);
    logmsg("[wake] tx=%p o_pipe=%d alive=%d", (void *)u->tx, (int)u->o_pipe, (int)u->alive);
    memcpy(u->tx, WAKE_CMD, sizeof(WAKE_CMD));
    logmsg("[wake] copiado; mando el comando...");
    r = usbInterruptTransfer(u->o_pipe, u->tx, sizeof(WAKE_CMD), wake_sent_done, u);
    logmsg("[wake] usbInterruptTransfer(OUT) devolvio %d", (int)r);
}

static s32 wake_probe(s32 dev_id)
{
    usbDeviceDescriptor *d = (usbDeviceDescriptor *)usbScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
    if (!d)
        return USB_PROBE_FAILED;
    logmsg("[wake] probe: dev_id=%d VID=%04x PID=%04x", (int)dev_id,
           (unsigned)SWAP16(d->idVendor), (unsigned)SWAP16(d->idProduct));
    return USB_PROBE_SUCCEEDED;
}

static s32 wake_attach(s32 dev_id)
{
    usbConfigDescriptor *cdesc;
    usbInterfaceDescriptor *idesc;
    usbEndpointDescriptor *eout;
    g923_unit *u = &g_wake;

    logmsg("[wake] attach: dev_id=%d", (int)dev_id);
    memset(u, 0, sizeof(*u));
    u->dev_id = dev_id;

    if (!get_descriptors(dev_id, &cdesc, &idesc)) {
        logmsg("[wake] no pude leer los descriptores");
        return USB_ATTACH_FAILED;
    }
    logmsg("[wake] interfaz: clase=%02x sub=%02x prot=%02x endpoints=%d",
           idesc->bInterfaceClass, idesc->bInterfaceSubClass, idesc->bInterfaceProtocol,
           idesc->bNumEndpoints);

    eout = find_endpoint(dev_id, idesc, EP_OUT);
    if (!eout) {
        logmsg("[wake] no hay endpoint OUT 0x01");
        return USB_ATTACH_FAILED;
    }
    if ((u->c_pipe = usbOpenPipe(dev_id, NULL)) < 0) {
        logmsg("[wake] usbOpenPipe(control) fallo: %d", (int)u->c_pipe);
        return USB_ATTACH_FAILED;
    }
    if ((u->o_pipe = usbOpenPipe(dev_id, eout)) < 0) {
        logmsg("[wake] usbOpenPipe(OUT) fallo: %d", (int)u->o_pipe);
        return USB_ATTACH_FAILED;
    }
    u->tx = usb_alloc(64);
    if (!u->tx) {
        logmsg("[wake] usbAllocateMemory fallo");
        return USB_ATTACH_FAILED;
    }
    u->alive = 1;
    usbSetPrivateData(dev_id, u);
    usbSetConfiguration(u->c_pipe, cdesc->bConfigurationValue, wake_cfg_done, u);
    return USB_ATTACH_SUCCEEDED;
}

static s32 wake_detach(s32 dev_id)
{
    g923_unit *u = (g923_unit *)usbGetPrivateData(dev_id);
    logmsg("[wake] detach: dev_id=%d (esperado: el volante se reinicia)", (int)dev_id);
    if (!u)
        return USB_DETACH_FAILED;
    u->alive = 0;
    if (u->o_pipe > 0) usbClosePipe(u->o_pipe);
    if (u->c_pipe > 0) usbClosePipe(u->c_pipe);
    if (u->tx) usbFreeMemory(u->tx);
    u->tx = NULL;
    return USB_DETACH_SUCCEEDED;
}


/* ------------------------------------------------------------------ */
/* Etapa 2: 046D:C26E (HID) -> leer reportes                           */
/* ------------------------------------------------------------------ */

static void hid_rx(g923_unit *u);

static void hid_rx_done(s32 result, s32 count, void *arg)
{
    g923_unit *u = (g923_unit *)arg;

    if (result == 0 && count > 0 && count <= 64) {
        if (count == 11 && u->rx[0] == 0x01) {
            memcpy((void *)g_last, u->rx, 11);
            g_last_len = 11;
            g_reports++;
        } else {
            g_other++;
        }
    }
    /* Si el volante se desconecta, el sistema llama a detach y alive
     * pasa a 0: ahi dejamos de pedir mas datos. */
    if (u->alive)
        hid_rx(u);
}

static void hid_rx(g923_unit *u)
{
    usbInterruptTransfer(u->i_pipe, u->rx, u->payload, hid_rx_done, u);
}

static void hid_cfg_done(s32 result, s32 count, void *arg)
{
    g923_unit *u = (g923_unit *)arg;
    (void)count;
    logmsg("[hid] SET_CONFIGURATION terminado: result=%d; empiezo a leer", (int)result);
    hid_rx(u);
}

static s32 hid_probe(s32 dev_id)
{
    usbDeviceDescriptor *d = (usbDeviceDescriptor *)usbScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
    if (!d)
        return USB_PROBE_FAILED;
    logmsg("[hid] probe: dev_id=%d VID=%04x PID=%04x", (int)dev_id,
           (unsigned)SWAP16(d->idVendor), (unsigned)SWAP16(d->idProduct));
    return USB_PROBE_SUCCEEDED;
}

static s32 hid_attach(s32 dev_id)
{
    usbConfigDescriptor *cdesc;
    usbInterfaceDescriptor *idesc;
    usbEndpointDescriptor *ein;
    g923_unit *u = &g_hid;

    logmsg("[hid] attach: dev_id=%d  <-- el volante reaparecio como C26E", (int)dev_id);
    memset(u, 0, sizeof(*u));
    u->dev_id = dev_id;

    if (!get_descriptors(dev_id, &cdesc, &idesc)) {
        logmsg("[hid] no pude leer los descriptores");
        return USB_ATTACH_FAILED;
    }
    logmsg("[hid] interfaz 0: clase=%02x sub=%02x prot=%02x endpoints=%d",
           idesc->bInterfaceClass, idesc->bInterfaceSubClass, idesc->bInterfaceProtocol,
           idesc->bNumEndpoints);

    ein = find_endpoint(dev_id, idesc, EP_IN);
    if (!ein) {
        logmsg("[hid] no hay endpoint IN 0x81");
        return USB_ATTACH_FAILED;
    }
    u->payload = SWAP16(ein->wMaxPacketSize);
    if (u->payload <= 0 || u->payload > 64)
        u->payload = 64;

    if ((u->c_pipe = usbOpenPipe(dev_id, NULL)) < 0) {
        logmsg("[hid] usbOpenPipe(control) fallo: %d", (int)u->c_pipe);
        return USB_ATTACH_FAILED;
    }
    if ((u->i_pipe = usbOpenPipe(dev_id, ein)) < 0) {
        logmsg("[hid] usbOpenPipe(IN) fallo: %d", (int)u->i_pipe);
        return USB_ATTACH_FAILED;
    }
    u->rx = usb_alloc(64);
    if (!u->rx) {
        logmsg("[hid] usbAllocateMemory fallo");
        return USB_ATTACH_FAILED;
    }
    u->alive = 1;
    usbSetPrivateData(dev_id, u);
    usbSetConfiguration(u->c_pipe, cdesc->bConfigurationValue, hid_cfg_done, u);
    return USB_ATTACH_SUCCEEDED;
}

static s32 hid_detach(s32 dev_id)
{
    g923_unit *u = (g923_unit *)usbGetPrivateData(dev_id);
    logmsg("[hid] detach: dev_id=%d", (int)dev_id);
    if (!u)
        return USB_DETACH_FAILED;
    u->alive = 0;
    if (u->i_pipe > 0) usbClosePipe(u->i_pipe);
    if (u->c_pipe > 0) usbClosePipe(u->c_pipe);
    /* No liberamos rx aca: puede haber una transferencia en vuelo. */
    return USB_DETACH_SUCCEEDED;
}

/* ------------------------------------------------------------------ */
/* Un solo driver para los dos IDs.                                    */
/* El wrapper de PSL1GHT le pone el mismo nombre ("desc") a todo LDD  */
/* que se registra, asi que dos drivers distintos chocan: error        */
/* 0x80110006 (ya registrado). Se registra el mismo para C26D y C26E.  */
/* ------------------------------------------------------------------ */

static u16 dev_pid(s32 dev_id)
{
    usbDeviceDescriptor *d = (usbDeviceDescriptor *)usbScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
    return d ? SWAP16(d->idProduct) : 0;
}

static s32 g_probe(s32 dev_id)
{
    u16 pid = dev_pid(dev_id);
    logmsg("[usb] probe dev_id=%d PID=%04x", (int)dev_id, (unsigned)pid);
    return (pid == G923_PID_XBOX || pid == G923_PID_HID) ? USB_PROBE_SUCCEEDED : USB_PROBE_FAILED;
}

static s32 g_attach(s32 dev_id)
{
    u16 pid = dev_pid(dev_id);
    if (pid == G923_PID_XBOX)
        return wake_attach(dev_id);
    if (pid == G923_PID_HID)
        return hid_attach(dev_id);
    return USB_ATTACH_FAILED;
}

static s32 g_detach(s32 dev_id)
{
    g923_unit *u = (g923_unit *)usbGetPrivateData(dev_id);
    if (u == &g_wake)
        return wake_detach(dev_id);
    if (u == &g_hid)
        return hid_detach(dev_id);
    logmsg("[usb] detach dev_id=%d (sin datos privados)", (int)dev_id);
    return USB_DETACH_SUCCEEDED;
}

static usbLddOps g_ops = { "g923", g_probe, g_attach, g_detach };

/* ------------------------------------------------------------------ */
/* Salida limpia con el boton PS                                       */
/* ------------------------------------------------------------------ */


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

/* ---- Indicador en pantalla: bandas de color (sin texto) ----
 * 0 arranque  1 log abierto  2 callback sysutil  3 modulo USB
 * 4 usbInit   5 driver C26D  6 driver C26E       7 llegaron reportes
 * gris = pendiente, verde = ok, rojo = fallo
 */
#define NBANDS 8
static u8  g_band[NBANDS];
static int g_video_ok = 0;

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    rsxSetScissor(context, (u16)x, (u16)y, (u16)w, (u16)h);
    rsxSetClearColor(context, color);
    rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

static void screen_draw(int beat)
{
    (void)beat;     /* sin dibujo: ver nota en main() */
}

static int g_beat = 0;
static void mark(int band, int ok)
{
    if (band >= 0 && band < NBANDS)
        g_band[band] = ok ? 1 : 2;
    logmsg("paso %d: %s", band, ok ? "OK" : "FALLO");
    screen_draw(g_beat);
}

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
    (void)param;
    (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME)
        g_running = 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Etapa 4: control virtual de la PS3 alimentado con el volante        */
/* ------------------------------------------------------------------ */
#if STAGE >= 4

/* Posiciones dentro de padData.button[] (iguales a CELL_PAD_BTN_OFFSET_*) */
#define PB_D1        2   /* digital 1: cruceta, start, select, L3, R3 */
#define PB_D2        3   /* digital 2: cruz, circulo, ..., R1, L1, R2, L2 */
#define PB_RX        4
#define PB_RY        5
#define PB_LX        6
#define PB_LY        7
#define PB_P_RIGHT   8
#define PB_P_LEFT    9
#define PB_P_UP     10
#define PB_P_DOWN   11
#define PB_P_TRI    12
#define PB_P_CIR    13
#define PB_P_CRO    14
#define PB_P_SQU    15
#define PB_P_L1     16
#define PB_P_R1     17
#define PB_P_L2     18
#define PB_P_R2     19
#define PB_SENS_X   20

/* bits de digital 1 */
#define D1_LEFT   0x80
#define D1_DOWN   0x40
#define D1_RIGHT  0x20
#define D1_UP     0x10
#define D1_START  0x08
#define D1_R3     0x04
#define D1_L3     0x02
#define D1_SELECT 0x01
/* bits de digital 2 */
#define D2_SQUARE   0x80
#define D2_CROSS    0x40
#define D2_CIRCLE   0x20
#define D2_TRIANGLE 0x10
#define D2_R1       0x08
#define D2_L1       0x04
#define D2_R2       0x02
#define D2_L2       0x01

/* Levas del volante */
#define WB_PADDLE_R  0x000010
#define WB_PADDLE_L  0x000020

static s32 g_vpad  = -1;
static s32 g_vport = -1;
static u32 g_vins  = 0;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* pedal invertido (255 suelto) -> 0..255 pisado */
static int pedal(int raw, int rest, int full)
{
    int span = rest - full;
    int v;
    if (span <= 0)
        return 0;
    v = (rest - raw) * 255 / span;
    v = clampi(v, 0, 255);
    return v < 10 ? 0 : v;          /* zona muerta */
}

static void vpad_pump(void)
{
    u8 snap[11];
    G923Report rep;
    padData d;
    u16 *b = d.button;
    int acc, brk, clu;
    u16 d1 = 0, d2 = 0;
    int have;

    if (g_vpad < 0)
        return;

    memset(&d, 0, sizeof(d));
    d.len = 24;
    b[PB_RX] = b[PB_RY] = b[PB_LX] = b[PB_LY] = 0x80;
    b[PB_SENS_X] = b[PB_SENS_X + 1] = b[PB_SENS_X + 2] = b[PB_SENS_X + 3] = 0x200;

    have = 0;
    if (g_last_len == 11) {
        memcpy(snap, (const void *)g_last, 11);
        have = g923_decode(snap, 11, &rep);
    }

    if (have) {
        /* volante -> stick izquierdo horizontal (0..255, centro ~128) */
        b[PB_LX] = (u16)(rep.steering >> 8);

        acc = pedal(rep.axis_y,  235, 0);       /* acelerador */
        brk = pedal(rep.axis_z,  255, 11);      /* freno      */
        clu = pedal(rep.axis_rz, 255, 0);       /* embrague   */

        b[PB_P_R2] = (u16)acc;
        b[PB_P_L2] = (u16)brk;
        if (acc > 16) d2 |= D2_R2;
        if (brk > 16) d2 |= D2_L2;
        /* embrague -> stick derecho vertical (arriba = pisado) */
        b[PB_RY] = (u16)(0x80 - (clu >> 1));

        /* cruceta: en ESTE volante izquierda/derecha estan invertidas
         * respecto del HID estandar (confirmado con tu prueba: derecha=6, izquierda=2) */
        switch (rep.hat) {
        case 0: d1 |= D1_UP; break;
        case 1: d1 |= D1_UP | D1_LEFT; break;
        case 2: d1 |= D1_LEFT; break;
        case 3: d1 |= D1_DOWN | D1_LEFT; break;
        case 4: d1 |= D1_DOWN; break;
        case 5: d1 |= D1_DOWN | D1_RIGHT; break;
        case 6: d1 |= D1_RIGHT; break;
        case 7: d1 |= D1_UP | D1_RIGHT; break;
        default: break;
        }

        /* botones (mapa segun la foto numerada) */
        if (rep.buttons & WB_PADDLE_R) { d2 |= D2_R1; b[PB_P_R1] = 0xFF; }   /* leva derecha   */
        if (rep.buttons & WB_PADDLE_L) { d2 |= D2_L1; b[PB_P_L1] = 0xFF; }   /* leva izquierda */
        if (rep.buttons & 0x000001) { d2 |= D2_SQUARE;   b[PB_P_SQU] = 0xFF; } /* X */
        if (rep.buttons & 0x000002) { d2 |= D2_CIRCLE;   b[PB_P_CIR] = 0xFF; } /* B */
        if (rep.buttons & 0x000004) { d2 |= D2_CROSS;    b[PB_P_CRO] = 0xFF; } /* A */
        if (rep.buttons & 0x000008) { d2 |= D2_TRIANGLE; b[PB_P_TRI] = 0xFF; } /* Y */
        if (rep.buttons & 0x000040) d1 |= D1_START;     /* menu  (10) */
        if (rep.buttons & 0x000080) d1 |= D1_SELECT;    /* vista (9)  */
        if (rep.buttons & 0x000200) d1 |= D1_L3;        /* LSB (11)   */
        if (rep.buttons & 0x000100) d1 |= D1_R3;        /* RSB (12)   */
        /* sin asignar todavia: 0x40000 (+ 13), 0x80000 (- 14), 0x400000 y 0x100000 (rueda 15) */
        if (d1 & D1_UP)    b[PB_P_UP]    = 0xFF;
        if (d1 & D1_DOWN)  b[PB_P_DOWN]  = 0xFF;
        if (d1 & D1_LEFT)  b[PB_P_LEFT]  = 0xFF;
        if (d1 & D1_RIGHT) b[PB_P_RIGHT] = 0xFF;
    }

    b[PB_D1] = d1;
    b[PB_D2] = d2;

    ioPadLddDataInsert(g_vpad, &d);
    g_vins++;
}

/* Lee de vuelta el pad virtual por su puerto, para ver si el sistema lo recibe. */
static void vpad_loopback_log(void)
{
    padData rd;
    s32 r;
    if (g_vport < 0)
        return;
    memset(&rd, 0, sizeof(rd));
    r = ioPadGetData((u32)g_vport, &rd);
    logmsg("[pad] insertados=%u lectura rc=%d len=%d D1=%04x D2=%04x LX=%u RY=%u R2=%u L2=%u",
           (unsigned)g_vins, (int)r, (int)rd.len,
           (unsigned)rd.button[PB_D1], (unsigned)rd.button[PB_D2],
           (unsigned)rd.button[PB_LX], (unsigned)rd.button[PB_RY],
           (unsigned)rd.button[PB_P_R2], (unsigned)rd.button[PB_P_L2]);
}

#endif /* STAGE >= 4 */

int main(int argc, char *argv[])
{
    s32 r;
    u32 last_reports = 0;
    u32 ticks = 0;
    u8  snap[11];
    G923Report rep, prev;
    int have_prev = 0;

    (void)argc;
    (void)argv;


    {
        /* Prueba varias rutas: la primera que se pueda escribir queda como log */
        static const char *const paths[] = {
            LOG_PATH,
            "/dev_hdd0/game/G923INIT0/USRDIR/g923_log.txt",
            "/dev_hdd0/g923_log.txt",
            "/dev_usb000/g923_log.txt",
            "/dev_usb001/g923_log.txt",
        };
        size_t pi;
        for (pi = 0; pi < sizeof(paths) / sizeof(paths[0]) && !g_log; pi++) {
            g_log = fopen(paths[pi], "w");
            if (g_log)
                logmsg("log abierto en %s", paths[pi]);
        }
    }
    logmsg("ETAPA %d", (int)STAGE);
    mark(0, 1);
    mark(1, g_log != NULL);
    {
        /* El video se inicializa solo para que la PS3 no cierre la app;
         * NO se dibuja nada (los cambios de cuadro no terminan en esta consola). */
        void *host_addr = memalign(1024 * 1024, HOST_SIZE);
        if (host_addr) {
            init_screen(host_addr, HOST_SIZE);
            g_video_ok = 1;
        } else {
            logmsg("sin memoria para el video");
        }
    }
    logmsg("G923 PS3 Initializer v0.4 (sale solo a los 2 min)");
    logmsg("Esperando el volante (enchufalo ahora si no lo estaba)...");

    r = sysUtilRegisterCallback(0, sysutil_cb, NULL);
    mark(2, r == 0);

#if STAGE >= 1
    r = sysModuleLoad(SYSMODULE_USB);
    logmsg("sysModuleLoad(USB) = 0x%x", (unsigned)r);
    mark(3, r == 0);
#endif
#if STAGE >= 2
    r = usbInit();
    logmsg("usbInit() = %d", (int)r);
    mark(4, r == 0);
#endif
#if STAGE >= 3
    r = usbRegisterExtraLdd2(&g_ops, G923_VID, G923_PID_XBOX, G923_PID_HID);
    logmsg("registrar C26D..C26E (un solo registro) = %d (0x%x)", (int)r, (unsigned)r);
    mark(5, r == 0);
    mark(6, r == 0);
#endif
#if STAGE >= 4
    r = ioPadInit(7);
    logmsg("ioPadInit(7) = 0x%x", (unsigned)r);
    g_vpad = ioPadLddRegisterController();
    logmsg("ioPadLddRegisterController() = %d", (int)g_vpad);
    usleep(20 * 1000);          /* darle tiempo a la PS3 a registrarlo */
    if (g_vpad >= 0) {
        g_vport = ioPadLddGetPortNo(g_vpad);
        logmsg("puerto del pad virtual = %d", (int)g_vport);
    }
    mark(7, g_vpad >= 0);
#endif
    (void)r;

    memset(&prev, 0, sizeof(prev));

    while (g_running) {
        sysUtilCheckCallback();
#if STAGE >= 4
        {
            int k;
            for (k = 0; k < 25; k++) {      /* 25 x 4 ms = 100 ms */
                vpad_pump();
                G923_SLEEP_US(4 * 1000);
            }
        }
#else
        G923_SLEEP_US(100 * 1000);          /* 10 veces por segundo */
#endif
        ticks++;
        if (ticks >= 1200) {            /* 2 minutos: salida automatica */
            logmsg("tiempo cumplido (2 min): salgo solo");
            g_running = 0;
        }
        if ((ticks % 5) == 0) {
            g_beat ^= 1;
            if (g_reports > 0)
                g_band[7] = 1;
            screen_draw(g_beat);
        }

        if (g_reports != last_reports && g_last_len == 11) {
            memcpy(snap, (const void *)g_last, 11);
            last_reports = g_reports;
            if (g923_decode(snap, 11, &rep)) {
                /* Registrar solo si cambio algo importante. */
                if (!have_prev ||
                    rep.hat != prev.hat || rep.buttons != prev.buttons ||
                    (rep.steering >> 8) != (prev.steering >> 8) ||
                    (rep.axis_y >> 4) != (prev.axis_y >> 4) ||
                    (rep.axis_z >> 4) != (prev.axis_z >> 4) ||
                    (rep.axis_rz >> 4) != (prev.axis_rz >> 4)) {
                    logmsg("t=%u.%u rep=%u hat=%u btn=%06x volante=%5u Y=%3u Z=%3u Rz=%3u extra=%u",
                           (unsigned)(ticks / 10), (unsigned)(ticks % 10), (unsigned)g_reports,
                           rep.hat, (unsigned)rep.buttons, rep.steering,
                           rep.axis_y, rep.axis_z, rep.axis_rz, rep.extra);
                    prev = rep;
                    have_prev = 1;
                }
            }
        }

#if STAGE >= 4
        if ((ticks % 10) == 0)
            vpad_loopback_log();
#endif
        if ((ticks % 100) == 0)
            logmsg("[estado] t=%us reportes=%u otros=%u", (unsigned)(ticks / 10),
                   (unsigned)g_reports, (unsigned)g_other);
    }

    logmsg("Saliendo...");
#if STAGE >= 4
    if (g_vpad >= 0)
        logmsg("ioPadLddUnregisterController = 0x%x", (unsigned)ioPadLddUnregisterController(g_vpad));
#endif
#if STAGE >= 3
    usbUnregisterExtraLdd(&g_ops);
#endif
#if STAGE >= 2
    usbEnd();
#endif
#if STAGE >= 1
    sysModuleUnload(SYSMODULE_USB);
#endif
    if (g_log)
        fclose(g_log);
    sysUtilUnregisterCallback(0);
    return 0;
}
