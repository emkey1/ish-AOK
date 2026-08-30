//
//  CurrentRoot.m
//  iSH
//
//  Created by Theodore Dubois on 11/4/21.
//

#import "CurrentRoot.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "debug.h"


int fs_ish_version;
int fs_ish_apk_version;

static ssize_t read_file(const char *path, char *buf, size_t size) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    ssize_t n = fd->ops->read(fd, buf, size);
    fd_close(fd);
    if (n == size)
        return _ENAMETOOLONG;
    return n;
}

static ssize_t write_file(const char *path, const char *buf, size_t size) {
    struct fd *fd = generic_open(path, O_WRONLY_|O_CREAT_|O_TRUNC_, 0644);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    ssize_t n = fd->ops->write(fd, buf, size);
    fd_close(fd);
    return n;
}
static int remove_directory(const char *path) {
    return generic_rmdirat(AT_PWD, path);
}

static bool file_exists(const char *path) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return false;
    fd_close(fd);
    return true;
}

static int copy_file_mode(const char *src, const char *dst, mode_t_ mode) {
    struct fd *src_fd = generic_open(src, O_RDONLY_, 0);
    if (IS_ERR(src_fd))
        return (int) PTR_ERR(src_fd);

    struct fd *dst_fd = generic_open(dst, O_WRONLY_ | O_CREAT_ | O_TRUNC_, mode);
    if (IS_ERR(dst_fd)) {
        int err = (int) PTR_ERR(dst_fd);
        fd_close(src_fd);
        return err;
    }

    int err = 0;
    char buf[8192];
    for (;;) {
        ssize_t n = src_fd->ops->read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            err = (int) n;
            break;
        }
        if (n == 0)
            break;

        size_t written = 0;
        while (written < (size_t) n) {
            ssize_t m = dst_fd->ops->write(dst_fd, buf + written, n - written);
            if (m <= 0) {
                err = m < 0 ? (int) m : _EIO;
                goto out;
            }
            written += m;
        }
    }

out:
    fd_close(dst_fd);
    fd_close(src_fd);
    if (err < 0)
        generic_unlinkat(AT_PWD, dst);
    return err;
}

static void maybe_restore_login_binary(void) {
    char method[64];
    ssize_t n = read_file("/etc/opt/AOK-login_method", method, sizeof(method));
    if (n < 0)
        return;
    if (n < 7 || strncmp(method, "enabled", 7) != 0)
        return;
    if (!file_exists("/bin/login.original"))
        return;

    if (!file_exists("/bin/login.aok-broken")) {
        int rename_err = generic_renameat(AT_PWD, "/bin/login", AT_PWD, "/bin/login.aok-broken", 0);
        if (rename_err < 0 && rename_err != _ENOENT) {
            printk("WARNING: failed to back up /bin/login: %d\n", rename_err);
            return;
        }
    }

    int copy_err = copy_file_mode("/bin/login.original", "/bin/login", 0755);
    if (copy_err < 0) {
        printk("WARNING: failed to restore /bin/login from /bin/login.original: %d\n", copy_err);
        return;
    }

}

static void maybe_normalize_unmanaged_apk_repositories(void) {
    char repositories[4096];
    ssize_t n = read_file("/etc/apk/repositories", repositories, sizeof(repositories) - 1);
    if (n <= 0)
        return;

    repositories[n] = '\0';
    NSString *existing = [[NSString alloc] initWithBytes:repositories length:n encoding:NSUTF8StringEncoding];
    if (existing == nil)
        existing = [[NSString alloc] initWithCString:repositories encoding:NSISOLatin1StringEncoding];
    if (existing == nil)
        return;

    static NSString *const kAlpineHttpsPrefix = @"https://dl-cdn.alpinelinux.org/alpine/";
    if ([existing rangeOfString:kAlpineHttpsPrefix].location == NSNotFound)
        return;

    NSString *updated = [existing stringByReplacingOccurrencesOfString:kAlpineHttpsPrefix
                                                            withString:@"http://dl-cdn.alpinelinux.org/alpine/"];
    if ([updated isEqualToString:existing])
        return;

    ssize_t wrote = write_file("/etc/apk/repositories",
                               updated.UTF8String,
                               [updated lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    if (wrote < 0) {
        printk("WARNING: failed to normalize /etc/apk/repositories for unmanaged Alpine root: %zd\n", wrote);
        return;
    }

}

void FsInitialize(void) {
    maybe_restore_login_binary();
    fs_ish_version = 0;
    fs_ish_apk_version = 0;

    // /ish/version is the last ish version that opened this root. Used to migrate the filesystem.
    char buf[1000];
    ssize_t n = read_file("/ish/version", buf, sizeof(buf));
    if (n >= 0) {
        NSString *currentVersion = NSBundle.mainBundle.infoDictionary[(__bridge NSString *) kCFBundleVersionKey];
        NSString *currentVersionFile = [NSString stringWithFormat:@"%@\n", currentVersion];

        NSString *version = [[NSString alloc] initWithBytesNoCopy:buf length:n encoding:NSUTF8StringEncoding freeWhenDone:NO];
        version = [version stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        fs_ish_version = version.intValue;

        version = nil;

        n = read_file("/ish/apk-version", buf, sizeof(buf));
        if (n >= 0) {
            NSString *version = [[NSString alloc] initWithBytesNoCopy:buf length:n encoding:NSUTF8StringEncoding freeWhenDone:NO];
            version = [version stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            fs_ish_apk_version = version.intValue;
        }

        if (fs_ish_apk_version >= COMPATIBLE_APK_VERSION)
            FsUpdateRepositories();

        if (currentVersion.intValue > fs_ish_version) {
            fs_ish_version = currentVersion.intValue;
            write_file("/ish/version", currentVersionFile.UTF8String, [currentVersionFile lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        }
    } else {
        maybe_normalize_unmanaged_apk_repositories();
    }
}

bool FsIsManaged(void) {
    return fs_ish_version != 0;
}

bool FsNeedsRepositoryUpdate(void) {
    return FsIsManaged() && fs_ish_apk_version < COMPATIBLE_APK_VERSION;
}

void FsUpdateOnlyRepositoriesFile(void) {
    NSURL *repositories = [NSBundle.mainBundle URLForResource:@"repositories" withExtension:@"txt"];
    if (repositories != nil) {
        NSMutableData *repositoriesData = [@"# This file contains pinned repositories managed by iSH. If the /ish directory\n"
                                           @"# exists, iSH uses the metadata stored in it to keep this file up to date (by\n"
                                           @"# overwriting the contents on boot.)\n" dataUsingEncoding:NSUTF8StringEncoding].mutableCopy;
        [repositoriesData appendData:[NSData dataWithContentsOfURL:repositories]];
        write_file("/etc/apk/repositories", repositoriesData.bytes, repositoriesData.length);
    }
}

void FsUpdateRepositories(void) {
    NSString *currentVersion = NSBundle.mainBundle.infoDictionary[(__bridge NSString *) kCFBundleVersionKey];
    NSString *currentVersionFile = [NSString stringWithFormat:@"%@\n", currentVersion];
    FsUpdateOnlyRepositoriesFile();
    fs_ish_apk_version = currentVersion.intValue;
    write_file("/ish/apk-version", currentVersionFile.UTF8String, [currentVersionFile lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    remove_directory("/ish/apk");
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:FsUpdatedNotification object:nil];
    });
}

NSString *const FsUpdatedNotification = @"FsUpdatedNotification";
