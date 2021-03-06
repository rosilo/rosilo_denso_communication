/*
# Copyright (c) 2016-2020 Murilo Marques Marinho
#
#    This file is part of rosilo_denso_communcation.
#
#    rosilo_denso_communcation is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Lesser General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    rosilo_denso_communcation is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Lesser General Public License for more details.
#
#    You should have received a copy of the GNU Lesser General Public License
#    along with rosilo_denso_communcation.  If not, see <https://www.gnu.org/licenses/>.
#
# ################################################################
#
#   Author: Murilo M. Marinho, email: murilo@nml.t.u-tokyo.ac.jp
#
# ################################################################*/

#include "rosilo_denso_communication_node.h"
#include <rosilo_conversions/rosilo_conversions.h>
#include <dqrobotics/utils/DQ_Math.h>

/*********************************************
 * SIGNAL HANDLER
 * *******************************************/
#include<signal.h>
#include<atomic>
static std::atomic_bool kill_this_process(false);
void SigIntHandler(int)
{
    kill_this_process = true;
    //ROS_INFO_STREAM("SHUTDOWN SIGNAL RECEIVED");
}

/*********************************************
 * GLOBAL SCOPE FUNCTIONS (INCLUDING MAIN)
 * *******************************************/
rosilo::DensoCommunication* create_instance_from_ros_parameter_server()
{
    ros::NodeHandle nodehandle;
    std::string robot_ip_address;
    int port;
    int thread_sampling_time_nsec;
    int thread_estimated_computation_time_upper_bound_nsec;
    int thread_relative_deadline_nsec;
    bool enable_real_time_scheduling;
    bool read_only;
    ROS_INFO_STREAM("Trying to load Denso Communication Node parameters for node " << ros::this_node::getName());
    if(!nodehandle.getParam(ros::this_node::getName()+"/robot_ip_address",robot_ip_address)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/port",port)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/thread_sampling_time_nsec",thread_sampling_time_nsec)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/thread_estimated_computation_time_upper_bound_nsec",thread_estimated_computation_time_upper_bound_nsec)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/enable_real_time_scheduling",enable_real_time_scheduling)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/thread_relative_deadline_nsec",thread_relative_deadline_nsec)){return nullptr;}
    if(!nodehandle.getParam(ros::this_node::getName()+"/read_only",read_only)){return nullptr;}

    return new rosilo::DensoCommunication(robot_ip_address,port,thread_sampling_time_nsec,thread_estimated_computation_time_upper_bound_nsec,thread_relative_deadline_nsec,enable_real_time_scheduling,read_only,&kill_this_process);
}

int main(int argc, char** argv)
{
    if(signal(SIGINT, SigIntHandler) == SIG_ERR)
    {
        std::cout << "Error setting the signal int handler." << std::endl;
        return 1;
    }

    ros::init(argc, argv,"uninitialized_denso_communication_node",ros::init_options::NoSigintHandler);

    rosilo::DensoCommunication* dc = create_instance_from_ros_parameter_server();
    if(dc != nullptr)
    {
        dc->control_loop();
        delete dc;
    }
    else
    {
        ROS_ERROR_STREAM("Unable to read parameter from parameter server.");
    }

    return 0;
}

namespace rosilo
{

/*********************************************
 * CLASS METHODS
 * *******************************************/
void DensoCommunication::updateTargetJointPositionsCallback(const std_msgs::Float64MultiArray::ConstPtr& msg)
{
    target_joint_positions_ = VectorXd::Map(&msg->data[0], msg->data.size());
}

void DensoCommunication::publishJointStates(const VectorXd& joint_positions, const VectorXd& joint_velocities)
{
    publisher_joint_state_msg_.header = std_msgs::Header();
    publisher_joint_state_msg_.position = std::vector<double>(joint_positions.data(),joint_positions.data()+6);
    publisher_joint_state_msg_.velocity = std::vector<double>(joint_velocities.data(),joint_velocities.data()+6);
    publisher_joint_state_msg_.name = {"J1","J2","J3","J4","J5","J6"};
    publisher_joint_state_.publish(publisher_joint_state_msg_);
}

void DensoCommunication::publishToolPose(const DQ &tool_pose)
{
    publisher_tool_pose_msg_.header = std_msgs::Header();
    publisher_tool_pose_msg_ = rosilo::dq_to_geometry_msgs_pose_stamped(tool_pose);
    publisher_tool_pose_.publish(publisher_tool_pose_msg_);
}

//Constructor
DensoCommunication::DensoCommunication(const std::string& robot_ip_address, const int& port, const int thread_sampling_time_nsec, const int thread_estimated_computation_time_upper_bound_nsec, const int thread_relative_deadline_nsec, const bool enable_realtime_scheduling, bool read_only, std::atomic_bool *kill_this_node)
{
    read_only_ = read_only;
    realtime_scheduling_enabled_ = enable_realtime_scheduling;

    kill_this_node_ = kill_this_node;

    node_prefix_ = ros::this_node::getName();

    subscriber_node_handle_.setCallbackQueue(&subscriber_callback_queue_);
    subscriber_target_joint_positions_ = subscriber_node_handle_.subscribe(node_prefix_+"/set/target_joint_positions", 1, &DensoCommunication::updateTargetJointPositionsCallback, this);

    publisher_node_handle_.setCallbackQueue(&publisher_callback_queue_);
    publisher_joint_state_ = publisher_node_handle_.advertise<sensor_msgs::JointState>(node_prefix_+"/get/joint_state",1);
    publisher_tool_pose_   = publisher_node_handle_.advertise<geometry_msgs::PoseStamped>(node_prefix_+"/get/tool_pose",1);

    datalogger_node_handle_.setCallbackQueue(&datalogger_callback_queue_);
    datalogger_ = std::unique_ptr<rosilo::DataloggerInterface>(new rosilo::DataloggerInterface(datalogger_node_handle_,10));

    rosilo::RobotDriverDensoConfiguration robot_configuration;
    robot_configuration.ip_address = robot_ip_address;
    robot_configuration.port = port;
    robot_configuration.speed = 100.0;
    robot_ = std::unique_ptr<rosilo::RobotDriverDenso>(new rosilo::RobotDriverDenso(robot_configuration,kill_this_node_));

    //Initialize vectors
    target_joint_positions_  = VectorXd::Zero(6);
    last_joint_positions_    = VectorXd::Zero(6);
    joint_positions_         = VectorXd::Zero(6);
    joint_velocities_        = VectorXd::Zero(6);

    //Initialize ros messages
    publisher_joint_state_msg_.effort.resize(6,0.0);
    publisher_joint_state_msg_.position.resize(6,0.0);
    publisher_joint_state_msg_.velocity.resize(6,0.0);

    thread_estimated_computation_time_upper_bound_nsec_ = thread_estimated_computation_time_upper_bound_nsec;
    thread_relative_deadline_nsec_ = thread_relative_deadline_nsec;
    thread_sampling_time_nsec_   = thread_sampling_time_nsec;
    thread_sampling_time_nsec_d_ = double(thread_sampling_time_nsec_);
    thread_sampling_time_sec_d_  = thread_sampling_time_nsec_d_/rosilo::NSEC_TO_SEC_D;
    clock_             =  std::unique_ptr<rosilo::Clock>(new rosilo::Clock(thread_sampling_time_nsec_));
}

int DensoCommunication::control_loop()
{
    try{
        clock_->init();
        connect();
        //if(!read_only_)
        //{
        //    motorOn();
        //}

        while(not (*kill_this_node_))
        {
            //Sleep
            clock_->update_and_sleep();

            //Check for new topic messages
            subscriber_callback_queue_.callAvailable();
            if(!read_only_)
            {
                //Store last joint positions
                last_joint_positions_ = joint_positions_;
                //Send desired joint positions and get the current joint positions
                robot_->set_target_joint_positions(target_joint_positions_);
                joint_positions_ = robot_->get_joint_positions();
                //std::tie(joint_positions_, robot_communication_ok_) = robot_->set_and_get_joint_positions(rad2deg(target_joint_positions_)); //Convert to DEGREES before sending
                //joint_positions_ = deg2rad(joint_positions_); //Convert to RADIANS
                if(!robot_communication_ok_)
                {
                    ROS_ERROR_STREAM("Error setting joint positions of robot " << node_prefix_);
                    break;
                }
            }
            else
            {
                //Store last joint positions
                last_joint_positions_ = joint_positions_;
                //Get current joint positions DEGREES
                //std::tie(joint_positions_, robot_communication_ok_) = robot_->get_joint_positions();
                joint_positions_ = robot_->get_joint_positions();
                //joint_positions_ = deg2rad(joint_positions_);//Convert to RADIANS
                if(!robot_communication_ok_)
                {
                    ROS_ERROR_STREAM("Error getting joint positions from robot " << node_prefix_);
                    break;
                }
                //Get Tool Pose (FROM RC8)
                try {
                    tool_pose_ = robot_->get_end_effector_pose_dq();
                    if(!robot_communication_ok_)
                    {
                        ROS_ERROR_STREAM("Error getting end effector pose from robot " << node_prefix_);
                        break;
                    }
                    publishToolPose(tool_pose_);
                } catch (const std::range_error& e) {
                    ROS_WARN_STREAM("Error converting RC8 end effector to DQ." + std::string(e.what()));
                }
            }

            //Calculate average joint velocity in the sampling time
            joint_velocities_ = (joint_positions_-last_joint_positions_)/(clock_->get_effective_thread_sampling_time_sec());

            publishJointStates(joint_positions_,joint_velocities_);
            publisher_callback_queue_.callAvailable();

            //Send data to datalogger
            datalogger_->log(node_prefix_.substr(1)+"_joint_positions",joint_positions_);
            datalogger_->log(node_prefix_.substr(1)+"_joint_velocities",joint_velocities_);
            datalogger_->log(node_prefix_.substr(1)+"_target_joint_positions",target_joint_positions_);
            datalogger_->log(node_prefix_.substr(1)+"_computational_time",clock_->get_computation_time());
            datalogger_->log(node_prefix_.substr(1)+"_desired_sampling_time",clock_->get_desired_thread_sampling_time_sec());
            datalogger_callback_queue_.callAvailable();

        }//End while not kill this node
    } catch (const std::exception& e) {
        ROS_ERROR_STREAM("Exception caught: " << e.what());
    }
    disconnect();

    return 0;
}//End function

void DensoCommunication::connect()
{
    try
    {
        robot_->initialize();
        robot_->connect();
        //The DENSO robot gives us garbage joint positions initially and afaik
        //we cannot know whether they are garbage in a smart way. Therefore we just
        //keep reading them until the robot gives us something different from zero
        VectorXd local_joint_positions = VectorXd::Zero(6);
        while(local_joint_positions.norm() < 1 && not (*kill_this_node_))
        {
            local_joint_positions = robot_->get_joint_positions();
        }
        //Initialize the robot buffers
        joint_positions_        = deg2rad(local_joint_positions);
        target_joint_positions_ = deg2rad(local_joint_positions);
    }
    catch(const std::runtime_error& e)
    {
        throw e;
    }
}

void DensoCommunication::disconnect()
{
    robot_->deinitialize();
    robot_->disconnect();
}


void DensoCommunication::shutdown()
{
    (*kill_this_node_) = true;
}

}
