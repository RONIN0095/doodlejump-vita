/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_jni.c
 * @brief Minimal per-object Java emulation on top of FalsoJNI.
 *
 * FalsoJNI keeps providing strings and arrays. Classes, objects, instance
 * fields and static method calls are replaced here, because the game creates
 * message objects with NewObject(), fills them with Set*Field() and hands them
 * to NotificationCenter.receiveMessageFromNative(), expecting the "Java side"
 * to write results back into the very same instance.
 */

#include "dj_jni.h"
#include "dj_java.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define DJ_MAX_FIELDS 24
#define DJ_OBJSET_SIZE 8192 // must be a power of two

typedef struct DJClass {
    char name[128]; // must stay first: FalsoJNI treats jclass as char*
    struct DJClass *next;
} DJClass;

typedef struct DJField {
    char name[64];
    char sig[64];
    struct DJField *next;
} DJField;

typedef struct DJMethod {
    DJClass *cls;
    char name[64];
    char sig[128];
    struct DJMethod *next;
} DJMethod;

typedef struct DJObject {
    DJClass *cls;
    int nfields;
    struct {
        DJField *f;
        jvalue v;
    } fields[DJ_MAX_FIELDS];
} DJObject;

static DJClass *classes = NULL;
static DJField *fields = NULL;
static DJMethod *methods = NULL;

static SceKernelLwMutexWork dj_lock;

// Open-addressing set of live DJObject pointers, so that we can tell our
// objects apart from FalsoJNI strings/arrays without dereferencing anything.
static uintptr_t objset[DJ_OBJSET_SIZE];
static int objset_count = 0;

static inline uint32_t ptr_hash(uintptr_t p) {
    return (uint32_t)((p >> 3) * 2654435761u) & (DJ_OBJSET_SIZE - 1);
}

static void objset_add(uintptr_t p) {
    if (objset_count >= DJ_OBJSET_SIZE * 3 / 4) {
        l_error("[DJ] Object set is full, leaking object tracking for %p", (void *)p);
        return;
    }
    uint32_t i = ptr_hash(p);
    while (objset[i] != 0 && objset[i] != 1)
        i = (i + 1) & (DJ_OBJSET_SIZE - 1);
    objset[i] = p;
    objset_count++;
}

static int objset_find(uintptr_t p) {
    uint32_t i = ptr_hash(p);
    for (int n = 0; n < DJ_OBJSET_SIZE; n++) {
        if (objset[i] == 0) return -1;
        if (objset[i] == p) return (int)i;
        i = (i + 1) & (DJ_OBJSET_SIZE - 1);
    }
    return -1;
}

static void objset_remove(uintptr_t p) {
    int i = objset_find(p);
    if (i >= 0) {
        objset[i] = 1; // tombstone
        objset_count--;
    }
}

/*
 * Interning
 */

static DJClass *intern_class(const char *name) {
    for (DJClass *c = classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    DJClass *c = calloc(1, sizeof(DJClass));
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->next = classes;
    classes = c;
    return c;
}

// Fields are interned by name only: Java-side helpers do not know signatures,
// and storage lives in the object anyway.
static DJField *intern_field(const char *name, const char *sig) {
    for (DJField *f = fields; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    DJField *f = calloc(1, sizeof(DJField));
    strncpy(f->name, name, sizeof(f->name) - 1);
    if (sig) strncpy(f->sig, sig, sizeof(f->sig) - 1);
    f->next = fields;
    fields = f;
    return f;
}

static DJMethod *intern_method(DJClass *cls, const char *name, const char *sig) {
    for (DJMethod *m = methods; m; m = m->next)
        if (m->cls == cls && strcmp(m->name, name) == 0 && strcmp(m->sig, sig) == 0)
            return m;
    DJMethod *m = calloc(1, sizeof(DJMethod));
    m->cls = cls;
    strncpy(m->name, name, sizeof(m->name) - 1);
    strncpy(m->sig, sig, sizeof(m->sig) - 1);
    m->next = methods;
    methods = m;
    return m;
}

/*
 * Objects
 */

int dj_is_object(jobject obj) {
    if (!obj) return 0;
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    int r = objset_find((uintptr_t)obj) >= 0;
    sceKernelUnlockLwMutex(&dj_lock, 1);
    return r;
}

static jobject new_object_of(DJClass *cls) {
    DJObject *o = calloc(1, sizeof(DJObject));
    o->cls = cls;
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    objset_add((uintptr_t)o);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    return (jobject)o;
}

jobject dj_new_object(const char *class_name) {
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    DJClass *c = intern_class(class_name);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    return new_object_of(c);
}

void dj_free_object(jobject obj) {
    if (!dj_is_object(obj)) return;
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    objset_remove((uintptr_t)obj);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    free(obj);
}

const char *dj_class_name(jobject obj) {
    if (!dj_is_object(obj)) return "<not an object>";
    return ((DJObject *)obj)->cls->name;
}

static jvalue *field_slot(jobject obj, DJField *f, int create) {
    if (!dj_is_object(obj)) {
        l_warn("[DJ] Field access '%s' on non-object %p", f ? f->name : "?", obj);
        return NULL;
    }
    DJObject *o = (DJObject *)obj;
    for (int i = 0; i < o->nfields; i++)
        if (o->fields[i].f == f) return &o->fields[i].v;
    if (!create) return NULL;
    if (o->nfields >= DJ_MAX_FIELDS) {
        l_error("[DJ] Too many fields on %s", o->cls->name);
        return NULL;
    }
    o->fields[o->nfields].f = f;
    memset(&o->fields[o->nfields].v, 0, sizeof(jvalue));
    return &o->fields[o->nfields++].v;
}

static jvalue field_get(jobject obj, jfieldID fid) {
    jvalue *v = field_slot(obj, (DJField *)fid, 0);
    if (v) return *v;
    jvalue zero;
    memset(&zero, 0, sizeof(zero));
    return zero;
}

static void field_set(jobject obj, jfieldID fid, jvalue val) {
    jvalue *v = field_slot(obj, (DJField *)fid, 1);
    if (v) *v = val;
}

static DJField *field_by_name(const char *name) {
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    DJField *f = intern_field(name, NULL);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    return f;
}

jint dj_get_int(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).i; }
jlong dj_get_long(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).j; }
jfloat dj_get_float(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).f; }
jboolean dj_get_bool(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).z; }
jobject dj_get_obj(jobject obj, const char *n) { return field_get(obj, (jfieldID)field_by_name(n)).l; }

char *dj_get_string(jobject obj, const char *n) {
    return dj_jstring_to_utf8((jstring)dj_get_obj(obj, n));
}

void dj_set_int(jobject obj, const char *n, jint v) { jvalue x; x.j = 0; x.i = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void dj_set_long(jobject obj, const char *n, jlong v) { jvalue x; x.j = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void dj_set_float(jobject obj, const char *n, jfloat v) { jvalue x; x.j = 0; x.f = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void dj_set_bool(jobject obj, const char *n, jboolean v) { jvalue x; x.j = 0; x.z = v; field_set(obj, (jfieldID)field_by_name(n), x); }
void dj_set_obj(jobject obj, const char *n, jobject v) { jvalue x; x.j = 0; x.l = v; field_set(obj, (jfieldID)field_by_name(n), x); }

void dj_set_string(jobject obj, const char *n, const char *utf8) {
    jstring s = utf8 ? (*dj_env())->NewStringUTF(dj_env(), utf8) : NULL;
    dj_set_obj(obj, n, s);
}

char *dj_jstring_to_utf8(jstring s) {
    if (!s) return NULL;
    JNIEnv *env = dj_env();
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (!c) return NULL;
    char *r = strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, (char *)c);
    return r;
}

JNIEnv *dj_env(void) {
    return &jni;
}

/*
 * Argument unpacking according to a method signature
 */

#define DJ_MAX_ARGS 16

static const char *sig_next(const char *p, char *type) {
    *type = *p;
    if (*p == 'L') {
        while (*p && *p != ';') p++;
        return *p ? p + 1 : p;
    }
    if (*p == '[') {
        while (*p == '[') p++;
        if (*p == 'L') {
            while (*p && *p != ';') p++;
            return *p ? p + 1 : p;
        }
        return *p ? p + 1 : p;
    }
    return p + 1;
}

static int args_from_va(const char *sig, va_list ap, jvalue *out) {
    const char *p = strchr(sig, '(');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ')' && n < DJ_MAX_ARGS) {
        char t;
        p = sig_next(p, &t);
        jvalue v;
        v.j = 0;
        switch (t) {
            case 'J': v.j = va_arg(ap, jlong); break;
            case 'F': v.f = (jfloat)va_arg(ap, double); break;
            case 'D': v.d = va_arg(ap, double); break;
            case 'L':
            case '[': v.l = va_arg(ap, jobject); break;
            default:  v.i = va_arg(ap, jint); break;
        }
        out[n++] = v;
    }
    return n;
}

static int args_from_a(const char *sig, const jvalue *in, jvalue *out) {
    const char *p = strchr(sig, '(');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ')' && n < DJ_MAX_ARGS) {
        char t;
        p = sig_next(p, &t);
        out[n] = in[n];
        n++;
    }
    return n;
}

/*
 * Static method dispatch: this is the "Java" the game calls into.
 */

static jvalue dispatch_static(DJMethod *m, const jvalue *a, int argc) {
    jvalue r;
    r.j = 0;

    if (!m) {
        l_error("[DJ] Static call on NULL method");
        return r;
    }

    if (strcmp(m->name, "receiveMessageFromNative") == 0 && argc >= 4) {
        r.i = dj_java_receive_message(a[0].i, a[1].l, a[2].i, a[3].i);
    } else if (strcmp(m->name, "receiveAudioMessageFromNative") == 0 && argc >= 4) {
        r.i = dj_java_receive_audio_message(a[0].i, a[1].j, a[2].f, a[3].i);
    } else if (strcmp(m->name, "getAssetManager") == 0) {
        r.l = dj_java_asset_manager();
    } else {
        l_warn("[DJ] Unhandled static method %s.%s%s", m->cls ? m->cls->name : "?", m->name, m->sig);
    }
    return r;
}

/*
 * JNIEnv replacements
 */

static jclass DJ_FindClass(JNIEnv *env, const char *name) {
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    DJClass *c = intern_class(name);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    l_debug("[DJ] FindClass(%s) = %p", name, c);
    return (jclass)c;
}

static jclass DJ_GetObjectClass(JNIEnv *env, jobject obj) {
    if (dj_is_object(obj)) return (jclass)((DJObject *)obj)->cls;
    return (jclass)intern_class("java/lang/Object");
}

static jboolean DJ_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz) {
    if (!obj) return JNI_TRUE;
    if (!dj_is_object(obj)) return JNI_FALSE;
    return ((DJObject *)obj)->cls == (DJClass *)clazz ? JNI_TRUE : JNI_FALSE;
}

static jobject DJ_AllocObject(JNIEnv *env, jclass clazz) {
    return new_object_of((DJClass *)clazz);
}

static jobject DJ_NewObjectV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) {
    DJMethod *m = (DJMethod *)mid;
    if (m && strcmp(m->sig, "()V") != 0)
        l_warn("[DJ] NewObject %s with ctor %s: arguments ignored", ((DJClass *)clazz)->name, m->sig);
    return new_object_of((DJClass *)clazz);
}

static jobject DJ_NewObject(JNIEnv *env, jclass clazz, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    jobject r = DJ_NewObjectV(env, clazz, mid, ap);
    va_end(ap);
    return r;
}

static jobject DJ_NewObjectA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *args) {
    return new_object_of((DJClass *)clazz);
}

static jmethodID DJ_GetMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    DJMethod *m = intern_method((DJClass *)clazz, name, sig);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    l_debug("[DJ] GetMethodID(%s, %s%s)", ((DJClass *)clazz)->name, name, sig);
    return (jmethodID)m;
}

static jfieldID DJ_GetFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    sceKernelLockLwMutex(&dj_lock, 1, NULL);
    DJField *f = intern_field(name, sig);
    sceKernelUnlockLwMutex(&dj_lock, 1);
    return (jfieldID)f;
}

static void DJ_DeleteLocalRef(JNIEnv *env, jobject obj) {
    // Message objects are short-lived and only referenced by the game.
    // Everything else (strings/arrays) is left to FalsoJNI semantics: ignored.
}

#define DJ_GET_FIELD(Name, Type, member) \
    static Type DJ_Get##Name##Field(JNIEnv *env, jobject obj, jfieldID fid) { \
        return field_get(obj, fid).member; \
    }

#define DJ_SET_FIELD(Name, Type, member) \
    static void DJ_Set##Name##Field(JNIEnv *env, jobject obj, jfieldID fid, Type val) { \
        jvalue v; v.j = 0; v.member = val; field_set(obj, fid, v); \
    }

DJ_GET_FIELD(Object, jobject, l)
DJ_GET_FIELD(Boolean, jboolean, z)
DJ_GET_FIELD(Byte, jbyte, b)
DJ_GET_FIELD(Char, jchar, c)
DJ_GET_FIELD(Short, jshort, s)
DJ_GET_FIELD(Int, jint, i)
DJ_GET_FIELD(Long, jlong, j)
DJ_GET_FIELD(Float, jfloat, f)
DJ_GET_FIELD(Double, jdouble, d)

DJ_SET_FIELD(Object, jobject, l)
DJ_SET_FIELD(Boolean, jboolean, z)
DJ_SET_FIELD(Byte, jbyte, b)
DJ_SET_FIELD(Char, jchar, c)
DJ_SET_FIELD(Short, jshort, s)
DJ_SET_FIELD(Int, jint, i)
DJ_SET_FIELD(Long, jlong, j)
DJ_SET_FIELD(Float, jfloat, f)
DJ_SET_FIELD(Double, jdouble, d)

static jmethodID DJ_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    return DJ_GetMethodID(env, clazz, name, sig);
}

#define DJ_CALL_STATIC(Name, Type, member) \
    static Type DJ_CallStatic##Name##MethodV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_va(((DJMethod *)mid)->sig, ap, a); \
        return (Type)dispatch_static((DJMethod *)mid, a, n).member; \
    } \
    static Type DJ_CallStatic##Name##Method(JNIEnv *env, jclass clazz, jmethodID mid, ...) { \
        va_list ap; \
        va_start(ap, mid); \
        Type r = DJ_CallStatic##Name##MethodV(env, clazz, mid, ap); \
        va_end(ap); \
        return r; \
    } \
    static Type DJ_CallStatic##Name##MethodA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *in) { \
        jvalue a[DJ_MAX_ARGS]; \
        int n = args_from_a(((DJMethod *)mid)->sig, in, a); \
        return (Type)dispatch_static((DJMethod *)mid, a, n).member; \
    }

DJ_CALL_STATIC(Object, jobject, l)
DJ_CALL_STATIC(Boolean, jboolean, z)
DJ_CALL_STATIC(Int, jint, i)
DJ_CALL_STATIC(Long, jlong, j)
DJ_CALL_STATIC(Float, jfloat, f)

static void DJ_CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID mid, va_list ap) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_va(((DJMethod *)mid)->sig, ap, a);
    dispatch_static((DJMethod *)mid, a, n);
}

static void DJ_CallStaticVoidMethod(JNIEnv *env, jclass clazz, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    DJ_CallStaticVoidMethodV(env, clazz, mid, ap);
    va_end(ap);
}

static void DJ_CallStaticVoidMethodA(JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *in) {
    jvalue a[DJ_MAX_ARGS];
    int n = args_from_a(((DJMethod *)mid)->sig, in, a);
    dispatch_static((DJMethod *)mid, a, n);
}

// Instance methods are never expected; log them so they show up while testing.
static void log_instance_call(jobject obj, jmethodID mid) {
    DJMethod *m = (DJMethod *)mid;
    (void)m; // only used when logging is enabled
    l_warn("[DJ] Unhandled instance method %s%s on %s", m ? m->name : "?", m ? m->sig : "", dj_class_name(obj));
}

#define DJ_CALL_INSTANCE(Name, Type) \
    static Type DJ_Call##Name##MethodV(JNIEnv *env, jobject obj, jmethodID mid, va_list ap) { \
        log_instance_call(obj, mid); \
        return (Type)0; \
    } \
    static Type DJ_Call##Name##Method(JNIEnv *env, jobject obj, jmethodID mid, ...) { \
        log_instance_call(obj, mid); \
        return (Type)0; \
    } \
    static Type DJ_Call##Name##MethodA(JNIEnv *env, jobject obj, jmethodID mid, const jvalue *in) { \
        log_instance_call(obj, mid); \
        return (Type)0; \
    }

DJ_CALL_INSTANCE(Object, jobject)
DJ_CALL_INSTANCE(Boolean, jboolean)
DJ_CALL_INSTANCE(Int, jint)
DJ_CALL_INSTANCE(Long, jlong)
DJ_CALL_INSTANCE(Float, jfloat)

static void DJ_CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID mid, va_list ap) {
    log_instance_call(obj, mid);
}

static void DJ_CallVoidMethod(JNIEnv *env, jobject obj, jmethodID mid, ...) {
    log_instance_call(obj, mid);
}

static void DJ_CallVoidMethodA(JNIEnv *env, jobject obj, jmethodID mid, const jvalue *in) {
    log_instance_call(obj, mid);
}

static jfieldID DJ_GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
    l_warn("[DJ] GetStaticFieldID(%s, %s, %s): static fields are not emulated", ((DJClass *)clazz)->name, name, sig);
    return DJ_GetFieldID(env, clazz, name, sig);
}

#define DJ_GET_STATIC_FIELD(Name, Type) \
    static Type DJ_GetStatic##Name##Field(JNIEnv *env, jclass clazz, jfieldID fid) { return (Type)0; }

DJ_GET_STATIC_FIELD(Object, jobject)
DJ_GET_STATIC_FIELD(Boolean, jboolean)
DJ_GET_STATIC_FIELD(Int, jint)
DJ_GET_STATIC_FIELD(Long, jlong)
DJ_GET_STATIC_FIELD(Float, jfloat)

/*
 * Direct byte buffers (Ljava/nio/Buffer;): a DJObject holding a pointer
 */

static jobject DJ_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
    jobject o = dj_new_object("java/nio/ByteBuffer");
    dj_set_long(o, "__address", (jlong)(uintptr_t)address);
    dj_set_long(o, "__capacity", capacity);
    return o;
}

static void *DJ_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
    return (void *)(uintptr_t)dj_get_long(buf, "__address");
}

static jlong DJ_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
    return dj_get_long(buf, "__capacity");
}

void dj_jni_install(void) {
    sceKernelCreateLwMutex(&dj_lock, "dj_jni", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);

    struct JNINativeInterface *t = (struct JNINativeInterface *)jni;

    t->FindClass = DJ_FindClass;
    t->GetObjectClass = DJ_GetObjectClass;
    t->IsInstanceOf = DJ_IsInstanceOf;
    t->AllocObject = DJ_AllocObject;
    t->NewObject = DJ_NewObject;
    t->NewObjectV = DJ_NewObjectV;
    t->NewObjectA = DJ_NewObjectA;
    t->DeleteLocalRef = DJ_DeleteLocalRef;

    t->GetMethodID = DJ_GetMethodID;
    t->GetFieldID = DJ_GetFieldID;

    t->GetObjectField = DJ_GetObjectField;
    t->GetBooleanField = DJ_GetBooleanField;
    t->GetByteField = DJ_GetByteField;
    t->GetCharField = DJ_GetCharField;
    t->GetShortField = DJ_GetShortField;
    t->GetIntField = DJ_GetIntField;
    t->GetLongField = DJ_GetLongField;
    t->GetFloatField = DJ_GetFloatField;
    t->GetDoubleField = DJ_GetDoubleField;

    t->SetObjectField = DJ_SetObjectField;
    t->SetBooleanField = DJ_SetBooleanField;
    t->SetByteField = DJ_SetByteField;
    t->SetCharField = DJ_SetCharField;
    t->SetShortField = DJ_SetShortField;
    t->SetIntField = DJ_SetIntField;
    t->SetLongField = DJ_SetLongField;
    t->SetFloatField = DJ_SetFloatField;
    t->SetDoubleField = DJ_SetDoubleField;

    t->CallObjectMethod = DJ_CallObjectMethod;
    t->CallObjectMethodV = DJ_CallObjectMethodV;
    t->CallObjectMethodA = DJ_CallObjectMethodA;
    t->CallBooleanMethod = DJ_CallBooleanMethod;
    t->CallBooleanMethodV = DJ_CallBooleanMethodV;
    t->CallBooleanMethodA = DJ_CallBooleanMethodA;
    t->CallIntMethod = DJ_CallIntMethod;
    t->CallIntMethodV = DJ_CallIntMethodV;
    t->CallIntMethodA = DJ_CallIntMethodA;
    t->CallLongMethod = DJ_CallLongMethod;
    t->CallLongMethodV = DJ_CallLongMethodV;
    t->CallLongMethodA = DJ_CallLongMethodA;
    t->CallFloatMethod = DJ_CallFloatMethod;
    t->CallFloatMethodV = DJ_CallFloatMethodV;
    t->CallFloatMethodA = DJ_CallFloatMethodA;
    t->CallVoidMethod = DJ_CallVoidMethod;
    t->CallVoidMethodV = DJ_CallVoidMethodV;
    t->CallVoidMethodA = DJ_CallVoidMethodA;

    t->GetStaticMethodID = DJ_GetStaticMethodID;
    t->CallStaticObjectMethod = DJ_CallStaticObjectMethod;
    t->CallStaticObjectMethodV = DJ_CallStaticObjectMethodV;
    t->CallStaticObjectMethodA = DJ_CallStaticObjectMethodA;
    t->CallStaticBooleanMethod = DJ_CallStaticBooleanMethod;
    t->CallStaticBooleanMethodV = DJ_CallStaticBooleanMethodV;
    t->CallStaticBooleanMethodA = DJ_CallStaticBooleanMethodA;
    t->CallStaticIntMethod = DJ_CallStaticIntMethod;
    t->CallStaticIntMethodV = DJ_CallStaticIntMethodV;
    t->CallStaticIntMethodA = DJ_CallStaticIntMethodA;
    t->CallStaticLongMethod = DJ_CallStaticLongMethod;
    t->CallStaticLongMethodV = DJ_CallStaticLongMethodV;
    t->CallStaticLongMethodA = DJ_CallStaticLongMethodA;
    t->CallStaticFloatMethod = DJ_CallStaticFloatMethod;
    t->CallStaticFloatMethodV = DJ_CallStaticFloatMethodV;
    t->CallStaticFloatMethodA = DJ_CallStaticFloatMethodA;
    t->CallStaticVoidMethod = DJ_CallStaticVoidMethod;
    t->CallStaticVoidMethodV = DJ_CallStaticVoidMethodV;
    t->CallStaticVoidMethodA = DJ_CallStaticVoidMethodA;

    t->GetStaticFieldID = DJ_GetStaticFieldID;
    t->GetStaticObjectField = DJ_GetStaticObjectField;
    t->GetStaticBooleanField = DJ_GetStaticBooleanField;
    t->GetStaticIntField = DJ_GetStaticIntField;
    t->GetStaticLongField = DJ_GetStaticLongField;
    t->GetStaticFloatField = DJ_GetStaticFloatField;

    t->NewDirectByteBuffer = DJ_NewDirectByteBuffer;
    t->GetDirectBufferAddress = DJ_GetDirectBufferAddress;
    t->GetDirectBufferCapacity = DJ_GetDirectBufferCapacity;

    l_success("[DJ] JNI object model installed.");
}
