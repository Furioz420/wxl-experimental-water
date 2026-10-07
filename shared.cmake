list(APPEND WXL_EXT_SHARED_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extension-support/environment/EnvironmentModule.cpp")

# The accepted 3.3.5 split-core runtime uses LatticeCompat.cpp's stock-topology
# adapter. The donor refinement/ownership and ride units depend on liquid
# lifecycle hooks and OnM2Placement that this core does not expose. Keep their
# source for later verified integration, but do not compile them into this DLL.
# This belongs in the extension, so a standalone checkout builds the same
# feature set as the integrated WXL workspace.
list(FILTER WXL_EXT_SRC EXCLUDE REGEX "/mesh/(Lattice|Owned)\\.cpp$|/world/Ride\\.cpp$")
