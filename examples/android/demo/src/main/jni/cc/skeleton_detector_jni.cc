//
// Created by tencent on 2020-12-11.
//
#include "skeleton_detector_jni.h"
#include "skeleton_detector.h"
#include "kannarotate.h"
#include "yuv420sp_to_rgb_fast_asm.h"
#include <jni.h>
#include "helper_jni.h"
#include <android/bitmap.h>
#include "tnn/utils/mat_utils.h"

static std::shared_ptr<TNN_NS::SkeletonDetector> gDetector;
static int gComputeUnitType = 0; // 0 is cpu, 1 is gpu, 2 is huawei_npu
static jclass clsObjectInfo;
static jmethodID midconstructorObjectInfo;
static jfieldID fidx1;
static jfieldID fidy1;
static jfieldID fidx2;
static jfieldID fidy2;
static jfieldID fidscore;
static jfieldID fidcls;
static jfieldID fidkeypoints;
static jfieldID fidlines;
// Jni functions

JNIEXPORT JNICALL jint TNN_SKELETON_DETECTOR(init)(JNIEnv *env, jobject thiz, jstring modelPath, jint width, jint height, jint computUnitType)
{
    // Reset bench description
    setBenchResult("");
    std::vector<int> nchw = {1, 3, height, width};
    gDetector = std::make_shared<TNN_NS::SkeletonDetector>();
    std::string protoContent, modelContent;
    std::string modelPathStr(jstring2string(env, modelPath));
    protoContent = fdLoadFile(modelPathStr + "/skeleton.tnnproto");
    modelContent = fdLoadFile(modelPathStr + "/skeleton.tnnmodel");
    LOGI("proto content size %d model content size %d", protoContent.length(), modelContent.length());
    gComputeUnitType = computUnitType;

    TNN_NS::Status status = TNN_NS::TNN_OK;
    auto option = std::make_shared<TNN_NS::SkeletonDetectorOption>();
    option->compute_units = TNN_NS::TNNComputeUnitsCPU;
    option->library_path  = "";
    option->proto_content = protoContent;
    option->model_content = modelContent;
    option->min_threshold = 0.15f;
    LOGI("device type: %d", gComputeUnitType);
    if (gComputeUnitType == 1) {
        option->compute_units = TNN_NS::TNNComputeUnitsGPU;
        status = gDetector->Init(option);
    } else if (gComputeUnitType == 2) {
        //add for huawei_npu store the om file
        option->compute_units = TNN_NS::TNNComputeUnitsHuaweiNPU;
        gDetector->setNpuModelPath(modelPathStr + "/");
        gDetector->setCheckNpuSwitch(false);
        status = gDetector->Init(option);
    } else {
	    option->compute_units = TNN_NS::TNNComputeUnitsCPU;
    	status = gDetector->Init(option);
    }

    if (status != TNN_NS::TNN_OK) {
        LOGE("detector init failed %d", (int)status);
        return -1;
    }

    if (clsObjectInfo == NULL) {
        clsObjectInfo = static_cast<jclass>(env->NewGlobalRef(env->FindClass("com/tencent/tnn/demo/ObjectInfo")));
        midconstructorObjectInfo = env->GetMethodID(clsObjectInfo, "<init>", "()V");
        fidx1 = env->GetFieldID(clsObjectInfo, "x1" , "F");
        fidy1 = env->GetFieldID(clsObjectInfo, "y1" , "F");
        fidx2 = env->GetFieldID(clsObjectInfo, "x2" , "F");
        fidy2 = env->GetFieldID(clsObjectInfo, "y2" , "F");
        fidscore = env->GetFieldID(clsObjectInfo, "score" , "F");
        fidcls = env->GetFieldID(clsObjectInfo, "class_id", "I");
        fidkeypoints = env->GetFieldID(clsObjectInfo, "key_points", "[[F");
        fidlines = env->GetFieldID(clsObjectInfo, "lines", "[[I");
    }

    return 0;
}

JNIEXPORT JNICALL jboolean TNN_SKELETON_DETECTOR(checkNpu)(JNIEnv *env, jobject thiz, jstring modelPath) {
    TNN_NS::SkeletonDetector tmpDetector;
    std::string protoContent, modelContent;
    std::string modelPathStr(jstring2string(env, modelPath));
    protoContent = fdLoadFile(modelPathStr + "/skeleton.tnnproto");
    modelContent = fdLoadFile(modelPathStr + "/skeleton.tnnmodel");
    auto option = std::make_shared<TNN_NS::SkeletonDetectorOption>();
    option->compute_units = TNN_NS::TNNComputeUnitsHuaweiNPU;
    option->library_path = "";
    option->proto_content = protoContent;
    option->model_content = modelContent;
    option->min_threshold = 0.15f;
    tmpDetector.setNpuModelPath(modelPathStr + "/");
    tmpDetector.setCheckNpuSwitch(true);
    TNN_NS::Status ret = tmpDetector.Init(option);
    return ret == TNN_NS::TNN_OK;
}

JNIEXPORT JNICALL jint TNN_SKELETON_DETECTOR(deinit)(JNIEnv *env, jobject thiz)
{
    gDetector = nullptr;
    return 0;
}

JNIEXPORT JNICALL jobjectArray TNN_SKELETON_DETECTOR(detectFromStream)(JNIEnv *env, jobject thiz, jbyteArray yuv420sp, jint width, jint height, jint view_width, jint view_height, jint rotate)
{
    jobjectArray objectInfoArray;
    auto asyncRefDetector = gDetector;
    TNN_NS::SkeletonInfo objectInfo;
    // Convert yuv to rgb
    LOGI("detect from stream %d x %d r %d", width, height, rotate);
    unsigned char *yuvData = new unsigned char[height * width * 3 / 2];
    jbyte *yuvDataRef = env->GetByteArrayElements(yuv420sp, 0);
    int ret = kannarotate_yuv420sp((const unsigned char*)yuvDataRef, (int)width, (int)height, (unsigned char*)yuvData, (int)rotate);
    env->ReleaseByteArrayElements(yuv420sp, yuvDataRef, 0);
    unsigned char *rgbaData = new unsigned char[height * width * 4];
    yuv420sp_to_rgba_fast_asm((const unsigned char*)yuvData, height, width, (unsigned char*)rgbaData);
    TNN_NS::DeviceType dt = TNN_NS::DEVICE_ARM;

    TNN_NS::DimsVector input_dims = {1, 4, width, height};

    auto input_mat = std::make_shared<TNN_NS::Mat>(dt, TNN_NS::N8UC4, input_dims, rgbaData);

    std::shared_ptr<TNN_NS::TNNSDKInput> input = std::make_shared<TNN_NS::TNNSDKInput>(input_mat);
    std::shared_ptr<TNN_NS::TNNSDKOutput> output = std::make_shared<TNN_NS::TNNSDKOutput>();

    TNN_NS::Status status = asyncRefDetector->Predict(input, output);

    objectInfo = dynamic_cast<TNN_NS::SkeletonDetectorOutput *>(output.get())->keypoints;
    delete [] yuvData;
    delete [] rgbaData;
    if (status != TNN_NS::TNN_OK) {
        LOGE("failed to detect %d", (int)status);
        return 0;
    }

    objectInfoArray = env->NewObjectArray(1, clsObjectInfo, NULL);
    jobject objObjectInfo = env->NewObject(clsObjectInfo, midconstructorObjectInfo);
    int keypointsNum = objectInfo.key_points.size();
    int linesNum = objectInfo.lines.size();
    LOGI("object %f %f %f %f score %f key points size %d, label_id: %d, line num: %d",
            objectInfo.x1, objectInfo.y1, objectInfo.x2,
            objectInfo.y2, objectInfo.score, keypointsNum, objectInfo.class_id, linesNum);
    auto object_orig = objectInfo.AdjustToViewSize(view_height, view_width, 2);
    //from here start to create point
    jclass cls1dArr = env->FindClass("[F");
    // Create the returnable jobjectArray with an initial value
    jobjectArray outer = env->NewObjectArray(keypointsNum, cls1dArr, NULL);
    for (int j = 0; j < keypointsNum; j++) {
        jfloatArray inner = env->NewFloatArray(2);
        float temp[] = {object_orig.key_points[j].first, object_orig.key_points[j].second};
        env->SetFloatArrayRegion(inner, 0, 2, temp);
        env->SetObjectArrayElement(outer, j, inner);
        env->DeleteLocalRef(inner);
    }

    //from here start to create line
    jclass line1dArr = env->FindClass("[I");
    // Create the returnable jobjectArray with an initial value
    jobjectArray lineOuter = env->NewObjectArray(linesNum, line1dArr, NULL);
    for (int j = 0; j < linesNum; j++) {
        jintArray lineInner = env->NewIntArray(2);
        int temp[] = {object_orig.lines[j].first, object_orig.lines[j].second};
        env->SetIntArrayRegion(lineInner, 0, 2, temp);
        env->SetObjectArrayElement(lineOuter, j, lineInner);
        env->DeleteLocalRef(lineInner);
    }
    env->SetFloatField(objObjectInfo, fidx1, object_orig.x1);
    env->SetFloatField(objObjectInfo, fidy1, object_orig.y1);
    env->SetFloatField(objObjectInfo, fidx2, object_orig.x2);
    env->SetFloatField(objObjectInfo, fidy2, object_orig.y2);
    env->SetFloatField(objObjectInfo, fidscore, object_orig.score);
    env->SetIntField(objObjectInfo, fidcls, object_orig.class_id);
    env->SetObjectField(objObjectInfo, fidkeypoints, outer);
    env->SetObjectField(objObjectInfo, fidlines, lineOuter);
    env->SetObjectArrayElement(objectInfoArray, 0, objObjectInfo);
    env->DeleteLocalRef(objObjectInfo);

    return objectInfoArray;

}
