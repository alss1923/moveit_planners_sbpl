#include <moveit_planners_sbpl/interface/ik_command_interactive_marker.h>

#include <Eigen/Dense>
#include <eigen_conversions/eigen_msg.h>

#include <moveit_planners_sbpl/interface/robot_command_model.h>

#include "utils.h"

namespace sbpl_interface {

IKCommandInteractiveMarker::IKCommandInteractiveMarker(RobotCommandModel* model)
    : m_im_server("phantom_controls")
{
    assert(model != NULL);
    m_model = model;
    connect(m_model, SIGNAL(robotLoaded()), this, SLOT(updateRobotModel()));
    connect(m_model, SIGNAL(robotStateChanged()), this, SLOT(updateRobotState()));
}

void IKCommandInteractiveMarker::setActiveJointGroup(const std::string& group_name) {
    if (group_name != m_active_group_name) {
        reinitInteractiveMarkers();
        m_active_group_name = group_name;
        Q_EMIT updateActiveJointGroup(group_name);
    }
}

static std::string markerNameFromTipName(const std::string& tip_name)
{
    return tip_name + "_controls";
}

static std::string tipNameFromMarkerName(const std::string& marker_name)
{
    return marker_name.substr(0, marker_name.rfind("_control"));
}

void IKCommandInteractiveMarker::updateRobotModel()
{
    reinitInteractiveMarkers();
}

void IKCommandInteractiveMarker::updateRobotState()
{
    updateInteractiveMarkers();
}

void IKCommandInteractiveMarker::processInteractiveMarkerFeedback(
    const visualization_msgs::InteractiveMarkerFeedbackConstPtr& msg)
{
    ROS_DEBUG("Interactive marker feedback");
    ROS_DEBUG("  Marker: %s", msg->marker_name.c_str());
    ROS_DEBUG("  Control: %s", msg->control_name.c_str());
    ROS_DEBUG("  Event Type: %u", (unsigned)msg->event_type);

    auto* robot_state = m_model->getRobotState();

    switch (msg->event_type) {
    case visualization_msgs::InteractiveMarkerFeedback::KEEP_ALIVE:
        break;
    case visualization_msgs::InteractiveMarkerFeedback::POSE_UPDATE:
    {
        auto* jg = robot_state->getJointModelGroup(m_active_group_name);
        if (!jg) {
            ROS_ERROR("Failed to retrieve joint group '%s'", m_active_group_name.c_str());
            break;
        }

        // run ik from this tip link
        Eigen::Affine3d wrist_pose;
        tf::poseMsgToEigen(msg->pose, wrist_pose);

        // extract the seed
        std::vector<double> seed;
        robot_state->copyJointGroupPositions(jg, seed);

        auto& rm = m_model->getRobotModel();
        assert(rm);

        if (m_model->setFromIK(jg, wrist_pose)) {
            // for each variable corresponding to a revolute joint
            for (size_t gvidx = 0; gvidx < seed.size(); ++gvidx) {
                ROS_DEBUG("Check variable '%s' for bounded revoluteness", jg->getVariableNames()[gvidx].c_str());

                int vidx = jg->getVariableIndexList()[gvidx];
                const moveit::core::JointModel* j = rm->getJointOfVariable(vidx);
                if (j->getType() != moveit::core::JointModel::REVOLUTE ||
                    !j->getVariableBounds()[0].position_bounded_)
                {
                    continue;
                }

                ROS_DEBUG("  Normalize variable '%s'", jg->getVariableNames()[gvidx].c_str());

                double spos = robot_state->getVariablePosition(vidx);
                double vdiff = seed[gvidx] - spos;
                int twopi_hops = (int)std::fabs(vdiff / (2.0 * M_PI));

                ROS_DEBUG(" -> seed pos: %0.3f", seed[gvidx]);
                ROS_DEBUG(" ->  sol pos: %0.3f", spos);
                ROS_DEBUG(" ->    vdiff: %0.3f", vdiff);
                ROS_DEBUG(" -> num hops: %d", twopi_hops);

                double npos = spos + (2.0 * M_PI) * twopi_hops * std::copysign(1.0, vdiff);
                if (fabs(npos - seed[gvidx]) > M_PI) {
                    npos += 2.0 * M_PI * std::copysign(1.0, vdiff);
                }

                ROS_DEBUG(" ->     npos: %0.3f", npos);

                if (twopi_hops) {
                    ROS_DEBUG(" -> Attempt to normalize variable '%s' to %0.3f from %0.3f", jg->getVariableNames()[gvidx].c_str(), npos, spos);
                } else {
                    ROS_DEBUG("No hops necessary");
                }

                m_model->setVariablePosition(vidx, npos);
                if (!robot_state->satisfiesBounds(j)) {
                    ROS_WARN("normalized value for '%s' out of bounds",  jg->getVariableNames()[gvidx].c_str());
                    m_model->setVariablePosition(vidx, spos);
                }
            }
        } else {
            // TODO: anything special here?
        }
    }   break;
    case visualization_msgs::InteractiveMarkerFeedback::MENU_SELECT:
    case visualization_msgs::InteractiveMarkerFeedback::BUTTON_CLICK:
    default:
        break;
    }
    std::string tip_link_name = tipNameFromMarkerName(msg->marker_name);
}

// This gets called whenever the robot model or active joint group changes.
void IKCommandInteractiveMarker::reinitInteractiveMarkers()
{
    auto& robot_model = m_model->getRobotModel();

    ROS_INFO("Setup Interactive Markers for Robot");

    ROS_INFO(" -> Remove any existing markers");
    m_im_server.clear();
    m_int_marker_names.clear();

    bool have_robot = (bool)robot_model;
    bool have_active_group = !m_active_group_name.empty();
    if (!have_robot || !have_active_group) {
        if (!have_robot) {
            ROS_WARN("No robot model to initialize interactive markers from");
        }
        if (!have_active_group) {
            ROS_WARN("No active joint group to initialize interactive markers from");
        }
        m_im_server.applyChanges(); // TODO: defer idiom here
        return;
    }

    auto* jg = robot_model->getJointModelGroup(m_active_group_name);
    if (!jg) {
        ROS_ERROR("Failed to retrieve joint group '%s'", m_active_group_name.c_str());
        m_im_server.applyChanges();
        return;
    }

    auto tips = GetTipLinks(*jg);

    for (auto* tip_link : tips) {
        ROS_INFO("Adding interactive marker for controlling pose of link %s", tip_link->getName().c_str());

        visualization_msgs::InteractiveMarker tip_marker;
        tip_marker.header.frame_id = robot_model->getModelFrame();

        tip_marker.pose.orientation.w = 1.0;
        tip_marker.pose.orientation.x = 0.0;
        tip_marker.pose.orientation.y = 0.0;
        tip_marker.pose.orientation.z = 0.0;
        tip_marker.pose.position.x = 0.0;
        tip_marker.pose.position.y = 0.0;
        tip_marker.pose.position.z = 0.0;

        tip_marker.name = markerNameFromTipName(tip_link->getName());
        tip_marker.description = "ik control of link " + tip_link->getName();
        tip_marker.scale = 0.20f;

        visualization_msgs::InteractiveMarkerControl dof_control;
        dof_control.orientation_mode =
                visualization_msgs::InteractiveMarkerControl::INHERIT;
        dof_control.always_visible = false;
//        dof_control.description = "pose_control";

        dof_control.orientation.w = 1.0;
        dof_control.orientation.x = 1.0;
        dof_control.orientation.y = 0.0;
        dof_control.orientation.z = 0.0;

        dof_control.name = "rotate_x";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
        tip_marker.controls.push_back(dof_control);

        dof_control.name = "move_x";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
        tip_marker.controls.push_back(dof_control);

        dof_control.orientation.w = 1.0;
        dof_control.orientation.x = 0.0;
        dof_control.orientation.y = 1.0;
        dof_control.orientation.z = 0.0;

        dof_control.name = "rotate_z";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
        tip_marker.controls.push_back(dof_control);

        dof_control.name = "move_z";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
        tip_marker.controls.push_back(dof_control);

        dof_control.orientation.w = 1.0;
        dof_control.orientation.x = 0.0;
        dof_control.orientation.y = 0.0;
        dof_control.orientation.z = 1.0;

        dof_control.name = "rotate_y";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::ROTATE_AXIS;
        tip_marker.controls.push_back(dof_control);

        dof_control.name = "move_y";
        dof_control.interaction_mode =
                visualization_msgs::InteractiveMarkerControl::MOVE_AXIS;
        tip_marker.controls.push_back(dof_control);

        auto feedback_fn = [this](const visualization_msgs::InteractiveMarkerFeedbackConstPtr& msg)
        {
            return this->processInteractiveMarkerFeedback(msg);
        };
        m_im_server.insert(tip_marker, feedback_fn);
        m_int_marker_names.push_back(tip_marker.name);
    }

    m_im_server.applyChanges();

//    updateInteractiveMarkers();
}

void IKCommandInteractiveMarker::updateInteractiveMarkers()
{
    auto* robot_state = m_model->getRobotState();
    for (auto& marker_name : m_int_marker_names) {
        // stuff the current pose
        std::string tip_link_name = tipNameFromMarkerName(marker_name);
        auto& T_model_tip = robot_state->getGlobalLinkTransform(tip_link_name);

        geometry_msgs::Pose tip_pose;
        tf::poseEigenToMsg(T_model_tip, tip_pose);

        // update the pose of the interactive marker
        std_msgs::Header header;
        header.frame_id = m_model->getRobotModel()->getModelFrame();
        header.stamp = ros::Time(0);
        if (!m_im_server.setPose(marker_name, tip_pose, header)) {
            ROS_ERROR("Failed to set pose of interactive marker '%s'", marker_name.c_str());
        }
    }

    m_im_server.applyChanges();
}

} // namespace sbpl_interface