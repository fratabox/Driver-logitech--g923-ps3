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

#include <sys/systime.h>
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
    memcpy(u->tx, WAKE_CMD, sizeof(WAKE_CMD));
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
    if (usbAllocateMemory((void **)&u->tx, 64) < 0 || !u->tx) {
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

static usbLddOps wake_ops = { "g923wake", wake_probe, wake_attach, wake_detach };

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
    if (usbAllocateMemory((void **)&u->rx, 64) < 0 || !u->rx) {
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

static usbLddOps hid_ops = { "g923hid", hid_probe, hid_attach, hid_detach };

/* ------------------------------------------------------------------ */
/* Salida limpia con el boton PS                                       */
/* ------------------------------------------------------------------ */

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

    g_log = fopen(LOG_PATH, "w");
    logmsg("G923 PS3 Initializer v0.1");
    logmsg("Esperando el volante (enchufalo ahora si no lo estaba)...");

    sysUtilRegisterCallback(0, sysutil_cb, NULL);

    r = usbInit();
    logmsg("usbInit() = %d", (int)r);
    r = usbRegisterExtraLdd(&wake_ops, G923_VID, G923_PID_XBOX);
    logmsg("registrar C26D = %d", (int)r);
    r = usbRegisterExtraLdd(&hid_ops, G923_VID, G923_PID_HID);
    logmsg("registrar C26E = %d", (int)r);

    memset(&prev, 0, sizeof(prev));

    while (g_running) {
        sysUtilCheckCallback();
        sysUsleep(100 * 1000);          /* 10 veces por segundo */
        ticks++;

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

        if ((ticks % 100) == 0)
            logmsg("[estado] t=%us reportes=%u otros=%u", (unsigned)(ticks / 10),
                   (unsigned)g_reports, (unsigned)g_other);
    }

    logmsg("Saliendo...");
    usbUnregisterExtraLdd(&wake_ops);
    usbUnregisterExtraLdd(&hid_ops);
    usbEnd();
    if (g_log)
        fclose(g_log);
    sysUtilUnregisterCallback(0);
    return 0;
}
