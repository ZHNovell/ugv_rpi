import yaml
p = '/root/ugv_rpi/config.yaml'
f = yaml.safe_load(open(p))
for section in f.values():
    if isinstance(section, dict) and 'slam_mode' in section:
        section['slam_mode'] = True
    if isinstance(section, dict) and 'use_lidar' in section:
        section['use_lidar'] = False
yaml.dump(f, open(p, 'w'), default_flow_style=False)
print('slam_mode=True, use_lidar=False')
