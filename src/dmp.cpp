/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Scott Niekum
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

/**
  * \author Scott Niekum
  */


#include "dmp/dmp.h"
using namespace std;

#include <iostream>
#include <cmath>
#include <Eigen/Dense>

namespace dmp{

#define MAX_PLAN_LENGTH 1000

double alpha = -log(0.01); //Ensures 99% phase convergence at t=tau

/**
 * @brief Calculate an exp-decaying 1 to 0 phase based on time and the time scaling constant tau
 * @param[in] curr_time The current time in seconds from the start of DMP execution / trajectory
 * @param[in] tau The DMP time scaling constant
 * @return A zero to one phase
 */
double calcPhase(double curr_time, double tau)
{
	return exp(-(alpha/tau)*curr_time);
}


/**
 * @brief Given a single demo trajectory, produces a multi-dim DMP
 * @param[in] demo An n-dim demonstration trajectory
 * @param[in] k_gains A proportional gain for each demo dimension
 * @param[in] d_gains A vector of differential gains for each demo dimension
 * @param[in] num_bases The number of basis functions to use for the fxn approx (i.e. the order of the Fourier series)
 * @param[out] dmp_list An n-dim list of DMPs that are all linked by a single canonical (phase) system
 */
void learnFromDemo(const DMPTraj &demo,
				   const vector<double> &k_gains,
				   const vector<double> &d_gains,
				   const int &num_bases,
				   vector<DMPData> &dmp_list)
{
	//Determine traj length and dim
	int n_pts = demo.points.size();
	if(n_pts < 1){
		ROS_ERROR("Empty trajectory passed to learn_dmp_from_demo service!");
		return;
	}
	int dims = demo.points[0].positions.size();
	double tau = demo.times[n_pts-1];

	double *x_demo = new double[n_pts];
	double *v_demo = new double[n_pts];
	double *v_dot_demo = new double[n_pts];
	double *f_domain = new double[n_pts];
	double *f_targets = new double[n_pts];
	//FunctionApprox *f_approx = new LinearApprox();
	FunctionApprox *f_approx = new FourierApprox(num_bases);

	//Compute the DMP weights for each DOF separately
	for(int d=0; d<dims; d++){
		double curr_k = k_gains[d];
		double curr_d = d_gains[d];
		double x_0 = demo.points[0].positions[d];
		double goal = demo.points[n_pts-1].positions[d];
		x_demo[0] = demo.points[0].positions[d];
		v_demo[0] = 0;
		v_dot_demo[0] = 0;

		//Calculate the demonstration v and v dot by assuming constant acceleration over a time period
		for(int i=1; i<n_pts; i++){
			x_demo[i] = demo.points[i].positions[d];
			double dx = x_demo[i] - x_demo[i-1];
			double dt = demo.times[i] - demo.times[i-1];
			v_demo[i] = dx/dt;
			v_dot_demo[i] = (v_demo[i] - v_demo[i-1]) / dt;
		}

		//Calculate the target pairs so we can solve for the weights
		for(int i=0; i<n_pts; i++){
			double phase = calcPhase(demo.times[i],tau);
			f_domain[i] = demo.times[i]/tau;  //Scaled time is cleaner than phase for spacing reasons
			f_targets[i] = ((tau*tau*v_dot_demo[i] + curr_d*tau*v_demo[i]) / curr_k) - (goal-x_demo[i]) + ((goal-x_0)*phase);
			f_targets[i] /= phase; // Do this instead of having fxn approx scale its output based on phase
		}

		//Solve for weights
		f_approx->leastSquaresWeights(f_domain, f_targets, n_pts);

		//Create the DMP structures
		DMPData *curr_dmp = new DMPData();
		curr_dmp->weights = f_approx->getWeights();
		curr_dmp->k_gain = curr_k;
		curr_dmp->d_gain = curr_d;
                for(int i=0; i<n_pts; i++){
                    curr_dmp->f_domain.push_back(f_domain[i]); 
                    curr_dmp->f_targets.push_back(f_targets[i]);
                }
		dmp_list.push_back(*curr_dmp);
	}

	delete[] x_demo;
	delete[] v_demo;
	delete[] v_dot_demo;
	delete[] f_domain;
	delete[] f_targets;
	delete f_approx;
}

double epsilon = 1e-10;

/**
 * @ Compute the coupling term of the artificial potential field for obstacle avoidance
 * @param[out] apf_ct artificial potential field coupling acceleration
 * @param[in] x The (n-dim) previous state
 * @param[in] v The (n-dim) previous instantaneous change in state
 * @param[in] o (3-dim) obstacle state vector
 * @param[in] beta angle coeficient, adjust influence of the angle in the coupling term
 * @param[in] gamma potential field amplitude
 * @param[in] k distance coeficient, adjust influence of the angle in the coupling term
 */
void artificialPotentialFieldCoupling(vector<double> &apf_ct,
									  const vector<double> &x,
									  const vector<double> &v,
									  const vector<vector<double>> &o, 
									  vector<double> &beta, 
									  vector<double> &gamma,
									  vector<double> &k,
									  double m,
									  double n) 
{

	Eigen::Vector3d x_e(x[0], x[1], x[2]); 
	Eigen::Vector3d v_e(v[0], v[1], v[2]); 
	Eigen::Vector3d apf_ct_e;
	if (o.size()==1) {
		//std::cout<<o[0]<<" "<< o[1]<<" "<< o[2]<<std::endl;
		Eigen::Vector3d o_e(o[0][0], o[0][1], o[0][2]); 
		Eigen::Vector3d o_diff = o_e - x_e;
		double r = 0.5 * M_PI * (o_diff.cross(v_e)).norm();
		Eigen::Matrix3d R = Eigen::AngleAxisd(r, o_diff.cross(v_e).normalized()).toRotationMatrix();
		double theta = std::acos(o_diff.dot(v_e) /
								(o_diff.norm() * v_e.norm() + epsilon));
		apf_ct_e = gamma[0] * R * v_e * theta * std::exp(-beta[0] * theta) * std::exp(-k[0] * o_diff.norm());
	}
	else {
		Eigen::Vector3d dim_e = calculate_bounding_box_dimensions(o); 
		//std::cout<<dim[0]<<" "<<dim[1]<<" "<<dim[2]<<std::endl;

		Eigen::MatrixXd ov_e = verticesVectorToEigen(o);
		Eigen::Vector3d oc_e = calculateCentroid(ov_e);
		Eigen::Vector3d op_e = nearestObjectPoint(ov_e,x_e);
		
		
		//std::cout<<oc_e[0]<<" "<<oc_e[1]<<" "<<oc_e[2]<<std::endl;
		//std::cout<<"beta:"<<beta[0]<<" gamma:"<<gamma[0]<<" k:"<<k[0];
		//std::cout<<"beta:"<<beta[1]<<" gamma:"<<gamma[1]<<" k:"<<k[1]<<std::endl;

		if(beta.size()<2)
			beta=std::vector<double>{beta[0],0.0};
		if(gamma.size()<3)
			gamma=std::vector<double>{gamma[0],0.0,0.0};
		if(k.size()<3)
			k=std::vector<double>{k[0],0.0,0.0};

		// Obstacle centroid point coupling
		Eigen::Vector3d oc_diff = oc_e - x_e;
		double r = 0.5 * M_PI * (oc_diff.cross(v_e)).norm();
		Eigen::Matrix3d R = Eigen::AngleAxisd(r, oc_diff.cross(v_e).normalized()).toRotationMatrix();
		double theta_o = std::acos(oc_diff.dot(v_e) /
								(oc_diff.norm() * v_e.norm() + epsilon));
		apf_ct_e = gamma[0] * R * v_e * theta_o * std::exp(-beta[0] * theta_o) * std::exp(-k[0] * oc_diff.norm());
		
		// Nearest point coupling with angle(asumed colinear with centroid-path_point)
		Eigen::Vector3d op_diff = op_e - x_e;
		//std::cout<<"oc_d: "<<oc_diff.norm()<<"op_d: "<<op_diff.norm()<<std::endl;
		double r_p = 0.5 * M_PI * (op_diff.cross(v_e)).norm();
		Eigen::Matrix3d R_p = Eigen::AngleAxisd(r_p, op_diff.cross(v_e).normalized()).toRotationMatrix();
		double theta_p = std::acos(op_diff.dot(v_e) /
								(op_diff.norm() * v_e.norm() + epsilon));
		//apf_ct_e += gamma[1] * R_p * v_e * theta_p * std::exp(-beta[1] * theta_p) * std::exp(-k[1] * op_diff.norm());
		apf_ct_e += gamma[1] * R * v_e * theta_p * std::exp(-beta[1] * theta_p) * std::exp(-k[1] * op_diff.norm());
	
		// Obstacle centroid point without angle
		apf_ct_e += gamma[2]*R*v_e*std::exp(-k[2]*op_diff.norm());

		// Scale the coupling with the object bounding box dimensions
		Eigen::Vector3d dim_s_e(n+m*dim_e.x(), n+m*dim_e.y(), n+m*dim_e.y()); 
		apf_ct_e=apf_ct_e.array() * dim_s_e.array();
		
	}

	apf_ct.push_back(apf_ct_e.x());
	apf_ct.push_back(apf_ct_e.y());
	apf_ct.push_back(apf_ct_e.z());
}

Eigen::Vector3d calculate_bounding_box_dimensions(const std::vector<std::vector<double>>& points) {
    // Check if the points vector is not empty
    if (points.empty() || points[0].empty()) {
        std::cerr << "Error: Empty input points vector." << std::endl;
        return {};
    }

    // Calculate min and max values for each dimension
    std::vector<double> min_values = points[0];
    std::vector<double> max_values = points[0];

    for (const auto& point : points) {
        for (size_t i = 0; i < point.size(); ++i) {
            min_values[i] = std::min(min_values[i], point[i]);
            max_values[i] = std::max(max_values[i], point[i]);
        }
    }

    // Calculate dimensions
    std::vector<double> dimensions;
    for (size_t i = 0; i < min_values.size(); ++i) {
        dimensions.push_back(max_values[i] - min_values[i]);
    }

	Eigen::Vector3d dim(dimensions[0],dimensions[1],dimensions[2]);
    return dim;
}

Eigen::Vector3d calculateCentroid(const Eigen::MatrixXd& points) 
{
    Eigen::Vector3d centroid(0.0, 0.0, 0.0);

	for (int i = 0; i < points.rows(); ++i) {
		centroid += points.row(i);
	}
	centroid /= points.rows();

    return centroid;
};

Eigen::MatrixXd verticesVectorToEigen(const std::vector<vector<double>>& vertices)
{
	int num_rows = vertices.size();

	Eigen::MatrixXd vertices_matrix(num_rows,3);

	for (int i = 0; i < num_rows; ++i)
		for (int j = 0; j < 3; ++j) 
			vertices_matrix(i, j) = vertices[i][j];

	return vertices_matrix;
};

Eigen::Vector3d nearestObjectPoint(const Eigen::MatrixXd& vertices_matrix, const  Eigen::Vector3d& path_point) 
{	
	// Calculate distances
    Eigen::VectorXd distances = (vertices_matrix.rowwise() - path_point.transpose()).rowwise().norm();

	// Find the index of the minimum distance point
	int index = 0;
    double min_dist = distances.transpose().minCoeff(&index);
	
	//std::cout<<"min_dist: "<< min_dist<<" Id:"<<index<<std::endl;

	// Get the nearest point
    Eigen::Vector3d nearest_point = vertices_matrix.row(index).transpose();

    return nearest_point;
};


/**
 * @brief Use the current active multi-dim DMP to create a plan starting from x_0 toward a goal
 * @param[in] dmp_list An n-dim list of DMPs that are all linked by a single canonical (phase) system
 * @param[in] x_0 The (n-dim) starting state for planning
 * @param[in] x_dot_0 The (n-dim) starting instantaneous change in state for planning
 * @param[in] t_0 The time in seconds at which to begin the planning segment. Should only be nonzero when doing
              a partial segment plan that does not start at beginning of DMP
 * @param[in] goal The (n-dim) goal point for planning
 * @param[in] goal_thresh Planning will continue until system is within the specified threshold of goal
              in each dimension
 * @param[in] seg_length The length of the requested plan segment in seconds. Set to -1 if plan until goal is desired.
 * @param[in] tau The time scaling constant (in this implementation, it is the desired length of the TOTAL
              (not just this segment) DMP execution in seconds)
 * @param[in] total_dt The desired time resolution of the plan
 * @param[in] integrate_iter The number of loops used when numerically integrating accelerations
 * @param[out] plan An n-dim plan starting from x_0
 * @param[out] at_goal True if the final time is greater than tau AND the planned position is within goal_thresh
               of the goal		   
 * @param[in] obstacle (1-dim(3-dim)) obstacle state vector or (n-dim(3-dim)) vertices vector
 * @param[in] beta angle coeficient, adjust influence of the angle in the coupling term
 * @param[in] gamma potential field amplitude
 * @param[in] k distance coeficient, adjust influence of the angle in the coupling term
 */
void generatePlan(const vector<DMPData> &dmp_list,
				  const vector<double> &x_0,
				  const vector<double> &x_dot_0,
				  const double &t_0,
				  const vector<double> &goal,
				  const vector<double> &goal_thresh,
				  const double &seg_length,
				  const double &tau,
				  const double &total_dt,
				  const int &integrate_iter,
				  DMPTraj &plan,
				  uint8_t &at_goal, 
				  vector<vector<double>> obstacle,
				  vector<double> beta,
				  vector<double> gamma,
				  vector<double> k,
				  double m,
				  double n)
{
	plan.points.clear();
	plan.times.clear();
	at_goal = false;

	int dims = dmp_list.size();
	int n_pts = 0;
	double dt = total_dt / integrate_iter;

	vector<double> *x_vecs, *x_dot_vecs;
	vector<double> t_vec;
	x_vecs = new vector<double>[dims];
	x_dot_vecs = new vector<double>[dims];
	FunctionApprox **f = new FunctionApprox*[dims];

	for(int i=0; i<dims; i++)
		f[i] = new FourierApprox(dmp_list[i].weights);
		//f[i] = new LinearApprox(dmp_list[i].f_domain, dmp_list[i].f_targets);

	double t = 0;
	double f_eval;

	vector<double> x_avd,v_avd;

	//Plan for at least tau seconds.  After that, plan until goal_thresh is satisfied.
	//Cut off if plan exceeds MAX_PLAN_LENGTH seconds, in case of overshoot / oscillation
	//Only plan for seg_length seconds if specified
	bool seg_end = false;
	while(((t+t_0) < tau || (!at_goal && t<MAX_PLAN_LENGTH)) && !seg_end){
		//Check if we've planned to the segment end yet
		if(seg_length > 0){
			if (t > seg_length) seg_end = true;
		}

		// Artificial potential filed coupling
		std::vector<double> apf_ct;
		if(obstacle.size()>0 && (dims==3 || dims==6)){
			apf_ct.clear();
			if(n_pts==0){
				x_avd={x_0[0],x_0[1],x_0[2]};
				v_avd={x_dot_0[0],x_dot_0[1],x_dot_0[2]};
			}
			else{
				x_avd={x_vecs[0][n_pts-1],x_vecs[1][n_pts-1],x_vecs[2][n_pts-1]};
				v_avd={x_dot_vecs[0][n_pts-1]*tau,x_dot_vecs[1][n_pts-1]*tau,x_dot_vecs[2][n_pts-1]*tau};
			}
			artificialPotentialFieldCoupling(apf_ct,x_avd,v_avd, obstacle, beta ,gamma, k, m, n);
			if(dims==6)
				apf_ct.resize(6,0.0);
		}		
		else{
			apf_ct=std::vector<double>(dims,0.0);
		}

		//std::cout<<apf_ct[0]<<" "<< apf_ct[1]<<" "<< apf_ct[2]<<std::endl;
		
		//Plan in each dimension
		for(int i=0; i<dims; i++){
            double x,v;
            if(n_pts==0){
                x = x_0[i];
                v = x_dot_0[i];
            }
            else{			
                x = x_vecs[i][n_pts-1];
			    v = x_dot_vecs[i][n_pts-1] * tau;
			}

			//Numerically integrate to get new x and v
			for(int iter=0; iter<integrate_iter; iter++)
			{
				//Compute the phase and the log of the phase to assist with some numerical issues
				//Then, evaluate the function approximator at the log of the phase
				double s = calcPhase((t+t_0) + (dt*iter), tau);
				double log_s = (t+t_0)/tau;
				if(log_s >= 1.0){
					f_eval = 0;
				}
				else{
					f_eval = f[i]->evalAt(log_s) * s;
				}
				
				//Update v dot and x dot based on DMP differential equations
				double v_dot = (dmp_list[i].k_gain*((goal[i]-x) - (goal[i]-x_0[i])*s + f_eval) - dmp_list[i].d_gain*v + apf_ct[i]) / tau;
				//double v_dot = (dmp_list[i].k_gain*((goal[i]-x) - (goal[i]-x_0[i])*s + f_eval) - dmp_list[i].d_gain*v) / tau;
				
				double x_dot = v/tau;

				//Update state variables
				v += v_dot * dt;
				x += x_dot * dt;
			}

			//Add current state to the plan
			x_vecs[i].push_back(x);
			x_dot_vecs[i].push_back(v/tau);
		}
		t += total_dt;
		t_vec.push_back(t);
		n_pts++;

		//If plan is at least minimum length, check to see if we are close enough to goal
		if((t+t_0) >= tau){
			at_goal = true;
			for(int i=0; i<dims; i++){
				if(goal_thresh[i] > 0){
					if(fabs(x_vecs[i][n_pts-1] - goal[i]) > goal_thresh[i])
						at_goal = false;
				}
			}
		}
	}

	//Create a plan from the generated trajectories
	plan.points.resize(n_pts);
	for(int j=0; j<n_pts; j++){
		plan.points[j].positions.resize(dims);
		plan.points[j].velocities.resize(dims);
	}
	for(int i=0; i<dims; i++){
		for(int j=0; j<n_pts; j++){
			plan.points[j].positions[i] = x_vecs[i][j];
			plan.points[j].velocities[i] = x_dot_vecs[i][j];
		}
	}
	plan.times = t_vec;

	//Clean up
	for(int i=0; i<dims; i++){
		delete f[i];
	}
	delete[] f;
	delete[] x_vecs;
	delete[] x_dot_vecs;
}

}
