#!/usr/bin/env python3
from launch import LaunchDescription
from launch_ros.actions import Node
import os

def generate_launch_description():
    config_dir = '/ws/carto_config'
    return LaunchDescription([
        # Static TF: base_footprint -> base_lidar_link
        Node(
            package='tf2_ros',
            executable='static_transform_publisher',
            name='base_footprint_to_base_lidar_link',
            arguments=['0.0398', '0', '0.12', '1.5708', '0', '0', 'base_footprint', 'base_lidar_link'],
        ),
        # LiDAR (D500 = LD19, порт ttyACM1)
        Node(
            package='ldlidar',
            executable='ldlidar_node',
            name='ldlidar',
            output='screen',
            parameters=[
                {'product_name': 'LDLiDAR_LD19'},
                {'topic_name': 'scan'},
                {'frame_id': 'base_lidar_link'},
                {'port_name': '/dev/ttyACM1'},
                {'port_baudrate': 230400},
                {'laser_scan_dir': True},
                {'enable_angle_crop_func': True},
                {'angle_crop_min': 225.0},
                {'angle_crop_max': 315.0},
            ],
        ),
        # IMU complementary filter (как малинка: /imu/data_raw -> /imu/data)
        Node(
            package='imu_complementary_filter',
            executable='complementary_filter_node',
            name='complementary_filter_gain_node',
            output='screen',
            parameters=[
                {'do_bias_estimation': True},
                {'do_adaptive_gain': True},
                {'use_mag': False},
                {'gain_acc': 0.01},
                {'gain_mag': 0.01},
            ],
        ),
        # EKF (robot_localization): фьюзит /odom_raw + /imu/data -> /odom + TF
        Node(
            package='robot_localization',
            executable='ekf_node',
            name='ekf_filter_node',
            output='screen',
            parameters=['/ws/ekf.yaml'],
            remappings=[('/odometry/filtered', '/odom')],
        ),
        # Cartographer SLAM node
        Node(
            package='cartographer_ros',
            executable='cartographer_node',
            name='cartographer_node',
            output='screen',
            arguments=['-configuration_directory', config_dir,
                       '-configuration_basename', 'mapping_2d.lua'],
        ),
        # Cartographer occupancy grid (publishes /map)
        Node(
            package='cartographer_ros',
            executable='cartographer_occupancy_grid_node',
            name='cartographer_occupancy_grid_node',
            output='screen',
            arguments=['-resolution', '0.05', '-publish_period_sec', '1.0'],
        ),
        # rosbridge WebSocket (порт 9090) — для Foxglove на ПК
        Node(
            package='rosbridge_server',
            executable='rosbridge_websocket',
            name='rosbridge_websocket',
            output='screen',
            parameters=[{'port': 9090}],
        ),
        # rosapi — сервисы для Foxglove
        Node(
            package='rosapi',
            executable='rosapi_node',
            name='rosapi',
            output='screen',
        ),
    ])
