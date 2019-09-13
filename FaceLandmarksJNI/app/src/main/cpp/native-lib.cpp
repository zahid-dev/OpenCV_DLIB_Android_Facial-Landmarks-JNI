
#include <jni.h>



#include <android/log.h>

#define TAG "faceDetAlignLib"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,TAG ,__VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,TAG ,__VA_ARGS__)

#include <string>
#include <cstdint>
#include <opencv2/opencv.hpp>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing.h>
#include <opencv2/objdetect.hpp>
#include <dlib/opencv.h>


using namespace std;
using namespace cv;

class FaceDetector {
public:
    FaceDetector(std::string cascadePath, int alignedFacesCacheSize);
    ~FaceDetector();
    void loadShapePredictor(std::string spPath);
    void clearCache();

    void fromDroidCamToCV(cv::Mat &m, int front1orback0, int orientCase);
    void detectFaces(cv::Mat &BGRMat);
    void alignFaces();
    void detectRaw(int width, int height, unsigned char* frmCData, int front1orback0, int orientCase, bool doalign = false);

    std::vector<cv::Rect> getFacesPos();
    std::vector<cv::Rect> getLandmarksPos();
    std::vector<cv::Mat>* getAlignedFacesCacheAddr();

private:
    cv::CascadeClassifier facecascade;
    cv::Mat kernel;
    cv::Mat BGRMat, GRAYMat, bbHSV, skinMask, BGRMatToAlign, faceAligned;
    std::vector<cv::Rect> bbs, bbsFiltered, bbsLandmarks;
    std::vector<dlib::rectangle> dlibROIs;
    dlib::frontal_face_detector dlibDetector;
    dlib::shape_predictor dlibLandmarker;
    std::vector<cv::Mat> alignedFacesCache;
    int alignedFacesCacheSize;
};
FaceDetector::FaceDetector(string cascadePath, int cacheSize) {
    bool suc = facecascade.load(cascadePath);
    if (suc) {
        LOGD("Cascade File loaded.");
    } else {
        LOGD("failed to load Cascade File");
    }
    dlibDetector = dlib::get_frontal_face_detector();
    kernel = getStructuringElement(MORPH_ELLIPSE, Size(3,3));
    alignedFacesCache.clear();
    alignedFacesCacheSize = cacheSize;
}


void FaceDetector::fromDroidCamToCV(Mat &m, int front1orback0, int orientCase) {
    // from android camera dataframe coordinate system to natural/viewport coordinate system.
    // this is the coordinate system in which opencv will see what we see.
    switch (orientCase) {
        case 0:
            transpose(m, m);
            flip(m, m, 1-front1orback0);
            break;
        case 1:
            flip(m, m, -1);
            break;
        case 2:
            transpose(m, m);
            flip(m, m, front1orback0);
            break;
        default:
            break;
    }

}

void FaceDetector::detectFaces(Mat &BGRMat) {
    BGRMatToAlign = BGRMat;
    Mat BGRMatBak = BGRMat;
    int width = BGRMat.cols;
    int height = BGRMat.rows;
    float scale = max(width, height) / 480.;
    resize(BGRMat, BGRMat, Size(::round(BGRMat.cols / scale), ::round(BGRMat.rows / scale)), INTER_AREA);

    // LOGD("after transformation: %d x %d", BGRMat.cols, BGRMat.rows);

    // face detection
    cvtColor(BGRMat, GRAYMat, CV_BGR2GRAY);
    equalizeHist(GRAYMat, GRAYMat);
    facecascade.detectMultiScale(GRAYMat, bbs, 1.1, 3, CV_HAAR_SCALE_IMAGE, cvSize(40, 40));
    LOGD("1stStage: Detect %d faces", (int) bbs.size());

    // filters: skin + dlib
    bbsFiltered.clear();
    for(vector<Rect>::const_iterator r = bbs.begin(); r != bbs.end(); r++) {
        cvtColor(BGRMat(*r), bbHSV, CV_BGR2HSV);
        inRange(bbHSV, Scalar(0, 48, 60), Scalar(30, 255, 255), skinMask);
        dilate(skinMask, skinMask, kernel, Point(-1, -1), 2);
        skinMask = skinMask(Rect(r->width/4, r->height/5, r->width/2, r->height*4/5));
        if (sum(skinMask)[0] / (skinMask.rows * skinMask.cols * 255.0) <= 0.2)
            continue;
        // pass skin filter

        bbsFiltered.push_back(Rect(r->x * scale, r->y * scale, r->width * scale, r->height * scale));
    }
    LOGD("2ndStage: Detect %d faces", (int) bbsFiltered.size());

    scale = max(width, height) / 960.;
    resize(BGRMatToAlign, BGRMatToAlign, Size(round(BGRMatToAlign.cols / scale), round(BGRMatToAlign.rows / scale)), INTER_AREA);

    int maxx = BGRMatToAlign.cols;
    int maxy = BGRMatToAlign.rows;

    dlibROIs.clear();
    bbsLandmarks.clear();

    for(vector<Rect>::iterator r = bbsFiltered.begin(); r != bbsFiltered.end(); ) {
        int l = r->x * 1.0 / scale;
        int t = r->y * 1.0 / scale;
        int w = r->width * 1.0 / scale;
        int h = r->height * 1.0 / scale;
        Rect patch = Rect(Point(max(l - 15, 0), max(t - 15, 0)), Point(min(l + w + 15, maxx), min(t + h + 15, maxy)));
        // LOGD("%d X %d, (%d, %d, %d, %d)", maxx, maxy, patch.x, patch.y, patch.width, patch.height);
        dlib::cv_image<dlib::bgr_pixel> dlibpatch(BGRMatToAlign(patch));
        vector<dlib::rectangle> dfaces = dlibDetector(dlibpatch,1);


            dlib::rectangle roi((patch.x + dfaces[0].left()) * scale, (patch.y + dfaces[0].top()) * scale, (patch.x + dfaces[0].right()) * scale, (patch.y + dfaces[0].bottom()) * scale);
            dlibROIs.push_back(roi);
            r++;

    }

    LOGD("3rdStage: Detect %d faces", (int) bbsFiltered.size());
    BGRMatToAlign = BGRMatBak;
}

void FaceDetector::alignFaces() {
    dlib::cv_image<dlib::bgr_pixel> dlibimg(BGRMatToAlign);

    // fill aligned face cache for future feature extraction

    Point2f anchorPts[3];
    anchorPts[0] = Point2f(29.08151817, 40.56148148);
    anchorPts[1] = Point2f(90.19503021, 39.43851852);
    anchorPts[2] = Point2f((40.68895721 + 80.59230804) / 2, (88.38999939 + 87.61001587) / 2);
    Point2f pts[3];
    Mat warp_H(2, 3, CV_32FC1);

    int landMarksIdx[15] = {36, 39, 42, 45, 31, 33, 35, 48, 54, 50, 51, 52, 55, 57, 59};

    if ((int) alignedFacesCache.size() >= alignedFacesCacheSize)
        alignedFacesCache.clear();

    for (vector<dlib::rectangle>::iterator r = dlibROIs.begin(); r != dlibROIs.end(); r++) {
        // face alignment
        dlib::full_object_detection shape = dlibLandmarker(dlibimg, *r);
        // LOGD("%d, %d, %d, %d", (int) r->left(), (int) r->top(), (int) r->right(), (int) r->bottom());
        pts[0].x = shape.part(36).x(), pts[0].y = shape.part(36).y();
        pts[1].x = shape.part(45).x(), pts[1].y = shape.part(45).y();
        pts[2].x = (shape.part(48).x() + shape.part(54).x()) / 2., pts[2].y = (shape.part(48).y() + shape.part(54).y()) / 2.;
        warp_H = getAffineTransform(pts, anchorPts);
        warpAffine(BGRMatToAlign, faceAligned, warp_H, Size(128, 128));
        // LOGD("Aligned face size: %d x %d", faceAligned.cols, faceAligned.rows);
        cvtColor(faceAligned, faceAligned, CV_BGR2GRAY);
        equalizeHist(faceAligned, faceAligned);
        faceAligned.convertTo(faceAligned, CV_32FC1);
        alignedFacesCache.push_back(faceAligned / 255.0);

        for (int i=0; i<15; i++)
            bbsLandmarks.push_back(Rect(shape.part(landMarksIdx[i]).x(), shape.part(landMarksIdx[i]).y(), 1, 1));

    }

    LOGD("aligned faces cache size: %d", (int) alignedFacesCache.size());
}

void FaceDetector::detectRaw(int width, int height, unsigned char *frmCData, int front1orback0, int orientCase, bool doalign) {
    Mat YUVMat(height + height / 2, width, CV_8UC1, frmCData);
    cvtColor(YUVMat, BGRMat, CV_YUV420sp2BGR);
    fromDroidCamToCV(BGRMat, front1orback0, orientCase);
    detectFaces(BGRMat);
    if (doalign)
        alignFaces();
}

void FaceDetector::loadShapePredictor(string spPath) {
    LOGD("Start loading face landmarks.");
    dlib::deserialize(spPath) >> dlibLandmarker;
    LOGD("Face landmarks loaded.");
}

std::vector<cv::Rect> FaceDetector::getFacesPos() {
    return bbsFiltered;
}

std::vector<cv::Rect> FaceDetector::getLandmarksPos() {
    return bbsLandmarks;
}

vector<Mat>* FaceDetector::getAlignedFacesCacheAddr() {
    return &alignedFacesCache;
}

void FaceDetector::clearCache() {
    alignedFacesCache.clear();
}


extern "C" {


    JNIEXPORT jlong JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_create(JNIEnv* env, jclass, jstring cascadeFile, jint alignedFacesCacheSize) {
        // LOGD("native create() called.");
        jlong detector = 0;
        const char* cascadeFilePath = env->GetStringUTFChars(cascadeFile, NULL);
        detector = (jlong)new FaceDetector(std::string(cascadeFilePath), (int) alignedFacesCacheSize);
        env->ReleaseStringUTFChars(cascadeFile, cascadeFilePath);
        return detector;
    }

    JNIEXPORT void JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_loadShapePredictor(JNIEnv* env, jclass, jlong thiz, jstring landmarksFilePath) {
        const char* spPath = env->GetStringUTFChars(landmarksFilePath, NULL);
        ((FaceDetector*)thiz)->loadShapePredictor(spPath);
        env->ReleaseStringUTFChars(landmarksFilePath, spPath);
    }

    JNIEXPORT jbyteArray JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_droidJPEGCalibrate(JNIEnv* env, jclass, jlong thiz, jbyteArray jpegdata, jint front1orback0, jint orientCase) {
        jbyte* picjData = env->GetByteArrayElements(jpegdata, 0);
        uchar* buf = (uchar*) picjData;
        size_t len = env->GetArrayLength(jpegdata);
        std::vector<uchar> cdata(buf, buf+len);
        cv::Mat m = cv::imdecode(cdata, CV_LOAD_IMAGE_COLOR);

        // do calibration: rotate + flip
        ((FaceDetector*)thiz)->fromDroidCamToCV(m, front1orback0, orientCase);

        LOGD("picture size after calibrated: %d X %d", m.rows, m.cols);
        std::vector<int> params;
        params.push_back(100);
        params.push_back(100);
        std::vector<uchar> cdataEnc;
       cv::imencode(".jpg", m, cdataEnc, params);
        jbyteArray jpegCalibrated = env->NewByteArray(cdataEnc.size());
        env->SetByteArrayRegion(jpegCalibrated, 0, cdataEnc.size(), (jbyte*)&cdataEnc[0]);

        env->ReleaseByteArrayElements(jpegdata, picjData, JNI_ABORT);
        return jpegCalibrated;
    }

    JNIEXPORT void JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_detectFromJPEG(JNIEnv* env, jclass, jlong thiz, jbyteArray jpegdata, jboolean doalign) {
        jbyte* picjData = env->GetByteArrayElements(jpegdata, 0);
        uchar* buf = (uchar*) picjData;
        size_t len = env->GetArrayLength(jpegdata);
        std::vector<uchar> cdata(buf, buf+len);
        cv::Mat m = cv::imdecode(cdata, CV_LOAD_IMAGE_COLOR);

        ((FaceDetector*)thiz)->detectFaces(m);
        if (doalign)
            ((FaceDetector*)thiz)->alignFaces();

        env->ReleaseByteArrayElements(jpegdata, picjData, JNI_ABORT);
    }

    JNIEXPORT void JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_detectFromRaw(JNIEnv* env, jclass, jlong thiz, jint width, jint height, jbyteArray frmdata, jint front1orback0, jint orientCase, jboolean doalign) {
        // LOGD("native detect() called.");
        jbyte* frmjData = env->GetByteArrayElements(frmdata, 0);
        ((FaceDetector*)thiz)->detectRaw((int) width, (int) height, (unsigned char*) frmjData, (int) front1orback0, (int) orientCase, (bool) doalign);
        env->ReleaseByteArrayElements(frmdata, frmjData, JNI_ABORT);
    }

    JNIEXPORT jintArray JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_getPos(JNIEnv* env, jclass, jlong thiz, jint face0landmarks1, jint cv0canvas1, jint width, jint height, jint front1orback0, jint orientCase) {
        std::vector<cv::Rect> pos;
        if (!face0landmarks1)
            pos = ((FaceDetector*)thiz)->getFacesPos();
        else
            pos = ((FaceDetector*)thiz)->getLandmarksPos();

        jintArray posArr = env->NewIntArray(pos.size() * 4);
        jint posBuf[4];
        int p = 0;
        for(std::vector<cv::Rect>::const_iterator r = pos.begin(); r != pos.end(); r++) {
            if (!cv0canvas1) {
                posBuf[0] = r->x;
                posBuf[1] = r->y;
                posBuf[2] = r->x + r->width;
                posBuf[3] = r->y + r->height;
            } else {
                switch (orientCase) {
                    case 0:
                        posBuf[0] = front1orback0 ? height - r->x - r->width : r->x;
                        posBuf[1] = r->y;
                        posBuf[2] = posBuf[0] + r->width;
                        posBuf[3] = posBuf[1] + r->height;
                        break;

                    case 1:
                        posBuf[0] = r->y;
                        posBuf[1] = front1orback0 ? r->x : width - r->x - r->width;
                        posBuf[2] = posBuf[0] + r->height;
                        posBuf[3] = posBuf[1] + r->width;
                        break;

                    case 2:
                        posBuf[0] = front1orback0 ? r->x : height - r->x - r->width;
                        posBuf[1] = width - r->y - r->height;
                        posBuf[2] = posBuf[0] + r->width;
                        posBuf[3] = posBuf[1] + r->height;
                        break;

                    case 3:
                        posBuf[0] = height - r->y - r->height;
                        posBuf[1] = front1orback0 ? width - r->x - r->width : r->x;
                        posBuf[2] = posBuf[0] + r->height;
                        posBuf[3] = posBuf[1] + r->width;
                        break;

                    default:
                        LOGD("Wrong orientCase value, should be {0, 1, 2, 3}");
                        break;
                }
            }

            env->SetIntArrayRegion(posArr, p, 4, posBuf);
            p += 4;
        }

        return posArr;
    }


    JNIEXPORT jlong JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_getAlignedFacesAddr(JNIEnv* env, jclass, jlong thiz) {
        return (jlong) ((FaceDetector*)thiz)->getAlignedFacesCacheAddr();
    }

    JNIEXPORT void JNICALL Java_com_example_zahid_facelandmarksjni_FaceDetAlign_clearCache(JNIEnv* env, jclass, jlong thiz) {
        ((FaceDetector*)thiz)->clearCache();
    }



}


