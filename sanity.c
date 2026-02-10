#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>

/* --- Power Management --- */

void poweroff(int sig) { 
    printf("** %s... **\n", "Shutting down");
    sync();
    reboot(RB_POWER_OFF); 
}

void restart(int sig)  { 
    printf("** %s... **\n", "Rebooting");
    sync();
    reboot(RB_AUTOBOOT); 
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
    printf("** %s... **\n", "Mounting /proc and /sys");
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    
    printf("** %s... **\n", "Mounting /run and /dev/shm");
    mkdir("/run", 0755);
    mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=32M");
    mkdir("/dev/shm", 0755);
    mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

void setup_terminal_subsystem() {
    printf("** %s... **\n", "Mounting /dev/pts for openpty");
    mkdir("/dev/pts", 0755);
    /* gid=5 is 'tty' on Slackware; mode=620 allows WezTerm/others to allocate PTYs */
    if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620") != 0) {
        perror("devpts mount failed");
    }
}

void setup_hardware() {
    printf("** %s... **\n", "Starting udevd");
    pid_t udev_pid = fork();
    if (udev_pid == 0) {
        char *udev_args[] = {"/sbin/udevd", "--daemon", NULL};
        execv("/sbin/udevd", udev_args);
        _exit(1);
    }
    sleep(1); 

    printf("** %s... **\n", "Triggering hardware scan");
    char *trigger_args[] = {"/sbin/udevadm", "trigger", "--action=add", NULL};
    run("/sbin/udevadm", trigger_args);

    printf("** %s... **\n", "Settling devices");
    char *settle_args[] = {"/sbin/udevadm", "settle", NULL};
    run("/sbin/udevadm", settle_args);
}

void setup_identity() {
    printf("** %s... **\n", "Remounting root read-write");
    mount(NULL, "/", NULL, MS_REMOUNT, NULL);

    FILE *fp = fopen("/etc/HOSTNAME", "r");
    if (fp) {
        char name[64];
        if (fgets(name, sizeof(name), fp)) {
            name[strcspn(name, "\n")] = 0;
            sethostname(name, strlen(name));
            printf("** %s: %s... **\n", "Setting hostname", name);
        }
        fclose(fp);
    }
}

void setup_network() {
    printf("** %s... **\n", "Initializing Network");
    char *net_args[] = {"/etc/rc.d/rc.inet1", "start", NULL};
    run("/etc/rc.d/rc.inet1", net_args);
}

/* --- Main Entry --- */

int main() {
    if (getpid() != 1) return 1;

    /* Initial Environment */
    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin", 1);
    setenv("TERM", "linux", 1);

    /* Signal Handling */
    signal(SIGUSR1, poweroff);
    signal(SIGINT,  restart);
    reboot(RB_DISABLE_CAD);

    printf("** %s... **\n", "Booting Sanity Standalone");

    /* Execution Flow */
    setup_api_filesystems();
    setup_hardware();           /* Hardware scan must happen before terminal/net */
    setup_terminal_subsystem(); /* Fixes WezTerm/openpty screams */
    setup_identity();
    setup_network();

    /* Supervisor Loop */
    printf("** %s... **\n", "Launching Getty");
    pid_t getty_pid = -1;

    while (1) {
        if (getty_pid == -1) {
            getty_pid = fork();
            if (getty_pid == 0) {
                char *agetty_args[] = {"/sbin/agetty", "--noclear", "tty1", "115200", "linux", NULL};
                execv("/sbin/agetty", agetty_args);
                _exit(1);
            }
        }

        int status;
        pid_t reaped = wait(&status);

        if (reaped == getty_pid) {
            printf("** %s... **\n", "Getty exited. Respawning");
            getty_pid = -1;
            sleep(1); 
        } 
        /* Back-end reaper handles any other orphaned children */
    }

    return 0;
}

