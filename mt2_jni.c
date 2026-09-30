

#include <jni.h>
#include <string.h>
#include <stdlib.h>


#include <unordered_map>
#define main mt2_main_internal
#include "mt2_v3.cpp"
#undef main


JNIEXPORT jstring JNICALL
Java_com_mt2_Mt2_version(JNIEnv *env, jclass cls) {
    return (*env)->NewStringUTF(env, "mt2 v3.0 (JNI build)");
}

JNIEXPORT jboolean JNICALL
Java_com_mt2_Mt2_hasZipSupport(JNIEnv *env, jclass cls) {
#ifdef HAS_LIBZIP
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jint JNICALL
Java_com_mt2_Mt2_exec(JNIEnv *env, jclass cls, jstring jcmd, jobjectArray jargs) {
    const char* cmd = (*env)->GetStringUTFChars(env, jcmd, NULL);
    if (!cmd) return 1;

    jsize argc = (*env)->GetArrayLength(env, jargs) + 1;


    const char** argv = (const char**)malloc(sizeof(char*) * argc);
    if (!argv) {
        (*env)->ReleaseStringUTFChars(env, jcmd, cmd);
        return 1;
    }

    argv[0] = cmd;
    jstring* jstrs = (jstring*)malloc(sizeof(jstring) * (argc - 1));
    if (!jstrs) {
        free(argv);
        (*env)->ReleaseStringUTFChars(env, jcmd, cmd);
        return 1;
    }

    for (jsize i = 0; i < argc - 1; i++) {
        jstrs[i] = (jstring)(*env)->GetObjectArrayElement(env, jargs, i);
        argv[i + 1] = (*env)->GetStringUTFChars(env, jstrs[i], NULL);
    }


    int ret = mt2_exec(argc, argv);


    for (jsize i = 0; i < argc - 1; i++) {
        (*env)->ReleaseStringUTFChars(env, jstrs[i], argv[i + 1]);
        (*env)->DeleteLocalRef(env, jstrs[i]);
    }
    free(jstrs);
    free(argv);
    (*env)->ReleaseStringUTFChars(env, jcmd, cmd);

    return ret;
}

JNIEXPORT jint JNICALL
Java_com_mt2_Mt2_execStr(JNIEnv *env, jclass cls, jstring jcmdStr) {
    const char* cmd_str = (*env)->GetStringUTFChars(env, jcmdStr, NULL);
    if (!cmd_str) return 1;

    int ret = mt2_exec_str(cmd_str);

    (*env)->ReleaseStringUTFChars(env, jcmdStr, cmd_str);
    return ret;
}

JNIEXPORT void JNICALL
Java_com_mt2_Mt2_setLanguage(JNIEnv *env, jclass cls, jint lang) {
    mt2_set_language(lang);
}

JNIEXPORT jstring JNICALL
Java_com_mt2_Mt2_help(JNIEnv *env, jclass cls) {
    const char* h = mt2_help();
    return (*env)->NewStringUTF(env, h);
}


__attribute__((constructor))
static void mt2_jni_init() {
    install_signal_handlers();
}
