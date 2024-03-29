/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "dumpstate"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/inotify.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <numeric>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <android/content/pm/IPackageManagerNative.h>
#include <android/hardware/dumpstate/1.0/IDumpstateDevice.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <android/os/IIncidentCompanion.h>
#include <binder/IServiceManager.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <debuggerd/client.h>
#include <dumpsys.h>
#include <dumputils/dump_utils.h>
#include <hardware_legacy/power.h>
#include <hidl/ServiceManagement.h>
#include <log/log.h>
#include <openssl/sha.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>
#include <serviceutils/PriorityDumper.h>
#include <utils/StrongPointer.h>
#include "DumpstateInternal.h"
#include "DumpstateService.h"
#include "dumpstate.h"

using ::android::hardware::dumpstate::V1_0::IDumpstateDevice;
using ::std::literals::chrono_literals::operator""ms;
using ::std::literals::chrono_literals::operator""s;

// TODO: remove once moved to namespace
using android::defaultServiceManager;
using android::Dumpsys;
using android::INVALID_OPERATION;
using android::IServiceManager;
using android::OK;
using android::sp;
using android::status_t;
using android::String16;
using android::String8;
using android::TIMED_OUT;
using android::UNKNOWN_ERROR;
using android::Vector;
using android::base::StringPrintf;
using android::os::IDumpstateListener;
using android::os::dumpstate::CommandOptions;
using android::os::dumpstate::DumpFileToFd;
using android::os::dumpstate::PropertiesHelper;

// Keep in sync with
// frameworks/base/services/core/java/com/android/server/am/ActivityManagerService.java
static const int TRACE_DUMP_TIMEOUT_MS = 10000; // 10 seconds

/* Most simple commands have 10 as timeout, so 5 is a good estimate */
static const int32_t WEIGHT_FILE = 5;

// TODO: temporary variables and functions used during C++ refactoring
static Dumpstate& ds = Dumpstate::GetInstance();
static int RunCommand(const std::string& title, const std::vector<std::string>& full_command,
                      const CommandOptions& options = CommandOptions::DEFAULT,
                      bool verbose_duration = false) {
    return ds.RunCommand(title, full_command, options, verbose_duration);
}

// Reasonable value for max stats.
static const int STATS_MAX_N_RUNS = 1000;
static const long STATS_MAX_AVERAGE = 100000;

CommandOptions Dumpstate::DEFAULT_DUMPSYS = CommandOptions::WithTimeout(30).Build();

typedef Dumpstate::ConsentCallback::ConsentResult UserConsentResult;

/* read before root is shed */
static char cmdline_buf[16384] = "(unknown)";
static const char *dump_traces_path = nullptr;
static const uint64_t USER_CONSENT_TIMEOUT_MS = 30 * 1000;

// TODO: variables and functions below should be part of dumpstate object

static std::set<std::string> mount_points;
void add_mountinfo();

#define PSTORE_LAST_KMSG "/sys/fs/pstore/console-ramoops"
#define ALT_PSTORE_LAST_KMSG "/sys/fs/pstore/console-ramoops-0"
#define BLK_DEV_SYS_DIR "/sys/block"

#define RECOVERY_DIR "/cache/recovery"
#define RECOVERY_DATA_DIR "/data/misc/recovery"
#define UPDATE_ENGINE_LOG_DIR "/data/misc/update_engine_log"
#define LOGPERSIST_DATA_DIR "/data/misc/logd"
#define PROFILE_DATA_DIR_CUR "/data/misc/profiles/cur"
#define PROFILE_DATA_DIR_REF "/data/misc/profiles/ref"
#define XFRM_STAT_PROC_FILE "/proc/net/xfrm_stat"
#define WLUTIL "/vendor/xbin/wlutil"
#define WMTRACE_DATA_DIR "/data/misc/wmtrace"
#define OTA_METADATA_DIR "/metadata/ota"

// TODO(narayan): Since this information has to be kept in sync
// with tombstoned, we should just put it in a common header.
//
// File: system/core/debuggerd/tombstoned/tombstoned.cpp
static const std::string TOMBSTONE_DIR = "/data/tombstones/";
static const std::string TOMBSTONE_FILE_PREFIX = "tombstone_";
static const std::string ANR_DIR = "/data/anr/";
static const std::string ANR_FILE_PREFIX = "anr_";

// TODO: temporary variables and functions used during C++ refactoring

#define RETURN_IF_USER_DENIED_CONSENT()                                                        \
    if (ds.IsUserConsentDenied()) {                                                            \
        MYLOGE("Returning early as user denied consent to share bugreport with calling app."); \
        return Dumpstate::RunStatus::USER_CONSENT_DENIED;                                      \
    }

// Runs func_ptr, but checks user consent before and after running it. Returns USER_CONSENT_DENIED
// if consent is found to be denied.
#define RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(func_ptr, ...) \
    RETURN_IF_USER_DENIED_CONSENT();                        \
    func_ptr(__VA_ARGS__);                                  \
    RETURN_IF_USER_DENIED_CONSENT();

static const char* WAKE_LOCK_NAME = "dumpstate_wakelock";

namespace android {
namespace os {
namespace {

static int Open(std::string path, int flags, mode_t mode = 0) {
    int fd = TEMP_FAILURE_RETRY(open(path.c_str(), flags, mode));
    if (fd == -1) {
        MYLOGE("open(%s, %s)\n", path.c_str(), strerror(errno));
    }
    return fd;
}


static int OpenForRead(std::string path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

bool CopyFile(int in_fd, int out_fd) {
    char buf[4096];
    ssize_t byte_count;
    while ((byte_count = TEMP_FAILURE_RETRY(read(in_fd, buf, sizeof(buf)))) > 0) {
        if (!android::base::WriteFully(out_fd, buf, byte_count)) {
            return false;
        }
    }
    return (byte_count != -1);
}

static bool CopyFileToFd(const std::string& input_file, int out_fd) {
    MYLOGD("Going to copy file (%s) to %d\n", input_file.c_str(), out_fd);

    // Obtain a handle to the source file.
    android::base::unique_fd in_fd(OpenForRead(input_file));
    if (out_fd != -1 && in_fd.get() != -1) {
        if (CopyFile(in_fd.get(), out_fd)) {
            return true;
        }
        MYLOGE("Failed to copy file: %s\n", strerror(errno));
    }
    return false;
}

static bool UnlinkAndLogOnError(const std::string& file) {
    if (unlink(file.c_str())) {
        MYLOGE("Failed to unlink file (%s): %s\n", file.c_str(), strerror(errno));
        return false;
    }
    return true;
}

static bool IsFileEmpty(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if(file.bad()) {
        MYLOGE("Cannot open file: %s\n", file_path.c_str());
        return true;
    }
    return file.tellg() <= 0;
}

int64_t GetModuleMetadataVersion() {
    auto binder = defaultServiceManager()->getService(android::String16("package_native"));
    if (binder == nullptr) {
        MYLOGE("Failed to retrieve package_native service");
        return 0L;
    }
    auto package_service = android::interface_cast<content::pm::IPackageManagerNative>(binder);
    std::string package_name;
    auto status = package_service->getModuleMetadataPackageName(&package_name);
    if (!status.isOk()) {
        MYLOGE("Failed to retrieve module metadata package name: %s", status.toString8().c_str());
        return 0L;
    }
    MYLOGD("Module metadata package name: %s\n", package_name.c_str());
    int64_t version_code;
    status = package_service->getVersionCodeForPackage(android::String16(package_name.c_str()),
                                                       &version_code);
    if (!status.isOk()) {
        MYLOGE("Failed to retrieve module metadata version: %s", status.toString8().c_str());
        return 0L;
    }
    return version_code;
}

}  // namespace
}  // namespace os
}  // namespace android

static void RunDumpsys(const std::string& title, const std::vector<std::string>& dumpsysArgs,
                       const CommandOptions& options = Dumpstate::DEFAULT_DUMPSYS,
                       long dumpsysTimeoutMs = 0) {
    return ds.RunDumpsys(title, dumpsysArgs, options, dumpsysTimeoutMs);
}
static int DumpFile(const std::string& title, const std::string& path) {
    return ds.DumpFile(title, path);
}

// Relative directory (inside the zip) for all files copied as-is into the bugreport.
static const std::string ZIP_ROOT_DIR = "FS";

static const std::string kProtoPath = "proto/";
static const std::string kProtoExt = ".proto";
static const std::string kDumpstateBoardFiles[] = {
    "dumpstate_board.txt",
    "dumpstate_board.bin"
};
static const int NUM_OF_DUMPS = arraysize(kDumpstateBoardFiles);

static constexpr char PROPERTY_EXTRA_OPTIONS[] = "dumpstate.options";
static constexpr char PROPERTY_LAST_ID[] = "dumpstate.last_id";
static constexpr char PROPERTY_VERSION[] = "dumpstate.version";
static constexpr char PROPERTY_EXTRA_TITLE[] = "dumpstate.options.title";
static constexpr char PROPERTY_EXTRA_DESCRIPTION[] = "dumpstate.options.description";

static const CommandOptions AS_ROOT_20 = CommandOptions::WithTimeout(20).AsRoot().Build();

/*
 * Returns a vector of dump fds under |dir_path| with a given |file_prefix|.
 * The returned vector is sorted by the mtimes of the dumps. If |limit_by_mtime|
 * is set, the vector only contains files that were written in the last 30 minutes.
 */
static std::vector<DumpData> GetDumpFds(const std::string& dir_path,
                                        const std::string& file_prefix,
                                        bool limit_by_mtime) {
    const time_t thirty_minutes_ago = ds.now_ - 60 * 30;

    std::unique_ptr<DIR, decltype(&closedir)> dump_dir(opendir(dir_path.c_str()), closedir);

    if (dump_dir == nullptr) {
        MYLOGW("Unable to open directory %s: %s\n", dir_path.c_str(), strerror(errno));
        return std::vector<DumpData>();
    }

    std::vector<DumpData> dump_data;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dump_dir.get()))) {
        if (entry->d_type != DT_REG) {
            continue;
        }

        const std::string base_name(entry->d_name);
        if (base_name.find(file_prefix) != 0) {
            continue;
        }

        const std::string abs_path = dir_path + base_name;
        android::base::unique_fd fd(
            TEMP_FAILURE_RETRY(open(abs_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)));
        if (fd == -1) {
            MYLOGW("Unable to open dump file %s: %s\n", abs_path.c_str(), strerror(errno));
            break;
        }

        struct stat st = {};
        if (fstat(fd, &st) == -1) {
            MYLOGW("Unable to stat dump file %s: %s\n", abs_path.c_str(), strerror(errno));
            continue;
        }

        if (limit_by_mtime && st.st_mtime < thirty_minutes_ago) {
            MYLOGI("Excluding stale dump file: %s\n", abs_path.c_str());
            continue;
        }

        dump_data.emplace_back(DumpData{abs_path, std::move(fd), st.st_mtime});
    }

    return dump_data;
}

static bool AddDumps(const std::vector<DumpData>::const_iterator start,
                     const std::vector<DumpData>::const_iterator end,
                     const char* type_name, const bool add_to_zip) {
    bool dumped = false;
    for (auto it = start; it != end; ++it) {
        const std::string& name = it->name;
        const int fd = it->fd;
        dumped = true;

        // Seek to the beginning of the file before dumping any data. A given
        // DumpData entry might be dumped multiple times in the report.
        //
        // For example, the most recent ANR entry is dumped to the body of the
        // main entry and it also shows up as a separate entry in the bugreport
        // ZIP file.
        if (lseek(fd, 0, SEEK_SET) != static_cast<off_t>(0)) {
            MYLOGE("Unable to add %s to zip file, lseek failed: %s\n", name.c_str(),
                   strerror(errno));
        }

        if (ds.IsZipping() && add_to_zip) {
            if (ds.AddZipEntryFromFd(ZIP_ROOT_DIR + name, fd, /* timeout = */ 0ms) != OK) {
                MYLOGE("Unable to add %s to zip file, addZipEntryFromFd failed\n", name.c_str());
            }
        } else {
            dump_file_from_fd(type_name, name.c_str(), fd);
        }
    }

    return dumped;
}

// for_each_pid() callback to get mount info about a process.
void do_mountinfo(int pid, const char* name __attribute__((unused))) {
    char path[PATH_MAX];

    // Gets the the content of the /proc/PID/ns/mnt link, so only unique mount points
    // are added.
    snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);
    char linkname[PATH_MAX];
    ssize_t r = readlink(path, linkname, PATH_MAX);
    if (r == -1) {
        MYLOGE("Unable to read link for %s: %s\n", path, strerror(errno));
        return;
    }
    linkname[r] = '\0';

    if (mount_points.find(linkname) == mount_points.end()) {
        // First time this mount point was found: add it
        snprintf(path, sizeof(path), "/proc/%d/mountinfo", pid);
        if (ds.AddZipEntry(ZIP_ROOT_DIR + path, path)) {
            mount_points.insert(linkname);
        } else {
            MYLOGE("Unable to add mountinfo %s to zip file\n", path);
        }
    }
}

void add_mountinfo() {
    if (!ds.IsZipping()) return;
    std::string title = "MOUNT INFO";
    mount_points.clear();
    DurationReporter duration_reporter(title, true);
    for_each_pid(do_mountinfo, nullptr);
    MYLOGD("%s: %d entries added to zip file\n", title.c_str(), (int)mount_points.size());
}

static void dump_dev_files(const char *title, const char *driverpath, const char *filename)
{
    DIR *d;
    struct dirent *de;
    char path[PATH_MAX];

    d = opendir(driverpath);
    if (d == nullptr) {
        return;
    }

    while ((de = readdir(d))) {
        if (de->d_type != DT_LNK) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/%s", driverpath, de->d_name, filename);
        DumpFile(title, path);
    }

    closedir(d);
}

static bool skip_not_stat(const char *path) {
    static const char stat[] = "/stat";
    size_t len = strlen(path);
    if (path[len - 1] == '/') { /* Directory? */
        return false;
    }
    return strcmp(path + len - sizeof(stat) + 1, stat); /* .../stat? */
}

static bool skip_none(const char* path __attribute__((unused))) {
    return false;
}

unsigned long worst_write_perf = 20000; /* in KB/s */

//
//  stat offsets
// Name            units         description
// ----            -----         -----------
// read I/Os       requests      number of read I/Os processed
#define __STAT_READ_IOS      0
// read merges     requests      number of read I/Os merged with in-queue I/O
#define __STAT_READ_MERGES   1
// read sectors    sectors       number of sectors read
#define __STAT_READ_SECTORS  2
// read ticks      milliseconds  total wait time for read requests
#define __STAT_READ_TICKS    3
// write I/Os      requests      number of write I/Os processed
#define __STAT_WRITE_IOS     4
// write merges    requests      number of write I/Os merged with in-queue I/O
#define __STAT_WRITE_MERGES  5
// write sectors   sectors       number of sectors written
#define __STAT_WRITE_SECTORS 6
// write ticks     milliseconds  total wait time for write requests
#define __STAT_WRITE_TICKS   7
// in_flight       requests      number of I/Os currently in flight
#define __STAT_IN_FLIGHT     8
// io_ticks        milliseconds  total time this block device has been active
#define __STAT_IO_TICKS      9
// time_in_queue   milliseconds  total wait time for all requests
#define __STAT_IN_QUEUE     10
#define __STAT_NUMBER_FIELD 11
//
// read I/Os, write I/Os
// =====================
//
// These values increment when an I/O request completes.
//
// read merges, write merges
// =========================
//
// These values increment when an I/O request is merged with an
// already-queued I/O request.
//
// read sectors, write sectors
// ===========================
//
// These values count the number of sectors read from or written to this
// block device.  The "sectors" in question are the standard UNIX 512-byte
// sectors, not any device- or filesystem-specific block size.  The
// counters are incremented when the I/O completes.
#define SECTOR_SIZE 512
//
// read ticks, write ticks
// =======================
//
// These values count the number of milliseconds that I/O requests have
// waited on this block device.  If there are multiple I/O requests waiting,
// these values will increase at a rate greater than 1000/second; for
// example, if 60 read requests wait for an average of 30 ms, the read_ticks
// field will increase by 60*30 = 1800.
//
// in_flight
// =========
//
// This value counts the number of I/O requests that have been issued to
// the device driver but have not yet completed.  It does not include I/O
// requests that are in the queue but not yet issued to the device driver.
//
// io_ticks
// ========
//
// This value counts the number of milliseconds during which the device has
// had I/O requests queued.
//
// time_in_queue
// =============
//
// This value counts the number of milliseconds that I/O requests have waited
// on this block device.  If there are multiple I/O requests waiting, this
// value will increase as the product of the number of milliseconds times the
// number of requests waiting (see "read ticks" above for an example).
#define S_TO_MS 1000
//

static int dump_stat_from_fd(const char *title __unused, const char *path, int fd) {
    unsigned long long fields[__STAT_NUMBER_FIELD];
    bool z;
    char *cp, *buffer = nullptr;
    size_t i = 0;
    FILE *fp = fdopen(dup(fd), "rb");
    getline(&buffer, &i, fp);
    fclose(fp);
    if (!buffer) {
        return -errno;
    }
    i = strlen(buffer);
    while ((i > 0) && (buffer[i - 1] == '\n')) {
        buffer[--i] = '\0';
    }
    if (!*buffer) {
        free(buffer);
        return 0;
    }
    z = true;
    for (cp = buffer, i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
        fields[i] = strtoull(cp, &cp, 10);
        if (fields[i] != 0) {
            z = false;
        }
    }
    if (z) { /* never accessed */
        free(buffer);
        return 0;
    }

    if (!strncmp(path, BLK_DEV_SYS_DIR, sizeof(BLK_DEV_SYS_DIR) - 1)) {
        path += sizeof(BLK_DEV_SYS_DIR) - 1;
    }

    printf("%-30s:%9s%9s%9s%9s%9s%9s%9s%9s%9s%9s%9s\n%-30s:\t%s\n", "Block-Dev",
           "R-IOs", "R-merg", "R-sect", "R-wait", "W-IOs", "W-merg", "W-sect",
           "W-wait", "in-fli", "activ", "T-wait", path, buffer);
    free(buffer);

    if (fields[__STAT_IO_TICKS]) {
        unsigned long read_perf = 0;
        unsigned long read_ios = 0;
        if (fields[__STAT_READ_TICKS]) {
            unsigned long long divisor = fields[__STAT_READ_TICKS]
                                       * fields[__STAT_IO_TICKS];
            read_perf = ((unsigned long long)SECTOR_SIZE
                           * fields[__STAT_READ_SECTORS]
                           * fields[__STAT_IN_QUEUE] + (divisor >> 1))
                                        / divisor;
            read_ios = ((unsigned long long)S_TO_MS * fields[__STAT_READ_IOS]
                           * fields[__STAT_IN_QUEUE] + (divisor >> 1))
                                        / divisor;
        }

        unsigned long write_perf = 0;
        unsigned long write_ios = 0;
        if (fields[__STAT_WRITE_TICKS]) {
            unsigned long long divisor = fields[__STAT_WRITE_TICKS]
                                       * fields[__STAT_IO_TICKS];
            write_perf = ((unsigned long long)SECTOR_SIZE
                           * fields[__STAT_WRITE_SECTORS]
                           * fields[__STAT_IN_QUEUE] + (divisor >> 1))
                                        / divisor;
            write_ios = ((unsigned long long)S_TO_MS * fields[__STAT_WRITE_IOS]
                           * fields[__STAT_IN_QUEUE] + (divisor >> 1))
                                        / divisor;
        }

        unsigned queue = (fields[__STAT_IN_QUEUE]
                             + (fields[__STAT_IO_TICKS] >> 1))
                                 / fields[__STAT_IO_TICKS];

        if (!write_perf && !write_ios) {
            printf("%-30s: perf(ios) rd: %luKB/s(%lu/s) q: %u\n", path, read_perf, read_ios, queue);
        } else {
            printf("%-30s: perf(ios) rd: %luKB/s(%lu/s) wr: %luKB/s(%lu/s) q: %u\n", path, read_perf,
                   read_ios, write_perf, write_ios, queue);
        }

        /* bugreport timeout factor adjustment */
        if ((write_perf > 1) && (write_perf < worst_write_perf)) {
            worst_write_perf = write_perf;
        }
    }
    return 0;
}

static const long MINIMUM_LOGCAT_TIMEOUT_MS = 50000;

/* timeout in ms to read a list of buffers */
static unsigned long logcat_timeout(const std::vector<std::string>& buffers) {
    unsigned long timeout_ms = 0;
    for (const auto& buffer : buffers) {
        log_id_t id = android_name_to_log_id(buffer.c_str());
        unsigned long property_size = __android_logger_get_buffer_size(id);
        /* Engineering margin is ten-fold our guess */
        timeout_ms += 10 * (property_size + worst_write_perf) / worst_write_perf;
    }
    return timeout_ms > MINIMUM_LOGCAT_TIMEOUT_MS ? timeout_ms : MINIMUM_LOGCAT_TIMEOUT_MS;
}

Dumpstate::ConsentCallback::ConsentCallback() : result_(UNAVAILABLE), start_time_(Nanotime()) {
}

android::binder::Status Dumpstate::ConsentCallback::onReportApproved() {
    std::lock_guard<std::mutex> lock(lock_);
    result_ = APPROVED;
    MYLOGD("User approved consent to share bugreport\n");
    return android::binder::Status::ok();
}

android::binder::Status Dumpstate::ConsentCallback::onReportDenied() {
    std::lock_guard<std::mutex> lock(lock_);
    result_ = DENIED;
    MYLOGW("User denied consent to share bugreport\n");
    return android::binder::Status::ok();
}

UserConsentResult Dumpstate::ConsentCallback::getResult() {
    std::lock_guard<std::mutex> lock(lock_);
    return result_;
}

uint64_t Dumpstate::ConsentCallback::getElapsedTimeMs() const {
    return Nanotime() - start_time_;
}

void Dumpstate::PrintHeader() const {
    std::string build, fingerprint, radio, bootloader, network;
    char date[80];

    build = android::base::GetProperty("ro.build.display.id", "(unknown)");
    fingerprint = android::base::GetProperty("ro.build.fingerprint", "(unknown)");
    radio = android::base::GetProperty("gsm.version.baseband", "(unknown)");
    bootloader = android::base::GetProperty("ro.bootloader", "(unknown)");
    network = android::base::GetProperty("gsm.operator.alpha", "(unknown)");
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now_));

    printf("========================================================\n");
    printf("== dumpstate: %s\n", date);
    printf("========================================================\n");

    printf("\n");
    printf("Build: %s\n", build.c_str());
    // NOTE: fingerprint entry format is important for other tools.
    printf("Build fingerprint: '%s'\n", fingerprint.c_str());
    printf("Bootloader: %s\n", bootloader.c_str());
    printf("Radio: %s\n", radio.c_str());
    printf("Network: %s\n", network.c_str());
    int64_t module_metadata_version = android::os::GetModuleMetadataVersion();
    if (module_metadata_version != 0) {
        printf("Module Metadata version: %" PRId64 "\n", module_metadata_version);
    }

    printf("Kernel: ");
    DumpFileToFd(STDOUT_FILENO, "", "/proc/version");
    printf("Command line: %s\n", strtok(cmdline_buf, "\n"));
    printf("Uptime: ");
    RunCommandToFd(STDOUT_FILENO, "", {"uptime", "-p"},
                   CommandOptions::WithTimeout(1).Always().Build());
    printf("Bugreport format version: %s\n", version_.c_str());
    printf("Dumpstate info: id=%d pid=%d dry_run=%d args=%s extra_options=%s\n", id_, pid_,
           PropertiesHelper::IsDryRun(), options_->args.c_str(), options_->extra_options.c_str());
    printf("\n");
}

// List of file extensions that can cause a zip file attachment to be rejected by some email
// service providers.
static const std::set<std::string> PROBLEMATIC_FILE_EXTENSIONS = {
      ".ade", ".adp", ".bat", ".chm", ".cmd", ".com", ".cpl", ".exe", ".hta", ".ins", ".isp",
      ".jar", ".jse", ".lib", ".lnk", ".mde", ".msc", ".msp", ".mst", ".pif", ".scr", ".sct",
      ".shb", ".sys", ".vb",  ".vbe", ".vbs", ".vxd", ".wsc", ".wsf", ".wsh"
};

status_t Dumpstate::AddZipEntryFromFd(const std::string& entry_name, int fd,
                                      std::chrono::milliseconds timeout = 0ms) {
    if (!IsZipping()) {
        MYLOGD("Not adding zip entry %s from fd because it's not a zipped bugreport\n",
               entry_name.c_str());
        return INVALID_OPERATION;
    }
    std::string valid_name = entry_name;

    // Rename extension if necessary.
    size_t idx = entry_name.rfind('.');
    if (idx != std::string::npos) {
        std::string extension = entry_name.substr(idx);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (PROBLEMATIC_FILE_EXTENSIONS.count(extension) != 0) {
            valid_name = entry_name + ".renamed";
            MYLOGI("Renaming entry %s to %s\n", entry_name.c_str(), valid_name.c_str());
        }
    }

    // Logging statement  below is useful to time how long each entry takes, but it's too verbose.
    // MYLOGD("Adding zip entry %s\n", entry_name.c_str());
    int32_t err = zip_writer_->StartEntryWithTime(valid_name.c_str(), ZipWriter::kCompress,
                                                  get_mtime(fd, ds.now_));
    if (err != 0) {
        MYLOGE("zip_writer_->StartEntryWithTime(%s): %s\n", valid_name.c_str(),
               ZipWriter::ErrorCodeString(err));
        return UNKNOWN_ERROR;
    }
    bool finished_entry = false;
    auto finish_entry = [this, &finished_entry] {
        if (!finished_entry) {
            // This should only be called when we're going to return an earlier error,
            // which would've been logged. This may imply the file is already corrupt
            // and any further logging from FinishEntry is more likely to mislead than
            // not.
            this->zip_writer_->FinishEntry();
        }
    };
    auto scope_guard = android::base::make_scope_guard(finish_entry);
    auto start = std::chrono::steady_clock::now();
    auto end = start + timeout;
    struct pollfd pfd = {fd, POLLIN};

    std::vector<uint8_t> buffer(65536);
    while (1) {
        if (timeout.count() > 0) {
            // lambda to recalculate the timeout.
            auto time_left_ms = [end]() {
                auto now = std::chrono::steady_clock::now();
                auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
                return std::max(diff.count(), 0LL);
            };

            int rc = TEMP_FAILURE_RETRY(poll(&pfd, 1, time_left_ms()));
            if (rc < 0) {
                MYLOGE("Error in poll while adding from fd to zip entry %s:%s\n",
                       entry_name.c_str(), strerror(errno));
                return -errno;
            } else if (rc == 0) {
                MYLOGE("Timed out adding from fd to zip entry %s:%s Timeout:%lldms\n",
                       entry_name.c_str(), strerror(errno), timeout.count());
                return TIMED_OUT;
            }
        }

        ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buffer.data(), buffer.size()));
        if (bytes_read == 0) {
            break;
        } else if (bytes_read == -1) {
            MYLOGE("read(%s): %s\n", entry_name.c_str(), strerror(errno));
            return -errno;
        }
        err = zip_writer_->WriteBytes(buffer.data(), bytes_read);
        if (err) {
            MYLOGE("zip_writer_->WriteBytes(): %s\n", ZipWriter::ErrorCodeString(err));
            return UNKNOWN_ERROR;
        }
    }

    err = zip_writer_->FinishEntry();
    finished_entry = true;
    if (err != 0) {
        MYLOGE("zip_writer_->FinishEntry(): %s\n", ZipWriter::ErrorCodeString(err));
        return UNKNOWN_ERROR;
    }

    return OK;
}

bool Dumpstate::AddZipEntry(const std::string& entry_name, const std::string& entry_path) {
    android::base::unique_fd fd(
        TEMP_FAILURE_RETRY(open(entry_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)));
    if (fd == -1) {
        MYLOGE("open(%s): %s\n", entry_path.c_str(), strerror(errno));
        return false;
    }

    return (AddZipEntryFromFd(entry_name, fd.get()) == OK);
}

/* adds a file to the existing zipped bugreport */
static int _add_file_from_fd(const char* title __attribute__((unused)), const char* path, int fd) {
    return (ds.AddZipEntryFromFd(ZIP_ROOT_DIR + path, fd) == OK) ? 0 : 1;
}

void Dumpstate::AddDir(const std::string& dir, bool recursive) {
    if (!IsZipping()) {
        MYLOGD("Not adding dir %s because it's not a zipped bugreport\n", dir.c_str());
        return;
    }
    MYLOGD("Adding dir %s (recursive: %d)\n", dir.c_str(), recursive);
    DurationReporter duration_reporter(dir, true);
    dump_files("", dir.c_str(), recursive ? skip_none : is_dir, _add_file_from_fd);
}

bool Dumpstate::AddTextZipEntry(const std::string& entry_name, const std::string& content) {
    if (!IsZipping()) {
        MYLOGD("Not adding text zip entry %s because it's not a zipped bugreport\n",
               entry_name.c_str());
        return false;
    }
    MYLOGD("Adding zip text entry %s\n", entry_name.c_str());
    int32_t err = zip_writer_->StartEntryWithTime(entry_name.c_str(), ZipWriter::kCompress, ds.now_);
    if (err != 0) {
        MYLOGE("zip_writer_->StartEntryWithTime(%s): %s\n", entry_name.c_str(),
               ZipWriter::ErrorCodeString(err));
        return false;
    }

    err = zip_writer_->WriteBytes(content.c_str(), content.length());
    if (err != 0) {
        MYLOGE("zip_writer_->WriteBytes(%s): %s\n", entry_name.c_str(),
               ZipWriter::ErrorCodeString(err));
        return false;
    }

    err = zip_writer_->FinishEntry();
    if (err != 0) {
        MYLOGE("zip_writer_->FinishEntry(): %s\n", ZipWriter::ErrorCodeString(err));
        return false;
    }

    return true;
}

static void DoKmsg() {
    struct stat st;
    if (!stat(PSTORE_LAST_KMSG, &st)) {
        /* Also TODO: Make console-ramoops CAP_SYSLOG protected. */
        DumpFile("LAST KMSG", PSTORE_LAST_KMSG);
    } else if (!stat(ALT_PSTORE_LAST_KMSG, &st)) {
        DumpFile("LAST KMSG", ALT_PSTORE_LAST_KMSG);
    } else {
        /* TODO: Make last_kmsg CAP_SYSLOG protected. b/5555691 */
        DumpFile("LAST KMSG", "/proc/last_kmsg");
    }
}

static void DoKernelLogcat() {
    unsigned long timeout_ms = logcat_timeout({"kernel"});
    RunCommand(
        "KERNEL LOG",
        {"logcat", "-b", "kernel", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v"},
        CommandOptions::WithTimeoutInMs(timeout_ms).Build());
}

static void DoSystemLogcat(time_t since) {
    char since_str[80];
    strftime(since_str, sizeof(since_str), "%Y-%m-%d %H:%M:%S.000", localtime(&since));

    unsigned long timeout_ms = logcat_timeout({"main", "system", "crash"});
    RunCommand("SYSTEM LOG",
               {"logcat", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v", "-T",
                since_str},
               CommandOptions::WithTimeoutInMs(timeout_ms).Build());
}

static void DoLogcat() {
    unsigned long timeout_ms;
    // DumpFile("EVENT LOG TAGS", "/etc/event-log-tags");
    // calculate timeout
    timeout_ms = logcat_timeout({"main", "system", "crash"});
    RunCommand("SYSTEM LOG",
               {"logcat", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v"},
               CommandOptions::WithTimeoutInMs(timeout_ms).Build());
    timeout_ms = logcat_timeout({"events"});
    RunCommand(
        "EVENT LOG",
        {"logcat", "-b", "events", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v"},
        CommandOptions::WithTimeoutInMs(timeout_ms).Build(), true /* verbose_duration */);
    timeout_ms = logcat_timeout({"stats"});
    RunCommand(
        "STATS LOG",
        {"logcat", "-b", "stats", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v"},
        CommandOptions::WithTimeoutInMs(timeout_ms).Build(), true /* verbose_duration */);
    timeout_ms = logcat_timeout({"radio"});
    RunCommand(
        "RADIO LOG",
        {"logcat", "-b", "radio", "-v", "threadtime", "-v", "printable", "-v", "uid", "-d", "*:v"},
        CommandOptions::WithTimeoutInMs(timeout_ms).Build(), true /* verbose_duration */);

    RunCommand("LOG STATISTICS", {"logcat", "-b", "all", "-S"});

    /* kernels must set CONFIG_PSTORE_PMSG, slice up pstore with device tree */
    RunCommand("LAST LOGCAT", {"logcat", "-L", "-b", "all", "-v", "threadtime", "-v", "printable",
                               "-v", "uid", "-d", "*:v"});
}

static void DumpIncidentReport() {
    if (!ds.IsZipping()) {
        MYLOGD("Not dumping incident report because it's not a zipped bugreport\n");
        return;
    }
    DurationReporter duration_reporter("INCIDENT REPORT");
    const std::string path = ds.bugreport_internal_dir_ + "/tmp_incident_report";
    auto fd = android::base::unique_fd(TEMP_FAILURE_RETRY(open(path.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)));
    if (fd < 0) {
        MYLOGE("Could not open %s to dump incident report.\n", path.c_str());
        return;
    }
    RunCommandToFd(fd, "", {"incident", "-u"}, CommandOptions::WithTimeout(120).Build());
    bool empty = 0 == lseek(fd, 0, SEEK_END);
    if (!empty) {
        // Use a different name from "incident.proto"
        // /proto/incident.proto is reserved for incident service dump
        // i.e. metadata for debugging.
        ds.AddZipEntry(kProtoPath + "incident_report" + kProtoExt, path);
    }
    unlink(path.c_str());
}

static void DumpIpTablesAsRoot() {
    RunCommand("IPTABLES", {"iptables", "-L", "-nvx"});
    RunCommand("IP6TABLES", {"ip6tables", "-L", "-nvx"});
    RunCommand("IPTABLES NAT", {"iptables", "-t", "nat", "-L", "-nvx"});
    /* no ip6 nat */
    RunCommand("IPTABLES MANGLE", {"iptables", "-t", "mangle", "-L", "-nvx"});
    RunCommand("IP6TABLES MANGLE", {"ip6tables", "-t", "mangle", "-L", "-nvx"});
    RunCommand("IPTABLES RAW", {"iptables", "-t", "raw", "-L", "-nvx"});
    RunCommand("IP6TABLES RAW", {"ip6tables", "-t", "raw", "-L", "-nvx"});
}

static void DumpDynamicPartitionInfo() {
    if (!::android::base::GetBoolProperty("ro.boot.dynamic_partitions", false)) {
        return;
    }

    RunCommand("LPDUMP", {"lpdump", "--all"});
    RunCommand("DEVICE-MAPPER", {"gsid", "dump-device-mapper"});
}

static void AddAnrTraceDir(const bool add_to_zip, const std::string& anr_traces_dir) {
    MYLOGD("AddAnrTraceDir(): dump_traces_file=%s, anr_traces_dir=%s\n", dump_traces_path,
           anr_traces_dir.c_str());

    // If we're here, dump_traces_path will always be a temporary file
    // (created with mkostemp or similar) that contains dumps taken earlier
    // on in the process.
    if (dump_traces_path != nullptr) {
        if (add_to_zip) {
            ds.AddZipEntry(ZIP_ROOT_DIR + anr_traces_dir + "/traces-just-now.txt", dump_traces_path);
        } else {
            MYLOGD("Dumping current ANR traces (%s) to the main bugreport entry\n",
                   dump_traces_path);
            ds.DumpFile("VM TRACES JUST NOW", dump_traces_path);
        }

        const int ret = unlink(dump_traces_path);
        if (ret == -1) {
            MYLOGW("Error unlinking temporary trace path %s: %s\n", dump_traces_path,
                   strerror(errno));
        }
    }

    // Add a specific message for the first ANR Dump.
    if (ds.anr_data_.size() > 0) {
        AddDumps(ds.anr_data_.begin(), ds.anr_data_.begin() + 1,
                 "VM TRACES AT LAST ANR", add_to_zip);

        // The "last" ANR will always be included as separate entry in the zip file. In addition,
        // it will be present in the body of the main entry if |add_to_zip| == false.
        //
        // Historical ANRs are always included as separate entries in the bugreport zip file.
        AddDumps(ds.anr_data_.begin() + ((add_to_zip) ? 1 : 0), ds.anr_data_.end(),
                 "HISTORICAL ANR", true /* add_to_zip */);
    } else {
        printf("*** NO ANRs to dump in %s\n\n", ANR_DIR.c_str());
    }
}

static void AddAnrTraceFiles() {
    const bool add_to_zip = ds.IsZipping() && ds.version_ == VERSION_SPLIT_ANR;

    std::string anr_traces_dir = "/data/anr";

    AddAnrTraceDir(add_to_zip, anr_traces_dir);

    RunCommand("ANR FILES", {"ls", "-lt", ANR_DIR});

    // Slow traces for slow operations.
    struct stat st;
    int i = 0;
    while (true) {
        const std::string slow_trace_path =
            anr_traces_dir + android::base::StringPrintf("slow%02d.txt", i);
        if (stat(slow_trace_path.c_str(), &st)) {
            // No traces file at this index, done with the files.
            break;
        }
        ds.DumpFile("VM TRACES WHEN SLOW", slow_trace_path.c_str());
        i++;
    }
}

static void DumpBlockStatFiles() {
    DurationReporter duration_reporter("DUMP BLOCK STAT");

    std::unique_ptr<DIR, std::function<int(DIR*)>> dirptr(opendir(BLK_DEV_SYS_DIR), closedir);

    if (dirptr == nullptr) {
        MYLOGE("Failed to open %s: %s\n", BLK_DEV_SYS_DIR, strerror(errno));
        return;
    }

    printf("------ DUMP BLOCK STAT ------\n\n");
    while (struct dirent *d = readdir(dirptr.get())) {
        if ((d->d_name[0] == '.')
         && (((d->d_name[1] == '.') && (d->d_name[2] == '\0'))
          || (d->d_name[1] == '\0'))) {
            continue;
        }
        const std::string new_path =
            android::base::StringPrintf("%s/%s", BLK_DEV_SYS_DIR, d->d_name);
        printf("------ BLOCK STAT (%s) ------\n", new_path.c_str());
        dump_files("", new_path.c_str(), skip_not_stat, dump_stat_from_fd);
        printf("\n");
    }
     return;
}

static void DumpPacketStats() {
    DumpFile("NETWORK DEV INFO", "/proc/net/dev");
    DumpFile("QTAGUID NETWORK INTERFACES INFO", "/proc/net/xt_qtaguid/iface_stat_all");
    DumpFile("QTAGUID NETWORK INTERFACES INFO (xt)", "/proc/net/xt_qtaguid/iface_stat_fmt");
    DumpFile("QTAGUID CTRL INFO", "/proc/net/xt_qtaguid/ctrl");
    DumpFile("QTAGUID STATS INFO", "/proc/net/xt_qtaguid/stats");
}

static void DumpIpAddrAndRules() {
    /* The following have a tendency to get wedged when wifi drivers/fw goes belly-up. */
    RunCommand("NETWORK INTERFACES", {"ip", "link"});
    RunCommand("IPv4 ADDRESSES", {"ip", "-4", "addr", "show"});
    RunCommand("IPv6 ADDRESSES", {"ip", "-6", "addr", "show"});
    RunCommand("IP RULES", {"ip", "rule", "show"});
    RunCommand("IP RULES v6", {"ip", "-6", "rule", "show"});
}

static Dumpstate::RunStatus RunDumpsysTextByPriority(const std::string& title, int priority,
                                                     std::chrono::milliseconds timeout,
                                                     std::chrono::milliseconds service_timeout) {
    auto start = std::chrono::steady_clock::now();
    sp<android::IServiceManager> sm = defaultServiceManager();
    Dumpsys dumpsys(sm.get());
    Vector<String16> args;
    Dumpsys::setServiceArgs(args, /* asProto = */ false, priority);
    Vector<String16> services = dumpsys.listServices(priority, /* supports_proto = */ false);
    for (const String16& service : services) {
        RETURN_IF_USER_DENIED_CONSENT();
        std::string path(title);
        path.append(" - ").append(String8(service).c_str());
        size_t bytes_written = 0;
        status_t status = dumpsys.startDumpThread(Dumpsys::Type::DUMP, service, args);
        if (status == OK) {
            dumpsys.writeDumpHeader(STDOUT_FILENO, service, priority);
            std::chrono::duration<double> elapsed_seconds;
            status = dumpsys.writeDump(STDOUT_FILENO, service, service_timeout,
                                       /* as_proto = */ false, elapsed_seconds, bytes_written);
            dumpsys.writeDumpFooter(STDOUT_FILENO, service, elapsed_seconds);
            bool dump_complete = (status == OK);
            dumpsys.stopDumpThread(dump_complete);
        }

        auto elapsed_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed_duration > timeout) {
            MYLOGE("*** command '%s' timed out after %llums\n", title.c_str(),
                   elapsed_duration.count());
            break;
        }
    }
    return Dumpstate::RunStatus::OK;
}

static void RunDumpsysText(const std::string& title, int priority,
                           std::chrono::milliseconds timeout,
                           std::chrono::milliseconds service_timeout) {
    DurationReporter duration_reporter(title);
    dprintf(STDOUT_FILENO, "------ %s (/system/bin/dumpsys) ------\n", title.c_str());
    fsync(STDOUT_FILENO);
    RunDumpsysTextByPriority(title, priority, timeout, service_timeout);
}

/* Dump all services registered with Normal or Default priority. */
static Dumpstate::RunStatus RunDumpsysTextNormalPriority(const std::string& title,
                                                         std::chrono::milliseconds timeout,
                                                         std::chrono::milliseconds service_timeout) {
    DurationReporter duration_reporter(title);
    dprintf(STDOUT_FILENO, "------ %s (/system/bin/dumpsys) ------\n", title.c_str());
    fsync(STDOUT_FILENO);
    RunDumpsysTextByPriority(title, IServiceManager::DUMP_FLAG_PRIORITY_NORMAL, timeout,
                             service_timeout);

    RETURN_IF_USER_DENIED_CONSENT();

    return RunDumpsysTextByPriority(title, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT, timeout,
                                    service_timeout);
}

static Dumpstate::RunStatus RunDumpsysProto(const std::string& title, int priority,
                                            std::chrono::milliseconds timeout,
                                            std::chrono::milliseconds service_timeout) {
    if (!ds.IsZipping()) {
        MYLOGD("Not dumping %s because it's not a zipped bugreport\n", title.c_str());
        return Dumpstate::RunStatus::OK;
    }
    sp<android::IServiceManager> sm = defaultServiceManager();
    Dumpsys dumpsys(sm.get());
    Vector<String16> args;
    Dumpsys::setServiceArgs(args, /* asProto = */ true, priority);
    DurationReporter duration_reporter(title);

    auto start = std::chrono::steady_clock::now();
    Vector<String16> services = dumpsys.listServices(priority, /* supports_proto = */ true);
    for (const String16& service : services) {
        RETURN_IF_USER_DENIED_CONSENT();
        std::string path(kProtoPath);
        path.append(String8(service).c_str());
        if (priority == IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL) {
            path.append("_CRITICAL");
        } else if (priority == IServiceManager::DUMP_FLAG_PRIORITY_HIGH) {
            path.append("_HIGH");
        }
        path.append(kProtoExt);
        status_t status = dumpsys.startDumpThread(Dumpsys::Type::DUMP, service, args);
        if (status == OK) {
            status = ds.AddZipEntryFromFd(path, dumpsys.getDumpFd(), service_timeout);
            bool dumpTerminated = (status == OK);
            dumpsys.stopDumpThread(dumpTerminated);
        }
        ZipWriter::FileEntry file_entry;
        ds.zip_writer_->GetLastEntry(&file_entry);

        auto elapsed_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed_duration > timeout) {
            MYLOGE("*** command '%s' timed out after %llums\n", title.c_str(),
                   elapsed_duration.count());
            break;
        }
    }
    return Dumpstate::RunStatus::OK;
}

// Runs dumpsys on services that must dump first and will take less than 100ms to dump.
static Dumpstate::RunStatus RunDumpsysCritical() {
    RunDumpsysText("DUMPSYS CRITICAL", IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL,
                   /* timeout= */ 5s, /* service_timeout= */ 500ms);

    RETURN_IF_USER_DENIED_CONSENT();

    return RunDumpsysProto("DUMPSYS CRITICAL PROTO", IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL,
                           /* timeout= */ 5s, /* service_timeout= */ 500ms);
}

// Runs dumpsys on services that must dump first but can take up to 250ms to dump.
static Dumpstate::RunStatus RunDumpsysHigh() {
    // TODO meminfo takes ~10s, connectivity takes ~5sec to dump. They are both
    // high priority. Reduce timeout once they are able to dump in a shorter time or
    // moved to a parallel task.
    RunDumpsysText("DUMPSYS HIGH", IServiceManager::DUMP_FLAG_PRIORITY_HIGH,
                   /* timeout= */ 90s, /* service_timeout= */ 30s);

    RETURN_IF_USER_DENIED_CONSENT();

    return RunDumpsysProto("DUMPSYS HIGH PROTO", IServiceManager::DUMP_FLAG_PRIORITY_HIGH,
                           /* timeout= */ 5s, /* service_timeout= */ 1s);
}

// Runs dumpsys on services that must dump but can take up to 10s to dump.
static Dumpstate::RunStatus RunDumpsysNormal() {
    RunDumpsysTextNormalPriority("DUMPSYS", /* timeout= */ 90s, /* service_timeout= */ 10s);

    RETURN_IF_USER_DENIED_CONSENT();

    return RunDumpsysProto("DUMPSYS PROTO", IServiceManager::DUMP_FLAG_PRIORITY_NORMAL,
                           /* timeout= */ 90s, /* service_timeout= */ 10s);
}

static void DumpHals() {
    if (!ds.IsZipping()) {
        RunCommand("HARDWARE HALS", {"lshal", "-lVSietrpc", "--types=b,c,l,z", "--debug"},
                   CommandOptions::WithTimeout(10).AsRootIfAvailable().Build());
        return;
    }
    DurationReporter duration_reporter("DUMP HALS");
    RunCommand("HARDWARE HALS", {"lshal", "-lVSietrpc", "--types=b,c,l,z"},
               CommandOptions::WithTimeout(10).AsRootIfAvailable().Build());

    using android::hidl::manager::V1_0::IServiceManager;
    using android::hardware::defaultServiceManager;

    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == nullptr) {
        MYLOGE("Could not retrieve hwservicemanager to dump hals.\n");
        return;
    }

    auto ret = sm->list([&](const auto& interfaces) {
        for (const std::string& interface : interfaces) {
            std::string cleanName = interface;
            std::replace_if(cleanName.begin(),
                            cleanName.end(),
                            [](char c) {
                                return !isalnum(c) &&
                                    std::string("@-_:.").find(c) == std::string::npos;
                            }, '_');
            const std::string path = ds.bugreport_internal_dir_ + "/lshal_debug_" + cleanName;

            {
                auto fd = android::base::unique_fd(
                    TEMP_FAILURE_RETRY(open(path.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)));
                if (fd < 0) {
                    MYLOGE("Could not open %s to dump additional hal information.\n", path.c_str());
                    continue;
                }
                RunCommandToFd(fd,
                        "",
                        {"lshal", "debug", "-E", interface},
                        CommandOptions::WithTimeout(2).AsRootIfAvailable().Build());

                bool empty = 0 == lseek(fd, 0, SEEK_END);
                if (!empty) {
                    ds.AddZipEntry("lshal-debug/" + cleanName + ".txt", path);
                }
            }

            unlink(path.c_str());
        }
    });

    if (!ret.isOk()) {
        MYLOGE("Could not list hals from hwservicemanager.\n");
    }
}

static void DumpExternalFragmentationInfo() {
    struct stat st;
    if (stat("/proc/buddyinfo", &st) != 0) {
        MYLOGE("Unable to dump external fragmentation info\n");
        return;
    }

    printf("------ EXTERNAL FRAGMENTATION INFO ------\n");
    std::ifstream ifs("/proc/buddyinfo");
    auto unusable_index_regex = std::regex{"Node\\s+([0-9]+),\\s+zone\\s+(\\S+)\\s+(.*)"};
    for (std::string line; std::getline(ifs, line);) {
        std::smatch match_results;
        if (std::regex_match(line, match_results, unusable_index_regex)) {
            std::stringstream free_pages(std::string{match_results[3]});
            std::vector<int> free_pages_per_order(std::istream_iterator<int>{free_pages},
                                                  std::istream_iterator<int>());

            int total_free_pages = 0;
            for (size_t i = 0; i < free_pages_per_order.size(); i++) {
                total_free_pages += (free_pages_per_order[i] * std::pow(2, i));
            }

            printf("Node %s, zone %8s", match_results[1].str().c_str(),
                   match_results[2].str().c_str());

            int usable_free_pages = total_free_pages;
            for (size_t i = 0; i < free_pages_per_order.size(); i++) {
                auto unusable_index = (total_free_pages - usable_free_pages) /
                        static_cast<double>(total_free_pages);
                printf(" %5.3f", unusable_index);
                usable_free_pages -= (free_pages_per_order[i] * std::pow(2, i));
            }

            printf("\n");
        }
    }
    printf("\n");
}

// Dumps various things. Returns early with status USER_CONSENT_DENIED if user denies consent
// via the consent they are shown. Ignores other errors that occur while running various
// commands. The consent checking is currently done around long running tasks, which happen to
// be distributed fairly evenly throughout the function.
static Dumpstate::RunStatus dumpstate() {
    DurationReporter duration_reporter("DUMPSTATE");

    // Dump various things. Note that anything that takes "long" (i.e. several seconds) should
    // check intermittently (if it's intrerruptable like a foreach on pids) and/or should be wrapped
    // in a consent check (via RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK).
    dump_dev_files("TRUSTY VERSION", "/sys/bus/platform/drivers/trusty", "trusty_version");
    RunCommand("UPTIME", {"uptime"});
    DumpBlockStatFiles();
    DumpFile("MEMORY INFO", "/proc/meminfo");
    RunCommand("CPU INFO", {"top", "-b", "-n", "1", "-H", "-s", "6", "-o",
                            "pid,tid,user,pr,ni,%cpu,s,virt,res,pcy,cmd,name"});

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunCommand, "PROCRANK", {"procrank"}, AS_ROOT_20);

    DumpFile("VIRTUAL MEMORY STATS", "/proc/vmstat");
    DumpFile("VMALLOC INFO", "/proc/vmallocinfo");
    DumpFile("SLAB INFO", "/proc/slabinfo");
    DumpFile("ZONEINFO", "/proc/zoneinfo");
    DumpFile("PAGETYPEINFO", "/proc/pagetypeinfo");
    DumpFile("BUDDYINFO", "/proc/buddyinfo");
    DumpExternalFragmentationInfo();

    DumpFile("KERNEL WAKE SOURCES", "/d/wakeup_sources");
    DumpFile("KERNEL CPUFREQ", "/sys/devices/system/cpu/cpu0/cpufreq/stats/time_in_state");

    RunCommand("PROCESSES AND THREADS",
               {"ps", "-A", "-T", "-Z", "-O", "pri,nice,rtprio,sched,pcy,time"});

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunCommand, "LIBRANK", {"librank"},
                                         CommandOptions::AS_ROOT);

    DumpHals();

    RunCommand("PRINTENV", {"printenv"});
    RunCommand("NETSTAT", {"netstat", "-nW"});
    struct stat s;
    if (stat("/proc/modules", &s) != 0) {
        MYLOGD("Skipping 'lsmod' because /proc/modules does not exist\n");
    } else {
        RunCommand("LSMOD", {"lsmod"});
    }

    if (__android_logger_property_get_bool(
            "ro.logd.kernel", BOOL_DEFAULT_TRUE | BOOL_DEFAULT_FLAG_ENG | BOOL_DEFAULT_FLAG_SVELTE)) {
        DoKernelLogcat();
    } else {
        do_dmesg();
    }

    RunCommand("LIST OF OPEN FILES", {"lsof"}, CommandOptions::AS_ROOT);

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(for_each_pid, do_showmap, "SMAPS OF ALL PROCESSES");

    for_each_tid(show_wchan, "BLOCKED PROCESS WAIT-CHANNELS");
    for_each_pid(show_showtime, "PROCESS TIMES (pid cmd user system iowait+percentage)");

    /* Dump Bluetooth HCI logs */
    ds.AddDir("/data/misc/bluetooth/logs", true);

    if (ds.options_->do_fb && !ds.do_early_screenshot_) {
        MYLOGI("taking late screenshot\n");
        ds.TakeScreenshot();
    }

    AddAnrTraceFiles();

    // NOTE: tombstones are always added as separate entries in the zip archive
    // and are not interspersed with the main report.
    const bool tombstones_dumped = AddDumps(ds.tombstone_data_.begin(), ds.tombstone_data_.end(),
                                            "TOMBSTONE", true /* add_to_zip */);
    if (!tombstones_dumped) {
        printf("*** NO TOMBSTONES to dump in %s\n\n", TOMBSTONE_DIR.c_str());
    }

    DumpPacketStats();

    RunDumpsys("EBPF MAP STATS", {"netd", "trafficcontroller"});

    DoKmsg();

    DumpIpAddrAndRules();

    dump_route_tables();

    RunCommand("ARP CACHE", {"ip", "-4", "neigh", "show"});
    RunCommand("IPv6 ND CACHE", {"ip", "-6", "neigh", "show"});
    RunCommand("MULTICAST ADDRESSES", {"ip", "maddr"});

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunDumpsysHigh);

    RunCommand("SYSTEM PROPERTIES", {"getprop"});

    RunCommand("STORAGED IO INFO", {"storaged", "-u", "-p"});

    RunCommand("FILESYSTEMS & FREE SPACE", {"df"});

    /* Binder state is expensive to look at as it uses a lot of memory. */
    DumpFile("BINDER FAILED TRANSACTION LOG", "/sys/kernel/debug/binder/failed_transaction_log");
    DumpFile("BINDER TRANSACTION LOG", "/sys/kernel/debug/binder/transaction_log");
    DumpFile("BINDER TRANSACTIONS", "/sys/kernel/debug/binder/transactions");
    DumpFile("BINDER STATS", "/sys/kernel/debug/binder/stats");
    DumpFile("BINDER STATE", "/sys/kernel/debug/binder/state");

    /* Add window and surface trace files. */
    if (!PropertiesHelper::IsUserBuild()) {
        ds.AddDir(WMTRACE_DATA_DIR, false);
    }

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(ds.DumpstateBoard);

    /* Migrate the ril_dumpstate to a device specific dumpstate? */
    int rilDumpstateTimeout = android::base::GetIntProperty("ril.dumpstate.timeout", 0);
    if (rilDumpstateTimeout > 0) {
        // su does not exist on user builds, so try running without it.
        // This way any implementations of vril-dump that do not require
        // root can run on user builds.
        CommandOptions::CommandOptionsBuilder options =
            CommandOptions::WithTimeout(rilDumpstateTimeout);
        if (!PropertiesHelper::IsUserBuild()) {
            options.AsRoot();
        }
        RunCommand("DUMP VENDOR RIL LOGS", {"vril-dump"}, options.Build());
    }

    printf("========================================================\n");
    printf("== Android Framework Services\n");
    printf("========================================================\n");

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunDumpsysNormal);

    printf("========================================================\n");
    printf("== Checkins\n");
    printf("========================================================\n");

    RunDumpsys("CHECKIN BATTERYSTATS", {"batterystats", "-c"});

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunDumpsys, "CHECKIN MEMINFO", {"meminfo", "--checkin"});

    RunDumpsys("CHECKIN NETSTATS", {"netstats", "--checkin"});
    RunDumpsys("CHECKIN PROCSTATS", {"procstats", "-c"});
    RunDumpsys("CHECKIN USAGESTATS", {"usagestats", "-c"});
    RunDumpsys("CHECKIN PACKAGE", {"package", "--checkin"});

    printf("========================================================\n");
    printf("== Running Application Activities\n");
    printf("========================================================\n");

    // The following dumpsys internally collects output from running apps, so it can take a long
    // time. So let's extend the timeout.

    const CommandOptions DUMPSYS_COMPONENTS_OPTIONS = CommandOptions::WithTimeout(60).Build();

    RunDumpsys("APP ACTIVITIES", {"activity", "-v", "all"}, DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Running Application Services (platform)\n");
    printf("========================================================\n");

    RunDumpsys("APP SERVICES PLATFORM", {"activity", "service", "all-platform-non-critical"},
            DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Running Application Services (non-platform)\n");
    printf("========================================================\n");

    RunDumpsys("APP SERVICES NON-PLATFORM", {"activity", "service", "all-non-platform"},
            DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Running Application Providers (platform)\n");
    printf("========================================================\n");

    RunDumpsys("APP PROVIDERS PLATFORM", {"activity", "provider", "all-platform"},
            DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Running Application Providers (non-platform)\n");
    printf("========================================================\n");

    RunDumpsys("APP PROVIDERS NON-PLATFORM", {"activity", "provider", "all-non-platform"},
            DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Dropbox crashes\n");
    printf("========================================================\n");

    RunDumpsys("DROPBOX SYSTEM SERVER CRASHES", {"dropbox", "-p", "system_server_crash"});
    RunDumpsys("DROPBOX SYSTEM APP CRASHES", {"dropbox", "-p", "system_app_crash"});

    printf("========================================================\n");
    printf("== Final progress (pid %d): %d/%d (estimated %d)\n", ds.pid_, ds.progress_->Get(),
           ds.progress_->GetMax(), ds.progress_->GetInitialMax());
    printf("========================================================\n");
    printf("== dumpstate: done (id %d)\n", ds.id_);
    printf("========================================================\n");

    printf("========================================================\n");
    printf("== Obtaining statsd metadata\n");
    printf("========================================================\n");
    // This differs from the usual dumpsys stats, which is the stats report data.
    RunDumpsys("STATSDSTATS", {"stats", "--metadata"});

    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(DumpIncidentReport);

    return Dumpstate::RunStatus::OK;
}

/*
 * Dumps state for the default case; drops root after it's no longer necessary.
 *
 * Returns RunStatus::OK if everything went fine.
 * Returns RunStatus::ERROR if there was an error.
 * Returns RunStatus::USER_DENIED_CONSENT if user explicitly denied consent to sharing the bugreport
 * with the caller.
 */
static Dumpstate::RunStatus DumpstateDefault() {
    // Invoking the following dumpsys calls before DumpTraces() to try and
    // keep the system stats as close to its initial state as possible.
    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(RunDumpsysCritical);

    // Capture first logcat early on; useful to take a snapshot before dumpstate logs take over the
    // buffer.
    DoLogcat();
    // Capture timestamp after first logcat to use in next logcat
    time_t logcat_ts = time(nullptr);

    /* collect stack traces from Dalvik and native processes (needs root) */
    RUN_SLOW_FUNCTION_WITH_CONSENT_CHECK(ds.DumpTraces, &dump_traces_path);

    /* Run some operations that require root. */
    ds.tombstone_data_ = GetDumpFds(TOMBSTONE_DIR, TOMBSTONE_FILE_PREFIX, !ds.IsZipping());
    ds.anr_data_ = GetDumpFds(ANR_DIR, ANR_FILE_PREFIX, !ds.IsZipping());

    ds.AddDir(RECOVERY_DIR, true);
    ds.AddDir(RECOVERY_DATA_DIR, true);
    ds.AddDir(UPDATE_ENGINE_LOG_DIR, true);
    ds.AddDir(LOGPERSIST_DATA_DIR, false);
    if (!PropertiesHelper::IsUserBuild()) {
        ds.AddDir(PROFILE_DATA_DIR_CUR, true);
        ds.AddDir(PROFILE_DATA_DIR_REF, true);
    }
    add_mountinfo();
    DumpIpTablesAsRoot();
    DumpDynamicPartitionInfo();
    ds.AddDir(OTA_METADATA_DIR, true);

    // Capture any IPSec policies in play. No keys are exposed here.
    RunCommand("IP XFRM POLICY", {"ip", "xfrm", "policy"}, CommandOptions::WithTimeout(10).Build());

    // Dump IPsec stats. No keys are exposed here.
    DumpFile("XFRM STATS", XFRM_STAT_PROC_FILE);

    // Run ss as root so we can see socket marks.
    RunCommand("DETAILED SOCKET STATE", {"ss", "-eionptu"}, CommandOptions::WithTimeout(10).Build());

    // Run iotop as root to show top 100 IO threads
    RunCommand("IOTOP", {"iotop", "-n", "1", "-m", "100"});

    // Gather shared memory buffer info if the product implements it
    struct stat st;
    if (!stat("/product/bin/dmabuf_dump", &st)) {
        RunCommand("Dmabuf dump", {"/product/bin/dmabuf_dump"});
    }

    DumpFile("PSI cpu", "/proc/pressure/cpu");
    DumpFile("PSI memory", "/proc/pressure/memory");
    DumpFile("PSI io", "/proc/pressure/io");

    if (!DropRootUser()) {
        return Dumpstate::RunStatus::ERROR;
    }

    RETURN_IF_USER_DENIED_CONSENT();
    Dumpstate::RunStatus status = dumpstate();
    // Capture logcat since the last time we did it.
    DoSystemLogcat(logcat_ts);
    return status;
}

// This method collects common dumpsys for telephony and wifi
static void DumpstateRadioCommon() {
    DumpIpTablesAsRoot();

    ds.AddDir(LOGPERSIST_DATA_DIR, false);

    if (!DropRootUser()) {
        return;
    }

    do_dmesg();
    DoLogcat();
    DumpPacketStats();
    DoKmsg();
    DumpIpAddrAndRules();
    dump_route_tables();
    DumpHals();

    RunDumpsys("NETWORK DIAGNOSTICS", {"connectivity", "--diag"},
               CommandOptions::WithTimeout(10).Build());
}

// This method collects dumpsys for telephony debugging only
static void DumpstateTelephonyOnly() {
    DurationReporter duration_reporter("DUMPSTATE");
    const CommandOptions DUMPSYS_COMPONENTS_OPTIONS = CommandOptions::WithTimeout(60).Build();

    DumpstateRadioCommon();

    RunCommand("SYSTEM PROPERTIES", {"getprop"});

    printf("========================================================\n");
    printf("== Android Framework Services\n");
    printf("========================================================\n");

    RunDumpsys("DUMPSYS", {"connectivity"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));
    RunDumpsys("DUMPSYS", {"connmetrics"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));
    RunDumpsys("DUMPSYS", {"netd"}, CommandOptions::WithTimeout(90).Build(), SEC_TO_MSEC(10));
    RunDumpsys("DUMPSYS", {"carrier_config"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));
    RunDumpsys("DUMPSYS", {"wifi"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));
    RunDumpsys("BATTERYSTATS", {"batterystats"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));

    printf("========================================================\n");
    printf("== Running Application Services\n");
    printf("========================================================\n");

    RunDumpsys("TELEPHONY SERVICES", {"activity", "service", "TelephonyDebugService"});

    printf("========================================================\n");
    printf("== Running Application Services (non-platform)\n");
    printf("========================================================\n");

    RunDumpsys("APP SERVICES NON-PLATFORM", {"activity", "service", "all-non-platform"},
            DUMPSYS_COMPONENTS_OPTIONS);

    printf("========================================================\n");
    printf("== Checkins\n");
    printf("========================================================\n");

    RunDumpsys("CHECKIN BATTERYSTATS", {"batterystats", "-c"});

    printf("========================================================\n");
    printf("== dumpstate: done (id %d)\n", ds.id_);
    printf("========================================================\n");
}

// This method collects dumpsys for wifi debugging only
static void DumpstateWifiOnly() {
    DurationReporter duration_reporter("DUMPSTATE");

    DumpstateRadioCommon();

    printf("========================================================\n");
    printf("== Android Framework Services\n");
    printf("========================================================\n");

    RunDumpsys("DUMPSYS", {"connectivity"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));
    RunDumpsys("DUMPSYS", {"wifi"}, CommandOptions::WithTimeout(90).Build(),
               SEC_TO_MSEC(10));

    printf("========================================================\n");
    printf("== dumpstate: done (id %d)\n", ds.id_);
    printf("========================================================\n");
}

Dumpstate::RunStatus Dumpstate::DumpTraces(const char** path) {
    DurationReporter duration_reporter("DUMP TRACES");

    const std::string temp_file_pattern = "/data/anr/dumptrace_XXXXXX";
    const size_t buf_size = temp_file_pattern.length() + 1;
    std::unique_ptr<char[]> file_name_buf(new char[buf_size]);
    memcpy(file_name_buf.get(), temp_file_pattern.c_str(), buf_size);

    // Create a new, empty file to receive all trace dumps.
    //
    // TODO: This can be simplified once we remove support for the old style
    // dumps. We can have a file descriptor passed in to dump_traces instead
    // of creating a file, closing it and then reopening it again.
    android::base::unique_fd fd(mkostemp(file_name_buf.get(), O_APPEND | O_CLOEXEC));
    if (fd < 0) {
        MYLOGE("mkostemp on pattern %s: %s\n", file_name_buf.get(), strerror(errno));
        return RunStatus::OK;
    }

    // Nobody should have access to this temporary file except dumpstate, but we
    // temporarily grant 'read' to 'others' here because this file is created
    // when tombstoned is still running as root, but dumped after dropping. This
    // can go away once support for old style dumping has.
    const int chmod_ret = fchmod(fd, 0666);
    if (chmod_ret < 0) {
        MYLOGE("fchmod on %s failed: %s\n", file_name_buf.get(), strerror(errno));
        return RunStatus::OK;
    }

    std::unique_ptr<DIR, decltype(&closedir)> proc(opendir("/proc"), closedir);
    if (proc.get() == nullptr) {
        MYLOGE("opendir /proc failed: %s\n", strerror(errno));
        return RunStatus::OK;
    }

    // Number of times process dumping has timed out. If we encounter too many
    // failures, we'll give up.
    int timeout_failures = 0;
    bool dalvik_found = false;

    const std::set<int> hal_pids = get_interesting_hal_pids();

    struct dirent* d;
    while ((d = readdir(proc.get()))) {
        RETURN_IF_USER_DENIED_CONSENT();
        int pid = atoi(d->d_name);
        if (pid <= 0) {
            continue;
        }

        const std::string link_name = android::base::StringPrintf("/proc/%d/exe", pid);
        std::string exe;
        if (!android::base::Readlink(link_name, &exe)) {
            continue;
        }

        bool is_java_process;
        if (exe == "/system/bin/app_process32" || exe == "/system/bin/app_process64") {
            // Don't bother dumping backtraces for the zygote.
            if (IsZygote(pid)) {
                continue;
            }

            dalvik_found = true;
            is_java_process = true;
        } else if (should_dump_native_traces(exe.c_str()) || hal_pids.find(pid) != hal_pids.end()) {
            is_java_process = false;
        } else {
            // Probably a native process we don't care about, continue.
            continue;
        }

        // If 3 backtrace dumps fail in a row, consider debuggerd dead.
        if (timeout_failures == 3) {
            dprintf(fd, "ERROR: Too many stack dump failures, exiting.\n");
            break;
        }

        const uint64_t start = Nanotime();
        const int ret = dump_backtrace_to_file_timeout(
            pid, is_java_process ? kDebuggerdJavaBacktrace : kDebuggerdNativeBacktrace,
            is_java_process ? 5 : 20, fd);

        if (ret == -1) {
            // For consistency, the header and footer to this message match those
            // dumped by debuggerd in the success case.
            dprintf(fd, "\n---- pid %d at [unknown] ----\n", pid);
            dprintf(fd, "Dump failed, likely due to a timeout.\n");
            dprintf(fd, "---- end %d ----", pid);
            timeout_failures++;
            continue;
        }

        // We've successfully dumped stack traces, reset the failure count
        // and write a summary of the elapsed time to the file and continue with the
        // next process.
        timeout_failures = 0;

        dprintf(fd, "[dump %s stack %d: %.3fs elapsed]\n", is_java_process ? "dalvik" : "native",
                pid, (float)(Nanotime() - start) / NANOS_PER_SEC);
    }

    if (!dalvik_found) {
        MYLOGE("Warning: no Dalvik processes found to dump stacks\n");
    }

    *path = file_name_buf.release();
    return RunStatus::OK;
}

void Dumpstate::DumpstateBoard() {
    DurationReporter duration_reporter("dumpstate_board()");
    printf("========================================================\n");
    printf("== Board\n");
    printf("========================================================\n");

    if (!IsZipping()) {
        MYLOGD("Not dumping board info because it's not a zipped bugreport\n");
        return;
    }

    std::vector<std::string> paths;
    std::vector<android::base::ScopeGuard<std::function<void()>>> remover;
    for (int i = 0; i < NUM_OF_DUMPS; i++) {
        paths.emplace_back(StringPrintf("%s/%s", ds.bugreport_internal_dir_.c_str(),
                                        kDumpstateBoardFiles[i].c_str()));
        remover.emplace_back(android::base::make_scope_guard(
            std::bind([](std::string path) { android::os::UnlinkAndLogOnError(path); }, paths[i])));
    }

    sp<IDumpstateDevice> dumpstate_device(IDumpstateDevice::getService());
    if (dumpstate_device == nullptr) {
        MYLOGE("No IDumpstateDevice implementation\n");
        return;
    }

    using ScopedNativeHandle =
            std::unique_ptr<native_handle_t, std::function<void(native_handle_t*)>>;
    ScopedNativeHandle handle(native_handle_create(static_cast<int>(paths.size()), 0),
                              [](native_handle_t* handle) {
                                  native_handle_close(handle);
                                  native_handle_delete(handle);
                              });
    if (handle == nullptr) {
        MYLOGE("Could not create native_handle\n");
        return;
    }

    // TODO(128270426): Check for consent in between?
    for (size_t i = 0; i < paths.size(); i++) {
        MYLOGI("Calling IDumpstateDevice implementation using path %s\n", paths[i].c_str());

        android::base::unique_fd fd(TEMP_FAILURE_RETRY(
            open(paths[i].c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)));
        if (fd < 0) {
            MYLOGE("Could not open file %s: %s\n", paths[i].c_str(), strerror(errno));
            return;
        }
        handle.get()->data[i] = fd.release();
    }

    // Given that bugreport is required to diagnose failures, it's better to
    // set an arbitrary amount of timeout for IDumpstateDevice than to block the
    // rest of bugreport. In the timeout case, we will kill dumpstate board HAL
    // and grab whatever dumped
    std::packaged_task<bool()>
            dumpstate_task([paths, dumpstate_device, &handle]() -> bool {
            android::hardware::Return<void> status = dumpstate_device->dumpstateBoard(handle.get());
            if (!status.isOk()) {
                MYLOGE("dumpstateBoard failed: %s\n", status.description().c_str());
                return false;
            }
            return true;
        });

    auto result = dumpstate_task.get_future();
    std::thread(std::move(dumpstate_task)).detach();

    constexpr size_t timeout_sec = 30;
    if (result.wait_for(std::chrono::seconds(timeout_sec)) != std::future_status::ready) {
        MYLOGE("dumpstateBoard timed out after %zus, killing dumpstate vendor HAL\n", timeout_sec);
        if (!android::base::SetProperty("ctl.interface_restart",
                                        android::base::StringPrintf("%s/default",
                                                                    IDumpstateDevice::descriptor))) {
            MYLOGE("Couldn't restart dumpstate HAL\n");
        }
    }
    // Wait some time for init to kill dumpstate vendor HAL
    constexpr size_t killing_timeout_sec = 10;
    if (result.wait_for(std::chrono::seconds(killing_timeout_sec)) != std::future_status::ready) {
        MYLOGE("killing dumpstateBoard timed out after %zus, continue and "
               "there might be racing in content\n", killing_timeout_sec);
    }

    auto file_sizes = std::make_unique<ssize_t[]>(paths.size());
    for (size_t i = 0; i < paths.size(); i++) {
        struct stat s;
        if (fstat(handle.get()->data[i], &s) == -1) {
            MYLOGE("Failed to fstat %s: %s\n", kDumpstateBoardFiles[i].c_str(),
                   strerror(errno));
            file_sizes[i] = -1;
            continue;
        }
        file_sizes[i] = s.st_size;
    }

    for (size_t i = 0; i < paths.size(); i++) {
        if (file_sizes[i] == -1) {
            continue;
        }
        if (file_sizes[i] == 0) {
            MYLOGE("Ignoring empty %s\n", kDumpstateBoardFiles[i].c_str());
            continue;
        }
        AddZipEntry(kDumpstateBoardFiles[i], paths[i]);
    }

    printf("*** See dumpstate-board.txt entry ***\n");
}

static void ShowUsage() {
    fprintf(stderr,
            "usage: dumpstate [-h] [-b soundfile] [-e soundfile] [-d] [-p] "
            "[-z]] [-s] [-S] [-q] [-B] [-P] [-R] [-V version]\n"
            "  -h: display this help message\n"
            "  -b: play sound file instead of vibrate, at beginning of job\n"
            "  -e: play sound file instead of vibrate, at end of job\n"
            "  -d: append date to filename\n"
            "  -p: capture screenshot to filename.png\n"
            "  -z: generate zipped file\n"
            "  -s: write output to control socket (for init)\n"
            "  -S: write file location to control socket (for init; requires -z)\n"
            "  -q: disable vibrate\n"
            "  -B: send broadcast when finished\n"
            "  -P: send broadcast when started and update system properties on "
            "progress (requires -B)\n"
            "  -R: take bugreport in remote mode (requires -z, -d and -B, "
            "shouldn't be used with -P)\n"
            "  -w: start binder service and make it wait for a call to startBugreport\n"
            "  -v: prints the dumpstate header and exit\n");
}

static void register_sig_handler() {
    signal(SIGPIPE, SIG_IGN);
}

bool Dumpstate::FinishZipFile() {
    std::string entry_name = base_name_ + "-" + name_ + ".txt";
    MYLOGD("Adding main entry (%s) from %s to .zip bugreport\n", entry_name.c_str(),
           tmp_path_.c_str());
    // Final timestamp
    char date[80];
    time_t the_real_now_please_stand_up = time(nullptr);
    strftime(date, sizeof(date), "%Y/%m/%d %H:%M:%S", localtime(&the_real_now_please_stand_up));
    MYLOGD("dumpstate id %d finished around %s (%ld s)\n", ds.id_, date,
           the_real_now_please_stand_up - ds.now_);

    if (!ds.AddZipEntry(entry_name, tmp_path_)) {
        MYLOGE("Failed to add text entry to .zip file\n");
        return false;
    }
    if (!AddTextZipEntry("main_entry.txt", entry_name)) {
        MYLOGE("Failed to add main_entry.txt to .zip file\n");
        return false;
    }

    // Add log file (which contains stderr output) to zip...
    fprintf(stderr, "dumpstate_log.txt entry on zip file logged up to here\n");
    if (!ds.AddZipEntry("dumpstate_log.txt", ds.log_path_.c_str())) {
        MYLOGE("Failed to add dumpstate log to .zip file\n");
        return false;
    }
    // TODO: Should truncate the existing file.
    // ... and re-open it for further logging.
    if (!redirect_to_existing_file(stderr, const_cast<char*>(ds.log_path_.c_str()))) {
        return false;
    }
    fprintf(stderr, "\n");

    int32_t err = zip_writer_->Finish();
    if (err != 0) {
        MYLOGE("zip_writer_->Finish(): %s\n", ZipWriter::ErrorCodeString(err));
        return false;
    }

    // TODO: remove once FinishZipFile() is automatically handled by Dumpstate's destructor.
    ds.zip_file.reset(nullptr);

    MYLOGD("Removing temporary file %s\n", tmp_path_.c_str())
    android::os::UnlinkAndLogOnError(tmp_path_);

    return true;
}

static std::string SHA256_file_hash(const std::string& filepath) {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(filepath.c_str(), O_RDONLY | O_NONBLOCK
            | O_CLOEXEC | O_NOFOLLOW)));
    if (fd == -1) {
        MYLOGE("open(%s): %s\n", filepath.c_str(), strerror(errno));
        return nullptr;
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    std::vector<uint8_t> buffer(65536);
    while (1) {
        ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd.get(), buffer.data(), buffer.size()));
        if (bytes_read == 0) {
            break;
        } else if (bytes_read == -1) {
            MYLOGE("read(%s): %s\n", filepath.c_str(), strerror(errno));
            return nullptr;
        }

        SHA256_Update(&ctx, buffer.data(), bytes_read);
    }

    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    char hash_buffer[SHA256_DIGEST_LENGTH * 2 + 1];
    for(size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hash_buffer + (i * 2), "%02x", hash[i]);
    }
    hash_buffer[sizeof(hash_buffer) - 1] = 0;
    return std::string(hash_buffer);
}

static void SendBroadcast(const std::string& action, const std::vector<std::string>& args) {
    // clang-format off
    std::vector<std::string> am = {"/system/bin/cmd", "activity", "broadcast", "--user", "0",
                    "--receiver-foreground", "--receiver-include-background", "-a", action};
    // clang-format on

    am.insert(am.end(), args.begin(), args.end());

    RunCommand("", am,
               CommandOptions::WithTimeout(20)
                   .Log("Sending broadcast: '%s'\n")
                   .Always()
                   .DropRoot()
                   .RedirectStderr()
                   .Build());
}

static void Vibrate(int duration_ms) {
    // clang-format off
    RunCommand("", {"cmd", "vibrator", "vibrate", "-f", std::to_string(duration_ms), "dumpstate"},
               CommandOptions::WithTimeout(10)
                   .Log("Vibrate: '%s'\n")
                   .Always()
                   .Build());
    // clang-format on
}

static void MaybeResolveSymlink(std::string* path) {
    std::string resolved_path;
    if (android::base::Readlink(*path, &resolved_path)) {
        *path = resolved_path;
    }
}

/*
 * Prepares state like filename, screenshot path, etc in Dumpstate. Also initializes ZipWriter
 * if we are writing zip files and adds the version file.
 */
static void PrepareToWriteToFile() {
    MaybeResolveSymlink(&ds.bugreport_internal_dir_);

    std::string build_id = android::base::GetProperty("ro.build.id", "UNKNOWN_BUILD");
    std::string device_name = android::base::GetProperty("ro.product.name", "UNKNOWN_DEVICE");
    ds.base_name_ = StringPrintf("bugreport-%s-%s", device_name.c_str(), build_id.c_str());
    if (ds.options_->do_add_date) {
        char date[80];
        strftime(date, sizeof(date), "%Y-%m-%d-%H-%M-%S", localtime(&ds.now_));
        ds.name_ = date;
    } else {
        ds.name_ = "undated";
    }

    if (ds.options_->telephony_only) {
        ds.base_name_ += "-telephony";
    } else if (ds.options_->wifi_only) {
        ds.base_name_ += "-wifi";
    }

    if (ds.options_->do_fb) {
        ds.screenshot_path_ = ds.GetPath(".png");
    }
    ds.tmp_path_ = ds.GetPath(".tmp");
    ds.log_path_ = ds.GetPath("-dumpstate_log-" + std::to_string(ds.pid_) + ".txt");

    std::string destination = ds.options_->bugreport_fd.get() != -1
                                  ? StringPrintf("[fd:%d]", ds.options_->bugreport_fd.get())
                                  : ds.bugreport_internal_dir_.c_str();
    MYLOGD(
        "Bugreport dir: %s\n"
        "Base name: %s\n"
        "Suffix: %s\n"
        "Log path: %s\n"
        "Temporary path: %s\n"
        "Screenshot path: %s\n",
        destination.c_str(), ds.base_name_.c_str(), ds.name_.c_str(), ds.log_path_.c_str(),
        ds.tmp_path_.c_str(), ds.screenshot_path_.c_str());

    if (ds.options_->do_zip_file) {
        ds.path_ = ds.GetPath(".zip");
        MYLOGD("Creating initial .zip file (%s)\n", ds.path_.c_str());
        create_parent_dirs(ds.path_.c_str());
        ds.zip_file.reset(fopen(ds.path_.c_str(), "wb"));
        if (ds.zip_file == nullptr) {
            MYLOGE("fopen(%s, 'wb'): %s\n", ds.path_.c_str(), strerror(errno));
        } else {
            ds.zip_writer_.reset(new ZipWriter(ds.zip_file.get()));
        }
        ds.AddTextZipEntry("version.txt", ds.version_);
    }
}

/*
 * Finalizes writing to the file by renaming or zipping the tmp file to the final location,
 * printing zipped file status, etc.
 */
static void FinalizeFile() {
    /* check if user changed the suffix using system properties */
    std::string name =
        android::base::GetProperty(android::base::StringPrintf("dumpstate.%d.name", ds.pid_), "");
    bool change_suffix = false;
    if (!name.empty()) {
        /* must whitelist which characters are allowed, otherwise it could cross directories */
        std::regex valid_regex("^[-_a-zA-Z0-9]+$");
        if (std::regex_match(name.c_str(), valid_regex)) {
            change_suffix = true;
        } else {
            MYLOGE("invalid suffix provided by user: %s\n", name.c_str());
        }
    }
    if (change_suffix) {
        MYLOGI("changing suffix from %s to %s\n", ds.name_.c_str(), name.c_str());
        ds.name_ = name;
        if (!ds.screenshot_path_.empty()) {
            std::string new_screenshot_path = ds.GetPath(".png");
            if (rename(ds.screenshot_path_.c_str(), new_screenshot_path.c_str())) {
                MYLOGE("rename(%s, %s): %s\n", ds.screenshot_path_.c_str(),
                       new_screenshot_path.c_str(), strerror(errno));
            } else {
                ds.screenshot_path_ = new_screenshot_path;
            }
        }
    }

    bool do_text_file = true;
    if (ds.options_->do_zip_file) {
        if (!ds.FinishZipFile()) {
            MYLOGE("Failed to finish zip file; sending text bugreport instead\n");
            do_text_file = true;
        } else {
            do_text_file = false;
            // If the user has changed the suffix, we need to change the zip file name.
            std::string new_path = ds.GetPath(".zip");
            if (ds.path_ != new_path) {
                MYLOGD("Renaming zip file from %s to %s\n", ds.path_.c_str(), new_path.c_str());
                if (rename(ds.path_.c_str(), new_path.c_str())) {
                    MYLOGE("rename(%s, %s): %s\n", ds.path_.c_str(), new_path.c_str(),
                           strerror(errno));
                } else {
                    ds.path_ = new_path;
                }
            }
        }
    }
    if (do_text_file) {
        ds.path_ = ds.GetPath(".txt");
        MYLOGD("Generating .txt bugreport at %s from %s\n", ds.path_.c_str(), ds.tmp_path_.c_str());
        if (rename(ds.tmp_path_.c_str(), ds.path_.c_str())) {
            MYLOGE("rename(%s, %s): %s\n", ds.tmp_path_.c_str(), ds.path_.c_str(), strerror(errno));
            ds.path_.clear();
        }
    }
    if (ds.options_->use_control_socket) {
        if (do_text_file) {
            dprintf(ds.control_socket_fd_,
                    "FAIL:could not create zip file, check %s "
                    "for more details\n",
                    ds.log_path_.c_str());
        } else {
            dprintf(ds.control_socket_fd_, "OK:%s\n", ds.path_.c_str());
        }
    }
}

/* Broadcasts that we are done with the bugreport */
static void SendBugreportFinishedBroadcast() {
    // TODO(b/111441001): use callback instead of broadcast.
    if (!ds.path_.empty()) {
        MYLOGI("Final bugreport path: %s\n", ds.path_.c_str());
        // clang-format off

        std::vector<std::string> am_args = {
             "--receiver-permission", "android.permission.DUMP",
             "--ei", "android.intent.extra.ID", std::to_string(ds.id_),
             "--ei", "android.intent.extra.PID", std::to_string(ds.pid_),
             "--ei", "android.intent.extra.MAX", std::to_string(ds.progress_->GetMax()),
             "--es", "android.intent.extra.BUGREPORT", ds.path_,
             "--es", "android.intent.extra.DUMPSTATE_LOG", ds.log_path_
        };
        // clang-format on
        if (ds.options_->do_fb && !android::os::IsFileEmpty(ds.screenshot_path_)) {
            am_args.push_back("--es");
            am_args.push_back("android.intent.extra.SCREENSHOT");
            am_args.push_back(ds.screenshot_path_);
        }
        if (!ds.options_->notification_title.empty()) {
            am_args.push_back("--es");
            am_args.push_back("android.intent.extra.TITLE");
            am_args.push_back(ds.options_->notification_title);
            if (!ds.options_->notification_description.empty()) {
                am_args.push_back("--es");
                am_args.push_back("android.intent.extra.DESCRIPTION");
                am_args.push_back(ds.options_->notification_description);
            }
        }
        if (ds.options_->is_remote_mode) {
            am_args.push_back("--es");
            am_args.push_back("android.intent.extra.REMOTE_BUGREPORT_HASH");
            am_args.push_back(SHA256_file_hash(ds.path_));
            SendBroadcast("com.android.internal.intent.action.REMOTE_BUGREPORT_FINISHED", am_args);
        } else {
            SendBroadcast("com.android.internal.intent.action.BUGREPORT_FINISHED", am_args);
        }
    } else {
        MYLOGE("Skipping finished broadcast because bugreport could not be generated\n");
    }
}

static inline const char* ModeToString(Dumpstate::BugreportMode mode) {
    switch (mode) {
        case Dumpstate::BugreportMode::BUGREPORT_FULL:
            return "BUGREPORT_FULL";
        case Dumpstate::BugreportMode::BUGREPORT_INTERACTIVE:
            return "BUGREPORT_INTERACTIVE";
        case Dumpstate::BugreportMode::BUGREPORT_REMOTE:
            return "BUGREPORT_REMOTE";
        case Dumpstate::BugreportMode::BUGREPORT_WEAR:
            return "BUGREPORT_WEAR";
        case Dumpstate::BugreportMode::BUGREPORT_TELEPHONY:
            return "BUGREPORT_TELEPHONY";
        case Dumpstate::BugreportMode::BUGREPORT_WIFI:
            return "BUGREPORT_WIFI";
        case Dumpstate::BugreportMode::BUGREPORT_DEFAULT:
            return "BUGREPORT_DEFAULT";
    }
}

static void SetOptionsFromMode(Dumpstate::BugreportMode mode, Dumpstate::DumpOptions* options) {
    options->extra_options = ModeToString(mode);
    switch (mode) {
        case Dumpstate::BugreportMode::BUGREPORT_FULL:
            options->do_broadcast = true;
            options->do_fb = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_INTERACTIVE:
            // Currently, the dumpstate binder is only used by Shell to update progress.
            options->do_start_service = true;
            options->do_progress_updates = true;
            options->do_fb = false;
            options->do_broadcast = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_REMOTE:
            options->do_vibrate = false;
            options->is_remote_mode = true;
            options->do_fb = false;
            options->do_broadcast = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_WEAR:
            options->do_start_service = true;
            options->do_progress_updates = true;
            options->do_zip_file = true;
            options->do_fb = true;
            options->do_broadcast = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_TELEPHONY:
            options->telephony_only = true;
            options->do_fb = false;
            options->do_broadcast = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_WIFI:
            options->wifi_only = true;
            options->do_zip_file = true;
            options->do_fb = false;
            options->do_broadcast = true;
            break;
        case Dumpstate::BugreportMode::BUGREPORT_DEFAULT:
            break;
    }
}

static Dumpstate::BugreportMode getBugreportModeFromProperty() {
    // If the system property is not set, it's assumed to be a default bugreport.
    Dumpstate::BugreportMode mode = Dumpstate::BugreportMode::BUGREPORT_DEFAULT;

    std::string extra_options = android::base::GetProperty(PROPERTY_EXTRA_OPTIONS, "");
    if (!extra_options.empty()) {
        // Framework uses a system property to override some command-line args.
        // Currently, it contains the type of the requested bugreport.
        if (extra_options == "bugreportplus") {
            mode = Dumpstate::BugreportMode::BUGREPORT_INTERACTIVE;
        } else if (extra_options == "bugreportfull") {
            mode = Dumpstate::BugreportMode::BUGREPORT_FULL;
        } else if (extra_options == "bugreportremote") {
            mode = Dumpstate::BugreportMode::BUGREPORT_REMOTE;
        } else if (extra_options == "bugreportwear") {
            mode = Dumpstate::BugreportMode::BUGREPORT_WEAR;
        } else if (extra_options == "bugreporttelephony") {
            mode = Dumpstate::BugreportMode::BUGREPORT_TELEPHONY;
        } else if (extra_options == "bugreportwifi") {
            mode = Dumpstate::BugreportMode::BUGREPORT_WIFI;
        } else {
            MYLOGE("Unknown extra option: %s\n", extra_options.c_str());
        }
        // Reset the property
        android::base::SetProperty(PROPERTY_EXTRA_OPTIONS, "");
    }
    return mode;
}

// TODO: Move away from system properties when we have options passed via binder calls.
/* Sets runtime options from the system properties and then clears those properties. */
static void SetOptionsFromProperties(Dumpstate::DumpOptions* options) {
    Dumpstate::BugreportMode mode = getBugreportModeFromProperty();
    SetOptionsFromMode(mode, options);

    options->notification_title = android::base::GetProperty(PROPERTY_EXTRA_TITLE, "");
    if (!options->notification_title.empty()) {
        // Reset the property
        android::base::SetProperty(PROPERTY_EXTRA_TITLE, "");

        options->notification_description =
            android::base::GetProperty(PROPERTY_EXTRA_DESCRIPTION, "");
        if (!options->notification_description.empty()) {
            // Reset the property
            android::base::SetProperty(PROPERTY_EXTRA_DESCRIPTION, "");
        }
        MYLOGD("notification (title:  %s, description: %s)\n", options->notification_title.c_str(),
               options->notification_description.c_str());
    }
}

static void LogDumpOptions(const Dumpstate::DumpOptions& options) {
    MYLOGI("do_zip_file: %d\n", options.do_zip_file);
    MYLOGI("do_add_date: %d\n", options.do_add_date);
    MYLOGI("do_vibrate: %d\n", options.do_vibrate);
    MYLOGI("use_socket: %d\n", options.use_socket);
    MYLOGI("use_control_socket: %d\n", options.use_control_socket);
    MYLOGI("do_fb: %d\n", options.do_fb);
    MYLOGI("do_broadcast: %d\n", options.do_broadcast);
    MYLOGI("is_remote_mode: %d\n", options.is_remote_mode);
    MYLOGI("show_header_only: %d\n", options.show_header_only);
    MYLOGI("do_start_service: %d\n", options.do_start_service);
    MYLOGI("telephony_only: %d\n", options.telephony_only);
    MYLOGI("wifi_only: %d\n", options.wifi_only);
    MYLOGI("do_progress_updates: %d\n", options.do_progress_updates);
    MYLOGI("fd: %d\n", options.bugreport_fd.get());
    MYLOGI("extra_options: %s\n", options.extra_options.c_str());
    MYLOGI("args: %s\n", options.args.c_str());
    MYLOGI("notification_title: %s\n", options.notification_title.c_str());
    MYLOGI("notification_description: %s\n", options.notification_description.c_str());
}

void Dumpstate::DumpOptions::Initialize(BugreportMode bugreport_mode,
                                        const android::base::unique_fd& bugreport_fd_in,
                                        const android::base::unique_fd& screenshot_fd_in) {
    // In the new API world, date is always added; output is always a zip file.
    // TODO(111441001): remove these options once they are obsolete.
    do_add_date = true;
    do_zip_file = true;

    // Duplicate the fds because the passed in fds don't outlive the binder transaction.
    bugreport_fd.reset(dup(bugreport_fd_in.get()));
    screenshot_fd.reset(dup(screenshot_fd_in.get()));

    extra_options = ModeToString(bugreport_mode);
    SetOptionsFromMode(bugreport_mode, this);
}

Dumpstate::RunStatus Dumpstate::DumpOptions::Initialize(int argc, char* argv[]) {
    RunStatus status = RunStatus::OK;
    int c;
    while ((c = getopt(argc, argv, "dho:svqzpPBRSV:w")) != -1) {
        switch (c) {
            // clang-format off
            case 'd': do_add_date = true;            break;
            case 'z': do_zip_file = true;            break;
            // o=use_outfile not supported anymore.
            // TODO(b/111441001): Remove when all callers have migrated.
            case 'o': break;
            case 's': use_socket = true;             break;
            case 'S': use_control_socket = true;     break;
            case 'v': show_header_only = true;       break;
            case 'q': do_vibrate = false;            break;
            case 'p': do_fb = true;                  break;
            case 'P': do_progress_updates = true;    break;
            case 'R': is_remote_mode = true;         break;
            case 'B': do_broadcast = true;           break;
            case 'V':                                break;  // compatibility no-op
            case 'w':
                // This was already processed
                break;
            case 'h':
                status = RunStatus::HELP;
                break;
            default:
                fprintf(stderr, "Invalid option: %c\n", c);
                status = RunStatus::INVALID_INPUT;
                break;
                // clang-format on
        }
    }

    for (int i = 0; i < argc; i++) {
        args += argv[i];
        if (i < argc - 1) {
            args += " ";
        }
    }

    // Reset next index used by getopt so this can be called multiple times, for eg, in tests.
    optind = 1;

    SetOptionsFromProperties(this);
    return status;
}

bool Dumpstate::DumpOptions::ValidateOptions() const {
    if (bugreport_fd.get() != -1 && !do_zip_file) {
        return false;
    }

    if ((do_zip_file || do_add_date || do_progress_updates || do_broadcast) && !OutputToFile()) {
        return false;
    }

    if (use_control_socket && !do_zip_file) {
        return false;
    }

    if (do_progress_updates && !do_broadcast) {
        return false;
    }

    if (is_remote_mode && (do_progress_updates || !do_broadcast || !do_zip_file || !do_add_date)) {
        return false;
    }
    return true;
}

void Dumpstate::SetOptions(std::unique_ptr<DumpOptions> options) {
    options_ = std::move(options);
}

Dumpstate::RunStatus Dumpstate::Run(int32_t calling_uid, const std::string& calling_package) {
    Dumpstate::RunStatus status = RunInternal(calling_uid, calling_package);
    if (listener_ != nullptr) {
        switch (status) {
            case Dumpstate::RunStatus::OK:
                listener_->onFinished();
                break;
            case Dumpstate::RunStatus::HELP:
                break;
            case Dumpstate::RunStatus::INVALID_INPUT:
                listener_->onError(IDumpstateListener::BUGREPORT_ERROR_INVALID_INPUT);
                break;
            case Dumpstate::RunStatus::ERROR:
                listener_->onError(IDumpstateListener::BUGREPORT_ERROR_RUNTIME_ERROR);
                break;
            case Dumpstate::RunStatus::USER_CONSENT_DENIED:
                listener_->onError(IDumpstateListener::BUGREPORT_ERROR_USER_DENIED_CONSENT);
                break;
            case Dumpstate::RunStatus::USER_CONSENT_TIMED_OUT:
                listener_->onError(IDumpstateListener::BUGREPORT_ERROR_USER_CONSENT_TIMED_OUT);
                break;
        }
    }
    return status;
}

/*
 * Dumps relevant information to a bugreport based on the given options.
 *
 * The bugreport can be dumped to a file or streamed to a socket.
 *
 * How dumping to file works:
 * stdout is redirected to a temporary file. This will later become the main bugreport entry.
 * stderr is redirected a log file.
 *
 * The temporary bugreport is then populated via printfs, dumping contents of files and
 * output of commands to stdout.
 *
 * If zipping, the temporary bugreport file is added to the zip archive. Else it's renamed to final
 * text file.
 *
 * If zipping, a bunch of other files and dumps also get added to the zip archive. The log file also
 * gets added to the archive.
 *
 * Bugreports are first generated in a local directory and later copied to the caller's fd if
 * supplied.
 */
Dumpstate::RunStatus Dumpstate::RunInternal(int32_t calling_uid,
                                            const std::string& calling_package) {
    LogDumpOptions(*options_);
    if (!options_->ValidateOptions()) {
        MYLOGE("Invalid options specified\n");
        return RunStatus::INVALID_INPUT;
    }
    /* set as high priority, and protect from OOM killer */
    setpriority(PRIO_PROCESS, 0, -20);

    FILE* oom_adj = fopen("/proc/self/oom_score_adj", "we");
    if (oom_adj) {
        fputs("-1000", oom_adj);
        fclose(oom_adj);
    } else {
        /* fallback to kernels <= 2.6.35 */
        oom_adj = fopen("/proc/self/oom_adj", "we");
        if (oom_adj) {
            fputs("-17", oom_adj);
            fclose(oom_adj);
        }
    }

    if (version_ == VERSION_DEFAULT) {
        version_ = VERSION_CURRENT;
    }

    if (version_ != VERSION_CURRENT && version_ != VERSION_SPLIT_ANR) {
        MYLOGE("invalid version requested ('%s'); suppported values are: ('%s', '%s', '%s')\n",
               version_.c_str(), VERSION_DEFAULT.c_str(), VERSION_CURRENT.c_str(),
               VERSION_SPLIT_ANR.c_str());
        return RunStatus::INVALID_INPUT;
    }

    if (options_->show_header_only) {
        PrintHeader();
        return RunStatus::OK;
    }

    if (options_->bugreport_fd.get() != -1) {
        // If the output needs to be copied over to the caller's fd, get user consent.
        android::String16 package(calling_package.c_str());
        CheckUserConsent(calling_uid, package);
    }

    // Redirect output if needed
    bool is_redirecting = options_->OutputToFile();

    // TODO: temporarily set progress until it's part of the Dumpstate constructor
    std::string stats_path =
        is_redirecting
            ? android::base::StringPrintf("%s/dumpstate-stats.txt", bugreport_internal_dir_.c_str())
            : "";
    progress_.reset(new Progress(stats_path));

    /* gets the sequential id */
    uint32_t last_id = android::base::GetIntProperty(PROPERTY_LAST_ID, 0);
    id_ = ++last_id;
    android::base::SetProperty(PROPERTY_LAST_ID, std::to_string(last_id));

    MYLOGI("begin\n");

    if (acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME) < 0) {
        MYLOGE("Failed to acquire wake lock: %s\n", strerror(errno));
    } else {
        // Wake lock will be released automatically on process death
        MYLOGD("Wake lock acquired.\n");
    }

    register_sig_handler();

    // TODO(b/111441001): maybe skip if already started?
    if (options_->do_start_service) {
        MYLOGI("Starting 'dumpstate' service\n");
        android::status_t ret;
        if ((ret = android::os::DumpstateService::Start()) != android::OK) {
            MYLOGE("Unable to start DumpstateService: %d\n", ret);
        }
    }

    if (PropertiesHelper::IsDryRun()) {
        MYLOGI("Running on dry-run mode (to disable it, call 'setprop dumpstate.dry_run false')\n");
    }

    MYLOGI("dumpstate info: id=%d, args='%s', extra_options= %s)\n", id_, options_->args.c_str(),
           options_->extra_options.c_str());

    MYLOGI("bugreport format version: %s\n", version_.c_str());

    do_early_screenshot_ = options_->do_progress_updates;

    // If we are going to use a socket, do it as early as possible
    // to avoid timeouts from bugreport.
    if (options_->use_socket) {
        if (!redirect_to_socket(stdout, "dumpstate")) {
            return ERROR;
        }
    }

    if (options_->use_control_socket) {
        MYLOGD("Opening control socket\n");
        control_socket_fd_ = open_socket("dumpstate");
        if (control_socket_fd_ == -1) {
            return ERROR;
        }
        options_->do_progress_updates = 1;
    }

    if (is_redirecting) {
        PrepareToWriteToFile();

        if (options_->do_progress_updates) {
            if (options_->do_broadcast) {
                // clang-format off
                std::vector<std::string> am_args = {
                     "--receiver-permission", "android.permission.DUMP",
                     "--es", "android.intent.extra.NAME", name_,
                     "--ei", "android.intent.extra.ID", std::to_string(id_),
                     "--ei", "android.intent.extra.PID", std::to_string(pid_),
                     "--ei", "android.intent.extra.MAX", std::to_string(progress_->GetMax()),
                };
                // clang-format on
                SendBroadcast("com.android.internal.intent.action.BUGREPORT_STARTED", am_args);
            }
            if (options_->use_control_socket) {
                dprintf(control_socket_fd_, "BEGIN:%s\n", path_.c_str());
            }
        }
    }

    /* read /proc/cmdline before dropping root */
    FILE *cmdline = fopen("/proc/cmdline", "re");
    if (cmdline) {
        fgets(cmdline_buf, sizeof(cmdline_buf), cmdline);
        fclose(cmdline);
    }

    if (options_->do_vibrate) {
        Vibrate(150);
    }

    if (options_->do_fb && do_early_screenshot_) {
        MYLOGI("taking early screenshot\n");
        TakeScreenshot();
    }

    if (options_->do_zip_file && zip_file != nullptr) {
        if (chown(path_.c_str(), AID_SHELL, AID_SHELL)) {
            MYLOGE("Unable to change ownership of zip file %s: %s\n", path_.c_str(),
                   strerror(errno));
        }
    }

    int dup_stdout_fd;
    int dup_stderr_fd;
    if (is_redirecting) {
        // Redirect stderr to log_path_ for debugging.
        TEMP_FAILURE_RETRY(dup_stderr_fd = dup(fileno(stderr)));
        if (!redirect_to_file(stderr, const_cast<char*>(log_path_.c_str()))) {
            return ERROR;
        }
        if (chown(log_path_.c_str(), AID_SHELL, AID_SHELL)) {
            MYLOGE("Unable to change ownership of dumpstate log file %s: %s\n", log_path_.c_str(),
                   strerror(errno));
        }

        // Redirect stdout to tmp_path_. This is the main bugreport entry and will be
        // moved into zip file later, if zipping.
        TEMP_FAILURE_RETRY(dup_stdout_fd = dup(fileno(stdout)));
        // TODO: why not write to a file instead of stdout to overcome this problem?
        /* TODO: rather than generating a text file now and zipping it later,
           it would be more efficient to redirect stdout to the zip entry
           directly, but the libziparchive doesn't support that option yet. */
        if (!redirect_to_file(stdout, const_cast<char*>(tmp_path_.c_str()))) {
            return ERROR;
        }
        if (chown(tmp_path_.c_str(), AID_SHELL, AID_SHELL)) {
            MYLOGE("Unable to change ownership of temporary bugreport file %s: %s\n",
                   tmp_path_.c_str(), strerror(errno));
        }
    }

    // Don't buffer stdout
    setvbuf(stdout, nullptr, _IONBF, 0);

    // NOTE: there should be no stdout output until now, otherwise it would break the header.
    // In particular, DurationReport objects should be created passing 'title, NULL', so their
    // duration is logged into MYLOG instead.
    PrintHeader();

    if (options_->telephony_only) {
        DumpstateTelephonyOnly();
        DumpstateBoard();
    } else if (options_->wifi_only) {
        DumpstateWifiOnly();
    } else {
        // Dump state for the default case. This also drops root.
        RunStatus s = DumpstateDefault();
        if (s != RunStatus::OK) {
            if (s == RunStatus::USER_CONSENT_DENIED) {
                HandleUserConsentDenied();
            }
            return s;
        }
    }

    /* close output if needed */
    if (is_redirecting) {
        TEMP_FAILURE_RETRY(dup2(dup_stdout_fd, fileno(stdout)));
    }

    // Rename, and/or zip the (now complete) .tmp file within the internal directory.
    if (options_->OutputToFile()) {
        FinalizeFile();
    }

    // Share the final file with the caller if the user has consented.
    Dumpstate::RunStatus status = Dumpstate::RunStatus::OK;
    if (options_->bugreport_fd.get() != -1) {
        status = CopyBugreportIfUserConsented();
        if (status != Dumpstate::RunStatus::OK &&
            status != Dumpstate::RunStatus::USER_CONSENT_TIMED_OUT) {
            // Do an early return if there were errors. We make an exception for consent
            // timing out because it's possible the user got distracted. In this case the
            // bugreport is not shared but made available for manual retrieval.
            MYLOGI("User denied consent. Returning\n");
            return status;
        }
        if (options_->do_fb && options_->screenshot_fd.get() != -1) {
            bool copy_succeeded = android::os::CopyFileToFd(screenshot_path_,
                                                            options_->screenshot_fd.get());
            if (copy_succeeded) {
                android::os::UnlinkAndLogOnError(screenshot_path_);
            }
        }
        if (status == Dumpstate::RunStatus::USER_CONSENT_TIMED_OUT) {
            MYLOGI(
                "Did not receive user consent yet."
                " Will not copy the bugreport artifacts to caller.\n");
            const String16 incidentcompanion("incidentcompanion");
            sp<android::IBinder> ics(defaultServiceManager()->getService(incidentcompanion));
            if (ics != nullptr) {
                MYLOGD("Canceling user consent request via incidentcompanion service\n");
                android::interface_cast<android::os::IIncidentCompanion>(ics)->cancelAuthorization(
                        consent_callback_.get());
            } else {
                MYLOGD("Unable to cancel user consent; incidentcompanion service unavailable\n");
            }
        }
    }

    /* vibrate a few but shortly times to let user know it's finished */
    if (options_->do_vibrate) {
        for (int i = 0; i < 3; i++) {
            Vibrate(75);
            usleep((75 + 50) * 1000);
        }
    }

    /* tell activity manager we're done */
    if (options_->do_broadcast) {
        SendBugreportFinishedBroadcast();
        // Note that listener_ is notified in Run();
    }

    MYLOGD("Final progress: %d/%d (estimated %d)\n", progress_->Get(), progress_->GetMax(),
           progress_->GetInitialMax());
    progress_->Save();
    MYLOGI("done (id %d)\n", id_);

    if (is_redirecting) {
        TEMP_FAILURE_RETRY(dup2(dup_stderr_fd, fileno(stderr)));
    }

    if (options_->use_control_socket && control_socket_fd_ != -1) {
        MYLOGD("Closing control socket\n");
        close(control_socket_fd_);
    }

    tombstone_data_.clear();
    anr_data_.clear();

    return (consent_callback_ != nullptr &&
            consent_callback_->getResult() == UserConsentResult::UNAVAILABLE)
               ? USER_CONSENT_TIMED_OUT
               : RunStatus::OK;
}

void Dumpstate::CheckUserConsent(int32_t calling_uid, const android::String16& calling_package) {
    consent_callback_ = new ConsentCallback();
    const String16 incidentcompanion("incidentcompanion");
    sp<android::IBinder> ics(defaultServiceManager()->getService(incidentcompanion));
    if (ics != nullptr) {
        MYLOGD("Checking user consent via incidentcompanion service\n");
        android::interface_cast<android::os::IIncidentCompanion>(ics)->authorizeReport(
            calling_uid, calling_package, String16(), String16(),
            0x1 /* FLAG_CONFIRMATION_DIALOG */, consent_callback_.get());
    } else {
        MYLOGD("Unable to check user consent; incidentcompanion service unavailable\n");
    }
}

bool Dumpstate::IsUserConsentDenied() const {
    return ds.consent_callback_ != nullptr &&
           ds.consent_callback_->getResult() == UserConsentResult::DENIED;
}

void Dumpstate::CleanupFiles() {
    android::os::UnlinkAndLogOnError(tmp_path_);
    android::os::UnlinkAndLogOnError(screenshot_path_);
    android::os::UnlinkAndLogOnError(path_);
}

Dumpstate::RunStatus Dumpstate::HandleUserConsentDenied() {
    MYLOGD("User denied consent; deleting files and returning\n");
    CleanupFiles();
    return USER_CONSENT_DENIED;
}

Dumpstate::RunStatus Dumpstate::CopyBugreportIfUserConsented() {
    // If the caller has asked to copy the bugreport over to their directory, we need explicit
    // user consent.
    UserConsentResult consent_result = consent_callback_->getResult();
    if (consent_result == UserConsentResult::UNAVAILABLE) {
        // User has not responded yet.
        uint64_t elapsed_ms = consent_callback_->getElapsedTimeMs();
        if (elapsed_ms < USER_CONSENT_TIMEOUT_MS) {
            uint delay_seconds = (USER_CONSENT_TIMEOUT_MS - elapsed_ms) / 1000;
            MYLOGD("Did not receive user consent yet; going to wait for %d seconds", delay_seconds);
            sleep(delay_seconds);
        }
        consent_result = consent_callback_->getResult();
    }
    if (consent_result == UserConsentResult::DENIED) {
        // User has explicitly denied sharing with the app. To be safe delete the
        // internal bugreport & tmp files.
        return HandleUserConsentDenied();
    }
    if (consent_result == UserConsentResult::APPROVED) {
        bool copy_succeeded = android::os::CopyFileToFd(path_, options_->bugreport_fd.get());
        if (copy_succeeded) {
            android::os::UnlinkAndLogOnError(path_);
        }
        return copy_succeeded ? Dumpstate::RunStatus::OK : Dumpstate::RunStatus::ERROR;
    } else if (consent_result == UserConsentResult::UNAVAILABLE) {
        // consent_result is still UNAVAILABLE. The user has likely not responded yet.
        // Since we do not have user consent to share the bugreport it does not get
        // copied over to the calling app but remains in the internal directory from
        // where the user can manually pull it.
        return Dumpstate::RunStatus::USER_CONSENT_TIMED_OUT;
    }
    // Unknown result; must be a programming error.
    MYLOGE("Unknown user consent result:%d\n", consent_result);
    return Dumpstate::RunStatus::ERROR;
}

Dumpstate::RunStatus Dumpstate::ParseCommandlineAndRun(int argc, char* argv[]) {
    std::unique_ptr<Dumpstate::DumpOptions> options = std::make_unique<Dumpstate::DumpOptions>();
    Dumpstate::RunStatus status = options->Initialize(argc, argv);
    if (status == Dumpstate::RunStatus::OK) {
        SetOptions(std::move(options));
        // When directly running dumpstate binary, the output is not expected to be written
        // to any external file descriptor.
        assert(options_->bugreport_fd.get() == -1);

        // calling_uid and calling_package are for user consent to share the bugreport with
        // an app; they are irrelvant here because bugreport is only written to a local
        // directory, and not shared.
        status = Run(-1 /* calling_uid */, "" /* calling_package */);
    }
    return status;
}

/* Main entry point for dumpstate binary. */
int run_main(int argc, char* argv[]) {
    Dumpstate::RunStatus status = ds.ParseCommandlineAndRun(argc, argv);

    switch (status) {
        case Dumpstate::RunStatus::OK:
            exit(0);
        case Dumpstate::RunStatus::HELP:
            ShowUsage();
            exit(0);
        case Dumpstate::RunStatus::INVALID_INPUT:
            fprintf(stderr, "Invalid combination of args\n");
            ShowUsage();
            exit(1);
        case Dumpstate::RunStatus::ERROR:
            FALLTHROUGH_INTENDED;
        case Dumpstate::RunStatus::USER_CONSENT_DENIED:
            FALLTHROUGH_INTENDED;
        case Dumpstate::RunStatus::USER_CONSENT_TIMED_OUT:
            exit(2);
    }
}

// TODO(111441001): Default DumpOptions to sensible values.
Dumpstate::Dumpstate(const std::string& version)
    : pid_(getpid()),
      options_(new Dumpstate::DumpOptions()),
      last_reported_percent_progress_(0),
      version_(version),
      now_(time(nullptr)) {
}

Dumpstate& Dumpstate::GetInstance() {
    static Dumpstate singleton_(android::base::GetProperty("dumpstate.version", VERSION_CURRENT));
    return singleton_;
}

DurationReporter::DurationReporter(const std::string& title, bool logcat_only, bool verbose)
    : title_(title), logcat_only_(logcat_only), verbose_(verbose) {
    if (!title_.empty()) {
        started_ = Nanotime();
    }
}

DurationReporter::~DurationReporter() {
    if (!title_.empty()) {
        float elapsed = (float)(Nanotime() - started_) / NANOS_PER_SEC;
        if (elapsed >= .5f || verbose_) {
            MYLOGD("Duration of '%s': %.2fs\n", title_.c_str(), elapsed);
        }
        if (!logcat_only_) {
            // Use "Yoda grammar" to make it easier to grep|sort sections.
            printf("------ %.3fs was the duration of '%s' ------\n", elapsed, title_.c_str());
        }
    }
}

const int32_t Progress::kDefaultMax = 5000;

Progress::Progress(const std::string& path) : Progress(Progress::kDefaultMax, 1.1, path) {
}

Progress::Progress(int32_t initial_max, int32_t progress, float growth_factor)
    : Progress(initial_max, growth_factor, "") {
    progress_ = progress;
}

Progress::Progress(int32_t initial_max, float growth_factor, const std::string& path)
    : initial_max_(initial_max),
      progress_(0),
      max_(initial_max),
      growth_factor_(growth_factor),
      n_runs_(0),
      average_max_(0),
      path_(path) {
    if (!path_.empty()) {
        Load();
    }
}

void Progress::Load() {
    MYLOGD("Loading stats from %s\n", path_.c_str());
    std::string content;
    if (!android::base::ReadFileToString(path_, &content)) {
        MYLOGI("Could not read stats from %s; using max of %d\n", path_.c_str(), max_);
        return;
    }
    if (content.empty()) {
        MYLOGE("No stats (empty file) on %s; using max of %d\n", path_.c_str(), max_);
        return;
    }
    std::vector<std::string> lines = android::base::Split(content, "\n");

    if (lines.size() < 1) {
        MYLOGE("Invalid stats on file %s: not enough lines (%d). Using max of %d\n", path_.c_str(),
               (int)lines.size(), max_);
        return;
    }
    char* ptr;
    n_runs_ = strtol(lines[0].c_str(), &ptr, 10);
    average_max_ = strtol(ptr, nullptr, 10);
    if (n_runs_ <= 0 || average_max_ <= 0 || n_runs_ > STATS_MAX_N_RUNS ||
        average_max_ > STATS_MAX_AVERAGE) {
        MYLOGE("Invalid stats line on file %s: %s\n", path_.c_str(), lines[0].c_str());
        initial_max_ = Progress::kDefaultMax;
    } else {
        initial_max_ = average_max_;
    }
    max_ = initial_max_;

    MYLOGI("Average max progress: %d in %d runs; estimated max: %d\n", average_max_, n_runs_, max_);
}

void Progress::Save() {
    int32_t total = n_runs_ * average_max_ + progress_;
    int32_t runs = n_runs_ + 1;
    int32_t average = floor(((float)total) / runs);
    MYLOGI("Saving stats (total=%d, runs=%d, average=%d) on %s\n", total, runs, average,
           path_.c_str());
    if (path_.empty()) {
        return;
    }

    std::string content = android::base::StringPrintf("%d %d\n", runs, average);
    if (!android::base::WriteStringToFile(content, path_)) {
        MYLOGE("Could not save stats on %s\n", path_.c_str());
    }
}

int32_t Progress::Get() const {
    return progress_;
}

bool Progress::Inc(int32_t delta_sec) {
    bool changed = false;
    if (delta_sec >= 0) {
        progress_ += delta_sec;
        if (progress_ > max_) {
            int32_t old_max = max_;
            max_ = floor((float)progress_ * growth_factor_);
            MYLOGD("Adjusting max progress from %d to %d\n", old_max, max_);
            changed = true;
        }
    }
    return changed;
}

int32_t Progress::GetMax() const {
    return max_;
}

int32_t Progress::GetInitialMax() const {
    return initial_max_;
}

void Progress::Dump(int fd, const std::string& prefix) const {
    const char* pr = prefix.c_str();
    dprintf(fd, "%sprogress: %d\n", pr, progress_);
    dprintf(fd, "%smax: %d\n", pr, max_);
    dprintf(fd, "%sinitial_max: %d\n", pr, initial_max_);
    dprintf(fd, "%sgrowth_factor: %0.2f\n", pr, growth_factor_);
    dprintf(fd, "%spath: %s\n", pr, path_.c_str());
    dprintf(fd, "%sn_runs: %d\n", pr, n_runs_);
    dprintf(fd, "%saverage_max: %d\n", pr, average_max_);
}

bool Dumpstate::IsZipping() const {
    return zip_writer_ != nullptr;
}

std::string Dumpstate::GetPath(const std::string& suffix) const {
    return GetPath(bugreport_internal_dir_, suffix);
}

std::string Dumpstate::GetPath(const std::string& directory, const std::string& suffix) const {
    return android::base::StringPrintf("%s/%s-%s%s", directory.c_str(), base_name_.c_str(),
                                       name_.c_str(), suffix.c_str());
}

void Dumpstate::SetProgress(std::unique_ptr<Progress> progress) {
    progress_ = std::move(progress);
}

void for_each_userid(void (*func)(int), const char *header) {
    std::string title = header == nullptr ? "for_each_userid" : android::base::StringPrintf(
                                                                    "for_each_userid(%s)", header);
    DurationReporter duration_reporter(title);
    if (PropertiesHelper::IsDryRun()) return;

    DIR *d;
    struct dirent *de;

    if (header) printf("\n------ %s ------\n", header);
    func(0);

    if (!(d = opendir("/data/system/users"))) {
        printf("Failed to open /data/system/users (%s)\n", strerror(errno));
        return;
    }

    while ((de = readdir(d))) {
        int userid;
        if (de->d_type != DT_DIR || !(userid = atoi(de->d_name))) {
            continue;
        }
        func(userid);
    }

    closedir(d);
}

static void __for_each_pid(void (*helper)(int, const char *, void *), const char *header, void *arg) {
    DIR *d;
    struct dirent *de;

    if (!(d = opendir("/proc"))) {
        printf("Failed to open /proc (%s)\n", strerror(errno));
        return;
    }

    if (header) printf("\n------ %s ------\n", header);
    while ((de = readdir(d))) {
        if (ds.IsUserConsentDenied()) {
            MYLOGE(
                "Returning early because user denied consent to share bugreport with calling app.");
            closedir(d);
            return;
        }
        int pid;
        int fd;
        char cmdpath[255];
        char cmdline[255];

        if (!(pid = atoi(de->d_name))) {
            continue;
        }

        memset(cmdline, 0, sizeof(cmdline));

        snprintf(cmdpath, sizeof(cmdpath), "/proc/%d/cmdline", pid);
        if ((fd = TEMP_FAILURE_RETRY(open(cmdpath, O_RDONLY | O_CLOEXEC))) >= 0) {
            TEMP_FAILURE_RETRY(read(fd, cmdline, sizeof(cmdline) - 2));
            close(fd);
            if (cmdline[0]) {
                helper(pid, cmdline, arg);
                continue;
            }
        }

        // if no cmdline, a kernel thread has comm
        snprintf(cmdpath, sizeof(cmdpath), "/proc/%d/comm", pid);
        if ((fd = TEMP_FAILURE_RETRY(open(cmdpath, O_RDONLY | O_CLOEXEC))) >= 0) {
            TEMP_FAILURE_RETRY(read(fd, cmdline + 1, sizeof(cmdline) - 4));
            close(fd);
            if (cmdline[1]) {
                cmdline[0] = '[';
                size_t len = strcspn(cmdline, "\f\b\r\n");
                cmdline[len] = ']';
                cmdline[len+1] = '\0';
            }
        }
        if (!cmdline[0]) {
            strcpy(cmdline, "N/A");
        }
        helper(pid, cmdline, arg);
    }

    closedir(d);
}

static void for_each_pid_helper(int pid, const char *cmdline, void *arg) {
    for_each_pid_func *func = (for_each_pid_func*) arg;
    func(pid, cmdline);
}

void for_each_pid(for_each_pid_func func, const char *header) {
    std::string title = header == nullptr ? "for_each_pid"
                                          : android::base::StringPrintf("for_each_pid(%s)", header);
    DurationReporter duration_reporter(title);
    if (PropertiesHelper::IsDryRun()) return;

    __for_each_pid(for_each_pid_helper, header, (void *) func);
}

static void for_each_tid_helper(int pid, const char *cmdline, void *arg) {
    DIR *d;
    struct dirent *de;
    char taskpath[255];
    for_each_tid_func *func = (for_each_tid_func *) arg;

    snprintf(taskpath, sizeof(taskpath), "/proc/%d/task", pid);

    if (!(d = opendir(taskpath))) {
        printf("Failed to open %s (%s)\n", taskpath, strerror(errno));
        return;
    }

    func(pid, pid, cmdline);

    while ((de = readdir(d))) {
        if (ds.IsUserConsentDenied()) {
            MYLOGE(
                "Returning early because user denied consent to share bugreport with calling app.");
            closedir(d);
            return;
        }
        int tid;
        int fd;
        char commpath[255];
        char comm[255];

        if (!(tid = atoi(de->d_name))) {
            continue;
        }

        if (tid == pid)
            continue;

        snprintf(commpath, sizeof(commpath), "/proc/%d/comm", tid);
        memset(comm, 0, sizeof(comm));
        if ((fd = TEMP_FAILURE_RETRY(open(commpath, O_RDONLY | O_CLOEXEC))) < 0) {
            strcpy(comm, "N/A");
        } else {
            char *c;
            TEMP_FAILURE_RETRY(read(fd, comm, sizeof(comm) - 2));
            close(fd);

            c = strrchr(comm, '\n');
            if (c) {
                *c = '\0';
            }
        }
        func(pid, tid, comm);
    }

    closedir(d);
}

void for_each_tid(for_each_tid_func func, const char *header) {
    std::string title = header == nullptr ? "for_each_tid"
                                          : android::base::StringPrintf("for_each_tid(%s)", header);
    DurationReporter duration_reporter(title);

    if (PropertiesHelper::IsDryRun()) return;

    __for_each_pid(for_each_tid_helper, header, (void *) func);
}

void show_wchan(int pid, int tid, const char *name) {
    if (PropertiesHelper::IsDryRun()) return;

    char path[255];
    char buffer[255];
    int fd, ret, save_errno;
    char name_buffer[255];

    memset(buffer, 0, sizeof(buffer));

    snprintf(path, sizeof(path), "/proc/%d/wchan", tid);
    if ((fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC))) < 0) {
        printf("Failed to open '%s' (%s)\n", path, strerror(errno));
        return;
    }

    ret = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof(buffer)));
    save_errno = errno;
    close(fd);

    if (ret < 0) {
        printf("Failed to read '%s' (%s)\n", path, strerror(save_errno));
        return;
    }

    snprintf(name_buffer, sizeof(name_buffer), "%*s%s",
             pid == tid ? 0 : 3, "", name);

    printf("%-7d %-32s %s\n", tid, name_buffer, buffer);

    return;
}

// print time in centiseconds
static void snprcent(char *buffer, size_t len, size_t spc,
                     unsigned long long time) {
    static long hz; // cache discovered hz

    if (hz <= 0) {
        hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0) {
            hz = 1000;
        }
    }

    // convert to centiseconds
    time = (time * 100 + (hz / 2)) / hz;

    char str[16];

    snprintf(str, sizeof(str), " %llu.%02u",
             time / 100, (unsigned)(time % 100));
    size_t offset = strlen(buffer);
    snprintf(buffer + offset, (len > offset) ? len - offset : 0,
             "%*s", (spc > offset) ? (int)(spc - offset) : 0, str);
}

// print permille as a percent
static void snprdec(char *buffer, size_t len, size_t spc, unsigned permille) {
    char str[16];

    snprintf(str, sizeof(str), " %u.%u%%", permille / 10, permille % 10);
    size_t offset = strlen(buffer);
    snprintf(buffer + offset, (len > offset) ? len - offset : 0,
             "%*s", (spc > offset) ? (int)(spc - offset) : 0, str);
}

void show_showtime(int pid, const char *name) {
    if (PropertiesHelper::IsDryRun()) return;

    char path[255];
    char buffer[1023];
    int fd, ret, save_errno;

    memset(buffer, 0, sizeof(buffer));

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if ((fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC))) < 0) {
        printf("Failed to open '%s' (%s)\n", path, strerror(errno));
        return;
    }

    ret = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof(buffer)));
    save_errno = errno;
    close(fd);

    if (ret < 0) {
        printf("Failed to read '%s' (%s)\n", path, strerror(save_errno));
        return;
    }

    // field 14 is utime
    // field 15 is stime
    // field 42 is iotime
    unsigned long long utime = 0, stime = 0, iotime = 0;
    if (sscanf(buffer,
               "%*u %*s %*s %*d %*d %*d %*d %*d %*d %*d %*d "
               "%*d %*d %llu %llu %*d %*d %*d %*d %*d %*d "
               "%*d %*d %*d %*d %*d %*d %*d %*d %*d %*d "
               "%*d %*d %*d %*d %*d %*d %*d %*d %*d %llu ",
               &utime, &stime, &iotime) != 3) {
        return;
    }

    unsigned long long total = utime + stime;
    if (!total) {
        return;
    }

    unsigned permille = (iotime * 1000 + (total / 2)) / total;
    if (permille > 1000) {
        permille = 1000;
    }

    // try to beautify and stabilize columns at <80 characters
    snprintf(buffer, sizeof(buffer), "%-6d%s", pid, name);
    if ((name[0] != '[') || utime) {
        snprcent(buffer, sizeof(buffer), 57, utime);
    }
    snprcent(buffer, sizeof(buffer), 65, stime);
    if ((name[0] != '[') || iotime) {
        snprcent(buffer, sizeof(buffer), 73, iotime);
    }
    if (iotime) {
        snprdec(buffer, sizeof(buffer), 79, permille);
    }
    puts(buffer);  // adds a trailing newline

    return;
}

void do_dmesg() {
    const char *title = "KERNEL LOG (dmesg)";
    DurationReporter duration_reporter(title);
    printf("------ %s ------\n", title);

    if (PropertiesHelper::IsDryRun()) return;

    /* Get size of kernel buffer */
    int size = klogctl(KLOG_SIZE_BUFFER, nullptr, 0);
    if (size <= 0) {
        printf("Unexpected klogctl return value: %d\n\n", size);
        return;
    }
    char *buf = (char *) malloc(size + 1);
    if (buf == nullptr) {
        printf("memory allocation failed\n\n");
        return;
    }
    int retval = klogctl(KLOG_READ_ALL, buf, size);
    if (retval < 0) {
        printf("klogctl failure\n\n");
        free(buf);
        return;
    }
    buf[retval] = '\0';
    printf("%s\n\n", buf);
    free(buf);
    return;
}

void do_showmap(int pid, const char *name) {
    char title[255];
    char arg[255];

    snprintf(title, sizeof(title), "SHOW MAP %d (%s)", pid, name);
    snprintf(arg, sizeof(arg), "%d", pid);
    RunCommand(title, {"showmap", "-q", arg}, CommandOptions::AS_ROOT);
}

int Dumpstate::DumpFile(const std::string& title, const std::string& path) {
    DurationReporter duration_reporter(title);

    int status = DumpFileToFd(STDOUT_FILENO, title, path);

    UpdateProgress(WEIGHT_FILE);

    return status;
}

int read_file_as_long(const char *path, long int *output) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd < 0) {
        int err = errno;
        MYLOGE("Error opening file descriptor for %s: %s\n", path, strerror(err));
        return -1;
    }
    char buffer[50];
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof(buffer)));
    if (bytes_read == -1) {
        MYLOGE("Error reading file %s: %s\n", path, strerror(errno));
        return -2;
    }
    if (bytes_read == 0) {
        MYLOGE("File %s is empty\n", path);
        return -3;
    }
    *output = atoi(buffer);
    return 0;
}

/* calls skip to gate calling dump_from_fd recursively
 * in the specified directory. dump_from_fd defaults to
 * dump_file_from_fd above when set to NULL. skip defaults
 * to false when set to NULL. dump_from_fd will always be
 * called with title NULL.
 */
int dump_files(const std::string& title, const char* dir, bool (*skip)(const char* path),
               int (*dump_from_fd)(const char* title, const char* path, int fd)) {
    DurationReporter duration_reporter(title);
    DIR *dirp;
    struct dirent *d;
    char *newpath = nullptr;
    const char *slash = "/";
    int retval = 0;

    if (!title.empty()) {
        printf("------ %s (%s) ------\n", title.c_str(), dir);
    }
    if (PropertiesHelper::IsDryRun()) return 0;

    if (dir[strlen(dir) - 1] == '/') {
        ++slash;
    }
    dirp = opendir(dir);
    if (dirp == nullptr) {
        retval = -errno;
        MYLOGE("%s: %s\n", dir, strerror(errno));
        return retval;
    }

    if (!dump_from_fd) {
        dump_from_fd = dump_file_from_fd;
    }
    for (; ((d = readdir(dirp))); free(newpath), newpath = nullptr) {
        if ((d->d_name[0] == '.')
         && (((d->d_name[1] == '.') && (d->d_name[2] == '\0'))
          || (d->d_name[1] == '\0'))) {
            continue;
        }
        asprintf(&newpath, "%s%s%s%s", dir, slash, d->d_name,
                 (d->d_type == DT_DIR) ? "/" : "");
        if (!newpath) {
            retval = -errno;
            continue;
        }
        if (skip && (*skip)(newpath)) {
            continue;
        }
        if (d->d_type == DT_DIR) {
            int ret = dump_files("", newpath, skip, dump_from_fd);
            if (ret < 0) {
                retval = ret;
            }
            continue;
        }
        android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(newpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC)));
        if (fd.get() < 0) {
            retval = -1;
            printf("*** %s: %s\n", newpath, strerror(errno));
            continue;
        }
        (*dump_from_fd)(nullptr, newpath, fd.get());
    }
    closedir(dirp);
    if (!title.empty()) {
        printf("\n");
    }
    return retval;
}

/* fd must have been opened with the flag O_NONBLOCK. With this flag set,
 * it's possible to avoid issues where opening the file itself can get
 * stuck.
 */
int dump_file_from_fd(const char *title, const char *path, int fd) {
    if (PropertiesHelper::IsDryRun()) return 0;

    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        printf("*** %s: failed to get flags on fd %d: %s\n", path, fd, strerror(errno));
        return -1;
    } else if (!(flags & O_NONBLOCK)) {
        printf("*** %s: fd must have O_NONBLOCK set.\n", path);
        return -1;
    }
    return DumpFileFromFdToFd(title, path, fd, STDOUT_FILENO, PropertiesHelper::IsDryRun());
}

int Dumpstate::RunCommand(const std::string& title, const std::vector<std::string>& full_command,
                          const CommandOptions& options, bool verbose_duration) {
    DurationReporter duration_reporter(title, false /* logcat_only */, verbose_duration);

    int status = RunCommandToFd(STDOUT_FILENO, title, full_command, options);

    /* TODO: for now we're simplifying the progress calculation by using the
     * timeout as the weight. It's a good approximation for most cases, except when calling dumpsys,
     * where its weight should be much higher proportionally to its timeout.
     * Ideally, it should use a options.EstimatedDuration() instead...*/
    UpdateProgress(options.Timeout());

    return status;
}

void Dumpstate::RunDumpsys(const std::string& title, const std::vector<std::string>& dumpsys_args,
                           const CommandOptions& options, long dumpsysTimeoutMs) {
    long timeout_ms = dumpsysTimeoutMs > 0 ? dumpsysTimeoutMs : options.TimeoutInMs();
    std::vector<std::string> dumpsys = {"/system/bin/dumpsys", "-T", std::to_string(timeout_ms)};
    dumpsys.insert(dumpsys.end(), dumpsys_args.begin(), dumpsys_args.end());
    RunCommand(title, dumpsys, options);
}

int open_socket(const char *service) {
    int s = android_get_control_socket(service);
    if (s < 0) {
        MYLOGE("android_get_control_socket(%s): %s\n", service, strerror(errno));
        return -1;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);

    // Set backlog to 0 to make sure that queue size will be minimum.
    // In Linux, because the minimum queue will be 1, connect() will be blocked
    // if the other clients already called connect() and the connection request was not accepted.
    if (listen(s, 0) < 0) {
        MYLOGE("listen(control socket): %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr addr;
    socklen_t alen = sizeof(addr);
    int fd = accept4(s, &addr, &alen, SOCK_CLOEXEC);

    // Close socket just after accept(), to make sure that connect() by client will get error
    // when the socket is used by the other services.
    // There is still a race condition possibility between accept and close, but there is no way
    // to close-on-accept atomically.
    // See detail; b/123306389#comment25
    close(s);

    if (fd < 0) {
        MYLOGE("accept(control socket): %s\n", strerror(errno));
        return -1;
    }

    return fd;
}

/* redirect output to a service control socket */
bool redirect_to_socket(FILE* redirect, const char* service) {
    int fd = open_socket(service);
    if (fd == -1) {
        return false;
    }
    fflush(redirect);
    // TODO: handle dup2 failure
    TEMP_FAILURE_RETRY(dup2(fd, fileno(redirect)));
    close(fd);
    return true;
}

// TODO: should call is_valid_output_file and/or be merged into it.
void create_parent_dirs(const char *path) {
    char *chp = const_cast<char *> (path);

    /* skip initial slash */
    if (chp[0] == '/')
        chp++;

    /* create leading directories, if necessary */
    struct stat dir_stat;
    while (chp && chp[0]) {
        chp = strchr(chp, '/');
        if (chp) {
            *chp = 0;
            if (stat(path, &dir_stat) == -1 || !S_ISDIR(dir_stat.st_mode)) {
                MYLOGI("Creating directory %s\n", path);
                if (mkdir(path, 0770)) { /* drwxrwx--- */
                    MYLOGE("Unable to create directory %s: %s\n", path, strerror(errno));
                } else if (chown(path, AID_SHELL, AID_SHELL)) {
                    MYLOGE("Unable to change ownership of dir %s: %s\n", path, strerror(errno));
                }
            }
            *chp++ = '/';
        }
    }
}

bool _redirect_to_file(FILE* redirect, char* path, int truncate_flag) {
    create_parent_dirs(path);

    int fd = TEMP_FAILURE_RETRY(open(path,
                                     O_WRONLY | O_CREAT | truncate_flag | O_CLOEXEC | O_NOFOLLOW,
                                     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
    if (fd < 0) {
        MYLOGE("%s: %s\n", path, strerror(errno));
        return false;
    }

    TEMP_FAILURE_RETRY(dup2(fd, fileno(redirect)));
    close(fd);
    return true;
}

bool redirect_to_file(FILE* redirect, char* path) {
    return _redirect_to_file(redirect, path, O_TRUNC);
}

bool redirect_to_existing_file(FILE* redirect, char* path) {
    return _redirect_to_file(redirect, path, O_APPEND);
}

void dump_route_tables() {
    DurationReporter duration_reporter("DUMP ROUTE TABLES");
    if (PropertiesHelper::IsDryRun()) return;
    const char* const RT_TABLES_PATH = "/data/misc/net/rt_tables";
    ds.DumpFile("RT_TABLES", RT_TABLES_PATH);
    FILE* fp = fopen(RT_TABLES_PATH, "re");
    if (!fp) {
        printf("*** %s: %s\n", RT_TABLES_PATH, strerror(errno));
        return;
    }
    char table[16];
    // Each line has an integer (the table number), a space, and a string (the table name). We only
    // need the table number. It's a 32-bit unsigned number, so max 10 chars. Skip the table name.
    // Add a fixed max limit so this doesn't go awry.
    for (int i = 0; i < 64 && fscanf(fp, " %10s %*s", table) == 1; ++i) {
        RunCommand("ROUTE TABLE IPv4", {"ip", "-4", "route", "show", "table", table});
        RunCommand("ROUTE TABLE IPv6", {"ip", "-6", "route", "show", "table", table});
    }
    fclose(fp);
}

// TODO: make this function thread safe if sections are generated in parallel.
void Dumpstate::UpdateProgress(int32_t delta_sec) {
    if (progress_ == nullptr) {
        MYLOGE("UpdateProgress: progress_ not set\n");
        return;
    }

    // Always update progess so stats can be tuned...
    progress_->Inc(delta_sec);

    // ...but only notifiy listeners when necessary.
    if (!options_->do_progress_updates) return;

    int progress = progress_->Get();
    int max = progress_->GetMax();
    int percent = 100 * progress / max;

    if (last_reported_percent_progress_ > 0 && percent <= last_reported_percent_progress_) {
        return;
    }
    last_reported_percent_progress_ = percent;

    if (control_socket_fd_ >= 0) {
        dprintf(control_socket_fd_, "PROGRESS:%d/%d\n", progress, max);
        fsync(control_socket_fd_);
    }

    if (listener_ != nullptr) {
        if (percent % 5 == 0) {
            // We don't want to spam logcat, so only log multiples of 5.
            MYLOGD("Setting progress (%s): %d/%d (%d%%)\n", listener_name_.c_str(), progress, max,
                   percent);
        } else {
            // stderr is ignored on normal invocations, but useful when calling
            // /system/bin/dumpstate directly for debuggging.
            fprintf(stderr, "Setting progress (%s): %d/%d (%d%%)\n", listener_name_.c_str(),
                    progress, max, percent);
        }

        listener_->onProgress(percent);
    }
}

void Dumpstate::TakeScreenshot(const std::string& path) {
    const std::string& real_path = path.empty() ? screenshot_path_ : path;
    int status =
        RunCommand("", {"/system/bin/screencap", "-p", real_path},
                   CommandOptions::WithTimeout(10).Always().DropRoot().RedirectStderr().Build());
    if (status == 0) {
        MYLOGD("Screenshot saved on %s\n", real_path.c_str());
    } else {
        MYLOGE("Failed to take screenshot on %s\n", real_path.c_str());
    }
}

bool is_dir(const char* pathname) {
    struct stat info;
    if (stat(pathname, &info) == -1) {
        return false;
    }
    return S_ISDIR(info.st_mode);
}

time_t get_mtime(int fd, time_t default_mtime) {
    struct stat info;
    if (fstat(fd, &info) == -1) {
        return default_mtime;
    }
    return info.st_mtime;
}
