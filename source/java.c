#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <stdlib.h>

#include "utils/logger.h"

/*
 * JNI Methods
 *
 * Java side being replaced: org.libsdl.app.SDLActivity (SDL 2.0.x) and
 * com.doublefine.cq.CqActivity (OUYA build). Names/signatures come from the
 * strings in libCq.so and the decompiled classes.dex.
 */

// Any non-NULL handle works for objects that are only passed back to us
#define FAKE_OBJECT ((jobject)0x42424242)

// SDLActivity.getNativeSurface ()Landroid/view/Surface;
// Not really used: Android_JNI_GetNativeWindow is hooked in patch.c
jobject SDLActivity_getNativeSurface(jmethodID id, va_list args) {
    return FAKE_OBJECT;
}

// SDLActivity.getContext ()Landroid/content/Context;
jobject SDLActivity_getContext(jmethodID id, va_list args) {
    return FAKE_OBJECT;
}

// SDLActivity.flipBuffers ()V
// Unused: Android_GLES_SwapWindow is hooked in patch.c
void SDLActivity_flipBuffers(jmethodID id, va_list args) {}

// SDLActivity.audioInit (IZZI)I
// SDL's own audio output. The game outputs sound through FMOD (see
// fmod_audio.c); fail here so SDL falls back to its dummy driver.
jint SDLActivity_audioInit(jmethodID id, va_list args) {
    int freq = va_arg(args, int);
    l_warn("SDLActivity.audioInit(%d Hz): not supported, using FMOD only", freq);
    return -1;
}

// SDLActivity.audioWriteShortBuffer ([S)V / audioWriteByteBuffer ([B)V / audioQuit ()V
void SDLActivity_audioNop(jmethodID id, va_list args) {}

// SDLActivity.setActivityTitle (Ljava/lang/String;)Z
jboolean SDLActivity_setActivityTitle(jmethodID id, va_list args) {
    return JNI_TRUE;
}

// SDLActivity.sendMessage (II)Z
jboolean SDLActivity_sendMessage(jmethodID id, va_list args) {
    int command = va_arg(args, int);
    int param = va_arg(args, int);
    l_debug("SDLActivity.sendMessage(%d, %d)", command, param);
    return JNI_TRUE;
}

// SDLActivity.showTextInput (IIII)Z
jboolean SDLActivity_showTextInput(jmethodID id, va_list args) {
    return JNI_FALSE;
}

// Context.getFilesDir ()Ljava/io/File;
// Context.getExternalFilesDir (Ljava/lang/String;)Ljava/io/File;
jobject Context_getFilesDir(jmethodID id, va_list args) {
    return FAKE_OBJECT;
}

// File.getAbsolutePath ()Ljava/lang/String;
// Only ever called on the File objects returned above. No trailing slash,
// like Android's getFilesDir().
jobject File_getAbsolutePath(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "ux0:data/costumequest");
}

// Environment.getExternalStorageState ()Ljava/lang/String;
jobject Environment_getExternalStorageState(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "mounted");
}

// CqActivity.isPurchased ()Z
// Trial check of the OUYA store; the Java side returns true off-OUYA.
jboolean CqActivity_isPurchased(jmethodID id, va_list args) {
    return JNI_TRUE;
}

// CqActivity.startTrialUpgrade ()V
void CqActivity_startTrialUpgrade(jmethodID id, va_list args) {}

// Activity.finish ()V
void Activity_finish(jmethodID id, va_list args) {
    l_info("Activity.finish(): exiting");
    exit(0);
}

NameToMethodID nameToMethodId[] = {
    { 100, "getNativeSurface", METHOD_TYPE_OBJECT },
    { 101, "getContext", METHOD_TYPE_OBJECT },
    { 102, "flipBuffers", METHOD_TYPE_VOID },
    { 103, "audioInit", METHOD_TYPE_INT },
    { 104, "audioWriteShortBuffer", METHOD_TYPE_VOID },
    { 105, "audioWriteByteBuffer", METHOD_TYPE_VOID },
    { 106, "audioQuit", METHOD_TYPE_VOID },
    { 107, "setActivityTitle", METHOD_TYPE_BOOLEAN },
    { 108, "sendMessage", METHOD_TYPE_BOOLEAN },
    { 109, "showTextInput", METHOD_TYPE_BOOLEAN },
    { 110, "getFilesDir", METHOD_TYPE_OBJECT },
    { 111, "getExternalFilesDir", METHOD_TYPE_OBJECT },
    { 112, "getAbsolutePath", METHOD_TYPE_OBJECT },
    { 113, "getExternalStorageState", METHOD_TYPE_OBJECT },
    { 114, "isPurchased", METHOD_TYPE_BOOLEAN },
    { 115, "startTrialUpgrade", METHOD_TYPE_VOID },
    { 116, "finish", METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {
    { 107, SDLActivity_setActivityTitle },
    { 108, SDLActivity_sendMessage },
    { 109, SDLActivity_showTextInput },
    { 114, CqActivity_isPurchased },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { 103, SDLActivity_audioInit },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { 100, SDLActivity_getNativeSurface },
    { 101, SDLActivity_getContext },
    { 110, Context_getFilesDir },
    { 111, Context_getFilesDir },
    { 112, File_getAbsolutePath },
    { 113, Environment_getExternalStorageState },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 102, SDLActivity_flipBuffers },
    { 104, SDLActivity_audioNop },
    { 105, SDLActivity_audioNop },
    { 106, SDLActivity_audioNop },
    { 115, CqActivity_startTrialUpgrade },
    { 116, Activity_finish },
};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
