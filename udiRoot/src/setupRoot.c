/** @file setupRoot.c
 *  @brief Prepare a shifter environment based on image in the filesystem.
 *
 *  The setupRoot program prepares a shifter environment, including performing
 *  site-required modifications and user-requested bind mounts.  This is 
 *  intended to be run by a WLM prologue prior to batch script execution.
 *
 *  @author Douglas M. Jacobsen <dmjacobsen@lbl.gov>
 */

/* Shifter, Copyright (c) 2015, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. Neither the name of the University of California, Lawrence Berkeley
 *     National Laboratory, U.S. Dept. of Energy nor the names of its
 *     contributors may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *  
 * You are under no obligation whatsoever to provide any bug fixes, patches, or
 * upgrades to the features, functionality or performance of the source code
 * ("Enhancements") to anyone; however, if you choose to make your Enhancements
 * available either publicly, or directly to Lawrence Berkeley National
 * Laboratory, without imposing a separate written license agreement for such
 * Enhancements, then you hereby grant the following license: a  non-exclusive,
 * royalty-free perpetual license to install, use, modify, prepare derivative
 * works, incorporate into other computer software, distribute, and sublicense
 * such enhancements or derivative works thereof, in binary and source code
 * form.
 */


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include "ImageData.h"
#include "UdiRootConfig.h"

typedef struct _SetupRootConfig {
    char *sshPubKey;
    char *user;
    char *imageType;
    char *imageIdentifier;
    uid_t uid;
    char *minNodeSpec;
    char **volumeMapFrom;
    char **volumeMapTo;
    char **volumeMapFlags;
    size_t volumeMapCount;
    int verbose;
} SetupRootConfig;

static void _usage(int exitStatus);
static int _forkAndExec(char **args);
static int _bindMount(const char **mountCache, const char *from, const char *to, unsigned long flags);
static int _sortFsForward(const void *, const void *);
static int _sortFsReverse(const void *, const void *);
int parse_SetupRootConfig(int argc, char **argv, SetupRootConfig *config);
void free_SetupRootConfig(SetupRootConfig *config);
void fprint_SetupRootConfig(FILE *, SetupRootConfig *config);
int getImage(ImageData *, SetupRootConfig *, UdiRootConfig *);
char **parseMounts(size_t *n_mounts);

static char *_filterString(char *);

int main(int argc, char **argv) {
    UdiRootConfig udiConfig;
    SetupRootConfig config;

    ImageData image;

    memset(&udiConfig, 0, sizeof(UdiRootConfig));
    memset(&config, 0, sizeof(SetupRootConfig));
    memset(&image, 0, sizeof(ImageData));

    clearenv();
    setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

    if (parse_SetupRootConfig(argc, argv, &config) != 0) {
        fprintf(stderr, "FAILED to parse command line arguments. Exiting.\n");
        _usage(1);
    }
    if (parse_UdiRootConfig(&udiConfig, 0) != 0) {
        fprintf(stderr, "FAILED to parse udiRoot configuration. Exiting.\n");
        exit(1);
    }
    if (config.verbose) {
        fprint_SetupRootConfig(stdout, &config);
        fprint_UdiRootConfig(stdout, &udiConfig);
    }

    if (getImage(&image, &config, &udiConfig) != 0) {
        fprintf(stderr, "FAILED to get image %s of type %s\n", config.imageIdentifier, config.imageType);
        exit(1);
    }
    fprint_ImageData(stdout, &image);
    return 0;
}

static void _usage(int exitStatus) {
    exit(exitStatus);
}

int parse_SetupRootConfig(int argc, char **argv, SetupRootConfig *config) {
    int opt = 0;
    optind = 1;

    while ((opt = getopt(argc, argv, "v:s:u:U:N:V")) != -1) {
        switch (opt) {
            case 'V': config->verbose = 1; break;
            case 'v':
                {
                    char *from  = strtok(optarg, ":");
                    char *to    = strtok(NULL,   ":");
                    char *flags = strtok(NULL,   ":");

                    if (from == NULL || to == NULL) {
                        fprintf(stderr, "ERROR: invalid format for volume map!");
                        _usage(1);
                    }

                    config->volumeMapFrom = (char **) realloc(
                        config->volumeMapFrom,
                        sizeof(char*) * (config->volumeMapCount + 1)
                    );
                    config->volumeMapTo = (char **) realloc(
                        config->volumeMapTo,
                        sizeof(char*) * (config->volumeMapCount + 1)
                    );
                    config->volumeMapFlags = (char **) realloc(
                        config->volumeMapFlags,
                        sizeof(char*) * (config->volumeMapCount + 1)
                    );

                    config->volumeMapFrom[config->volumeMapCount] = strdup(from);
                    config->volumeMapTo[config->volumeMapCount] = strdup(to);
                    config->volumeMapFlags[config->volumeMapCount] = (flags != NULL ? strdup(flags) : NULL);
                    config->volumeMapCount++;
                }

                break;
            case 's':
                config->sshPubKey = strdup(optarg);
                break;
            case 'u':
                config->user = strdup(optarg);
                break;
            case 'U':
                config->uid = strtoul(optarg, NULL, 10);
                break;
            case 'N':
                config->minNodeSpec = strdup(optarg);
                break;
            case '?':
                fprintf(stderr, "Missing an argument!\n");
                _usage(1);
                break;
            default:
                break;
        }
    }

    int remaining = argc - optind;
    if (remaining != 2) {
        fprintf(stderr, "Must specify image type and image identifier\n");
        _usage(1);
    }
    config->imageType = _filterString(argv[optind++]);
    config->imageIdentifier = _filterString(argv[optind++]);
    return 0;
}

void free_SetupRootConfig(SetupRootConfig *config) {
    size_t idx = 0;
    if (config->sshPubKey != NULL) {
        free(config->sshPubKey);
    }
    if (config->user != NULL) {
        free(config->user);
    }
    if (config->imageType != NULL) {
        free(config->imageType);
    }
    if (config->imageIdentifier != NULL) {
        free(config->imageIdentifier);
    }
    if (config->minNodeSpec != NULL) {
        free(config->minNodeSpec);
    }
    for (idx = 0; idx < config->volumeMapCount; idx++) {
        if (config->volumeMapFrom && config->volumeMapFrom[idx]) {
            free(config->volumeMapFrom[idx]);
        }
        if (config->volumeMapTo && config->volumeMapTo[idx]) {
            free(config->volumeMapTo[idx]);
        }
        if (config->volumeMapFlags && config->volumeMapFlags[idx]) {
            free(config->volumeMapFlags[idx]);
        }
    }
    if (config->volumeMapFrom) {
        free(config->volumeMapFrom);
    }
    if (config->volumeMapTo) {
        free(config->volumeMapTo);
    }
    if (config->volumeMapFlags) {
        free(config->volumeMapFlags);
    }
    free(config);
}

void fprint_SetupRootConfig(FILE *fp, SetupRootConfig *config) {
    if (config == NULL || fp == NULL) return;
    fprintf(fp, "***** SetupRootConfig *****\n");
    fprintf(fp, "imageType: %s\n", (config->imageType ? config->imageType : ""));
    fprintf(fp, "imageIdentifier: %s\n", (config->imageIdentifier ? config->imageIdentifier : ""));
    fprintf(fp, "sshPubKey: %s\n", (config->sshPubKey ? config->sshPubKey : ""));
    fprintf(fp, "user: %s\n", (config->user ? config->user : ""));
    fprintf(fp, "uid: %d\n", config->uid);
    fprintf(fp, "minNodeSpec: %s\n", (config->minNodeSpec ? config->minNodeSpec : ""));
    fprintf(fp, "volumeMap: %lu maps\n", config->volumeMapCount);
    if (config->volumeMapCount > 0) {
        size_t idx = 0;
        for (idx = 0; idx < config->volumeMapCount; idx++) {
            fprintf(fp, "    FROM: %s, TO: %s, FLAGS: %s\n",
                (config->volumeMapFrom && config->volumeMapFrom[idx] ? config->volumeMapFrom[idx] : "Unknown"),
                (config->volumeMapTo && config->volumeMapTo[idx] ? config->volumeMapTo[idx] : "Unknown"),
                (config->volumeMapFlags && config->volumeMapFlags[idx] ? config->volumeMapFlags[idx] : "NONE")
            );
        }
    }
    fprintf(fp, "***** END SetupRootConfig *****\n");
}

int getImage(ImageData *imageData, SetupRootConfig *config, UdiRootConfig *udiConfig) {
    int ret = parse_ImageData(config->imageType, config->imageIdentifier, udiConfig, imageData);
    return ret;
}

//! Setup all required files/paths for site mods to the image
/*!
  Setup all required files/paths for site mods to the image.  This should be
  called before performing any bindmounts or other discovery of the user image.
  Any paths created here should then be ignored by all other setup function-
  ality; i.e., no bind-mounts over these locations/paths.

  The site-configured bind-mounts will also be performed here.

  \param config configuration for this invokation of setupRoot
  \param udiConfig global configuration for udiRoot
  \return 0 for succss, 1 otherwise
*/
int prepareSiteModifications(SetupRootConfig *config, UdiRootConfig *udiConfig) {

    // construct path to "live" copy of the image.
    char udiRoot[PATH_MAX];
    char mntBuffer[PATH_MAX];
    char srcBuffer[PATH_MAX];
    char **volPtr = NULL;
    char **mountCache = NULL;
    char **mntPtr = NULL;
    int ret = 0;
    size_t mountCache_cnt = 0;
    struct stat statData;

    snprintf(udiRoot, PATH_MAX, "%s%s", udiConfig->nodeContextPrefix, udiConfig->udiMountPoint);
    udiRoot[PATH_MAX-1] = 0;
    if (chdir(udiRoot) != 0) {
        fprintf(stderr, "FAILED to chdir to %s. Exiting.\n", udiRoot);
        return 1;
    }

    // get list of current mounts for this namespace
    mountCache = parseMounts(&mountCache_cnt);
    if (mountCache == NULL) {
        fprintf(stderr, "FAILED to get list of current mount points\n");
        return 1;
    }
    qsort(mountCache, mountCache_cnt, sizeof(char*), _sortFsForward);

    // create all the directories needed for initial setup
#define _MKDIR(dir, perm) if (mkdir(dir, perm) != 0) { \
    fprintf(stderr, "FAILED to mkdir %s. Exiting.\n", dir); \
    ret = 1; \
    goto _prepSiteMod_unclean; \
}
#define _BINDMOUNT(mountCache, from, to, ro) if (_bindMount(mountCache, from, to, ro) != 0) { \
    fprintf(stderr, "BIND MOUNT FAILED from %s to %s\n", from, to); \
    ret = 1; \
    goto _prepSiteMod_unclean; \
}

    _MKDIR("etc", 0755);
    _MKDIR("etc/local", 0755);
    _MKDIR("etc/udiImage", 0755);
    _MKDIR("etc", 0755);
    _MKDIR("opt", 0755);
    _MKDIR("opt/udiImage", 0755);
    _MKDIR("var", 0755);
    _MKDIR("var/spool", 0755);
    _MKDIR("var/run", 0755);
    _MKDIR("proc", 0755);
    _MKDIR("sys", 0755);
    _MKDIR("dev", 0755);
    _MKDIR("tmp", 0777);

    // run site-defined pre-mount procedure
    if (strlen(udiConfig->sitePreMountHook) > 0) {
        char *args[3] = {
            "/bin/sh", udiConfig->sitePreMountHook, NULL
        };
        int ret = _forkAndExec(args);
        if (ret != 0) {
            fprintf(stderr, "Site premount hook failed. Exiting.\n");
            ret = 1;
            goto _prepSiteMod_unclean;
        }
    }

    // do site-defined mount activities
    for (volPtr = udiConfig->volumes; *volPtr != NULL; volPtr++) {
        char to_buffer[PATH_MAX];
        snprintf(to_buffer, PATH_MAX, "%s/%s", udiRoot, *volPtr);
        to_buffer[PATH_MAX-1] = 0;
        _MKDIR(to_buffer, 0755);
        _BINDMOUNT(mountCache, *volPtr, to_buffer, 0);
    }

    // run site-defined post-mount procedure
    if (strlen(udiConfig->sitePostMountHook) > 0) {
        char *args[3] = {
            "/bin/sh", udiConfig->sitePostMountHook, NULL
        };
        int ret = _forkAndExec(args);
        if (ret != 0) {
            fprintf(stderr, "Site premount hook failed. Exiting.\n");
            ret = 1;
            goto _prepSiteMod_unclean;
        }
    }

    // setup site needs for /etc
    snprintf(mntBuffer, PATH_MAX, "%s/etc/local", udiRoot);
    mntBuffer[PATH_MAX-1] = 0;
    _BINDMOUNT(mountCache, "/etc", mntBuffer, 0);
    // --> loop over everything in site etc-files and copy into image etc
    snprintf(srcBuffer, PATH_MAX, "%s/%s", udiConfig->nodeContextPrefix, udiConfig->etcDir);
    srcBuffer[PATH_MAX-1] = 0;
    memset(&statData, 0, sizeof(struct stat));
    if (stat(srcBuffer, &statData) == 0) {
        DIR *etcDir = opendir(srcBuffer);
        struct dirent *entry = NULL;
        while ((entry = readdir(etcDir)) != NULL) {
            char *filename = _filterString(entry->d_name);
            if (filename == NULL) {
                fprintf(stderr, "FAILED to allocate filename string.\n");
                goto _prepSiteMod_unclean;
            }
            snprintf(srcBuffer, PATH_MAX, "%s/%s/%s", udiConfig->nodeContextPrefix, udiConfig->etcDir, filename);
            srcBuffer[PATH_MAX-1] = 0;
            snprintf(mntBuffer, PATH_MAX, "%s/etc/%s", udiRoot, filename);
            mntBuffer[PATH_MAX-1] = 0;
            free(filename);

            if (lstat(srcBuffer, &statData) != 0) {
                fprintf(stderr, "Coudldn't fine source file, check if there are illegal characters: %s\n", srcBuffer);
                goto _prepSiteMod_unclean;
            }

            if (lstat(mntBuffer, &statData) == 0) {
                fprintf(stderr, "Couldn't copy %s because file already exists.\n", mntBuffer);
                goto _prepSiteMod_unclean;
            } else {
                char *args[5] = { "cp", "-p", srcBuffer, mntBuffer, NULL };
                if (_forkAndExec(args) != 0) {
                    fprintf(stderr, "Failed to copy %s to %s.\n", srcBuffer, mntBuffer);
                    goto _prepSiteMod_unclean;
                }
            }
        }
        closedir(etcDir);
    } else {
        fprintf(stderr, "Couldn't stat udiRoot etc dir: %s\n", srcBuffer);
        ret = 1;
        goto _prepSiteMod_unclean;
    }

    // setup built-in sshd

    // setup hostlist for current allocation

    //***** setup linux needs ******//
    // mount /proc
    snprintf(mntBuffer, PATH_MAX, "%s/proc", udiRoot);
    mntBuffer[PATH_MAX-1] = 0;
    if (mount(NULL, mntBuffer, "proc", MS_REC|MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) != 0) {
        fprintf(stderr, "FAILED to mount /proc\n");
        ret = 1;
        goto _prepSiteMod_unclean;
    }

    // mount /sys
    snprintf(mntBuffer, PATH_MAX, "%s/sys", udiRoot);
    mntBuffer[PATH_MAX-1] = 0;
    _BINDMOUNT(mountCache, "/sys", mntBuffer, 0);

    // mount /dev
    snprintf(mntBuffer, PATH_MAX, "%s/dev", udiRoot);
    mntBuffer[PATH_MAX-1] = 0;
    _BINDMOUNT(mountCache, "/dev", mntBuffer, 0);

    // mount any mount points under /dev
    for (mntPtr = mountCache; *mntPtr != NULL; mntPtr++) {
        if (strncmp(*mntPtr, "/dev/", 4) == 0) {
            snprintf(mntBuffer, PATH_MAX, "%s/%s", udiRoot, *mntPtr);
            mntBuffer[PATH_MAX-1] = 0;
            _BINDMOUNT(mountCache, *mntPtr, mntBuffer, 0);
        }
    }

#undef _MKDIR
#undef _BINDMOUNT

    return 0;
_prepSiteMod_unclean:
    if (mountCache != NULL) {
        char **ptr = NULL;
        for (ptr = mountCache; *ptr != NULL; ptr++) {
            free(*ptr);
        }
        free(mountCache);
        mountCache = NULL;
    }
    mountCache = parseMounts(&mountCache_cnt);
    if (mountCache != NULL) {
        size_t udiRoot_len = strlen(udiRoot);
        char **ptr = NULL;

        qsort(mountCache, mountCache_cnt, sizeof(char*), _sortFsReverse);
        for (ptr = mountCache; *ptr != NULL; ptr++) {
            if (strncmp(*ptr, udiRoot, udiRoot_len) == 0) {
                umount(*ptr);
            }
            free(*ptr);
        }
        free(mountCache);
        mountCache = NULL;
    }
    return 1;
}

int mountImageVFS(ImageData *imageData, SetupRootConfig *config, UdiRootConfig *udiConfig) {
    return 0;
}

int mountImageLoop(ImageData *imageData, SetupRootConfig *config, UdiRootConfig *udiConfig) {
    return 0;
} 

static int _forkAndExec(char **args) {
    pid_t pid = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "FAILED to fork! Exiting.\n");
        return 1;
    }
    if (pid > 0) {
        // this is the parent
        int status = 0;
        do {
            pid_t ret = waitpid(pid, &status, 0);
            if (ret != pid) {
                fprintf(stderr, "This might be impossible: forked by couldn't wait, FAIL!\n");
                return 1;
            }
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        if (WIFEXITED(status)) {
            status = WEXITSTATUS(status);
        } else {
            status = 1;
        }
        return status;
    }
    // this is the child
    execv(args[0], args);
    fprintf(stderr, "FAILED to execv! Exiting.\n");
    exit(1);
    return 1;
}

static int _bindMount(const char **mountCache, const char *from, const char *to, int ro) {
    int ret = 0;
    int err = 0;
    const char **ptr = mountCache;
    char *to_real = NULL;
    unsigned long remountFlags = MS_REMOUNT|MS_BIND|MS_NOSUID;

    if (from == NULL || to == NULL || mountCache == NULL) {
        fprintf(stderr, "INVALID input to bind-mount. Fail\n");
        return 1;
    }

    to_real = realpath(to, NULL);
    if (to_real == NULL) {
        fprintf(stderr, "Couldn't lookup path %s, fail.\n", to);
        return 1;
    }

    // not interesting in mounting over existing mounts, prevents
    // things from getting into a weird state later.
    if (mountCache != NULL) {
        for (ptr = mountCache; *ptr; ptr++) {
            if (strcmp(*ptr, to_real) == 0) {
                fprintf(stderr, "%s is already mounted. Refuse to remount.  Fail.\n", to_real);
                ret = 1;
                goto _bindMount_exit;
            }
        }
    }

    // perform the initial bind-mount
    ret = mount(from, to, "bind", MS_BIND, NULL);
    if (ret != 0) {
        goto _bindMount_unclean;
    }

    // if the source is exactly /dev or starts with /dev/ then
    // ALLOW device entires, otherwise remount with noDev
    if (strcmp(from, "/dev") != 0 && strncmp(from, "/dev/", 5) != 0) {
        remountFlags |= MS_NODEV;
    }

    // if readonly is requested, then do it
    if (ro) {
        remountFlags |= MS_RDONLY;
    }

    // remount the bind-mount to get the needed mount flags
    ret = mount(from, to, "bind", remountFlags, NULL);
    if (ret != 0) {
        goto _bindMount_unclean;
    }
_bindMount_exit:
    if (to_real != NULL) {
        free(to_real);
        to_real = NULL;
    }
    return ret;
_bindMount_unclean:
    if (to_real != NULL) {
        ret = umount(to_real);
        free(to_real);
        to_real = NULL;
    } else {
        ret = umount(to);
    }
    if (ret != 0) {
        fprintf(stderr, "ERROR: unclean exit from bind-mount routine. %s may still be mounted.\n", to);
    }
    return 1;
}

static char *_filterString(char *input) {
    size_t len = 0;
    char *ret = NULL;
    char *rptr = NULL;
    char *wptr = NULL;
    if (input == NULL) return NULL;

    len = strlen(input) + 1;
    ret = (char *) malloc(sizeof(char) * len);
    if (ret == NULL) return NULL;

    rptr = input;
    wptr = ret;
    while (wptr - ret < len && *rptr != 0) {
        if (isalnum(*rptr) || *rptr == '_' || *rptr == ':' || *rptr == '.' || *rptr == '+' || *rptr == '-') {
            *wptr++ = *rptr;
        }
        rptr++;
    }
    *wptr = 0;
    return ret;
}

char **parseMounts(size_t *n_mounts) {
    pid_t pid = getpid();
    char fname_buffer[PATH_MAX];
    FILE *fp = NULL;
    char *lineBuffer = NULL;
    size_t lineBuffer_size = 0;
    ssize_t nRead = 0;

    char **ret = NULL;
    char **ret_ptr = NULL;
    size_t ret_capacity = 0;

    snprintf(fname_buffer, PATH_MAX, "/proc/%d/mounts", pid);
    fp = fopen(fname_buffer, "r");
    if (fp == NULL) {
        fprintf(stderr, "FAILED to open %s\n", fname_buffer);
        *n_mounts = 0;
        return NULL;
    }
    while (!feof(fp) && !ferror(fp)) {
        char *search = NULL;
        char *ptr = NULL;
        nRead = getline(&lineBuffer, &lineBuffer_size, fp);
        if (nRead == 0 || feof(fp) || ferror(fp)) {
            break;
        }
        /* want second space-seperated column */
        ptr = strtok(lineBuffer, " ");
        ptr = strtok(NULL, " ");

        if (ptr != NULL) {
            size_t curr_count = ret_ptr - ret;
            if (ret == NULL || (curr_count + 2) >= ret_capacity) {
                char **tRet = realloc(ret, (ret_capacity + MOUNT_ALLOC_BLOCK) * sizeof(char*));
                if (tRet == NULL) {
                    fprintf(stderr, "FAILED to allocate enough memory for mounts listing\n");
                    goto _parseMounts_errClean;
                }
                ret = tRet;
                ret_capacity += MOUNT_ALLOC_BLOCK;
                ret_ptr = tRet + curr_count;
            }
            *ret_ptr = strdup(ptr);
            ret_ptr++;
            *ret_ptr = NULL;
        }
    }
    fclose(fp);
    if (lineBuffer != NULL) {
        free(lineBuffer);
    }
    *n_mounts = ret_ptr - ret;
    return ret;
_parseMounts_errClean:
    if (lineBuffer != NULL) {
        free(lineBuffer);
    }
    if (ret != NULL) {
        for (ret_ptr = ret; *ret_ptr; ret_ptr++) {
            free(*ret_ptr);
        }
        free(ret);
    }
    *n_mounts = 0;
    return NULL;
}

static int _sortFsForward(const void *, const void *) {
}
static int _sortFsReverse(const void *, const void *);