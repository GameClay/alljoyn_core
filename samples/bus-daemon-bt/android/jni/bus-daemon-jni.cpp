/******************************************************************************
 * Copyright 2010 - 2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 *
 ******************************************************************************/
#define QCC_OS_GROUP_POSIX
#include <string.h>
#include <jni.h>
#include <stdio.h>
#include <assert.h>
#include "BTLiteController.h"
#include "qcc/String.h"
#include <qcc/Debug.h>
#include <qcc/Log.h>
#include <qcc/ManagedObj.h>

#define LOG_TAG "bus-daemon-jni"
#define QCC_MODULE "ALLJOYN_Daemon"
#define QCC_OS_ANDROID

using namespace qcc;
using namespace ajn;
//
// The AllJoyn daemon has an alternate personality in that it is built as a
// static library, liballjoyn-daemon.a.  In this case, the entry point main() is
// replaced by a function called DaemonMain.  Calling DaemonMain() here
// essentially runs the AllJoyn daemon like it had been run on the command line.
//
extern "C" {
extern int DaemonMain(int argc, char** argv, char* config);
}

namespace ajn {
extern BTLiteController* z_btLiteController;
}

/** The cached JVM pointer, valid across all contexts. */
static JavaVM* jvm = NULL;

/** java/lang cached items - these are guaranteed to be loaded at all times. */
static jclass CLS_Object = NULL;
static jclass CLS_String = NULL;
static jclass CLS_BTLiteController = NULL;

/**
 * Get a valid JNIEnv pointer.
 *
 * A JNIEnv pointer is only valid in an associated JVM thread.  In a callback
 * function (from C++), there is no associated JVM thread, so we need to obtain
 * a valid JNIEnv.  This is a helper function to make that happen.
 *
 * @return The JNIEnv pointer valid in the calling context.
 */
static JNIEnv* GetEnv(jint* result = 0)
{
    JNIEnv* env;
    jint ret = jvm->GetEnv((void**)&env, JNI_VERSION_1_2);
    if (result) {
        *result = ret;
    }
    if (JNI_EDETACHED == ret) {
#if defined(QCC_OS_ANDROID)
        ret = jvm->AttachCurrentThread(&env, NULL);
#else
        ret = jvm->AttachCurrentThread((void**)&env, NULL);
#endif
    }
    assert(JNI_OK == ret);
    return env;
}

/**
 * Inverse of GetEnv.
 */
static void DeleteEnv(jint result)
{
    if (JNI_EDETACHED == result) {
        jvm->DetachCurrentThread();
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,
                                  void* reserved)
{
    QCC_DbgPrintf(("JNI_OnLoad()\n"));
    jvm = vm;
    JNIEnv* env;
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_2)) {
        return JNI_ERR;
    } else {
        jclass clazz;
        clazz = env->FindClass("java/lang/Object");
        if (!clazz) {
            return JNI_ERR;
        }
        CLS_Object = (jclass)env->NewGlobalRef(clazz);

        clazz = env->FindClass("java/lang/String");
        if (!clazz) {
            return JNI_ERR;
        }
        CLS_String = (jclass)env->NewGlobalRef(clazz);

        clazz = env->FindClass("org/alljoyn/bus/daemonservice/BTLiteController");
        if (!clazz) {
            return JNI_ERR;
        }
        CLS_BTLiteController = (jclass)env->NewGlobalRef(clazz);
        return JNI_VERSION_1_2;
    }
}

/**
 * A helper class to wrap local references ensuring proper release.
 */
template <class T>
class JLocalRef {
  public:
    JLocalRef() : jobj(NULL) { }
    JLocalRef(const T& obj) : jobj(obj) { }
    ~JLocalRef() { if (jobj) GetEnv()->DeleteLocalRef(jobj); }
    JLocalRef& operator=(T obj)
    {
        if (jobj) GetEnv()->DeleteLocalRef(jobj);
        jobj = obj;
        return *this;
    }
    operator T() { return jobj; }
    T move()
    {
        T ret = jobj;
        jobj = NULL;
        return ret;
    }
  private:
    T jobj;
};

/**
 * Helper function to wrap StringUTFChars to ensure proper release of resource.
 *
 * @warning NULL is a valid value, so exceptions must be checked for explicitly
 * by the caller after constructing the JString.
 */
class JString {
  public:
    JString(jstring s);
    ~JString();
    const char* c_str() { return str; }
  private:
    jstring jstr;
    const char* str;
};

/**
 * Construct a representation of a string with wraped StringUTFChars.
 *
 * @param s the string to wrap.
 */
JString::JString(jstring s)
    : jstr(s), str(jstr ? GetEnv()->GetStringUTFChars(jstr, NULL) : NULL)
{
}

/**
 * Destroy a string with wraped StringUTFChars.
 */
JString::~JString()
{
    if (str) GetEnv()->ReleaseStringUTFChars(jstr, str);
}

/**
 * A scoped JNIEnv pointer to ensure proper release.
 */
class JScopedEnv {
  public:
    JScopedEnv();
    ~JScopedEnv();
    JNIEnv* operator->() { return env; }
  private:
    JNIEnv* env;
    jint detached;
};

/**
 * Construct a scoped JNIEnv pointer.
 */
JScopedEnv::JScopedEnv()
    : env(GetEnv(&detached))
{
}

/**
 * Destroy a scoped JNIEnv pointer.
 */
JScopedEnv::~JScopedEnv()
{
    /* Clear any pending exceptions before detaching. */
    {
        JLocalRef<jthrowable> ex = env->ExceptionOccurred();
        if (ex) {
            env->ExceptionClear();
            // env->CallStaticVoidMethod(CLS_BusException, MID_BusException_log, (jthrowable)ex); TODO
        }
    }
    DeleteEnv(detached);
}

/**
 * Helper function to throw an exception
 */
static void Throw(const char* name, const char* msg)
{
    JNIEnv* env = GetEnv();
    JLocalRef<jclass> clazz = env->FindClass(name);
    if (clazz) {
        env->ThrowNew(clazz, msg);
    }
}


/**
 * Get the native C++ handle of a given Java object.
 *
 * If we have an object that has a native counterpart, we need a way to get at
 * the native object from the Java object.  We do this by storing the native
 * pointer as an opaque handle in a Java field named "handle".  We use Java
 * reflection to pull the field out and return the handle value.
 *
 * Think of this handle as the counterpart to the object reference found in
 * the C++ objects that need to call into Java.  Java objects use the handle to
 * get at the C++ objects, and C++ objects use an object reference to get at
 * the Java objects.
 *
 * @return The handle value as a pointer.  NULL is a valid value, so
 *         exceptions must be checked for explicitly by the caller.
 */
static void* GetHandle(jobject jobj)
{
    JNIEnv* env = GetEnv();
    if (!jobj) {
        Throw("java/lang/NullPointerException", "failed to get native handle on null object");
        return NULL;
    }
    JLocalRef<jclass> clazz = env->GetObjectClass(jobj);
    jfieldID fid = env->GetFieldID(clazz, "handle", "J");
    void* handle = NULL;
    if (fid) {
        handle = (void*)env->GetLongField(jobj, fid);
    }
    return handle;
}

/**
 * Set the native C++ handle of a given Java object.
 *
 * If we have an object that has a native counterpart, we need a way to get at
 * the native object from the Java object.  We do this by storing the native
 * pointer as an opaque handle in a Java field named "handle".  We use Java
 * reflection to determine the field out and set the handle value.
 *
 * @param jobj The Java object which needs to have its handle set.
 * @param handle The pointer to the C++ object which is the handle value.
 *
 * @warning May throw an exception.
 */
static void SetHandle(jobject jobj, void* handle)
{
    JNIEnv* env = GetEnv();
    if (!jobj) {
        Throw("java/lang/NullPointerException", "failed to set native handle on null object");
        return;
    }
    JLocalRef<jclass> clazz = env->GetObjectClass(jobj);
    jfieldID fid = env->GetFieldID(clazz, "handle", "J");
    if (fid) {
        env->SetLongField(jobj, fid, (jlong)handle);
    }
}

class JBTLiteController : public ajn::BTLiteController {
  public:
    JBTLiteController(jobject jobj);
    ~JBTLiteController();

    void EnsureDiscoverable();
    void EnableAdvertisement(qcc::String name);
    void DisableAdvertisement(qcc::String name);
    void EnableDiscovery(qcc::String namePrefix);
    void DisableDiscovery(qcc::String namePrefix);
    void StartListen();
    qcc::String Connect(qcc::String spec);
    int DisConnect(qcc::String spec);
    void EndpointExit(qcc::String uniqueID);

  private:
    jweak jbtcontroller;
    jmethodID MID_ensureDiscoverable;
    jmethodID MID_advertiseName;
    jmethodID MID_removeAvertizedName;
    jmethodID MID_startDiscovery;
    jmethodID MID_stopDiscovery;
    jmethodID MID_startListen;
    jmethodID MID_connect;
    jmethodID MID_disConnect;
    jmethodID MID_endpointExit;
};

JBTLiteController::JBTLiteController(jobject jobj)
{
    JNIEnv* env = GetEnv();
    jbtcontroller = (jweak)env->NewGlobalRef(jobj);
    if (!jbtcontroller) {
        return;
    }
    JLocalRef<jclass> clazz = env->GetObjectClass(jbtcontroller);

    MID_ensureDiscoverable = env->GetMethodID(clazz, "ensureDiscoverable", "()V");
    if (!MID_ensureDiscoverable) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find ensureDiscoverable() in jbtcontroller\n"));
        return;
    }

    MID_advertiseName = env->GetMethodID(clazz, "advertiseName", "(Ljava/lang/String;)V");
    if (!MID_advertiseName) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find advertiseName() in jbtcontroller\n"));
        return;
    }

    MID_removeAvertizedName = env->GetMethodID(clazz, "removeAvertizedName", "(Ljava/lang/String;)V");
    if (!MID_removeAvertizedName) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find removeAvertizedName() in jbtcontroller\n"));
        return;
    }

    MID_startListen = env->GetMethodID(clazz, "startListen", "()V");
    if (!MID_startListen) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find startListen() in jbtcontroller\n"));
        return;
    }

    MID_startDiscovery = env->GetMethodID(clazz, "startDiscovery", "(Ljava/lang/String;)V");
    if (!MID_startDiscovery) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find startDiscovery() in jbtcontroller\n"));
        return;
    }

    MID_stopDiscovery = env->GetMethodID(clazz, "stopDiscovery", "(Ljava/lang/String;)V");
    if (!MID_startDiscovery) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find stopDiscovery() in jbtcontroller\n"));
        return;
    }

    MID_connect = env->GetMethodID(clazz, "connect", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!MID_connect) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find connect() in jbtcontroller\n"));
        return;
    }

    MID_disConnect = env->GetMethodID(clazz, "disConnect", "(Ljava/lang/String;)I");
    if (!MID_disConnect) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find disConnect() in jbtcontroller\n"));
        return;
    }

    MID_endpointExit = env->GetMethodID(clazz, "endpointExit", "(Ljava/lang/String;)V");
    if (!MID_endpointExit) {
        QCC_LogError(ER_FAIL, ("JBTLiteController::JBTLiteController(): Can't find endpointExit() in jbtcontroller\n"));
        return;
    }
}

JBTLiteController::~JBTLiteController()
{
    JNIEnv* env = GetEnv();
    if (jbtcontroller) {
        env->DeleteGlobalRef(jbtcontroller);
        jbtcontroller = NULL;
    }
}

void JBTLiteController::EnsureDiscoverable()
{
    QCC_DbgPrintf(("JBTLiteController::EnsureDiscoverable"));
    JScopedEnv env;
    env->CallVoidMethod(jbtcontroller, MID_ensureDiscoverable);
}

void JBTLiteController::EnableAdvertisement(String name)
{
    JScopedEnv env;
    JLocalRef<jstring> jname = env->NewStringUTF(name.c_str());
    if (env->ExceptionCheck()) {
        return;
    }
    QCC_DbgPrintf(("JBTLiteController::EnableAdvertisement"));
    env->CallVoidMethod(jbtcontroller, MID_advertiseName, (jstring)jname);
}

void JBTLiteController::DisableAdvertisement(String name)
{
    JScopedEnv env;
    JLocalRef<jstring> jname = env->NewStringUTF(name.c_str());
    if (env->ExceptionCheck()) {
        return;
    }
    env->CallVoidMethod(jbtcontroller, MID_removeAvertizedName, (jstring)jname);
}

void JBTLiteController::EnableDiscovery(String namePrefix)
{
    JScopedEnv env;
    JLocalRef<jstring> jname = env->NewStringUTF(namePrefix.c_str());
    if (env->ExceptionCheck()) {
        return;
    }
    env->CallVoidMethod(jbtcontroller, MID_startDiscovery, (jstring)jname);
}

void JBTLiteController::DisableDiscovery(qcc::String namePrefix) {
    JScopedEnv env;
    JLocalRef<jstring> jname = env->NewStringUTF(namePrefix.c_str());
    if (env->ExceptionCheck()) {
        return;
    }
    env->CallVoidMethod(jbtcontroller, MID_stopDiscovery, (jstring)jname);
}

void JBTLiteController::StartListen()
{
    JScopedEnv env;
    env->CallVoidMethod(jbtcontroller, MID_startListen);
}

qcc::String JBTLiteController::Connect(qcc::String spec)
{
    JScopedEnv env;
    JLocalRef<jstring> jspec = env->NewStringUTF(spec.c_str());
    if (env->ExceptionCheck()) {
        return (const char*)NULL;
    }
    jstring juniqueID = (jstring)env->CallObjectMethod(jbtcontroller, MID_connect, (jstring)jspec);
    if (juniqueID == NULL) {
        return (const char*)NULL;
    }

    JString uniqueID(juniqueID);
    return uniqueID.c_str();
}

int JBTLiteController::DisConnect(qcc::String spec)
{
    JScopedEnv env;
    JLocalRef<jstring> jspec = env->NewStringUTF(spec.c_str());
    if (env->ExceptionCheck()) {
        return -1;
    }
    return (int)env->CallObjectMethod(jbtcontroller, MID_disConnect, (jstring)jspec);
}

void JBTLiteController::EndpointExit(qcc::String uniqueID)
{
    JScopedEnv env;
    JLocalRef<jstring> juniqueID = env->NewStringUTF(uniqueID.c_str());
    if (env->ExceptionCheck()) {
        return;
    }
    env->CallVoidMethod(jbtcontroller, MID_endpointExit, (jstring)juniqueID);
}

extern "C" {

JNIEXPORT void JNICALL Java_org_alljoyn_bus_daemonservice_DaemonService_registerBTController(JNIEnv* env, jobject thiz, jobject jbtcontroller) {

    JBTLiteController* btcontroller = jbtcontroller ? new JBTLiteController(jbtcontroller) : NULL;
    if (!btcontroller) {
        Throw("java/lang/OutOfMemoryError", NULL);
    }
    if (env->ExceptionCheck()) {
        return;
    }

    /*
     * Point the handle field in the Java object to the C++ object.  To avoid
     * memory leaks, this handle field must be zero.  Since GetHandle is going
     * to do reflection, it may throw an exception.
     */
    assert(GetHandle(jbtcontroller) == NULL);
    SetHandle(jbtcontroller, btcontroller);
    if (env->ExceptionCheck()) {
        delete btcontroller;
        return;
    }

    ajn::z_btLiteController = btcontroller;
}

JNIEXPORT void JNICALL Java_org_alljoyn_bus_daemonservice_DaemonService_unregisterBTController(JNIEnv* env, jobject thiz, jobject jbtcontroller) {
    JBTLiteController* btController = (JBTLiteController*)GetHandle(jbtcontroller);
    if (btController) {
        QCC_DbgPrintf(("Java_org_alljoyn_bus_daemonservice_DaemonService_unregisterBTController\n"));
        delete btController;
        SetHandle(jbtcontroller, NULL);
    }
    ajn::z_btLiteController = NULL;
}

JNIEXPORT void JNICALL Java_org_alljoyn_bus_daemonservice_BTLiteController_foundName(JNIEnv* env, jobject thiz, jstring jname, jstring jguid, jstring jaddr, jstring jport) {
    ajn::BTLiteController* btcontroller = (ajn::BTLiteController*)GetHandle(thiz);
    assert(btcontroller);
    JString name(jname);
    JString addr(jaddr);
    JString port(jport);
    JString guid(jguid);
    if (env->ExceptionCheck()) {
        return;
    }
    QCC_DbgPrintf(("Java_org_alljoyn_bus_daemonservice_BTLiteController_foundName() %s", name.c_str()));
    btcontroller->FoundName(name.c_str(), guid.c_str(), addr.c_str(), port.c_str());
}

JNIEXPORT jstring JNICALL Java_org_alljoyn_bus_daemonservice_BTLiteController_getGlobalGUID(JNIEnv* env, jobject thiz) {
    ajn::BTLiteController* btcontroller = (ajn::BTLiteController*)GetHandle(thiz);
    assert(btcontroller);
    if (env->ExceptionCheck()) {
        return NULL;
    }
    QCC_DbgPrintf(("Java_org_alljoyn_bus_daemonservice_BTLiteController_getGlobalGUID()"));
    return env->NewStringUTF(btcontroller->GetGlobalGUID().c_str());
}

JNIEXPORT void JNICALL Java_org_alljoyn_bus_daemonservice_BTLiteController_accepted(JNIEnv* env, jobject thiz, jstring juniqueID) {
    ajn::BTLiteController* btcontroller = (ajn::BTLiteController*)GetHandle(thiz);
    assert(btcontroller);
    JString uniqueID(juniqueID);
    if (env->ExceptionCheck()) {
        return;
    }
    QCC_DbgPrintf(("Java_org_alljoyn_bus_daemonservice_BTLiteController_accepted() %s", uniqueID.c_str()));
    btcontroller->Accepted(uniqueID.c_str());
}

JNIEXPORT void JNICALL Java_org_alljoyn_bus_daemonservice_DaemonService_runDaemon(JNIEnv* env, jobject thiz, jobjectArray jargv, jstring jconfig)
{
    int i;
    jsize argc;

    QCC_DbgPrintf(("runDaemon()\n"));

    argc = env->GetArrayLength(jargv);
    QCC_DbgPrintf(("runDaemon(): argc = %d\n", argc));

    int nBytes = argc * sizeof(char*);
    QCC_DbgPrintf(("runDaemon(): nBytes = %d\n", nBytes));

    QCC_DbgPrintf(("runDaemon(): malloc(%d)\n", nBytes));
    char const** argv  = (char const**)malloc(nBytes);

    for (i = 0; i < argc; ++i) {
        QCC_DbgPrintf(("runDaemon(): copy out string %d\n", i));
        jstring jstr = (jstring)env->GetObjectArrayElement(jargv, i);
        QCC_DbgPrintf(("runDaemon(): set pointer in argv[%d]\n", i));
        argv[i] = env->GetStringUTFChars(jstr, 0);
        QCC_DbgPrintf(("runDaemon(): argv[%d] = %s\n", i, argv[i]));
    }

    char const* config = env->GetStringUTFChars(jconfig, 0);
    QCC_DbgPrintf(("runDaemon(): config = %s\n", config));

    QCC_DbgPrintf(("runDaemon(): calling DaemonMain()\n"));
    int rc = DaemonMain(argc, (char**)argv, (char*)config);

    env->ReleaseStringUTFChars(jconfig, config);

    free(argv);
}
}


