#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
extern char **environ;
#endif

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s <dylib-path> <program> [args...]\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

#ifndef __APPLE__
    fprintf(stderr, "error: injector is only functional on macOS (dyld required)\n");
    (void)argv;
    return 1;
#else
    const char *dylib = argv[1];
    char **child_argv = &argv[2];

    if (access(dylib, R_OK) != 0) {
        perror("dylib");
        return 1;
    }

    if (access(child_argv[0], X_OK) != 0) {
        perror("program");
        return 1;
    }

    char insert_env[4096];
    if (snprintf(insert_env, sizeof(insert_env), "DYLD_INSERT_LIBRARIES=%s", dylib) <= 0) {
        fprintf(stderr, "error: failed to format DYLD_INSERT_LIBRARIES\n");
        return 1;
    }

    const char *force_flat = "DYLD_FORCE_FLAT_NAMESPACE=1";

    // Build child environment: keep existing env, append our dyld vars.
    size_t env_count = 0;
    while (environ[env_count]) env_count++;

    char **child_env = calloc(env_count + 3, sizeof(char *));
    if (!child_env) {
        perror("calloc");
        return 1;
    }

    for (size_t i = 0; i < env_count; i++) {
        child_env[i] = environ[i];
    }
    child_env[env_count] = insert_env;
    child_env[env_count + 1] = (char *)force_flat;
    child_env[env_count + 2] = NULL;

    pid_t pid = 0;
    int rc = posix_spawn(&pid, child_argv[0], NULL, NULL, child_argv, child_env);
    if (rc != 0) {
        fprintf(stderr, "posix_spawn failed: %d\n", rc);
        free(child_env);
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        free(child_env);
        return 1;
    }

    free(child_env);
    return WEXITSTATUS(status);
#endif
}
