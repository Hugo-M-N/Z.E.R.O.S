/*
 * init.c — Proceso init de Z.E.R.O.S (PID 1)
 *
 * El kernel arranca y ejecuta este proceso primero.
 * Su trabajo es preparar el entorno mínimo y lanzar la shell.
 *
 * PID 1 es especial: si muere, el kernel entra en kernel panic.
 * Por eso la shell se lanza con fork+exec en un bucle: si zeros
 * termina por cualquier razón, init la reinicia automáticamente.
 *
 * Conceptos:
 *   - mount(): monta sistemas de archivos virtuales del kernel
 *   - fork()+exec(): lanza la shell como proceso hijo
 *   - waitpid(): espera a que el hijo termine y lo reinicia
 *   - /proc: expone el estado interno del kernel
 *   - /sys:  expone dispositivos y drivers
 *   - /dev:  expone dispositivos como archivos
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

/* Detecta qué dispositivo de bloque contiene el disco ZEROS.
 * QEMU virtio  → /dev/vda
 * VirtualBox SATA/IDE → /dev/sda, /dev/hda, /dev/hdb
 * Devuelve la primera ruta que exista, o NULL si no hay ninguna. */
static const char *find_disk(void) {
    static const char *candidates[] = {
        "/dev/vda", "/dev/sda", "/dev/hda", "/dev/hdb",
        "/dev/vdb", "/dev/sdb", "/dev/sdc", NULL
    };
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0)
            return candidates[i];
    }
    return NULL;
}

/* Inserta un módulo del kernel usando BusyBox insmod.
 * Falla silenciosamente si el módulo no existe o ya está cargado. */
static void load_module(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;   /* no existe → saltar */
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { "/bin/busybox", "insmod", (char *)path, NULL };
        char *envp[] = { NULL };
        execve("/bin/busybox", argv, envp);
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

int main(void) {
    printf("\n");
    printf("  \033[1;32m███████╗███████╗██████╗  ██████╗ ███████╗\033[0m\n");
    printf("  \033[1;32m   ███╔╝██╔════╝██╔══██╗██╔═══██╗██╔════╝\033[0m\n");
    printf("  \033[1;32m  ███╔╝ █████╗  ██████╔╝██║   ██║███████╗\033[0m\n");
    printf("  \033[1;32m ███╔╝  ██╔══╝  ██╔══██╗██║   ██║╚════██║\033[0m\n");
    printf("  \033[1;32m███████╗███████╗██║  ██║╚██████╔╝███████║\033[0m\n");
    printf("  \033[1;32m╚══════╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝\033[0m\n");
    printf("\n  Z.E.R.O.S init — arrancando...\n\n");

    /* ── Montar sistemas de archivos virtuales ────────────── */
    if (mount("proc",     "/proc", "proc",     0, NULL) < 0) perror("init: mount /proc");
    if (mount("sysfs",    "/sys",  "sysfs",    0, NULL) < 0) perror("init: mount /sys");
    if (mount("devtmpfs", "/dev",  "devtmpfs", 0, NULL) < 0) perror("init: mount /dev");

    printf("  Sistemas de archivos virtuales montados.\n");

    /* ── Cargar mapa de teclado español ─────────────────────
     * BusyBox loadkmap lee un mapa binario desde stdin. */
    {
        int fd = open("/etc/es.bkmap", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            pid_t pid = fork();
            if (pid == 0) {
                fd = open("/etc/es.bkmap", O_RDONLY);
                dup2(fd, STDIN_FILENO);
                close(fd);
                char *lm_argv[] = { "/bin/loadkmap", NULL };
                char *lm_envp[] = { NULL };
                execve("/bin/loadkmap", lm_argv, lm_envp);
                _exit(1);
            }
            if (pid > 0) waitpid(pid, NULL, 0);
        }
    }

    /* ── Cargar drivers de almacenamiento ───────────────────
     *
     * El kernel de Ubuntu/Debian compila la mayoría de drivers
     * como módulos (.ko), no dentro del binario del kernel.
     * Sin cargarlos, /dev/sda o /dev/vda nunca aparecen.
     *
     * Orden importante: primero las librerías base (libata, libahci),
     * luego los drivers concretos (ahci para SATA, ata_piix para IDE,
     * virtio_blk para QEMU). scsi_mod/sd_mod son necesarios para que
     * los discos SATA aparezcan como /dev/sdX.
     *
     * Los .ko los copia build.sh desde el host a /lib/modules/.
     * Si un módulo ya está compilado dentro del kernel, insmod
     * falla silenciosamente — no es un error. */
    printf("  Cargando drivers de almacenamiento...\n");
    load_module("/lib/modules/scsi_mod.ko");
    load_module("/lib/modules/libata.ko");
    load_module("/lib/modules/libahci.ko");
    load_module("/lib/modules/ahci.ko");       /* VirtualBox SATA → /dev/sda */
    load_module("/lib/modules/ata_piix.ko");   /* VirtualBox IDE  → /dev/hda */
    load_module("/lib/modules/sd_mod.ko");     /* disco SCSI/SATA → /dev/sdX */
    load_module("/lib/modules/virtio.ko");
    load_module("/lib/modules/virtio_blk.ko"); /* QEMU VirtIO     → /dev/vda */
    sleep(2);   /* dar tiempo al kernel para crear los nodos en /dev
                 * 1s era insuficiente en máquinas lentas o con SATA */

    /* ── Detectar disco ZEROS ────────────────────────────────
     * QEMU usa /dev/vda (virtio), VirtualBox usa /dev/sda (SATA).
     * Esperamos hasta 10 segundos a que el kernel exponga el dispositivo. */
    const char *disk = NULL;
    for (int i = 0; i < 10 && !disk; i++) {
        disk = find_disk();
        if (!disk) sleep(1);
    }

    char disk_arg[32] = "";
    if (disk) {
        strncpy(disk_arg, disk, sizeof(disk_arg) - 1);
        printf("  Disco detectado: %s\n", disk_arg);
    } else {
        printf("  Aviso: no se encontro disco — arrancando sin filesystem.\n");
    }

    /* ── Comprobar si el disco tiene formato ZEROS ──────────
     *
     * El superbloque ocupa el bloque 0. Los primeros 4 bytes
     * son el magic: 0x5A45524F ('ZERO').
     * Si no coincide, el disco está en blanco (o tiene otro
     * sistema de archivos) y preguntamos al usuario si quiere
     * formatearlo antes de lanzar la shell.
     *
     * zeros_format se incluye en el initramfs como /bin/zeros_format.
     */
    if (disk) {
        uint32_t magic = 0;
        int fd_chk = open(disk, O_RDONLY);
        if (fd_chk >= 0) {
            read(fd_chk, &magic, 4);
            close(fd_chk);
        }

        if (magic != 0x5A45524F) {
            printf("  \033[1;33mAviso: el disco %s no tiene formato ZEROS.\033[0m\n", disk);
            printf("  Se formateará perdiendo todos los datos actuales.\n");
            printf("  ¿Formatear ahora? [s/N]: ");
            fflush(stdout);

            char resp[8] = {0};
            if (fgets(resp, sizeof(resp), stdin) && (resp[0] == 's' || resp[0] == 'S')) {
                printf("  Formateando %s...\n", disk);
                pid_t pid_fmt = fork();
                if (pid_fmt == 0) {
                    char *fmtargv[] = { "/bin/zeros_format", (char *)disk, NULL };
                    char *fmtenvp[] = { NULL };
                    execve("/bin/zeros_format", fmtargv, fmtenvp);
                    perror("init: no se pudo lanzar /bin/zeros_format");
                    _exit(1);
                }
                if (pid_fmt > 0) {
                    int fst;
                    waitpid(pid_fmt, &fst, 0);
                    if (fst != 0) {
                        printf("  \033[1;31mError al formatear el disco.\033[0m\n");
                    } else {
                        printf("  Disco formateado correctamente.\n");

                        /* ── Copiar fuentes del sistema al disco recién formateado ──
                         * Las fuentes viven en /sys/src/ del initramfs.
                         * zeros_populate las copia al disco ZEROS para que puedan
                         * editarse y recompilarse desde dentro del propio sistema. */
                        printf("  Copiando fuentes y binarios al disco...\n");
                        pid_t pid_pop = fork();
                        if (pid_pop == 0) {
                            /* Origen:  /usr/src/zeros/  (initramfs, no tapado por sysfs)
                             * Destino: /sys/src/        (disco ZEROS, accesible desde la shell) */
                            char *pop_argv[] = {
                                "/bin/zeros_populate", (char *)disk,
                                /* shell */
                                "/sys/src/shell/shell.c",       "/usr/src/zeros/shell/shell.c",
                                "/sys/src/shell/editor.c",      "/usr/src/zeros/shell/editor.c",
                                "/sys/src/shell/editor.h",      "/usr/src/zeros/shell/editor.h",
                                /* fs */
                                "/sys/src/fs/zeros_fs.h",       "/usr/src/zeros/fs/zeros_fs.h",
                                "/sys/src/fs/zeros_mount.h",    "/usr/src/zeros/fs/zeros_mount.h",
                                "/sys/src/fs/zeros_mount.c",    "/usr/src/zeros/fs/zeros_mount.c",
                                "/sys/src/fs/zeros_format.c",   "/usr/src/zeros/fs/zeros_format.c",
                                "/sys/src/fs/zeros_populate.c", "/usr/src/zeros/fs/zeros_populate.c",
                                /* vm */
                                "/sys/src/vm/init.c",           "/usr/src/zeros/vm/init.c",
                                "/sys/src/vm/build.sh",         "/usr/src/zeros/vm/build.sh",
                                /* binarios del sistema — el disco puede ejecutarse sin initramfs */
                                "/bin/zeros",                   "/bin/zeros",
                                "/bin/tcc",                     "/bin/tcc",
                                "/bin/zeros_format",            "/bin/zeros_format",
                                "/bin/zeros_populate",          "/bin/zeros_populate",
                                "/bin/zeros_update",            "/bin/zeros_update",
                                "/bin/zeros_upgrade",           "/bin/zeros_upgrade",
                                "/bin/zeros_shell_update",      "/bin/zeros_shell_update",
                                NULL
                            };
                            char *pop_envp[] = { NULL };
                            execve("/bin/zeros_populate", pop_argv, pop_envp);
                            perror("init: no se pudo lanzar /bin/zeros_populate");
                            _exit(1);
                        }
                        if (pid_pop > 0) {
                            int pst;
                            waitpid(pid_pop, &pst, 0);
                            if (pst == 0)
                                printf("  Fuentes copiadas correctamente.\n");
                            else
                                printf("  \033[1;33mAviso: algunas fuentes no se copiaron.\033[0m\n");
                        } else {
                            perror("init: fork para zeros_populate");
                        }
                    }
                } else {
                    perror("init: fork para zeros_format");
                }
            } else {
                printf("  Arrancando sin filesystem.\n");
                disk_arg[0] = '\0';   /* no pasar el disco a zeros */
            }
        }
    }

    /* ── Levantar red (DHCP sobre eth0) ─────────────────────
     *
     * udhcpc es el cliente DHCP de BusyBox. Con -n falla si no
     * obtiene IP en lugar de quedarse en background, y con -q
     * sale tras obtenerla. Así el arranque no se bloquea si no
     * hay red disponible. */
    /* ── Configurar red (VirtualBox NAT) ────────────────────
     *
     * VirtualBox NAT siempre asigna la misma dirección (10.0.2.15).
     * Configuramos la interfaz directamente sin depender de DHCP. */
    printf("  Levantando red...\n");
    load_module("/lib/modules/e1000.ko");

    static const char *net_cmds[][8] = {
        { "/bin/busybox", "ifconfig", "eth0", "10.0.2.15",
          "netmask", "255.255.255.0", "up", NULL },
        { "/bin/busybox", "route", "add", "default",
          "gw", "10.0.2.2", NULL },
        { NULL }
    };

    int net_ok = 1;
    for (int c = 0; net_cmds[c][0]; c++) {
        pid_t pid = fork();
        if (pid == 0) {
            char *envp[] = { NULL };
            execve(net_cmds[c][0], (char **)net_cmds[c], envp);
            _exit(1);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) net_ok = 0;
        }
    }

    /* DNS — 10.0.2.3 es el proxy NAT de VirtualBox, 8.8.8.8 como fallback */
    FILE *resolv = fopen("/etc/resolv.conf", "w");
    if (resolv) {
        fprintf(resolv, "nameserver 10.0.2.3\n");
        fprintf(resolv, "nameserver 8.8.8.8\n");
        fclose(resolv);
    }

    printf("  Red %s.\n", net_ok ? "lista (10.0.2.15)" : "con errores");

    printf("  Lanzando shell...\n\n");

    /* ── Lanzar la shell en bucle ────────────────────────────
     *
     * Usamos fork()+exec() en lugar de execve() directamente.
     * Así, si zeros termina (exit, error, señal), init la reinicia
     * en lugar de morir y causar kernel panic.
     */
    char *envp[] = {
        "HOME=/",
        "PATH=/bin",
        "TERM=linux",
        NULL
    };

    while (1) {
        pid_t pid = fork();

        if (pid == 0) {
            /* Crear nueva sesión: este proceso se convierte en líder de sesión.
             * Sin setsid(), el kernel no sabe a quién enviar SIGINT al pulsar
             * Ctrl+C — el carácter se echa en pantalla pero la señal no llega.
             * Con setsid() + TIOCSCTTY, /dev/tty0 queda como terminal de
             * control de la sesión y el driver TTY puede enviar señales. */
            setsid();
            int tty_fd = open("/dev/tty0", O_RDWR);
            if (tty_fd >= 0) {
                ioctl(tty_fd, TIOCSCTTY, 0);
                dup2(tty_fd, STDIN_FILENO);
                dup2(tty_fd, STDOUT_FILENO);
                dup2(tty_fd, STDERR_FILENO);
                if (tty_fd > STDERR_FILENO) close(tty_fd);
            }
            /* Proceso hijo: lanzar la shell */
            if (disk_arg[0]) {
                char *argv[] = { "/bin/zeros", disk_arg, NULL };
                execve("/bin/zeros", argv, envp);
            } else {
                char *argv[] = { "/bin/zeros", NULL };
                execve("/bin/zeros", argv, envp);
            }
            perror("init: no se pudo lanzar /bin/zeros");
            _exit(1);
        }

        if (pid < 0) {
            perror("init: fork");
            sleep(2);
            continue;
        }

        /* Esperar a que zeros termine */
        int status;
        waitpid(pid, &status, 0);
        printf("\n  init: shell terminada — reiniciando...\n\n");
        sleep(1);
    }

    return 0;
}
