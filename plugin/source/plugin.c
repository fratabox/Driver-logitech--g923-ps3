/*
 * Plugin de prueba para el proyecto G923 en PS3.
 *
 * Hace una sola cosa: al cargarse, agrega una linea a
 * /dev_hdd0/tmp/g923_plugin_log.txt. No toca USB, controles ni juegos.
 * Sirve para comprobar si la PS3 (HEN) carga plugins propios.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/file.h>
#include <sys/prx_module.h>

#define LOG_PATH "/dev_hdd0/tmp/g923_plugin_log.txt"

#define F_WRONLY 000001
#define F_CREAT  000100
#define F_APPEND 002000

static void log_line(const char *msg)
{
	s32 fd = -1;
	u64 written = 0;

	if (sysLv2FsOpen(LOG_PATH, F_WRONLY | F_CREAT | F_APPEND, &fd, 0666, NULL, 0) != 0)
		return;
	sysLv2FsWrite(fd, msg, strlen(msg), &written);
	sysLv2FsClose(fd);
}

int module_start(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	log_line("g923_plugin_test: module_start ejecutado (el plugin se cargo)\n");
	return SYS_PRX_RESIDENT;
}

int module_stop(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	log_line("g923_plugin_test: module_stop ejecutado\n");
	return SYS_PRX_RESIDENT;
}
