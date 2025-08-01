#include <airsim_ros_wrapper.h>
#include "common/AirSimSettings.hpp"
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <chrono>
#include <future>
#include <thread>
#include <algorithm>

using namespace std::placeholders;

constexpr char AirsimROSWrapper::CAM_YML_NAME[];
constexpr char AirsimROSWrapper::WIDTH_YML_NAME[];
constexpr char AirsimROSWrapper::HEIGHT_YML_NAME[];
constexpr char AirsimROSWrapper::K_YML_NAME[];
constexpr char AirsimROSWrapper::D_YML_NAME[];
constexpr char AirsimROSWrapper::R_YML_NAME[];
constexpr char AirsimROSWrapper::P_YML_NAME[];
constexpr char AirsimROSWrapper::DMODEL_YML_NAME[];

const std::unordered_map<int, std::string> AirsimROSWrapper::image_type_int_to_string_map_ = {
    { 0, "Scene" },
    { 1, "DepthPlanar" },
    { 2, "DepthPerspective" },
    { 3, "DepthVis" },
    { 4, "DisparityNormalized" },
    { 5, "Segmentation" },
    { 6, "SurfaceNormals" },
    { 7, "Infrared" },
    { 8, "OpticalFlow" },
    { 9, "OpticalFlowVis" },
    { 10, "Lighting" },
    { 11, "Annotation" },
};

AirsimROSWrapper::AirsimROSWrapper(const std::shared_ptr<rclcpp::Node> nh, const std::shared_ptr<rclcpp::Node> nh_img, const std::shared_ptr<rclcpp::Node> nh_lidar, const std::shared_ptr<rclcpp::Node> nh_gpulidar, const std::shared_ptr<rclcpp::Node> nh_echo, const std::string& host_ip, const std::shared_ptr<rclcpp::CallbackGroup> callbackGroup, bool enable_api_control, bool enable_object_transforms_list, uint16_t host_port)
    : is_used_lidar_timer_cb_queue_(false)
    , is_used_img_timer_cb_queue_(false)
    , is_used_gpulidar_timer_cb_queue_(false)
    , is_used_echo_timer_cb_queue_(false)
    , airsim_settings_parser_(host_ip, host_port)
    , host_ip_(host_ip)
    , host_port_(host_port)
    , enable_api_control_(enable_api_control)
    , enable_object_transforms_list_(enable_object_transforms_list)
    , airsim_client_(nullptr)
    , airsim_client_images_(host_ip, host_port)
    , airsim_client_lidar_(host_ip, host_port)
    , airsim_client_gpulidar_(host_ip, host_port)
    , airsim_client_echo_(host_ip, host_port)
    , nh_(nh)
    , nh_img_(nh_img)
    , nh_lidar_(nh_lidar)
    , nh_gpulidar_(nh_gpulidar)
    , nh_echo_(nh_echo)
    , cb_(callbackGroup)
    , publish_clock_(false)
{
    ros_clock_.clock = rclcpp::Time(0);

    if (AirSimSettings::singleton().simmode_name == AirSimSettings::kSimModeTypeMultirotor) {
        airsim_mode_ = AIRSIM_MODE::DRONE;
        RCLCPP_INFO(nh_->get_logger(), "Setting ROS wrapper to DRONE mode");
    }
    else if (AirSimSettings::singleton().simmode_name == AirSimSettings::kSimModeTypeCar || AirSimSettings::singleton().simmode_name == AirSimSettings::kSimModeTypeSkidVehicle){
        airsim_mode_ = AIRSIM_MODE::CAR;
        RCLCPP_INFO(nh_->get_logger(), "Setting ROS wrapper to CAR mode");
    }else{
        airsim_mode_ = AIRSIM_MODE::COMPUTERVISION;
        RCLCPP_INFO(nh_->get_logger(), "Setting ROS wrapper to COMPUTERVISION mode");
    }
    tf_broadcaster_ = std::make_shared<tf2_ros::TransformBroadcaster>(nh_);
    static_tf_pub_ = std::make_shared<tf2_ros::StaticTransformBroadcaster>(nh_);

    initialize_ros();

    RCLCPP_INFO(nh_->get_logger(), "AirsimROSWrapper Initialized!");
}

void AirsimROSWrapper::initialize_airsim()
{
    // todo do not reset if already in air?
    try {

        if (airsim_mode_ == AIRSIM_MODE::DRONE) {
            airsim_client_ = std::unique_ptr<msr::airlib::RpcLibClientBase>(new msr::airlib::MultirotorRpcLibClient(host_ip_, host_port_));
        }
        else if(airsim_mode_ == AIRSIM_MODE::CAR) {
            airsim_client_ = std::unique_ptr<msr::airlib::RpcLibClientBase>(new msr::airlib::CarRpcLibClient(host_ip_, host_port_));
        }else{
            airsim_client_ = std::unique_ptr<msr::airlib::RpcLibClientBase>(new msr::airlib::ComputerVisionRpcLibClient(host_ip_, host_port_));
        }
        airsim_client_->confirmConnection();
        airsim_client_images_.confirmConnection();
        airsim_client_lidar_.confirmConnection();
        airsim_client_gpulidar_.confirmConnection();
        airsim_client_echo_.confirmConnection();

        if(enable_api_control_){
            for (const auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {            
                airsim_client_->enableApiControl(true, vehicle_name_ptr_pair.first); // todo expose as rosservice?
                airsim_client_->armDisarm(true, vehicle_name_ptr_pair.first); // todo exposes as rosservice?
            }
        }

        origin_geo_point_ = get_origin_geo_point();
        // todo there's only one global origin geopoint for environment. but airsim API accept a parameter vehicle_name? inside carsimpawnapi.cpp, there's a geopoint being assigned in the constructor. by?
        origin_geo_point_msg_ = get_gps_msg_from_airsim_geo_point(origin_geo_point_);
        
        auto vehicle_name_ptr_pair = vehicle_name_ptr_map_.begin();
        auto& vehicle_ros = vehicle_name_ptr_pair->second;
        airsim_interfaces::msg::InstanceSegmentationList instance_segmentation_list_msg = get_instance_segmentation_list_msg_from_airsim();      
        instance_segmentation_list_msg.header.stamp = vehicle_ros->stamp_;
        instance_segmentation_list_msg.header.frame_id = world_frame_id_;
        vehicle_ros->instance_segmentation_pub_->publish(instance_segmentation_list_msg);

        if(enable_object_transforms_list_){
            airsim_interfaces::msg::ObjectTransformsList object_transforms_list_msg = get_object_transforms_list_msg_from_airsim(vehicle_ros->stamp_);      
            vehicle_ros->object_transforms_pub_->publish(object_transforms_list_msg);
        }
    }
    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API, something went wrong.\n%s", msg.c_str());
        rclcpp::shutdown();
    }
}

void AirsimROSWrapper::initialize_ros()
{

    // ros params
    double update_airsim_control_every_n_sec;
    nh_->get_parameter("is_vulkan", is_vulkan_);
    nh_->get_parameter("update_airsim_control_every_n_sec", update_airsim_control_every_n_sec);
    nh_->get_parameter("publish_clock", publish_clock_);
    nh_->get_parameter_or("world_frame_id", world_frame_id_, world_frame_id_);
    nh_->get_parameter_or("odom_frame_id", odom_frame_id_, odom_frame_id_);
    vel_cmd_duration_ = 0.05; // todo rosparam
    // todo enforce dynamics constraints in this node as well?
    // nh_->get_parameter("max_vert_vel_", max_vert_vel_);
    // nh_->get_parameter("max_horz_vel", max_horz_vel_)

    nh_->declare_parameter("vehicle_name", rclcpp::ParameterValue(""));
    create_ros_pubs_from_settings_json();
    airsim_control_update_timer_ = nh_->create_wall_timer(std::chrono::duration<double>(update_airsim_control_every_n_sec), std::bind(&AirsimROSWrapper::drone_state_timer_cb, this), cb_);
}

void AirsimROSWrapper::create_ros_pubs_from_settings_json()
{
    // subscribe to control commands on global nodehandle
    gimbal_angle_quat_cmd_sub_ = nh_->create_subscription<airsim_interfaces::msg::GimbalAngleQuatCmd>("~/gimbal_angle_quat_cmd", 50, std::bind(&AirsimROSWrapper::gimbal_angle_quat_cmd_cb, this, _1));
    gimbal_angle_euler_cmd_sub_ = nh_->create_subscription<airsim_interfaces::msg::GimbalAngleEulerCmd>("~/gimbal_angle_euler_cmd", 50, std::bind(&AirsimROSWrapper::gimbal_angle_euler_cmd_cb, this, _1));
    origin_geo_point_pub_ = nh_->create_publisher<airsim_interfaces::msg::GPSYaw>("~/origin_geo_point", 10);

    airsim_img_request_vehicle_name_pair_vec_.clear();
    image_pub_vec_.clear();
    cam_info_pub_vec_.clear();
    camera_info_msg_vec_.clear();
    vehicle_name_ptr_map_.clear();
    size_t lidar_cnt = 0;
    size_t gpulidar_cnt = 0;
    size_t echo_cnt = 0;

    image_transport::ImageTransport image_transporter(nh_);

    // iterate over std::map<std::string, std::unique_ptr<VehicleSetting>> vehicles;
    for (const auto& curr_vehicle_elem : AirSimSettings::singleton().vehicles) {
        auto& vehicle_setting = curr_vehicle_elem.second;
        auto curr_vehicle_name = curr_vehicle_elem.first;

        nh_->set_parameter(rclcpp::Parameter("vehicle_name", curr_vehicle_name));

        set_nans_to_zeros_in_pose(*vehicle_setting);

        std::unique_ptr<VehicleROS> vehicle_ros = nullptr;

        if (airsim_mode_ == AIRSIM_MODE::DRONE) {
            vehicle_ros = std::unique_ptr<MultiRotorROS>(new MultiRotorROS());
        }
        else if(airsim_mode_ == AIRSIM_MODE::CAR){
            vehicle_ros = std::unique_ptr<CarROS>(new CarROS());
        }else{
            vehicle_ros = std::unique_ptr<ComputerVisionROS>(new ComputerVisionROS());
        }

        vehicle_ros->odom_frame_id_ = curr_vehicle_name + "/" + odom_frame_id_;
        vehicle_ros->vehicle_name_ = curr_vehicle_name;

        append_static_vehicle_tf(vehicle_ros.get(), *vehicle_setting);

        const std::string topic_prefix = "~/" + curr_vehicle_name;
        vehicle_ros->odom_local_pub_ = nh_->create_publisher<nav_msgs::msg::Odometry>(topic_prefix + "/" + odom_frame_id_, 10);

        vehicle_ros->env_pub_ = nh_->create_publisher<airsim_interfaces::msg::Environment>(topic_prefix + "/environment", 10);

        vehicle_ros->global_gps_pub_ = nh_->create_publisher<sensor_msgs::msg::NavSatFix>(topic_prefix + "/global_gps", 10);

        auto qos_settings = rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable();

        vehicle_ros->instance_segmentation_pub_ = nh_->create_publisher<airsim_interfaces::msg::InstanceSegmentationList>("~/instance_segmentation_labels", qos_settings);

        std::function<bool(std::shared_ptr<airsim_interfaces::srv::RefreshInstanceSegmentation::Request>, std::shared_ptr<airsim_interfaces::srv::RefreshInstanceSegmentation::Response>)> fcn_ins_seg_refresh_srvr = std::bind(&AirsimROSWrapper::instance_segmentation_refresh_cb, this, _1, _2);
        vehicle_ros->instance_segmentation_refresh_srvr_ = nh_->create_service<airsim_interfaces::srv::RefreshInstanceSegmentation>(topic_prefix + "/instance_segmentation_refresh", fcn_ins_seg_refresh_srvr);

        if(enable_object_transforms_list_){
            vehicle_ros->object_transforms_pub_ = nh_->create_publisher<airsim_interfaces::msg::ObjectTransformsList>("~/object_transforms", qos_settings);

            std::function<bool(std::shared_ptr<airsim_interfaces::srv::RefreshObjectTransforms::Request>, std::shared_ptr<airsim_interfaces::srv::RefreshObjectTransforms::Response>)> fcn_obj_trans_refresh_srvr = std::bind(&AirsimROSWrapper::object_transforms_refresh_cb, this, _1, _2);
            vehicle_ros->object_transforms_refresh_srvr_ = nh_->create_service<airsim_interfaces::srv::RefreshObjectTransforms>(topic_prefix + "/object_transforms_refresh", fcn_obj_trans_refresh_srvr);
        }

        if (airsim_mode_ == AIRSIM_MODE::DRONE) {
            auto drone = static_cast<MultiRotorROS*>(vehicle_ros.get());

            // bind to a single callback. todo optimal subs queue length
            // bind multiple topics to a single callback, but keep track of which vehicle name it was by passing curr_vehicle_name as the 2nd argument

            std::function<void(const airsim_interfaces::msg::VelCmd::SharedPtr)> fcn_vel_cmd_body_frame_sub = std::bind(&AirsimROSWrapper::vel_cmd_body_frame_cb, this, _1, vehicle_ros->vehicle_name_);
            drone->vel_cmd_body_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmd>(topic_prefix + "/vel_cmd_body_frame", 1, fcn_vel_cmd_body_frame_sub); // todo ros::TransportHints().tcpNoDelay();

            std::function<void(const airsim_interfaces::msg::VelCmd::SharedPtr)> fcn_vel_cmd_world_frame_sub = std::bind(&AirsimROSWrapper::vel_cmd_world_frame_cb, this, _1, vehicle_ros->vehicle_name_);
            drone->vel_cmd_world_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmd>(topic_prefix + "/vel_cmd_world_frame", 1, fcn_vel_cmd_world_frame_sub);

            std::function<bool(std::shared_ptr<airsim_interfaces::srv::Takeoff::Request>, std::shared_ptr<airsim_interfaces::srv::Takeoff::Response>)> fcn_takeoff_srvr = std::bind(&AirsimROSWrapper::takeoff_srv_cb, this, _1, _2, vehicle_ros->vehicle_name_);
            drone->takeoff_srvr_ = nh_->create_service<airsim_interfaces::srv::Takeoff>(topic_prefix + "/takeoff", fcn_takeoff_srvr);

            std::function<bool(std::shared_ptr<airsim_interfaces::srv::Land::Request>, std::shared_ptr<airsim_interfaces::srv::Land::Response>)> fcn_land_srvr = std::bind(&AirsimROSWrapper::land_srv_cb, this, _1, _2, vehicle_ros->vehicle_name_);
            drone->land_srvr_ = nh_->create_service<airsim_interfaces::srv::Land>(topic_prefix + "/land", fcn_land_srvr);

            std::function<bool(std::shared_ptr<airsim_interfaces::srv::SetAltitude::Request>, std::shared_ptr<airsim_interfaces::srv::SetAltitude::Response>)> fcn_set_altitude_srvr = std::bind(&AirsimROSWrapper::set_altitude_srv_cb, this, _1, _2, vehicle_ros->vehicle_name_);
            drone->set_altitude_srvr_ = nh_->create_service<airsim_interfaces::srv::SetAltitude>(topic_prefix + "/set_altitude", fcn_set_altitude_srvr);

            std::function<bool(std::shared_ptr<airsim_interfaces::srv::SetLocalPosition::Request>, std::shared_ptr<airsim_interfaces::srv::SetLocalPosition::Response>)> fcn_set_local_position_srvr = std::bind(&AirsimROSWrapper::set_local_position_srv_cb, this, _1, _2, vehicle_ros->vehicle_name_);
            drone->set_local_position_srvr_ = nh_->create_service<airsim_interfaces::srv::SetLocalPosition>(topic_prefix + "/set_local_position", fcn_set_local_position_srvr);

            // vehicle_ros.reset_srvr = nh_->create_service(curr_vehicle_name + "/reset",&AirsimROSWrapper::reset_srv_cb, this);
        }
        else if(airsim_mode_ == AIRSIM_MODE::CAR) {
            auto car = static_cast<CarROS*>(vehicle_ros.get());
            if(enable_api_control_){
                std::function<void(const airsim_interfaces::msg::CarControls::SharedPtr)> fcn_car_cmd_sub = std::bind(&AirsimROSWrapper::car_cmd_cb, this, _1, vehicle_ros->vehicle_name_);
                car->car_cmd_sub_ = nh_->create_subscription<airsim_interfaces::msg::CarControls>(topic_prefix + "/car_cmd", 1, fcn_car_cmd_sub);
            }
            car->car_state_pub_ = nh_->create_publisher<airsim_interfaces::msg::CarState>(topic_prefix + "/car_state", 10);
        }else{
            auto computer_vision = static_cast<ComputerVisionROS*>(vehicle_ros.get());
            computer_vision->computer_vision_state_pub_ = nh_->create_publisher<airsim_interfaces::msg::ComputerVisionState>(topic_prefix + "/computervision_state", 10);
        }

        // iterate over camera map std::map<std::string, CameraSetting> .cameras;
        for (auto& curr_camera_elem : vehicle_setting->cameras) {
            auto& camera_setting = curr_camera_elem.second;
            auto& curr_camera_name = curr_camera_elem.first;

            set_nans_to_zeros_in_pose(*vehicle_setting, camera_setting);
            append_static_camera_tf(vehicle_ros.get(), curr_camera_name, camera_setting);
            // camera_setting.gimbal
            std::vector<ImageRequest> current_image_request_vec;
            current_image_request_vec.clear();

            // iterate over capture_setting std::map<int, CaptureSetting> capture_settings
            for (const auto& curr_capture_elem : camera_setting.capture_settings) {
                auto& capture_setting = curr_capture_elem.second;                

                // todo why does AirSimSettings::loadCaptureSettings calls AirSimSettings::initializeCaptureSettings()
                // which initializes default capture settings for _all_ NINE msr::airlib::ImageCaptureBase::ImageType
                if (!(std::isnan(capture_setting.fov_degrees))) {
                    ImageType curr_image_type = msr::airlib::Utils::toEnum<ImageType>(capture_setting.image_type);
                    if(curr_image_type == ImageType::Annotation) {
                        for( const auto& curr_annotation_element :  AirSimSettings::singleton().annotator_settings){
                            current_image_request_vec.push_back(ImageRequest(curr_camera_name, curr_image_type, false, false, curr_annotation_element.name));            
                            const std::string camera_topic_prefix = topic_prefix + "/" + curr_camera_name + "_" + image_type_int_to_string_map_.at(capture_setting.image_type) + "_" + curr_annotation_element.name;
                            const std::string image_topic = camera_topic_prefix + "/image";
                            const std::string camera_info_topic = camera_topic_prefix + "/camera_info";
                            image_pub_vec_.push_back(image_transporter.advertise(image_topic, 1));
                            cam_info_pub_vec_.push_back(nh_->create_publisher<sensor_msgs::msg::CameraInfo>(camera_info_topic, 10));
                            camera_info_msg_vec_.push_back(generate_cam_info(curr_camera_name, camera_setting, capture_setting));
                        }
                    }else{
                        if (curr_image_type == ImageType::DepthPlanar || curr_image_type == ImageType::DepthPerspective || curr_image_type == ImageType::DepthVis || curr_image_type == ImageType::DisparityNormalized) {
                            current_image_request_vec.push_back(ImageRequest(curr_camera_name, curr_image_type, true));
                        }
                        else {
                            current_image_request_vec.push_back(ImageRequest(curr_camera_name, curr_image_type, false, false));                        
                        }
                        const std::string camera_topic_prefix = topic_prefix + "/" + curr_camera_name + "_" + image_type_int_to_string_map_.at(capture_setting.image_type);
                        const std::string image_topic = camera_topic_prefix + "/image";
                        const std::string camera_info_topic = camera_topic_prefix + "/camera_info";
                        image_pub_vec_.push_back(image_transporter.advertise(image_topic, 1));
                        cam_info_pub_vec_.push_back(nh_->create_publisher<sensor_msgs::msg::CameraInfo>(camera_info_topic, 10));
                        camera_info_msg_vec_.push_back(generate_cam_info(curr_camera_name, camera_setting, capture_setting));
                    }                    
                }
            }
            // push back pair (vector of image captures, current vehicle name)
            airsim_img_request_vehicle_name_pair_vec_.push_back(std::make_pair(current_image_request_vec, curr_vehicle_name));
        }

        // iterate over sensors
        for (auto& curr_sensor_map : vehicle_setting->sensors) {
            auto& sensor_name = curr_sensor_map.first;
            auto& sensor_setting = curr_sensor_map.second;

            if (sensor_setting->enabled) {

                switch (sensor_setting->sensor_type) {
                case SensorBase::SensorType::Barometer: {
                    SensorPublisher<airsim_interfaces::msg::Altimeter> sensor_publisher =
                        create_sensor_publisher<airsim_interfaces::msg::Altimeter>("Barometer sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/altimeter/" + sensor_name, 10);
                    vehicle_ros->barometer_pubs_.emplace_back(sensor_publisher);
                    break;
                }
                case SensorBase::SensorType::Imu: {
                    SensorPublisher<sensor_msgs::msg::Imu> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::Imu>("Imu sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/imu/" + sensor_name, 10);
                    vehicle_ros->imu_pubs_.emplace_back(sensor_publisher);
                    break;
                }
                case SensorBase::SensorType::Gps: {
                    SensorPublisher<sensor_msgs::msg::NavSatFix> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::NavSatFix>("Gps sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/gps/" + sensor_name, 10);
                    vehicle_ros->gps_pubs_.emplace_back(sensor_publisher);
                    break;
                }
                case SensorBase::SensorType::Magnetometer: {
                    SensorPublisher<sensor_msgs::msg::MagneticField> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::MagneticField>("Magnetometer sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/magnetometer/" + sensor_name, 10);
                    vehicle_ros->magnetometer_pubs_.emplace_back(sensor_publisher);
                    break;
                }
                case SensorBase::SensorType::Distance: {
                    SensorPublisher<sensor_msgs::msg::Range> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::Range>("Distance sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/distance/" + sensor_name, 10);
                    vehicle_ros->distance_pubs_.emplace_back(sensor_publisher);
                    break;
                }
                case SensorBase::SensorType::Lidar: {
                    auto lidar_setting = *static_cast<LidarSetting*>(sensor_setting.get());
                    msr::airlib::LidarSimpleParams params;
                    params.initializeFromSettings(lidar_setting);
                    append_static_lidar_tf(vehicle_ros.get(), sensor_name, params);

                    SensorPublisher<sensor_msgs::msg::PointCloud2> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::PointCloud2>("Lidar sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/lidar/points/" + sensor_name, 10);
                    vehicle_ros->lidar_pubs_.emplace_back(sensor_publisher);
                    SensorPublisher<airsim_interfaces::msg::StringArray> sensor_publisher_labels =
                        create_sensor_publisher<airsim_interfaces::msg::StringArray>("", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/lidar/labels/" + sensor_name, 10);
                    vehicle_ros->lidar_labels_pubs_.emplace_back(sensor_publisher_labels);
                    lidar_cnt += 1;
                    break;
                }
                case SensorBase::SensorType::GPULidar: {
                    auto gpulidar_setting = *static_cast<GPULidarSetting*>(sensor_setting.get());
                    msr::airlib::GPULidarSimpleParams params;
                    params.initializeFromSettings(gpulidar_setting);
                    append_static_gpulidar_tf(vehicle_ros.get(), sensor_name, params);

                    SensorPublisher<sensor_msgs::msg::PointCloud2> sensor_publisher =
                        create_sensor_publisher<sensor_msgs::msg::PointCloud2>("GPULidar sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/gpulidar/points/" + sensor_name, 10);
                    vehicle_ros->gpulidar_pubs_.emplace_back(sensor_publisher);
                    gpulidar_cnt += 1;
                    break;
                }
                case SensorBase::SensorType::Echo: {
                    auto echo_setting = *static_cast<EchoSetting*>(sensor_setting.get());
                    msr::airlib::EchoSimpleParams params;
                    params.initializeFromSettings(echo_setting);
                    append_static_echo_tf(vehicle_ros.get(), sensor_name, params);
                    if(params.active){
                        SensorPublisher<sensor_msgs::msg::PointCloud2> sensor_publisher =
                            create_sensor_publisher<sensor_msgs::msg::PointCloud2>("Echo (active) sensor", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/echo/active/points/" + sensor_name, 10);
                        vehicle_ros->echo_active_pubs_.emplace_back(sensor_publisher);
                        SensorPublisher<airsim_interfaces::msg::StringArray> sensor_publisher_labels =
                        create_sensor_publisher<airsim_interfaces::msg::StringArray>("", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/echo/active/labels/" + sensor_name, 10);
                        vehicle_ros->echo_active_labels_pubs_.emplace_back(sensor_publisher_labels);

                    }
                    if(params.passive){
                        SensorPublisher<sensor_msgs::msg::PointCloud2> sensor_publisher =
                            create_sensor_publisher<sensor_msgs::msg::PointCloud2>("Echo (passive) sensor ", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/echo/passive/points/" + sensor_name, 10);
                        vehicle_ros->echo_passive_pubs_.emplace_back(sensor_publisher);
                        SensorPublisher<airsim_interfaces::msg::StringArray> sensor_publisher_labels =
                        create_sensor_publisher<airsim_interfaces::msg::StringArray>("", sensor_setting->sensor_name, sensor_setting->sensor_type, curr_vehicle_name + "/echo/passive/labels/" + sensor_name, 10);
                        vehicle_ros->echo_passive_labels_pubs_.emplace_back(sensor_publisher_labels);
                    }                    
                    echo_cnt += 1;
                    break;
                }
                default: {
                    throw std::invalid_argument("Unexpected sensor type");
                }
                }
            }
        }

        vehicle_name_ptr_map_.emplace(curr_vehicle_name, std::move(vehicle_ros)); // allows fast lookup in command callbacks in case of a lot of drones
    
      
    }   

    // add takeoff and land all services if more than 2 drones
    if (vehicle_name_ptr_map_.size() > 1 && airsim_mode_ == AIRSIM_MODE::DRONE) {
        takeoff_all_srvr_ = nh_->create_service<airsim_interfaces::srv::Takeoff>("~/all_robots/takeoff", std::bind(&AirsimROSWrapper::takeoff_all_srv_cb, this, _1, _2));
        land_all_srvr_ = nh_->create_service<airsim_interfaces::srv::Land>("~/all_robots/land", std::bind(&AirsimROSWrapper::land_all_srv_cb, this, _1, _2));

        vel_cmd_all_body_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmd>("~/all_robots/vel_cmd_body_frame", 1, std::bind(&AirsimROSWrapper::vel_cmd_all_body_frame_cb, this, _1));
        vel_cmd_all_world_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmd>("~/all_robots/vel_cmd_world_frame", 1, std::bind(&AirsimROSWrapper::vel_cmd_all_world_frame_cb, this, _1));

        vel_cmd_group_body_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmdGroup>("~/group_of_robots/vel_cmd_body_frame", 1, std::bind(&AirsimROSWrapper::vel_cmd_group_body_frame_cb, this, _1));
        vel_cmd_group_world_frame_sub_ = nh_->create_subscription<airsim_interfaces::msg::VelCmdGroup>("~/group_of_robots/vel_cmd_world_frame", 1, std::bind(&AirsimROSWrapper::vel_cmd_group_world_frame_cb, this, _1));

        takeoff_group_srvr_ = nh_->create_service<airsim_interfaces::srv::TakeoffGroup>("~/group_of_robots/takeoff", std::bind(&AirsimROSWrapper::takeoff_group_srv_cb, this, _1, _2));
        land_group_srvr_ = nh_->create_service<airsim_interfaces::srv::LandGroup>("~/group_of_robots/land", std::bind(&AirsimROSWrapper::land_group_srv_cb, this, _1, _2));
    }

    // todo add per vehicle reset in AirLib API
    reset_srvr_ = nh_->create_service<airsim_interfaces::srv::Reset>("~/reset", std::bind(&AirsimROSWrapper::reset_srv_cb, this, _1, _2));

    // Coordinated height and land service (multi-vehicle coordination)
    coordinated_height_and_land_srvr_ = nh_->create_service<airsim_interfaces::srv::CoordinatedHeightAndLand>("~/coordinated_height_and_land", std::bind(&AirsimROSWrapper::coordinated_height_and_land_srv_cb, this, _1, _2));

    list_scene_object_tags_srvr_ = nh_->create_service<airsim_interfaces::srv::ListSceneObjectTags>("~/list_scene_object_tags", std::bind(&AirsimROSWrapper::list_scene_object_tags_srv_cb, this, _1, _2));

    if (publish_clock_) {
        clock_pub_ = nh_->create_publisher<rosgraph_msgs::msg::Clock>("~/clock", 1);
    }

    // if >0 cameras, add one more thread for img_request_timer_cb
    if (!airsim_img_request_vehicle_name_pair_vec_.empty()) {
        double update_airsim_img_response_every_n_sec;
        nh_->get_parameter("update_airsim_img_response_every_n_sec", update_airsim_img_response_every_n_sec);

        airsim_img_response_timer_ = nh_img_->create_wall_timer(std::chrono::duration<double>(update_airsim_img_response_every_n_sec), std::bind(&AirsimROSWrapper::img_response_timer_cb, this), cb_);
        is_used_img_timer_cb_queue_ = true;
    }

    // lidar, gpulidar and echo sensors update on their own callbacks/threads at a given rate
    if (lidar_cnt > 0) {
        double update_lidar_every_n_sec;
        nh_->get_parameter("update_lidar_every_n_sec", update_lidar_every_n_sec);
        airsim_lidar_update_timer_ = nh_lidar_->create_wall_timer(std::chrono::duration<double>(update_lidar_every_n_sec), std::bind(&AirsimROSWrapper::lidar_timer_cb, this), cb_);
        is_used_lidar_timer_cb_queue_ = true;
    }
    if (gpulidar_cnt > 0) {
        double update_gpulidar_every_n_sec;
        nh_->get_parameter("update_gpulidar_every_n_sec", update_gpulidar_every_n_sec);
        airsim_gpulidar_update_timer_ = nh_gpulidar_->create_wall_timer(std::chrono::duration<double>(update_gpulidar_every_n_sec), std::bind(&AirsimROSWrapper::gpulidar_timer_cb, this), cb_);
        is_used_gpulidar_timer_cb_queue_ = true;
    }

    if (echo_cnt > 0) {
        double update_echo_every_n_sec;
        nh_->get_parameter("update_echo_every_n_sec", update_echo_every_n_sec);
        airsim_echo_update_timer_ = nh_echo_->create_wall_timer(std::chrono::duration<double>(update_echo_every_n_sec), std::bind(&AirsimROSWrapper::echo_timer_cb, this), cb_);
        is_used_echo_timer_cb_queue_ = true;
    }


    initialize_airsim();
}

// QoS - The depth of the publisher message queue.
// more details here - https://docs.ros.org/en/foxy/Concepts/About-Quality-of-Service-Settings.html
template <typename T>
const SensorPublisher<T> AirsimROSWrapper::create_sensor_publisher(const std::string& sensor_type_name, const std::string& sensor_name,
                                                                   SensorBase::SensorType sensor_type, const std::string& topic_name, int QoS)
{
    if(sensor_type_name != "")
        RCLCPP_INFO_STREAM(nh_->get_logger(), "Publishing " << sensor_type_name << " '" << sensor_name << "'");

    SensorPublisher<T> sensor_publisher;
    sensor_publisher.sensor_name = sensor_name;
    sensor_publisher.sensor_type = sensor_type;
    sensor_publisher.publisher = nh_->create_publisher<T>("~/" + topic_name, QoS);
    return sensor_publisher;
}

// todo: error check. if state is not landed, return error.
bool AirsimROSWrapper::takeoff_srv_cb(std::shared_ptr<airsim_interfaces::srv::Takeoff::Request> request, std::shared_ptr<airsim_interfaces::srv::Takeoff::Response> response, const std::string& vehicle_name)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name)->waitOnLastTask(); // todo value for timeout_sec?
    // response->success =
    else
        static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name);
    // response->success =

    return true;
}

bool AirsimROSWrapper::takeoff_group_srv_cb(std::shared_ptr<airsim_interfaces::srv::TakeoffGroup::Request> request, std::shared_ptr<airsim_interfaces::srv::TakeoffGroup::Response> response)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        for (const auto& vehicle_name : request->vehicle_names)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name)->waitOnLastTask(); // todo value for timeout_sec?
    // response->success =
    else
        for (const auto& vehicle_name : request->vehicle_names)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name);
    // response->success =

    return true;
}

bool AirsimROSWrapper::takeoff_all_srv_cb(std::shared_ptr<airsim_interfaces::srv::Takeoff::Request> request, std::shared_ptr<airsim_interfaces::srv::Takeoff::Response> response)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        for (const auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name_ptr_pair.first)->waitOnLastTask(); // todo value for timeout_sec?
    // response->success =
    else
        for (const auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->takeoffAsync(20, vehicle_name_ptr_pair.first);
    // response->success =

    return true;
}

bool AirsimROSWrapper::instance_segmentation_refresh_cb(const std::shared_ptr<airsim_interfaces::srv::RefreshInstanceSegmentation::Request> request, const std::shared_ptr<airsim_interfaces::srv::RefreshInstanceSegmentation::Response> response)
{
    unused(response);
    unused(request);
    std::lock_guard<std::mutex> guard(control_mutex_);

    RCLCPP_INFO_STREAM(nh_->get_logger(), "Starting instance segmentation refresh...");

    auto vehicle_name_ptr_pair = vehicle_name_ptr_map_.begin();
    auto& vehicle_ros = vehicle_name_ptr_pair->second;
    airsim_interfaces::msg::InstanceSegmentationList instance_segmentation_list_msg  = get_instance_segmentation_list_msg_from_airsim();      
    instance_segmentation_list_msg.header.stamp = vehicle_ros->stamp_;
    instance_segmentation_list_msg.header.frame_id = world_frame_id_;
    vehicle_ros->instance_segmentation_pub_->publish(instance_segmentation_list_msg);

    RCLCPP_INFO_STREAM(nh_->get_logger(), "Completed instance segmentation refresh!");

    return true;
}

bool AirsimROSWrapper::object_transforms_refresh_cb(const std::shared_ptr<airsim_interfaces::srv::RefreshObjectTransforms::Request> request, const std::shared_ptr<airsim_interfaces::srv::RefreshObjectTransforms::Response> response)
{
    unused(response);
    unused(request);
    std::lock_guard<std::mutex> guard(control_mutex_);

    RCLCPP_INFO_STREAM(nh_->get_logger(), "Starting object transforms refresh...");

    auto vehicle_name_ptr_pair = vehicle_name_ptr_map_.begin();
    auto& vehicle_ros = vehicle_name_ptr_pair->second;
    airsim_interfaces::msg::ObjectTransformsList object_transforms_list_msg  = get_object_transforms_list_msg_from_airsim(vehicle_ros->stamp_);      
    vehicle_ros->object_transforms_pub_->publish(object_transforms_list_msg);

    RCLCPP_INFO_STREAM(nh_->get_logger(), "Completed object transforms refresh!");

    return true;
}

bool AirsimROSWrapper::land_srv_cb(std::shared_ptr<airsim_interfaces::srv::Land::Request> request, std::shared_ptr<airsim_interfaces::srv::Land::Response> response, const std::string& vehicle_name)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name)->waitOnLastTask();
    else
        static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name);

    return true; //todo
}

bool AirsimROSWrapper::land_group_srv_cb(std::shared_ptr<airsim_interfaces::srv::LandGroup::Request> request, std::shared_ptr<airsim_interfaces::srv::LandGroup::Response> response)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        for (const auto& vehicle_name : request->vehicle_names)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name)->waitOnLastTask();
    else
        for (const auto& vehicle_name : request->vehicle_names)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name);

    return true; //todo
}

bool AirsimROSWrapper::land_all_srv_cb(std::shared_ptr<airsim_interfaces::srv::Land::Request> request, std::shared_ptr<airsim_interfaces::srv::Land::Response> response)
{
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    if (request->wait_on_last_task)
        for (const auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name_ptr_pair.first)->waitOnLastTask();
    else
        for (const auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_)
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name_ptr_pair.first);

    return true; //todo
}

bool AirsimROSWrapper::set_altitude_srv_cb(std::shared_ptr<airsim_interfaces::srv::SetAltitude::Request> request, std::shared_ptr<airsim_interfaces::srv::SetAltitude::Response> response, const std::string& vehicle_name)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    try {
        // Default velocity if not specified or invalid
        float velocity = (request->velocity > 0) ? request->velocity : 5.0f;
        
        RCLCPP_INFO(nh_->get_logger(), "Setting altitude for vehicle '%s' to z=%.2f with velocity=%.2f", 
                    vehicle_name.c_str(), request->z, velocity);
        
        bool success;
        if (request->wait_on_last_task) {
            success = static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveToZAsync(
                request->z, velocity, 30.0f, msr::airlib::YawMode(), -1, 1, vehicle_name)->waitOnLastTask();
        } else {
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveToZAsync(
                request->z, velocity, 30.0f, msr::airlib::YawMode(), -1, 1, vehicle_name);
            success = true; // For async calls, we assume success unless there's an immediate error
        }
        
        response->success = success;
        response->message = success ? "Altitude change command sent successfully" : "Failed to execute altitude change";
        
        RCLCPP_INFO(nh_->get_logger(), "Altitude service response: %s", response->message.c_str());
        
    } catch (const std::exception& e) {
        response->success = false;
        response->message = std::string("Error setting altitude: ") + e.what();
        RCLCPP_ERROR(nh_->get_logger(), "Exception in set_altitude_srv_cb: %s", e.what());
    }

    return true;
}

bool AirsimROSWrapper::set_local_position_srv_cb(std::shared_ptr<airsim_interfaces::srv::SetLocalPosition::Request> request, std::shared_ptr<airsim_interfaces::srv::SetLocalPosition::Response> response, const std::string& vehicle_name)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    try {
        RCLCPP_INFO(nh_->get_logger(), "Setting local position for vehicle '%s' to (%.2f, %.2f, %.2f) with yaw=%.2f", 
                    vehicle_name.c_str(), request->x, request->y, request->z, request->yaw);
        
        // Create position vector and yaw mode
        msr::airlib::Vector3r position(request->x, request->y, request->z);
        msr::airlib::YawMode yaw_mode;
        yaw_mode.is_rate = false;
        yaw_mode.yaw_or_rate = request->yaw;
        
        bool success;
        if (request->wait_on_last_task) {
            success = static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveToPositionAsync(
                position.x(), position.y(), position.z(), 5.0f, 30.0f, msr::airlib::DrivetrainType::MaxDegreeOfFreedom, 
                yaw_mode, -1, 1, vehicle_name)->waitOnLastTask();
        } else {
            static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveToPositionAsync(
                position.x(), position.y(), position.z(), 5.0f, 30.0f, msr::airlib::DrivetrainType::MaxDegreeOfFreedom,
                yaw_mode, -1, 1, vehicle_name);
            success = true; // For async calls, we assume success unless there's an immediate error
        }
        
        response->success = success;
        response->message = success ? "Local position command sent successfully" : "Failed to execute local position command";
        
        RCLCPP_INFO(nh_->get_logger(), "Local position service response: %s", response->message.c_str());
        
    } catch (const std::exception& e) {
        response->success = false;
        response->message = std::string("Error setting local position: ") + e.what();
        RCLCPP_ERROR(nh_->get_logger(), "Exception in set_local_position_srv_cb: %s", e.what());
    }

    return true;
}

// todo add reset by vehicle_name API to airlib
// todo not async remove wait_on_last_task
bool AirsimROSWrapper::reset_srv_cb(std::shared_ptr<airsim_interfaces::srv::Reset::Request> request, std::shared_ptr<airsim_interfaces::srv::Reset::Response> response)
{
    unused(request);
    unused(response);
    std::lock_guard<std::mutex> guard(control_mutex_);

    airsim_client_->reset();
    return true; //todo
}

bool AirsimROSWrapper::list_scene_object_tags_srv_cb(const std::shared_ptr<airsim_interfaces::srv::ListSceneObjectTags::Request> request, const std::shared_ptr<airsim_interfaces::srv::ListSceneObjectTags::Response> response)
{
    std::lock_guard<std::mutex> guard(control_mutex_);
    std::string regex_name = request->regex_name.empty() ? ".*": request->regex_name;
    std::vector<std::pair<std::string, std::string>> first_tag = airsim_client_->simListSceneObjectsTags(regex_name);

    for(const auto& pair: first_tag)
    {
        response->objects.push_back(pair.first);
        response->tags.push_back(pair.second);
    }
    return true;
}

bool AirsimROSWrapper::coordinated_height_and_land_srv_cb(std::shared_ptr<airsim_interfaces::srv::CoordinatedHeightAndLand::Request> request, std::shared_ptr<airsim_interfaces::srv::CoordinatedHeightAndLand::Response> response)
{
    std::lock_guard<std::mutex> guard(control_mutex_);
    
    auto start_time = std::chrono::steady_clock::now();
    
    try {
        // Determine which vehicles to command
        std::vector<std::string> target_vehicles;
        if (request->vehicle_names.empty()) {
            // Use all available vehicles
            for (const auto& vehicle_pair : vehicle_name_ptr_map_) {
                target_vehicles.push_back(vehicle_pair.first);
            }
        } else {
            // Use specified vehicles
            target_vehicles = request->vehicle_names;
        }
        
        if (target_vehicles.empty()) {
            response->success = false;
            response->message = "No vehicles available for coordinated operation";
            response->total_time = 0.0;
            response->vehicles_completed = 0;
            response->vehicles_attempted = 0;
            return true;
        }
        
        RCLCPP_INFO(nh_->get_logger(), "Starting coordinated height and land operation for %lu vehicles to height %.2f", 
                    target_vehicles.size(), request->target_height);
        
        response->vehicles_attempted = static_cast<int32_t>(target_vehicles.size());
        std::vector<std::string> failed_vehicles;
        
        // Default values
        float ascent_speed = (request->ascent_speed > 0) ? request->ascent_speed : 2.0f;
        float hover_time = (request->hover_time >= 0) ? request->hover_time : 2.0f;
        
        // Phase 1: Send all vehicles to target height simultaneously
        RCLCPP_INFO(nh_->get_logger(), "Phase 1: Moving all vehicles to target height %.2f", request->target_height);
        
        std::vector<std::future<bool>> height_futures;
        for (const auto& vehicle_name : target_vehicles) {
            try {
                auto future = static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveToZAsync(
                    request->target_height, ascent_speed, 30.0f, msr::airlib::YawMode(), -1, 1, vehicle_name);
                height_futures.push_back(std::move(future));
            } catch (const std::exception& e) {
                RCLCPP_ERROR(nh_->get_logger(), "Failed to send height command to vehicle %s: %s", vehicle_name.c_str(), e.what());
                failed_vehicles.push_back(vehicle_name);
            }
        }
        
        // Wait for all height commands to complete if requested
        if (request->wait_on_last_task) {
            for (size_t i = 0; i < height_futures.size(); ++i) {
                try {
                    bool success = height_futures[i].waitOnLastTask();
                    if (!success) {
                        RCLCPP_WARN(nh_->get_logger(), "Vehicle %s failed to reach target height", target_vehicles[i].c_str());
                        failed_vehicles.push_back(target_vehicles[i]);
                    }
                } catch (const std::exception& e) {
                    RCLCPP_ERROR(nh_->get_logger(), "Exception waiting for vehicle %s height: %s", target_vehicles[i].c_str(), e.what());
                    failed_vehicles.push_back(target_vehicles[i]);
                }
            }
        }
        
        // Phase 2: Hover at target height
        if (hover_time > 0) {
            RCLCPP_INFO(nh_->get_logger(), "Phase 2: Hovering for %.1f seconds", hover_time);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(hover_time * 1000)));
        }
        
        // Phase 3: Land all vehicles simultaneously
        RCLCPP_INFO(nh_->get_logger(), "Phase 3: Landing all vehicles");
        
        std::vector<std::future<bool>> land_futures;
        for (const auto& vehicle_name : target_vehicles) {
            // Skip vehicles that failed height command
            if (std::find(failed_vehicles.begin(), failed_vehicles.end(), vehicle_name) != failed_vehicles.end()) {
                continue;
            }
            
            try {
                auto future = static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->landAsync(60, vehicle_name);
                land_futures.push_back(std::move(future));
            } catch (const std::exception& e) {
                RCLCPP_ERROR(nh_->get_logger(), "Failed to send land command to vehicle %s: %s", vehicle_name.c_str(), e.what());
                failed_vehicles.push_back(vehicle_name);
            }
        }
        
        // Wait for all landing commands to complete if requested
        if (request->wait_on_last_task) {
            size_t land_index = 0;
            for (const auto& vehicle_name : target_vehicles) {
                // Skip vehicles that failed height command
                if (std::find(failed_vehicles.begin(), failed_vehicles.end(), vehicle_name) != failed_vehicles.end()) {
                    continue;
                }
                
                try {
                    bool success = land_futures[land_index].waitOnLastTask();
                    if (!success) {
                        RCLCPP_WARN(nh_->get_logger(), "Vehicle %s failed to land", vehicle_name.c_str());
                        failed_vehicles.push_back(vehicle_name);
                    }
                    land_index++;
                } catch (const std::exception& e) {
                    RCLCPP_ERROR(nh_->get_logger(), "Exception waiting for vehicle %s landing: %s", vehicle_name.c_str(), e.what());
                    failed_vehicles.push_back(vehicle_name);
                    land_index++;
                }
            }
        }
        
        // Calculate results
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        response->total_time = duration.count() / 1000.0;
        response->vehicles_completed = response->vehicles_attempted - static_cast<int32_t>(failed_vehicles.size());
        response->failed_vehicles = failed_vehicles;
        response->success = failed_vehicles.empty();
        
        if (response->success) {
            response->message = "All vehicles successfully completed coordinated height and land operation";
            RCLCPP_INFO(nh_->get_logger(), "Coordinated operation completed successfully in %.2f seconds", response->total_time);
        } else {
            response->message = "Coordinated operation completed with failures on " + std::to_string(failed_vehicles.size()) + " vehicles";
            RCLCPP_WARN(nh_->get_logger(), "Coordinated operation completed with %lu failures in %.2f seconds", failed_vehicles.size(), response->total_time);
        }
        
    } catch (const std::exception& e) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        response->success = false;
        response->message = std::string("Coordinated operation failed: ") + e.what();
        response->total_time = duration.count() / 1000.0;
        response->vehicles_completed = 0;
        RCLCPP_ERROR(nh_->get_logger(), "Exception in coordinated_height_and_land_srv_cb: %s", e.what());
    }

    return true;
}

tf2::Quaternion AirsimROSWrapper::get_tf2_quat(const msr::airlib::Quaternionr& airlib_quat) const
{
    return tf2::Quaternion(airlib_quat.x(), airlib_quat.y(), airlib_quat.z(), airlib_quat.w());
}

msr::airlib::Quaternionr AirsimROSWrapper::get_airlib_quat(const geometry_msgs::msg::Quaternion& geometry_msgs_quat) const
{
    return msr::airlib::Quaternionr(geometry_msgs_quat.w, geometry_msgs_quat.x, geometry_msgs_quat.y, geometry_msgs_quat.z);
}

msr::airlib::Quaternionr AirsimROSWrapper::get_airlib_quat(const tf2::Quaternion& tf2_quat) const
{
    return msr::airlib::Quaternionr(tf2_quat.w(), tf2_quat.x(), tf2_quat.y(), tf2_quat.z());
}

void AirsimROSWrapper::car_cmd_cb(const airsim_interfaces::msg::CarControls::SharedPtr msg, const std::string& vehicle_name)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    auto car = static_cast<CarROS*>(vehicle_name_ptr_map_[vehicle_name].get());
    car->car_cmd_.throttle = msg->throttle;
    car->car_cmd_.steering = msg->steering;
    car->car_cmd_.brake = msg->brake;
    car->car_cmd_.handbrake = msg->handbrake;
    car->car_cmd_.is_manual_gear = msg->manual;
    car->car_cmd_.manual_gear = msg->manual_gear;
    car->car_cmd_.gear_immediate = msg->gear_immediate;

    car->has_car_cmd_ = true;
}

msr::airlib::Pose AirsimROSWrapper::get_airlib_pose(const float& x, const float& y, const float& z, const msr::airlib::Quaternionr& airlib_quat) const
{
    return msr::airlib::Pose(msr::airlib::Vector3r(x, y, z), airlib_quat);
}

void AirsimROSWrapper::vel_cmd_body_frame_cb(const airsim_interfaces::msg::VelCmd::SharedPtr msg, const std::string& vehicle_name)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_map_[vehicle_name].get());
    drone->vel_cmd_ = get_airlib_body_vel_cmd(*msg, drone->curr_drone_state_.kinematics_estimated.pose.orientation);
    drone->has_vel_cmd_ = true;
}

void AirsimROSWrapper::vel_cmd_group_body_frame_cb(const airsim_interfaces::msg::VelCmdGroup::SharedPtr msg)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    for (const auto& vehicle_name : msg->vehicle_names) {
        auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_map_[vehicle_name].get());
        drone->vel_cmd_ = get_airlib_body_vel_cmd(msg->vel_cmd, drone->curr_drone_state_.kinematics_estimated.pose.orientation);
        drone->has_vel_cmd_ = true;
    }
}

void AirsimROSWrapper::vel_cmd_all_body_frame_cb(const airsim_interfaces::msg::VelCmd::SharedPtr msg)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    // todo expose wait_on_last_task or nah?
    for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
        auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_pair.second.get());
        drone->vel_cmd_ = get_airlib_body_vel_cmd(*msg, drone->curr_drone_state_.kinematics_estimated.pose.orientation);
        drone->has_vel_cmd_ = true;
    }
}

void AirsimROSWrapper::vel_cmd_world_frame_cb(const airsim_interfaces::msg::VelCmd::SharedPtr msg, const std::string& vehicle_name)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_map_[vehicle_name].get());
    drone->vel_cmd_ = get_airlib_world_vel_cmd(*msg);
    drone->has_vel_cmd_ = true;
}

// this is kinda unnecessary but maybe it makes life easier for the end user.
void AirsimROSWrapper::vel_cmd_group_world_frame_cb(const airsim_interfaces::msg::VelCmdGroup::SharedPtr msg)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    for (const auto& vehicle_name : msg->vehicle_names) {
        auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_map_[vehicle_name].get());
        drone->vel_cmd_ = get_airlib_world_vel_cmd(msg->vel_cmd);
        drone->has_vel_cmd_ = true;
    }
}

void AirsimROSWrapper::vel_cmd_all_world_frame_cb(const airsim_interfaces::msg::VelCmd::SharedPtr msg)
{
    std::lock_guard<std::mutex> guard(control_mutex_);

    // todo expose wait_on_last_task or nah?
    for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
        auto drone = static_cast<MultiRotorROS*>(vehicle_name_ptr_pair.second.get());
        drone->vel_cmd_ = get_airlib_world_vel_cmd(*msg);
        drone->has_vel_cmd_ = true;
    }
}

// todo support multiple gimbal commands
void AirsimROSWrapper::gimbal_angle_quat_cmd_cb(const airsim_interfaces::msg::GimbalAngleQuatCmd::SharedPtr gimbal_angle_quat_cmd_msg)
{
    tf2::Quaternion quat_control_cmd;
    try {
        tf2::convert(gimbal_angle_quat_cmd_msg->orientation, quat_control_cmd);
        quat_control_cmd.normalize();
        gimbal_cmd_.target_quat = get_airlib_quat(quat_control_cmd); // airsim uses wxyz
        gimbal_cmd_.camera_name = gimbal_angle_quat_cmd_msg->camera_name;
        gimbal_cmd_.vehicle_name = gimbal_angle_quat_cmd_msg->vehicle_name;
        has_gimbal_cmd_ = true;
    }
    catch (tf2::TransformException& ex) {
        RCLCPP_WARN(nh_->get_logger(), "%s", ex.what());
    }
}

// todo support multiple gimbal commands
// 1. find quaternion of default gimbal pose
// 2. forward multiply with quaternion equivalent to desired euler commands (in degrees)
// 3. call airsim client's setCameraPose which sets camera pose wrt world (or takeoff?) ned frame. todo
void AirsimROSWrapper::gimbal_angle_euler_cmd_cb(const airsim_interfaces::msg::GimbalAngleEulerCmd::SharedPtr gimbal_angle_euler_cmd_msg)
{
    try {
        tf2::Quaternion quat_control_cmd;
        quat_control_cmd.setRPY(math_common::deg2rad(gimbal_angle_euler_cmd_msg->roll), math_common::deg2rad(gimbal_angle_euler_cmd_msg->pitch), math_common::deg2rad(gimbal_angle_euler_cmd_msg->yaw));
        quat_control_cmd.normalize();
        gimbal_cmd_.target_quat = get_airlib_quat(quat_control_cmd);
        gimbal_cmd_.camera_name = gimbal_angle_euler_cmd_msg->camera_name;
        gimbal_cmd_.vehicle_name = gimbal_angle_euler_cmd_msg->vehicle_name;
        has_gimbal_cmd_ = true;
    }
    catch (tf2::TransformException& ex) {
        RCLCPP_WARN(nh_->get_logger(), "%s", ex.what());
    }
}

airsim_interfaces::msg::CarState AirsimROSWrapper::get_roscarstate_msg_from_car_state(const msr::airlib::CarApiBase::CarState& car_state) const
{
    airsim_interfaces::msg::CarState state_msg;
    const auto odo = get_odom_msg_from_car_state(car_state);

    state_msg.pose = odo.pose;
    state_msg.twist = odo.twist;
    state_msg.speed = car_state.speed;
    state_msg.gear = car_state.gear;
    state_msg.rpm = car_state.rpm;
    state_msg.maxrpm = car_state.maxrpm;
    state_msg.handbrake = car_state.handbrake;
    state_msg.header.stamp = rclcpp::Time(car_state.timestamp);

    return state_msg;
}

airsim_interfaces::msg::ComputerVisionState AirsimROSWrapper::get_roscomputervisionstate_msg_from_computer_vision_state(const msr::airlib::ComputerVisionApiBase::ComputerVisionState& computer_vision_state) const
{
    airsim_interfaces::msg::ComputerVisionState state_msg;
    const auto odo = get_odom_msg_from_computer_vision_state(computer_vision_state);

    state_msg.pose = odo.pose;
    state_msg.twist = odo.twist;    
    state_msg.header.stamp = rclcpp::Time(computer_vision_state.timestamp);

    return state_msg;
}


nav_msgs::msg::Odometry AirsimROSWrapper::get_odom_msg_from_kinematic_state(const msr::airlib::Kinematics::State& kinematics_estimated) const
{
    nav_msgs::msg::Odometry odom_msg;
   
    odom_msg.pose.pose.position.x = kinematics_estimated.pose.position.x();
    odom_msg.pose.pose.position.y = kinematics_estimated.pose.position.y();
    odom_msg.pose.pose.position.z = kinematics_estimated.pose.position.z();
    odom_msg.pose.pose.orientation.x = kinematics_estimated.pose.orientation.x();
    odom_msg.pose.pose.orientation.y = kinematics_estimated.pose.orientation.y();
    odom_msg.pose.pose.orientation.z = kinematics_estimated.pose.orientation.z();
    odom_msg.pose.pose.orientation.w = kinematics_estimated.pose.orientation.w();

    odom_msg.twist.twist.linear.x = kinematics_estimated.twist.linear.x();
    odom_msg.twist.twist.linear.y = kinematics_estimated.twist.linear.y();
    odom_msg.twist.twist.linear.z = kinematics_estimated.twist.linear.z();
    odom_msg.twist.twist.angular.x = kinematics_estimated.twist.angular.x();
    odom_msg.twist.twist.angular.y = kinematics_estimated.twist.angular.y();
    odom_msg.twist.twist.angular.z = kinematics_estimated.twist.angular.z();

    odom_msg.pose.pose.position.y = -odom_msg.pose.pose.position.y;
    odom_msg.pose.pose.position.z = -odom_msg.pose.pose.position.z;
    odom_msg.pose.pose.orientation.y = -odom_msg.pose.pose.orientation.y;
    odom_msg.pose.pose.orientation.z = -odom_msg.pose.pose.orientation.z;
    odom_msg.twist.twist.linear.y = -odom_msg.twist.twist.linear.y;
    odom_msg.twist.twist.linear.z = -odom_msg.twist.twist.linear.z;
    odom_msg.twist.twist.angular.y = -odom_msg.twist.twist.angular.y;
    odom_msg.twist.twist.angular.z = -odom_msg.twist.twist.angular.z;   
    
    return odom_msg;
}

nav_msgs::msg::Odometry AirsimROSWrapper::get_odom_msg_from_car_state(const msr::airlib::CarApiBase::CarState& car_state) const
{
    return get_odom_msg_from_kinematic_state(car_state.kinematics_estimated);
}

nav_msgs::msg::Odometry AirsimROSWrapper::get_odom_msg_from_computer_vision_state(const msr::airlib::ComputerVisionApiBase::ComputerVisionState& computer_vision_state) const
{
    return get_odom_msg_from_kinematic_state(computer_vision_state.kinematics_estimated);
}

nav_msgs::msg::Odometry AirsimROSWrapper::get_odom_msg_from_multirotor_state(const msr::airlib::MultirotorState& drone_state) const
{
    return get_odom_msg_from_kinematic_state(drone_state.kinematics_estimated);
}

void fixPointCloud(std::vector<float>& data, int offset, std::vector<int> flip_indexes) {
    for (size_t i = 1; i < data.size(); i += offset) {
        data[i] = -data[i];
        for (int flip_index : flip_indexes) {
            if (i + flip_index < data.size()) {
                data[i + flip_index] = -data[i + flip_index];
            }
        }
    }
}

// https://docs.ros.org/jade/api/sensor_msgs/html/point__cloud__conversion_8h_source.html#l00066
// look at UnrealLidarSensor.cpp UnrealLidarSensor::getPointCloud() for math
// read this carefully https://docs.ros.org/kinetic/api/sensor_msgs/html/msg/PointCloud2.html
sensor_msgs::msg::PointCloud2 AirsimROSWrapper::get_lidar_msg_from_airsim(const msr::airlib::LidarData& lidar_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    sensor_msgs::msg::PointCloud2 lidar_msg;
    lidar_msg.header.stamp = rclcpp::Time(lidar_data.time_stamp);
    lidar_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (lidar_data.point_cloud.size() > 3) {
        lidar_msg.height = 1;
        lidar_msg.width = lidar_data.point_cloud.size() / 3;

        lidar_msg.fields.resize(3);
        lidar_msg.fields[0].name = "x";
        lidar_msg.fields[1].name = "y";
        lidar_msg.fields[2].name = "z";

        int offset = 0;

        for (size_t d = 0; d < lidar_msg.fields.size(); ++d, offset += 4) {
            lidar_msg.fields[d].offset = offset;
            lidar_msg.fields[d].datatype = sensor_msgs::msg::PointField::FLOAT32;
            lidar_msg.fields[d].count = 1;
        }

        lidar_msg.is_bigendian = false;
        lidar_msg.point_step = offset; // 4 * num fields
        lidar_msg.row_step = lidar_msg.point_step * lidar_msg.width;

        lidar_msg.is_dense = true; // todo
        std::vector<float> data_std = lidar_data.point_cloud;
        fixPointCloud(data_std, 3, {1});

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data_std.data());
        std::vector<unsigned char> lidar_msg_data(bytes, bytes + sizeof(float) * data_std.size());
        lidar_msg.data = std::move(lidar_msg_data);   
    }
    else {
        // msg = []
    }

    return lidar_msg;
}

airsim_interfaces::msg::StringArray AirsimROSWrapper::get_lidar_labels_msg_from_airsim(const msr::airlib::LidarData& lidar_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    airsim_interfaces::msg::StringArray lidar_labels_msg;
    lidar_labels_msg.header.stamp = rclcpp::Time(lidar_data.time_stamp);
    lidar_labels_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (lidar_data.point_cloud.size() > 3) {
        lidar_labels_msg.data = std::move(lidar_data.groundtruth);           
    }
    else {
        // msg = []
    }

    return lidar_labels_msg;
}

sensor_msgs::msg::PointCloud2 AirsimROSWrapper::get_gpulidar_msg_from_airsim(const msr::airlib::GPULidarData& gpulidar_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    sensor_msgs::msg::PointCloud2 gpulidar_msg;    
    gpulidar_msg.header.stamp = rclcpp::Time(gpulidar_data.time_stamp);
    gpulidar_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (gpulidar_data.point_cloud.size() > 5) {    
        
        std::vector<float> data_std = gpulidar_data.point_cloud;
        fixPointCloud(data_std, 5, {1});

        size_t num_points = data_std.size() / 5;
        pcl::PointCloud<PointXYZRGBI> cloud;   
        cloud.points.resize(num_points); 
        cloud.width = static_cast<uint32_t>(num_points);
        cloud.height = 1; 
        cloud.is_dense = true;

        for (size_t i = 0; i < num_points; ++i)
        {
            // Extract x, y, z, and rgb values
            float x = data_std[i * 5 + 0];
            float y = data_std[i * 5 + 1];
            float z = data_std[i * 5 + 2];
            float rgb_packed = data_std[i * 5 + 3];
            float intensity = data_std[i * 5 + 4];
            
            // Unpack the RGB value
            uint8_t r = ((std::uint32_t)rgb_packed >> 16) & 0x0000ff;
            uint8_t g = ((std::uint32_t)rgb_packed >> 8) & 0x0000ff;
            uint8_t b = ((std::uint32_t)rgb_packed) & 0x0000ff;

            // Populate the point
            PointXYZRGBI point;
            point.x = x;
            point.y = y;
            point.z = z;
            std::uint32_t rgb = ((std::uint32_t)r << 16 | (std::uint32_t)g << 8 | (std::uint32_t)b);
            point.rgb = *reinterpret_cast<float*>(&rgb);
            point.intensity = intensity;
            cloud.points[i] = point;
        }

        pcl::toROSMsg(cloud, gpulidar_msg);
        gpulidar_msg.header.stamp = rclcpp::Time(gpulidar_data.time_stamp);
        gpulidar_msg.header.frame_id = vehicle_name + "/" + sensor_name;
    }
    else {
        // msg = []
    }

    return gpulidar_msg;
}

sensor_msgs::msg::PointCloud2 AirsimROSWrapper::get_active_echo_msg_from_airsim(const msr::airlib::EchoData& echo_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    sensor_msgs::msg::PointCloud2 echo_msg;
    echo_msg.header.stamp = rclcpp::Time(echo_data.time_stamp);
    echo_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (echo_data.point_cloud.size() > 6) {
        echo_msg.height = 1;
        echo_msg.width = echo_data.point_cloud.size() / 6;

        echo_msg.fields.resize(6);
        echo_msg.fields[0].name = "x";
        echo_msg.fields[1].name = "y";
        echo_msg.fields[2].name = "z";
        echo_msg.fields[3].name = "a";
        echo_msg.fields[4].name = "d";
        echo_msg.fields[5].name = "r";


        int offset = 0;
        for (size_t d = 0; d < echo_msg.fields.size(); ++d, offset += 4) {
            echo_msg.fields[d].offset = offset;
            echo_msg.fields[d].datatype = sensor_msgs::msg::PointField::FLOAT32;
            echo_msg.fields[d].count = 1;
        }

        echo_msg.is_bigendian = false;
        echo_msg.point_step = offset;
        echo_msg.row_step = echo_msg.point_step * echo_msg.width;

        echo_msg.is_dense = true; 
        std::vector<float> data_std = echo_data.point_cloud;
        fixPointCloud(data_std, 6, {1});

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data_std.data());
        std::vector<unsigned char> echo_msg_data(bytes, bytes + sizeof(float) * data_std.size());
        echo_msg.data = std::move(echo_msg_data);   
    }
    else {
        // msg = []
    }

    return echo_msg;
}

airsim_interfaces::msg::StringArray AirsimROSWrapper::get_active_echo_labels_msg_from_airsim(const msr::airlib::EchoData& echo_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    airsim_interfaces::msg::StringArray echo_active_labels_msg;
    echo_active_labels_msg.header.stamp = rclcpp::Time(echo_data.time_stamp);
    echo_active_labels_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (echo_data.point_cloud.size() > 6) {
        echo_active_labels_msg.data = std::move(echo_data.groundtruth);           
    }
    else {
        // msg = []
    }

    return echo_active_labels_msg;
}

sensor_msgs::msg::PointCloud2 AirsimROSWrapper::get_passive_echo_msg_from_airsim(const msr::airlib::EchoData& echo_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    sensor_msgs::msg::PointCloud2 echo_msg;
    echo_msg.header.stamp = rclcpp::Time(echo_data.time_stamp);
    echo_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (echo_data.passive_beacons_point_cloud.size() > 9) {
        echo_msg.height = 1;
        echo_msg.width = echo_data.passive_beacons_point_cloud.size() / 9;

        echo_msg.fields.resize(9);
        echo_msg.fields[0].name = "x";
        echo_msg.fields[1].name = "y";
        echo_msg.fields[2].name = "z";
        echo_msg.fields[3].name = "a";
        echo_msg.fields[4].name = "d";
        echo_msg.fields[5].name = "r";
        echo_msg.fields[6].name = "xd";
        echo_msg.fields[7].name = "yd";
        echo_msg.fields[8].name = "zd";


        int offset = 0;
        for (size_t d = 0; d < echo_msg.fields.size(); ++d, offset += 4) {
            echo_msg.fields[d].offset = offset;
            echo_msg.fields[d].datatype = sensor_msgs::msg::PointField::FLOAT32;
            echo_msg.fields[d].count = 1;
        }

        echo_msg.is_bigendian = false;
        echo_msg.point_step = offset;
        echo_msg.row_step = echo_msg.point_step * echo_msg.width;

        echo_msg.is_dense = true; 
        std::vector<float> data_std = echo_data.passive_beacons_point_cloud;
        fixPointCloud(data_std, 9, {1, 6, 7});

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data_std.data());
        std::vector<unsigned char> echo_msg_data(bytes, bytes + sizeof(float) * data_std.size());
        echo_msg.data = std::move(echo_msg_data);   
    }
    else {
        // msg = []
    }

    return echo_msg;
}

airsim_interfaces::msg::StringArray AirsimROSWrapper::get_passive_echo_labels_msg_from_airsim(const msr::airlib::EchoData& echo_data, const std::string& vehicle_name, const std::string& sensor_name) const
{
    airsim_interfaces::msg::StringArray echo_passive_labels_msg;
    echo_passive_labels_msg.header.stamp = rclcpp::Time(echo_data.time_stamp);
    echo_passive_labels_msg.header.frame_id = vehicle_name + "/" + sensor_name;

    if (echo_data.point_cloud.size() > 9) {
        echo_passive_labels_msg.data = std::move(echo_data.passive_beacons_groundtruth);           
    }
    else {
        // msg = []
    }

    return echo_passive_labels_msg;
}

airsim_interfaces::msg::Environment AirsimROSWrapper::get_environment_msg_from_airsim(const msr::airlib::Environment::State& env_data) const
{
    airsim_interfaces::msg::Environment env_msg;
    env_msg.position.x = env_data.position.x();
    env_msg.position.y = env_data.position.y();
    env_msg.position.z = env_data.position.z();
    env_msg.geo_point.latitude = env_data.geo_point.latitude;
    env_msg.geo_point.longitude = env_data.geo_point.longitude;
    env_msg.geo_point.altitude = env_data.geo_point.altitude;
    env_msg.gravity.x = env_data.gravity.x();
    env_msg.gravity.y = env_data.gravity.y();
    env_msg.gravity.z = env_data.gravity.z();
    env_msg.air_pressure = env_data.air_pressure;
    env_msg.temperature = env_data.temperature;
    env_msg.air_density = env_data.temperature;

    return env_msg;
}

sensor_msgs::msg::MagneticField AirsimROSWrapper::get_mag_msg_from_airsim(const msr::airlib::MagnetometerBase::Output& mag_data) const
{
    sensor_msgs::msg::MagneticField mag_msg;
    mag_msg.magnetic_field.x = mag_data.magnetic_field_body.x();
    mag_msg.magnetic_field.y = mag_data.magnetic_field_body.y();
    mag_msg.magnetic_field.z = mag_data.magnetic_field_body.z();
    std::copy(std::begin(mag_data.magnetic_field_covariance),
              std::end(mag_data.magnetic_field_covariance),
              std::begin(mag_msg.magnetic_field_covariance));
    mag_msg.header.stamp = rclcpp::Time(mag_data.time_stamp);

    return mag_msg;
}

// todo covariances
sensor_msgs::msg::NavSatFix AirsimROSWrapper::get_gps_msg_from_airsim(const msr::airlib::GpsBase::Output& gps_data) const
{
    sensor_msgs::msg::NavSatFix gps_msg;
    gps_msg.header.stamp = rclcpp::Time(gps_data.time_stamp);
    gps_msg.latitude = gps_data.gnss.geo_point.latitude;
    gps_msg.longitude = gps_data.gnss.geo_point.longitude;
    gps_msg.altitude = gps_data.gnss.geo_point.altitude;
    gps_msg.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GLONASS;
    gps_msg.status.status = gps_data.gnss.fix_type;
    // gps_msg.position_covariance_type =
    // gps_msg.position_covariance =

    return gps_msg;
}

sensor_msgs::msg::Range AirsimROSWrapper::get_range_from_airsim(const msr::airlib::DistanceSensorData& dist_data) const
{
    sensor_msgs::msg::Range dist_msg;
    dist_msg.header.stamp = rclcpp::Time(dist_data.time_stamp);
    dist_msg.range = dist_data.distance;
    dist_msg.min_range = dist_data.min_distance;
    dist_msg.max_range = dist_data.max_distance;

    return dist_msg;
}


airsim_interfaces::msg::InstanceSegmentationList AirsimROSWrapper::get_instance_segmentation_list_msg_from_airsim() const{
    airsim_interfaces::msg::InstanceSegmentationList instance_segmentation_list_msg;
    std::vector<std::string> object_name_list = airsim_client_->simListInstanceSegmentationObjects();
    std::vector<msr::airlib::Vector3r> color_map = airsim_client_->simGetInstanceSegmentationColorMap();
    int object_index = 0;
    std::vector<std::string>::iterator it = object_name_list.begin();
    for(; it < object_name_list.end(); it++, object_index++ )
    {
        airsim_interfaces::msg::InstanceSegmentationLabel instance_segmentation_label_msg;
        instance_segmentation_label_msg.name = *it;
        instance_segmentation_label_msg.r = color_map[object_index].x();
        instance_segmentation_label_msg.g = color_map[object_index].y();
        instance_segmentation_label_msg.b = color_map[object_index].z();
        instance_segmentation_label_msg.index = object_index;
        instance_segmentation_list_msg.labels.push_back(instance_segmentation_label_msg);
    }
    return instance_segmentation_list_msg;
}

airsim_interfaces::msg::ObjectTransformsList AirsimROSWrapper::get_object_transforms_list_msg_from_airsim(rclcpp::Time timestamp) const{
    airsim_interfaces::msg::ObjectTransformsList object_transforms_list_msg;
    std::vector<std::string> object_name_list = airsim_client_->simListInstanceSegmentationObjects();
    std::vector<msr::airlib::Pose> poses = airsim_client_->simListInstanceSegmentationPoses();
    int object_index = 0;
    std::vector<std::string>::iterator it = object_name_list.begin();
    for(; it < object_name_list.end(); it++, object_index++ )
    {
        msr::airlib::Pose cur_object_pose = poses[object_index]; 
        if(!std::isnan(cur_object_pose.position.x())){
            geometry_msgs::msg::TransformStamped object_transform_msg;
            object_transform_msg.child_frame_id = *it;
            object_transform_msg.transform.translation.x = cur_object_pose.position.x();
            object_transform_msg.transform.translation.y = -cur_object_pose.position.y();
            object_transform_msg.transform.translation.z = -cur_object_pose.position.z();
            object_transform_msg.transform.rotation.x = cur_object_pose.orientation.inverse().x();
            object_transform_msg.transform.rotation.y = cur_object_pose.orientation.inverse().y();
            object_transform_msg.transform.rotation.z = cur_object_pose.orientation.inverse().z();
            object_transform_msg.transform.rotation.w = cur_object_pose.orientation.inverse().w();
            object_transform_msg.header.stamp = timestamp;
            object_transforms_list_msg.objects.push_back(object_transform_msg);            
        }
    }
    object_transforms_list_msg.header.stamp = timestamp;
    object_transforms_list_msg.header.frame_id = world_frame_id_;
    return object_transforms_list_msg;
}

airsim_interfaces::msg::Altimeter AirsimROSWrapper::get_altimeter_msg_from_airsim(const msr::airlib::BarometerBase::Output& alt_data) const
{
    airsim_interfaces::msg::Altimeter alt_msg;
    alt_msg.header.stamp = rclcpp::Time(alt_data.time_stamp);
    alt_msg.altitude = alt_data.altitude;
    alt_msg.pressure = alt_data.pressure;
    alt_msg.qnh = alt_data.qnh;

    return alt_msg;
}

// todo covariances
sensor_msgs::msg::Imu AirsimROSWrapper::get_imu_msg_from_airsim(const msr::airlib::ImuBase::Output& imu_data) const
{
    sensor_msgs::msg::Imu imu_msg;
    
    // imu_msg.header.frame_id = "/airsim/odom_local_ned";// todo multiple drones
    imu_msg.header.stamp = rclcpp::Time(imu_data.time_stamp);
    imu_msg.orientation.x = imu_data.orientation.inverse().x();
    imu_msg.orientation.y = imu_data.orientation.inverse().y();
    imu_msg.orientation.z = imu_data.orientation.inverse().z();
    imu_msg.orientation.w = imu_data.orientation.inverse().w();

    // todo radians per second
    imu_msg.angular_velocity.x = imu_data.angular_velocity.x();
    imu_msg.angular_velocity.y = -imu_data.angular_velocity.y();
    imu_msg.angular_velocity.z = -imu_data.angular_velocity.z();

    // meters/s2^m
    imu_msg.linear_acceleration.x = imu_data.linear_acceleration.x();
    imu_msg.linear_acceleration.y = -imu_data.linear_acceleration.y();
    imu_msg.linear_acceleration.z = -imu_data.linear_acceleration.z();


    return imu_msg;
}

void AirsimROSWrapper::publish_odom_tf(const nav_msgs::msg::Odometry& odom_msg)
{
    geometry_msgs::msg::TransformStamped odom_tf;
    odom_tf.header = odom_msg.header;
    odom_tf.child_frame_id = odom_msg.child_frame_id;
    odom_tf.transform.translation.x = odom_msg.pose.pose.position.x;
    odom_tf.transform.translation.y = odom_msg.pose.pose.position.y;
    odom_tf.transform.translation.z = odom_msg.pose.pose.position.z;
    odom_tf.transform.rotation = odom_msg.pose.pose.orientation;
    tf_broadcaster_->sendTransform(odom_tf);
}

airsim_interfaces::msg::GPSYaw AirsimROSWrapper::get_gps_msg_from_airsim_geo_point(const msr::airlib::GeoPoint& geo_point) const
{
    airsim_interfaces::msg::GPSYaw gps_msg;
    gps_msg.latitude = geo_point.latitude;
    gps_msg.longitude = geo_point.longitude;
    gps_msg.altitude = geo_point.altitude;
    return gps_msg;
}

sensor_msgs::msg::NavSatFix AirsimROSWrapper::get_gps_sensor_msg_from_airsim_geo_point(const msr::airlib::GeoPoint& geo_point) const
{
    sensor_msgs::msg::NavSatFix gps_msg;
    gps_msg.latitude = geo_point.latitude;
    gps_msg.longitude = geo_point.longitude;
    gps_msg.altitude = geo_point.altitude;
    return gps_msg;
}

msr::airlib::GeoPoint AirsimROSWrapper::get_origin_geo_point() const
{
    msr::airlib::HomeGeoPoint geo_point = AirSimSettings::singleton().origin_geopoint;
    return geo_point.home_geo_point;
}

VelCmd AirsimROSWrapper::get_airlib_world_vel_cmd(const airsim_interfaces::msg::VelCmd& msg) const
{
    VelCmd vel_cmd;
    vel_cmd.x = msg.twist.linear.x;
    vel_cmd.y = msg.twist.linear.y;
    vel_cmd.z = msg.twist.linear.z;
    vel_cmd.drivetrain = msr::airlib::DrivetrainType::MaxDegreeOfFreedom;
    vel_cmd.yaw_mode.is_rate = true;
    vel_cmd.yaw_mode.yaw_or_rate = math_common::rad2deg(msg.twist.angular.z);
    return vel_cmd;
}

VelCmd AirsimROSWrapper::get_airlib_body_vel_cmd(const airsim_interfaces::msg::VelCmd& msg, const msr::airlib::Quaternionr& airlib_quat) const
{
    VelCmd vel_cmd;
    double roll, pitch, yaw;
    tf2::Matrix3x3(get_tf2_quat(airlib_quat)).getRPY(roll, pitch, yaw); // ros uses xyzw

    // todo do actual body frame?
    vel_cmd.x = (msg.twist.linear.x * cos(yaw)) - (msg.twist.linear.y * sin(yaw)); //body frame assuming zero pitch roll
    vel_cmd.y = (msg.twist.linear.x * sin(yaw)) + (msg.twist.linear.y * cos(yaw)); //body frame
    vel_cmd.z = msg.twist.linear.z;
    vel_cmd.drivetrain = msr::airlib::DrivetrainType::MaxDegreeOfFreedom;
    vel_cmd.yaw_mode.is_rate = true;
    // airsim uses degrees
    vel_cmd.yaw_mode.yaw_or_rate = math_common::rad2deg(msg.twist.angular.z);

    return vel_cmd;
}

geometry_msgs::msg::Transform AirsimROSWrapper::get_transform_msg_from_airsim(const msr::airlib::Vector3r& position, const msr::airlib::AirSimSettings::Rotation& rotation)
{
    geometry_msgs::msg::Transform transform;
    transform.translation.x = position.x();
    transform.translation.y = position.y();
    transform.translation.z = position.z();
    tf2::Quaternion quat;
    quat.setRPY(rotation.roll * (M_PI / 180.0), rotation.pitch * (M_PI / 180.0), rotation.yaw * (M_PI / 180.0));
    transform.rotation.x = quat.x();
    transform.rotation.y = quat.y();
    transform.rotation.z = quat.z();
    transform.rotation.w = quat.w();

    return transform;
}

geometry_msgs::msg::Transform AirsimROSWrapper::get_transform_msg_from_airsim(const msr::airlib::Vector3r& position, const msr::airlib::Quaternionr& quaternion)
{
    geometry_msgs::msg::Transform transform;
    transform.translation.x = position.x();
    transform.translation.y = position.y();
    transform.translation.z = position.z();
    transform.rotation.x = quaternion.x();
    transform.rotation.y = quaternion.y();
    transform.rotation.z = quaternion.z();
    transform.rotation.w = quaternion.w();

    return transform;
}

void AirsimROSWrapper::drone_state_timer_cb()
{
    try {
        // todo this is global origin
        origin_geo_point_pub_->publish(origin_geo_point_msg_);

        // get the basic vehicle pose and environmental state
        const auto now = update_state();

        // on init, will publish 0 to /clock as expected for use_sim_time compatibility
        if (!airsim_client_->simIsPaused()) {
            // airsim_client needs to provide the simulation time in a future version of the API
            ros_clock_.clock = now;
        }
        // publish the simulation clock
        if (publish_clock_) {
            clock_pub_->publish(ros_clock_);
        }

        // publish vehicle state, odom, and all basic sensor types
        publish_vehicle_state();

        // send any commands out to the vehicles
        update_commands();
    }
    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API:\n%s", msg.c_str());
    }
}

void AirsimROSWrapper::update_and_publish_static_transforms(VehicleROS* vehicle_ros)
{
    if (vehicle_ros && !vehicle_ros->static_tf_msg_vec_.empty()) {
        for (auto& static_tf_msg : vehicle_ros->static_tf_msg_vec_) {
            static_tf_msg.header.stamp = vehicle_ros->stamp_;
            static_tf_pub_->sendTransform(static_tf_msg);
        }
    }
}

rclcpp::Time AirsimROSWrapper::update_state()
{
    bool got_sim_time = false;
    rclcpp::Time curr_ros_time = nh_->now();

    //should be easier way to get the sim time through API, something like:
    //msr::airlib::Environment::State env = airsim_client_->simGetGroundTruthEnvironment("");
    //curr_ros_time = rclcpp::Time(env.clock().nowNanos());

    // iterate over drones
    for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
        rclcpp::Time vehicle_time;
        // get drone state from airsim
        auto& vehicle_ros = vehicle_name_ptr_pair.second;

        // vehicle environment, we can get ambient temperature here and other truths
        auto env_data = airsim_client_->simGetGroundTruthEnvironment(vehicle_ros->vehicle_name_);

        if (airsim_mode_ == AIRSIM_MODE::DRONE) {
            auto drone = static_cast<MultiRotorROS*>(vehicle_ros.get());
            auto rpc = static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get());
            drone->curr_drone_state_ = rpc->getMultirotorState(vehicle_ros->vehicle_name_);

            vehicle_time = rclcpp::Time(drone->curr_drone_state_.timestamp);
            if (!got_sim_time) {
                curr_ros_time = vehicle_time;
                got_sim_time = true;
            }

            vehicle_ros->gps_sensor_msg_ = get_gps_sensor_msg_from_airsim_geo_point(drone->curr_drone_state_.gps_location);
            vehicle_ros->gps_sensor_msg_.header.stamp = vehicle_time;

            vehicle_ros->curr_odom_ = get_odom_msg_from_multirotor_state(drone->curr_drone_state_);
        }
        else if(airsim_mode_ == AIRSIM_MODE::CAR) {
            auto car = static_cast<CarROS*>(vehicle_ros.get());
            auto rpc = static_cast<msr::airlib::CarRpcLibClient*>(airsim_client_.get());
            car->curr_car_state_ = rpc->getCarState(vehicle_ros->vehicle_name_);

            vehicle_time = rclcpp::Time(car->curr_car_state_.timestamp);
            if (!got_sim_time) {
                curr_ros_time = vehicle_time;
                got_sim_time = true;
            }

            vehicle_ros->gps_sensor_msg_ = get_gps_sensor_msg_from_airsim_geo_point(env_data.geo_point);
            vehicle_ros->gps_sensor_msg_.header.stamp = vehicle_time;

            vehicle_ros->curr_odom_ = get_odom_msg_from_car_state(car->curr_car_state_);

            airsim_interfaces::msg::CarState state_msg = get_roscarstate_msg_from_car_state(car->curr_car_state_);
            state_msg.header.frame_id = vehicle_ros->vehicle_name_;
            car->car_state_msg_ = state_msg;
        }else{
            auto computer_vision = static_cast<ComputerVisionROS*>(vehicle_ros.get());
            auto rpc = static_cast<msr::airlib::ComputerVisionRpcLibClient*>(airsim_client_.get());
            computer_vision->curr_computer_vision_state_ = rpc->getComputerVisionState(vehicle_ros->vehicle_name_);

            vehicle_time = rclcpp::Time(computer_vision->curr_computer_vision_state_.timestamp);
            if (!got_sim_time) {
                curr_ros_time = vehicle_time;
                got_sim_time = true;
            }

            vehicle_ros->gps_sensor_msg_ = get_gps_sensor_msg_from_airsim_geo_point(env_data.geo_point);
            vehicle_ros->gps_sensor_msg_.header.stamp = vehicle_time;

            vehicle_ros->curr_odom_ = get_odom_msg_from_computer_vision_state(computer_vision->curr_computer_vision_state_);

            airsim_interfaces::msg::ComputerVisionState state_msg = get_roscomputervisionstate_msg_from_computer_vision_state(computer_vision->curr_computer_vision_state_);
            state_msg.header.frame_id = vehicle_ros->vehicle_name_;
            computer_vision->computer_vision_state_msg_ = state_msg;
        }

        vehicle_ros->stamp_ = vehicle_time;

        airsim_interfaces::msg::Environment env_msg = get_environment_msg_from_airsim(env_data);
        env_msg.header.frame_id = vehicle_ros->vehicle_name_;
        env_msg.header.stamp = vehicle_time;
        vehicle_ros->env_msg_ = env_msg;

        // convert airsim drone state to ROS msgs
        vehicle_ros->curr_odom_.header.frame_id = vehicle_ros->vehicle_name_;
        vehicle_ros->curr_odom_.child_frame_id = vehicle_ros->odom_frame_id_;
        vehicle_ros->curr_odom_.header.stamp = vehicle_time;
    }

    return curr_ros_time;
}

void AirsimROSWrapper::publish_vehicle_state()
{
    for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
        auto& vehicle_ros = vehicle_name_ptr_pair.second;

        // simulation environment truth
        vehicle_ros->env_pub_->publish(vehicle_ros->env_msg_);

        if (airsim_mode_ == AIRSIM_MODE::CAR) {
            // dashboard reading from car, RPM, gear, etc
            auto car = static_cast<CarROS*>(vehicle_ros.get());
            car->car_state_pub_->publish(car->car_state_msg_);
        }else if(airsim_mode_ == AIRSIM_MODE::COMPUTERVISION){
            auto computer_vision = static_cast<ComputerVisionROS*>(vehicle_ros.get());
            computer_vision->computer_vision_state_pub_->publish(computer_vision->computer_vision_state_msg_);
        }

        // odom and transforms
        vehicle_ros->odom_local_pub_->publish(vehicle_ros->curr_odom_);
        publish_odom_tf(vehicle_ros->curr_odom_);

        // ground truth GPS position from sim/HITL
        vehicle_ros->global_gps_pub_->publish(vehicle_ros->gps_sensor_msg_);

        for (auto& sensor_publisher : vehicle_ros->barometer_pubs_) {
            auto baro_data = airsim_client_->getBarometerData(sensor_publisher.sensor_name, vehicle_ros->vehicle_name_);
            airsim_interfaces::msg::Altimeter alt_msg = get_altimeter_msg_from_airsim(baro_data);
            alt_msg.header.frame_id = vehicle_ros->vehicle_name_;
            sensor_publisher.publisher->publish(alt_msg);
        }

        for (auto& sensor_publisher : vehicle_ros->imu_pubs_) {
            auto imu_data = airsim_client_->getImuData(sensor_publisher.sensor_name, vehicle_ros->vehicle_name_);
            sensor_msgs::msg::Imu imu_msg = get_imu_msg_from_airsim(imu_data);
            imu_msg.header.frame_id = vehicle_ros->vehicle_name_;
            sensor_publisher.publisher->publish(imu_msg);
        }
        for (auto& sensor_publisher : vehicle_ros->distance_pubs_) {
            auto distance_data = airsim_client_->getDistanceSensorData(sensor_publisher.sensor_name, vehicle_ros->vehicle_name_);
            sensor_msgs::msg::Range dist_msg = get_range_from_airsim(distance_data);
            dist_msg.header.frame_id = vehicle_ros->vehicle_name_;
            sensor_publisher.publisher->publish(dist_msg);
        }
        for (auto& sensor_publisher : vehicle_ros->gps_pubs_) {
            auto gps_data = airsim_client_->getGpsData(sensor_publisher.sensor_name, vehicle_ros->vehicle_name_);
            sensor_msgs::msg::NavSatFix gps_msg = get_gps_msg_from_airsim(gps_data);
            gps_msg.header.frame_id = vehicle_ros->vehicle_name_;
            sensor_publisher.publisher->publish(gps_msg);
        }
        for (auto& sensor_publisher : vehicle_ros->magnetometer_pubs_) {
            auto mag_data = airsim_client_->getMagnetometerData(sensor_publisher.sensor_name, vehicle_ros->vehicle_name_);
            sensor_msgs::msg::MagneticField mag_msg = get_mag_msg_from_airsim(mag_data);
            mag_msg.header.frame_id = vehicle_ros->vehicle_name_;
            sensor_publisher.publisher->publish(mag_msg);
        }

        update_and_publish_static_transforms(vehicle_ros.get());
    }
}

void AirsimROSWrapper::update_commands()
{
    for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
        auto& vehicle_ros = vehicle_name_ptr_pair.second;

        if (airsim_mode_ == AIRSIM_MODE::DRONE) {
            auto drone = static_cast<MultiRotorROS*>(vehicle_ros.get());

            // send control commands from the last callback to airsim
            if (drone->has_vel_cmd_) {
                std::lock_guard<std::mutex> guard(control_mutex_);
                static_cast<msr::airlib::MultirotorRpcLibClient*>(airsim_client_.get())->moveByVelocityAsync(drone->vel_cmd_.x, drone->vel_cmd_.y, drone->vel_cmd_.z, vel_cmd_duration_, msr::airlib::DrivetrainType::MaxDegreeOfFreedom, drone->vel_cmd_.yaw_mode, drone->vehicle_name_);
            }
            drone->has_vel_cmd_ = false;
        }
        else if (airsim_mode_ == AIRSIM_MODE::CAR){
            // send control commands from the last callback to airsim
            auto car = static_cast<CarROS*>(vehicle_ros.get());
            if(enable_api_control_){
                if (car->has_car_cmd_) {
                    std::lock_guard<std::mutex> guard(control_mutex_);
                    static_cast<msr::airlib::CarRpcLibClient*>(airsim_client_.get())->setCarControls(car->car_cmd_, vehicle_ros->vehicle_name_);
                }
            }
            car->has_car_cmd_ = false;
        }
    }

    // Only camera rotation, no translation movement of camera
    if (has_gimbal_cmd_) {
        std::lock_guard<std::mutex> guard(control_mutex_);
        airsim_client_->simSetCameraPose(gimbal_cmd_.camera_name, get_airlib_pose(0, 0, 0, gimbal_cmd_.target_quat), gimbal_cmd_.vehicle_name);
    }

    has_gimbal_cmd_ = false;
}

// airsim uses nans for zeros in settings.json. we set them to zeros here for handling tfs in ROS
void AirsimROSWrapper::set_nans_to_zeros_in_pose(VehicleSetting& vehicle_setting) const
{
    if (std::isnan(vehicle_setting.position.x()))
        vehicle_setting.position.x() = 0.0;

    if (std::isnan(vehicle_setting.position.y()))
        vehicle_setting.position.y() = 0.0;

    if (std::isnan(vehicle_setting.position.z()))
        vehicle_setting.position.z() = 0.0;

    if (std::isnan(vehicle_setting.rotation.yaw))
        vehicle_setting.rotation.yaw = 0.0;

    if (std::isnan(vehicle_setting.rotation.pitch))
        vehicle_setting.rotation.pitch = 0.0;

    if (std::isnan(vehicle_setting.rotation.roll))
        vehicle_setting.rotation.roll = 0.0;
}

// if any nan's in camera pose, set them to match vehicle pose (which has already converted any potential nans to zeros)
void AirsimROSWrapper::set_nans_to_zeros_in_pose(const VehicleSetting& vehicle_setting, CameraSetting& camera_setting) const
{
    if (std::isnan(camera_setting.position.x()))
        camera_setting.position.x() = vehicle_setting.position.x();

    if (std::isnan(camera_setting.position.y()))
        camera_setting.position.y() = vehicle_setting.position.y();

    if (std::isnan(camera_setting.position.z()))
        camera_setting.position.z() = vehicle_setting.position.z();

    if (std::isnan(camera_setting.rotation.yaw))
        camera_setting.rotation.yaw = vehicle_setting.rotation.yaw;

    if (std::isnan(camera_setting.rotation.pitch))
        camera_setting.rotation.pitch = vehicle_setting.rotation.pitch;

    if (std::isnan(camera_setting.rotation.roll))
        camera_setting.rotation.roll = vehicle_setting.rotation.roll;
}

void AirsimROSWrapper::convert_tf_msg_to_enu(geometry_msgs::msg::TransformStamped& tf_msg)
{
    std::swap(tf_msg.transform.translation.x, tf_msg.transform.translation.y);
    std::swap(tf_msg.transform.rotation.x, tf_msg.transform.rotation.y);
    tf_msg.transform.translation.z = -tf_msg.transform.translation.z;
    tf_msg.transform.rotation.z = -tf_msg.transform.rotation.z;
}

void AirsimROSWrapper::convert_tf_msg_to_ros(geometry_msgs::msg::TransformStamped& tf_msg)
{
    tf_msg.transform.translation.z = -tf_msg.transform.translation.z;
    tf_msg.transform.translation.y = -tf_msg.transform.translation.y;
    tf_msg.transform.rotation.z = -tf_msg.transform.rotation.z;
    tf_msg.transform.rotation.y = -tf_msg.transform.rotation.y;
}

geometry_msgs::msg::Transform AirsimROSWrapper::get_camera_optical_tf_from_body_tf(const geometry_msgs::msg::Transform& body_tf) const
{
    geometry_msgs::msg::Transform optical_tf = body_tf; //same translation
    auto opticalQ = msr::airlib::Quaternionr(optical_tf.rotation.w, optical_tf.rotation.x, optical_tf.rotation.y, optical_tf.rotation.z);
    opticalQ *= msr::airlib::Quaternionr(0.5, -0.5, 0.5, -0.5); 
    optical_tf.rotation.w = opticalQ.w();
    optical_tf.rotation.x = opticalQ.x();
    optical_tf.rotation.y = opticalQ.y();
    optical_tf.rotation.z = opticalQ.z();
    return optical_tf;
}

void AirsimROSWrapper::append_static_vehicle_tf(VehicleROS* vehicle_ros, const VehicleSetting& vehicle_setting)
{
    geometry_msgs::msg::TransformStamped vehicle_tf_msg;
    vehicle_tf_msg.header.frame_id = world_frame_id_;
    vehicle_tf_msg.header.stamp = nh_->now();
    vehicle_tf_msg.child_frame_id = vehicle_ros->vehicle_name_;
    vehicle_tf_msg.transform = get_transform_msg_from_airsim(vehicle_setting.position, vehicle_setting.rotation);

    convert_tf_msg_to_ros(vehicle_tf_msg);

    vehicle_ros->static_tf_msg_vec_.emplace_back(vehicle_tf_msg);
}

void AirsimROSWrapper::append_static_lidar_tf(VehicleROS* vehicle_ros, const std::string& lidar_name, const msr::airlib::LidarSimpleParams& lidar_setting)
{
    geometry_msgs::msg::TransformStamped lidar_tf_msg;
    if(lidar_setting.external)
        lidar_tf_msg.header.frame_id = world_frame_id_;
    else
        lidar_tf_msg.header.frame_id = vehicle_ros->vehicle_name_ + "/" + odom_frame_id_;
    lidar_tf_msg.child_frame_id = vehicle_ros->vehicle_name_ + "/" + lidar_name;
    auto lidar_data  = airsim_client_lidar_.getLidarData(lidar_name, vehicle_ros->vehicle_name_);

    lidar_tf_msg.transform = get_transform_msg_from_airsim(lidar_data.pose.position, lidar_data.pose.orientation);

    convert_tf_msg_to_ros(lidar_tf_msg);

    vehicle_ros->static_tf_msg_vec_.emplace_back(lidar_tf_msg);
}

void AirsimROSWrapper::append_static_gpulidar_tf(VehicleROS* vehicle_ros, const std::string& gpulidar_name, const msr::airlib::GPULidarSimpleParams& gpulidar_setting)
{
    geometry_msgs::msg::TransformStamped gpulidar_tf_msg;
    if(gpulidar_setting.external)
        gpulidar_tf_msg.header.frame_id = world_frame_id_;
    else
        gpulidar_tf_msg.header.frame_id = vehicle_ros->vehicle_name_ + "/" + odom_frame_id_;
    gpulidar_tf_msg.child_frame_id = vehicle_ros->vehicle_name_ + "/" + gpulidar_name;
    auto gpulidar_data  = airsim_client_gpulidar_.getGPULidarData(gpulidar_name, vehicle_ros->vehicle_name_);

    gpulidar_tf_msg.transform = get_transform_msg_from_airsim(gpulidar_data.pose.position, gpulidar_data.pose.orientation);

    convert_tf_msg_to_ros(gpulidar_tf_msg);

    vehicle_ros->static_tf_msg_vec_.emplace_back(gpulidar_tf_msg);
}

void AirsimROSWrapper::append_static_echo_tf(VehicleROS* vehicle_ros, const std::string& echo_name, const msr::airlib::EchoSimpleParams& echo_setting)
{
    geometry_msgs::msg::TransformStamped echo_tf_msg;
    if(echo_setting.external)
        echo_tf_msg.header.frame_id = world_frame_id_;
    else
        echo_tf_msg.header.frame_id = vehicle_ros->vehicle_name_ + "/" + odom_frame_id_;
    echo_tf_msg.child_frame_id = vehicle_ros->vehicle_name_ + "/" + echo_name;
    auto echo_data  = airsim_client_echo_.getEchoData(echo_name, vehicle_ros->vehicle_name_);

    echo_tf_msg.transform = get_transform_msg_from_airsim(echo_data.pose.position, echo_data.pose.orientation);

    convert_tf_msg_to_ros(echo_tf_msg);

    vehicle_ros->static_tf_msg_vec_.emplace_back(echo_tf_msg);
}

void AirsimROSWrapper::append_static_camera_tf(VehicleROS* vehicle_ros, const std::string& camera_name, const CameraSetting& camera_setting)
{
    geometry_msgs::msg::TransformStamped static_cam_tf_body_msg;
    if(camera_setting.external)
        static_cam_tf_body_msg.header.frame_id = world_frame_id_;
    else
        static_cam_tf_body_msg.header.frame_id = vehicle_ros->vehicle_name_ + "/" + odom_frame_id_;
    static_cam_tf_body_msg.child_frame_id = vehicle_ros->vehicle_name_ + "/" + camera_name + "_body";

    auto camera_info_data = airsim_client_images_.simGetCameraInfo(camera_name, vehicle_ros->vehicle_name_);
    static_cam_tf_body_msg.transform = get_transform_msg_from_airsim(camera_info_data.pose.position, camera_info_data.pose.orientation);

    convert_tf_msg_to_ros(static_cam_tf_body_msg);

    geometry_msgs::msg::TransformStamped static_cam_tf_optical_msg = static_cam_tf_body_msg;
    if(camera_setting.external)
        static_cam_tf_body_msg.header.frame_id = world_frame_id_;
    else
        static_cam_tf_body_msg.header.frame_id = vehicle_ros->vehicle_name_ + "/" + odom_frame_id_;
    static_cam_tf_optical_msg.child_frame_id = vehicle_ros->vehicle_name_ + "/" + camera_name + "_optical";
    static_cam_tf_optical_msg.transform = get_camera_optical_tf_from_body_tf(static_cam_tf_body_msg.transform);

    vehicle_ros->static_tf_msg_vec_.emplace_back(static_cam_tf_body_msg);
    vehicle_ros->static_tf_msg_vec_.emplace_back(static_cam_tf_optical_msg);
}

void AirsimROSWrapper::img_response_timer_cb()
{
    try {
        int image_response_idx = 0;
        for (const auto& airsim_img_request_vehicle_name_pair : airsim_img_request_vehicle_name_pair_vec_) {
            const std::vector<ImageResponse>& img_response = airsim_client_images_.simGetImages(airsim_img_request_vehicle_name_pair.first, airsim_img_request_vehicle_name_pair.second);

            if (img_response.size() == airsim_img_request_vehicle_name_pair.first.size()) {
                process_and_publish_img_response(img_response, image_response_idx, airsim_img_request_vehicle_name_pair.second);
                image_response_idx += img_response.size();
            }
        }
    }

    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API, didn't get image response.\n%s", msg.c_str());
    }
}

void AirsimROSWrapper::lidar_timer_cb()
{
    try {
        for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
            if (!vehicle_name_ptr_pair.second->lidar_pubs_.empty()) {
                std::unordered_map<std::string, msr::airlib::LidarData> sensor_names_to_lidar_data_map;
                for (auto& lidar_publisher : vehicle_name_ptr_pair.second->lidar_pubs_) {
                    auto lidar_data = airsim_client_lidar_.getLidarData(lidar_publisher.sensor_name, vehicle_name_ptr_pair.first);
                    sensor_names_to_lidar_data_map[lidar_publisher.sensor_name] = lidar_data;
                    sensor_msgs::msg::PointCloud2 lidar_msg = get_lidar_msg_from_airsim(lidar_data, vehicle_name_ptr_pair.first, lidar_publisher.sensor_name);
                    lidar_publisher.publisher->publish(lidar_msg);
                }
                for (auto& lidar_labels_publisher : vehicle_name_ptr_pair.second->lidar_labels_pubs_) {
                    msr::airlib::LidarData lidar_data;
                    auto it = sensor_names_to_lidar_data_map.find(lidar_labels_publisher.sensor_name);
                    if (it != sensor_names_to_lidar_data_map.end()) {
                        lidar_data = it->second;
                    }
                    else {
                        lidar_data = airsim_client_echo_.getLidarData(lidar_labels_publisher.sensor_name, vehicle_name_ptr_pair.first);
                    }
                    airsim_interfaces::msg::StringArray lidar_label_msg = get_lidar_labels_msg_from_airsim(lidar_data, vehicle_name_ptr_pair.first, lidar_labels_publisher.sensor_name);
                    lidar_labels_publisher.publisher->publish(lidar_label_msg);
                }
            }
        }
    }
    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API, didn't get lidar response.\n%s", msg.c_str());
    }
}

void AirsimROSWrapper::gpulidar_timer_cb()
{
    try {
        for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
            if (!vehicle_name_ptr_pair.second->gpulidar_pubs_.empty()) {
                for (auto& gpulidar_publisher : vehicle_name_ptr_pair.second->gpulidar_pubs_) {
                    auto gpulidar_data = airsim_client_gpulidar_.getGPULidarData(gpulidar_publisher.sensor_name, vehicle_name_ptr_pair.first);
                    sensor_msgs::msg::PointCloud2 gpulidar_msg = get_gpulidar_msg_from_airsim(gpulidar_data, vehicle_name_ptr_pair.first, gpulidar_publisher.sensor_name);
                    gpulidar_publisher.publisher->publish(gpulidar_msg);
                }
            }
        }
    }
    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API, didn't get gpulidar response.\n%s", msg.c_str());
    }
}

void AirsimROSWrapper::echo_timer_cb()
{
    try {
        for (auto& vehicle_name_ptr_pair : vehicle_name_ptr_map_) {
            if (!vehicle_name_ptr_pair.second->echo_active_pubs_.empty() && !vehicle_name_ptr_pair.second->echo_passive_pubs_.empty()) {
                std::unordered_map<std::string, msr::airlib::EchoData> sensor_names_to_echo_data_map;
                for (auto& active_echo_publisher : vehicle_name_ptr_pair.second->echo_active_pubs_) {                    
                    auto echo_data = airsim_client_echo_.getEchoData(active_echo_publisher.sensor_name, vehicle_name_ptr_pair.first);
                    sensor_names_to_echo_data_map[active_echo_publisher.sensor_name] = echo_data;
                    sensor_msgs::msg::PointCloud2 echo_msg = get_active_echo_msg_from_airsim(echo_data, vehicle_name_ptr_pair.first, active_echo_publisher.sensor_name);
                    active_echo_publisher.publisher->publish(echo_msg);
                }
                for (auto& passive_echo_publisher : vehicle_name_ptr_pair.second->echo_passive_pubs_) {
                    msr::airlib::EchoData echo_data;
                    auto it = sensor_names_to_echo_data_map.find(passive_echo_publisher.sensor_name);
                    if (it != sensor_names_to_echo_data_map.end()) {
                        echo_data = it->second;
                    }
                    else {
                        echo_data = airsim_client_echo_.getEchoData(passive_echo_publisher.sensor_name, vehicle_name_ptr_pair.first);
                        sensor_names_to_echo_data_map[passive_echo_publisher.sensor_name] = echo_data;
                    }
                    sensor_msgs::msg::PointCloud2 echo_msg = get_passive_echo_msg_from_airsim(echo_data, vehicle_name_ptr_pair.first, passive_echo_publisher.sensor_name);
                    passive_echo_publisher.publisher->publish(echo_msg);
                }
                for (auto& active_echo_labels_publisher : vehicle_name_ptr_pair.second->echo_active_labels_pubs_) {
                    msr::airlib::EchoData echo_data;
                    auto it = sensor_names_to_echo_data_map.find(active_echo_labels_publisher.sensor_name);
                    if (it != sensor_names_to_echo_data_map.end()) {
                        echo_data = it->second;
                    }
                    else {
                        echo_data = airsim_client_echo_.getEchoData(active_echo_labels_publisher.sensor_name, vehicle_name_ptr_pair.first);
                        sensor_names_to_echo_data_map[active_echo_labels_publisher.sensor_name] = echo_data;
                    }
                    airsim_interfaces::msg::StringArray echo_labels_msg = get_active_echo_labels_msg_from_airsim(echo_data, vehicle_name_ptr_pair.first, active_echo_labels_publisher.sensor_name);
                    active_echo_labels_publisher.publisher->publish(echo_labels_msg);
                }
                for (auto& passive_echo_labels_publisher : vehicle_name_ptr_pair.second->echo_passive_labels_pubs_) {
                    msr::airlib::EchoData echo_data;
                    auto it = sensor_names_to_echo_data_map.find(passive_echo_labels_publisher.sensor_name);
                    if (it != sensor_names_to_echo_data_map.end()) {
                        echo_data = it->second;
                    }
                    else {
                        echo_data = airsim_client_echo_.getEchoData(passive_echo_labels_publisher.sensor_name, vehicle_name_ptr_pair.first);
                    }
                    airsim_interfaces::msg::StringArray echo_labels_msg = get_passive_echo_labels_msg_from_airsim(echo_data, vehicle_name_ptr_pair.first, passive_echo_labels_publisher.sensor_name);
                    passive_echo_labels_publisher.publisher->publish(echo_labels_msg);
                }
            }
        }
    }
    catch (rpc::rpc_error& e) {
        std::string msg = e.get_error().as<std::string>();
        RCLCPP_ERROR(nh_->get_logger(), "Exception raised by the API, didn't get echo response.\n%s", msg.c_str());
    }
}

std::shared_ptr<sensor_msgs::msg::Image> AirsimROSWrapper::get_img_msg_from_response(const ImageResponse& img_response,
                                                                                     const rclcpp::Time curr_ros_time,
                                                                                     const std::string frame_id)
{
    unused(curr_ros_time);
    std::shared_ptr<sensor_msgs::msg::Image> img_msg_ptr = std::make_shared<sensor_msgs::msg::Image>();
    img_msg_ptr->data = img_response.image_data_uint8;
    img_msg_ptr->step = img_response.image_data_uint8.size() / img_response.height;
    img_msg_ptr->header.stamp = rclcpp::Time(img_response.time_stamp);
    img_msg_ptr->header.frame_id = frame_id;
    img_msg_ptr->height = img_response.height;
    img_msg_ptr->width = img_response.width;
    img_msg_ptr->encoding = "bgr8";
    if (is_vulkan_)
        img_msg_ptr->encoding = "rgb8";
    img_msg_ptr->is_bigendian = 0;
    return img_msg_ptr;
}

std::shared_ptr<sensor_msgs::msg::Image> AirsimROSWrapper::get_depth_img_msg_from_response(const ImageResponse& img_response,
                                                                                           const rclcpp::Time curr_ros_time,
                                                                                           const std::string frame_id)
{
    unused(curr_ros_time);
    auto depth_img_msg = std::make_shared<sensor_msgs::msg::Image>();
    depth_img_msg->width = img_response.width;
    depth_img_msg->height = img_response.height;
    depth_img_msg->data.resize(img_response.image_data_float.size() * sizeof(float));
    memcpy(depth_img_msg->data.data(), img_response.image_data_float.data(), depth_img_msg->data.size());
    depth_img_msg->encoding = "32FC1";
    depth_img_msg->step = depth_img_msg->data.size() / img_response.height;
    depth_img_msg->is_bigendian = 0;
    depth_img_msg->header.stamp = rclcpp::Time(img_response.time_stamp);
    depth_img_msg->header.frame_id = frame_id;
    return depth_img_msg;
}

// todo have a special stereo pair mode and get projection matrix by calculating offset wrt drone body frame?
sensor_msgs::msg::CameraInfo AirsimROSWrapper::generate_cam_info(const std::string& camera_name,
                                                                 const CameraSetting& camera_setting,
                                                                 const CaptureSetting& capture_setting) const
{
    unused(camera_setting);
    sensor_msgs::msg::CameraInfo cam_info_msg;
    cam_info_msg.header.frame_id = camera_name + "_optical";
    cam_info_msg.height = capture_setting.height;
    cam_info_msg.width = capture_setting.width;
    float f_x = (capture_setting.width / 2.0) / tan(math_common::deg2rad(capture_setting.fov_degrees / 2.0));
    // todo focal length in Y direction should be same as X it seems. this can change in future a scene capture component which exactly correponds to a cine camera
    // float f_y = (capture_setting.height / 2.0) / tan(math_common::deg2rad(fov_degrees / 2.0));
    cam_info_msg.k = { f_x, 0.0, capture_setting.width / 2.0, 0.0, f_x, capture_setting.height / 2.0, 0.0, 0.0, 1.0 };
    cam_info_msg.p = { f_x, 0.0, capture_setting.width / 2.0, 0.0, 0.0, f_x, capture_setting.height / 2.0, 0.0, 0.0, 0.0, 1.0, 0.0 };
    cam_info_msg.d = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    cam_info_msg.r = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    return cam_info_msg;
}

void AirsimROSWrapper::process_and_publish_img_response(const std::vector<ImageResponse>& img_response_vec, const int img_response_idx, const std::string& vehicle_name)
{
    // todo add option to use airsim time (image_response.TTimePoint) like Gazebo /use_sim_time param
    rclcpp::Time curr_ros_time = nh_->now();
    int img_response_idx_internal = img_response_idx;

    for (const auto& curr_img_response : img_response_vec) {
   
        // todo simGetCameraInfo is wrong + also it's only for image type -1.
        // msr::airlib::CameraInfo camera_info = airsim_client_.simGetCameraInfo(curr_img_response.camera_name);

        // update timestamp of saved cam info msgs

        camera_info_msg_vec_[img_response_idx_internal].header.stamp = rclcpp::Time(curr_img_response.time_stamp);
        cam_info_pub_vec_[img_response_idx_internal]->publish(camera_info_msg_vec_[img_response_idx_internal]);

        // DepthPlanar / DepthPerspective / DepthVis / DisparityNormalized
        if (curr_img_response.pixels_as_float) {
            image_pub_vec_[img_response_idx_internal].publish(get_depth_img_msg_from_response(curr_img_response,
                                                                                              curr_ros_time,
                                                                                              vehicle_name + "/" + curr_img_response.camera_name + "_optical"));
        }
        // All the others
        else {
            image_pub_vec_[img_response_idx_internal].publish(get_img_msg_from_response(curr_img_response,
                                                                                        curr_ros_time,
                                                                                        vehicle_name + "/" + curr_img_response.camera_name + "_optical"));
        }
        img_response_idx_internal++;
    }
}

void AirsimROSWrapper::convert_yaml_to_simple_mat(const YAML::Node& node, SimpleMatrix& m) const
{
    int rows, cols;
    rows = node["rows"].as<int>();
    cols = node["cols"].as<int>();
    const YAML::Node& data = node["data"];
    for (int i = 0; i < rows * cols; ++i) {
        m.data[i] = data[i].as<double>();
    }
}

void AirsimROSWrapper::read_params_from_yaml_and_fill_cam_info_msg(const std::string& file_name, sensor_msgs::msg::CameraInfo& cam_info) const
{
    std::ifstream fin(file_name.c_str());
    YAML::Node doc = YAML::Load(fin);

    cam_info.width = doc[WIDTH_YML_NAME].as<int>();
    cam_info.height = doc[HEIGHT_YML_NAME].as<int>();

    SimpleMatrix K_(3, 3, &cam_info.k[0]);
    convert_yaml_to_simple_mat(doc[K_YML_NAME], K_);
    SimpleMatrix R_(3, 3, &cam_info.r[0]);
    convert_yaml_to_simple_mat(doc[R_YML_NAME], R_);
    SimpleMatrix P_(3, 4, &cam_info.p[0]);
    convert_yaml_to_simple_mat(doc[P_YML_NAME], P_);

    cam_info.distortion_model = doc[DMODEL_YML_NAME].as<std::string>();

    const YAML::Node& D_node = doc[D_YML_NAME];
    int D_rows, D_cols;
    D_rows = D_node["rows"].as<int>();
    D_cols = D_node["cols"].as<int>();
    const YAML::Node& D_data = D_node["data"];
    cam_info.d.resize(D_rows * D_cols);
    for (int i = 0; i < D_rows * D_cols; ++i) {
        cam_info.d[i] = D_data[i].as<float>();
    }
}