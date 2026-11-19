// Standalone Hector SLAM: reads scan points from stdin, outputs pose to stdout.
// Input line: "x1 y1 x2 y2 ..." (points in METERS, robot frame).
// Output line: "x y yaw" (pose in METERS + radians).
#include <iostream>
#include <sstream>
#include <string>
#include <Eigen/Core>
#include <hector_slam_lib/slam_main/HectorSlamProcessor.h>
#include <hector_slam_lib/scan/DataPointContainer.h>

int main(int argc, char** argv) {
    float resolution = 0.025f;
    int mapSize = 1024;
    int levels = 3;
    hectorslam::HectorSlamProcessor slam(resolution, mapSize, mapSize,
                                          Eigen::Vector2f(0.5f, 0.5f),
                                          levels);
    slam.setUpdateFactorFree(0.4f);
    slam.setUpdateFactorOccupied(0.9f);
    slam.setMapUpdateMinDistDiff(0.4f);
    slam.setMapUpdateMinAngleDiff(0.9f);
    float scale = slam.getScaleToMap();
    std::cerr << "hector_standalone ready, scale=" << scale << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        float yaw_prior = 0.0f;
        iss >> yaw_prior;  // heading prior (radians) from IMU
        hectorslam::DataContainer container;
        container.setOrigo(Eigen::Vector2f::Zero());
        float x, y;
        while (iss >> x >> y) {
            container.add(Eigen::Vector2f(x * scale, y * scale));
        }
        if (container.getSize() == 0) {
            continue;
        }
        Eigen::Vector3f pose = slam.getLastScanMatchPose();
        pose.z() = yaw_prior;  // use IMU heading as the initial yaw estimate
        slam.update(container, pose);
        pose = slam.getLastScanMatchPose();
        std::cout << pose.x() << " " << pose.y() << " " << pose.z() << std::endl;
    }
    return 0;
}
