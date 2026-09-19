/* Offline tests for the first-run bootstrap. No network access. */

#include "atperson/bootstrap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char temp_dir[512];

static const char *make_temp_dir(const char *tag) {
    snprintf(temp_dir, sizeof(temp_dir), "%s/atperson-bootstrap-%s-%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", tag, (int)getpid());
    /* Remove-all: no portable recursive delete in plain C, but the bootstrap
     * only ever creates one directory level plus files, so remove the files
     * then the directory. */
    char path[600];
    snprintf(path, sizeof(path), "%s/.env", temp_dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/model.bin", temp_dir);
    remove(path);
    rmdir(temp_dir);
    return temp_dir;
}

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static void test_creates_directory_and_env(void) {
    const char *dir = make_temp_dir("create");
    assert(!file_exists(dir));

    char notice[ATP_BOOTSTRAP_NOTICE_BYTES] = {0};
    assert(atp_bootstrap_home(dir, notice, sizeof(notice)) == ATP_OK);
    assert(file_exists(dir));

    char env_path[600];
    snprintf(env_path, sizeof(env_path), "%s/.env", dir);
    assert(file_exists(env_path));

    /* Notice mentions both creations. */
    assert(notice[0] != '\0');
    assert(strstr(notice, "data directory") != NULL);
    assert(strstr(notice, ".env") != NULL);
}

static void test_idempotent_second_run(void) {
    const char *dir = make_temp_dir("idempotent");
    char notice[ATP_BOOTSTRAP_NOTICE_BYTES] = {0};
    assert(atp_bootstrap_home(dir, notice, sizeof(notice)) == ATP_OK);
    assert(notice[0] != '\0');

    /* Second run: nothing created, notice empty. */
    notice[0] = '\0';
    assert(atp_bootstrap_home(dir, notice, sizeof(notice)) == ATP_OK);
    assert(notice[0] == '\0');
}

static void test_env_template_lists_credentials(void) {
    const char *dir = make_temp_dir("template");
    assert(atp_bootstrap_home(dir, NULL, 0) == ATP_OK);

    char env_path[600];
    snprintf(env_path, sizeof(env_path), "%s/.env", dir);
    FILE *file = fopen(env_path, "r");
    assert(file != NULL);

    char contents[4096];
    const size_t read = fread(contents, 1, sizeof(contents) - 1, file);
    contents[read] = '\0';
    fclose(file);

    assert(strstr(contents, "ATPERSON_IDENTIFIER=") != NULL);
    assert(strstr(contents, "ATPERSON_APP_PASSWORD=") != NULL);
    assert(strstr(contents, "ATPERSON_SERVICE=") != NULL);
    assert(strstr(contents, "ATPERSON_HOME=") != NULL);
    assert(strstr(contents, "ATPERSON_MEMORY_BUDGET_BYTES=") != NULL);
    assert(strstr(contents, "ATPERSON_DISK_RESERVE_BYTES=") != NULL);
    assert(strstr(contents, "ATPERSON_NODE_CAPACITY=") != NULL);
    assert(strstr(contents, "ATPERSON_EDGE_CAPACITY=") != NULL);
    assert(strstr(contents, "ATPERSON_NEURAL_CAPACITY=") != NULL);
    assert(strstr(contents, "ATPERSON_NEURAL_EMBEDDING_DIM=") != NULL);
    assert(strstr(contents, "ATPERSON_NEURAL_HIDDEN_LAYERS=") != NULL);
    assert(strstr(contents, "ATPERSON_NEURAL_HIDDEN_WIDTHS=") != NULL);
    assert(strstr(contents, "ATPERSON_SYNC_PAGE_SIZE=") != NULL);
    assert(strstr(contents, "ATPERSON_SYNC_MAX_OBSERVATIONS=") != NULL);
    assert(strstr(contents, "ATPERSON_JETSTREAM_STATE=") != NULL);
    assert(strstr(contents, "ATPERSON_JETSTREAM_ENDPOINT=") != NULL);
    assert(strstr(contents, "ATPERSON_JETSTREAM_COLLECTIONS_FILE=") != NULL);
    assert(strstr(contents, "ATPERSON_JETSTREAM_DIDS_FILE=") != NULL);
}

static void test_env_permissions_are_owner_only(void) {
    const char *dir = make_temp_dir("perms");
    assert(atp_bootstrap_home(dir, NULL, 0) == ATP_OK);

    char env_path[600];
    snprintf(env_path, sizeof(env_path), "%s/.env", dir);
    struct stat info;
    assert(stat(env_path, &info) == 0);
    assert((info.st_mode & 0777) == 0600);

    assert(stat(dir, &info) == 0);
    assert((info.st_mode & 0777) == 0700);
}

static void test_existing_env_is_not_touched(void) {
    const char *dir = make_temp_dir("preserve");
    assert(mkdir(dir, 0700) == 0);

    char env_path[600];
    snprintf(env_path, sizeof(env_path), "%s/.env", dir);
    FILE *file = fopen(env_path, "w");
    assert(file != NULL);
    fputs("ATPERSON_IDENTIFIER=custom\n", file);
    fclose(file);

    char notice[ATP_BOOTSTRAP_NOTICE_BYTES] = {0};
    assert(atp_bootstrap_home(dir, notice, sizeof(notice)) == ATP_OK);
    /* Directory existed, .env existed: nothing created. */
    assert(notice[0] == '\0');

    file = fopen(env_path, "r");
    char contents[256];
    const size_t read = fread(contents, 1, sizeof(contents) - 1, file);
    contents[read] = '\0';
    fclose(file);
    assert(strcmp(contents, "ATPERSON_IDENTIFIER=custom\n") == 0);
}

static void test_invalid_arguments(void) {
    assert(atp_bootstrap_home(NULL, NULL, 0) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_bootstrap_home("", NULL, 0) == ATP_ERR_INVALID_ARGUMENT);
}

static void test_default_home_directory(void) {
    char buffer[4096];
    size_t required = 0;

    /* With ATPERSON_HOME set, it wins verbatim. */
    setenv("ATPERSON_HOME", "/tmp/atperson-override", 1);
    assert(atp_default_home_directory(buffer, sizeof(buffer), &required) == buffer);
    assert(strcmp(buffer, "/tmp/atperson-override") == 0);
    assert(required == strlen("/tmp/atperson-override") + 1);
    unsetenv("ATPERSON_HOME");

    /* Without it, $HOME/.ewanc26/atperson. */
    assert(atp_default_home_directory(buffer, sizeof(buffer), &required) == buffer);
    const char *home = getenv("HOME");
    assert(home != NULL);
    assert(strncmp(buffer, home, strlen(home)) == 0);
    assert(strstr(buffer, "/.ewanc26/atperson") != NULL);

    /* Too-small buffer reports the required size and returns NULL. */
    assert(atp_default_home_directory(buffer, 2, &required) == NULL);
    assert(required > 2);
}

int main(void) {
    test_creates_directory_and_env();
    test_idempotent_second_run();
    test_env_template_lists_credentials();
    test_env_permissions_are_owner_only();
    test_existing_env_is_not_touched();
    test_invalid_arguments();
    test_default_home_directory();

    printf("bootstrap tests passed\n");
    return 0;
}
