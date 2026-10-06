# Bullet Physics 3.25 (bulletphysics/bullet3, tag 3.25), vendored as three static libs.
#
# Only src/{LinearMath,BulletCollision,BulletDynamics} plus the two convenience headers are kept -
# no Bullet3* (the experimental GPU codebase), no BulletSoftBody, no BulletInverseDynamics, no
# examples/tests/docs. Bullet's own CMake is deliberately NOT used.
#
# The x86-32 / SSE2 / static-CRT / charset settings all come from the root CMakeLists (vendor/ is
# added before the Alpine-only charset options), so nothing is restated here.

set(BULLET_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(GLOB_RECURSE LINEARMATH_SRCS       "${BULLET_SRC_DIR}/LinearMath/*.cpp"       "${BULLET_SRC_DIR}/LinearMath/*.h")
file(GLOB_RECURSE BULLETCOLLISION_SRCS  "${BULLET_SRC_DIR}/BulletCollision/*.cpp"  "${BULLET_SRC_DIR}/BulletCollision/*.h")
file(GLOB_RECURSE BULLETDYNAMICS_SRCS   "${BULLET_SRC_DIR}/BulletDynamics/*.cpp"   "${BULLET_SRC_DIR}/BulletDynamics/*.h")

add_library(LinearMath      STATIC ${LINEARMATH_SRCS})
add_library(BulletCollision STATIC ${BULLETCOLLISION_SRCS})
add_library(BulletDynamics  STATIC ${BULLETDYNAMICS_SRCS})

foreach(bullet_target LinearMath BulletCollision BulletDynamics)
    # Includes are written as "LinearMath/btScalar.h" etc, relative to src/.
    target_include_directories(${bullet_target} PUBLIC "${BULLET_SRC_DIR}")
    target_compile_features(${bullet_target} PUBLIC cxx_std_17)
    set_target_properties(${bullet_target} PROPERTIES CXX_EXTENSIONS NO)
    if(MSVC)
        # Third-party code: keep its own warning noise out of the Alpine build log.
        target_compile_options(${bullet_target} PRIVATE /W0)
    else()
        target_compile_options(${bullet_target} PRIVATE -w)
    endif()
endforeach()

# Dynamics depends on Collision depends on LinearMath.
target_link_libraries(BulletCollision PUBLIC LinearMath)
target_link_libraries(BulletDynamics PUBLIC BulletCollision)
