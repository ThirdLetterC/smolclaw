#include "plugin_script_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef SC_HAVE_CPYTHON
#include <Python.h>
#endif

#ifndef SC_HAVE_CPYTHON
sc_status sc_plugin_load_python_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out)
{
    (void)host;
    (void)manifest;
    (void)out;
    return sc_status_unsupported("sc.plugin_loader.python_unavailable");
}
#else
typedef struct sc_python_runtime_state {
    sc_allocator *alloc;
    sc_plugin *plugin;
    PyObject *module;
    PyObject *shutdown_fn;
} sc_python_runtime_state;

typedef struct sc_python_host_object {
    PyObject_HEAD
    sc_plugin *plugin;
} sc_python_host_object;

static sc_status python_runtime_acquire(void);
static void python_runtime_release(void);
static sc_status python_read_script(sc_allocator *alloc, sc_str path, sc_string *out);
static PyObject *python_host_register_tool(PyObject *self, PyObject *args);
static void python_host_dealloc(PyObject *self);
static sc_status python_invoke(void *runtime_state,
                               void *invoke_ref,
                               const sc_tool_call *call,
                               sc_str args_json,
                               sc_allocator *alloc,
                               sc_tool_result *out);
static void python_release_invoke(void *runtime_state, void *invoke_ref);
static void python_shutdown(void *runtime_state);
static void python_destroy(void *runtime_state);
static PyObject *python_json_dumps(PyObject *value);
static PyObject *python_json_loads(sc_str value);
static sc_status python_result_from_object(PyObject *result, sc_allocator *alloc, sc_tool_result *out);
static sc_status python_status_from_exception(const char *key);
static void python_set_status_exception(sc_status status);
static PyObject *PyInit_smolclaw(void);

static PyMethodDef python_host_methods[] = {
    {"register_tool", python_host_register_tool, METH_VARARGS, "Register a SmolClaw tool."},
    {nullptr, nullptr, 0, nullptr},
};

static PyTypeObject python_host_type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "smolclaw.Host",
    .tp_basicsize = sizeof(sc_python_host_object),
    .tp_dealloc = python_host_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = python_host_methods,
    .tp_new = PyType_GenericNew,
};

static PyModuleDef python_host_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "smolclaw",
    .m_doc = "SmolClaw trusted in-process plugin host API.",
    .m_size = -1,
};

static const sc_script_runtime_vtab python_runtime_vtab = {
    .struct_size = sizeof(python_runtime_vtab),
    .feature_flag = "SC_ENABLE_PYTHON_PLUGINS",
    .invoke = python_invoke,
    .release_invoke = python_release_invoke,
    .shutdown = python_shutdown,
    .destroy = python_destroy,
};

static size_t python_runtime_refs;
static bool python_module_appended;

sc_status sc_plugin_load_python_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out)
{
    sc_python_runtime_state *runtime = nullptr;
    sc_plugin *plugin = nullptr;
    sc_string source = {0};
    sc_status status;
    bool runtime_acquired = false;

    if (host == nullptr || manifest == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.python_invalid_argument");
    }
    *out = nullptr;
    runtime = sc_alloc(sc_allocator_heap(), sizeof(*runtime), _Alignof(sc_python_runtime_state));
    if (runtime == nullptr) {
        return sc_status_no_memory();
    }
    *runtime = (sc_python_runtime_state){.alloc = sc_allocator_heap()};

    status = python_runtime_acquire();
    if (sc_status_is_ok(status)) {
        runtime_acquired = true;
        status = sc_plugin_script_load_begin(host,
                                             manifest,
                                             SC_PLUGIN_CAP_PYTHON,
                                             runtime,
                                             &python_runtime_vtab,
                                             &plugin);
    }
    if (sc_status_is_ok(status)) {
        runtime->alloc = sc_plugin_script_allocator(plugin);
        runtime->plugin = plugin;
        status = python_read_script(runtime->alloc, sc_plugin_script_artifact_path(plugin), &source);
    }
    if (sc_status_is_ok(status)) {
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject *code = nullptr;
        PyObject *module = nullptr;
        PyObject *init_fn = nullptr;
        PyObject *host_obj = nullptr;
        PyObject *result = nullptr;
        PyObject *shutdown_fn = nullptr;
        char module_name[128] = {0};
        sc_str path = sc_plugin_script_artifact_path(plugin);

        (void)snprintf(module_name, sizeof(module_name), "smolclaw_plugin_%p", (void *)plugin);
        code = Py_CompileString(source.ptr, path.ptr, Py_file_input);
        if (code == nullptr) {
            status = python_status_from_exception("sc.plugin_loader.python_compile_failed");
            goto python_cleanup;
        }
        module = PyImport_ExecCodeModule(module_name, code);
        if (module == nullptr) {
            status = python_status_from_exception("sc.plugin_loader.python_import_failed");
            goto python_cleanup;
        }
        init_fn = PyObject_GetAttrString(module, "sc_plugin_init");
        if (init_fn == nullptr || !PyCallable_Check(init_fn)) {
            PyErr_Clear();
            status = sc_status_invalid_argument("sc.plugin_loader.python_missing_init");
            goto python_cleanup;
        }
        if (PyType_Ready(&python_host_type) < 0) {
            status = python_status_from_exception("sc.plugin_loader.python_host_failed");
            goto python_cleanup;
        }
        host_obj = PyObject_CallObject((PyObject *)&python_host_type, nullptr);
        if (host_obj == nullptr) {
            status = python_status_from_exception("sc.plugin_loader.python_host_failed");
            goto python_cleanup;
        }
        ((sc_python_host_object *)host_obj)->plugin = plugin;
        result = PyObject_CallFunctionObjArgs(init_fn, host_obj, nullptr);
        if (result == nullptr) {
            status = python_status_from_exception("sc.plugin_loader.python_init_failed");
            goto python_cleanup;
        }
        shutdown_fn = PyObject_GetAttrString(module, "sc_plugin_shutdown");
        if (shutdown_fn != nullptr) {
            if (!PyCallable_Check(shutdown_fn)) {
                status = sc_status_invalid_argument("sc.plugin_loader.python_shutdown_not_callable");
                goto python_cleanup;
            }
            runtime->shutdown_fn = shutdown_fn;
            shutdown_fn = nullptr;
        } else {
            PyErr_Clear();
        }
        runtime->module = module;
        module = nullptr;

python_cleanup:
        Py_XDECREF(shutdown_fn);
        Py_XDECREF(result);
        Py_XDECREF(host_obj);
        Py_XDECREF(init_fn);
        Py_XDECREF(module);
        Py_XDECREF(code);
        PyGILState_Release(gil);
    }
    if (sc_status_is_ok(status)) {
        status = sc_plugin_script_load_finish(plugin, out);
    }
    sc_string_clear(&source);
    if (!sc_status_is_ok(status)) {
        if (plugin != nullptr) {
            sc_plugin_script_load_abort(plugin);
        } else {
            if (runtime_acquired) {
                python_runtime_release();
            }
            sc_free(sc_allocator_heap(), runtime, sizeof(*runtime), _Alignof(sc_python_runtime_state));
        }
        return status;
    }
    return sc_status_ok();
}

static sc_status python_runtime_acquire(void)
{
    PyStatus py_status;
    PyConfig config;

    if (python_runtime_refs > 0) {
        python_runtime_refs += 1;
        return sc_status_ok();
    }
    if (!python_module_appended) {
        if (PyImport_AppendInittab("smolclaw", PyInit_smolclaw) != 0) {
            return sc_status_unsupported("sc.plugin_loader.python_inittab_failed");
        }
        python_module_appended = true;
    }
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    config.site_import = 0;
    py_status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(py_status)) {
        PyErr_Clear();
        return sc_status_unsupported("sc.plugin_loader.python_init_failed");
    }
    python_runtime_refs = 1;
    return sc_status_ok();
}

static void python_runtime_release(void)
{
    if (python_runtime_refs == 0) {
        return;
    }
    python_runtime_refs -= 1;
    if (python_runtime_refs == 0 && Py_IsInitialized()) {
        (void)Py_FinalizeEx();
    }
}

static sc_status python_read_script(sc_allocator *alloc, sc_str path, sc_string *out)
{
    enum { SC_PYTHON_MAX_SCRIPT_BYTES = 1 * 1'024 * 1'024 };
    FILE *file = nullptr;
    long size = 0;
    sc_string text = {0};
    sc_status status = sc_status_ok();

    if (path.ptr == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.python_invalid_argument");
    }
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        return sc_status_io("sc.plugin_loader.python_open_failed");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        status = sc_status_io("sc.plugin_loader.python_seek_failed");
        goto cleanup;
    }
    size = ftell(file);
    if (size <= 0 || size > SC_PYTHON_MAX_SCRIPT_BYTES) {
        status = sc_status_invalid_argument("sc.plugin_loader.python_size_invalid");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.plugin_loader.python_seek_failed");
        goto cleanup;
    }
    text.ptr = sc_alloc(alloc, (size_t)size + 1, _Alignof(char));
    if (text.ptr == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    text.alloc = alloc;
    text.len = (size_t)size;
    if (fread(text.ptr, 1, text.len, file) != text.len) {
        status = sc_status_io("sc.plugin_loader.python_read_failed");
        goto cleanup;
    }
    text.ptr[text.len] = '\0';
    *out = text;
    text = (sc_string){0};

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&text);
    return status;
}

static PyObject *python_host_register_tool(PyObject *self, PyObject *args)
{
    sc_python_host_object *host = (sc_python_host_object *)self;
    PyObject *spec = nullptr;
    PyObject *invoke = nullptr;
    PyObject *spec_json = nullptr;
    Py_ssize_t spec_len = 0;
    const char *spec_ptr = nullptr;
    sc_status status;

    if (!PyArg_ParseTuple(args, "OO:register_tool", &spec, &invoke)) {
        return nullptr;
    }
    if (host == nullptr || host->plugin == nullptr || !PyCallable_Check(invoke)) {
        PyErr_SetString(PyExc_TypeError, "register_tool requires a callable invoke function");
        return nullptr;
    }
    if (PyUnicode_Check(spec)) {
        spec_json = spec;
        Py_INCREF(spec_json);
    } else {
        spec_json = python_json_dumps(spec);
        if (spec_json == nullptr) {
            return nullptr;
        }
    }
    spec_ptr = PyUnicode_AsUTF8AndSize(spec_json, &spec_len);
    if (spec_ptr == nullptr || spec_len < 0) {
        Py_DECREF(spec_json);
        return nullptr;
    }
    Py_INCREF(invoke);
    status = sc_plugin_script_register_tool(host->plugin,
                                            sc_str_from_parts(spec_ptr, (size_t)spec_len),
                                            invoke);
    if (!sc_status_is_ok(status)) {
        Py_DECREF(invoke);
        Py_DECREF(spec_json);
        python_set_status_exception(status);
        return nullptr;
    }
    Py_DECREF(spec_json);
    Py_RETURN_NONE;
}

static void python_host_dealloc(PyObject *self)
{
    ((sc_python_host_object *)self)->plugin = nullptr;
    Py_TYPE(self)->tp_free(self);
}

static sc_status python_invoke(void *runtime_state,
                               void *invoke_ref,
                               const sc_tool_call *call,
                               sc_str args_json,
                               sc_allocator *alloc,
                               sc_tool_result *out)
{
    PyGILState_STATE gil;
    PyObject *args_obj = nullptr;
    PyObject *call_obj = nullptr;
    PyObject *call_id = nullptr;
    PyObject *result = nullptr;
    sc_status status;

    (void)runtime_state;
    if (invoke_ref == nullptr || call == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.python_invoke_invalid_argument");
    }
    gil = PyGILState_Ensure();
    args_obj = python_json_loads(args_json);
    if (args_obj == nullptr) {
        status = python_status_from_exception("sc.plugin_loader.python_args_parse_failed");
        goto cleanup;
    }
    call_obj = PyDict_New();
    if (call_obj == nullptr) {
        status = python_status_from_exception("sc.plugin_loader.python_call_create_failed");
        goto cleanup;
    }
    call_id = PyUnicode_FromStringAndSize(call->call_id.ptr == nullptr ? "" : call->call_id.ptr,
                                          (Py_ssize_t)call->call_id.len);
    if (call_id == nullptr || PyDict_SetItemString(call_obj, "call_id", call_id) != 0) {
        status = python_status_from_exception("sc.plugin_loader.python_call_create_failed");
        goto cleanup;
    }
    result = PyObject_CallFunctionObjArgs((PyObject *)invoke_ref, args_obj, call_obj, nullptr);
    if (result == nullptr) {
        status = python_status_from_exception("sc.plugin_loader.python_invoke_failed");
        goto cleanup;
    }
    status = python_result_from_object(result, alloc, out);

cleanup:
    Py_XDECREF(result);
    Py_XDECREF(call_id);
    Py_XDECREF(call_obj);
    Py_XDECREF(args_obj);
    PyGILState_Release(gil);
    return status;
}

static void python_release_invoke(void *runtime_state, void *invoke_ref)
{
    PyGILState_STATE gil;

    (void)runtime_state;
    if (invoke_ref == nullptr || !Py_IsInitialized()) {
        return;
    }
    gil = PyGILState_Ensure();
    Py_DECREF((PyObject *)invoke_ref);
    PyGILState_Release(gil);
}

static void python_shutdown(void *runtime_state)
{
    sc_python_runtime_state *runtime = runtime_state;
    PyGILState_STATE gil;
    PyObject *result = nullptr;

    if (runtime == nullptr || runtime->shutdown_fn == nullptr || !Py_IsInitialized()) {
        return;
    }
    gil = PyGILState_Ensure();
    result = PyObject_CallNoArgs(runtime->shutdown_fn);
    if (result == nullptr) {
        PyErr_Clear();
    }
    Py_XDECREF(result);
    PyGILState_Release(gil);
}

static void python_destroy(void *runtime_state)
{
    sc_python_runtime_state *runtime = runtime_state;

    if (runtime == nullptr) {
        return;
    }
    if (Py_IsInitialized()) {
        PyGILState_STATE gil = PyGILState_Ensure();
        Py_XDECREF(runtime->shutdown_fn);
        Py_XDECREF(runtime->module);
        PyGILState_Release(gil);
    }
    python_runtime_release();
    sc_free(runtime->alloc == nullptr ? sc_allocator_heap() : runtime->alloc,
            runtime,
            sizeof(*runtime),
            _Alignof(sc_python_runtime_state));
}

static PyObject *python_json_dumps(PyObject *value)
{
    PyObject *json = PyImport_ImportModule("json");
    PyObject *result = nullptr;

    if (json == nullptr) {
        return nullptr;
    }
    result = PyObject_CallMethod(json, "dumps", "O", value);
    Py_DECREF(json);
    return result;
}

static PyObject *python_json_loads(sc_str value)
{
    PyObject *json = PyImport_ImportModule("json");
    PyObject *text = nullptr;
    PyObject *result = nullptr;

    if (json == nullptr) {
        return nullptr;
    }
    text = PyUnicode_FromStringAndSize(value.ptr == nullptr ? "" : value.ptr, (Py_ssize_t)value.len);
    if (text != nullptr) {
        result = PyObject_CallMethod(json, "loads", "O", text);
    }
    Py_XDECREF(text);
    Py_DECREF(json);
    return result;
}

static sc_status python_result_from_object(PyObject *result, sc_allocator *alloc, sc_tool_result *out)
{
    PyObject *output_obj = nullptr;
    PyObject *success_obj = nullptr;
    Py_ssize_t len = 0;
    const char *ptr = nullptr;
    bool success = true;
    sc_status status;

    if (PyUnicode_Check(result)) {
        output_obj = result;
        Py_INCREF(output_obj);
    } else if (PyDict_Check(result)) {
        success_obj = PyDict_GetItemString(result, "success");
        if (success_obj != nullptr) {
            int truth = PyObject_IsTrue(success_obj);
            if (truth < 0) {
                return python_status_from_exception("sc.plugin_loader.python_result_invalid");
            }
            success = truth != 0;
        }
        output_obj = PyDict_GetItemString(result, "output");
        if (output_obj == nullptr || !PyUnicode_Check(output_obj)) {
            return sc_status_invalid_argument("sc.plugin_loader.python_result_invalid");
        }
        Py_INCREF(output_obj);
    } else {
        return sc_status_invalid_argument("sc.plugin_loader.python_result_invalid");
    }

    ptr = PyUnicode_AsUTF8AndSize(output_obj, &len);
    if (ptr == nullptr || len < 0) {
        Py_DECREF(output_obj);
        return python_status_from_exception("sc.plugin_loader.python_result_invalid");
    }
    out->success = success;
    status = sc_string_from_str(alloc, sc_str_from_parts(ptr, (size_t)len), &out->output);
    Py_DECREF(output_obj);
    return status;
}

static sc_status python_status_from_exception(const char *key)
{
    PyErr_Clear();
    return sc_status_invalid_argument(key);
}

static void python_set_status_exception(sc_status status)
{
    const char *key = status.error_key == nullptr ? "sc.plugin_loader.python_error" : status.error_key;
    PyErr_SetString(PyExc_RuntimeError, key);
    sc_status_clear(&status);
}

static PyObject *PyInit_smolclaw(void)
{
    PyObject *module = nullptr;

    if (PyType_Ready(&python_host_type) < 0) {
        return nullptr;
    }
    module = PyModule_Create(&python_host_module);
    if (module == nullptr) {
        return nullptr;
    }
    Py_INCREF(&python_host_type);
    if (PyModule_AddObject(module, "Host", (PyObject *)&python_host_type) != 0) {
        Py_DECREF(&python_host_type);
        Py_DECREF(module);
        return nullptr;
    }
    return module;
}
#endif
