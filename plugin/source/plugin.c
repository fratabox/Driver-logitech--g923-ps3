/*
 * Plugin de prueba v2 para el proyecto G923 en PS3.
 *
 * Al cargarse deja varios rastros independientes (si uno falla, otro puede salir):
 *  1. Carpeta  /dev_hdd0/tmp/g923_plugin_ejecutado          (marcador)
 *  2. Log      /dev_hdd0/tmp/g923_plugin_log.txt            (con los codigos devueltos)
 *  3. Si abrir el log falla: carpeta /dev_hdd0/tmp/g923_open_rc_<codigo en hex>
 * No toca USB, controles ni juegos.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/file.h>
#include <sys/prx_module.h>

#define DIR_BASE  "/dev_hdd0/tmp/"
#define F_WRONLY  000001
#define F_CREAT   000100
#define F_APPEND  002000

static void hex(char *out, u32 v)
{
	static const char d[] = "0123456789abcdef";
	for (int i = 0; i < 8; i++)
		out[i] = d[(v >> (28 - 4 * i)) & 0xf];
	out[8] = 0;
}

static void append_line(char *buf, size_t *n, const char *s)
{
	size_t l = strlen(s);
	memcpy(buf + *n, s, l);
	*n += l;
}

static void trace(const char *event)
{
	char text[256];
	char h[9];
	size_t n = 0;
	s32 fd = -1;
	u64 written = 0;
	s32 rc_mkdir, rc_open;

	rc_mkdir = sysLv2FsMkdir(DIR_BASE "g923_plugin_ejecutado", 0777);
	rc_open = sysLv2FsOpen(DIR_BASE "g923_plugin_log.txt",
	                       F_WRONLY | F_CREAT | F_APPEND, &fd, 0666, NULL, 0);

	if (rc_open != 0) {
		char name[64];
		size_t m = 0;
		append_line(name, &m, DIR_BASE "g923_open_rc_");
		hex(h, (u32)rc_open);
		append_line(name, &m, h);
		name[m] = 0;
		sysLv2FsMkdir(name, 0777);
		return;
	}

	append_line(text, &n, "g923_plugin_test v2: ");
	append_line(text, &n, event);
	append_line(text, &n, " mkdir=0x");
	hex(h, (u32)rc_mkdir);
	append_line(text, &n, h);
	append_line(text, &n, "\n");
	text[n] = 0;
	sysLv2FsWrite(fd, text, n, &written);
	sysLv2FsClose(fd);
}

int module_start(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	trace("module_start");
	return SYS_PRX_RESIDENT;
}

int module_stop(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	trace("module_stop");
	return SYS_PRX_RESIDENT;
}
