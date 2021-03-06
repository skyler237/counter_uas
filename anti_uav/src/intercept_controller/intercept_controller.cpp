#include "intercept_controller/intercept_controller.h"
#include <stdio.h>

typedef Eigen::Matrix<double, 3, 4> Matrix3x4d;
static double avg_intruder_x_vel;
static double avg_intruder_y_vel;
static double avg_intruder_z_vel;
bool first_vel_measurement = true;
static Eigen::Vector3d  intruder_accel(0.0, 0.0, 0.0);
static double deriv_gain = 0.05;

// Used for debug messages
#define TARGET_CALLBACK 1
#define COMPUTE_CONTROL 2
#define PLAN_PATH 4
#define COMPUTE_FLEET_STATE 8
#define TARGET_PREDICT 16
#define ADAPTIVE_RADIUS 32
static int debug_print = TARGET_CALLBACK;

#define DIRECT 0
#define WAYPOINT 1
static int path_type = DIRECT;


namespace intercept_controller
{

InterceptController::InterceptController() :
  nh_(ros::NodeHandle()),
  nh_private_("~")
{
  // retrieve params
  thrust_eq_= nh_private_.param<double>("equilibrium_thrust", 0.34);
  is_flying_ = false;

  // System parameters
  fleet_square_width_ = nh_private_.param<double>("fleet_square_width", 4.0); // [m]
  distance_start_rotation_ = nh_private_.param<double>("distance_start_rotation", 30.0); // [m]
  distance_finish_rotation_ = nh_private_.param<double>("distance_finish_rotation", 10.0); // [m]
  deriv_gain = nh_private_.param<double>("deriv_gain", 0.05);

  // Maximum values
  max_.velocity = nh_private_.param<double>("max_velocity", 15.0);
  max_.acceleration = nh_private_.param<double>("max_acceleration", 5.0);
  max_.psi_rate = nh_private_.param<double>("max_yaw_rate", 0.03);
  max_.pos_error = nh_private_.param<double>("position_error", 2.0);
  max_.waypoint_cnt = nh_private_.param<int>("waypoint_cnt", 10);
  max_.control_delay = nh_private_.param<double>("control_delay", 2.0); // assumed delay of our control in responding to the target's movements

  // Protective radius parameters
  protection_radius_ = nh_private_.param<double>("protection_radius", 100.0);
  max_radius_ = protection_radius_;
  min_radius_ = nh_private_.param<double>("minimum_radius", 40.0);;
  current_radius_ = 10.0; // Initialize current target radius to 10 m
  full_response_radius_ = nh_private_.param<double>("full_response_radius", 20);

  alpha0_ = nh_private_.param<double>("exp_avg_alpha0", 0.01); // Used for exponential rolling average in target prediction
  debug_print = nh_private_.param<int>("debug_print", 0); // Used to determine which debug messages to print
  path_type = nh_private_.param<int>("path_type", 0); // Determines path type (either direct or waypoint)

  // Initialize all positions and velocities to zero
  resetStates();

  // Set up Publishers and Subscriber
  is_flying_sub_ = nh_.subscribe("is_flying", 1, &InterceptController::isFlyingCallback, this);
  target_sub_ = nh_.subscribe("target", 1, &InterceptController::targetCallback, this);

  // States of each fleet UAV
  uav1_state_sub_ = nh_.subscribe("uav1_pose", 1, &InterceptController::uav1_stateCallback, this);
  uav2_state_sub_ = nh_.subscribe("uav2_pose", 1, &InterceptController::uav2_stateCallback, this);
  uav3_state_sub_ = nh_.subscribe("uav3_pose", 1, &InterceptController::uav3_stateCallback, this);
  uav4_state_sub_ = nh_.subscribe("uav4_pose", 1, &InterceptController::uav4_stateCallback, this);

  // Subscribe to simulation results to know when to reset the simulation state variables
  // sim_results_sub_ = nh_.subscribe("results", 10, &InterceptController::sim_resultsCallback, this);

  // Desired position of the center of the fleet
  fleet_goal_pub_ = nh_.advertise<nav_msgs::Odometry>("fleet_goal", 1000);

  // This message is just used for debugging
  path_coeff_pub_ = nh_.advertise<anti_uav::PathCoeff>("path_coeff", 10);
}

void InterceptController::isFlyingCallback(const std_msgs::BoolConstPtr &msg)
{
  is_flying_ = msg->data;
}

// Retrieves the position and velocity of the target/intruder
// May also attempt to predict the position of the intruder
void InterceptController::targetCallback(const nav_msgs::OdometryConstPtr &msg)
{
  xt_.x = msg->pose.pose.position.x;
  xt_.y = msg->pose.pose.position.y;
  xt_.z = msg->pose.pose.position.z;

  Eigen::Vector3d distance(xt_.x - fleet_state_.x, xt_.y - fleet_state_.y, xt_.z - fleet_state_.z);
  if(debug_print & TARGET_CALLBACK) ROS_INFO("Distance=%f", distance.norm());

  xt_.xdot = msg->twist.twist.linear.x;
  xt_.ydot = msg->twist.twist.linear.y;
  xt_.zdot = msg->twist.twist.linear.z;

  // Initialize rolling average
  if(first_vel_measurement) {
    avg_intruder_x_vel = xt_.xdot;
    avg_intruder_y_vel = xt_.ydot;
    avg_intruder_z_vel = xt_.zdot;
    first_vel_measurement = false;
  }

  // Calculate exponential rolling average of velocities (helps filter out jerky movements)
  // Uses dynamic alpha value - the further we are from the intruder, the more we average; when we are closer, we stop averaging to get full responsiveness
  double alpha = (protection_radius_ - distance.norm())/(protection_radius_ - full_response_radius_); // Higher alpha discounts older observations faster
  alpha = saturate(alpha, 0.9, alpha0_);
  if(debug_print & TARGET_CALLBACK) ROS_INFO("alpha=%f", alpha);
  avg_intruder_x_vel = alpha*xt_.xdot + (1.0 - alpha)*avg_intruder_x_vel;
  avg_intruder_y_vel = alpha*xt_.ydot + (1.0 - alpha)*avg_intruder_y_vel;
  avg_intruder_z_vel = alpha*xt_.zdot + (1.0 - alpha)*avg_intruder_z_vel;
  if(debug_print & TARGET_CALLBACK){
    ROS_INFO("Target: Actual Velocity: x=%f, y=%f, z=%f", xt_.xdot, xt_.ydot, xt_.zdot);
    ROS_INFO("Target: Avg'd Velocity: x=%f, y=%f, z=%f", avg_intruder_x_vel, avg_intruder_y_vel, avg_intruder_z_vel);
  }

  // Convert Quaternion to RPY
  tf::Quaternion tf_quat;
  tf::quaternionMsgToTF(msg->pose.pose.orientation, tf_quat);
  tf::Matrix3x3(tf_quat).getRPY(xt_.phi, xt_.theta, xt_.psi);
  xt_.theta = xt_.theta;
  xt_.psi = xt_.psi;

  xt_.phidot = msg->twist.twist.angular.x;
  xt_.thetadot = msg->twist.twist.angular.x;
  xt_.psidot = msg->twist.twist.angular.x;

  if(is_flying_)
  {
    computeControl();
    publishCommand();
  }
  else
  {
    prev_time_ = ros::Time::now().toSec();
  }
}

void InterceptController::uav1_stateCallback(const nav_msgs::OdometryConstPtr &msg) {
  uav1_state_.x = msg->pose.pose.position.x;
  uav1_state_.y = msg->pose.pose.position.y;
  uav1_state_.z = msg->pose.pose.position.z;

  uav1_state_.xdot = msg->twist.twist.linear.x;
  uav1_state_.ydot = msg->twist.twist.linear.y;
  uav1_state_.zdot = msg->twist.twist.linear.z;
}

void InterceptController::uav2_stateCallback(const nav_msgs::OdometryConstPtr &msg) {
  uav2_state_.x = msg->pose.pose.position.x;
  uav2_state_.y = msg->pose.pose.position.y;
  uav2_state_.z = msg->pose.pose.position.z;

  uav2_state_.xdot = msg->twist.twist.linear.x;
  uav2_state_.ydot = msg->twist.twist.linear.y;
  uav2_state_.zdot = msg->twist.twist.linear.z;
}

void InterceptController::uav3_stateCallback(const nav_msgs::OdometryConstPtr &msg) {
  uav3_state_.x = msg->pose.pose.position.x;
  uav3_state_.y = msg->pose.pose.position.y;
  uav3_state_.z = msg->pose.pose.position.z;

  uav3_state_.xdot = msg->twist.twist.linear.x;
  uav3_state_.ydot = msg->twist.twist.linear.y;
  uav3_state_.zdot = msg->twist.twist.linear.z;
}

void InterceptController::uav4_stateCallback(const nav_msgs::OdometryConstPtr &msg) {
  uav4_state_.x = msg->pose.pose.position.x;
  uav4_state_.y = msg->pose.pose.position.y;
  uav4_state_.z = msg->pose.pose.position.z;

  uav4_state_.xdot = msg->twist.twist.linear.x;
  uav4_state_.ydot = msg->twist.twist.linear.y;
  uav4_state_.zdot = msg->twist.twist.linear.z;
}

// Computes the average/central position of the 4 fleet UAVs
void InterceptController::computeFleetState() {
  fleet_state_.x = (uav1_state_.x + uav2_state_.x + uav3_state_.x + uav4_state_.x)/4.0;
  fleet_state_.y = (uav1_state_.y + uav2_state_.y + uav3_state_.y + uav4_state_.y)/4.0;
  fleet_state_.z = (uav1_state_.z + uav2_state_.z + uav3_state_.z + uav4_state_.z)/4.0;

  if(debug_print & COMPUTE_FLEET_STATE) {
    ROS_INFO("uav1 state: x=%f, y=%f, z=%f", uav1_state_.x, uav1_state_.y, uav1_state_.z);
    ROS_INFO("uav2 state: x=%f, y=%f, z=%f", uav2_state_.x, uav2_state_.y, uav2_state_.z);
    ROS_INFO("uav3 state: x=%f, y=%f, z=%f", uav3_state_.x, uav3_state_.y, uav3_state_.z);
    ROS_INFO("uav4 state: x=%f, y=%f, z=%f", uav4_state_.x, uav4_state_.y, uav4_state_.z);
  }

  fleet_state_.xdot = (uav1_state_.xdot + uav2_state_.xdot + uav3_state_.xdot + uav4_state_.xdot)/4.0;
  fleet_state_.ydot = (uav1_state_.ydot + uav2_state_.ydot + uav3_state_.ydot + uav4_state_.ydot)/4.0;
  fleet_state_.zdot = (uav1_state_.zdot + uav2_state_.zdot + uav3_state_.zdot + uav4_state_.zdot)/4.0;

}

void InterceptController::computeControl()
{
    double now = ros::Time::now().toSec();
    double dt = now - prev_time_;
    prev_time_ = now;
    static double tau = 0.0; // Path progress variable

    static Eigen::Vector3d z_r; // Desired/reference position
    static Eigen::Vector3d z; // Actual position

    static Eigen::Vector3d error(0.0,0.0,0.0); // Error between desired and actual position
    static const double epsilon = 0.5; // Minimum distance to switch waypoints
    static Eigen::Vector3d target_d1(0.0,0.0,0.0); // for calculating acceleration
    Eigen::Vector3d target_pos(xt_.x, xt_.y, xt_.z);



    if(dt > 0.0)
    {
      // Compute the average position/velocity of the individual multirotors
      computeFleetState();
      z << fleet_state_.x, fleet_state_.y, fleet_state_.z;

      if(debug_print & COMPUTE_CONTROL) {
        ROS_INFO("============================= Computing Control ================================================");
        ROS_INFO("Fleet state: x=%f, y=%f, z=%f", fleet_state_.x, fleet_state_.y, fleet_state_.z);
        ROS_INFO("Intruder pos: x=%f, y=%f, z=%f", xt_.x, xt_.y, xt_.z);
        ROS_INFO("Intruder vel: x=%f, y=%f, z=%f", xt_.xdot, xt_.ydot, xt_.zdot);
      }

      // Calculate the current error
      error = z - z_r;

      Eigen::Vector3d target_vel(xt_.xdot, xt_.ydot, xt_.zdot);

      // Calculate intruder acceleration using "dirty derivative"
      intruder_accel = (2*deriv_gain - dt)/(2*deriv_gain + dt)*intruder_accel + 2/(2*deriv_gain + dt)*(target_pos - target_d1);
      intruder_accel = saturate_vector(intruder_accel, max_.acceleration, -1.0*max_.acceleration);
      target_d1 = target_pos;
      if(debug_print & COMPUTE_CONTROL) ROS_INFO("Intruder accel: x=%f, y=%f, z=%f", intruder_accel(0), intruder_accel(1), intruder_accel(2));

      // // Predict the target position
      // Eigen::Vector3d target_predicted = targetPredict(computeInterceptTime());
      // // Plan a path_ to the predicted position
      // Path_t path_ = planPath(z, target_predicted, target_pos);

      // Plan a path_ using the target's actual position
      path_ = planPath(z, target_pos, target_pos); // Using target position vector as intercept vector.

      // Debugging print waypoints loop
      if(debug_print & COMPUTE_CONTROL) {
        for(int i = 0; i < path_.waypoint_cnt; i++) {
          ROS_INFO("Waypoint %d: x=%f, y=%f, z=%f", i, path_.waypoints[i](0), path_.waypoints[i](1), path_.waypoints[i](2));
        }
      }

      // Control the desired positions based on this waypoint path.

      // Extract the distance to the next waypoint (if there is a next one...)
      Eigen::Vector3d dist_to_waypoint;
      if(waypoint_index_ < (path_.waypoint_cnt - 1)) {

        dist_to_waypoint = z_r - path_.waypoints[waypoint_index_+1];

        // Check if we are close enough to the waypoint
        if(tau >= 1 || (dist_to_waypoint.norm() < epsilon && error.norm() < max_.pos_error)) {
          // IF so, go to next waypoint -- TODO: We could make this waypoint transitioning much more effective (half planes, etc.)
          waypoint_index_++;
          tau = 0;
          z_r = path_.waypoints[waypoint_index_];
        }
        // Otherwise, calculate next step along the path.
        else {
          // Get the current point-to-point path distance
          Eigen::Vector3d path_distance = path_.waypoints[waypoint_index_ + 1] - path_.waypoints[waypoint_index_];

          // Calculate taudot (delta change along path)
          double taudot = saturate(max_.velocity/(path_distance.norm())*saturate(((max_.pos_error - error.norm())/max_.pos_error),1.0, 0.0), 1.0, 0.0);

          if(debug_print & COMPUTE_CONTROL) {
            ROS_INFO("Error: = %f", error.norm());
            ROS_INFO("dt=%f", dt);
            ROS_INFO("taudot numerator = %f", max_.velocity);
            ROS_INFO("taudot denominator = %f", path_distance.norm());
            ROS_INFO("Error correction = %f", ((max_.pos_error - error.norm())/max_.pos_error));
            ROS_INFO("taudot = %f", taudot);
          }

          tau += dt*taudot;

          if(debug_print & COMPUTE_CONTROL) ROS_INFO("tau = %f", tau);

          // Calculate desired position between waypoints
          Eigen::Vector3d next_waypoint = path_.waypoints[waypoint_index_+1];
          if(debug_print & COMPUTE_CONTROL) ROS_INFO("Moving to waypoint %d at x=%f, y=%f, z=%f",waypoint_index_+1, next_waypoint(0), next_waypoint(1), next_waypoint(2));
          z_r = (1-tau)*path_.waypoints[waypoint_index_] + tau*next_waypoint;
        }
      }
      else {
        // At the final waypoint...
        if(debug_print & COMPUTE_CONTROL) ROS_INFO("At final waypoint");
        z_r = path_.waypoints[path_.waypoint_cnt - 1];
      }

      if(debug_print & COMPUTE_CONTROL) {
        ROS_INFO("z_r: x=%f, y=%f, z=%f", fleet_goal_.x, fleet_goal_.y, fleet_goal_.z);
        ROS_INFO("Waypoint index: %d", waypoint_index_);
      }

      // Assign desired position to the command variable
      if(path_type == WAYPOINT) {
        fleet_goal_.x = z_r(0);
        fleet_goal_.y = z_r(1);
        fleet_goal_.z = z_r(2);
      }

      // >>>>>>> Test: Try making the movement both quick smooth by using max velocity
      // if(target_pos.norm() != 0) {
      //   Eigen::Vector3d desired_movement = saturate_vector(target_pos, max_.velocity*dt, -max_.velocity*dt);
      //   ROS_INFO("max_velocity: %f", max_.velocity);
      //   ROS_INFO("dt: %f", dt);
      //   ROS_INFO("desired_movement: x=%f, y=%f, z=%f", desired_movement(0), desired_movement(1), desired_movement(2));
      //   fleet_goal_.x += desired_movement(0);
      //   fleet_goal_.y += desired_movement(1);
      //   fleet_goal_.z += desired_movement(2);
      // }

      // >>>>> TEST 2: Just try giving it the position and see how fast it can get there
      if(path_type == DIRECT) {
        Eigen::Vector3d desired_pos = target_pos.normalized()*current_radius_;
        fleet_goal_.x = desired_pos(0);
        fleet_goal_.y = desired_pos(1);
        fleet_goal_.z = desired_pos(2);
      }

      double normal_x = xt_.x;
      double normal_y = xt_.y;
      double normal_z = xt_.z;

      // Calculate target angles
      double phi_t = atan2(-1.0*normal_y, normal_z);

      double theta_t = atan2(normal_x, -1.0*normal_z);
      // double psi_t = atan2(l(1),l(0)); // Calculate yaw with respect to line of sight vector
      double psi_t = atan2(normal_y, normal_x); // Calculate yaw with respect to line of sight vector

      double d_rs = distance_start_rotation_;
      double d_rf = distance_finish_rotation_;
      double angle_gain = 0;

      // Ramp the angle gain from 0 to 1 in desired distance interval
      Eigen::Vector3d l = path_.waypoints[path_.waypoint_cnt - 1] - z;
      double L = l.norm();
      if(L <= d_rs && L >= d_rf) {
        angle_gain = (d_rs - L)/(d_rs - d_rf);
      }
      else if(L < d_rf) {
        angle_gain = 1.0; //We should be fully rotated by d_rf
      }


      // fleet_goal_.phi = phi_t*angle_gain;
      fleet_goal_.theta = theta_t*angle_gain;
      double delta_psi = psi_t - fleet_goal_.psi;
      double delta_psi_sat = saturate(delta_psi, max_.psi_rate, -1.0*max_.psi_rate);
      fleet_goal_.psi += delta_psi_sat; // Start tracking yaw from beginning

      // Publish command
      publishCommand();
    }
}

/**
 * Returns the predicted position of the target a given time away from the present
 * @param time - the time into the future we want to predict the target position
 * @return Vector3d - predicted position vector of the target
 */
 // TODO: Do we want to incorporate acceleration or rotations into this calculation?
Eigen::Vector3d InterceptController::targetPredict(double time) {

  // Extrapolate the specified amount of time into the future
  Eigen::Vector3d prediction(xt_.x, xt_.y, xt_.z);

  if(debug_print & TARGET_PREDICT) {
    ROS_INFO("Time = %f", time);
    ROS_INFO("Target position: x=%f, y=%f, z=%f", prediction(0), prediction(1), prediction(2));
  }
  Eigen::Vector3d acceleration(intruder_accel(0), intruder_accel(1), intruder_accel(2));


  // Eigen::Vector3d velocity(xt_.xdot, xt_.ydot, xt_.zdot);
  Eigen::Vector3d velocity(avg_intruder_x_vel, avg_intruder_y_vel, avg_intruder_z_vel);
  if(debug_print & TARGET_PREDICT) {
    ROS_INFO("Actual Velocity: x=%f, y=%f, z=%f", xt_.xdot, xt_.ydot, xt_.zdot);
    ROS_INFO("Avg'd Velocity: x=%f, y=%f, z=%f", velocity(0), velocity(1), velocity(2));
  }
  Eigen::Vector3d vel_with_accel = velocity + time*acceleration;
  if(debug_print & TARGET_PREDICT) ROS_INFO("Velocity with accel: x=%f, y=%f, z=%f", vel_with_accel(0), vel_with_accel(1), vel_with_accel(2));
  // velocity = saturate_vector(vel_with_accel, max_.velocity, -1.0*max_.velocity);
  velocity = saturate_vector(velocity, max_.velocity, -1.0*max_.velocity);
  prediction += time*velocity;
  // We don't care if the intruder is past the "protection radius"
  prediction = saturate_vector(prediction, protection_radius_, 0.0);
  // We don't want to predict the intruder to be below a certain level (about half the net width)
  prediction(2) = saturate(prediction(2), -0.6*fleet_square_width_, -1.0*protection_radius_);

  if(debug_print & TARGET_PREDICT) {
    ROS_INFO("Target velocity: x=%f, y=%f, z=%f", velocity(0), velocity(1), velocity(2));
    ROS_INFO("Target acceleration: x=%f, y=%f, z=%f", intruder_accel(0), intruder_accel(1), intruder_accel(2));
    ROS_INFO("Target prediction: x=%f, y=%f, z=%f", prediction(0), prediction(1), prediction(2));
  }
  // Return the estimated position vector
  return prediction;
}

Path_t InterceptController::getSimpleWaypointPath(Eigen::Vector3d fleet_pos, Eigen::Vector3d target_pos, Eigen::Vector3d v_final) {
  Path_t path;
  int waypoint_cnt = 0;
  double flyby_distance_ = 5.0;

  // Initial position
  path.waypoints[waypoint_cnt] = fleet_pos;
  // ROS_INFO("Waypoint 1: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;
  // Flyby initial position (in front of target)
  path.waypoints[waypoint_cnt] = target_pos - v_final.normalized()*flyby_distance_;
  // ROS_INFO("Waypoint 2: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;
  // Intercept target
  path.waypoints[waypoint_cnt] = target_pos;
  // ROS_INFO("Waypoint 3: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;
  // Flyby end position (past target)
  path.waypoints[waypoint_cnt] = target_pos + v_final.normalized()*flyby_distance_;
  // ROS_INFO("Waypoint 4: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;

  path.waypoint_cnt = waypoint_cnt;

  return path;
}

Path_t InterceptController::getSmoothedWaypointPath(Eigen::Vector3d fleet_pos, Eigen::Vector3d target_pos, Eigen::Vector3d v_final) {
  Path_t path;
  int waypoint_cnt = 0;

  if(v_final.norm() != 0) { // Only normalize if the vector is non-zero
    v_final = v_final.normalized()*max_.velocity;
  }
  Matrix3x4d endpoints;
  endpoints <<  fleet_pos(0), fleet_state_.xdot, target_pos(0), v_final(0),
                fleet_pos(1), fleet_state_.ydot, target_pos(1), v_final(1),
                fleet_pos(2), fleet_state_.zdot, target_pos(2), v_final(2);

  // ROS_INFO("Enpoints matrix:");
  // for(int i = 0; i < 3; i++) {
  //   ROS_INFO("%f, %f, %f, %f", endpoints(i,0), endpoints(i,1), endpoints(i,2), endpoints(i,3));
  // }


  Eigen::Matrix4d coeff_transform;
  coeff_transform <<  1,  0, -3,  2,
                      0,  1, -2,  1,
                      0,  0,  3, -2,
                      0,  0, -1,  1;

  Matrix3x4d path_coeff = endpoints*coeff_transform;

  // Extract debug information
  boost::array<double, 4ul> x_coeff = {path_coeff(0,0), path_coeff(0,1), path_coeff(0,2), path_coeff(0,3)};
  boost::array<double, 4ul> y_coeff = {path_coeff(1,0), path_coeff(1,1), path_coeff(1,2), path_coeff(1,3)};
  boost::array<double, 4ul> z_coeff = {path_coeff(2,0), path_coeff(2,1), path_coeff(2,2), path_coeff(2,3)};
  debug_msg_.x_path_coeff = x_coeff;
  debug_msg_.y_path_coeff = y_coeff;
  debug_msg_.z_path_coeff = z_coeff;

  Eigen::Vector4d Phi;
  // Fill in path waypoints, by selecting points along the trajectory
  for(int i = 0; i < max_.waypoint_cnt; i++) {
    if(i >= MAX_WAYPOINT_CNT) {
      ROS_INFO("WARNING: too many waypoints! Desired %d waypoints; max is %d waypoints", max_.waypoint_cnt, MAX_WAYPOINT_CNT);
      break;
    }
    double tau = ((double) i)/(max_.waypoint_cnt - 1);
    Phi << 1.0, tau, tau*tau, tau*tau*tau;
    path.waypoints[i] = path_coeff*Phi;
    // ROS_INFO("Tau = %f", tau);
    // ROS_INFO("Phi: 0=%f, 1=%f, 2=%f, 3=%f", Phi(0), Phi(1), Phi(2), Phi(3));
    // ROS_INFO("(pathPlanner) Waypoint %d: x=%f, y=%f, z=%f", i, path.waypoints[i](0), path.waypoints[i](1), path.waypoints[i](2));
    waypoint_cnt++;
  }
  path.waypoint_cnt = waypoint_cnt;

  return path;
}

Path_t InterceptController::getProtectionRadiusWaypointPath(Eigen::Vector3d fleet_pos, Eigen::Vector3d target_pos, Eigen::Vector3d v_final) {
  Path_t path;
  int waypoint_cnt = 0;

  // Initial position
  path.waypoints[waypoint_cnt] = fleet_pos;
  waypoint_cnt++;

  // Protection Radius defense position
  path.waypoints[waypoint_cnt] = target_pos.normalized()*protection_radius_;
  waypoint_cnt++;

  path.waypoint_cnt = waypoint_cnt;

  return path;
}

Path_t InterceptController::getAdaptiveProtectionRadiusWaypointPath(Eigen::Vector3d fleet_pos, Eigen::Vector3d target_pos) {
  double now = ros::Time::now().toSec();
  static double prev_time = 0;
  double dt = now - prev_time;
  prev_time = now;

  Path_t path;
  int waypoint_cnt = 0;

  // Calculate theta (target approach angle (relative to line of sight vector))
  fleet_pos << fleet_state_.x, fleet_state_.y, fleet_state_.z;
  target_pos << xt_.x, xt_.y, xt_.z;

  Eigen::Vector3d v_t(xt_.xdot, xt_.ydot, xt_.zdot);
  Eigen::Vector3d v_f(fleet_state_.xdot, fleet_state_.ydot, fleet_state_.zdot);
  Eigen::Vector3d d = target_pos - current_radius_*target_pos.normalized();
  double v_t_norm = v_t.norm();
  double d_norm = d.norm();
  double theta_net = atan(fleet_square_width_/6 / d_norm); // Angle from target to edge of "capture region" of the net
  double theta;
  if(v_t_norm != 0 && d_norm != 0) {
     // Only count theta deviations greater than the angle to the edge of the "capture region"
     // Any angle within the range of the net is counted as zero.
    theta = saturate(fabs(d.dot(v_t)/(v_t_norm*d_norm)) - theta_net, M_PI, 0.0);
  }
  else {
    theta = 0.0;
  }

  // Calculate phi (fleet divergent angle) using bisection search
  double phi1 = -1.0*M_PI/2.0;
  double phi2 = M_PI/2.0;
  double phi = (phi1 + phi2)/2.0;
  // double v_fleet = v_f.norm();
  double v_fleet = max_.velocity;
  double v_intr = v_t.norm();
  double t_fleet = (d_norm*(sin(theta)))/(v_fleet*(cos(theta + phi))) + max_.control_delay;
  double t_intr = (d_norm*(cos(phi)) / (v_intr*(cos(theta+phi))));
  double diff = t_intr - t_fleet;


  double epsilon = 0.01;
  int timeout = 5000;
  while(fabs(diff) > epsilon && theta != 0) { // If theta = 0, skip this loop
    phi = (phi1 + phi2)/2.0;
    t_fleet = (d_norm*(sin(theta)))/(v_fleet*(cos(theta + phi))) + max_.control_delay;
    t_intr = (d_norm*(cos(phi))/(v_intr*(cos(theta+phi))));
    diff = t_intr - t_fleet;
    if(diff < 0) {
      phi1 = phi;
    }
    else {
      phi2 = phi;
    }

    timeout--;
    if(timeout == 0) {
      if(debug_print & ADAPTIVE_RADIUS) {
        ROS_INFO("ERROR: phi computation timeout");
      }
      // phi = 0.0;
      break;
    }
  }

  double delta_radius;
  if(theta == 0) {
    // Don't change radius if theta is zero
    delta_radius = 0;
  }
  else {
    delta_radius = -1.0*max_.velocity*dt*sin(phi);
  }


  current_radius_ = saturate(delta_radius + current_radius_, max_radius_, min_radius_);
  if(debug_print & ADAPTIVE_RADIUS) {
    ROS_INFO("PHI COMPUTATIONS >>>>>>>>>>>>>>>>>>>>>>");
    ROS_INFO("d = %f", d_norm);
    ROS_INFO("v_fleet = %f", v_fleet);
    ROS_INFO("v_intr = %f", v_intr);
    ROS_INFO("Theta_deg = %f", theta/M_PI*180.0);
    ROS_INFO("Phi_deg = %f", phi/M_PI*180.0);
    ROS_INFO("Delta_radius = %f", delta_radius);
    ROS_INFO("Current radius = %f", current_radius_);
  }


  // Initial position
  path.waypoints[waypoint_cnt] = fleet_pos;
  // ROS_INFO("Waypoint 1: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;
  // Protection Radius defense position
  path.waypoints[waypoint_cnt] = target_pos.normalized()*current_radius_;
  // ROS_INFO("Waypoint 2: x=%f, y=%f, z=%f", path.waypoints[waypoint_cnt](0), path.waypoints[waypoint_cnt](1), path.waypoints[waypoint_cnt](2));
  waypoint_cnt++;

  path.waypoint_cnt = waypoint_cnt;

  return path;
}

/**
 * Returns a path from the fleet to the target with the velocity at time of interception begin "v_final"
 * @return Path - struct containing array of waypoints making up the path and an integer with the number of waypoints
 */
Path_t InterceptController::planPath(Eigen::Vector3d fleet_pos, Eigen::Vector3d target_pos, Eigen::Vector3d v_final) {
  Path_t path;

  // ============= Four waypoint implementation ========================
  // path = getSimpleWaypointPath(fleet_pos, target_pos, v_final);

  // // ============= Smoothed trajectory implementation ========================
  // path = getSmoothedWaypointPath(fleet_pos, target_pos, v_final);

  // // ============= Protection Radius Defense implementation ========================
  // path = getProtectionRadiusWaypointPath(fleet_pos, target_pos, v_final);

  // ============= Adaptive Protection Radius Defense implementation ========================
  path = getAdaptiveProtectionRadiusWaypointPath(fleet_pos, target_pos);

  return path;
}

/**
 * Returns a the minimum intercept time, given the positions of the fleet and target
 * @return - the optimal intercept time
 */
 // TODO: should I use the actual current velocity, or some precomputed constant value?
double InterceptController::computeInterceptTime() {
  double v = max_.velocity;
  double t1 = 0.0;
  double t2 = 0.0;
  Eigen::Vector3d z(fleet_state_.x, fleet_state_.y, fleet_state_.z);
  Eigen::Vector3d v_intercept(xt_.x, xt_.y, xt_.z); // Let the position vector determine the intercept trajectory

  // Initialize t1 and t2 for bisection search
  double time_step = 10.0;
  while (time_delta(v, t2, z, v_intercept) <= 0.0)  {
    t2 += time_step;
    if(t2 > protection_radius_/v) {
      ROS_INFO("Breaking out of loop");
      // We are probably in an infinite loop -- this means the target is "running away"
      // For now, just return the time it takes to get to the current position of the target
      Eigen::Vector3d target_pos(xt_.x, xt_.y, xt_.z);
      Path_t current_path = planPath(z, target_pos, v_intercept);
      return pathLength(&current_path)/v;
    }
  }

  // Perform bisection search
  double epsilon = 0.001;
  double time_diff = time_delta(v, (t1 + t2)/2.0, z, v_intercept);
  double abs_time_diff = time_diff*sgn(time_diff);
  while(abs_time_diff > epsilon) {
    if (time_diff > 0.0) {
      t2 = (t1 + t2)/2.0;
    }
    else {
      t1 = (t1 + t2)/2.0;
    }
    // Calculate new time difference
    time_diff = time_delta(v, (t1 + t2)/2.0, z, v_intercept);
    abs_time_diff = time_diff*sgn(time_diff);
  }

  // return optimal value
  return (t1 + t2)/2.0;
}

double InterceptController::time_delta(double v, double T, Eigen::Vector3d z, Eigen::Vector3d v_intercept) {
  Path_t path = planPath(z, targetPredict(T), v_intercept);
  return (v*T - pathLength(&path));
}

/**
 * Returns a the length of the given path
 * @return double - length (in meters) of that path
 */
double InterceptController::pathLength(Path_t* path) {
  int point_cnt = path->waypoint_cnt;
  double length = 0.0;
  for(int i = 0; i < point_cnt - 1; i++) {
    Eigen::Vector3d diff = path->waypoints[i+1] - path->waypoints[i];
    length += diff.norm();
  }
  return length;
}

void InterceptController::publishCommand()
{
  command_msg_.header.stamp = ros::Time::now();
  debug_msg_.header.stamp = ros::Time::now();

  // Saturate the goal to be far enough above ground to never crash, even if fully rotated.
  fleet_goal_.z = saturate(fleet_goal_.z, -1.0*(fleet_square_width_/2.0 + 2.0), -10000);

  command_msg_.pose.pose.position.x = fleet_goal_.x;
  command_msg_.pose.pose.position.y = fleet_goal_.y;
  command_msg_.pose.pose.position.z = fleet_goal_.z;

  geometry_msgs::Quaternion odom_quat = tf::createQuaternionMsgFromRollPitchYaw(fleet_goal_.phi, fleet_goal_.theta, fleet_goal_.psi);
  command_msg_.pose.pose.orientation = odom_quat;

  fleet_goal_pub_.publish(command_msg_);

  // Also publish debug information
  path_coeff_pub_.publish(debug_msg_);
}

void InterceptController::resetStates() {
  fleet_goal_.x = 0;
  fleet_goal_.y = 0;
  fleet_goal_.z = 0;
  fleet_goal_.xdot = 0.0;
  fleet_goal_.ydot = 0.0;
  fleet_goal_.zdot = 0.0; // must initialize some velocity
  fleet_goal_.phi = 0;
  fleet_goal_.theta = 0;
  fleet_goal_.psi = 0;
  fleet_goal_.phidot = 0;
  fleet_goal_.thetadot = 0;
  fleet_goal_.psidot = 0;

  fleet_state_.x = 0;
  fleet_state_.y = 0;
  fleet_state_.z = 0;
  fleet_state_.xdot = 0;
  fleet_state_.ydot = 0;
  fleet_state_.zdot = 0;

  uav1_state_.x = 0;
  uav1_state_.y = 0;
  uav1_state_.z = 0;
  uav1_state_.xdot = 0;
  uav1_state_.ydot = 0;
  uav1_state_.zdot = 0;

  uav2_state_.x = 0;
  uav2_state_.y = 0;
  uav2_state_.z = 0;
  uav2_state_.xdot = 0;
  uav2_state_.ydot = 0;
  uav2_state_.zdot = 0;

  uav3_state_.x = 0;
  uav3_state_.y = 0;
  uav3_state_.z = 0;
  uav3_state_.xdot = 0;
  uav3_state_.ydot = 0;
  uav3_state_.zdot = 0;

  uav4_state_.x = 0;
  uav4_state_.y = 0;
  uav4_state_.z = 0;
  uav4_state_.xdot = 0;
  uav4_state_.ydot = 0;
  uav4_state_.zdot = 0;
}

double InterceptController::saturate(double x, double max, double min)
{
  x = (x > max) ? max : x;
  x = (x < min) ? min : x;
  return x;
}

Eigen::Vector3d InterceptController::saturate_vector(Eigen::Vector3d vector, double max, double min)
{
  if(vector.norm() > 0) {
    return vector.normalized()*saturate(vector.norm(),max,min);
  }
  else {
    return vector;
  }
}

double InterceptController::sgn(double x)
{
  return (x >= 0.0) ? 1.0 : -1.0;
}

double InterceptController::min(double x, double y) {
  return (x < y) ? x : y;
}


} // namespace intercept_controller
