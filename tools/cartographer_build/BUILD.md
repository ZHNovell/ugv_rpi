# Cartographer build on Orange Pi (Armbian trixie / Debian 13, arm64)

Native standalone build of libcartographer (no ROS). Verified working.

## Version pins (critical — Debian 13 defaults are all incompatible)
- Abseil: `20230125.3` (master breaks `absl::Condition`; 20211102.0 breaks GCC 13)
- protobuf: `v3.21.12` (Debian 13 ships 25.3 whose generated .pb.h uses PROTOBUF_TSAN_* macros missing from the runtime)
- Ceres: `2.0.0` (2.2.0 removed `LocalParameterization`/`QuaternionParameterization` used by Cartographer 3D code)

## Steps
```sh
# 1. apt deps
apt-get install -y libceres-dev libprotobuf-dev liblua5.2-dev libgflags-dev \
  libgoogle-glog-dev libsuitesparse-dev libcairo2-dev ninja-build protobuf-compiler \
  libgmock-dev libgtest-dev stow

# 2. Abseil 20230125
git clone https://github.com/abseil/abseil-cpp.git && cd abseil-cpp
git checkout 20230125.3
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local/stow/absl ..
ninja && ninja install
cd /usr/local/stow && stow absl

# 3. protobuf 3.21 (overrides system 25.3)
git clone --depth 1 --branch v3.21.12 https://github.com/protocolbuffers/protobuf.git protobuf321
cd protobuf321 && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr/local . && ninja && ninja install

# 4. Ceres 2.0.0 (SuiteSparse 7.x + TBB oneTBB layout are incompatible with old Ceres)
git clone --depth 1 --branch 2.0.0 https://ceres-solver.googlesource.com/ceres-solver.git
cd ceres-solver
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_BENCHMARKS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_TBB=TRUE -DSUITESPARSE=OFF \
  -DCXSPARSE=OFF -DCMAKE_INSTALL_PREFIX=/usr/local .
ninja && ninja install

# 5. Cartographer (with patches from this dir)
git clone --depth 1 https://github.com/cartographer-project/cartographer.git
# patch: find_package(Ceres REQUIRED COMPONENTS SuiteSparse) -> find_package(Ceres REQUIRED)
python3 /tmp/patch_ceres.py   # see patch_ceres.py
# patch: absl::Condition(&predicate) -> absl::Condition(+predicate, this) + lambda [this]()->[](self)
python3 /tmp/patch_condition.py  # see patch_condition.py
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_CXX_FLAGS=-include/root/tsan_fix.h ..
ninja && ninja install
```

## tsan_fix.h (-include)
Defines no-op PROTOBUF_TSAN_* / ABSL_DCHECK / thread-annotations macros (Debian
protobuf+glog mismatch) + `#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"`
(GCC 14 false positive on Eigen).

Result: `/usr/local/lib` (static lib + headers), config in
`/usr/local/share/cartographer/configuration_files/`.
