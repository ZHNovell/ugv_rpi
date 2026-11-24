import os
from launch import LaunchDescription
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory

def generate_launch_description():
    # Малинкин SLAM-стек (bringup_lidar + rf2o + base_node + cartographer + robot_pose_publisher)
    cartographer_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            os.path.join(get_package_share_directory('ugv_slam'), 'launch', 'cartographer.launch.py')
        ),
        launch_arguments={'use_rviz': 'false'}.items()
    )

    # rosbridge + rosapi для Foxglove
    rosbridge = Node(
        package='rosbridge_server',
        executable='rosbridge_websocket',
        parameters=[{'port': 9090}],
    )
    rosapi = Node(
        package='rosapi',
        executable='rosapi_node',
    )

    return LaunchDescription([
        cartographer_launch,
        rosbridge,
        rosapi,
    ])
