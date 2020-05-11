#include <aerial_robot_model/transformable_aerial_robot_model.h>

namespace aerial_robot_model {

  void RobotModel::updateJacobians()
  {
    updateJacobians(getJointPositions(), false);
  }

  void RobotModel::updateJacobians(const KDL::JntArray& joint_positions, bool update_model)
  {
    if(update_model) updateRobotModel(joint_positions);

    calcCoGMomentumJacobian(); // should be processed first
    calcBasicKinematicsJacobian(); // need cog_jacobian_

    calcLambdaJacobian();

    calcJointTorque();
    calcJointTorqueJacobian();

    calcWrenchMarginJacobian();

#if 0
    thrustForceNumericalJacobian(getJointPositions(), lambda_jacobian_);
    jointTorqueNumericalJacobian(getJointPositions(), joint_torque_jacobian_);
    cogMomentumNumericalJacobian(getJointPositions(), cog_jacobian_, l_momentum_jacobian_);
    wrenchMarginNumericalJacobian(getJointPositions(), wrench_margin_f_min_jacobian_, wrench_margin_t_min_jacobian_);

    throw std::runtime_error("test");
#endif
  }

  Eigen::MatrixXd RobotModel::getJacobian(const KDL::JntArray& joint_positions, std::string segment_name, KDL::Vector offset)
  {
    const auto& tree = getTree();
    const auto& seg_frames = getSegmentsTf();
    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());

    KDL::TreeJntToJacSolver solver(tree);
    KDL::Jacobian jac(tree.getNrOfJoints());
    int status = solver.JntToJac(joint_positions, jac, segment_name);
    jac.changeRefPoint(seg_frames.at(segment_name).M * offset);

    // joint part
    Eigen::MatrixXd jac_joint = convertJacobian(jac.data);
    // return jac_joint; // only joint jacobian

    // add virtual 6dof root
    Eigen::MatrixXd jac_all = Eigen::MatrixXd::Identity(6, 6 + getJointNum());
    jac_all.rightCols(getJointNum()) = jac_joint;
    Eigen::Vector3d p = aerial_robot_model::kdlToEigen(seg_frames.at(segment_name).p + seg_frames.at(segment_name).M * offset);
    jac_all.block(0,3,3,3) = -aerial_robot_model::skew(p);

    jac_all.topRows(3) = root_rot * jac_all.topRows(3);
    jac_all.bottomRows(3) = root_rot * jac_all.bottomRows(3);
    return jac_all;

  }

  inline Eigen::MatrixXd RobotModel::convertJacobian(const Eigen::MatrixXd& in)
  {
    const auto& joint_indices = getJointIndices();
    Eigen::MatrixXd out(6, getJointNum());

    int col_index = 0;
    for (const auto& joint_index : joint_indices) { // fix bug of order
      out.col(col_index) = in.col(joint_index);
      col_index++;
    }
    return out;
  }


  // @deprecated
  Eigen::VectorXd RobotModel::getHessian(std::string ref_frame, int joint_i, int joint_j, KDL::Vector offset)
  {
    const auto& segment_map = getTree().getSegments();
    const auto& seg_frames = getSegmentsTf();
    const auto& joint_hierachy = getJointHierachy();
    const auto& joint_segment_map = getJointSegmentMap();
    const auto& joint_names = getJointNames();
    const auto& joint_parent_link_names = getJointParentLinkNames();

    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());

    Eigen::Vector3d p_e = aerial_robot_model::kdlToEigen(seg_frames.at(ref_frame).p + seg_frames.at(ref_frame).M * offset);

    std::string joint_i_name = joint_names.at(joint_i);
    std::string joint_j_name = joint_names.at(joint_j);

    if (joint_hierachy.at(joint_i_name) > joint_hierachy.at(joint_j_name)) {
      std::swap(joint_i_name, joint_j_name);
    }

    std::vector<std::string> joint_i_child_segments = joint_segment_map.at(joint_i_name);
    std::vector<std::string> joint_j_child_segments = joint_segment_map.at(joint_j_name);

    if (std::find(joint_i_child_segments.begin(), joint_i_child_segments.end(), ref_frame) == joint_i_child_segments.end() ||  std::find(joint_j_child_segments.begin(), joint_j_child_segments.end(), ref_frame) == joint_j_child_segments.end()) {
      return Eigen::VectorXd::Zero(6);
    }

    std::string joint_i_child_segment_name = joint_i_child_segments.at(0);
    std::string joint_j_child_segment_name = joint_j_child_segments.at(0);
    const KDL::Segment& joint_i_child_segment = GetTreeElementSegment(segment_map.at(joint_i_child_segment_name));
    const KDL::Segment& joint_j_child_segment = GetTreeElementSegment(segment_map.at(joint_j_child_segment_name));
    Eigen::Vector3d a_i = aerial_robot_model::kdlToEigen(seg_frames.at(joint_parent_link_names.at(joint_i)).M * joint_i_child_segment.getJoint().JointAxis());
    Eigen::Vector3d a_j = aerial_robot_model::kdlToEigen(seg_frames.at(joint_parent_link_names.at(joint_j)).M * joint_j_child_segment.getJoint().JointAxis());
    Eigen::Vector3d p_j = aerial_robot_model::kdlToEigen(seg_frames.at(joint_j_child_segment_name).p); //joint pos

    Eigen::VectorXd derivative(6);
    derivative.topRows(3) = root_rot * a_i.cross(a_j.cross(p_e - p_j));
    derivative.bottomRows(3) = root_rot * a_i.cross(a_j);

    return derivative;
  }

  Eigen::MatrixXd RobotModel::getSecondDerivative(std::string ref_frame, int joint_i, KDL::Vector offset)
  {
    const auto& segment_map = getTree().getSegments();
    const auto& seg_frames = getSegmentsTf();
    const auto& joint_hierachy = getJointHierachy();
    const auto& joint_segment_map = getJointSegmentMap();
    const auto& joint_names = getJointNames();
    const auto& joint_parent_link_names = getJointParentLinkNames();
    const auto& joint_num = getJointNum();
    const int full_body_ndof = 6 + joint_num;
    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());

    Eigen::Vector3d p_e = aerial_robot_model::kdlToEigen(seg_frames.at(ref_frame).p + seg_frames.at(ref_frame).M * offset);

    std::string joint_i_name = joint_names.at(joint_i);
    std::vector<std::string> joint_i_child_segments = joint_segment_map.at(joint_i_name);
    std::string joint_i_child_segment_name = joint_i_child_segments.at(0);
    const KDL::Segment& joint_i_child_segment = GetTreeElementSegment(segment_map.at(joint_i_child_segment_name));
    Eigen::Vector3d a_i = aerial_robot_model::kdlToEigen(seg_frames.at(joint_parent_link_names.at(joint_i)).M * joint_i_child_segment.getJoint().JointAxis());
    Eigen::Vector3d p_i = aerial_robot_model::kdlToEigen(seg_frames.at(joint_i_child_segment_name).p);

    Eigen::MatrixXd jacobian_i = Eigen::MatrixXd::Zero(6, full_body_ndof);

    if (std::find(joint_i_child_segments.begin(), joint_i_child_segments.end(), ref_frame) == joint_i_child_segments.end()) {
      return jacobian_i;
    }

    // joint part
    for(int j = 0; j < joint_num; j++)
      {
        std::string joint_j_name = joint_names.at(j);
        std::vector<std::string> joint_j_child_segments = joint_segment_map.at(joint_j_name);
        if (std::find(joint_j_child_segments.begin(), joint_j_child_segments.end(), ref_frame) == joint_j_child_segments.end()) {
          continue;
        }

        std::string joint_j_child_segment_name = joint_j_child_segments.at(0);
        const KDL::Segment& joint_j_child_segment = GetTreeElementSegment(segment_map.at(joint_j_child_segment_name));
        Eigen::Vector3d a_j = aerial_robot_model::kdlToEigen(seg_frames.at(joint_parent_link_names.at(j)).M * joint_j_child_segment.getJoint().JointAxis());
        Eigen::Vector3d p_j = aerial_robot_model::kdlToEigen(seg_frames.at(joint_j_child_segment_name).p);

        Eigen::Vector3d a_i_j(0,0,0);
        Eigen::Vector3d p_i_j(0,0,0);
        Eigen::Vector3d p_e_j = a_j.cross(p_e - p_j);
        if ( joint_hierachy.at(joint_j_name) <= joint_hierachy.at(joint_i_name)){
          // joint_j is close to root, or i = j

          // both i and j is revolute jont
          a_i_j = a_j.cross(a_i);
          p_i_j = a_j.cross(p_i - p_j);
        }

        jacobian_i.block(0, 6 + j, 3, 1) = root_rot * a_i_j.cross(p_e - p_i) + root_rot * a_i.cross(p_e_j - p_i_j); // force
        jacobian_i.block(3, 6 + j, 3, 1) = root_rot * a_i_j; // torque
      }

    // virtual 6dof root
    jacobian_i.block(0, 3, 3, 3) = - root_rot * aerial_robot_model::skew(a_i.cross(p_e - p_i));
    jacobian_i.block(3, 3, 3, 3) = - root_rot * aerial_robot_model::skew(a_i);
    return jacobian_i;
  }

  Eigen::MatrixXd RobotModel::getSecondDerivativeRoot(std::string ref_frame, KDL::Vector offset)
  {
    const int joint_num = getJointNum();
    const int full_body_ndof = 6 + joint_num;
    const auto& joint_parent_link_names = getJointParentLinkNames();
    const auto& segment_map = getTree().getSegments();
    const auto& seg_frames = getSegmentsTf();
    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());

    Eigen::Vector3d p_e = aerial_robot_model::kdlToEigen(seg_frames.at(ref_frame).p + seg_frames.at(ref_frame).M * offset);
    Eigen::MatrixXd out = Eigen::MatrixXd::Zero(3, full_body_ndof);

    // joint part
    for (int i = 0; i < joint_num; ++i) {

      std::string joint_name = getJointNames().at(i);
      std::vector<std::string> joint_child_segments = getJointSegmentMap().at(joint_name);

      if (std::find(joint_child_segments.begin(), joint_child_segments.end(), ref_frame) == joint_child_segments.end())
        {
          continue;
        }

      std::string joint_child_segment_name = joint_child_segments.at(0);
      const KDL::Segment& joint_child_segment = GetTreeElementSegment(segment_map.at(joint_child_segment_name));
      Eigen::Vector3d a = aerial_robot_model::kdlToEigen(seg_frames.at(joint_parent_link_names.at(i)).M * joint_child_segment.getJoint().JointAxis()); // fix bug
      Eigen::Vector3d p = aerial_robot_model::kdlToEigen(seg_frames.at(joint_child_segment_name).p);

      out.col(6 + i) = root_rot * a.cross(p_e - p);
    }

    // virtual root 6dof
    out.leftCols(3) = root_rot;
    out.middleCols(3,3) = - root_rot * aerial_robot_model::skew(p_e);

    return out;
  }

  void RobotModel::calcBasicKinematicsJacobian()
  {
    const std::vector<Eigen::Vector3d> p = getRotorsOriginFromCog<Eigen::Vector3d>();
    const std::vector<Eigen::Vector3d> u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& joint_positions = getJointPositions();
    const auto& sigma = getRotorDirection();
    const int rotor_num = getRotorNum();
    const double m_f_rate = getMFRate();

    //calc jacobian of u(thrust direction, force vector), p(thrust position)
    for (int i = 0; i < rotor_num; ++i) {
      std::string seg_name = std::string("thrust") + std::to_string(i + 1);
      Eigen::MatrixXd thrust_coord_jacobian = RobotModel::getJacobian(joint_positions, seg_name);
      thrust_coord_jacobians_.at(i) = thrust_coord_jacobian;
      u_jacobians_.at(i) = -skew(u.at(i)) * thrust_coord_jacobian.bottomRows(3);
      p_jacobians_.at(i) = thrust_coord_jacobian.topRows(3) - cog_jacobian_;
    }
  }

  bool RobotModel::stabilityCheck(bool verbose)
  {
    if(wrench_margin_f_min_ < wrench_margin_f_min_thre_)
      {
        ROS_ERROR_STREAM("wrench_margin_f_min " << wrench_margin_f_min_ << " is lower than the threshold " <<  wrench_margin_f_min_thre_);
          return false;
      }

    if(wrench_margin_t_min_ < wrench_margin_t_min_thre_)
      {
        ROS_ERROR_STREAM("wrench_margin_t_min " << wrench_margin_t_min_ << " is lower than the threshold " <<  wrench_margin_t_min_thre_);
        return false;
      }

    if(static_thrust_.maxCoeff() > thrust_max_ || static_thrust_.minCoeff() < thrust_min_)
      {
        ROS_ERROR("Invalid static thrust, max: %f, min: %f", static_thrust_.maxCoeff(), static_thrust_.minCoeff());
        return false;
      }

    return true;
  }

  void RobotModel::calcStaticThrust()
  {
    calcWrenchMatrixOnRoot(); // update Q matrix
    Eigen::VectorXd wrench_g = getGravityWrenchOnRoot();
    static_thrust_ = aerial_robot_model::pseudoinverse(q_mat_) * (-wrench_g); // TODO: add external force

#if 1
    Eigen::VectorXd static_thrust_from_cog = aerial_robot_model::pseudoinverse(calcWrenchMatrixOnCoG()) * getMass() * gravity_;
    ROS_DEBUG_STREAM("wrench gravity:" << wrench_g.transpose());
    ROS_DEBUG_STREAM("static thrust w.r.t. cog:" << static_thrust_from_cog.transpose());
    ROS_DEBUG_STREAM("static thrust w.r.t. root :" << static_thrust_.transpose());

    ROS_DEBUG_STREAM("q Mat : \n" << q_mat_);
#endif
  }

  void RobotModel::calcJointTorque()
  {
    const auto& sigma = getRotorDirection();
    const auto& joint_positions = getJointPositions();
    const auto& inertia_map = getInertiaMap();
    const int joint_num = getJointNum();
    const int rotor_num = getRotorNum();
    const double m_f_rate = getMFRate();

    joint_torque_ = Eigen::VectorXd::Zero(joint_num);

    // update coord jacobians for cog point and convert to joint torque
    int seg_index = 0;
    for(const auto& inertia : inertia_map)
      {
        cog_coord_jacobians_.at(seg_index) = RobotModel::getJacobian(joint_positions, inertia.first, inertia.second.getCOG());
        joint_torque_ -= cog_coord_jacobians_.at(seg_index).rightCols(joint_num).transpose() * inertia.second.getMass() * (-gravity_);
        seg_index ++;
      }

    // thrust
    for (int i = 0; i < rotor_num; ++i) {
      Eigen::VectorXd wrench = thrust_wrench_units_.at(i) * static_thrust_(i);
      joint_torque_ -= thrust_coord_jacobians_.at(i).rightCols(joint_num).transpose() * wrench;
    }
  }

  inline double RobotModel::calcTripleProduct(const Eigen::Vector3d& ui, const Eigen::Vector3d& uj, const Eigen::Vector3d& uk)
  {
    Eigen::Vector3d uixuj = ui.cross(uj);
    if (uixuj.norm() < 0.00001) {
      return 0.0;
    }
    return uixuj.dot(uk) / uixuj.norm();
  }

  std::vector<Eigen::Vector3d> RobotModel::calcV()
  {
    const std::vector<Eigen::Vector3d> p = getRotorsOriginFromCog<Eigen::Vector3d>();
    const std::vector<Eigen::Vector3d> u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& sigma = getRotorDirection();
    const int rotor_num = getRotorNum();
    const double m_f_rate = getMFRate();
    std::vector<Eigen::Vector3d> v(rotor_num);

    for (int i = 0; i < rotor_num; ++i)
      v.at(i) = p.at(i).cross(u.at(i)) + m_f_rate * sigma.at(i + 1) * u.at(i);
    return v;
  }

  void RobotModel::calcWrenchMarginF()
  {
    const int rotor_num = getRotorNum();
    const double thrust_max = getThrustUpperLimit();

    const auto& u = getRotorsNormalFromCog<Eigen::Vector3d>();
    Eigen::Vector3d gravity_force = getMass() * gravity_3d_;

    int index = 0;
    for (int i = 0; i < rotor_num; ++i) {
      const Eigen::Vector3d& u_i = u.at(i);
      for (int j = 0; j < rotor_num; ++j) {
        if (i == j) continue;
        const Eigen::Vector3d& u_j = u.at(j);

        double f_min_ij = 0.0;
        for (int k = 0; k < rotor_num; ++k) {
          if (i == k || j == k) continue;
          const Eigen::Vector3d& u_k = u.at(k);
          double u_triple_product = calcTripleProduct(u_i, u_j, u_k);
          f_min_ij += std::max(0.0, u_triple_product * thrust_max);
        }

        Eigen::Vector3d uixuj = u_i.cross(u_j);
        wrench_margin_f_min_ij_(index) = fabs(f_min_ij - (uixuj.dot(gravity_force) / uixuj.norm()));
        index++;
      }
    }
    wrench_margin_f_min_ = wrench_margin_f_min_ij_.minCoeff();

  }

  void RobotModel::calcWrenchMarginT()
  {
    const int rotor_num = getRotorNum();
    const double thrust_max = getThrustUpperLimit();

    const auto v = calcV();
    int index = 0;

    for (int i = 0; i < rotor_num; ++i) {
      const Eigen::Vector3d& v_i = v.at(i);
      for (int j = 0; j < rotor_num; ++j) {
        if (i == j) continue;
        const Eigen::Vector3d& v_j = v.at(j);
        double t_min_ij = 0.0;
        for (int k = 0; k < rotor_num; ++k) {
          if (i == k || j == k) continue;
          const Eigen::Vector3d& v_k = v.at(k);
          double v_triple_product = calcTripleProduct(v_i, v_j, v_k);
          t_min_ij += std::max(0.0, v_triple_product * thrust_max);
        }
        wrench_margin_t_min_ij_(index) = t_min_ij;
        index++;
      }
    }

    wrench_margin_t_min_ = wrench_margin_t_min_ij_.minCoeff();
  }

  void RobotModel::calcCoGMomentumJacobian()
  {
    double mass_all = getMass();
    const auto cog_all = getCog<KDL::Frame>().p;
    const auto& segment_map = getTree().getSegments();
    const auto& seg_frames = getSegmentsTf();
    const auto& inertia_map = getInertiaMap();
    const auto& joint_names = getJointNames();
    const auto& joint_segment_map = getJointSegmentMap();
    const auto& joint_parent_link_names = getJointParentLinkNames();
    const auto joint_num = getJointNum();

    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());
    /*
      Note: the jacobian about the cog velocity (linear momentum) and angular momentum.

      Please refer to Eq.13 ~ 22 of following paper:
      ============================================================================
      S. Kajita et al., "Resolved momentum control: humanoid motion planning based on the linear and angular momentum,".
      ============================================================================

      1. the cog velocity is w.r.t in the root link frame.
      2. the angular momentum is w.r.t in cog frame
    */


    int col_index = 0;
    /* fix bug: the joint_segment_map is reordered, which is not match the order of  joint_indeices_ or joint_names_ */
    // joint part
    for (const auto& joint_name : joint_names){
      std::string joint_child_segment_name = joint_segment_map.at(joint_name).at(0);
      KDL::Segment joint_child_segment = GetTreeElementSegment(segment_map.at(joint_child_segment_name));
      KDL::Vector a = seg_frames.at(joint_parent_link_names.at(col_index)).M * joint_child_segment.getJoint().JointAxis();

      KDL::Vector r = seg_frames.at(joint_child_segment_name).p;
      KDL::RigidBodyInertia inertia = KDL::RigidBodyInertia::Zero();
      for (const auto& seg : joint_segment_map.at(joint_name)) {
        if (seg.find("thrust") == std::string::npos) {
          KDL::Frame f = seg_frames.at(seg);
          inertia = inertia + f * inertia_map.at(seg);
        }
      }
      KDL::Vector c = inertia.getCOG();
      double m = inertia.getMass();

      KDL::Vector p_momentum_jacobian_col = a * (c - r) * m;
      KDL::Vector l_momentum_jacobian_col = (c - cog_all) * p_momentum_jacobian_col + inertia.RefPoint(c).getRotationalInertia() * a;

      cog_jacobian_.col(6 + col_index) = aerial_robot_model::kdlToEigen(p_momentum_jacobian_col / mass_all);
      l_momentum_jacobian_.col(6 + col_index) = aerial_robot_model::kdlToEigen(l_momentum_jacobian_col);
      col_index++;
    }

    // virtual 6dof root
    cog_jacobian_.leftCols(3) = Eigen::MatrixXd::Identity(3, 3);
    cog_jacobian_.middleCols(3, 3) = - aerial_robot_model::skew(aerial_robot_model::kdlToEigen(cog_all));
    cog_jacobian_ = root_rot * cog_jacobian_;

    l_momentum_jacobian_.leftCols(3) = Eigen::MatrixXd::Zero(3, 3);
    l_momentum_jacobian_.middleCols(3, 3) = getInertia<Eigen::Matrix3d>() * root_rot; // aready converted
    l_momentum_jacobian_.rightCols(joint_num) = root_rot * l_momentum_jacobian_.rightCols(joint_num);
  }

  void RobotModel::calcLambdaJacobian()
  {
    // w.r.t root
    const auto& inertia_map = getInertiaMap();
    const auto& rotor_direction = getRotorDirection();
    const auto& joint_positions = getJointPositions();
    const int rotor_num = getRotorNum();
    const int joint_num = getJointNum();
    const int ndof = thrust_coord_jacobians_.at(0).cols();
    const double m_f_rate = getMFRate();
    const int wrench_dof = q_mat_.rows(); // default: 6, under-actuated: 4
    Eigen::MatrixXd q_pseudo_inv = aerial_robot_model::pseudoinverse(q_mat_);

    /* derivative for gravity jacobian */
    Eigen::MatrixXd wrench_gravity_jacobian = Eigen::MatrixXd::Zero(6, ndof);
    for(const auto& inertia : inertia_map){
      wrench_gravity_jacobian.bottomRows(3) -= aerial_robot_model::skew(-inertia.second.getMass() * gravity_3d_) * getSecondDerivativeRoot(inertia.first, inertia.second.getCOG());
    }

    ROS_DEBUG_STREAM("wrench_gravity_jacobian w.r.t. root : \n" << wrench_gravity_jacobian);

    if(wrench_dof == 6) // fully-actuated
      lambda_jacobian_ = -q_pseudo_inv * wrench_gravity_jacobian; // trans, rot
    else // under-actuated
      lambda_jacobian_ = -q_pseudo_inv * (wrench_gravity_jacobian).middleRows(2, wrench_dof); // z, rot

    /* derivative for thrust jacobian */
    std::vector<Eigen::MatrixXd> q_mat_jacobians;
    Eigen::MatrixXd q_inv_jacobian = Eigen::MatrixXd::Zero(6, ndof);
    for (int i = 0; i < rotor_num; ++i) {
      std::string thrust_name = std::string("thrust") + std::to_string(i + 1);
      Eigen::MatrixXd q_mat_jacobian = Eigen::MatrixXd::Zero(6, ndof);

      q_mat_jacobian.bottomRows(3) -= aerial_robot_model::skew(thrust_wrench_units_.at(i).head(3)) * getSecondDerivativeRoot(thrust_name);

      Eigen::MatrixXd wrench_unit_jacobian = Eigen::MatrixXd::Zero(6, ndof);
      wrench_unit_jacobian.topRows(3) = -skew(thrust_wrench_units_.at(i).head(3)) * thrust_coord_jacobians_.at(i).bottomRows(3);
      wrench_unit_jacobian.bottomRows(3) = -skew(thrust_wrench_units_.at(i).tail(3)) * thrust_coord_jacobians_.at(i).bottomRows(3);
      q_mat_jacobian += (thrust_wrench_allocations_.at(i).transpose() * wrench_unit_jacobian);

      q_inv_jacobian += (q_mat_jacobian * static_thrust_(i));
      q_mat_jacobians.push_back(q_mat_jacobian);

#if 0
      q_mat_jacobian.bottomRows(3) -= aerial_robot_model::skew(thrust_wrench_units_.at(i).head(3)) * getSecondDerivativeRoot(thrust_name) * static_thrust_(i);
      q_mat_jacobian += (thrust_wrench_allocations_.at(i).transpose() * wrench_unit_jacobian * static_thrust_(i));
#endif
    }

    ROS_DEBUG_STREAM("q_inv_jacobian: \n" << q_inv_jacobian);

     if(wrench_dof == 6) // fully-actuated
      lambda_jacobian_ += -q_pseudo_inv * q_inv_jacobian; // trans, rot
    else // under-actuated
      lambda_jacobian_ += -q_pseudo_inv * (q_inv_jacobian).middleRows(2, wrench_dof); // z, rot

    // https://mathoverflow.net/questions/25778/analytical-formula-for-numerical-derivative-of-the-matrix-pseudo-inverse, the third tmer
    Eigen::MatrixXd q_pseudo_inv_jacobian = Eigen::MatrixXd::Zero(rotor_num, ndof);
    Eigen::VectorXd pseudo_wrech = q_pseudo_inv.transpose() * static_thrust_;
    for(int i = 0; i < rotor_num; i++)
      {
        if(wrench_dof == 6) // fully-actuated
          q_pseudo_inv_jacobian.row(i) = pseudo_wrech.transpose() * q_mat_jacobians.at(i);
        else // under-actuated
          q_pseudo_inv_jacobian.row(i) = pseudo_wrech.transpose() * q_mat_jacobians.at(i).middleRows(2, wrench_dof);
      }
    lambda_jacobian_ += (Eigen::MatrixXd::Identity(rotor_num, rotor_num) - q_pseudo_inv * q_mat_) * q_pseudo_inv_jacobian;

    ROS_DEBUG_STREAM("lambda_jacobian: \n" << lambda_jacobian_);
  }

  void RobotModel::calcJointTorqueJacobian()
  {
    const auto& sigma = getRotorDirection();
    const auto& inertia_map = getInertiaMap();
    const double m_f_rate = getMFRate();
    const int rotor_num = getRotorNum();
    const int joint_num = getJointNum();
    const int ndof = lambda_jacobian_.cols();

    joint_torque_jacobian_ = Eigen::MatrixXd::Zero(joint_num, ndof);
    // gravity
    for(const auto& inertia : inertia_map)
      {
        for (int j = 0; j < joint_num; ++j) {
          joint_torque_jacobian_.row(j) += inertia.second.getMass() * (-gravity_.transpose()) * getSecondDerivative(inertia.first, j, inertia.second.getCOG());
        }
      }
    // thrust
    for (int i = 0; i < rotor_num; ++i) {
      Eigen::VectorXd wrench = thrust_wrench_units_.at(i) * static_thrust_(i);
      std::string thrust_name = std::string("thrust") + std::to_string(i + 1);

      // fix bug: not using hessian
      for (int j = 0; j < joint_num; ++j) {
        joint_torque_jacobian_.row(j) += wrench.transpose() * getSecondDerivative(thrust_name, j);
      }

      Eigen::MatrixXd wrench_unit_jacobian = Eigen::MatrixXd::Zero(6, ndof);
      // wrench_jacobian.topRows(3) = u_jacobians_.at(i) * static_thrust_(i);
      // wrench_jacobian.bottomRows(3) = m_f_rate * sigma.at(i+1) * u_jacobians_.at(i) * static_thrust_(i); // fix bug
      wrench_unit_jacobian.topRows(3) = -skew(thrust_wrench_units_.at(i).head(3)) * thrust_coord_jacobians_.at(i).bottomRows(3);
      wrench_unit_jacobian.bottomRows(3) = -skew(thrust_wrench_units_.at(i).tail(3)) * thrust_coord_jacobians_.at(i).bottomRows(3);

      //ROS_WARN_STREAM("wrench_unit_jacobian: \n" << wrench_unit_jacobian);
      joint_torque_jacobian_ += thrust_coord_jacobians_.at(i).rightCols(joint_num).transpose() * (wrench_unit_jacobian * static_thrust_(i) + thrust_wrench_units_.at(i) * lambda_jacobian_.row(i));
    }
    joint_torque_jacobian_ *= -1;
  }


  void RobotModel::calcWrenchMarginJacobian()
  {
    // reference:
    // https://ieeexplore.ieee.org/stamp/stamp.jsp?tp=&arnumber=8967725

    const int rotor_num = getRotorNum();
    const int joint_num = getJointNum();
    const int ndof = 6 + joint_num;
    const auto& p = getRotorsOriginFromCog<Eigen::Vector3d>();
    const auto& u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& sigma = getRotorDirection();
    const double thrust_max = getThrustUpperLimit();
    Eigen::Vector3d fg = getMass() * gravity_3d_;
    const double m_f_rate = getMFRate();

    const auto v = calcV();
    std::vector<Eigen::MatrixXd> v_jacobians;
    for (int i = 0; i < rotor_num; ++i) {
      v_jacobians.push_back(-skew(u.at(i)) * p_jacobians_.at(i) + skew(p.at(i)) * u_jacobians_.at(i) + m_f_rate * sigma.at(i + 1) * u_jacobians_.at(i));
    }

    //calc jacobian of f_min_ij, t_min_ij
    int index = 0;
    for (int i = 0; i < rotor_num; ++i) {
      for (int j = 0; j < rotor_num; ++j) {
        double approx_f_min = 0.0;
        double approx_t_min = 0.0;
        Eigen::MatrixXd d_f_min = Eigen::MatrixXd::Zero(1, ndof);
        Eigen::MatrixXd d_t_min = Eigen::MatrixXd::Zero(1, ndof);
        const Eigen::Vector3d& u_i = u.at(i);
        const Eigen::Vector3d& u_j = u.at(j);
        const Eigen::Vector3d uixuj = u_i.cross(u_j);
        const Eigen::MatrixXd& d_u_i = u_jacobians_.at(i);
        const Eigen::MatrixXd& d_u_j = u_jacobians_.at(j);
        const Eigen::MatrixXd d_uixuj = -skew(u_j) * d_u_i  + skew(u_i) * d_u_j;

        const Eigen::Vector3d& v_i = v.at(i);
        const Eigen::Vector3d& v_j = v.at(j);
        const Eigen::Vector3d vixvj = v_i.cross(v_j);
        const Eigen::MatrixXd& d_v_i = v_jacobians.at(i);
        const Eigen::MatrixXd& d_v_j = v_jacobians.at(j);
        const Eigen::MatrixXd d_vixvj = -skew(v_j) * d_v_i  + skew(v_i) * d_v_j;

        for (int k = 0; k < rotor_num; ++k) {
          if (i == j || j == k || k == i) {
          } else {

            // u
            const Eigen::Vector3d& u_k = u.at(k);
            const double u_triple_product = calcTripleProduct(u_i, u_j, u_k);
            const Eigen::MatrixXd& d_u_k = u_jacobians_.at(k);
            Eigen::MatrixXd d_u_triple_product = (uixuj / uixuj.norm()).transpose() * d_u_k + u_k.transpose() * (d_uixuj / uixuj.norm() - uixuj / (uixuj.norm() * uixuj.squaredNorm()) * uixuj.transpose() * d_uixuj);
            d_f_min += sigmoid(u_triple_product * thrust_max, epsilon_) * d_u_triple_product * thrust_max;
            approx_f_min += reluApprox(u_triple_product * thrust_max, epsilon_);

            // v
            const Eigen::Vector3d& v_k = v.at(k);
            const double v_triple_product = calcTripleProduct(v_i, v_j, v_k);
            const Eigen::MatrixXd& d_v_k = v_jacobians.at(k);
            Eigen::MatrixXd d_v_triple_product = (vixvj / vixvj.norm()).transpose() * d_v_k + v_k.transpose() * (d_vixvj / vixvj.norm() - vixvj / (vixvj.norm() * vixvj.squaredNorm()) * vixvj.transpose() * d_vixvj);
            d_t_min += sigmoid(v_triple_product * thrust_max, epsilon_) * d_v_triple_product * thrust_max;
            approx_t_min += reluApprox(v_triple_product * thrust_max, epsilon_);
          } //if
        } //k

        if (i != j) {
          double uixuj_fg = uixuj.dot(fg)/uixuj.norm();
          Eigen::MatrixXd d_uixuj_fg = Eigen::MatrixXd::Zero(1, ndof);
          d_uixuj_fg = fg.transpose() * (d_uixuj / uixuj.norm() - uixuj / (uixuj.norm() * uixuj.squaredNorm()) * uixuj.transpose() * d_uixuj);

          approx_wrench_margin_f_min_ij_(index) = absApprox(approx_f_min - uixuj_fg, epsilon_);
          wrench_margin_f_min_jacobian_.row(index) = tanh(approx_f_min - uixuj_fg, epsilon_) * (d_f_min - d_uixuj_fg);

          approx_wrench_margin_t_min_ij_(index) = approx_t_min;
          wrench_margin_t_min_jacobian_.row(index) = d_t_min;

          for(int l = 0; l < ndof; l++)
            {
              if(std::isnan(wrench_margin_f_min_jacobian_.row(index)(0, l)))
                wrench_margin_f_min_jacobian_.row(index)(0, l) = 0;
              if(std::isnan(wrench_margin_t_min_jacobian_.row(index)(0, l)))
                wrench_margin_t_min_jacobian_.row(index)(0, l) = 0;
            }
          index++;
        }
      } //j
    } //i

    // ROS_WARN_STREAM("approx_wrench_margin_f_min_ij_: " << approx_wrench_margin_f_min_ij_.transpose());
    // ROS_WARN_STREAM("approx_wrench_margin_t_min_ij_: " << approx_wrench_margin_t_min_ij_.transpose());
  }

#if 0
  // @depreacated 
  void RobotModel::calcWrenchMarginJacobian()
  {
    // reference:
    // https://ieeexplore.ieee.org/stamp/stamp.jsp?tp=&arnumber=8967725

    const int rotor_num = getRotorNum();
    const int joint_num = getJointNum();
    const int ndof = 6 + joint_num;
    const auto& p = getRotorsOriginFromCog<Eigen::Vector3d>();
    const auto& u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& sigma = getRotorDirection();
    const double thrust_max = getThrustUpperLimit();
    Eigen::Vector3d fg = getMass() * gravity_3d_;
    const double m_f_rate = getMFRate();

    const auto v = calcV();
    std::vector<Eigen::MatrixXd> v_jacobians;
    for (int i = 0; i < rotor_num; ++i) {
      v_jacobians.push_back(-skew(u.at(i)) * p_jacobians_.at(i) + skew(p.at(i)) * u_jacobians_.at(i) + m_f_rate * sigma.at(i + 1) * u_jacobians_.at(i));
    }

    // triple_product_jacobian
    std::vector<std::vector<std::vector<Eigen::VectorXd> > > u_triple_product_jacobian;
    std::vector<std::vector<std::vector<Eigen::VectorXd> > > v_triple_product_jacobian;
    u_triple_product_jacobian.resize(rotor_num);
    for (auto& j : u_triple_product_jacobian) {
      j.resize(rotor_num);
      for (auto& k : j) {
        k.resize(rotor_num);
        for (auto& vec : k) {
          vec.resize(ndof);
        }
      }
    }

    v_triple_product_jacobian.resize(rotor_num);
    for (auto& j : v_triple_product_jacobian) {
      j.resize(rotor_num);
      for (auto& k : j) {
        k.resize(rotor_num);
        for (auto& vec : k) {
          vec.resize(ndof);
        }
      }
    }

    //calc jacobian of f_min_ij, t_min_ij
    int f_min_index = 0;
    for (int i = 0; i < rotor_num; ++i) {
      for (int j = 0; j < rotor_num; ++j) {
        double f_min = 0.0;
        double t_min = 0.0;
        Eigen::VectorXd d_f_min = Eigen::VectorXd::Zero(ndof);
        Eigen::VectorXd d_t_min = Eigen::VectorXd::Zero(ndof);
        const Eigen::Vector3d& u_i = u.at(i);
        const Eigen::Vector3d& u_j = u.at(j);
        const Eigen::Vector3d uixuj = u_i.cross(u_j);
        const Eigen::Vector3d& v_i = v.at(i);
        const Eigen::Vector3d& v_j = v.at(j);
        const Eigen::Vector3d vixvj = v_i.cross(v_j);

        for (int k = 0; k < rotor_num; ++k) {
          if (i == j || j == k || k == i) {
            u_triple_product_jacobian.at(i).at(j).at(k) = Eigen::VectorXd::Zero(ndof);
            v_triple_product_jacobian.at(i).at(j).at(k) = Eigen::VectorXd::Zero(ndof);
          } else {
            const Eigen::Vector3d& u_k = u.at(k);
            const Eigen::Vector3d& v_k = v.at(k);
            const double u_triple_product = calcTripleProduct(u_i, u_j, u_k);
            const double v_triple_product = calcTripleProduct(v_i, v_j, v_k);
            for (int l = 0; l < ndof; ++l) {
              {
                const Eigen::Vector3d& d_u_i = u_jacobians_.at(i).col(l);
                const Eigen::Vector3d& d_u_j = u_jacobians_.at(j).col(l);
                const Eigen::Vector3d& d_u_k = u_jacobians_.at(k).col(l);
                const Eigen::Vector3d d_uixuj = u_i.cross(d_u_j) + d_u_i.cross(u_j);

                double d_u_triple_product = (uixuj / uixuj.norm()).dot(d_u_k) + u_k.dot(1/uixuj.norm() * d_uixuj - uixuj / (uixuj.norm() * uixuj.squaredNorm()) * uixuj.dot(d_uixuj));
                u_triple_product_jacobian.at(i).at(j).at(k)(l) = d_u_triple_product;
                d_f_min(l) += sigmoid(u_triple_product * thrust_max, epsilon_) * d_u_triple_product * thrust_max;
              }
              {
                const Eigen::Vector3d& d_v_i = v_jacobians.at(i).col(l);
                const Eigen::Vector3d& d_v_j = v_jacobians.at(j).col(l);
                const Eigen::Vector3d& d_v_k = v_jacobians.at(k).col(l);
                const Eigen::Vector3d d_vixvj = v_i.cross(d_v_j) + d_v_i.cross(v_j);

                double d_v_triple_product = (vixvj / vixvj.norm()).dot(d_v_k) + v_k.dot(1/vixvj.norm() * d_vixvj - vixvj / (vixvj.norm() * vixvj.squaredNorm()) * vixvj.dot(d_vixvj));
                v_triple_product_jacobian.at(i).at(j).at(k)(l) = d_v_triple_product;
                d_t_min(l) += sigmoid(v_triple_product * thrust_max, epsilon_) * d_v_triple_product * thrust_max;
              }
            } //l
            f_min += reluApprox(u_triple_product * thrust_max, epsilon_);
            t_min += reluApprox(v_triple_product * thrust_max, epsilon_);
          } //if
        } //k

        if (i != j) {
          double uixuj_fg = uixuj.dot(fg)/uixuj.norm();
          Eigen::VectorXd d_uixuj_fg(ndof);
          for (int l = 0; l < ndof; ++l) {
            const Eigen::Vector3d& d_u_i = u_jacobians_.at(i).col(l);
            const Eigen::Vector3d& d_u_j = u_jacobians_.at(j).col(l);
            const Eigen::Vector3d d_uixuj = u_i.cross(d_u_j) + d_u_i.cross(u_j);
            d_uixuj_fg(l) = fg.dot(1/uixuj.norm() * d_uixuj - uixuj / (uixuj.norm() * uixuj.squaredNorm()) * uixuj.dot(d_uixuj));
          } //l
          approx_wrench_margin_f_min_ij_(f_min_index) = absApprox(f_min - uixuj_fg, epsilon_);
          approx_wrench_margin_t_min_ij_(f_min_index) = absApprox(t_min, epsilon_);
          wrench_margin_f_min_jacobian_.row(f_min_index) = (tanh(f_min - uixuj_fg, epsilon_) * (d_f_min - d_uixuj_fg)).transpose();
          wrench_margin_t_min_jacobian_.row(f_min_index) = (tanh(t_min, epsilon_) * (d_t_min)).transpose();
          f_min_index++;
        }
      } //j
    } //i
  }
#endif

  Eigen::VectorXd RobotModel::getGravityWrenchOnRoot()
  {
    const auto& seg_frames = getSegmentsTf();
    const auto& inertia_map = getInertiaMap();

    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());
    Eigen::VectorXd wrench_g = Eigen::VectorXd::Zero(6);
    KDL::RigidBodyInertia link_inertia = KDL::RigidBodyInertia::Zero();
    for(const auto& inertia : inertia_map)
      {
        Eigen::MatrixXd jacobi_root = Eigen::MatrixXd::Identity(3, 6);
        Eigen::Vector3d p = root_rot * aerial_robot_model::kdlToEigen(seg_frames.at(inertia.first).p + seg_frames.at(inertia.first).M * inertia.second.getCOG());
        jacobi_root.rightCols(3) = - aerial_robot_model::skew(p);
        wrench_g += jacobi_root.transpose() *  inertia.second.getMass() * (-gravity_);
      }
    return wrench_g;
  }

  // TODO move to hydrus, this is only usefull to check the singularity like hydrus.
  Eigen::MatrixXd RobotModel::calcWrenchMatrixOnCoG()
  {
    const std::vector<Eigen::Vector3d> p = getRotorsOriginFromCog<Eigen::Vector3d>();
    const std::vector<Eigen::Vector3d> u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& sigma = getRotorDirection();
    const auto& rotor_direction = getRotorDirection();
    const int rotor_num = getRotorNum();
    const double m_f_rate = getMFRate();

    //Q : WrenchAllocationMatrix
    Eigen::MatrixXd Q(6, rotor_num);
    for (unsigned int i = 0; i < rotor_num; ++i) {
      Q.block(0, i, 3, 1) = u.at(i);
      Q.block(3, i, 3, 1) = p.at(i).cross(u.at(i)) + m_f_rate * sigma.at(i + 1) * u.at(i);
    }
    return Q;
  }

  void RobotModel::calcWrenchMatrixOnRoot()
  {
    const auto& seg_frames = getSegmentsTf();
    const std::vector<Eigen::Vector3d>& u = getRotorsNormalFromCog<Eigen::Vector3d>();
    const auto& sigma = getRotorDirection();
    const int rotor_num = getRotorNum();
    const double m_f_rate = getMFRate();

    Eigen::MatrixXd root_rot = aerial_robot_model::kdlToEigen(getCogDesireOrientation<KDL::Rotation>() * seg_frames.at(baselink_).M.Inverse());

    q_mat_ = Eigen::MatrixXd::Zero(6, rotor_num);
    for (unsigned int i = 0; i < rotor_num; ++i) {
      std::string rotor = "thrust" + std::to_string(i + 1);
      Eigen::MatrixXd q_i = Eigen::MatrixXd::Identity(6, 6);
      Eigen::Vector3d p = root_rot * aerial_robot_model::kdlToEigen(seg_frames.at(rotor).p);
      q_i.bottomLeftCorner(3,3) = aerial_robot_model::skew(p);

      Eigen::VectorXd wrench_unit = Eigen::VectorXd::Zero(6);
      wrench_unit.head(3) = u.at(i);
      wrench_unit.tail(3) = m_f_rate * sigma.at(i + 1) * u.at(i);

      thrust_wrench_units_.at(i) = wrench_unit;
      thrust_wrench_allocations_.at(i) = q_i;
      q_mat_.col(i) = q_i * wrench_unit;
    }
  }

} //namespace aerial_robot_model
