/*
 * Copyright (C) 2026 Doodle Jump Vita port contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dj_jni.h
 * @brief Minimal per-object Java emulation on top of FalsoJNI.
 *
 * Doodle Jump talks to its Java side through small message objects
 * (Messages$Msg*Data) whose fields are read and written with JNI on a
 * per-instance basis. FalsoJNI only knows about global fields, so we replace
 * the object/field/static-call parts of its JNIEnv with our own.
 */

#ifndef DJ_JNI_H
#define DJ_JNI_H

#include <falso_jni/FalsoJNI.h>

void dj_jni_install(void);

JNIEnv *dj_env(void);

jobject dj_new_object(const char *class_name);
void dj_free_object(jobject obj);
int dj_is_object(jobject obj);
const char *dj_class_name(jobject obj);

jint dj_get_int(jobject obj, const char *field);
jlong dj_get_long(jobject obj, const char *field);
jfloat dj_get_float(jobject obj, const char *field);
jboolean dj_get_bool(jobject obj, const char *field);
jobject dj_get_obj(jobject obj, const char *field);
// Returns a malloc'd UTF-8 copy of a String field, or NULL.
char *dj_get_string(jobject obj, const char *field);

void dj_set_int(jobject obj, const char *field, jint v);
void dj_set_long(jobject obj, const char *field, jlong v);
void dj_set_float(jobject obj, const char *field, jfloat v);
void dj_set_bool(jobject obj, const char *field, jboolean v);
void dj_set_obj(jobject obj, const char *field, jobject v);
void dj_set_string(jobject obj, const char *field, const char *utf8);

// Returns a malloc'd UTF-8 copy of a Java string, or NULL.
char *dj_jstring_to_utf8(jstring s);

#endif // DJ_JNI_H
