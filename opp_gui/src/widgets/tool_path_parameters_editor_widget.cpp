/*
 * Copyright 2019 Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "opp_gui/widgets/tool_path_parameters_editor_widget.h"

#include <QMessageBox>
#include <QProgressDialog>

#include <eigen_conversions/eigen_msg.h>
#include <ros/ros.h>
#include <std_srvs/Trigger.h>

#include <opp_area_selection/selection_artist.h>
#include <opp_path_selection/path_selection_artist.h>
#include "opp_gui/utils.h"
#include "ui_tool_path_parameters_editor.h"
#include <heat_msgs/PolylineSmoother.h> // the service to smooth and sample a trajectory

//#include <smooth_pose_traj/polyline_smoother.hpp>

const static std::string GENERATE_TOOLPATHS_ACTION = "generate_tool_paths";
const static std::string GENERATE_HEAT_TOOLPATHS_ACTION = "generate_heat_tool_paths";
static const std::string SMOOTH_POLYLINE_SERVICE = "polyline_smoother";

namespace opp_gui
{
ToolPathParametersEditorWidget::ToolPathParametersEditorWidget(ros::NodeHandle& nh, QWidget* parent)
  : QWidget(parent), client_(GENERATE_TOOLPATHS_ACTION, false), heat_client_(GENERATE_HEAT_TOOLPATHS_ACTION, false)
{
  ui_ = new Ui::ToolPathParametersEditor();
  ui_->setupUi(this);

  ui_->double_spin_box_line_spacing->setRange(0.0, std::numeric_limits<double>::max());
  ui_->double_spin_box_min_hole_size->setRange(0.0, std::numeric_limits<double>::max());
  ui_->double_spin_box_point_spacing->setRange(0.0, std::numeric_limits<double>::max());
  ui_->double_spin_box_tool_z_offset->setRange(-std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
  ui_->double_spin_box_min_segment_length->setRange(0.0, std::numeric_limits<double>::max());
  ui_->double_spin_box_intersecting_plane_height->setRange(0.0, std::numeric_limits<double>::max());
  ui_->double_spin_box_raster_angle->setRange(-180., 180.);

  ui_->spin_box_dwell_time->setRange(0, std::numeric_limits<int>::max());

  // Add values to the process type drop-down
  ui_->combo_box_process_type->addItem("None", QVariant(opp_msgs::ProcessType::NONE));
  ui_->combo_box_process_type->addItem("Process Paint", QVariant(opp_msgs::ProcessType::PROCESS_PAINT));
  ui_->combo_box_process_type->addItem("Process De-paint", QVariant(opp_msgs::ProcessType::PROCESS_DEPAINT));

  // Connect the button press to the function that calls the toolpath generation action
  connect(ui_->push_button_generate, &QPushButton::clicked, this, &ToolPathParametersEditorWidget::generateToolPath);
  connect(ui_->combo_box_process_type,
          &QComboBox::currentTextChanged,
          this,
          &ToolPathParametersEditorWidget::updateProcessType);
  connect(ui_->spin_box_dwell_time,
          static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this,
          &ToolPathParametersEditorWidget::updateDwellTime);
  connect(this,
    &ToolPathParametersEditorWidget::polylinePath,
          this,
          &ToolPathParametersEditorWidget::onPolylinePath);
  connect(this,
	  &ToolPathParametersEditorWidget::polylinePathGen,
          this,
          &ToolPathParametersEditorWidget::onPolylinePathGen);
  connect(this,
	  &ToolPathParametersEditorWidget::QWarningBox,
          this,
          &ToolPathParametersEditorWidget::onQWarningBox);

  polyline_smooth_client_ = nh.serviceClient<heat_msgs::PolylineSmoother>(SMOOTH_POLYLINE_SERVICE);
}

void ToolPathParametersEditorWidget::init(const shape_msgs::Mesh& mesh) { mesh_.reset(new shape_msgs::Mesh(mesh)); }

void ToolPathParametersEditorWidget::setToolPathConfig(const noether_msgs::ToolPathConfig& config)
{
  ui_->double_spin_box_point_spacing->setValue(config.pt_spacing);
  ui_->double_spin_box_tool_z_offset->setValue(config.tool_offset);
  ui_->double_spin_box_line_spacing->setValue(config.line_spacing);
  ui_->double_spin_box_min_hole_size->setValue(config.min_hole_size);
  ui_->double_spin_box_min_segment_length->setValue(config.min_segment_size);
  ui_->double_spin_box_intersecting_plane_height->setValue(config.intersecting_plane_height);
  ui_->double_spin_box_raster_angle->setValue(config.raster_angle * 180.0 / M_PI);
}

noether_msgs::ToolPathConfig ToolPathParametersEditorWidget::getToolPathConfig() const
{
  noether_msgs::ToolPathConfig config;

  // Create a path configuration from the line edit fields
  config.pt_spacing = ui_->double_spin_box_point_spacing->value();
  config.tool_offset = ui_->double_spin_box_tool_z_offset->value();
  config.line_spacing = ui_->double_spin_box_line_spacing->value();
  config.min_hole_size = ui_->double_spin_box_min_hole_size->value();
  config.min_segment_size = ui_->double_spin_box_min_segment_length->value();
  config.intersecting_plane_height = ui_->double_spin_box_intersecting_plane_height->value();
  config.raster_angle = ui_->double_spin_box_raster_angle->value() * M_PI / 180.0;
  config.raster_wrt_global_axes = false;

  return config;
}

heat_msgs::HeatToolPathConfig ToolPathParametersEditorWidget::getHeatToolPathConfig() const
{
  heat_msgs::HeatToolPathConfig config;

  // Create a path configuration from the line edit fields
  config.pt_spacing = ui_->double_spin_box_point_spacing->value();
  config.line_spacing = ui_->double_spin_box_line_spacing->value();
  config.tool_offset = ui_->double_spin_box_tool_z_offset->value();
  config.min_hole_size = ui_->double_spin_box_min_hole_size->value();
  config.min_segment_size = ui_->double_spin_box_min_segment_length->value();
  config.raster_angle = ui_->double_spin_box_raster_angle->value() * M_PI / 180.0;
  config.generate_extra_rasters = false;  // No option to set this from GUI at present.
  return config;
}

void ToolPathParametersEditorWidget::setToolPath(const opp_msgs::ToolPath& tool_path)
{
  if (!tool_path_)
  {
    tool_path_.reset(new opp_msgs::ToolPath(tool_path));
  }
  else
  {
    *tool_path_ = tool_path;
    ui_->combo_box_process_type->setCurrentIndex(ui_->combo_box_process_type->findData(tool_path_->process_type.val));
    ui_->spin_box_dwell_time->setValue(static_cast<int>(tool_path_->dwell_time));
  }
}

bool ToolPathParametersEditorWidget::getToolPath(opp_msgs::ToolPath& tool_path) const
{
  if (tool_path_)
  {
    tool_path = *tool_path_;
    return true;
  }
  return false;
}

void ToolPathParametersEditorWidget::generateToolPath()
{
  if (!client_.isServerConnected())
  {
    std::string message = "Action server on '" + GENERATE_TOOLPATHS_ACTION + "' is not connected";
    emit QWarningBox(message);
    return;
  }

  if (!mesh_)
  {
    emit QWarningBox("Mesh has not yet been specified");
    return;
  }

  // Create an action goal
  noether_msgs::GenerateToolPathsGoal goal;
  goal.path_configs.push_back(getToolPathConfig());
  goal.surface_meshes.push_back(*mesh_);
  goal.proceed_on_failure = false;

  client_.sendGoal(goal, boost::bind(&ToolPathParametersEditorWidget::onGenerateToolPathsComplete, this, _1, _2));

  progress_dialog_ = new QProgressDialog(this);
  progress_dialog_->setModal(true);
  progress_dialog_->setLabelText("Path Planning Progress");
  progress_dialog_->setMinimum(0);
  progress_dialog_->setMaximum(100);

  progress_dialog_->setValue(progress_dialog_->minimum());
  progress_dialog_->show();
}

void ToolPathParametersEditorWidget::onPolylinePath(const std::vector<int> pnt_indices)
{
  if (!mesh_)
  {
    emit QWarningBox("Mesh has not yet been specified");
    return;
  }

  heat_msgs::PolylineSmoother::Request smooth_req;
  heat_msgs::PolylineSmoother::Response smooth_res;
  smooth_req.surface_mesh = *mesh_;
  for(int i=0; i<pnt_indices.size(); i++)
  {
    smooth_req.source_indices.push_back(pnt_indices[i]);
  }
  if(pnt_indices.size() == 0)
  {
    emit QWarningBox("No path points selected, using raster angle instead");
  }
  smooth_req.pt_spacing = ui_->double_spin_box_point_spacing->value();;

  polyline_smooth_client_.call(smooth_req, smooth_res);

  actionlib::SimpleClientGoalState state = actionlib::SimpleClientGoalState::SUCCEEDED;
  heat_msgs::GenerateHeatToolPathsResult goal_;
  goal_.success = true;
  goal_.tool_path_validities.push_back(true);
  heat_msgs::HeatToolPath tool_path;
  tool_path.paths.push_back(smooth_res.output_poses);
  goal_.tool_raster_paths.push_back(tool_path);
  auto goal = boost::make_shared<heat_msgs::GenerateHeatToolPathsResult>(goal_);

  progress_dialog_ = new QProgressDialog(this);
  progress_dialog_->setModal(true);
  progress_dialog_->setLabelText("Heat Path Planning Progress");
  progress_dialog_->setMinimum(0);
  progress_dialog_->setMaximum(100);
  progress_dialog_->setValue(progress_dialog_->minimum());
  progress_dialog_->show();

  ToolPathParametersEditorWidget::onGenerateHeatToolPathsComplete(state, goal);
}


void ToolPathParametersEditorWidget::onPolylinePathGen(const std::vector<int> pnt_indices)
{
  // TODO call heat method server with mesh and pnt_indices to generate a new set of paths.
  if (!heat_client_.isServerConnected())
  {
    std::string message = "Action server on '" + GENERATE_HEAT_TOOLPATHS_ACTION + "' is not connected";
    emit QWarningBox(message);
    return;
  }

  if (!mesh_)
  {
    emit QWarningBox("Mesh has not yet been specified");
    return;
  }

  // Create an action goal with only one set of sources and configs
  heat_msgs::GenerateHeatToolPathsGoal goal;
  // copy pnt_indices into goal as the sources
  heat_msgs::Source S;
  for(int i=0; i<pnt_indices.size(); i++)
    {
      S.source_indices.push_back(pnt_indices[i]);
    }
  goal.sources.push_back(S);
  goal.path_configs.push_back(getHeatToolPathConfig());
  goal.surface_meshes.push_back(*mesh_);
  goal.proceed_on_failure = false;

  heat_client_.sendGoal(goal, boost::bind(&ToolPathParametersEditorWidget::onGenerateHeatToolPathsComplete, this, _1, _2));
  
  progress_dialog_ = new QProgressDialog(this);
  progress_dialog_->setModal(true);
  progress_dialog_->setLabelText("Heat Path Planning Progress");
  progress_dialog_->setMinimum(0);
  progress_dialog_->setMaximum(100);

  progress_dialog_->setValue(progress_dialog_->minimum());
  progress_dialog_->show();
}

void ToolPathParametersEditorWidget::onGenerateToolPathsComplete(
    const actionlib::SimpleClientGoalState& state,
    const noether_msgs::GenerateToolPathsResultConstPtr& res)
{
  for (int i = progress_dialog_->minimum(); i < progress_dialog_->maximum(); ++i)
  {
    progress_dialog_->setValue(i);
    ros::Duration(0.01).sleep();
  }
  progress_dialog_->hide();

  if (state != actionlib::SimpleClientGoalState::SUCCEEDED)
  {
    std::string message = "Action '" + GENERATE_TOOLPATHS_ACTION + "' failed to succeed";
    emit QWarningBox(message);
  }
  else
  {
    if (!res->success || !res->tool_path_validities[0])
    {
      emit QWarningBox("Tool path generation failed");
    }
    else
    {
      ROS_INFO_STREAM("Successfully generated tool path");

      opp_msgs::ToolPath tp;
      tp.header.stamp = ros::Time::now();
      tp.process_type.val = qvariant_cast<opp_msgs::ProcessType::_val_type>(ui_->combo_box_process_type->currentData());
      tp.paths = res->tool_raster_paths[0].paths;
      tp.params.config.pt_spacing = ui_->double_spin_box_point_spacing->value();
      tp.params.config.tool_offset = ui_->double_spin_box_tool_z_offset->value();
      tp.params.config.line_spacing = ui_->double_spin_box_line_spacing->value();
      tp.params.config.raster_angle = ui_->double_spin_box_raster_angle->value() * M_PI / 180.0;
      tp.params.config.min_hole_size = ui_->double_spin_box_min_hole_size->value();
      tp.params.config.min_segment_size = ui_->double_spin_box_min_segment_length->value();
      tp.params.config.generate_extra_rasters = false;  // No option to set this from GUI at present.
      tp.params.config.raster_wrt_global_axes = false;  // No option to set this from GUI at present.
      tp.params.config.intersecting_plane_height = ui_->double_spin_box_intersecting_plane_height->value();

      // Create the tool path offset transform
      // 1. Add z offset
      // 2. Rotate 180 degrees about X
      Eigen::Isometry3d tool_offset = Eigen::Isometry3d::Identity();
      tool_offset.rotate(
          Eigen::AngleAxisd(ui_->double_spin_box_tool_pitch->value() * M_PI / 180.0, Eigen::Vector3d::UnitY()));
      tool_offset.translate(Eigen::Vector3d(0.0, 0.0, ui_->double_spin_box_tool_z_offset->value()));
      tool_offset.rotate(Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX()));
      tf::poseEigenToMsg(tool_offset, tp.tool_offset);

      // Save the toolpath
      tool_path_.reset(new opp_msgs::ToolPath(tp));

      emit dataChanged();
    }
  }
}

void ToolPathParametersEditorWidget::onQWarningBox(std::string warn_string)
{
  QMessageBox::warning(this, "Tool Path Planning Warning", QString(warn_string.c_str()));
}

void ToolPathParametersEditorWidget::onGenerateHeatToolPathsComplete(
    const actionlib::SimpleClientGoalState& state,
    const heat_msgs::GenerateHeatToolPathsResultConstPtr& res)
{
  for (int i = progress_dialog_->minimum(); i < progress_dialog_->maximum(); ++i)
  {
    progress_dialog_->setValue(i);
    ros::Duration(0.01).sleep();
  }
  progress_dialog_->hide();
  if (state != actionlib::SimpleClientGoalState::SUCCEEDED)
  {
    std::string message = "Action '" + GENERATE_HEAT_TOOLPATHS_ACTION + "' failed to succeed";
    emit QWarningBox(message);
  }
  else
  {
    if (!res->success || !res->tool_path_validities[0])
    {
      emit QWarningBox(std::string("Heat tool path generation failed"));
    }
    else
    {
      ROS_INFO_STREAM("Successfully generated heat tool path");

      opp_msgs::ToolPath tp;
      tp.header.stamp = ros::Time::now();
      tp.process_type.val = qvariant_cast<opp_msgs::ProcessType::_val_type>(ui_->combo_box_process_type->currentData());
      tp.paths = res->tool_raster_paths[0].paths;
      tp.params.config.pt_spacing = ui_->double_spin_box_point_spacing->value();
      tp.params.config.tool_offset = ui_->double_spin_box_tool_z_offset->value();
      tp.params.config.line_spacing = ui_->double_spin_box_line_spacing->value();
      tp.params.config.raster_angle = ui_->double_spin_box_raster_angle->value() * M_PI / 180.0;
      tp.params.config.min_hole_size = ui_->double_spin_box_min_hole_size->value();
      tp.params.config.min_segment_size = ui_->double_spin_box_min_segment_length->value();
      tp.params.config.generate_extra_rasters = false;  // No option to set this from GUI at present.
      tp.params.config.raster_wrt_global_axes = false;  // No option to set this from GUI at present.
      tp.params.config.intersecting_plane_height = ui_->double_spin_box_intersecting_plane_height->value();

      // Create the tool path offset transform
      // 1. Add z offset
      // 2. Rotate 180 degrees about X
      Eigen::Isometry3d tool_offset = Eigen::Isometry3d::Identity();
      tool_offset.rotate(
          Eigen::AngleAxisd(ui_->double_spin_box_tool_pitch->value() * M_PI / 180.0, Eigen::Vector3d::UnitY()));
      tool_offset.translate(Eigen::Vector3d(0.0, 0.0, ui_->double_spin_box_tool_z_offset->value()));
      tool_offset.rotate(Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX()));
      tf::poseEigenToMsg(tool_offset, tp.tool_offset);

      // Save the toolpath
      tool_path_.reset(new opp_msgs::ToolPath(tp));

      emit dataChanged();
    }
  }
}

void ToolPathParametersEditorWidget::updateProcessType(const QString&)
{
  if (tool_path_)
  {
    tool_path_->process_type.val =
        qvariant_cast<opp_msgs::ProcessType::_val_type>(ui_->combo_box_process_type->currentData());
  }
}

void ToolPathParametersEditorWidget::updateDwellTime(const int value)
{
  if (tool_path_)
  {
    tool_path_->dwell_time = static_cast<uint32_t>(value * 60);
  }
}

}  // namespace opp_gui
