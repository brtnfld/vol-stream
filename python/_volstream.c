/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The CPython extension behind the `volstream` package: a subscriber-only
 * binding over the connector's own C API (docs/python-plan.md).
 *
 * Deliberately thin. Each RawFile method is one connector call; the per-step
 * reassembly lives in the Python layer. No hid_t crosses into Python: types
 * come back as (kind, size, byte order) and extents as tuples, and a RawFile
 * owns its file id and FAPL.
 *
 * A pushed payload comes back as a Buffer that owns the malloc()'d bytes
 * H5Fget_subscribed_data() returned and frees them when the last view is
 * released. NumPy wraps it with np.frombuffer() without copying. It is also
 * the one place a different allocator or a device-memory interface would go
 * (python-plan.md, "The Phase 3 seam").
 *
 * Threads. Every HDF5 call runs with the GIL released and g_hdf5_lock held,
 * so other Python threads run while a call blocks, but never enter HDF5 at
 * the same time -- a stock HDF5 build is not thread-safe. The lock is taken
 * only after the GIL is released and dropped before it is retaken, so the two
 * can never be waited on in the opposite order. Nothing that touches a
 * Python object runs while g_hdf5_lock is held.
 *
 * Signals. Blocking calls wait in slices of at most POLL_MS and check for
 * pending signals between slices, so Ctrl-C interrupts a long wait.
 *
 * fork(). The transport's threads do not survive fork(), so a RawFile may be
 * used only by the process that opened it, and open() refuses to run in a
 * child of the process that first registered the connector. In a child,
 * close() and garbage collection drop the ids without calling HDF5.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pythread.h>

#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "hdf5.h"
#include "H5VLstream.h"

#define POLL_MS 100

static PyObject          *VolstreamError;
static PyThread_type_lock g_hdf5_lock;

/* Registered on first open and kept for the life of the process. */
static hid_t g_vol_id   = H5I_INVALID_HID;
static pid_t g_init_pid = 0;

/* Brackets every HDF5 call: GIL released, g_hdf5_lock held, HDF5's automatic
 * error printing off. The body must not touch Python objects. */
#define HDF5_BEGIN                                                                                            \
    Py_BEGIN_ALLOW_THREADS PyThread_acquire_lock(g_hdf5_lock, WAIT_LOCK);                                     \
    H5E_BEGIN_TRY                                                                                             \
    {
#define HDF5_END                                                                                              \
    }                                                                                                         \
    H5E_END_TRY                                                                                               \
    PyThread_release_lock(g_hdf5_lock);                                                                       \
    Py_END_ALLOW_THREADS

static uint64_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* A timed call that fails well inside its slice did not time out -- the
 * transport is unavailable, say -- so waiting out the deadline would only spin. */
static int
failed_fast(uint64_t t0, uint64_t slice)
{
    return slice > 0 && now_ms() - t0 < slice / 2;
}

/* ---------------------------------------------------------------- errors -- */

typedef struct {
    char desc[256];
    int  found;
} hdf5_err_t;

static herr_t
innermost_cb(unsigned n, const H5E_error2_t *err, void *udata)
{
    hdf5_err_t *out = (hdf5_err_t *)udata;

    (void)n;
    if (!out->found && err->desc && err->desc[0]) {
        snprintf(out->desc, sizeof(out->desc), "%s", err->desc);
        out->found = 1;
    }
    return 0;
}

/* Inside HDF5_BEGIN/END, right after a failed call: keep the most specific
 * message on HDF5's error stack and clear the stack. */
static void
capture_error(hdf5_err_t *err)
{
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, innermost_cb, err);
    H5Eclear2(H5E_DEFAULT);
}

static void
raise_error(const char *what, PyObject *path, const hdf5_err_t *err)
{
    if (path && err && err->found)
        PyErr_Format(VolstreamError, "%s %R: %s", what, path, err->desc);
    else if (path)
        PyErr_Format(VolstreamError, "%s %R", what, path);
    else if (err && err->found)
        PyErr_Format(VolstreamError, "%s: %s", what, err->desc);
    else
        PyErr_SetString(VolstreamError, what);
}

/* ---------------------------------------------------------------- Buffer -- */

typedef struct {
    PyObject_HEAD
    void      *data;
    Py_ssize_t size;
} BufferObject;

static void
Buffer_dealloc(BufferObject *self)
{
    free(self->data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Buffer_getbuffer(BufferObject *self, Py_buffer *view, int flags)
{
    return PyBuffer_FillInfo(view, (PyObject *)self, self->data, self->size, 0, flags);
}

static Py_ssize_t
Buffer_length(BufferObject *self)
{
    return self->size;
}

static PyBufferProcs Buffer_as_buffer = {
    .bf_getbuffer = (getbufferproc)Buffer_getbuffer,
};

static PySequenceMethods Buffer_as_sequence = {
    .sq_length = (lenfunc)Buffer_length,
};

static PyTypeObject BufferType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "volstream._volstream.Buffer",
    .tp_doc         = "Bytes of one pushed payload, freed when the last view of them is released.",
    .tp_basicsize   = sizeof(BufferObject),
    .tp_flags       = Py_TPFLAGS_DEFAULT,
    .tp_dealloc     = (destructor)Buffer_dealloc,
    .tp_as_buffer   = &Buffer_as_buffer,
    .tp_as_sequence = &Buffer_as_sequence,
};

/* Takes ownership of data, freeing it on failure. */
static PyObject *
buffer_wrap(void *data, size_t size)
{
    BufferObject *b = PyObject_New(BufferObject, &BufferType);

    if (!b) {
        free(data);
        return NULL;
    }
    b->data = data;
    b->size = (Py_ssize_t)size;
    return (PyObject *)b;
}

/* --------------------------------------------------------------- RawFile -- */

typedef struct {
    PyObject_HEAD
    hid_t     fid;
    hid_t     fapl;
    pid_t     pid;
    PyObject *path;
} RawFileObject;

static int
check_usable(RawFileObject *self)
{
    if (self->pid != getpid()) {
        PyErr_Format(VolstreamError,
                     "%R was opened by process %ld and cannot be used after fork(); open it in this "
                     "process instead (with multiprocessing or a PyTorch DataLoader, use the 'spawn' "
                     "start method, or open the file inside each worker)",
                     self->path, (long)self->pid);
        return -1;
    }
    if (self->fid < 0) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed file");
        return -1;
    }
    return 0;
}

/* Close the ids, or in a forked child just forget them. Idempotent. */
static herr_t
rawfile_release(RawFileObject *self)
{
    herr_t status = 0;
    hid_t  fid = self->fid, fapl = self->fapl;

    self->fid  = H5I_INVALID_HID;
    self->fapl = H5I_INVALID_HID;
    if (self->pid != getpid() || (fid < 0 && fapl < 0))
        return 0;

    HDF5_BEGIN
    if (fid >= 0)
        status = H5Fclose(fid);
    if (fapl >= 0)
        H5Pclose(fapl);
    H5Eclear2(H5E_DEFAULT);
    HDF5_END
    return status;
}

static void
RawFile_dealloc(RawFileObject *self)
{
    rawfile_release(self);
    Py_XDECREF(self->path);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
RawFile_close(RawFileObject *self, PyObject *Py_UNUSED(ignored))
{
    if (rawfile_release(self) < 0) {
        raise_error("could not close", self->path, NULL);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* ---- schema ---- */

typedef struct {
    char   *path;
    int     is_attr;
    int     rank; /* -1 if not a simple dataspace */
    hsize_t dims[H5S_MAX_RANK];
    char   *type_json; /* see describe_type() */
} var_desc_t;

/* A growable string, built under HDF5_BEGIN/END without touching Python. */
typedef struct {
    char  *buf;
    size_t len, cap;
    int    oom;
} sbuf_t;

static void
sb_put(sbuf_t *b, const char *str, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        char  *grown;

        while (b->len + n + 1 > cap)
            cap *= 2;
        if (NULL == (grown = realloc(b->buf, cap))) {
            b->oom = 1;
            return;
        }
        b->buf = grown;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, str, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* Append a string literal; its length is taken at compile time. */
#define SB_LIT(b, lit) sb_put((b), (lit), sizeof(lit) - 1)

static void
sb_printf(sbuf_t *b, const char *fmt, ...)
{
    char    tmp[128];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        sb_put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static void
sb_json_string(sbuf_t *b, const char *str)
{
    const unsigned char *p;

    SB_LIT(b, "\"");
    for (p = (const unsigned char *)str; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = {'\\', (char)*p};

            sb_put(b, esc, 2);
        }
        else if (*p < 0x20)
            sb_printf(b, "\\u%04x", *p);
        else
            sb_put(b, (const char *)p, 1);
    }
    SB_LIT(b, "\"");
}

/* Under HDF5_BEGIN/END. Describes type_id as JSON the Python layer turns
 * into a NumPy dtype:
 *   {"k":"i"|"u"|"f", "s":size, "o":"<"|">"}   integer or float
 *   {"k":"S", "s":size}                        fixed-length string
 *   {"k":"vls", "s":size}                      variable-length string
 *   {"k":"V", "s":size}                        opaque
 *   {"k":"array", "dims":[...], "base":T}      array
 *   {"k":"compound", "s":size, "members":[{"name":..., "offset":..., "type":T}, ...]}
 *   {"k":null, "s":size}                       anything else (a variable-
 *                                              length sequence, reference,
 *                                              bitfield)
 * An enum is described as its base integer type. */
static void
describe_type(hid_t t, sbuf_t *b, int depth)
{
    H5T_class_t cls  = H5Tget_class(t);
    size_t      size = H5Tget_size(t);
    const char *ord  = H5Tget_order(t) == H5T_ORDER_BE ? ">" : "<";

    if (depth > 16) {
        sb_printf(b, "{\"k\":null,\"s\":%zu}", size);
        return;
    }
    switch (cls) {
        case H5T_INTEGER:
            sb_printf(b, "{\"k\":\"%s\",\"s\":%zu,\"o\":\"%s\"}",
                      H5Tget_sign(t) == H5T_SGN_NONE ? "u" : "i", size, ord);
            return;
        case H5T_FLOAT:
            sb_printf(b, "{\"k\":\"f\",\"s\":%zu,\"o\":\"%s\"}", size, ord);
            return;
        case H5T_STRING:
            if (H5Tis_variable_str(t) > 0)
                sb_printf(b, "{\"k\":\"vls\",\"s\":%zu}", size);
            else
                sb_printf(b, "{\"k\":\"S\",\"s\":%zu}", size);
            return;
        case H5T_OPAQUE:
            sb_printf(b, "{\"k\":\"V\",\"s\":%zu}", size);
            return;
        case H5T_ENUM: {
            hid_t base = H5Tget_super(t);

            if (base < 0)
                break;
            describe_type(base, b, depth + 1);
            H5Tclose(base);
            return;
        }
        case H5T_ARRAY: {
            hsize_t adims[H5S_MAX_RANK];
            int     r = H5Tget_array_ndims(t), i;
            hid_t   base;

            if (r < 0 || r > H5S_MAX_RANK || H5Tget_array_dims2(t, adims) < 0 || (base = H5Tget_super(t)) < 0)
                break;
            SB_LIT(b, "{\"k\":\"array\",\"dims\":[");
            for (i = 0; i < r; i++)
                sb_printf(b, "%s%llu", i ? "," : "", (unsigned long long)adims[i]);
            SB_LIT(b, "],\"base\":");
            describe_type(base, b, depth + 1);
            SB_LIT(b, "}");
            H5Tclose(base);
            return;
        }
        case H5T_COMPOUND: {
            int nm = H5Tget_nmembers(t), i;

            if (nm < 0)
                break;
            sb_printf(b, "{\"k\":\"compound\",\"s\":%zu,\"members\":[", size);
            for (i = 0; i < nm; i++) {
                char  *name = H5Tget_member_name(t, (unsigned)i);
                hid_t  mt   = H5Tget_member_type(t, (unsigned)i);
                size_t off  = H5Tget_member_offset(t, (unsigned)i);

                if (i)
                    SB_LIT(b, ",");
                SB_LIT(b, "{\"name\":");
                sb_json_string(b, name ? name : "");
                sb_printf(b, ",\"offset\":%zu,\"type\":", off);
                if (mt >= 0) {
                    describe_type(mt, b, depth + 1);
                    H5Tclose(mt);
                }
                else
                    sb_printf(b, "{\"k\":null,\"s\":0}");
                SB_LIT(b, "}");
                H5free_memory(name);
            }
            SB_LIT(b, "]}");
            return;
        }
        default:
            break;
    }
    sb_printf(b, "{\"k\":null,\"s\":%zu}", size);
}

/* Under HDF5_BEGIN/END. */
static void
describe_var(const H5F_stream_var_t *v, var_desc_t *d)
{
    sbuf_t b = {NULL, 0, 0, 0};

    d->path    = strdup(v->path);
    d->is_attr = v->is_attr ? 1 : 0;
    d->rank    = H5Sget_simple_extent_ndims(v->space_id);
    if (d->rank > 0)
        H5Sget_simple_extent_dims(v->space_id, d->dims, NULL);
    describe_type(v->type_id, &b, 0);
    d->type_json = b.oom ? (free(b.buf), NULL) : b.buf;
}

static PyObject *
var_to_python(const var_desc_t *d)
{
    PyObject *dims, *type, *item;
    int       i;

    if (d->rank < 0)
        dims = Py_NewRef(Py_None);
    else if (NULL != (dims = PyTuple_New(d->rank)))
        for (i = 0; i < d->rank; i++) {
            PyObject *v = PyLong_FromUnsignedLongLong((unsigned long long)d->dims[i]);

            if (!v) {
                Py_CLEAR(dims);
                break;
            }
            PyTuple_SET_ITEM(dims, i, v);
        }
    if (!dims)
        return NULL;
    if (!d->type_json) {
        Py_DECREF(dims);
        return PyErr_NoMemory();
    }
    type = PyUnicode_FromString(d->type_json);
    if (!type) {
        Py_DECREF(dims);
        return NULL;
    }
    item = Py_BuildValue("(sONN)", d->path ? d->path : "", d->is_attr ? Py_True : Py_False, dims, type);
    return item;
}

static PyObject *
RawFile_schema(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           deadline, t0, step = 0;
    size_t             n = 0, i;
    var_desc_t        *descs = NULL;
    herr_t             status = -1;
    hdf5_err_t         err    = {{0}, 0};
    PyObject          *list = NULL, *result = NULL;

    if (!PyArg_ParseTuple(args, "K:schema", &timeout_ms) || check_usable(self) < 0)
        return NULL;
    deadline = now_ms() + timeout_ms;

    for (;;) {
        uint64_t now = now_ms(), slice = deadline > now ? deadline - now : 0;

        if (slice > POLL_MS)
            slice = POLL_MS;
        t0 = now_ms();
        HDF5_BEGIN
        H5F_stream_var_t *vars = NULL;

        status = H5Fget_stream_schema(self->fid, slice, &step, &n, &vars);
        if (status < 0)
            capture_error(&err);
        else if (NULL != (descs = calloc(n ? n : 1, sizeof(*descs)))) {
            for (i = 0; i < n; i++)
                describe_var(&vars[i], &descs[i]);
        }
        if (status >= 0)
            H5Ffree_stream_schema(n, vars);
        HDF5_END
        if (status >= 0 || now_ms() >= deadline || failed_fast(t0, slice))
            break;
        if (PyErr_CheckSignals() < 0)
            return NULL;
    }
    if (status < 0) {
        raise_error("no stream schema (timed out, transport unavailable, or undecodable) for", self->path, &err);
        return NULL;
    }
    if (!descs) {
        PyErr_NoMemory();
        return NULL;
    }

    if (NULL == (list = PyList_New((Py_ssize_t)n)))
        goto done;
    for (i = 0; i < n; i++) {
        PyObject *item = var_to_python(&descs[i]);

        if (!item) {
            Py_CLEAR(list);
            goto done;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, item);
    }
    result = Py_BuildValue("(KO)", (unsigned long long)step, list);

done:
    Py_XDECREF(list);
    for (i = 0; i < n; i++) {
        free(descs[i].path);
        free(descs[i].type_json);
    }
    free(descs);
    return result;
}

/* ---- subscribe ---- */

typedef struct {
    const char *path;
    int         rank;
    int         has_sel;
    int         deflate; /* -1: no re-filtering */
    int         has_chunk;
    hsize_t     dims[H5S_MAX_RANK], start[H5S_MAX_RANK], count[H5S_MAX_RANK], chunk[H5S_MAX_RANK];
} sub_entry_t;

static int
parse_dims(PyObject *seq, const char *what, hsize_t *out, int *rank)
{
    PyObject  *fast = PySequence_Fast(seq, what);
    Py_ssize_t n, i;

    if (!fast)
        return -1;
    n = PySequence_Fast_GET_SIZE(fast);
    if (n > H5S_MAX_RANK) {
        PyErr_Format(PyExc_ValueError, "%s has rank %zd, more than HDF5's %d", what, n, H5S_MAX_RANK);
        Py_DECREF(fast);
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned long long v = PyLong_AsUnsignedLongLong(PySequence_Fast_GET_ITEM(fast, i));

        if (v == (unsigned long long)-1 && PyErr_Occurred()) {
            Py_DECREF(fast);
            return -1;
        }
        out[i] = (hsize_t)v;
    }
    *rank = (int)n;
    Py_DECREF(fast);
    return 0;
}

/* (path, dims, start, count[, deflate[, chunk]]); start and count None for
 * the whole extent, deflate a level 0-9 or -1 for none, chunk the chunk shape
 * for deflate (default: the selection, or the extent). e->path borrows from
 * the entry, which the caller keeps alive. */
static int
parse_entry(PyObject *entry, sub_entry_t *e)
{
    PyObject *path, *dims_o, *start_o, *count_o, *chunk_o = Py_None;
    int       srank, crank, i;

    e->deflate = -1;
    if (!PyArg_ParseTuple(entry, "UOOO|iO:subscribe entry", &path, &dims_o, &start_o, &count_o, &e->deflate,
                          &chunk_o))
        return -1;
    if (NULL == (e->path = PyUnicode_AsUTF8(path)) || parse_dims(dims_o, "dims", e->dims, &e->rank) < 0)
        return -1;
    if ((start_o == Py_None) != (count_o == Py_None)) {
        PyErr_SetString(PyExc_ValueError, "start and count must both be given, or neither");
        return -1;
    }
    e->has_sel = start_o != Py_None;
    if (e->has_sel) {
        if (parse_dims(start_o, "start", e->start, &srank) < 0 ||
            parse_dims(count_o, "count", e->count, &crank) < 0)
            return -1;
        if (srank != e->rank || crank != e->rank) {
            PyErr_Format(PyExc_ValueError, "selection for %R has rank %d/%d, but the dataset has rank %d", path,
                         srank, crank, e->rank);
            return -1;
        }
    }
    if (e->deflate != -1) {
        if (e->deflate < 0 || e->deflate > 9) {
            PyErr_Format(PyExc_ValueError, "deflate level must be 0-9, not %d", e->deflate);
            return -1;
        }
        if (e->rank == 0) {
            PyErr_Format(PyExc_ValueError, "%R is a scalar; it cannot be delivered deflated", path);
            return -1;
        }
        e->has_chunk = chunk_o != Py_None;
        if (e->has_chunk) {
            int chrank;

            if (parse_dims(chunk_o, "chunk", e->chunk, &chrank) < 0)
                return -1;
            if (chrank != e->rank) {
                PyErr_Format(PyExc_ValueError, "chunk for %R has rank %d, but the dataset has rank %d", path,
                             chrank, e->rank);
                return -1;
            }
        }
        else
            for (i = 0; i < e->rank; i++)
                e->chunk[i] = e->has_sel ? e->count[i] : e->dims[i];
        for (i = 0; i < e->rank; i++)
            if (e->chunk[i] == 0) {
                PyErr_Format(PyExc_ValueError, "the chunk for %R is empty; nothing to deflate", path);
                return -1;
            }
    }
    return 0;
}

static PyObject *
RawFile_subscribe(RawFileObject *self, PyObject *args)
{
    PyObject    *entries, *fast;
    Py_ssize_t   n, i;
    sub_entry_t *e      = NULL;
    const char **paths  = NULL;
    hid_t       *spaces = NULL;
    hid_t       *plists = NULL;
    int          any_deflate = 0;
    herr_t       status = -1;
    hdf5_err_t   err    = {{0}, 0};
    PyObject    *result = NULL;
    PyObject    *from_obj = Py_None;
    unsigned long long from_step = 0;

    if (!PyArg_ParseTuple(args, "O|O:subscribe", &entries, &from_obj) || check_usable(self) < 0)
        return NULL;
    if (from_obj != Py_None) {
        from_step = PyLong_AsUnsignedLongLong(from_obj);
        if (PyErr_Occurred())
            return NULL;
    }
    if (NULL == (fast = PySequence_Fast(entries, "subscribe() takes a sequence of entries")))
        return NULL;
    if ((n = PySequence_Fast_GET_SIZE(fast)) == 0) {
        PyErr_SetString(PyExc_ValueError, "nothing to subscribe to");
        goto done;
    }
    e      = PyMem_Calloc((size_t)n, sizeof(*e));
    paths  = PyMem_Calloc((size_t)n, sizeof(*paths));
    spaces = PyMem_Calloc((size_t)n, sizeof(*spaces));
    plists = PyMem_Calloc((size_t)n, sizeof(*plists));
    if (!e || !paths || !spaces || !plists) {
        PyErr_NoMemory();
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (parse_entry(PySequence_Fast_GET_ITEM(fast, i), &e[i]) < 0)
            goto done;
        paths[i]  = e[i].path;
        spaces[i] = H5I_INVALID_HID;
        plists[i] = H5P_DEFAULT;
        any_deflate |= e[i].deflate >= 0;
    }

    HDF5_BEGIN
    status = 0;
    for (i = 0; i < n && status >= 0; i++) {
        spaces[i] = e[i].rank == 0 ? H5Screate(H5S_SCALAR) : H5Screate_simple(e[i].rank, e[i].dims, NULL);
        if (spaces[i] < 0 || (e[i].rank > 0 && e[i].has_sel &&
                              H5Sselect_hyperslab(spaces[i], H5S_SELECT_SET, e[i].start, NULL, e[i].count,
                                                  NULL) < 0))
            status = -1;
    }
    /* Per-subscriber precision: the writer re-filters this subscriber's data
     * through the DCPL's pipeline in transit, one chunk spanning the
     * selection; H5Fget_subscribed_data() hands back decoded values. */
    for (i = 0; i < n && status >= 0; i++) {
        if (e[i].deflate < 0)
            continue;
        if ((plists[i] = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
            H5Pset_chunk(plists[i], e[i].rank, e[i].chunk) < 0 ||
            H5Pset_deflate(plists[i], (unsigned)e[i].deflate) < 0)
            status = -1;
    }
    if (status >= 0)
        status = from_obj != Py_None
                     ? H5Fsubscribe_from(self->fid, (uint64_t)from_step, (size_t)n, paths, spaces,
                                         any_deflate ? plists : NULL)
                     : H5Fsubscribe(self->fid, (size_t)n, paths, spaces, any_deflate ? plists : NULL);
    if (status < 0)
        capture_error(&err);
    for (i = 0; i < n; i++) {
        if (spaces[i] >= 0)
            H5Sclose(spaces[i]);
        if (plists[i] > 0 && plists[i] != H5P_DEFAULT)
            H5Pclose(plists[i]);
    }
    HDF5_END

    if (status < 0)
        raise_error("subscribe failed (transport unavailable, or a bad selection) for", self->path, &err);
    else
        result = Py_NewRef(Py_None);

done:
    PyMem_Free(e);
    PyMem_Free(paths);
    PyMem_Free(spaces);
    PyMem_Free(plists);
    Py_DECREF(fast);
    return result;
}

/* An index into native_type() for (kind, size), or -1 after raising. Split in
 * two because the H5T_NATIVE_* macros read HDF5 globals, so native_type()
 * must run under HDF5_BEGIN while the ValueError here needs the GIL. */
static int
native_type_code(const char *kind, Py_ssize_t size)
{
    static const Py_ssize_t sizes[] = {1, 2, 4, 8};
    int                     k;

    for (k = 0; k < 4; k++)
        if (sizes[k] == size) {
            if (!strcmp(kind, "i"))
                return k;
            if (!strcmp(kind, "u"))
                return 4 + k;
            if (!strcmp(kind, "f") && size >= 4)
                return 8 + k - 2;
        }
    PyErr_Format(PyExc_ValueError, "no native HDF5 type for kind '%s', size %zd", kind, size);
    return -1;
}

static hid_t
native_type(int code)
{
    switch (code) {
        case 0:
            return H5T_NATIVE_INT8;
        case 1:
            return H5T_NATIVE_INT16;
        case 2:
            return H5T_NATIVE_INT32;
        case 3:
            return H5T_NATIVE_INT64;
        case 4:
            return H5T_NATIVE_UINT8;
        case 5:
            return H5T_NATIVE_UINT16;
        case 6:
            return H5T_NATIVE_UINT32;
        case 7:
            return H5T_NATIVE_UINT64;
        case 8:
            return H5T_NATIVE_FLOAT;
        case 9:
            return H5T_NATIVE_DOUBLE;
    }
    return H5I_INVALID_HID;
}

static PyObject *
RawFile_subscribe_type(RawFileObject *self, PyObject *args)
{
    const char *path, *kind = NULL;
    Py_ssize_t  size = 0;
    int         code = -1;
    herr_t      status;
    hdf5_err_t  err = {{0}, 0};

    if (!PyArg_ParseTuple(args, "szn:subscribe_type", &path, &kind, &size) || check_usable(self) < 0)
        return NULL;
    if (kind && (code = native_type_code(kind, size)) < 0)
        return NULL;

    HDF5_BEGIN
    status = H5Fsubscribe_type(self->fid, path, code < 0 ? H5I_INVALID_HID : native_type(code));
    if (status < 0)
        capture_error(&err);
    HDF5_END
    if (status < 0) {
        raise_error("subscribe_type failed (is the path subscribed?) for", self->path, &err);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
RawFile_subscribe_predicate(RawFileObject *self, PyObject *args)
{
    const char *path;
    int         op, is_float;
    PyObject   *value;
    long long   ivalue = 0;
    double      fvalue = 0;
    herr_t      status;
    hdf5_err_t  err = {{0}, 0};

    if (!PyArg_ParseTuple(args, "siO:subscribe_predicate", &path, &op, &value) || check_usable(self) < 0)
        return NULL;
    if (op < H5VL_STREAM_PRED_LT || op > H5VL_STREAM_PRED_NE) {
        PyErr_Format(PyExc_ValueError, "unknown predicate operator %d", op);
        return NULL;
    }
    if (PyLong_Check(value)) {
        ivalue = PyLong_AsLongLong(value);
        if (ivalue == -1 && PyErr_Occurred())
            return NULL;
        is_float = 0;
    }
    else if (PyFloat_Check(value)) {
        fvalue   = PyFloat_AS_DOUBLE(value);
        is_float = 1;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "predicate value must be an int or a float");
        return NULL;
    }

    HDF5_BEGIN
    status = is_float ? H5Fsubscribe_predicate(self->fid, path, (H5VL_stream_pred_op_t)op, H5T_NATIVE_DOUBLE,
                                               &fvalue)
                      : H5Fsubscribe_predicate(self->fid, path, (H5VL_stream_pred_op_t)op, H5T_NATIVE_LLONG,
                                               &ivalue);
    if (status < 0)
        capture_error(&err);
    HDF5_END
    if (status < 0) {
        raise_error("subscribe_predicate failed (is the path subscribed?) for", self->path, &err);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* ---- blocking waits ---- */

/* (physical_step, wall_time_ns), or None on timeout. */
static PyObject *
RawFile_wait_step_ready(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           deadline, t0, phys = 0, wall_ns = 0;
    herr_t             status;

    if (!PyArg_ParseTuple(args, "K:wait_step_ready", &timeout_ms) || check_usable(self) < 0)
        return NULL;
    deadline = now_ms() + timeout_ms;

    for (;;) {
        uint64_t now = now_ms(), slice = deadline > now ? deadline - now : 0;

        if (slice > POLL_MS)
            slice = POLL_MS;
        t0 = now_ms();
        HDF5_BEGIN
        status = H5Fwait_step_ready(self->fid, slice, &phys, &wall_ns);
        H5Eclear2(H5E_DEFAULT);
        HDF5_END
        if (status >= 0)
            return Py_BuildValue("(KK)", (unsigned long long)phys, (unsigned long long)wall_ns);
        if (now_ms() >= deadline || failed_fast(t0, slice))
            Py_RETURN_NONE;
        if (PyErr_CheckSignals() < 0)
            return NULL;
    }
}

/* (physical_step, path, elem_start, elem_count, Buffer), or None on timeout. */
static PyObject *
RawFile_get(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           deadline, t0, phys = 0, elem_start = 0, elem_count = 0;
    char              *path = NULL;
    void              *buf  = NULL;
    size_t             size     = 0;
    uint32_t           delivery = 0;
    herr_t             status;
    PyObject          *pybuf, *result;

    if (!PyArg_ParseTuple(args, "K:get", &timeout_ms) || check_usable(self) < 0)
        return NULL;
    deadline = now_ms() + timeout_ms;

    for (;;) {
        uint64_t now = now_ms(), slice = deadline > now ? deadline - now : 0;

        if (slice > POLL_MS)
            slice = POLL_MS;
        t0 = now_ms();
        HDF5_BEGIN
        status = H5Fget_subscribed_data(self->fid, slice, &phys, &path, &buf, &size, &elem_start, &elem_count,
                                        &delivery);
        H5Eclear2(H5E_DEFAULT);
        HDF5_END
        if (status >= 0)
            break;
        if (now_ms() >= deadline || failed_fast(t0, slice))
            Py_RETURN_NONE;
        if (PyErr_CheckSignals() < 0)
            return NULL;
    }

    if (NULL == (pybuf = buffer_wrap(buf, size))) {
        free(path);
        return NULL;
    }
    result = Py_BuildValue("(KsKKNI)", (unsigned long long)phys, path, (unsigned long long)elem_start,
                           (unsigned long long)elem_count, pybuf, (unsigned int)delivery);
    free(path);
    return result;
}

/* Best-effort ack of a consumed step; True if the writer received it. */
static PyObject *
RawFile_ack(RawFileObject *self, PyObject *args)
{
    unsigned long long phys;
    herr_t             status;

    if (!PyArg_ParseTuple(args, "K:ack", &phys) || check_usable(self) < 0)
        return NULL;
    HDF5_BEGIN
    status = H5Fack_stream_step(self->fid, (uint64_t)phys);
    H5Eclear2(H5E_DEFAULT);
    HDF5_END
    return PyBool_FromLong(status >= 0);
}

/* True once every writer has left and every step it announced was consumed. */
static PyObject *
RawFile_end_of_stream(RawFileObject *self, PyObject *Py_UNUSED(ignored))
{
    H5F_step_status_t st     = H5F_STEP_NOT_IN_STEP;
    herr_t            status;

    if (check_usable(self) < 0)
        return NULL;
    HDF5_BEGIN
    status = H5Fstep_status(self->fid, &st);
    H5Eclear2(H5E_DEFAULT);
    HDF5_END
    return PyBool_FromLong(status >= 0 && st == H5F_STEP_EOS);
}

static PyObject *
RawFile_get_closed(RawFileObject *self, void *Py_UNUSED(closure))
{
    return PyBool_FromLong(self->fid < 0);
}

static PyObject *
RawFile_get_path(RawFileObject *self, void *Py_UNUSED(closure))
{
    return Py_NewRef(self->path);
}

static PyMethodDef RawFile_methods[] = {
    {"close", (PyCFunction)RawFile_close, METH_NOARGS, "Close the file. Safe to call more than once."},
    {"schema", (PyCFunction)RawFile_schema, METH_VARARGS,
     "schema(timeout_ms) -> (step, [(path, is_attr, dims, type_json)])"},
    {"subscribe", (PyCFunction)RawFile_subscribe, METH_VARARGS,
     "subscribe([(path, dims, start, count[, deflate[, chunk]]), ...][, from_step]); start/count None for "
     "the whole extent; from_step backfills the steps missed from that one (H5Fsubscribe_from)"},
    {"subscribe_type", (PyCFunction)RawFile_subscribe_type, METH_VARARGS,
     "subscribe_type(path, kind, size); kind None clears the narrowing"},
    {"subscribe_predicate", (PyCFunction)RawFile_subscribe_predicate, METH_VARARGS,
     "subscribe_predicate(path, op, value); op is an H5VL_stream_pred_op_t value"},
    {"wait_step_ready", (PyCFunction)RawFile_wait_step_ready, METH_VARARGS,
     "wait_step_ready(timeout_ms) -> (physical_step, wall_time_ns) or None on timeout"},
    {"get", (PyCFunction)RawFile_get, METH_VARARGS,
     "get(timeout_ms) -> (physical_step, path, elem_start, elem_count, Buffer, delivery) or None on timeout; "
     "delivery is the DELIVERY_* bits for that push"},
    {"ack", (PyCFunction)RawFile_ack, METH_VARARGS,
     "ack(physical_step) -> bool; tell the writer this step was consumed (best-effort)"},
    {"end_of_stream", (PyCFunction)RawFile_end_of_stream, METH_NOARGS,
     "True once every writer has left and every step it announced was consumed"},
    {NULL, NULL, 0, NULL}};

static PyGetSetDef RawFile_getset[] = {
    {"closed", (getter)RawFile_get_closed, NULL, "True once close() has run.", NULL},
    {"path", (getter)RawFile_get_path, NULL, "The path this file was opened with.", NULL},
    {NULL, NULL, NULL, NULL, NULL}};

static PyTypeObject RawFileType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "volstream._volstream.RawFile",
    .tp_doc       = "A vol-stream file opened for reading; one method per connector call.",
    .tp_basicsize = sizeof(RawFileObject),
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_dealloc   = (destructor)RawFile_dealloc,
    .tp_methods   = RawFile_methods,
    .tp_getset    = RawFile_getset,
};

/* ---------------------------------------------------------------- module -- */

static PyObject *
vs_open(PyObject *Py_UNUSED(module), PyObject *args, PyObject *kwargs)
{
    static char   *kwlist[] = {"path", NULL};
    PyObject      *path_bytes = NULL;
    PyObject      *path_str   = NULL;
    RawFileObject *file       = NULL;
    const char    *cpath;
    hid_t          fapl = H5I_INVALID_HID, fid = H5I_INVALID_HID, vol_id;
    hdf5_err_t     err  = {{0}, 0};
    int            registered;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O&:open", kwlist, PyUnicode_FSConverter, &path_bytes))
        return NULL;
    if (NULL == (path_str = PyUnicode_DecodeFSDefault(PyBytes_AS_STRING(path_bytes))))
        goto done;
    if (g_init_pid && g_init_pid != getpid()) {
        PyErr_Format(VolstreamError,
                     "volstream was initialized in process %ld and cannot open files after fork(); use the "
                     "'spawn' start method, or import volstream only inside each worker",
                     (long)g_init_pid);
        goto done;
    }
    cpath = PyBytes_AS_STRING(path_bytes);

    HDF5_BEGIN
    if (g_vol_id < 0 && (g_vol_id = H5VL_stream_register()) >= 0)
        g_init_pid = getpid();
    registered = g_vol_id >= 0;
    vol_id     = g_vol_id;
    if (registered && (fapl = H5Pcreate(H5P_FILE_ACCESS)) >= 0 && H5Pset_vol(fapl, vol_id, NULL) >= 0 &&
        H5Pset_file_locking(fapl, false, true) >= 0)
        fid = H5Fopen(cpath, H5F_ACC_RDONLY, fapl);
    if (fid < 0) {
        capture_error(&err);
        if (fapl >= 0)
            H5Pclose(fapl);
    }
    HDF5_END

    if (!registered) {
        raise_error("could not register the vol-stream connector", NULL, &err);
        goto done;
    }
    if (fid < 0) {
        raise_error("could not open", path_str, &err);
        goto done;
    }

    if (NULL == (file = PyObject_New(RawFileObject, &RawFileType))) {
        HDF5_BEGIN
        H5Fclose(fid);
        H5Pclose(fapl);
        HDF5_END
        goto done;
    }
    file->fid  = fid;
    file->fapl = fapl;
    file->pid  = getpid();
    file->path = Py_NewRef(path_str);

done:
    Py_XDECREF(path_str);
    Py_XDECREF(path_bytes);
    return (PyObject *)file;
}

/* vl_strings(buffer, count) -> [str | None, ...]
 *
 * A pushed variable-length string object arrives decoded into one
 * allocation: count char * pointers, then the bytes they point to (see
 * H5Fget_subscribed_data()). The pointers mean something only while that
 * allocation lives, so they are turned into str objects here, from the
 * Buffer that owns it. NULL (an unset string) becomes None. */
static PyObject *
vs_vl_strings(PyObject *Py_UNUSED(module), PyObject *args)
{
    PyObject   *obj, *list;
    Py_ssize_t  count, i;
    Py_buffer   view;
    char *const *ptrs;

    if (!PyArg_ParseTuple(args, "On:vl_strings", &obj, &count))
        return NULL;
    if (count < 0 || PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0)
        return count < 0 ? PyErr_Format(PyExc_ValueError, "negative count") : NULL;
    if ((size_t)view.len < (size_t)count * sizeof(char *)) {
        PyBuffer_Release(&view);
        return PyErr_Format(PyExc_ValueError, "a %zd-byte buffer cannot hold %zd string pointers", view.len,
                            count);
    }
    ptrs = (char *const *)view.buf;
    if (NULL == (list = PyList_New(count))) {
        PyBuffer_Release(&view);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        PyObject *v = ptrs[i] ? PyUnicode_DecodeUTF8(ptrs[i], (Py_ssize_t)strlen(ptrs[i]), "surrogateescape")
                              : Py_NewRef(Py_None);

        if (!v) {
            Py_DECREF(list);
            PyBuffer_Release(&view);
            return NULL;
        }
        PyList_SET_ITEM(list, i, v);
    }
    PyBuffer_Release(&view);
    return list;
}

static PyMethodDef module_methods[] = {
    {"open", (PyCFunction)(void (*)(void))vs_open, METH_VARARGS | METH_KEYWORDS,
     "open(path) -> RawFile\n\nOpen a vol-stream file for reading."},
    {"vl_strings", vs_vl_strings, METH_VARARGS,
     "vl_strings(buffer, count) -> list\n\nThe strings of a pushed variable-length string object."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef volstream_module = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "volstream._volstream",
    .m_doc     = "C extension behind the volstream package.",
    .m_size    = -1,
    .m_methods = module_methods,
};

PyMODINIT_FUNC
PyInit__volstream(void)
{
    PyObject *m;

    if (!g_hdf5_lock && NULL == (g_hdf5_lock = PyThread_allocate_lock())) {
        PyErr_NoMemory();
        return NULL;
    }
    if (PyType_Ready(&RawFileType) < 0 || PyType_Ready(&BufferType) < 0)
        return NULL;
    if (NULL == (m = PyModule_Create(&volstream_module)))
        return NULL;

    VolstreamError = PyErr_NewExceptionWithDoc("volstream.Error", "A vol-stream or HDF5 call failed.", NULL, NULL);
    if (!VolstreamError || PyModule_AddObjectRef(m, "Error", VolstreamError) < 0 ||
        PyModule_AddObjectRef(m, "RawFile", (PyObject *)&RawFileType) < 0 ||
        PyModule_AddObjectRef(m, "Buffer", (PyObject *)&BufferType) < 0 ||
        PyModule_AddIntConstant(m, "PRED_LT", H5VL_STREAM_PRED_LT) < 0 ||
        PyModule_AddIntConstant(m, "PRED_LE", H5VL_STREAM_PRED_LE) < 0 ||
        PyModule_AddIntConstant(m, "PRED_GT", H5VL_STREAM_PRED_GT) < 0 ||
        PyModule_AddIntConstant(m, "PRED_GE", H5VL_STREAM_PRED_GE) < 0 ||
        PyModule_AddIntConstant(m, "PRED_EQ", H5VL_STREAM_PRED_EQ) < 0 ||
        PyModule_AddIntConstant(m, "PRED_NE", H5VL_STREAM_PRED_NE) < 0 ||
        PyModule_AddIntConstant(m, "DELIVERY_SELECTION_SPAN", H5VL_STREAM_DELIVERY_SELECTION_SPAN) < 0 ||
        PyModule_AddIntConstant(m, "DELIVERY_PREDICATE_UNEVALUATED",
                                H5VL_STREAM_DELIVERY_PREDICATE_UNEVALUATED) < 0 ||
        PyModule_AddIntConstant(m, "DELIVERY_PREDICATE_SPAN", H5VL_STREAM_DELIVERY_PREDICATE_SPAN) < 0 ||
        PyModule_AddIntConstant(m, "DELIVERY_TYPE_NATIVE", H5VL_STREAM_DELIVERY_TYPE_NATIVE) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
