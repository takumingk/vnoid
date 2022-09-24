﻿#include "stabilizer.h"

#include "robot.h"
#include "rollpitchyaw.h"

namespace cnoid{
namespace vnoid{

Stabilizer::Stabilizer(){
    min_contact_force        = 1.0;
	force_ctrl_damping       = 0.0;
	force_ctrl_gain          = 0.0;
	force_ctrl_limit         = 0.0;
	moment_ctrl_damping      = 0.0;
	moment_ctrl_gain         = 0.0;
	moment_ctrl_limit        = 0.0;
	orientation_ctrl_gain_p  = 0.0;
	orientation_ctrl_gain_d  = 0.0;

	gain  = Eigen::Matrix<double,  6, 12>::Zero();
	state = Eigen::Matrix<double, 12,  1>::Zero();
	input = Eigen::Matrix<double,  6,  1>::Zero();

	phi_mod     = Vector3(0.0, 0.0, 0.0);
	phid_mod    = Vector3(0.0, 0.0, 0.0);
	com_pos_mod = Vector3(0.0, 0.0, 0.0);
	com_vel_mod = Vector3(0.0, 0.0, 0.0);
	
    for(int i = 0; i < 2; i++){
        dpos[i] = Vector3(0.0, 0.0, 0.0);
	    drot[i] = Vector3(0.0, 0.0, 0.0);
    }

}

void Stabilizer::CalcZmp(const Param& param, Centroid& centroid, vector<Foot>& foot){
    // get actual force from the sensor
	for(int i = 0; i < 2; i++){
		// set contact state
		foot[i].contact = (foot[i].force.z() >= min_contact_force);

		// measure continuous contact duration
		if(foot[i].contact){
			foot[i].zmp = Vector3(-foot[i].moment.y()/foot[i].force.z(), foot[i].moment.x()/foot[i].force.z(), 0.0);
		}
		else{
			foot[i].zmp = Vector3(0.0, 0.0, 0.0);
		}
	}

	// both feet not in contact
	if(!foot[0].contact && !foot[1].contact){
		foot[0].balance = 0.5;
		foot[1].balance = 0.5;
		centroid.zmp = Vector3(0.0, 0.0, 0.0);
	}
	else{
		double f0 = std::max(0.0, foot[0].force.z());
		double f1 = std::max(0.0, foot[1].force.z());
		foot[0].balance = f0/(f0 + f1);
		foot[1].balance = f1/(f0 + f1);
		centroid.zmp =
			     (foot[0].balance) * (foot[0].pos_ref + foot[0].ori_ref * foot[0].zmp)
	           + (foot[1].balance) * (foot[1].pos_ref + foot[1].ori_ref * foot[1].zmp);
	}
}

void Stabilizer::CalcForceDistribution(const Param& param, Centroid& centroid, vector<Foot>& foot){
	// switch based on contact state
	if(!foot[0].contact_ref && !foot[1].contact_ref){
		foot[0].balance_ref = 0.5;
		foot[1].balance_ref = 0.5;
		foot[0].zmp_ref = Vector3(0.0, 0.0, 0.0);
		foot[1].zmp_ref = Vector3(0.0, 0.0, 0.0);
	}
	if( foot[0].contact_ref && !foot[1].contact_ref){
		foot[0].balance_ref = 1.0;
		foot[1].balance_ref = 0.0;
		foot[0].zmp_ref = foot[0].ori_ref.conjugate() * (centroid.zmp_ref - foot[0].pos_ref);
		foot[1].zmp_ref = Vector3(0.0, 0.0, 0.0);
	}
	if(!foot[0].contact_ref &&  foot[1].contact_ref){
		foot[0].balance_ref = 0.0;
		foot[1].balance_ref = 1.0;
		foot[0].zmp_ref = Vector3(0.0, 0.0, 0.0);
		foot[1].zmp_ref = foot[1].ori_ref.conjugate() * (centroid.zmp_ref - foot[1].pos_ref);
	}
	if( foot[0].contact_ref &&  foot[1].contact_ref){
		//
		Vector2 b;
		Vector3 pdiff  = foot[1].pos_ref - foot[0].pos_ref;
		double  pdiff2 = pdiff.squaredNorm();
		const double eps = 1.0e-10;
		if(pdiff2 < eps){
			b[0] = b[1] = 0.5;
		}
		else{
			b[0] = (pdiff.dot(foot[1].pos_ref - centroid.zmp_ref))/pdiff2;
			b[0] = std::min(std::max(0.0, b[0]), 1.0);
			b[1] = 1.0 - b[0];
		}

		foot[0].balance_ref = b[0];
		foot[1].balance_ref = b[1];

		Vector3 zmp_proj = b[0]*foot[0].pos_ref + b[1]*foot[1].pos_ref;

		double b2 = b.squaredNorm();
		foot[0].zmp_ref = (b[0]/b2) * (foot[0].ori_ref.conjugate() * (centroid.zmp_ref - zmp_proj));
		foot[1].zmp_ref = (b[1]/b2) * (foot[1].ori_ref.conjugate() * (centroid.zmp_ref - zmp_proj));
	}

	// limit zmp
	for(int i = 0; i < 2; i++){
		for(int j = 0; j < 3; j++){
			foot[i].zmp_ref[j] = std::min(std::max(param.zmp_min[j], foot[i].zmp_ref[j]), param.zmp_max[j]);
		}
	}

	for(int i = 0; i < 2; i++){
		// force and moment to realize desired Zmp
		foot[i].force_ref     =  foot[i].ori_ref.conjugate() * (foot[i].balance_ref * centroid.force_ref);
		foot[i].moment_ref[0] =  foot[i].force_ref.z() * foot[i].zmp_ref.y();
		foot[i].moment_ref[1] = -foot[i].force_ref.z() * foot[i].zmp_ref.x();
		foot[i].moment_ref[2] =  foot[i].balance_ref * centroid.moment_ref.z();
	}
}

void Stabilizer::Update(const Timer& timer, const Param& param, Centroid& centroid, Base& base, vector<Foot>& foot){
    // calc zmp from forces
    CalcZmp(param, centroid, foot);

	centroid.force_ref = param.total_mass*(centroid.com_acc_ref + Vector3(0.0, 0.0, param.gravity));
	centroid.moment_ref = Vector3(0.0, 0.0, 0.0);

	Vector3 zmp_mod1(0.0, 0.0, 0.0);
	Vector3 zmp_mod2(0.0, 0.0, 0.0);
	Vector3 com_acc_mod(0.0, 0.0, 0.0);
	Vector3 phidd_mod(0.0, 0.0, 0.0);

	// PD control of base link orientation using zmp only
	{
		Vector3 m(0.0, 0.0, 0.0);
		m[0] = orientation_ctrl_gain_p * (base.angle_ref[0] - base.angle[0]) + orientation_ctrl_gain_d * (base.angvel_ref[0] - base.angvel[0]);
		m[1] = orientation_ctrl_gain_p * (base.angle_ref[1] - base.angle[1]) + orientation_ctrl_gain_d * (base.angvel_ref[1] - base.angvel[1]);

		zmp_mod1 = (1.0/centroid.force_ref[2])*Vector3(-m[1], m[0], 0.0);
	}
	// more general state feedback control
	{
		// calc state
		Vector3 theta = base.angle  - base.angle_ref;
		Vector3 omega = base.angvel - base.angvel_ref;
		state << theta.x(), theta.y(),
			     omega.x(), omega.y(),
			     phi_mod.x(), phi_mod.y(),
			     phid_mod.x(), phid_mod.y(),
			     com_pos_mod.x(), com_pos_mod.y(),
			     com_vel_mod.x(), com_vel_mod.y();
		
		// u = -K*x
		input = -gain*state;

		phidd_mod.x() = input(0);
		phidd_mod.y() = input(1);
		com_acc_mod.x() = input(2);
		com_acc_mod.y() = input(3);
		zmp_mod2.x() = input(4);
		zmp_mod2.y() = input(5);

		com_pos_mod += com_vel_mod * timer.dt;
		com_vel_mod += com_acc_mod * timer.dt;

		phi_mod  += phid_mod  * timer.dt;
		phid_mod += phidd_mod * timer.dt;
	}

	// modify CoM
	centroid.com_pos_ref += base.ori_ref * com_pos_mod;
	centroid.com_vel_ref += base.ori_ref * com_vel_mod;
	centroid.com_acc_ref += base.ori_ref * com_acc_mod;

	// modify swing foot position so that the relative position between CoM is maintained
	for(int i = 0; i < 2; i++){
		if(!foot[i].contact_ref){
			foot[i].pos_ref += base.ori_ref * com_pos_mod;
		}
	}

	// modify ZMP
	centroid.zmp_ref     += base.ori_ref * (zmp_mod1 + zmp_mod2);

	// calculate desired forces from desired zmp
	CalcForceDistribution(param, centroid, foot);

	for(int i = 0; i < 2; i++){
		// ground reaction force control
		if( foot[i].contact ){
			for(int j = 0; j < 3; j++){
				dpos[i][j] += (-force_ctrl_damping*dpos[i][j] + force_ctrl_gain*(foot[i].force_ref[j] - foot[i].force[j]))*timer.dt;
				dpos[i][j] = std::min(std::max(-force_ctrl_limit, dpos[i][j]), force_ctrl_limit);

				drot[i][j] += (-moment_ctrl_damping*drot[i][j] + moment_ctrl_gain*(foot[i].moment_ref[j] - foot[i].moment[j]))*timer.dt;
				drot[i][j] = std::min(std::max(-moment_ctrl_limit, drot[i][j]), moment_ctrl_limit);
			}

			// feedback to desired foot pose
			foot[i].pos_ref   += -dpos[i];
			foot[i].angle_ref += -drot[i];
            foot[i].ori_ref = FromRollPitchYaw(foot[i].angle_ref);
		}
	}

}


}
}
