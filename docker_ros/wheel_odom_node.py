#!/usr/bin/env python3
"""Wheel odometry + IMU node (1:1 порт малинки: ugv_bringup + base_node_ekf).

Читает ESP32 telemetry (odl/odr/gz) из /dev/shm/esp_telemetry.json.
Публикует:
  - /odom_raw  (nav_msgs/Odometry): позиция+угол из колёс, малинкины ковариации.
  - /imu/data_raw (sensor_msgs/Imu): аксел+гироскоп (малинкины формулы).
imu_complementary_filter -> /imu/data, EKF фьюзит /odom_raw + /imu/data -> /odom + TF.
"""
import rclpy
from rclpy.node import Node
from nav_msgs.msg import Odometry
from sensor_msgs.msg import Imu
from geometry_msgs.msg import Quaternion
import math
import json
import os
import time

WHEEL_BASE = 0.175   # малинка
# 1 тик = pi*WHEEL_D/ONE_CIRCLE_PLUSES = 0.1196 мм/тик (прошивка ESP32, рабочее 08:45)
MM_PER_TICK = 0.1196
SHM = '/dev/shm/esp_telemetry.json'

# малинкины ковариации (base_node_ekf.cpp)
ODOM_POSE_COV = [1e-3,0.0,0.0,0.0,0.0,0.0,  0.0,1e-3,0.0,0.0,0.0,0.0,  0.0,0.0,1e6,0.0,0.0,0.0,  0.0,0.0,0.0,1e6,0.0,0.0,  0.0,0.0,0.0,0.0,1e6,0.0,  0.0,0.0,0.0,0.0,0.0,1e3]
ODOM_POSE_COV2 = [1e-9,0.0,0.0,0.0,0.0,0.0, 0.0,1e-3,0.0,0.0,0.0,0.0,  0.0,0.0,1e6,0.0,0.0,0.0,  0.0,0.0,0.0,1e6,0.0,0.0,  0.0,0.0,0.0,0.0,1e6,0.0,  0.0,0.0,0.0,0.0,0.0,1e-9]
ODOM_TWIST_COV = [1e-3,0.0,0.0,0.0,0.0,0.0,  0.0,1e-3,0.0,0.0,0.0,0.0,  0.0,0.0,1e6,0.0,0.0,0.0,  0.0,0.0,0.0,1e6,0.0,0.0,  0.0,0.0,0.0,0.0,1e6,0.0,  0.0,0.0,0.0,0.0,0.0,1e3]
ODOM_TWIST_COV2 = [1e-9,0.0,0.0,0.0,0.0,0.0, 0.0,1e-3,0.0,0.0,0.0,0.0,  0.0,0.0,1e6,0.0,0.0,0.0,  0.0,0.0,0.0,1e6,0.0,0.0,  0.0,0.0,0.0,0.0,1e6,0.0,  0.0,0.0,0.0,0.0,0.0,1e-9]


def quat_from_yaw(yaw):
    q = Quaternion()
    q.z = math.sin(yaw / 2.0)
    q.w = math.cos(yaw / 2.0)
    return q


class RawOdomNode(Node):
    def __init__(self):
        super().__init__('raw_odom')
        self.odom_pub = self.create_publisher(Odometry, '/odom_raw', 10)
        self.imu_pub = self.create_publisher(Imu, '/imu/data_raw', 10)

        self.x = 0.0
        self.y = 0.0
        self.yaw = 0.0
        self.prev_odl = None
        self.prev_odr = None
        self.last_ts = None
        self.vx = 0.0
        self.vyaw = 0.0
        self._dbg_count = 0

        self.timer = self.create_timer(1.0 / 30.0, self.tick)
        self.get_logger().info('raw_odom started, reading ' + SHM)

    def tick(self):
        if not os.path.exists(SHM):
            return
        try:
            with open(SHM, 'r') as f:
                d = json.load(f)
        except Exception:
            return
        if d.get('T') != 1001:
            return

        now = time.time()
        odl = d.get('odl', 0) * MM_PER_TICK / 1000.0
        odr = d.get('odr', 0) * MM_PER_TICK / 1000.0

        dt = now - self.last_ts if self.last_ts is not None else 0.0
        self.last_ts = now
        if dt <= 0:
            return

        if self.prev_odl is not None:
            dleft = odl - self.prev_odl
            dright = odr - self.prev_odr
            dxy_ave = (dright + dleft) / 2.0
            dth = (dright - dleft) / WHEEL_BASE

            if dxy_ave != 0:
                dx = math.cos(dth / 2) * dxy_ave
                dy = math.sin(dth / 2) * dxy_ave
                self.x += math.cos(self.yaw) * dx - math.sin(self.yaw) * dy
                self.y += math.sin(self.yaw) * dx + math.cos(self.yaw) * dy
            if dth != 0:
                self.yaw += dth

            self.vx = dxy_ave / dt if dt > 0 else 0.0
            self.vyaw = dth / dt if dt > 0 else 0.0

            if abs(dleft) > 0.0001 or abs(dright) > 0.0001:
                self._dbg_count += 1
                if self._dbg_count % 20 == 1:
                    self.get_logger().info('x=%.3f y=%.3f yaw=%.3f vx=%.3f vyaw=%.3f'
                                           % (self.x, self.y, self.yaw, self.vx, self.vyaw))

        self.prev_odl = odl
        self.prev_odr = odr

        # publish odometry (малинкины ковариации)
        odom = Odometry()
        odom.header.stamp = self.get_clock().now().to_msg()
        odom.header.frame_id = 'odom'
        odom.child_frame_id = 'base_footprint'
        odom.pose.pose.position.x = self.x
        odom.pose.pose.position.y = self.y
        odom.pose.pose.orientation = quat_from_yaw(self.yaw)
        odom.twist.twist.linear.x = self.vx
        odom.twist.twist.angular.z = self.vyaw
        if self.vx == 0 and self.vyaw == 0:
            odom.pose.covariance = ODOM_POSE_COV2
            odom.twist.covariance = ODOM_TWIST_COV2
        else:
            odom.pose.covariance = ODOM_POSE_COV
            odom.twist.covariance = ODOM_TWIST_COV
        self.odom_pub.publish(odom)

        # publish IMU (малинкины формулы)
        imu = Imu()
        imu.header.stamp = odom.header.stamp
        imu.header.frame_id = 'base_imu_link'
        imu.linear_acceleration.x = 9.8 * float(d.get('ax', 0)) / 8192.0
        imu.linear_acceleration.y = 9.8 * float(d.get('ay', 0)) / 8192.0
        imu.linear_acceleration.z = 9.8 * float(d.get('az', 0)) / 8192.0
        imu.angular_velocity.x = 3.1415926 * float(d.get('gx', 0)) / (16.4 * 180.0)
        imu.angular_velocity.y = 3.1415926 * float(d.get('gy', 0)) / (16.4 * 180.0)
        imu.angular_velocity.z = 3.1415926 * float(d.get('gz', 0)) / (16.4 * 180.0)
        self.imu_pub.publish(imu)


def main(args=None):
    rclpy.init(args=args)
    node = RawOdomNode()
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
