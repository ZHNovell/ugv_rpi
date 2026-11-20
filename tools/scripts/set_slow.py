import yaml
p = '/root/ugv_rpi/config.yaml'
f = yaml.safe_load(open(p))
if 'args_config' in f and 'slow_speed' in f['args_config']:
    f['args_config']['slow_speed'] = 0.1
yaml.dump(f, open(p, 'w'), default_flow_style=False)
print('slow_speed = 0.1')
