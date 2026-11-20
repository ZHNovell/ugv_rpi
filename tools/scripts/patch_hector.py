p = '/root/hector_slam/hector_mapping/include/hector_slam_lib/util/UtilFunctions.h'
c = open(p).read()
c = c.replace('#include <tf/LinearMath/Transform.h>\n',
              '// #include <tf/LinearMath/Transform.h>  // removed (ROS-free)\n')
c = c.replace('''static double getYawFromQuat(const geometry_msgs::Quaternion &quat)
{
  return tf::getYaw(tf::Quaternion(quat.x, quat.y, quat.z, quat.w));
}''', '// getYawFromQuat removed (ROS-free)')
open(p, 'w').write(c)
print('patched UtilFunctions.h')
