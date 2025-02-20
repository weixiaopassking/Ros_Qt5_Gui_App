/**
 * @file /src/qnode.cpp
 *
 * @brief Ros communication central!
 *
 * @date February 2011
 **/

/*****************************************************************************
** Includes
*****************************************************************************/

#include "qnode.hpp"

#include <ros/network.h>
#include <ros/ros.h>
#include <std_msgs/String.h>

#include <QDebug>
#include <sstream>
#include <string>

namespace ros_qt5_gui_app {

/*****************************************************************************
** Implementation
*****************************************************************************/

QNode::QNode(int argc, char** argv) : init_argc(argc), init_argv(argv) {
  //    读取topic的设置
  QSettings topic_setting("ros_qt5_gui_app", "settings");
  odom_topic = topic_setting.value("topic/topic_odom", "odom").toString();
  batteryState_topic =
      topic_setting.value("topic/topic_power", "battery_state").toString();
  initPose_topic =
      topic_setting.value("topic/topic_init_pose", "/initialpose").toString();
  naviGoal_topic =
      topic_setting.value("topic/topic_goal", "move_base_simple/goal")
          .toString();
  pose_topic = topic_setting.value("topic/topic_amcl", "amcl_pose").toString();
  m_frameRate = topic_setting.value("main/framerate", 40).toInt();
  m_threadNum = topic_setting.value("main/thread_num", 6).toInt();
  QSettings settings("ros_qt5_gui_app", "Displays");
  map_topic = settings.value("Map/topic", QString("/map")).toString();
  laser_topic = settings.value("Laser/topic", QString("/scan")).toString();
  laser_frame =
      settings.value("frame/laserFrame", "/base_scan").toString().toStdString();
  map_frame = settings.value("frame/mapFrame", "/map").toString().toStdString();
  base_frame =
      settings.value("frame/baseFrame", "/base_link").toString().toStdString();
  path_topic =
      settings.value("GlobalPlan/topic", "/movebase").toString().toStdString();
  qRegisterMetaType<sensor_msgs::BatteryState>("sensor_msgs::BatteryState");
  qRegisterMetaType<RobotPose>("RobotPose");
  qRegisterMetaType<RobotStatus>("RobotStatus");
  qRegisterMetaType<QVector<int>>("QVector<int>");
}

QNode::~QNode() {
  if (ros::isStarted()) {
    ros::shutdown();  // explicitly needed since we use ros::start();
    ros::waitForShutdown();
  }
  wait();
}

bool QNode::init() {
  ros::init(init_argc, init_argv, "ros_qt5_gui_app",
            ros::init_options::AnonymousName);
  if (!ros::master::check()) {
    return false;
  }
  ros::start();  // explicitly needed since our nodehandle is going out of
                 // scope.
  SubAndPubTopic();
  start();
  return true;
}

//初始化的函数*********************************
bool QNode::init(const std::string& master_url, const std::string& host_url) {
  std::map<std::string, std::string> remappings;
  remappings["__master"] = master_url;
  remappings["__hostname"] = host_url;
  ros::init(remappings, "ros_qt5_gui_app", ros::init_options::AnonymousName);
  if (!ros::master::check()) {
    return false;
  }
  ros::start();  // explicitly needed since our nodehandle is going out of
                 // scope.
  SubAndPubTopic();
  start();
  return true;
}
//创建订阅者与发布者
void QNode::SubAndPubTopic() {
  ros::NodeHandle n;
  // Add your ros communications here.
  //创建速度话题的订阅者
  cmdVel_sub = n.subscribe<nav_msgs::Odometry>(odom_topic.toStdString(), 200,
                                               &QNode::speedCallback, this);
  battery_sub = n.subscribe(batteryState_topic.toStdString(), 1000,
                            &QNode::batteryCallback, this);
  //地图订阅
  map_sub = n.subscribe("map", 1000, &QNode::mapCallback, this);
  //导航目标点发送话题
  goal_pub = n.advertise<geometry_msgs::PoseStamped>(
      naviGoal_topic.toStdString(), 1000);
  //速度控制话题
  cmd_pub = n.advertise<geometry_msgs::Twist>("cmd_vel", 1000);
  //激光雷达点云话题订阅
  m_laserSub = n.subscribe(laser_topic.toStdString(), 1000,
                           &QNode::laserScanCallback, this);
  //全局规划Path
  m_plannerPathSub =
      n.subscribe(path_topic, 1000, &QNode::plannerPathCallback, this);
  m_initialposePub = n.advertise<geometry_msgs::PoseWithCovarianceStamped>(
      initPose_topic.toStdString(), 10);
  image_transport::ImageTransport it(n);
  m_imageMapPub = it.advertise("image/map", 10);
  m_robotPoselistener = new tf::TransformListener;
  m_Laserlistener = new tf::TransformListener;
  try {
    m_robotPoselistener->waitForTransform(map_frame, base_frame, ros::Time(0),
                                          ros::Duration(0.4));
    m_Laserlistener->waitForTransform(map_frame, laser_frame, ros::Time(0),
                                      ros::Duration(0.4));
  } catch (tf::TransformException& ex) {
    log(Error, ("laser and robot pose tf listener: " + QString(ex.what()))
                   .toStdString());
  }
  movebase_client = new MoveBaseClient("move_base", true);
  movebase_client->waitForServer(ros::Duration(1.0));
}

QMap<QString, QString> QNode::get_topic_list() {
  ros::master::V_TopicInfo topic_list;
  ros::master::getTopics(topic_list);
  QMap<QString, QString> res;
  for (auto topic : topic_list) {
    res.insert(QString::fromStdString(topic.name),
               QString::fromStdString(topic.datatype));
  }
  return res;
}
// planner的路径话题回调
void QNode::plannerPathCallback(nav_msgs::Path::ConstPtr path) {
  plannerPoints.clear();
  for (int i = 0; i < path->poses.size(); i++) {
    QPointF roboPos = transWordPoint2Scene(QPointF(
        path->poses[i].pose.position.x, path->poses[i].pose.position.y));
    plannerPoints.append(roboPos);
  }
  emit plannerPath(plannerPoints);
}

//激光雷达点云话题回调
void QNode::laserScanCallback(sensor_msgs::LaserScanConstPtr laser_msg) {
  geometry_msgs::PointStamped laser_point;
  geometry_msgs::PointStamped map_point;
  laser_point.header.frame_id = laser_msg->header.frame_id;
  std::vector<float> ranges = laser_msg->ranges;
  laserPoints.clear();
  //转换到二维XY平面坐标系下;
  for (int i = 0; i < ranges.size(); i++) {
    // scan_laser坐标系下
    double angle = laser_msg->angle_min + i * laser_msg->angle_increment;
    double X = ranges[i] * cos(angle);
    double Y = ranges[i] * sin(angle);
    laser_point.point.x = X;
    laser_point.point.y = Y;
    laser_point.point.z = 0.0;
    // change to map frame
    try {
      m_Laserlistener->transformPoint(map_frame, laser_point, map_point);
    } catch (tf::TransformException& ex) {
      log(Error, ("laser tf transform: " + QString(ex.what())).toStdString());
      try {
        m_robotPoselistener->waitForTransform(map_frame, base_frame,
                                              ros::Time(0), ros::Duration(0.4));
        m_Laserlistener->waitForTransform(map_frame, laser_frame, ros::Time(0),
                                          ros::Duration(0.4));
      } catch (tf::TransformException& ex) {
        log(Error, ("laser tf transform: " + QString(ex.what())).toStdString());
      }
    }
    //转化为图元坐标系
    QPointF roboPos =
        transWordPoint2Scene(QPointF(map_point.point.x, map_point.point.y));
    laserPoints.append(roboPos);
  }
  emit updateLaserScan(laserPoints);
}

void QNode::updateRobotPose() {
  try {
    tf::StampedTransform transform;
    m_robotPoselistener->lookupTransform(map_frame, base_frame, ros::Time(0),
                                         transform);
    tf::Quaternion q = transform.getRotation();
    double x = transform.getOrigin().getX();
    double y = transform.getOrigin().getY();
    tf::Matrix3x3 mat(q);
    double roll, pitch, yaw;
    mat.getRPY(roll, pitch, yaw);
    //坐标转化为图元坐标系
    QPointF roboPos = transWordPoint2Scene(QPointF(x, y));
    RobotPose pos{roboPos.x(), roboPos.y(), yaw};
    emit updateRoboPose(pos);
  } catch (tf::TransformException& ex) {
    log(Error,
        ("robot pose tf transform: " + QString(ex.what())).toStdString());
    try {
      m_robotPoselistener->waitForTransform(map_frame, base_frame, ros::Time(0),
                                            ros::Duration(0.4));
      m_Laserlistener->waitForTransform(map_frame, laser_frame, ros::Time(0),
                                        ros::Duration(0.4));
    } catch (tf::TransformException& ex) {
      log(Error,
          ("robot pose tf transform: " + QString(ex.what())).toStdString());
    }
  }
}
void QNode::batteryCallback(const sensor_msgs::BatteryState& message) {
  emit batteryState(message);
}
void QNode::myCallback(const std_msgs::Float64& message_holder) {
  qDebug() << message_holder.data << endl;
}
//发布导航目标点信息
void QNode::set_goal(QString frame, double x, double y, double z, double w) {
  geometry_msgs::PoseStamped goal;
  //设置frame
  goal.header.frame_id = frame.toStdString();
  //设置时刻
  goal.header.stamp = ros::Time::now();
  goal.pose.position.x = x;
  goal.pose.position.y = y;
  goal.pose.position.z = 0;
  goal.pose.orientation.z = z;
  goal.pose.orientation.w = w;
  goal_pub.publish(goal);
}
//地图信息订阅回调函数
void QNode::mapCallback(nav_msgs::OccupancyGrid::ConstPtr msg) {
  int width = msg->info.width;
  int height = msg->info.height;

  m_mapResolution = msg->info.resolution;
  double origin_x = msg->info.origin.position.x;
  double origin_y = msg->info.origin.position.y;
  QImage map_image(width, height, QImage::Format_RGB32);
  for (int i = 0; i < msg->data.size(); i++) {
    int x = i % width;
    int y = (int)i / width;
    //计算像素值
    QColor color;
    if (msg->data[i] == 100) {
      color = Qt::black;  // black
    } else if (msg->data[i] == 0) {
      color = Qt::white;  // white
    } else if (msg->data[i] == -1) {
      color = Qt::gray;  // gray
    }
    map_image.setPixel(x, y, qRgb(color.red(), color.green(), color.blue()));
  }
  //延y翻转地图 因为解析到的栅格地图的坐标系原点为左下角
  //但是图元坐标系为左上角度
  map_image = rotateMapWithY(map_image);
  emit updateMap(map_image);
  //计算翻转后的图元坐标系原点的世界坐标
  double origin_x_ = origin_x;
  double origin_y_ = origin_y + height * m_mapResolution;
  //世界坐标系原点在图元坐标系下的坐标
  m_wordOrigin.setX(fabs(origin_x_) / m_mapResolution);
  m_wordOrigin.setY(fabs(origin_y_) / m_mapResolution);
}

//速度回调函数
void QNode::speedCallback(const nav_msgs::Odometry::ConstPtr& msg) {
  emit speed_x(msg->twist.twist.linear.x);
  emit speed_y(msg->twist.twist.linear.y);
}
void QNode::run() {
  ros::Rate loop_rate(m_frameRate);
  ros::AsyncSpinner spinner(m_threadNum);
  spinner.start();
  //当当前节点没有关闭时
  while (ros::ok()) {
    updateRobotPose();
    emit updateRobotStatus(RobotStatus::normal);
    loop_rate.sleep();
  }
  //如果当前节点关闭
  Q_EMIT
  rosShutdown();  // used to signal the gui for a shutdown (useful to roslaunch)
}
//发布机器人速度控制
void QNode::move_base(char k, float speed_linear, float speed_trun) {
  std::map<char, std::vector<float>> moveBindings{
      {'i', {1, 0, 0, 0}},  {'o', {1, 0, 0, -1}},  {'j', {0, 0, 0, 1}},
      {'l', {0, 0, 0, -1}}, {'u', {1, 0, 0, 1}},   {',', {-1, 0, 0, 0}},
      {'.', {-1, 0, 0, 1}}, {'m', {-1, 0, 0, -1}}, {'O', {1, -1, 0, 0}},
      {'I', {1, 0, 0, 0}},  {'J', {0, 1, 0, 0}},   {'L', {0, -1, 0, 0}},
      {'U', {1, 1, 0, 0}},  {'<', {-1, 0, 0, 0}},  {'>', {-1, -1, 0, 0}},
      {'M', {-1, 1, 0, 0}}, {'t', {0, 0, 1, 0}},   {'b', {0, 0, -1, 0}},
      {'k', {0, 0, 0, 0}},  {'K', {0, 0, 0, 0}}};
  char key = k;
  //计算是往哪个方向
  float x = moveBindings[key][0];
  float y = moveBindings[key][1];
  float z = moveBindings[key][2];
  float th = moveBindings[key][3];
  //计算线速度和角速度
  float speed = speed_linear;
  float turn = speed_trun;
  // Update the Twist message
  geometry_msgs::Twist twist;
  twist.linear.x = x * speed;
  twist.linear.y = y * speed;
  twist.linear.z = z * speed;

  twist.angular.x = 0;
  twist.angular.y = 0;
  twist.angular.z = th * turn;

  // Publish it and resolve any remaining callbacks
  cmd_pub.publish(twist);
  ros::spinOnce();
}
//订阅图片话题，并在label上显示
void QNode::Sub_Image(QString topic, int frame_id) {
  ros::NodeHandle n;
  image_transport::ImageTransport it_(n);
  if (frame_id == 0) {
    m_compressedImgSub0 =
        n.subscribe(topic.toStdString(), 100, &QNode::imageCallback0, this);
  } else if (frame_id == 1) {
    m_compressedImgSub1 =
        n.subscribe(topic.toStdString(), 100, &QNode::imageCallback1, this);
  }
  ros::spinOnce();
}
void QNode::pub2DPose(QPointF start_pose,QPointF end_pose){
    start_pose =transScenePoint2Word(start_pose);
    end_pose =transScenePoint2Word(end_pose);
    double angle = atan2(end_pose.y()-start_pose.y(),end_pose.x()-start_pose.x());
    geometry_msgs::PoseWithCovarianceStamped goal;
    //设置frame
    goal.header.frame_id = "map";
    //设置时刻
    goal.header.stamp = ros::Time::now();
    goal.pose.pose.position.x = start_pose.x();
    goal.pose.pose.position.y = start_pose.y();
    goal.pose.pose.position.z = 0;
    goal.pose.pose.orientation =
        tf::createQuaternionMsgFromRollPitchYaw(0, 0, angle);
    m_initialposePub.publish(goal);

}
void QNode::pub2DGoal(QPointF start_pose,QPointF end_pose){
    start_pose =transScenePoint2Word(start_pose);
    end_pose =transScenePoint2Word(end_pose);
    double angle = atan2(end_pose.y()-start_pose.y(),end_pose.x()-start_pose.x());
    move_base_msgs::MoveBaseGoal goal;
    goal.target_pose.header.frame_id = "map";
    goal.target_pose.header.stamp = ros::Time::now();

    goal.target_pose.pose.position.x = start_pose.x();
    goal.target_pose.pose.position.y = start_pose.y();
    goal.target_pose.pose.position.z = 0;
    goal.target_pose.pose.orientation =
        tf::createQuaternionMsgFromRollPitchYaw(0, 0, angle);
    movebase_client->sendGoal(goal);
}
QPointF QNode::transScenePoint2Word(QPointF pose) {
  QPointF res;
  res.setX((pose.x() - m_wordOrigin.x()) * m_mapResolution);
  // y坐标系相反
  res.setY(-1 * (pose.y() - m_wordOrigin.y()) * m_mapResolution);
  return res;
}
QPointF QNode::transWordPoint2Scene(QPointF pose) {
  //    qDebug()<<pose;
  QPointF res;
  res.setX(m_wordOrigin.x() + pose.x() / m_mapResolution);
  res.setY(m_wordOrigin.y() - (pose.y() / m_mapResolution));
  return res;
}

void QNode::pub_imageMap(QImage map) {
  cv::Mat image = QImage2Mat(map);
  sensor_msgs::ImagePtr msg =
      cv_bridge::CvImage(std_msgs::Header(), "bgr8", image).toImageMsg();
  m_imageMapPub.publish(msg);
}
//图像话题的回调函数
void QNode::imageCallback0(const sensor_msgs::CompressedImageConstPtr& msg) {
  cv_bridge::CvImagePtr cv_ptr;

  try {
    //深拷贝转换为opencv类型
    cv_bridge::CvImagePtr cv_ptr_compressed =
        cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    QImage im = Mat2QImage(cv_ptr_compressed->image);
    emit Show_image(0, im);
  } catch (cv_bridge::Exception& e) {
    log(Error, ("video frame0 exception: " + QString(e.what())).toStdString());
    return;
  }
}
void QNode::imageCallback1(const sensor_msgs::CompressedImageConstPtr& msg) {
  cv_bridge::CvImagePtr cv_ptr;

  try {
    //深拷贝转换为opencv类型
    cv_bridge::CvImagePtr cv_ptr_compressed =
        cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    QImage im = Mat2QImage(cv_ptr_compressed->image);
    emit Show_image(1, im);
  } catch (cv_bridge::Exception& e) {
    log(Error, ("video frame0 exception: " + QString(e.what())).toStdString());
    return;
  }
}
QImage QNode::rotateMapWithY(QImage map) {
  QImage res = map;
  for (int x = 0; x < map.width(); x++) {
    for (int y = 0; y < map.height(); y++) {
      res.setPixelColor(x, map.height() - y - 1, map.pixel(x, y));
    }
  }
  return res;
}
QImage QNode::Mat2QImage(cv::Mat const& src) {
  QImage dest(src.cols, src.rows, QImage::Format_ARGB32);

  const float scale = 255.0;

  if (src.depth() == CV_8U) {
    if (src.channels() == 1) {
      for (int i = 0; i < src.rows; ++i) {
        for (int j = 0; j < src.cols; ++j) {
          int level = src.at<quint8>(i, j);
          dest.setPixel(j, i, qRgb(level, level, level));
        }
      }
    } else if (src.channels() == 3) {
      for (int i = 0; i < src.rows; ++i) {
        for (int j = 0; j < src.cols; ++j) {
          cv::Vec3b bgr = src.at<cv::Vec3b>(i, j);
          dest.setPixel(j, i, qRgb(bgr[2], bgr[1], bgr[0]));
        }
      }
    }
  } else if (src.depth() == CV_32F) {
    if (src.channels() == 1) {
      for (int i = 0; i < src.rows; ++i) {
        for (int j = 0; j < src.cols; ++j) {
          int level = scale * src.at<float>(i, j);
          dest.setPixel(j, i, qRgb(level, level, level));
        }
      }
    } else if (src.channels() == 3) {
      for (int i = 0; i < src.rows; ++i) {
        for (int j = 0; j < src.cols; ++j) {
          cv::Vec3f bgr = scale * src.at<cv::Vec3f>(i, j);
          dest.setPixel(j, i, qRgb(bgr[2], bgr[1], bgr[0]));
        }
      }
    }
  }

  return dest;
}
cv::Mat QNode::QImage2Mat(QImage& image) {
  cv::Mat mat;
  switch (image.format()) {
    case QImage::Format_ARGB32:
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32_Premultiplied:
      mat = cv::Mat(image.height(), image.width(), CV_8UC4,
                    (void*)image.constBits(), image.bytesPerLine());
      break;
    case QImage::Format_RGB888:
      mat = cv::Mat(image.height(), image.width(), CV_8UC3,
                    (void*)image.constBits(), image.bytesPerLine());
      cv::cvtColor(mat, mat, CV_BGR2RGB);
      break;
    case QImage::Format_Indexed8:
      mat = cv::Mat(image.height(), image.width(), CV_8UC1,
                    (void*)image.constBits(), image.bytesPerLine());
      break;
  }
  return mat;
}
void QNode::log(const LogLevel& level, const std::string& msg) {
  logging_model.insertRows(logging_model.rowCount(), 1);
  std::stringstream logging_model_msg;
  switch (level) {
    case (Debug): {
      logging_model_msg << "[DEBUG] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case (Info): {
      logging_model_msg << "[INFO] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case (Warn): {
      emit updateRobotStatus(RobotStatus::warn);
      logging_model_msg << "[INFO] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case (Error): {
      emit updateRobotStatus(RobotStatus::error);
      logging_model_msg << "[ERROR] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case (Fatal): {
      logging_model_msg << "[FATAL] [" << ros::Time::now() << "]: " << msg;
      break;
    }
  }
  QVariant new_row(QString(logging_model_msg.str().c_str()));
  logging_model.setData(logging_model.index(logging_model.rowCount() - 1),
                        new_row);
  Q_EMIT loggingUpdated();  // used to readjust the scrollbar
}

}  // namespace ros_qt5_gui_app
