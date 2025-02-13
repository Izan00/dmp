#include "dmp/dmp.h"
using namespace dmp;

std::vector<DMPData> active_dmp_list;

bool lfdCallback(LearnDMPFromDemo::Request  &req,
			     LearnDMPFromDemo::Response &res )
{
	learnFromDemo(req.demo, req.k_gains, req.d_gains, req.num_bases, res.dmp_list);
	res.tau = req.demo.times[req.demo.times.size()-1];
	return true;
}

bool planCallback(GetDMPPlan::Request  &req,
			      GetDMPPlan::Response &res )
{
	std::vector<std::vector<double>> obstacle;
	if (req.obstacle.size()>3)
		for(int i=0; i<req.obstacle.size();i+=3)
			obstacle.push_back(std::vector<double>{req.obstacle[i],req.obstacle[i+1],req.obstacle[i+2]});
	else if(req.obstacle.size()==3)
		obstacle.push_back(req.obstacle);
	generatePlan(active_dmp_list, req.x_0, req.x_dot_0, req.t_0, req.goal, req.goal_thresh,
			     req.seg_length, req.tau, req.dt, req.integrate_iter, res.plan, res.at_goal, 
				 obstacle, req.beta, req.gamma, req.k, req.scale_m, req.scale_n);
	return true;
}

bool activeCallback(SetActiveDMP::Request &req,
					SetActiveDMP::Response &res )
{
	active_dmp_list = req.dmp_list;
	res.success = true;
	return true;
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "dmp_server");
	ros::NodeHandle n;

	ros::ServiceServer service1 = n.advertiseService("learn_dmp_from_demo", lfdCallback);
	ros::ServiceServer service2 = n.advertiseService("get_dmp_plan", planCallback);
	ros::ServiceServer service3 = n.advertiseService("set_active_dmp", activeCallback);
	ROS_INFO("DMP services now ready");
	ros::spin();

	return 0;
}
