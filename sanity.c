#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <sys/swap.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define FIFO_PATH "/run/sanity.fifo"

/* --- Power Management & Solaris-style Kill --- */

void kill_the_world() {
    sync();
    printf("\n** Sending SIGTERM to all processes... **\n");
    kill(-1, SIGTERM);
    sleep(2);
    printf("** Sending SIGKILL to all processes... **\n");
    kill(-1, SIGKILL);
    sync();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
}

void poweroff(int sig) {
    printf("\n** The system is going DOWN now... **\n");
    kill_the_world();
    reboot(RB_POWER_OFF);
}

void restart(int sig)  {
    printf("\n** The system is going DOWN \'n then UP now... **\n");
    kill_the_world();
    reboot(RB_AUTOBOOT);
}

void panic() {
    printf("\n** KERNEL PANIC: Manually exiting PID 1... **\n");
    exit(1); 
}

/* --- Helpers --- */

void run(char *cmd, char *args[]) {
    if (access(cmd, X_OK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        execv(cmd, args);
        _exit(1);
    }
    waitpid(pid, NULL, 0);
}

/* --- Subsystem Modules --- */

void setup_api_filesystems() {
    printf("** Mounting API filesystems... **\n");
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mkdir("/run", 0755);
    mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=32M");
    mkdir("/dev/shm", 0755);
    mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

void setup_terminal_subsystem() {
    printf("** Setting up devpts... **\n");
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620");
}

void setup_hardware() {
    printf("** Starting udevd and triggering scan... **\n");
    pid_t udev_pid = fork();
    if (udev_pid == 0) {
        char *udev_args[] = {"/sbin/udevd", "--daemon", NULL};
        execv("/sbin/udevd", udev_args);
        _exit(1);
    }
    sleep(1);
    char *trigger_args[] = {"/sbin/udevadm", "trigger", "--action=add", NULL};
    run("/sbin/udevadm", trigger_args);
    char *settle_args[] = {"/sbin/udevadm", "settle", NULL};
    run("/sbin/udevadm", settle_args);
}

void setup_identity() {
    printf("** Remounting root RW and setting hostname... **\n");
    mount(NULL, "/", NULL, MS_REMOUNT, NULL);
    FILE *fp = fopen("/etc/HOSTNAME", "r");
    if (fp) {
        char name[64];
        if (fgets(name, sizeof(name), fp)) {
            name[strcspn(name, "\r\n")] = 0;
            sethostname(name, strlen(name));
        }
        fclose(fp);
    }
}

void setup_swap() {
    printf("** Activating swap... **\n");
    FILE *fp = fopen("/etc/SWAP", "r");
    if (fp) {
        char swap_path[256];
        if (fgets(swap_path, sizeof(swap_path), fp)) {
            swap_path[strcspn(swap_path, "\r\n")] = 0;
            if (swapon(swap_path, 0) == 0) {
                printf("** Enabled swap on %s **\n", swap_path);
            }
        }
        fclose(fp);
    }
}

void run_rc_scripts() {
    struct dirent **namelist;
    int n;
    char path[512];
    printf("** Processing /etc/sanity.d/ **\n");
    n = scandir("/etc/sanity.d", &namelist, NULL, alphasort);
    if (n < 0) {
        perror("scandir /etc/sanity.d");
        return;
    }
    for (int i = 0; i < n; i++) {
        if (strstr(namelist[i]->d_name, ".rc")) {
            snprintf(path, sizeof(path), "/etc/sanity.d/%s", namelist[i]->d_name);
            printf("  [rc] Executing %s\n", namelist[i]->d_name);
            char *args[] = {"/bin/sh", path, "start", NULL};
            run("/bin/sh", args);
        }
        free(namelist[i]);
    }
    free(namelist);
}

/* --- Main Entry --- */

int main() {
    if (getpid() != 1) return 1;

    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin", 1);
    setenv("TERM", "linux", 1);

    signal(SIGUSR1, poweroff);
    signal(SIGINT,  restart);
    reboot(RB_DISABLE_CAD);

    printf("\n** Init v0.5: Proper Boot & IPC **\n");

    /* Original Bootstrap */
    setup_api_filesystems();
    setup_hardware();
    setup_identity();
    setup_terminal_subsystem();
    setup_swap();
    run_rc_scripts();

    /* IPC Setup */
    unlink(FIFO_PATH);
    mkfifo(FIFO_PATH, 0600);
    int fifo_fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);

    printf("** Launching Supervisor Loop (TTY1) **\n");

    pid_t getty_pid = -1;

    while (1) {
        /* Check IPC Commands */
        char buf[64];
        ssize_t cmd_len = read(fifo_fd, buf, sizeof(buf) - 1);
        if (cmd_len > 0) {
            buf[cmd_len] = '\0';
            if (strcmp(buf, "die") == 0) {
                printf("** The system is going DOWN now... **\n");
                kill_the_world();
                printf("** All processes killed. System Halted. **\n");
                while(1) pause();
            } 
            else if (strcmp(buf, "panic") == 0) panic();
            else if (strcmp(buf, "off") == 0) poweroff(0);
            else if (strcmp(buf, "reboot") == 0) restart(0);
        }

        /* Respawn Getty */
        if (getty_pid <= 0) {
            getty_pid = fork();
            if (getty_pid == 0) {
                setsid();
                int fd = open("/dev/tty1", O_RDWR);
                if (fd >= 0) {
                    ioctl(fd, TIOCSCTTY, 1);
                    close(fd);
                }
                char *agetty_args[] = {"/sbin/agetty", "--noclear", "tty1", "115200", "linux", NULL};
                execv("/sbin/agetty", agetty_args);
                _exit(1);
            }
        }

        /* The Reaper */
        int status;
        pid_t reaped = waitpid(-1, &status, WNOHANG);
        if (reaped == getty_pid) {
            printf("** Getty on tty1 exited. Respawning... **\n");
            getty_pid = -1;
        }

        usleep(50000); 
    }

    return 0;
}
