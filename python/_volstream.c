/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.  All rights reserved.                          *
 * This file is part of vol-stream.  See the LICENSE file at the root of the   *
 * source distribution, or https://www.hdfgroup.org/licenses.                  *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The CPython extension behind the `volstream` package: a subscriber-only
 * binding over the connector's own C API (docs/python-plan.md).
 *
 * No hid_t crosses into Python. A File owns its file id and FAPL and is the
 * only thing that ever closes them.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "hdf5.h"
#include "H5VLstream.h"

static PyObject *VolstreamError;

/* Registered on first open and kept for the life of the process. */
static hid_t g_vol_id = H5I_INVALID_HID;

typedef struct {
    PyObject_HEAD
    hid_t     fid;
    hid_t     fapl;
    PyObject *path;
} FileObject;

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

static void
file_release(FileObject *self, herr_t *close_status)
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
File_dealloc(FileObject *self)
{
    file_release(self, NULL);
    Py_XDECREF(self->path);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
File_close(FileObject *self, PyObject *Py_UNUSED(ignored))
{
    herr_t status;

    if (self->fid < 0)
        Py_RETURN_NONE;
    file_release(self, &status);
    if (status < 0) {
        raise_hdf5_error("could not close", self->path);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
File_get_closed(FileObject *self, void *Py_UNUSED(closure))
{
    return PyBool_FromLong(self->fid < 0);
}

static PyObject *
File_get_path(FileObject *self, void *Py_UNUSED(closure))
{
    return Py_NewRef(self->path);
}

static PyObject *
File_repr(FileObject *self)
{
    return PyUnicode_FromFormat("<volstream.File %R (%s)>", self->path, self->fid < 0 ? "closed" : "open");
}

static PyMethodDef File_methods[] = {
    {"close", (PyCFunction)File_close, METH_NOARGS,
     "Close the file. Safe to call more than once."},
    {NULL, NULL, 0, NULL}};

static PyGetSetDef File_getset[] = {
    {"closed", (getter)File_get_closed, NULL, "True once close() has run.", NULL},
    {"path", (getter)File_get_path, NULL, "The path this file was opened with.", NULL},
    {NULL, NULL, NULL, NULL, NULL}};

static PyTypeObject FileType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "volstream.File",
    .tp_doc                                = "A vol-stream file opened for reading. Create with volstream.open().",
    .tp_basicsize                          = sizeof(FileObject),
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_dealloc                            = (destructor)File_dealloc,
    .tp_repr                               = (reprfunc)File_repr,
    .tp_methods                            = File_methods,
    .tp_getset                             = File_getset,
};

static PyObject *
vs_open(PyObject *Py_UNUSED(module), PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"path", NULL};
    PyObject    *path_bytes = NULL;
    PyObject    *path_str   = NULL;
    FileObject  *file       = NULL;
    hid_t        fapl       = H5I_INVALID_HID;
    hid_t        fid        = H5I_INVALID_HID;

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

    if (NULL == (file = PyObject_New(FileObject, &FileType)))
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
     "open(path) -> File\n\nOpen a vol-stream file for reading."},
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

    if (PyType_Ready(&FileType) < 0)
        return NULL;
    if (NULL == (m = PyModule_Create(&volstream_module)))
        return NULL;

    VolstreamError = PyErr_NewExceptionWithDoc("volstream.Error", "A vol-stream or HDF5 call failed.", NULL, NULL);
    if (!VolstreamError || PyModule_AddObjectRef(m, "Error", VolstreamError) < 0 ||
        PyModule_AddObjectRef(m, "File", (PyObject *)&FileType) < 0) {
        Py_DECREF(m);
        return NULL;
    }
    return m;
}
