/* ROS interface for implementing full path given by multiple pse goals using moveit functionality.
	
*/
//Header
#include "include/ikplanner.hpp"

// ROS message types.
#include <geometry_msgs/Pose.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>

// ROS and "MoveIt!" packages. Some of these are probably not needed. 
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/kinematic_constraints/kinematic_constraint.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/planning_interface/planning_interface.h>
#include <moveit/move_group_interface/move_group.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit_msgs/PlanningScene.h>
#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include <moveit_msgs/AttachedCollisionObject.h>
#include <moveit_msgs/GetStateValidity.h>
#include <moveit_msgs/DisplayRobotState.h>
#include <moveit/kinematics_base/kinematics_base.h>
#include <pluginlib/class_list_macros.h>
#include <pluginlib/class_loader.h>

#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>

//Inverse kinematics 
//#include <descartes_moveit/moveit_state_adapter.h>
#include <urdf/model.h>
#include <eigen_conversions/eigen_kdl.h>
#include <kdl_parser/kdl_parser.hpp>
#include <tf_conversions/tf_kdl.h>
#include <ur_kinematics/ur_kin.h>

//Action lib
//#include <actionlib/client/simple_action_client.h>



int main(int argc, char **argv)
{
/*
	Setup rosnode and spinner.
*/
	ros::init(argc, argv, "path_sim");
	ros::NodeHandle n;	
	ros::AsyncSpinner spinner(1);
	spinner.start();

/*
	Load urdf, setup joint model groups and kinematic state. 
*/
	robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
  robot_model::RobotModelPtr robot_model = robot_model_loader.getModel();
	ROS_INFO("Model frame: %s", robot_model->getModelFrame().c_str());
	planning_scene::PlanningScenePtr planning_scene(new planning_scene::PlanningScene(robot_model));
	
	robot_state::RobotStatePtr kinematic_state(new robot_state::RobotState(robot_model));
	kinematic_state->setToDefaultValues();

 	//Get joint model names and group values
	const robot_state::JointModelGroup * joint_model_group = robot_model->getJointModelGroup("manipulator");
	const std::vector<std::string>  joint_names = joint_model_group->getJointModelNames();
	std::vector<double> joint_values;	
	kinematic_state->copyJointGroupPositions(joint_model_group, joint_values);

	ROS_INFO("Robot joint group loaded... names of joints are as follows:"); 
	for(std::size_t i = 0; i < joint_names.size(); ++i)
	{
  	ROS_INFO("Joint %s", joint_names[i].c_str());
	}	


/* -----------------------------Loading planner --------------------- */
	boost::scoped_ptr<pluginlib::ClassLoader<planning_interface::PlannerManager> > planner_plugin_loader;
  planning_interface::PlannerManagerPtr planner_instance;
  std::string planner_plugin_name = "ompl_interface/OMPLPlanner";

  // Get name from the ROS param server, and then load the planner
  if (!n.getParam("planning_plugin", planner_plugin_name))
    ROS_FATAL_STREAM("Could not find planner plugin name");
  try{
    planner_plugin_loader.reset(new pluginlib::ClassLoader<planning_interface::PlannerManager>("moveit_core", "planning_interface::PlannerManager"));
  }catch(pluginlib::PluginlibException& ex){
    ROS_FATAL_STREAM("Exception while creating planning plugin loader " << ex.what());
  }try{
    planner_instance.reset(planner_plugin_loader->createUnmanagedInstance(planner_plugin_name));
    if (!planner_instance->initialize(robot_model, n.getNamespace()))
      ROS_FATAL_STREAM("Could not initialize planner instance");
    ROS_INFO_STREAM("Using planning interface '" << planner_instance->getDescription() << "'");
  }catch(pluginlib::PluginlibException& ex){
    const std::vector<std::string> &classes = planner_plugin_loader->getDeclaredClasses();
    std::stringstream ss;
    for (std::size_t i = 0 ; i < classes.size() ; ++i)
      ss << classes[i] << " ";
    ROS_ERROR_STREAM("Exception while loading planner '" << planner_plugin_name << "': " << ex.what() << std::endl
                     << "Available plugins: " << ss.str());
	}

/* ----------------- End planner loader --------*/

	/* Moveit group interface for actually moving the robot */	
	moveit::planning_interface::MoveGroup group("manipulator");

	/* Specify tolerance and declare request and response */
	double tolerance_below = 1e-3;
	double tolerance_above = 1e-3;	

	planning_interface::MotionPlanRequest req;
	req.group_name = "manipulator";
	planning_interface::MotionPlanResponse res;
	moveit_msgs::MotionPlanResponse response;
/* Specify bounds on the workspace */

	double scale = 100; //Viewpoint planning done in cm, but ros operates with m.

	req.workspace_parameters.min_corner.z = -0.05;
	req.workspace_parameters.min_corner.x = req.workspace_parameters.min_corner.y = -0.7;
	req.workspace_parameters.max_corner.x = req.workspace_parameters.max_corner.y = req.workspace_parameters.max_corner.z =  0.7;

	/* Open and terate through path composed of several pose targets */	

	std::ifstream f("/home/magnus/Documents/path_ctrl/src/plan_pkg/paths/detailedPath.csv");
	if( !f.is_open()){
		ROS_ERROR("Could not open path, exiting.."); 
		return 0;
	}
	ROS_INFO("file is open");
	std::string line;	

	int count = 0;
	
	double tf_matrix[4][4];

	//KDL::Frame kdl_pose;

	while(std::getline(f, line))
	{	
		/*
			IDEA: Rewrite this to operate on start position and goal position. 
						- Test if that makes anything more simple. 
		*/

		double pose [6];
		getPose(line, pose);

		tf::Quaternion q = tf::createQuaternionFromRPY(pose[3], pose[4],pose[5]);
	
		geometry_msgs::Pose viewpoint;

		pose[0] = pose[0]/scale;
		pose[1] = pose[1]/scale;
		pose[2] = pose[2]/scale;

		viewpoint.orientation.x = q.x();
		viewpoint.orientation.y = q.y();
		viewpoint.orientation.z = q.z();
		viewpoint.orientation.w = q.w();
		viewpoint.position.x = (pose[0])+ 0.2;
		viewpoint.position.y = (pose[1]);
		viewpoint.position.z = (pose[2]);

		getTransform(pose,tf_matrix);
		//Need to fix the pose matrix.
		double q_ik_sols[8][6];
		std::vector< std::vector<double> > valid_solutions;

		//int num_sols = ur_kinematics::inverse((double*) tf_matrix,(double*) q_ik_sols, 0.0f); //Last variable defaults to 0.0f, but is added to remember that it is there. 
		//Need to check validity of theese solutions though...
		//for(int i = 0; i < num_sols; i++){
		//	std::vector<double> val_sol;
			// Push back points one by one, checking validity, changing them if necessary. 						
			// Check for nans
			// Check for limit violations, and if +/- 2pi solves this violation.  
			// 
		//}
		
		//ROS_INFO("Inverse kinematics gave: %i, solutions where %i where valid", num_sols, (int) valid_solutions.size()); 
		
		
		bool found_ik = kinematic_state->setFromIK(joint_model_group, viewpoint, 10, 0.1);
		

		/*
			Consider using ros descartes_moveit wrapper, as it contains this type of functionallity.
		*/

		if (found_ik)
		{
			
			//joint_values.clear();
			/*			
			joint_values.push_back(q_ik_sols[0][0]);
			joint_values.push_back(q_ik_sols[0][1]);
			joint_values.push_back(q_ik_sols[0][2]);
			joint_values.push_back(q_ik_sols[0][3]);
			joint_values.push_back(q_ik_sols[0][4]);
			joint_values.push_back(q_ik_sols[0][5]);			
			*/
  		kinematic_state->copyJointGroupPositions(joint_model_group, joint_values);

			

			ROS_INFO("Succesful IK for viewpoint number: %i", count); 
			planning_scene->setCurrentState(*group.getCurrentState());

			//Define goals etc.
			robot_state::RobotState goal_state(robot_model);
			goal_state.setJointGroupPositions(joint_model_group, joint_values);
  		moveit_msgs::Constraints joint_goal = kinematic_constraints::constructGoalConstraints(goal_state, joint_model_group, tolerance_below, tolerance_above);
  		req.goal_constraints.clear();
			req.goal_constraints.push_back(joint_goal);		

			/* Call the Planner */
			planning_interface::PlanningContextPtr context = planner_instance->getPlanningContext(planning_scene, req, res.error_code_);
  		context->solve(res);
  		/* Check that the planning was successful */
  		if(res.error_code_.val != res.error_code_.SUCCESS)
  		{
   			ROS_ERROR("Could not compute plan successfully... printing goal");
				for(std::size_t i=0; i < joint_names.size(); ++i)
				{
    			ROS_INFO("Joint %s: %f", joint_names[i].c_str(), joint_values[i]);
  			}
		
				//return 0; 
				continue;
			}else{
				/*
					Apparantly the planning was sucessfull. 								
				*/
				res.getMessage(response);				
				moveit::planning_interface::MoveGroup::Plan plan;
				moveit_msgs::RobotTrajectory trajectory = response.trajectory;
				
				// Using suggested fix to solve time parameterization of the proposed trajectory 
				robot_trajectory::RobotTrajectory rt(group.getCurrentState()->getRobotModel(), "manipulator");
  			rt.setRobotTrajectoryMsg(*group.getCurrentState(), trajectory);
  			trajectory_processing::IterativeParabolicTimeParameterization iptp;
  			bool success = iptp.computeTimeStamps(rt);
  			rt.getRobotTrajectoryMsg(trajectory);
				
				//Finaly have a properly parametarized trajectory. 
				plan.start_state_ = response.trajectory_start;			
				plan.trajectory_ = trajectory;
				
				//And this works somehow.
				group.execute(plan);
				//ros::Duration(0.5).sleep();
				

			}



		}else{
  		ROS_ERROR("Did not find IK solution for viewpoint %i", count);
		}
		count++;
		
	}
return 0;
}


//Simple help function
void getPose(std::string s, double* v){
	int p = 0;
	int q = 0;
	for(int i = 0; i< s.size(); i++){
		if(s[i] == ',' || i == s.size() -1){
			char tab2[16];
			strcpy(tab2, s.substr(p,i-p).c_str());
			v[q] = std::strtod(tab2, NULL);
			p = i+1;
			q++; 
		}
	}
	return;
}

//Simpe but maybe unneccesary help function.
void getTransform(double * pose, double tf[][4]){
	double roll = pose[3];
	double pitch = pose[4];
	double yaw = pose[5];
	
//Standard Rot matrix from RPY.
	tf[0][0] = cos(yaw)*cos(pitch);
	tf[0][1] = cos(yaw)*sin(pitch)*sin(roll) - sin(yaw)*cos(roll);
	tf[0][2] = cos(yaw)*sin(pitch)*cos(roll) + sin(yaw)*sin(roll);

	tf[1][0] = sin(yaw)*cos(pitch);
	tf[1][1] = sin(yaw)*sin(pitch)*sin(roll) + cos(yaw)*cos(roll);	
	tf[1][2] = sin(yaw)*sin(pitch)*cos(roll) - cos(yaw)*sin(roll);

	tf[2][0] = -sin(pitch);
	tf[2][1] = cos(pitch)*sin(roll);
	tf[2][2] = cos(pitch)*cos(roll);

//position (4th column)
	tf[0][3] = pose[0];
	tf[1][3] = pose[1];
	tf[2][3] = pose[2]; 

	return;
}


