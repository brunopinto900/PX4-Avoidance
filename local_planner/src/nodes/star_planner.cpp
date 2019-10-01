#include "local_planner/star_planner.h"

#include "avoidance/common.h"
#include "local_planner/planner_functions.h"
#include "local_planner/tree_node.h"

#include <ros/console.h>

namespace avoidance {

StarPlanner::StarPlanner() {}

// set parameters changed by dynamic rconfigure
void StarPlanner::dynamicReconfigureSetStarParams(const avoidance::LocalPlannerNodeConfig& config, uint32_t level) {
  children_per_node_ = config.children_per_node_;
  n_expanded_nodes_ = config.n_expanded_nodes_;
  tree_node_duration_ = static_cast<float>(config.tree_node_duration_);
  max_path_length_ = static_cast<float>(config.max_sensor_range_);
  smoothing_margin_degrees_ = static_cast<float>(config.smoothing_margin_degrees_);
  tree_heuristic_weight_ = static_cast<float>(config.tree_heuristic_weight_);
  max_sensor_range_ = static_cast<float>(config.max_sensor_range_);
  min_sensor_range_ = static_cast<float>(config.min_sensor_range_);
}

void StarPlanner::setParams(const costParameters& cost_params, const simulation_limits& limits, float acc_rad) {
  cost_params_ = cost_params;
  lims_ = limits;
  acceptance_radius_ = acc_rad;
}

void StarPlanner::setPose(const Eigen::Vector3f& pos, const Eigen::Vector3f& vel, const Eigen::Quaternionf& q) {
  position_ = pos;
  velocity_ = vel;
  q_ = q;
}

void StarPlanner::setGoal(const Eigen::Vector3f& goal) { goal_ = goal; }

void StarPlanner::setPointcloud(const kdtree_t& cloud) { cloud_ = cloud; }

void StarPlanner::setClosestPointOnLine(const Eigen::Vector3f& closest_pt) { closest_pt_ = closest_pt; }

float StarPlanner::treeHeuristicFunction(int node_number) const {
  return (goal_ - tree_[node_number].getPosition()).norm() * tree_heuristic_weight_;
}

void StarPlanner::buildLookAheadTree() {
  std::clock_t start_time = std::clock();
  // Simple 6-way unit direction setpoints allowed only.
  // TODO: If compute allowws, make this more fine-grained
  // These are in a shitty local-aligned but body-centered frame
  const std::vector<Eigen::Vector3f> candidates{Eigen::Vector3f{1.0f, 0.0f, 0.0f},  Eigen::Vector3f{0.0f, 1.0f, 0.0f},
                                                Eigen::Vector3f{0.0f, 0.0f, 1.0f},  Eigen::Vector3f{-1.0f, 0.0f, 0.0f},
                                                Eigen::Vector3f{0.0f, -1.0f, 0.0f}, Eigen::Vector3f{0.0f, 0.0f, -1.0f},
                                                Eigen::Vector3f{0.707f, 0.707f, 0.0f}, Eigen::Vector3f{0.707f, -0.707f, 0.0f},
                                                Eigen::Vector3f{-0.707f, 0.707f, 0.0f}, Eigen::Vector3f{-0.707f, -0.707f, 0.0f}};
  bool has_reached_goal = false;

  tree_.clear();
  closed_set_.clear();

  // insert first node
  simulation_state start_state;
  start_state.position = position_;
  start_state.velocity = velocity_;
  start_state.acceleration = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
  start_state.time = ros::Time::now().toSec();
  tree_.push_back(TreeNode(0, start_state, Eigen::Vector3f::Zero()));
  tree_.back().setCosts(treeHeuristicFunction(0), treeHeuristicFunction(0));

  int origin = 0;
  while (!has_reached_goal) {
    Eigen::Vector3f origin_position = tree_[origin].getPosition();
    Eigen::Vector3f origin_velocity = tree_[origin].getVelocity();

    simulation_limits limits = lims_;
    simulation_state state = tree_[origin].state;
    limits.max_xy_velocity_norm = std::min(
        std::min(
            computeMaxSpeedFromBrakingDistance(lims_.max_jerk_norm, lims_.max_acceleration_norm,
                                               (state.position - goal_).head<2>().norm()),
            computeMaxSpeedFromBrakingDistance(lims_.max_jerk_norm, lims_.max_acceleration_norm, max_sensor_range_)),
        lims_.max_xy_velocity_norm);

    // add candidates as nodes
    // insert new nodes
    int children = 0;
    // If we reach the acceptance radius or the sensor horizon, add goal as last node and exit
    if (origin > 1 && ((tree_[origin].getPosition() - goal_).norm() < acceptance_radius_) ||
        (tree_[origin].getPosition() - position_).norm() >= 2.0f * max_sensor_range_) {
      tree_.push_back(TreeNode(origin, simulation_state(0.f, goal_), goal_ - tree_[origin].getPosition()));
      closed_set_.push_back(origin);
      closed_set_.push_back(tree_.size() - 1);
      has_reached_goal = true;
      break;
    }

    for (const auto& candidate : candidates) {
      simulation_state state = tree_[origin].state;
      TrajectorySimulator sim(limits, state, 0.05f);  // todo: parameterize simulation step size [s]
      std::vector<simulation_state> trajectory = sim.generate_trajectory(q_*candidate, tree_node_duration_);

      // check if another close node has been added
      float dist = 1.f;
      int close_nodes = 0;
      for (size_t i = 0; i < tree_.size(); i++) {
        dist = (tree_[i].getPosition() - trajectory.back().position).norm();
        if (dist < 0.2f) {
          close_nodes++;
          break;
        }
      }

      if (close_nodes == 0) {
        tree_.push_back(TreeNode(origin, trajectory.back(), q_*candidate));
        float h = treeHeuristicFunction(tree_.size() - 1);
        tree_.back().heuristic_ = h;
        tree_.back().total_cost_ = tree_[origin].total_cost_ - tree_[origin].heuristic_ +
                                   simpleCost(tree_.back(), goal_, cost_params_, cloud_) + h;
        children++;
      }
    }

    closed_set_.push_back(origin);
    tree_[origin].closed_ = true;

    // find best node to continue
    float minimal_cost = FLT_MAX;
    for (size_t i = 0; i < tree_.size(); i++) {
      if (!(tree_[i].closed_)) {
        if (tree_[i].total_cost_ < minimal_cost) {
          minimal_cost = tree_[i].total_cost_;
          origin = i;
        }
      }
    }

    // if there is only one node in the tree, we already expanded it.
    if (tree_.size() <= 1) {
      has_reached_goal = true;
    }
  }

  // Get setpoints into member vector
  int tree_end = origin;
  path_node_setpoints_.clear();
  while (tree_end > 0) {
    path_node_setpoints_.push_back(tree_[tree_end].getSetpoint());
    tree_end = tree_[tree_end].origin_;
  }

  path_node_setpoints_.push_back(tree_[0].getSetpoint());
  if ((path_node_setpoints_.size() - 2) >= 0) {
    starting_direction_ = path_node_setpoints_[path_node_setpoints_.size() - 2];
  }
}
}
