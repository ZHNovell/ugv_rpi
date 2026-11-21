#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
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
#include "cartographer/common/port.h"
#include "cartographer/common/time.h"
#include "cartographer/mapping/id.h"
#include "cartographer/mapping/map_builder.h"
#include "cartographer/mapping/proto/submap_visualization.pb.h"
#include "cartographer/mapping/trajectory_builder_interface.h"
#include "cartographer/sensor/odometry_data.h"
#include "cartographer/sensor/point_cloud.h"
#include "cartographer/sensor/rangefinder_point.h"
#include "cartographer/sensor/timed_point_cloud_data.h"
#include "cartographer/transform/rigid_transform.h"
#include "cartographer/transform/transform.h"

#include "gflags/gflags.h"
#include "glog/logging.h"

namespace {

using cartographer::common::FromUniversal;
using cartographer::transform::Rigid3d;
using cartographer::mapping::MapBuilder;
using cartographer::mapping::proto::SubmapQuery;

int64_t NowUtsTicks() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() /
         100;
}

double NowSeconds() {
  return std::chrono::duration_cast<std::chrono::duration<double>>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WriteMap(MapBuilder& map_builder, const std::string& path) {
  constexpr int kGridSize = 1000;
  constexpr double kResolution = 0.05;  // m / cell
  std::vector<uint8_t> grid(kGridSize * kGridSize, 128);  // 128 = unknown

  const auto submap_poses = map_builder.pose_graph()->GetAllSubmapPoses();
  for (const auto& entry : submap_poses) {
    const auto& submap_id = entry.id;
    SubmapQuery::Response response;
    map_builder.SubmapToProto(submap_id, &response);
    for (const auto& texture : response.textures()) {
      std::string decompressed;
      cartographer::common::FastGunzipString(texture.cells(), &decompressed);
      const int w = texture.width();
      const int h = texture.height();
      if (static_cast<int>(decompressed.size()) < 2 * w * h) {
        continue;
      }
      const double res = texture.resolution();
      const Rigid3d slice_pose =
          cartographer::transform::ToRigid3(texture.slice_pose());
      {
        static bool dumped = false;
        if (!dumped) {
          dumped = true;
          std::ofstream dbg("/tmp/wm.txt");
          dbg << "slice " << slice_pose.translation().x() << " " << slice_pose.translation().y() << "\n";
          dbg << "wh " << w << " " << h << " res " << res << "\n";
          dbg.close();
        }
      }
      for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
          const int idx = py * w + px;
          const uint8_t value = static_cast<uint8_t>(decompressed[2 * idx]);
          const uint8_t alpha = static_cast<uint8_t>(decompressed[2 * idx + 1]);
          uint8_t map_val;
          if (value == 0 && alpha == 0) {
            map_val = 128;       // unknown
          } else if (value == 0) {
            map_val = 255;       // wall
          } else {
            map_val = 0;         // free
          }
          const Eigen::Vector3d local(-py * res, -px * res, 0.0);
          const Eigen::Vector3d world = slice_pose * local;
          const int gx = kGridSize / 2 -
                         static_cast<int>(std::floor(world.x() / kResolution));
          const int gy = static_cast<int>(std::floor(world.y() / kResolution)) +
                         kGridSize / 2;
          if (gx < 0 || gx >= kGridSize || gy < 0 || gy >= kGridSize) {
            continue;
          }
          uint8_t& cell = grid[gy * kGridSize + gx];
          if (map_val == 255) {
            cell = 255;
          } else if (map_val == 0 && cell == 128) {
            cell = 0;
          }
        }
      }
    }
  }

  const std::string tmp = path + ".tmp";
  std::ofstream ofs(tmp, std::ios::binary);
  ofs.write(reinterpret_cast<const char*>(grid.data()),
            static_cast<std::streamsize>(grid.size()));
  ofs.close();
  std::rename(tmp.c_str(), path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  using cartographer::mapping::TrajectoryBuilderInterface;
  using cartographer::common::ConfigurationFileResolver;
  using cartographer::common::LuaParameterDictionary;
  using cartographer::sensor::OdometryData;
  using cartographer::sensor::TimedPointCloud;
  using cartographer::sensor::TimedPointCloudData;

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
      SensorId{SensorId::SensorType::RANGE, "echoes"},
      SensorId{SensorId::SensorType::ODOMETRY, "odometry"}};

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

  double last_map_write = 0.0;

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream iss(line);
    double x = 0.0, y = 0.0, yaw = 0.0;
    if (!(iss >> x >> y >> yaw)) {
      continue;
    }

    TimedPointCloud ranges;
    float px = 0.0f, py = 0.0f;
    while (iss >> px >> py) {
      ranges.push_back({Eigen::Vector3f(px, py, 0.0f), 0.0f});
    }
    if (ranges.empty()) {
      continue;
    }

    const cartographer::common::Time time = FromUniversal(NowUtsTicks());

    // Odometry (pose of the robot / range sensor in the tracking frame).
    const Rigid3d odom_pose(
        Eigen::Vector3d(x, y, 0.0),
        Eigen::Quaterniond(std::cos(yaw / 2.0), 0.0, 0.0,
                           std::sin(yaw / 2.0)));
    trajectory_builder->AddSensorData(
        "odometry", OdometryData{time, odom_pose});

    // Range data: origin is the fixed lidar offset (center-mounted => 0,0,0).
    TimedPointCloudData data;
    data.time = time;
    data.origin = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
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

    const Rigid3d local_to_global =
        map_builder.pose_graph()->GetLocalToGlobalTransform(trajectory_id);
    const Eigen::Vector3d lt = local_pose.translation();
    const double lyaw = 2.0 * std::atan2(local_pose.rotation().z(), local_pose.rotation().w());
    const Eigen::Vector3d T = local_to_global.translation();
    const double Tyaw = 2.0 * std::atan2(local_to_global.rotation().z(), local_to_global.rotation().w());
    std::cout << lt.x() << " " << lt.y() << " " << lyaw << " "
              << T.x() << " " << T.y() << " " << Tyaw << " "
              << t.x() << " " << t.y() << " " << yaw_out << std::endl;

    const double now = NowSeconds();
    if (now - last_map_write > 1.0) {
      last_map_write = now;
      WriteMap(map_builder, "/dev/shm/carto_map.raw");
    }
  }

  map_builder.FinishTrajectory(trajectory_id);
  WriteMap(map_builder, "/dev/shm/carto_map.raw");
  return 0;
}
