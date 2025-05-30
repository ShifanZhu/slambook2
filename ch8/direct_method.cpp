#include <opencv2/opencv.hpp>
#include <sophus/se3.hpp>
#include <boost/format.hpp>
#include <pangolin/pangolin.h>

using namespace std;

typedef vector<Eigen::Vector2d, Eigen::aligned_allocator<Eigen::Vector2d>> VecVector2d;

// Camera intrinsics
// double fx = 718.856, fy = 718.856, cx = 607.1928, cy = 185.2157;
// double fx = 278.708, fy = 278.657, cx = 169.431, cy = 124.534; // event camera
double fx = 637.27803366, fy = 637.30526147, cx = 636.3285782, cy = 377.00039794; // rgbd camera
int cols, rows;
// baseline
double baseline = 0.573;
int huber_threshold = 8;
// paths
string rgbd_dataset_path_ = "/home/zh/data/img";
// string left_file = "../left.png";
string left_file = "../event6.png";
// string disparity_file = "../disparity.png";
string disparity_file = "../depth6.png";
boost::format fmt_file("%s/image_%d/%06d.png"); // 10 us
// boost::format fmt_others("../%06d.png");    // other files

// useful typedefs
typedef Eigen::Matrix<double, 6, 6> Matrix6d;
typedef Eigen::Matrix<double, 2, 6> Matrix26d;
typedef Eigen::Matrix<double, 6, 1> Vector6d;

/// class for accumulator jacobians in parallel
class JacobianAccumulator {
public:
    JacobianAccumulator(
        const cv::Mat &img1_,
        const cv::Mat &img2_,
        const VecVector2d &px_ref_,
        const vector<double> depth_ref_,
        vector<bool> &outlier_,
        Sophus::SE3d &T21_) :
        img1(img1_), img2(img2_), px_ref(px_ref_), depth_ref(depth_ref_), outlier(outlier_), T21(T21_) {
        projection = VecVector2d(px_ref.size(), Eigen::Vector2d(0, 0));
        projection_outlier = VecVector2d(px_ref.size(), Eigen::Vector2d(0, 0));
    }

    JacobianAccumulator(
        const cv::Mat &img1_,
        const cv::Mat &img2_,
        const VecVector2d &px_ref_,
        const vector<double> depth_ref_,
        vector<bool> &outlier_,
        vector<double> &outlier_cost_,
        Sophus::SE3d &T21_) :
        img1(img1_), img2(img2_), px_ref(px_ref_), depth_ref(depth_ref_), outlier(outlier_), outlier_cost(outlier_cost_), T21(T21_) {
        projection = VecVector2d(px_ref.size(), Eigen::Vector2d(0, 0));
        projection_outlier = VecVector2d(px_ref.size(), Eigen::Vector2d(0, 0));
    }

    /// accumulate jacobians in a range
    void accumulate_jacobian(const cv::Range &range);
    void compute_outlier(const cv::Range &range);

    /// get hessian matrix
    Matrix6d hessian() const { return H; }

    /// get bias
    Vector6d bias() const { return b; }

    /// get total cost
    double cost_func() const { return cost; }

    /// get projected points
    VecVector2d projected_points() const { return projection; }
    VecVector2d projected_outlier_points() const { return projection_outlier; }

    /// reset h, b, cost to zero
    void reset() {
        H = Matrix6d::Zero();
        b = Vector6d::Zero();
        cost = 0;
    }

private:
    const cv::Mat &img1;
    const cv::Mat &img2;
    const VecVector2d &px_ref;
    const vector<double> depth_ref;
    vector<bool> outlier;
    vector<double> outlier_cost;
    Sophus::SE3d &T21;
    VecVector2d projection; // projected points
    VecVector2d projection_outlier; // projected points

    std::mutex hessian_mutex;
    Matrix6d H = Matrix6d::Zero();
    Vector6d b = Vector6d::Zero();
    double cost = 0;
};

/**
 * pose estimation using direct method
 * @param img1
 * @param img2
 * @param px_ref
 * @param depth_ref
 * @param T21
 */
void DirectPoseEstimationMultiLayer(
    const cv::Mat &img1,
    const cv::Mat &img2,
    const VecVector2d &px_ref,
    const vector<double> depth_ref,
    Sophus::SE3d &T21
);

/**
 * pose estimation using direct method
 * @param img1
 * @param img2
 * @param px_ref
 * @param depth_ref
 * @param T21
 */
void DirectPoseEstimationSingleLayer(
    const cv::Mat &img1,
    const cv::Mat &img2,
    const VecVector2d &px_ref,
    const vector<double> depth_ref,
    Sophus::SE3d &T21
);

// bilinear interpolation
inline float GetPixelValue(const cv::Mat &img, float x, float y) {
    // boundary check
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= img.cols) x = img.cols - 1;
    if (y >= img.rows) y = img.rows - 1;
    uchar *data = &img.data[int(y) * img.step + int(x)];
    float xx = x - floor(x);
    float yy = y - floor(y);
    return float(
        (1 - xx) * (1 - yy) * data[0] +
        xx * (1 - yy) * data[1] +
        (1 - xx) * yy * data[img.step] +
        xx * yy * data[img.step + 1]
    );
}

// mode 0: random selection
// mode 1: 
void extractFeatures(cv::Mat& img, cv::Mat& disparity_img, VecVector2d& pixels_ref, vector<double>& depth_ref, int mode = 0) {
    pixels_ref.clear();
    depth_ref.clear();
    cout << "mode = " << mode << endl;
    switch (mode)
    {
        case 0: {
            cout << "mode 0" << endl;
            // let's randomly pick pixels in the first image and generate some 3d points in the first image's frame
            cv::RNG rng;
            int nPoints = 200;
            int boarder = 20;
            // generate pixels in ref and load depth data
            for (int i = 0; i < nPoints; i++) {
                int x = rng.uniform(boarder, img.cols - boarder);  // don't pick pixels close to boarder
                int y = rng.uniform(boarder, img.rows - boarder);  // don't pick pixels close to boarder
                int disparity = disparity_img.at<uchar>(y, x);
                // double depth = fx * baseline / disparity; // you know this is disparity to depth
                // double depth = float(disparity) / 30.0; // you know this is disparity to depth
                double depth = float(disparity) * 0.001; // you know this is disparity to depth
                if(depth < 0.1) continue;
                depth_ref.push_back(depth);
                pixels_ref.push_back(Eigen::Vector2d(x, y));
            }
            break;
        }
        case 1: {
            cout << "mode 1" << endl;
            // select the pixels with high gradiants 
            for ( int x=1; x<img.cols-1; x+=3 ) {
                for ( int y=1; y<img.rows-1; y+=3 ) {
                    Eigen::Vector2d delta (
                        img.ptr<uchar>(y)[x+1] - img.ptr<uchar>(y)[x-1], 
                        img.ptr<uchar>(y+1)[x] - img.ptr<uchar>(y-1)[x]
                    );

                    if ( delta.norm() < 50 )
                        continue;
                    ushort disparity = disparity_img.at<ushort>(y, x);
                    double depth = float(disparity) * 0.001;// you know this is disparity to depth
                    if(depth < 0.3 || depth > 8) continue;
                    depth_ref.push_back(depth);
                    pixels_ref.push_back(Eigen::Vector2d(x, y));
                }
            }
            cout << "extracted " << depth_ref.size() << " featuers" << endl;
            break;
        }
    }
    return;
}

int main(int argc, char **argv) {

    // cv::Mat left_img = cv::imread(left_file, 0);
    cv::Mat left_img = cv::imread(rgbd_dataset_path_+"/image_0/000699.png", 0);
    cols = left_img.cols;
    rows = left_img.rows;
    // cv::Mat disparity_img = cv::imread(disparity_file, 0);
    cv::Mat disparity_img = cv::imread(rgbd_dataset_path_+"/image_1/000699.png", cv::IMREAD_UNCHANGED);


    // let's randomly pick pixels in the first image and generate some 3d points in the first image's frame
    cv::RNG rng;
    int nPoints = 2000;
    int boarder = 20;
    VecVector2d pixels_ref;
    vector<double> depth_ref;

    // generate pixels in ref and load depth data
    for (int i = 0; i < nPoints; i++) {
        int x = rng.uniform(boarder, left_img.cols - boarder);  // don't pick pixels close to boarder
        int y = rng.uniform(boarder, left_img.rows - boarder);  // don't pick pixels close to boarder
        int disparity = disparity_img.at<uchar>(y, x);
        double depth = fx * baseline / disparity; // you know this is disparity to depth
        depth_ref.push_back(depth);
        pixels_ref.push_back(Eigen::Vector2d(x, y));
    }

    extractFeatures(left_img, disparity_img, pixels_ref, depth_ref, 1);

    // estimates 01~05.png's pose using this information
    Sophus::SE3d T_cur_ref;

    for (int i = 700; i < 3500; i+=1) {  // 1~10
    // for (int i = 1; i < 6; i++) {  // 1~10
        // cv::Mat img = cv::imread((fmt_others % i).str(), 0);
        // cv::Mat img = cv::imread("../event"+std::to_string(i)+".png", 0);
        cv::Mat img = cv::imread((fmt_file % rgbd_dataset_path_ % 0 % i).str(), 0);
        if (i % 3 == 0) {
            left_img = img.clone();
            disparity_img = cv::imread((fmt_file % rgbd_dataset_path_ % 1 % i).str(), cv::IMREAD_UNCHANGED);
            extractFeatures(left_img, disparity_img, pixels_ref, depth_ref, 1);
            continue;
        }
        // DirectPoseEstimationMultiLayer(left_img, img, pixels_ref, depth_ref, T_cur_ref); // CHECK GUESS
        // try single layer by uncomment this line
        DirectPoseEstimationSingleLayer(left_img, img, pixels_ref, depth_ref, T_cur_ref);
    }
    return 0;
}

void DirectPoseEstimationSingleLayer(
    const cv::Mat &img1,
    const cv::Mat &img2,
    const VecVector2d &px_ref,
    const vector<double> depth_ref,
    Sophus::SE3d &T21) {

    const int iterations = 4;
    double cost = 0, lastCost = 0;
    auto t1 = chrono::steady_clock::now();
    vector<bool> outlier(px_ref.size(), false);
    vector<double> outlier_cost(px_ref.size(), 0.0);
    JacobianAccumulator jaco_accu(img1, img2, px_ref, depth_ref, outlier, T21);
    JacobianAccumulator com_outlier(img1, img2, px_ref, depth_ref, outlier, outlier_cost, T21);

    for (int iter = 0; iter < iterations; iter++) {
        auto t1 = std::chrono::steady_clock::now();
        jaco_accu.reset();
        cv::parallel_for_(cv::Range(0, px_ref.size()),
                          std::bind(&JacobianAccumulator::accumulate_jacobian, &jaco_accu, std::placeholders::_1));
        auto t2 = std::chrono::steady_clock::now();
        auto time_used =
            std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
        std::cout<< "DirectMethod cost time: " << time_used.count() << " seconds" <<std::endl;
        
        Matrix6d H = jaco_accu.hessian();
        Vector6d b = jaco_accu.bias();

        // solve update and put it into estimation
        Vector6d update = H.ldlt().solve(b);
        Sophus::SE3d T_cw_tmp = Sophus::SE3d::exp(update) * T21;
        // T21 = Sophus::SE3d::exp(update) * T21;
        cost = jaco_accu.cost_func();

        if (std::isnan(update[0])) {
            // sometimes occurred when we have a black or white patch and H is irreversible
            cout << "update is nan" << endl;
            break;
        }
        if (iter > 0 && cost > lastCost) {
            cout << "cost increased: " << cost << ", " << lastCost << endl;
            break;
        }
        T21 = T_cw_tmp;
        cv::parallel_for_(cv::Range(0, px_ref.size()),
                          std::bind(&JacobianAccumulator::compute_outlier, &com_outlier, std::placeholders::_1));
        if (update.norm() < 1e-3) {
            // converge
            break;
        }

        lastCost = cost;
        cout << "iteration: " << iter << ", cost: " << cost << endl;
    }

    cout << "T21 = \n" << T21.matrix() << endl;
    auto t2 = chrono::steady_clock::now();
    auto time_used = chrono::duration_cast<chrono::duration<double>>(t2 - t1);
    cout << "direct method for single layer: " << time_used.count() << endl;
    if ( rand() > RAND_MAX/5 ) {cout << "val = " << (float(rand())/RAND_MAX*100-50) << endl;}

    // plot the projected pixels here
    cv::Mat img2_show;
    cv::cvtColor(img2, img2_show, cv::COLOR_GRAY2BGR);
    VecVector2d projection = com_outlier.projected_points();
    VecVector2d projection_outlier = com_outlier.projected_outlier_points();
    for (size_t i = 0; i < px_ref.size(); ++i) {
        auto p_ref = px_ref[i];
        auto p_cur = projection[i];
        auto p_cur_outlier = projection_outlier[i];
        if (p_cur[0] > 0 && p_cur[1] > 0) {
            cv::circle(img2_show, cv::Point2f(p_cur[0], p_cur[1]), 2, cv::Scalar(0, 250, 0), 1);
            // cv::line(img2_show, cv::Point2f(p_ref[0], p_ref[1]), cv::Point2f(p_cur[0], p_cur[1]),
            //          cv::Scalar(0, 250, 0));
        }
        if (p_cur_outlier[0] > 0 && p_cur_outlier[1] > 0) {
            cv::circle(img2_show, cv::Point2f(p_cur_outlier[0], p_cur_outlier[1]), 2, cv::Scalar(0, 0, 250), 1);
            // cv::line(img2_show, cv::Point2f(p_ref[0], p_ref[1]), cv::Point2f(p_cur[0], p_cur[1]),
            //          cv::Scalar(0, 250, 0));
        }
    }
    if (img1.rows == rows) {
        cv::imshow("current", img2_show);
        cv::waitKey();
    }
}

void JacobianAccumulator::accumulate_jacobian(const cv::Range &range) {

    // parameters
    const int half_patch_size = 1;
    int cnt_good = 0;
    int cnt_outlier = 0;
    Matrix6d hessian = Matrix6d::Zero();
    Vector6d bias = Vector6d::Zero();
    double cost_tmp = 0;
    double cost_outlier = 0;
    // std::cout << "cost_tmp = "<<cost_tmp<<std::endl;

    for (size_t i = range.start; i < range.end; i++) {

        if (outlier[i]) continue;
        // compute the projection in the second image
        Eigen::Vector3d point_ref =
            depth_ref[i] * Eigen::Vector3d((px_ref[i][0] - cx) / fx, (px_ref[i][1] - cy) / fy, 1);
        Eigen::Vector3d point_cur = T21 * point_ref;
        if (point_cur[2] < 0)   // depth invalid
            continue;

        float u = fx * point_cur[0] / point_cur[2] + cx, v = fy * point_cur[1] / point_cur[2] + cy;
        if (u < half_patch_size || u > img2.cols - half_patch_size || v < half_patch_size ||
            v > img2.rows - half_patch_size)
            continue;

        // projection[i] = Eigen::Vector2d(u, v);
        double X = point_cur[0], Y = point_cur[1], Z = point_cur[2],
            Z2 = Z * Z, Z_inv = 1.0 / Z, Z2_inv = Z_inv * Z_inv;
        cnt_good++;

        // and compute error and jacobian
        for (int x = -half_patch_size; x <= half_patch_size; x++)
            for (int y = -half_patch_size; y <= half_patch_size; y++) {
                // cout << "ixy = "<<i<<" "<<x<<" "<<y<<endl;

                double error = GetPixelValue(img1, px_ref[i][0] + x, px_ref[i][1] + y) -
                               GetPixelValue(img2, u + x, v + y);
                // cout << "error before = " << error << endl;
                // if ( rand() > RAND_MAX/5 ) {error = error + (float(rand())/RAND_MAX*100-50);}
                // error = error + (float(rand())/RAND_MAX*400-200);
                // outlier_cost[i] += error*error;
                // cnt_outlier++;
                float hw = fabs(error) < huber_threshold ? 1 : huber_threshold / fabs(error);
                // cout << "error = after = " << error << "  hw = " << hw << endl;

                Matrix26d J_pixel_xi;
                Eigen::Vector2d J_img_pixel;

                J_pixel_xi(0, 0) = fx * Z_inv;
                J_pixel_xi(0, 1) = 0;
                J_pixel_xi(0, 2) = -fx * X * Z2_inv;
                J_pixel_xi(0, 3) = -fx * X * Y * Z2_inv;
                J_pixel_xi(0, 4) = fx + fx * X * X * Z2_inv;
                J_pixel_xi(0, 5) = -fx * Y * Z_inv;

                J_pixel_xi(1, 0) = 0;
                J_pixel_xi(1, 1) = fy * Z_inv;
                J_pixel_xi(1, 2) = -fy * Y * Z2_inv;
                J_pixel_xi(1, 3) = -fy - fy * Y * Y * Z2_inv;
                J_pixel_xi(1, 4) = fy * X * Y * Z2_inv;
                J_pixel_xi(1, 5) = fy * X * Z_inv;

                J_img_pixel = Eigen::Vector2d(
                    0.5 * (GetPixelValue(img2, u + 1 + x, v + y) - GetPixelValue(img2, u - 1 + x, v + y)),
                    0.5 * (GetPixelValue(img2, u + x, v + 1 + y) - GetPixelValue(img2, u + x, v - 1 + y))
                );

                // total jacobian
                Vector6d J = -1.0 * (J_img_pixel.transpose() * J_pixel_xi).transpose();

                // hessian += J * J.transpose();
                // bias += -error * J;
                // cost_tmp += error * error;

                hessian += J * J.transpose() * hw * hw;
                bias += -error * J * hw * hw;
                cost_tmp += hw * error * error * hw;
                // cout << "error = " << error << endl;
            }
        // cost_outlier /= cnt_outlier;
        // outlier_cost[i] /= cnt_outlier;
        // if (cost_outlier > 100) {
        //     outlier[i] = true;
        // } else {
        //     outlier[i] = false;
        // }
    }

    if (cnt_good) {
        // set hessian, bias and cost
        // std::cout<<"cnt_good = "<<cnt_good<<std::endl;
        unique_lock<mutex> lck(hessian_mutex);
        H += hessian;
        b += bias;
        cost += cost_tmp / cnt_good;
    }
    // std::cout<<"Finish optimization (direct) "<<std::endl;
}

void JacobianAccumulator::compute_outlier(const cv::Range &range) {

    // parameters
    const int half_patch_size = 1;
    int cnt_good = 0;
    int cnt_outlier = 0;
    double cost_outlier = 0;
    // projection.clear();

    for (size_t i = range.start; i < range.end; i++) {

        // if (outlier[i]) continue;
        // compute the projection in the second image
        Eigen::Vector3d point_ref =
            depth_ref[i] * Eigen::Vector3d((px_ref[i][0] - cx) / fx, (px_ref[i][1] - cy) / fy, 1);
        Eigen::Vector3d point_cur = T21 * point_ref;
        if (point_cur[2] <= 0)   // depth invalid
            continue;

        float u = fx * point_cur[0] / point_cur[2] + cx, v = fy * point_cur[1] / point_cur[2] + cy;
        if (u < half_patch_size || u > img2.cols - half_patch_size || v < half_patch_size ||
            v > img2.rows - half_patch_size)
            continue;

        cnt_good++;
        cost_outlier = 0;

        // and compute error and jacobian
        for (int x = -half_patch_size; x <= half_patch_size; x++)
            for (int y = -half_patch_size; y <= half_patch_size; y++) {

                double error = GetPixelValue(img1, px_ref[i][0] + x, px_ref[i][1] + y) -
                               GetPixelValue(img2, u + x, v + y);
                // cout << "error before = " << error << endl;
                // if ( rand() > RAND_MAX/5 ) {error = error + (float(rand())/RAND_MAX*100-50);}
                // error = error + (float(rand())/RAND_MAX*400-200);
                // outlier_cost[i] += error*error;
                cost_outlier += error * error;
                cnt_outlier++;
            }
        cost_outlier /= cnt_outlier;
        // outlier_cost[i] /= cnt_outlier;
        if (cost_outlier > 300) {
            outlier[i] = true;
            projection[i] = Eigen::Vector2d(0, 0);
            projection_outlier[i] = Eigen::Vector2d(u, v);

        } else {
            outlier[i] = false;
            projection[i] = Eigen::Vector2d(u, v);
            projection_outlier[i] = Eigen::Vector2d(0, 0);
            // cout << " " << projection[i].transpose();
        }
    }
}

void DirectPoseEstimationMultiLayer(
    const cv::Mat &img1,
    const cv::Mat &img2,
    const VecVector2d &px_ref,
    const vector<double> depth_ref,
    Sophus::SE3d &T21) {

    // parameters
    int pyramids = 4;
    double pyramid_scale = 0.5;
    double scales[] = {1.0, 0.5, 0.25, 0.125};

    // create pyramids
    vector<cv::Mat> pyr1, pyr2; // image pyramids
    for (int i = 0; i < pyramids; i++) {
        if (i == 0) {
            pyr1.push_back(img1);
            pyr2.push_back(img2);
        } else {
            cv::Mat img1_pyr, img2_pyr;
            cv::resize(pyr1[i - 1], img1_pyr,
                       cv::Size(pyr1[i - 1].cols * pyramid_scale, pyr1[i - 1].rows * pyramid_scale));
            cv::resize(pyr2[i - 1], img2_pyr,
                       cv::Size(pyr2[i - 1].cols * pyramid_scale, pyr2[i - 1].rows * pyramid_scale));
            pyr1.push_back(img1_pyr);
            pyr2.push_back(img2_pyr);
        }
    }

    double fxG = fx, fyG = fy, cxG = cx, cyG = cy;  // backup the old values
    for (int level = pyramids - 1; level >= 0; level--) {
        VecVector2d px_ref_pyr; // set the keypoints in this pyramid level
        for (auto &px: px_ref) {
            px_ref_pyr.push_back(scales[level] * px);
        }

        // scale fx, fy, cx, cy in different pyramid levels
        fx = fxG * scales[level];
        fy = fyG * scales[level];
        cx = cxG * scales[level];
        cy = cyG * scales[level];
        DirectPoseEstimationSingleLayer(pyr1[level], pyr2[level], px_ref_pyr, depth_ref, T21);
    }

}
