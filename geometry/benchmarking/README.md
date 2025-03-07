Runtime Performance Benchmarks for Geometry Operations
------------------------------------------------------

# Supported experiments

## render

    $ bazel run //geometry/benchmarking:render_experiment -- --output_dir=foo

Benchmark program to help characterize the relative costs of different
RenderEngine implementations with varying scene complexity and rendering. It is
designed so users can assess the relative cost of the renderers on their own
hardware configuration, aiding in design decisions for understanding the cost of
renderer choice.

## mesh_intersection

    $ bazel run //geometry/benchmarking:mesh_intersection_experiment -- --output_dir=foo

Benchmark program to evaluate bounding volume hierarchy impact on mesh-mesh
intersections across varying mesh attributes and overlaps. It is targeted toward
developers during the process of optimizing the performance of hydroelastic
contact and may be removed once sufficient work has been done in that effort.

# Additional information

Documentation for command line arguments is here:
https://github.com/google/benchmark#command-line
