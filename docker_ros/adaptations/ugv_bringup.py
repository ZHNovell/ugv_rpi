import json
import os
import time
import rclpy
from rclpy.node import Node
from std_msgs.msg import Header, Float32MultiArray, Float32
from sensor_msgs.msg import Imu, MagneticField
from geometry_msgs.msg import Twist

# Адаптация для Orange Pi: телеметрия ESP32 читается из /dev/shm/esp_telemetry.json,
# который пишет хост-приложение app.py (владелец последовательного порта /dev/ttyACM0).
# Всё остальное (темы, frame_id, преобразования) - 1:1 как у малинки ugv_bringup.py.
SHM_PATH = "/dev/shm/esp_telemetry.json"
CMD_VEL_SHM_PATH = "/dev/shm/cmd_vel.json"


class ShmTelemetry:
    """Читает последнюю телеметрию ESP32 из разделяемого файла."""

    def __init__(self):
        self.base_data = {
            "T": 1001, "L": 0, "R": 0,
            "ax": 0, "ay": 0, "az": 0,
            "gx": 0, "gy": 0, "gz": 0,
            "mx": 0, "my": 0, "mz": 0,
            "odl": 0, "odr": 0, "v": 0,
        }

    def feedback_data(self):
        try:
            with open(SHM_PATH, "r") as f:
                data = json.load(f)
            if isinstance(data, dict) and data.get("T") == 1001:
                self.base_data = data
        except (json.JSONDecodeError, OSError, ValueError):
            pass
        return self.base_data


class ugv_bringup(Node):
    def __init__(self):
        super().__init__('ugv_bringup')
        # Publishers for IMU data, magnetic field data, odometry, and voltage
        self.imu_data_raw_publisher_ = self.create_publisher(Imu, "imu/data_raw", 100)
        self.imu_mag_publisher_ = self.create_publisher(MagneticField, "imu/mag", 100)
        self.odom_publisher_ = self.create_publisher(Float32MultiArray, "odom/odom_raw", 100)
        self.voltage_publisher_ = self.create_publisher(Float32, "voltage", 50)
        # Bridge nav2 /cmd_vel to the host app via /dev/shm (host owns the serial)
        self.cmd_vel_sub_ = self.create_subscription(Twist, "cmd_vel", self.cmd_vel_cb, 10)
        # Telemetry source: shared file (instead of serial)
        self.telemetry = ShmTelemetry()
        self.feedback_timer = self.create_timer(0.02, self.feedback_loop)

    # Bridge: write nav2 /cmd_vel to /dev/shm/cmd_vel.json for the host app
    def cmd_vel_cb(self, msg):
        data = {"X": float(msg.linear.x), "Z": float(msg.angular.z), "ts": time.time()}
        try:
            tmp = CMD_VEL_SHM_PATH + ".tmp"
            with open(tmp, "w") as f:
                json.dump(data, f)
            os.rename(tmp, CMD_VEL_SHM_PATH)
        except OSError:
            pass

    # Main loop for reading sensor feedback and publishing it to ROS topics
    def feedback_loop(self):
        self.telemetry.feedback_data()
        if self.telemetry.base_data.get("T") == 1001:
            self.publish_imu_data_raw()
            self.publish_imu_mag()
            self.publish_odom_raw()
            self.publish_voltage()

    # Publish IMU data to the ROS topic "imu/data_raw"
    def publish_imu_data_raw(self):
        msg = Imu()
        msg.header = Header()
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.header.frame_id = "base_imu_link"
        imu_raw_data = self.telemetry.base_data

        msg.linear_acceleration.x = 9.8 * float(imu_raw_data["ax"]) / 8192
        msg.linear_acceleration.y = 9.8 * float(imu_raw_data["ay"]) / 8192
        msg.linear_acceleration.z = 9.8 * float(imu_raw_data["az"]) / 8192

        msg.angular_velocity.x = 3.1415926 * float(imu_raw_data["gx"]) / (16.4 * 180)
        msg.angular_velocity.y = 3.1415926 * float(imu_raw_data["gy"]) / (16.4 * 180)
        msg.angular_velocity.z = 3.1415926 * float(imu_raw_data["gz"]) / (16.4 * 180)

        self.imu_data_raw_publisher_.publish(msg)

    # Publish magnetic field data to the ROS topic "imu/mag"
    def publish_imu_mag(self):
        msg = MagneticField()
        msg.header = Header()
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.header.frame_id = "base_imu_link"
        imu_raw_data = self.telemetry.base_data

        msg.magnetic_field.x = float(imu_raw_data["mx"]) * 0.15
        msg.magnetic_field.y = float(imu_raw_data["my"]) * 0.15
        msg.magnetic_field.z = float(imu_raw_data["mz"]) * 0.15

        self.imu_mag_publisher_.publish(msg)

    # Publish odometry data to the ROS topic "odom/odom_raw"
    def publish_odom_raw(self):
        odom_raw_data = self.telemetry.base_data
        array = [odom_raw_data["odl"] / 100, odom_raw_data["odr"] / 100]
        msg = Float32MultiArray(data=array)
        self.odom_publisher_.publish(msg)

    # Publish voltage data to the ROS topic "voltage"
    def publish_voltage(self):
        voltage_data = self.telemetry.base_data
        msg = Float32()
        msg.data = float(voltage_data["v"]) / 100
        self.voltage_publisher_.publish(msg)


def main(args=None):
    rclpy.init(args=args)
    node = ugv_bringup()
    rclpy.spin(node)
    rclpy.shutdown()


if __name__ == '__main__':
    main()
