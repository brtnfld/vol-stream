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
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "hdf5.h"
#include "H5VLstream.h"

static PyObject *VolstreamError;

/* Registered on first open and kept for the life of the process. */
static hid_t g_vol_id = H5I_INVALID_HID;

/* ---------------------------------------------------------------- errors -- */

typedef struct {
    char desc[256];
    int  found;
} innermost_t;

static herr_t
innermost_cb(unsigned n, const H5E_error2_t *err, void *udata)
{
    innermost_t *out = (innermost_t *)udata;

    (void)n;
    if (!out->found && err->desc && err->desc[0]) {
        snprintf(out->desc, sizeof(out->desc), "%s", err->desc);
        out->found = 1;
    }
    return 0;
}

/* Raise volstream.Error for a failed HDF5 call, naming the most specific
 * message on HDF5's error stack, then clear the stack. */
static void
raise_hdf5_error(const char *what, PyObject *path)
{
    innermost_t inner = {{0}, 0};

    H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, innermost_cb, &inner);
    H5Eclear2(H5E_DEFAULT);

    if (path && inner.found)
        PyErr_Format(VolstreamError, "%s %R: %s", what, path, inner.desc);
    else if (path)
        PyErr_Format(VolstreamError, "%s %R", what, path);
    else if (inner.found)
        PyErr_Format(VolstreamError, "%s: %s", what, inner.desc);
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
    .tp_doc       = "Bytes of one pushed payload, freed when the last view of them is released.",
    .tp_basicsize = sizeof(BufferObject),
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_dealloc   = (destructor)Buffer_dealloc,
    .tp_as_buffer = &Buffer_as_buffer,
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
    PyObject *path;
} RawFileObject;

static int
check_open(RawFileObject *self)
{
    if (self->fid < 0) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed file");
        return -1;
    }
    return 0;
}

static void
rawfile_release(RawFileObject *self, herr_t *close_status)
{
    herr_t status = 0;

    if (self->fid >= 0) {
        H5E_BEGIN_TRY
        {
            status = H5Fclose(self->fid);
        }
        H5E_END_TRY
        self->fid = H5I_INVALID_HID;
    }
    if (self->fapl >= 0) {
        H5E_BEGIN_TRY
        {
            H5Pclose(self->fapl);
        }
        H5E_END_TRY
        self->fapl = H5I_INVALID_HID;
    }
    if (close_status)
        *close_status = status;
}

static void
RawFile_dealloc(RawFileObject *self)
{
    rawfile_release(self, NULL);
    Py_XDECREF(self->path);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
RawFile_close(RawFileObject *self, PyObject *Py_UNUSED(ignored))
{
    herr_t status;

    if (self->fid < 0)
        Py_RETURN_NONE;
    rawfile_release(self, &status);
    if (status < 0) {
        raise_hdf5_error("could not close", self->path);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* (kind, size, order) for an atomic integer or float type; kind is None for
 * anything else. kind is 'i', 'u' or 'f'; order is '<' or '>'. */
static PyObject *
describe_type(hid_t type_id)
{
    H5T_class_t cls  = H5Tget_class(type_id);
    size_t      size = H5Tget_size(type_id);
    const char *kind = NULL;
    const char *order;

    if (cls == H5T_INTEGER)
        kind = H5Tget_sign(type_id) == H5T_SGN_NONE ? "u" : "i";
    else if (cls == H5T_FLOAT)
        kind = "f";
    order = H5Tget_order(type_id) == H5T_ORDER_BE ? ">" : "<";

    if (!kind)
        return Py_BuildValue("(OnO)", Py_None, (Py_ssize_t)size, Py_None);
    return Py_BuildValue("(sns)", kind, (Py_ssize_t)size, order);
}

static PyObject *
describe_extent(hid_t space_id)
{
    hsize_t   dims[H5S_MAX_RANK];
    int       rank = H5Sget_simple_extent_ndims(space_id);
    PyObject *tup;
    int       i;

    if (rank < 0)
        Py_RETURN_NONE;
    H5Sget_simple_extent_dims(space_id, dims, NULL);
    if (NULL == (tup = PyTuple_New(rank)))
        return NULL;
    for (i = 0; i < rank; i++) {
        PyObject *d = PyLong_FromUnsignedLongLong((unsigned long long)dims[i]);

        if (!d) {
            Py_DECREF(tup);
            return NULL;
        }
        PyTuple_SET_ITEM(tup, i, d);
    }
    return tup;
}

static PyObject *
RawFile_schema(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           step = 0;
    size_t             n = 0, i;
    H5F_stream_var_t  *vars = NULL;
    herr_t             status;
    PyObject          *list = NULL, *result = NULL;

    if (!PyArg_ParseTuple(args, "K:schema", &timeout_ms) || check_open(self) < 0)
        return NULL;

    H5E_BEGIN_TRY
    {
        status = H5Fget_stream_schema(self->fid, (uint64_t)timeout_ms, &step, &n, &vars);
    }
    H5E_END_TRY
    if (status < 0) {
        raise_hdf5_error("no stream schema (timed out, transport unavailable, or undecodable)", self->path);
        return NULL;
    }

    if (NULL == (list = PyList_New((Py_ssize_t)n)))
        goto done;
    for (i = 0; i < n; i++) {
        PyObject *type = describe_type(vars[i].type_id);
        PyObject *ext  = type ? describe_extent(vars[i].space_id) : NULL;
        PyObject *item = ext ? Py_BuildValue("(sOOO)", vars[i].path, vars[i].is_attr ? Py_True : Py_False,
                                            ext, type)
                             : NULL;

        Py_XDECREF(type);
        Py_XDECREF(ext);
        if (!item) {
            Py_CLEAR(list);
            goto done;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, item);
    }
    result = Py_BuildValue("(KO)", (unsigned long long)step, list);

done:
    Py_XDECREF(list);
    H5E_BEGIN_TRY
    {
        H5Ffree_stream_schema(n, vars);
    }
    H5E_END_TRY
    return result;
}

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

/* Dataspace for one subscription entry: (path, dims, start, count), where
 * start and count are None for the whole extent. */
static hid_t
entry_space(PyObject *entry, const char **path_out)
{
    PyObject *path, *dims_o, *start_o, *count_o;
    hsize_t   dims[H5S_MAX_RANK], start[H5S_MAX_RANK], count[H5S_MAX_RANK];
    int       rank, srank, crank;
    hid_t     space = H5I_INVALID_HID;

    if (!PyArg_ParseTuple(entry, "UOOO:subscribe entry", &path, &dims_o, &start_o, &count_o))
        return H5I_INVALID_HID;
    if (NULL == (*path_out = PyUnicode_AsUTF8(path)))
        return H5I_INVALID_HID;
    if (parse_dims(dims_o, "dims", dims, &rank) < 0)
        return H5I_INVALID_HID;

    if ((start_o == Py_None) != (count_o == Py_None)) {
        PyErr_SetString(PyExc_ValueError, "start and count must both be given, or neither");
        return H5I_INVALID_HID;
    }
    if (start_o != Py_None) {
        if (parse_dims(start_o, "start", start, &srank) < 0 || parse_dims(count_o, "count", count, &crank) < 0)
            return H5I_INVALID_HID;
        if (srank != rank || crank != rank) {
            PyErr_Format(PyExc_ValueError, "selection for %R has rank %d/%d, but the dataset has rank %d", path,
                         srank, crank, rank);
            return H5I_INVALID_HID;
        }
    }

    H5E_BEGIN_TRY
    {
        space = rank == 0 ? H5Screate(H5S_SCALAR) : H5Screate_simple(rank, dims, NULL);
        if (space >= 0 && rank > 0 && start_o != Py_None &&
            H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0) {
            H5Sclose(space);
            space = H5I_INVALID_HID;
        }
    }
    H5E_END_TRY
    if (space < 0)
        raise_hdf5_error("could not build the selection for", path);
    return space;
}

static PyObject *
RawFile_subscribe(RawFileObject *self, PyObject *args)
{
    PyObject    *entries, *fast = NULL, *result = NULL;
    Py_ssize_t   n = 0, i, built = 0;
    const char **paths  = NULL;
    hid_t       *spaces = NULL;
    herr_t       status;

    if (!PyArg_ParseTuple(args, "O:subscribe", &entries) || check_open(self) < 0)
        return NULL;
    if (NULL == (fast = PySequence_Fast(entries, "subscribe() takes a sequence of entries")))
        return NULL;
    if ((n = PySequence_Fast_GET_SIZE(fast)) == 0) {
        PyErr_SetString(PyExc_ValueError, "nothing to subscribe to");
        goto done;
    }
    paths  = PyMem_Calloc((size_t)n, sizeof(*paths));
    spaces = PyMem_Calloc((size_t)n, sizeof(*spaces));
    if (!paths || !spaces) {
        PyErr_NoMemory();
        goto done;
    }
    for (i = 0; i < n; i++, built++)
        if ((spaces[i] = entry_space(PySequence_Fast_GET_ITEM(fast, i), &paths[i])) < 0)
            goto done;

    H5E_BEGIN_TRY
    {
        status = H5Fsubscribe(self->fid, (size_t)n, paths, spaces, NULL);
    }
    H5E_END_TRY
    if (status < 0) {
        raise_hdf5_error("subscribe failed (transport unavailable?) for", self->path);
        goto done;
    }
    result = Py_NewRef(Py_None);

done:
    for (i = 0; i < built; i++)
        H5Sclose(spaces[i]);
    PyMem_Free(paths);
    PyMem_Free(spaces);
    Py_XDECREF(fast);
    return result;
}

/* A native in-memory type for (kind, size), or H5I_INVALID_HID after raising. */
static hid_t
native_type(const char *kind, Py_ssize_t size)
{
    if (kind[0] == 'i' && kind[1] == '\0') {
        switch (size) {
            case 1:
                return H5T_NATIVE_INT8;
            case 2:
                return H5T_NATIVE_INT16;
            case 4:
                return H5T_NATIVE_INT32;
            case 8:
                return H5T_NATIVE_INT64;
        }
    }
    else if (kind[0] == 'u' && kind[1] == '\0') {
        switch (size) {
            case 1:
                return H5T_NATIVE_UINT8;
            case 2:
                return H5T_NATIVE_UINT16;
            case 4:
                return H5T_NATIVE_UINT32;
            case 8:
                return H5T_NATIVE_UINT64;
        }
    }
    else if (kind[0] == 'f' && kind[1] == '\0') {
        if (size == 4)
            return H5T_NATIVE_FLOAT;
        if (size == 8)
            return H5T_NATIVE_DOUBLE;
    }
    PyErr_Format(PyExc_ValueError, "no native HDF5 type for kind '%s', size %zd", kind, size);
    return H5I_INVALID_HID;
}

static PyObject *
RawFile_subscribe_type(RawFileObject *self, PyObject *args)
{
    const char *path, *kind = NULL;
    Py_ssize_t  size = 0;
    hid_t       type = H5I_INVALID_HID;
    herr_t      status;

    if (!PyArg_ParseTuple(args, "szn:subscribe_type", &path, &kind, &size) || check_open(self) < 0)
        return NULL;
    if (kind && (type = native_type(kind, size)) < 0)
        return NULL;

    H5E_BEGIN_TRY
    {
        status = H5Fsubscribe_type(self->fid, path, type);
    }
    H5E_END_TRY
    if (status < 0) {
        raise_hdf5_error("subscribe_type failed (is the path subscribed?) for", self->path);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
RawFile_subscribe_predicate(RawFileObject *self, PyObject *args)
{
    const char *path;
    int         op;
    PyObject   *value;
    long long   ivalue;
    double      fvalue;
    const void *vptr;
    hid_t       vtype;
    herr_t      status;

    if (!PyArg_ParseTuple(args, "siO:subscribe_predicate", &path, &op, &value) || check_open(self) < 0)
        return NULL;
    if (op < H5VL_STREAM_PRED_LT || op > H5VL_STREAM_PRED_NE) {
        PyErr_Format(PyExc_ValueError, "unknown predicate operator %d", op);
        return NULL;
    }
    if (PyLong_Check(value)) {
        ivalue = PyLong_AsLongLong(value);
        if (ivalue == -1 && PyErr_Occurred())
            return NULL;
        vptr  = &ivalue;
        vtype = H5T_NATIVE_LLONG;
    }
    else if (PyFloat_Check(value)) {
        fvalue = PyFloat_AS_DOUBLE(value);
        vptr   = &fvalue;
        vtype  = H5T_NATIVE_DOUBLE;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "predicate value must be an int or a float");
        return NULL;
    }

    H5E_BEGIN_TRY
    {
        status = H5Fsubscribe_predicate(self->fid, path, (H5VL_stream_pred_op_t)op, vtype, vptr);
    }
    H5E_END_TRY
    if (status < 0) {
        raise_hdf5_error("subscribe_predicate failed (is the path subscribed?) for", self->path);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* (physical_step, wall_time_ns), or None on timeout. */
static PyObject *
RawFile_wait_step_ready(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           phys = 0, wall_ns = 0;
    herr_t             status;

    if (!PyArg_ParseTuple(args, "K:wait_step_ready", &timeout_ms) || check_open(self) < 0)
        return NULL;
    H5E_BEGIN_TRY
    {
        status = H5Fwait_step_ready(self->fid, (uint64_t)timeout_ms, &phys, &wall_ns);
    }
    H5E_END_TRY
    H5Eclear2(H5E_DEFAULT);
    if (status < 0)
        Py_RETURN_NONE;
    return Py_BuildValue("(KK)", (unsigned long long)phys, (unsigned long long)wall_ns);
}

/* (physical_step, path, elem_start, elem_count, Buffer), or None on timeout. */
static PyObject *
RawFile_get(RawFileObject *self, PyObject *args)
{
    unsigned long long timeout_ms;
    uint64_t           phys = 0, elem_start = 0, elem_count = 0;
    char              *path = NULL;
    void              *buf  = NULL;
    size_t             size = 0;
    herr_t             status;
    PyObject          *pybuf, *result;

    if (!PyArg_ParseTuple(args, "K:get", &timeout_ms) || check_open(self) < 0)
        return NULL;
    H5E_BEGIN_TRY
    {
        status = H5Fget_subscribed_data(self->fid, (uint64_t)timeout_ms, &phys, &path, &buf, &size, &elem_start,
                                        &elem_count);
    }
    H5E_END_TRY
    H5Eclear2(H5E_DEFAULT);
    if (status < 0)
        Py_RETURN_NONE;

    if (NULL == (pybuf = buffer_wrap(buf, size))) {
        free(path);
        return NULL;
    }
    result = Py_BuildValue("(KsKKN)", (unsigned long long)phys, path, (unsigned long long)elem_start,
                           (unsigned long long)elem_count, pybuf);
    free(path);
    return result;
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
     "schema(timeout_ms) -> (step, [(path, is_attr, dims, (kind, size, order))])"},
    {"subscribe", (PyCFunction)RawFile_subscribe, METH_VARARGS,
     "subscribe([(path, dims, start, count), ...]); start/count None for the whole extent"},
    {"subscribe_type", (PyCFunction)RawFile_subscribe_type, METH_VARARGS,
     "subscribe_type(path, kind, size); kind None clears the narrowing"},
    {"subscribe_predicate", (PyCFunction)RawFile_subscribe_predicate, METH_VARARGS,
     "subscribe_predicate(path, op, value); op is an H5VL_stream_pred_op_t value"},
    {"wait_step_ready", (PyCFunction)RawFile_wait_step_ready, METH_VARARGS,
     "wait_step_ready(timeout_ms) -> (physical_step, wall_time_ns) or None on timeout"},
    {"get", (PyCFunction)RawFile_get, METH_VARARGS,
     "get(timeout_ms) -> (physical_step, path, elem_start, elem_count, Buffer) or None on timeout"},
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
    hid_t          fapl       = H5I_INVALID_HID;
    hid_t          fid        = H5I_INVALID_HID;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O&:open", kwlist, PyUnicode_FSConverter, &path_bytes))
        return NULL;
    if (NULL == (path_str = PyUnicode_DecodeFSDefault(PyBytes_AS_STRING(path_bytes))))
        goto done;

    if (g_vol_id < 0) {
        H5E_BEGIN_TRY
        {
            g_vol_id = H5VL_stream_register();
        }
        H5E_END_TRY
        if (g_vol_id < 0) {
            raise_hdf5_error("could not register the vol-stream connector", NULL);
            goto done;
        }
    }

    H5E_BEGIN_TRY
    {
        if ((fapl = H5Pcreate(H5P_FILE_ACCESS)) >= 0 && H5Pset_vol(fapl, g_vol_id, NULL) >= 0 &&
            H5Pset_file_locking(fapl, false, true) >= 0)
            fid = H5Fopen(PyBytes_AS_STRING(path_bytes), H5F_ACC_RDONLY, fapl);
    }
    H5E_END_TRY
    if (fid < 0) {
        raise_hdf5_error("could not open", path_str);
        goto done;
    }

    if (NULL == (file = PyObject_New(RawFileObject, &RawFileType)))
        goto done;
    file->fid  = fid;
    file->fapl = fapl;
    file->path = Py_NewRef(path_str);
    fid        = H5I_INVALID_HID;
    fapl       = H5I_INVALID_HID;

done:
    if (fid >= 0) {
        H5E_BEGIN_TRY
        {
            H5Fclose(fid);
        }
        H5E_END_TRY
    }
    if (fapl >= 0) {
        H5E_BEGIN_TRY
        {
            H5Pclose(fapl);
        }
        H5E_END_TRY
    }
    Py_XDECREF(path_str);
    Py_XDECREF(path_bytes);
    return (PyObject *)file;
}

static PyMethodDef module_methods[] = {
    {"open", (PyCFunction)(void (*)(void))vs_open, METH_VARARGS | METH_KEYWORDS,
     "open(path) -> RawFile\n\nOpen a vol-stream file for reading."},
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
        PyModule_AddIntConstant(m, "PRED_NE", H5VL_STREAM_PRED_NE) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
