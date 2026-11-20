#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Eigen/Core"
#include "Eigen/Geometry"

#include "absl/memory/memory.h"

#include "cartographer/common/configuration_file_resolver.h"
#include "cartographer/common/lua_parameter_dictionary.h"
#include "cartographer/common/time.h"
#include "cartographer/mapping/map_builder.h"
#include "cartographer/mapping/trajectory_builder_interface.h"
#include "cartographer/sensor/point_cloud.h"
#include "cartographer/sensor/rangefinder_point.h"
#include "cartographer/sensor/timed_point_cloud_data.h"
#include "cartographer/transform/rigid_transform.h"

#include "gflags/gflags.h"
#include "glog/logging.h"

namespace {

int64_t NowUtsTicks() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() /
         100;  // 100 ns ticks (UTS)
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  using cartographer::mapping::MapBuilder;
  using cartographer::mapping::TrajectoryBuilderInterface;
  using cartographer::common::ConfigurationFileResolver;
  using cartographer::common::LuaParameterDictionary;
  using cartographer::transform::Rigid3d;

  const std::string config_dir =
      "/usr/local/share/cartographer/configuration_files";
  const std::string wrapper_dir = "/root/carto_standalone";
  const std::vector<std::string> resolver_dirs = {wrapper_dir, config_dir};

  auto resolver = absl::make_unique<ConfigurationFileResolver>(resolver_dirs);
  auto top_dict = absl::make_unique<LuaParameterDictionary>(
      resolver->GetFileContentOrDie("carto_2d.lua"),
      absl::make_unique<ConfigurationFileResolver>(resolver_dirs));

  auto map_builder_options = cartographer::mapping::CreateMapBuilderOptions(
      top_dict->GetDictionary("map_builder").get());
  auto trajectory_builder_options =
      cartographer::mapping::CreateTrajectoryBuilderOptions(
          top_dict->GetDictionary("trajectory_builder").get());

  MapBuilder map_builder(map_builder_options);

  using SensorId = TrajectoryBuilderInterface::SensorId;
  const std::set<SensorId> expected_sensor_ids = {
      SensorId{SensorId::SensorType::RANGE, "echoes"}};

  // Store the latest local SLAM pose estimate (written from the SLAM thread).
  Rigid3d latest_local_pose = Rigid3d::Identity();
  std::mutex pose_mutex;

  auto local_slam_callback =
      [&](int, cartographer::common::Time, Rigid3d local_pose,
          cartographer::sensor::RangeData,
          std::unique_ptr<const TrajectoryBuilderInterface::InsertionResult>) {
        std::lock_guard<std::mutex> lock(pose_mutex);
        latest_local_pose = local_pose;
      };

  const int trajectory_id = map_builder.AddTrajectoryBuilder(
      expected_sensor_ids, trajectory_builder_options, local_slam_callback);

  TrajectoryBuilderInterface* trajectory_builder =
      map_builder.GetTrajectoryBuilder(trajectory_id);
  CHECK(trajectory_builder != nullptr);

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream iss(line);
    double x = 0.0, y = 0.0, yaw = 0.0;
    if (!(iss >> x >> y >> yaw)) {
      continue;
    }

    cartographer::sensor::TimedPointCloud ranges;
    float px = 0.0f, py = 0.0f;
    while (iss >> px >> py) {
      ranges.push_back({Eigen::Vector3f(px, py, 0.0f), 0.0f});
    }
    if (ranges.empty()) {
      continue;
    }

    cartographer::sensor::TimedPointCloudData data;
    data.time = cartographer::common::FromUniversal(NowUtsTicks());
    data.origin =
        Eigen::Vector3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
    data.ranges = std::move(ranges);

    trajectory_builder->AddSensorData("echoes", data);

    Rigid3d local_pose;
    {
      std::lock_guard<std::mutex> lock(pose_mutex);
      local_pose = latest_local_pose;
    }
    const Rigid3d map_pose =
        map_builder.pose_graph()->GetLocalToGlobalTransform(trajectory_id) *
        local_pose;
    const Eigen::Vector3d t = map_pose.translation();
    const Eigen::Quaterniond q = map_pose.rotation();
    const double yaw_out = 2.0 * std::atan2(q.z(), q.w());

    std::cout << t.x() << " " << t.y() << " " << yaw_out << std::endl;
  }

  map_builder.FinishTrajectory(trajectory_id);
  return 0;
}
