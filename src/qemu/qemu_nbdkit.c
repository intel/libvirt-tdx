/*
 * qemu_nbdkit.c: helpers for using nbdkit
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
 *
 */

#include <config.h>
#include <glib.h>

#include "configmake.h"
#include "vircommand.h"
#include "virerror.h"
#include "virlog.h"
#include "virpidfile.h"
#include "virutil.h"
#include "qemu_block.h"
#include "qemu_conf.h"
#include "qemu_domain.h"
#include "qemu_extdevice.h"
#include "qemu_nbdkit.h"
#include "qemu_security.h"

#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.nbdkit");

#define NBDKIT_MODDIR LIBDIR "/nbdkit"
#define NBDKIT_PLUGINDIR NBDKIT_MODDIR "/plugins"
#define NBDKIT_FILTERDIR NBDKIT_MODDIR "/filters"

VIR_ENUM_IMPL(qemuNbdkitCaps,
    QEMU_NBDKIT_CAPS_LAST,
    /* 0 */
    "plugin-curl", /* QEMU_NBDKIT_CAPS_PLUGIN_CURL */
    "plugin-ssh", /* QEMU_NBDKIT_CAPS_PLUGIN_SSH */
    "filter-readahead", /* QEMU_NBDKIT_CAPS_FILTER_READAHEAD */
);

struct _qemuNbdkitCaps {
    GObject parent;

    char *path;
    char *version;
    time_t ctime;
    time_t libvirtCtime;
    time_t pluginDirMtime;
    time_t filterDirMtime;
    unsigned int libvirtVersion;

    virBitmap *flags;
};
G_DEFINE_TYPE(qemuNbdkitCaps, qemu_nbdkit_caps, G_TYPE_OBJECT);


static void
qemuNbdkitCheckCommandCap(qemuNbdkitCaps *nbdkit,
                          virCommand *cmd,
                          qemuNbdkitCapsFlags cap)
{
    if (virCommandRun(cmd, NULL) != 0)
        return;

    VIR_DEBUG("Setting nbdkit capability %i", cap);
    ignore_value(virBitmapSetBit(nbdkit->flags, cap));
}


static void
qemuNbdkitQueryFilter(qemuNbdkitCaps *nbdkit,
                      const char *filter,
                      qemuNbdkitCapsFlags cap)
{
    g_autoptr(virCommand) cmd = virCommandNewArgList(nbdkit->path,
                                                     "--version",
                                                     NULL);

    virCommandAddArgPair(cmd, "--filter", filter);

    /* use null plugin to check for filter */
    virCommandAddArg(cmd, "null");

    qemuNbdkitCheckCommandCap(nbdkit, cmd, cap);
}


static void
qemuNbdkitQueryPlugin(qemuNbdkitCaps *nbdkit,
                      const char *plugin,
                      qemuNbdkitCapsFlags cap)
{
    g_autoptr(virCommand) cmd = virCommandNewArgList(nbdkit->path,
                                                     plugin,
                                                     "--version",
                                                     NULL);

    qemuNbdkitCheckCommandCap(nbdkit, cmd, cap);
}


static void
qemuNbdkitCapsQueryPlugins(qemuNbdkitCaps *nbdkit)
{
    qemuNbdkitQueryPlugin(nbdkit, "curl", QEMU_NBDKIT_CAPS_PLUGIN_CURL);
    qemuNbdkitQueryPlugin(nbdkit, "ssh", QEMU_NBDKIT_CAPS_PLUGIN_SSH);
}


static void
qemuNbdkitCapsQueryFilters(qemuNbdkitCaps *nbdkit)
{
    qemuNbdkitQueryFilter(nbdkit, "readahead",
                          QEMU_NBDKIT_CAPS_FILTER_READAHEAD);
}


static int
qemuNbdkitCapsQueryVersion(qemuNbdkitCaps *nbdkit)
{
    g_autoptr(virCommand) cmd = virCommandNewArgList(nbdkit->path,
                                                     "--version",
                                                     NULL);

    virCommandSetOutputBuffer(cmd, &nbdkit->version);

    if (virCommandRun(cmd, NULL) != 0)
        return -1;

    VIR_DEBUG("Got nbdkit version %s", nbdkit->version);
    return 0;
}


static void
qemuNbdkitCapsFinalize(GObject *object)
{
    qemuNbdkitCaps *nbdkit = QEMU_NBDKIT_CAPS(object);

    g_clear_pointer(&nbdkit->path, g_free);
    g_clear_pointer(&nbdkit->version, g_free);
    g_clear_pointer(&nbdkit->flags, virBitmapFree);

    G_OBJECT_CLASS(qemu_nbdkit_caps_parent_class)->finalize(object);
}


void
qemu_nbdkit_caps_init(qemuNbdkitCaps *caps)
{
    caps->flags = virBitmapNew(QEMU_NBDKIT_CAPS_LAST);
    caps->version = NULL;
}


static void
qemu_nbdkit_caps_class_init(qemuNbdkitCapsClass *klass)
{
    GObjectClass *obj = G_OBJECT_CLASS(klass);

    obj->finalize = qemuNbdkitCapsFinalize;
}


qemuNbdkitCaps *
qemuNbdkitCapsNew(const char *path)
{
    qemuNbdkitCaps *caps = g_object_new(QEMU_TYPE_NBDKIT_CAPS, NULL);
    caps->path = g_strdup(path);

    return caps;
}


static time_t
qemuNbdkitGetDirMtime(const char *moddir)
{
    struct stat st;

    if (stat(moddir, &st) < 0) {
        VIR_DEBUG("Failed to stat nbdkit module directory '%s': %s",
                  moddir,
                  g_strerror(errno));
        return 0;
    }

    return st.st_mtime;
}


static void
qemuNbdkitCapsQuery(qemuNbdkitCaps *caps)
{
    struct stat st;

    if (stat(caps->path, &st) < 0) {
        VIR_DEBUG("Failed to stat nbdkit binary '%s': %s",
                  caps->path,
                  g_strerror(errno));
        caps->ctime  = 0;
        return;
    }

    caps->ctime = st.st_ctime;
    caps->filterDirMtime = qemuNbdkitGetDirMtime(NBDKIT_FILTERDIR);
    caps->pluginDirMtime = qemuNbdkitGetDirMtime(NBDKIT_PLUGINDIR);
    caps->libvirtCtime = virGetSelfLastChanged();
    caps->libvirtVersion = LIBVIR_VERSION_NUMBER;

    qemuNbdkitCapsQueryPlugins(caps);
    qemuNbdkitCapsQueryFilters(caps);
    qemuNbdkitCapsQueryVersion(caps);
}


bool
qemuNbdkitCapsGet(qemuNbdkitCaps *nbdkitCaps,
                  qemuNbdkitCapsFlags flag)
{
    return virBitmapIsBitSet(nbdkitCaps->flags, flag);
}


void
qemuNbdkitCapsSet(qemuNbdkitCaps *nbdkitCaps,
                  qemuNbdkitCapsFlags flag)
{
    ignore_value(virBitmapSetBit(nbdkitCaps->flags, flag));
}


static bool
virNbkditCapsCheckModdir(const char *moddir,
                         time_t expectedMtime)
{
    time_t mtime = qemuNbdkitGetDirMtime(moddir);

    if (mtime != expectedMtime) {
        VIR_DEBUG("Outdated capabilities for nbdkit: module directory '%s' changed (%lld vs %lld)",
                  moddir, (long long)mtime, (long long)expectedMtime);
        return false;
    }
    return true;
}


static bool
virNbdkitCapsIsValid(void *data,
                     void *privData G_GNUC_UNUSED)
{
    qemuNbdkitCaps *nbdkitCaps = data;
    struct stat st;

    if (!nbdkitCaps->path)
        return true;

    if (!virNbkditCapsCheckModdir(NBDKIT_PLUGINDIR, nbdkitCaps->pluginDirMtime))
        return false;
    if (!virNbkditCapsCheckModdir(NBDKIT_FILTERDIR, nbdkitCaps->filterDirMtime))
        return false;

    if (nbdkitCaps->libvirtCtime != virGetSelfLastChanged() ||
        nbdkitCaps->libvirtVersion != LIBVIR_VERSION_NUMBER) {
        VIR_DEBUG("Outdated capabilities for '%s': libvirt changed (%lld vs %lld, %lu vs %lu)",
                  nbdkitCaps->path,
                  (long long)nbdkitCaps->libvirtCtime,
                  (long long)virGetSelfLastChanged(),
                  (unsigned long)nbdkitCaps->libvirtVersion,
                  (unsigned long)LIBVIR_VERSION_NUMBER);
        return false;
    }

    if (stat(nbdkitCaps->path, &st) < 0) {
        VIR_DEBUG("Failed to stat nbdkit binary '%s': %s",
                  nbdkitCaps->path,
                  g_strerror(errno));
        return false;
    }

    if (st.st_ctime != nbdkitCaps->ctime) {
        VIR_DEBUG("Outdated capabilities for '%s': nbdkit binary changed (%lld vs %lld)",
                  nbdkitCaps->path,
                  (long long)st.st_ctime, (long long)nbdkitCaps->ctime);
        return false;
    }

    return true;
}


static void*
virNbdkitCapsNewData(const char *binary,
                     void *privData G_GNUC_UNUSED)
{
    qemuNbdkitCaps *caps = qemuNbdkitCapsNew(binary);
    qemuNbdkitCapsQuery(caps);

    return caps;
}


static int
qemuNbdkitCapsValidateBinary(qemuNbdkitCaps *nbdkitCaps,
                             xmlXPathContextPtr ctxt)
{
    g_autofree char *str = NULL;

    if (!(str = virXPathString("string(./path)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("missing path in nbdkit capabilities cache"));
        return -1;
    }

    if (STRNEQ(str, nbdkitCaps->path)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Expected caps for '%1$s' but saw '%2$s'"),
                       nbdkitCaps->path, str);
        return -1;
    }

    return 0;
}


static int
qemuNbdkitCapsParseFlags(qemuNbdkitCaps *nbdkitCaps,
                         xmlXPathContextPtr ctxt)
{
    g_autofree xmlNodePtr *nodes = NULL;
    size_t i;
    int n;

    if ((n = virXPathNodeSet("./flag", ctxt, &nodes)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse qemu capabilities flags"));
        return -1;
    }

    VIR_DEBUG("Got flags %d", n);
    for (i = 0; i < n; i++) {
        unsigned int flag;

        if (virXMLPropEnum(nodes[i], "name", qemuNbdkitCapsTypeFromString,
                           VIR_XML_PROP_REQUIRED, &flag) < 0)
            return -1;

        qemuNbdkitCapsSet(nbdkitCaps, flag);
    }

    return 0;
}


/*
 * Parsing a doc that looks like
 *
 * <nbdkitCaps>
 *   <path>/some/path</path>
 *   <nbdkitctime>234235253</nbdkitctime>
 *   <plugindirmtime>234235253</plugindirmtime>
 *   <filterdirmtime>234235253</filterdirmtime>
 *   <selfctime>234235253</selfctime>
 *   <selfvers>1002016</selfvers>
 *   <flag name='foo'/>
 *   <flag name='bar'/>
 *   ...
 * </nbdkitCaps>
 *
 * Returns 0 on success, 1 if outdated, -1 on error
 */
static int
qemuNbdkitCapsLoadCache(qemuNbdkitCaps *nbdkitCaps,
                        const char *filename)
{
    g_autoptr(xmlDoc) doc = NULL;
    g_autoptr(xmlXPathContext) ctxt = NULL;
    long long int l;

    if (!(doc = virXMLParse(filename, NULL, NULL, "nbdkitCaps", &ctxt, NULL, false)))
        return -1;

    if (virXPathLongLong("string(./selfctime)", ctxt, &l) < 0) {
        VIR_DEBUG("missing selfctime in nbdkit capabilities XML");
        return -1;
    }
    nbdkitCaps->libvirtCtime = (time_t)l;

    nbdkitCaps->libvirtVersion = 0;
    virXPathUInt("string(./selfvers)", ctxt, &nbdkitCaps->libvirtVersion);

    if (nbdkitCaps->libvirtCtime != virGetSelfLastChanged() ||
        nbdkitCaps->libvirtVersion != LIBVIR_VERSION_NUMBER) {
        VIR_DEBUG("Outdated capabilities in %s: libvirt changed (%lld vs %lld, %lu vs %lu), stopping load",
                  nbdkitCaps->path,
                  (long long)nbdkitCaps->libvirtCtime,
                  (long long)virGetSelfLastChanged(),
                  (unsigned long)nbdkitCaps->libvirtVersion,
                  (unsigned long)LIBVIR_VERSION_NUMBER);
        return 1;
    }

    if (qemuNbdkitCapsValidateBinary(nbdkitCaps, ctxt) < 0)
        return -1;

    if (virXPathLongLong("string(./nbdkitctime)", ctxt, &l) < 0) {
        VIR_DEBUG("missing nbdkitctime in nbdkit capabilities XML");
        return -1;
    }
    nbdkitCaps->ctime = (time_t)l;

    if (virXPathLongLong("string(./plugindirmtime)", ctxt, &l) < 0) {
        VIR_DEBUG("missing plugindirmtime in nbdkit capabilities XML");
        return -1;
    }
    nbdkitCaps->pluginDirMtime = (time_t)l;

    if (virXPathLongLong("string(./filterdirmtime)", ctxt, &l) < 0) {
        VIR_DEBUG("missing filterdirmtime in nbdkit capabilities XML");
        return -1;
    }
    nbdkitCaps->filterDirMtime = (time_t)l;

    if (qemuNbdkitCapsParseFlags(nbdkitCaps, ctxt) < 0)
        return -1;

    if ((nbdkitCaps->version = virXPathString("string(./version)", ctxt)) == NULL) {
        VIR_DEBUG("missing version in nbdkit capabilities cache");
        return -1;
    }

    return 0;
}


static void*
virNbdkitCapsLoadFile(const char *filename,
                      const char *binary,
                      void *privData G_GNUC_UNUSED,
                      bool *outdated)
{
    g_autoptr(qemuNbdkitCaps) nbdkitCaps = qemuNbdkitCapsNew(binary);
    int ret;

    if (!nbdkitCaps)
        return NULL;

    ret = qemuNbdkitCapsLoadCache(nbdkitCaps, filename);
    if (ret < 0)
        return NULL;
    if (ret == 1) {
        *outdated = true;
        return NULL;
    }

    return g_steal_pointer(&nbdkitCaps);
}


static char*
qemuNbdkitCapsFormatCache(qemuNbdkitCaps *nbdkitCaps)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    size_t i;

    virBufferAddLit(&buf, "<nbdkitCaps>\n");
    virBufferAdjustIndent(&buf, 2);

    virBufferEscapeString(&buf, "<path>%s</path>\n",
                          nbdkitCaps->path);
    virBufferAsprintf(&buf, "<nbdkitctime>%lu</nbdkitctime>\n",
                      nbdkitCaps->ctime);
    virBufferAsprintf(&buf, "<plugindirmtime>%lu</plugindirmtime>\n",
                      nbdkitCaps->pluginDirMtime);
    virBufferAsprintf(&buf, "<filterdirmtime>%lu</filterdirmtime>\n",
                      nbdkitCaps->filterDirMtime);
    virBufferAsprintf(&buf, "<selfctime>%lu</selfctime>\n",
                      nbdkitCaps->libvirtCtime);
    virBufferAsprintf(&buf, "<selfvers>%u</selfvers>\n",
                      nbdkitCaps->libvirtVersion);

    for (i = 0; i < QEMU_NBDKIT_CAPS_LAST; i++) {
        if (qemuNbdkitCapsGet(nbdkitCaps, i)) {
            virBufferAsprintf(&buf, "<flag name='%s'/>\n",
                              qemuNbdkitCapsTypeToString(i));
        }
    }

    virBufferAsprintf(&buf, "<version>%s</version>\n",
                      nbdkitCaps->version);

    virBufferAdjustIndent(&buf, -2);
    virBufferAddLit(&buf, "</nbdkitCaps>\n");

    return virBufferContentAndReset(&buf);
}


static int
virNbdkitCapsSaveFile(void *data,
                      const char *filename,
                      void *privData G_GNUC_UNUSED)
{
    qemuNbdkitCaps *nbdkitCaps = data;
    g_autofree char *xml = NULL;

    xml = qemuNbdkitCapsFormatCache(nbdkitCaps);

    if (virFileWriteStr(filename, xml, 0600) < 0) {
        virReportSystemError(errno,
                             _("Failed to save '%1$s' for '%2$s'"),
                             filename, nbdkitCaps->path);
        return -1;
    }

    VIR_DEBUG("Saved caps '%s' for '%s' with (%lu, %lu)",
              filename, nbdkitCaps->path,
              nbdkitCaps->ctime,
              nbdkitCaps->libvirtCtime);

    return 0;
}


virFileCacheHandlers nbdkitCapsCacheHandlers = {
    .isValid = virNbdkitCapsIsValid,
    .newData = virNbdkitCapsNewData,
    .loadFile = virNbdkitCapsLoadFile,
    .saveFile = virNbdkitCapsSaveFile,
    .privFree = NULL,
};


virFileCache*
qemuNbdkitCapsCacheNew(const char *cachedir)
{
    g_autofree char *dir = g_build_filename(cachedir, "nbdkitcapabilities", NULL);
    return virFileCacheNew(dir, "xml", &nbdkitCapsCacheHandlers);
}