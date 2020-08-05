#ifdef USE_PANGOLIN_VIEWER
#include <pangolin_viewer/viewer.h>
#elif USE_SOCKET_PUBLISHER
#include <socket_publisher/publisher.h>
#endif

#include <openvslam/system.h>
#include <openvslam/config.h>

#include <iostream>
#include <chrono>
#include <numeric>
#include <string>

#include <ros/ros.h>
#include <std_msgs/String.h>
#include <std_msgs/Bool.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2_ros/transform_broadcaster.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <popl.hpp>

#ifdef USE_STACK_TRACE_LOGGER
#include <glog/logging.h>
#endif

#ifdef USE_GOOGLE_PERFTOOLS
#include <gperftools/profiler.h>
#endif

void pose_odometry_pub(auto cam_pose_, auto pose_pub_, auto odometry_pub_){
    Eigen::Matrix3d rotation_matrix = cam_pose_.block(0, 0, 3, 3);
    Eigen::Vector3d translation_vector = cam_pose_.block(0, 3, 3, 1);

    tf2::Matrix3x3 tf_rotation_matrix(rotation_matrix(0, 0), rotation_matrix(0, 1), rotation_matrix(0, 2),
                                      rotation_matrix(1, 0), rotation_matrix(1, 1), rotation_matrix(1, 2),
                                      rotation_matrix(2, 0), rotation_matrix(2, 1), rotation_matrix(2, 2));

    tf2::Vector3 tf_translation_vector(translation_vector(0), translation_vector(1), translation_vector(2));

    tf_rotation_matrix = tf_rotation_matrix.inverse();
    tf_translation_vector = -(tf_rotation_matrix * tf_translation_vector);

    tf2::Transform transform_tf(tf_rotation_matrix, tf_translation_vector);

    tf2::Matrix3x3 rot_open_to_ros (0, 0, 1,
                                  -1, 0, 0,
                                   0,-1, 0);

    tf2::Transform transformA(rot_open_to_ros, tf2::Vector3(0.0, 0.0, 0.0));
    tf2::Transform transformB(rot_open_to_ros.inverse(), tf2::Vector3(0.0, 0.0, 0.0));

    transform_tf = transformA * transform_tf * transformB;

    ros::Time now = ros::Time::now();

    nav_msgs::Odometry odom_msg_;
    odom_msg_.header.stamp = now;
    odom_msg_.header.frame_id = "map";
    odom_msg_.child_frame_id = "base_link_frame";
    odom_msg_.pose.pose.orientation.x = transform_tf.getRotation().getX();
    odom_msg_.pose.pose.orientation.y = transform_tf.getRotation().getY();
    odom_msg_.pose.pose.orientation.z = transform_tf.getRotation().getZ();
    odom_msg_.pose.pose.orientation.w = transform_tf.getRotation().getW();
    odom_msg_.pose.pose.position.x = transform_tf.getOrigin().getX();
    odom_msg_.pose.pose.position.y = transform_tf.getOrigin().getY();
    odom_msg_.pose.pose.position.z = transform_tf.getOrigin().getZ();
    odometry_pub_.publish(odom_msg_);

    // Create pose message and update it with current camera pose
    geometry_msgs::PoseStamped camera_pose_msg_;
    camera_pose_msg_.header.stamp = now;
    camera_pose_msg_.header.frame_id = "map";
    camera_pose_msg_.pose.position.x = transform_tf.getOrigin().getX()*1;
    camera_pose_msg_.pose.position.y = transform_tf.getOrigin().getY()*1;
    camera_pose_msg_.pose.position.z = transform_tf.getOrigin().getZ()*1;
    camera_pose_msg_.pose.orientation.x = transform_tf.getRotation().getX();
    camera_pose_msg_.pose.orientation.y = transform_tf.getRotation().getY();
    camera_pose_msg_.pose.orientation.z = transform_tf.getRotation().getZ();
    camera_pose_msg_.pose.orientation.w = transform_tf.getRotation().getW();
    pose_pub_.publish(camera_pose_msg_);


    // transform broadcast
    static tf2_ros::TransformBroadcaster tf_br;

    geometry_msgs::TransformStamped transformStamped;

    transformStamped.header.stamp = ros::Time::now();
    transformStamped.header.frame_id = "map";
    transformStamped.child_frame_id = "base_link_frame";
    transformStamped.transform.translation.x = transform_tf.getOrigin().getX();
    transformStamped.transform.translation.y = transform_tf.getOrigin().getY();
    transformStamped.transform.translation.z = transform_tf.getOrigin().getZ();
    transformStamped.transform.rotation.x = transform_tf.getRotation().getX();
    transformStamped.transform.rotation.y = transform_tf.getRotation().getY();
    transformStamped.transform.rotation.z = transform_tf.getRotation().getZ();
    transformStamped.transform.rotation.w = transform_tf.getRotation().getW();

    tf_br.sendTransform(transformStamped);
}

std::vector<double> track_times;
const auto tp_0 = std::chrono::steady_clock::now();

sensor_msgs::ImageConstPtr last_left_;
sensor_msgs::ImageConstPtr last_right_;
const sensor_msgs::ImageConstPtr nullImagePtr;

openvslam::system* outSLAM;

ros::Publisher camera_pose_publisher;
ros::Publisher odometry_pub_publisher;
ros::Publisher pause_publisher;
std::string robot_name;

void process_input(){
  if(last_left_ && last_right_){
    const auto tp_1 = std::chrono::steady_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::duration<double>>(tp_1 - tp_0).count();

    // input the current frame and estimate the camera pose
    auto cam_pose = outSLAM->feed_stereo_frame(cv_bridge::toCvShare(last_left_, "bgr8")->image, cv_bridge::toCvShare(last_right_, "bgr8")->image, timestamp, cv::Mat{});

    last_left_ = last_right_ = nullImagePtr;
    const auto tp_2 = std::chrono::steady_clock::now();

    const auto track_time = std::chrono::duration_cast<std::chrono::duration<double>>(tp_2 - tp_1).count();
    track_times.push_back(track_time);

    pose_odometry_pub(cam_pose, camera_pose_publisher, odometry_pub_publisher);

    // publish whether mapping is enabled or disabled
    pause_publisher.publish(outSLAM->mapping_module_is_enabled());
  }
}

void left_callback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::CameraInfoConstPtr& info){
  if(last_left_)
    ROS_WARN_THROTTLE(0.5, "Dropping left image (did not get right before next left)");
  last_left_ = msg;
  process_input();
}

void right_callback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::CameraInfoConstPtr& info){
  if(last_right_)
    ROS_WARN_THROTTLE(0.5, "Dropping right image (did not get left before next right)");
  last_right_ = msg;
  process_input();
}


void vslam_command(const std_msgs::String::ConstPtr& msg) {
  outSLAM->request_reset();
}

void mono_tracking(const std::shared_ptr<openvslam::config>& cfg, const std::string& vocab_file_path, const bool eval_log, const std::string& map_db_path) {
}

void stereo_tracking(const std::shared_ptr<openvslam::config>& cfg, const std::string& vocab_file_path, const bool eval_log, const std::string& map_db_path) {

    // build a SLAM system
    openvslam::system SLAM(cfg, vocab_file_path);
    outSLAM = &SLAM;

    // startup the SLAM process
    SLAM.startup();

    // SLAM.disable_loop_detector(); // will not be going in loops

    // create a viewer object
    // and pass the frame_publisher and the map_publisher
#ifdef USE_PANGOLIN_VIEWER
    pangolin_viewer::viewer viewer(cfg, SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());
#endif

    // initialize this node
    ros::NodeHandle nh;
    image_transport::ImageTransport it(nh);

    image_transport::CameraSubscriber left_sub_ = it.subscribeCamera("/" + robot_name + "/camera/left/image_raw", 1, left_callback);
    image_transport::CameraSubscriber right_sub_ = it.subscribeCamera("/" + robot_name + "/camera/right/image_raw", 1, right_callback);
    pause_publisher = nh.advertise<std_msgs::Bool>("/" + robot_name + "/openvslam/enabled", 1);
    camera_pose_publisher = nh.advertise<geometry_msgs::PoseStamped>("/" + robot_name + "/openvslam/camera_pose", 1);
    odometry_pub_publisher = nh.advertise<nav_msgs::Odometry>("/" + robot_name + "/openvslam/odometry", 1);
    ros::Subscriber reset_sub = nh.subscribe("/" + robot_name + "/vslam/command", 5, vslam_command);


    // run the viewer in another thread
#ifdef USE_PANGOLIN_VIEWER
    std::thread thread([&]() {
        viewer.run();
        if (SLAM.terminate_is_requested()) {
            // wait until the loop BA is finished
            while (SLAM.loop_BA_is_running()) {
                std::this_thread::sleep_for(std::chrono::microseconds(5000));
            }
            ros::shutdown();
        }
    });
#endif

    ros::spin();

    // automatically close the viewer
#ifdef USE_PANGOLIN_VIEWER
    viewer.request_terminate();
    thread.join();
#endif

    // shutdown the SLAM process
    SLAM.shutdown();

    if (eval_log) {
        // output the trajectories for evaluation
        SLAM.save_frame_trajectory("frame_trajectory.txt", "TUM");
        SLAM.save_keyframe_trajectory("keyframe_trajectory.txt", "TUM");
        // output the tracking times for evaluation
        std::ofstream ofs("track_times.txt", std::ios::out);
        if (ofs.is_open()) {
            for (const auto track_time : track_times) {
                ofs << track_time << std::endl;
            }
            ofs.close();
        }
    }

    if (!map_db_path.empty()) {
        // output the map database
        SLAM.save_map_database(map_db_path);
    }

    if (track_times.size()) {
        std::sort(track_times.begin(), track_times.end());
        const auto total_track_time = std::accumulate(track_times.begin(), track_times.end(), 0.0);
        std::cout << "median tracking time: " << track_times.at(track_times.size() / 2) << "[s]" << std::endl;
        std::cout << "mean tracking time: " << total_track_time / track_times.size() << "[s]" << std::endl;
    }
}

int main(int argc, char* argv[]) {
#ifdef USE_STACK_TRACE_LOGGER
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
#endif

    // create options
    popl::OptionParser op("Allowed options");
    auto help = op.add<popl::Switch>("h", "help", "produce help message");
    auto vocab_file_path = op.add<popl::Value<std::string>>("v", "vocab", "vocabulary file path");
    auto setting_file_path = op.add<popl::Value<std::string>>("c", "config", "setting file path");
    auto rn = op.add<popl::Value<std::string>>("r", "robot", "robot name (e.g., scout_1)");
    auto debug_mode = op.add<popl::Switch>("", "debug", "debug mode");
    auto eval_log = op.add<popl::Switch>("", "eval-log", "store trajectory and tracking times for evaluation");
    auto map_db_path = op.add<popl::Value<std::string>>("", "map-db", "store a map database at this path after SLAM", "");
    try {
        op.parse(argc, argv);
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // check validness of options
    if (help->is_set()) {
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }
    if (!vocab_file_path->is_set() || !setting_file_path->is_set()) {
        std::cerr << "invalid arguments" << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // setup logger
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%L] %v%$");
    if (debug_mode->is_set()) {
        spdlog::set_level(spdlog::level::debug);
    }
    else {
        spdlog::set_level(spdlog::level::info);
    }

    robot_name = rn->value();
    ros::init(argc, argv, robot_name);

    // load configuration
    std::shared_ptr<openvslam::config> cfg;
    try {
        cfg = std::make_shared<openvslam::config>(setting_file_path->value());
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStart("slam.prof");
#endif

    // run tracking
    if (cfg->camera_->setup_type_ == openvslam::camera::setup_type_t::Monocular) {
        mono_tracking(cfg, vocab_file_path->value(), eval_log->is_set(), map_db_path->value());
    }
    else if (cfg->camera_->setup_type_ == openvslam::camera::setup_type_t::Stereo) {
        stereo_tracking(cfg, vocab_file_path->value(), eval_log->is_set(), map_db_path->value());
    }
    else {
        throw std::runtime_error("Invalid setup type: " + cfg->camera_->get_setup_type_string());
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStop();
#endif

    return EXIT_SUCCESS;
}
