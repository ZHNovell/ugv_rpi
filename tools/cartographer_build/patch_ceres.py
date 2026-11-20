p = '/root/cartographer/CMakeLists.txt'
c = open(p).read()
c = c.replace('find_package(Ceres REQUIRED COMPONENTS SuiteSparse)',
              'find_package(Ceres REQUIRED)')
open(p, 'w').write(c)
print('patched:', 'find_package(Ceres REQUIRED)' in c)
