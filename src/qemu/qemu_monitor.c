/*
 * qemu_monitor.c: interaction with QEMU monitor console
 *
 * Copyright (C) 2006-2015 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <gio/gio.h>

#include "qemu_alias.h"
#include "qemu_monitor.h"
#include "qemu_monitor_json.h"
#include "qemu_domain.h"
#include "qemu_capabilities.h"
#include "virerror.h"
#include "viralloc.h"
#include "virlog.h"
#include "virfile.h"
#include "virprocess.h"
#include "virobject.h"
#include "virprobe.h"
#include "virstring.h"
#include "virtime.h"
#include "virutil.h"

#ifdef WITH_DTRACE_PROBES
# include "libvirt_qemu_probes.h"
#endif

#define LIBVIRT_QEMU_MONITOR_PRIV_H_ALLOW
#include "qemu_monitor_priv.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_monitor");

/* We read from QEMU until seeing a \r\n pair to indicate a
 * completed reply or event. To avoid memory denial-of-service
 * though, we must have a size limit on amount of data we
 * buffer. 10 MB is large enough that it ought to cope with
 * normal QEMU replies, and small enough that we're not
 * consuming unreasonable mem.
 */
#define QEMU_MONITOR_MAX_RESPONSE (10 * 1024 * 1024)


/**
 * QEMU_CHECK_MONITOR_FULL:
 * @mon: monitor pointer variable to check, evaluated multiple times, no parentheses
 * @exit: statement that is used to exit the function
 *
 * This macro checks that the monitor is valid for given operation and exits
 * the function if not. The macro also adds a debug statement regarding the
 * monitor.
 */
#define QEMU_CHECK_MONITOR_FULL(mon, exit) \
    do { \
        if (!mon) { \
            virReportError(VIR_ERR_INVALID_ARG, "%s", \
                           _("monitor must not be NULL")); \
            exit; \
        } \
        VIR_DEBUG("mon:%p vm:%p monfd:%d", mon, mon->vm, mon->fd); \
    } while (0)

/* Check monitor and return NULL on error */
#define QEMU_CHECK_MONITOR_NULL(mon) \
    QEMU_CHECK_MONITOR_FULL(mon, return NULL)

/* Check monitor and return -1 on error */
#define QEMU_CHECK_MONITOR(mon) \
    QEMU_CHECK_MONITOR_FULL(mon, return -1)

/* Check monitor and jump to the provided label */
#define QEMU_CHECK_MONITOR_GOTO(mon, label) \
    QEMU_CHECK_MONITOR_FULL(mon, goto label)

static virClass *qemuMonitorClass;
static __thread bool qemuMonitorDisposed;
static void qemuMonitorDispose(void *obj);

static int qemuMonitorOnceInit(void)
{
    if (!VIR_CLASS_NEW(qemuMonitor, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(qemuMonitor);

VIR_ENUM_IMPL(qemuMonitorJob,
              QEMU_MONITOR_JOB_TYPE_LAST,
              "",
              "commit",
              "stream",
              "mirror",
              "backup",
              "create",
);

VIR_ENUM_IMPL(qemuMonitorJobStatus,
              QEMU_MONITOR_JOB_STATUS_LAST,
              "",
              "created",
              "running",
              "paused",
              "ready",
              "standby",
              "waiting",
              "pending",
              "aborting",
              "concluded",
              "undefined",
              "null",
);

VIR_ENUM_IMPL(qemuMonitorCPUProperty,
              QEMU_MONITOR_CPU_PROPERTY_LAST,
              "boolean",
              "string",
              "number",
);

VIR_ENUM_IMPL(qemuMonitorMigrationStatus,
              QEMU_MONITOR_MIGRATION_STATUS_LAST,
              "inactive",
              "setup",
              "active",
              "pre-switchover",
              "device",
              "postcopy-active",
              "postcopy-paused",
              "postcopy-recover",
              "postcopy-recover-setup",
              "completed",
              "failed",
              "cancelling",
              "cancelled",
              "wait-unplug",
);

VIR_ENUM_IMPL(qemuMonitorVMStatus,
              QEMU_MONITOR_VM_STATUS_LAST,
              "debug",
              "inmigrate",
              "internal-error",
              "io-error",
              "paused",
              "postmigrate",
              "prelaunch",
              "finish-migrate",
              "restore-vm",
              "running",
              "save-vm",
              "shutdown",
              "watchdog",
              "guest-panicked",
);

typedef enum {
    QEMU_MONITOR_BLOCK_IO_STATUS_OK,
    QEMU_MONITOR_BLOCK_IO_STATUS_FAILED,
    QEMU_MONITOR_BLOCK_IO_STATUS_NOSPACE,

    QEMU_MONITOR_BLOCK_IO_STATUS_LAST
} qemuMonitorBlockIOStatus;

VIR_ENUM_DECL(qemuMonitorBlockIOStatus);

VIR_ENUM_IMPL(qemuMonitorBlockIOStatus,
              QEMU_MONITOR_BLOCK_IO_STATUS_LAST,
              "ok", "failed", "nospace",
);

VIR_ENUM_IMPL(qemuMonitorDumpStatus,
              QEMU_MONITOR_DUMP_STATUS_LAST,
              "none", "active", "completed", "failed",
);

VIR_ENUM_IMPL(qemuMonitorMemoryFailureRecipient,
              QEMU_MONITOR_MEMORY_FAILURE_RECIPIENT_LAST,
              "hypervisor", "guest");

VIR_ENUM_IMPL(qemuMonitorMemoryFailureAction,
              QEMU_MONITOR_MEMORY_FAILURE_ACTION_LAST,
              "ignore", "inject",
              "fatal", "reset");

static void
qemuMonitorDispose(void *obj)
{
    qemuMonitor *mon = obj;

    VIR_DEBUG("mon=%p", mon);
    qemuMonitorDisposed = true;
    virObjectUnref(mon->vm);

    g_main_context_unref(mon->context);
    virResetError(&mon->lastError);
    virCondDestroy(&mon->notify);
    g_free(mon->buffer);
    g_free(mon->balloonpath);
    g_free(mon->domainName);
}


static int
qemuMonitorOpenUnix(const char *monitor)
{
    struct sockaddr_un addr = { 0 };
    VIR_AUTOCLOSE monfd = -1;
    int ret = -1;

    if ((monfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno,
                             "%s", _("failed to create socket"));
        return -1;
    }

    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, monitor) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Monitor path %1$s too big for destination"), monitor);
        return -1;
    }

    if (connect(monfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to connect to monitor socket"));
        return -1;
    }

    ret = monfd;
    monfd = -1;

    return ret;
}


/* This method processes data that has been received
 * from the monitor. Looking for async events and
 * replies/errors.
 */
static int
qemuMonitorIOProcess(qemuMonitor *mon)
{
    int len;
    qemuMonitorMessage *msg = NULL;

    /* See if there's a message & whether its ready for its reply
     * ie whether its completed writing all its data */
    if (mon->msg && mon->msg->txOffset == mon->msg->txLength)
        msg = mon->msg;


    PROBE_QUIET(QEMU_MONITOR_IO_PROCESS, "mon=%p buf=%s len=%zu",
                mon, mon->buffer, mon->bufferOffset);

    len = qemuMonitorJSONIOProcess(mon,
                                   mon->buffer, mon->bufferOffset,
                                   msg);
    if (len < 0)
        return -1;

    if (len && mon->waitGreeting)
        mon->waitGreeting = false;

    if (len < mon->bufferOffset) {
        memmove(mon->buffer, mon->buffer + len, mon->bufferOffset - len);
        mon->bufferOffset -= len;
    } else {
        VIR_FREE(mon->buffer);
        mon->bufferOffset = mon->bufferLength = 0;
    }
    /* As the monitor mutex was unlocked in qemuMonitorJSONIOProcess()
     * while dealing with qemu event, mon->msg could be changed which
     * means the above 'msg' may be invalid, thus we use 'mon->msg' here */
    if (mon->msg && mon->msg->finished)
        virCondBroadcast(&mon->notify);
    return len;
}


/* Call this function while holding the monitor lock. */
int
qemuMonitorIOWriteWithFD(qemuMonitor *mon,
                         const char *data,
                         size_t len,
                         int fd)
{
    int fds[] = { fd };

    return virSocketSendMsgWithFDs(mon->fd, data, len, fds, G_N_ELEMENTS(fds));
}


/*
 * Called when the monitor is able to write data
 * Call this function while holding the monitor lock.
 */
static int
qemuMonitorIOWrite(qemuMonitor *mon)
{
    int done;
    const char *buf;
    size_t len;

    /* If no active message, or fully transmitted, the no-op */
    if (!mon->msg || mon->msg->txOffset == mon->msg->txLength)
        return 0;

    buf = mon->msg->txBuffer + mon->msg->txOffset;
    len = mon->msg->txLength - mon->msg->txOffset;
    if (mon->msg->txFD == -1)
        done = write(mon->fd, buf, len); /* sc_avoid_write */
    else
        done = qemuMonitorIOWriteWithFD(mon, buf, len, mon->msg->txFD);

    PROBE(QEMU_MONITOR_IO_WRITE,
          "mon=%p buf=%s len=%zu ret=%d errno=%d",
          mon, buf, len, done, done < 0 ? errno : 0);

    if (mon->msg->txFD != -1) {
        PROBE(QEMU_MONITOR_IO_SEND_FD,
              "mon=%p fd=%d ret=%d errno=%d",
              mon, mon->msg->txFD, done, done < 0 ? errno : 0);
    }

    if (done < 0) {
        if (errno == EAGAIN)
            return 0;

        virReportSystemError(errno, "%s",
                             _("Unable to write to monitor"));
        return -1;
    }
    mon->msg->txOffset += done;
    return done;
}


/*
 * Called when the monitor has incoming data to read
 * Call this function while holding the monitor lock.
 *
 * Returns -1 on error, or number of bytes read
 */
static int
qemuMonitorIORead(qemuMonitor *mon)
{
    size_t avail = mon->bufferLength - mon->bufferOffset;
    int ret = 0;

    if (avail < 1024) {
        if (mon->bufferLength >= QEMU_MONITOR_MAX_RESPONSE) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("QEMU monitor reply exceeds buffer size (%1$d bytes)"),
                           QEMU_MONITOR_MAX_RESPONSE);
            return -1;
        }
        VIR_REALLOC_N(mon->buffer, mon->bufferLength + 1024);
        mon->bufferLength += 1024;
        avail += 1024;
    }

    /* Read as much as we can get into our buffer,
       until we block on EAGAIN, or hit EOF */
    while (avail > 1) {
        int got;
        got = read(mon->fd,
                   mon->buffer + mon->bufferOffset,
                   avail - 1);
        if (got < 0) {
            if (errno == EAGAIN)
                break;
            virReportSystemError(errno, "%s",
                                 _("Unable to read from monitor"));
            ret = -1;
            break;
        }
        if (got == 0)
            break;

        ret += got;
        avail -= got;
        mon->bufferOffset += got;
        mon->buffer[mon->bufferOffset] = '\0';
    }

    return ret;
}


static void
qemuMonitorUpdateWatch(qemuMonitor *mon)
{
    qemuMonitorUnregister(mon);
    if (mon->socket)
        qemuMonitorRegister(mon);
}


static gboolean
qemuMonitorIO(GSocket *socket G_GNUC_UNUSED,
              GIOCondition cond,
              gpointer opaque)
{
    qemuMonitor *mon = opaque;
    bool error = false;
    bool hangup = false;

    virObjectRef(mon);

    /* lock access to the monitor and protect fd */
    virObjectLock(mon);
    if (mon->fd == -1 || !mon->watch) {
        virObjectUnlock(mon);
        virObjectUnref(mon);
        return G_SOURCE_REMOVE;
    }

    if (mon->lastError.code != VIR_ERR_OK) {
        if (cond & (G_IO_HUP | G_IO_ERR))
            mon->goteof = true;
        error = true;
    } else {
        if (cond & G_IO_OUT) {
            if (qemuMonitorIOWrite(mon) < 0) {
                error = true;
                if (errno == ECONNRESET)
                    hangup = true;
            }
        }

        if (!error && cond & G_IO_IN) {
            int got = qemuMonitorIORead(mon);
            if (got < 0) {
                error = true;
                if (errno == ECONNRESET)
                    hangup = true;
            } else if (got == 0) {
                mon->goteof = true;
            } else {
                /* Ignore hangup/error cond if we read some data, to
                 * give time for that data to be consumed */
                cond = 0;

                if (qemuMonitorIOProcess(mon) < 0)
                    error = true;
            }
        }

        if (cond & G_IO_HUP) {
            hangup = true;
            if (!error)
                mon->goteof = true;
        }

        if (!error && !mon->goteof &&
            cond & G_IO_ERR) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid file descriptor while waiting for monitor (vm='%1$s')"), mon->domainName);
            mon->goteof = true;
        }
    }

    if (error || mon->goteof) {
        if (hangup && mon->logFunc != NULL) {
            g_autofree char *errmsg = NULL;

            /* Check if an error message from qemu is available and if so, use
             * it to overwrite the actual message. It's done only in early
             * startup phases or during incoming migration when the message
             * from qemu is certainly more interesting than a
             * "connection reset by peer" message.
             */

            errmsg = g_strdup_printf(_("QEMU unexpectedly closed the monitor (vm='%1$s')"),
                                     mon->domainName);
            mon->logFunc(mon, errmsg, mon->logOpaque);
            virCopyLastError(&mon->lastError);
            virResetLastError();
        }

        if (mon->lastError.code != VIR_ERR_OK) {
            /* Already have an error, so clear any new error */
            virResetLastError();
        } else {
            if (virGetLastErrorCode() == VIR_ERR_OK && !mon->goteof)
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Error while processing monitor IO (vm='%1$s')"), mon->domainName);
            virCopyLastError(&mon->lastError);
            virResetLastError();
        }

        VIR_DEBUG("Error on monitor %s mon=%p vm=%p name=%s",
                  NULLSTR(mon->lastError.message), mon, mon->vm, mon->domainName);
        /* If IO process resulted in an error & we have a message,
         * then wakeup that waiter */
        if (mon->msg && !mon->msg->finished) {
            mon->msg->finished = true;
            virCondSignal(&mon->notify);
        }
    }

    qemuMonitorUpdateWatch(mon);

    /* We have to unlock to avoid deadlock against command thread,
     * but is this safe ?  I think it is, because the callback
     * will try to acquire the virDomainObj *mutex next */
    if (mon->goteof) {
        qemuMonitorEofNotifyCallback eofNotify = mon->cb->eofNotify;
        virDomainObj *vm = mon->vm;

        /* Make sure anyone waiting wakes up now */
        virCondSignal(&mon->notify);
        virObjectUnlock(mon);
        VIR_DEBUG("Triggering EOF callback mon=%p vm=%p name=%s",
                  mon, mon->vm, mon->domainName);
        (eofNotify)(mon, vm);
        virObjectUnref(mon);
    } else if (error) {
        qemuMonitorErrorNotifyCallback errorNotify = mon->cb->errorNotify;
        virDomainObj *vm = mon->vm;

        /* Make sure anyone waiting wakes up now */
        virCondSignal(&mon->notify);
        virObjectUnlock(mon);
        VIR_DEBUG("Triggering error callback mon=%p vm=%p name=%s",
                  mon, mon->vm, mon->domainName);
        (errorNotify)(mon, vm);
        virObjectUnref(mon);
    } else {
        virObjectUnlock(mon);
        virObjectUnref(mon);
    }

    return G_SOURCE_REMOVE;
}


static qemuMonitor *
qemuMonitorOpenInternal(virDomainObj *vm,
                        int fd,
                        GMainContext *context,
                        qemuMonitorCallbacks *cb)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    qemuMonitor *mon;
    g_autoptr(GError) gerr = NULL;

    if (!cb->eofNotify) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("EOF notify callback must be supplied"));
        return NULL;
    }
    if (!cb->errorNotify) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Error notify callback must be supplied"));
        return NULL;
    }

    if (qemuMonitorInitialize() < 0)
        return NULL;

    if (!(mon = virObjectLockableNew(qemuMonitorClass)))
        return NULL;

    if (virCondInit(&mon->notify) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot initialize monitor condition"));
        goto cleanup;
    }
    mon->fd = fd;
    mon->context = g_main_context_ref(context);
    mon->vm = virObjectRef(vm);
    mon->domainName = g_strdup(NULLSTR(vm->def->name));
    mon->waitGreeting = true;
    mon->cb = cb;

    if (priv) {
        mon->blockjobMaskProtocol = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKJOB_BACKING_MASK_PROTOCOL);
    }

    if (virSetCloseExec(mon->fd) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Unable to set monitor close-on-exec flag"));
        goto cleanup;
    }

    mon->socket = g_socket_new_from_fd(fd, &gerr);
    if (!mon->socket) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unable to create socket object: %1$s"),
                       gerr->message);
        goto cleanup;
    }

    virObjectLock(mon);
    qemuMonitorRegister(mon);

    PROBE(QEMU_MONITOR_NEW, "mon=%p fd=%d", mon, mon->fd);
    virObjectUnlock(mon);

    return mon;

 cleanup:
    /* We don't want the 'destroy' callback invoked during
     * cleanup from construction failure, because that can
     * give a double-unref on virDomainObj *in the caller,
     * so kill the callbacks now.
     */
    mon->cb = NULL;
    /* The caller owns 'fd' on failure */
    mon->fd = -1;
    qemuMonitorClose(mon);
    return NULL;
}


/**
 * qemuMonitorOpen:
 * @vm: domain object
 * @config: monitor configuration
 * @cb: monitor event handles
 *
 * Opens the monitor for running qemu.
 *
 * Returns monitor object, NULL on error.
 */
qemuMonitor *
qemuMonitorOpen(virDomainObj *vm,
                virDomainChrSourceDef *config,
                GMainContext *context,
                qemuMonitorCallbacks *cb)
{
    VIR_AUTOCLOSE fd = -1;
    qemuMonitor *ret = NULL;

    if (config->type != VIR_DOMAIN_CHR_TYPE_UNIX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to handle monitor type: %1$s"),
                       virDomainChrTypeToString(config->type));
        return NULL;
    }

    virObjectUnlock(vm);
    fd = qemuMonitorOpenUnix(config->data.nix.path);
    virObjectLock(vm);

    if (fd < 0)
        return NULL;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("domain is not running"));
        return NULL;
    }

    ret = qemuMonitorOpenInternal(vm, fd, context, cb);
    fd = -1;
    return ret;
}


void qemuMonitorWatchDispose(void)
{
    qemuMonitorDisposed = false;
}


bool qemuMonitorWasDisposed(void)
{
    return qemuMonitorDisposed;
}


/**
 * qemuMonitorRegister:
 * @mon: QEMU monitor
 *
 * Registers the monitor in the event loop. The caller has to hold the
 * lock for @mon.
 */
void
qemuMonitorRegister(qemuMonitor *mon)
{
    GIOCondition cond = 0;

    if (mon->lastError.code == VIR_ERR_OK) {
        cond |= G_IO_IN;

        if ((mon->msg && mon->msg->txOffset < mon->msg->txLength) &&
            !mon->waitGreeting)
            cond |= G_IO_OUT;
    }

    mon->watch = g_socket_create_source(mon->socket,
                                        cond,
                                        NULL);

    virObjectRef(mon);
    g_source_set_callback(mon->watch,
                          (GSourceFunc)qemuMonitorIO,
                          mon,
                          (GDestroyNotify)virObjectUnref);

    g_source_attach(mon->watch,
                    mon->context);
}


/**
 * qemuMonitorUnregister:
 * @mon: monitor object
 *
 * Unregister monitor from the event loop. The monitor object
 * must be locked before calling this function.
 */
void
qemuMonitorUnregister(qemuMonitor *mon)
{
    if (mon->watch) {
        g_source_destroy(mon->watch);
        g_source_unref(mon->watch);
        mon->watch = NULL;
    }
}

void
qemuMonitorClose(qemuMonitor *mon)
{
    if (!mon)
        return;

    virObjectLock(mon);
    PROBE(QEMU_MONITOR_CLOSE, "mon=%p", mon);

    qemuMonitorSetDomainLogLocked(mon, NULL, NULL, NULL);

    if (mon->socket) {
        qemuMonitorUnregister(mon);
        g_clear_pointer(&mon->socket, g_object_unref);
        mon->fd = -1;
    }

    /* In case another thread is waiting for its monitor command to be
     * processed, we need to wake it up with appropriate error set.
     */
    if (mon->msg) {
        if (mon->lastError.code == VIR_ERR_OK) {
            virErrorPtr err;

            virErrorPreserveLast(&err);

            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("QEMU monitor was closed"));
            virCopyLastError(&mon->lastError);
            if (err)
                virErrorRestore(&err);
            else
                virResetLastError();
        }
        mon->msg->finished = true;
        virCondSignal(&mon->notify);
    }

    /* Propagate existing monitor error in case the current thread has no
     * error set.
     */
    if (mon->lastError.code != VIR_ERR_OK && virGetLastErrorCode() == VIR_ERR_OK)
        virSetError(&mon->lastError);

    virObjectUnlock(mon);
    virObjectUnref(mon);
}


char *
qemuMonitorNextCommandID(qemuMonitor *mon)
{
    return g_strdup_printf("libvirt-%d", ++mon->nextSerial);
}


/* for use only in the test suite */
void
qemuMonitorResetCommandID(qemuMonitor *mon)
{
    mon->nextSerial = 0;
}


int
qemuMonitorSend(qemuMonitor *mon,
                qemuMonitorMessage *msg)
{
    int ret = -1;

    /* Check whether qemu quit unexpectedly */
    if (mon->lastError.code != VIR_ERR_OK) {
        VIR_DEBUG("Attempt to send command while error is set %s mon=%p vm=%p name=%s",
                  NULLSTR(mon->lastError.message), mon, mon->vm, mon->domainName);
        virSetError(&mon->lastError);
        return -1;
    }
    if (mon->goteof) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("End of file from qemu monitor (vm='%1$s')"), mon->domainName);
        return -1;
    }

    mon->msg = msg;
    qemuMonitorUpdateWatch(mon);

    PROBE(QEMU_MONITOR_SEND_MSG,
          "mon=%p msg=%s fd=%d",
          mon, mon->msg->txBuffer, mon->msg->txFD);

    while (!mon->msg->finished) {
        if (virCondWait(&mon->notify, &mon->parent.lock) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unable to wait on monitor condition (vm='%1$s')"), mon->domainName);
            goto cleanup;
        }
    }

    if (mon->lastError.code != VIR_ERR_OK) {
        VIR_DEBUG("Send command resulted in error %s mon=%p vm=%p name=%s",
                  NULLSTR(mon->lastError.message), mon, mon->vm, mon->domainName);
        virSetError(&mon->lastError);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    mon->msg = NULL;
    qemuMonitorUpdateWatch(mon);

    return ret;
}


/**
 * This function returns a new virError object; the caller is responsible
 * for freeing it.
 */
virErrorPtr
qemuMonitorLastError(qemuMonitor *mon)
{
    if (mon->lastError.code == VIR_ERR_OK)
        return NULL;

    return virErrorCopyNew(&mon->lastError);
}


/**
 * Search the qom objects for the balloon driver object by its known names
 * of "virtio-balloon-pci" or "virtio-balloon-ccw". The entry for the driver
 * will be found by using function "qemuMonitorJSONFindLinkPath".
 *
 * Once found, check the entry to ensure it has the correct property listed.
 * If it does not, then obtaining statistics from QEMU will not be possible.
 * This feature was added to QEMU 1.5.
 */
static void
qemuMonitorInitBalloonObjectPath(qemuMonitor *mon,
                                 virDomainMemballoonDef *balloon)
{
    ssize_t i, nprops = 0;
    char *path = NULL;
    const char *name;
    qemuMonitorJSONListPath **bprops = NULL;

    if (mon->balloonpath) {
        return;
    } else if (mon->ballooninit) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot determine balloon device path"));
        return;
    }
    mon->ballooninit = true;

    switch (balloon->info.type) {
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_PCI:
        switch (balloon->model) {
            case VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO:
                name = "virtio-balloon-pci";
                break;
            case VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_TRANSITIONAL:
                name = "virtio-balloon-pci-transitional";
                break;
            case VIR_DOMAIN_MEMBALLOON_MODEL_VIRTIO_NON_TRANSITIONAL:
                name = "virtio-balloon-pci-non-transitional";
                break;
            case VIR_DOMAIN_MEMBALLOON_MODEL_XEN:
            case VIR_DOMAIN_MEMBALLOON_MODEL_NONE:
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("invalid model for virtio-balloon-pci"));
                return;
            case VIR_DOMAIN_MEMBALLOON_MODEL_LAST:
            default:
                virReportEnumRangeError(virDomainMemballoonModel,
                                        balloon->model);
                return;
        }
        break;
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCW:
        name = "virtio-balloon-ccw";
        break;
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DRIVE:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_SERIAL:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_CCID:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_USB:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_SPAPRVIO:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_S390:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_VIRTIO_MMIO:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_ISA:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_DIMM:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_UNASSIGNED:
    case VIR_DOMAIN_DEVICE_ADDRESS_TYPE_LAST:
    default:
        return;
    }

    if (qemuMonitorJSONFindLinkPath(mon, name, balloon->info.alias, &path) < 0)
        return;

    nprops = qemuMonitorJSONGetObjectListPaths(mon, path, &bprops);
    if (nprops < 0)
        goto cleanup;

    for (i = 0; i < nprops; i++) {
        if (STREQ(bprops[i]->name, "guest-stats-polling-interval")) {
            VIR_DEBUG("Found Balloon Object Path %s", path);
            mon->balloonpath = g_steal_pointer(&path);
            goto cleanup;
        }
    }


    /* If we get here, we found the path, but not the property */
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Property 'guest-stats-polling-interval' not found on memory balloon driver."));

 cleanup:
    for (i = 0; i < nprops; i++)
        qemuMonitorJSONListPathFree(bprops[i]);
    VIR_FREE(bprops);
    VIR_FREE(path);
    return;
}


/**
 * To update video memory size in status XML we need to load correct values from
 * QEMU.
 *
 * Returns 0 on success, -1 on failure and sets proper error message.
 */
int
qemuMonitorUpdateVideoMemorySize(qemuMonitor *mon,
                                 virDomainVideoDef *video,
                                 const char *videoName)
{
    int rc = -1;
    g_autofree char *path = NULL;

    QEMU_CHECK_MONITOR(mon);

    rc = qemuMonitorJSONFindLinkPath(mon, videoName,
                                     video->info.alias, &path);
    if (rc < 0) {
        if (rc == -2)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to find QOM Object path for device '%1$s'"),
                           videoName);
        return -1;
    }

    return qemuMonitorJSONUpdateVideoMemorySize(mon, video, path);
}


/**
 * To update video vram64 size in status XML we need to load correct value from
 * QEMU.
 *
 * Returns 0 on success, -1 on failure and sets proper error message.
 */
int
qemuMonitorUpdateVideoVram64Size(qemuMonitor *mon,
                                 virDomainVideoDef *video,
                                 const char *videoName)
{
    int rc = -1;
    g_autofree char *path = NULL;

    QEMU_CHECK_MONITOR(mon);

    rc = qemuMonitorJSONFindLinkPath(mon, videoName,
                                     video->info.alias, &path);
    if (rc < 0) {
        if (rc == -2)
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to find QOM Object path for device '%1$s'"),
                           videoName);
        return -1;
    }

    return qemuMonitorJSONUpdateVideoVram64Size(mon, video, path);
}


/* Ensure proper locking around callbacks.  */
#define QEMU_MONITOR_CALLBACK(mon, callback, ...) \
    do { \
        virObjectRef(mon); \
        virObjectUnlock(mon); \
        if ((mon)->cb && (mon)->cb->callback) \
            (mon)->cb->callback(mon, __VA_ARGS__); \
        virObjectLock(mon); \
        virObjectUnref(mon); \
    } while (0)


void
qemuMonitorEmitEvent(qemuMonitor *mon, const char *event,
                     long long seconds, unsigned int micros,
                     const char *details)
{
    VIR_DEBUG("mon=%p event=%s", mon, event);

    QEMU_MONITOR_CALLBACK(mon, domainEvent, mon->vm, event, seconds,
                          micros, details);
}


void
qemuMonitorEmitShutdown(qemuMonitor *mon, virTristateBool guest,
                        const char *reason)
{
    virDomainObj *vm = mon->vm;

    VIR_DEBUG("mon=%p guest=%u", mon, guest);

    /* This isn't best place to set FakeReboot but we need to access
     * mon->vm which is defined in this file. Reboot command in guest
     * will trigger SHUTDOWN event for TDX guest, so we has to deal
     * with it here. */
    if (vm->def->sec &&
        vm->def->sec->sectype == VIR_DOMAIN_LAUNCH_SECURITY_TDX) {
        qemuDomainObjPrivate *priv = vm->privateData;

        /* For secure guest, FakeReboot kills original QEMU instance and
         * create new one. During this process, QEMU send SHUTDOWN event
         * with "host-signal" reason which can trigger another FakeReboot.
         * Check if a FakeReboot is ongoing and bypass "host-signal"
         * processing which is originally come from FakeReboot. */
        if (priv->fakeReboot && STREQ_NULLABLE(reason, "host-signal"))
            return;

        if ((STREQ_NULLABLE(reason, "guest-shutdown") &&
             vm->def->onPoweroff == VIR_DOMAIN_LIFECYCLE_ACTION_RESTART) ||
            (STREQ_NULLABLE(reason, "guest-reset") &&
             vm->def->onReboot == VIR_DOMAIN_LIFECYCLE_ACTION_RESTART))
            qemuDomainSetFakeReboot(vm, true);
    }

    QEMU_MONITOR_CALLBACK(mon, domainShutdown, mon->vm, guest);
}


void
qemuMonitorEmitReset(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainReset, mon->vm);
}


void
qemuMonitorEmitStop(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainStop, mon->vm);
}


void
qemuMonitorEmitResume(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainResume, mon->vm);
}


void
qemuMonitorEmitGuestPanic(qemuMonitor *mon,
                          qemuMonitorEventPanicInfo *info)
{
    VIR_DEBUG("mon=%p", mon);
    QEMU_MONITOR_CALLBACK(mon, domainGuestPanic, mon->vm, info);
}


void
qemuMonitorEmitRTCChange(qemuMonitor *mon, long long offset)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainRTCChange, mon->vm, offset);
}


void
qemuMonitorEmitWatchdog(qemuMonitor *mon, int action)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainWatchdog, mon->vm, action);
}


void
qemuMonitorEmitIOError(qemuMonitor *mon,
                       const char *device,
                       const char *qompath,
                       const char *nodename,
                       int action,
                       bool nospace,
                       const char *reason)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainIOError, mon->vm,
                          device, qompath, nodename, action, nospace, reason);
}


void
qemuMonitorEmitGraphics(qemuMonitor *mon,
                        int phase,
                        int localFamily,
                        const char *localNode,
                        const char *localService,
                        int remoteFamily,
                        const char *remoteNode,
                        const char *remoteService,
                        const char *authScheme,
                        const char *x509dname,
                        const char *saslUsername)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainGraphics, mon->vm, phase,
                          localFamily, localNode, localService,
                          remoteFamily, remoteNode, remoteService,
                          authScheme, x509dname, saslUsername);
}


void
qemuMonitorEmitTrayChange(qemuMonitor *mon,
                          const char *devAlias,
                          const char *devid,
                          int reason)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainTrayChange, mon->vm,
                          devAlias, devid, reason);
}


void
qemuMonitorEmitPMWakeup(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainPMWakeup, mon->vm);
}


void
qemuMonitorEmitPMSuspend(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainPMSuspend, mon->vm);
}


void
qemuMonitorEmitPMSuspendDisk(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainPMSuspendDisk, mon->vm);
}


void
qemuMonitorEmitJobStatusChange(qemuMonitor *mon,
                               const char *jobname,
                               qemuMonitorJobStatus status)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, jobStatusChange, mon->vm, jobname, status);
}


void
qemuMonitorEmitBalloonChange(qemuMonitor *mon,
                             unsigned long long actual)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainBalloonChange, mon->vm, actual);
}


void
qemuMonitorEmitDeviceDeleted(qemuMonitor *mon,
                             const char *devAlias)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainDeviceDeleted, mon->vm, devAlias);
}


void
qemuMonitorEmitDeviceUnplugErr(qemuMonitor *mon,
                               const char *devPath,
                               const char *devAlias)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainDeviceUnplugError, mon->vm,
                          devPath, devAlias);
}


void
qemuMonitorEmitNicRxFilterChanged(qemuMonitor *mon,
                                  const char *devAlias)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainNicRxFilterChanged, mon->vm, devAlias);
}


void
qemuMonitorEmitNetdevStreamDisconnected(qemuMonitor *mon,
                                        const char *devAlias)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainNetdevStreamDisconnected,
                          mon->vm, devAlias);
}


void
qemuMonitorEmitNetdevVhostUserDisconnected(qemuMonitor *mon,
                                           const char *devAlias)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainNetdevVhostUserDisconnected,
                          mon->vm, devAlias);
}


void
qemuMonitorEmitSerialChange(qemuMonitor *mon,
                            const char *devAlias,
                            bool connected)
{
    VIR_DEBUG("mon=%p, devAlias='%s', connected=%d", mon, devAlias, connected);

    QEMU_MONITOR_CALLBACK(mon, domainSerialChange, mon->vm, devAlias, connected);
}


void
qemuMonitorEmitSpiceMigrated(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainSpiceMigrated, mon->vm);
}


void
qemuMonitorEmitMemoryDeviceSizeChange(qemuMonitor *mon,
                                      const char *devAlias,
                                      unsigned long long size)
{
    VIR_DEBUG("mon=%p, devAlias='%s', size=%llu", mon, devAlias, size);

    QEMU_MONITOR_CALLBACK(mon, domainMemoryDeviceSizeChange, mon->vm, devAlias, size);
}


void
qemuMonitorEmitMemoryFailure(qemuMonitor *mon,
                             qemuMonitorEventMemoryFailure *mfp)
{
    QEMU_MONITOR_CALLBACK(mon, domainMemoryFailure, mon->vm, mfp);
}


void
qemuMonitorEmitMigrationStatus(qemuMonitor *mon,
                               int status)
{
    VIR_DEBUG("mon=%p, status=%s",
              mon, NULLSTR(qemuMonitorMigrationStatusTypeToString(status)));

    QEMU_MONITOR_CALLBACK(mon, domainMigrationStatus, mon->vm, status);
}


void
qemuMonitorEmitMigrationPass(qemuMonitor *mon,
                             int pass)
{
    VIR_DEBUG("mon=%p, pass=%d", mon, pass);

    QEMU_MONITOR_CALLBACK(mon, domainMigrationPass, mon->vm, pass);
}


void
qemuMonitorEmitAcpiOstInfo(qemuMonitor *mon,
                           const char *alias,
                           const char *slotType,
                           const char *slot,
                           unsigned int source,
                           unsigned int status)
{
    VIR_DEBUG("mon=%p, alias='%s', slotType='%s', slot='%s', source='%u' status=%u",
              mon, NULLSTR(alias), slotType, slot, source, status);

    QEMU_MONITOR_CALLBACK(mon, domainAcpiOstInfo, mon->vm,
                          alias, slotType, slot, source, status);
}


void
qemuMonitorEmitBlockThreshold(qemuMonitor *mon,
                              const char *nodename,
                              unsigned long long threshold,
                              unsigned long long excess)
{
    VIR_DEBUG("mon=%p, node-name='%s', threshold='%llu', excess='%llu'",
              mon, nodename, threshold, excess);

    QEMU_MONITOR_CALLBACK(mon, domainBlockThreshold, mon->vm,
                          nodename, threshold, excess);
}


void
qemuMonitorEmitDumpCompleted(qemuMonitor *mon,
                             int status,
                             qemuMonitorDumpStats *stats,
                             const char *error)
{
    VIR_DEBUG("mon=%p", mon);

    QEMU_MONITOR_CALLBACK(mon, domainDumpCompleted, mon->vm,
                          status, stats, error);
}


void
qemuMonitorEmitPRManagerStatusChanged(qemuMonitor *mon,
                                      const char *prManager,
                                      bool connected)
{
    VIR_DEBUG("mon=%p, prManager='%s', connected=%d", mon, prManager, connected);

    QEMU_MONITOR_CALLBACK(mon, domainPRManagerStatusChanged,
                          mon->vm, prManager, connected);
}


void
qemuMonitorEmitRdmaGidStatusChanged(qemuMonitor *mon,
                                    const char *netdev,
                                    bool gid_status,
                                    unsigned long long subnet_prefix,
                                    unsigned long long interface_id)
{
    VIR_DEBUG("netdev=%s, gid_status=%d, subnet_prefix=0x%llx, interface_id=0x%llx",
              netdev, gid_status, subnet_prefix, interface_id);

    QEMU_MONITOR_CALLBACK(mon, domainRdmaGidStatusChanged, mon->vm,
                          netdev, gid_status, subnet_prefix, interface_id);
}


void
qemuMonitorEmitGuestCrashloaded(qemuMonitor *mon)
{
    VIR_DEBUG("mon=%p", mon);
    QEMU_MONITOR_CALLBACK(mon, domainGuestCrashloaded, mon->vm);
}


int
qemuMonitorSetCapabilities(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetCapabilities(mon);
}


int
qemuMonitorStartCPUs(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONStartCPUs(mon);
}


int
qemuMonitorStopCPUs(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONStopCPUs(mon);
}


int
qemuMonitorGetStatus(qemuMonitor *mon,
                     bool *running,
                     virDomainPausedReason *reason)
{
    VIR_DEBUG("running=%p, reason=%p", running, reason);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetStatus(mon, running, reason);
}


int
qemuMonitorSystemPowerdown(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSystemPowerdown(mon);
}


int
qemuMonitorSystemReset(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSystemReset(mon);
}


static void
qemuMonitorCPUInfoClear(qemuMonitorCPUInfo *cpus,
                        size_t ncpus)
{
    size_t i;

    for (i = 0; i < ncpus; i++) {
        cpus[i].id = 0;
        cpus[i].qemu_id = -1;
        cpus[i].socket_id = -1;
        cpus[i].die_id = -1;
        cpus[i].cluster_id = -1;
        cpus[i].core_id = -1;
        cpus[i].thread_id = -1;
        cpus[i].node_id = -1;
        cpus[i].vcpus = 0;
        cpus[i].tid = 0;
        cpus[i].halted = false;

        VIR_FREE(cpus[i].qom_path);
        VIR_FREE(cpus[i].alias);
        VIR_FREE(cpus[i].type);
        g_clear_pointer(&cpus[i].props, virJSONValueFree);
    }
}


void
qemuMonitorCPUInfoFree(qemuMonitorCPUInfo *cpus,
                       size_t ncpus)
{
    if (!cpus)
        return;

    qemuMonitorCPUInfoClear(cpus, ncpus);

    g_free(cpus);
}

void
qemuMonitorQueryCpusFree(struct qemuMonitorQueryCpusEntry *entries,
                         size_t nentries)
{
    size_t i;

    if (!entries)
        return;

    for (i = 0; i < nentries; i++)
        g_free(entries[i].qom_path);

    g_free(entries);
}


/**
 * Legacy approach doesn't allow out of order cpus, thus no complex matching
 * algorithm is necessary */
static void
qemuMonitorGetCPUInfoLegacy(struct qemuMonitorQueryCpusEntry *cpuentries,
                            size_t ncpuentries,
                            qemuMonitorCPUInfo *vcpus,
                            size_t maxvcpus)
{
    size_t i;

    for (i = 0; i < maxvcpus; i++) {
        if (i < ncpuentries) {
            vcpus[i].tid = cpuentries[i].tid;
            vcpus[i].halted = cpuentries[i].halted;
            vcpus[i].qemu_id = cpuentries[i].qemu_id;
            vcpus[i].qom_path = g_strdup(cpuentries[i].qom_path);
        }

        /* for legacy hotplug to work we need to fake the vcpu count added by
         * enabling a given vcpu */
        vcpus[i].vcpus = 1;
    }
}


/**
 * qemuMonitorGetCPUInfoHotplug:
 *
 * This function stitches together data retrieved via query-hotpluggable-cpus
 * which returns entities on the hotpluggable level (which may describe more
 * than one guest logical vcpu) with the output of query-cpus-fast,
 * having an entry per enabled guest logical vcpu.
 *
 * query-hotpluggable-cpus conveys following information:
 * - topology information and number of logical vcpus this entry creates
 * - device type name of the entry that needs to be used when hotplugging
 * - qom path in qemu which can be used to map the entry against
 *   query-cpus-fast
 *
 * query-cpus-fast conveys following information:
 * - thread id of a given guest logical vcpu
 * - order in which the vcpus were inserted
 * - qom path to allow mapping the two together
 *
 * The libvirt's internal structure has an entry for each possible (even
 * disabled) guest vcpu. The purpose is to map the data together so that we are
 * certain of the thread id mapping and the information required for vcpu
 * hotplug.
 *
 * This function returns 0 on success and -1 on error, but does not report
 * libvirt errors so that fallback approach can be used.
 */
static int
qemuMonitorGetCPUInfoHotplug(struct qemuMonitorQueryHotpluggableCpusEntry *hotplugvcpus,
                             size_t nhotplugvcpus,
                             struct qemuMonitorQueryCpusEntry *cpuentries,
                             size_t ncpuentries,
                             qemuMonitorCPUInfo *vcpus,
                             size_t maxvcpus)
{
    char *tmp;
    int order = 1;
    size_t totalvcpus = 0;
    size_t mainvcpu; /* this iterator is used for iterating hotpluggable entities */
    size_t subvcpu; /* this corresponds to subentries of a hotpluggable entry */
    size_t anyvcpu; /* this iterator is used for any vcpu entry in the result */
    size_t i;
    size_t j;

    /* ensure that the total vcpu count reported by query-hotpluggable-cpus equals
     * to the libvirt maximum cpu count */
    for (i = 0; i < nhotplugvcpus; i++)
        totalvcpus += hotplugvcpus[i].vcpus;

    /* trim '/thread...' suffix from the data returned by query-cpus-fast */
    for (i = 0; i < ncpuentries; i++) {
        if (cpuentries[i].qom_path &&
            (tmp = strstr(cpuentries[i].qom_path, "/thread")))
            *tmp = '\0';
    }

    if (totalvcpus != maxvcpus) {
        VIR_DEBUG("expected '%zu' total vcpus got '%zu'", maxvcpus, totalvcpus);
        return -1;
    }

    /* Note the order in which the hotpluggable entities are inserted by
     * matching them to the query-cpus-fast entries */
    for (i = 0; i < ncpuentries; i++) {
        for (j = 0; j < nhotplugvcpus; j++) {
            if (!cpuentries[i].qom_path ||
                !hotplugvcpus[j].qom_path ||
                STRNEQ(cpuentries[i].qom_path, hotplugvcpus[j].qom_path))
                continue;

            /* add ordering info for hotpluggable entries */
            if (hotplugvcpus[j].enable_id == 0)
                hotplugvcpus[j].enable_id = order++;

            break;
        }
    }

    /* transfer appropriate data from the hotpluggable list to corresponding
     * entries. the entries returned by qemu may in fact describe multiple
     * logical vcpus in the guest */
    mainvcpu = 0;
    for (i = 0; i < nhotplugvcpus; i++) {
        vcpus[mainvcpu].online = !!hotplugvcpus[i].qom_path;
        vcpus[mainvcpu].hotpluggable = !!hotplugvcpus[i].alias ||
                                         !vcpus[mainvcpu].online;
        vcpus[mainvcpu].socket_id = hotplugvcpus[i].socket_id;
        vcpus[mainvcpu].die_id = hotplugvcpus[i].die_id;
        vcpus[mainvcpu].cluster_id = hotplugvcpus[i].cluster_id;
        vcpus[mainvcpu].core_id = hotplugvcpus[i].core_id;
        vcpus[mainvcpu].thread_id = hotplugvcpus[i].thread_id;
        vcpus[mainvcpu].node_id = hotplugvcpus[i].node_id;
        vcpus[mainvcpu].vcpus = hotplugvcpus[i].vcpus;
        vcpus[mainvcpu].qom_path = g_steal_pointer(&hotplugvcpus[i].qom_path);
        vcpus[mainvcpu].alias = g_steal_pointer(&hotplugvcpus[i].alias);
        vcpus[mainvcpu].type = g_steal_pointer(&hotplugvcpus[i].type);
        vcpus[mainvcpu].props = g_steal_pointer(&hotplugvcpus[i].props);
        vcpus[mainvcpu].id = hotplugvcpus[i].enable_id;

        /* copy state information to sub vcpus */
        for (subvcpu = mainvcpu + 1; subvcpu < mainvcpu + hotplugvcpus[i].vcpus; subvcpu++) {
            vcpus[subvcpu].online = vcpus[mainvcpu].online;
            vcpus[subvcpu].hotpluggable = vcpus[mainvcpu].hotpluggable;
        }

        /* calculate next master vcpu (hotpluggable unit) entry */
        mainvcpu += hotplugvcpus[i].vcpus;
    }

    /* match entries from query cpus to the output array taking into account
     * multi-vcpu objects */
    for (j = 0; j < ncpuentries; j++) {
        /* find the correct entry or beginning of group of entries */
        for (anyvcpu = 0; anyvcpu < maxvcpus; anyvcpu++) {
            if (cpuentries[j].qom_path && vcpus[anyvcpu].qom_path &&
                STREQ(cpuentries[j].qom_path, vcpus[anyvcpu].qom_path))
                break;
        }

        if (anyvcpu == maxvcpus) {
            VIR_DEBUG("too many query-cpus-fast entries for a given "
                      "query-hotpluggable-cpus entry");
            return -1;
        }

        if (vcpus[anyvcpu].vcpus != 1) {
            /* find a possibly empty vcpu thread for core granularity systems */
            for (; anyvcpu < maxvcpus; anyvcpu++) {
                if (vcpus[anyvcpu].tid == 0)
                    break;
            }
        }

        vcpus[anyvcpu].qemu_id = cpuentries[j].qemu_id;
        vcpus[anyvcpu].tid = cpuentries[j].tid;
        vcpus[anyvcpu].halted = cpuentries[j].halted;
    }

    return 0;
}


/**
 * qemuMonitorGetCPUInfo:
 * @mon: monitor
 * @vcpus: pointer filled by array of qemuMonitorCPUInfo structures
 * @maxvcpus: total possible number of vcpus
 * @hotplug: query data relevant for hotplug support
 *
 * Detects VCPU information. If qemu doesn't support or fails reporting
 * information this function will return success as other parts of libvirt
 * are able to cope with that.
 *
 * Returns 0 on success (including if qemu didn't report any data) and
 *  -1 on error (reports libvirt error).
 */
int
qemuMonitorGetCPUInfo(qemuMonitor *mon,
                      qemuMonitorCPUInfo **vcpus,
                      size_t maxvcpus,
                      bool hotplug)
{
    struct qemuMonitorQueryHotpluggableCpusEntry *hotplugcpus = NULL;
    size_t nhotplugcpus = 0;
    struct qemuMonitorQueryCpusEntry *cpuentries = NULL;
    size_t ncpuentries = 0;
    int ret = -1;
    int rc;
    qemuMonitorCPUInfo *info = NULL;

    QEMU_CHECK_MONITOR(mon);

    info = g_new0(qemuMonitorCPUInfo, maxvcpus);

    /* initialize a few non-zero defaults */
    qemuMonitorCPUInfoClear(info, maxvcpus);

    if (hotplug &&
        (qemuMonitorJSONGetHotpluggableCPUs(mon, &hotplugcpus, &nhotplugcpus)) < 0)
        goto cleanup;

    rc = qemuMonitorJSONQueryCPUs(mon, &cpuentries, &ncpuentries, hotplug);

    if (rc < 0) {
        if (!hotplug && rc == -2) {
            *vcpus = g_steal_pointer(&info);
            ret = 0;
        }

        goto cleanup;
    }

    if (!hotplugcpus ||
        qemuMonitorGetCPUInfoHotplug(hotplugcpus, nhotplugcpus,
                                     cpuentries, ncpuentries,
                                     info, maxvcpus) < 0) {
        /* Fallback to the legacy algorithm. Hotplug paths will make sure that
         * the appropriate data is present */
        qemuMonitorCPUInfoClear(info, maxvcpus);
        qemuMonitorGetCPUInfoLegacy(cpuentries, ncpuentries, info, maxvcpus);
    }

    *vcpus = g_steal_pointer(&info);
    ret = 0;

 cleanup:
    qemuMonitorQueryHotpluggableCpusFree(hotplugcpus, nhotplugcpus);
    qemuMonitorQueryCpusFree(cpuentries, ncpuentries);
    qemuMonitorCPUInfoFree(info, maxvcpus);
    return ret;
}


/**
 * qemuMonitorGetCpuHalted:
 *
 * Returns a bitmap of vcpu id's that are halted. The id's correspond to the
 * 'CPU' field as reported by query-cpus-fast'.
 */
virBitmap *
qemuMonitorGetCpuHalted(qemuMonitor *mon,
                        size_t maxvcpus)
{
    struct qemuMonitorQueryCpusEntry *cpuentries = NULL;
    size_t ncpuentries = 0;
    size_t i;
    int rc;
    virBitmap *ret = NULL;

    QEMU_CHECK_MONITOR_NULL(mon);

    rc = qemuMonitorJSONQueryCPUs(mon, &cpuentries, &ncpuentries, false);

    if (rc < 0)
        goto cleanup;

    ret = virBitmapNew(maxvcpus);

    for (i = 0; i < ncpuentries; i++) {
        if (cpuentries[i].halted)
            ignore_value(virBitmapSetBit(ret, cpuentries[i].qemu_id));
    }

 cleanup:
    qemuMonitorQueryCpusFree(cpuentries, ncpuentries);
    return ret;
}


int
qemuMonitorSetLink(qemuMonitor *mon,
                   const char *name,
                   virDomainNetInterfaceLinkState state)
{
    VIR_DEBUG("name=%s, state=%u", name, state);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetLink(mon, name, state);
}


/**
 * Returns: 0 if balloon not supported, +1 if balloon query worked
 * or -1 on failure
 */
int
qemuMonitorGetBalloonInfo(qemuMonitor *mon,
                          unsigned long long *currmem)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetBalloonInfo(mon, currmem);
}


int
qemuMonitorGetMemoryStats(qemuMonitor *mon,
                          virDomainMemballoonDef *balloon,
                          virDomainMemoryStatPtr stats,
                          unsigned int nr_stats)
{
    VIR_DEBUG("stats=%p nstats=%u", stats, nr_stats);

    QEMU_CHECK_MONITOR(mon);

    qemuMonitorInitBalloonObjectPath(mon, balloon);
    return qemuMonitorJSONGetMemoryStats(mon, mon->balloonpath,
                                         stats, nr_stats);
}


/**
 * qemuMonitorSetMemoryStatsPeriod:
 *
 * This function sets balloon stats update period.
 *
 * Returns 0 on success and -1 on error, but does *not* set an error.
 */
int
qemuMonitorSetMemoryStatsPeriod(qemuMonitor *mon,
                                virDomainMemballoonDef *balloon,
                                int period)
{
    int ret = -1;
    VIR_DEBUG("mon=%p period=%d", mon, period);

    if (!mon)
        return -1;

    if (period < 0)
        return -1;

    qemuMonitorInitBalloonObjectPath(mon, balloon);
    if (mon->balloonpath) {
        ret = qemuMonitorJSONSetMemoryStatsPeriod(mon, mon->balloonpath,
                                                  period);

        /*
         * Most of the calls to this function are supposed to be
         * non-fatal and the only one that should be fatal wants its
         * own error message.  More details for debugging will be in
         * the log file.
         */
        if (ret < 0)
            virResetLastError();
    }
    return ret;
}


int
qemuMonitorBlockIOStatusToError(const char *status)
{
    int st = qemuMonitorBlockIOStatusTypeFromString(status);

    if (st < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown block IO status: %1$s"), status);
        return -1;
    }

    switch ((qemuMonitorBlockIOStatus) st) {
    case QEMU_MONITOR_BLOCK_IO_STATUS_OK:
        return VIR_DOMAIN_DISK_ERROR_NONE;
    case QEMU_MONITOR_BLOCK_IO_STATUS_FAILED:
        return VIR_DOMAIN_DISK_ERROR_UNSPEC;
    case QEMU_MONITOR_BLOCK_IO_STATUS_NOSPACE:
        return VIR_DOMAIN_DISK_ERROR_NO_SPACE;

    /* unreachable */
    case QEMU_MONITOR_BLOCK_IO_STATUS_LAST:
        break;
    }
    return -1;
}


static void
qemuDomainDiskInfoFree(void *value)
{
    struct qemuDomainDiskInfo *info = value;

    g_free(info->nodename);
    g_free(info);
}


GHashTable *
qemuMonitorGetBlockInfo(qemuMonitor *mon)
{
    g_autoptr(GHashTable) table = virHashNew(qemuDomainDiskInfoFree);

    QEMU_CHECK_MONITOR_NULL(mon);

    if (qemuMonitorJSONGetBlockInfo(mon, table) < 0) {
        return NULL;
    }

    return g_steal_pointer(&table);
}


/**
 * qemuMonitorQueryBlockstats:
 * @mon: monitor object
 *
 * Returns data from a call to 'query-blockstats' without using 'query-nodes'
 */
virJSONValue *
qemuMonitorQueryBlockstats(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONQueryBlockstats(mon, false);
}


/**
 * qemuMonitorGetAllBlockStatsInfo:
 * @mon: monitor object
 * @ret_stats: pointer that is filled with a hash table containing the stats
 *
 * Creates a hash table in @ret_stats with block stats of all devices and the
 * backing chains for the block devices.
 *
 * Returns < 0 on error, count of supported block stats fields on success.
 */
int
qemuMonitorGetAllBlockStatsInfo(qemuMonitor *mon,
                                GHashTable **ret_stats)
{
    int ret;
    g_autoptr(GHashTable) stats = virHashNew(g_free);

    QEMU_CHECK_MONITOR(mon);

    ret = qemuMonitorJSONGetAllBlockStatsInfo(mon, stats);

    if (ret < 0)
        return -1;

    *ret_stats = g_steal_pointer(&stats);
    return ret;
}


int
qemuMonitorBlockStatsUpdateCapacityBlockdev(qemuMonitor *mon,
                                            GHashTable *stats)
{
    VIR_DEBUG("stats=%p", stats);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockStatsUpdateCapacityBlockdev(mon, stats);
}


/**
 * qemuMonitorBlockGetNamedNodeData:
 * @mon: monitor object
 *
 * Uses 'query-named-block-nodes' to retrieve information about individual
 * storage nodes and returns them in a hash table of qemuBlockNamedNodeData *s
 * filled with the data. The hash table keys are node names.
 */
GHashTable *
qemuMonitorBlockGetNamedNodeData(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONBlockGetNamedNodeData(mon);
}


int
qemuMonitorBlockResize(qemuMonitor *mon,
                       const char *device,
                       const char *nodename,
                       unsigned long long size)
{
    VIR_DEBUG("device=%s nodename=%s size=%llu",
              NULLSTR(device), NULLSTR(nodename), size);

    QEMU_CHECK_MONITOR(mon);

    if ((!device && !nodename) || (device && nodename)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("exactly one of 'device' and 'nodename' need to be specified"));
        return -1;
    }

    return qemuMonitorJSONBlockResize(mon, device, nodename, size);
}


static const char *
qemuMonitorTypeToProtocol(int type)
{
    switch (type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        return "vnc";
    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        return "spice";
    default:
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported protocol type %1$s"),
                       virDomainGraphicsTypeToString(type));
        return NULL;
    }
}


int
qemuMonitorSetPassword(qemuMonitor *mon,
                       int type,
                       const char *password,
                       const char *action_if_connected)
{
    const char *protocol = qemuMonitorTypeToProtocol(type);

    if (!protocol)
        return -1;

    VIR_DEBUG("protocol=%s, action_if_connected=%s",
              protocol, action_if_connected);

    QEMU_CHECK_MONITOR(mon);

    if (!password)
        password = "";

    if (!action_if_connected)
        action_if_connected = "keep";

    return qemuMonitorJSONSetPassword(mon, protocol, password, action_if_connected);
}


int
qemuMonitorExpirePassword(qemuMonitor *mon,
                          int type,
                          const char *expire_time)
{
    const char *protocol = qemuMonitorTypeToProtocol(type);

    if (!protocol)
        return -1;

    VIR_DEBUG("protocol=%s, expire_time=%s", protocol, expire_time);

    QEMU_CHECK_MONITOR(mon);

    if (!expire_time)
        expire_time = "now";

    return qemuMonitorJSONExpirePassword(mon, protocol, expire_time);
}


/*
 * Returns: 0 if balloon not supported, +1 if balloon adjust worked
 * or -1 on failure
 */
int
qemuMonitorSetBalloon(qemuMonitor *mon,
                      unsigned long long newmem)
{
    VIR_DEBUG("newmem=%llu", newmem);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetBalloon(mon, newmem);
}


int
qemuMonitorSaveVirtualMemory(qemuMonitor *mon,
                             unsigned long long offset,
                             unsigned long long length,
                             const char *path)
{
    VIR_DEBUG("offset=%llu length=%llu path=%s", offset, length, path);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSaveVirtualMemory(mon, offset, length, path);
}


int
qemuMonitorSavePhysicalMemory(qemuMonitor *mon,
                              unsigned long long offset,
                              unsigned long long length,
                              const char *path)
{
    VIR_DEBUG("offset=%llu length=%llu path=%s", offset, length, path);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSavePhysicalMemory(mon, offset, length, path);
}


int
qemuMonitorSetDBusVMStateIdList(qemuMonitor *mon,
                                GSList *list)
{
    g_autofree char *path = NULL;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    GSList *next;

    VIR_DEBUG("list=%p", list);

    QEMU_CHECK_MONITOR(mon);

    if (!list)
        return 0;

    for (next = list; next; next = next->next)
        virBufferAsprintf(&buf, "%s,", (const char *) next->data);

    virBufferTrim(&buf, ",");

    path = g_strdup_printf("/objects/%s", qemuDomainGetDBusVMStateAlias());

    return qemuMonitorJSONSetDBusVMStateIdList(mon, path,
                                               virBufferCurrentContent(&buf));
}


int
qemuMonitorSetUSBDiskAttached(qemuMonitor *mon,
                              const char *alias)
{
    QEMU_CHECK_MONITOR(mon);

    VIR_DEBUG("alias=%s", alias);

    return qemuMonitorJSONSetUSBDiskAttached(mon, alias);
}


/**
 * qemuMonitorGetMigrationParams:
 * @mon: Pointer to the monitor object.
 * @params: Where to store migration parameters.
 *
 * The caller is responsible for freeing @params.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuMonitorGetMigrationParams(qemuMonitor *mon,
                              virJSONValue **params)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetMigrationParams(mon, params);
}


/**
 * qemuMonitorSetMigrationParams:
 * @mon: Pointer to the monitor object.
 * @params: Migration parameters.
 *
 * The @params object is consumed and cleared on success and some errors.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuMonitorSetMigrationParams(qemuMonitor *mon,
                              virJSONValue **params)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetMigrationParams(mon, params);
}


int
qemuMonitorGetMigrationStats(qemuMonitor *mon,
                             qemuMonitorMigrationStats *stats,
                             char **error)
{
    QEMU_CHECK_MONITOR(mon);

    if (error)
        *error = NULL;

    return qemuMonitorJSONGetMigrationStats(mon, stats, error);
}


int
qemuMonitorMigrateToFd(qemuMonitor *mon,
                       unsigned int flags,
                       int fd)
{
    int ret;
    VIR_DEBUG("fd=%d flags=0x%x", fd, flags);

    QEMU_CHECK_MONITOR(mon);

    if (qemuMonitorSendFileHandle(mon, "migrate", fd) < 0)
        return -1;

    ret = qemuMonitorJSONMigrate(mon, flags, "fd:migrate");

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, "migrate") < 0)
            VIR_WARN("failed to close migration handle");
    }

    return ret;
}


int
qemuMonitorMigrateToFdSet(virDomainObj *vm,
                          unsigned int flags,
                          int *fd,
                          int *directFd)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    qemuMonitor *mon = priv->mon;
    off_t offset;
    g_autoptr(qemuFDPass) fdPassMigrate = NULL;
    g_autofree char *uri = NULL;
    int ret;

    VIR_DEBUG("fd=%d directFd=%d flags=0x%x", *fd, *directFd, flags);

    QEMU_CHECK_MONITOR(mon);

    if ((offset = lseek(*fd, 0, SEEK_CUR)) == -1) {
        virReportSystemError(errno,
                             "%s", _("failed to seek on file descriptor"));
        return -1;
    }

    fdPassMigrate = qemuFDPassNew("libvirt-outgoing-migrate", priv);
    qemuFDPassAddFD(fdPassMigrate, fd, "-fd");
    if (*directFd != -1)
        qemuFDPassAddFD(fdPassMigrate, directFd, "-directio-fd");
    qemuFDPassTransferMonitor(fdPassMigrate, mon);

    uri = g_strdup_printf("file:%s,offset=%#jx",
                          qemuFDPassGetPath(fdPassMigrate), (uintmax_t)offset);
    ret = qemuMonitorJSONMigrate(mon, flags, uri);

    return ret;
}


int
qemuMonitorMigrateToHost(qemuMonitor *mon,
                         unsigned int flags,
                         const char *protocol,
                         const char *hostname,
                         int port)
{
    int ret;
    char *uri = NULL;
    VIR_DEBUG("hostname=%s port=%d flags=0x%x", hostname, port, flags);

    QEMU_CHECK_MONITOR(mon);

    if (strchr(hostname, ':')) {
        uri = g_strdup_printf("%s:[%s]:%d", protocol, hostname, port);
    } else uri = g_strdup_printf("%s:%s:%d", protocol, hostname, port);

    ret = qemuMonitorJSONMigrate(mon, flags, uri);

    VIR_FREE(uri);
    return ret;
}


int
qemuMonitorMigrateToSocket(qemuMonitor *mon,
                           unsigned int flags,
                           const char *socketPath)
{
    g_autofree char *uri = g_strdup_printf("unix:%s", socketPath);

    VIR_DEBUG("socketPath=%s flags=0x%x", socketPath, flags);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrate(mon, flags, uri);
}


int
qemuMonitorMigrateCancel(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrateCancel(mon);
}


int
qemuMonitorMigratePause(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigratePause(mon);
}


int
qemuMonitorQueryDump(qemuMonitor *mon,
                     qemuMonitorDumpStats *stats)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONQueryDump(mon, stats);
}


/**
 * Returns 1 if @capability is supported, 0 if it's not, or -1 on error.
 */
int
qemuMonitorGetDumpGuestMemoryCapability(qemuMonitor *mon,
                                        const char *capability)
{
    VIR_DEBUG("capability=%s", capability);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetDumpGuestMemoryCapability(mon, capability);
}


int
qemuMonitorDumpToFd(qemuMonitor *mon,
                    int fd,
                    const char *dumpformat)
{
    int ret;
    VIR_DEBUG("fd=%d dumpformat=%s", fd, dumpformat);

    QEMU_CHECK_MONITOR(mon);

    if (qemuMonitorSendFileHandle(mon, "dump", fd) < 0)
        return -1;

    ret = qemuMonitorJSONDump(mon, "fd:dump", dumpformat);

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, "dump") < 0)
            VIR_WARN("failed to close dumping handle");
    }

    return ret;
}


int
qemuMonitorGraphicsRelocate(qemuMonitor *mon,
                            int type,
                            const char *hostname,
                            int port,
                            int tlsPort,
                            const char *tlsSubject)
{
    VIR_DEBUG("type=%d hostname=%s port=%d tlsPort=%d tlsSubject=%s",
              type, hostname, port, tlsPort, NULLSTR(tlsSubject));

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGraphicsRelocate(mon,
                                           type,
                                           hostname,
                                           port,
                                           tlsPort,
                                           tlsSubject);
}


/**
 * qemuMonitorAddFileHandleToSet:
 * @mon: monitor object
 * @fd: file descriptor to pass to qemu
 * @fdset: the fdset to register this fd with, -1 to create a new fdset
 * @opaque: opaque data to associated with this fd
 *
 * Attempts to register a file descriptor with qemu that can then be referenced
 * via the file path /dev/fdset/$FDSETID
 * Returns 0 if ok, and -1 on failure */
int
qemuMonitorAddFileHandleToSet(qemuMonitor *mon,
                              int fd,
                              int fdset,
                              const char *opaque)
{
    VIR_DEBUG("fd=%d,fdset=%i,opaque=%s", fd, fdset, opaque);

    QEMU_CHECK_MONITOR(mon);

    if (fd < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("fd must be valid"));
        return -1;
    }

    return qemuMonitorJSONAddFileHandleToSet(mon, fd, fdset, opaque);
}


/**
 * qemuMonitorRemoveFdset:
 * @mon: monitor object
 * @fdset: id of the fdset to remove
 *
 * Attempts to remove @fdset from qemu and close associated file descriptors
 * Returns 0 if ok, and -1 on failure */
int
qemuMonitorRemoveFdset(qemuMonitor *mon,
                       unsigned int fdset)
{
    VIR_DEBUG("fdset=%u", fdset);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONRemoveFdset(mon, fdset);
}


void qemuMonitorFdsetsFree(qemuMonitorFdsets *fdsets)
{
    size_t i;

    for (i = 0; i < fdsets->nfdsets; i++) {
        size_t j;
        qemuMonitorFdsetInfo *set = &fdsets->fdsets[i];

        for (j = 0; j < set->nfds; j++)
            g_free(set->fds[j].opaque);

        g_free(set->fds);
    }
    g_free(fdsets->fdsets);
    g_free(fdsets);
}


/**
 * qemuMonitorQueryFdsets:
 * @mon: monitor object
 * @fdsets: a pointer that is filled with a new qemuMonitorFdsets struct
 *
 * Queries qemu for the fdsets that are registered with that instance, and
 * returns a structure describing those fdsets. The returned struct should be
 * freed with qemuMonitorFdsetsFree();
 *
 * Returns 0 if ok, and -1 on failure */
int
qemuMonitorQueryFdsets(qemuMonitor *mon,
                       qemuMonitorFdsets **fdsets)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONQueryFdsets(mon, fdsets);
}


int
qemuMonitorSendFileHandle(qemuMonitor *mon,
                          const char *fdname,
                          int fd)
{
    VIR_DEBUG("fdname=%s fd=%d", fdname, fd);

    QEMU_CHECK_MONITOR(mon);

    if (fd < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("fd must be valid"));
        return -1;
    }

    return qemuMonitorJSONSendFileHandle(mon, fdname, fd);
}


int
qemuMonitorCloseFileHandle(qemuMonitor *mon,
                           const char *fdname)
{
    int ret = -1;
    virErrorPtr error;

    VIR_DEBUG("fdname=%s", fdname);

    virErrorPreserveLast(&error);

    QEMU_CHECK_MONITOR_GOTO(mon, cleanup);

    ret = qemuMonitorJSONCloseFileHandle(mon, fdname);

 cleanup:
    virErrorRestore(&error);
    return ret;
}


int
qemuMonitorAddNetdev(qemuMonitor *mon,
                     virJSONValue **props)
{

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONAddNetdev(mon, props);
}


int
qemuMonitorRemoveNetdev(qemuMonitor *mon,
                        const char *alias)
{
    VIR_DEBUG("alias=%s", alias);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONRemoveNetdev(mon, alias);
}


/**
 * qemuMonitorQueryRxFilter:
 * @mon: monitor object
 * @alias: alias of the network interface
 * @filter: where to store the result (can be NULL)
 *
 * Issues query-rx-filter command for given device (@alias) and stores parsed
 * output at @filter (if not NULL). If @filter is NULL, the command is executed
 * but nothing is parsed.
 *
 * Returns 0 on success, -1 otherwise.
 */
int
qemuMonitorQueryRxFilter(qemuMonitor *mon, const char *alias,
                         virNetDevRxFilter **filter)
{
    VIR_DEBUG("alias=%s filter=%p", alias, filter);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONQueryRxFilter(mon, alias, filter);
}


void
qemuMonitorChardevInfoFree(void *data)
{
    qemuMonitorChardevInfo *info = data;

    g_free(info->ptyPath);
    g_free(info);
}


int
qemuMonitorGetChardevInfo(qemuMonitor *mon,
                          GHashTable **retinfo)
{
    g_autoptr(GHashTable) info = virHashNew(qemuMonitorChardevInfoFree);

    VIR_DEBUG("retinfo=%p", retinfo);

    QEMU_CHECK_MONITOR(mon);

    if (qemuMonitorJSONGetChardevInfo(mon, info) < 0)
        return -1;

    *retinfo = g_steal_pointer(&info);
    return 0;
}


/**
 * @mon: monitor object
 * @devalias: alias of the device to detach
 *
 * Sends device detach request to qemu.
 *
 * Returns: 0 on success,
 *         -2 if DeviceNotFound error encountered (error NOT reported)
 *         -1 otherwise (error reported)
 */
int
qemuMonitorDelDevice(qemuMonitor *mon,
                     const char *devalias)
{
    VIR_DEBUG("devalias=%s", devalias);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONDelDevice(mon, devalias);
}


/**
 * qemuMonitorAddDeviceProps:
 * @mon: monitor object
 * @props: JSON object describing the device to add, the object is consumed
 *         and cleared.
 *
 * Adds a device described by @props.
 * Returns 0 on success -1 on error.
 */
int
qemuMonitorAddDeviceProps(qemuMonitor *mon,
                          virJSONValue **props)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONAddDeviceProps(mon, props);
}


/**
 * qemuMonitorCreateObjectProps:
 * @propsret: returns full object properties
 * @type: Type name of object to add
 * @objalias: Alias of the new object
 * @...: Optional arguments for the given object. See virJSONValueObjectAddVArgs.
 *
 * Returns a JSONValue containing everything on success and NULL on error.
 */
int
qemuMonitorCreateObjectProps(virJSONValue **propsret,
                             const char *type,
                             const char *alias,
                             ...)
{
    g_autoptr(virJSONValue) props = NULL;
    int rc;
    va_list args;

    if (virJSONValueObjectAdd(&props,
                              "s:qom-type", type,
                              "s:id", alias,
                              NULL) < 0)
        return -1;


    va_start(args, alias);

    rc = virJSONValueObjectAddVArgs(&props, args);

    va_end(args);

    if (rc < 0)
        return -1;

    *propsret = g_steal_pointer(&props);
    return 0;
}


/**
 * qemuMonitorAddObject:
 * @mon: Pointer to monitor object
 * @props: Pointer to a JSON object holding configuration of the object to add.
 *         The object must be non-null and contain at least the "qom-type" and
 *         "id" field. The object is consumed and the pointer is cleared.
 * @alias: If not NULL, returns the alias of the added object if it was added
 *         successfully to qemu. Caller should free the returned pointer.
 *
 * Returns 0 on success -1 on error.
 */
int
qemuMonitorAddObject(qemuMonitor *mon,
                     virJSONValue **props,
                     char **alias)
{
    const char *type = NULL;
    const char *id = NULL;
    g_autofree char *aliasCopy = NULL;

    if (!*props) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("object props can't be NULL"));
        return -1;
    }

    type = virJSONValueObjectGetString(*props, "qom-type");
    id = virJSONValueObjectGetString(*props, "id");

    VIR_DEBUG("type=%s id=%s", NULLSTR(type), NULLSTR(id));

    QEMU_CHECK_MONITOR(mon);

    if (!id || !type) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("missing alias or qom-type for qemu object '%1$s'"),
                       NULLSTR(type));
        return -1;
    }

    if (alias)
        aliasCopy = g_strdup(id);

    if (qemuMonitorJSONAddObject(mon, props) < 0)
        return -1;

    if (alias)
        *alias = g_steal_pointer(&aliasCopy);

    return 0;
}


int
qemuMonitorDelObject(qemuMonitor *mon,
                     const char *objalias,
                     bool report_error)
{
    VIR_DEBUG("objalias=%s", objalias);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONDelObject(mon, objalias, report_error);
}


int
qemuMonitorSnapshotSave(qemuMonitor *mon,
                        const char *jobname,
                        const char *snapshotname,
                        const char *vmstate_disk,
                        const char **disks)
{
    VIR_DEBUG("jobname='%s', snapshotname='%s', vmstate_disk='%s'",
              jobname, snapshotname, vmstate_disk);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSnapshotSave(mon, jobname, snapshotname, vmstate_disk, disks);
}


int
qemuMonitorSnapshotLoad(qemuMonitor *mon,
                        const char *jobname,
                        const char *snapshotname,
                        const char *vmstate_disk,
                        const char **disks)
{
    VIR_DEBUG("jobname='%s', snapshotname='%s', vmstate_disk='%s'",
              jobname, snapshotname, vmstate_disk);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSnapshotLoad(mon, jobname, snapshotname, vmstate_disk, disks);
}


int
qemuMonitorSnapshotDelete(qemuMonitor *mon,
                          const char *jobname,
                          const char *snapshotname,
                          const char **disks)
{
    VIR_DEBUG("jobname='%s', snapshotname='%s'", jobname, snapshotname);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSnapshotDelete(mon, jobname, snapshotname, disks);
}


int
qemuMonitorBlockdevMirror(qemuMonitor *mon,
                          const char *jobname,
                          bool persistjob,
                          const char *device,
                          const char *target,
                          const char *replaces,
                          unsigned long long bandwidth,
                          unsigned int granularity,
                          unsigned long long buf_size,
                          bool shallow,
                          bool syncWrite)
{
    VIR_DEBUG("jobname=%s, persistjob=%d, device=%s, target=%s, replaces=%s, bandwidth=%lld, "
              "granularity=%#x, buf_size=%lld, shallow=%d syncWrite=%d",
              NULLSTR(jobname), persistjob, device, target, NULLSTR(replaces),
              bandwidth, granularity, buf_size, shallow, syncWrite);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevMirror(mon, jobname, persistjob, device, target, replaces,
                                         bandwidth, granularity, buf_size, shallow,
                                         syncWrite);
}


/* Use the transaction QMP command to run atomic snapshot commands.  */
int
qemuMonitorTransaction(qemuMonitor *mon, virJSONValue **actions)
{
    VIR_DEBUG("actions=%p", *actions);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONTransaction(mon, actions);
}


/* Start a block-commit block job.  bandwidth is in bytes/sec.  */
int
qemuMonitorBlockCommit(qemuMonitor *mon,
                       const char *device,
                       const char *jobname,
                       const char *topNode,
                       const char *baseNode,
                       const char *backingName,
                       unsigned long long bandwidth,
                       virTristateBool autofinalize)
{
    VIR_DEBUG("device=%s, jobname=%s, topNode=%s, baseNode=%s, backingName=%s, bandwidth=%llu, autofinalize=%d",
              device, NULLSTR(jobname), NULLSTR(topNode),
              NULLSTR(baseNode), NULLSTR(backingName), bandwidth, autofinalize);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockCommit(mon, device, jobname, topNode, baseNode,
                                      backingName, bandwidth, autofinalize);
}


int
qemuMonitorArbitraryCommand(qemuMonitor *mon,
                            const char *cmd,
                            int fd,
                            char **reply,
                            bool hmp)
{
    VIR_DEBUG("cmd=%s, fd=%d, reply=%p, hmp=%d", cmd, fd, reply, hmp);

    QEMU_CHECK_MONITOR(mon);

    if (hmp)
        return qemuMonitorJSONHumanCommand(mon, cmd, fd, reply);
    else
        return qemuMonitorJSONArbitraryCommand(mon, cmd, fd, reply);
}


int
qemuMonitorInjectNMI(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONInjectNMI(mon);
}


int
qemuMonitorSendKey(qemuMonitor *mon,
                   unsigned int holdtime,
                   unsigned int *keycodes,
                   unsigned int nkeycodes)
{
    VIR_DEBUG("holdtime=%u, nkeycodes=%u", holdtime, nkeycodes);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSendKey(mon, holdtime, keycodes, nkeycodes);
}


int
qemuMonitorScreendump(qemuMonitor *mon,
                      const char *device,
                      unsigned int head,
                      const char *format,
                      const char *file)
{
    VIR_DEBUG("device=%s head=%u format=%s file=%s",
              device, head, NULLSTR(format), file);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONScreendump(mon, device, head, format, file);
}


/* bandwidth is in bytes/sec */
int
qemuMonitorBlockStream(qemuMonitor *mon,
                       const char *device,
                       const char *jobname,
                       const char *baseNode,
                       const char *backingName,
                       unsigned long long bandwidth)
{
    VIR_DEBUG("device=%s, jobname=%s, baseNode=%s, backingName=%s, bandwidth=%lluB",
              device, NULLSTR(jobname),
              NULLSTR(baseNode), NULLSTR(backingName), bandwidth);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockStream(mon, device, jobname,
                                      baseNode, backingName, bandwidth);
}


int
qemuMonitorBlockJobCancel(qemuMonitor *mon,
                          const char *jobname,
                          bool force)
{
    VIR_DEBUG("jobname=%s force=%d", jobname, force);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockJobCancel(mon, jobname, force);
}


int
qemuMonitorBlockJobSetSpeed(qemuMonitor *mon,
                            const char *jobname,
                            unsigned long long bandwidth)
{
    VIR_DEBUG("jobname=%s, bandwidth=%lluB", jobname, bandwidth);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockJobSetSpeed(mon, jobname, bandwidth);
}


GHashTable *
qemuMonitorGetAllBlockJobInfo(qemuMonitor *mon,
                              bool rawjobname)
{
    QEMU_CHECK_MONITOR_NULL(mon);
    return qemuMonitorJSONGetAllBlockJobInfo(mon, rawjobname);
}


int
qemuMonitorJobDismiss(qemuMonitor *mon,
                      const char *jobname)
{
    VIR_DEBUG("jobname=%s", jobname);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONJobDismiss(mon, jobname);
}


int
qemuMonitorJobFinalize(qemuMonitor *mon,
                       const char *jobname)
{
    VIR_DEBUG("jobname=%s", jobname);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONJobFinalize(mon, jobname);
}


int
qemuMonitorJobComplete(qemuMonitor *mon,
                       const char *jobname)
{
    VIR_DEBUG("jobname=%s", jobname);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONJobComplete(mon, jobname);
}


int
qemuMonitorSetBlockIoThrottle(qemuMonitor *mon,
                              const char *qomid,
                              virDomainBlockIoTuneInfo *info)
{
    VIR_DEBUG("qomid=%s, info=%p", NULLSTR(qomid), info);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetBlockIoThrottle(mon, qomid, info);
}


int
qemuMonitorGetBlockIoThrottle(qemuMonitor *mon,
                              const char *qdevid,
                              virDomainBlockIoTuneInfo *reply)
{
    VIR_DEBUG("qdevid=%s, reply=%p", NULLSTR(qdevid), reply);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetBlockIoThrottle(mon, qdevid, reply);
}


int
qemuMonitorThrottleGroupLimits(virJSONValue *limits,
                               const virDomainThrottleGroupDef *group)
{
    return qemuMonitorMakeThrottleGroupLimits(limits, group);
}


int
qemuMonitorUpdateThrottleGroup(qemuMonitor *mon,
                               const char *qomid,
                               virDomainBlockIoTuneInfo *info)
{
    VIR_DEBUG("qomid=%s, info=%p", qomid, info);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONUpdateThrottleGroup(mon, qomid, info);
}


int
qemuMonitorVMStatusToPausedReason(const char *status)
{
    int st;

    if (!status)
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    if ((st = qemuMonitorVMStatusTypeFromString(status)) < 0) {
        VIR_WARN("QEMU reported unknown VM status: '%s'", status);
        return VIR_DOMAIN_PAUSED_UNKNOWN;
    }

    switch ((qemuMonitorVMStatus) st) {
    case QEMU_MONITOR_VM_STATUS_DEBUG:
    case QEMU_MONITOR_VM_STATUS_INTERNAL_ERROR:
    case QEMU_MONITOR_VM_STATUS_RESTORE_VM:
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    case QEMU_MONITOR_VM_STATUS_INMIGRATE:
    case QEMU_MONITOR_VM_STATUS_POSTMIGRATE:
    case QEMU_MONITOR_VM_STATUS_FINISH_MIGRATE:
        return VIR_DOMAIN_PAUSED_MIGRATION;

    case QEMU_MONITOR_VM_STATUS_IO_ERROR:
        return VIR_DOMAIN_PAUSED_IOERROR;

    case QEMU_MONITOR_VM_STATUS_PAUSED:
    case QEMU_MONITOR_VM_STATUS_PRELAUNCH:
        return VIR_DOMAIN_PAUSED_USER;

    case QEMU_MONITOR_VM_STATUS_RUNNING:
        VIR_WARN("QEMU reports the guest is paused but status is 'running'");
        return VIR_DOMAIN_PAUSED_UNKNOWN;

    case QEMU_MONITOR_VM_STATUS_SAVE_VM:
        return VIR_DOMAIN_PAUSED_SAVE;

    case QEMU_MONITOR_VM_STATUS_SHUTDOWN:
        return VIR_DOMAIN_PAUSED_SHUTTING_DOWN;

    case QEMU_MONITOR_VM_STATUS_WATCHDOG:
        return VIR_DOMAIN_PAUSED_WATCHDOG;

    case QEMU_MONITOR_VM_STATUS_GUEST_PANICKED:
        return VIR_DOMAIN_PAUSED_CRASHED;

    /* unreachable from this point on */
    case QEMU_MONITOR_VM_STATUS_LAST:
        ;
    }
    return VIR_DOMAIN_PAUSED_UNKNOWN;
}


int
qemuMonitorOpenGraphics(qemuMonitor *mon,
                        const char *protocol,
                        int fd,
                        const char *fdname,
                        bool skipauth)
{
    int ret;

    VIR_DEBUG("protocol=%s fd=%d fdname=%s skipauth=%d",
              protocol, fd, NULLSTR(fdname), skipauth);

    QEMU_CHECK_MONITOR(mon);

    if (qemuMonitorSendFileHandle(mon, fdname, fd) < 0)
        return -1;

    ret = qemuMonitorJSONOpenGraphics(mon, protocol, fdname, skipauth);

    if (ret < 0) {
        if (qemuMonitorCloseFileHandle(mon, fdname) < 0)
            VIR_WARN("failed to close device handle '%s'", fdname);
    }

    return ret;
}


int
qemuMonitorSystemWakeup(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSystemWakeup(mon);
}


int
qemuMonitorGetVersion(qemuMonitor *mon,
                      int *major,
                      int *minor,
                      int *micro,
                      char **package)
{
    VIR_DEBUG("major=%p minor=%p micro=%p package=%p",
              major, minor, micro, package);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetVersion(mon, major, minor, micro, package);
}


int
qemuMonitorGetMachines(qemuMonitor *mon,
                       qemuMonitorMachineInfo ***machines)
{
    VIR_DEBUG("machines=%p", machines);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetMachines(mon, machines);
}


void
qemuMonitorMachineInfoFree(qemuMonitorMachineInfo *machine)
{
    if (!machine)
        return;
    g_free(machine->name);
    g_free(machine->alias);
    g_free(machine->defaultCPU);
    g_free(machine->defaultRAMid);
    g_free(machine);
}


int
qemuMonitorGetCPUDefinitions(qemuMonitor *mon,
                             qemuMonitorCPUDefs **cpuDefs)
{
    VIR_DEBUG("cpuDefs=%p", cpuDefs);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCPUDefinitions(mon, cpuDefs);
}


void
qemuMonitorCPUDefsFree(qemuMonitorCPUDefs *defs)
{
    size_t i;

    if (!defs)
        return;

    for (i = 0; i < defs->ncpus; i++) {
        g_strfreev(defs->cpus[i].blockers);
        g_free(defs->cpus[i].name);
        g_free(defs->cpus[i].type);
    }

    g_free(defs->cpus);
    g_free(defs);
}


qemuMonitorCPUDefs *
qemuMonitorCPUDefsNew(size_t count)
{
    g_autoptr(qemuMonitorCPUDefs) defs = NULL;

    defs = g_new0(qemuMonitorCPUDefs, 1);
    defs->cpus = g_new0(qemuMonitorCPUDefInfo, count);
    defs->ncpus = count;

    return g_steal_pointer(&defs);
}


qemuMonitorCPUDefs *
qemuMonitorCPUDefsCopy(qemuMonitorCPUDefs *src)
{
    g_autoptr(qemuMonitorCPUDefs) defs = NULL;
    size_t i;

    if (!src)
        return NULL;

    defs = qemuMonitorCPUDefsNew(src->ncpus);
    for (i = 0; i < src->ncpus; i++) {
        qemuMonitorCPUDefInfo *cpuDst = defs->cpus + i;
        qemuMonitorCPUDefInfo *cpuSrc = src->cpus + i;

        cpuDst->usable = cpuSrc->usable;
        cpuDst->name = g_strdup(cpuSrc->name);
        cpuDst->type = g_strdup(cpuSrc->type);
        cpuDst->blockers = g_strdupv(cpuSrc->blockers);
        cpuDst->deprecated = cpuSrc->deprecated;
    }

    return g_steal_pointer(&defs);
}


int
qemuMonitorGetCPUModelExpansion(qemuMonitor *mon,
                                qemuMonitorCPUModelExpansionType type,
                                virCPUDef *cpu,
                                bool migratable,
                                bool hv_passthrough,
                                bool fail_no_props,
                                qemuMonitorCPUModelInfo **model_info)
{
    VIR_DEBUG("type=%d cpu=%p migratable=%d", type, cpu, migratable);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCPUModelExpansion(mon, type, cpu,
                                               migratable, hv_passthrough,
                                               fail_no_props, model_info);
}


int
qemuMonitorGetCPUModelBaseline(qemuMonitor *mon,
                               virCPUDef *cpu_a,
                               virCPUDef *cpu_b,
                               qemuMonitorCPUModelInfo **baseline)
{
    VIR_DEBUG("cpu_a=%p cpu_b=%p", cpu_a, cpu_b);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCPUModelBaseline(mon, cpu_a, cpu_b, baseline);
}


int
qemuMonitorGetCPUModelComparison(qemuMonitor *mon,
                                 virCPUDef *cpu_a,
                                 virCPUDef *cpu_b,
                                 char **result)
{
    VIR_DEBUG("cpu_a=%p cpu_b=%p", cpu_a, cpu_b);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCPUModelComparison(mon, cpu_a, cpu_b, result);
}


void
qemuMonitorCPUModelInfoFree(qemuMonitorCPUModelInfo *model_info)
{
    size_t i;

    if (!model_info)
        return;

    for (i = 0; i < model_info->nprops; i++) {
        g_free(model_info->props[i].name);
        if (model_info->props[i].type == QEMU_MONITOR_CPU_PROPERTY_STRING)
            g_free(model_info->props[i].value.string);
    }

    g_strfreev(model_info->deprecated_props);
    g_free(model_info->props);
    g_free(model_info->name);
    g_free(model_info);
}


qemuMonitorCPUModelInfo *
qemuMonitorCPUModelInfoCopy(const qemuMonitorCPUModelInfo *orig)
{
    qemuMonitorCPUModelInfo *copy;
    size_t i;

    copy = g_new0(qemuMonitorCPUModelInfo, 1);

    copy->props = g_new0(qemuMonitorCPUProperty, orig->nprops);

    copy->name = g_strdup(orig->name);

    copy->migratability = orig->migratability;
    copy->nprops = orig->nprops;

    for (i = 0; i < orig->nprops; i++) {
        copy->props[i].name = g_strdup(orig->props[i].name);

        copy->props[i].migratable = orig->props[i].migratable;
        copy->props[i].type = orig->props[i].type;
        switch (orig->props[i].type) {
        case QEMU_MONITOR_CPU_PROPERTY_BOOLEAN:
            copy->props[i].value.boolean = orig->props[i].value.boolean;
            break;

        case QEMU_MONITOR_CPU_PROPERTY_STRING:
            copy->props[i].value.string = g_strdup(orig->props[i].value.string);
            break;

        case QEMU_MONITOR_CPU_PROPERTY_NUMBER:
            copy->props[i].value.number = orig->props[i].value.number;
            break;

        case QEMU_MONITOR_CPU_PROPERTY_LAST:
            break;
        }
    }

    copy->deprecated_props = g_strdupv(orig->deprecated_props);

    return copy;
}


GHashTable *
qemuMonitorGetCommandLineOptions(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONGetCommandLineOptions(mon);
}


int
qemuMonitorGetKVMState(qemuMonitor *mon,
                       bool *enabled,
                       bool *present)
{
    VIR_DEBUG("enabled=%p present=%p", enabled, present);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetKVMState(mon, enabled, present);
}


int
qemuMonitorGetObjectTypes(qemuMonitor *mon,
                          char ***types)
{
    VIR_DEBUG("types=%p", types);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetObjectTypes(mon, types);
}


GHashTable *
qemuMonitorGetDeviceProps(qemuMonitor *mon,
                          const char *device)
{
    VIR_DEBUG("device=%s", device);

    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONGetDeviceProps(mon, device);
}


int
qemuMonitorGetObjectProps(qemuMonitor *mon,
                          const char *object,
                          char ***props)
{
    VIR_DEBUG("object=%s props=%p", object, props);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetObjectProps(mon, object, props);
}


char *
qemuMonitorGetTargetArch(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONGetTargetArch(mon);
}


int
qemuMonitorGetMigrationCapabilities(qemuMonitor *mon,
                                    char ***capabilities)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetMigrationCapabilities(mon, capabilities);
}


/**
 * qemuMonitorSetMigrationCapabilities:
 * @mon: Pointer to the monitor object.
 * @caps: Migration capabilities.
 *
 * The @caps object is consumed cleared on success and some errors.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuMonitorSetMigrationCapabilities(qemuMonitor *mon,
                                    virJSONValue **caps)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetMigrationCapabilities(mon, caps);
}


/**
 * qemuMonitorGetGICCapabilities:
 * @mon: QEMU monitor
 * @capabilities: where to store the GIC capabilities
 *
 * See qemuMonitorJSONGetGICCapabilities().
 */
int
qemuMonitorGetGICCapabilities(qemuMonitor *mon,
                              virGICCapability **capabilities)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetGICCapabilities(mon, capabilities);
}


int
qemuMonitorGetSEVCapabilities(qemuMonitor *mon,
                              virSEVCapability **capabilities)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetSEVCapabilities(mon, capabilities);
}


int
qemuMonitorGetSGXCapabilities(qemuMonitor *mon,
                              virSGXCapability **capabilities)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetSGXCapabilities(mon, capabilities);
}


int
qemuMonitorNBDServerStart(qemuMonitor *mon,
                          const virStorageNetHostDef *server,
                          const char *tls_alias)
{
    /* Peek inside the struct for nicer logging */
    if (server->transport == VIR_STORAGE_NET_HOST_TRANS_TCP)
        VIR_DEBUG("server={tcp host=%s port=%u} tls_alias=%s",
                  NULLSTR(server->name), server->port, NULLSTR(tls_alias));
    else
        VIR_DEBUG("server={unix socket=%s} tls_alias=%s",
                  NULLSTR(server->socket), NULLSTR(tls_alias));

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONNBDServerStart(mon, server, tls_alias);
}


int
qemuMonitorNBDServerStop(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONNBDServerStop(mon);
}


int
qemuMonitorBlockExportAdd(qemuMonitor *mon,
                          virJSONValue **props)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockExportAdd(mon, props);
}


int
qemuMonitorGetTPMModels(qemuMonitor *mon,
                        char ***tpmmodels)
{
    VIR_DEBUG("tpmmodels=%p", tpmmodels);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetTPMModels(mon, tpmmodels);
}


int
qemuMonitorGetTPMTypes(qemuMonitor *mon,
                       char ***tpmtypes)
{
    VIR_DEBUG("tpmtypes=%p", tpmtypes);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetTPMTypes(mon, tpmtypes);
}


int
qemuMonitorAttachCharDev(qemuMonitor *mon,
                         virJSONValue **props,
                         char **ptypath)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONAttachCharDev(mon, props, ptypath);
}


int
qemuMonitorDetachCharDev(qemuMonitor *mon,
                         const char *chrID)
{
    VIR_DEBUG("chrID=%s", chrID);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONDetachCharDev(mon, chrID);
}


int
qemuMonitorGetDeviceAliases(qemuMonitor *mon,
                            char ***aliases)
{
    VIR_DEBUG("aliases=%p", aliases);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetDeviceAliases(mon, aliases);
}


/**
 * qemuMonitorSetDomainLogLocked:
 * @mon: Locked monitor object to set the log file reading on
 * @func: the callback to report errors
 * @opaque: data to pass to @func
 * @destroy: optional callback to free @opaque
 *
 * Set the file descriptor of the open VM log file to report potential
 * early startup errors of qemu. This function requires @mon to be
 * locked already!
 */
void
qemuMonitorSetDomainLogLocked(qemuMonitor *mon,
                              qemuMonitorReportDomainLogError func,
                              void *opaque,
                              virFreeCallback destroy)
{
    if (mon->logDestroy && mon->logOpaque)
        mon->logDestroy(mon->logOpaque);

    mon->logFunc = func;
    mon->logOpaque = opaque;
    mon->logDestroy = destroy;
}


/**
 * qemuMonitorSetDomainLog:
 * @mon: Unlocked monitor object to set the log file reading on
 * @func: the callback to report errors
 * @opaque: data to pass to @func
 * @destroy: optional callback to free @opaque
 *
 * Set the file descriptor of the open VM log file to report potential
 * early startup errors of qemu. This functions requires @mon to be
 * unlocked.
 */
void
qemuMonitorSetDomainLog(qemuMonitor *mon,
                        qemuMonitorReportDomainLogError func,
                        void *opaque,
                        virFreeCallback destroy)
{
    virObjectLock(mon);
    qemuMonitorSetDomainLogLocked(mon, func, opaque, destroy);
    virObjectUnlock(mon);
}


/**
 * qemuMonitorJSONGetGuestCPUx86:
 * @mon: Pointer to the monitor
 * @cpuQOMPath: QOM path of a CPU to probe
 * @data: returns the cpu data
 * @disabled: returns the CPU data for features which were disabled by QEMU
 *
 * Retrieve the definition of the guest CPU from a running qemu instance.
 *
 * Returns 0 on success, -2 if the operation is not supported by the guest,
 * -1 on other errors.
 */
int
qemuMonitorGetGuestCPUx86(qemuMonitor *mon,
                          const char *cpuQOMPath,
                          virCPUData **data,
                          virCPUData **disabled)
{
    VIR_DEBUG("cpuQOMPath=%s data=%p disabled=%p", cpuQOMPath, data, disabled);

    QEMU_CHECK_MONITOR(mon);

    *data = NULL;
    if (disabled)
        *disabled = NULL;

    return qemuMonitorJSONGetGuestCPUx86(mon, cpuQOMPath, data, disabled);
}


/**
 * qemuMonitorGetGuestCPU:
 * @mon: Pointer to the monitor
 * @arch: CPU architecture
 * @cpuQOMPath: QOM path of a CPU to probe
 * @translate: callback for translating CPU feature names from QEMU to libvirt
 * @opaque: data for @translate callback
 * @enabled: returns the CPU data for all enabled features
 * @disabled: returns the CPU data for features which we asked for
 *      (either explicitly or via a named CPU model) but QEMU disabled them
 *
 * Retrieve the definition of the guest CPU from a running QEMU instance.
 *
 * Returns 0 on success, -1 on error.
 */
int
qemuMonitorGetGuestCPU(qemuMonitor *mon,
                       virArch arch,
                       const char *cpuQOMPath,
                       qemuMonitorCPUFeatureTranslationCallback translate,
                       virCPUData **enabled,
                       virCPUData **disabled)
{
    VIR_DEBUG("arch=%s cpuQOMPath=%s translate=%p enabled=%p disabled=%p",
              virArchToString(arch), cpuQOMPath, translate, enabled, disabled);

    QEMU_CHECK_MONITOR(mon);

    *enabled = NULL;
    if (disabled)
        *disabled = NULL;

    return qemuMonitorJSONGetGuestCPU(mon, arch, cpuQOMPath, translate,
                                      enabled, disabled);
}


/**
 * qemuMonitorRTCResetReinjection:
 * @mon: Pointer to the monitor
 *
 * Issue rtc-reset-reinjection command.
 * This should be used in cases where guest time is restored via
 * guest agent, so RTC injection is not needed (in fact it would
 * confuse guest's RTC).
 *
 * Returns 0 on success
 *        -1 on error.
 */
int
qemuMonitorRTCResetReinjection(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONRTCResetReinjection(mon);
}


/**
 * qemuMonitorGetIOThreads:
 * @mon: Pointer to the monitor
 * @iothreads: Location to return array of IOThreadInfo data
 * @niothreads: Count of the number of IOThreads in the array
 *
 * Issue query-iothreads command.
 * Retrieve the list of iothreads defined/running for the machine
 *
 * Returns 0 on success
 *        -1 on error.
 */
int
qemuMonitorGetIOThreads(qemuMonitor *mon,
                        qemuMonitorIOThreadInfo ***iothreads,
                        int *niothreads)
{
    VIR_DEBUG("iothreads=%p", iothreads);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetIOThreads(mon, iothreads, niothreads);
}


/**
 * qemuMonitorSetIOThread:
 * @mon: Pointer to the monitor
 * @iothreadInfo: filled IOThread info with data
 *
 * Alter the specified IOThread's IOThreadInfo values.
 */
int
qemuMonitorSetIOThread(qemuMonitor *mon,
                       qemuMonitorIOThreadInfo *iothreadInfo)
{
    VIR_DEBUG("iothread=%p", iothreadInfo);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetIOThread(mon, iothreadInfo);
}


/**
 * qemuMonitorGetMemoryDeviceInfo:
 * @mon: pointer to the monitor
 * @info: Location to return the hash of qemuMonitorMemoryDeviceInfo
 *
 * Retrieve state and addresses of frontend memory devices present in
 * the guest.
 *
 * Returns: 0 on success and fills @info with a newly allocated struct,
 *         -1 otherwise.
 */
int
qemuMonitorGetMemoryDeviceInfo(qemuMonitor *mon,
                               GHashTable **info)
{
    g_autoptr(GHashTable) hash = virHashNew(g_free);
    int ret;

    VIR_DEBUG("info=%p", info);

    *info = NULL;

    QEMU_CHECK_MONITOR(mon);

    if ((ret = qemuMonitorJSONGetMemoryDeviceInfo(mon, hash)) >= 0)
        *info = g_steal_pointer(&hash);

    return ret;
}


int
qemuMonitorMigrateIncoming(qemuMonitor *mon,
                           const char *uri,
                           virTristateBool exitOnError)
{
    VIR_DEBUG("uri=%s, exitOnError=%s",
              uri, virTristateBoolTypeToString(exitOnError));

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrateIncoming(mon, uri, exitOnError);
}


int
qemuMonitorMigrateStartPostCopy(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrateStartPostCopy(mon);
}


int
qemuMonitorMigrateContinue(qemuMonitor *mon,
                           qemuMonitorMigrationStatus status)
{
    VIR_DEBUG("status=%s", qemuMonitorMigrationStatusTypeToString(status));

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrateContinue(mon, status);
}


int
qemuMonitorGetRTCTime(qemuMonitor *mon,
                      struct tm *tm)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetRTCTime(mon, tm);
}


virJSONValue *
qemuMonitorQueryQMPSchema(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONQueryQMPSchema(mon);
}


int
qemuMonitorSetBlockThreshold(qemuMonitor *mon,
                             const char *nodename,
                             unsigned long long threshold)
{
    VIR_DEBUG("node='%s', threshold=%llu", nodename, threshold);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetBlockThreshold(mon, nodename, threshold);
}


char *
qemuMonitorGuestPanicEventInfoFormatMsg(qemuMonitorEventPanicInfo *info)
{
    char *ret = NULL;

    switch (info->type) {
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_HYPERV:
        ret = g_strdup_printf("hyper-v: arg1='0x%llx', arg2='0x%llx', "
                              "arg3='0x%llx', arg4='0x%llx', arg5='0x%llx'",
                              info->data.hyperv.arg1, info->data.hyperv.arg2,
                              info->data.hyperv.arg3, info->data.hyperv.arg4,
                              info->data.hyperv.arg5);
        break;
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_S390:
        ret = g_strdup_printf("s390: core='%d' psw-mask='0x%016llx' "
                              "psw-addr='0x%016llx' reason='%s'",
                              info->data.s390.core,
                              info->data.s390.psw_mask,
                              info->data.s390.psw_addr,
                              info->data.s390.reason);
        break;
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_TDX:
        if (info->data.tdx.has_gpa)
            ret = g_strdup_printf("tdx: error_code='0x%x' message='%s' "
                                  "additional error information can be found "
                                  "at gpa page: '0x%016llx'",
                                  info->data.tdx.error_code,
                                  info->data.tdx.message,
                                  info->data.tdx.gpa);
        else
            ret = g_strdup_printf("tdx: error_code='0x%x' message='%s'",
                                  info->data.tdx.error_code,
                                  info->data.tdx.message);
        break;
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_NONE:
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_LAST:
        break;
    }

    return ret;
}


void
qemuMonitorEventPanicInfoFree(qemuMonitorEventPanicInfo *info)
{
    if (!info)
        return;

    switch (info->type) {
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_S390:
        g_free(info->data.s390.reason);
        break;
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_TDX:
        g_free(info->data.tdx.message);
        break;
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_NONE:
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_HYPERV:
    case QEMU_MONITOR_EVENT_PANIC_INFO_TYPE_LAST:
        break;
    }

    g_free(info);
}


void
qemuMonitorEventRdmaGidStatusFree(qemuMonitorRdmaGidStatus *info)
{
    if (!info)
        return;

    g_free(info->netdev);
    g_free(info);
}


void
qemuMonitorMemoryDeviceSizeChangeFree(qemuMonitorMemoryDeviceSizeChangePtr info)
{
    if (!info)
        return;

    g_free(info->devAlias);
}


int
qemuMonitorSetWatchdogAction(qemuMonitor *mon,
                             const char *action)
{
    VIR_DEBUG("watchdogAction=%s", action);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetWatchdogAction(mon, action);
}


/**
 * qemuMonitorBlockdevCreate:
 * @mon: monitor object
 * @jobname: name of the job
 * @props: JSON object describing the blockdev to add (consumed on success)
 *
 * Instructs qemu to create/format a new storage or format layer. Note that
 * the job does not add the created/formatted image into qemu and
 * qemuMonitorBlockdevAdd needs to be called separately with corresponding
 * arguments. Note that the arguments for creating and adding are different.
 */
int
qemuMonitorBlockdevCreate(qemuMonitor *mon,
                          const char *jobname,
                          virJSONValue **props)
{
    VIR_DEBUG("jobname=%s props=%p", jobname, props);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevCreate(mon, jobname, props);
}

/**
 * qemuMonitorBlockdevAdd:
 * @mon: monitor object
 * @props: JSON object describing the blockdev to add
 *
 * Adds a new block device (BDS) to qemu. Note that *@props is consumed
 * and set to NULL on success.
 */
int
qemuMonitorBlockdevAdd(qemuMonitor *mon,
                       virJSONValue **props)
{
    VIR_DEBUG("props=%p (node-name=%s)", *props,
              NULLSTR(virJSONValueObjectGetString(*props, "node-name")));

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevAdd(mon, props);
}


int
qemuMonitorBlockdevReopen(qemuMonitor *mon,
                          virJSONValue **options)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevReopen(mon, options);
}


int
qemuMonitorBlockdevDel(qemuMonitor *mon,
                       const char *nodename)
{
    VIR_DEBUG("nodename=%s", nodename);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevDel(mon, nodename);
}

int
qemuMonitorBlockdevTrayOpen(qemuMonitor *mon,
                            const char *id,
                            bool force)
{
    VIR_DEBUG("id=%s force=%d", id, force);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevTrayOpen(mon, id, force);
}


int
qemuMonitorBlockdevTrayClose(qemuMonitor *mon,
                             const char *id)
{
    VIR_DEBUG("id=%s", id);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevTrayClose(mon, id);
}


int
qemuMonitorBlockdevMediumRemove(qemuMonitor *mon,
                                const char *id)
{
    VIR_DEBUG("id=%s", id);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevMediumRemove(mon, id);
}


int
qemuMonitorBlockdevMediumInsert(qemuMonitor *mon,
                                const char *id,
                                const char *nodename)
{
    VIR_DEBUG("id=%s nodename=%s", id, nodename);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBlockdevMediumInsert(mon, id, nodename);
}


char *
qemuMonitorGetSEVMeasurement(qemuMonitor *mon)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONGetSEVMeasurement(mon);
}


int
qemuMonitorGetSEVInfo(qemuMonitor *mon,
                      qemuMonitorSEVInfo *info)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetSEVInfo(mon, info);
}


int
qemuMonitorSetLaunchSecurityState(qemuMonitor *mon,
                                  const char *secrethdr,
                                  const char *secret,
                                  unsigned long long setaddr,
                                  bool hasSetaddr)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetLaunchSecurityState(mon, secrethdr, secret,
                                                 setaddr, hasSetaddr);
}


int
qemuMonitorGetPRManagerInfo(qemuMonitor *mon,
                            GHashTable **retinfo)
{
    g_autoptr(GHashTable) info = virHashNew(g_free);

    *retinfo = NULL;

    QEMU_CHECK_MONITOR(mon);

    if (qemuMonitorJSONGetPRManagerInfo(mon, info) < 0)
        return -1;

    *retinfo = g_steal_pointer(&info);
    return 0;
}


int
qemuMonitorGetCurrentMachineInfo(qemuMonitor *mon,
                                 qemuMonitorCurrentMachineInfo *info)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCurrentMachineInfo(mon, info);
}


void
qemuMonitorJobInfoFree(qemuMonitorJobInfo *job)
{
    if (!job)
        return;

    g_free(job->id);
    g_free(job->error);
    g_free(job);
}


int
qemuMonitorGetJobInfo(qemuMonitor *mon,
                      qemuMonitorJobInfo ***jobs,
                      size_t *njobs)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetJobInfo(mon, jobs, njobs);
}


/* qemuMonitorGetCPUMigratable:
 *
 * Get the migratable property of the CPU object.
 *
 * Returns -1 on error,
 *          1 when the property is not supported,
 *          0 on success (@migratable is set accordingly).
 */
int
qemuMonitorGetCPUMigratable(qemuMonitor *mon,
                            const char *cpuQOMPath,
                            bool *migratable)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetCPUMigratable(mon, cpuQOMPath, migratable);
}


int
qemuMonitorTransactionBitmapAdd(virJSONValue *actions,
                                const char *node,
                                const char *name,
                                bool persistent,
                                bool disabled,
                                unsigned long long granularity)
{
    return qemuMonitorJSONTransactionBitmapAdd(actions, node, name, persistent,
                                               disabled, granularity);
}


int
qemuMonitorTransactionBitmapRemove(virJSONValue *actions,
                                   const char *node,
                                   const char *name)
{
    return qemuMonitorJSONTransactionBitmapRemove(actions, node, name);
}


int
qemuMonitorBitmapRemove(qemuMonitor *mon,
                        const char *node,
                        const char *name)
{
    VIR_DEBUG("node='%s', name='%s'", node, name);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONBitmapRemove(mon, node, name);
}


int
qemuMonitorTransactionBitmapEnable(virJSONValue *actions,
                                   const char *node,
                                   const char *name)
{
    return qemuMonitorJSONTransactionBitmapEnable(actions, node, name);
}


int
qemuMonitorTransactionBitmapDisable(virJSONValue *actions,
                                    const char *node,
                                    const char *name)
{
    return qemuMonitorJSONTransactionBitmapDisable(actions, node, name);
}


int
qemuMonitorTransactionBitmapMerge(virJSONValue *actions,
                                  const char *node,
                                  const char *target,
                                  virJSONValue **sources)
{
    return qemuMonitorJSONTransactionBitmapMerge(actions, node, target, sources);
}


int
qemuMonitorTransactionBitmapMergeSourceAddBitmap(virJSONValue *sources,
                                                 const char *sourcenode,
                                                 const char *sourcebitmap)
{
    return qemuMonitorJSONTransactionBitmapMergeSourceAddBitmap(sources, sourcenode, sourcebitmap);
}


int
qemuMonitorTransactionSnapshotBlockdev(virJSONValue *actions,
                                       const char *node,
                                       const char *overlay)
{
    return qemuMonitorJSONTransactionSnapshotBlockdev(actions, node, overlay);
}


int
qemuMonitorTransactionBackup(virJSONValue *actions,
                             const char *device,
                             const char *jobname,
                             const char *target,
                             const char *bitmap,
                             qemuMonitorTransactionBackupSyncMode syncmode)
{
    return qemuMonitorJSONTransactionBackup(actions, device, jobname, target,
                                            bitmap, syncmode);
}


VIR_ENUM_IMPL(qemuMonitorDirtyRateCalcMode,
              QEMU_MONITOR_DIRTYRATE_CALC_MODE_LAST,
              "page-sampling",
              "dirty-bitmap",
              "dirty-ring",
);


int
qemuMonitorStartDirtyRateCalc(qemuMonitor *mon,
                              int seconds,
                              qemuMonitorDirtyRateCalcMode mode)
{
    VIR_DEBUG("seconds=%d", seconds);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONStartDirtyRateCalc(mon, seconds, mode);
}


int
qemuMonitorQueryDirtyRate(qemuMonitor *mon,
                          qemuMonitorDirtyRateInfo *info)
{
    VIR_DEBUG("info=%p", info);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONQueryDirtyRate(mon, info);
}


int
qemuMonitorSetAction(qemuMonitor *mon,
                     qemuMonitorActionShutdown shutdown,
                     qemuMonitorActionReboot reboot,
                     qemuMonitorActionWatchdog watchdog,
                     qemuMonitorActionPanic panic)
{
    VIR_DEBUG("shutdown=%u, reboot=%u, watchdog=%u panic=%u",
              shutdown, reboot, watchdog, panic);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONSetAction(mon, shutdown, reboot, watchdog, panic);
}


int
qemuMonitorChangeMemoryRequestedSize(qemuMonitor *mon,
                                     const char *alias,
                                     unsigned long long requestedsize)
{
    VIR_DEBUG("alias=%s requestedsize=%llu", alias, requestedsize);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONChangeMemoryRequestedSize(mon, alias, requestedsize);
}


int
qemuMonitorMigrateRecover(qemuMonitor *mon,
                          const char *uri)
{
    VIR_DEBUG("uri=%s", uri);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONMigrateRecover(mon, uri);
}


int
qemuMonitorGetMigrationBlockers(qemuMonitor *mon,
                                char ***blockers)
{
    VIR_DEBUG("blockers=%p", blockers);

    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONGetMigrationBlockers(mon, blockers);
}


VIR_ENUM_IMPL(qemuMonitorQueryStatsTarget,
              QEMU_MONITOR_QUERY_STATS_TARGET_LAST,
              "vm",
              "vcpu",
);


VIR_ENUM_IMPL(qemuMonitorQueryStatsName,
              QEMU_MONITOR_QUERY_STATS_NAME_LAST,
              "halt_poll_success_ns",
              "halt_poll_fail_ns",
);


VIR_ENUM_IMPL(qemuMonitorQueryStatsProvider,
              QEMU_MONITOR_QUERY_STATS_PROVIDER_LAST,
              "kvm",
);


void
qemuMonitorQueryStatsProviderFree(qemuMonitorQueryStatsProvider *provider)
{
    virBitmapFree(provider->names);
    g_free(provider);
}


qemuMonitorQueryStatsProvider *
qemuMonitorQueryStatsProviderNew(qemuMonitorQueryStatsProviderType provider_type,
                                 ...)
{
    qemuMonitorQueryStatsProvider *provider = g_new0(qemuMonitorQueryStatsProvider, 1);
    qemuMonitorQueryStatsNameType stat;
    va_list name_list;

    /*
     * This can be lowered later in case of the enum getting quite large, hence
     *  the virBitmapSetExpand below which also incidentally makes this function
     *  non-fallible.
     */
    provider->names = virBitmapNew(QEMU_MONITOR_QUERY_STATS_NAME_LAST);
    provider->type = provider_type;

    va_start(name_list, provider_type);

    while ((stat = va_arg(name_list, qemuMonitorQueryStatsNameType)) !=
           QEMU_MONITOR_QUERY_STATS_NAME_LAST)
        virBitmapSetBitExpand(provider->names, stat);

    va_end(name_list);

    return provider;
}


VIR_ENUM_IMPL(qemuMonitorQueryStatsUnit,
              QEMU_MONITOR_QUERY_STATS_UNIT_LAST,
              "bytes",
              "seconds",
              "cycles",
              "boolean",
);


VIR_ENUM_IMPL(qemuMonitorQueryStatsType,
              QEMU_MONITOR_QUERY_STATS_TYPE_LAST,
              "cumulative",
              "instant",
              "peak",
              "linear-histogram",
              "log2-histogram",
);


GHashTable *
qemuMonitorQueryStatsSchema(qemuMonitor *mon,
                            qemuMonitorQueryStatsProviderType provider_type)
{
    QEMU_CHECK_MONITOR_NULL(mon);

    return qemuMonitorJSONQueryStatsSchema(mon, provider_type);
}


virJSONValue *
qemuMonitorQueryStats(qemuMonitor *mon,
                      qemuMonitorQueryStatsTargetType target,
                      char **vcpus,
                      GPtrArray *providers)
{
    VIR_DEBUG("target=%u vcpus=%p providers=%p", target, vcpus, providers);

    QEMU_CHECK_MONITOR_NULL(mon);

    if (target != QEMU_MONITOR_QUERY_STATS_TARGET_VCPU && vcpus)
        return NULL;

    return qemuMonitorJSONQueryStats(mon, target, vcpus, providers);
}


/**
 * qemuMonitorExtractQueryStats:
 * @info: One of the array members returned by qemuMonitorQueryStat
 *
 * Converts all the statistics into a GHashTable similar to virQEMU
 * except only object with the key "value" is stored as the value i
 *
 * Returns NULL on failure.
 */
GHashTable *
qemuMonitorExtractQueryStats(virJSONValue *info)
{
    g_autoptr(GHashTable) hash_table = NULL;
    virJSONValue *stats = NULL;
    size_t i;

    if (!virJSONValueIsObject(info))
        return NULL;

    stats = virJSONValueObjectGetArray(info, "stats");
    if (!stats)
        return NULL;

    hash_table = virHashNew(virJSONValueHashFree);

    for (i = 0; i < virJSONValueArraySize(stats); i++) {
        virJSONValue *stat = virJSONValueArrayGet(stats, i);
        virJSONValue *value = NULL;
        const char *name = NULL;

        if (!virJSONValueIsObject(stat))
            continue;

        name = virJSONValueObjectGetString(stat, "name");
        if (!name)
            continue;

        if (virJSONValueObjectRemoveKey(stat, "value", &value) < 0)
            continue;

        virHashAddEntry(hash_table, name, value);
    }

    return g_steal_pointer(&hash_table);
}


/**
 * qemuMonitorStatsSchemaByQOMPath:
 * @arr: Array of objects returned by qemuMonitorQueryStats
 *
 * Returns the object which matches the QOM path of the vCPU
 *
 * Returns NULL on failure.
 */
virJSONValue *
qemuMonitorGetStatsByQOMPath(virJSONValue *arr,
                             char *qom_path)
{
    size_t i;

    if (!virJSONValueIsArray(arr) || !qom_path)
        return NULL;

    for (i = 0; i < virJSONValueArraySize(arr); i++) {
        virJSONValue *obj = virJSONValueArrayGet(arr, i);
        const char *test_qom_path = NULL;

        if (!obj)
            return NULL;

        test_qom_path = virJSONValueObjectGetString(obj, "qom-path");
        if (!test_qom_path)
            return NULL;

        if (STRCASEEQ(qom_path, test_qom_path))
            return obj;
    }

    return NULL;
}

int
qemuMonitorDisplayReload(qemuMonitor *mon,
                         const char *type,
                         bool tlsCerts)
{
    QEMU_CHECK_MONITOR(mon);

    return qemuMonitorJSONDisplayReload(mon, type, tlsCerts);
}


/**
 * qemuMonitorBlockdevSetActive:
 * @mon: monitor object
 * @nodename: optional nodename to (de)activate
 * @active: requested state
 *
 * Activate or deactivate @nodename based on @active. If @nodename is NULL,
 * qemu will act on all block nodes.
 */
int
qemuMonitorBlockdevSetActive(qemuMonitor *mon,
                             const char *nodename,
                             bool active)
{
    QEMU_CHECK_MONITOR(mon);
    VIR_DEBUG("nodename='%s', active='%d'", NULLSTR(nodename), active);

    return qemuMonitorJSONBlockdevSetActive(mon, nodename, active);
}
