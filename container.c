#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <linux/limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <wait.h>

#include "change_root.h"

#define CONTAINER_ID_MAX 16
#define CHILD_STACK_SIZE 4096 * 10

typedef struct container {
  char id[CONTAINER_ID_MAX];
  // Add fields
  char image[PATH_MAX];
  int argc;
  char** argv;
} container_t;

/**
 * `usage` prints the usage of `client` and exists the program with
 * `EXIT_FAILURE`
 */
void usage(char* cmd) {
  printf("Usage: %s [ID] [IMAGE] [CMD]...\n", cmd);
  exit(EXIT_FAILURE);
}

/**
 * `container_exec` is an entry point of a child process and responsible for
 * creating an overlay filesystem, calling `change_root` and executing the
 * command given as arguments.
 */
int container_exec(void* arg) {
  container_t* container = (container_t*)arg;
  // this line is required on some systems
  if (mount("/", "/", "none", MS_PRIVATE | MS_REC, NULL) < 0) {
    err(1, "mount / private");
  }

  char lower_path[PATH_MAX];
  char upper_path[PATH_MAX];
  char work_path[PATH_MAX];
  char merged_path[PATH_MAX];

  // Create a overlay filesystem

  // `lowerdir`  should be the image directory: ${cwd}/images/${image}
  sprintf(lower_path, "%s/images/%s", getcwd(NULL, 0), container->image);
  if (mkdir(lower_path, 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create a directory to store container file systems");
  }

  char id_path[PATH_MAX];
  sprintf(id_path, "/tmp/container/%s", container->id);
  if (mkdir(id_path, 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create container/id directory");
  }
  // `upperdir`  should be `/tmp/container/{id}/upper`
  sprintf(upper_path, "%s/upper", id_path);
  if (mkdir(upper_path, 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create a directory to store container file systems");
  }
  printf("made upper dir\n");
  // `workdir`   should be `/tmp/container/{id}/work`
  sprintf(work_path, "%s/work", id_path);
  if (mkdir(work_path, 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create a directory to store container file systems");
  }
  printf("made work dir\n");
  // `merged`    should be `/tmp/container/{id}/merged`
  sprintf(merged_path, "%s/merged", id_path);
  if (mkdir(merged_path, 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create a directory to store container file systems");
  }
  printf("made merge dir\n");
  // ensure all directories exist (create if not exists) and
  // call `mount("overlay", merged, "overlay", MS_RELATIME,
  //    lowerdir={lowerdir},upperdir={upperdir},workdir={workdir})`
  char all_dirs[PATH_MAX * 4];
  sprintf(all_dirs, "lowerdir=%s,upperdir=%s,workdir=%s", lower_path,
          upper_path, work_path);
  int return_val =
      mount("overlay", merged_path, "overlay", MS_RELATIME, all_dirs);
  if (return_val < 0) {
    err(1, "Failed to mount overlay");
  }

  // Call `change_root` with the `merged` directory
  change_root(merged_path);

  // use `execvp` to run the given command and return its return value
  int ret_val = execvp(container->argv[3], &container->argv[3]);
  return ret_val;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    usage(argv[0]);
  }

  /* Create tmpfs and mount it to `/tmp/container` so overlayfs can be used
   * inside Docker containers */
  if (mkdir("/tmp/container", 0700) < 0 && errno != EEXIST) {
    err(1, "Failed to create a directory to store container file systems");
  }
  if (errno != EEXIST) {
    if (mount("tmpfs", "/tmp/container", "tmpfs", 0, "") < 0) {
      err(1, "Failed to mount tmpfs on /tmp/container");
    }
  }

  /* cwd contains the absolute path to the current working directory which can
   * be useful constructing path for image */
  char cwd[PATH_MAX];
  getcwd(cwd, PATH_MAX);

  container_t container;
  strncpy(container.id, argv[1], CONTAINER_ID_MAX);
  strncpy(container.image, argv[2], PATH_MAX);
  container.argc = argc;
  container.argv = argv;

  // store all necessary information to `container`

  /* Use `clone` to create a child process */
  char child_stack[CHILD_STACK_SIZE];  // statically allocate stack for child
  int clone_flags = SIGCHLD | CLONE_NEWNS | CLONE_NEWPID;
  int pid = clone(container_exec, &child_stack, clone_flags, &container);
  if (pid < 0) {
    err(1, "Failed to clone");
  }

  waitpid(pid, NULL, 0);
  return EXIT_SUCCESS;
}
